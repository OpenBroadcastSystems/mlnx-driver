/*
 * Copyright (c) 2015-2016, Mellanox Technologies. All rights reserved.
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

#include <net/tc_act/tc_gact.h>
#include <net/pkt_cls.h>
#include <linux/mlx5/fs.h>
#include <net/switchdev.h>
#if defined(HAVE_KERNEL_WITH_VXLAN_SUPPORT_ON)
#include <net/vxlan.h>
#endif
#ifdef HAVE_NETDEV_BPF
#include <linux/bpf.h>
#endif
#include "eswitch.h"
#include "en.h"
#include "en_tc.h"
#include "en_rep.h"
#include "en_accel/ipsec.h"
#include "en_accel/ipsec_rxtx.h"
#include "accel/ipsec.h"
#if defined(HAVE_KERNEL_WITH_VXLAN_SUPPORT_ON)
#include "vxlan.h"
#endif

#if defined(CONFIG_NETMAP) || defined(CONFIG_NETMAP_MODULE)
/*
 * mlx5_netmap_linux.h contains functions for netmap support
 * that extend the standard driver.
 */
#define NETMAP_MLX5_MAIN
#define DEV_NETMAP
#include "mlx5_netmap_linux.h"
#endif

struct mlx5e_rq_param {
	u32			rqc[MLX5_ST_SZ_DW(rqc)];
	struct mlx5_wq_param	wq;
	struct mlx5e_rq_frag_info frag_info[MLX5E_MAX_RX_FRAGS];
	u8 num_frags;
	u8 log_num_frags;
	u8 wqe_bulk;
};

struct mlx5e_sq_param {
	u32                        sqc[MLX5_ST_SZ_DW(sqc)];
	struct mlx5_wq_param       wq;
};

struct mlx5e_cq_param {
	u32                        cqc[MLX5_ST_SZ_DW(cqc)];
	struct mlx5_wq_param       wq;
	u16                        eq_ix;
	u8                         cq_period_mode;
};

struct mlx5e_channel_param {
	struct mlx5e_rq_param      rq;
	struct mlx5e_sq_param      sq;
#ifdef HAVE_NETDEV_BPF
	struct mlx5e_sq_param      xdp_sq;
#endif
	struct mlx5e_sq_param      icosq;
	struct mlx5e_cq_param      rx_cq;
	struct mlx5e_cq_param      tx_cq;
	struct mlx5e_cq_param      icosq_cq;
};

static bool mlx5e_check_fragmented_striding_rq_cap(struct mlx5_core_dev *mdev)
{
#ifdef DEV_NETMAP
	return 0;
#endif
	return MLX5_CAP_GEN(mdev, striding_rq) &&
		MLX5_CAP_GEN(mdev, umr_ptr_rlky) &&
		MLX5_CAP_ETH(mdev, reg_umr_sq);
}

static inline bool mlx5e_rx_frag_sz_page_cross(u32 frag_sz)
{
	return frag_sz > PAGE_SIZE;
}

static u32 mlx5e_rx_get_linear_frag_sz(struct mlx5e_priv *priv,
				       struct mlx5e_params *params)
{
#ifdef HAVE_NETDEV_BPF
	if (!params->xdp_prog) {
#else
	if (true) {
#endif
		u16 hw_mtu = MLX5E_SW2HW_MTU(priv, priv->netdev->mtu);
		u16 rq_headroom = MLX5_RX_HEADROOM + NET_IP_ALIGN;

		return MLX5_SKB_FRAG_SZ(rq_headroom + hw_mtu);
	}

	return PAGE_SIZE;
}

static bool mlx5e_rx_is_linear_skb(struct mlx5e_priv *priv,
				   struct mlx5e_params *params)
{
	u32 frag_sz = mlx5e_rx_get_linear_frag_sz(priv, params);

#ifdef CONFIG_COMPAT_LRO_ENABLED_IPOIB
	return !IS_HW_LRO(params) && !mlx5e_rx_frag_sz_page_cross(frag_sz);
#else
	return !params->lro_en && !mlx5e_rx_frag_sz_page_cross(frag_sz);
#endif
}

u8 mlx5e_mpwqe_get_log_stride_size(struct mlx5e_priv *priv,
				   struct mlx5e_params *params)
{
	return MLX5E_MPWQE_STRIDE_SZ(priv->mdev,
		MLX5E_GET_PFLAG(params, MLX5E_PFLAG_RX_CQE_COMPRESS));
}

u8 mlx5e_mpwqe_get_log_num_strides(struct mlx5e_priv *priv,
				   struct mlx5e_params *params)
{
	return MLX5_MPWRQ_LOG_WQE_SZ -
		mlx5e_mpwqe_get_log_stride_size(priv, params);
}

static u16 mlx5e_get_rq_headroom(struct mlx5e_priv *priv,
				 struct mlx5e_params *params)
{
#ifdef HAVE_NETDEV_BPF
	u16 linear_rq_headroom = params->xdp_prog ?
		XDP_PACKET_HEADROOM : MLX5_RX_HEADROOM;
#else
	u16 linear_rq_headroom = MLX5_RX_HEADROOM;
#endif

	linear_rq_headroom += NET_IP_ALIGN;

	if (params->rq_wq_type == MLX5_WQ_TYPE_CYCLIC)
		if (mlx5e_rx_is_linear_skb(priv, params))
			return linear_rq_headroom;

	return 0;
}

void mlx5e_init_rq_type_params(struct mlx5e_priv *priv,
			       struct mlx5e_params *params, u8 rq_type)
{
	params->rq_wq_type = rq_type;
	params->lro_wqe_sz = MLX5E_PARAMS_DEFAULT_LRO_WQE_SZ;
	switch (params->rq_wq_type) {
	case MLX5_WQ_TYPE_LINKED_LIST_STRIDING_RQ:
		params->log_rq_size = is_kdump_kernel() ?
			MLX5E_PARAMS_MINIMUM_LOG_RQ_SIZE_MPW :
			MLX5E_PARAMS_DEFAULT_LOG_RQ_SIZE_MPW;
		break;
	default: /* MLX5_WQ_TYPE_CYCLIC */
		params->log_rq_size = is_kdump_kernel() ?
			MLX5E_PARAMS_MINIMUM_LOG_RQ_SIZE :
			MLX5E_PARAMS_DEFAULT_LOG_RQ_SIZE;
	}

	mlx5_core_info(priv->mdev, "MLX5E: StrdRq(%d) RqSz(%ld) StrdSz(%ld) RxCqeCmprss(%d)\n",
		       params->rq_wq_type == MLX5_WQ_TYPE_LINKED_LIST_STRIDING_RQ,
		       BIT(params->log_rq_size),
		       BIT(mlx5e_mpwqe_get_log_stride_size(priv, params)),
		       MLX5E_GET_PFLAG(params, MLX5E_PFLAG_RX_CQE_COMPRESS));
}

static void mlx5e_set_rq_params(struct mlx5e_priv *priv,
				struct mlx5e_params *params)
{
	struct mlx5_core_dev *mdev = priv->mdev;
	u8 rq_type = mlx5e_check_fragmented_striding_rq_cap(mdev) &&
#ifdef HAVE_NETDEV_BPF
		    !params->xdp_prog && !MLX5_IPSEC_DEV(mdev) ?
#else
		    !MLX5_IPSEC_DEV(mdev) ?
#endif
		    MLX5_WQ_TYPE_LINKED_LIST_STRIDING_RQ :
		    MLX5_WQ_TYPE_CYCLIC;

	mlx5e_init_rq_type_params(priv, params, rq_type);
}

static void mlx5e_update_carrier(struct mlx5e_priv *priv)
{
	struct mlx5_core_dev *mdev = priv->mdev;
	u8 port_state;

	port_state = mlx5_query_vport_state(mdev,
					    MLX5_QUERY_VPORT_STATE_IN_OP_MOD_VNIC_VPORT,
					    0);

	if (port_state == VPORT_STATE_UP) {
		netdev_info(priv->netdev, "Link up\n");
		netif_carrier_on(priv->netdev);
	} else {
		netdev_info(priv->netdev, "Link down\n");
		netif_carrier_off(priv->netdev);
	}
}

static void mlx5e_update_carrier_work(struct work_struct *work)
{
	struct mlx5e_priv *priv = container_of(work, struct mlx5e_priv,
					       update_carrier_work);

	mutex_lock(&priv->state_lock);
	if (test_bit(MLX5E_STATE_OPENED, &priv->state))
		if (priv->profile->update_carrier)
			priv->profile->update_carrier(priv);
	mutex_unlock(&priv->state_lock);
}

static void mlx5e_tx_timeout_work(struct work_struct *work)
{
	struct mlx5e_priv *priv = container_of(work, struct mlx5e_priv,
					       tx_timeout_work);
	int err;

	rtnl_lock();
	mutex_lock(&priv->state_lock);
	if (!test_bit(MLX5E_STATE_OPENED, &priv->state))
		goto unlock;
	mlx5e_close_locked(priv->netdev);
	err = mlx5e_open_locked(priv->netdev);
	if (err)
		netdev_err(priv->netdev, "mlx5e_open_locked failed recovering from a tx_timeout, err(%d).\n",
			   err);
unlock:
	mutex_unlock(&priv->state_lock);
	rtnl_unlock();
}

void mlx5e_update_sw_counters(struct mlx5e_priv *priv)
{
	struct mlx5e_sw_stats temp, *s = &temp;
	struct mlx5e_rq_stats *rq_stats;
	struct mlx5e_sq_stats *sq_stats;
	struct mlx5e_ch_stats *ch_stats;
	int i, j;

	memset(s, 0, sizeof(*s));
	for (i = 0; i < priv->channels.num; i++) {
		struct mlx5e_channel *c = priv->channels.c[i];

		rq_stats = &c->rq.stats;
		ch_stats = &c->stats;

		s->rx_packets	+= rq_stats->packets;
		s->rx_bytes	+= rq_stats->bytes;
		s->rx_lro_packets += rq_stats->lro_packets;
		s->rx_lro_bytes	+= rq_stats->lro_bytes;
		s->rx_removed_vlan_packets += rq_stats->removed_vlan_packets;
		s->rx_csum_none	+= rq_stats->csum_none;
		s->rx_csum_complete += rq_stats->csum_complete;
		s->rx_csum_unnecessary += rq_stats->csum_unnecessary;
		s->rx_csum_unnecessary_inner += rq_stats->csum_unnecessary_inner;
		s->rx_xdp_drop += rq_stats->xdp_drop;
		s->rx_xdp_tx += rq_stats->xdp_tx;
		s->rx_xdp_tx_full += rq_stats->xdp_tx_full;
		s->rx_wqe_err   += rq_stats->wqe_err;
		s->rx_mpwqe_filler += rq_stats->mpwqe_filler;
		s->rx_buff_alloc_err += rq_stats->buff_alloc_err;
		s->rx_cqe_compress_blks += rq_stats->cqe_compress_blks;
		s->rx_cqe_compress_pkts += rq_stats->cqe_compress_pkts;
		s->rx_cache_reuse += rq_stats->cache_reuse;
		s->rx_cache_full  += rq_stats->cache_full;
		s->rx_cache_ext   += rq_stats->cache_ext;
		s->rx_cache_rdc   += rq_stats->cache_rdc;
		s->rx_cache_alloc += rq_stats->cache_alloc;
		s->rx_cache_waive += rq_stats->cache_waive;
		s->ch_eq_rearm += ch_stats->eq_rearm;

		for (j = 0; j < priv->channels.params.num_tc; j++) {
			sq_stats = &c->sq[j].stats;

			s->tx_packets		+= sq_stats->packets;
			s->tx_bytes		+= sq_stats->bytes;
			s->tx_tso_packets	+= sq_stats->tso_packets;
			s->tx_tso_bytes		+= sq_stats->tso_bytes;
			s->tx_tso_inner_packets	+= sq_stats->tso_inner_packets;
			s->tx_tso_inner_bytes	+= sq_stats->tso_inner_bytes;
			s->tx_added_vlan_packets += sq_stats->added_vlan_packets;
			s->tx_queue_stopped	+= sq_stats->stopped;
			s->tx_queue_wake	+= sq_stats->wake;
			s->tx_queue_dropped	+= sq_stats->dropped;
			s->tx_xmit_more		+= sq_stats->xmit_more;
			s->tx_csum_partial_inner += sq_stats->csum_partial_inner;
			s->tx_csum_none		+= sq_stats->csum_none;
			s->tx_csum_partial	+= sq_stats->csum_partial;
			s->tx_cqe_err		+= sq_stats->cqe_err;
			s->tx_recover		+= sq_stats->recover;
		}
	}

	s->link_down_events_phy = MLX5_GET(ppcnt_reg,
				priv->stats.pport.phy_counters,
				counter_set.phys_layer_cntrs.link_down_events);
	memcpy(&priv->stats.sw, s, sizeof(*s));
}

static void mlx5e_update_vport_counters(struct mlx5e_priv *priv)
{
	int outlen = MLX5_ST_SZ_BYTES(query_vport_counter_out);
	u32 *out = (u32 *)priv->stats.vport.query_vport_out;
	u32 in[MLX5_ST_SZ_DW(query_vport_counter_in)] = {0};
	struct mlx5_core_dev *mdev = priv->mdev;

	MLX5_SET(query_vport_counter_in, in, opcode,
		 MLX5_CMD_OP_QUERY_VPORT_COUNTER);
	MLX5_SET(query_vport_counter_in, in, op_mod, 0);
	MLX5_SET(query_vport_counter_in, in, other_vport, 0);

	mlx5_cmd_exec(mdev, in, sizeof(in), out, outlen);
}

static void mlx5e_update_vnic_env_counters(struct mlx5e_priv *priv)
{
	u32 *out = (u32 *)priv->stats.vnic.query_vnic_env_out;
	int outlen = MLX5_ST_SZ_BYTES(query_vnic_env_out);
	u32 in[MLX5_ST_SZ_DW(query_vnic_env_in)] = {0};
	struct mlx5_core_dev *mdev = priv->mdev;

	if (!MLX5_CAP_GEN(priv->mdev, nic_receive_steering_discard))
		return;

	MLX5_SET(query_vnic_env_in, in, opcode,
		 MLX5_CMD_OP_QUERY_VNIC_ENV);
	MLX5_SET(query_vnic_env_in, in, op_mod, 0);
	MLX5_SET(query_vnic_env_in, in, other_vport, 0);

	mlx5_cmd_exec(mdev, in, sizeof(in), out, outlen);
}

static void mlx5e_update_pport_counters(struct mlx5e_priv *priv, bool full)
{
	struct mlx5e_pport_stats *pstats = &priv->stats.pport;
	struct mlx5_core_dev *mdev = priv->mdev;
	u32 in[MLX5_ST_SZ_DW(ppcnt_reg)] = {0};
	int sz = MLX5_ST_SZ_BYTES(ppcnt_reg);
	int prio;
	void *out;

	MLX5_SET(ppcnt_reg, in, local_port, 1);

	out = pstats->IEEE_802_3_counters;
	MLX5_SET(ppcnt_reg, in, grp, MLX5_IEEE_802_3_COUNTERS_GROUP);
	mlx5_core_access_reg(mdev, in, sz, out, sz, MLX5_REG_PPCNT, 0, 0);

	if (!full)
		return;

	out = pstats->RFC_2863_counters;
	MLX5_SET(ppcnt_reg, in, grp, MLX5_RFC_2863_COUNTERS_GROUP);
	mlx5_core_access_reg(mdev, in, sz, out, sz, MLX5_REG_PPCNT, 0, 0);

	out = pstats->RFC_2819_counters;
	MLX5_SET(ppcnt_reg, in, grp, MLX5_RFC_2819_COUNTERS_GROUP);
	mlx5_core_access_reg(mdev, in, sz, out, sz, MLX5_REG_PPCNT, 0, 0);

	out = pstats->phy_counters;
	MLX5_SET(ppcnt_reg, in, grp, MLX5_PHYSICAL_LAYER_COUNTERS_GROUP);
	mlx5_core_access_reg(mdev, in, sz, out, sz, MLX5_REG_PPCNT, 0, 0);

	if (MLX5_CAP_PCAM_FEATURE(mdev, ppcnt_statistical_group)) {
		out = pstats->phy_statistical_counters;
		MLX5_SET(ppcnt_reg, in, grp, MLX5_PHYSICAL_LAYER_STATISTICAL_GROUP);
		mlx5_core_access_reg(mdev, in, sz, out, sz, MLX5_REG_PPCNT, 0, 0);
	}

	if (MLX5_CAP_PCAM_FEATURE(mdev, rx_buffer_fullness_counters)) {
		out = pstats->eth_ext_counters;
		MLX5_SET(ppcnt_reg, in, grp, MLX5_ETHERNET_EXTENDED_COUNTERS_GROUP);
		mlx5_core_access_reg(mdev, in, sz, out, sz, MLX5_REG_PPCNT, 0, 0);
	}

	MLX5_SET(ppcnt_reg, in, grp, MLX5_PER_PRIORITY_COUNTERS_GROUP);
	for (prio = 0; prio < NUM_PPORT_PRIO; prio++) {
		out = pstats->per_prio_counters[prio];
		MLX5_SET(ppcnt_reg, in, prio_tc, prio);
		mlx5_core_access_reg(mdev, in, sz, out, sz,
				     MLX5_REG_PPCNT, 0, 0);
	}
}

static void mlx5e_update_q_counter(struct mlx5e_priv *priv)
{
	struct mlx5e_qcounter_stats *qcnt = &priv->stats.qcnt;
	u32 out[MLX5_ST_SZ_DW(query_q_counter_out)];
	int err;

	if (!priv->q_counter)
		return;

	err = mlx5_core_query_q_counter(priv->mdev, priv->q_counter, 0, out, sizeof(out));
	if (err)
		return;

	qcnt->rx_out_of_buffer = MLX5_GET(query_q_counter_out, out, out_of_buffer);
}

static void mlx5e_update_pcie_counters(struct mlx5e_priv *priv)
{
	struct mlx5e_pcie_stats *pcie_stats = &priv->stats.pcie;
	struct mlx5_core_dev *mdev = priv->mdev;
	u32 in[MLX5_ST_SZ_DW(mpcnt_reg)] = {0};
	int sz = MLX5_ST_SZ_BYTES(mpcnt_reg);
	void *out;

	if (!MLX5_CAP_MCAM_FEATURE(mdev, pcie_performance_group))
		return;

	out = pcie_stats->pcie_perf_counters;
	MLX5_SET(mpcnt_reg, in, grp, MLX5_PCIE_PERFORMANCE_COUNTERS_GROUP);
	mlx5_core_access_reg(mdev, in, sz, out, sz, MLX5_REG_MPCNT, 0, 0);
}

#ifdef CONFIG_COMPAT_LRO_ENABLED_IPOIB
static void mlx5e_update_sw_lro_stats(struct mlx5e_priv *priv)
{
	int i;
	struct mlx5e_sw_stats *s = &priv->stats.sw;

	s->rx_sw_lro_aggregated = 0;
	s->rx_sw_lro_flushed = 0;
	s->rx_sw_lro_no_desc = 0;

	for (i = 0; i < priv->channels.num; i++) {
		struct mlx5e_rq *rq = &priv->channels.c[i]->rq;

		s->rx_sw_lro_aggregated += rq->sw_lro.lro_mgr.stats.aggregated;
		s->rx_sw_lro_flushed += rq->sw_lro.lro_mgr.stats.flushed;
		s->rx_sw_lro_no_desc += rq->sw_lro.lro_mgr.stats.no_desc;
	}
}
#endif

void mlx5e_update_stats(struct mlx5e_priv *priv, bool full)
{
	if (full) {
		mlx5e_update_pcie_counters(priv);
		mlx5e_ipsec_update_stats(priv);
		mlx5e_update_vnic_env_counters(priv);
	}
	mlx5e_update_pport_counters(priv, full);
	mlx5e_update_vport_counters(priv);
	mlx5e_update_q_counter(priv);
	mlx5e_update_sw_counters(priv);
#ifdef CONFIG_COMPAT_LRO_ENABLED_IPOIB
	mlx5e_update_sw_lro_stats(priv);
#endif
}

static void mlx5e_update_ndo_stats(struct mlx5e_priv *priv)
{
	mlx5e_update_stats(priv, false);
}

void mlx5e_update_stats_work(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct mlx5e_priv *priv = container_of(dwork, struct mlx5e_priv,
					       update_stats_work);
	mutex_lock(&priv->state_lock);
	if (test_bit(MLX5E_STATE_OPENED, &priv->state)) {
		priv->profile->update_stats(priv);
		queue_delayed_work(priv->wq, dwork,
				   msecs_to_jiffies(MLX5E_UPDATE_STATS_INTERVAL));
	}
	mutex_unlock(&priv->state_lock);
}

static void mlx5e_delay_drop_handler(struct work_struct *work)
{
	struct mlx5e_delay_drop *delay_drop =
		container_of(work, struct mlx5e_delay_drop, work);
	struct mlx5e_priv *priv = container_of(delay_drop, struct mlx5e_priv,
					       delay_drop);
	int err;

	mutex_lock(&delay_drop->lock);
	err = mlx5_core_set_delay_drop(priv->mdev,
				       delay_drop->usec_timeout);
	if (err) {
		mlx5_core_warn(priv->mdev, "Failed to enable delay drop err=%d\n",
			       err);
		delay_drop->activate = false;
	}
	mutex_unlock(&delay_drop->lock);
}

static void mlx5e_async_event(struct mlx5_core_dev *mdev, void *vpriv,
			      enum mlx5_dev_event event, unsigned long param)
{
	struct mlx5e_priv *priv = vpriv;

	if (!test_bit(MLX5E_STATE_ASYNC_EVENTS_ENABLED, &priv->state))
		return;

	switch (event) {
	case MLX5_DEV_EVENT_PORT_UP:
	case MLX5_DEV_EVENT_PORT_DOWN:
		queue_work(priv->wq, &priv->update_carrier_work);
		break;
	case MLX5_DEV_EVENT_DELAY_DROP_TIMEOUT:
		queue_work(priv->wq, &priv->delay_drop.work);
		break;
	default:
		break;
	}
}

static void mlx5e_enable_async_events(struct mlx5e_priv *priv)
{
	set_bit(MLX5E_STATE_ASYNC_EVENTS_ENABLED, &priv->state);
}

static void mlx5e_disable_async_events(struct mlx5e_priv *priv)
{
	clear_bit(MLX5E_STATE_ASYNC_EVENTS_ENABLED, &priv->state);
#ifdef HAVE_PCI_IRQ_API
	synchronize_irq(pci_irq_vector(priv->mdev->pdev, MLX5_EQ_VEC_ASYNC));
#else
	synchronize_irq(mlx5_get_msix_vec(priv->mdev, MLX5_EQ_VEC_ASYNC));
#endif
}

static inline int mlx5e_get_wqe_mtt_sz(void)
{
	/* UMR copies MTTs in units of MLX5_UMR_MTT_ALIGNMENT bytes.
	 * To avoid copying garbage after the mtt array, we allocate
	 * a little more.
	 */
	return ALIGN(MLX5_MPWRQ_PAGES_PER_WQE * sizeof(__be64),
		     MLX5_UMR_MTT_ALIGNMENT);
}

static inline void mlx5e_build_umr_wqe(struct mlx5e_rq *rq,
				       struct mlx5e_icosq *sq,
				       struct mlx5e_umr_wqe *wqe,
				       u16 ix)
{
	struct mlx5_wqe_ctrl_seg      *cseg = &wqe->ctrl;
	struct mlx5_wqe_umr_ctrl_seg *ucseg = &wqe->uctrl;
	struct mlx5_wqe_data_seg      *dseg = &wqe->data;
	struct mlx5e_mpw_info *wi = &rq->mpwqe.info[ix];
	u8 ds_cnt = DIV_ROUND_UP(sizeof(*wqe), MLX5_SEND_WQE_DS);
	u32 umr_wqe_mtt_offset = mlx5e_get_wqe_mtt_offset(rq, ix);

	cseg->qpn_ds    = cpu_to_be32((sq->sqn << MLX5_WQE_CTRL_QPN_SHIFT) |
				      ds_cnt);
	cseg->fm_ce_se  = MLX5_WQE_CTRL_CQ_UPDATE;
	cseg->imm       = rq->mkey_be;

	ucseg->flags = MLX5_UMR_TRANSLATION_OFFSET_EN;
	ucseg->xlt_octowords =
		cpu_to_be16(MLX5_MTT_OCTW(MLX5_MPWRQ_PAGES_PER_WQE));
	ucseg->bsf_octowords =
		cpu_to_be16(MLX5_MTT_OCTW(umr_wqe_mtt_offset));
	ucseg->mkey_mask     = cpu_to_be64(MLX5_MKEY_MASK_FREE);

	dseg->lkey = sq->mkey_be;
	dseg->addr = cpu_to_be64(wi->umr.mtt_addr);
}

static u32 mlx5e_rqwq_get_size(struct mlx5e_rq *rq)
{
	switch (rq->wq_type) {
	case MLX5_WQ_TYPE_LINKED_LIST_STRIDING_RQ:
		return mlx5_wq_ll_get_size(&rq->mpwqe.wq);
	default:
		return mlx5_wq_cyc_get_size(&rq->wqe.wq);
	}
}

static u32 mlx5e_rqwq_get_cur_sz(struct mlx5e_rq *rq)
{
	switch (rq->wq_type) {
	case MLX5_WQ_TYPE_LINKED_LIST_STRIDING_RQ:
		return rq->mpwqe.wq.cur_sz;
	default:
		return rq->wqe.wq.cur_sz;
	}
}

static int mlx5e_rq_alloc_mpwqe_info(struct mlx5e_rq *rq,
				     struct mlx5e_channel *c)
{
	int wq_sz = mlx5e_rqwq_get_size(rq);
	int mtt_sz = mlx5e_get_wqe_mtt_sz();
	int mtt_alloc = mtt_sz + MLX5_UMR_ALIGN - 1;
	int i;

	rq->mpwqe.info = kzalloc_node(wq_sz * sizeof(*rq->mpwqe.info),
				      GFP_KERNEL, cpu_to_node(c->cpu));
	if (!rq->mpwqe.info)
		goto err_out;

	/* We allocate more than mtt_sz as we will align the pointer */
	rq->mpwqe.mtt_no_align = kzalloc_node(mtt_alloc * wq_sz, GFP_KERNEL,
					cpu_to_node(c->cpu));
	if (unlikely(!rq->mpwqe.mtt_no_align))
		goto err_free_wqe_info;

	for (i = 0; i < wq_sz; i++) {
		struct mlx5e_mpw_info *wi = &rq->mpwqe.info[i];

		wi->umr.mtt = PTR_ALIGN(rq->mpwqe.mtt_no_align + i * mtt_alloc,
					MLX5_UMR_ALIGN);
		wi->umr.mtt_addr = dma_map_single(c->pdev, wi->umr.mtt, mtt_sz,
						  PCI_DMA_TODEVICE);
		if (unlikely(dma_mapping_error(c->pdev, wi->umr.mtt_addr)))
			goto err_unmap_mtts;

		mlx5e_build_umr_wqe(rq, &c->icosq, &wi->umr.wqe, i);
	}

	return 0;

err_unmap_mtts:
	while (--i >= 0) {
		struct mlx5e_mpw_info *wi = &rq->mpwqe.info[i];

		dma_unmap_single(c->pdev, wi->umr.mtt_addr, mtt_sz,
				 PCI_DMA_TODEVICE);
	}
	kfree(rq->mpwqe.mtt_no_align);
err_free_wqe_info:
	kfree(rq->mpwqe.info);

err_out:
	return -ENOMEM;
}

static void mlx5e_rq_free_mpwqe_info(struct mlx5e_rq *rq)
{
	int wq_sz = mlx5_wq_ll_get_size(&rq->mpwqe.wq);
	int mtt_sz = mlx5e_get_wqe_mtt_sz();
	int i;

	for (i = 0; i < wq_sz; i++) {
		struct mlx5e_mpw_info *wi = &rq->mpwqe.info[i];

		dma_unmap_single(rq->pdev, wi->umr.mtt_addr, mtt_sz,
				 PCI_DMA_TODEVICE);
	}
	kfree(rq->mpwqe.mtt_no_align);
	kfree(rq->mpwqe.info);
}

static int mlx5e_create_umr_mkey(struct mlx5_core_dev *mdev,
				 u64 npages, u8 page_shift,
				 struct mlx5_core_mkey *umr_mkey)
{
	int inlen = MLX5_ST_SZ_BYTES(create_mkey_in);
	void *mkc;
	u32 *in;
	int err;

	if (!MLX5E_VALID_NUM_MTTS(npages))
		return -EINVAL;

	in = kvzalloc(inlen, GFP_KERNEL);
	if (!in)
		return -ENOMEM;

	mkc = MLX5_ADDR_OF(create_mkey_in, in, memory_key_mkey_entry);

	MLX5_SET(mkc, mkc, free, 1);
	MLX5_SET(mkc, mkc, umr_en, 1);
	MLX5_SET(mkc, mkc, lw, 1);
	MLX5_SET(mkc, mkc, lr, 1);
	MLX5_SET(mkc, mkc, access_mode_1_0, MLX5_MKC_ACCESS_MODE_MTT);

	MLX5_SET(mkc, mkc, qpn, 0xffffff);
	MLX5_SET(mkc, mkc, pd, mdev->mlx5e_res.pdn);
	MLX5_SET64(mkc, mkc, len, npages << page_shift);
	MLX5_SET(mkc, mkc, translations_octword_size,
		 MLX5_MTT_OCTW(npages));
	MLX5_SET(mkc, mkc, log_page_size, page_shift);

	err = mlx5_core_create_mkey(mdev, umr_mkey, in, inlen);

