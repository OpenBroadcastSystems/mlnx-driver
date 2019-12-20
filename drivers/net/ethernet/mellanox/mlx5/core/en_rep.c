/*
 * Copyright (c) 2016, Mellanox Technologies. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifdef HAVE_UTSRELEASE_H
#include <generated/utsrelease.h>
#endif
#include <linux/mlx5/fs.h>
#include <net/switchdev.h>
#include <net/pkt_cls.h>
#ifdef HAVE_TC_SETUP_CB_EGDEV_REGISTER
#include <net/act_api.h>
#endif
#include <net/netevent.h>
#include <net/arp.h>
#include <net/addrconf.h>

#include "eswitch.h"
#include "en.h"
#include "en_rep.h"
#include "en_tc.h"
#include "fs_core.h"
#include "ecpf.h"
#include "en_stats.h"

#define MLX5E_REP_PARAMS_LOG_SQ_SIZE \
	max(0x6, MLX5E_PARAMS_MINIMUM_LOG_SQ_SIZE)
#define MLX5E_REP_PARAMS_DEF_NUM_CHANNELS 1

static const char mlx5e_rep_driver_name[] = "mlx5e_rep";

#ifdef HAVE_UTSRELEASE_H
static void mlx5e_rep_get_drvinfo(struct net_device *dev,
				  struct ethtool_drvinfo *drvinfo)
{
	strlcpy(drvinfo->driver, mlx5e_rep_driver_name,
		sizeof(drvinfo->driver));
	strlcpy(drvinfo->version, UTS_RELEASE, sizeof(drvinfo->version));
}
#endif

static void mlx5e_rep_get_strings(struct net_device *dev,
				  u32 stringset, uint8_t *data)
{
	struct mlx5e_priv *priv = netdev_priv(dev);
	int idx = 0;
	int i;

	switch (stringset) {
	case ETH_SS_PRIV_FLAGS:
		for (i = 0; i < mlx5e_priv_flags_num(); i++)
			strcpy(data + i * ETH_GSTRING_LEN, mlx5e_priv_flags[i]);
		break;
	case ETH_SS_STATS:
		for (i = 0; i < mlx5e_rep_num_stats_grps; i++)
			idx = mlx5e_rep_stats_grps[i].fill_strings(priv, data, idx);
		break;
	}
}

static void mlx5e_rep_update_stats(struct mlx5e_priv *priv)
{
	int i;

	for (i = mlx5e_rep_num_stats_grps - 1; i >= 0; i--)
		if (mlx5e_rep_stats_grps[i].update_stats)
			mlx5e_rep_stats_grps[i].update_stats(priv);
}

static void mlx5e_rep_get_ethtool_stats(struct net_device *dev,
					struct ethtool_stats *stats, u64 *data)
{
	struct mlx5e_priv *priv = netdev_priv(dev);
	int i, idx = 0;

	if (!data)
		return;

	mutex_lock(&priv->state_lock);
	mlx5e_update_stats(priv);
	mutex_unlock(&priv->state_lock);

	for (i = 0; i < mlx5e_rep_num_stats_grps; i++)
		idx = mlx5e_rep_stats_grps[i].fill_stats(priv, data, idx);
}

static int mlx5e_rep_get_sset_count(struct net_device *dev, int sset)
{
	struct mlx5e_priv *priv = netdev_priv(dev);
	int num_stats = 0;
	int i;

	switch (sset) {
	case ETH_SS_STATS:
		for (i = 0; i < mlx5e_rep_num_stats_grps; i++)
			num_stats += mlx5e_rep_stats_grps[i].get_num_stats(priv);
		return num_stats;
	case ETH_SS_PRIV_FLAGS:
		return mlx5e_priv_flags_num();
	default:
		return -EOPNOTSUPP;
	}
}

static void mlx5e_rep_get_ringparam(struct net_device *dev,
				struct ethtool_ringparam *param)
{
	struct mlx5e_priv *priv = netdev_priv(dev);

	mlx5e_ethtool_get_ringparam(priv, param);
}

static int mlx5e_rep_set_ringparam(struct net_device *dev,
			       struct ethtool_ringparam *param)
{
	struct mlx5e_priv *priv = netdev_priv(dev);

	return mlx5e_ethtool_set_ringparam(priv, param);
}

#ifdef HAVE_ETHTOOL_OPS_GET_SET_CHANNELS
static int mlx5e_replace_rep_vport_rx_rule(struct mlx5e_priv *priv,
					   struct mlx5_flow_destination *dest)
{
	struct mlx5_eswitch *esw = priv->mdev->priv.eswitch;
	struct mlx5e_rep_priv *rpriv = priv->ppriv;
	struct mlx5_eswitch_rep *rep = rpriv->rep;
	struct mlx5_flow_handle *flow_rule;

	flow_rule = mlx5_eswitch_create_vport_rx_rule(esw,
						      rep->vport,
						      dest);
	if (IS_ERR(flow_rule))
		return PTR_ERR(flow_rule);

	mlx5_del_flow_rules(rpriv->vport_rx_rule);
	rpriv->vport_rx_rule = flow_rule;
	return 0;
}

static void mlx5e_rep_get_channels(struct net_device *dev,
				   struct ethtool_channels *ch)
{
	struct mlx5e_priv *priv = netdev_priv(dev);

	mlx5e_ethtool_get_channels(priv, ch);
}

static int mlx5e_rep_set_channels(struct net_device *dev,
				  struct ethtool_channels *ch)
{
	struct mlx5e_priv *priv = netdev_priv(dev);
	u16 curr_channels_amount = priv->channels.params.num_channels;
	u32 new_channels_amount = ch->combined_count;
	struct mlx5_flow_destination new_dest;
	int err = 0;

	err = mlx5e_ethtool_set_channels(priv, ch);
	if (err)
		return err;

	if (curr_channels_amount == 1 && new_channels_amount > 1) {
		new_dest.type = MLX5_FLOW_DESTINATION_TYPE_FLOW_TABLE;
		new_dest.ft = priv->fs.ttc.ft.t;
	} else if (new_channels_amount == 1 && curr_channels_amount > 1) {
		new_dest.type = MLX5_FLOW_DESTINATION_TYPE_TIR;
		new_dest.tir_num = priv->direct_tir[0].tirn;
	} else {
		return 0;
	}

	err = mlx5e_replace_rep_vport_rx_rule(priv, &new_dest);
	if (err) {
		netdev_warn(priv->netdev, "Failed to update vport rx rule, when going from (%d) channels to (%d) channels\n",
			    curr_channels_amount, new_channels_amount);
		return err;
	}

	return 0;
}
#endif

static int mlx5e_rep_get_coalesce(struct net_device *netdev,
				  struct ethtool_coalesce *coal)
{
	struct mlx5e_priv *priv = netdev_priv(netdev);

	return mlx5e_ethtool_get_coalesce(priv, coal);
}

static int mlx5e_rep_set_coalesce(struct net_device *netdev,
				  struct ethtool_coalesce *coal)
{
	struct mlx5e_priv *priv = netdev_priv(netdev);

	return mlx5e_ethtool_set_coalesce(priv, coal);
}

#if defined(HAVE_GET_SET_RXFH) && !defined(HAVE_GET_SET_RXFH_INDIR_EXT)
static u32 mlx5e_rep_get_rxfh_key_size(struct net_device *netdev)
{
	struct mlx5e_priv *priv = netdev_priv(netdev);

	return mlx5e_ethtool_get_rxfh_key_size(priv);
}

static u32 mlx5e_rep_get_rxfh_indir_size(struct net_device *netdev)
{
	struct mlx5e_priv *priv = netdev_priv(netdev);

	return mlx5e_ethtool_get_rxfh_indir_size(priv);
}
#endif

static void mlx5e_uplink_rep_get_pauseparam(struct net_device *netdev,
					    struct ethtool_pauseparam *pauseparam)
{
	struct mlx5e_priv *priv = netdev_priv(netdev);

	mlx5e_ethtool_get_pauseparam(priv, pauseparam);
}

static int mlx5e_uplink_rep_set_pauseparam(struct net_device *netdev,
					   struct ethtool_pauseparam *pauseparam)
{
	struct mlx5e_priv *priv = netdev_priv(netdev);

	return mlx5e_ethtool_set_pauseparam(priv, pauseparam);
}

#ifdef HAVE_GET_SET_LINK_KSETTINGS
static int mlx5e_uplink_rep_get_link_ksettings(struct net_device *netdev,
					       struct ethtool_link_ksettings *link_ksettings)
{
	struct mlx5e_priv *priv = netdev_priv(netdev);

	return mlx5e_ethtool_get_link_ksettings(priv, link_ksettings);
}

static int mlx5e_uplink_rep_set_link_ksettings(struct net_device *netdev,
					       const struct ethtool_link_ksettings *link_ksettings)
{
	struct mlx5e_priv *priv = netdev_priv(netdev);

	return mlx5e_ethtool_set_link_ksettings(priv, link_ksettings);
}
#endif

static const struct ethtool_ops mlx5e_vf_rep_ethtool_ops = {
#ifdef HAVE_UTSRELEASE_H
	.get_drvinfo	   = mlx5e_rep_get_drvinfo,
#endif
	.get_link	   = ethtool_op_get_link,
	.get_strings       = mlx5e_rep_get_strings,
	.get_sset_count    = mlx5e_rep_get_sset_count,
	.get_ethtool_stats = mlx5e_rep_get_ethtool_stats,
	.get_ringparam     = mlx5e_rep_get_ringparam,
	.set_ringparam     = mlx5e_rep_set_ringparam,
#ifdef HAVE_ETHTOOL_OPS_GET_SET_CHANNELS
	.get_channels      = mlx5e_rep_get_channels,
	.set_channels      = mlx5e_rep_set_channels,
#endif
	.get_coalesce      = mlx5e_rep_get_coalesce,
	.set_coalesce      = mlx5e_rep_set_coalesce,
#if defined(HAVE_GET_SET_RXFH) && !defined(HAVE_GET_SET_RXFH_INDIR_EXT)
	.get_rxfh_key_size   = mlx5e_rep_get_rxfh_key_size,
	.get_rxfh_indir_size = mlx5e_rep_get_rxfh_indir_size,
#endif
#ifdef HAVE_GET_SET_PRIV_FLAGS
	.get_priv_flags    = mlx5e_get_priv_flags,
	.set_priv_flags    = mlx5e_set_priv_flags,
#endif
};

static const struct ethtool_ops mlx5e_uplink_rep_ethtool_ops = {
#ifdef HAVE_UTSRELEASE_H
	.get_drvinfo	   = mlx5e_rep_get_drvinfo,
#endif
	.get_link	   = ethtool_op_get_link,
	.get_strings       = mlx5e_rep_get_strings,
	.get_sset_count    = mlx5e_rep_get_sset_count,
	.get_ethtool_stats = mlx5e_rep_get_ethtool_stats,
	.get_ringparam     = mlx5e_rep_get_ringparam,
	.set_ringparam     = mlx5e_rep_set_ringparam,
#ifdef HAVE_ETHTOOL_OPS_GET_SET_CHANNELS
	.get_channels      = mlx5e_rep_get_channels,
	.set_channels      = mlx5e_rep_set_channels,
#endif
	.get_coalesce      = mlx5e_rep_get_coalesce,
	.set_coalesce      = mlx5e_rep_set_coalesce,
#ifdef HAVE_GET_SET_LINK_KSETTINGS
	.get_link_ksettings = mlx5e_uplink_rep_get_link_ksettings,
	.set_link_ksettings = mlx5e_uplink_rep_set_link_ksettings,
#endif
#ifdef HAVE_ETHTOOL_GET_SET_SETTINGS
	.get_settings  = mlx5e_get_settings,
	.set_settings  = mlx5e_set_settings,
#endif
#if defined(HAVE_GET_SET_RXFH) && !defined(HAVE_GET_SET_RXFH_INDIR_EXT)
	.get_rxfh_key_size   = mlx5e_rep_get_rxfh_key_size,
	.get_rxfh_indir_size = mlx5e_rep_get_rxfh_indir_size,
#endif
	.get_pauseparam    = mlx5e_uplink_rep_get_pauseparam,
	.set_pauseparam    = mlx5e_uplink_rep_set_pauseparam,
#ifdef HAVE_GET_SET_PRIV_FLAGS
	.get_priv_flags    = mlx5e_get_priv_flags,
	.set_priv_flags    = mlx5e_set_priv_flags,
#endif
};

#if (defined(HAVE_SWITCHDEV_OPS) && defined(CONFIG_NET_SWITCHDEV)) \
    || defined(HAVE_SWITCHDEV_H_COMPAT)
static int mlx5e_attr_get(struct net_device *dev, struct switchdev_attr *attr)
{
	struct mlx5e_priv *priv = netdev_priv(dev);
	struct mlx5_eswitch *esw = priv->mdev->priv.eswitch;
 	struct mlx5e_rep_priv *uplink_rpriv;
	struct net_device *uplink_dev;
	struct mlx5e_priv *uplink_priv;
	struct net_device *uplink_upper;

	if (esw->mode == SRIOV_NONE)
		return -EOPNOTSUPP;

 	uplink_rpriv = mlx5_eswitch_get_uplink_priv(esw, REP_ETH);
	uplink_dev = uplink_rpriv->netdev;
	uplink_priv = netdev_priv(uplink_dev);
	uplink_upper = netdev_master_upper_dev_get(uplink_dev);

	switch (attr->id) {
#ifdef HAVE_SWITCHDEV_ATTR_ID_PORT_PARENT_ID
	case SWITCHDEV_ATTR_ID_PORT_PARENT_ID:
#else
	case SWITCHDEV_ATTR_PORT_PARENT_ID:
#endif
		attr->u.ppid.id_len = ETH_ALEN;
		if (uplink_upper && mlx5_lag_is_sriov(uplink_priv->mdev)) {
			ether_addr_copy(attr->u.ppid.id, uplink_upper->dev_addr);
		} else {
			struct mlx5e_rep_priv *rpriv = priv->ppriv;
			struct mlx5_eswitch_rep *rep = rpriv->rep;

			ether_addr_copy(attr->u.ppid.id, rep->hw_id);
		}
		break;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}
#endif

static void mlx5e_sqs2vport_stop(struct mlx5_eswitch *esw,
				 struct mlx5_eswitch_rep *rep)
{
	struct mlx5e_rep_sq *rep_sq, *tmp;
	struct mlx5e_rep_priv *rpriv;

#ifndef CONFIG_BF_DEVICE_EMULATION
	if (esw->mode != SRIOV_OFFLOADS)
		return;
#else
	if (esw->mode != SRIOV_OFFLOADS ||
	    mlx5_core_is_dev_emulation_manager(esw->dev))
		return;
#endif

	rpriv = mlx5e_rep_to_rep_priv(rep);
	list_for_each_entry_safe(rep_sq, tmp, &rpriv->vport_sqs_list, list) {
		mlx5_eswitch_del_send_to_vport_rule(rep_sq->send_to_vport_rule);
		list_del(&rep_sq->list);
		kfree(rep_sq);
	}
}

static int mlx5e_sqs2vport_start(struct mlx5_eswitch *esw,
				 struct mlx5_eswitch_rep *rep,
				 u32 *sqns_array, int sqns_num)
{
	struct mlx5_flow_handle *flow_rule;
	struct mlx5e_rep_priv *rpriv;
	struct mlx5e_rep_sq *rep_sq;
	int err;
	int i;

#ifndef CONFIG_BF_DEVICE_EMULATION
	if (esw->mode != SRIOV_OFFLOADS)
		return 0;
#else
	if (esw->mode != SRIOV_OFFLOADS ||
	    mlx5_core_is_dev_emulation_manager(esw->dev))
		return 0;
#endif

	rpriv = mlx5e_rep_to_rep_priv(rep);
	for (i = 0; i < sqns_num; i++) {
		rep_sq = kzalloc(sizeof(*rep_sq), GFP_KERNEL);
		if (!rep_sq) {
			err = -ENOMEM;
			goto out_err;
		}

		/* Add re-inject rule to the PF/representor sqs */
		flow_rule = mlx5_eswitch_add_send_to_vport_rule(esw,
								rep->vport,
								sqns_array[i]);
		if (IS_ERR(flow_rule)) {
			err = PTR_ERR(flow_rule);
			kfree(rep_sq);
			goto out_err;
		}
		rep_sq->send_to_vport_rule = flow_rule;
		list_add(&rep_sq->list, &rpriv->vport_sqs_list);
	}
	return 0;

