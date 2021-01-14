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
#ifndef __MLX5_EN_H__
#define __MLX5_EN_H__

#ifdef HAVE_XDP_BUFF
#include <linux/bpf.h>
#endif
#include <linux/if_vlan.h>
#include <linux/etherdevice.h>
#include <linux/timecounter.h>
#include <linux/clocksource.h>
#include <linux/net_tstamp.h>
#if defined(HAVE_NDO_SET_TX_MAXRATE) || defined(HAVE_NDO_SET_TX_MAXRATE_EXTENDED)
#include <linux/hashtable.h>
#endif
#include <linux/crash_dump.h>
#include <linux/mlx5/driver.h>
#include <linux/mlx5/qp.h>
#include <linux/mlx5/cq.h>
#include <linux/mlx5/port.h>
#include <linux/mlx5/vport.h>
#include <linux/mlx5/transobj.h>
#include <linux/mlx5/fs.h>
#include <linux/rhashtable.h>
#ifdef HAVE_UDP_TUNNEL_NIC_INFO
#include <net/udp_tunnel.h>
#endif
#include <net/switchdev.h>
#include <net/xdp.h>
#include <linux/dim.h>
#ifdef HAVE_BITS_H
#include <linux/bits.h>
#endif
#include "wq.h"
#include "mlx5_core.h"
#include "en_stats.h"
#include "en/dcbnl.h"
#include "en/fs.h"
#include "lib/hv_vhca.h"
#ifdef CONFIG_COMPAT_LRO_ENABLED_IPOIB
#include <linux/inet_lro.h>
#else
#include <net/ip.h>
#endif

#ifndef HAVE_DEVLINK_HEALTH_REPORT_SUPPORT
/* The intention is to pass NULL for backports of old kernels */
struct devlink_health_reporter {};
#endif /* HAVE_DEVLINK_HEALTH_REPORT_SUPPORT */

extern const struct net_device_ops mlx5e_netdev_ops;
#ifdef HAVE_NET_PAGE_POOL_H
struct page_pool;
#endif

#define MLX5E_METADATA_ETHER_TYPE (0x8CE4)
#define MLX5E_METADATA_ETHER_LEN 8

#define MLX5_SET_CFG(p, f, v) MLX5_SET(create_flow_group_in, p, f, v)

#define MLX5E_ETH_HARD_MTU (ETH_HLEN + VLAN_HLEN + ETH_FCS_LEN)

#define MLX5E_HW2SW_MTU(params, hwmtu) ((hwmtu) - ((params)->hard_mtu))
#define MLX5E_SW2HW_MTU(params, swmtu) ((swmtu) + ((params)->hard_mtu))

#define MLX5E_MAX_NUM_TC	8
#define MLX5E_MIN_NUM_TC	0

#define MLX5_RX_HEADROOM NET_SKB_PAD
#define MLX5_SKB_FRAG_SZ(len)	(SKB_DATA_ALIGN(len) +	\
				 SKB_DATA_ALIGN(sizeof(struct skb_shared_info)))

#define MLX5E_RX_MAX_HEAD (256)

#define MLX5_MPWRQ_MIN_LOG_STRIDE_SZ(mdev) \
	(6 + MLX5_CAP_GEN(mdev, cache_line_128byte)) /* HW restriction */
#define MLX5_MPWRQ_LOG_STRIDE_SZ(mdev, req) \
	max_t(u32, MLX5_MPWRQ_MIN_LOG_STRIDE_SZ(mdev), req)
#define MLX5_MPWRQ_DEF_LOG_STRIDE_SZ(mdev) \
	MLX5_MPWRQ_LOG_STRIDE_SZ(mdev, order_base_2(MLX5E_RX_MAX_HEAD))

#define MLX5_MPWRQ_LOG_WQE_SZ			18
#define MLX5_MPWRQ_WQE_PAGE_ORDER  (MLX5_MPWRQ_LOG_WQE_SZ - PAGE_SHIFT > 0 ? \
				    MLX5_MPWRQ_LOG_WQE_SZ - PAGE_SHIFT : 0)
#define MLX5_MPWRQ_PAGES_PER_WQE		BIT(MLX5_MPWRQ_WQE_PAGE_ORDER)

#define MLX5_MTT_OCTW(npages) (ALIGN(npages, 8) / 2)
/* Add another page to MLX5E_REQUIRED_WQE_MTTS as a buffer between
 * WQEs, This page will absorb write overflow by the hardware, when
 * receiving packets larger than MTU. These oversize packets are
 * dropped by the driver at a later stage.
 */
#define MLX5E_REQUIRED_WQE_MTTS		(ALIGN(MLX5_MPWRQ_PAGES_PER_WQE + 1, 8))
#define MLX5E_LOG_ALIGNED_MPWQE_PPW	(ilog2(MLX5E_REQUIRED_WQE_MTTS))
#define MLX5E_REQUIRED_MTTS(wqes)	(wqes * MLX5E_REQUIRED_WQE_MTTS)
#define MLX5E_MAX_RQ_NUM_MTTS	\
	((1 << 16) * 2) /* So that MLX5_MTT_OCTW(num_mtts) fits into u16 */
#define MLX5E_ORDER2_MAX_PACKET_MTU (order_base_2(10 * 1024))
#define MLX5E_PARAMS_MAXIMUM_LOG_RQ_SIZE_MPW	\
		(ilog2(MLX5E_MAX_RQ_NUM_MTTS / MLX5E_REQUIRED_WQE_MTTS))
#define MLX5E_LOG_MAX_RQ_NUM_PACKETS_MPW \
	(MLX5E_PARAMS_MAXIMUM_LOG_RQ_SIZE_MPW + \
	 (MLX5_MPWRQ_LOG_WQE_SZ - MLX5E_ORDER2_MAX_PACKET_MTU))

#define MLX5E_MIN_SKB_FRAG_SZ		(MLX5_SKB_FRAG_SZ(MLX5_RX_HEADROOM))
#define MLX5E_LOG_MAX_RX_WQE_BULK	\
	(ilog2(PAGE_SIZE / roundup_pow_of_two(MLX5E_MIN_SKB_FRAG_SZ)))

#define MLX5E_PARAMS_MINIMUM_LOG_SQ_SIZE                0x6
#define MLX5E_PARAMS_DEFAULT_LOG_SQ_SIZE                0xa
#define MLX5E_PARAMS_MAXIMUM_LOG_SQ_SIZE                0xd

#define MLX5E_PARAMS_MINIMUM_LOG_RQ_SIZE (1 + MLX5E_LOG_MAX_RX_WQE_BULK)
#define MLX5E_PARAMS_DEFAULT_LOG_RQ_SIZE                0xa
#define MLX5E_PARAMS_MAXIMUM_LOG_RQ_SIZE min_t(u8, 0xd,	\
					       MLX5E_LOG_MAX_RQ_NUM_PACKETS_MPW)

#define MLX5E_PARAMS_MINIMUM_LOG_RQ_SIZE_MPW            0x2

#define MLX5E_PARAMS_DEFAULT_LRO_WQE_SZ                 (64 * 1024)

#ifdef CONFIG_PPC
#define MLX5E_DEFAULT_LRO_TIMEOUT                       1024
#else
#define MLX5E_DEFAULT_LRO_TIMEOUT                       32
#endif
#define MLX5E_LRO_TIMEOUT_ARR_SIZE                      4

#define MLX5E_PARAMS_DEFAULT_RX_CQ_MODERATION_USEC      0x10
#define MLX5E_PARAMS_DEFAULT_RX_CQ_MODERATION_USEC_FROM_CQE 0x3
#define MLX5E_PARAMS_DEFAULT_RX_CQ_MODERATION_PKTS      0x20
#define MLX5E_PARAMS_DEFAULT_TX_CQ_MODERATION_USEC      0x10
#define MLX5E_PARAMS_DEFAULT_TX_CQ_MODERATION_USEC_FROM_CQE 0x10
#define MLX5E_PARAMS_DEFAULT_TX_CQ_MODERATION_PKTS      0x20
#define MLX5E_PARAMS_DEFAULT_MIN_RX_WQES                0x80
#define MLX5E_PARAMS_DEFAULT_MIN_RX_WQES_MPW            0x2

#define MLX5E_LOG_INDIR_RQT_SIZE       0x7
#define MLX5E_INDIR_RQT_SIZE           BIT(MLX5E_LOG_INDIR_RQT_SIZE)
#define MLX5E_MIN_NUM_CHANNELS         0x1
#define MLX5E_MAX_NUM_CHANNELS         MLX5E_INDIR_RQT_SIZE
#define MLX5E_MAX_NUM_SQS              (MLX5E_MAX_NUM_CHANNELS * MLX5E_MAX_NUM_TC)