	kvfree(in);
	return err;
}

static int mlx5e_create_rq_umr_mkey(struct mlx5_core_dev *mdev, struct mlx5e_rq *rq)
{
	u64 num_mtts = MLX5E_REQUIRED_MTTS(mlx5e_rqwq_get_size(rq));

	return mlx5e_create_umr_mkey(mdev, num_mtts, PAGE_SHIFT, &rq->umr_mkey);
}

static void mlx5e_init_frags_partition(struct mlx5e_rq *rq)
{
	int wq_sz = mlx5e_rqwq_get_size(rq);
	struct mlx5e_wqe_frag_info next_frag;
	struct mlx5e_wqe_frag_info *prev;
	struct mlx5e_wqe_frag_info *frag;
	int i, f;

	next_frag.di = &rq->wqe.di[0];
	next_frag.offset = 0;
	prev = NULL;

	for (i = 0; i < wq_sz; i++) {
		struct mlx5e_rq_frag_info *frag_info = &rq->wqe.frag_info[0];

		frag = &rq->wqe.frags[i << rq->wqe.log_num_frags];

		for (f = 0; f < rq->wqe.num_frags; f++, frag++) {
			if (next_frag.offset + frag_info[f].frag_stride > PAGE_SIZE) {
				next_frag.di++;
				next_frag.offset = 0;
				if (prev)
					prev->last_in_page = true;
			}
			*frag = next_frag;

			/* prepare next */
			next_frag.offset += frag_info[f].frag_stride;
			prev = frag;
		}
	}

	if (prev)
		prev->last_in_page = true;
}

static int mlx5e_init_di_list(struct mlx5e_rq *rq,
			      struct mlx5e_params *params,
			      int wq_sz, int cpu)
{
	int len = wq_sz << rq->wqe.log_num_frags;

	rq->wqe.di = kzalloc_node(len * sizeof(*rq->wqe.di),
				 GFP_KERNEL, cpu_to_node(cpu));
	if (!rq->wqe.di)
		return -ENOMEM;

	mlx5e_init_frags_partition(rq);

	return 0;
}

static void mlx5e_rx_cache_reduce_clean_pending(struct mlx5e_rq *rq)
{
	struct mlx5e_page_cache_reduce *reduce = &rq->page_cache.reduce;
	int i;

	if (!test_bit(MLX5E_RQ_STATE_CACHE_REDUCE_PENDING, &rq->state))
		return;

	for (i = 0; i < reduce->npages; i++)
		put_page(reduce->pending[i].page);

	clear_bit(MLX5E_RQ_STATE_CACHE_REDUCE_PENDING, &rq->state);
}

static void mlx5e_rx_cache_reduce_work(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct mlx5e_page_cache_reduce *reduce =
		container_of(dwork, struct mlx5e_page_cache_reduce, reduce_work);
	struct mlx5e_page_cache *cache =
		container_of(reduce, struct mlx5e_page_cache, reduce);
	struct mlx5e_rq *rq = container_of(cache, struct mlx5e_rq, page_cache);

	local_bh_disable();
	napi_schedule(&rq->channel->napi);
	local_bh_enable();
	mlx5e_rx_cache_reduce_clean_pending(rq);

	if (ilog2(cache->sz) > cache->log_min_sz)
		schedule_delayed_work_on(smp_processor_id(),
					 dwork, reduce->delay);
}

static int mlx5e_rx_alloc_page_cache(struct mlx5e_rq *rq,
				     int node, u8 log_init_sz)
{
	struct mlx5e_page_cache *cache = &rq->page_cache;
	struct mlx5e_page_cache_reduce *reduce = &cache->reduce;
	u32 max_sz;

	cache->log_max_sz = log_init_sz + MLX5E_PAGE_CACHE_LOG_MAX_RQ_MULT;
	cache->log_min_sz = log_init_sz;
	max_sz = 1 << cache->log_max_sz;

	cache->page_cache = kvzalloc_node(max_sz * sizeof(*cache->page_cache),
					  GFP_KERNEL, node);
	if (!cache->page_cache)
		return -ENOMEM;

	reduce->pending = kvzalloc_node(max_sz * sizeof(*reduce->pending),
					GFP_KERNEL, node);
	if (!reduce->pending)
		goto err_free_cache;

	cache->sz = 1 << cache->log_min_sz;
	cache->head = -1;
	INIT_DELAYED_WORK(&reduce->reduce_work, mlx5e_rx_cache_reduce_work);
	reduce->delay = msecs_to_jiffies(MLX5E_PAGE_CACHE_REDUCE_WORK_INTERVAL);
	reduce->graceful_period = msecs_to_jiffies(MLX5E_PAGE_CACHE_REDUCE_GRACE_PERIOD);
	reduce->next_ts = MAX_JIFFY_OFFSET; /* in init, no reduce is needed */

	return 0;

err_free_cache:
	kvfree(cache->page_cache);

	return -ENOMEM;
}

static void mlx5e_rx_free_page_cache(struct mlx5e_rq *rq)
{
	struct mlx5e_page_cache *cache = &rq->page_cache;
	struct mlx5e_page_cache_reduce *reduce = &cache->reduce;
	int i;

	cancel_delayed_work_sync(&reduce->reduce_work);
	mlx5e_rx_cache_reduce_clean_pending(rq);
	kvfree(reduce->pending);

	for (i = 0; i <= cache->head; i++) {
		struct mlx5e_dma_info *dma_info = &cache->page_cache[i];

		put_page(dma_info->page);
	}
	kvfree(cache->page_cache);
}

static int mlx5e_alloc_rq(struct mlx5e_channel *c,
			  struct mlx5e_params *params,
			  struct mlx5e_rq_param *rqp,
			  struct mlx5e_rq *rq)
{
	struct mlx5_core_dev *mdev = c->mdev;
	void *rqc = rqp->rqc;
	void *rqc_wq = MLX5_ADDR_OF(rqc, rqc, wq);
	u32 cache_init_sz;
	int wq_sz;
	int err;
	int i;

	rqp->wq.db_numa_node = cpu_to_node(c->cpu);

	rq->wq_type = params->rq_wq_type;
	rq->pdev    = c->pdev;
	rq->netdev  = c->netdev;
	rq->tstamp  = c->tstamp;
	rq->clock   = &mdev->clock;
	rq->channel = c;
	rq->ix      = c->ix;
	rq->mdev    = mdev;

#ifdef HAVE_NETDEV_BPF
	rq->xdp_prog = params->xdp_prog ? bpf_prog_inc(params->xdp_prog) : NULL;
	if (IS_ERR(rq->xdp_prog)) {
		err = PTR_ERR(rq->xdp_prog);
		rq->xdp_prog = NULL;
		goto err_rq_wq_destroy;
	}

	rq->buff.map_dir = rq->xdp_prog ? DMA_BIDIRECTIONAL : DMA_FROM_DEVICE;
#else
	rq->buff.map_dir = DMA_FROM_DEVICE;
#endif
	rq->buff.headroom = mlx5e_get_rq_headroom(c->priv, params);

	switch (rq->wq_type) {
	case MLX5_WQ_TYPE_LINKED_LIST_STRIDING_RQ:
		err = mlx5_wq_ll_create(mdev, &rqp->wq, rqc_wq, &rq->mpwqe.wq,
					&rq->wq_ctrl);
		if (err)
			return err;

		rq->mpwqe.wq.db = &rq->mpwqe.wq.db[MLX5_RCV_DBR];

		wq_sz = mlx5e_rqwq_get_size(rq);

		cache_init_sz = wq_sz * MLX5_MPWRQ_PAGES_PER_WQE;
		rq->post_wqes = mlx5e_post_rx_mpwqes;
		rq->dealloc_wqe = mlx5e_dealloc_rx_mpwqe;

		rq->handle_rx_cqe = c->priv->profile->rx_handlers.handle_rx_cqe_mpwqe;
#ifdef CONFIG_MLX5_EN_IPSEC
		if (MLX5_IPSEC_DEV(mdev)) {
			err = -EINVAL;
			netdev_err(c->netdev, "MPWQE RQ with IPSec offload not supported\n");
			goto err_rq_wq_destroy;
		}
#endif
		if (!rq->handle_rx_cqe) {
			err = -EINVAL;
			netdev_err(c->netdev, "RX handler of MPWQE RQ is not set, err %d\n", err);
			goto err_rq_wq_destroy;
		}

		rq->mpwqe.log_stride_sz = mlx5e_mpwqe_get_log_stride_size(c->priv, params);
		rq->mpwqe.num_strides = BIT(mlx5e_mpwqe_get_log_num_strides(c->priv, params));

		err = mlx5e_create_rq_umr_mkey(mdev, rq);
		if (err)
			goto err_rq_wq_destroy;
		rq->mkey_be = cpu_to_be32(rq->umr_mkey.key);

		err = mlx5e_rq_alloc_mpwqe_info(rq, c);
		if (err)
			goto err_free;
		break;
	default: /* MLX5_WQ_TYPE_CYCLIC */
		err = mlx5_wq_cyc_create(mdev, &rqp->wq, rqc_wq, &rq->wqe.wq,
					 &rq->wq_ctrl);
		if (err)
			return err;

		rq->wqe.wq.db = &rq->wqe.wq.db[MLX5_RCV_DBR];

		wq_sz = mlx5e_rqwq_get_size(rq);

		cache_init_sz = wq_sz;
		rq->wqe.num_frags = rqp->num_frags;
		rq->wqe.log_num_frags = rqp->log_num_frags;
		rq->wqe.wqe_bulk = rqp->wqe_bulk;
		rq->wqe.frags = kzalloc_node(sizeof(*rq->wqe.frags) *
					     (wq_sz << rq->wqe.log_num_frags),
					     GFP_KERNEL, cpu_to_node(c->cpu));
		if (!rq->wqe.frags)
			goto err_free;

		memcpy(rq->wqe.frag_info, rqp->frag_info, sizeof(rq->wqe.frag_info));
		err = mlx5e_init_di_list(rq, params, wq_sz, c->cpu);
		if (err)
			goto err_free;
		rq->post_wqes = mlx5e_post_rx_wqes;
		rq->dealloc_wqe = mlx5e_dealloc_rx_wqe;

#ifdef CONFIG_MLX5_EN_IPSEC
		if (c->priv->ipsec)
			rq->handle_rx_cqe = mlx5e_ipsec_handle_rx_cqe;
		else
#endif
			rq->handle_rx_cqe = c->priv->profile->rx_handlers.handle_rx_cqe;
		if (!rq->handle_rx_cqe) {
			err = -EINVAL;
			netdev_err(c->netdev, "RX handler of RQ is not set, err %d\n", err);
			goto err_free;
		}

		rq->wqe.skb_from_cqe =
			mlx5e_rx_is_linear_skb(c->priv, params) ?
			mlx5e_skb_from_cqe_linear :
			mlx5e_skb_from_cqe_nonlinear;
		rq->mkey_be = c->mkey_be;
	}

	err = mlx5e_rx_alloc_page_cache(rq, cpu_to_node(c->cpu),
					ilog2(cache_init_sz));
	if (err)
		goto err_free;

	for (i = 0; i < wq_sz; i++) {
		if (rq->wq_type == MLX5_WQ_TYPE_LINKED_LIST_STRIDING_RQ) {
			struct mlx5e_rx_wqe_ll *wqe =
				mlx5_wq_ll_get_wqe(&rq->mpwqe.wq, i);
			u32 byte_count =
				rq->mpwqe.num_strides << rq->mpwqe.log_stride_sz;
			u64 dma_offset = (u64)mlx5e_get_wqe_mtt_offset(rq, i) << PAGE_SHIFT;

			wqe->data[0].addr = cpu_to_be64(dma_offset + rq->buff.headroom);
			wqe->data[0].byte_count = cpu_to_be32(byte_count);
			wqe->data[0].lkey = rq->mkey_be;
		} else {
			struct mlx5e_rx_wqe_cyc *wqe =
				mlx5_wq_cyc_get_wqe(&rq->wqe.wq, i);
			int f;

			for (f = 0; f < rq->wqe.num_frags; f++) {
				u32 frag_size = rq->wqe.frag_info[f].frag_size |
					MLX5_HW_START_PADDING;

				wqe->data[f].byte_count = cpu_to_be32(frag_size);
				wqe->data[f].lkey = rq->mkey_be;
			}
			if (rq->wqe.num_frags != (1 << rq->wqe.log_num_frags)) {
				wqe->data[f].byte_count = 0;
				wqe->data[f].lkey = cpu_to_be32(MLX5_INVALID_LKEY);
				wqe->data[f].addr = 0;
			}
		}
	}

	INIT_WORK(&rq->am.work, mlx5e_rx_am_work);
	rq->am.mode = params->rx_cq_moderation.cq_period_mode;

#ifdef DEV_NETMAP
	mlx5e_netmap_configure_rx_ring(rq, rq->ix);
#endif /* DEV_NETMAP */

	return 0;

err_free:
	switch (rq->wq_type) {
	case MLX5_WQ_TYPE_LINKED_LIST_STRIDING_RQ:
		kfree(rq->mpwqe.info);
		mlx5_core_destroy_mkey(mdev, &rq->umr_mkey);
		break;
	default: /* MLX5_WQ_TYPE_CYCLIC */
		kfree(rq->wqe.frags);
		kfree(rq->wqe.di);
	}

err_rq_wq_destroy:
#ifdef HAVE_NETDEV_BPF
	if (rq->xdp_prog)
		bpf_prog_put(rq->xdp_prog);
#endif
	mlx5_wq_destroy(&rq->wq_ctrl);

	return err;
}

static void mlx5e_free_rq(struct mlx5e_rq *rq)
{
#ifdef HAVE_NETDEV_BPF
	if (rq->xdp_prog)
		bpf_prog_put(rq->xdp_prog);
#endif

	if (rq->page_cache.page_cache)
		mlx5e_rx_free_page_cache(rq);

	switch (rq->wq_type) {
	case MLX5_WQ_TYPE_LINKED_LIST_STRIDING_RQ:
		mlx5e_rq_free_mpwqe_info(rq);
		mlx5_core_destroy_mkey(rq->mdev, &rq->umr_mkey);
		break;
	default: /* MLX5_WQ_TYPE_CYCLIC */
		kfree(rq->wqe.frags);
		kfree(rq->wqe.di);
	}

	mlx5_wq_destroy(&rq->wq_ctrl);
}

static int mlx5e_set_delay_drop(struct mlx5e_priv *priv,
				struct mlx5e_params *params)
{
	struct mlx5e_delay_drop *delay_drop = &priv->delay_drop;
	int err = 0;

	if (!MLX5E_GET_PFLAG(params, MLX5E_PFLAG_DROPLESS_RQ))
		return 0;

	mutex_lock(&delay_drop->lock);
	if (delay_drop->activate)
		goto out;

	err = mlx5_core_set_delay_drop(priv->mdev, delay_drop->usec_timeout);
	if (err)
		goto out;

	delay_drop->activate = true;
out:
	mutex_unlock(&delay_drop->lock);
	return err;
}

static int mlx5e_create_rq(struct mlx5e_rq *rq,
			   struct mlx5e_rq_param *param)
{
	struct mlx5_core_dev *mdev = rq->mdev;

	void *in;
	void *rqc;
	void *wq;
	int inlen;
	int err;

	inlen = MLX5_ST_SZ_BYTES(create_rq_in) +
		sizeof(u64) * rq->wq_ctrl.buf.npages;
	in = kvzalloc(inlen, GFP_KERNEL);
	if (!in)
		return -ENOMEM;

	rqc = MLX5_ADDR_OF(create_rq_in, in, ctx);
	wq  = MLX5_ADDR_OF(rqc, rqc, wq);

	memcpy(rqc, param->rqc, sizeof(param->rqc));

	MLX5_SET(rqc,  rqc, cqn,		rq->cq.mcq.cqn);
	MLX5_SET(rqc,  rqc, state,		MLX5_RQC_STATE_RST);
	MLX5_SET(wq,   wq,  log_wq_pg_sz,	rq->wq_ctrl.buf.page_shift -
						MLX5_ADAPTER_PAGE_SHIFT);
	MLX5_SET64(wq, wq,  dbr_addr,		rq->wq_ctrl.db.dma);

	mlx5_fill_page_array(&rq->wq_ctrl.buf,
			     (__be64 *)MLX5_ADDR_OF(wq, wq, pas));

	err = mlx5_core_create_rq(mdev, in, inlen, &rq->rqn);

	kvfree(in);

	return err;
}

static int mlx5e_modify_rq_state(struct mlx5e_rq *rq, int curr_state,
				 int next_state)
{
	struct mlx5e_channel *c = rq->channel;
	struct mlx5_core_dev *mdev = c->mdev;

	void *in;
	void *rqc;
	int inlen;
	int err;

	inlen = MLX5_ST_SZ_BYTES(modify_rq_in);
	in = kvzalloc(inlen, GFP_KERNEL);
	if (!in)
		return -ENOMEM;

	rqc = MLX5_ADDR_OF(modify_rq_in, in, ctx);

	MLX5_SET(modify_rq_in, in, rq_state, curr_state);
	MLX5_SET(rqc, rqc, state, next_state);

	err = mlx5_core_modify_rq(mdev, rq->rqn, in, inlen);

	kvfree(in);

	return err;
}

#ifdef HAVE_NETIF_F_RXFCS
static int mlx5e_modify_rq_scatter_fcs(struct mlx5e_rq *rq, bool enable)
{
	struct mlx5e_channel *c = rq->channel;
	struct mlx5e_priv *priv = c->priv;
	struct mlx5_core_dev *mdev = priv->mdev;

	void *in;
	void *rqc;
	int inlen;
	int err;

	inlen = MLX5_ST_SZ_BYTES(modify_rq_in);
	in = kvzalloc(inlen, GFP_KERNEL);
	if (!in)
		return -ENOMEM;

	rqc = MLX5_ADDR_OF(modify_rq_in, in, ctx);

	MLX5_SET(modify_rq_in, in, rq_state, MLX5_RQC_STATE_RDY);
	MLX5_SET64(modify_rq_in, in, modify_bitmask,
		   MLX5_MODIFY_RQ_IN_MODIFY_BITMASK_SCATTER_FCS);
	MLX5_SET(rqc, rqc, scatter_fcs, enable);
	MLX5_SET(rqc, rqc, state, MLX5_RQC_STATE_RDY);

	err = mlx5_core_modify_rq(mdev, rq->rqn, in, inlen);

	kvfree(in);

	return err;
}
#endif

static int mlx5e_modify_rq_vsd(struct mlx5e_rq *rq, bool vsd)
{
	struct mlx5e_channel *c = rq->channel;
	struct mlx5_core_dev *mdev = c->mdev;
	void *in;
	void *rqc;
	int inlen;
	int err;

	inlen = MLX5_ST_SZ_BYTES(modify_rq_in);
	in = kvzalloc(inlen, GFP_KERNEL);
	if (!in)
		return -ENOMEM;

	rqc = MLX5_ADDR_OF(modify_rq_in, in, ctx);

	MLX5_SET(modify_rq_in, in, rq_state, MLX5_RQC_STATE_RDY);
	MLX5_SET64(modify_rq_in, in, modify_bitmask,
		   MLX5_MODIFY_RQ_IN_MODIFY_BITMASK_VSD);
	MLX5_SET(rqc, rqc, vsd, vsd);
	MLX5_SET(rqc, rqc, state, MLX5_RQC_STATE_RDY);

	err = mlx5_core_modify_rq(mdev, rq->rqn, in, inlen);

	kvfree(in);

	return err;
}

static void mlx5e_destroy_rq(struct mlx5e_rq *rq)
{
	mlx5_core_destroy_rq(rq->mdev, rq->rqn);
}

static int mlx5e_wait_for_min_rx_wqes(struct mlx5e_rq *rq)
{
	unsigned long exp_time = jiffies + msecs_to_jiffies(20000);
	struct mlx5e_channel *c = rq->channel;

#ifdef DEV_NETMAP
	struct netmap_adapter *na = NA(c->netdev);
	if (nm_netmap_on(na) && na->rx_rings[rq->ix]->nr_mode == NKR_NETMAP_ON)
		return 0; /* no need to wait when netmap has built wqes */
#endif

	u16 min_wqes = mlx5_min_rx_wqes(rq->wq_type, mlx5e_rqwq_get_size(rq));

	while (time_before(jiffies, exp_time)) {
		if (mlx5e_rqwq_get_cur_sz(rq) >= min_wqes)
			return 0;

		msleep(20);
	}

	netdev_warn(c->netdev, "Failed to get min RX wqes on RQN[0x%x] wq cur_sz(%d) min_rx_wqes(%d)\n",
		    rq->rqn, mlx5e_rqwq_get_cur_sz(rq), min_wqes);
	return -ETIMEDOUT;
}

static void mlx5e_free_rx_descs(struct mlx5e_rq *rq)
{
	__be16 wqe_ix_be;
	u16 wqe_ix;

	/* UMR WQE (if in progress) is always at wq->head */
	if (rq->wq_type == MLX5_WQ_TYPE_LINKED_LIST_STRIDING_RQ) {
		struct mlx5_wq_ll *wq = &rq->mpwqe.wq;

		if (rq->mpwqe.umr_in_progress)
			mlx5e_free_rx_mpwqe(rq, &rq->mpwqe.info[wq->head]);

		while (!mlx5_wq_ll_is_empty(wq)) {
			struct mlx5e_rx_wqe_ll *wqe;

			wqe_ix_be = *wq->tail_next;
			wqe_ix    = be16_to_cpu(wqe_ix_be);
			wqe       = mlx5_wq_ll_get_wqe(wq, wqe_ix);
			rq->dealloc_wqe(rq, wqe_ix);
			mlx5_wq_ll_pop(wq, wqe_ix_be,
				       &wqe->next.next_wqe_index);
		}
	} else {
		struct mlx5_wq_cyc *wq = &rq->wqe.wq;

		while (!mlx5_wq_cyc_is_empty(wq)) {
			struct mlx5e_rx_wqe_cyc *wqe;

			wqe_ix    = mlx5_wq_cyc_ctr2ix(wq, wq->wqe_ctr - wq->cur_sz);
			wqe       = mlx5_wq_cyc_get_wqe(wq, wqe_ix);
#ifdef DEV_NETMAP
			struct netmap_adapter *na = NA(rq->channel->netdev);
			if (!nm_netmap_on(na) || na->rx_rings[rq->ix]->nr_mode == NKR_NETMAP_OFF)
#endif
			rq->dealloc_wqe(rq, wqe_ix);
			mlx5_wq_cyc_pop(wq);
		}
	}
}

#ifdef CONFIG_COMPAT_LRO_ENABLED_IPOIB
static int get_skb_hdr(struct sk_buff *skb, void **iphdr,
			void **tcph, u64 *hdr_flags, void *priv)
{
	unsigned int ip_len;
	struct iphdr *iph;

	if (unlikely(skb->protocol != htons(ETH_P_IP)))
		return -1;

	/*
	* In the future we may add an else clause that verifies the
	* checksum and allows devices which do not calculate checksum
	* to use LRO.
	*/
	if (unlikely(skb->ip_summed != CHECKSUM_UNNECESSARY))
		return -1;

	/* Check for non-TCP packet */
	skb_reset_network_header(skb);
	iph = ip_hdr(skb);
	if (iph->protocol != IPPROTO_TCP)
		return -1;

	ip_len = ip_hdrlen(skb);
	skb_set_transport_header(skb, ip_len);
	*tcph = tcp_hdr(skb);

	/* check if IP header and TCP header are complete */
	if (ntohs(iph->tot_len) < ip_len + tcp_hdrlen(skb))
		return -1;

	*hdr_flags = LRO_IPV4 | LRO_TCP;
	*iphdr = iph;

	return 0;
}

static void mlx5e_rq_sw_lro_init(struct mlx5e_rq *rq)
{
	rq->sw_lro.lro_mgr.max_aggr 		= 64;
	rq->sw_lro.lro_mgr.max_desc		= MLX5E_LRO_MAX_DESC;
	rq->sw_lro.lro_mgr.lro_arr		= rq->sw_lro.lro_desc;
	rq->sw_lro.lro_mgr.get_skb_header	= get_skb_hdr;
	rq->sw_lro.lro_mgr.features		= LRO_F_NAPI;
	rq->sw_lro.lro_mgr.frag_align_pad	= NET_IP_ALIGN;
	rq->sw_lro.lro_mgr.dev			= rq->netdev;
	rq->sw_lro.lro_mgr.ip_summed		= CHECKSUM_UNNECESSARY;
	rq->sw_lro.lro_mgr.ip_summed_aggr	= CHECKSUM_UNNECESSARY;
}
#endif

static int mlx5e_open_rq(struct mlx5e_channel *c,
			 struct mlx5e_params *params,
			 struct mlx5e_rq_param *param,
			 struct mlx5e_rq *rq)
{
	int err;

	err = mlx5e_alloc_rq(c, params, param, rq);
	if (err)
		return err;

	err = mlx5e_create_rq(rq, param);
	if (err)
		goto err_free_rq;

	err = mlx5e_set_delay_drop(rq->channel->priv, params);
	if (err)
		mlx5_core_warn(c->mdev, "Failed to enable delay drop err=%d\n",
			       err);

#ifdef CONFIG_COMPAT_LRO_ENABLED_IPOIB
	mlx5e_rq_sw_lro_init(rq);
#endif

	err = mlx5e_modify_rq_state(rq, MLX5_RQC_STATE_RST, MLX5_RQC_STATE_RDY);
	if (err)
		goto err_destroy_rq;

	if (params->rx_am_enabled)
		c->rq.state |= BIT(MLX5E_RQ_STATE_AM);

	return 0;

err_destroy_rq:
	mlx5e_destroy_rq(rq);
err_free_rq:
	mlx5e_free_rq(rq);

	return err;
}

static void mlx5e_activate_rq(struct mlx5e_rq *rq)
{
	struct mlx5e_icosq *sq = &rq->channel->icosq;
	u16 pi = sq->pc & sq->wq.sz_m1;
	struct mlx5e_tx_wqe *nopwqe;

	set_bit(MLX5E_RQ_STATE_ENABLED, &rq->state);
	sq->db.ico_wqe[pi].opcode     = MLX5_OPCODE_NOP;
	nopwqe = mlx5e_post_nop(&sq->wq, sq->sqn, &sq->pc);
	mlx5e_notify_hw(&sq->wq, sq->pc, sq->uar_map, &nopwqe->ctrl);
}

static void mlx5e_deactivate_rq(struct mlx5e_rq *rq)
{
	clear_bit(MLX5E_RQ_STATE_ENABLED, &rq->state);
	napi_synchronize(&rq->channel->napi); /* prevent mlx5e_post_rx_wqes */
}

static void mlx5e_close_rq(struct mlx5e_rq *rq)
{
	cancel_work_sync(&rq->am.work);
	mlx5e_destroy_rq(rq);
	mlx5e_free_rx_descs(rq);
	mlx5e_free_rq(rq);
}

#ifdef HAVE_NETDEV_BPF
static void mlx5e_free_xdpsq_db(struct mlx5e_xdpsq *sq)
{
	kfree(sq->db.di);
}

static int mlx5e_alloc_xdpsq_db(struct mlx5e_xdpsq *sq, int numa)
{
	int wq_sz = mlx5_wq_cyc_get_size(&sq->wq);

	sq->db.di = kzalloc_node(sizeof(*sq->db.di) * wq_sz,
				     GFP_KERNEL, numa);
	if (!sq->db.di) {
		mlx5e_free_xdpsq_db(sq);
		return -ENOMEM;
	}

	return 0;
}

static int mlx5e_alloc_xdpsq(struct mlx5e_channel *c,
			     struct mlx5e_params *params,
			     struct mlx5e_sq_param *param,
			     struct mlx5e_xdpsq *sq)
{
	void *sqc_wq               = MLX5_ADDR_OF(sqc, param->sqc, wq);
	struct mlx5_core_dev *mdev = c->mdev;
	int err;

	sq->pdev      = c->pdev;
	sq->mkey_be   = c->mkey_be;
	sq->channel   = c;
	sq->uar_map   = mdev->mlx5e_res.bfreg.map;
	sq->min_inline_mode = params->tx_min_inline_mode;

	param->wq.db_numa_node = cpu_to_node(c->cpu);
	err = mlx5_wq_cyc_create(mdev, &param->wq, sqc_wq, &sq->wq, &sq->wq_ctrl);
	if (err)
		return err;
	sq->wq.db = &sq->wq.db[MLX5_SND_DBR];

	err = mlx5e_alloc_xdpsq_db(sq, cpu_to_node(c->cpu));
	if (err)
		goto err_sq_wq_destroy;

	return 0;

err_sq_wq_destroy:
	mlx5_wq_destroy(&sq->wq_ctrl);

	return err;
}

static void mlx5e_free_xdpsq(struct mlx5e_xdpsq *sq)
{
	mlx5e_free_xdpsq_db(sq);
	mlx5_wq_destroy(&sq->wq_ctrl);
}
#endif

static void mlx5e_free_icosq_db(struct mlx5e_icosq *sq)
{
	kfree(sq->db.ico_wqe);
}

static int mlx5e_alloc_icosq_db(struct mlx5e_icosq *sq, int numa)
{
	u8 wq_sz = mlx5_wq_cyc_get_size(&sq->wq);

	sq->db.ico_wqe = kzalloc_node(sizeof(*sq->db.ico_wqe) * wq_sz,
				      GFP_KERNEL, numa);
	if (!sq->db.ico_wqe)
		return -ENOMEM;

	return 0;
}

static int mlx5e_alloc_icosq(struct mlx5e_channel *c,
			     struct mlx5e_sq_param *param,
			     struct mlx5e_icosq *sq)
{
	void *sqc_wq               = MLX5_ADDR_OF(sqc, param->sqc, wq);
	struct mlx5_core_dev *mdev = c->mdev;
	int err;

	sq->mkey_be   = c->mkey_be;
	sq->channel   = c;
	sq->uar_map   = mdev->mlx5e_res.bfreg.map;

	param->wq.db_numa_node = cpu_to_node(c->cpu);
	err = mlx5_wq_cyc_create(mdev, &param->wq, sqc_wq, &sq->wq, &sq->wq_ctrl);
	if (err)
		return err;
	sq->wq.db = &sq->wq.db[MLX5_SND_DBR];

	err = mlx5e_alloc_icosq_db(sq, cpu_to_node(c->cpu));
	if (err)
		goto err_sq_wq_destroy;

	sq->edge = (sq->wq.sz_m1 + 1) - MLX5E_ICOSQ_MAX_WQEBBS;