out_err:
	mlx5e_sqs2vport_stop(esw, rep);
	return err;
}

int mlx5e_add_sqs_fwd_rules(struct mlx5e_priv *priv)
{
	struct mlx5_eswitch *esw = priv->mdev->priv.eswitch;
	struct mlx5e_rep_priv *rpriv = priv->ppriv;
	struct mlx5_eswitch_rep *rep = rpriv->rep;
	struct mlx5e_channel *c;
	int n, tc, num_sqs = 0;
	int err = -ENOMEM;
	u32 *sqs;
	int num_txqs = priv->channels.params.num_channels * priv->channels.params.num_tc;

#ifdef CONFIG_MLX5_EN_SPECIAL_SQ
	num_txqs += priv->channels.params.num_rl_txqs;
#endif
	sqs = kcalloc(num_txqs, sizeof(*sqs), GFP_KERNEL);
	if (!sqs)
		goto out;

	for (n = 0; n < priv->channels.num; n++) {
		c = priv->channels.c[n];
		for (tc = 0; tc < c->num_tc; tc++)
			sqs[num_sqs++] = c->sq[tc].sqn;
#ifdef CONFIG_MLX5_EN_SPECIAL_SQ
		for (tc = 0; tc < c->num_special_sq; tc++)
			sqs[num_sqs++] = c->special_sq[tc].sqn;
#endif
	}

	err = mlx5e_sqs2vport_start(esw, rep, sqs, num_sqs);
	kfree(sqs);

out:
	if (err)
		netdev_warn(priv->netdev, "Failed to add SQs FWD rules %d\n", err);
	return err;
}

void mlx5e_remove_sqs_fwd_rules(struct mlx5e_priv *priv)
{
	struct mlx5_eswitch *esw = priv->mdev->priv.eswitch;
	struct mlx5e_rep_priv *rpriv = priv->ppriv;
	struct mlx5_eswitch_rep *rep = rpriv->rep;

	mlx5e_sqs2vport_stop(esw, rep);
}

#ifdef HAVE_TCF_TUNNEL_INFO
static void mlx5e_rep_neigh_update_init_interval(struct mlx5e_rep_priv *rpriv)
{
#if IS_ENABLED(CONFIG_IPV6)
	unsigned long ipv6_interval = (ipv6_stub && ipv6_stub->nd_tbl) ?
				      NEIGH_VAR(&ipv6_stub->nd_tbl->parms,
						DELAY_PROBE_TIME) : ~0UL;
#else
	unsigned long ipv6_interval = ~0UL;
#endif
	unsigned long ipv4_interval = NEIGH_VAR(&arp_tbl.parms,
						DELAY_PROBE_TIME);
	struct net_device *netdev = rpriv->netdev;
	struct mlx5e_priv *priv = netdev_priv(netdev);

	rpriv->neigh_update.min_interval = min_t(unsigned long, ipv6_interval, ipv4_interval);
	mlx5_fc_update_sampling_interval(priv->mdev, rpriv->neigh_update.min_interval);
}

void mlx5e_rep_queue_neigh_stats_work(struct mlx5e_priv *priv)
{
	struct mlx5e_rep_priv *rpriv = priv->ppriv;
	struct mlx5e_neigh_update_table *neigh_update = &rpriv->neigh_update;

	mlx5_fc_queue_stats_work(priv->mdev,
				 &neigh_update->neigh_stats_work,
				 neigh_update->min_interval);
}

bool mlx5e_rep_neigh_entry_hold(struct mlx5e_neigh_hash_entry *nhe)
{
#ifdef HAVE_REFCOUNT
	return refcount_inc_not_zero(&nhe->refcnt);
#else
	return atomic_inc_not_zero(&nhe->refcnt);
#endif
}

static void mlx5e_rep_neigh_entry_remove(struct mlx5e_priv *priv,
					 struct mlx5e_neigh_hash_entry *nhe);

static void mlx5e_rep_neigh_entry_release(struct mlx5e_priv *priv,
					  struct mlx5e_neigh_hash_entry *nhe)
{
#ifdef HAVE_REFCOUNT
	if (refcount_dec_and_test(&nhe->refcnt)) {
#else
	if (atomic_dec_and_test(&nhe->refcnt)) {
#endif
		mlx5e_rep_neigh_entry_remove(priv, nhe);
		kfree_rcu(nhe, rcu);
	}
}

#ifndef list_next_or_null_rcu
/**
 * list_next_or_null_rcu - get the first element from a list
 * @head:       the head for the list.
 * @ptr:        the list head to take the next element from.
 * @type:       the type of the struct this is embedded in.
 * @member:     the name of the list_head within the struct.
 *
 * Note that if the ptr is at the end of the list, NULL is returned.
 *
 * This primitive may safely run concurrently with the _rcu list-mutation
 * primitives such as list_add_rcu() as long as it's guarded by rcu_read_lock().
 */
#define list_next_or_null_rcu(head, ptr, type, member) \
({ \
	struct list_head *__head = (head); \
	struct list_head *__ptr = (ptr); \
	struct list_head *__next = READ_ONCE(__ptr->next); \
	likely(__next != __head) ? list_entry_rcu(__next, type, \
						  member) : NULL; \
})
#endif

static struct mlx5e_neigh_hash_entry *
mlx5e_get_next_nhe(struct mlx5e_rep_priv *rpriv,
		   struct mlx5e_neigh_hash_entry *nhe)
{
	struct net_device *netdev = rpriv->netdev;
	struct mlx5e_priv *priv = netdev_priv(netdev);
	struct mlx5e_neigh_hash_entry *next = NULL;