#ifdef CONFIG_MLX5_EN_SPECIAL_SQ
#define MLX5E_MAX_RL_QUEUES            512
#else
#define MLX5E_MAX_RL_QUEUES            0
#endif

#define MLX5E_TX_CQ_POLL_BUDGET        128
#define MLX5E_TX_XSK_POLL_BUDGET       64
#define MLX5E_SQ_RECOVER_MIN_INTERVAL  500 /* msecs */

#define MLX5E_UMR_WQE_INLINE_SZ \
	(sizeof(struct mlx5e_umr_wqe) + \
	 ALIGN(MLX5_MPWRQ_PAGES_PER_WQE * sizeof(struct mlx5_mtt), \
	       MLX5_UMR_MTT_ALIGNMENT))
#define MLX5E_UMR_WQEBBS \
	(DIV_ROUND_UP(MLX5E_UMR_WQE_INLINE_SZ, MLX5_SEND_WQE_BB))

#define MLX5E_MSG_LEVEL			NETIF_MSG_LINK

#define mlx5e_dbg(mlevel, priv, format, ...)                    \
do {                                                            \
	if (NETIF_MSG_##mlevel & (priv)->msglevel)              \
		netdev_warn(priv->netdev, format,               \
			    ##__VA_ARGS__);                     \
} while (0)

enum mlx5e_rq_group {
	MLX5E_RQ_GROUP_REGULAR,
	MLX5E_RQ_GROUP_XSK,
#define MLX5E_NUM_RQ_GROUPS(g) (1 + MLX5E_RQ_GROUP_##g)
};

static inline u8 mlx5e_get_num_lag_ports(struct mlx5_core_dev *mdev)
{
	if (mlx5_lag_is_lacp_owner(mdev))
		return 1;

	return clamp_t(u8, MLX5_CAP_GEN(mdev, num_lag_ports), 1, MLX5_MAX_PORTS);
}

static inline u16 mlx5_min_rx_wqes(int wq_type, u32 wq_size)
{
	switch (wq_type) {
	case MLX5_WQ_TYPE_LINKED_LIST_STRIDING_RQ:
		return min_t(u16, MLX5E_PARAMS_DEFAULT_MIN_RX_WQES_MPW,
			     wq_size / 2);
	default:
		return min_t(u16, MLX5E_PARAMS_DEFAULT_MIN_RX_WQES,
			     wq_size / 2);
	}
}

/* Use this function to get max num channels (rxqs/txqs) only to create netdev */
static inline int mlx5e_get_max_num_channels(struct mlx5_core_dev *mdev)
{
	return is_kdump_kernel() ?
		MLX5E_MIN_NUM_CHANNELS :
		min_t(int, mlx5_comp_vectors_count(mdev), MLX5E_MAX_NUM_CHANNELS);
}

enum {
	MLX5E_CON_PROTOCOL_802_1_RP,
	MLX5E_CON_PROTOCOL_R_ROCE_RP,
	MLX5E_CON_PROTOCOL_R_ROCE_NP,
	MLX5E_CONG_PROTOCOL_NUM,
};

struct mlx5e_tx_wqe {
	struct mlx5_wqe_ctrl_seg ctrl;
	struct mlx5_wqe_eth_seg  eth;
	struct mlx5_wqe_data_seg data[0];
};

struct mlx5e_rx_wqe_ll {
	struct mlx5_wqe_srq_next_seg  next;
	struct mlx5_wqe_data_seg      data[];
};

struct mlx5e_rx_wqe_cyc {
	struct mlx5_wqe_data_seg      data[0];
};

struct mlx5e_umr_wqe {
	struct mlx5_wqe_ctrl_seg       ctrl;
	struct mlx5_wqe_umr_ctrl_seg   uctrl;
	struct mlx5_mkey_seg           mkc;
	struct mlx5_mtt                inline_mtts[0];
};

extern const char mlx5e_self_tests[][ETH_GSTRING_LEN];

enum mlx5e_priv_flag {
	MLX5E_PFLAG_RX_CQE_BASED_MODER,
	MLX5E_PFLAG_TX_CQE_BASED_MODER,
	MLX5E_PFLAG_RX_CQE_COMPRESS,
	MLX5E_PFLAG_TX_CQE_COMPRESS,
	MLX5E_PFLAG_RX_STRIDING_RQ,
	MLX5E_PFLAG_RX_NO_CSUM_COMPLETE,
#ifdef HAVE_XDP_BUFF
	MLX5E_PFLAG_XDP_TX_MPWQE,
#endif
	MLX5E_PFLAG_SKB_TX_MPWQE,
	MLX5E_PFLAG_DROPLESS_RQ,
	MLX5E_PFLAG_PER_CH_STATS,
	/* OFED-specific private flags */
#ifdef CONFIG_COMPAT_LRO_ENABLED_IPOIB
	MLX5E_PFLAG_HWLRO,
#endif
	MLX5E_PFLAG_TX_XDP_CSUM,
	MLX5E_PFLAG_SKB_XMIT_MORE,
	MLX5E_PFLAG_TX_PORT_TS,
	MLX5E_NUM_PFLAGS, /* Keep last */
};

#define MLX5E_SET_PFLAG(params, pflag, enable)			\
	do {							\
		if (enable)					\
			(params)->pflags |= BIT(pflag);		\
		else						\
			(params)->pflags &= ~(BIT(pflag));	\
	} while (0)

#define MLX5E_GET_PFLAG(params, pflag) (!!((params)->pflags & (BIT(pflag))))

struct mlx5e_params {
	u8  log_sq_size;
	u8  rq_wq_type;
	u8  log_rq_mtu_frames;
	u16 num_channels;
	u8  num_tc;
#ifdef CONFIG_MLX5_EN_SPECIAL_SQ
	u16 num_rl_txqs;
#endif
	bool rx_cqe_compress_def;
	bool tunneled_offload_en;
	struct dim_cq_moder rx_cq_moderation;
	struct dim_cq_moder tx_cq_moderation;
	bool lro_en;
	u8  tx_min_inline_mode;
	bool vlan_strip_disable;
	bool scatter_fcs_en;
	bool rx_dim_enabled;
	bool tx_dim_enabled;
	u32 lro_timeout;
	u32 pflags;
#ifdef HAVE_XDP_BUFF
	struct bpf_prog *xdp_prog;
#endif
	struct mlx5e_xsk *xsk;
	unsigned int sw_mtu;
	int hard_mtu;
	struct {
		__u32 flag;
		u32 mst_size;
	}                          dump;
};

enum {
	MLX5E_RQ_STATE_ENABLED,
	MLX5E_RQ_STATE_RECOVERING,
	MLX5E_RQ_STATE_AM,
	MLX5E_RQ_STATE_NO_CSUM_COMPLETE,
	MLX5E_RQ_STATE_CSUM_FULL, /* cqe_csum_full hw bit is set */
	MLX5E_RQ_STATE_FPGA_TLS, /* FPGA TLS enabled */
	MLX5E_RQ_STATE_MINI_CQE_HW_STRIDX, /* set when mini_cqe_resp_stride_index cap is used */
	MLX5E_RQ_STATE_CACHE_REDUCE_PENDING,
	MLX5E_RQ_STATE_SKB_XMIT_MORE,
};

struct mlx5e_cq {
	/* data path - accessed per cqe */
	struct mlx5_cqwq           wq;

	/* data path - accessed per napi poll */
	u16                        event_ctr;
	struct napi_struct        *napi;
	struct mlx5_core_cq        mcq;
	struct mlx5e_ch_stats     *ch_stats;
#ifndef HAVE_NAPI_STATE_MISSED
	unsigned long             *ch_flags;
#endif

	/* control */
	struct net_device         *netdev;
	struct mlx5_core_dev      *mdev;
	struct mlx5e_priv         *priv;
	struct mlx5_wq_ctrl        wq_ctrl;
	bool no_arm;
} ____cacheline_aligned_in_smp;

struct mlx5e_cq_decomp {
	/* cqe decompression */
	struct mlx5_cqe64          title;
	struct mlx5_mini_cqe8      mini_arr[MLX5_MINI_CQE_ARRAY_SIZE];
	u8                         mini_arr_idx;
	u16                        left;
	u16                        wqe_counter;
} ____cacheline_aligned_in_smp;

enum mlx5e_dma_map_type {
	MLX5E_DMA_MAP_SINGLE,
	MLX5E_DMA_MAP_PAGE
};

struct mlx5e_sq_dma {
	dma_addr_t              addr;
	u32                     size;
	enum mlx5e_dma_map_type type;
};

enum {
	MLX5E_SQ_STATE_ENABLED,
	MLX5E_SQ_STATE_MPWQE,
	MLX5E_SQ_STATE_RECOVERING,
	MLX5E_SQ_STATE_IPSEC,
	MLX5E_SQ_STATE_AM,
	MLX5E_SQ_STATE_TLS,
	MLX5E_SQ_STATE_VLAN_NEED_L2_INLINE,
	MLX5E_SQ_STATE_PENDING_XSK_TX,
	MLX5E_SQ_STATE_SKB_XMIT_MORE,
#ifdef HAVE_XDP_REDIRECT
	MLX5E_SQ_STATE_REDIRECT,
#endif
};

struct mlx5e_tx_mpwqe {
	/* Current MPWQE session */
	struct mlx5e_tx_wqe *wqe;
	u32 bytes_count;
	u8 ds_count;
	u8 pkt_count;
	u8 inline_on;
};

#ifdef CONFIG_MLX5_EN_SPECIAL_SQ
#if defined(HAVE_NDO_SET_TX_MAXRATE) || defined(HAVE_NDO_SET_TX_MAXRATE_EXTENDED)
struct mlx5e_sq_flow_map {
	struct hlist_node hlist;
	u32               dst_ip;
	u16               dst_port;
	u16               queue_index;
};
#endif /* HAVE_NDO_SET_TX_MAXRATE || HAVE_NDO_SET_TX_MAXRATE_EXTENDED */
#endif

struct mlx5e_dim {
	struct dim dim;
	struct dim_sample sample;
};

struct mlx5e_skb_fifo {
	struct sk_buff **fifo;
	u16 *pc;
	u16 *cc;
	u16 mask;
};

struct mlx5e_ptpsq;

struct mlx5e_txqsq {
	/* data path */

	/* dirtied @completion */
	u16                        cc;
	u16                        skb_fifo_cc;
	u32                        dma_fifo_cc;
	struct mlx5e_dim           dim_obj; /* Adaptive Moderation */

	/* dirtied @xmit */
	u16                        pc ____cacheline_aligned_in_smp;
	u16                        skb_fifo_pc;
	u32                        dma_fifo_pc;
	struct mlx5e_tx_mpwqe      mpwqe;

	struct mlx5e_cq            cq;
	struct mlx5e_cq_decomp     cqd;

	/* read only */
	struct mlx5_wq_cyc         wq;
	u32                        dma_fifo_mask;
	struct mlx5e_sq_stats     *stats;
	struct {
		struct mlx5e_sq_dma       *dma_fifo;
		struct mlx5e_skb_fifo      skb_fifo;
		struct mlx5e_tx_wqe_info  *wqe_info;
	} db;
	void __iomem              *uar_map;
	struct netdev_queue       *txq;
	u32                        sqn;
	u16                        stop_room;
	u8                         min_inline_mode;
	struct device             *pdev;
	__be32                     mkey_be;
	unsigned long              state;
	unsigned int               hw_mtu;
	struct hwtstamp_config    *tstamp;
	struct mlx5_clock         *clock;
	struct net_device         *netdev;
	struct mlx5_core_dev      *mdev;
	struct mlx5e_ch_stats     *ch_stats;
	struct mlx5e_priv         *priv;

	/* control path */
	struct mlx5_wq_ctrl        wq_ctrl;
	int                        ch_ix;
	int                        txq_ix;
	u32                        rate_limit;
	struct work_struct         recover_work;
#if defined(CONFIG_MLX5_EN_SPECIAL_SQ) && (defined(HAVE_NDO_SET_TX_MAXRATE) || defined(HAVE_NDO_SET_TX_MAXRATE_EXTENDED))
	struct mlx5e_sq_flow_map   flow_map;
#endif
	struct mlx5e_ptpsq        *ptpsq;
} ____cacheline_aligned_in_smp;

struct mlx5e_dma_info {
	dma_addr_t addr;
	u32 refcnt_bias;
	union {
		struct page *page;
#ifdef HAVE_XSK_BUFF_ALLOC
		struct xdp_buff *xsk;
#else
		struct {
			u64 handle;
			void *data;
		} xsk;
#endif
	};
};

/* XDP packets can be transmitted in different ways. On completion, we need to
 * distinguish between them to clean up things in a proper way.
 */
#ifdef HAVE_XDP_BUFF
enum mlx5e_xdp_xmit_mode {
	/* An xdp_frame was transmitted due to either XDP_REDIRECT from another
	 * device or XDP_TX from an XSK RQ. The frame has to be unmapped and
	 * returned.
	 */
	MLX5E_XDP_XMIT_MODE_FRAME,

	/* The xdp_frame was created in place as a result of XDP_TX from a
	 * regular RQ. No DMA remapping happened, and the page belongs to us.
	 */
	MLX5E_XDP_XMIT_MODE_PAGE,

	/* No xdp_frame was created at all, the transmit happened from a UMEM
 * page. The UMEM Completion Ring producer pointer has to be increased.
	 */
	MLX5E_XDP_XMIT_MODE_XSK,
};

struct mlx5e_xdp_info {
	enum mlx5e_xdp_xmit_mode mode;
	union {
		struct {
			struct xdp_frame *xdpf;
			dma_addr_t dma_addr;
		} frame;
		struct {
			struct mlx5e_rq *rq;
			struct mlx5e_dma_info di;
		} page;
	};
};
#endif /* HAVE_XDP_BUFF */

struct mlx5e_xmit_data {
	dma_addr_t  dma_addr;
	void       *data;
	u32         len;
};

#ifdef HAVE_XDP_BUFF
struct mlx5e_xdp_info_fifo {
	struct mlx5e_xdp_info *xi;
	u32 *cc;
	u32 *pc;
	u32 mask;
};

struct mlx5e_xdpsq;
typedef int (*mlx5e_fp_xmit_xdp_frame_check)(struct mlx5e_xdpsq *);
typedef bool (*mlx5e_fp_xmit_xdp_frame)(struct mlx5e_xdpsq *,
					struct mlx5e_xmit_data *,
					struct mlx5e_xdp_info *,
					int);

struct mlx5e_xdpsq {
	/* data path */

	/* dirtied @completion */
	u32                        xdpi_fifo_cc;
	u16                        cc;

	/* dirtied @xmit */
	u32                        xdpi_fifo_pc ____cacheline_aligned_in_smp;
	u16                        pc;
	struct mlx5_wqe_ctrl_seg   *doorbell_cseg;
	struct mlx5e_tx_mpwqe      mpwqe;

	struct mlx5e_cq            cq;

	/* read only */
	struct xdp_umem           *umem;
	struct mlx5_wq_cyc         wq;
	struct mlx5e_xdpsq_stats  *stats;
	mlx5e_fp_xmit_xdp_frame_check xmit_xdp_frame_check;
	mlx5e_fp_xmit_xdp_frame    xmit_xdp_frame;
	struct {
		struct mlx5e_xdp_wqe_info *wqe_info;
		struct mlx5e_xdp_info_fifo xdpi_fifo;
	} db;
	void __iomem              *uar_map;
	u32                        sqn;
	struct device             *pdev;
	__be32                     mkey_be;
	u8                         min_inline_mode;
	unsigned long              state;
	unsigned int               hw_mtu;

	/* control path */
	struct mlx5_wq_ctrl        wq_ctrl;
	struct mlx5e_channel      *channel;
} ____cacheline_aligned_in_smp;
#endif /* #ifdef HAVE_XDP_BUFF */

struct mlx5e_icosq {
	/* data path */
	u16                        cc;
	u16                        pc;

	struct mlx5_wqe_ctrl_seg  *doorbell_cseg;
	struct mlx5e_cq            cq;

	/* write@xmit, read@completion */
	struct {
		struct mlx5e_icosq_wqe_info *wqe_info;
	} db;

	/* read only */
	struct mlx5_wq_cyc         wq;
	void __iomem              *uar_map;
	u32                        sqn;
	unsigned long              state;

	/* control path */
	struct mlx5_wq_ctrl        wq_ctrl;
	struct mlx5e_channel      *channel;

	struct work_struct         recover_work;
} ____cacheline_aligned_in_smp;

struct mlx5e_wqe_frag_info {
	struct mlx5e_dma_info *di;
	u32 offset;
	bool last_in_page;
};

struct mlx5e_umr_dma_info {
	struct mlx5e_dma_info  dma_info[MLX5_MPWRQ_PAGES_PER_WQE];
};

struct mlx5e_mpw_info {
	struct mlx5e_umr_dma_info umr;
	u16 consumed_strides;
#ifdef HAVE_XDP_BUFF
	DECLARE_BITMAP(xdp_xmit_bitmap, MLX5_MPWRQ_PAGES_PER_WQE);
#endif
};

#ifdef CONFIG_COMPAT_LRO_ENABLED_IPOIB
#define IS_HW_LRO(params) \
	((params)->lro_en && MLX5E_GET_PFLAG(params, MLX5E_PFLAG_HWLRO))
#define IS_SW_LRO(params) \
	((params)->lro_en && !MLX5E_GET_PFLAG(params, MLX5E_PFLAG_HWLRO))

/* SW LRO defines for MLX5 */
#define MLX5E_LRO_MAX_DESC	32
struct mlx5e_sw_lro {
	struct net_lro_mgr	lro_mgr;
	struct net_lro_desc	lro_desc[MLX5E_LRO_MAX_DESC];
};
#endif

#define MLX5E_MAX_RX_FRAGS 4

#define MLX5E_PAGE_CACHE_LOG_MAX_RQ_MULT	4
#define MLX5E_PAGE_CACHE_REDUCE_WORK_INTERVAL	200 /* msecs */
#define MLX5E_PAGE_CACHE_REDUCE_GRACE_PERIOD	1000 /* msecs */
#define MLX5E_PAGE_CACHE_REDUCE_SUCCESSIVE_CNT	5

struct mlx5e_page_cache_reduce {
	struct delayed_work reduce_work;
	u32 successive;
	unsigned long next_ts;
	unsigned long graceful_period;
	unsigned long delay;

	struct mlx5e_dma_info *pending;
	u32 npages;
};

struct mlx5e_page_cache {
	struct mlx5e_dma_info *page_cache;
	int head;
	u32 sz;
	u32 lrs; /* least recently sampled */
	u8 log_min_sz;
	u8 log_max_sz;
	struct mlx5e_page_cache_reduce reduce;
};

static inline void mlx5e_put_page(struct mlx5e_dma_info *dma_info)
{
	page_ref_sub(dma_info->page, dma_info->refcnt_bias);
	put_page(dma_info->page);
}

struct mlx5e_rq;
typedef void (*mlx5e_fp_handle_rx_cqe)(struct mlx5e_rq*, struct mlx5_cqe64*,
				       bool xmit_more);
typedef struct sk_buff *
(*mlx5e_fp_skb_from_cqe_mpwrq)(struct mlx5e_rq *rq, struct mlx5e_mpw_info *wi,
			       u16 cqe_bcnt, u32 head_offset, u32 page_idx);
typedef struct sk_buff *
(*mlx5e_fp_skb_from_cqe)(struct mlx5e_rq *rq, struct mlx5_cqe64 *cqe,
			 struct mlx5e_wqe_frag_info *wi, u32 cqe_bcnt);
typedef bool (*mlx5e_fp_post_rx_wqes)(struct mlx5e_rq *rq);
typedef void (*mlx5e_fp_dealloc_wqe)(struct mlx5e_rq*, u16);

int mlx5e_rq_set_handlers(struct mlx5e_rq *rq, struct mlx5e_params *params, bool xsk);

enum mlx5e_rq_flag {
	MLX5E_RQ_FLAG_XDP_XMIT,
	MLX5E_RQ_FLAG_XDP_REDIRECT,
};

struct mlx5e_rq_frag_info {
	int frag_size;
	int frag_stride;
};

struct mlx5e_rq_frags_info {
	struct mlx5e_rq_frag_info arr[MLX5E_MAX_RX_FRAGS];
	u8 num_frags;
	u8 log_num_frags;
	u8 wqe_bulk;
};

struct mlx5e_rq {
	/* data path */
	union {
		struct {
			struct mlx5_wq_cyc          wq;
			struct mlx5e_wqe_frag_info *frags;
			struct mlx5e_dma_info      *di;
			struct mlx5e_rq_frags_info  info;
			mlx5e_fp_skb_from_cqe       skb_from_cqe;
		} wqe;
		struct {
			struct mlx5_wq_ll      wq;
			struct mlx5e_umr_wqe   umr_wqe;
			struct mlx5e_mpw_info *info;
			mlx5e_fp_skb_from_cqe_mpwrq skb_from_cqe_mpwrq;
			u16                    num_strides;
			u16                    actual_wq_head;
			u8                     log_stride_sz;
			u8                     umr_in_progress;
			u8                     umr_last_bulk;
			u8                     umr_completed;
		} mpwqe;
	};
	struct {
#ifdef HAVE_XSK_SUPPORT
#ifndef HAVE_XSK_BUFF_ALLOC
		u16            umem_headroom;
#endif
#endif
		u16            headroom;
		u32            frame0_sz;
		u8             map_dir;   /* dma map direction */
	} buff;

	struct device         *pdev;
	struct net_device     *netdev;
	struct mlx5e_rq_stats *stats;
	struct mlx5e_cq        cq;
	struct mlx5e_cq_decomp cqd;
	struct mlx5e_page_cache page_cache;
	struct hwtstamp_config *tstamp;
	struct mlx5_clock      *clock;
	struct mlx5e_ch_stats *ch_stats;
	struct mlx5e_icosq    *icosq;
	struct mlx5e_priv     *priv;

	mlx5e_fp_handle_rx_cqe handle_rx_cqe;
	mlx5e_fp_post_rx_wqes  post_wqes;
	mlx5e_fp_dealloc_wqe   dealloc_wqe;

	unsigned long          state;
	int                    ix;
	unsigned int           hw_mtu;

	struct mlx5e_dim       dim_obj; /* Adaptive Moderation */

	/* XDP */
#ifdef HAVE_XDP_BUFF
	struct bpf_prog __rcu *xdp_prog;
	struct mlx5e_xdpsq    *xdpsq;
#endif
	DECLARE_BITMAP(flags, 8);
#ifdef HAVE_NET_PAGE_POOL_H
	struct page_pool      *page_pool;
#endif

#ifdef CONFIG_COMPAT_LRO_ENABLED_IPOIB
	struct mlx5e_sw_lro   *sw_lro;
#endif

	/* AF_XDP zero-copy */
#ifdef HAVE_XSK_SUPPORT
#ifndef HAVE_XSK_BUFF_ALLOC
	struct zero_copy_allocator zca;
#endif
#endif
	struct xdp_umem       *umem;

	struct work_struct     recover_work;

	/* control */
	struct mlx5_wq_ctrl    wq_ctrl;
	__be32                 mkey_be;
	u8                     wq_type;
	u32                    rqn;
	struct mlx5_core_dev  *mdev;
	struct mlx5_core_mkey  umr_mkey;
	struct mlx5e_dma_info  wqe_overflow;

	/* XDP read-mostly */
#ifdef HAVE_NET_XDP_H
       struct xdp_rxq_info    xdp_rxq;
#endif
} ____cacheline_aligned_in_smp;

#ifndef HAVE_NAPI_STATE_MISSED
enum channel_flags {
	MLX5E_CHANNEL_NAPI_SCHED = 1,
};
#endif

enum mlx5e_channel_state {
	MLX5E_CHANNEL_STATE_XSK,
	MLX5E_CHANNEL_NUM_STATES
};

struct mlx5e_channel {
	/* data path */
	struct mlx5e_rq            rq;
#ifdef HAVE_XDP_BUFF
	struct mlx5e_xdpsq         rq_xdpsq;
#endif
	struct mlx5e_txqsq         sq[MLX5E_MAX_NUM_TC];
#ifdef CONFIG_MLX5_EN_SPECIAL_SQ
	struct mlx5e_txqsq         *special_sq;
	u16			   num_special_sq;
#endif
	struct mlx5e_icosq         icosq;   /* internal control operations */
#ifdef HAVE_XDP_BUFF
	bool                       xdp;
#endif
	struct napi_struct         napi;
	struct device             *pdev;
	struct net_device         *netdev;
	__be32                     mkey_be;
	u8                         num_tc;
	u8                         lag_port;
#ifndef HAVE_NAPI_STATE_MISSED
	unsigned long              flags;
#endif

#ifdef HAVE_XDP_REDIRECT
	/* XDP_REDIRECT */
	struct mlx5e_xdpsq         xdpsq;
#endif

#ifdef HAVE_XSK_SUPPORT
	/* AF_XDP zero-copy */
	struct mlx5e_rq            xskrq;
	struct mlx5e_xdpsq         xsksq;
#endif

#if defined HAVE_XSK_SUPPORT || defined HAVE_KTLS_RX_SUPPORT
	/* Async ICOSQ */
	struct mlx5e_icosq         async_icosq;
	/* async_icosq can be accessed from any CPU - the spinlock protects it. */
	spinlock_t                 async_icosq_lock;
#endif

	/* data path - accessed per napi poll */
	struct irq_desc *irq_desc;
	struct mlx5e_ch_stats     *stats;

	/* control */
	struct mlx5e_priv         *priv;
	struct mlx5_core_dev      *mdev;
	struct hwtstamp_config    *tstamp;
	DECLARE_BITMAP(state, MLX5E_CHANNEL_NUM_STATES);
	int                        ix;
	int                        cpu;

	struct dentry             *dfs_root;
};

struct mlx5e_port_ptp;

struct mlx5e_channels {
	struct mlx5e_channel **c;
	struct mlx5e_port_ptp  *port_ptp;
	unsigned int           num;
	struct mlx5e_params    params;
};

struct mlx5e_channel_stats {
	struct mlx5e_ch_stats ch;
	struct mlx5e_sq_stats sq[MLX5E_MAX_NUM_TC];
	struct mlx5e_rq_stats rq;
	struct mlx5e_rq_stats xskrq;
#ifdef HAVE_XDP_BUFF
	struct mlx5e_xdpsq_stats rq_xdpsq;
#ifdef HAVE_XDP_REDIRECT
	struct mlx5e_xdpsq_stats xdpsq;
	struct mlx5e_xdpsq_stats xsksq;
#endif
#endif
} ____cacheline_aligned_in_smp;

struct mlx5e_port_ptp_stats {
	struct mlx5e_ch_stats ch;
	struct mlx5e_sq_stats sq[MLX5E_MAX_NUM_TC];
	struct mlx5e_ptp_cq_stats cq[MLX5E_MAX_NUM_TC];
} ____cacheline_aligned_in_smp;

enum {
	MLX5E_STATE_OPENED,
	MLX5E_STATE_DESTROYING,
	MLX5E_STATE_XDP_TX_ENABLED,
	MLX5E_STATE_XDP_ACTIVE,
};

struct mlx5e_rqt {
	u32              rqtn;
	bool		 enabled;
};

struct mlx5e_tir {
	u32		  tirn;
	struct mlx5e_rqt  rqt;
	struct list_head  list;
};

enum {
	MLX5E_TC_PRIO = 0,
	MLX5E_NIC_PRIO
};

struct mlx5e_ecn_rp_attributes {
	struct mlx5_core_dev	*mdev;
	/* ATTRIBUTES */
	struct kobj_attribute	enable;
	struct kobj_attribute	clamp_tgt_rate;
	struct kobj_attribute	clamp_tgt_rate_ati;
	struct kobj_attribute	rpg_time_reset;
	struct kobj_attribute	rpg_byte_reset;
	struct kobj_attribute	rpg_threshold;
	struct kobj_attribute	rpg_max_rate;
	struct kobj_attribute	rpg_ai_rate;
	struct kobj_attribute	rpg_hai_rate;
	struct kobj_attribute	rpg_gd;
	struct kobj_attribute	rpg_min_dec_fac;
	struct kobj_attribute	rpg_min_rate;
	struct kobj_attribute	rate2set_fcnp;
	struct kobj_attribute	dce_tcp_g;
	struct kobj_attribute	dce_tcp_rtt;
	struct kobj_attribute	rreduce_mperiod;
	struct kobj_attribute	initial_alpha_value;
};

struct mlx5e_ecn_np_attributes {
	struct mlx5_core_dev	*mdev;
	/* ATTRIBUTES */
	struct kobj_attribute	enable;
	struct kobj_attribute	min_time_between_cnps;
	struct kobj_attribute	cnp_dscp;
	struct kobj_attribute	cnp_802p_prio;
};

union mlx5e_ecn_attributes {
	struct mlx5e_ecn_rp_attributes rp_attr;
	struct mlx5e_ecn_np_attributes np_attr;
};

struct mlx5e_ecn_ctx {
	union mlx5e_ecn_attributes ecn_attr;
	struct kobject *ecn_proto_kobj;
	struct kobject *ecn_enable_kobj;
};

struct mlx5e_ecn_enable_ctx {
	int cong_protocol;
	int priority;
	struct mlx5_core_dev	*mdev;

	struct kobj_attribute	enable;
};

struct mlx5e_rss_params {
	u32	indirection_rqt[MLX5E_INDIR_RQT_SIZE];
	u32	rx_hash_fields[MLX5E_NUM_INDIR_TIRS];
	u8	toeplitz_hash_key[40];
	u8	hfunc;
};

struct mlx5e_modify_sq_param {
	int curr_state;
	int next_state;
	int rl_update;
	int rl_index;
};

#if IS_ENABLED(CONFIG_PCI_HYPERV_INTERFACE)
struct mlx5e_hv_vhca_stats_agent {
	struct mlx5_hv_vhca_agent *agent;
	struct delayed_work        work;
	u16                        delay;
	void                      *buf;
};
#endif

struct mlx5e_xsk {
	/* UMEMs are stored separately from channels, because we don't want to
	 * lose them when channels are recreated. The kernel also stores UMEMs,
	 * but it doesn't distinguish between zero-copy and non-zero-copy UMEMs,
	 * so rely on our mechanism.
	 */
	struct xdp_umem **umems;
	u16 refcnt;
	bool ever_used;
};

/* Temporary storage for variables that are allocated when struct mlx5e_priv is
 * initialized, and used where we can't allocate them because that functions
 * must not fail. Use with care and make sure the same variable is not used
 * simultaneously by multiple users.
 */
struct mlx5e_scratchpad {
	cpumask_var_t cpumask;
};

struct mlx5e_delay_drop {
	struct work_struct	work;
	/* serialize setting of delay drop */
	struct mutex		lock;
	u32			usec_timeout;
	bool			activate;
};

struct mlx5e_priv {
	/* priv data path fields - start */
	/* +1 for port ptp ts */
	struct mlx5e_txqsq *txq2sq[(MLX5E_MAX_NUM_CHANNELS + 1) * MLX5E_MAX_NUM_TC +
				   MLX5E_MAX_RL_QUEUES];
	int channel_tc2realtxq[MLX5E_MAX_NUM_CHANNELS][MLX5E_MAX_NUM_TC];
	int port_ptp_tc2realtxq[MLX5E_MAX_NUM_TC];
#if defined(CONFIG_MLX5_EN_SPECIAL_SQ) && (defined(HAVE_NDO_SET_TX_MAXRATE) || defined(HAVE_NDO_SET_TX_MAXRATE_EXTENDED))
	DECLARE_HASHTABLE(flow_map_hash, ilog2(MLX5E_MAX_RL_QUEUES));
#endif
#ifdef CONFIG_MLX5_CORE_EN_DCB
	struct mlx5e_dcbx_dp       dcbx_dp;
#endif
	/* priv data path fields - end */

	u32                        msglevel;
	unsigned long              state;
	struct mutex               state_lock; /* Protects Interface state */
	struct mlx5e_rq            drop_rq;

	struct mlx5e_channels      channels;
	u32                        tisn[MLX5_MAX_PORTS][MLX5E_MAX_NUM_TC];
	struct mlx5e_rqt           indir_rqt;
	struct mlx5e_tir           indir_tir[MLX5E_NUM_INDIR_TIRS];
	struct mlx5e_tir           inner_indir_tir[MLX5E_NUM_INDIR_TIRS];
	struct mlx5e_tir           direct_tir[MLX5E_MAX_NUM_CHANNELS];
	struct mlx5e_tir           xsk_tir[MLX5E_MAX_NUM_CHANNELS];
	struct mlx5e_rss_params    rss_params;
	u32                        tx_rates[MLX5E_MAX_NUM_SQS +
					    MLX5E_MAX_RL_QUEUES];

	struct mlx5e_flow_steering fs;

	struct workqueue_struct    *wq;
	struct work_struct         update_carrier_work;
	struct work_struct         set_rx_mode_work;
	struct work_struct         tx_timeout_work;
	struct work_struct         update_stats_work;
	struct work_struct         monitor_counters_work;
	struct mlx5_nb             monitor_counters_nb;

	struct mlx5_core_dev      *mdev;
	struct net_device         *netdev;
	struct mlx5e_stats         stats;
	struct mlx5e_channel_stats channel_stats[MLX5E_MAX_NUM_CHANNELS];
	struct mlx5e_port_ptp_stats port_ptp_stats;
	u16                        max_nch;
#ifdef CONFIG_COMPAT_LRO_ENABLED_IPOIB
	struct mlx5e_sw_lro        sw_lro[MLX5E_MAX_NUM_CHANNELS];
#endif
	u8                         max_opened_tc;
#if !defined(HAVE_NDO_GET_STATS64) && !defined(HAVE_NDO_GET_STATS64_RET_VOID)
	struct net_device_stats    netdev_stats;
#endif
	bool                       port_ptp_opened;
#ifdef CONFIG_MLX5_EN_SPECIAL_SQ
	struct mlx5e_sq_stats      special_sq_stats[MLX5E_MAX_RL_QUEUES];
	int                        max_opened_special_sq;
#endif
	struct hwtstamp_config     tstamp;
	u16                        q_counter;
	u16                        drop_rq_q_counter;
	struct notifier_block      events_nb;
	int                        num_tc_x_num_ch;

#ifdef HAVE_UDP_TUNNEL_NIC_INFO
	struct udp_tunnel_nic_info nic_info;
#endif
#ifdef CONFIG_MLX5_CORE_EN_DCB
	struct mlx5e_dcbx          dcbx;
#endif

	const struct mlx5e_profile *profile;
	void                      *ppriv;
#ifdef CONFIG_MLX5_EN_IPSEC
	struct mlx5e_ipsec        *ipsec;
#endif
#ifdef CONFIG_MLX5_EN_TLS
	struct mlx5e_tls          *tls;
#endif
	struct devlink_health_reporter *tx_reporter;
	struct devlink_health_reporter *rx_reporter;
#ifdef HAVE_DEVLINK_H
	struct devlink_port            dl_port;
#endif
	struct mlx5e_xsk           xsk;
#if IS_ENABLED(CONFIG_PCI_HYPERV_INTERFACE)
	struct mlx5e_hv_vhca_stats_agent stats_agent;
#endif
	struct mlx5e_scratchpad    scratchpad;
	struct dentry *dfs_root;

	struct kobject *ecn_root_kobj;
#ifdef CONFIG_MLX5_ESWITCH
	struct kobject *compat_kobj;
	struct kobject *devlink_kobj;
	void *devlink_attributes;
#endif

	struct mlx5e_ecn_ctx ecn_ctx[MLX5E_CONG_PROTOCOL_NUM];
	struct mlx5e_ecn_enable_ctx ecn_enable_ctx[MLX5E_CONG_PROTOCOL_NUM][8];
	struct mlx5e_delay_drop delay_drop;
};

struct mlx5e_rx_handlers {
	mlx5e_fp_handle_rx_cqe handle_rx_cqe;
	mlx5e_fp_handle_rx_cqe handle_rx_cqe_mpwqe;
};

extern const struct mlx5e_rx_handlers mlx5e_rx_handlers_nic;

struct mlx5e_profile {
	int	(*init)(struct mlx5_core_dev *mdev,
			struct net_device *netdev);
	void	(*cleanup)(struct mlx5e_priv *priv);
	int	(*init_rx)(struct mlx5e_priv *priv);
	void	(*cleanup_rx)(struct mlx5e_priv *priv);
	int	(*init_tx)(struct mlx5e_priv *priv);
	void	(*cleanup_tx)(struct mlx5e_priv *priv);
	void	(*enable)(struct mlx5e_priv *priv);
	void	(*disable)(struct mlx5e_priv *priv);
	int	(*update_rx)(struct mlx5e_priv *priv);
	void	(*update_stats)(struct mlx5e_priv *priv);
	void	(*update_carrier)(struct mlx5e_priv *priv);
	unsigned int (*stats_grps_num)(struct mlx5e_priv *priv);
	mlx5e_stats_grp_t *stats_grps;
	const struct mlx5e_rx_handlers *rx_handlers;
	int	max_tc;
	u8	rq_groups;
};

void mlx5e_create_debugfs(struct mlx5e_priv *priv);
void mlx5e_destroy_debugfs(struct mlx5e_priv *priv);

#ifdef __ETHTOOL_DECLARE_LINK_MODE_MASK
void mlx5e_build_ptys2ethtool_map(void);
#endif

bool mlx5e_check_fragmented_striding_rq_cap(struct mlx5_core_dev *mdev);
bool mlx5e_striding_rq_possible(struct mlx5_core_dev *mdev,
				struct mlx5e_params *params);

int mlx5e_sysfs_create(struct net_device *dev);
void mlx5e_sysfs_remove(struct net_device *dev);

#if defined(CONFIG_MLX5_EN_SPECIAL_SQ) && (defined(HAVE_NDO_SET_TX_MAXRATE) || defined(HAVE_NDO_SET_TX_MAXRATE_EXTENDED))
int mlx5e_rl_init_sysfs(struct net_device *netdev, struct mlx5e_params params);
void mlx5e_rl_remove_sysfs(struct mlx5e_priv *priv);
#endif

#if defined(HAVE_NDO_SETUP_TC_TAKES_TC_SETUP_TYPE) || defined(HAVE_NDO_SETUP_TC_RH_EXTENDED)
int mlx5e_setup_tc_mqprio(struct mlx5e_priv *priv,
			  struct tc_mqprio_qopt *mqprio);
#else
int mlx5e_setup_tc(struct net_device *netdev, u8 tc);
#endif

#ifdef HAVE_NDO_GET_STATS64_RET_VOID
void mlx5e_get_stats(struct net_device *dev, struct rtnl_link_stats64 *stats);
#elif defined(HAVE_NDO_GET_STATS64)
struct rtnl_link_stats64 * mlx5e_get_stats(struct net_device *dev, struct rtnl_link_stats64 *stats);
#else
struct net_device_stats * mlx5e_get_stats(struct net_device *dev);
#endif

void mlx5e_fold_sw_stats64(struct mlx5e_priv *priv, struct rtnl_link_stats64 *s);

void mlx5e_init_l2_addr(struct mlx5e_priv *priv);
int mlx5e_self_test_num(struct mlx5e_priv *priv);
void mlx5e_self_test(struct net_device *ndev, struct ethtool_test *etest,
		     u64 *buf);
void mlx5e_set_rx_mode_work(struct work_struct *work);

#ifdef HAVE_SIOCGHWTSTAMP
int mlx5e_hwstamp_set(struct mlx5e_priv *priv, struct ifreq *ifr);
int mlx5e_hwstamp_get(struct mlx5e_priv *priv, struct ifreq *ifr);
#else
int mlx5e_hwstamp_ioctl(struct mlx5e_priv *priv, struct ifreq *ifr);
#endif
int mlx5e_modify_rx_cqe_compression_locked(struct mlx5e_priv *priv, bool val);
int mlx5e_modify_tx_cqe_compression_locked(struct mlx5e_priv *priv, bool val);

int mlx5e_vlan_rx_add_vid(struct net_device *dev, __always_unused __be16 proto,
			  u16 vid);
int mlx5e_vlan_rx_kill_vid(struct net_device *dev, __always_unused __be16 proto,
			   u16 vid);
void mlx5e_timestamp_init(struct mlx5e_priv *priv);

struct mlx5e_redirect_rqt_param {
	bool is_rss;
	union {
		u32 rqn; /* Direct RQN (Non-RSS) */
		struct {
			u8 hfunc;
			struct mlx5e_channels *channels;
		} rss; /* RSS data */
	};
};

int mlx5e_redirect_rqt(struct mlx5e_priv *priv, u32 rqtn, int sz,
		       struct mlx5e_redirect_rqt_param rrp);
void mlx5e_build_indir_tir_ctx_hash(struct mlx5e_rss_params *rss_params,
				    const struct mlx5e_tirc_config *ttconfig,
				    void *tirc, bool inner);
void mlx5e_modify_tirs_hash(struct mlx5e_priv *priv, void *in);
void mlx5e_sysfs_modify_tirs_hash(struct mlx5e_priv *priv, void *in);
struct mlx5e_tirc_config mlx5e_tirc_get_default_config(enum mlx5e_traffic_types tt);

struct mlx5e_xsk_param;

struct mlx5e_rq_param;
int mlx5e_open_rq(struct mlx5e_channel *c, struct mlx5e_params *params,
		  struct mlx5e_rq_param *param, struct mlx5e_xsk_param *xsk,
		  struct xdp_umem *umem, struct mlx5e_rq *rq);
int mlx5e_wait_for_min_rx_wqes(struct mlx5e_rq *rq, int wait_time);
void mlx5e_deactivate_rq(struct mlx5e_rq *rq);
void mlx5e_close_rq(struct mlx5e_rq *rq);

struct mlx5e_sq_param;
int mlx5e_open_icosq(struct mlx5e_channel *c, struct mlx5e_params *params,
		     struct mlx5e_sq_param *param, struct mlx5e_icosq *sq);
void mlx5e_close_icosq(struct mlx5e_icosq *sq);
#ifdef HAVE_XDP_BUFF
void mlx5e_close_xdpsq(struct mlx5e_xdpsq *sq);
int mlx5e_open_xdpsq(struct mlx5e_channel *c, struct mlx5e_params *params,
                     struct mlx5e_sq_param *param, struct xdp_umem *umem,
#ifdef HAVE_XDP_REDIRECT
                            struct mlx5e_xdpsq *sq,
                            bool is_redirect);
#else
                            struct mlx5e_xdpsq *sq);
#endif
#endif

struct mlx5e_create_cq_param {
	struct napi_struct *napi;
	struct mlx5e_ch_stats *ch_stats;
	int node;
	int ix;
#ifndef HAVE_NAPI_STATE_MISSED
	unsigned long             *ch_flags;
#endif
};

struct mlx5e_cq_param;
int mlx5e_open_cq(struct mlx5e_priv *priv, struct dim_cq_moder moder,
		  struct mlx5e_cq_param *param, struct mlx5e_create_cq_param *ccp,
		  struct mlx5e_cq *cq);
void mlx5e_close_cq(struct mlx5e_cq *cq);
int mlx5e_create_cq(struct mlx5e_cq *cq, struct mlx5e_cq_param *param);
int mlx5e_alloc_cq_common(struct mlx5e_priv *priv,
			  struct mlx5e_cq_param *param,
			  struct mlx5e_cq *cq);
void mlx5e_free_cq(struct mlx5e_cq *cq);
int mlx5e_create_mkey(struct mlx5_core_dev *mdev, u32 pdn,
		      struct mlx5_core_mkey *mkey);

int mlx5e_open_locked(struct net_device *netdev);
int mlx5e_close_locked(struct net_device *netdev);

int mlx5e_open_channels(struct mlx5e_priv *priv,
			struct mlx5e_channels *chs);
void mlx5e_close_channels(struct mlx5e_channels *chs);

/* Function pointer to be used to modify HW or kernel settings while
 * switching channels
 */
typedef int (*mlx5e_fp_preactivate)(struct mlx5e_priv *priv, void *context);
#define MLX5E_DEFINE_PREACTIVATE_WRAPPER_CTX(fn) \
int fn##_ctx(struct mlx5e_priv *priv, void *context) \
{ \
	return fn(priv); \
}
int mlx5e_safe_reopen_channels(struct mlx5e_priv *priv);
int mlx5e_safe_switch_channels(struct mlx5e_priv *priv,
			       struct mlx5e_channels *new_chs,
			       mlx5e_fp_preactivate preactivate,
			       void *context);
int mlx5e_num_channels_changed(struct mlx5e_priv *priv);
int mlx5e_num_channels_changed_ctx(struct mlx5e_priv *priv, void *context);
void mlx5e_activate_priv_channels(struct mlx5e_priv *priv);
void mlx5e_deactivate_priv_channels(struct mlx5e_priv *priv);

void mlx5e_build_default_indir_rqt(u32 *indirection_rqt, int len,
				   int num_channels);

void mlx5e_reset_tx_moderation(struct mlx5e_params *params, u8 cq_period_mode);
void mlx5e_reset_rx_moderation(struct mlx5e_params *params, u8 cq_period_mode);
void mlx5e_set_tx_cq_mode_params(struct mlx5e_params *params, u8 cq_period_mode);
void mlx5e_set_rx_cq_mode_params(struct mlx5e_params *params, u8 cq_period_mode);

void mlx5e_set_rq_type(struct mlx5_core_dev *mdev, struct mlx5e_params *params);
void mlx5e_init_rq_type_params(struct mlx5_core_dev *mdev,
			       struct mlx5e_params *params);
int mlx5e_modify_rq_state(struct mlx5e_rq *rq, int curr_state, int next_state);
void mlx5e_activate_rq(struct mlx5e_rq *rq);
void mlx5e_deactivate_rq(struct mlx5e_rq *rq);
void mlx5e_activate_icosq(struct mlx5e_icosq *icosq);
void mlx5e_deactivate_icosq(struct mlx5e_icosq *icosq);

int mlx5e_modify_sq(struct mlx5_core_dev *mdev, u32 sqn,
		    struct mlx5e_modify_sq_param *p);
void mlx5e_activate_txqsq(struct mlx5e_txqsq *sq);
void mlx5e_deactivate_txqsq(struct mlx5e_txqsq *sq);
void mlx5e_free_txqsq(struct mlx5e_txqsq *sq);
void mlx5e_tx_disable_queue(struct netdev_queue *txq);
int mlx5e_alloc_txqsq_db(struct mlx5e_txqsq *sq, int numa);
void mlx5e_free_txqsq_db(struct mlx5e_txqsq *sq);
struct mlx5e_create_sq_param;
int mlx5e_create_sq_rdy(struct mlx5_core_dev *mdev,
			struct mlx5e_sq_param *param,
			struct mlx5e_create_sq_param *csp,
			u32 *sqn);
void mlx5e_tx_err_cqe_work(struct work_struct *recover_work);

static inline bool mlx5_tx_swp_supported(struct mlx5_core_dev *mdev)
{
	return MLX5_CAP_ETH(mdev, swp) &&
		MLX5_CAP_ETH(mdev, swp_csum) && MLX5_CAP_ETH(mdev, swp_lso);
}

extern const struct ethtool_ops mlx5e_ethtool_ops;

int mlx5e_create_tir(struct mlx5_core_dev *mdev, struct mlx5e_tir *tir,
		     u32 *in);
void mlx5e_destroy_tir(struct mlx5_core_dev *mdev,
		       struct mlx5e_tir *tir);
int mlx5e_create_mdev_resources(struct mlx5_core_dev *mdev);
void mlx5e_destroy_mdev_resources(struct mlx5_core_dev *mdev);
int mlx5e_refresh_tirs(struct mlx5e_priv *priv, bool enable_uc_lb,
		       bool enable_mc_lb);
int mlx5e_modify_tirs_lro(struct mlx5e_priv *priv);
int mlx5e_modify_tirs_lro_ctx(struct mlx5e_priv *priv, void *context);
int mlx5e_update_lro(struct net_device *netdev, bool enable);
void mlx5e_mkey_set_relaxed_ordering(struct mlx5_core_dev *mdev, void *mkc);

/* common netdev helpers */
void mlx5e_create_q_counters(struct mlx5e_priv *priv);
void mlx5e_destroy_q_counters(struct mlx5e_priv *priv);
int mlx5e_open_drop_rq(struct mlx5e_priv *priv,
		       struct mlx5e_rq *drop_rq);
void mlx5e_close_drop_rq(struct mlx5e_rq *drop_rq);

int mlx5e_create_indirect_rqt(struct mlx5e_priv *priv);

int mlx5e_create_indirect_tirs(struct mlx5e_priv *priv, bool inner_ttc);
void mlx5e_destroy_indirect_tirs(struct mlx5e_priv *priv);

int mlx5e_create_direct_rqts(struct mlx5e_priv *priv, struct mlx5e_tir *tirs);
void mlx5e_destroy_direct_rqts(struct mlx5e_priv *priv, struct mlx5e_tir *tirs);
int mlx5e_create_direct_tirs(struct mlx5e_priv *priv, struct mlx5e_tir *tirs);
void mlx5e_destroy_direct_tirs(struct mlx5e_priv *priv, struct mlx5e_tir *tirs);
void mlx5e_destroy_rqt(struct mlx5e_priv *priv, struct mlx5e_rqt *rqt);

int mlx5e_create_tis(struct mlx5_core_dev *mdev, void *in, u32 *tisn);
void mlx5e_destroy_tis(struct mlx5_core_dev *mdev, u32 tisn);

int mlx5e_create_tises(struct mlx5e_priv *priv);
void mlx5e_destroy_tises(struct mlx5e_priv *priv);
int mlx5e_update_nic_rx(struct mlx5e_priv *priv);
void mlx5e_update_carrier(struct mlx5e_priv *priv);
int mlx5e_close(struct net_device *netdev);
int mlx5e_open(struct net_device *netdev);
u32 mlx5e_choose_lro_timeout(struct mlx5_core_dev *mdev, u32 wanted_timeout);

void mlx5e_queue_update_stats(struct mlx5e_priv *priv);
int mlx5e_bits_invert(unsigned long a, int size);

int mlx5e_set_dev_port_mtu(struct mlx5e_priv *priv);
int mlx5e_set_dev_port_mtu_ctx(struct mlx5e_priv *priv, void *context);
int mlx5e_change_mtu(struct net_device *netdev, int new_mtu,
		     mlx5e_fp_preactivate preactivate);
#ifdef HAVE_UDP_TUNNEL_NIC_INFO
void mlx5e_vxlan_set_netdev_info(struct mlx5e_priv *priv);
#endif
/* ethtool helpers */
void mlx5e_ethtool_get_drvinfo(struct mlx5e_priv *priv,
			       struct ethtool_drvinfo *drvinfo);
void mlx5e_ethtool_get_strings(struct mlx5e_priv *priv,
			       uint32_t stringset, uint8_t *data);
int mlx5e_ethtool_get_sset_count(struct mlx5e_priv *priv, int sset);
void mlx5e_ethtool_get_ethtool_stats(struct mlx5e_priv *priv,
				     struct ethtool_stats *stats, u64 *data);
void mlx5e_ethtool_get_ringparam(struct mlx5e_priv *priv,
				 struct ethtool_ringparam *param);
int mlx5e_ethtool_set_ringparam(struct mlx5e_priv *priv,
				struct ethtool_ringparam *param);
void mlx5e_ethtool_get_channels(struct mlx5e_priv *priv,
				struct ethtool_channels *ch);
int mlx5e_ethtool_set_channels(struct mlx5e_priv *priv,
			       struct ethtool_channels *ch);
int mlx5e_ethtool_get_coalesce(struct mlx5e_priv *priv,
			       struct ethtool_coalesce *coal);
int mlx5e_ethtool_set_coalesce(struct mlx5e_priv *priv,
			       struct ethtool_coalesce *coal);
#ifdef HAVE_GET_SET_LINK_KSETTINGS
int mlx5e_ethtool_get_link_ksettings(struct mlx5e_priv *priv,
				     struct ethtool_link_ksettings *link_ksettings);
int mlx5e_ethtool_set_link_ksettings(struct mlx5e_priv *priv,
				     const struct ethtool_link_ksettings *link_ksettings);
#endif
#ifdef HAVE_ETHTOOL_GET_SET_SETTINGS
int mlx5e_get_settings(struct net_device *netdev, struct ethtool_cmd *cmd);
int mlx5e_set_settings(struct net_device *netdev, struct ethtool_cmd *cmd);
#endif
#ifdef HAVE_GET_SET_RXFH
#ifdef HAVE_ETH_SS_RSS_HASH_FUNCS
int mlx5e_get_rxfh(struct net_device *netdev, u32 *indir, u8 *key,
                         u8 *hfunc);
#else
int mlx5e_get_rxfh(struct net_device *netdev, u32 *indir, u8 *key);
#endif
#elif defined(HAVE_GET_SET_RXFH_INDIR)
int mlx5e_get_rxfh_indir(struct net_device *netdev, u32 *indir);
#endif

#ifdef HAVE_GET_SET_RXFH
int mlx5e_set_rxfh(struct net_device *dev, const u32 *indir,
#ifdef HAVE_ETH_SS_RSS_HASH_FUNCS
                  const u8 *key, const u8 hfunc);
#else
                  const u8 *key);