	return 0;

err_sq_wq_destroy:
	mlx5_wq_destroy(&sq->wq_ctrl);

	return err;
}

static void mlx5e_free_icosq(struct mlx5e_icosq *sq)
{
	mlx5e_free_icosq_db(sq);
	mlx5_wq_destroy(&sq->wq_ctrl);
}

static void mlx5e_free_txqsq_db(struct mlx5e_txqsq *sq)
{
	kfree(sq->db.wqe_info);
	kfree(sq->db.dma_fifo);
}

static int mlx5e_alloc_txqsq_db(struct mlx5e_txqsq *sq, int numa)
{
	int wq_sz = mlx5_wq_cyc_get_size(&sq->wq);
	int df_sz = wq_sz * MLX5_SEND_WQEBB_NUM_DS;

	sq->db.dma_fifo = kzalloc_node(df_sz * sizeof(*sq->db.dma_fifo),
					   GFP_KERNEL, numa);
	sq->db.wqe_info = kzalloc_node(wq_sz * sizeof(*sq->db.wqe_info),
					   GFP_KERNEL, numa);
	if (!sq->db.dma_fifo || !sq->db.wqe_info) {
		mlx5e_free_txqsq_db(sq);
		return -ENOMEM;
	}

	sq->dma_fifo_mask = df_sz - 1;

	return 0;
}

static void mlx5e_sq_recover(struct work_struct *work);
static int mlx5e_alloc_txqsq(struct mlx5e_channel *c,
			     int txq_ix,
			     struct mlx5e_params *params,
			     struct mlx5e_sq_param *param,
			     struct mlx5e_txqsq *sq)
{
	void *sqc_wq               = MLX5_ADDR_OF(sqc, param->sqc, wq);
	struct mlx5_core_dev *mdev = c->mdev;
	int err;

	sq->pdev      = c->pdev;
	sq->tstamp    = c->tstamp;
	sq->clock     = &mdev->clock;
	sq->mkey_be   = c->mkey_be;
	sq->channel   = c;
	sq->txq_ix    = txq_ix;
	sq->uar_map   = mdev->mlx5e_res.bfreg.map;
	sq->max_inline      = params->tx_max_inline;
	sq->min_inline_mode = params->tx_min_inline_mode;
	INIT_WORK(&sq->recover.recover_work, mlx5e_sq_recover);
	if (MLX5_IPSEC_DEV(c->priv->mdev))
		set_bit(MLX5E_SQ_STATE_IPSEC, &sq->state);

	param->wq.db_numa_node = cpu_to_node(c->cpu);
	err = mlx5_wq_cyc_create(mdev, &param->wq, sqc_wq, &sq->wq, &sq->wq_ctrl);
	if (err)
		return err;
	sq->wq.db    = &sq->wq.db[MLX5_SND_DBR];

	err = mlx5e_alloc_txqsq_db(sq, cpu_to_node(c->cpu));
	if (err)
		goto err_sq_wq_destroy;

	sq->edge = (sq->wq.sz_m1 + 1) - MLX5_SEND_WQE_MAX_WQEBBS;

#ifdef DEV_NETMAP
	if (mlx5e_netmap_configure_tx_ring(c->priv, txq_ix))
		return 0;
#endif /* DEV_NETMAP */

	return 0;

err_sq_wq_destroy:
	mlx5_wq_destroy(&sq->wq_ctrl);

	return err;
}

static void mlx5e_free_txqsq(struct mlx5e_txqsq *sq)
{
	mlx5e_free_txqsq_db(sq);
	mlx5_wq_destroy(&sq->wq_ctrl);
}

struct mlx5e_create_sq_param {
	struct mlx5_wq_ctrl        *wq_ctrl;
	u32                         cqn;
	u32                         tisn;
	u8                          tis_lst_sz;
	u8                          min_inline_mode;
};

static int mlx5e_create_sq(struct mlx5_core_dev *mdev,
			   struct mlx5e_sq_param *param,
			   struct mlx5e_create_sq_param *csp,
			   u32 *sqn)
{
	void *in;
	void *sqc;
	void *wq;
	int inlen;
	int err;

	inlen = MLX5_ST_SZ_BYTES(create_sq_in) +
		sizeof(u64) * csp->wq_ctrl->buf.npages;
	in = kvzalloc(inlen, GFP_KERNEL);
	if (!in)
		return -ENOMEM;

	sqc = MLX5_ADDR_OF(create_sq_in, in, ctx);
	wq = MLX5_ADDR_OF(sqc, sqc, wq);

	memcpy(sqc, param->sqc, sizeof(param->sqc));
	MLX5_SET(sqc,  sqc, tis_lst_sz, csp->tis_lst_sz);
	MLX5_SET(sqc,  sqc, tis_num_0, csp->tisn);
	MLX5_SET(sqc,  sqc, cqn, csp->cqn);

	if (MLX5_CAP_ETH(mdev, wqe_inline_mode) == MLX5_CAP_INLINE_MODE_VPORT_CONTEXT)
		MLX5_SET(sqc,  sqc, min_wqe_inline_mode, csp->min_inline_mode);

	MLX5_SET(sqc,  sqc, state, MLX5_SQC_STATE_RST);
	MLX5_SET(sqc,  sqc, flush_in_error_en, 1);

	MLX5_SET(wq,   wq, wq_type,       MLX5_WQ_TYPE_CYCLIC);
	MLX5_SET(wq,   wq, uar_page,      mdev->mlx5e_res.bfreg.index);
	MLX5_SET(wq,   wq, log_wq_pg_sz,  csp->wq_ctrl->buf.page_shift -
					  MLX5_ADAPTER_PAGE_SHIFT);
	MLX5_SET64(wq, wq, dbr_addr,      csp->wq_ctrl->db.dma);

	mlx5_fill_page_array(&csp->wq_ctrl->buf, (__be64 *)MLX5_ADDR_OF(wq, wq, pas));

	err = mlx5_core_create_sq(mdev, in, inlen, sqn);

	kvfree(in);

	return err;
}

struct mlx5e_modify_sq_param {
	int curr_state;
	int next_state;
	bool rl_update;
	int rl_index;
};

static int mlx5e_modify_sq(struct mlx5_core_dev *mdev, u32 sqn,
			   struct mlx5e_modify_sq_param *p)
{
	void *in;
	void *sqc;
	int inlen;
	int err;

	inlen = MLX5_ST_SZ_BYTES(modify_sq_in);
	in = kvzalloc(inlen, GFP_KERNEL);
	if (!in)
		return -ENOMEM;

	sqc = MLX5_ADDR_OF(modify_sq_in, in, ctx);

	MLX5_SET(modify_sq_in, in, sq_state, p->curr_state);
	MLX5_SET(sqc, sqc, state, p->next_state);
	if (p->rl_update && p->next_state == MLX5_SQC_STATE_RDY) {
		MLX5_SET64(modify_sq_in, in, modify_bitmask, 1);
		MLX5_SET(sqc,  sqc, packet_pacing_rate_limit_index, p->rl_index);
	}

	err = mlx5_core_modify_sq(mdev, sqn, in, inlen);

	kvfree(in);

	return err;
}

static void mlx5e_destroy_sq(struct mlx5_core_dev *mdev, u32 sqn)
{
	mlx5_core_destroy_sq(mdev, sqn);
}

static int mlx5e_create_sq_rdy(struct mlx5_core_dev *mdev,
			       struct mlx5e_sq_param *param,
			       struct mlx5e_create_sq_param *csp,
			       u32 *sqn)
{
	struct mlx5e_modify_sq_param msp = {0};
	int err;

	err = mlx5e_create_sq(mdev, param, csp, sqn);
	if (err)
		return err;

	msp.curr_state = MLX5_SQC_STATE_RST;
	msp.next_state = MLX5_SQC_STATE_RDY;
	err = mlx5e_modify_sq(mdev, *sqn, &msp);
	if (err)
		mlx5e_destroy_sq(mdev, *sqn);

	return err;
}

static int mlx5e_set_sq_maxrate(struct net_device *dev,
				struct mlx5e_txqsq *sq, u32 rate);

static int mlx5e_open_txqsq(struct mlx5e_channel *c,
			    u32 tisn,
			    int txq_ix,
			    struct mlx5e_params *params,
			    struct mlx5e_sq_param *param,
			    struct mlx5e_txqsq *sq)
{
	struct mlx5e_create_sq_param csp = {};
	u32 tx_rate;
	int err;

	err = mlx5e_alloc_txqsq(c, txq_ix, params, param, sq);
	if (err)
		return err;

	csp.tisn            = tisn;
	csp.tis_lst_sz      = 1;
	csp.cqn             = sq->cq.mcq.cqn;
	csp.wq_ctrl         = &sq->wq_ctrl;
	csp.min_inline_mode = sq->min_inline_mode;
	err = mlx5e_create_sq_rdy(c->mdev, param, &csp, &sq->sqn);
	if (err)
		goto err_free_txqsq;

	tx_rate = c->priv->tx_rates[sq->txq_ix];
	if (tx_rate)
		mlx5e_set_sq_maxrate(c->netdev, sq, tx_rate);

	return 0;

err_free_txqsq:
	clear_bit(MLX5E_SQ_STATE_ENABLED, &sq->state);
	mlx5e_free_txqsq(sq);

	return err;
}

static void mlx5e_reset_txqsq_cc_pc(struct mlx5e_txqsq *sq)
{
	WARN_ONCE(sq->cc != sq->pc,
		  "SQ 0x%x: cc (0x%x) ! = pc (0x%x)\n",
		  sq->sqn, sq->cc, sq->pc);
	sq->cc = 0;
	sq->dma_fifo_cc = 0;
	sq->pc = 0;
}

static void mlx5e_activate_txqsq(struct mlx5e_txqsq *sq)
{
	sq->txq = netdev_get_tx_queue(sq->channel->netdev, sq->txq_ix);
	clear_bit(MLX5E_SQ_STATE_RECOVERING, &sq->state);
	set_bit(MLX5E_SQ_STATE_ENABLED, &sq->state);
	netdev_tx_reset_queue(sq->txq);
	netif_tx_start_queue(sq->txq);
}

static inline void netif_tx_disable_queue(struct netdev_queue *txq)
{
	__netif_tx_lock_bh(txq);
	netif_tx_stop_queue(txq);
	__netif_tx_unlock_bh(txq);
}

static void mlx5e_deactivate_txqsq(struct mlx5e_txqsq *sq)
{
	struct mlx5e_channel *c = sq->channel;

	clear_bit(MLX5E_SQ_STATE_ENABLED, &sq->state);
	/* prevent netif_tx_wake_queue */
	napi_synchronize(&c->napi);

	netif_tx_disable_queue(sq->txq);

	/* last doorbell out, godspeed .. */
#ifdef DEV_NETMAP
	if (!nm_netmap_on(NA(sq->txq->dev))) // TODO
#endif
	if (mlx5e_wqc_has_room_for(&sq->wq, sq->cc, sq->pc, 1)) {
		struct mlx5e_tx_wqe *nop;

		sq->db.wqe_info[(sq->pc & sq->wq.sz_m1)].skb = NULL;
		nop = mlx5e_post_nop(&sq->wq, sq->sqn, &sq->pc);
		mlx5e_notify_hw(&sq->wq, sq->pc, sq->uar_map, &nop->ctrl);
	}
}

static void mlx5e_close_txqsq(struct mlx5e_txqsq *sq)
{
	struct mlx5e_channel *c = sq->channel;
	struct mlx5_core_dev *mdev = c->mdev;
	struct mlx5_rate_limit rl = {0};


#ifdef DEV_NETMAP
	if (nm_netmap_on(NA(sq->txq->dev))) // TODO
		mlx5e_netmap_tx_flush(sq); /* handle any CQEs */
#endif

	mlx5e_destroy_sq(mdev, sq->sqn);
	if (sq->rate_limit) {
		rl.rate = sq->rate_limit;
		mlx5_rl_remove_rate(mdev, &rl);
	}
	mlx5e_free_txqsq_descs(sq);
	mlx5e_free_txqsq(sq);
}

static int mlx5e_wait_for_sq_flush(struct mlx5e_txqsq *sq)
{
	unsigned long exp_time = jiffies + msecs_to_jiffies(2000);

	while (time_before(jiffies, exp_time)) {
		if (sq->cc == sq->pc)
			return 0;

		msleep(20);
#ifdef DEV_NETMAP
		if (nm_netmap_on(NA(sq->txq->dev))) // TODO
			mlx5e_netmap_tx_flush(sq); /* handle any CQEs */
#endif
	}

	netdev_err(sq->channel->netdev,
		   "Wait for SQ 0x%x flush timeout (sq cc = 0x%x, sq pc = 0x%x)\n",
		   sq->sqn, sq->cc, sq->pc);

	return -ETIMEDOUT;
}

static int mlx5e_sq_to_ready(struct mlx5e_txqsq *sq, int curr_state)
{
	struct mlx5_core_dev *mdev = sq->channel->mdev;
	struct net_device *dev = sq->channel->netdev;
	struct mlx5e_modify_sq_param msp = {0};
	int err;

	msp.curr_state = curr_state;
	msp.next_state = MLX5_SQC_STATE_RST;

	err = mlx5e_modify_sq(mdev, sq->sqn, &msp);
	if (err) {
		netdev_err(dev, "Failed to move sq 0x%x to reset\n", sq->sqn);
		return err;
	}

	memset(&msp, 0, sizeof(msp));
	msp.curr_state = MLX5_SQC_STATE_RST;
	msp.next_state = MLX5_SQC_STATE_RDY;

	err = mlx5e_modify_sq(mdev, sq->sqn, &msp);
	if (err) {
		netdev_err(dev, "Failed to move sq 0x%x to ready\n", sq->sqn);
		return err;
	}

	return 0;
}

static void mlx5e_sq_recover(struct work_struct *work)
{
	struct mlx5e_txqsq_recover *recover =
		container_of(work, struct mlx5e_txqsq_recover,
			     recover_work);
	struct mlx5e_txqsq *sq = container_of(recover, struct mlx5e_txqsq,
					      recover);
	struct mlx5_core_dev *mdev = sq->channel->mdev;
	struct net_device *dev = sq->channel->netdev;
	u8 state;
	int err;

	err = mlx5_core_query_sq_state(mdev, sq->sqn, &state);
	if (err) {
		netdev_err(dev, "Failed to query SQ 0x%x state. err = %d\n",
			   sq->sqn, err);
		return;
	}

	if (state != MLX5_RQC_STATE_ERR) {
		netdev_err(dev, "SQ 0x%x not in ERROR state\n", sq->sqn);
		return;
	}

	netif_tx_disable_queue(sq->txq);

	if (mlx5e_wait_for_sq_flush(sq))
		return;

	/* If the interval between two consecutive recovers per SQ is too
	 * short, don't recover to avoid infinite loop of ERR_CQE -> recover.
	 * If we reached this state, there is probably a bug that needs to be
	 * fixed. let's keep the queue close and let tx timeout cleanup.
	 */
	if (jiffies_to_msecs(jiffies - recover->last_recover) <
	    MLX5E_SQ_RECOVER_MIN_INTERVAL) {
		netdev_err(dev, "Recover SQ 0x%x canceled, too many error CQEs\n",
			   sq->sqn);
		return;
	}

	/* At this point, no new packets will arrive from the stack as TXQ is
	 * marked with QUEUE_STATE_DRV_XOFF. In addition, NAPI cleared all
	 * pending WQEs.  SQ can safely reset the SQ.
	 */
	if (mlx5e_sq_to_ready(sq, state))
		return;

	mlx5e_reset_txqsq_cc_pc(sq);
	sq->stats.recover++;
	recover->last_recover = jiffies;
	mlx5e_activate_txqsq(sq);
}

static int mlx5e_open_icosq(struct mlx5e_channel *c,
			    struct mlx5e_params *params,
			    struct mlx5e_sq_param *param,
			    struct mlx5e_icosq *sq)
{
	struct mlx5e_create_sq_param csp = {};
	int err;

	err = mlx5e_alloc_icosq(c, param, sq);
	if (err)
		return err;

	csp.cqn             = sq->cq.mcq.cqn;
	csp.wq_ctrl         = &sq->wq_ctrl;
	csp.min_inline_mode = params->tx_min_inline_mode;
	set_bit(MLX5E_SQ_STATE_ENABLED, &sq->state);
	err = mlx5e_create_sq_rdy(c->mdev, param, &csp, &sq->sqn);
	if (err)
		goto err_free_icosq;

	return 0;

err_free_icosq:
	clear_bit(MLX5E_SQ_STATE_ENABLED, &sq->state);
	mlx5e_free_icosq(sq);

	return err;
}

static void mlx5e_close_icosq(struct mlx5e_icosq *sq)
{
	struct mlx5e_channel *c = sq->channel;

	clear_bit(MLX5E_SQ_STATE_ENABLED, &sq->state);
	napi_synchronize(&c->napi);

	mlx5e_destroy_sq(c->mdev, sq->sqn);
	mlx5e_free_icosq(sq);
}

#ifdef HAVE_NETDEV_BPF
static int mlx5e_open_xdpsq(struct mlx5e_channel *c,
			    struct mlx5e_params *params,
			    struct mlx5e_sq_param *param,
			    struct mlx5e_xdpsq *sq)
{
	unsigned int ds_cnt = MLX5E_XDP_TX_DS_COUNT;
	struct mlx5e_create_sq_param csp = {};
	unsigned int inline_hdr_sz = 0;
	int err;
	int i;

	err = mlx5e_alloc_xdpsq(c, params, param, sq);
	if (err)
		return err;

	csp.tis_lst_sz      = 1;
	csp.tisn            = c->priv->tisn[0]; /* tc = 0 */
	csp.cqn             = sq->cq.mcq.cqn;
	csp.wq_ctrl         = &sq->wq_ctrl;
	csp.min_inline_mode = sq->min_inline_mode;
	set_bit(MLX5E_SQ_STATE_ENABLED, &sq->state);
	err = mlx5e_create_sq_rdy(c->mdev, param, &csp, &sq->sqn);
	if (err)
		goto err_free_xdpsq;

	if (sq->min_inline_mode != MLX5_INLINE_MODE_NONE) {
		inline_hdr_sz = MLX5E_XDP_MIN_INLINE;
		ds_cnt++;
	}

	/* Pre initialize fixed WQE fields */
	for (i = 0; i < mlx5_wq_cyc_get_size(&sq->wq); i++) {
		struct mlx5e_tx_wqe      *wqe  = mlx5_wq_cyc_get_wqe(&sq->wq, i);
		struct mlx5_wqe_ctrl_seg *cseg = &wqe->ctrl;
		struct mlx5_wqe_eth_seg  *eseg = &wqe->eth;
		struct mlx5_wqe_data_seg *dseg;

		cseg->qpn_ds = cpu_to_be32((sq->sqn << 8) | ds_cnt);
		eseg->inline_hdr.sz = cpu_to_be16(inline_hdr_sz);

		dseg = (struct mlx5_wqe_data_seg *)cseg + (ds_cnt - 1);
		dseg->lkey = sq->mkey_be;
	}

	return 0;

err_free_xdpsq:
	clear_bit(MLX5E_SQ_STATE_ENABLED, &sq->state);
	mlx5e_free_xdpsq(sq);

	return err;
}

static void mlx5e_close_xdpsq(struct mlx5e_xdpsq *sq)
{
	struct mlx5e_channel *c = sq->channel;

	clear_bit(MLX5E_SQ_STATE_ENABLED, &sq->state);
	napi_synchronize(&c->napi);

	mlx5e_destroy_sq(c->mdev, sq->sqn);
	mlx5e_free_xdpsq_descs(sq);
	mlx5e_free_xdpsq(sq);
}

#endif /* HAVE_NETDEV_BPF */

static int mlx5e_alloc_cq_common(struct mlx5_core_dev *mdev,
				 struct mlx5e_cq_param *param,
				 struct mlx5e_cq *cq)
{
	struct mlx5_core_cq *mcq = &cq->mcq;
	int eqn_not_used;
	unsigned int irqn;
	int err;
	u32 i;

	err = mlx5_cqwq_create(mdev, &param->wq, param->cqc, &cq->wq,
			       &cq->wq_ctrl);
	if (err)
		return err;

	mlx5_vector2eqn(mdev, param->eq_ix, &eqn_not_used, &irqn);

	mcq->cqe_sz     = 64;
	mcq->set_ci_db  = cq->wq_ctrl.db.db;
	mcq->arm_db     = cq->wq_ctrl.db.db + 1;
	*mcq->set_ci_db = 0;
	*mcq->arm_db    = 0;
	mcq->vector     = param->eq_ix;
	mcq->comp       = mlx5e_completion_event;
	mcq->event      = mlx5e_cq_error_event;
	mcq->irqn       = irqn;

	for (i = 0; i < mlx5_cqwq_get_size(&cq->wq); i++) {
		struct mlx5_cqe64 *cqe = mlx5_cqwq_get_wqe(&cq->wq, i);

		cqe->op_own = 0xf1;
	}

	cq->mdev = mdev;

	return 0;
}

static int mlx5e_alloc_cq(struct mlx5e_channel *c,
			  struct mlx5e_cq_param *param,
			  struct mlx5e_cq *cq)
{
	struct mlx5_core_dev *mdev = c->priv->mdev;
	int err;

	param->wq.buf_numa_node = cpu_to_node(c->cpu);
	param->wq.db_numa_node  = cpu_to_node(c->cpu);
	param->eq_ix   = c->ix % mdev->priv.eq_table.num_comp_vectors;

	err = mlx5e_alloc_cq_common(mdev, param, cq);

	cq->napi    = &c->napi;
	cq->channel = c;

	return err;
}

static void mlx5e_free_cq(struct mlx5e_cq *cq)
{
	mlx5_cqwq_destroy(&cq->wq_ctrl);
}

static int mlx5e_create_cq(struct mlx5e_cq *cq, struct mlx5e_cq_param *param)
{
	struct mlx5_core_dev *mdev = cq->mdev;
	struct mlx5_core_cq *mcq = &cq->mcq;

	void *in;
	void *cqc;
	int inlen;
	unsigned int irqn_not_used;
	int eqn;
	int err;

	inlen = MLX5_ST_SZ_BYTES(create_cq_in) +
		sizeof(u64) * cq->wq_ctrl.frag_buf.npages;
	in = kvzalloc(inlen, GFP_KERNEL);
	if (!in)
		return -ENOMEM;

	cqc = MLX5_ADDR_OF(create_cq_in, in, cq_context);

	memcpy(cqc, param->cqc, sizeof(param->cqc));

	mlx5_fill_page_frag_array(&cq->wq_ctrl.frag_buf,
				  (__be64 *)MLX5_ADDR_OF(create_cq_in, in, pas));

	mlx5_vector2eqn(mdev, param->eq_ix, &eqn, &irqn_not_used);

	MLX5_SET(cqc,   cqc, cq_period_mode, param->cq_period_mode);
	MLX5_SET(cqc,   cqc, c_eqn,         eqn);
	MLX5_SET(cqc,   cqc, uar_page,      mdev->priv.uar->index);
	MLX5_SET(cqc,   cqc, log_page_size, cq->wq_ctrl.frag_buf.page_shift -
					    MLX5_ADAPTER_PAGE_SHIFT);
	MLX5_SET64(cqc, cqc, dbr_addr,      cq->wq_ctrl.db.dma);

	err = mlx5_core_create_cq(mdev, mcq, in, inlen);

	kvfree(in);

	if (err)
		return err;

	mlx5e_cq_arm(cq);

	return 0;
}

static void mlx5e_destroy_cq(struct mlx5e_cq *cq)
{
	mlx5_core_destroy_cq(cq->mdev, &cq->mcq);
}

static int mlx5e_open_cq(struct mlx5e_channel *c,
			 struct mlx5e_cq_moder moder,
			 struct mlx5e_cq_param *param,
			 struct mlx5e_cq *cq)
{
	struct mlx5_core_dev *mdev = c->mdev;
	int err;

	err = mlx5e_alloc_cq(c, param, cq);
	if (err)
		return err;

	err = mlx5e_create_cq(cq, param);
	if (err)
		goto err_free_cq;

	if (MLX5_CAP_GEN(mdev, cq_moderation))
		mlx5_core_modify_cq_moderation(mdev, &cq->mcq, moder.usec, moder.pkts);
	return 0;

err_free_cq:
	mlx5e_free_cq(cq);

	return err;
}

static void mlx5e_close_cq(struct mlx5e_cq *cq)
{
	mlx5e_destroy_cq(cq);
	mlx5e_free_cq(cq);
}

static int mlx5e_get_cpu(struct mlx5e_priv *priv, int ix)
{
	ix %= priv->mdev->priv.eq_table.num_comp_vectors;
	return cpumask_first(priv->mdev->priv.irq_info[ix].mask);
}

static int mlx5e_open_tx_cqs(struct mlx5e_channel *c,
			     struct mlx5e_params *params,
			     struct mlx5e_channel_param *cparam)
{
	int err;
	int tc;
#ifdef CONFIG_MLX5_EN_SPECIAL_SQ
	int i;
#endif

	for (tc = 0; tc < c->num_tc; tc++) {
		err = mlx5e_open_cq(c, params->tx_cq_moderation,
				    &cparam->tx_cq, &c->sq[tc].cq);
		if (err)
			goto err_close_tx_cqs;
	}

#ifdef CONFIG_MLX5_EN_SPECIAL_SQ
	for (i = 0; i < c->num_special_sq; i++) {
		err = mlx5e_open_cq(c, params->tx_cq_moderation,
				    &cparam->tx_cq, &c->special_sq[i].cq);
		if (err)
			goto err_close_special_tx_cqs;
	}
#endif

	return 0;

#ifdef CONFIG_MLX5_EN_SPECIAL_SQ
err_close_special_tx_cqs:
	for (i--; i >= 0; i--)
		mlx5e_close_cq(&c->special_sq[i].cq);
#endif

err_close_tx_cqs:
	for (tc--; tc >= 0; tc--)
		mlx5e_close_cq(&c->sq[tc].cq);

	return err;
}

static void mlx5e_close_tx_cqs(struct mlx5e_channel *c)
{
	int tc;

#ifdef CONFIG_MLX5_EN_SPECIAL_SQ
	for (tc = 0; tc < c->num_special_sq; tc++)
		mlx5e_close_cq(&c->special_sq[tc].cq);
#endif

	for (tc = 0; tc < c->num_tc; tc++)
		mlx5e_close_cq(&c->sq[tc].cq);
}

static int mlx5e_open_sqs(struct mlx5e_channel *c,
			  struct mlx5e_params *params,
			  struct mlx5e_channel_param *cparam)
{
	int err;
	int tc;
#ifdef CONFIG_MLX5_EN_SPECIAL_SQ
	int i;
#endif

	for (tc = 0; tc < params->num_tc; tc++) {
		int txq_ix = c->ix + tc * params->num_channels;

		err = mlx5e_open_txqsq(c, c->priv->tisn[tc], txq_ix,
				       params, &cparam->sq, &c->sq[tc]);
		if (err)
			goto err_close_sqs;
	}

#ifdef CONFIG_MLX5_EN_SPECIAL_SQ
	for (i = 0; i < c->num_special_sq; i++) {
		int special_sq_ix = c->ix +
			(i + params->num_tc) * params->num_channels;

		err = mlx5e_open_txqsq(c, c->priv->tisn[0], special_sq_ix,
				       params, &cparam->sq, &c->special_sq[i]);
		if (err)
			goto err_close_special_sqs;
	}
#endif

	return 0;

#ifdef CONFIG_MLX5_EN_SPECIAL_SQ
err_close_special_sqs:
	for (i--; i >= 0; i--)
		mlx5e_close_txqsq(&c->special_sq[i]);
#endif

err_close_sqs:
	for (tc--; tc >= 0; tc--)
		mlx5e_close_txqsq(&c->sq[tc]);

	return err;
}

static void mlx5e_close_sqs(struct mlx5e_channel *c)
{
	int tc;

#ifdef CONFIG_MLX5_EN_SPECIAL_SQ
	for (tc = 0; tc < c->num_special_sq; tc++)
		mlx5e_close_txqsq(&c->special_sq[tc]);
#endif

	for (tc = 0; tc < c->num_tc; tc++)
		mlx5e_close_txqsq(&c->sq[tc]);
}

static int mlx5e_set_sq_maxrate(struct net_device *dev,
				struct mlx5e_txqsq *sq, u32 rate)
{
	struct mlx5e_priv *priv = netdev_priv(dev);
	struct mlx5_core_dev *mdev = priv->mdev;
	struct mlx5e_modify_sq_param msp = {0};
	struct mlx5_rate_limit rl = {0};
	u16 rl_index = 0;
	int err;

	if (rate == sq->rate_limit)
		/* nothing to do */
		return 0;

	if (sq->rate_limit) {
		rl.rate = sq->rate_limit;
		/* remove current rl index to free space to next ones */
		mlx5_rl_remove_rate(mdev, &rl);
	}

	sq->rate_limit = 0;

	if (rate) {
		rl.rate = rate;
		err = mlx5_rl_add_rate(mdev, &rl_index, &rl);
		if (err) {
			netdev_err(dev, "Failed configuring rate %u: %d\n",
				   rate, err);
			return err;
		}
	}

	msp.curr_state = MLX5_SQC_STATE_RDY;
	msp.next_state = MLX5_SQC_STATE_RDY;
	msp.rl_index   = rl_index;
	msp.rl_update  = true;
	err = mlx5e_modify_sq(mdev, sq->sqn, &msp);
	if (err) {
		netdev_err(dev, "Failed configuring rate %u: %d\n",
			   rate, err);
		/* remove the rate from the table */
		if (rate)
			mlx5_rl_remove_rate(mdev, &rl);
		return err;
	}