	rcu_read_lock();

	for (next = nhe ?
		     list_next_or_null_rcu(&rpriv->neigh_update.neigh_list,
					   &nhe->neigh_list,
					   struct mlx5e_neigh_hash_entry,
					   neigh_list) :
		     list_first_or_null_rcu(&rpriv->neigh_update.neigh_list,
					    struct mlx5e_neigh_hash_entry,
					    neigh_list);
	     next;
	     next = list_next_or_null_rcu(&rpriv->neigh_update.neigh_list,
					  &next->neigh_list,
					  struct mlx5e_neigh_hash_entry,
					  neigh_list))
		if (mlx5e_rep_neigh_entry_hold(next))
			break;

	rcu_read_unlock();

	if (nhe)
		mlx5e_rep_neigh_entry_release(priv, nhe);

	return next;
}

static void mlx5e_rep_neigh_stats_work(struct work_struct *work)
{
	struct mlx5e_rep_priv *rpriv = container_of(work, struct mlx5e_rep_priv,
						    neigh_update.neigh_stats_work.work);
	struct net_device *netdev = rpriv->netdev;
	struct mlx5e_priv *priv = netdev_priv(netdev);
	struct mlx5e_neigh_hash_entry *nhe = NULL;

	rtnl_lock();
	if (!list_empty(&rpriv->neigh_update.neigh_list))
		mlx5e_rep_queue_neigh_stats_work(priv);

	while ((nhe = mlx5e_get_next_nhe(rpriv, nhe)) != NULL)
		mlx5e_tc_update_neigh_used_value(nhe);

	rtnl_unlock();
}

static void mlx5e_rep_update_flows(struct mlx5e_priv *priv,
				   struct mlx5e_encap_entry *e,
				   bool neigh_connected,
				   unsigned char ha[ETH_ALEN],
				   unsigned long n_updated)
{
	struct ethhdr *eth = (struct ethhdr *)e->encap_header;

	ASSERT_RTNL();

	if ((e->flags & MLX5_ENCAP_ENTRY_VALID) &&
	    (!neigh_connected || !ether_addr_equal(e->h_dest, ha)))
		mlx5e_tc_encap_flows_del(priv, e, n_updated);

	if (neigh_connected && !(e->flags & MLX5_ENCAP_ENTRY_VALID)) {
		ether_addr_copy(e->h_dest, ha);
		ether_addr_copy(eth->h_dest, ha);

		mlx5e_tc_encap_flows_add(priv, e, n_updated);
	}
}

static void mlx5e_rep_neigh_update(struct work_struct *work)
{
	struct mlx5e_neigh_hash_entry *nhe =
		container_of(work, struct mlx5e_neigh_hash_entry, neigh_update_work);
	struct mlx5e_encap_entry *e, *tmp;
	struct neighbour *n = nhe->n;
	unsigned char ha[ETH_ALEN];
	struct mlx5e_priv *priv;
	unsigned long n_updated;
	LIST_HEAD(update_list);
	bool neigh_connected;
	bool encap_connected;
	u8 nud_state, dead;

	rtnl_lock();

	/* If these parameters are changed after we release the lock,
	 * we'll receive another event letting us know about it.
	 * We use this lock to avoid inconsistency between the neigh validity
	 * and it's hw address.
	 */
	read_lock_bh(&n->lock);
	memcpy(ha, n->ha, ETH_ALEN);
	nud_state = n->nud_state;
	dead = n->dead;
	n_updated = n->updated;
	read_unlock_bh(&n->lock);

	neigh_connected = (nud_state & NUD_VALID) && !dead;

	spin_lock(&nhe->encap_list_lock);
	/* First pass over encap entries takes references to encaps. Creating
	 * and deleting offloaded flows is blocking operation and can't be
	 * performed while holding spin lock.
	 */
	list_for_each_entry(e, &nhe->encap_list, encap_list) {
		if (!mlx5e_encap_take(e))
			continue;
		list_add(&e->neigh_update_list, &update_list);
	}
	spin_unlock(&nhe->encap_list_lock);

	/* At this point we hold references to all encaps and flows that are
	 * about to be updated, so it is safe to traverse them without any locks
	 * and call sleeping functions.
	 */
	list_for_each_entry_safe(e, tmp, &update_list, neigh_update_list) {
		encap_connected = !!(e->flags & MLX5_ENCAP_ENTRY_VALID);
		priv = netdev_priv(e->out_dev);

		if (encap_connected != neigh_connected ||
		    !ether_addr_equal(e->h_dest, ha))
			mlx5e_rep_update_flows(priv, e, neigh_connected, ha,
					       n_updated);
		mlx5e_encap_put(priv, e);
	}

	mlx5e_rep_neigh_entry_release(netdev_priv(nhe->m_neigh.dev), nhe);

	rtnl_unlock();
	neigh_release(n);
}

bool mlx5e_rep_queue_neigh_update_work(struct mlx5e_priv *priv,
				       struct mlx5e_neigh_hash_entry *nhe,
				       struct neighbour *n)
{
	/* Take a reference to ensure the neighbour and mlx5 encap
	 * entry won't be destructed until we drop the reference in
	 * delayed work.
	 */
	neigh_hold(n);

	/* This assignment is valid as long as the the neigh reference
	 * is taken
	 */
	nhe->n = n;

	if (!queue_work(priv->wq, &nhe->neigh_update_work)) {
		mlx5e_rep_neigh_entry_release(priv, nhe);
		neigh_release(n);
		return false;
	}
	return true;
}

static struct mlx5e_neigh_hash_entry *
mlx5e_rep_neigh_entry_lookup(struct mlx5e_priv *priv,
			     struct mlx5e_neigh *m_neigh);

static int mlx5e_rep_netevent_event(struct notifier_block *nb,
				    unsigned long event, void *ptr)
{
	struct mlx5e_rep_priv *rpriv = container_of(nb, struct mlx5e_rep_priv,
						    neigh_update.netevent_nb);
	struct mlx5e_neigh_update_table *neigh_update = &rpriv->neigh_update;
	struct net_device *netdev = rpriv->netdev;
	struct mlx5e_priv *priv = netdev_priv(netdev);
	struct mlx5e_neigh_hash_entry *nhe = NULL;
	struct mlx5e_neigh m_neigh = {};
#ifdef NETEVENT_DELAY_PROBE_TIME_UPDATE
	struct neigh_parms *p;
#endif
	struct neighbour *n;
#ifdef NETEVENT_DELAY_PROBE_TIME_UPDATE
	bool found = false;
#endif

	switch (event) {
	case NETEVENT_NEIGH_UPDATE:
		n = ptr;
#if IS_ENABLED(CONFIG_IPV6)
		if ((!ipv6_stub || !ipv6_stub->nd_tbl ||
		     n->tbl != ipv6_stub->nd_tbl) &&
		     n->tbl != &arp_tbl)
#else
		if (n->tbl != &arp_tbl)
#endif
			return NOTIFY_DONE;

		m_neigh.dev = n->dev;
#ifdef HAVE_TCF_TUNNEL_INFO
		m_neigh.family = n->ops->family;
#endif
		memcpy(&m_neigh.dst_ip, n->primary_key, n->tbl->key_len);

		rcu_read_lock();
		nhe = mlx5e_rep_neigh_entry_lookup(priv, &m_neigh);
		rcu_read_unlock();
		if (!nhe)
			return NOTIFY_DONE;

		mlx5e_rep_queue_neigh_update_work(priv, nhe, n);
		break;

#ifdef NETEVENT_DELAY_PROBE_TIME_UPDATE
	case NETEVENT_DELAY_PROBE_TIME_UPDATE:
		p = ptr;

		/* We check the device is present since we don't care about
		 * changes in the default table, we only care about changes
		 * done per device delay prob time parameter.
		 */
#if IS_ENABLED(CONFIG_IPV6)
		if (!p->dev ||
		    ((!ipv6_stub || !ipv6_stub->nd_tbl ||
		      p->tbl != ipv6_stub->nd_tbl) &&
		    p->tbl != &arp_tbl))
#else
		if (!p->dev || p->tbl != &arp_tbl)
#endif
			return NOTIFY_DONE;

		rcu_read_lock();
		list_for_each_entry_rcu(nhe, &neigh_update->neigh_list,
					neigh_list) {
			if (p->dev == nhe->m_neigh.dev) {
				found = true;
				break;
			}
		}
		rcu_read_unlock();
		if (!found)
			return NOTIFY_DONE;

		neigh_update->min_interval = min_t(unsigned long,
						   NEIGH_VAR(p, DELAY_PROBE_TIME),
						   neigh_update->min_interval);
		mlx5_fc_update_sampling_interval(priv->mdev,
						 neigh_update->min_interval);
		break;
#endif
	}
	return NOTIFY_DONE;
}
#endif /* HAVE_TCF_TUNNEL_INFO */

static const struct rhashtable_params mlx5e_neigh_ht_params = {
	.head_offset = offsetof(struct mlx5e_neigh_hash_entry, rhash_node),
	.key_offset = offsetof(struct mlx5e_neigh_hash_entry, m_neigh),
	.key_len = sizeof(struct mlx5e_neigh),
	.automatic_shrinking = true,
};

static int mlx5e_rep_neigh_init(struct mlx5e_rep_priv *rpriv)
{
	struct mlx5e_neigh_update_table *neigh_update = &rpriv->neigh_update;
	int err;

	err = rhashtable_init(&neigh_update->neigh_ht, &mlx5e_neigh_ht_params);
	if (err)
		return err;

	INIT_LIST_HEAD(&neigh_update->neigh_list);
#ifdef HAVE_TCF_TUNNEL_INFO
	spin_lock_init(&neigh_update->encap_lock);
	INIT_DELAYED_WORK(&neigh_update->neigh_stats_work,
			  mlx5e_rep_neigh_stats_work);
	mlx5e_rep_neigh_update_init_interval(rpriv);

	rpriv->neigh_update.netevent_nb.notifier_call = mlx5e_rep_netevent_event;
	err = register_netevent_notifier(&rpriv->neigh_update.netevent_nb);
	if (err)
		goto out_err;
	return 0;

out_err:
	rhashtable_destroy(&neigh_update->neigh_ht);
#endif
	return err;
}

static void mlx5e_rep_neigh_cleanup(struct mlx5e_rep_priv *rpriv)
{
	struct mlx5e_neigh_update_table *neigh_update = &rpriv->neigh_update;
#ifdef HAVE_TCF_TUNNEL_INFO
	struct mlx5e_priv *priv = netdev_priv(rpriv->netdev);

	unregister_netevent_notifier(&neigh_update->netevent_nb);

	flush_workqueue(priv->wq); /* flush neigh update works */

	cancel_delayed_work_sync(&rpriv->neigh_update.neigh_stats_work);
#endif

	rhashtable_destroy(&neigh_update->neigh_ht);
}

#ifdef HAVE_TCF_TUNNEL_INFO
static int mlx5e_rep_neigh_entry_insert(struct mlx5e_priv *priv,
					struct mlx5e_neigh_hash_entry *nhe)
{
	struct mlx5e_rep_priv *rpriv = priv->ppriv;
	int err;

