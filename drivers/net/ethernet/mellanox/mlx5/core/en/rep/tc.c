// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/* Copyright (c) 2020 Mellanox Technologies. */

#include <net/dst_metadata.h>
#include <net/psample.h>
#include <linux/netdevice.h>
#include <linux/list.h>
#include <linux/rculist.h>
#include <linux/rtnetlink.h>
#include <linux/workqueue.h>
#include <linux/spinlock.h>
#include "tc.h"
#include "neigh.h"
#include "en_rep.h"
#include "eswitch.h"
#include "lib/fs_chains.h"
#include "en/tc_ct.h"
#include "en/mapping.h"
#include "en/tc_tun.h"
#include "lib/port_tun.h"

struct mlx5e_rep_indr_block_priv {
	flow_setup_cb_t *cb;
	struct net_device *netdev;
	struct mlx5e_rep_priv *rpriv;
	int binder_type;

	struct list_head list;
};

int mlx5e_rep_encap_entry_attach(struct mlx5e_priv *priv,
				 struct mlx5e_encap_entry *e,
				 struct mlx5e_neigh *m_neigh,
				 struct net_device *neigh_dev)
{
	struct mlx5e_rep_priv *rpriv = priv->ppriv;
	struct mlx5_rep_uplink_priv *uplink_priv = &rpriv->uplink_priv;
	struct mlx5_tun_entropy *tun_entropy = &uplink_priv->tun_entropy;
	struct mlx5e_neigh_hash_entry *nhe;
	int err;

	err = mlx5_tun_entropy_refcount_inc(tun_entropy, e->reformat_type);
	if (err)
		return err;

	mutex_lock(&rpriv->neigh_update.encap_lock);
	nhe = mlx5e_rep_neigh_entry_lookup(priv, m_neigh);
	if (!nhe) {
		err = mlx5e_rep_neigh_entry_create(priv, m_neigh, neigh_dev, &nhe);
		if (err) {
			mutex_unlock(&rpriv->neigh_update.encap_lock);
			mlx5_tun_entropy_refcount_dec(tun_entropy,
						      e->reformat_type);
			return err;
		}
	}

	e->nhe = nhe;
	spin_lock(&nhe->encap_list_lock);
	list_add_rcu(&e->encap_list, &nhe->encap_list);
	spin_unlock(&nhe->encap_list_lock);

	mutex_unlock(&rpriv->neigh_update.encap_lock);

	return 0;
}

void mlx5e_rep_encap_entry_detach(struct mlx5e_priv *priv,
				  struct mlx5e_encap_entry *e)
{
	struct mlx5e_rep_priv *rpriv = priv->ppriv;
	struct mlx5_rep_uplink_priv *uplink_priv = &rpriv->uplink_priv;
	struct mlx5_tun_entropy *tun_entropy = &uplink_priv->tun_entropy;

	if (!e->nhe)
		return;

	spin_lock(&e->nhe->encap_list_lock);
	list_del_rcu(&e->encap_list);
	spin_unlock(&e->nhe->encap_list_lock);

	mlx5e_rep_neigh_entry_release(e->nhe);
	e->nhe = NULL;
	mlx5_tun_entropy_refcount_dec(tun_entropy, e->reformat_type);
}

void mlx5e_rep_update_flows(struct mlx5e_priv *priv,
			    struct mlx5e_encap_entry *e,
			    bool neigh_connected,
			    unsigned char ha[ETH_ALEN])
{
	struct ethhdr *eth = (struct ethhdr *)e->encap_header;
	struct mlx5_eswitch *esw = priv->mdev->priv.eswitch;
	bool encap_connected;
	LIST_HEAD(flow_list);

	ASSERT_RTNL();

	/* wait for encap to be fully initialized */
	wait_for_completion(&e->res_ready);

	mutex_lock(&esw->offloads.encap_tbl_lock);
	encap_connected = !!(e->flags & MLX5_ENCAP_ENTRY_VALID);
	if (e->compl_result < 0 || (encap_connected == neigh_connected &&
				    ether_addr_equal(e->h_dest, ha)))
		goto unlock;

	mlx5e_take_all_encap_flows(e, &flow_list);

	if ((e->flags & MLX5_ENCAP_ENTRY_VALID) &&
	    (!neigh_connected || !ether_addr_equal(e->h_dest, ha)))
		mlx5e_tc_encap_flows_del(priv, e, &flow_list);

	if (neigh_connected && !(e->flags & MLX5_ENCAP_ENTRY_VALID)) {
		ether_addr_copy(e->h_dest, ha);
		ether_addr_copy(eth->h_dest, ha);
		/* Update the encap source mac, in case that we delete
		 * the flows when encap source mac changed.
		 */
		ether_addr_copy(eth->h_source, e->route_dev->dev_addr);

		mlx5e_tc_encap_flows_add(priv, e, &flow_list);
	}
unlock:
	mutex_unlock(&esw->offloads.encap_tbl_lock);
	mlx5e_put_flow_list(priv, &flow_list);
}