	sq->rate_limit = rate;
	return 0;
}

#if defined(HAVE_NDO_SET_TX_MAXRATE) || defined(HAVE_NDO_SET_TX_MAXRATE_EXTENDED)
static int mlx5e_set_tx_maxrate(struct net_device *dev, int index, u32 rate)
{
	struct mlx5e_priv *priv = netdev_priv(dev);
	struct mlx5_core_dev *mdev = priv->mdev;
	struct mlx5e_txqsq *sq = priv->txq2sq[index];
	int err = 0;

	if (!mlx5_rl_is_supported(mdev)) {
		netdev_err(dev, "Rate limiting is not supported on this device\n");
		return -EINVAL;
	}

	/* rate is given in Mb/sec, HW config is in Kb/sec */
	rate = rate << 10;

	/* Check whether rate in valid range, 0 is always valid */
	if (rate && !mlx5_rl_is_in_range(mdev, rate)) {
		netdev_err(dev, "TX rate %u, is not in range\n", rate);
		return -ERANGE;
	}

	mutex_lock(&priv->state_lock);
	if (test_bit(MLX5E_STATE_OPENED, &priv->state))
		err = mlx5e_set_sq_maxrate(dev, sq, rate);
	if (!err)
		priv->tx_rates[index] = rate;
	mutex_unlock(&priv->state_lock);

	return err;
}
#endif

static int mlx5e_open_channel(struct mlx5e_priv *priv, int ix,
			      struct mlx5e_params *params,
			      struct mlx5e_channel_param *cparam,
			      struct mlx5e_channel **cp)
{
	struct mlx5e_cq_moder icocq_moder = {0, 0};
	struct net_device *netdev = priv->netdev;
	int cpu = mlx5e_get_cpu(priv, ix);
	struct mlx5e_channel *c;
#if defined(HAVE_IRQ_DESC_GET_IRQ_DATA) && defined(HAVE_IRQ_TO_DESC_EXPORTED)
	unsigned int irq;
#endif
	int err;
#if defined(HAVE_IRQ_DESC_GET_IRQ_DATA) && defined(HAVE_IRQ_TO_DESC_EXPORTED)
	int eqn;
#endif

	c = kzalloc_node(sizeof(*c), GFP_KERNEL, cpu_to_node(cpu));
	if (!c)
		return -ENOMEM;

	c->priv     = priv;
	c->mdev     = priv->mdev;
	c->tstamp   = &priv->tstamp;
	c->ix       = ix;
	c->cpu      = cpu;
	c->pdev     = &priv->mdev->pdev->dev;
	c->netdev   = priv->netdev;
	c->mkey_be  = cpu_to_be32(priv->mdev->mlx5e_res.mkey.key);
	c->num_tc   = params->num_tc;
#ifdef HAVE_NETDEV_BPF
	c->xdp      = !!params->xdp_prog;
#endif

#ifdef CONFIG_MLX5_EN_SPECIAL_SQ
	c->num_special_sq = params->num_rl_txqs / params->num_channels +
		!!(ix < params->num_rl_txqs % params->num_channels);
	c->special_sq = kzalloc_node(sizeof(struct mlx5e_txqsq) * c->num_special_sq,
				      GFP_KERNEL, cpu_to_node(cpu));
	if (!c->special_sq) {
		err = -ENOMEM;
		goto err_ch_free;
	}
#endif

#if defined(HAVE_IRQ_DESC_GET_IRQ_DATA) && defined(HAVE_IRQ_TO_DESC_EXPORTED)
	mlx5_vector2eqn(priv->mdev, ix, &eqn, &irq);
	c->irq_desc = irq_to_desc(irq);
#endif

	netif_napi_add(netdev, &c->napi, mlx5e_napi_poll, 64);

	err = mlx5e_open_cq(c, icocq_moder, &cparam->icosq_cq, &c->icosq.cq);
	if (err)
		goto err_napi_del;

	err = mlx5e_open_tx_cqs(c, params, cparam);
	if (err)
		goto err_close_icosq_cq;

	err = mlx5e_open_cq(c, params->rx_cq_moderation, &cparam->rx_cq, &c->rq.cq);
	if (err)
		goto err_close_tx_cqs;

#ifdef HAVE_NETDEV_BPF
	/* XDP SQ CQ params are same as normal TXQ sq CQ params */
	err = c->xdp ? mlx5e_open_cq(c, params->tx_cq_moderation,
				     &cparam->tx_cq, &c->rq.xdpsq.cq) : 0;
	if (err)
		goto err_close_rx_cq;
#endif

	napi_enable(&c->napi);

	err = mlx5e_open_icosq(c, params, &cparam->icosq, &c->icosq);
	if (err)
		goto err_disable_napi;

	err = mlx5e_open_sqs(c, params, cparam);
	if (err)
		goto err_close_icosq;

#ifdef HAVE_NETDEV_BPF
	err = c->xdp ? mlx5e_open_xdpsq(c, params, &cparam->xdp_sq, &c->rq.xdpsq) : 0;
	if (err)
		goto err_close_sqs;
#endif

	err = mlx5e_open_rq(c, params, &cparam->rq, &c->rq);
	if (err)
		goto err_close_xdp_sq;

	*cp = c;

	return 0;
err_close_xdp_sq:
#ifdef HAVE_NETDEV_BPF
	if (c->xdp)
		mlx5e_close_xdpsq(&c->rq.xdpsq);

err_close_sqs:
#endif
	mlx5e_close_sqs(c);

err_close_icosq:
	mlx5e_close_icosq(&c->icosq);

err_disable_napi:
	napi_disable(&c->napi);
#ifdef HAVE_NETDEV_BPF
	if (c->xdp)
		mlx5e_close_cq(&c->rq.xdpsq.cq);

err_close_rx_cq:
#endif
	mlx5e_close_cq(&c->rq.cq);

err_close_tx_cqs:
	mlx5e_close_tx_cqs(c);

err_close_icosq_cq:
	mlx5e_close_cq(&c->icosq.cq);

err_napi_del:
	netif_napi_del(&c->napi);
#ifdef CONFIG_MLX5_EN_SPECIAL_SQ
	kfree(c->special_sq);

err_ch_free:
#endif
	kfree(c);

	return err;
}

static void mlx5e_activate_channel(struct mlx5e_channel *c)
{
	int tc;

	for (tc = 0; tc < c->num_tc; tc++)
		mlx5e_activate_txqsq(&c->sq[tc]);
#ifdef CONFIG_MLX5_EN_SPECIAL_SQ
	for (tc = 0; tc < c->num_special_sq; tc++)
		mlx5e_activate_txqsq(&c->special_sq[tc]);
#endif
	mlx5e_activate_rq(&c->rq);
#if (LINUX_VERSION_CODE < KERNEL_VERSION(3,9,0)) || \
	defined(CONFIG_COMPAT_IS_NETIF_SET_XPS_QUEUE_NOT_CONST_CPUMASK)
	netif_set_xps_queue(c->netdev, (struct cpumask *)get_cpu_mask(c->cpu), c->ix);
#else
	netif_set_xps_queue(c->netdev, get_cpu_mask(c->cpu), c->ix);
#endif
	if (c->ix < c->priv->mdev->priv.eq_table.num_comp_vectors)
		mlx5_rename_comp_eq(c->priv->mdev, c->ix, c->priv->netdev->name);
}

static void mlx5e_deactivate_channel(struct mlx5e_channel *c)
{
	int tc;

	if (c->ix < c->priv->mdev->priv.eq_table.num_comp_vectors)
		mlx5_rename_comp_eq(c->priv->mdev, c->ix, NULL);
	mlx5e_deactivate_rq(&c->rq);
#ifdef CONFIG_MLX5_EN_SPECIAL_SQ
	for (tc = 0; tc < c->num_special_sq; tc++)
		mlx5e_deactivate_txqsq(&c->special_sq[tc]);
#endif
	for (tc = 0; tc < c->num_tc; tc++)
		mlx5e_deactivate_txqsq(&c->sq[tc]);
}

static void mlx5e_close_channel(struct mlx5e_channel *c)
{
	mlx5e_close_rq(&c->rq);
#ifdef HAVE_NETDEV_BPF
	if (c->xdp)
		mlx5e_close_xdpsq(&c->rq.xdpsq);
#endif
	mlx5e_close_sqs(c);
	mlx5e_close_icosq(&c->icosq);
	napi_disable(&c->napi);
#ifdef HAVE_NETDEV_BPF
	if (c->xdp)
		mlx5e_close_cq(&c->rq.xdpsq.cq);
#endif
	mlx5e_close_cq(&c->rq.cq);
	mlx5e_close_tx_cqs(c);
	mlx5e_close_cq(&c->icosq.cq);
	netif_napi_del(&c->napi);

#ifdef CONFIG_MLX5_EN_SPECIAL_SQ
	kfree(c->special_sq);
#endif
	kfree(c);
}

static void mlx5e_calc_rq_frags_info(struct mlx5e_priv *priv,
				     struct mlx5e_params *params,
				     struct mlx5e_rq_param *param)
{
	u16 hw_mtu = MLX5E_SW2HW_MTU(priv, priv->netdev->mtu);
	int frag_size_max = 2048;
	u32 byte_count = hw_mtu;
	u32 buf_size = 0;
	int i;

#ifdef CONFIG_MLX5_EN_IPSEC
	if (MLX5_IPSEC_DEV(priv->mdev))
		byte_count += MLX5E_METADATA_ETHER_LEN;
#endif

	if (mlx5e_rx_is_linear_skb(priv, params)) {
		int frag_stride = mlx5e_rx_get_linear_frag_sz(priv, params);

		frag_stride = roundup_pow_of_two(frag_stride);
		param->frag_info[0].frag_size = byte_count;
		param->frag_info[0].frag_stride = frag_stride;
		param->num_frags = 1;
		param->wqe_bulk = PAGE_SIZE / frag_stride;
		return;
	}

	if (byte_count > PAGE_SIZE +
	    (MLX5E_MAX_RX_FRAGS - 1) * frag_size_max)
		frag_size_max = PAGE_SIZE;

	i = 0;
	while (buf_size < byte_count) {
		int frag_size = byte_count - buf_size;
		int frag_stride;

		if (i < MLX5E_MAX_RX_FRAGS - 1)
			frag_size = min(frag_size, frag_size_max);

		param->frag_info[i].frag_size = frag_size;

		frag_stride = roundup_pow_of_two(frag_size);
		frag_stride = ALIGN(frag_stride, SMP_CACHE_BYTES); /* for small frag_size */
		param->frag_info[i].frag_stride = frag_stride;

		buf_size += frag_size;
		i++;
	}
	param->num_frags = i;
	/* number of different wqes sharing a page */
	param->wqe_bulk = 1 + (param->num_frags % 2);

}

static inline u8 mlx5e_get_rqwq_log_stride(u8 wq_type, int ndsegs)
{
	int ret = sizeof(struct mlx5_wqe_data_seg) * ndsegs;

	switch (wq_type) {
	case MLX5_WQ_TYPE_LINKED_LIST_STRIDING_RQ:
		ret += sizeof(struct mlx5e_rx_wqe_ll);
		break;
	default: /* MLX5_WQ_TYPE_CYCLIC */
		ret += sizeof(struct mlx5e_rx_wqe_cyc);
	}

	ret = order_base_2(ret);

	return ret;
}

static void mlx5e_build_rq_param(struct mlx5e_priv *priv,
				 struct mlx5e_params *params,
				 struct mlx5e_rq_param *param)
{
	void *rqc = param->rqc;
	void *wq = MLX5_ADDR_OF(rqc, rqc, wq);
	int ndsegs = 1;

	switch (params->rq_wq_type) {
	case MLX5_WQ_TYPE_LINKED_LIST_STRIDING_RQ:
		MLX5_SET(wq, wq, log_wqe_num_of_strides,
			 mlx5e_mpwqe_get_log_num_strides(priv, params) - 9);
		MLX5_SET(wq, wq, log_wqe_stride_size,
			 mlx5e_mpwqe_get_log_stride_size(priv, params) - 6);
		break;
	default: /* MLX5_WQ_TYPE_CYCLIC */
		mlx5e_calc_rq_frags_info(priv, params, param);
		param->wqe_bulk = max_t(u8, param->wqe_bulk, 8);
		param->log_num_frags = order_base_2(param->num_frags);
		ndsegs = param->num_frags;
	}

	MLX5_SET(wq, wq, wq_type,          params->rq_wq_type);
	MLX5_SET(wq, wq, end_padding_mode, MLX5_WQ_END_PAD_MODE_ALIGN);
	MLX5_SET(wq, wq, log_wq_stride, mlx5e_get_rqwq_log_stride(params->rq_wq_type, ndsegs));
	MLX5_SET(wq, wq, log_wq_sz,        params->log_rq_size);
	MLX5_SET(wq, wq, pd,               priv->mdev->mlx5e_res.pdn);
	MLX5_SET(rqc, rqc, counter_set_id, priv->q_counter);
	MLX5_SET(rqc, rqc, vsd,            params->vlan_strip_disable);
	MLX5_SET(rqc, rqc, scatter_fcs,    params->scatter_fcs_en);

	if (MLX5E_GET_PFLAG(params, MLX5E_PFLAG_DROPLESS_RQ))
		MLX5_SET(rqc, rqc, delay_drop_en, 1);

	param->wq.buf_numa_node = dev_to_node(&priv->mdev->pdev->dev);
	param->wq.linear = 1;
}

static void mlx5e_build_drop_rq_param(struct mlx5_core_dev *mdev,
				      struct mlx5e_rq_param *param)
{
	void *rqc = param->rqc;
	void *wq = MLX5_ADDR_OF(rqc, rqc, wq);

	MLX5_SET(wq, wq, wq_type, MLX5_WQ_TYPE_CYCLIC);
	MLX5_SET(wq, wq, log_wq_stride,
		 mlx5e_get_rqwq_log_stride(MLX5_WQ_TYPE_CYCLIC, 1));

	param->wq.buf_numa_node = dev_to_node(&mdev->pdev->dev);
}

static void mlx5e_build_sq_param_common(struct mlx5e_priv *priv,
					struct mlx5e_sq_param *param)
{
	void *sqc = param->sqc;
	void *wq = MLX5_ADDR_OF(sqc, sqc, wq);

	MLX5_SET(wq, wq, log_wq_stride, ilog2(MLX5_SEND_WQE_BB));
	MLX5_SET(wq, wq, pd,            priv->mdev->mlx5e_res.pdn);

	param->wq.buf_numa_node = dev_to_node(&priv->mdev->pdev->dev);
}

static void mlx5e_build_sq_param(struct mlx5e_priv *priv,
				 struct mlx5e_params *params,
				 struct mlx5e_sq_param *param)
{
	void *sqc = param->sqc;
	void *wq = MLX5_ADDR_OF(sqc, sqc, wq);

	mlx5e_build_sq_param_common(priv, param);
	MLX5_SET(wq, wq, log_wq_sz, params->log_sq_size);
	MLX5_SET(sqc, sqc, allow_swp, !!MLX5_IPSEC_DEV(priv->mdev));
}

static void mlx5e_build_common_cq_param(struct mlx5e_priv *priv,
					struct mlx5e_cq_param *param)
{
	void *cqc = param->cqc;

	MLX5_SET(cqc, cqc, uar_page, priv->mdev->priv.uar->index);
}

static void mlx5e_build_rx_cq_param(struct mlx5e_priv *priv,
				    struct mlx5e_params *params,
				    struct mlx5e_cq_param *param)
{
	void *cqc = param->cqc;
	u8 log_cq_size;

	switch (params->rq_wq_type) {
	case MLX5_WQ_TYPE_LINKED_LIST_STRIDING_RQ:
		log_cq_size = params->log_rq_size +
			mlx5e_mpwqe_get_log_num_strides(priv, params);
		break;
	default: /* MLX5_WQ_TYPE_CYCLIC */
		log_cq_size = params->log_rq_size;
	}

	MLX5_SET(cqc, cqc, log_cq_size, log_cq_size);
	if (MLX5E_GET_PFLAG(params, MLX5E_PFLAG_RX_CQE_COMPRESS)) {
		MLX5_SET(cqc, cqc, mini_cqe_res_format, MLX5_CQE_FORMAT_CSUM);
		MLX5_SET(cqc, cqc, cqe_comp_en, 1);
	}

	mlx5e_build_common_cq_param(priv, param);
	param->cq_period_mode = params->rx_cq_moderation.cq_period_mode;
}

static void mlx5e_build_tx_cq_param(struct mlx5e_priv *priv,
				    struct mlx5e_params *params,
				    struct mlx5e_cq_param *param)
{
	void *cqc = param->cqc;

	MLX5_SET(cqc, cqc, log_cq_size, params->log_sq_size);

	mlx5e_build_common_cq_param(priv, param);
	param->cq_period_mode = params->tx_cq_moderation.cq_period_mode;
}

static void mlx5e_build_ico_cq_param(struct mlx5e_priv *priv,
				     u8 log_wq_size,
				     struct mlx5e_cq_param *param)
{
	void *cqc = param->cqc;

	MLX5_SET(cqc, cqc, log_cq_size, log_wq_size);

	mlx5e_build_common_cq_param(priv, param);

	param->cq_period_mode = MLX5_CQ_PERIOD_MODE_START_FROM_EQE;
}

static void mlx5e_build_icosq_param(struct mlx5e_priv *priv,
				    u8 log_wq_size,
				    struct mlx5e_sq_param *param)
{
	void *sqc = param->sqc;
	void *wq = MLX5_ADDR_OF(sqc, sqc, wq);

	mlx5e_build_sq_param_common(priv, param);

	MLX5_SET(wq, wq, log_wq_sz, log_wq_size);
	MLX5_SET(sqc, sqc, reg_umr, MLX5_CAP_ETH(priv->mdev, reg_umr_sq));
}

#ifdef HAVE_NETDEV_BPF
static void mlx5e_build_xdpsq_param(struct mlx5e_priv *priv,
				    struct mlx5e_params *params,
				    struct mlx5e_sq_param *param)
{
	void *sqc = param->sqc;
	void *wq = MLX5_ADDR_OF(sqc, sqc, wq);

	mlx5e_build_sq_param_common(priv, param);
	MLX5_SET(wq, wq, log_wq_sz, params->log_sq_size);
}
#endif

static void mlx5e_build_channel_param(struct mlx5e_priv *priv,
				      struct mlx5e_params *params,
				      struct mlx5e_channel_param *cparam)
{
	u8 icosq_log_wq_sz = MLX5E_PARAMS_MINIMUM_LOG_SQ_SIZE;

	mlx5e_build_rq_param(priv, params, &cparam->rq);
	mlx5e_build_sq_param(priv, params, &cparam->sq);
#ifdef HAVE_NETDEV_BPF
	mlx5e_build_xdpsq_param(priv, params, &cparam->xdp_sq);
#endif
	mlx5e_build_icosq_param(priv, icosq_log_wq_sz, &cparam->icosq);
	mlx5e_build_rx_cq_param(priv, params, &cparam->rx_cq);
	mlx5e_build_tx_cq_param(priv, params, &cparam->tx_cq);
	mlx5e_build_ico_cq_param(priv, icosq_log_wq_sz, &cparam->icosq_cq);
}

#if defined (CONFIG_MLX5_EN_SPECIAL_SQ) && (defined(HAVE_NDO_SET_TX_MAXRATE) || defined (HAVE_NDO_SET_TX_MAXRATE_EXTENDED))
static void mlx5e_rl_cleanup(struct mlx5e_priv *priv)
{
	mlx5e_rl_remove_sysfs(priv);
	hash_init(priv->flow_map_hash);
}

static int mlx5e_rl_init(struct mlx5e_priv *priv,
			 struct mlx5e_params params)
{
	int err;

	err = mlx5e_rl_init_sysfs(priv->netdev, params);
	if (!err) {
		WARN_ON(!hash_empty(priv->flow_map_hash));
		hash_init(priv->flow_map_hash);
	} else {
		mlx5e_rl_cleanup(priv);
		mlx5_core_err(priv->mdev, "failed to init rate limit\n");
	}

	return err;
}
#endif

int mlx5e_open_channels(struct mlx5e_priv *priv,
			struct mlx5e_channels *chs)
{
	struct mlx5e_channel_param *cparam;
	int err = -ENOMEM;
	int i;

	chs->num = chs->params.num_channels;

	chs->c = kcalloc(chs->num, sizeof(struct mlx5e_channel *), GFP_KERNEL);
	cparam = kzalloc(sizeof(struct mlx5e_channel_param), GFP_KERNEL);
	if (!chs->c || !cparam)
		goto err_free;

	mlx5e_build_channel_param(priv, &chs->params, cparam);
	for (i = 0; i < chs->num; i++) {
		err = mlx5e_open_channel(priv, i, &chs->params, cparam, &chs->c[i]);
		if (err)
			goto err_close_channels;
	}

	kfree(cparam);
	return 0;

err_close_channels:
	for (i--; i >= 0; i--)
		mlx5e_close_channel(chs->c[i]);

err_free:
	kfree(chs->c);
	kfree(cparam);
	chs->num = 0;
	return err;
}

static void mlx5e_activate_channels(struct mlx5e_channels *chs)
{
	int i;

	for (i = 0; i < chs->num; i++)
		mlx5e_activate_channel(chs->c[i]);
}

static int mlx5e_wait_channels_min_rx_wqes(struct mlx5e_channels *chs)
{
	int err = 0;
	int i;

	for (i = 0; i < chs->num; i++) {
		err = mlx5e_wait_for_min_rx_wqes(&chs->c[i]->rq);
		if (err)
			break;
	}

	return err;
}

static void mlx5e_deactivate_channels(struct mlx5e_channels *chs)
{
	int i;

	for (i = 0; i < chs->num; i++)
		mlx5e_deactivate_channel(chs->c[i]);
}

void mlx5e_close_channels(struct mlx5e_channels *chs)
{
	int i;

	for (i = 0; i < chs->num; i++)
		mlx5e_close_channel(chs->c[i]);

	kfree(chs->c);
	chs->num = 0;
}

static int
mlx5e_create_rqt(struct mlx5e_priv *priv, int sz, struct mlx5e_rqt *rqt)
{
	struct mlx5_core_dev *mdev = priv->mdev;
	void *rqtc;
	int inlen;
	int err;
	u32 *in;
	int i;

	inlen = MLX5_ST_SZ_BYTES(create_rqt_in) + sizeof(u32) * sz;
	in = kvzalloc(inlen, GFP_KERNEL);
	if (!in)
		return -ENOMEM;

	rqtc = MLX5_ADDR_OF(create_rqt_in, in, rqt_context);

	MLX5_SET(rqtc, rqtc, rqt_actual_size, sz);
	MLX5_SET(rqtc, rqtc, rqt_max_size, sz);

	for (i = 0; i < sz; i++)
		MLX5_SET(rqtc, rqtc, rq_num[i], priv->drop_rq.rqn);

	err = mlx5_core_create_rqt(mdev, in, inlen, &rqt->rqtn);
	if (!err)
		rqt->enabled = true;

	kvfree(in);
	return err;
}

void mlx5e_destroy_rqt(struct mlx5e_priv *priv, struct mlx5e_rqt *rqt)
{
	rqt->enabled = false;
	mlx5_core_destroy_rqt(priv->mdev, rqt->rqtn);
}

int mlx5e_create_indirect_rqt(struct mlx5e_priv *priv)
{
	struct mlx5e_rqt *rqt = &priv->indir_rqt;
	int err;

	err = mlx5e_create_rqt(priv, MLX5E_INDIR_RQT_SIZE, rqt);
	if (err)
		mlx5_core_warn(priv->mdev, "create indirect rqts failed, %d\n", err);
	return err;
}

int mlx5e_create_direct_rqts(struct mlx5e_priv *priv)
{
	struct mlx5e_rqt *rqt;
	int err;
	int ix;

	for (ix = 0; ix < priv->profile->max_nch(priv->mdev); ix++) {
		rqt = &priv->direct_tir[ix].rqt;
		err = mlx5e_create_rqt(priv, 1 /*size */, rqt);
		if (err)
			goto err_destroy_rqts;
	}

	return 0;

err_destroy_rqts:
	mlx5_core_warn(priv->mdev, "create direct rqts failed, %d\n", err);
	for (ix--; ix >= 0; ix--)
		mlx5e_destroy_rqt(priv, &priv->direct_tir[ix].rqt);

	return err;
}

void mlx5e_destroy_direct_rqts(struct mlx5e_priv *priv)
{
	int i;

	for (i = 0; i < priv->profile->max_nch(priv->mdev); i++)
		mlx5e_destroy_rqt(priv, &priv->direct_tir[i].rqt);
}

static int mlx5e_rx_hash_fn(int hfunc)
{
#ifdef HAVE_ETH_SS_RSS_HASH_FUNCS
	return (hfunc == ETH_RSS_HASH_TOP) ?
	       MLX5_RX_HASH_FN_TOEPLITZ :
	       MLX5_RX_HASH_FN_INVERTED_XOR8;
#else
	return MLX5_RX_HASH_FN_INVERTED_XOR8;
#endif
}

static int mlx5e_bits_invert(unsigned long a, int size)
{
	int inv = 0;
	int i;

	for (i = 0; i < size; i++)
		inv |= (test_bit(size - i - 1, &a) ? 1 : 0) << i;

	return inv;
}

static void mlx5e_fill_rqt_rqns(struct mlx5e_priv *priv, int sz,
				struct mlx5e_redirect_rqt_param rrp, void *rqtc)
{
	int i;

	for (i = 0; i < sz; i++) {
		u32 rqn;

		if (rrp.is_rss) {
			int ix = i;

#ifdef HAVE_ETH_SS_RSS_HASH_FUNCS
			if (rrp.rss.hfunc == ETH_RSS_HASH_XOR)
#endif
				ix = mlx5e_bits_invert(i, ilog2(sz));

			ix = priv->channels.params.indirection_rqt[ix];
			rqn = rrp.rss.channels->c[ix]->rq.rqn;
		} else {
			rqn = rrp.rqn;
		}
		MLX5_SET(rqtc, rqtc, rq_num[i], rqn);
	}
}

int mlx5e_redirect_rqt(struct mlx5e_priv *priv, u32 rqtn, int sz,
		       struct mlx5e_redirect_rqt_param rrp)
{
	struct mlx5_core_dev *mdev = priv->mdev;
	void *rqtc;
	int inlen;
	u32 *in;
	int err;

	inlen = MLX5_ST_SZ_BYTES(modify_rqt_in) + sizeof(u32) * sz;
	in = kvzalloc(inlen, GFP_KERNEL);
	if (!in)
		return -ENOMEM;

	rqtc = MLX5_ADDR_OF(modify_rqt_in, in, ctx);

	MLX5_SET(rqtc, rqtc, rqt_actual_size, sz);
	MLX5_SET(modify_rqt_in, in, bitmask.rqn_list, 1);
	mlx5e_fill_rqt_rqns(priv, sz, rrp, rqtc);
	err = mlx5_core_modify_rqt(mdev, rqtn, in, inlen);

	kvfree(in);
	return err;
}

static u32 mlx5e_get_direct_rqn(struct mlx5e_priv *priv, int ix,
				struct mlx5e_redirect_rqt_param rrp)
{
	if (!rrp.is_rss)
		return rrp.rqn;

	if (ix >= rrp.rss.channels->num)
		return priv->drop_rq.rqn;

	return rrp.rss.channels->c[ix]->rq.rqn;
}

static void mlx5e_redirect_rqts(struct mlx5e_priv *priv,
				struct mlx5e_redirect_rqt_param rrp)
{
	u32 rqtn;
	int ix;

	if (priv->indir_rqt.enabled) {
		/* RSS RQ table */
		rqtn = priv->indir_rqt.rqtn;
		mlx5e_redirect_rqt(priv, rqtn, MLX5E_INDIR_RQT_SIZE, rrp);
	}

	for (ix = 0; ix < priv->profile->max_nch(priv->mdev); ix++) {
		struct mlx5e_redirect_rqt_param direct_rrp = {
			.is_rss = false,
			{
				.rqn    = mlx5e_get_direct_rqn(priv, ix, rrp)
			},
		};

		/* Direct RQ Tables */
		if (!priv->direct_tir[ix].rqt.enabled)
			continue;

		rqtn = priv->direct_tir[ix].rqt.rqtn;
		mlx5e_redirect_rqt(priv, rqtn, 1, direct_rrp);
	}
}

static void mlx5e_redirect_rqts_to_channels(struct mlx5e_priv *priv,
					    struct mlx5e_channels *chs)
{
	struct mlx5e_redirect_rqt_param rrp = {
		.is_rss        = true,
		{
			.rss = {
				.channels  = chs,
				.hfunc     = chs->params.rss_hfunc,
			}
		},
	};

	mlx5e_redirect_rqts(priv, rrp);
}

static void mlx5e_redirect_rqts_to_drop(struct mlx5e_priv *priv)
{
	struct mlx5e_redirect_rqt_param drop_rrp = {
		.is_rss = false,
		{
			.rqn = priv->drop_rq.rqn,
		},
	};

	mlx5e_redirect_rqts(priv, drop_rrp);
}

static void mlx5e_build_tir_ctx_lro(struct mlx5e_params *params, void *tirc)
{
#ifdef CONFIG_COMPAT_LRO_ENABLED_IPOIB
	if (!IS_HW_LRO(params))
#else
	if (!params->lro_en)
#endif
		return;

#define ROUGH_MAX_L2_L3_HDR_SZ 256

	MLX5_SET(tirc, tirc, lro_enable_mask,
		 MLX5_TIRC_LRO_ENABLE_MASK_IPV4_LRO |
		 MLX5_TIRC_LRO_ENABLE_MASK_IPV6_LRO);
	MLX5_SET(tirc, tirc, lro_max_ip_payload_size,
		 (params->lro_wqe_sz - ROUGH_MAX_L2_L3_HDR_SZ) >> 8);
	MLX5_SET(tirc, tirc, lro_timeout_period_usecs, params->lro_timeout);
}

void mlx5e_build_indir_tir_ctx_hash(struct mlx5e_params *params,
				    enum mlx5e_traffic_types tt,
				    void *tirc, bool inner)
{
	void *hfso = inner ? MLX5_ADDR_OF(tirc, tirc, rx_hash_field_selector_inner) :
			     MLX5_ADDR_OF(tirc, tirc, rx_hash_field_selector_outer);

#define MLX5_HASH_IP            (MLX5_HASH_FIELD_SEL_SRC_IP   |\
				 MLX5_HASH_FIELD_SEL_DST_IP)

#define MLX5_HASH_IP_L4PORTS    (MLX5_HASH_FIELD_SEL_SRC_IP   |\
				 MLX5_HASH_FIELD_SEL_DST_IP   |\
				 MLX5_HASH_FIELD_SEL_L4_SPORT |\
				 MLX5_HASH_FIELD_SEL_L4_DPORT)