	err = rhashtable_insert_fast(&rpriv->neigh_update.neigh_ht,
				     &nhe->rhash_node,
				     mlx5e_neigh_ht_params);
	if (err)
		return err;

	list_add_rcu(&nhe->neigh_list, &rpriv->neigh_update.neigh_list);

	return err;
}

static void mlx5e_rep_neigh_entry_remove(struct mlx5e_priv *priv,
					 struct mlx5e_neigh_hash_entry *nhe)
{
	struct mlx5e_rep_priv *rpriv = priv->ppriv;

#ifdef HAVE_TCF_TUNNEL_INFO
	spin_lock_bh(&rpriv->neigh_update.encap_lock);
#endif

	list_del_rcu(&nhe->neigh_list);

	rhashtable_remove_fast(&rpriv->neigh_update.neigh_ht,
			       &nhe->rhash_node,
			       mlx5e_neigh_ht_params);
#ifdef HAVE_TCF_TUNNEL_INFO
	spin_unlock_bh(&rpriv->neigh_update.encap_lock);
#endif
}

/* This function must only be called under RTNL lock or under the
 * representor's encap_lock in case RTNL mutex can't be held.
 */
static struct mlx5e_neigh_hash_entry *
mlx5e_rep_neigh_entry_lookup(struct mlx5e_priv *priv,
			     struct mlx5e_neigh *m_neigh)
{
	struct mlx5e_rep_priv *rpriv = priv->ppriv;
	struct mlx5e_neigh_update_table *neigh_update = &rpriv->neigh_update;
	struct mlx5e_neigh_hash_entry *nhe;

	nhe = rhashtable_lookup_fast(&neigh_update->neigh_ht, m_neigh,
				     mlx5e_neigh_ht_params);
	return nhe && mlx5e_rep_neigh_entry_hold(nhe) ? nhe : NULL;
}

static int mlx5e_rep_neigh_entry_create(struct mlx5e_priv *priv,
					struct mlx5e_encap_entry *e,
					struct mlx5e_neigh_hash_entry **nhe)
{
	int err;

	*nhe = kzalloc(sizeof(**nhe), GFP_ATOMIC);
	if (!*nhe)
		return -ENOMEM;

	memcpy(&(*nhe)->m_neigh, &e->m_neigh, sizeof(e->m_neigh));
	INIT_WORK(&(*nhe)->neigh_update_work, mlx5e_rep_neigh_update);
	spin_lock_init(&(*nhe)->encap_list_lock);
	INIT_LIST_HEAD(&(*nhe)->encap_list);
#ifdef HAVE_REFCOUNT
	refcount_set(&(*nhe)->refcnt, 1);
#else
	atomic_set(&(*nhe)->refcnt, 1);
#endif

	err = mlx5e_rep_neigh_entry_insert(priv, *nhe);
	if (err)
		goto out_free;
	return 0;

out_free:
	kfree(*nhe);
	return err;
}

int mlx5e_rep_encap_entry_attach(struct mlx5e_priv *priv,
				 struct mlx5e_encap_entry *e)
{
	struct mlx5e_rep_priv *rpriv = priv->ppriv;
	struct mlx5e_neigh_hash_entry *nhe;
	int err;

	spin_lock_bh(&rpriv->neigh_update.encap_lock);
	nhe = mlx5e_rep_neigh_entry_lookup(priv, &e->m_neigh);
	if (!nhe) {
		err = mlx5e_rep_neigh_entry_create(priv, e, &nhe);
		if (err) {
			spin_unlock_bh(&rpriv->neigh_update.encap_lock);
			return err;
		}
	}
	spin_unlock_bh(&rpriv->neigh_update.encap_lock);

	e->nhe = nhe;
	spin_lock(&nhe->encap_list_lock);
	list_add_rcu(&e->encap_list, &nhe->encap_list);
	spin_unlock(&nhe->encap_list_lock);

	return 0;
}

void mlx5e_rep_encap_entry_detach(struct mlx5e_priv *priv,
				  struct mlx5e_encap_entry *e)
{
	if (!e->nhe)
		return;

	spin_lock(&e->nhe->encap_list_lock);
	list_del_rcu(&e->encap_list);
	spin_unlock(&e->nhe->encap_list_lock);