#endif
#elif defined(HAVE_GET_SET_RXFH_INDIR)
int mlx5e_set_rxfh_indir(struct net_device *dev, const u32 *indir);
#endif

int mlx5e_get_rxnfc(struct net_device *dev, struct ethtool_rxnfc *info,
		    u32 *rule_locs);
int mlx5e_set_rxnfc(struct net_device *dev, struct ethtool_rxnfc *cmd);
#ifdef HAVE_GET_SET_RXFH
u32 mlx5e_ethtool_get_rxfh_key_size(struct mlx5e_priv *priv);
#endif
u32 mlx5e_ethtool_get_rxfh_indir_size(struct mlx5e_priv *priv);
int mlx5e_ethtool_get_ts_info(struct mlx5e_priv *priv,
			      struct ethtool_ts_info *info);
int mlx5e_ethtool_flash_device(struct mlx5e_priv *priv,
			       struct ethtool_flash *flash);
#ifdef HAVE_TC_SETUP_CB_EGDEV_REGISTER
#ifndef HAVE_TC_BLOCK_OFFLOAD
int mlx5e_setup_tc(struct net_device *dev, enum tc_setup_type type,
		   void *type_data);
#endif
#endif
void mlx5e_ethtool_get_pauseparam(struct mlx5e_priv *priv,
				  struct ethtool_pauseparam *pauseparam);