#define MLX5_HASH_IP_IPSEC_SPI  (MLX5_HASH_FIELD_SEL_SRC_IP   |\
				 MLX5_HASH_FIELD_SEL_DST_IP   |\
				 MLX5_HASH_FIELD_SEL_IPSEC_SPI)

	MLX5_SET(tirc, tirc, rx_hash_fn, mlx5e_rx_hash_fn(params->rss_hfunc));
#ifdef HAVE_ETH_SS_RSS_HASH_FUNCS
	if (params->rss_hfunc == ETH_RSS_HASH_TOP) {
		void *rss_key = MLX5_ADDR_OF(tirc, tirc,
					     rx_hash_toeplitz_key);
		size_t len = MLX5_FLD_SZ_BYTES(tirc,
					       rx_hash_toeplitz_key);

		MLX5_SET(tirc, tirc, rx_hash_symmetric, 1);
		memcpy(rss_key, params->toeplitz_hash_key, len);
	}
#endif

	switch (tt) {
	case MLX5E_TT_IPV4_TCP:
		MLX5_SET(rx_hash_field_select, hfso, l3_prot_type,
			 MLX5_L3_PROT_TYPE_IPV4);
		MLX5_SET(rx_hash_field_select, hfso, l4_prot_type,
			 MLX5_L4_PROT_TYPE_TCP);
		MLX5_SET(rx_hash_field_select, hfso, selected_fields,
			 MLX5_HASH_IP_L4PORTS);
		break;

	case MLX5E_TT_IPV6_TCP:
		MLX5_SET(rx_hash_field_select, hfso, l3_prot_type,
			 MLX5_L3_PROT_TYPE_IPV6);
		MLX5_SET(rx_hash_field_select, hfso, l4_prot_type,
			 MLX5_L4_PROT_TYPE_TCP);
		MLX5_SET(rx_hash_field_select, hfso, selected_fields,
			 MLX5_HASH_IP_L4PORTS);
		break;

	case MLX5E_TT_IPV4_UDP:
		MLX5_SET(rx_hash_field_select, hfso, l3_prot_type,
			 MLX5_L3_PROT_TYPE_IPV4);
		MLX5_SET(rx_hash_field_select, hfso, l4_prot_type,
			 MLX5_L4_PROT_TYPE_UDP);
		MLX5_SET(rx_hash_field_select, hfso, selected_fields,
			 MLX5_HASH_IP_L4PORTS);
		break;

	case MLX5E_TT_IPV6_UDP:
		MLX5_SET(rx_hash_field_select, hfso, l3_prot_type,
			 MLX5_L3_PROT_TYPE_IPV6);
		MLX5_SET(rx_hash_field_select, hfso, l4_prot_type,
			 MLX5_L4_PROT_TYPE_UDP);
		MLX5_SET(rx_hash_field_select, hfso, selected_fields,
			 MLX5_HASH_IP_L4PORTS);
		break;

	case MLX5E_TT_IPV4_IPSEC_AH:
		MLX5_SET(rx_hash_field_select, hfso, l3_prot_type,
			 MLX5_L3_PROT_TYPE_IPV4);
		MLX5_SET(rx_hash_field_select, hfso, selected_fields,
			 MLX5_HASH_IP_IPSEC_SPI);
		break;

	case MLX5E_TT_IPV6_IPSEC_AH:
		MLX5_SET(rx_hash_field_select, hfso, l3_prot_type,
			 MLX5_L3_PROT_TYPE_IPV6);
		MLX5_SET(rx_hash_field_select, hfso, selected_fields,
			 MLX5_HASH_IP_IPSEC_SPI);
		break;

	case MLX5E_TT_IPV4_IPSEC_ESP:
		MLX5_SET(rx_hash_field_select, hfso, l3_prot_type,
			 MLX5_L3_PROT_TYPE_IPV4);
		MLX5_SET(rx_hash_field_select, hfso, selected_fields,
			 MLX5_HASH_IP_IPSEC_SPI);
		break;

	case MLX5E_TT_IPV6_IPSEC_ESP:
		MLX5_SET(rx_hash_field_select, hfso, l3_prot_type,
			 MLX5_L3_PROT_TYPE_IPV6);
		MLX5_SET(rx_hash_field_select, hfso, selected_fields,
			 MLX5_HASH_IP_IPSEC_SPI);
		break;

	case MLX5E_TT_IPV4:
		MLX5_SET(rx_hash_field_select, hfso, l3_prot_type,
			 MLX5_L3_PROT_TYPE_IPV4);
		MLX5_SET(rx_hash_field_select, hfso, selected_fields,
			 MLX5_HASH_IP);
		break;

	case MLX5E_TT_IPV6:
		MLX5_SET(rx_hash_field_select, hfso, l3_prot_type,
			 MLX5_L3_PROT_TYPE_IPV6);
		MLX5_SET(rx_hash_field_select, hfso, selected_fields,
			 MLX5_HASH_IP);
		break;
	default:
		WARN_ONCE(true, "%s: bad traffic type!\n", __func__);
	}
}

int mlx5e_modify_tirs_lro(struct mlx5e_priv *priv)
{
	struct mlx5_core_dev *mdev = priv->mdev;
	struct mlx5e_tir *tir;
	void *in;
	void *tirc;
	int inlen;
	int err = 0;

	inlen = MLX5_ST_SZ_BYTES(modify_tir_in);
	in = kvzalloc(inlen, GFP_KERNEL);
	if (!in)
		return -ENOMEM;

	MLX5_SET(modify_tir_in, in, bitmask.lro, 1);
	tirc = MLX5_ADDR_OF(modify_tir_in, in, ctx);

	mlx5e_build_tir_ctx_lro(&priv->channels.params, tirc);

	list_for_each_entry(tir, &mdev->mlx5e_res.td.tirs_list, list) {
		err = mlx5_core_modify_tir(mdev, tir->tirn, in, inlen);
		if (err)
			break;
	}

	kvfree(in);

	return err;
}

static void mlx5e_build_inner_indir_tir_ctx(struct mlx5e_priv *priv,
					    enum mlx5e_traffic_types tt,
					    u32 *tirc)
{
	MLX5_SET(tirc, tirc, transport_domain, priv->mdev->mlx5e_res.td.tdn);

	mlx5e_build_tir_ctx_lro(&priv->channels.params, tirc);

	MLX5_SET(tirc, tirc, disp_type, MLX5_TIRC_DISP_TYPE_INDIRECT);
	MLX5_SET(tirc, tirc, indirect_table, priv->indir_rqt.rqtn);
	MLX5_SET(tirc, tirc, tunneled_offload_en, 0x1);

	mlx5e_build_indir_tir_ctx_hash(&priv->channels.params, tt, tirc, true);
}

static int mlx5e_set_mtu(struct mlx5e_priv *priv, u16 mtu)
{
	struct mlx5_core_dev *mdev = priv->mdev;
	u16 hw_mtu = MLX5E_SW2HW_MTU(priv, mtu);
	int err;

	err = mlx5_set_port_mtu(mdev, hw_mtu, 1);
	if (err)
		return err;

	/* Update vport context MTU */
	mlx5_modify_nic_vport_mtu(mdev, hw_mtu);
	return 0;
}

static void mlx5e_query_mtu(struct mlx5e_priv *priv, u16 *mtu)
{
	struct mlx5_core_dev *mdev = priv->mdev;
	u16 hw_mtu = 0;
	int err;

	err = mlx5_query_nic_vport_mtu(mdev, &hw_mtu);
	if (err || !hw_mtu) /* fallback to port oper mtu */
		mlx5_query_port_oper_mtu(mdev, &hw_mtu, 1);

	*mtu = MLX5E_HW2SW_MTU(priv, hw_mtu);
}

static int mlx5e_set_dev_port_mtu(struct mlx5e_priv *priv)
{
	struct net_device *netdev = priv->netdev;
	u16 mtu;
	int err;

	err = mlx5e_set_mtu(priv, netdev->mtu);
	if (err)
		return err;

	mlx5e_query_mtu(priv, &mtu);
	if (mtu != netdev->mtu)
		netdev_warn(netdev, "%s: VPort MTU %d is different than netdev mtu %d\n",
			    __func__, mtu, netdev->mtu);

	netdev->mtu = mtu;
	return 0;
}

static void mlx5e_netdev_set_tcs(struct mlx5e_priv *priv)
{
#ifdef HAVE_NETDEV_SET_TC_QUEUE
	int nch = priv->channels.params.num_channels;
#endif
	int ntc = priv->channels.params.num_tc;
#ifdef HAVE_NETDEV_SET_TC_QUEUE
	int tc;
#endif

#ifdef HAVE_NETDEV_RESET_TC
	netdev_reset_tc(priv->netdev);
#endif

	if (ntc == 1)
		return;

#ifdef HAVE_NETDEV_SET_NUM_TC
	netdev_set_num_tc(priv->netdev, ntc);
#endif

#ifdef HAVE_NETDEV_SET_TC_QUEUE
	/* Map netdev TCs to offset 0
	 * We have our own UP to TXQ mapping for QoS
	 */
	for (tc = 0; tc < ntc; tc++)
		netdev_set_tc_queue(priv->netdev, tc, nch, 0);
#endif
}

static void mlx5e_build_channels_tx_maps(struct mlx5e_priv *priv)
{
	struct mlx5e_channel *c;
	struct mlx5e_txqsq *sq;
	int i, tc;

	for (i = 0; i < priv->channels.num; i++)
		for (tc = 0; tc < priv->profile->max_tc; tc++)
			priv->channel_tc2txq[i][tc] = i + tc * priv->channels.num;

	for (i = 0; i < priv->channels.num; i++) {
		c = priv->channels.c[i];
		for (tc = 0; tc < c->num_tc; tc++) {
			sq = &c->sq[tc];
			priv->txq2sq[sq->txq_ix] = sq;
		}

#ifdef CONFIG_MLX5_EN_SPECIAL_SQ
		for (tc = 0; tc < c->num_special_sq; tc++) {
			sq = &c->special_sq[tc];
			priv->txq2sq[sq->txq_ix] = sq;
		}
#endif
	}
}

void mlx5e_activate_priv_channels(struct mlx5e_priv *priv)
{
	int num_txqs = priv->channels.num * priv->channels.params.num_tc;
	struct net_device *netdev = priv->netdev;

#ifdef CONFIG_MLX5_EN_SPECIAL_SQ
	num_txqs += priv->channels.params.num_rl_txqs;
#endif

	mlx5e_netdev_set_tcs(priv);
	netif_set_real_num_tx_queues(netdev, num_txqs);
#ifdef HAVE_NET_DEVICE_REAL_NUM_RX_QUEUES
	netif_set_real_num_rx_queues(netdev, priv->channels.num);
#endif

	mlx5e_build_channels_tx_maps(priv);
	mlx5e_activate_channels(&priv->channels);
	netif_tx_start_all_queues(priv->netdev);

	if (MLX5_VPORT_MANAGER(priv->mdev))
		mlx5e_add_sqs_fwd_rules(priv);

	mlx5e_wait_channels_min_rx_wqes(&priv->channels);
	mlx5e_redirect_rqts_to_channels(priv, &priv->channels);
}

void mlx5e_deactivate_priv_channels(struct mlx5e_priv *priv)
{
	mlx5e_redirect_rqts_to_drop(priv);

	if (MLX5_VPORT_MANAGER(priv->mdev))
		mlx5e_remove_sqs_fwd_rules(priv);

	/* FIXME: This is a W/A only for tx timeout watch dog false alarm when
	 * polling for inactive tx queues.
	 */
	netif_tx_stop_all_queues(priv->netdev);
	netif_tx_disable(priv->netdev);
	mlx5e_deactivate_channels(&priv->channels);
}

int mlx5e_switch_priv_channels(struct mlx5e_priv *priv,
			       struct mlx5e_channels *new_chs,
			       mlx5e_fp_hw_modify hw_modify)
{
	int new_num_txqs = new_chs->params.num_channels *
			   new_chs->params.num_tc;
	struct net_device *netdev = priv->netdev;
	int carrier_ok;
	int err;

	carrier_ok = netif_carrier_ok(netdev);
	netif_carrier_off(netdev);

#if defined (CONFIG_MLX5_EN_SPECIAL_SQ) && (defined(HAVE_NDO_SET_TX_MAXRATE) || defined (HAVE_NDO_SET_TX_MAXRATE_EXTENDED))
	mlx5e_rl_cleanup(priv);
	new_num_txqs += new_chs->params.num_rl_txqs;
#endif

	if (new_num_txqs < netdev->real_num_tx_queues) {
#ifdef HAVE_NETIF_SET_REAL_NUM_TX_QUEUES_NOT_VOID
		err = netif_set_real_num_tx_queues(netdev, new_num_txqs);
#else
		if (new_num_txqs < 1 || new_num_txqs > netdev->num_tx_queues) {
			err = -EINVAL;
		} else {
			netif_set_real_num_tx_queues(netdev, new_num_txqs);
			err = 0;
		}
#endif
		if (err) {
			netdev_err(netdev,
				   "real TX num queues set failed. new num txqs = %d, error = %d\n",
				   new_num_txqs, err);
			goto rl_init;
		}
	}

	mlx5e_deactivate_priv_channels(priv);

	err = mlx5e_open_channels(priv, new_chs);
	if (err)
		goto activate_channels;

	mlx5e_close_channels(&priv->channels);

	priv->channels = *new_chs;

	/* New channels are ready to roll, modify HW settings if needed */
	if (hw_modify)
		hw_modify(priv);

	mlx5e_refresh_tirs(priv, false);
activate_channels:
	mlx5e_activate_priv_channels(priv);
rl_init:
#if defined (CONFIG_MLX5_EN_SPECIAL_SQ) && (defined(HAVE_NDO_SET_TX_MAXRATE) || defined (HAVE_NDO_SET_TX_MAXRATE_EXTENDED))
	mlx5e_rl_init(priv, priv->channels.params);
#endif

	/* return carrier back if needed */
	if (carrier_ok)
		netif_carrier_on(netdev);

	return err;
}

void mlx5e_timestamp_init(struct mlx5e_priv *priv)
{
	priv->tstamp.tx_type   = HWTSTAMP_TX_OFF;
	priv->tstamp.rx_filter = HWTSTAMP_FILTER_NONE;
}

int mlx5e_open_locked(struct net_device *netdev)
{
	struct mlx5e_priv *priv = netdev_priv(netdev);
	int err;

	set_bit(MLX5E_STATE_OPENED, &priv->state);

	err = mlx5e_open_channels(priv, &priv->channels);
	if (err)
		goto err_clear_state_opened_flag;

	mlx5e_refresh_tirs(priv, false);
	mlx5e_activate_priv_channels(priv);

#if defined (CONFIG_MLX5_EN_SPECIAL_SQ) && (defined(HAVE_NDO_SET_TX_MAXRATE) || defined (HAVE_NDO_SET_TX_MAXRATE_EXTENDED))
	mlx5e_rl_init(priv, priv->channels.params);
#endif

	mlx5e_create_debugfs(priv);
	if (priv->profile->update_carrier)
		priv->profile->update_carrier(priv);

#ifdef DEV_NETMAP
	netmap_enable_all_rings(netdev); /* NOP if netmap not in use */
#endif

	if (priv->profile->update_stats)
		queue_delayed_work(priv->wq, &priv->update_stats_work, 0);

	return 0;

err_clear_state_opened_flag:
	clear_bit(MLX5E_STATE_OPENED, &priv->state);
	return err;
}

int mlx5e_open(struct net_device *netdev)
{
	struct mlx5e_priv *priv = netdev_priv(netdev);
	int err;

	mutex_lock(&priv->state_lock);
	err = mlx5e_open_locked(netdev);
	if (!err)
		mlx5_set_port_admin_status(priv->mdev, MLX5_PORT_UP);
	mutex_unlock(&priv->state_lock);

	return err;
}

int mlx5e_close_locked(struct net_device *netdev)
{
	struct mlx5e_priv *priv = netdev_priv(netdev);

	/* May already be CLOSED in case a previous configuration operation
	 * (e.g RX/TX queue size change) that involves close&open failed.
	 */
	if (!test_bit(MLX5E_STATE_OPENED, &priv->state))
		return 0;

	clear_bit(MLX5E_STATE_OPENED, &priv->state);

#ifdef DEV_NETMAP
	netmap_disable_all_rings(netdev);
#endif

	if (MLX5E_GET_PFLAG(&priv->channels.params, MLX5E_PFLAG_SNIFFER)) {
		mlx5e_sniffer_stop(priv);
		MLX5E_SET_PFLAG(&priv->channels.params, MLX5E_PFLAG_SNIFFER, 0);
	}

	netif_carrier_off(priv->netdev);
	mlx5e_destroy_debugfs(priv);
#if defined (CONFIG_MLX5_EN_SPECIAL_SQ) && (defined(HAVE_NDO_SET_TX_MAXRATE) || defined (HAVE_NDO_SET_TX_MAXRATE_EXTENDED))
	mlx5e_rl_cleanup(priv);
#endif
	mlx5e_deactivate_priv_channels(priv);
	mlx5e_close_channels(&priv->channels);

	return 0;
}

int mlx5e_close(struct net_device *netdev)
{
	struct mlx5e_priv *priv = netdev_priv(netdev);
	int err;

	if (!netif_device_present(netdev))
		return -ENODEV;

	mutex_lock(&priv->state_lock);
	mlx5_set_port_admin_status(priv->mdev, MLX5_PORT_DOWN);
	err = mlx5e_close_locked(netdev);
	mutex_unlock(&priv->state_lock);

	return err;
}

static int mlx5e_alloc_drop_rq(struct mlx5_core_dev *mdev,
			       struct mlx5e_rq *rq,
			       struct mlx5e_rq_param *param)
{
	void *rqc = param->rqc;
	void *rqc_wq = MLX5_ADDR_OF(rqc, rqc, wq);
	int err;

	param->wq.db_numa_node = param->wq.buf_numa_node;

	err = mlx5_wq_cyc_create(mdev, &param->wq, rqc_wq, &rq->wqe.wq,
				 &rq->wq_ctrl);
	if (err)
		return err;

	rq->mdev = mdev;

	return 0;
}

static int mlx5e_alloc_drop_cq(struct mlx5_core_dev *mdev,
			       struct mlx5e_cq *cq,
			       struct mlx5e_cq_param *param)
{
	param->wq.buf_numa_node = dev_to_node(&mdev->pdev->dev);
	param->wq.db_numa_node  = dev_to_node(&mdev->pdev->dev);

	return mlx5e_alloc_cq_common(mdev, param, cq);
}

static int mlx5e_open_drop_rq(struct mlx5_core_dev *mdev,
			      struct mlx5e_rq *drop_rq)
{
	struct mlx5e_cq_param cq_param = {};
	struct mlx5e_rq_param rq_param = {};
	struct mlx5e_cq *cq = &drop_rq->cq;
	int err;

	mlx5e_build_drop_rq_param(mdev, &rq_param);

	err = mlx5e_alloc_drop_cq(mdev, cq, &cq_param);
	if (err)
		return err;

	err = mlx5e_create_cq(cq, &cq_param);
	if (err)
		goto err_free_cq;

	err = mlx5e_alloc_drop_rq(mdev, drop_rq, &rq_param);
	if (err)
		goto err_destroy_cq;

	err = mlx5e_create_rq(drop_rq, &rq_param);
	if (err)
		goto err_free_rq;

	return 0;

err_free_rq:
	mlx5e_free_rq(drop_rq);

err_destroy_cq:
	mlx5e_destroy_cq(cq);

err_free_cq:
	mlx5e_free_cq(cq);

	return err;
}

static void mlx5e_close_drop_rq(struct mlx5e_rq *drop_rq)
{
	mlx5e_destroy_rq(drop_rq);
	mlx5e_free_rq(drop_rq);
	mlx5e_destroy_cq(&drop_rq->cq);
	mlx5e_free_cq(&drop_rq->cq);
}

int mlx5e_create_tis(struct mlx5_core_dev *mdev, int tc,
		     u32 underlay_qpn, u32 *tisn)
{
	u32 in[MLX5_ST_SZ_DW(create_tis_in)] = {0};
	void *tisc = MLX5_ADDR_OF(create_tis_in, in, ctx);

	MLX5_SET(tisc, tisc, prio, tc << 1);
	MLX5_SET(tisc, tisc, underlay_qpn, underlay_qpn);
	MLX5_SET(tisc, tisc, transport_domain, mdev->mlx5e_res.td.tdn);

	if (mlx5_lag_is_lacp_owner(mdev))
		MLX5_SET(tisc, tisc, strict_lag_tx_port_affinity, 1);

	return mlx5_core_create_tis(mdev, in, sizeof(in), tisn);
}

void mlx5e_destroy_tis(struct mlx5_core_dev *mdev, u32 tisn)
{
	mlx5_core_destroy_tis(mdev, tisn);
}

int mlx5e_create_tises(struct mlx5e_priv *priv)
{
	int err;
	int tc;

	for (tc = 0; tc < priv->profile->max_tc; tc++) {
		err = mlx5e_create_tis(priv->mdev, tc, 0, &priv->tisn[tc]);
		if (err)
			goto err_close_tises;
	}

	return 0;

err_close_tises:
	for (tc--; tc >= 0; tc--)
		mlx5e_destroy_tis(priv->mdev, priv->tisn[tc]);

	return err;
}

void mlx5e_cleanup_nic_tx(struct mlx5e_priv *priv)
{
	int tc;

	for (tc = 0; tc < priv->profile->max_tc; tc++)
		mlx5e_destroy_tis(priv->mdev, priv->tisn[tc]);
}

static void mlx5e_build_indir_tir_ctx(struct mlx5e_priv *priv,
				      enum mlx5e_traffic_types tt,
				      u32 *tirc)
{
	MLX5_SET(tirc, tirc, transport_domain, priv->mdev->mlx5e_res.td.tdn);

	mlx5e_build_tir_ctx_lro(&priv->channels.params, tirc);

	MLX5_SET(tirc, tirc, disp_type, MLX5_TIRC_DISP_TYPE_INDIRECT);
	MLX5_SET(tirc, tirc, indirect_table, priv->indir_rqt.rqtn);
	mlx5e_build_indir_tir_ctx_hash(&priv->channels.params, tt, tirc, false);
}

void mlx5e_build_direct_tir_ctx(struct mlx5e_priv *priv, u32 rqtn, u32 *tirc)
{
	MLX5_SET(tirc, tirc, transport_domain, priv->mdev->mlx5e_res.td.tdn);

	mlx5e_build_tir_ctx_lro(&priv->channels.params, tirc);

	MLX5_SET(tirc, tirc, disp_type, MLX5_TIRC_DISP_TYPE_INDIRECT);
	MLX5_SET(tirc, tirc, indirect_table, rqtn);
	MLX5_SET(tirc, tirc, rx_hash_fn, MLX5_RX_HASH_FN_INVERTED_XOR8);
}

int mlx5e_create_indirect_tirs(struct mlx5e_priv *priv)
{
	struct mlx5e_tir *tir;
	void *tirc;
	int inlen;
	int i = 0;
	int err;
	u32 *in;
	int tt;

	inlen = MLX5_ST_SZ_BYTES(create_tir_in);
	in = kvzalloc(inlen, GFP_KERNEL);
	if (!in)
		return -ENOMEM;

	for (tt = 0; tt < MLX5E_NUM_INDIR_TIRS; tt++) {
		memset(in, 0, inlen);
		tir = &priv->indir_tir[tt];
		tirc = MLX5_ADDR_OF(create_tir_in, in, ctx);
		mlx5e_build_indir_tir_ctx(priv, tt, tirc);
		err = mlx5e_create_tir(priv->mdev, tir, in, inlen);
		if (err) {
			mlx5_core_warn(priv->mdev, "create indirect tirs failed, %d\n", err);
			goto err_destroy_inner_tirs;
		}
	}

	if (!mlx5e_tunnel_inner_ft_supported(priv->mdev))
		goto out;

	for (i = 0; i < MLX5E_NUM_INDIR_TIRS; i++) {
		memset(in, 0, inlen);
		tir = &priv->inner_indir_tir[i];
		tirc = MLX5_ADDR_OF(create_tir_in, in, ctx);
		mlx5e_build_inner_indir_tir_ctx(priv, i, tirc);
		err = mlx5e_create_tir(priv->mdev, tir, in, inlen);
		if (err) {
			mlx5_core_warn(priv->mdev, "create inner indirect tirs failed, %d\n", err);
			goto err_destroy_inner_tirs;
		}
	}

out:
	kvfree(in);

	return 0;

err_destroy_inner_tirs:
	for (i--; i >= 0; i--)
		mlx5e_destroy_tir(priv->mdev, &priv->inner_indir_tir[i]);
	for (tt--; tt >= 0; tt--)
		mlx5e_destroy_tir(priv->mdev, &priv->indir_tir[tt]);

	kvfree(in);

	return err;
}

int mlx5e_create_direct_tirs(struct mlx5e_priv *priv)
{
	int nch = priv->profile->max_nch(priv->mdev);
	struct mlx5e_tir *tir;
	void *tirc;
	int inlen;
	int err;
	u32 *in;
	int ix;

	inlen = MLX5_ST_SZ_BYTES(create_tir_in);
	in = kvzalloc(inlen, GFP_KERNEL);
	if (!in)
		return -ENOMEM;

	for (ix = 0; ix < nch; ix++) {
		memset(in, 0, inlen);
		tir = &priv->direct_tir[ix];
		tirc = MLX5_ADDR_OF(create_tir_in, in, ctx);
		mlx5e_build_direct_tir_ctx(priv, priv->direct_tir[ix].rqt.rqtn, tirc);
		err = mlx5e_create_tir(priv->mdev, tir, in, inlen);
		if (err)
			goto err_destroy_ch_tirs;
	}

	kvfree(in);

	return 0;

err_destroy_ch_tirs:
	mlx5_core_warn(priv->mdev, "create direct tirs failed, %d\n", err);
	for (ix--; ix >= 0; ix--)
		mlx5e_destroy_tir(priv->mdev, &priv->direct_tir[ix]);

	kvfree(in);

	return err;
}

void mlx5e_destroy_indirect_tirs(struct mlx5e_priv *priv)
{
	int i;

	for (i = 0; i < MLX5E_NUM_INDIR_TIRS; i++)
		mlx5e_destroy_tir(priv->mdev, &priv->indir_tir[i]);

	if (!mlx5e_tunnel_inner_ft_supported(priv->mdev))
		return;

	for (i = 0; i < MLX5E_NUM_INDIR_TIRS; i++)
		mlx5e_destroy_tir(priv->mdev, &priv->inner_indir_tir[i]);
}

void mlx5e_destroy_direct_tirs(struct mlx5e_priv *priv)
{
	int nch = priv->profile->max_nch(priv->mdev);
	int i;

	for (i = 0; i < nch; i++)
		mlx5e_destroy_tir(priv->mdev, &priv->direct_tir[i]);
}

#ifdef HAVE_NETIF_F_RXFCS
static int mlx5e_modify_channels_scatter_fcs(struct mlx5e_channels *chs, bool enable)
{
	int err = 0;
	int i;

	for (i = 0; i < chs->num; i++) {
		err = mlx5e_modify_rq_scatter_fcs(&chs->c[i]->rq, enable);
		if (err)
			return err;
	}

	return 0;
}
#endif

#if !defined(LEGACY_ETHTOOL_OPS) && !defined(HAVE_GET_SET_FLAGS)
static
#endif
int mlx5e_modify_channels_vsd(struct mlx5e_channels *chs, bool vsd)
{
	int err = 0;
	int i;

	for (i = 0; i < chs->num; i++) {
		err = mlx5e_modify_rq_vsd(&chs->c[i]->rq, vsd);
		if (err)
			return err;
	}

	return 0;
}

#if defined(HAVE_NDO_SETUP_TC_TAKES_TC_SETUP_TYPE) || defined(HAVE_NDO_SETUP_TC_RH_EXTENDED)
int mlx5e_setup_tc_mqprio(struct net_device *netdev,
			  struct tc_mqprio_qopt *mqprio)
#else
int mlx5e_setup_tc(struct net_device *netdev, u8 tc)
#endif
{
	struct mlx5e_priv *priv = netdev_priv(netdev);
	struct mlx5e_channels new_channels = {};
#if defined(HAVE_NDO_SETUP_TC_TAKES_TC_SETUP_TYPE) || defined(HAVE_NDO_SETUP_TC_RH_EXTENDED)
	u8 tc = mqprio->num_tc;
#endif
	int err = 0;

#if defined(HAVE_NDO_SETUP_TC_TAKES_TC_SETUP_TYPE) || defined(HAVE_NDO_SETUP_TC_RH_EXTENDED)
	mqprio->hw = TC_MQPRIO_HW_OFFLOAD_TCS;
#endif

	if (tc && tc != MLX5E_MAX_NUM_TC)
		return -EINVAL;

	mutex_lock(&priv->state_lock);

	new_channels.params = priv->channels.params;
	new_channels.params.num_tc = tc ? tc : 1;

	if (!test_bit(MLX5E_STATE_OPENED, &priv->state)) {
		priv->channels.params = new_channels.params;
		goto out;
	}

	err = mlx5e_switch_priv_channels(priv, &new_channels, NULL);
out:
	mutex_unlock(&priv->state_lock);
	return err;
}