	mlx5e_rep_neigh_entry_release(priv, e->nhe);
	e->nhe = NULL;
}
#endif /* HAVE_TCF_TUNNEL_INFO */

static int mlx5e_vf_rep_open(struct net_device *dev)
{
	struct mlx5e_priv *priv = netdev_priv(dev);
	struct mlx5e_rep_priv *rpriv = priv->ppriv;
	struct mlx5_eswitch_rep *rep = rpriv->rep;
	int err;

	mutex_lock(&priv->state_lock);
	err = mlx5e_open_locked(dev);
	if (err)
		goto unlock;

	if (!mlx5_modify_vport_admin_state(priv->mdev,
					   MLX5_VPORT_STATE_OP_MOD_ESW_VPORT,
					   rep->vport, 1,
					   MLX5_VPORT_ADMIN_STATE_UP))
		netif_carrier_on(dev);

unlock:
	mutex_unlock(&priv->state_lock);
	return err;
}

static int mlx5e_vf_rep_close(struct net_device *dev)
{
	struct mlx5e_priv *priv = netdev_priv(dev);
	struct mlx5e_rep_priv *rpriv = priv->ppriv;
	struct mlx5_eswitch_rep *rep = rpriv->rep;
	int ret;

	mutex_lock(&priv->state_lock);
	mlx5_modify_vport_admin_state(priv->mdev,
				      MLX5_VPORT_STATE_OP_MOD_ESW_VPORT,
				      rep->vport, 1,
				      MLX5_VPORT_ADMIN_STATE_DOWN);
	ret = mlx5e_close_locked(dev);
	mutex_unlock(&priv->state_lock);
	return ret;
}

#if defined(HAVE_NDO_GET_PHYS_PORT_NAME) || defined(HAVE_SWITCHDEV_H_COMPAT) || defined(HAVE_NDO_GET_PHYS_PORT_NAME_EXTENDED)
static int mlx5e_rep_get_phys_port_name(struct net_device *dev,
					char *buf, size_t len)
{
	struct mlx5e_priv *priv = netdev_priv(dev);
	struct mlx5e_rep_priv *rpriv = priv->ppriv;
	struct mlx5_eswitch_rep *rep = rpriv->rep;
	unsigned int fn;
	int ret;

	fn = PCI_FUNC(priv->mdev->pdev->devfn);
	if (fn >= MLX5_MAX_PORTS)
		return -EOPNOTSUPP;

	if (rep->vport == MLX5_VPORT_UPLINK)
		ret = snprintf(buf, len, "p%d", fn);
	else
		ret = snprintf(buf, len, "pf%dvf%d", fn, rep->vport - 1);

	if (ret >= len)
		return -EOPNOTSUPP;

	return 0;
}
#endif

#if defined(HAVE_TC_FLOWER_OFFLOAD)
static int
#if defined(HAVE_NDO_SETUP_TC_TAKES_TC_SETUP_TYPE) || defined(HAVE_NDO_SETUP_TC_RH_EXTENDED)
#ifdef HAVE_TC_BLOCK_OFFLOAD
mlx5e_rep_setup_tc_cls_flower(struct mlx5e_priv *priv,
#else
mlx5e_rep_setup_tc_cls_flower(struct net_device *dev,
#endif
			      struct tc_cls_flower_offload *cls_flower, int flags)
#else
mlx5e_rep_setup_tc_cls_flower(struct net_device *dev,
			      u32 handle,
#ifdef HAVE_NDO_SETUP_TC_TAKES_CHAIN_INDEX
			      u32 chain_index,
#endif
			      __be16 proto,
			      struct tc_to_netdev *tc, int flags)
#endif
{
#if !defined(HAVE_NDO_SETUP_TC_TAKES_TC_SETUP_TYPE) && !defined(HAVE_NDO_SETUP_TC_RH_EXTENDED)
	struct tc_cls_flower_offload *cls_flower = tc->cls_flower;
#endif

#ifndef HAVE_TC_CLS_CAN_OFFLOAD_AND_CHAIN0
#ifdef HAVE_TC_BLOCK_OFFLOAD
	if (cls_flower->common.chain_index)
#else
	struct mlx5e_priv *priv = netdev_priv(dev);
#if defined(HAVE_NDO_SETUP_TC_TAKES_TC_SETUP_TYPE) || defined(HAVE_NDO_SETUP_TC_RH_EXTENDED)
	if (!is_classid_clsact_ingress(cls_flower->common.classid) ||
	    cls_flower->common.chain_index)
#else
	if (TC_H_MAJ(handle) != TC_H_MAJ(TC_H_INGRESS) ||
#ifdef HAVE_NDO_SETUP_TC_TAKES_CHAIN_INDEX
	    chain_index)
#else
	    0)
#endif
#endif
#endif
		return -EOPNOTSUPP;
#endif

#if defined(HAVE_TC_TO_NETDEV_EGRESS_DEV) || defined(HAVE_TC_CLS_FLOWER_OFFLOAD_EGRESS_DEV)
#ifndef HAVE_TC_SETUP_CB_EGDEV_REGISTER
#if defined(HAVE_NDO_SETUP_TC_TAKES_TC_SETUP_TYPE) || defined(HAVE_NDO_SETUP_TC_RH_EXTENDED)
	if (cls_flower->egress_dev) {
#else
	if (tc->egress_dev) {
#endif
		struct mlx5_eswitch *esw = priv->mdev->priv.eswitch;
		struct mlx5e_rep_priv * uplink_rpriv = mlx5_eswitch_get_uplink_priv(esw, REP_ETH);
		struct net_device *uplink_dev = uplink_rpriv->netdev;

		if (uplink_dev != dev) {
#if defined(HAVE_NDO_SETUP_TC_TAKES_TC_SETUP_TYPE)
		return dev->netdev_ops->ndo_setup_tc(uplink_dev, TC_SETUP_CLSFLOWER,
						      cls_flower);
#elif defined(HAVE_NDO_SETUP_TC_RH_EXTENDED)
		return dev->netdev_ops->extended.ndo_setup_tc_rh(uplink_dev,
							 TC_SETUP_CLSFLOWER,
							 cls_flower);
#else
		return dev->netdev_ops->ndo_setup_tc(uplink_dev, handle,
#ifdef HAVE_NDO_SETUP_TC_TAKES_CHAIN_INDEX
						      chain_index,
#endif
						      proto, tc);
#endif
		}
	 }
#endif
#endif

	switch (cls_flower->command) {
	case TC_CLSFLOWER_REPLACE:
		return mlx5e_configure_flower(priv, cls_flower, flags);
	case TC_CLSFLOWER_DESTROY:
		return mlx5e_delete_flower(priv, cls_flower, flags);
#ifdef HAVE_TC_CLSFLOWER_STATS
	case TC_CLSFLOWER_STATS:
		return mlx5e_stats_flower(priv, cls_flower, flags);
#endif
	default:
		return -EOPNOTSUPP;
	}
}
#endif /* defined(HAVE_TC_FLOWER_OFFLOAD) */

#ifdef HAVE_TC_BLOCK_OFFLOAD
static int mlx5e_rep_setup_tc_cb_egdev(enum tc_setup_type type, void *type_data,
				       void *cb_priv)
{
	struct mlx5e_priv *priv = cb_priv;

#if defined(HAVE_TC_CLS_FLOWER_OFFLOAD_COMMON) && defined (HAVE_IS_TCF_GACT_GOTO_CHAIN)  && defined (HAVE_TC_CLS_CAN_OFFLOAD_AND_CHAIN0)
	if (!tc_cls_can_offload_and_chain0(priv->netdev, type_data))
		return -EOPNOTSUPP;
#endif
	switch (type) {
	case TC_SETUP_CLSFLOWER:
		return mlx5e_rep_setup_tc_cls_flower(priv, type_data, MLX5E_TC_EGRESS |
						     MLX5E_TC_ESW_OFFLOAD);
	default:
		return -EOPNOTSUPP;
	}
}

static int mlx5e_rep_setup_tc_cb(enum tc_setup_type type, void *type_data,
				 void *cb_priv)
{
	struct mlx5e_priv *priv = cb_priv;

#if defined(HAVE_TC_CLS_FLOWER_OFFLOAD_COMMON) && defined (HAVE_IS_TCF_GACT_GOTO_CHAIN) && defined (HAVE_TC_CLS_CAN_OFFLOAD_AND_CHAIN0)
	if (!tc_cls_can_offload_and_chain0(priv->netdev, type_data))
		return -EOPNOTSUPP;
#endif
	switch (type) {
	case TC_SETUP_CLSFLOWER:
		return mlx5e_rep_setup_tc_cls_flower(priv, type_data, MLX5E_TC_INGRESS |
						     MLX5E_TC_ESW_OFFLOAD);
	default:
		return -EOPNOTSUPP;
	}
}

static int mlx5e_rep_setup_tc_block(struct net_device *dev,
				    struct tc_block_offload *f)
{
	struct mlx5e_priv *priv = netdev_priv(dev);

	if (f->binder_type != TCF_BLOCK_BINDER_TYPE_CLSACT_INGRESS)
		return -EOPNOTSUPP;

	switch (f->command) {
	case TC_BLOCK_BIND:
		return tcf_block_cb_register(f->block, mlx5e_rep_setup_tc_cb,
#ifdef HAVE_TC_BLOCK_OFFLOAD_EXTACK
					     priv, priv, f->extack);
#else

					     priv, priv);
#endif
	case TC_BLOCK_UNBIND:
		tcf_block_cb_unregister(f->block, mlx5e_rep_setup_tc_cb, priv);
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}
#endif

#if defined(HAVE_TC_FLOWER_OFFLOAD)
#if defined(HAVE_NDO_SETUP_TC_TAKES_TC_SETUP_TYPE) || defined(HAVE_NDO_SETUP_TC_RH_EXTENDED)
static int mlx5e_rep_setup_tc(struct net_device *dev, enum tc_setup_type type,
			      void *type_data)
#else
static int mlx5e_rep_setup_tc(struct net_device *dev, u32 handle,
#ifdef HAVE_NDO_SETUP_TC_TAKES_CHAIN_INDEX
			      u32 chain_index, __be16 proto,
#else
			      __be16 proto,
#endif
			      struct tc_to_netdev *tc)
#endif
{
#if !defined(HAVE_NDO_SETUP_TC_TAKES_TC_SETUP_TYPE) && !defined(HAVE_NDO_SETUP_TC_RH_EXTENDED)
	unsigned int type = tc->type;
#endif

	switch (type) {
#ifdef HAVE_TC_BLOCK_OFFLOAD
	case TC_SETUP_BLOCK:
		return mlx5e_rep_setup_tc_block(dev, type_data);
#else
	case TC_SETUP_CLSFLOWER:
#if defined(HAVE_NDO_SETUP_TC_TAKES_TC_SETUP_TYPE) || defined(HAVE_NDO_SETUP_TC_RH_EXTENDED)
		return mlx5e_rep_setup_tc_cls_flower(dev, type_data, MLX5E_TC_INGRESS |
				MLX5E_TC_ESW_OFFLOAD);
#else
		return mlx5e_rep_setup_tc_cls_flower(dev, handle,
#ifdef HAVE_NDO_SETUP_TC_TAKES_CHAIN_INDEX
						     chain_index,
#endif
						     proto, tc, MLX5E_TC_INGRESS |
						     MLX5E_TC_ESW_OFFLOAD);
#endif
#endif
	default:
		return -EOPNOTSUPP;
	}
}
#endif

#if !defined(HAVE_TC_BLOCK_OFFLOAD) && defined(HAVE_TC_SETUP_CB_EGDEV_REGISTER)
static int mlx5e_rep_setup_tc_cb(enum tc_setup_type type, void *type_data,
				 void *cb_priv)
{
	struct net_device *dev = cb_priv;

	return mlx5e_setup_tc(dev, type, type_data);
}
#endif

bool mlx5e_is_uplink_rep(struct mlx5e_priv *priv)
{
	struct mlx5e_rep_priv *rpriv = priv->ppriv;
	struct mlx5_eswitch_rep *rep;

	if (!MLX5_ESWITCH_MANAGER(priv->mdev))
		return false;

	if (!rpriv) /* non vport rep mlx5e instances don't use this field */
		return false;

	rep = rpriv->rep;
	return (rep->vport == MLX5_VPORT_UPLINK);
}

#if defined(NDO_HAS_OFFLOAD_STATS_GETS_NET_DEVICE) || defined(HAVE_NDO_HAS_OFFLOAD_STATS_EXTENDED)
static bool mlx5e_rep_has_offload_stats(const struct net_device *dev, int attr_id)
{
	switch (attr_id) {
	case IFLA_OFFLOAD_XSTATS_CPU_HIT:
			return true;
	}

	return false;
}
#endif

#if defined(HAVE_NDO_GET_OFFLOAD_STATS) || defined(HAVE_NDO_GET_OFFLOAD_STATS_EXTENDED)
static int
mlx5e_get_sw_stats64(const struct net_device *dev,
		     struct rtnl_link_stats64 *stats)
{
	struct mlx5e_priv *priv = netdev_priv(dev);
	struct mlx5e_sw_stats *sstats = &priv->stats.sw;

	stats->rx_packets = sstats->rx_packets;
	stats->rx_bytes   = sstats->rx_bytes;
	stats->tx_packets = sstats->tx_packets;
	stats->tx_bytes   = sstats->tx_bytes;

	stats->tx_dropped = sstats->tx_queue_dropped;

	return 0;
}

static int mlx5e_rep_get_offload_stats(int attr_id, const struct net_device *dev,
				       void *sp)
{
	switch (attr_id) {
	case IFLA_OFFLOAD_XSTATS_CPU_HIT:
		return mlx5e_get_sw_stats64(dev, sp);
	}

	return -EINVAL;
}
#endif /* defined(HAVE_NDO_GET_OFFLOAD_STATS) || defined(HAVE_NDO_GET_OFFLOAD_STATS_EXTENDED) */

static
#ifdef HAVE_NDO_GET_STATS64_RET_VOID
void mlx5e_rep_get_stats(struct net_device *dev, struct rtnl_link_stats64 *stats)
#elif defined(HAVE_NDO_GET_STATS64)
struct rtnl_link_stats64 * mlx5e_rep_get_stats(struct net_device *dev, struct rtnl_link_stats64 *stats)
#else
struct net_device_stats * mlx5e_rep_get_stats(struct net_device *dev)
#endif
{
	struct mlx5e_priv *priv = netdev_priv(dev);
#if !defined(HAVE_NDO_GET_STATS64) && !defined(HAVE_NDO_GET_STATS64_RET_VOID)
	struct net_device_stats *stats = &priv->netdev_stats;
#endif

	memcpy(stats, &priv->stats.vf_vport, sizeof(*stats));

#ifndef HAVE_NDO_GET_STATS64_RET_VOID
	return stats;
#endif
}

static int mlx5e_vf_rep_change_mtu(struct net_device *netdev, int new_mtu)
{
	return mlx5e_change_mtu(netdev, new_mtu, NULL);
}

static int mlx5e_uplink_rep_change_mtu(struct net_device *netdev, int new_mtu)
{
	return mlx5e_change_mtu(netdev, new_mtu, mlx5e_set_dev_port_mtu);
}

static int mlx5e_uplink_rep_set_mac(struct net_device *netdev, void *addr)
{
	struct sockaddr *saddr = addr;

	if (!is_valid_ether_addr(saddr->sa_data))
		return -EADDRNOTAVAIL;

	ether_addr_copy(netdev->dev_addr, saddr->sa_data);
	return 0;
}

#ifdef HAVE_VF_VLAN_PROTO
static int mlx5e_uplink_rep_set_vf_vlan(struct net_device *dev, int vf, u16 vlan, u8 qos,
					__be16 vlan_proto)
#else
static int mlx5e_uplink_rep_set_vf_vlan(struct net_device *dev, int vf, u16 vlan, u8 qos)
#endif
{
	netdev_warn_once(dev, "legacy vf vlan setting isn't supported in switchdev mode\n");

	if (vlan != 0)
		return -EOPNOTSUPP;

	/* allow setting 0-vid for compatibility with libvirt */
	return 0;
}

#ifdef HAVE_SWITCHDEV_OPS
#ifdef CONFIG_NET_SWITCHDEV
static const struct switchdev_ops mlx5e_rep_switchdev_ops = {
	.switchdev_port_attr_get	= mlx5e_attr_get,
};
#endif
#endif

static const struct net_device_ops mlx5e_netdev_ops_vf_rep = {
	.ndo_open                = mlx5e_vf_rep_open,
	.ndo_stop                = mlx5e_vf_rep_close,
	.ndo_start_xmit          = mlx5e_xmit,
#ifdef HAVE_NET_DEVICE_OPS_EXTENDED
	.ndo_size                = sizeof(struct net_device_ops),
#endif
#ifdef HAVE_NDO_GET_PHYS_PORT_NAME
	.ndo_get_phys_port_name  = mlx5e_rep_get_phys_port_name,
#elif defined(HAVE_NDO_GET_PHYS_PORT_NAME_EXTENDED)
	.extended.ndo_get_phys_port_name = mlx5e_rep_get_phys_port_name,
#endif
#if defined(HAVE_TC_FLOWER_OFFLOAD)
#ifdef HAVE_NDO_SETUP_TC_RH_EXTENDED
	.extended.ndo_setup_tc_rh = mlx5e_rep_setup_tc,
#else
	.ndo_setup_tc            = mlx5e_rep_setup_tc,
#endif
#endif
#if defined(HAVE_NDO_GET_STATS64) || defined(HAVE_NDO_GET_STATS64_RET_VOID)
	.ndo_get_stats64         = mlx5e_rep_get_stats,
#else
	.ndo_get_stats           = mlx5e_rep_get_stats,
#endif
#ifdef NDO_HAS_OFFLOAD_STATS_GETS_NET_DEVICE
	.ndo_has_offload_stats	 = mlx5e_rep_has_offload_stats,
#elif defined(HAVE_NDO_HAS_OFFLOAD_STATS_EXTENDED)
	.extended.ndo_has_offload_stats   = mlx5e_rep_has_offload_stats,
#endif
#ifdef HAVE_NDO_GET_OFFLOAD_STATS
	.ndo_get_offload_stats	 = mlx5e_rep_get_offload_stats,
#elif defined(HAVE_NDO_GET_OFFLOAD_STATS_EXTENDED)
	.extended.ndo_get_offload_stats   = mlx5e_rep_get_offload_stats,
#endif
#ifdef HAVE_NDO_CHANGE_MTU_EXTENDED
	.extended.ndo_change_mtu = mlx5e_vf_rep_change_mtu,
#else
	.ndo_change_mtu          = mlx5e_vf_rep_change_mtu,
#endif
};

static const struct net_device_ops mlx5e_netdev_ops_uplink_rep = {
	.ndo_open                = mlx5e_open,
	.ndo_stop                = mlx5e_close,
	.ndo_start_xmit          = mlx5e_xmit,
	.ndo_set_mac_address     = mlx5e_uplink_rep_set_mac,
#ifdef HAVE_NET_DEVICE_OPS_EXTENDED
	.ndo_size                = sizeof(struct net_device_ops),
#endif
#ifdef HAVE_NDO_GET_PHYS_PORT_NAME
	.ndo_get_phys_port_name  = mlx5e_rep_get_phys_port_name,
#elif defined(HAVE_NDO_GET_PHYS_PORT_NAME_EXTENDED)
	.extended.ndo_get_phys_port_name = mlx5e_rep_get_phys_port_name,
#endif
#if defined(HAVE_TC_FLOWER_OFFLOAD)
#ifdef HAVE_NDO_SETUP_TC_RH_EXTENDED
	.extended.ndo_setup_tc_rh = mlx5e_rep_setup_tc,
#else
	.ndo_setup_tc            = mlx5e_rep_setup_tc,
#endif
#endif
#if defined(HAVE_NDO_GET_STATS64) || defined(HAVE_NDO_GET_STATS64_RET_VOID)
	.ndo_get_stats64         = mlx5e_get_stats,
#else
	.ndo_get_stats           = mlx5e_get_stats,
#endif
#ifdef NDO_HAS_OFFLOAD_STATS_GETS_NET_DEVICE
	.ndo_has_offload_stats	 = mlx5e_rep_has_offload_stats,
#elif defined(HAVE_NDO_HAS_OFFLOAD_STATS_EXTENDED)
	.extended.ndo_has_offload_stats   = mlx5e_rep_has_offload_stats,
#endif
#ifdef HAVE_NDO_GET_OFFLOAD_STATS
	.ndo_get_offload_stats	 = mlx5e_rep_get_offload_stats,
#elif defined(HAVE_NDO_GET_OFFLOAD_STATS_EXTENDED)
	.extended.ndo_get_offload_stats   = mlx5e_rep_get_offload_stats,
#endif
#ifdef HAVE_NDO_CHANGE_MTU_EXTENDED
	.extended.ndo_change_mtu = mlx5e_uplink_rep_change_mtu,
#else
	.ndo_change_mtu          = mlx5e_uplink_rep_change_mtu,
#endif

#ifdef HAVE_KERNEL_WITH_VXLAN_SUPPORT_ON
#ifdef HAVE_NDO_UDP_TUNNEL_ADD
	.ndo_udp_tunnel_add      = mlx5e_add_vxlan_port,
	.ndo_udp_tunnel_del      = mlx5e_del_vxlan_port,
#elif defined(HAVE_NDO_UDP_TUNNEL_ADD_EXTENDED)
	.extended.ndo_udp_tunnel_add = mlx5e_add_vxlan_port,
	.extended.ndo_udp_tunnel_del = mlx5e_del_vxlan_port,
#elif defined(HAVE_NDO_ADD_VXLAN_PORT)
	.ndo_add_vxlan_port	 = mlx5e_add_vxlan_port,
	.ndo_del_vxlan_port	 = mlx5e_del_vxlan_port,
#endif
#endif
#ifdef HAVE_NETDEV_FEATURES_T
	.ndo_features_check      = mlx5e_features_check,
#elif defined(HAVE_KERNEL_WITH_VXLAN_SUPPORT_ON) && defined(HAVE_VXLAN_GSO_CHECK)
	.ndo_gso_check           = mlx5e_gso_check,
#endif

#ifdef HAVE_NDO_SET_VF_MAC
	.ndo_set_vf_mac          = mlx5e_set_vf_mac,
#endif
#ifdef HAVE_NDO_SET_VF_MAC
#ifdef HAVE_VF_TX_RATE_LIMITS
	.ndo_set_vf_rate         = mlx5e_set_vf_rate,
#else
	.ndo_set_vf_tx_rate      = mlx5e_set_vf_rate,
#endif
#endif
#ifdef HAVE_NDO_SET_VF_MAC
	.ndo_get_vf_config       = mlx5e_get_vf_config,
#endif
#ifdef HAVE_NDO_GET_VF_STATS
	.ndo_get_vf_stats        = mlx5e_get_vf_stats,
#endif
#if defined(HAVE_NDO_SET_VF_VLAN)
	.ndo_set_vf_vlan         = mlx5e_uplink_rep_set_vf_vlan,
#elif defined(HAVE_NDO_SET_VF_VLAN_EXTENDED)
	.extended.ndo_set_vf_vlan  = mlx5e_uplink_rep_set_vf_vlan,
#endif
};

static void mlx5e_build_rep_params(struct net_device *netdev)
{
	struct mlx5e_priv *priv = netdev_priv(netdev);
	struct mlx5e_rep_priv *rpriv = priv->ppriv;
	struct mlx5_eswitch_rep *rep = rpriv->rep;
	struct mlx5_core_dev *mdev = priv->mdev;
	struct mlx5e_params *params;

	u8 cq_period_mode = MLX5_CAP_GEN(mdev, cq_period_start_from_cqe) ?
					 MLX5_CQ_PERIOD_MODE_START_FROM_CQE :
					 MLX5_CQ_PERIOD_MODE_START_FROM_EQE;

	params = &priv->channels.params;
	params->hard_mtu    = MLX5E_ETH_HARD_MTU;
	params->sw_mtu      = netdev->mtu;

	/* SQ */
	if (rep->vport == MLX5_VPORT_UPLINK)
		params->log_sq_size = MLX5E_PARAMS_DEFAULT_LOG_SQ_SIZE;
	else
		params->log_sq_size = MLX5E_REP_PARAMS_LOG_SQ_SIZE;

	/* RQ */
	mlx5e_build_rq_params(mdev, params);

	/* CQ moderation params */
	params->rx_dim_enabled = MLX5_CAP_GEN(mdev, cq_moderation);
	mlx5e_set_rx_cq_mode_params(params, cq_period_mode);

	params->num_tc                = 1;
	params->tunneled_stateless_offload = false;
	mlx5_query_min_inline(mdev, &params->tx_min_inline_mode);

	MLX5E_SET_PFLAG(params, MLX5E_PFLAG_PER_CH_STATS, true);

#ifdef HAVE_ETH_SS_RSS_HASH_FUNCS
	/* RSS */
	mlx5e_build_rss_params(params);
#endif
}

static void mlx5e_build_rep_netdev(struct net_device *netdev)
{
	struct mlx5e_priv *priv = netdev_priv(netdev);
	struct mlx5e_rep_priv *rpriv = priv->ppriv;
	struct mlx5_eswitch_rep *rep = rpriv->rep;
	struct mlx5_core_dev *mdev = priv->mdev;

	if (rep->vport == MLX5_VPORT_UPLINK) {
		SET_NETDEV_DEV(netdev, &priv->mdev->pdev->dev);
		netdev->netdev_ops = &mlx5e_netdev_ops_uplink_rep;
		/* we want a persistent mac for the uplink rep */
		mlx5_query_nic_vport_mac_address(mdev, 0, netdev->dev_addr);
		netdev->ethtool_ops = &mlx5e_uplink_rep_ethtool_ops;
#ifdef HAVE_IEEE_DCBNL_ETS
#ifdef CONFIG_MLX5_CORE_EN_DCB
		if (MLX5_CAP_GEN(mdev, qos))
			netdev->dcbnl_ops = &mlx5e_dcbnl_ops;
#endif
#endif
	} else {
		netdev->netdev_ops = &mlx5e_netdev_ops_vf_rep;
		eth_hw_addr_random(netdev);
		netdev->ethtool_ops = &mlx5e_vf_rep_ethtool_ops;
	}

	netdev->watchdog_timeo    = 15 * HZ;


#ifdef HAVE_SWITCHDEV_OPS
#ifdef CONFIG_NET_SWITCHDEV
	netdev->switchdev_ops = &mlx5e_rep_switchdev_ops;
#endif
#endif

#ifdef HAVE_TC_FLOWER_OFFLOAD
	netdev->features	 |= NETIF_F_HW_TC | NETIF_F_NETNS_LOCAL;
#else
	netdev->features	 |= NETIF_F_NETNS_LOCAL;
#endif
#ifdef HAVE_NETDEV_HW_FEATURES
#ifdef HAVE_TC_FLOWER_OFFLOAD
	netdev->hw_features      |= NETIF_F_HW_TC;
#endif

	netdev->hw_features    |= NETIF_F_SG;
	netdev->hw_features    |= NETIF_F_IP_CSUM;
	netdev->hw_features    |= NETIF_F_IPV6_CSUM;
	netdev->hw_features    |= NETIF_F_GRO;
	netdev->hw_features    |= NETIF_F_TSO;
	netdev->hw_features    |= NETIF_F_TSO6;
	netdev->hw_features    |= NETIF_F_RXCSUM;

	netdev->features |= netdev->hw_features;
#endif

	if (rep->vport != MLX5_VPORT_UPLINK)
		netdev->features |= NETIF_F_VLAN_CHALLENGED;
}

static int mlx5e_init_rep(struct mlx5_core_dev *mdev,
			  struct net_device *netdev,
			  const struct mlx5e_profile *profile,
			  void *ppriv)
{
	struct mlx5e_priv *priv = netdev_priv(netdev);
	int err;

	err = mlx5e_netdev_init(netdev, priv, mdev, profile, ppriv);
	if (err)
		return err;

	priv->channels.params.num_channels = MLX5E_REP_PARAMS_DEF_NUM_CHANNELS;

	mlx5e_build_rep_params(netdev);
	mlx5e_build_rep_netdev(netdev);

	mlx5e_timestamp_init(priv);

	return 0;
}

static void mlx5e_cleanup_rep(struct mlx5e_priv *priv)
{
	mlx5e_netdev_cleanup(priv->netdev, priv);
}

static int mlx5e_create_rep_ttc_table(struct mlx5e_priv *priv)
{
	struct ttc_params ttc_params = {};
	int tt, err;

	priv->fs.ns = mlx5_get_flow_namespace(priv->mdev,
					      MLX5_FLOW_NAMESPACE_KERNEL);

	/* The inner_ttc in the ttc params is intentionally not set */
	ttc_params.any_tt_tirn = priv->direct_tir[0].tirn;
	mlx5e_set_ttc_ft_params(&ttc_params);
	for (tt = 0; tt < MLX5E_NUM_INDIR_TIRS; tt++)
		ttc_params.indir_tirn[tt] = priv->indir_tir[tt].tirn;

	err = mlx5e_create_ttc_table(priv, &ttc_params, &priv->fs.ttc);
	if (err) {
		netdev_err(priv->netdev, "Failed to create rep ttc table, err=%d\n", err);
		return err;
	}
	return 0;
}

static int mlx5e_create_rep_vport_rx_rule(struct mlx5e_priv *priv)
{
	struct mlx5_eswitch *esw = priv->mdev->priv.eswitch;
	struct mlx5e_rep_priv *rpriv = priv->ppriv;
	struct mlx5_eswitch_rep *rep = rpriv->rep;
	struct mlx5_flow_handle *flow_rule;
	struct mlx5_flow_destination dest;

	dest.type = MLX5_FLOW_DESTINATION_TYPE_TIR;
	dest.tir_num = priv->direct_tir[0].tirn;
	flow_rule = mlx5_eswitch_create_vport_rx_rule(esw,
						      rep->vport,
						      &dest);
	if (IS_ERR(flow_rule))
		return PTR_ERR(flow_rule);
	rpriv->vport_rx_rule = flow_rule;
	return 0;
}

static int mlx5e_init_rep_rx(struct mlx5e_priv *priv)
{
	struct mlx5_core_dev *mdev = priv->mdev;
	int err;

#ifdef CONFIG_BF_DEVICE_EMULATION
	if (mlx5_core_is_dev_emulation_manager(mdev))
		return 0;
#endif

	mlx5e_init_l2_addr(priv);

	err = mlx5e_open_drop_rq(priv, &priv->drop_rq);
	if (err) {
		mlx5_core_err(mdev, "open drop rq failed, %d\n", err);
		return err;
	}

	err = mlx5e_create_indirect_rqt(priv);
	if (err)
		goto err_close_drop_rq;

	err = mlx5e_create_direct_rqts(priv);
	if (err)
		goto err_destroy_indirect_rqts;

	err = mlx5e_create_indirect_tirs(priv, false);
	if (err)
		goto err_destroy_direct_rqts;

	err = mlx5e_create_direct_tirs(priv);
	if (err)
		goto err_destroy_indirect_tirs;

	err = mlx5e_create_rep_ttc_table(priv);
	if (err)
		goto err_destroy_direct_tirs;

	err = mlx5e_create_rep_vport_rx_rule(priv);
	if (err)
		goto err_destroy_ttc_table;

	return 0;

err_destroy_ttc_table:
	mlx5e_destroy_ttc_table(priv, &priv->fs.ttc);
err_destroy_direct_tirs:
	mlx5e_destroy_direct_tirs(priv);
err_destroy_indirect_tirs:
	mlx5e_destroy_indirect_tirs(priv, false);
err_destroy_direct_rqts:
	mlx5e_destroy_direct_rqts(priv);
err_destroy_indirect_rqts:
	mlx5e_destroy_rqt(priv, &priv->indir_rqt);
err_close_drop_rq:
	mlx5e_close_drop_rq(&priv->drop_rq);
	return err;
}

static void mlx5e_cleanup_rep_rx(struct mlx5e_priv *priv)
{
	struct mlx5e_rep_priv *rpriv = priv->ppriv;

#ifdef CONFIG_BF_DEVICE_EMULATION
	if (mlx5_core_is_dev_emulation_manager(priv->mdev))
		return;
#endif

	mlx5_del_flow_rules(rpriv->vport_rx_rule);
	mlx5e_destroy_ttc_table(priv, &priv->fs.ttc);
	mlx5e_destroy_direct_tirs(priv);
	mlx5e_destroy_indirect_tirs(priv, false);
	mlx5e_destroy_direct_rqts(priv);
	mlx5e_destroy_rqt(priv, &priv->indir_rqt);
	mlx5e_close_drop_rq(&priv->drop_rq);
}

static int mlx5e_init_rep_tx(struct mlx5e_priv *priv)
{
	struct mlx5e_rep_priv *rpriv = priv->ppriv;
	int tc, err;

	err = mlx5e_create_tises(priv);
	if (err) {
		mlx5_core_warn(priv->mdev, "create tises failed, %d\n", err);
		return err;
	}

	if (rpriv->rep->vport == MLX5_VPORT_UPLINK) {
		INIT_LIST_HEAD(&rpriv->unready_flows);

		/* init shared tc flow table */
		spin_lock_init(&rpriv->tc_ht_lock);
		err = mlx5e_tc_esw_init(&rpriv->tc_ht);
		if (err)
			goto destroy_tises;
	}

	return 0;

destroy_tises:
	for (tc = 0; tc < priv->profile->max_tc; tc++)
		mlx5e_destroy_tis(priv->mdev, priv->tisn[tc]);
	return err;
}

static void mlx5e_cleanup_rep_tx(struct mlx5e_priv *priv)
{
	struct mlx5e_rep_priv *rpriv = priv->ppriv;
	int tc;

	for (tc = 0; tc < priv->profile->max_tc; tc++)
		mlx5e_destroy_tis(priv->mdev, priv->tisn[tc]);

	if (rpriv->rep->vport == MLX5_VPORT_UPLINK) {
		/* delete shared tc flow table */
		mlx5e_tc_esw_cleanup(&rpriv->tc_ht);
	}
}

static void mlx5e_vf_rep_enable(struct mlx5e_priv *priv)
{
#if defined(HAVE_NET_DEVICE_MIN_MAX_MTU) || defined(HAVE_NET_DEVICE_MIN_MAX_MTU_EXTENDED)
	struct net_device *netdev = priv->netdev;
	struct mlx5_core_dev *mdev = priv->mdev;
	u16 max_mtu;
#endif

#if defined(HAVE_NET_DEVICE_MIN_MAX_MTU)
	netdev->min_mtu = ETH_MIN_MTU;
	mlx5_query_port_max_mtu(mdev, &max_mtu, 1);
	netdev->max_mtu = MLX5E_HW2SW_MTU(&priv->channels.params, max_mtu);
#elif defined(HAVE_NET_DEVICE_MIN_MAX_MTU_EXTENDED)
	netdev->extended->min_mtu = ETH_MIN_MTU;
	mlx5_query_port_max_mtu(mdev, &max_mtu, 1);
	netdev->extended->max_mtu = MLX5E_HW2SW_MTU(&priv->channels.params, max_mtu);
#endif

	if (priv->profile->update_stats)
		queue_delayed_work(priv->wq, &priv->update_stats_work, 0);
}

static void mlx5e_vf_rep_disable(struct mlx5e_priv *priv)
{
	cancel_delayed_work_sync(&priv->update_stats_work);
}

static void mlx5e_uplink_rep_enable(struct mlx5e_priv *priv)
{
	struct net_device *netdev = priv->netdev;
	struct mlx5_core_dev *mdev = priv->mdev;
#if defined(HAVE_NET_DEVICE_MIN_MAX_MTU) || defined(HAVE_NET_DEVICE_MIN_MAX_MTU_EXTENDED)
	u16 max_mtu;
#endif

#if defined(HAVE_NET_DEVICE_MIN_MAX_MTU)
	netdev->min_mtu = ETH_MIN_MTU;
	mlx5_query_port_max_mtu(priv->mdev, &max_mtu, 1);
	netdev->max_mtu = MLX5E_HW2SW_MTU(&priv->channels.params, max_mtu);
#elif defined(HAVE_NET_DEVICE_MIN_MAX_MTU_EXTENDED)
	netdev->extended->min_mtu = ETH_MIN_MTU;
	mlx5_query_port_max_mtu(mdev, &max_mtu, 1);
	netdev->extended->max_mtu = MLX5E_HW2SW_MTU(&priv->channels.params, max_mtu);
#endif
	mlx5e_set_dev_port_mtu(priv);

	mlx5_lag_add(mdev, netdev);
	mlx5e_enable_async_events(priv);
#ifdef HAVE_IEEE_DCBNL_ETS
#ifdef CONFIG_MLX5_CORE_EN_DCB
	mlx5e_dcbnl_initialize(priv);
	mlx5e_dcbnl_init_app(priv);
#endif
#endif

	if (priv->profile->update_stats)
		queue_delayed_work(priv->wq, &priv->update_stats_work, 0);
}

static void mlx5e_uplink_rep_disable(struct mlx5e_priv *priv)
{
	struct mlx5_core_dev *mdev = priv->mdev;

	cancel_delayed_work_sync(&priv->update_stats_work);
#ifdef HAVE_IEEE_DCBNL_ETS
#ifdef CONFIG_MLX5_CORE_EN_DCB
	mlx5e_dcbnl_delete_app(priv);
#endif
#endif
	mlx5e_disable_async_events(priv);
	mlx5_lag_remove(mdev);
}

static const struct mlx5e_profile mlx5e_vf_rep_profile = {
	.init			= mlx5e_init_rep,
	.cleanup		= mlx5e_cleanup_rep,
	.init_rx		= mlx5e_init_rep_rx,
	.cleanup_rx		= mlx5e_cleanup_rep_rx,
	.init_tx		= mlx5e_init_rep_tx,
	.cleanup_tx		= mlx5e_cleanup_rep_tx,
	.enable		        = mlx5e_vf_rep_enable,
	.disable	        = mlx5e_vf_rep_disable,
	.update_stats           = mlx5e_rep_update_stats,
	.rx_handlers.handle_rx_cqe       = mlx5e_handle_rx_cqe_rep,
	.rx_handlers.handle_rx_cqe_mpwqe = mlx5e_handle_rx_cqe_mpwrq,
	.max_tc			= 1,
};

static const struct mlx5e_profile mlx5e_uplink_rep_profile = {
	.init			= mlx5e_init_rep,
	.cleanup		= mlx5e_cleanup_rep,
	.init_rx		= mlx5e_init_rep_rx,
	.cleanup_rx		= mlx5e_cleanup_rep_rx,
	.init_tx		= mlx5e_init_rep_tx,
	.cleanup_tx		= mlx5e_cleanup_rep_tx,
	.enable		        = mlx5e_uplink_rep_enable,
	.disable	        = mlx5e_uplink_rep_disable,
	.update_stats           = mlx5e_rep_update_stats,
	.update_carrier	        = mlx5e_update_carrier,
	.rx_handlers.handle_rx_cqe       = mlx5e_handle_rx_cqe_rep,
	.rx_handlers.handle_rx_cqe_mpwqe = mlx5e_handle_rx_cqe_mpwrq,
	.max_tc			= MLX5E_MAX_NUM_TC,
};

/* e-Switch vport representors */
#ifdef HAVE_SWITCHDEV_H_COMPAT
static inline int dev_isalive(const struct net_device *dev)
{
	return dev->reg_state <= NETREG_REGISTERED;
}

static ssize_t phys_port_name_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct net_device *netdev = to_net_dev(dev);
	ssize_t ret = -EINVAL;

	if (!rtnl_trylock())
		return restart_syscall();

	if (dev_isalive(netdev)) {
		char name[IFNAMSIZ];

		ret = mlx5e_rep_get_phys_port_name(netdev, name, sizeof(name));
		if (!ret)
			ret = sprintf(buf, "%s\n", name);
	}
	rtnl_unlock();

	return ret;
}

ssize_t phys_switch_id_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct net_device *netdev = to_net_dev(dev);
	ssize_t ret = -EINVAL;