int mlx5e_ethtool_set_pauseparam(struct mlx5e_priv *priv,
				 struct ethtool_pauseparam *pauseparam);

/* mlx5e generic netdev management API */
static inline unsigned int
mlx5e_calc_max_nch(struct mlx5e_priv *priv, const struct mlx5e_profile *profile)
{
	return priv->netdev->num_rx_queues / max_t(u8, profile->rq_groups, 1);
}

int mlx5e_netdev_init(struct net_device *netdev,
		      struct mlx5e_priv *priv,
		      struct mlx5_core_dev *mdev);
void mlx5e_netdev_cleanup(struct net_device *netdev, struct mlx5e_priv *priv);
struct net_device *
mlx5e_create_netdev(struct mlx5_core_dev *mdev, unsigned int txqs, unsigned int rxqs);
int mlx5e_attach_netdev(struct mlx5e_priv *priv);
void mlx5e_detach_netdev(struct mlx5e_priv *priv);
void mlx5e_destroy_netdev(struct mlx5e_priv *priv);
int mlx5e_netdev_change_profile(struct mlx5e_priv *priv,
				const struct mlx5e_profile *new_profile, void *new_ppriv);
void mlx5e_netdev_attach_nic_profile(struct mlx5e_priv *priv);
void mlx5e_set_netdev_mtu_boundaries(struct mlx5e_priv *priv);
void mlx5e_build_nic_params(struct mlx5e_priv *priv, struct mlx5e_xsk *xsk, u16 mtu);
void mlx5e_build_rq_params(struct mlx5_core_dev *mdev,
			   struct mlx5e_params *params);