#if defined(HAVE_TC_FLOWER_OFFLOAD) || defined(HAVE_FLOW_CLS_OFFLOAD)
static int
#if defined(HAVE_NDO_SETUP_TC_TAKES_TC_SETUP_TYPE) || defined(HAVE_NDO_SETUP_TC_RH_EXTENDED)
#if defined( HAVE_TC_BLOCK_OFFLOAD) || defined(HAVE_FLOW_BLOCK_OFFLOAD)
mlx5e_rep_setup_tc_cls_flower(struct mlx5e_priv *priv,
#else
mlx5e_rep_setup_tc_cls_flower(struct net_device *dev,
#endif
			      struct flow_cls_offload *cls_flower, int flags)
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
#if !defined(HAVE_NDO_SETUP_TC_RH_EXTENDED) || defined(HAVE_TC_CLS_FLOWER_OFFLOAD_EGRESS_DEV)
#if defined(HAVE_NDO_SETUP_TC_TAKES_TC_SETUP_TYPE) || defined(HAVE_NDO_SETUP_TC_RH_EXTENDED)
	if (cls_flower->egress_dev) {
#else
	if (tc->egress_dev) {
#endif
		struct mlx5_eswitch *esw = priv->mdev->priv.eswitch;
		struct mlx5e_rep_priv * uplink_rpriv = mlx5_eswitch_get_uplink_priv(esw, REP_ETH);
		struct net_device *uplink_dev = uplink_rpriv->netdev;
		int err;
#if defined(HAVE_TC_BLOCK_OFFLOAD) && \
    (defined(HAVE_NDO_SETUP_TC_TAKES_TC_SETUP_TYPE) || \
     defined(HAVE_NDO_SETUP_TC_RH_EXTENDED))
		struct net_device *dev = priv->netdev;
#endif

		flags = (flags & (~MLX5_TC_FLAG(INGRESS))) | MLX5_TC_FLAG(EGRESS);

		if (uplink_dev != dev) {
#if defined(HAVE_NDO_SETUP_TC_TAKES_TC_SETUP_TYPE)
		err = dev->netdev_ops->ndo_setup_tc(uplink_dev, TC_SETUP_CLSFLOWER,
						      cls_flower);
#elif defined(HAVE_NDO_SETUP_TC_RH_EXTENDED)
		err = dev->netdev_ops->extended.ndo_setup_tc_rh(uplink_dev,
							 TC_SETUP_CLSFLOWER,
							 cls_flower);

#else
		err = dev->netdev_ops->ndo_setup_tc(uplink_dev, handle,
#ifdef HAVE_NDO_SETUP_TC_TAKES_CHAIN_INDEX
						      chain_index,
#endif
						      proto, tc);
#endif
		return err;
		}
	 }
#endif /* !HAVE_NDO_SETUP_TC_RH_EXTENDED || HAVE_TC_CLS_FLOWER_OFFLOAD_EGRESS_DEV */
#endif /* !HAVE_TC_SETUP_CB_EGDEV_REGISTER */
#endif /* HAVE_TC_TO_NETDEV_EGRESS_DEV || HAVE_TC_CLS_FLOWER_OFFLOAD_EGRESS_DEV */

	switch (cls_flower->command) {
	case FLOW_CLS_REPLACE:
		return mlx5e_configure_flower(priv->netdev, priv, cls_flower,
					      flags);
	case FLOW_CLS_DESTROY:
		return mlx5e_delete_flower(priv->netdev, priv, cls_flower,
					   flags);
#ifdef HAVE_TC_CLSFLOWER_STATS
	case FLOW_CLS_STATS:
		return mlx5e_stats_flower(priv->netdev, priv, cls_flower,
					  flags);
#endif
	default:
		return -EOPNOTSUPP;
	}
}
#endif /* defined(HAVE_TC_FLOWER_OFFLOAD) */

#ifdef HAVE_TC_CLSMATCHALL_STATS
static
int mlx5e_rep_setup_tc_cls_matchall(struct mlx5e_priv *priv,
				    struct tc_cls_matchall_offload *ma)
{
	switch (ma->command) {
	case TC_CLSMATCHALL_REPLACE:
		return mlx5e_tc_configure_matchall(priv, ma);
	case TC_CLSMATCHALL_DESTROY:
		return mlx5e_tc_delete_matchall(priv, ma);
	case TC_CLSMATCHALL_STATS:
		mlx5e_tc_stats_matchall(priv, ma);
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}
#endif /* HAVE_TC_CLSMATCHALL_STATS */

#if defined(HAVE_TC_BLOCK_OFFLOAD) || defined(HAVE_FLOW_CLS_OFFLOAD)

static int mlx5e_rep_setup_tc_cb(enum tc_setup_type type, void *type_data,
				 void *cb_priv)
{
	unsigned long flags = MLX5_TC_FLAG(INGRESS) | MLX5_TC_FLAG(ESW_OFFLOAD);
	struct mlx5e_priv *priv = cb_priv;

	switch (type) {
	case TC_SETUP_CLSFLOWER:
		return mlx5e_rep_setup_tc_cls_flower(priv, type_data, flags);
#ifdef HAVE_TC_CLSMATCHALL_STATS
	case TC_SETUP_CLSMATCHALL:
		return mlx5e_rep_setup_tc_cls_matchall(priv, type_data);
#endif
	default:
		return -EOPNOTSUPP;
	}
}

#ifdef HAVE_FLOW_CLS_OFFLOAD
static LIST_HEAD(mlx5e_rep_block_cb_list);
#endif

#ifndef HAVE_FLOW_BLOCK_CB_SETUP_SIMPLE
static int mlx5e_rep_setup_tc_block(struct net_device *dev,
				    struct tc_block_offload *f)
{
	struct mlx5e_priv *priv = netdev_priv(dev);
#ifdef HAVE_FLOW_CLS_OFFLOAD
	struct flow_block_cb *block_cb;
#endif

	if (f->binder_type != FLOW_BLOCK_BINDER_TYPE_CLSACT_INGRESS)
		return -EOPNOTSUPP;

#ifdef HAVE_FLOW_CLS_OFFLOAD
	f->driver_block_list = &mlx5e_rep_block_cb_list;
#endif

	switch (f->command) {
	case TC_BLOCK_BIND:
#ifdef HAVE_FLOW_CLS_OFFLOAD
		block_cb = flow_block_cb_alloc(mlx5e_rep_setup_tc_cb, priv, priv, NULL);
#else
		return tcf_block_cb_register(f->block, mlx5e_rep_setup_tc_cb,
#ifdef HAVE_TC_BLOCK_OFFLOAD_EXTACK
					     priv, priv, f->extack);
#else

					     priv, priv);
#endif
#endif /* HAVE_FLOW_CLS_OFFLOAD */
#ifdef HAVE_FLOW_CLS_OFFLOAD
                if (IS_ERR(block_cb)) {
                        return -ENOENT;
                }
                flow_block_cb_add(block_cb, f);
                list_add_tail(&block_cb->driver_list, f->driver_block_list);
                return 0;
#endif
	case TC_BLOCK_UNBIND:
#ifndef HAVE_FLOW_CLS_OFFLOAD
		tcf_block_cb_unregister(f->block, mlx5e_rep_setup_tc_cb, priv);
#else
		block_cb = flow_block_cb_lookup(f->block, mlx5e_rep_setup_tc_cb, priv);
		if (!block_cb)
			return -ENOENT;

		flow_block_cb_remove(block_cb, f);
		list_del(&block_cb->driver_list);
#endif
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}
#endif /* HAVE_FLOW_BLOCK_CB_SETUP_SIMPLE */
#endif /* HAVE_TC_BLOCK_OFFLOAD */

#if defined(HAVE_TC_FLOWER_OFFLOAD) || defined(HAVE_FLOW_CLS_OFFLOAD)
#ifdef HAVE_FLOW_BLOCK_CB_SETUP_SIMPLE
static int mlx5e_rep_setup_tc_e2e_cb(enum tc_setup_type type, void *type_data,
				     void *cb_priv)
{
	unsigned long flags = MLX5_TC_FLAG(INGRESS) |
			      MLX5_TC_FLAG(ESW_OFFLOAD) |
			      MLX5_TC_FLAG(E2E_OFFLOAD);
	struct mlx5e_priv *priv = cb_priv;

	if (type != TC_SETUP_CLSFLOWER)
		return -EOPNOTSUPP;

	return mlx5e_rep_setup_tc_cls_flower(priv, type_data, flags);
}
#endif
#endif

#ifdef HAVE_TC_SETUP_FT
static int mlx5e_rep_setup_ft_cb(enum tc_setup_type type, void *type_data,
				 void *cb_priv)
{
	struct flow_cls_offload tmp, *f = type_data;
	struct mlx5e_priv *priv = cb_priv;
	struct mlx5_eswitch *esw;
	unsigned long flags;
	int err;

	flags = MLX5_TC_FLAG(INGRESS) |
		MLX5_TC_FLAG(ESW_OFFLOAD) |
		MLX5_TC_FLAG(FT_OFFLOAD);
	esw = priv->mdev->priv.eswitch;

	switch (type) {
	case TC_SETUP_CLSFLOWER:
		memcpy(&tmp, f, sizeof(*f));

		if (!mlx5_chains_prios_supported(esw_chains(esw)))
			return -EOPNOTSUPP;

		/* Re-use tc offload path by moving the ft flow to the
		 * reserved ft chain.
		 *
		 * FT offload can use prio range [0, INT_MAX], so we normalize
		 * it to range [1, mlx5_esw_chains_get_prio_range(esw)]
		 * as with tc, where prio 0 isn't supported.
		 *
		 * We only support chain 0 of FT offload.
		 */
		if (tmp.common.prio >= mlx5_chains_get_prio_range(esw_chains(esw)))
			return -EOPNOTSUPP;
		if (tmp.common.chain_index != 0)
			return -EOPNOTSUPP;

		tmp.common.chain_index = mlx5_chains_get_nf_ft_chain(esw_chains(esw));
		tmp.common.prio++;
		err = mlx5e_rep_setup_tc_cls_flower(priv, &tmp, flags);
		memcpy(&f->stats, &tmp.stats, sizeof(f->stats));
		return err;
	default:
		return -EOPNOTSUPP;
	}
}
#endif

#if defined(HAVE_TC_FLOWER_OFFLOAD) || defined(HAVE_FLOW_CLS_OFFLOAD)
static LIST_HEAD(mlx5e_rep_block_tc_cb_list);
static LIST_HEAD(mlx5e_rep_block_ft_cb_list);
#if defined(HAVE_NDO_SETUP_TC_TAKES_TC_SETUP_TYPE) || defined(HAVE_NDO_SETUP_TC_RH_EXTENDED)
int mlx5e_rep_setup_tc(struct net_device *dev, enum tc_setup_type tc_setup_type,
		       void *type_data)
#else
int mlx5e_rep_setup_tc(struct net_device *dev, u32 handle,
#ifdef HAVE_NDO_SETUP_TC_TAKES_CHAIN_INDEX
		       u32 chain_index, __be16 proto,
#else
		       __be16 proto,
#endif
		       struct tc_to_netdev *tc)
#endif
{
#ifdef HAVE_FLOW_BLOCK_CB_SETUP_SIMPLE
	struct mlx5e_priv *priv = netdev_priv(dev);
#endif
#if !defined(HAVE_NDO_SETUP_TC_TAKES_TC_SETUP_TYPE) && !defined(HAVE_NDO_SETUP_TC_RH_EXTENDED)
	unsigned int type = tc->type;
#else
	int type = (int) tc_setup_type;
#endif
#if !defined(HAVE_TC_BLOCK_OFFLOAD) && ! defined(HAVE_FLOW_BLOCK_OFFLOAD)
	unsigned long flags = MLX5_TC_FLAG(INGRESS) | MLX5_TC_FLAG(ESW_OFFLOAD);
#endif
#ifdef HAVE_UNLOCKED_DRIVER_CB
	struct flow_block_offload *f = type_data;

	f->unlocked_driver_cb = true;
#endif

	switch (type) {
#if defined(HAVE_TC_BLOCK_OFFLOAD) || defined(HAVE_FLOW_BLOCK_OFFLOAD)
	case TC_SETUP_BLOCK:
#ifdef HAVE_FLOW_BLOCK_CB_SETUP_SIMPLE
		return flow_block_cb_setup_simple(type_data,
						  &mlx5e_rep_block_tc_cb_list,
						  mlx5e_rep_setup_tc_cb,
						  priv, priv, true);
#else
		return mlx5e_rep_setup_tc_block(dev, type_data);
#endif /* HAVE_FLOW_BLOCK_CB_SETUP_SIMPLE */
#else /* HAVE_TC_BLOCK_OFFLOAD || HAVE_FLOW_BLOCK_OFFLOAD */
	case TC_SETUP_CLSFLOWER:
#if defined(HAVE_NDO_SETUP_TC_TAKES_TC_SETUP_TYPE) || defined(HAVE_NDO_SETUP_TC_RH_EXTENDED)
		return mlx5e_rep_setup_tc_cls_flower(dev, type_data, flags);
#else
		return mlx5e_rep_setup_tc_cls_flower(dev, handle,
#ifdef HAVE_NDO_SETUP_TC_TAKES_CHAIN_INDEX
						     chain_index,
#endif /* HAVE_NDO_SETUP_TC_TAKES_CHAIN_INDEX */
						     proto, tc, flags);
#endif /* HAVE_NDO_SETUP_TC_TAKES_TC_SETUP_TYPE || HAVE_NDO_SETUP_TC_RH_EXTENDED */
#endif /* HAVE_TC_BLOCK_OFFLOAD || HAVE_FLOW_BLOCK_OFFLOAD */
#ifdef HAVE_FLOW_BLOCK_CB_SETUP_SIMPLE
	case TC_SETUP_E2E_BLOCK:
		return flow_block_cb_setup_simple(type_data,
						  &mlx5e_rep_block_tc_cb_list,
						  mlx5e_rep_setup_tc_e2e_cb,
						  priv, priv, false);
#endif
#ifdef HAVE_TC_SETUP_FT
	case TC_SETUP_FT:
		return flow_block_cb_setup_simple(type_data,
						  &mlx5e_rep_block_ft_cb_list,
						  mlx5e_rep_setup_ft_cb,
						  priv, priv, true);
#endif /* HAVE_TC_SETUP_FT */
	default:
		return -EOPNOTSUPP;
	}
}
#endif

#ifdef HAVE_TC_SETUP_CB_EGDEV_REGISTER
#ifdef HAVE_TC_BLOCK_OFFLOAD
int mlx5e_rep_setup_tc_cb_egdev(enum tc_setup_type type, void *type_data,
				void *cb_priv)
{
	unsigned long flags = MLX5_TC_FLAG(EGRESS) | MLX5_TC_FLAG(ESW_OFFLOAD);
	struct mlx5e_priv *priv = cb_priv;

#ifdef HAVE_TC_INDR_API
	/* some rhel kernels have indirect offload and egdev,
	 * so dont use egdev. e.g. rhel8.0
	 */
	return -EOPNOTSUPP;
#endif

	switch (type) {
	case TC_SETUP_CLSFLOWER:
		return mlx5e_rep_setup_tc_cls_flower(priv, type_data, flags);
	default:
		return -EOPNOTSUPP;
	}
}
#else
int mlx5e_rep_setup_tc_cb(enum tc_setup_type type, void *type_data,
			  void *cb_priv)
{
	struct net_device *dev = cb_priv;

	return mlx5e_setup_tc(dev, type, type_data);
}
#endif
#endif

int mlx5e_rep_tc_init(struct mlx5e_rep_priv *rpriv)
{
	struct mlx5_rep_uplink_priv *uplink_priv = &rpriv->uplink_priv;
	int err;

	mutex_init(&uplink_priv->unready_flows_lock);
	INIT_LIST_HEAD(&uplink_priv->unready_flows);

	/* init shared tc flow table */
	err = mlx5e_tc_esw_init(&uplink_priv->tc_ht);
	return err;
}

void mlx5e_rep_tc_cleanup(struct mlx5e_rep_priv *rpriv)
{
	/* delete shared tc flow table */
	mlx5e_tc_esw_cleanup(&rpriv->uplink_priv.tc_ht);
	mutex_destroy(&rpriv->uplink_priv.unready_flows_lock);
}

void mlx5e_rep_tc_enable(struct mlx5e_priv *priv)
{
	struct mlx5e_rep_priv *rpriv = priv->ppriv;

	INIT_WORK(&rpriv->uplink_priv.reoffload_flows_work,
		  mlx5e_tc_reoffload_flows_work);
}

void mlx5e_rep_tc_disable(struct mlx5e_priv *priv)
{
	struct mlx5e_rep_priv *rpriv = priv->ppriv;

	cancel_work_sync(&rpriv->uplink_priv.reoffload_flows_work);
}

int mlx5e_rep_tc_event_port_affinity(struct mlx5e_priv *priv)
{
	struct mlx5e_rep_priv *rpriv = priv->ppriv;

	queue_work(priv->wq, &rpriv->uplink_priv.reoffload_flows_work);

	return NOTIFY_OK;
}

#if defined( HAVE_TC_BLOCK_OFFLOAD) || defined(HAVE_FLOW_BLOCK_OFFLOAD)
static struct mlx5e_rep_indr_block_priv *
mlx5e_rep_indr_block_priv_lookup(struct mlx5e_rep_priv *rpriv,
				 struct net_device *netdev,
				 int binder_type)
{
	struct mlx5e_rep_indr_block_priv *cb_priv;

	/* All callback list access should be protected by RTNL. */
	ASSERT_RTNL();

	list_for_each_entry(cb_priv,
			    &rpriv->uplink_priv.tc_indr_block_priv_list,
			    list)
		if (cb_priv->netdev == netdev &&
		    cb_priv->binder_type == binder_type)
			return cb_priv;

	return NULL;
}

static int
mlx5e_rep_indr_offload(struct net_device *netdev,
		       struct flow_cls_offload *flower,
		       struct mlx5e_rep_indr_block_priv *indr_priv,
		       unsigned long flags)
{
	struct mlx5e_priv *priv = netdev_priv(indr_priv->rpriv->netdev);
	int err = 0;

	switch (flower->command) {
	case FLOW_CLS_REPLACE:
		err = mlx5e_configure_flower(netdev, priv, flower, flags);
		break;
	case FLOW_CLS_DESTROY:
		err = mlx5e_delete_flower(netdev, priv, flower, flags);
		break;
	case FLOW_CLS_STATS:
		err = mlx5e_stats_flower(netdev, priv, flower, flags);
		break;
	default:
		err = -EOPNOTSUPP;
	}

	return err;
}

static int mlx5e_rep_indr_setup_tc_cb(enum tc_setup_type type,
				      void *type_data, void *indr_priv)
{
	unsigned long flags = MLX5_TC_FLAG(ESW_OFFLOAD);
	struct mlx5e_rep_indr_block_priv *priv = indr_priv;

	flags |= (priv->binder_type == FLOW_BLOCK_BINDER_TYPE_CLSACT_EGRESS) ?
		MLX5_TC_FLAG(EGRESS) :
		MLX5_TC_FLAG(INGRESS);

	switch (type) {
	case TC_SETUP_CLSFLOWER:
		return mlx5e_rep_indr_offload(priv->netdev, type_data, priv,
					      flags);
	default:
		return -EOPNOTSUPP;
	}
}

static int mlx5e_rep_indr_setup_block_e2e_cb(enum tc_setup_type type,
					     void *type_data, void *indr_priv)
{
	unsigned long flags = MLX5_TC_FLAG(EGRESS) |
			      MLX5_TC_FLAG(ESW_OFFLOAD) |
			      MLX5_TC_FLAG(E2E_OFFLOAD);
	struct mlx5e_rep_indr_block_priv *priv = indr_priv;

	if (type != TC_SETUP_CLSFLOWER)
		return -EOPNOTSUPP;

	return mlx5e_rep_indr_offload(priv->netdev, type_data, priv, flags);
}

#ifdef HAVE_TC_SETUP_FT
static int mlx5e_rep_indr_setup_ft_cb(enum tc_setup_type type,
				      void *type_data, void *indr_priv)
{
	struct mlx5e_rep_indr_block_priv *priv = indr_priv;
	struct flow_cls_offload *f = type_data;
	struct flow_cls_offload tmp;
	struct mlx5e_priv *mpriv;
	struct mlx5_eswitch *esw;
	unsigned long flags;
	int err;

	mpriv = netdev_priv(priv->rpriv->netdev);
	esw = mpriv->mdev->priv.eswitch;

	flags = MLX5_TC_FLAG(EGRESS) |
		MLX5_TC_FLAG(ESW_OFFLOAD) |
		MLX5_TC_FLAG(FT_OFFLOAD);

	switch (type) {
	case TC_SETUP_CLSFLOWER:
		memcpy(&tmp, f, sizeof(*f));

		/* Re-use tc offload path by moving the ft flow to the
		 * reserved ft chain.
		 *
		 * FT offload can use prio range [0, INT_MAX], so we normalize
		 * it to range [1, mlx5_esw_chains_get_prio_range(esw)]
		 * as with tc, where prio 0 isn't supported.
		 *
		 * We only support chain 0 of FT offload.
		 */
		if (!mlx5_chains_prios_supported(esw_chains(esw)) ||
		    tmp.common.prio >= mlx5_chains_get_prio_range(esw_chains(esw)) ||
		    tmp.common.chain_index)
			return -EOPNOTSUPP;

		tmp.common.chain_index = mlx5_chains_get_nf_ft_chain(esw_chains(esw));
		tmp.common.prio++;
		err = mlx5e_rep_indr_offload(priv->netdev, &tmp, priv, flags);
		memcpy(&f->stats, &tmp.stats, sizeof(f->stats));
		return err;
	default:
		return -EOPNOTSUPP;
	}
}
#endif

#ifdef HAVE_FLOW_BLOCK_CB_ALLOC
static void mlx5e_rep_indr_block_unbind(void *cb_priv)
{
	struct mlx5e_rep_indr_block_priv *indr_priv = cb_priv;

	list_del(&indr_priv->list);
	kfree(indr_priv);
}
#endif

static LIST_HEAD(mlx5e_block_cb_list);

static int
mlx5e_rep_indr_setup_block(struct net_device *netdev, struct Qdisc *sch,
			   struct mlx5e_rep_priv *rpriv,
			   struct flow_block_offload *f,
			   flow_setup_cb_t *setup_cb,
			   void *data,
			   void (*cleanup)(struct flow_block_cb *block_cb))
{
	struct mlx5e_priv *priv = netdev_priv(rpriv->netdev);
	struct mlx5e_rep_indr_block_priv *indr_priv;
#ifdef HAVE_FLOW_BLOCK_CB_ALLOC
	struct flow_block_cb *block_cb;
#else
	int err = 0;
#endif

	if (!mlx5e_tc_tun_device_to_offload(priv, netdev) &&
	    !(is_vlan_dev(netdev) && vlan_dev_real_dev(netdev) == rpriv->netdev) &&
	    !netif_is_ovs_master(netdev))
		return -EOPNOTSUPP;

	if (f->binder_type != FLOW_BLOCK_BINDER_TYPE_CLSACT_INGRESS &&
	    f->binder_type != FLOW_BLOCK_BINDER_TYPE_CLSACT_INGRESS_E2E &&
	    (f->binder_type == FLOW_BLOCK_BINDER_TYPE_CLSACT_EGRESS &&
	     !netif_is_ovs_master(netdev)))
		return -EOPNOTSUPP;

#ifdef HAVE_UNLOCKED_DRIVER_CB
	f->unlocked_driver_cb = true;
#endif

#ifdef HAVE_FLOW_BLOCK_OFFLOAD
	f->driver_block_list = &mlx5e_block_cb_list;
#endif

	switch (f->command) {
	case FLOW_BLOCK_BIND:
		indr_priv = mlx5e_rep_indr_block_priv_lookup(rpriv, netdev, f->binder_type);
		if (indr_priv)
			return -EEXIST;

		indr_priv = kmalloc(sizeof(*indr_priv), GFP_KERNEL);
		if (!indr_priv)
			return -ENOMEM;

		indr_priv->netdev = netdev;
		indr_priv->rpriv = rpriv;
		indr_priv->binder_type = f->binder_type;

		list_add(&indr_priv->list,
			 &rpriv->uplink_priv.tc_indr_block_priv_list);

#ifdef HAVE_FLOW_BLOCK_CB_ALLOC
#ifdef HAVE_FLOW_INDR_BLOCK_CB_ALLOC
		block_cb = flow_indr_block_cb_alloc(setup_cb, indr_priv, indr_priv,
						    mlx5e_rep_indr_block_unbind,
						    f, netdev, sch, data, rpriv,
						    cleanup);
#else
		block_cb = flow_block_cb_alloc(setup_cb, indr_priv, indr_priv,
					       mlx5e_rep_indr_block_unbind);
#endif
		if (IS_ERR(block_cb)) {
			list_del(&indr_priv->list);
			kfree(indr_priv);
			return PTR_ERR(block_cb);
		}
		flow_block_cb_add(block_cb, f);
		list_add_tail(&block_cb->driver_list, &mlx5e_block_cb_list);

		return 0;
#else
		err = tcf_block_cb_register(f->block,
					    mlx5e_rep_indr_setup_tc_cb,
					    indr_priv, indr_priv
#ifdef HAVE_TC_BLOCK_OFFLOAD_EXTACK
					    , f->extack
#endif
					   );
		if (err) {
			list_del(&indr_priv->list);
			kfree(indr_priv);
		}

		return err;
#endif

	case FLOW_BLOCK_UNBIND:
		indr_priv = mlx5e_rep_indr_block_priv_lookup(rpriv, netdev, f->binder_type);
		if (!indr_priv)
			return -ENOENT;

#ifdef HAVE_FLOW_BLOCK_CB_ALLOC
		block_cb = flow_block_cb_lookup(f->block, setup_cb, indr_priv);
		if (!block_cb)
			return -ENOENT;

		flow_indr_block_cb_remove(block_cb, f);
		list_del(&block_cb->driver_list);
#else
		tcf_block_cb_unregister(f->block,
					mlx5e_rep_indr_setup_tc_cb,
					indr_priv);
		list_del(&indr_priv->list);
		kfree(indr_priv);
#endif

		return 0;
	default:
		return -EOPNOTSUPP;
	}
	return 0;
}

static
#ifdef HAVE_FLOW_INDR_BLOCK_BIND_CB_T_7_PARAMS
int mlx5e_rep_indr_setup_cb(struct net_device *netdev, struct Qdisc *sch, void *cb_priv,
#else
int mlx5e_rep_indr_setup_cb(struct net_device *netdev, void *cb_priv,
#endif
			    enum tc_setup_type tc_setup_type, void *type_data
#if !defined(HAVE_FLOW_INDR_BLOCK_BIND_CB_T_4_PARAMS) && defined(HAVE_FLOW_INDR_DEV_REGISTER)
			    , void *data,
			    void (*cleanup)(struct flow_block_cb *block_cb)
#endif
			    )
{
#ifndef HAVE_FLOW_INDR_BLOCK_BIND_CB_T_7_PARAMS
	struct Qdisc *sch = NULL;
#endif
#if defined(HAVE_FLOW_INDR_BLOCK_BIND_CB_T_4_PARAMS) || !defined(HAVE_FLOW_INDR_DEV_REGISTER)
	void *data = NULL;
	void *cleanup = NULL;
#endif
	int type = (int) tc_setup_type;

	switch (type) {
	case TC_SETUP_BLOCK:
		return mlx5e_rep_indr_setup_block(netdev, sch, cb_priv, type_data,
						  mlx5e_rep_indr_setup_tc_cb,
						  data, cleanup);
	case TC_SETUP_E2E_BLOCK:
		return mlx5e_rep_indr_setup_block(netdev, sch, cb_priv, type_data,
						  mlx5e_rep_indr_setup_block_e2e_cb,
						  data, cleanup);
#ifdef HAVE_TC_SETUP_FT
	case TC_SETUP_FT:
		return mlx5e_rep_indr_setup_block(netdev, sch, cb_priv, type_data,
						  mlx5e_rep_indr_setup_ft_cb,
						  data, cleanup);
#endif
	default:
		return -EOPNOTSUPP;
	}
}

#ifndef HAVE_FLOW_INDR_DEV_REGISTER
static int mlx5e_rep_indr_register_block(struct mlx5e_rep_priv *rpriv,
					 struct net_device *netdev)
{
	int err;

	err = __flow_indr_block_cb_register(netdev, rpriv,
					    mlx5e_rep_indr_setup_cb,
					    rpriv);
	if (err) {
		struct mlx5e_priv *priv = netdev_priv(rpriv->netdev);

		mlx5_core_err(priv->mdev, "Failed to register remote block notifier for %s err=%d\n",
			      netdev_name(netdev), err);
	}
	return err;
}

static void mlx5e_rep_indr_unregister_block(struct mlx5e_rep_priv *rpriv,
					    struct net_device *netdev)
{
	__flow_indr_block_cb_unregister(netdev, mlx5e_rep_indr_setup_cb,
					rpriv);
}

void mlx5e_rep_indr_clean_block_privs(struct mlx5e_rep_priv *rpriv)
{
	struct mlx5e_rep_indr_block_priv *cb_priv, *temp;
	struct list_head *head = &rpriv->uplink_priv.tc_indr_block_priv_list;

	list_for_each_entry_safe(cb_priv, temp, head, list) {
		mlx5e_rep_indr_unregister_block(rpriv, cb_priv->netdev);
		kfree(cb_priv);
	}
}

static int mlx5e_nic_rep_netdevice_event(struct notifier_block *nb,
					 unsigned long event, void *ptr)
{
	struct mlx5e_rep_priv *rpriv = container_of(nb, struct mlx5e_rep_priv,
						     uplink_priv.netdevice_nb);
	struct mlx5e_priv *priv = netdev_priv(rpriv->netdev);
	struct net_device *netdev = netdev_notifier_info_to_dev(ptr);

	if (!mlx5e_tc_tun_device_to_offload(priv, netdev) &&
	    !(is_vlan_dev(netdev) && vlan_dev_real_dev(netdev) == rpriv->netdev) &&
	    !netif_is_ovs_master(netdev))
		return NOTIFY_OK;

	switch (event) {
	case NETDEV_REGISTER:
		mlx5e_rep_indr_register_block(rpriv, netdev);
		break;
	case NETDEV_UNREGISTER:
		mlx5e_rep_indr_unregister_block(rpriv, netdev);
		break;
	}
	return NOTIFY_OK;
}
#endif /* HAVE_FLOW_INDR_DEV_REGISTER */

int mlx5e_rep_tc_netdevice_event_register(struct mlx5e_rep_priv *rpriv)
{
	struct mlx5_rep_uplink_priv *uplink_priv = &rpriv->uplink_priv;

	/* init indirect block notifications */
	INIT_LIST_HEAD(&uplink_priv->tc_indr_block_priv_list);

#ifdef HAVE_FLOW_INDR_DEV_REGISTER
	return flow_indr_dev_register(mlx5e_rep_indr_setup_cb, rpriv);
#else
	uplink_priv->netdevice_nb.notifier_call = mlx5e_nic_rep_netdevice_event;
	return register_netdevice_notifier_dev_net(rpriv->netdev,
						   &uplink_priv->netdevice_nb,
						   &uplink_priv->netdevice_nn);
#endif
}

void mlx5e_rep_tc_netdevice_event_unregister(struct mlx5e_rep_priv *rpriv)
{
#ifndef HAVE_FLOW_INDR_DEV_REGISTER
	struct mlx5_rep_uplink_priv *uplink_priv = &rpriv->uplink_priv;

	/* clean indirect TC block notifications */
	unregister_netdevice_notifier_dev_net(rpriv->netdev,
					      &uplink_priv->netdevice_nb,
					      &uplink_priv->netdevice_nn);
#else
	flow_indr_dev_unregister(mlx5e_rep_indr_setup_cb, rpriv,
#ifdef HAVE_FLOW_INDR_DEV_UNREGISTER_FLOW_SETUP_CB_T
				mlx5e_rep_indr_setup_tc_cb);
#else
				 mlx5e_rep_indr_block_unbind);
#endif
#endif
}
#endif /* HAVE_TC_BLOCK_OFFLOAD || HAVE_FLOW_BLOCK_OFFLOAD */

#if IS_ENABLED(CONFIG_NET_TC_SKB_EXT) || IS_ENABLED(CONFIG_MLX5_TC_SAMPLE)
static bool mlx5e_restore_tunnel(struct mlx5e_priv *priv, struct sk_buff *skb,
				 struct mlx5e_tc_update_priv *tc_priv,
				 u32 tunnel_id)
{
	struct mlx5_eswitch *esw = priv->mdev->priv.eswitch;
	struct tunnel_match_enc_opts enc_opts = {};
	struct mlx5_rep_uplink_priv *uplink_priv;
	struct mlx5e_rep_priv *uplink_rpriv;
	struct metadata_dst *tun_dst;
	struct tunnel_match_key key;
	u32 tun_id, enc_opts_id;
	struct net_device *dev;
	int err;

	enc_opts_id = tunnel_id & ENC_OPTS_BITS_MASK;
	tun_id = (tunnel_id >> ENC_OPTS_BITS) & TUNNEL_INFO_BITS_MASK;

	if (!tun_id)
		return true;

	uplink_rpriv = mlx5_eswitch_get_uplink_priv(esw, REP_ETH);
	uplink_priv = &uplink_rpriv->uplink_priv;

	err = mapping_find(uplink_priv->tunnel_mapping, tun_id, &key);
	if (err) {
		netdev_dbg(priv->netdev,
			   "Couldn't find tunnel for tun_id: %d, err: %d\n",
			   tun_id, err);
		return false;
	}

	if (enc_opts_id) {
		err = mapping_find(uplink_priv->tunnel_enc_opts_mapping,
				   enc_opts_id, &enc_opts);
		if (err) {
			netdev_dbg(priv->netdev,
				   "Couldn't find tunnel (opts) for tun_id: %d, err: %d\n",
				   enc_opts_id, err);
			return false;
		}
	}

	if (key.enc_control.addr_type == FLOW_DISSECTOR_KEY_IPV4_ADDRS) {
		tun_dst = __ip_tun_set_dst(key.enc_ipv4.src, key.enc_ipv4.dst,
					   key.enc_ip.tos, key.enc_ip.ttl,
					   key.enc_tp.dst, TUNNEL_KEY,
					   key32_to_tunnel_id(key.enc_key_id.keyid),
					   enc_opts.key.len);
	} else if (key.enc_control.addr_type == FLOW_DISSECTOR_KEY_IPV6_ADDRS) {
		tun_dst = __ipv6_tun_set_dst(&key.enc_ipv6.src, &key.enc_ipv6.dst,
					     key.enc_ip.tos, key.enc_ip.ttl,
					     key.enc_tp.dst, 0, TUNNEL_KEY,
					     key32_to_tunnel_id(key.enc_key_id.keyid),
					     enc_opts.key.len);
	} else {
		netdev_dbg(priv->netdev,
			   "Couldn't restore tunnel, unsupported addr_type: %d\n",
			   key.enc_control.addr_type);
		return false;
	}

	if (!tun_dst) {
		netdev_dbg(priv->netdev, "Couldn't restore tunnel, no tun_dst\n");
		return false;
	}

	tun_dst->u.tun_info.key.tp_src = key.enc_tp.src;

	if (enc_opts.key.len)
#if defined(HAVE_IP_TUNNEL_INFO_OPTS_SET_4_PARAMS)
		ip_tunnel_info_opts_set(&tun_dst->u.tun_info,
					enc_opts.key.data,
					enc_opts.key.len,
					enc_opts.key.dst_opt_type);
#else
		ip_tunnel_info_opts_set(&tun_dst->u.tun_info,
					enc_opts.key.data,
					enc_opts.key.len);
#endif

	skb_dst_set(skb, (struct dst_entry *)tun_dst);
	dev = dev_get_by_index(&init_net, key.filter_ifindex);
	if (!dev) {
		netdev_dbg(priv->netdev,
			   "Couldn't find tunnel device with ifindex: %d\n",
			   key.filter_ifindex);
		return false;
	}

	/* Set tun_dev so we do dev_put() after datapath */
	tc_priv->tun_dev = dev;

	skb->dev = dev;

	return true;
}
#endif /* CONFIG_NET_TC_SKB_EXT || CONFIG_MLX5_TC_SAMPLE */

static bool mlx5e_handle_int_port(struct mlx5_cqe64 *cqe,
				  struct sk_buff *skb,
				  struct mlx5_mapped_obj *mapped_obj)
{
	struct mlx5e_priv *priv = netdev_priv(skb->dev);
	struct mlx5_esw_int_vport *int_vport;
	struct net_device *netdev;
	struct mlx5_eswitch *esw;
	u32 int_vport_metadata;
	int type, err, ifindex;

	esw = priv->mdev->priv.eswitch;
	int_vport_metadata = mapped_obj->int_vport_metadata;

	rcu_read_lock();
	int_vport = mlx5_esw_get_int_vport_from_metadata(esw, int_vport_metadata);
	if (IS_ERR_OR_NULL(int_vport)) {
		rcu_read_unlock();
		mlx5_core_dbg(priv->mdev, "Unable to find a valid int port with metadata 0x%.8x\n",
			      int_vport_metadata);
		return false;
	}

	type = int_vport->type;
	ifindex = int_vport->ifindex;
	rcu_read_unlock();

	netdev = dev_get_by_index(&init_net, ifindex);
	if (!netdev) {
		netdev_warn(priv->netdev,
			   "Couldn't find tunnel device with ifindex: %d\n",
			   ifindex);
		return false;
	}

	skb->skb_iif = netdev->ifindex;
	skb->dev = netdev;

	if (type) {
		skb_push_rcsum(skb, skb->mac_len);
#ifdef HAVE_SKB_SET_REDIRECTED
		skb_set_redirected(skb, false);
#endif
		err = dev_queue_xmit(skb);
	} else {
		skb->pkt_type = PACKET_HOST;
#ifdef HAVE_SKB_SET_REDIRECTED
		skb_set_redirected(skb, true);
#endif	
		err = netif_receive_skb(skb);
	}

	if (err)
		netdev_dbg(priv->netdev, "failed to fwd packet to int port, err %d, intf %s, type %d\n",
			   err, netdev->name, type);

	dev_put(netdev);

	return true;
}

bool mlx5e_rep_tc_update_skb(struct mlx5_cqe64 *cqe,
			     struct sk_buff *skb,
			     struct mlx5e_tc_update_priv *tc_priv,
			     bool *free_skb)
{
	struct mlx5_mapped_obj mapped_obj;
	u32 chain = 0, reg_c0, reg_c1;
	struct mlx5_eswitch *esw;
	struct mlx5e_priv *priv;
	u32 tunnel_id;
	int err;

	*free_skb = false;

	reg_c0 = (be32_to_cpu(cqe->sop_drop_qpn) & MLX5E_TC_FLOW_ID_MASK);
	if (reg_c0 == MLX5_FS_DEFAULT_FLOW_TAG)
		reg_c0 = 0;
	reg_c1 = be32_to_cpu(cqe->ft_metadata);
	tunnel_id = reg_c1 >> ESW_TUN_OFFSET;

	if (!reg_c0)
		return true;

	priv = netdev_priv(skb->dev);
	esw = priv->mdev->priv.eswitch;

	err = mlx5_get_mapped_object(esw_chains(esw), reg_c0, &mapped_obj);
	if (err) {
		netdev_dbg(priv->netdev,
			   "Couldn't find chain for chain tag: %d, err: %d\n",
			   reg_c0, err);
		goto out_free_skb;
	}

	if (mapped_obj.type == MLX5_MAPPED_OBJ_CHAIN) {
		chain = mapped_obj.chain;
#if IS_ENABLED(CONFIG_MLX5_TC_SAMPLE)
	} else if (mapped_obj.type == MLX5_MAPPED_OBJ_SAMPLE) {
		struct psample_group psample_group = {};
		int iif = skb->dev->ifindex;
		struct sample_obj *sample;
		u32 size;

		sample = &mapped_obj.sample;
		psample_group.group_num = sample->group_id;
		psample_group.net = &init_net;
		skb_push(skb, skb->mac_len);
		size = sample->trunc_size ? min(sample->trunc_size, skb->len) : skb->len;

		mlx5e_restore_tunnel(priv, skb, tc_priv, tunnel_id);
		psample_sample_packet(&psample_group, skb, size, iif, 0, sample->rate);
		mlx5_rep_tc_post_napi_receive(tc_priv);
		consume_skb(skb);

		return false;
#endif /* CONFIG_MLX5_TC_SAMPLE */
	} else if (mapped_obj.type == MLX5_MAPPED_OBJ_INT_VPORT_METADATA) {
		if (!tunnel_id) {
			if (mlx5e_handle_int_port(cqe, skb, &mapped_obj))
				return false;
			else
				goto out_free_skb;
		}
	} else {
		netdev_dbg(priv->netdev, "Invalid mapped object type: %d\n", mapped_obj.type);
		goto out_free_skb;
	}


#if !IS_ENABLED(CONFIG_NET_TC_SKB_EXT)
	return true;
#else
	if (chain) {
		struct mlx5_rep_uplink_priv *uplink_priv;
		struct mlx5e_rep_priv *uplink_rpriv;
		struct tc_skb_ext *tc_skb_ext;
		u32 zone_restore_id;

		tc_skb_ext = skb_ext_add(skb, TC_SKB_EXT);
		if (!tc_skb_ext) {
			WARN_ON(1);
			goto out_incr_rx_counter;
		}

		tc_skb_ext->chain = chain;

		zone_restore_id = reg_c1 & ESW_ZONE_ID_MASK;

		uplink_rpriv = mlx5_eswitch_get_uplink_priv(esw, REP_ETH);
		uplink_priv = &uplink_rpriv->uplink_priv;
		if (!mlx5e_tc_ct_restore_flow(uplink_priv->ct_priv, skb,
					      zone_restore_id))
			goto out_incr_rx_counter;
	}

	if (mlx5e_restore_tunnel(priv, skb, tc_priv, tunnel_id))
		return true;

out_incr_rx_counter:
	atomic_inc(&esw->dev->priv.ct_debugfs->stats.rx_dropped);
#endif /* CONFIG_NET_TC_SKB_EXT */

out_free_skb:
	*free_skb = true;
	return false;
}

void mlx5_rep_tc_post_napi_receive(struct mlx5e_tc_update_priv *tc_priv)
{
	if (tc_priv->tun_dev)
		dev_put(tc_priv->tun_dev);
}