	if (!rtnl_trylock())
		return restart_syscall();

	if (dev_isalive(netdev)) {
		struct switchdev_attr attr = {
			.orig_dev = netdev,
			.id = SWITCHDEV_ATTR_ID_PORT_PARENT_ID,
			.flags = SWITCHDEV_F_NO_RECURSE,
		};

		ret = mlx5e_attr_get(netdev, &attr);
		if (!ret)
			ret = sprintf(buf, "%*phN\n", attr.u.ppid.id_len,
				      attr.u.ppid.id);
	}
	rtnl_unlock();

	return ret;
}

static DEVICE_ATTR(phys_port_name, S_IRUGO, phys_port_name_show, NULL);
static DEVICE_ATTR(phys_switch_id, S_IRUGO, phys_switch_id_show, NULL);

static struct attribute *rep_sysfs_attrs[] = {
	&dev_attr_phys_port_name.attr,
	&dev_attr_phys_switch_id.attr,
	NULL,
};

static struct attribute_group rep_sysfs_attr_group = {
	.attrs = rep_sysfs_attrs,
};
#endif /* HAVE_SWITCHDEV_H_COMPAT */

static int
mlx5e_vport_rep_load(struct mlx5_core_dev *dev, struct mlx5_eswitch_rep *rep)
{
#ifdef HAVE_TC_BLOCK_OFFLOAD
	struct mlx5e_rep_priv *uplink_rpriv;
#endif
	const struct mlx5e_profile *profile;
	struct mlx5e_rep_priv *rpriv;
	struct net_device *netdev;
#ifdef HAVE_TC_BLOCK_OFFLOAD
	struct mlx5e_priv *upriv;
#endif
	int nch, err;

	rpriv = kzalloc(sizeof(*rpriv), GFP_KERNEL);
	if (!rpriv)
		return -ENOMEM;

	/* rpriv->rep to be looked up when profile->init() is called */
	rpriv->rep = rep;

	nch = mlx5e_get_max_num_channels(dev);
	profile = (rep->vport == MLX5_VPORT_UPLINK) ? &mlx5e_uplink_rep_profile : &mlx5e_vf_rep_profile;
	netdev = mlx5e_create_netdev(dev, profile, nch, rpriv);
	if (!netdev) {
		pr_warn("Failed to create representor netdev for vport %d\n",
			rep->vport);
		kfree(rpriv);
		return -EINVAL;
	}

	rpriv->netdev = netdev;
	rep->rep_if[REP_ETH].priv = rpriv;
	INIT_LIST_HEAD(&rpriv->vport_sqs_list);

	if (rep->vport == MLX5_VPORT_UPLINK) {
		err = mlx5e_create_mdev_resources(dev);
		if (err)
			goto err_destroy_netdev;
	}

	err = mlx5e_attach_netdev(netdev_priv(netdev));
	if (err) {
		pr_warn("Failed to attach representor netdev for vport %d\n",
			rep->vport);
		goto err_destroy_mdev_resources;
	}

	err = mlx5e_rep_neigh_init(rpriv);
	if (err) {
		pr_warn("Failed to initialized neighbours handling for vport %d\n",
			rep->vport);
		goto err_detach_netdev;
	}

#ifdef HAVE_TC_SETUP_CB_EGDEV_REGISTER
	uplink_rpriv = mlx5_eswitch_get_uplink_priv(dev->priv.eswitch, REP_ETH);
#ifdef HAVE_TC_BLOCK_OFFLOAD
	upriv = netdev_priv(uplink_rpriv->netdev);
	err = tc_setup_cb_egdev_register(netdev, mlx5e_rep_setup_tc_cb_egdev,
					 upriv);
#else
	err = tc_setup_cb_egdev_register(netdev, mlx5e_rep_setup_tc_cb,
					 uplink_rpriv->netdev);
#endif
	if (err)
		goto err_neigh_cleanup;
#endif

#ifdef HAVE_SWITCHDEV_H_COMPAT
	if (!netdev->sysfs_groups[0]) {
		netdev->sysfs_groups[0] = &rep_sysfs_attr_group;
	}
#endif

	err = register_netdev(netdev);
	if (err) {
		pr_warn("Failed to register representor netdev for vport %d\n",
			rep->vport);
		goto err_egdev_cleanup;
	}

	if (rep->vport == MLX5_VPORT_UPLINK) {
		mlx5_smartnic_sysfs_init(netdev);
		mlx5_eswitch_compat_sysfs_init(netdev);
	}

	return 0;

err_egdev_cleanup:
#ifdef HAVE_TC_SETUP_CB_EGDEV_REGISTER
#ifdef HAVE_TC_BLOCK_OFFLOAD
	tc_setup_cb_egdev_unregister(netdev, mlx5e_rep_setup_tc_cb_egdev,
				     upriv);
#else
	tc_setup_cb_egdev_unregister(netdev, mlx5e_rep_setup_tc_cb,
				     uplink_rpriv->netdev);
#endif

err_neigh_cleanup:
#endif
	mlx5e_rep_neigh_cleanup(rpriv);

err_detach_netdev:
	mlx5e_detach_netdev(netdev_priv(netdev));

err_destroy_mdev_resources:
	if (rep->vport == MLX5_VPORT_UPLINK)
		mlx5e_destroy_mdev_resources(dev);

err_destroy_netdev:
	mlx5e_destroy_netdev(netdev_priv(netdev));
	kfree(rpriv);
	return err;
}