void mlx5e_build_txq_maps(struct mlx5e_priv *priv);
void mlx5e_build_rss_params(struct mlx5e_rss_params *rss_params,
			    u16 num_channels);

int mlx5e_get_dump_flag(struct net_device *netdev, struct ethtool_dump *dump);
int mlx5e_get_dump_data(struct net_device *netdev, struct ethtool_dump *dump,
			void *buffer);
int mlx5e_set_dump(struct net_device *dev, struct ethtool_dump *dump);

static inline bool mlx5e_dropless_rq_supported(struct mlx5_core_dev *mdev)
{
	return (MLX5_CAP_GEN(mdev, rq_delay_drop) &&
		MLX5_CAP_GEN(mdev, general_notification_event));
}

void mlx5e_rx_dim_work(struct work_struct *work);
void mlx5e_tx_dim_work(struct work_struct *work);

#ifdef HAVE_GET_SET_LINK_KSETTINGS
int mlx5e_get_link_ksettings(struct net_device *netdev,
			     struct ethtool_link_ksettings *link_ksettings);
int mlx5e_set_link_ksettings(struct net_device *netdev,
			     const struct ethtool_link_ksettings *link_ksettings);
#endif

#if defined(HAVE_NDO_UDP_TUNNEL_ADD) || defined(HAVE_NDO_UDP_TUNNEL_ADD_EXTENDED)
void mlx5e_add_vxlan_port(struct net_device *netdev, struct udp_tunnel_info *ti);
void mlx5e_del_vxlan_port(struct net_device *netdev, struct udp_tunnel_info *ti);
#elif defined(HAVE_NDO_ADD_VXLAN_PORT)
void mlx5e_add_vxlan_port(struct net_device *netdev, sa_family_t sa_family, __be16 port);
void mlx5e_del_vxlan_port(struct net_device *netdev, sa_family_t sa_family, __be16 port);
#endif