#if defined(HAVE_NDO_SETUP_TC_TAKES_TC_SETUP_TYPE) || defined(HAVE_NDO_SETUP_TC_RH_EXTENDED)
#ifdef CONFIG_MLX5_ESWITCH
#ifdef HAVE_TC_BLOCK_OFFLOAD
static int mlx5e_setup_tc_cls_flower(struct mlx5e_priv *priv,
#else
static int mlx5e_setup_tc_cls_flower(struct net_device *dev,
#endif
				     struct tc_cls_flower_offload *cls_flower)
{
#ifdef HAVE_TC_BLOCK_OFFLOAD
	if (cls_flower->common.chain_index)
#else
	struct mlx5e_priv *priv = netdev_priv(dev);

	if (!is_classid_clsact_ingress(cls_flower->common.classid) ||
	    cls_flower->common.chain_index)
#endif
		return -EOPNOTSUPP;

	switch (cls_flower->command) {
	case TC_CLSFLOWER_REPLACE:
		return mlx5e_configure_flower(priv, cls_flower);
	case TC_CLSFLOWER_DESTROY:
		return mlx5e_delete_flower(priv, cls_flower);
	case TC_CLSFLOWER_STATS:
		return mlx5e_stats_flower(priv, cls_flower);
	default:
		return -EOPNOTSUPP;
	}
}

#ifdef HAVE_TC_BLOCK_OFFLOAD
int mlx5e_setup_tc_block_cb(enum tc_setup_type type, void *type_data,
			    void *cb_priv)
{
	struct mlx5e_priv *priv = cb_priv;

	if (!tc_can_offload(priv->netdev))
		return -EOPNOTSUPP;

	switch (type) {
	case TC_SETUP_CLSFLOWER:
		return mlx5e_setup_tc_cls_flower(priv, type_data);
	default:
		return -EOPNOTSUPP;
	}
}

static int mlx5e_setup_tc_block(struct net_device *dev,
				struct tc_block_offload *f)
{
	struct mlx5e_priv *priv = netdev_priv(dev);

	if (f->binder_type != TCF_BLOCK_BINDER_TYPE_CLSACT_INGRESS)
		return -EOPNOTSUPP;

	switch (f->command) {
	case TC_BLOCK_BIND:
		return tcf_block_cb_register(f->block, mlx5e_setup_tc_block_cb,
					     priv, priv);
	case TC_BLOCK_UNBIND:
		tcf_block_cb_unregister(f->block, mlx5e_setup_tc_block_cb,
					priv);
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}
#endif
#endif

#ifdef HAVE_TC_SETUP_CB_EGDEV_REGISTER
int mlx5e_setup_tc(struct net_device *dev, enum tc_setup_type type,
		   void *type_data)
#else
static int mlx5e_setup_tc(struct net_device *dev, enum tc_setup_type type,
			  void *type_data)
#endif
{
	switch (type) {
#ifdef CONFIG_MLX5_ESWITCH
#ifdef HAVE_TC_BLOCK_OFFLOAD
	case TC_SETUP_BLOCK:
		return mlx5e_setup_tc_block(dev, type_data);
#else
	case TC_SETUP_CLSFLOWER:
		return mlx5e_setup_tc_cls_flower(dev, type_data);
#endif
#endif
	case TC_SETUP_QDISC_MQPRIO:
		return mlx5e_setup_tc_mqprio(dev, type_data);
	default:
		return -EOPNOTSUPP;
	}
}
#else
#if defined(HAVE_NDO_SETUP_TC_4_PARAMS) || defined(HAVE_NDO_SETUP_TC_TAKES_CHAIN_INDEX)
static int mlx5e_ndo_setup_tc(struct net_device *dev, u32 handle,
#ifdef HAVE_NDO_SETUP_TC_TAKES_CHAIN_INDEX
			      u32 chain_index, __be16 proto,
#else
			      __be16 proto,
#endif
			      struct tc_to_netdev *tc)
{
#ifdef HAVE_TC_FLOWER_OFFLOAD
	struct mlx5e_priv *priv = netdev_priv(dev);

	if (TC_H_MAJ(handle) != TC_H_MAJ(TC_H_INGRESS))
		goto mqprio;

#ifdef HAVE_NDO_SETUP_TC_TAKES_CHAIN_INDEX
	if (chain_index)
		return -EOPNOTSUPP;
#endif

	switch (tc->type) {
	case TC_SETUP_CLSFLOWER:
		switch (tc->cls_flower->command) {
		case TC_CLSFLOWER_REPLACE:
			return mlx5e_configure_flower(priv, tc->cls_flower);
		case TC_CLSFLOWER_DESTROY:
			return mlx5e_delete_flower(priv, tc->cls_flower);
#ifdef HAVE_TC_CLSFLOWER_STATS
		case TC_CLSFLOWER_STATS:
			return mlx5e_stats_flower(priv, tc->cls_flower);
#endif
		}
	default:
		return -EOPNOTSUPP;
	}

mqprio:
#endif /* HAVE_TC_FLOWER_OFFLOAD */
	if (tc->type != TC_SETUP_MQPRIO)
		return -EINVAL;

#ifdef HAVE_TC_TO_NETDEV_TC
	return mlx5e_setup_tc(dev, tc->tc);
#else
	tc->mqprio->hw = TC_MQPRIO_HW_OFFLOAD_TCS;

	return mlx5e_setup_tc(dev, tc->mqprio->num_tc);
#endif
}
#endif
#endif

static
#ifdef HAVE_NDO_GET_STATS64_RET_VOID
void mlx5e_get_stats(struct net_device *dev, struct rtnl_link_stats64 *stats)
#elif defined(HAVE_NDO_GET_STATS64)
struct rtnl_link_stats64 * mlx5e_get_stats(struct net_device *dev, struct rtnl_link_stats64 *stats)
#else
struct net_device_stats * mlx5e_get_stats(struct net_device *dev)
#endif
{
	struct mlx5e_priv *priv = netdev_priv(dev);
	struct mlx5e_sw_stats *sstats = &priv->stats.sw;
	struct mlx5e_vport_stats *vstats = &priv->stats.vport;
	struct mlx5e_pport_stats *pstats = &priv->stats.pport;

#if !defined(HAVE_NDO_GET_STATS64) && !defined(HAVE_NDO_GET_STATS64_RET_VOID)
	struct net_device_stats *stats = &priv->netdev_stats;
#endif

	if (mlx5e_is_uplink_rep(priv)) {
		stats->rx_packets = PPORT_802_3_GET(pstats, a_frames_received_ok);
		stats->rx_bytes   = PPORT_802_3_GET(pstats, a_octets_received_ok);
		stats->tx_packets = PPORT_802_3_GET(pstats, a_frames_transmitted_ok);
		stats->tx_bytes   = PPORT_802_3_GET(pstats, a_octets_transmitted_ok);
	} else {
		stats->rx_packets = sstats->rx_packets;
		stats->rx_bytes   = sstats->rx_bytes;
		stats->tx_packets = sstats->tx_packets;
		stats->tx_bytes   = sstats->tx_bytes;
		stats->tx_dropped = sstats->tx_queue_dropped;
	}

	stats->rx_dropped = priv->stats.qcnt.rx_out_of_buffer;

	stats->rx_length_errors =
		PPORT_802_3_GET(pstats, a_in_range_length_errors) +
		PPORT_802_3_GET(pstats, a_out_of_range_length_field) +
		PPORT_802_3_GET(pstats, a_frame_too_long_errors);
	stats->rx_crc_errors =
		PPORT_802_3_GET(pstats, a_frame_check_sequence_errors);
	stats->rx_frame_errors = PPORT_802_3_GET(pstats, a_alignment_errors);
	stats->tx_aborted_errors = PPORT_2863_GET(pstats, if_out_discards);
	stats->rx_errors = stats->rx_length_errors + stats->rx_crc_errors +
			   stats->rx_frame_errors;
	stats->tx_errors = stats->tx_aborted_errors + stats->tx_carrier_errors;

	/* vport multicast also counts packets that are dropped due to steering
	 * or rx out of buffer
	 */
	stats->multicast =
		VPORT_COUNTER_GET(vstats, received_eth_multicast.packets);

#ifndef HAVE_NDO_GET_STATS64_RET_VOID
	return stats;
#endif
}

static void mlx5e_set_rx_mode(struct net_device *dev)
{
	struct mlx5e_priv *priv = netdev_priv(dev);

	queue_work(priv->wq, &priv->set_rx_mode_work);
}

static int mlx5e_set_mac(struct net_device *netdev, void *addr)
{
	struct mlx5e_priv *priv = netdev_priv(netdev);
	struct sockaddr *saddr = addr;

	if (!is_valid_ether_addr(saddr->sa_data))
		return -EADDRNOTAVAIL;

	netif_addr_lock_bh(netdev);
	ether_addr_copy(netdev->dev_addr, saddr->sa_data);
	netif_addr_unlock_bh(netdev);

	queue_work(priv->wq, &priv->set_rx_mode_work);

	return 0;
}

#define MLX5E_SET_FEATURE(features, feature, enable)	\
	do {						\
		if (enable)				\
			*features |= feature;		\
		else					\
			*features &= ~feature;		\
	} while (0)

typedef int (*mlx5e_feature_handler)(struct net_device *netdev, bool enable);

#if (defined(HAVE_NDO_SET_FEATURES) || defined(HAVE_NET_DEVICE_OPS_EXT))
static int set_feature_lro(struct net_device *netdev, bool enable)
#else
int mlx5e_update_lro(struct net_device *netdev, bool enable)
#endif
{
	struct mlx5e_priv *priv = netdev_priv(netdev);
	struct mlx5e_channels new_channels = {};
	int err = 0;
	bool reset;

#if (defined(HAVE_NDO_SET_FEATURES) || defined(HAVE_NET_DEVICE_OPS_EXT))
	mutex_lock(&priv->state_lock);
#endif

	reset = test_bit(MLX5E_STATE_OPENED, &priv->state);

	new_channels.params = priv->channels.params;
	new_channels.params.lro_en = enable;

#ifdef CONFIG_COMPAT_LRO_ENABLED_IPOIB
	if (IS_HW_LRO(&new_channels.params) &&
#else
	if (new_channels.params.lro_en &&
#endif
	    new_channels.params.rq_wq_type == MLX5_WQ_TYPE_CYCLIC) {
		netdev_warn(netdev, "can't set HW LRO with legacy RQ\n");
		err = -EINVAL;
		goto out;
	}

	if (!reset) {
		priv->channels.params = new_channels.params;
		err = mlx5e_modify_tirs_lro(priv);
		goto out;
	}

	err = mlx5e_switch_priv_channels(priv, &new_channels,
					 mlx5e_modify_tirs_lro);
out:
#if (defined(HAVE_NDO_SET_FEATURES) || defined(HAVE_NET_DEVICE_OPS_EXT))
	mutex_unlock(&priv->state_lock);
#endif
	return err;
}

#if (defined(HAVE_NDO_SET_FEATURES) || defined(HAVE_NET_DEVICE_OPS_EXT))
static int set_feature_cvlan_filter(struct net_device *netdev, bool enable)
{
	struct mlx5e_priv *priv = netdev_priv(netdev);

	if (enable)
		mlx5e_enable_cvlan_filter(priv);
	else
		mlx5e_disable_cvlan_filter(priv);

	return 0;
}
#endif /* (defined(HAVE_NDO_SET_FEATURES) || defined(HAVE_NET_DEVICE_OPS_EXT)) */

#ifdef HAVE_TC_FLOWER_OFFLOAD
static int set_feature_tc_num_filters(struct net_device *netdev, bool enable)
{
	struct mlx5e_priv *priv = netdev_priv(netdev);

	if (!enable && mlx5e_tc_num_filters(priv)) {
		netdev_err(netdev,
			   "Active offloaded tc filters, can't turn hw_tc_offload off\n");
		return -EINVAL;
	}

	return 0;
}
#endif

#ifdef HAVE_NETIF_F_RXALL
static int set_feature_rx_all(struct net_device *netdev, bool enable)
{
	struct mlx5e_priv *priv = netdev_priv(netdev);
	struct mlx5_core_dev *mdev = priv->mdev;

	return mlx5_set_port_fcs(mdev, !enable);
}
#endif

#ifdef HAVE_NETIF_F_RXFCS
static int set_feature_rx_fcs(struct net_device *netdev, bool enable)
{
	struct mlx5e_priv *priv = netdev_priv(netdev);
	int err;

	mutex_lock(&priv->state_lock);

	priv->channels.params.scatter_fcs_en = enable;
	err = mlx5e_modify_channels_scatter_fcs(&priv->channels, enable);
	if (err)
		priv->channels.params.scatter_fcs_en = !enable;

	mutex_unlock(&priv->state_lock);

	return err;
}
#endif

#if (defined(HAVE_NDO_SET_FEATURES) || defined(HAVE_NET_DEVICE_OPS_EXT))
static int set_feature_rx_vlan(struct net_device *netdev, bool enable)
{
	struct mlx5e_priv *priv = netdev_priv(netdev);
	int err = 0;

	mutex_lock(&priv->state_lock);

	priv->channels.params.vlan_strip_disable = !enable;
	if (!test_bit(MLX5E_STATE_OPENED, &priv->state))
		goto unlock;

	err = mlx5e_modify_channels_vsd(&priv->channels, !enable);
	if (err)
		priv->channels.params.vlan_strip_disable = enable;

unlock:
	mutex_unlock(&priv->state_lock);

	return err;
}
#endif

#ifdef CONFIG_RFS_ACCEL
static int set_feature_arfs(struct net_device *netdev, bool enable)
{
	struct mlx5e_priv *priv = netdev_priv(netdev);
	int err;

	if (enable)
		err = mlx5e_arfs_enable(priv);
	else
		err = mlx5e_arfs_disable(priv);

	return err;
}
#endif

#if (defined(HAVE_NDO_SET_FEATURES) || defined(HAVE_NET_DEVICE_OPS_EXT))
static int mlx5e_handle_feature(struct net_device *netdev,
				netdev_features_t *features,
#ifndef HAVE_NET_DEVICE_OPS_EXT
				netdev_features_t wanted_features,
				netdev_features_t feature,
#else
				u32 wanted_features,
				u32 feature,
#endif
				mlx5e_feature_handler feature_handler)
{
#ifndef HAVE_NET_DEVICE_OPS_EXT
	netdev_features_t changes = wanted_features ^ netdev->features;
#else
	u32 changes = wanted_features ^ netdev->features;
#endif
	bool enable = !!(wanted_features & feature);
	int err;

	if (!(changes & feature))
		return 0;

	err = feature_handler(netdev, enable);
	if (err) {
#ifndef HAVE_NET_DEVICE_OPS_EXT
		netdev_err(netdev, "%s feature %pNF failed, err %d\n",
			   enable ? "Enable" : "Disable", &feature, err);
#else
		netdev_err(netdev, "%s feature 0x%ux failed err %d\n",
			   enable ? "Enable" : "Disable", feature, err);
#endif
		return err;
	}

	MLX5E_SET_FEATURE(features, feature, enable);
	return 0;
}
#endif

#if (defined(HAVE_NDO_SET_FEATURES) || defined(HAVE_NET_DEVICE_OPS_EXT))
static int mlx5e_set_features(struct net_device *netdev,
#ifdef HAVE_NET_DEVICE_OPS_EXT
			      u32 features)
#else
			      netdev_features_t features)
#endif
{
	netdev_features_t oper_features = netdev->features;
	int err;

	err  = mlx5e_handle_feature(netdev, &oper_features, features,
				    NETIF_F_LRO, set_feature_lro);
	err |= mlx5e_handle_feature(netdev, &oper_features, features,
				    NETIF_F_HW_VLAN_CTAG_FILTER,
				    set_feature_cvlan_filter);
#ifdef HAVE_TC_FLOWER_OFFLOAD
	err |= mlx5e_handle_feature(netdev, &oper_features, features,
				    NETIF_F_HW_TC, set_feature_tc_num_filters);
#endif
#ifdef HAVE_NETIF_F_RXALL
	err |= mlx5e_handle_feature(netdev, &oper_features, features,
				    NETIF_F_RXALL, set_feature_rx_all);
#endif
#ifdef HAVE_NETIF_F_RXFCS
	err |= mlx5e_handle_feature(netdev, &oper_features, features,
				    NETIF_F_RXFCS, set_feature_rx_fcs);
#endif
	err |= mlx5e_handle_feature(netdev, &oper_features, features,
				    NETIF_F_HW_VLAN_CTAG_RX, set_feature_rx_vlan);
#ifdef CONFIG_RFS_ACCEL
	err |= mlx5e_handle_feature(netdev, &oper_features, features,
				    NETIF_F_NTUPLE, set_feature_arfs);
#endif

	if (err) {
		netdev->features = oper_features;
		return -EINVAL;
	}

	return 0;
}
#endif

#ifdef HAVE_NETIF_F_HW_VLAN_STAG_RX
static netdev_features_t mlx5e_fix_features(struct net_device *netdev,
					    netdev_features_t features)
{
	struct mlx5e_priv *priv = netdev_priv(netdev);
	struct mlx5e_params *params;

	mutex_lock(&priv->state_lock);
	params = &priv->channels.params;
	if (!bitmap_empty(priv->fs.vlan.active_svlans, VLAN_N_VID)) {
		/* HW strips the outer C-tag header, this is a problem
		 * for S-tag traffic.
		 */
		features &= ~NETIF_F_HW_VLAN_CTAG_RX;
		if (!params->vlan_strip_disable)
			netdev_warn(netdev, "Dropping C-tag vlan stripping offload due to S-tag vlan\n");
	}
	if (params->rq_wq_type == MLX5_WQ_TYPE_CYCLIC &&
#ifdef CONFIG_COMPAT_LRO_ENABLED_IPOIB
	    IS_HW_LRO(params)) {
#else
	    true) {
#endif
		features &= ~NETIF_F_LRO;
		if (params->lro_en)
			netdev_warn(netdev, "Disabling HW LRO, not supported in legacy RQ\n");
	}

	mutex_unlock(&priv->state_lock);

	return features;
}
#endif

#define MXL5_HW_MIN_MTU 64
#define MXL5E_MIN_MTU (MXL5_HW_MIN_MTU + ETH_FCS_LEN)

static int mlx5e_change_mtu(struct net_device *netdev, int new_mtu)
{
	struct mlx5e_priv *priv = netdev_priv(netdev);
	struct mlx5_core_dev *mdev = priv->mdev;
	struct mlx5e_channels new_channels = {};
	u16 max_mtu;
	u16 min_mtu;
	int curr_mtu;
	int err = 0;
	bool reset;

	mlx5_query_port_max_mtu(mdev, &max_mtu, 1);

	max_mtu = MLX5E_HW2SW_MTU(priv, max_mtu);
	min_mtu = MLX5E_HW2SW_MTU(priv, MXL5E_MIN_MTU);

	if (new_mtu > max_mtu || new_mtu < min_mtu) {
		netdev_err(netdev,
			   "%s: Bad MTU (%d), valid range is: [%d..%d]\n",
			   __func__, new_mtu, min_mtu, max_mtu);
		return -EINVAL;
	}

	mutex_lock(&priv->state_lock);

#ifdef CONFIG_COMPAT_LRO_ENABLED_IPOIB
	reset = !IS_HW_LRO(&priv->channels.params) &&
#else
	reset = !priv->channels.params.lro_en &&
#endif
		(priv->channels.params.rq_wq_type !=
		 MLX5_WQ_TYPE_LINKED_LIST_STRIDING_RQ);

	reset = reset && test_bit(MLX5E_STATE_OPENED, &priv->state);

	curr_mtu    = netdev->mtu;
	netdev->mtu = new_mtu;

	if (!reset) {
		mlx5e_set_dev_port_mtu(priv);
		goto out;
	}

	new_channels.params = priv->channels.params;

	err = mlx5e_switch_priv_channels(priv, &new_channels, mlx5e_set_dev_port_mtu);
	if (err)
		netdev->mtu = curr_mtu;

out:
	mutex_unlock(&priv->state_lock);
	return err;
}

#ifdef HAVE_SIOCGHWTSTAMP
int mlx5e_hwstamp_set(struct mlx5e_priv *priv, struct ifreq *ifr)
{
#else
int mlx5e_hwstamp_ioctl(struct net_device *dev, struct ifreq *ifr)
{
       struct mlx5e_priv *priv = netdev_priv(dev);
#endif
	struct hwtstamp_config config;
	int err;

	if (!MLX5_CAP_GEN(priv->mdev, device_frequency_khz))
		return -EOPNOTSUPP;

	if (copy_from_user(&config, ifr->ifr_data, sizeof(config)))
		return -EFAULT;

	/* TX HW timestamp */
	switch (config.tx_type) {
	case HWTSTAMP_TX_OFF:
	case HWTSTAMP_TX_ON:
		break;
	default:
		return -ERANGE;
	}

	mutex_lock(&priv->state_lock);
	/* RX HW timestamp */
	switch (config.rx_filter) {
	case HWTSTAMP_FILTER_NONE:
		/* Reset CQE compression to Admin default */
		mlx5e_modify_rx_cqe_compression_locked(priv, priv->channels.params.rx_cqe_compress_def);
		break;
	case HWTSTAMP_FILTER_ALL:
	case HWTSTAMP_FILTER_SOME:
	case HWTSTAMP_FILTER_PTP_V1_L4_EVENT:
	case HWTSTAMP_FILTER_PTP_V1_L4_SYNC:
	case HWTSTAMP_FILTER_PTP_V1_L4_DELAY_REQ:
	case HWTSTAMP_FILTER_PTP_V2_L4_EVENT:
	case HWTSTAMP_FILTER_PTP_V2_L4_SYNC:
	case HWTSTAMP_FILTER_PTP_V2_L4_DELAY_REQ:
	case HWTSTAMP_FILTER_PTP_V2_L2_EVENT:
	case HWTSTAMP_FILTER_PTP_V2_L2_SYNC:
	case HWTSTAMP_FILTER_PTP_V2_L2_DELAY_REQ:
	case HWTSTAMP_FILTER_PTP_V2_EVENT:
	case HWTSTAMP_FILTER_PTP_V2_SYNC:
	case HWTSTAMP_FILTER_PTP_V2_DELAY_REQ:
	case HWTSTAMP_FILTER_NTP_ALL:
		/* Disable CQE compression */
		netdev_warn(priv->netdev, "Disabling cqe compression");
		err = mlx5e_modify_rx_cqe_compression_locked(priv, false);
		if (err) {
			netdev_err(priv->netdev, "Failed disabling cqe compression err=%d\n", err);
			mutex_unlock(&priv->state_lock);
			return err;
		}
		config.rx_filter = HWTSTAMP_FILTER_ALL;
		break;
	default:
		mutex_unlock(&priv->state_lock);
		return -ERANGE;
	}

	memcpy(&priv->tstamp, &config, sizeof(config));
	mutex_unlock(&priv->state_lock);

	return copy_to_user(ifr->ifr_data, &config,
			    sizeof(config)) ? -EFAULT : 0;
}

#ifdef HAVE_SIOCGHWTSTAMP
int mlx5e_hwstamp_get(struct mlx5e_priv *priv, struct ifreq *ifr)
{
	struct hwtstamp_config *cfg = &priv->tstamp;

	if (!MLX5_CAP_GEN(priv->mdev, device_frequency_khz))
		return -EOPNOTSUPP;

	return copy_to_user(ifr->ifr_data, cfg, sizeof(*cfg)) ? -EFAULT : 0;
}
#endif

static int mlx5e_ioctl(struct net_device *dev, struct ifreq *ifr, int cmd)
{
	struct mlx5e_priv *priv = netdev_priv(dev);

	switch (cmd) {
	case SIOCSHWTSTAMP:
#ifdef HAVE_SIOCGHWTSTAMP
		return mlx5e_hwstamp_set(priv, ifr);
	case SIOCGHWTSTAMP:
		return mlx5e_hwstamp_get(priv, ifr);
#else
		return mlx5e_hwstamp_ioctl(priv->netdev, ifr);
#endif
	default:
		return -EOPNOTSUPP;
	}
}

#if defined(HAVE_VLAN_GRO_RECEIVE) || defined(HAVE_VLAN_HWACCEL_RX)
void mlx5e_vlan_register(struct net_device *netdev, struct vlan_group *grp)
{
        struct mlx5e_priv *priv = netdev_priv(netdev);
        priv->channels.params.vlan_grp = grp;
}
#endif

#ifdef HAVE_NDO_SET_VF_MAC
#ifdef CONFIG_MLX5_ESWITCH
static int mlx5e_set_vf_mac(struct net_device *dev, int vf, u8 *mac)
{
	struct mlx5e_priv *priv = netdev_priv(dev);
	struct mlx5_core_dev *mdev = priv->mdev;

	return mlx5_eswitch_set_vport_mac(mdev->priv.eswitch, vf + 1, mac);
}
#endif
#endif /* HAVE_NDO_SET_VF_MAC */

#if defined(HAVE_NDO_SET_VF_VLAN) || defined(HAVE_NDO_SET_VF_VLAN_EXTENDED)
#ifdef HAVE_VF_VLAN_PROTO
static int mlx5e_set_vf_vlan(struct net_device *dev, int vf, u16 vlan, u8 qos,
			     __be16 vlan_proto)
#else
static int mlx5e_set_vf_vlan(struct net_device *dev, int vf, u16 vlan, u8 qos)
#endif
{
	struct mlx5e_priv *priv = netdev_priv(dev);
	struct mlx5_core_dev *mdev = priv->mdev;
#ifndef HAVE_VF_VLAN_PROTO
	__be16 vlan_proto = htons(ETH_P_8021Q);
#endif

	return mlx5_eswitch_set_vport_vlan(mdev->priv.eswitch, vf + 1,
					   vlan, qos, vlan_proto);
}
#endif /* HAVE_NDO_SET_VF_VLAN */

#ifdef HAVE_NETDEV_OPS_NDO_SET_VF_TRUNK_RANGE
static int mlx5e_add_vf_vlan_trunk_range(struct net_device *dev, int vf,
					 u16 start_vid, u16 end_vid,
					 __be16 vlan_proto)
{
	struct mlx5e_priv *priv = netdev_priv(dev);
	struct mlx5_core_dev *mdev = priv->mdev;

	if (vlan_proto != htons(ETH_P_8021Q))
		return -EPROTONOSUPPORT;

	return mlx5_eswitch_add_vport_trunk_range(mdev->priv.eswitch, vf + 1,
						  start_vid, end_vid);
}

static int mlx5e_del_vf_vlan_trunk_range(struct net_device *dev, int vf,
					 u16 start_vid, u16 end_vid,
					 __be16 vlan_proto)
{
	struct mlx5e_priv *priv = netdev_priv(dev);
	struct mlx5_core_dev *mdev = priv->mdev;

	if (vlan_proto != htons(ETH_P_8021Q))
		return -EPROTONOSUPPORT;

	return mlx5_eswitch_del_vport_trunk_range(mdev->priv.eswitch, vf + 1,
						  start_vid, end_vid);
}
#endif

#if defined(HAVE_VF_INFO_SPOOFCHK) || defined(HAVE_NETDEV_OPS_EXT_NDO_SET_VF_SPOOFCHK)
static int mlx5e_set_vf_spoofchk(struct net_device *dev, int vf, bool setting)
{
	struct mlx5e_priv *priv = netdev_priv(dev);
	struct mlx5_core_dev *mdev = priv->mdev;

	return mlx5_eswitch_set_vport_spoofchk(mdev->priv.eswitch, vf + 1, setting);
}
#endif

#if defined(HAVE_NETDEV_OPS_NDO_SET_VF_TRUST) || defined(HAVE_NETDEV_OPS_NDO_SET_VF_TRUST_EXTENDED)
static int mlx5e_set_vf_trust(struct net_device *dev, int vf, bool setting)
{
	struct mlx5e_priv *priv = netdev_priv(dev);
	struct mlx5_core_dev *mdev = priv->mdev;

	return mlx5_eswitch_set_vport_trust(mdev->priv.eswitch, vf + 1, setting);
}
#endif

#ifdef HAVE_NDO_SET_VF_MAC
#ifdef HAVE_VF_TX_RATE
static int mlx5e_set_vf_rate(struct net_device *dev, int vf, int max_tx_rate)
#else
static int mlx5e_set_vf_rate(struct net_device *dev, int vf, int min_tx_rate,
			     int max_tx_rate)
#endif
{
	struct mlx5e_priv *priv = netdev_priv(dev);
	struct mlx5_core_dev *mdev = priv->mdev;
#ifdef HAVE_VF_TX_RATE
	struct mlx5_eswitch *esw = mdev->priv.eswitch;
	int min_tx_rate;
	int vport = vf + 1;

	if (!esw || !MLX5_CAP_GEN(esw->dev, vport_group_manager) ||
	    MLX5_CAP_GEN(esw->dev, port_type) != MLX5_CAP_PORT_TYPE_ETH)
		return -EPERM;
	if (vport < 0 || vport >= esw->total_vports)
		return -EINVAL;

	mutex_lock(&esw->state_lock);
	min_tx_rate = esw->vports[vport].info.min_rate;
	mutex_unlock(&esw->state_lock);
#endif

	return mlx5_eswitch_set_vport_rate(mdev->priv.eswitch, vf + 1,
					   max_tx_rate, min_tx_rate);
}
#endif

#ifdef HAVE_LINKSTATE
static int mlx5_vport_link2ifla(u8 esw_link)
{
	switch (esw_link) {
	case MLX5_ESW_VPORT_ADMIN_STATE_DOWN:
		return IFLA_VF_LINK_STATE_DISABLE;
	case MLX5_ESW_VPORT_ADMIN_STATE_UP:
		return IFLA_VF_LINK_STATE_ENABLE;
	}
	return IFLA_VF_LINK_STATE_AUTO;
}

static int mlx5_ifla_link2vport(u8 ifla_link)
{
	switch (ifla_link) {
	case IFLA_VF_LINK_STATE_DISABLE:
		return MLX5_ESW_VPORT_ADMIN_STATE_DOWN;
	case IFLA_VF_LINK_STATE_ENABLE:
		return MLX5_ESW_VPORT_ADMIN_STATE_UP;
	}
	return MLX5_ESW_VPORT_ADMIN_STATE_AUTO;
}

#endif
#if defined(HAVE_NETDEV_OPS_NDO_SET_VF_LINK_STATE) || defined(HAVE_NETDEV_OPS_EXT_NDO_SET_VF_LINK_STATE)
static int mlx5e_set_vf_link_state(struct net_device *dev, int vf,
				   int link_state)
{
	struct mlx5e_priv *priv = netdev_priv(dev);
	struct mlx5_core_dev *mdev = priv->mdev;

	return mlx5_eswitch_set_vport_state(mdev->priv.eswitch, vf + 1,
					    mlx5_ifla_link2vport(link_state));
}
#endif

#ifdef HAVE_NDO_SET_VF_MAC
static int mlx5e_get_vf_config(struct net_device *dev,
			       int vf, struct ifla_vf_info *ivi)
{
	struct mlx5e_priv *priv = netdev_priv(dev);
	struct mlx5_core_dev *mdev = priv->mdev;
	int err;

	err = mlx5_eswitch_get_vport_config(mdev->priv.eswitch, vf + 1, ivi);
	if (err)
		return err;
#ifdef HAVE_LINKSTATE
	ivi->linkstate = mlx5_vport_link2ifla(ivi->linkstate);
#endif
	return 0;
}
#endif

#ifdef HAVE_NDO_GET_VF_STATS
static int mlx5e_get_vf_stats(struct net_device *dev,
			      int vf, struct ifla_vf_stats *vf_stats)
{
	struct mlx5e_priv *priv = netdev_priv(dev);
	struct mlx5_core_dev *mdev = priv->mdev;

	return mlx5_eswitch_get_vport_stats(mdev->priv.eswitch, vf + 1,
					    vf_stats);
}
#endif

#ifdef HAVE_KERNEL_WITH_VXLAN_SUPPORT_ON
#if defined(HAVE_NDO_UDP_TUNNEL_ADD) || defined(HAVE_NDO_UDP_TUNNEL_ADD_EXTENDED)
static void mlx5e_add_vxlan_port(struct net_device *netdev,
				 struct udp_tunnel_info *ti)
{
	struct mlx5e_priv *priv = netdev_priv(netdev);

	if (ti->type != UDP_TUNNEL_TYPE_VXLAN)
		return;

	if (!mlx5e_vxlan_allowed(priv->mdev))
		return;

	mlx5e_vxlan_queue_work(priv, ti->sa_family, be16_to_cpu(ti->port), 1);
}

static void mlx5e_del_vxlan_port(struct net_device *netdev,
				 struct udp_tunnel_info *ti)
{
	struct mlx5e_priv *priv = netdev_priv(netdev);

	if (ti->type != UDP_TUNNEL_TYPE_VXLAN)
		return;

	if (!mlx5e_vxlan_allowed(priv->mdev))
		return;

	mlx5e_vxlan_queue_work(priv, ti->sa_family, be16_to_cpu(ti->port), 0);
}
#elif defined(HAVE_NDO_ADD_VXLAN_PORT)
static void mlx5e_add_vxlan_port(struct net_device *netdev,
				 sa_family_t sa_family, __be16 port)
{
	struct mlx5e_priv *priv = netdev_priv(netdev);

	if (!mlx5e_vxlan_allowed(priv->mdev))
		return;

	mlx5e_vxlan_queue_work(priv, sa_family, be16_to_cpu(port), 1);
}

static void mlx5e_del_vxlan_port(struct net_device *netdev,
				 sa_family_t sa_family, __be16 port)
{
	struct mlx5e_priv *priv = netdev_priv(netdev);

	if (!mlx5e_vxlan_allowed(priv->mdev))
		return;

	mlx5e_vxlan_queue_work(priv, sa_family, be16_to_cpu(port), 0);
}
#endif
#endif /* HAVE_KERNEL_WITH_VXLAN_SUPPORT_ON */

#ifdef HAVE_NETDEV_FEATURES_T
static netdev_features_t mlx5e_tunnel_features_check(struct mlx5e_priv *priv,
						     struct sk_buff *skb,
						     netdev_features_t features)
{
	unsigned int offset = 0;
#ifdef HAVE_KERNEL_WITH_VXLAN_SUPPORT_ON
	struct udphdr *udph;
#endif
	u8 proto;
#ifdef HAVE_KERNEL_WITH_VXLAN_SUPPORT_ON
	u16 port;
#endif

	switch (vlan_get_protocol(skb)) {
	case htons(ETH_P_IP):
		proto = ip_hdr(skb)->protocol;
		break;
	case htons(ETH_P_IPV6):
		proto = ipv6_find_hdr(skb, &offset, -1, NULL, NULL);
		break;
	default:
		goto out;
	}

	switch (proto) {
	case IPPROTO_GRE:
		return features;
#ifdef HAVE_KERNEL_WITH_VXLAN_SUPPORT_ON
	case IPPROTO_UDP:
		udph = udp_hdr(skb);
		port = be16_to_cpu(udph->dest);

		/* Verify if UDP port is being offloaded by HW */
		if (mlx5e_vxlan_lookup_port(priv, port))
			return features;
#endif
	}

out:
	/* Disable CSUM and GSO if the udp dport is not offloaded by HW */
	return features & ~(NETIF_F_CSUM_MASK | NETIF_F_GSO_MASK);
}

static netdev_features_t mlx5e_features_check(struct sk_buff *skb,
					      struct net_device *netdev,
					      netdev_features_t features)
{
	struct mlx5e_priv *priv = netdev_priv(netdev);

#ifdef HAVE_VLAN_FEATURES_CHECK
	features = vlan_features_check(skb, features);
#endif
#ifdef HAVE_KERNEL_WITH_VXLAN_SUPPORT_ON
#ifdef HAVE_VXLAN_FEATURES_CHECK
	features = vxlan_features_check(skb, features);
#endif
#endif /* HAVE_KERNEL_WITH_VXLAN_SUPPORT_ON */

#ifdef CONFIG_MLX5_EN_IPSEC
	if (mlx5e_ipsec_feature_check(skb, netdev, features))
		return features;
#endif

	/* Validate if the tunneled packet is being offloaded by HW */
	if (skb->encapsulation &&
	    (features & NETIF_F_CSUM_MASK || features & NETIF_F_GSO_MASK))
		return mlx5e_tunnel_features_check(priv, skb, features);

	return features;
}

#elif defined(HAVE_KERNEL_WITH_VXLAN_SUPPORT_ON) && defined(HAVE_VXLAN_GSO_CHECK)
static bool mlx5e_gso_check(struct sk_buff *skb, struct net_device *netdev)
{
	struct mlx5e_priv *priv = netdev_priv(netdev);
	struct udphdr *udph;
	u16 port;

	if (!vxlan_gso_check(skb))
		return false;

	if (!skb->encapsulation)
		return true;

	udph = udp_hdr(skb);
	port = be16_to_cpu(udph->dest);

	if (!mlx5e_vxlan_lookup_port(priv, port)) {
		skb->ip_summed = CHECKSUM_NONE;
		return false;
	}

	return true;
}
#endif

static bool mlx5e_tx_timeout_eq_recover(struct mlx5e_priv *priv,
					struct mlx5e_txqsq *sq)
{
	struct mlx5_core_dev *mdev = priv->mdev;
	int irqn_not_used, eqn;
	struct mlx5_eq *eq;
	u32 eqe_count;

	if (mlx5_vector2eqn(mdev, sq->cq.mcq.vector, &eqn, &irqn_not_used))
		return false;

	eq = mlx5_eqn2eq(mdev, eqn);
	if (IS_ERR(eq))
		return false;

	netdev_err(priv->netdev, "EQ 0x%x: Cons = 0x%x, irqn = 0x%x\n",
		   eqn, eq->cons_index, eq->irqn);

	eqe_count = mlx5_eq_poll_irq_disabled(eq);
	if (!eqe_count)
		return false;

	netdev_err(priv->netdev, "Recover %d eqes on EQ 0x%x\n",
		   eqe_count, eq->eqn);
	sq->channel->stats.eq_rearm++;
	return true;
}

void mlx5e_do_tx_timeout(struct mlx5e_priv *priv)
{
	bool reopen_channels = false;
	int i, num_sqs;

	num_sqs = priv->channels.num * priv->channels.params.num_tc;
#ifdef CONFIG_MLX5_EN_SPECIAL_SQ
	num_sqs += priv->channels.params.num_rl_txqs;
#endif
	netdev_err(priv->netdev, "TX timeout detected\n");
 
#if (defined(HAVE_NETIF_XMIT_STOPPED) || defined(HAVE_NETIF_TX_QUEUE_STOPPED)) && defined (HAVE_NETDEV_GET_TX_QUEUE)
	for (i = 0; i < num_sqs; i++) {
		struct netdev_queue *dev_queue =
			netdev_get_tx_queue(priv->netdev, i);
		struct mlx5e_txqsq *sq = priv->txq2sq[i];

#if defined(HAVE_NETIF_XMIT_STOPPED)
		if (!netif_xmit_stopped(dev_queue))
#else
		if (!netif_tx_queue_stopped(dev_queue))
#endif
			continue;
		netdev_err(priv->netdev, "TX timeout on queue: %d, SQ: 0x%x, CQ: 0x%x, SQ Cons: 0x%x SQ Prod: 0x%x, usecs since last trans: %u\n",
			   i, sq->sqn, sq->cq.mcq.cqn, sq->cc, sq->pc,
			   jiffies_to_usecs(jiffies - dev_queue->trans_start));

		/* If we recover a lost interrupt, most likely TX timeout will
		 * be resolved, skip reopening channels
		 */
		if (!mlx5e_tx_timeout_eq_recover(priv, sq)) {
			clear_bit(MLX5E_SQ_STATE_ENABLED, &sq->state);
			reopen_channels = true;
		}
	}
#else
	reopen_channels = true;
#endif

	if (reopen_channels && test_bit(MLX5E_STATE_OPENED, &priv->state))
		schedule_work(&priv->tx_timeout_work);
}

static void mlx5e_tx_timeout(struct net_device *dev)
{
	struct mlx5e_priv *priv = netdev_priv(dev);

	return mlx5e_do_tx_timeout(priv);
}

#ifdef HAVE_NETDEV_BPF
static int mlx5e_xdp_set(struct net_device *netdev, struct bpf_prog *prog)
{
	struct mlx5e_priv *priv = netdev_priv(netdev);
	struct bpf_prog *old_prog;
	int err = 0;
	bool reset, was_opened;
	int i;

	mutex_lock(&priv->state_lock);

	if ((netdev->features & NETIF_F_LRO) && prog) {
		netdev_warn(netdev, "can't set XDP while LRO is on, disable LRO first\n");
		err = -EINVAL;
		goto unlock;
	}

#ifdef CONFIG_MLX5_EN_IPSEC
	if ((netdev->features & NETIF_F_HW_ESP) && prog) {
		netdev_warn(netdev, "can't set XDP with IPSec offload\n");
		err = -EINVAL;
		goto unlock;
	}
#endif

	was_opened = test_bit(MLX5E_STATE_OPENED, &priv->state);
	/* no need for full reset when exchanging programs */
	reset = (!priv->channels.params.xdp_prog || !prog);

	if (was_opened && reset)
		mlx5e_close_locked(netdev);
	if (was_opened && !reset) {
		/* num_channels is invariant here, so we can take the
		 * batched reference right upfront.
		 */
		prog = bpf_prog_add(prog, priv->channels.num);
		if (IS_ERR(prog)) {
			err = PTR_ERR(prog);
			goto unlock;
		}
	}

	/* exchange programs, extra prog reference we got from caller
	 * as long as we don't fail from this point onwards.
	 */
	old_prog = xchg(&priv->channels.params.xdp_prog, prog);
	if (old_prog)
		bpf_prog_put(old_prog);

	if (reset) /* change RQ type according to priv->xdp_prog */
		mlx5e_set_rq_params(priv, &priv->channels.params);

	if (was_opened && reset)
		mlx5e_open_locked(netdev);

	if (!test_bit(MLX5E_STATE_OPENED, &priv->state) || reset)
		goto unlock;

	/* exchanging programs w/o reset, we update ref counts on behalf
	 * of the channels RQs here.
	 */
	for (i = 0; i < priv->channels.num; i++) {
		struct mlx5e_channel *c = priv->channels.c[i];

		clear_bit(MLX5E_RQ_STATE_ENABLED, &c->rq.state);
		napi_synchronize(&c->napi);
		/* prevent mlx5e_poll_rx_cq from accessing rq->xdp_prog */

		old_prog = xchg(&c->rq.xdp_prog, prog);

		set_bit(MLX5E_RQ_STATE_ENABLED, &c->rq.state);
		/* napi_schedule in case we have missed anything */
#ifndef HAVE_NAPI_COMPLETE_DONE_RET_VALUE
		set_bit(MLX5E_CHANNEL_NAPI_SCHED, &c->flags);
#endif
		napi_schedule(&c->napi);

		if (old_prog)
			bpf_prog_put(old_prog);
	}

unlock:
	mutex_unlock(&priv->state_lock);
	return err;
}

#ifdef HAVE_BPF_PROG_AUX_FEILD_ID
static u32 mlx5e_xdp_query(struct net_device *dev)
{
	struct mlx5e_priv *priv = netdev_priv(dev);
	const struct bpf_prog *xdp_prog;
	u32 prog_id = 0;

	mutex_lock(&priv->state_lock);
	xdp_prog = priv->channels.params.xdp_prog;
	if (xdp_prog)
		prog_id = xdp_prog->aux->id;
	mutex_unlock(&priv->state_lock);

	return prog_id;
}
#else /* HAVE_BPF_PROG_AUX_FEILD_ID */
static bool mlx5e_xdp_attached(struct net_device *dev)
{
	struct mlx5e_priv *priv = netdev_priv(dev);

	return !!priv->channels.params.xdp_prog;
}
#endif /* HAVE_BPF_PROG_AUX_FEILD_ID */

static int mlx5e_xdp(struct net_device *dev, struct netdev_bpf *xdp)
{
	switch (xdp->command) {
	case XDP_SETUP_PROG:
		return mlx5e_xdp_set(dev, xdp->prog);
	case XDP_QUERY_PROG:
#ifdef HAVE_BPF_PROG_AUX_FEILD_ID
		xdp->prog_id = mlx5e_xdp_query(dev);
		xdp->prog_attached = !!xdp->prog_id;
#else
		xdp->prog_attached = mlx5e_xdp_attached(dev);
#endif
		return 0;
	default:
		return -EINVAL;
	}
}
#endif

#ifdef CONFIG_NET_POLL_CONTROLLER
/* Fake "interrupt" called by netpoll (eg netconsole) to send skbs without
 * reenabling interrupts.
 */
static void mlx5e_netpoll(struct net_device *dev)
{
	struct mlx5e_priv *priv = netdev_priv(dev);
	struct mlx5e_channels *chs = &priv->channels;

	int i;

	for (i = 0; i < chs->num; i++)
		napi_schedule(&chs->c[i]->napi);
}
#endif

static const struct net_device_ops mlx5e_netdev_ops = {
	.ndo_open                = mlx5e_open,
	.ndo_stop                = mlx5e_close,
	.ndo_start_xmit          = mlx5e_xmit,
#ifdef HAVE_NDO_SETUP_TC_RH_EXTENDED
	.extended.ndo_setup_tc_rh = mlx5e_setup_tc,
#else
#ifdef HAVE_NDO_SETUP_TC
#ifdef HAVE_NDO_SETUP_TC_TAKES_TC_SETUP_TYPE
	.ndo_setup_tc            = mlx5e_setup_tc,
#else
#if defined(HAVE_NDO_SETUP_TC_4_PARAMS) || defined(HAVE_NDO_SETUP_TC_TAKES_CHAIN_INDEX)
	.ndo_setup_tc            = mlx5e_ndo_setup_tc,
#else
	.ndo_setup_tc            = mlx5e_setup_tc,
#endif
#endif
#endif
#endif
	.ndo_select_queue        = mlx5e_select_queue,
#if defined(HAVE_NDO_GET_STATS64) || defined(HAVE_NDO_GET_STATS64_RET_VOID)
	.ndo_get_stats64         = mlx5e_get_stats,
#else
	.ndo_get_stats           = mlx5e_get_stats,
#endif
	.ndo_set_rx_mode         = mlx5e_set_rx_mode,
	.ndo_set_mac_address     = mlx5e_set_mac,
	.ndo_vlan_rx_add_vid     = mlx5e_vlan_rx_add_vid,
	.ndo_vlan_rx_kill_vid    = mlx5e_vlan_rx_kill_vid,
#if defined(HAVE_VLAN_GRO_RECEIVE) || defined(HAVE_VLAN_HWACCEL_RX)
	.ndo_vlan_rx_register    = mlx5e_vlan_register,
#endif
#if (defined(HAVE_NDO_SET_FEATURES) && !defined(HAVE_NET_DEVICE_OPS_EXT))
	.ndo_set_features        = mlx5e_set_features,
#endif
#ifdef HAVE_NETIF_F_HW_VLAN_STAG_RX
	.ndo_fix_features        = mlx5e_fix_features,
#endif
#ifdef HAVE_NDO_CHANGE_MTU_EXTENDED
	.extended.ndo_change_mtu = mlx5e_change_mtu,
#else
	.ndo_change_mtu          = mlx5e_change_mtu,
#endif
	.ndo_do_ioctl            = mlx5e_ioctl,
#ifdef HAVE_NDO_SET_TX_MAXRATE
	.ndo_set_tx_maxrate      = mlx5e_set_tx_maxrate,
#elif defined(HAVE_NDO_SET_TX_MAXRATE_EXTENDED)
	.extended.ndo_set_tx_maxrate      = mlx5e_set_tx_maxrate,
#endif

#ifdef HAVE_KERNEL_WITH_VXLAN_SUPPORT_ON
#ifdef HAVE_NDO_UDP_TUNNEL_ADD
	.ndo_udp_tunnel_add      = mlx5e_add_vxlan_port,
	.ndo_udp_tunnel_del      = mlx5e_del_vxlan_port,
#elif defined(HAVE_NDO_UDP_TUNNEL_ADD_EXTENDED)
	.extended.ndo_udp_tunnel_add      = mlx5e_add_vxlan_port,
	.extended.ndo_udp_tunnel_del      = mlx5e_del_vxlan_port,
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
#ifdef HAVE_NDO_RX_FLOW_STEER
#ifdef CONFIG_RFS_ACCEL
	.ndo_rx_flow_steer	 = mlx5e_rx_flow_steer,
#endif
#endif
	.ndo_tx_timeout          = mlx5e_tx_timeout,
#ifdef HAVE_NDO_XDP_EXTENDED
	.extended.ndo_xdp        = mlx5e_xdp,
#elif defined(HAVE_NETDEV_BPF)
	.ndo_bpf		 = mlx5e_xdp,
#endif

#ifdef CONFIG_NET_POLL_CONTROLLER
	.ndo_poll_controller     = mlx5e_netpoll,
#endif
#ifdef HAVE_NET_DEVICE_OPS_EXTENDED
	.ndo_size = sizeof(struct net_device_ops),
#endif
#ifdef CONFIG_MLX5_ESWITCH
	/* SRIOV E-Switch NDOs */
#ifdef HAVE_NDO_SET_VF_MAC
	.ndo_set_vf_mac          = mlx5e_set_vf_mac,
#endif
#if defined(HAVE_NDO_SET_VF_VLAN)
	.ndo_set_vf_vlan         = mlx5e_set_vf_vlan,
#elif defined(HAVE_NDO_SET_VF_VLAN_EXTENDED)
	.extended.ndo_set_vf_vlan  = mlx5e_set_vf_vlan,
#endif

	/* these ndo's are not upstream yet */
#ifdef HAVE_NETDEV_OPS_NDO_SET_VF_TRUNK_RANGE
	.ndo_add_vf_vlan_trunk_range = mlx5e_add_vf_vlan_trunk_range,
	.ndo_del_vf_vlan_trunk_range = mlx5e_del_vf_vlan_trunk_range,
#endif

#if (defined(HAVE_NETDEV_OPS_NDO_SET_VF_SPOOFCHK) && !defined(HAVE_NET_DEVICE_OPS_EXT))
	.ndo_set_vf_spoofchk     = mlx5e_set_vf_spoofchk,
#endif
#ifdef HAVE_NETDEV_OPS_NDO_SET_VF_TRUST
	.ndo_set_vf_trust        = mlx5e_set_vf_trust,
#elif defined(HAVE_NETDEV_OPS_NDO_SET_VF_TRUST_EXTENDED)
	.extended.ndo_set_vf_trust        = mlx5e_set_vf_trust,
#endif
#ifdef HAVE_NDO_SET_VF_MAC
#ifndef HAVE_VF_TX_RATE
	.ndo_set_vf_rate         = mlx5e_set_vf_rate,
#else
	.ndo_set_vf_tx_rate      = mlx5e_set_vf_rate,
#endif
#endif
#ifdef HAVE_NDO_SET_VF_MAC
	.ndo_get_vf_config       = mlx5e_get_vf_config,
#endif
#if (defined(HAVE_NETDEV_OPS_NDO_SET_VF_LINK_STATE) && !defined(HAVE_NET_DEVICE_OPS_EXT))
	.ndo_set_vf_link_state   = mlx5e_set_vf_link_state,
#endif
#ifdef HAVE_NDO_GET_VF_STATS
	.ndo_get_vf_stats        = mlx5e_get_vf_stats,
#endif
#ifdef NDO_HAS_OFFLOAD_STATS_GETS_NET_DEVICE
	.ndo_has_offload_stats	 = mlx5e_has_offload_stats,
#elif defined(HAVE_NDO_HAS_OFFLOAD_STATS_EXTENDED)
	.extended.ndo_has_offload_stats   = mlx5e_has_offload_stats,
#endif
#ifdef HAVE_NDO_GET_OFFLOAD_STATS
	.ndo_get_offload_stats	 = mlx5e_get_offload_stats,
#elif defined(HAVE_NDO_GET_OFFLOAD_STATS_EXTENDED)
	.extended.ndo_get_offload_stats   = mlx5e_get_offload_stats,
#endif
#endif /* CONFIG_MLX5_ESWITCH */
};

#ifdef HAVE_NET_DEVICE_OPS_EXT
static const struct net_device_ops_ext mlx5e_netdev_ops_ext= {
	.size             = sizeof(struct net_device_ops_ext),
	.ndo_set_features = mlx5e_set_features,
#ifdef HAVE_NETDEV_OPS_EXT_NDO_SET_VF_SPOOFCHK
	.ndo_set_vf_spoofchk    = mlx5e_set_vf_spoofchk,
#endif
#ifdef HAVE_NETDEV_OPS_EXT_NDO_SET_VF_LINK_STATE
	.ndo_set_vf_link_state  = mlx5e_set_vf_link_state,
#endif
};
#endif /* HAVE_NET_DEVICE_OPS_EXT */

static int mlx5e_check_required_hca_cap(struct mlx5_core_dev *mdev)
{
	if (MLX5_CAP_GEN(mdev, port_type) != MLX5_CAP_PORT_TYPE_ETH)
		return -EOPNOTSUPP;
	if (!MLX5_CAP_GEN(mdev, eth_net_offloads) ||
	    !MLX5_CAP_GEN(mdev, nic_flow_table) ||
	    !MLX5_CAP_ETH(mdev, csum_cap) ||
	    !MLX5_CAP_ETH(mdev, max_lso_cap) ||
	    !MLX5_CAP_ETH(mdev, vlan_cap) ||
	    !MLX5_CAP_ETH(mdev, rss_ind_tbl_cap) ||
	    MLX5_CAP_FLOWTABLE(mdev,
			       flow_table_properties_nic_receive.max_ft_level)
			       < 3) {
		mlx5_core_warn(mdev,
			       "Not creating net device, some required device capabilities are missing\n");
		return -EOPNOTSUPP;
	}
	if (!MLX5_CAP_ETH(mdev, self_lb_en_modifiable))
		mlx5_core_warn(mdev, "Self loop back prevention is not supported\n");
	if (!MLX5_CAP_GEN(mdev, cq_moderation))
		mlx5_core_warn(mdev, "CQ moderation is not supported\n");

	return 0;
}

u16 mlx5e_get_max_inline_cap(struct mlx5_core_dev *mdev)
{
	int bf_buf_size = (1 << MLX5_CAP_GEN(mdev, log_bf_reg_size)) / 2;

	return bf_buf_size -
	       sizeof(struct mlx5e_tx_wqe) +
	       2 /*sizeof(mlx5e_tx_wqe.inline_hdr_start)*/;
}

void mlx5e_build_default_indir_rqt(u32 *indirection_rqt, int len,
				   int num_channels)
{
	int i;

	for (i = 0; i < len; i++)
		indirection_rqt[i] = i % num_channels;
}

static int mlx5e_get_pci_bw(struct mlx5_core_dev *mdev, u32 *pci_bw)
{
	enum pcie_link_width width;
	enum pci_bus_speed speed;
	int err = 0;

	err = pcie_get_minimum_link(mdev->pdev, &speed, &width);
	if (err)
		return err;

	if (speed == PCI_SPEED_UNKNOWN || width == PCIE_LNK_WIDTH_UNKNOWN)
		return -EINVAL;

	switch (speed) {
	case PCIE_SPEED_2_5GT:
		*pci_bw = 2500 * width;
		break;
	case PCIE_SPEED_5_0GT:
		*pci_bw = 5000 * width;
		break;
	case PCIE_SPEED_8_0GT:
		*pci_bw = 8000 * width;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static bool cqe_compress_heuristic(u32 link_speed, u32 pci_bw)
{
	return (link_speed && pci_bw &&
		(pci_bw < 40000) && (pci_bw < link_speed));
}

static bool hw_lro_heuristic(u32 link_speed, u32 pci_bw)
{
	return !(link_speed && pci_bw &&
		 (pci_bw <= 16000) && (pci_bw < link_speed));
}

void mlx5e_set_tx_cq_mode_params(struct mlx5e_params *params, u8 cq_period_mode)
{
	params->tx_cq_moderation.cq_period_mode = cq_period_mode;

	params->tx_cq_moderation.pkts =
		MLX5E_PARAMS_DEFAULT_TX_CQ_MODERATION_PKTS;
	params->tx_cq_moderation.usec =
		MLX5E_PARAMS_DEFAULT_TX_CQ_MODERATION_USEC;

	if (cq_period_mode == MLX5_CQ_PERIOD_MODE_START_FROM_CQE)
		params->tx_cq_moderation.usec =
			MLX5E_PARAMS_DEFAULT_TX_CQ_MODERATION_USEC_FROM_CQE;

	MLX5E_SET_PFLAG(params, MLX5E_PFLAG_TX_CQE_BASED_MODER,
			params->tx_cq_moderation.cq_period_mode ==
				MLX5_CQ_PERIOD_MODE_START_FROM_CQE);
}

void mlx5e_set_rx_cq_mode_params(struct mlx5e_params *params, u8 cq_period_mode)
{
	params->rx_cq_moderation.cq_period_mode = cq_period_mode;

	params->rx_cq_moderation.pkts =
		MLX5E_PARAMS_DEFAULT_RX_CQ_MODERATION_PKTS;
	params->rx_cq_moderation.usec =
		MLX5E_PARAMS_DEFAULT_RX_CQ_MODERATION_USEC;

	if (cq_period_mode == MLX5_CQ_PERIOD_MODE_START_FROM_CQE)
		params->rx_cq_moderation.usec =
			MLX5E_PARAMS_DEFAULT_RX_CQ_MODERATION_USEC_FROM_CQE;

	if (params->rx_am_enabled)
		params->rx_cq_moderation =
			mlx5e_am_get_def_profile(cq_period_mode);

	MLX5E_SET_PFLAG(params, MLX5E_PFLAG_RX_CQE_BASED_MODER,
			params->rx_cq_moderation.cq_period_mode ==
				MLX5_CQ_PERIOD_MODE_START_FROM_CQE);
}

u32 mlx5e_choose_lro_timeout(struct mlx5_core_dev *mdev, u32 wanted_timeout)
{
	int i;

	/* The supported periods are organized in ascending order */
	for (i = 0; i < MLX5E_LRO_TIMEOUT_ARR_SIZE - 1; i++)
		if (MLX5_CAP_ETH(mdev, lro_timer_supported_periods[i]) >= wanted_timeout)
			break;

	return MLX5_CAP_ETH(mdev, lro_timer_supported_periods[i]);
}

static void mlx5e_init_delay_drop(struct mlx5e_priv *priv,
				  struct mlx5e_params *params)
{
	if (!mlx5e_dropless_rq_supported(priv->mdev))
		return;

	mutex_init(&priv->delay_drop.lock);
	priv->delay_drop.activate = false;
	priv->delay_drop.usec_timeout = MLX5_MAX_DELAY_DROP_TIMEOUT_MS * 1000;
	INIT_WORK(&priv->delay_drop.work, mlx5e_delay_drop_handler);
}

void mlx5e_build_nic_params(struct mlx5e_priv *priv,
			    struct mlx5e_params *params,
			    u16 max_channels)
{
	struct mlx5_core_dev *mdev = priv->mdev;
	u8 cq_period_mode = 0;
	u32 link_speed = 0;
	u32 pci_bw = 0;

	params->num_channels = max_channels;
	params->num_tc       = 1;

	mlx5e_get_max_linkspeed(mdev, &link_speed);
	mlx5e_get_pci_bw(mdev, &pci_bw);
	mlx5_core_dbg(mdev, "Max link speed = %d, PCI BW = %d\n",
		      link_speed, pci_bw);

	/* SQ */
	params->log_sq_size = is_kdump_kernel() ?
		MLX5E_PARAMS_MINIMUM_LOG_SQ_SIZE :
		MLX5E_PARAMS_DEFAULT_LOG_SQ_SIZE;

	/* set CQE compression */
	params->rx_cqe_compress_def = false;
	if (MLX5_CAP_GEN(mdev, cqe_compression) &&
	    MLX5_CAP_GEN(mdev, vport_group_manager))
		params->rx_cqe_compress_def = cqe_compress_heuristic(link_speed, pci_bw);

	MLX5E_SET_PFLAG(params, MLX5E_PFLAG_RX_CQE_COMPRESS, params->rx_cqe_compress_def);

	/* RQ */
	mlx5e_set_rq_params(priv, params);

	/* HW LRO */

	/* TODO: && MLX5_CAP_ETH(mdev, lro_cap) */
	if (params->rq_wq_type == MLX5_WQ_TYPE_LINKED_LIST_STRIDING_RQ)
		params->lro_en = hw_lro_heuristic(link_speed, pci_bw);
	params->lro_timeout = mlx5e_choose_lro_timeout(mdev, MLX5E_DEFAULT_LRO_TIMEOUT);

	/* CQ moderation params */
	cq_period_mode = MLX5_CAP_GEN(mdev, cq_period_start_from_cqe) ?
			MLX5_CQ_PERIOD_MODE_START_FROM_CQE :
			MLX5_CQ_PERIOD_MODE_START_FROM_EQE;

#ifdef CONFIG_COMPAT_LRO_ENABLED_IPOIB
	MLX5E_SET_PFLAG(params, MLX5E_PFLAG_HWLRO, priv->channels.params.lro_en);
#endif

	params->rx_am_enabled = MLX5_CAP_GEN(mdev, cq_moderation);
	mlx5e_set_rx_cq_mode_params(params, cq_period_mode);
	mlx5e_set_tx_cq_mode_params(params, MLX5_CQ_PERIOD_MODE_START_FROM_EQE);

	/* TX inline */
	params->tx_max_inline = mlx5e_get_max_inline_cap(mdev);
	params->tx_min_inline_mode = mlx5e_params_calculate_tx_min_inline(mdev);

	/* RSS */
#ifdef HAVE_ETH_SS_RSS_HASH_FUNCS
	params->rss_hfunc = ETH_RSS_HASH_XOR;
#endif
	netdev_rss_key_fill(params->toeplitz_hash_key, sizeof(params->toeplitz_hash_key));
	mlx5e_build_default_indir_rqt(params->indirection_rqt,
				      MLX5E_INDIR_RQT_SIZE, max_channels);

	/* Sniffer is off by default - performance wise */
	MLX5E_SET_PFLAG(params, MLX5E_PFLAG_SNIFFER, 0);

	MLX5E_SET_PFLAG(params, MLX5E_PFLAG_PER_CH_STATS, true);
}

static void mlx5e_build_nic_netdev_priv(struct mlx5_core_dev *mdev,
					struct net_device *netdev,
					const struct mlx5e_profile *profile,
					void *ppriv)
{
	struct mlx5e_priv *priv = netdev_priv(netdev);

	priv->mdev        = mdev;
	priv->netdev      = netdev;
	priv->profile     = profile;
	priv->ppriv       = ppriv;
	priv->msglevel    = MLX5E_MSG_LEVEL;
	priv->hard_mtu = MLX5E_ETH_HARD_MTU;
#ifdef CONFIG_MLX5_EN_SPECIAL_SQ
	priv->channels.params.num_rl_txqs = 0;
#endif

	mlx5e_build_nic_params(priv, &priv->channels.params, profile->max_nch(mdev));

	mutex_init(&priv->state_lock);

	INIT_WORK(&priv->update_carrier_work, mlx5e_update_carrier_work);
	INIT_WORK(&priv->set_rx_mode_work, mlx5e_set_rx_mode_work);
	INIT_WORK(&priv->tx_timeout_work, mlx5e_tx_timeout_work);
	INIT_DELAYED_WORK(&priv->update_stats_work, mlx5e_update_stats_work);

	mlx5e_timestamp_init(priv);

	mlx5e_init_delay_drop(priv, &priv->channels.params);
}

static void mlx5e_set_netdev_dev_addr(struct net_device *netdev)
{
	struct mlx5e_priv *priv = netdev_priv(netdev);

	mlx5_query_nic_vport_mac_address(priv->mdev, 0, netdev->dev_addr);
	if (is_zero_ether_addr(netdev->dev_addr) &&
	    !MLX5_CAP_GEN(priv->mdev, vport_group_manager)) {
		eth_hw_addr_random(netdev);
		mlx5_core_info(priv->mdev, "Assigned random MAC address %pM\n", netdev->dev_addr);
	}
}

#ifdef HAVE_SWITCHDEV_OPS
#if IS_ENABLED(CONFIG_NET_SWITCHDEV) && IS_ENABLED(CONFIG_MLX5_ESWITCH)
static const struct switchdev_ops mlx5e_switchdev_ops = {
	.switchdev_port_attr_get	= mlx5e_attr_get,
};
#endif
#endif

static void mlx5e_build_nic_netdev(struct net_device *netdev)
{
	struct mlx5e_priv *priv = netdev_priv(netdev);
	struct mlx5_core_dev *mdev = priv->mdev;
#ifdef HAVE_NETDEV_HW_FEATURES
	bool fcs_supported;
	bool fcs_enabled;
#endif

	SET_NETDEV_DEV(netdev, &mdev->pdev->dev);

	netdev->netdev_ops = &mlx5e_netdev_ops;

#ifdef HAVE_IEEE_DCBNL_ETS
#ifdef CONFIG_MLX5_CORE_EN_DCB
	if (MLX5_CAP_GEN(mdev, vport_group_manager) && MLX5_CAP_GEN(mdev, qos))
		netdev->dcbnl_ops = &mlx5e_dcbnl_ops;
#endif
#endif

	netdev->watchdog_timeo    = 15 * HZ;

#ifdef HAVE_ETHTOOL_OPS_EXT
	SET_ETHTOOL_OPS(netdev, &mlx5e_ethtool_ops);
	set_ethtool_ops_ext(netdev, &mlx5e_ethtool_ops_ext);
#else
	netdev->ethtool_ops	  = &mlx5e_ethtool_ops;
#endif

	netdev->vlan_features    |= NETIF_F_SG;
	netdev->vlan_features    |= NETIF_F_IP_CSUM;
	netdev->vlan_features    |= NETIF_F_IPV6_CSUM;
	netdev->vlan_features    |= NETIF_F_GRO;
	netdev->vlan_features    |= NETIF_F_TSO;
	netdev->vlan_features    |= NETIF_F_TSO6;
	netdev->vlan_features    |= NETIF_F_RXCSUM;
#ifdef HAVE_NETIF_F_RXHASH
	netdev->vlan_features    |= NETIF_F_RXHASH;
#endif

	if (!!MLX5_CAP_ETH(mdev, lro_cap) &&
	    mlx5e_check_fragmented_striding_rq_cap(mdev))
		netdev->vlan_features    |= NETIF_F_LRO;

#ifdef HAVE_NETDEV_HW_FEATURES
	netdev->hw_features       = netdev->vlan_features;
	netdev->hw_features      |= NETIF_F_HW_VLAN_CTAG_TX;
	netdev->hw_features      |= NETIF_F_HW_VLAN_CTAG_RX;
	netdev->hw_features      |= NETIF_F_HW_VLAN_CTAG_FILTER;
#ifdef HAVE_NETIF_F_HW_VLAN_STAG_RX
	netdev->hw_features      |= NETIF_F_HW_VLAN_STAG_TX;
#endif

#if defined(HAVE_NETDEV_FEATURES_T) || defined(HAVE_NDO_GSO_CHECK)
#ifdef HAVE_KERNEL_WITH_VXLAN_SUPPORT_ON
	if (mlx5e_vxlan_allowed(mdev) || MLX5_CAP_ETH(mdev, tunnel_stateless_gre)) {
#else
	if (MLX5_CAP_ETH(mdev, tunnel_stateless_gre)) {
#endif
#ifdef HAVE_NETIF_F_GSO_PARTIAL
		netdev->hw_features     |= NETIF_F_GSO_PARTIAL;
#endif
#ifdef HAVE_NETDEV_HW_ENC_FEATURES
		netdev->hw_enc_features |= NETIF_F_IP_CSUM;
		netdev->hw_enc_features |= NETIF_F_IPV6_CSUM;
		netdev->hw_enc_features |= NETIF_F_TSO;
		netdev->hw_enc_features |= NETIF_F_TSO6;
#ifdef HAVE_NETIF_F_GSO_PARTIAL
		netdev->hw_enc_features |= NETIF_F_GSO_PARTIAL;
#endif
#endif
	}
#endif

#ifdef HAVE_KERNEL_WITH_VXLAN_SUPPORT_ON
	if (mlx5e_vxlan_allowed(mdev)) {
#ifdef HAVE_NETIF_F_GSO_UDP_TUNNEL
		netdev->hw_features     |= NETIF_F_GSO_UDP_TUNNEL |
#ifdef HAVE_NETIF_F_GSO_UDP_TUNNEL_CSUM
					   NETIF_F_GSO_UDP_TUNNEL_CSUM;
#else
					   0;
#endif
#endif

#ifdef HAVE_NETDEV_HW_ENC_FEATURES
#ifdef HAVE_NETIF_F_GSO_UDP_TUNNEL
		netdev->hw_enc_features |= NETIF_F_GSO_UDP_TUNNEL |
#ifdef HAVE_NETIF_F_GSO_UDP_TUNNEL_CSUM
					   NETIF_F_GSO_UDP_TUNNEL_CSUM;
#else
					   0;
#endif
#endif
#endif

#ifdef HAVE_NETIF_F_GSO_PARTIAL
		netdev->gso_partial_features = NETIF_F_GSO_UDP_TUNNEL_CSUM;
#endif
	}
#endif

	if (MLX5_CAP_ETH(mdev, tunnel_stateless_gre)) {
#ifdef HAVE_NETIF_F_GSO_GRE_CSUM
		netdev->hw_features     |= NETIF_F_GSO_GRE |
					   NETIF_F_GSO_GRE_CSUM;
#ifdef HAVE_NETDEV_HW_ENC_FEATURES
		netdev->hw_enc_features |= NETIF_F_GSO_GRE |
					   NETIF_F_GSO_GRE_CSUM;
#endif
#endif
#ifdef HAVE_NETIF_F_GSO_PARTIAL
		netdev->gso_partial_features |= NETIF_F_GSO_GRE |
						NETIF_F_GSO_GRE_CSUM;
#endif
	}

	mlx5_query_port_fcs(mdev, &fcs_supported, &fcs_enabled);

#ifdef HAVE_NETIF_F_RXALL
	if (fcs_supported)
		netdev->hw_features |= NETIF_F_RXALL;
#endif

#ifdef HAVE_NETIF_F_RXFCS
	if (MLX5_CAP_ETH(mdev, scatter_fcs))
		netdev->hw_features |= NETIF_F_RXFCS;
#endif

	netdev->features          = netdev->hw_features;
#else
	netdev->features       = netdev->vlan_features;
	netdev->features      |= NETIF_F_HW_VLAN_CTAG_TX;
	netdev->features      |= NETIF_F_HW_VLAN_CTAG_RX;
	netdev->features      |= NETIF_F_HW_VLAN_CTAG_FILTER;
#ifdef HAVE_SET_NETDEV_HW_FEATURES
	set_netdev_hw_features(netdev, netdev->features);
#endif
#endif
	if (!priv->channels.params.lro_en)
		netdev->features  &= ~NETIF_F_LRO;

#ifdef HAVE_NETIF_F_RXALL
	if (fcs_enabled)
		netdev->features  &= ~NETIF_F_RXALL;
#endif

#ifdef HAVE_NETIF_F_RXFCS
	if (!priv->channels.params.scatter_fcs_en)
		netdev->features  &= ~NETIF_F_RXFCS;
#endif

#ifdef HAVE_NETDEV_HW_FEATURES
#define FT_CAP(f) MLX5_CAP_FLOWTABLE(mdev, flow_table_properties_nic_receive.f)
	if (FT_CAP(flow_modify_en) &&
	    FT_CAP(modify_root) &&
	    FT_CAP(identified_miss_table_mode) &&
	    FT_CAP(flow_table_modify)) {
#ifdef HAVE_TC_FLOWER_OFFLOAD
		netdev->hw_features      |= NETIF_F_HW_TC;
#endif
#ifdef CONFIG_RFS_ACCEL
		netdev->hw_features	 |= NETIF_F_NTUPLE;
#endif
	}
#endif

	netdev->features         |= NETIF_F_HIGHDMA;
#ifdef HAVE_NETIF_F_HW_VLAN_STAG_RX
	netdev->features         |= NETIF_F_HW_VLAN_STAG_FILTER;
#endif

#ifdef HAVE_NETDEV_IFF_UNICAST_FLT
	netdev->priv_flags       |= IFF_UNICAST_FLT;
#endif

#ifdef HAVE_NET_DEVICE_OPS_EXT
	set_netdev_ops_ext(netdev, &mlx5e_netdev_ops_ext);
#endif

	mlx5e_set_netdev_dev_addr(netdev);

#ifdef HAVE_SWITCHDEV_OPS
#if IS_ENABLED(CONFIG_NET_SWITCHDEV) && IS_ENABLED(CONFIG_MLX5_ESWITCH)
	if (MLX5_VPORT_MANAGER(mdev))
		netdev->switchdev_ops = &mlx5e_switchdev_ops;
#endif
#endif

	mlx5e_ipsec_build_netdev(priv);
}

static void mlx5e_create_q_counter(struct mlx5e_priv *priv)
{
	struct mlx5_core_dev *mdev = priv->mdev;
	int err;

	err = mlx5_core_alloc_q_counter(mdev, &priv->q_counter);
	if (err) {
		mlx5_core_warn(mdev, "alloc queue counter failed, %d\n", err);
		priv->q_counter = 0;
	}
}

static void mlx5e_destroy_q_counter(struct mlx5e_priv *priv)
{
	if (!priv->q_counter)
		return;

	mlx5_core_dealloc_q_counter(priv->mdev, priv->q_counter);
}

static void mlx5e_nic_init(struct mlx5_core_dev *mdev,
			   struct net_device *netdev,
			   const struct mlx5e_profile *profile,
			   void *ppriv)
{
	struct mlx5e_priv *priv = netdev_priv(netdev);
	int err;

	mlx5e_build_nic_netdev_priv(mdev, netdev, profile, ppriv);
	err = mlx5e_ipsec_init(priv);
	if (err)
		mlx5_core_err(mdev, "IPSec initialization failed, %d\n", err);
	mlx5e_build_nic_netdev(netdev);
}

static void mlx5e_nic_cleanup(struct mlx5e_priv *priv)
{
	mlx5e_ipsec_cleanup(priv);

#ifdef HAVE_NETDEV_BPF
	if (priv->channels.params.xdp_prog)
		bpf_prog_put(priv->channels.params.xdp_prog);
#endif
}

static int mlx5e_init_nic_rx(struct mlx5e_priv *priv)
{
	struct mlx5_core_dev *mdev = priv->mdev;
	int err;

	err = mlx5e_create_indirect_rqt(priv);
	if (err)
		return err;

	err = mlx5e_create_direct_rqts(priv);
	if (err)
		goto err_destroy_indirect_rqts;

	err = mlx5e_create_indirect_tirs(priv);
	if (err)
		goto err_destroy_direct_rqts;

	err = mlx5e_create_direct_tirs(priv);
	if (err)
		goto err_destroy_indirect_tirs;

	err = mlx5e_create_flow_steering(priv);
	if (err) {
		mlx5_core_warn(mdev, "create flow steering failed, %d\n", err);
		goto err_destroy_direct_tirs;
	}

	err = mlx5e_tc_init(priv);
	if (err)
		goto err_destroy_flow_steering;

#ifdef HAVE_KERNEL_WITH_VXLAN_SUPPORT_ON
	mlx5e_vxlan_init(priv);
#endif
	return 0;

err_destroy_flow_steering:
	mlx5e_destroy_flow_steering(priv);
err_destroy_direct_tirs:
	mlx5e_destroy_direct_tirs(priv);
err_destroy_indirect_tirs:
	mlx5e_destroy_indirect_tirs(priv);
err_destroy_direct_rqts:
	mlx5e_destroy_direct_rqts(priv);
err_destroy_indirect_rqts:
	mlx5e_destroy_rqt(priv, &priv->indir_rqt);
	return err;
}

static void mlx5e_cleanup_nic_rx(struct mlx5e_priv *priv)
{
#ifdef HAVE_KERNEL_WITH_VXLAN_SUPPORT_ON
	mlx5e_vxlan_cleanup(priv);
#endif
	mlx5e_tc_cleanup(priv);
	mlx5e_destroy_flow_steering(priv);
	mlx5e_destroy_direct_tirs(priv);
	mlx5e_destroy_indirect_tirs(priv);
	mlx5e_destroy_direct_rqts(priv);
	mlx5e_destroy_rqt(priv, &priv->indir_rqt);
}

static int mlx5e_init_nic_tx(struct mlx5e_priv *priv)
{
	int err;

	err = mlx5e_create_tises(priv);
	if (err) {
		mlx5_core_warn(priv->mdev, "create tises failed, %d\n", err);
		return err;
	}
#ifdef HAVE_IEEE_DCBNL_ETS
#ifdef CONFIG_MLX5_CORE_EN_DCB
	mlx5e_dcbnl_initialize(priv);
#endif
#endif
	return 0;
}

static void mlx5e_nic_enable(struct mlx5e_priv *priv)
{
	struct net_device *netdev = priv->netdev;
	struct mlx5_core_dev *mdev = priv->mdev;
#if defined(HAVE_NET_DEVICE_MIN_MAX_MTU) || defined(HAVE_NET_DEVICE_MIN_MAX_MTU_EXTENDED)
	u16 max_mtu;
#endif

	mlx5e_init_l2_addr(priv);

	/* Marking the link as currently not needed by the Driver */
	if (!netif_running(netdev))
		mlx5_set_port_admin_status(mdev, MLX5_PORT_DOWN);

#ifdef HAVE_NET_DEVICE_MIN_MAX_MTU
	/* MTU range: 68 - hw-specific max */
	netdev->min_mtu = ETH_MIN_MTU;
	mlx5_query_port_max_mtu(priv->mdev, &max_mtu, 1);
	netdev->max_mtu = MLX5E_HW2SW_MTU(priv, max_mtu);
#elif defined(HAVE_NET_DEVICE_MIN_MAX_MTU_EXTENDED)
	netdev->extended->min_mtu = ETH_MIN_MTU;
	mlx5_query_port_max_mtu(priv->mdev, &max_mtu, 1);
	netdev->extended->max_mtu = MLX5E_HW2SW_MTU(priv, max_mtu);

#endif
	mlx5e_set_dev_port_mtu(priv);

	mlx5_lag_add(mdev, netdev);

	if (!is_valid_ether_addr(netdev->perm_addr))
		memcpy(netdev->perm_addr, netdev->dev_addr, netdev->addr_len);

	mlx5e_enable_async_events(priv);

	if (MLX5_VPORT_MANAGER(priv->mdev))
		mlx5e_register_vport_reps(priv);

	if (netdev->reg_state != NETREG_REGISTERED)
		return;
#ifdef HAVE_IEEE_DCBNL_ETS
#ifdef CONFIG_MLX5_CORE_EN_DCB
	mlx5e_dcbnl_init_app(priv);
#endif
#endif

#ifdef HAVE_KERNEL_WITH_VXLAN_SUPPORT_ON
	/* Device already registered: sync netdev system state */
	if (mlx5e_vxlan_allowed(mdev)) {
		rtnl_lock();
#if defined(HAVE_NDO_UDP_TUNNEL_ADD) || defined(HAVE_NDO_UDP_TUNNEL_ADD_EXTENDED)
		udp_tunnel_get_rx_info(netdev);
#elif defined(HAVE_NDO_ADD_VXLAN_PORT)
		vxlan_get_rx_port(netdev);
#endif
		rtnl_unlock();
	}
#endif /* HAVE_KERNEL_WITH_VXLAN_SUPPORT_ON */

	queue_work(priv->wq, &priv->set_rx_mode_work);

	rtnl_lock();
	if (netif_running(netdev))
		mlx5e_open(netdev);
	else
		mlx5_set_port_admin_status(mdev, MLX5_PORT_DOWN);
	netif_device_attach(netdev);
	rtnl_unlock();
}

static void mlx5e_nic_disable(struct mlx5e_priv *priv)
{
	struct mlx5_core_dev *mdev = priv->mdev;

#ifdef HAVE_IEEE_DCBNL_ETS
#ifdef CONFIG_MLX5_CORE_EN_DCB
	if (priv->netdev->reg_state == NETREG_REGISTERED)
		mlx5e_dcbnl_delete_app(priv);
#endif
#endif

	rtnl_lock();
	if (netif_running(priv->netdev))
		mlx5e_close(priv->netdev);
	netif_device_detach(priv->netdev);
	rtnl_unlock();

	queue_work(priv->wq, &priv->set_rx_mode_work);

	if (MLX5_VPORT_MANAGER(priv->mdev))
		mlx5e_unregister_vport_reps(priv);

	mlx5e_disable_async_events(priv);
	mlx5_lag_remove(mdev);
}

static const struct mlx5e_profile mlx5e_nic_profile = {
	.init		   = mlx5e_nic_init,
	.cleanup	   = mlx5e_nic_cleanup,
	.init_rx	   = mlx5e_init_nic_rx,
	.cleanup_rx	   = mlx5e_cleanup_nic_rx,
	.init_tx	   = mlx5e_init_nic_tx,
	.cleanup_tx	   = mlx5e_cleanup_nic_tx,
	.enable		   = mlx5e_nic_enable,
	.disable	   = mlx5e_nic_disable,
	.update_stats	   = mlx5e_update_ndo_stats,
	.max_nch	   = mlx5e_get_max_num_channels,
	.update_carrier	   = mlx5e_update_carrier,
	.rx_handlers.handle_rx_cqe       = mlx5e_handle_rx_cqe,
	.rx_handlers.handle_rx_cqe_mpwqe = mlx5e_handle_rx_cqe_mpwrq,
	.max_tc		   = MLX5E_MAX_NUM_TC,
};

/* mlx5e generic netdev management API (move to en_common.c) */

struct net_device *mlx5e_create_netdev(struct mlx5_core_dev *mdev,
				       const struct mlx5e_profile *profile,
				       void *ppriv)
{
	int nch = profile->max_nch(mdev);
	struct net_device *netdev;
	struct mlx5e_priv *priv;

	if (MLX5_CAP_GEN(mdev, qos) &&
	    MLX5_CAP_QOS(mdev, packet_pacing))
		mdev->mlx5e_res.max_rl_queues = MLX5E_MAX_RL_QUEUES;

#ifdef HAVE_NEW_TX_RING_SCHEME
	netdev = alloc_etherdev_mqs(sizeof(struct mlx5e_priv),
				    nch * profile->max_tc + mdev->mlx5e_res.max_rl_queues,
				    nch);
#else
	netdev = alloc_etherdev_mq(sizeof(struct mlx5e_priv),
				   nch * profile->max_tc + mdev->mlx5e_res.max_rl_queues);
#ifdef HAVE_NETIF_SET_REAL_NUM_RX_QUEUES
	netif_set_real_num_rx_queues(netdev, nch);
#endif
#endif
	if (!netdev) {
		mlx5_core_err(mdev, "alloc_etherdev_mqs() failed\n");
		return NULL;
	}

#ifdef HAVE_NETDEV_RX_CPU_RMAP
#ifdef CONFIG_RFS_ACCEL
	netdev->rx_cpu_rmap = mdev->rmap;
#endif
#endif

	profile->init(mdev, netdev, profile, ppriv);

	netif_carrier_off(netdev);

	priv = netdev_priv(netdev);

	priv->wq = create_singlethread_workqueue("mlx5e");
	if (!priv->wq)
		goto err_cleanup_nic;

	return netdev;

err_cleanup_nic:
	if (profile->cleanup)
		profile->cleanup(priv);
	free_netdev(netdev);

	return NULL;
}

int mlx5e_attach_netdev(struct mlx5e_priv *priv)
{
	struct mlx5_core_dev *mdev = priv->mdev;
	const struct mlx5e_profile *profile;
	int err;

	profile = priv->profile;
	clear_bit(MLX5E_STATE_DESTROYING, &priv->state);

	err = profile->init_tx(priv);
	if (err)
		goto out;

	err = mlx5e_open_drop_rq(mdev, &priv->drop_rq);
	if (err) {
		mlx5_core_err(mdev, "open drop rq failed, %d\n", err);
		goto err_cleanup_tx;
	}

	err = profile->init_rx(priv);
	if (err)
		goto err_close_drop_rq;

	mlx5e_create_q_counter(priv);

	if (profile->enable)
		profile->enable(priv);

	return 0;

err_close_drop_rq:
	mlx5e_close_drop_rq(&priv->drop_rq);

err_cleanup_tx:
	profile->cleanup_tx(priv);

out:
	return err;
}

void mlx5e_detach_netdev(struct mlx5e_priv *priv)
{
	const struct mlx5e_profile *profile = priv->profile;

	set_bit(MLX5E_STATE_DESTROYING, &priv->state);

	if (profile->disable)
		profile->disable(priv);
	flush_workqueue(priv->wq);

	mlx5e_destroy_q_counter(priv);
	profile->cleanup_rx(priv);
	mlx5e_close_drop_rq(&priv->drop_rq);
	profile->cleanup_tx(priv);
	cancel_delayed_work_sync(&priv->update_stats_work);
}

void mlx5e_destroy_netdev(struct mlx5e_priv *priv)
{
	const struct mlx5e_profile *profile = priv->profile;
	struct net_device *netdev = priv->netdev;

#ifdef DEV_NETMAP
	netmap_detach(netdev);
#endif /* DEV_NETMAP */


	destroy_workqueue(priv->wq);
	if (profile->cleanup)
		profile->cleanup(priv);
	free_netdev(netdev);
}

/* mlx5e_attach and mlx5e_detach scope should be only creating/destroying
 * hardware contexts and to connect it to the current netdev.
 */
static int mlx5e_attach(struct mlx5_core_dev *mdev, void *vpriv)
{
	struct mlx5e_priv *priv = vpriv;
	struct net_device *netdev = priv->netdev;
	int err;

	if (netif_device_present(netdev))
		return 0;

	err = mlx5e_create_mdev_resources(mdev);
	if (err)
		return err;

	err = mlx5e_attach_netdev(priv);
	if (err) {
		mlx5e_destroy_mdev_resources(mdev);
		return err;
	}

	return 0;
}

static void mlx5e_detach(struct mlx5_core_dev *mdev, void *vpriv)
{
	struct mlx5e_priv *priv = vpriv;
	struct net_device *netdev = priv->netdev;

	if (!netif_device_present(netdev))
		return;

	mlx5e_detach_netdev(priv);
	mlx5e_destroy_mdev_resources(mdev);
}

#ifdef HAVE_SWITCHDEV_H_COMPAT
static DEVICE_ATTR(phys_switch_id, S_IRUGO, phys_switch_id_show, NULL);

static struct attribute *pf_sysfs_attrs[] = {
	&dev_attr_phys_switch_id.attr,
	NULL,
};

static struct attribute_group pf_sysfs_attr_group = {
	.attrs = pf_sysfs_attrs,
};
#endif

static void *mlx5e_add(struct mlx5_core_dev *mdev)
{
	struct net_device *netdev;
	void *rpriv = NULL;
	void *priv;
	int err;

	err = mlx5e_check_required_hca_cap(mdev);
	if (err)
		return NULL;

#ifdef CONFIG_MLX5_ESWITCH
	if (MLX5_VPORT_MANAGER(mdev)) {
		rpriv = mlx5e_alloc_nic_rep_priv(mdev);
		if (!rpriv) {
			mlx5_core_warn(mdev, "Failed to alloc NIC rep priv data\n");
			return NULL;
		}
	}
#endif

	netdev = mlx5e_create_netdev(mdev, &mlx5e_nic_profile, rpriv);
	if (!netdev) {
		mlx5_core_err(mdev, "mlx5e_create_netdev failed\n");
		goto err_free_rpriv;
	}

	priv = netdev_priv(netdev);

	err = mlx5e_attach(mdev, priv);
	if (err) {
		mlx5_core_err(mdev, "mlx5e_attach failed, %d\n", err);
		goto err_destroy_netdev;
	}

#ifdef HAVE_SWITCHDEV_H_COMPAT
	if (MLX5_VPORT_MANAGER(mdev) && !netdev->sysfs_groups[0]) {
		netdev->sysfs_groups[0] = &pf_sysfs_attr_group;
	}
#endif

	err = register_netdev(netdev);
	if (err) {
		mlx5_core_err(mdev, "register_netdev failed, %d\n", err);
		goto err_detach;
	}

	err = mlx5e_sysfs_create(netdev);
	if (err)
		goto err_unregister_netdev;

#ifdef HAVE_IEEE_DCBNL_ETS
#ifdef CONFIG_MLX5_CORE_EN_DCB
	mlx5e_dcbnl_init_app(priv);
#endif
#endif

#ifdef DEV_NETMAP
	mlx5e_netmap_attach(priv);
#endif /* DEV_NETMAP */

	return priv;

err_unregister_netdev:
	unregister_netdev(netdev);

err_detach:
	mlx5e_detach(mdev, priv);
err_destroy_netdev:
	mlx5e_destroy_netdev(priv);
err_free_rpriv:
	kfree(rpriv);
	return NULL;
}

static void mlx5e_remove(struct mlx5_core_dev *mdev, void *vpriv)
{
	struct mlx5e_priv *priv = vpriv;
	void *ppriv = priv->ppriv;

#ifdef HAVE_IEEE_DCBNL_ETS
#ifdef CONFIG_MLX5_CORE_EN_DCB
	mlx5e_dcbnl_delete_app(priv);
#endif
#endif

	mlx5e_sysfs_remove(priv->netdev);
	unregister_netdev(priv->netdev);
	mlx5e_detach(mdev, vpriv);
	mlx5e_destroy_netdev(priv);
	kfree(ppriv);
}

static void *mlx5e_get_netdev(void *vpriv)
{
	struct mlx5e_priv *priv = vpriv;

	return priv->netdev;
}

static struct mlx5_interface mlx5e_interface = {
	.add       = mlx5e_add,
	.remove    = mlx5e_remove,
	.attach    = mlx5e_attach,
	.detach    = mlx5e_detach,
	.event     = mlx5e_async_event,
	.protocol  = MLX5_INTERFACE_PROTOCOL_ETH,
	.get_dev   = mlx5e_get_netdev,
};

void mlx5e_init(void)
{
	mlx5e_ipsec_build_inverse_table();
#ifdef __ETHTOOL_DECLARE_LINK_MODE_MASK
	mlx5e_build_ptys2ethtool_map();
#endif
	mlx5_register_interface(&mlx5e_interface);
}

void mlx5e_cleanup(void)
{
	mlx5_unregister_interface(&mlx5e_interface);
}