static void
mlx5e_vport_rep_unload(struct mlx5_eswitch_rep *rep)
{
	struct mlx5e_rep_priv *rpriv = mlx5e_rep_to_rep_priv(rep);
	struct net_device *netdev = rpriv->netdev;
	struct mlx5e_priv *priv = netdev_priv(netdev);
#ifdef HAVE_TC_BLOCK_OFFLOAD
	struct mlx5e_rep_priv *uplink_rpriv;
#endif
	void *ppriv = priv->ppriv;
#ifdef HAVE_TC_BLOCK_OFFLOAD
	struct mlx5e_priv *upriv;
#endif

	if (rep->vport == MLX5_VPORT_UPLINK) {
		mlx5_eswitch_compat_sysfs_cleanup(netdev);
		mlx5_smartnic_sysfs_cleanup(netdev);
	}

	unregister_netdev(netdev);
#ifdef HAVE_TC_SETUP_CB_EGDEV_REGISTER
	uplink_rpriv = mlx5_eswitch_get_uplink_priv(priv->mdev->priv.eswitch,
						    REP_ETH);
#ifdef HAVE_TC_BLOCK_OFFLOAD
	upriv = netdev_priv(uplink_rpriv->netdev);
	tc_setup_cb_egdev_unregister(netdev, mlx5e_rep_setup_tc_cb_egdev,
				     upriv);
#endif
#endif
	mlx5e_rep_neigh_cleanup(rpriv);
	mlx5e_detach_netdev(priv);
	if (rep->vport == MLX5_VPORT_UPLINK)
		mlx5e_destroy_mdev_resources(priv->mdev);
	mlx5e_destroy_netdev(priv);
	kfree(ppriv); /* mlx5e_rep_priv */
}