netdev_features_t mlx5e_features_check(struct sk_buff *skb,
				       struct net_device *netdev,
				       netdev_features_t features);

#ifdef HAVE_NETDEV_FEATURES_T
netdev_features_t mlx5e_features_check(struct sk_buff *skb, struct net_device *netdev,
 				       netdev_features_t features);
#elif defined(HAVE_KERNEL_WITH_VXLAN_SUPPORT_ON) && defined(HAVE_VXLAN_GSO_CHECK)
bool mlx5e_gso_check(struct sk_buff *skb, struct net_device *netdev);
#endif

int mlx5e_set_features(struct net_device *netdev, netdev_features_t features);

#ifdef CONFIG_MLX5_ESWITCH
int mlx5e_set_vf_mac(struct net_device *dev, int vf, u8 *mac);
#ifdef HAVE_VF_TX_RATE_LIMITS
int mlx5e_set_vf_rate(struct net_device *dev, int vf, int min_tx_rate, int max_tx_rate);
#else
int mlx5e_set_vf_rate(struct net_device *dev, int vf, int max_tx_rate);
#endif
#ifdef HAVE_NDO_GET_VF_STATS
int mlx5e_get_vf_config(struct net_device *dev, int vf, struct ifla_vf_info *ivi);
int mlx5e_get_vf_stats(struct net_device *dev, int vf, struct ifla_vf_stats *vf_stats);
#endif
#endif

void mlx5e_build_common_cq_param(struct mlx5e_priv *priv,
				 struct mlx5e_cq_param *param);

void mlx5e_destroy_sq(struct mlx5_core_dev *mdev, u32 sqn);
int mlx5e_create_sq_rdy(struct mlx5_core_dev *mdev,
			struct mlx5e_sq_param *param,
			struct mlx5e_create_sq_param *csp,
			u32 *sqn);
#endif /* __MLX5_EN_H__ */