static void *mlx5e_vport_rep_get_proto_dev(struct mlx5_eswitch_rep *rep)
{
	struct mlx5e_rep_priv *rpriv;

	rpriv = mlx5e_rep_to_rep_priv(rep);

	return rpriv->netdev;
}

void mlx5e_rep_register_vport_reps(struct mlx5_core_dev *mdev)
{
	struct mlx5_eswitch *esw = mdev->priv.eswitch;
	struct mlx5_eswitch_rep_if rep_if = {};

	rep_if.load = mlx5e_vport_rep_load;
	rep_if.unload = mlx5e_vport_rep_unload;
	rep_if.get_proto_dev = mlx5e_vport_rep_get_proto_dev;

	mlx5_eswitch_register_vport_reps(esw, &rep_if, REP_ETH);
}

void mlx5e_rep_unregister_vport_reps(struct mlx5_core_dev *mdev)
{
	struct mlx5_eswitch *esw = mdev->priv.eswitch;

	mlx5_eswitch_unregister_vport_reps(esw, REP_ETH);
}

bool mlx5e_eswitch_rep(struct net_device *netdev)
{
       if (netdev->netdev_ops == &mlx5e_netdev_ops_vf_rep ||
           netdev->netdev_ops == &mlx5e_netdev_ops_uplink_rep)
               return true;

       return false;
}
