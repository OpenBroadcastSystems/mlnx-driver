/*
 * Copyright (c) 2015, Mellanox Technologies. All rights reserved.
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

#include <linux/prefetch.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/tcp.h>
#include <linux/bpf_trace.h>
#ifdef CONFIG_NET_RX_BUSY_POLL
#include <net/busy_poll.h>
#endif
#include <net/ip6_checksum.h>
#ifdef HAVE_NET_PAGE_POOL_H
#include <net/page_pool.h>
#endif
#include "en.h"
#include "en_tc.h"
#include "eswitch.h"
#include "en_rep.h"
#include "ipoib/ipoib.h"
#include "en_accel/ipsec_rxtx.h"
#include "lib/clock.h"

static inline bool mlx5e_rx_hw_stamp(struct hwtstamp_config *config)
{
	return config->rx_filter == HWTSTAMP_FILTER_ALL;
}

static inline void mlx5e_read_cqe_slot(struct mlx5e_cq *cq, u32 cqcc,
				       void *data)
{
	u32 ci = mlx5_cqwq_ctr2ix(&cq->wq, cqcc);

	memcpy(data, mlx5_cqwq_get_wqe(&cq->wq, ci), sizeof(struct mlx5_cqe64));
}

static inline void mlx5e_read_title_slot(struct mlx5e_rq *rq,
					 struct mlx5e_cq *cq, u32 cqcc)
{
	mlx5e_read_cqe_slot(cq, cqcc, &cq->title);
	cq->decmprs_left        = be32_to_cpu(cq->title.byte_cnt);
	cq->decmprs_wqe_counter = be16_to_cpu(cq->title.wqe_counter);
	rq->stats->cqe_compress_blks++;
}

static inline void mlx5e_read_mini_arr_slot(struct mlx5e_cq *cq, u32 cqcc)
{
	mlx5e_read_cqe_slot(cq, cqcc, cq->mini_arr);
	cq->mini_arr_idx = 0;
}

static inline void mlx5e_cqes_update_owner(struct mlx5e_cq *cq, u32 cqcc, int n)
{
	struct mlx5_cqwq *wq = &cq->wq;

	u8  op_own = mlx5_cqwq_get_ctr_wrap_cnt(wq, cqcc) & 1;
	u32 ci     = mlx5_cqwq_ctr2ix(wq, cqcc);
	u32 wq_sz  = mlx5_cqwq_get_size(wq);
	u32 ci_top = min_t(u32, wq_sz, ci + n);

	for (; ci < ci_top; ci++, n--) {
		struct mlx5_cqe64 *cqe = mlx5_cqwq_get_wqe(&cq->wq, ci);

		cqe->op_own = op_own;
	}

	if (unlikely(ci == wq_sz)) {
		op_own = !op_own;
		for (ci = 0; ci < n; ci++) {
			struct mlx5_cqe64 *cqe = mlx5_cqwq_get_wqe(&cq->wq, ci);

			cqe->op_own = op_own;
		}
	}
}

static inline void mlx5e_decompress_cqe(struct mlx5e_rq *rq,
					struct mlx5e_cq *cq, u32 cqcc)
{
	cq->title.byte_cnt     = cq->mini_arr[cq->mini_arr_idx].byte_cnt;
	cq->title.check_sum    = cq->mini_arr[cq->mini_arr_idx].checksum;
	cq->title.op_own      &= 0xf0;
	cq->title.op_own      |= 0x01 & (cqcc >> cq->wq.fbc.log_sz);
	cq->title.wqe_counter  = cpu_to_be16(cq->decmprs_wqe_counter);

	if (rq->wq_type == MLX5_WQ_TYPE_LINKED_LIST_STRIDING_RQ)
		cq->decmprs_wqe_counter +=
			mpwrq_get_cqe_consumed_strides(&cq->title);
	else
		cq->decmprs_wqe_counter =
			mlx5_wq_cyc_ctr2ix(&rq->wqe.wq, cq->decmprs_wqe_counter + 1);
}

static inline void mlx5e_decompress_cqe_no_hash(struct mlx5e_rq *rq,
						struct mlx5e_cq *cq, u32 cqcc)
{
	mlx5e_decompress_cqe(rq, cq, cqcc);
	cq->title.rss_hash_type   = 0;
	cq->title.rss_hash_result = 0;
}

static inline u32 mlx5e_decompress_cqes_cont(struct mlx5e_rq *rq,
					     struct mlx5e_cq *cq,
					     int update_owner_only,
					     int budget_rem)
{
	u32 cqcc = cq->wq.cc + update_owner_only;
	u32 cqe_count;
	u32 i;

	cqe_count = min_t(u32, cq->decmprs_left, budget_rem);

	for (i = update_owner_only; i < cqe_count;
	     i++, cq->mini_arr_idx++, cqcc++) {
		if (cq->mini_arr_idx == MLX5_MINI_CQE_ARRAY_SIZE)
			mlx5e_read_mini_arr_slot(cq, cqcc);

		mlx5e_decompress_cqe_no_hash(rq, cq, cqcc);
		rq->handle_rx_cqe(rq, &cq->title);
	}
	mlx5e_cqes_update_owner(cq, cq->wq.cc, cqcc - cq->wq.cc);
	cq->wq.cc = cqcc;
	cq->decmprs_left -= cqe_count;
	rq->stats->cqe_compress_pkts += cqe_count;

	return cqe_count;
}

static inline u32 mlx5e_decompress_cqes_start(struct mlx5e_rq *rq,
					      struct mlx5e_cq *cq,
					      int budget_rem)
{
	mlx5e_read_title_slot(rq, cq, cq->wq.cc);
	mlx5e_read_mini_arr_slot(cq, cq->wq.cc + 1);
	mlx5e_decompress_cqe(rq, cq, cq->wq.cc);
	rq->handle_rx_cqe(rq, &cq->title);
	cq->mini_arr_idx++;

	return mlx5e_decompress_cqes_cont(rq, cq, 1, budget_rem) - 1;
}

static inline void mlx5e_rx_cache_page_swap(struct mlx5e_page_cache *cache,
					    u32 a, u32 b)
{
	struct mlx5e_dma_info tmp;

	tmp = cache->page_cache[a];
	cache->page_cache[a] = cache->page_cache[b];
	cache->page_cache[b] = tmp;
}

static inline void
mlx5e_rx_cache_reduce_reset_watch(struct mlx5e_page_cache *cache)
{
	struct mlx5e_page_cache_reduce *reduce = &cache->reduce;

	reduce->next_ts = ilog2(cache->sz) == cache->log_min_sz ?
		MAX_JIFFY_OFFSET :
		jiffies + reduce->graceful_period;
	reduce->successive = 0;
}

static inline bool mlx5e_rx_cache_is_empty(struct mlx5e_page_cache *cache)
{
	return cache->head < 0;
}
static inline bool mlx5e_rx_cache_page_busy(struct mlx5e_page_cache *cache,
					    u32 i)
{
#ifdef HAVE_PAGE_REF_COUNT_ADD_SUB_INC
	return page_ref_count(cache->page_cache[i].page) != 1;
#else
	return atomic_read(&cache->page_cache[i].page->_count) != 1;
#endif
}

static inline bool mlx5e_rx_cache_check_reduce(struct mlx5e_rq *rq)
{
	struct mlx5e_page_cache *cache = &rq->page_cache;

	if (unlikely(test_bit(MLX5E_RQ_STATE_CACHE_REDUCE_PENDING, &rq->state)))
		return false;

	if (time_before(jiffies, cache->reduce.next_ts))
		return false;

	if (likely(!mlx5e_rx_cache_is_empty(cache)) &&
	    mlx5e_rx_cache_page_busy(cache, cache->head))
		goto reset_watch;

	if (ilog2(cache->sz) == cache->log_min_sz)
		goto reset_watch;

	/* would like to reduce */
	if (cache->reduce.successive < MLX5E_PAGE_CACHE_REDUCE_SUCCESSIVE_CNT) {
		cache->reduce.successive++;
		return false;
	}

	return true;

reset_watch:
	mlx5e_rx_cache_reduce_reset_watch(cache);
	return false;

}

static inline void mlx5e_rx_cache_may_reduce(struct mlx5e_rq *rq)
{
	struct mlx5e_page_cache *cache = &rq->page_cache;
	struct mlx5e_page_cache_reduce *reduce = &cache->reduce;
	int max_new_head;

	if (!mlx5e_rx_cache_check_reduce(rq))
		return;

	/* do reduce */
	rq->stats->cache_rdc++;
	cache->sz >>= 1;
	max_new_head = (cache->sz >> 1) - 1;
	if (cache->head > max_new_head) {
		u32 npages = cache->head - max_new_head;

		cache->head = max_new_head;
		if (cache->lrs >= cache->head)
			cache->lrs = 0;

		memcpy(reduce->pending, &cache->page_cache[cache->head + 1],
		       npages * sizeof(*reduce->pending));
		reduce->npages = npages;
		set_bit(MLX5E_RQ_STATE_CACHE_REDUCE_PENDING, &rq->state);
	}

	mlx5e_rx_cache_reduce_reset_watch(cache);
}

static inline bool mlx5e_rx_cache_extend(struct mlx5e_rq *rq)
{
	struct mlx5e_page_cache *cache = &rq->page_cache;
	struct mlx5e_page_cache_reduce *reduce = &cache->reduce;

	if (ilog2(cache->sz) == cache->log_max_sz)
		return false;

	rq->stats->cache_ext++;
	cache->sz <<= 1;

	mlx5e_rx_cache_reduce_reset_watch(cache);
	schedule_delayed_work_on(smp_processor_id(), &reduce->reduce_work,
				 reduce->delay);
	return true;
}

static inline bool mlx5e_page_is_reserved(struct page *page)
{
#ifdef HAVE_PAGE_IS_PFMEMALLOC
	return page_is_pfmemalloc(page) || page_to_nid(page) != numa_mem_id();
#else
	return page_to_nid(page) != numa_node_id();
#endif
}

static inline bool mlx5e_rx_cache_put(struct mlx5e_rq *rq,
				      struct mlx5e_dma_info *dma_info)
{
	struct mlx5e_page_cache *cache = &rq->page_cache;
	struct mlx5e_rq_stats *stats = rq->stats;

	if (unlikely(cache->head == cache->sz - 1)) {
		if (!mlx5e_rx_cache_extend(rq)) {
			rq->stats->cache_full++;
			return false;
		}
	}

	if (unlikely(mlx5e_page_is_reserved(dma_info->page))) {
		stats->cache_waive++;
		return false;
	}

	cache->page_cache[++cache->head] = *dma_info;
	return true;
}

static inline bool mlx5e_rx_cache_get(struct mlx5e_rq *rq,
				      struct mlx5e_dma_info *dma_info)
{
	struct mlx5e_page_cache *cache = &rq->page_cache;
	struct mlx5e_rq_stats *stats = rq->stats;

	if (unlikely(mlx5e_rx_cache_is_empty(cache)))
		goto err_no_page;

	mlx5e_rx_cache_page_swap(cache, cache->head, cache->lrs);
	cache->lrs++;
	if (cache->lrs >= cache->head)
		cache->lrs = 0;
	if (mlx5e_rx_cache_page_busy(cache, cache->head))
		goto err_no_page;

	stats->cache_reuse++;
	*dma_info = cache->page_cache[cache->head--];

	return true;

err_no_page:
	cache->reduce.successive = 0;

	return false;
}

static inline int mlx5e_page_alloc_mapped(struct mlx5e_rq *rq,
					  struct mlx5e_dma_info *dma_info)
{
	if (!mlx5e_rx_cache_get(rq, dma_info)) {
#ifdef HAVE_NET_PAGE_POOL_H
		dma_info->page = page_pool_dev_alloc_pages(rq->page_pool);
#else
		dma_info->page = dev_alloc_page();
#endif
		if (unlikely(!dma_info->page))
			return -ENOMEM;
		rq->stats->cache_alloc++;
	}

	dma_info->addr = dma_map_page(rq->pdev, dma_info->page, 0,
				      PAGE_SIZE, rq->buff.map_dir);
	if (unlikely(dma_mapping_error(rq->pdev, dma_info->addr))) {
		put_page(dma_info->page);
		dma_info->page = NULL;
		return -ENOMEM;
	}

	return 0;
}

static void mlx5e_page_dma_unmap(struct mlx5e_rq *rq,
					struct mlx5e_dma_info *dma_info)
{
	dma_unmap_page(rq->pdev, dma_info->addr, PAGE_SIZE, rq->buff.map_dir);
}

void mlx5e_page_release(struct mlx5e_rq *rq, struct mlx5e_dma_info *dma_info,
			bool recycle)
{
	mlx5e_page_dma_unmap(rq, dma_info);
#ifdef HAVE_NET_PAGE_POOL_H
	if (likely(recycle)) {
		if (mlx5e_rx_cache_put(rq, dma_info))
			return;

		page_pool_recycle_direct(rq->page_pool, dma_info->page);
	} else {
		put_page(dma_info->page);
	}
#else
	if (likely(recycle) && mlx5e_rx_cache_put(rq, dma_info))
		return;

	put_page(dma_info->page);

#endif
}

static inline int mlx5e_get_rx_frag(struct mlx5e_rq *rq,
				    struct mlx5e_wqe_frag_info *frag)
{
	int err = 0;

	if (!frag->offset)
		/* On first frag (offset == 0), replenish page (dma_info actually).
		 * Other frags that point to the same dma_info (with a different
		 * offset) should just use the new one without replenishing again
		 * by themselves.
		 */
		err = mlx5e_page_alloc_mapped(rq, frag->di);

	return err;
}

static inline void mlx5e_put_rx_frag(struct mlx5e_rq *rq,
				     struct mlx5e_wqe_frag_info *frag,
				     bool recycle)
{
	if (frag->last_in_page)
		mlx5e_page_release(rq, frag->di, recycle);
}

static inline struct mlx5e_wqe_frag_info *get_frag(struct mlx5e_rq *rq, u16 ix)
{
	return &rq->wqe.frags[ix << rq->wqe.info.log_num_frags];
}

static int mlx5e_alloc_rx_wqe(struct mlx5e_rq *rq, struct mlx5e_rx_wqe_cyc *wqe,
			      u16 ix)
{
	struct mlx5e_wqe_frag_info *frag = get_frag(rq, ix);
	int err;
	int i;

	for (i = 0; i < rq->wqe.info.num_frags; i++, frag++) {
		err = mlx5e_get_rx_frag(rq, frag);
		if (unlikely(err))
			goto free_frags;

		wqe->data[i].addr = cpu_to_be64(frag->di->addr +
						frag->offset + rq->buff.headroom);
	}

	return 0;

free_frags:
	while (--i >= 0)
		mlx5e_put_rx_frag(rq, --frag, true);

	return err;
}

static inline void mlx5e_free_rx_wqe(struct mlx5e_rq *rq,
				     struct mlx5e_wqe_frag_info *wi,
				     bool recycle)
{
	int i;

	for (i = 0; i < rq->wqe.info.num_frags; i++, wi++)
		mlx5e_put_rx_frag(rq, wi, recycle);
}

void mlx5e_dealloc_rx_wqe(struct mlx5e_rq *rq, u16 ix)
{
	struct mlx5e_wqe_frag_info *wi = get_frag(rq, ix);

	mlx5e_free_rx_wqe(rq, wi, false);
}

static int mlx5e_alloc_rx_wqes(struct mlx5e_rq *rq, u16 ix, u8 wqe_bulk)
{
	struct mlx5_wq_cyc *wq = &rq->wqe.wq;
	int err;
	int i;

	for (i = 0; i < wqe_bulk; i++) {
		struct mlx5e_rx_wqe_cyc *wqe = mlx5_wq_cyc_get_wqe(wq, ix + i);

		err = mlx5e_alloc_rx_wqe(rq, wqe, ix + i);
		if (unlikely(err))
			goto free_wqes;
	}

	return 0;

free_wqes:
	while (--i >= 0)
		mlx5e_dealloc_rx_wqe(rq, ix + i);

	return err;
}

static inline void
mlx5e_add_skb_frag(struct mlx5e_rq *rq, struct sk_buff *skb,
		   struct mlx5e_dma_info *di, u32 frag_offset, u32 len,
		   unsigned int truesize)
{
	dma_sync_single_for_cpu(rq->pdev,
				di->addr + frag_offset,
				len, DMA_FROM_DEVICE);
#ifdef HAVE_PAGE_REF_COUNT_ADD_SUB_INC
	page_ref_inc(di->page);
#else
	atomic_inc(&di->page->_count);
#endif
	skb_add_rx_frag(skb, skb_shinfo(skb)->nr_frags,
			di->page, frag_offset, len, truesize);
}

static inline void
mlx5e_copy_skb_header(struct device *pdev, struct sk_buff *skb,
		      struct mlx5e_dma_info *dma_info,
		      int offset_from, int offset_to, u32 headlen)
{
	const void *from = page_address(dma_info->page) + offset_from;
	/* Aligning len to sizeof(long) optimizes memcpy performance */
	unsigned int len = ALIGN(headlen, sizeof(long));

	dma_sync_single_for_cpu(pdev, dma_info->addr + offset_from, len,
				DMA_FROM_DEVICE);
	skb_copy_to_linear_data_offset(skb, offset_to, from, len);
}

static inline void
mlx5e_copy_skb_header_mpwqe(struct device *pdev,
			    struct sk_buff *skb,
			    struct mlx5e_dma_info *dma_info,
			    u32 offset, u32 headlen)
{
	u16 headlen_pg = min_t(u32, headlen, PAGE_SIZE - offset);

	mlx5e_copy_skb_header(pdev, skb, dma_info, offset, 0, headlen_pg);

	if (unlikely(offset + headlen > PAGE_SIZE)) {
		dma_info++;
		mlx5e_copy_skb_header(pdev, skb, dma_info, 0, headlen_pg,
				      headlen - headlen_pg);
	}
}

void mlx5e_free_rx_mpwqe(struct mlx5e_rq *rq, struct mlx5e_mpw_info *wi,
			 bool recycle)
{
#ifdef HAVE_NETDEV_BPF
	const bool no_xdp_xmit =
		bitmap_empty(wi->xdp_xmit_bitmap, MLX5_MPWRQ_PAGES_PER_WQE);
	struct mlx5e_dma_info *dma_info = wi->umr.dma_info;
	int i;

	for (i = 0; i < MLX5_MPWRQ_PAGES_PER_WQE; i++)
		if (no_xdp_xmit || !test_bit(i, wi->xdp_xmit_bitmap))
			mlx5e_page_release(rq, &dma_info[i], recycle);
#else
	struct mlx5e_dma_info *dma_info = &wi->umr.dma_info[0];
	int i;

	for (i = 0; i < MLX5_MPWRQ_PAGES_PER_WQE; i++, dma_info++)
		mlx5e_page_release(rq, dma_info, true);
#endif
}

static void mlx5e_post_rx_mpwqe(struct mlx5e_rq *rq)
{
	struct mlx5_wq_ll *wq = &rq->mpwqe.wq;
	struct mlx5e_rx_wqe_ll *wqe = mlx5_wq_ll_get_wqe(wq, wq->head);

	rq->mpwqe.umr_in_progress = false;

	mlx5_wq_ll_push(wq, be16_to_cpu(wqe->next.next_wqe_index));

	/* ensure wqes are visible to device before updating doorbell record */
#ifdef dma_wmb
	dma_wmb();
#else
	wmb();
#endif

	mlx5_wq_ll_update_db_record(wq);

	mlx5e_rx_cache_may_reduce(rq);
}

static inline u16 mlx5e_icosq_wrap_cnt(struct mlx5e_icosq *sq)
{
	return sq->pc >> MLX5E_PARAMS_MINIMUM_LOG_SQ_SIZE;
}

static inline void mlx5e_fill_icosq_frag_edge(struct mlx5e_icosq *sq,
					      struct mlx5_wq_cyc *wq,
					      u16 pi, u16 nnops)
{
	struct mlx5e_sq_wqe_info *edge_wi, *wi = &sq->db.ico_wqe[pi];

	edge_wi = wi + nnops;

	/* fill sq frag edge with nops to avoid wqe wrapping two pages */
	for (; wi < edge_wi; wi++) {
		wi->opcode = MLX5_OPCODE_NOP;
		mlx5e_post_nop(wq, sq->sqn, &sq->pc);
	}
}

static int mlx5e_alloc_rx_mpwqe(struct mlx5e_rq *rq, u16 ix)
{
	struct mlx5e_mpw_info *wi = &rq->mpwqe.info[ix];
	struct mlx5e_dma_info *dma_info = &wi->umr.dma_info[0];
	struct mlx5e_icosq *sq = &rq->channel->icosq;
	struct mlx5_wq_cyc *wq = &sq->wq;
	struct mlx5e_umr_wqe *umr_wqe;
	u16 xlt_offset = ix << (MLX5E_LOG_ALIGNED_MPWQE_PPW - 1);
	u16 pi, contig_wqebbs_room;
	int err;
	int i;

	pi = mlx5_wq_cyc_ctr2ix(wq, sq->pc);
	contig_wqebbs_room = mlx5_wq_cyc_get_contig_wqebbs(wq, pi);
	if (unlikely(contig_wqebbs_room < MLX5E_UMR_WQEBBS)) {
		mlx5e_fill_icosq_frag_edge(sq, wq, pi, contig_wqebbs_room);
		pi = mlx5_wq_cyc_ctr2ix(wq, sq->pc);
	}

	umr_wqe = mlx5_wq_cyc_get_wqe(wq, pi);
	if (unlikely(mlx5e_icosq_wrap_cnt(sq) < 2))
		memcpy(umr_wqe, &rq->mpwqe.umr_wqe,
		       offsetof(struct mlx5e_umr_wqe, inline_mtts));

	for (i = 0; i < MLX5_MPWRQ_PAGES_PER_WQE; i++, dma_info++) {
		err = mlx5e_page_alloc_mapped(rq, dma_info);
		if (unlikely(err))
			goto err_unmap;
		umr_wqe->inline_mtts[i].ptag = cpu_to_be64(dma_info->addr | MLX5_EN_WR);
	}

#ifdef HAVE_NETDEV_BPF
	bitmap_zero(wi->xdp_xmit_bitmap, MLX5_MPWRQ_PAGES_PER_WQE);
#endif
	wi->consumed_strides = 0;

	rq->mpwqe.umr_in_progress = true;

	umr_wqe->ctrl.opmod_idx_opcode =
		cpu_to_be32((sq->pc << MLX5_WQE_CTRL_WQE_INDEX_SHIFT) |
			    MLX5_OPCODE_UMR);
	umr_wqe->uctrl.xlt_offset = cpu_to_be16(xlt_offset);

	sq->db.ico_wqe[pi].opcode = MLX5_OPCODE_UMR;
	sq->pc += MLX5E_UMR_WQEBBS;
	mlx5e_notify_hw(&sq->wq, sq->pc, sq->uar_map, &umr_wqe->ctrl);

	return 0;

err_unmap:
	while (--i >= 0) {
		dma_info--;
		mlx5e_page_release(rq, dma_info, true);
	}
	rq->stats->buff_alloc_err++;

	return err;
}

void mlx5e_dealloc_rx_mpwqe(struct mlx5e_rq *rq, u16 ix)
{
	struct mlx5e_mpw_info *wi = &rq->mpwqe.info[ix];

	mlx5e_free_rx_mpwqe(rq, wi, false);
}

bool mlx5e_post_rx_wqes(struct mlx5e_rq *rq)
{
	struct mlx5_wq_cyc *wq = &rq->wqe.wq;
	u8 wqe_bulk;
	int err;

	if (unlikely(!test_bit(MLX5E_RQ_STATE_ENABLED, &rq->state)))
		return false;

	wqe_bulk = rq->wqe.info.wqe_bulk;

	if (mlx5_wq_cyc_missing(wq) < wqe_bulk)
		return false;

	do {
		u16 head = mlx5_wq_cyc_get_head(wq);

		err = mlx5e_alloc_rx_wqes(rq, head, wqe_bulk);
		if (unlikely(err)) {
			rq->stats->buff_alloc_err++;
			break;
		}

		mlx5_wq_cyc_push_n(wq, wqe_bulk);
	} while (mlx5_wq_cyc_missing(wq) >= wqe_bulk);

	/* ensure wqes are visible to device before updating doorbell record */
#ifdef dma_wmb
	dma_wmb();
#else
	wmb();
#endif

	mlx5_wq_cyc_update_db_record(wq);

	mlx5e_rx_cache_may_reduce(rq);

	return !!err;
}

static inline void mlx5e_poll_ico_single_cqe(struct mlx5e_cq *cq,
					     struct mlx5e_icosq *sq,
					     struct mlx5e_rq *rq,
					     struct mlx5_cqe64 *cqe)
{
	struct mlx5_wq_cyc *wq = &sq->wq;
	u16 ci = mlx5_wq_cyc_ctr2ix(wq, be16_to_cpu(cqe->wqe_counter));
	struct mlx5e_sq_wqe_info *icowi = &sq->db.ico_wqe[ci];

	mlx5_cqwq_pop(&cq->wq);

	if (unlikely((cqe->op_own >> 4) != MLX5_CQE_REQ)) {
		netdev_WARN_ONCE(cq->channel->netdev,
				 "Bad OP in ICOSQ CQE: 0x%x\n", cqe->op_own);
		return;
	}

	if (likely(icowi->opcode == MLX5_OPCODE_UMR)) {
		mlx5e_post_rx_mpwqe(rq);
		return;
	}

	if (unlikely(icowi->opcode != MLX5_OPCODE_NOP))
		netdev_WARN_ONCE(cq->channel->netdev,
				 "Bad OPCODE in ICOSQ WQE info: 0x%x\n", icowi->opcode);
}

static void mlx5e_poll_ico_cq(struct mlx5e_cq *cq, struct mlx5e_rq *rq)
{
	struct mlx5e_icosq *sq = container_of(cq, struct mlx5e_icosq, cq);
	struct mlx5_cqe64 *cqe;

	if (unlikely(!test_bit(MLX5E_SQ_STATE_ENABLED, &sq->state)))
		return;

	cqe = mlx5_cqwq_get_cqe(&cq->wq);
	if (likely(!cqe))
		return;

	/* by design, there's only a single cqe */
	mlx5e_poll_ico_single_cqe(cq, sq, rq, cqe);

	mlx5_cqwq_update_db_record(&cq->wq);
}

bool mlx5e_post_rx_mpwqes(struct mlx5e_rq *rq)
{
	struct mlx5_wq_ll *wq = &rq->mpwqe.wq;

	if (unlikely(!test_bit(MLX5E_RQ_STATE_ENABLED, &rq->state)))
		return false;

	mlx5e_poll_ico_cq(&rq->channel->icosq.cq, rq);

	if (mlx5_wq_ll_is_full(wq))
		return false;

	if (!rq->mpwqe.umr_in_progress)
		mlx5e_alloc_rx_mpwqe(rq, wq->head);

	return false;
}

static void mlx5e_lro_update_tcp_hdr(struct mlx5_cqe64 *cqe, struct tcphdr *tcp)
{
	u8 l4_hdr_type = get_cqe_l4_hdr_type(cqe);
	u8 tcp_ack     = (l4_hdr_type == CQE_L4_HDR_TYPE_TCP_ACK_NO_DATA) ||
			 (l4_hdr_type == CQE_L4_HDR_TYPE_TCP_ACK_AND_DATA);

	tcp->check                      = 0;
	tcp->psh                        = get_cqe_lro_tcppsh(cqe);

	if (tcp_ack) {
		tcp->ack                = 1;
		tcp->ack_seq            = cqe->lro_ack_seq_num;
		tcp->window             = cqe->lro_tcp_win;
	}
}

static void mlx5e_lro_update_hdr(struct sk_buff *skb, struct mlx5_cqe64 *cqe,
				 u32 cqe_bcnt)
{
	struct ethhdr	*eth = (struct ethhdr *)(skb->data);
	struct tcphdr	*tcp;
	int network_depth = 0;
	__wsum check;
	__be16 proto;
	u16 tot_len;
	void *ip_p;

	proto = __vlan_get_protocol(skb, eth->h_proto, &network_depth);

	tot_len = cqe_bcnt - network_depth;
	ip_p = skb->data + network_depth;

	if (proto == htons(ETH_P_IP)) {
		struct iphdr *ipv4 = ip_p;

		tcp = ip_p + sizeof(struct iphdr);
		skb_shinfo(skb)->gso_type = SKB_GSO_TCPV4;

		ipv4->ttl               = cqe->lro_min_ttl;
		ipv4->tot_len           = cpu_to_be16(tot_len);
		ipv4->check             = 0;
		ipv4->check             = ip_fast_csum((unsigned char *)ipv4,
						       ipv4->ihl);

		mlx5e_lro_update_tcp_hdr(cqe, tcp);
		check = csum_partial(tcp, tcp->doff * 4,
				     csum_unfold((__force __sum16)cqe->check_sum));
		/* Almost done, don't forget the pseudo header */
		tcp->check = csum_tcpudp_magic(ipv4->saddr, ipv4->daddr,
					       tot_len - sizeof(struct iphdr),
					       IPPROTO_TCP, check);
	} else {
		u16 payload_len = tot_len - sizeof(struct ipv6hdr);
		struct ipv6hdr *ipv6 = ip_p;

		tcp = ip_p + sizeof(struct ipv6hdr);
		skb_shinfo(skb)->gso_type = SKB_GSO_TCPV6;

		ipv6->hop_limit         = cqe->lro_min_ttl;
		ipv6->payload_len       = cpu_to_be16(payload_len);

		mlx5e_lro_update_tcp_hdr(cqe, tcp);
		check = csum_partial(tcp, tcp->doff * 4,
				     csum_unfold((__force __sum16)cqe->check_sum));
		/* Almost done, don't forget the pseudo header */
		tcp->check = csum_ipv6_magic(&ipv6->saddr, &ipv6->daddr, payload_len,
					     IPPROTO_TCP, check);
	}
}

#ifdef HAVE_NETIF_F_RXHASH
static inline void mlx5e_skb_set_hash(struct mlx5_cqe64 *cqe,
				      struct sk_buff *skb)
{
#ifdef HAVE_SKB_SET_HASH
	u8 cht = cqe->rss_hash_type;
	int ht = (cht & CQE_RSS_HTYPE_L4) ? PKT_HASH_TYPE_L4 :
		 (cht & CQE_RSS_HTYPE_IP) ? PKT_HASH_TYPE_L3 :
					    PKT_HASH_TYPE_NONE;
	skb_set_hash(skb, be32_to_cpu(cqe->rss_hash_result), ht);
#else
	skb->rxhash = be32_to_cpu(cqe->rss_hash_result);
#endif
}
#endif

static inline bool is_last_ethertype_ip(struct sk_buff *skb, int *network_depth)
{
	__be16 ethertype = ((struct ethhdr *)skb->data)->h_proto;

	ethertype = __vlan_get_protocol(skb, ethertype, network_depth);
	return (ethertype == htons(ETH_P_IP) || ethertype == htons(ETH_P_IPV6));
}

#ifdef HAVE_NETIF_F_RXFCS
static __be32 mlx5e_get_fcs(struct sk_buff *skb)
{
	int last_frag_sz, bytes_in_prev, nr_frags;
	u8 *fcs_p1, *fcs_p2;
	skb_frag_t *last_frag;
	__be32 fcs_bytes;

	if (!skb_is_nonlinear(skb))
		return *(__be32 *)(skb->data + skb->len - ETH_FCS_LEN);

	nr_frags = skb_shinfo(skb)->nr_frags;
	last_frag = &skb_shinfo(skb)->frags[nr_frags - 1];
	last_frag_sz = skb_frag_size(last_frag);

	/* If all FCS data is in last frag */
	if (last_frag_sz >= ETH_FCS_LEN)
		return *(__be32 *)(skb_frag_address(last_frag) +
				   last_frag_sz - ETH_FCS_LEN);

	fcs_p2 = (u8 *)skb_frag_address(last_frag);
	bytes_in_prev = ETH_FCS_LEN - last_frag_sz;

	/* Find where the other part of the FCS is - Linear or another frag */
	if (nr_frags == 1) {
		fcs_p1 = skb_tail_pointer(skb);
	} else {
		skb_frag_t *prev_frag = &skb_shinfo(skb)->frags[nr_frags - 2];

		fcs_p1 = skb_frag_address(prev_frag) +
			    skb_frag_size(prev_frag);
	}
	fcs_p1 -= bytes_in_prev;

	memcpy(&fcs_bytes, fcs_p1, bytes_in_prev);
	memcpy(((u8 *)&fcs_bytes) + bytes_in_prev, fcs_p2, last_frag_sz);

	return fcs_bytes;
}
#endif

static inline void mlx5e_handle_csum(struct net_device *netdev,
				     struct mlx5_cqe64 *cqe,
				     struct mlx5e_rq *rq,
				     struct sk_buff *skb,
				     bool   lro)
{
	struct mlx5e_rq_stats *stats = rq->stats;
	int network_depth = 0;

	if (unlikely(!(netdev->features & NETIF_F_RXCSUM)))
		goto csum_none;

	if (lro) {
		skb->ip_summed = CHECKSUM_UNNECESSARY;
		stats->csum_unnecessary++;
		return;
	}

	if (likely(is_last_ethertype_ip(skb, &network_depth))) {
		skb->ip_summed = CHECKSUM_COMPLETE;
		skb->csum = csum_unfold((__force __sum16)cqe->check_sum);
		if (network_depth > ETH_HLEN)
			/* CQE csum is calculated from the IP header and does
			 * not cover VLAN headers (if present). This will add
			 * the checksum manually.
			 */
			skb->csum = csum_partial(skb->data + ETH_HLEN,
						 network_depth - ETH_HLEN,
						 skb->csum);
#ifdef HAVE_NETIF_F_RXFCS
		if (unlikely(netdev->features & NETIF_F_RXFCS))
			skb->csum = csum_add(skb->csum,
					     (__force __wsum)mlx5e_get_fcs(skb));
#endif
		stats->csum_complete++;
		return;
	}

	if (likely((cqe->hds_ip_ext & CQE_L3_OK) &&
		   (cqe->hds_ip_ext & CQE_L4_OK))) {
		skb->ip_summed = CHECKSUM_UNNECESSARY;
		if (cqe_is_tunneled(cqe)) {
#ifdef HAVE_SK_BUFF_CSUM_LEVEL
			skb->csum_level = 1;
#endif
#ifdef HAVE_SK_BUFF_ENCAPSULATION
			skb->encapsulation = 1;
#endif
			stats->csum_unnecessary_inner++;
			return;
		}
		stats->csum_unnecessary++;
		return;
	}
csum_none:
	skb->ip_summed = CHECKSUM_NONE;
	stats->csum_none++;
}

static inline void mlx5e_build_rx_skb(struct mlx5_cqe64 *cqe,
				      u32 cqe_bcnt,
				      struct mlx5e_rq *rq,
				      struct sk_buff *skb)
{
	u8 lro_num_seg = be32_to_cpu(cqe->srqn) >> 24;
	struct mlx5e_rq_stats *stats = rq->stats;
	struct net_device *netdev = rq->netdev;
#ifdef CONFIG_COMPAT_LRO_ENABLED_IPOIB
	struct mlx5e_priv *priv = netdev_priv(netdev);
	u8 l4_hdr_type;
#endif

	skb->mac_len = ETH_HLEN;
	if (lro_num_seg > 1) {
		mlx5e_lro_update_hdr(skb, cqe, cqe_bcnt);
		skb_shinfo(skb)->gso_size = DIV_ROUND_UP(cqe_bcnt, lro_num_seg);
		/* Subtract one since we already counted this as one
		 * "regular" packet in mlx5e_complete_rx_cqe()
		 */
		stats->packets += lro_num_seg - 1;
		stats->lro_packets++;
		stats->lro_bytes += cqe_bcnt;
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
		/* Flush GRO to avoid OOO packets, since GSO bypasses the
		 * GRO queue. This was fixed in dev_gro_receive() in kernel 4.10
		 */
#ifdef NAPI_GRO_FLUSH_2_PARAMS
		napi_gro_flush(rq->cq.napi, false);
#else
		napi_gro_flush(rq->cq.napi);
#endif
#endif
	}

	if (unlikely(mlx5e_rx_hw_stamp(rq->tstamp)))
		skb_hwtstamps(skb)->hwtstamp =
				mlx5_timecounter_cyc2time(rq->clock, get_cqe_ts(cqe));

	skb_record_rx_queue(skb, rq->ix);

#ifdef HAVE_NETIF_F_RXHASH
	if (likely(netdev->features & NETIF_F_RXHASH))
		mlx5e_skb_set_hash(cqe, skb);
#endif

	if (cqe_has_vlan(cqe)) {
#ifndef HAVE_3_PARAMS_FOR_VLAN_HWACCEL_PUT_TAG
		__vlan_hwaccel_put_tag(skb, be16_to_cpu(cqe->vlan_info));
#else
		__vlan_hwaccel_put_tag(skb, htons(ETH_P_8021Q),
				       be16_to_cpu(cqe->vlan_info));
#endif
		stats->removed_vlan_packets++;
	}

	skb->mark = be32_to_cpu(cqe->sop_drop_qpn) & MLX5E_TC_FLOW_ID_MASK;

#ifdef CONFIG_COMPAT_LRO_ENABLED_IPOIB
	l4_hdr_type = get_cqe_l4_hdr_type(cqe);
	mlx5e_handle_csum(netdev, cqe, rq, skb,
			  !!lro_num_seg ||
			  (IS_SW_LRO(&priv->channels.params) &&
			  (l4_hdr_type != CQE_L4_HDR_TYPE_NONE) &&
			  (l4_hdr_type != CQE_L4_HDR_TYPE_UDP)));
#else
	mlx5e_handle_csum(netdev, cqe, rq, skb, !!lro_num_seg);
#endif

	skb->protocol = eth_type_trans(skb, netdev);
	if (unlikely(mlx5_get_cqe_ft(cqe) ==
		     cpu_to_be32(MLX5_FS_OFFLOAD_FLOW_TAG)))
		skb->protocol = 0xffff;
}

static inline void mlx5e_complete_rx_cqe(struct mlx5e_rq *rq,
					 struct mlx5_cqe64 *cqe,
					 u32 cqe_bcnt,
					 struct sk_buff *skb)
{
	struct mlx5e_rq_stats *stats = rq->stats;
	u8 l4_hdr_type = get_cqe_l4_hdr_type(cqe);

	stats->packets++;
	stats->bytes += cqe_bcnt;
	mlx5e_build_rx_skb(cqe, cqe_bcnt, rq, skb);

	if (l4_hdr_type != CQE_L4_HDR_TYPE_TCP_ACK_NO_DATA) {
		rq->dim_obj.sample.pkt_ctr  = rq->stats->packets;
		rq->dim_obj.sample.byte_ctr = rq->stats->bytes;
	}
}

#ifdef HAVE_NETDEV_BPF
static inline void mlx5e_xmit_xdp_doorbell(struct mlx5e_xdpsq *sq)
{
	struct mlx5_wq_cyc *wq = &sq->wq;
	struct mlx5e_tx_wqe *wqe;
	u16 pi = mlx5_wq_cyc_ctr2ix(wq, sq->pc - 1); /* last pi */

	wqe  = mlx5_wq_cyc_get_wqe(wq, pi);

	mlx5e_notify_hw(wq, sq->pc, sq->uar_map, &wqe->ctrl);
}

static inline bool mlx5e_xmit_xdp_frame(struct mlx5e_rq *rq,
					struct mlx5e_dma_info *di,
#ifdef HAVE_XDP_BUFF_DATA_HARD_START
					const struct xdp_buff *xdp)
#else
					unsigned int data_offset,
					int len)
#endif
{
	struct mlx5e_xdpsq       *sq   = &rq->xdpsq;
	struct mlx5_wq_cyc       *wq   = &sq->wq;
	u16                       pi   = mlx5_wq_cyc_ctr2ix(wq, sq->pc);
	struct mlx5e_tx_wqe      *wqe  = mlx5_wq_cyc_get_wqe(wq, pi);

	struct mlx5_wqe_ctrl_seg *cseg = &wqe->ctrl;
	struct mlx5_wqe_eth_seg  *eseg = &wqe->eth;
	struct mlx5_wqe_data_seg *dseg;
	struct mlx5e_priv *priv = netdev_priv(rq->netdev);

#ifdef HAVE_XDP_BUFF_DATA_HARD_START
	ptrdiff_t data_offset = xdp->data - xdp->data_hard_start;
#endif
	dma_addr_t dma_addr  = di->addr + data_offset;
#ifdef HAVE_XDP_BUFF_DATA_HARD_START
	unsigned int dma_len = xdp->data_end - xdp->data;

	struct mlx5e_rq_stats *stats = rq->stats;

	prefetchw(wqe);

	if (unlikely(dma_len < MLX5E_XDP_MIN_INLINE || rq->hw_mtu < dma_len)) {
		stats->xdp_drop++;
		return false;
	}
#else
	unsigned int dma_len = len - MLX5E_XDP_MIN_INLINE;
	void *data           = page_address(di->page) + data_offset;

	struct mlx5e_rq_stats *stats = rq->stats;
#endif

	if (unlikely(!mlx5e_wqc_has_room_for(wq, sq->cc, sq->pc, 1))) {
		if (sq->db.doorbell) {
			/* SQ is full, ring doorbell */
			mlx5e_xmit_xdp_doorbell(sq);
			sq->db.doorbell = false;
		}
		stats->xdp_tx_full++;
		return false;
	}

	dma_sync_single_for_device(sq->pdev, dma_addr, dma_len, PCI_DMA_TODEVICE);

	cseg->fm_ce_se = 0;

	dseg = (struct mlx5_wqe_data_seg *)eseg + 1;

	/* copy the inline part if required */
	if (sq->min_inline_mode != MLX5_INLINE_MODE_NONE) {
#ifdef HAVE_XDP_BUFF_DATA_HARD_START
		memcpy(eseg->inline_hdr.start, xdp->data, MLX5E_XDP_MIN_INLINE);
#else
		memcpy(eseg->inline_hdr.start, data, MLX5E_XDP_MIN_INLINE);
#endif
		eseg->inline_hdr.sz = cpu_to_be16(MLX5E_XDP_MIN_INLINE);
		dma_len  -= MLX5E_XDP_MIN_INLINE;
		dma_addr += MLX5E_XDP_MIN_INLINE;
		dseg++;
	}

	if (MLX5E_GET_PFLAG(&priv->channels.params, MLX5E_PFLAG_TX_XDP_CSUM))
		eseg->cs_flags = MLX5_ETH_WQE_L3_CSUM | MLX5_ETH_WQE_L4_CSUM;

	/* write the dma part */
	dseg->addr       = cpu_to_be64(dma_addr);
	dseg->byte_count = cpu_to_be32(dma_len);

	cseg->opmod_idx_opcode = cpu_to_be32((sq->pc << 8) | MLX5_OPCODE_SEND);

	/* move page to reference to sq responsibility,
	 * and mark so it's not put back in page-cache.
	 */
	__set_bit(MLX5E_RQ_FLAG_XDP_XMIT, rq->flags); /* non-atomic */
	sq->db.di[pi] = *di;
	sq->pc++;

	sq->db.doorbell = true;

	stats->xdp_tx++;
	return true;
}

/* returns true if packet was consumed by xdp */
#ifdef HAVE_XDP_BUFF_DATA_HARD_START
static inline bool mlx5e_xdp_handle(struct mlx5e_rq *rq,
				    struct mlx5e_dma_info *di,
				    void *va, u16 *rx_headroom, u32 *len)
#else
static inline bool mlx5e_xdp_handle(struct mlx5e_rq *rq,
				    const struct bpf_prog *prog,
				    struct mlx5e_dma_info *di,
				    void *data, u16 len)
#endif
{
#ifdef HAVE_XDP_BUFF_DATA_HARD_START
	struct bpf_prog *prog = READ_ONCE(rq->xdp_prog);
#endif
	struct xdp_buff xdp;
	u32 act;
#ifdef HAVE_XDP_REDIRECT
	int err;
#endif

	if (!prog)
		return false;

#ifdef HAVE_XDP_BUFF_DATA_HARD_START
	xdp.data = va + *rx_headroom;
#ifdef HAVE_XDP_SET_DATA_META_INVALID
	xdp_set_data_meta_invalid(&xdp);
#endif
	xdp.data_end = xdp.data + *len;
	xdp.data_hard_start = va;
#else
	xdp.data = data;
	xdp.data_end = xdp.data + len;
#endif
#ifdef HAVE_NET_XDP_H
	xdp.rxq = &rq->xdp_rxq;
#endif

	act = bpf_prog_run_xdp(prog, &xdp);
	switch (act) {
	case XDP_PASS:
#ifdef HAVE_XDP_BUFF_DATA_HARD_START
		*rx_headroom = xdp.data - xdp.data_hard_start;
		*len = xdp.data_end - xdp.data;
#endif
		return false;
	case XDP_TX:
#if defined(HAVE_TRACE_XDP_EXCEPTION) && !defined(MLX_DISABLE_TRACEPOINTS)
		if (unlikely(!mlx5e_xmit_xdp_frame(rq, di, &xdp)))
			trace_xdp_exception(rq->netdev, prog, act);
#else
#ifdef HAVE_XDP_BUFF_DATA_HARD_START
		mlx5e_xmit_xdp_frame(rq, di, &xdp);
#else
		mlx5e_xmit_xdp_frame(rq, di, MLX5_RX_HEADROOM, len);
#endif
#endif
		return true;
#ifdef HAVE_XDP_REDIRECT
	case XDP_REDIRECT:
		/* When XDP enabled then page-refcnt==1 here */
		err = xdp_do_redirect(rq->netdev, &xdp, prog);
		if (!err) {
			__set_bit(MLX5E_RQ_FLAG_XDP_XMIT, rq->flags);
			rq->xdpsq.db.redirect_flush = true;
			mlx5e_page_dma_unmap(rq, di);
		}
		return true;
#endif
	default:
		bpf_warn_invalid_xdp_action(act);
	case XDP_ABORTED:
#if defined(HAVE_TRACE_XDP_EXCEPTION) && !defined(MLX_DISABLE_TRACEPOINTS)
		trace_xdp_exception(rq->netdev, prog, act);
#endif
	case XDP_DROP:
		rq->stats->xdp_drop++;
		return true;
	}
}
#endif /* HAVE_NETDEV_BPF */

#ifndef HAVE_BUILD_SKB
static inline struct sk_buff *mlx5e_compat_build_skb(struct mlx5e_rq *rq,
						struct page *page,
						u32 cqe_bcnt,
						unsigned int offset)
{
	u16 headlen = min_t(u32, MLX5E_RX_MAX_HEAD, cqe_bcnt);
	u32 frag_size = cqe_bcnt - headlen;
	struct sk_buff *skb;
	void *head_ptr = page_address(page) + offset + rq->buff.headroom;

	skb = netdev_alloc_skb(rq->netdev, headlen + rq->buff.headroom);
	if (unlikely(!skb))
		return NULL;

	if (frag_size) {
		u32 frag_offset = offset + rq->buff.headroom + headlen;
		unsigned int truesize =	SKB_TRUESIZE(frag_size);

		skb_add_rx_frag(skb, skb_shinfo(skb)->nr_frags,
				page, frag_offset,
				frag_size, truesize);
	}

	/* copy header */
	skb_reserve(skb, rq->buff.headroom);
	skb_copy_to_linear_data(skb, head_ptr, headlen);

	/* skb linear part was allocated with headlen and aligned to long */
	skb->tail += headlen;
	skb->len  += headlen;
	return skb;
}
#endif

#ifdef HAVE_BUILD_SKB
static inline
struct sk_buff *mlx5e_build_linear_skb(struct mlx5e_rq *rq, void *va,
				       u32 frag_size, u16 headroom,
				       u32 cqe_bcnt)
{
	struct sk_buff *skb = build_skb(va, frag_size);

	if (unlikely(!skb)) {
		rq->stats->buff_alloc_err++;
		return NULL;
	}

	skb_reserve(skb, headroom);
	skb_put(skb, cqe_bcnt);

	return skb;
}
#endif

struct sk_buff *
mlx5e_skb_from_cqe_linear(struct mlx5e_rq *rq, struct mlx5_cqe64 *cqe,
			  struct mlx5e_wqe_frag_info *wi, u32 cqe_bcnt)
{
	struct mlx5e_dma_info *di = wi->di;
	u16 rx_headroom = rq->buff.headroom;
	struct sk_buff *skb;
	void *va, *data;
#ifdef HAVE_NETDEV_BPF
	bool consumed;
#endif
	u32 frag_size;

	va             = page_address(di->page) + wi->offset;
	data           = va + rx_headroom;
	frag_size      = MLX5_SKB_FRAG_SZ(rx_headroom + cqe_bcnt);

	dma_sync_single_range_for_cpu(rq->pdev, di->addr, wi->offset,
				      frag_size, DMA_FROM_DEVICE);
	prefetchw(va); /* xdp_frame data area */
	prefetch(data);

	if (unlikely((cqe->op_own >> 4) != MLX5_CQE_RESP_SEND)) {
		rq->stats->wqe_err++;
		return NULL;
	}

#ifdef HAVE_NETDEV_BPF
	rcu_read_lock();
#ifdef HAVE_XDP_BUFF_DATA_HARD_START
	consumed = mlx5e_xdp_handle(rq, di, va, &rx_headroom, &cqe_bcnt);
#else
	consumed = mlx5e_xdp_handle(rq, READ_ONCE(rq->xdp_prog), di, data,
				    cqe_bcnt);
#endif
	rcu_read_unlock();
	if (consumed)
		return NULL; /* page/packet was consumed by XDP */
#endif

#ifdef HAVE_BUILD_SKB
	skb = mlx5e_build_linear_skb(rq, va, frag_size, rx_headroom, cqe_bcnt);
#else
	skb = mlx5e_compat_build_skb(rq, di->page, cqe_bcnt, wi->offset);
#endif
	if (unlikely(!skb))
		return NULL;

	/* queue up for recycling/reuse */
#ifndef HAVE_BUILD_SKB
	if (skb_shinfo(skb)->nr_frags)
#endif
#ifdef HAVE_PAGE_REF_COUNT_ADD_SUB_INC
	page_ref_inc(di->page);
#else
	atomic_inc(&di->page->_count);
#endif

	return skb;
}

struct sk_buff *
mlx5e_skb_from_cqe_nonlinear(struct mlx5e_rq *rq, struct mlx5_cqe64 *cqe,
			     struct mlx5e_wqe_frag_info *wi, u32 cqe_bcnt)
{
	struct mlx5e_rq_frag_info *frag_info = &rq->wqe.info.arr[0];
	struct mlx5e_wqe_frag_info *head_wi = wi;
	u16 headlen      = min_t(u32, MLX5E_RX_MAX_HEAD, cqe_bcnt);
	u16 frag_headlen = headlen;
	u16 byte_cnt     = cqe_bcnt - headlen;
	struct sk_buff *skb;

	if (unlikely((cqe->op_own >> 4) != MLX5_CQE_RESP_SEND)) {
		rq->stats->wqe_err++;
		return NULL;
	}

	/* XDP is not supported in this configuration, as incoming packets
	 * might spread among multiple pages.
	 */
#ifdef HAVE_NAPI_ALLOC_SKB
	skb = napi_alloc_skb(rq->cq.napi,
#else
	skb = netdev_alloc_skb_ip_align(rq->netdev,
#endif
			     ALIGN(MLX5E_RX_MAX_HEAD, sizeof(long)));
	if (unlikely(!skb)) {
		rq->stats->buff_alloc_err++;
		return NULL;
	}

	prefetchw(skb->data);

	while (byte_cnt) {
		u16 frag_consumed_bytes =
			min_t(u16, frag_info->frag_size - frag_headlen, byte_cnt);

		mlx5e_add_skb_frag(rq, skb, wi->di, wi->offset + frag_headlen,
				   frag_consumed_bytes, frag_info->frag_stride);
		byte_cnt -= frag_consumed_bytes;
		frag_headlen = 0;
		frag_info++;
		wi++;
	}

	/* copy header */
	mlx5e_copy_skb_header(rq->pdev, skb, head_wi->di, head_wi->offset,
			      0, headlen);
	/* skb linear part was allocated with headlen and aligned to long */
	skb->tail += headlen;
	skb->len  += headlen;

	return skb;
}

void mlx5e_handle_rx_cqe(struct mlx5e_rq *rq, struct mlx5_cqe64 *cqe)
{
#if defined(HAVE_VLAN_GRO_RECEIVE) || defined(HAVE_VLAN_HWACCEL_RX) || defined(CONFIG_COMPAT_LRO_ENABLED_IPOIB)
	struct mlx5e_priv *priv = netdev_priv(rq->netdev);
#endif
	struct mlx5_wq_cyc *wq = &rq->wqe.wq;
	struct mlx5e_wqe_frag_info *wi;
	struct sk_buff *skb;
	u32 cqe_bcnt;
	u16 ci;

	ci       = mlx5_wq_cyc_ctr2ix(wq, be16_to_cpu(cqe->wqe_counter));
	wi       = get_frag(rq, ci);
	cqe_bcnt = be32_to_cpu(cqe->byte_cnt);

	skb = rq->wqe.skb_from_cqe(rq, cqe, wi, cqe_bcnt);
	if (!skb) {
		/* probably for XDP */
		if (__test_and_clear_bit(MLX5E_RQ_FLAG_XDP_XMIT, rq->flags)) {
			/* do not return page to cache,
			 * it will be returned on XDP_TX completion.
			 */
			goto wq_cyc_pop;
		}
		goto free_wqe;
	}

	mlx5e_complete_rx_cqe(rq, cqe, cqe_bcnt, skb);
#ifdef CONFIG_COMPAT_LRO_ENABLED_IPOIB
	if (IS_SW_LRO(&priv->channels.params))
#if defined(HAVE_VLAN_GRO_RECEIVE) || defined(HAVE_VLAN_HWACCEL_RX)
		if (priv->channels.params.vlan_grp && cqe_has_vlan(cqe))
			lro_vlan_hwaccel_receive_skb(&rq->sw_lro->lro_mgr,
						     skb, priv->channels.params.vlan_grp,
						     be16_to_cpu(cqe->vlan_info),
						     NULL);
		else
#endif
		lro_receive_skb(&rq->sw_lro->lro_mgr, skb, NULL);
	else
#endif
#if defined(HAVE_VLAN_GRO_RECEIVE) || defined(HAVE_VLAN_HWACCEL_RX)
                if (priv->channels.params.vlan_grp && cqe_has_vlan(cqe))
#ifdef HAVE_VLAN_GRO_RECEIVE
                        vlan_gro_receive(rq->cq.napi, priv->channels.params.vlan_grp,
                                         be16_to_cpu(cqe->vlan_info),
                                         skb);
#else
                        vlan_hwaccel_receive_skb(skb, priv->channels.params.vlan_grp,
                                        be16_to_cpu(cqe->vlan_info));
#endif
		else
#endif
	napi_gro_receive(rq->cq.napi, skb);

free_wqe:
	mlx5e_free_rx_wqe(rq, wi, true);
wq_cyc_pop:
	mlx5_wq_cyc_pop(wq);
}

#ifdef CONFIG_MLX5_ESWITCH
void mlx5e_handle_rx_cqe_rep(struct mlx5e_rq *rq, struct mlx5_cqe64 *cqe)
{
#if defined(HAVE_SKB_VLAN_POP) || defined(HAVE_VLAN_GRO_RECEIVE) || defined(HAVE_VLAN_HWACCEL_RX)
	struct net_device *netdev = rq->netdev;
	struct mlx5e_priv *priv = netdev_priv(netdev);
#ifdef HAVE_SKB_VLAN_POP
	struct mlx5e_rep_priv *rpriv  = priv->ppriv;
	struct mlx5_eswitch_rep *rep = rpriv->rep;
#endif
#endif
	struct mlx5_wq_cyc *wq = &rq->wqe.wq;
	struct mlx5e_wqe_frag_info *wi;
	struct sk_buff *skb;
	u32 cqe_bcnt;
	u16 ci;

	ci       = mlx5_wq_cyc_ctr2ix(wq, be16_to_cpu(cqe->wqe_counter));
	wi       = get_frag(rq, ci);
	cqe_bcnt = be32_to_cpu(cqe->byte_cnt);

	skb = rq->wqe.skb_from_cqe(rq, cqe, wi, cqe_bcnt);
	if (!skb) {
		/* probably for XDP */
		if (__test_and_clear_bit(MLX5E_RQ_FLAG_XDP_XMIT, rq->flags)) {
			/* do not return page to cache,
			 * it will be returned on XDP_TX completion.
			 */
			goto wq_cyc_pop;
		}
		goto free_wqe;
	}

	mlx5e_complete_rx_cqe(rq, cqe, cqe_bcnt, skb);

#ifdef HAVE_SKB_VLAN_POP
	if (rep->vlan && skb_vlan_tag_present(skb))
		skb_vlan_pop(skb);
#endif

#if defined(HAVE_VLAN_GRO_RECEIVE) || defined(HAVE_VLAN_HWACCEL_RX)
	if (priv->channels.params.vlan_grp && cqe_has_vlan(cqe))
#ifdef HAVE_VLAN_GRO_RECEIVE
		vlan_gro_receive(rq->cq.napi, priv->channels.params.vlan_grp,
				 be16_to_cpu(cqe->vlan_info),
				 skb);
#else
	vlan_hwaccel_receive_skb(skb, priv->channels.params.vlan_grp,
				 be16_to_cpu(cqe->vlan_info));
#endif
	else
#endif
	napi_gro_receive(rq->cq.napi, skb);

free_wqe:
	mlx5e_free_rx_wqe(rq, wi, true);
wq_cyc_pop:
	mlx5_wq_cyc_pop(wq);
}
#endif

struct sk_buff *
mlx5e_skb_from_cqe_mpwrq_nonlinear(struct mlx5e_rq *rq, struct mlx5e_mpw_info *wi,
				   u16 cqe_bcnt, u32 head_offset, u32 page_idx)
{
	u16 headlen = min_t(u16, MLX5E_RX_MAX_HEAD, cqe_bcnt);
	struct mlx5e_dma_info *di = &wi->umr.dma_info[page_idx];
	u32 frag_offset    = head_offset + headlen;
	u32 byte_cnt       = cqe_bcnt - headlen;
	struct mlx5e_dma_info *head_di = di;
	struct sk_buff *skb;

#ifdef HAVE_NAPI_ALLOC_SKB
	skb = napi_alloc_skb(rq->cq.napi,
#else
	skb = netdev_alloc_skb_ip_align(rq->netdev,
#endif
			     ALIGN(MLX5E_RX_MAX_HEAD, sizeof(long)));
	if (unlikely(!skb)) {
		rq->stats->buff_alloc_err++;
		return NULL;
	}

	prefetchw(skb->data);

	if (unlikely(frag_offset >= PAGE_SIZE)) {
		di++;
		frag_offset -= PAGE_SIZE;
	}

	while (byte_cnt) {
		u32 pg_consumed_bytes =
			min_t(u32, PAGE_SIZE - frag_offset, byte_cnt);
		unsigned int truesize =
			ALIGN(pg_consumed_bytes, BIT(rq->mpwqe.log_stride_sz));

		mlx5e_add_skb_frag(rq, skb, di, frag_offset,
				   pg_consumed_bytes, truesize);
		byte_cnt -= pg_consumed_bytes;
		frag_offset = 0;
		di++;
	}
	/* copy header */
	mlx5e_copy_skb_header_mpwqe(rq->pdev, skb, head_di,
				    head_offset, headlen);
	/* skb linear part was allocated with headlen and aligned to long */
	skb->tail += headlen;
	skb->len  += headlen;

	return skb;
}

struct sk_buff *
mlx5e_skb_from_cqe_mpwrq_linear(struct mlx5e_rq *rq, struct mlx5e_mpw_info *wi,
				u16 cqe_bcnt, u32 head_offset, u32 page_idx)
{
	struct mlx5e_dma_info *di = &wi->umr.dma_info[page_idx];
	u16 rx_headroom = rq->buff.headroom;
	u32 cqe_bcnt32 = cqe_bcnt;
	struct sk_buff *skb;
	void *va, *data;
	u32 frag_size;
#ifdef HAVE_NETDEV_BPF
	bool consumed;
#endif

	/* Check packet size. Note LRO doesn't use linear SKB. */
	if (unlikely(cqe_bcnt > rq->hw_mtu)) {
		rq->stats->oversize_pkts_sw_drop++;
		return NULL;
	}

	va             = page_address(di->page) + head_offset;
	data           = va + rx_headroom;
	frag_size      = MLX5_SKB_FRAG_SZ(rx_headroom + cqe_bcnt32);

	dma_sync_single_range_for_cpu(rq->pdev, di->addr, head_offset,
				      frag_size, DMA_FROM_DEVICE);
	prefetch(data);

#ifdef HAVE_NETDEV_BPF
	rcu_read_lock();
#ifdef HAVE_XDP_BUFF_DATA_HARD_START
	consumed = mlx5e_xdp_handle(rq, di, va, &rx_headroom, &cqe_bcnt32);
#else
	consumed = mlx5e_xdp_handle(rq, READ_ONCE(rq->xdp_prog), di, data,
				    cqe_bcnt32);
#endif

	rcu_read_unlock();
	if (consumed) {
		if (__test_and_clear_bit(MLX5E_RQ_FLAG_XDP_XMIT, rq->flags))
			__set_bit(page_idx, wi->xdp_xmit_bitmap); /* non-atomic */
		return NULL; /* page/packet was consumed by XDP */
	}
#endif

#ifdef HAVE_BUILD_SKB
	skb = mlx5e_build_linear_skb(rq, va, frag_size, rx_headroom, cqe_bcnt32);
#else
	skb = mlx5e_compat_build_skb(rq, di->page, cqe_bcnt32, head_offset);
#endif
	if (unlikely(!skb))
		return NULL;

	/* queue up for recycling/reuse */
#ifndef HAVE_BUILD_SKB
	if (skb_shinfo(skb)->nr_frags)
#endif
#ifdef HAVE_PAGE_REF_COUNT_ADD_SUB_INC
	page_ref_inc(di->page);
#else
	atomic_inc(&di->page->_count);
#endif

	return skb;
}

void mlx5e_handle_rx_cqe_mpwrq(struct mlx5e_rq *rq, struct mlx5_cqe64 *cqe)
{
#ifdef CONFIG_COMPAT_LRO_ENABLED_IPOIB
	struct mlx5e_priv *priv = netdev_priv(rq->netdev);
#endif
	u16 cstrides       = mpwrq_get_cqe_consumed_strides(cqe);
	u16 wqe_id         = be16_to_cpu(cqe->wqe_id);
	struct mlx5e_mpw_info *wi = &rq->mpwqe.info[wqe_id];
	u16 stride_ix      = mpwrq_get_cqe_stride_index(cqe);
	u32 wqe_offset     = stride_ix << rq->mpwqe.log_stride_sz;
	u32 head_offset    = wqe_offset & (PAGE_SIZE - 1);
	u32 page_idx       = wqe_offset >> PAGE_SHIFT;
	struct mlx5e_rx_wqe_ll *wqe;
	struct mlx5_wq_ll *wq;
	struct sk_buff *skb;
	u16 cqe_bcnt;

	wi->consumed_strides += cstrides;

	if (unlikely((cqe->op_own >> 4) != MLX5_CQE_RESP_SEND)) {
		rq->stats->wqe_err++;
		goto mpwrq_cqe_out;
	}

	if (unlikely(mpwrq_is_filler_cqe(cqe))) {
		rq->stats->mpwqe_filler++;
		goto mpwrq_cqe_out;
	}

	cqe_bcnt = mpwrq_get_cqe_byte_cnt(cqe);

	skb = rq->mpwqe.skb_from_cqe_mpwrq(rq, wi, cqe_bcnt, head_offset,
					   page_idx);
	if (!skb)
		goto mpwrq_cqe_out;

	mlx5e_complete_rx_cqe(rq, cqe, cqe_bcnt, skb);
#ifdef CONFIG_COMPAT_LRO_ENABLED_IPOIB
	if (IS_SW_LRO(&priv->channels.params))
#if defined(HAVE_VLAN_GRO_RECEIVE) || defined(HAVE_VLAN_HWACCEL_RX)
		if (priv->channels.params.vlan_grp && cqe_has_vlan(cqe))
			lro_vlan_hwaccel_receive_skb(&rq->sw_lro->lro_mgr,
						     skb, priv->channels.params.vlan_grp,
						     be16_to_cpu(cqe->vlan_info),
						     NULL);
		else
#endif
		lro_receive_skb(&rq->sw_lro->lro_mgr, skb, NULL);
	else
#endif
#if defined(HAVE_VLAN_GRO_RECEIVE) || defined(HAVE_VLAN_HWACCEL_RX)
                if (priv->channels.params.vlan_grp && cqe_has_vlan(cqe))
#ifdef HAVE_VLAN_GRO_RECEIVE
                        vlan_gro_receive(rq->cq.napi, priv->channels.params.vlan_grp,
                                         be16_to_cpu(cqe->vlan_info),
                                         skb);
#else
                        vlan_hwaccel_receive_skb(skb, priv->channels.params.vlan_grp,
                                        be16_to_cpu(cqe->vlan_info));
#endif
		else
#endif
	napi_gro_receive(rq->cq.napi, skb);

mpwrq_cqe_out:
	if (likely(wi->consumed_strides < rq->mpwqe.num_strides))
		return;

	wq  = &rq->mpwqe.wq;
	wqe = mlx5_wq_ll_get_wqe(wq, wqe_id);
	mlx5e_free_rx_mpwqe(rq, wi, true);
	mlx5_wq_ll_pop(wq, cqe->wqe_id, &wqe->next.next_wqe_index);
}

int mlx5e_poll_rx_cq(struct mlx5e_cq *cq, int budget)
{
	struct mlx5e_rq *rq = container_of(cq, struct mlx5e_rq, cq);
#ifdef HAVE_NETDEV_BPF
	struct mlx5e_xdpsq *xdpsq;
#endif
	struct mlx5_cqe64 *cqe;
	int work_done = 0;
#ifdef CONFIG_COMPAT_LRO_ENABLED_IPOIB
	struct mlx5e_priv *priv;
#ifdef CONFIG_MLX5_CORE_IPOIB
	if (MLX5_CAP_GEN(cq->mdev, port_type) != MLX5_CAP_PORT_TYPE_ETH)
		priv = mlx5i_epriv(rq->netdev);
	else
#endif
		priv = netdev_priv(rq->netdev);
#endif

	if (unlikely(!test_bit(MLX5E_RQ_STATE_ENABLED, &rq->state)))
		return 0;

	if (cq->decmprs_left)
		work_done += mlx5e_decompress_cqes_cont(rq, cq, 0, budget);

	cqe = mlx5_cqwq_get_cqe(&cq->wq);
	if (!cqe)
		return 0;

#ifdef HAVE_NETDEV_BPF
	xdpsq = &rq->xdpsq;
#endif

	do {
		if (mlx5_get_cqe_format(cqe) == MLX5_COMPRESSED) {
			work_done +=
				mlx5e_decompress_cqes_start(rq, cq,
							    budget - work_done);
			continue;
		}

		mlx5_cqwq_pop(&cq->wq);

		rq->handle_rx_cqe(rq, cqe);
	} while ((++work_done < budget) && (cqe = mlx5_cqwq_get_cqe(&cq->wq)));

#ifdef HAVE_NETDEV_BPF
	if (xdpsq->db.doorbell) {
		mlx5e_xmit_xdp_doorbell(xdpsq);
		xdpsq->db.doorbell = false;
	}

#ifdef HAVE_XDP_REDIRECT
	if (xdpsq->db.redirect_flush) {
		xdp_do_flush_map();
		xdpsq->db.redirect_flush = false;
	}
#endif

#endif

	mlx5_cqwq_update_db_record(&cq->wq);

	/* ensure cq space is freed before enabling more cqes */
	wmb();

#ifdef CONFIG_COMPAT_LRO_ENABLED_IPOIB
	if (IS_SW_LRO(&priv->channels.params))
		lro_flush_all(&rq->sw_lro->lro_mgr);
#endif

	return work_done;
}

bool mlx5e_poll_xdpsq_cq(struct mlx5e_cq *cq)
{
	struct mlx5e_xdpsq *sq;
	struct mlx5_cqe64 *cqe;
	struct mlx5e_rq *rq;
	u16 sqcc;
	int i;

	sq = container_of(cq, struct mlx5e_xdpsq, cq);

	if (unlikely(!test_bit(MLX5E_SQ_STATE_ENABLED, &sq->state)))
		return false;

	cqe = mlx5_cqwq_get_cqe(&cq->wq);
	if (!cqe)
		return false;

	rq = container_of(sq, struct mlx5e_rq, xdpsq);

	/* sq->cc must be updated only after mlx5_cqwq_update_db_record(),
	 * otherwise a cq overrun may occur
	 */
	sqcc = sq->cc;

	i = 0;
	do {
		u16 wqe_counter;
		bool last_wqe;

		mlx5_cqwq_pop(&cq->wq);

		wqe_counter = be16_to_cpu(cqe->wqe_counter);

		do {
			struct mlx5e_dma_info *di;
			u16 ci;

			last_wqe = (sqcc == wqe_counter);

			ci = mlx5_wq_cyc_ctr2ix(&sq->wq, sqcc);
			di = &sq->db.di[ci];

			sqcc++;
			/* Recycle RX page */
			mlx5e_page_release(rq, di, true);
		} while (!last_wqe);
	} while ((++i < MLX5E_TX_CQ_POLL_BUDGET) && (cqe = mlx5_cqwq_get_cqe(&cq->wq)));

	mlx5_cqwq_update_db_record(&cq->wq);

	/* ensure cq space is freed before enabling more cqes */
	wmb();

	sq->cc = sqcc;
	return (i == MLX5E_TX_CQ_POLL_BUDGET);
}

void mlx5e_free_xdpsq_descs(struct mlx5e_xdpsq *sq)
{
	struct mlx5e_rq *rq = container_of(sq, struct mlx5e_rq, xdpsq);
	struct mlx5e_dma_info *di;
	u16 ci;

	while (sq->cc != sq->pc) {
		ci = mlx5_wq_cyc_ctr2ix(&sq->wq, sq->cc);
		di = &sq->db.di[ci];
		sq->cc++;

		mlx5e_page_release(rq, di, false);
	}
}

#ifdef CONFIG_MLX5_CORE_IPOIB

#define MLX5_IB_GRH_DGID_OFFSET 24
#define MLX5_GID_SIZE           16

static inline void mlx5i_complete_rx_cqe(struct mlx5e_rq *rq,
					 struct mlx5_cqe64 *cqe,
					 u32 cqe_bcnt,
					 struct sk_buff *skb)
{
	struct mlx5e_rq_stats *stats = rq->stats;
	struct hwtstamp_config *tstamp;
	struct net_device *netdev;
	struct mlx5e_priv *priv;
	char *pseudo_header;
	u32 qpn;
	u8 *dgid;
	u8 g;
#ifdef CONFIG_COMPAT_LRO_ENABLED_IPOIB
       struct mlx5e_priv *parent_priv = mlx5i_epriv(rq->netdev);
#endif

	qpn = be32_to_cpu(cqe->sop_drop_qpn) & 0xffffff;
	netdev = mlx5i_pkey_get_netdev(rq->netdev, qpn);

	/* No mapping present, cannot process SKB. This might happen if a child
	 * interface is going down while having unprocessed CQEs on parent RQ
	 */
	if (unlikely(!netdev)) {
		/* TODO: add drop counters support */
		skb->dev = NULL;
		pr_warn_once("Unable to map QPN %u to dev - dropping skb\n", qpn);
		return;
	}

	priv = mlx5i_epriv(netdev);
	tstamp = &priv->tstamp;

	g = (be32_to_cpu(cqe->flags_rqpn) >> 28) & 3;
	dgid = skb->data + MLX5_IB_GRH_DGID_OFFSET;
	if ((!g) || dgid[0] != 0xff)
		skb->pkt_type = PACKET_HOST;
	else if (memcmp(dgid, netdev->broadcast + 4, MLX5_GID_SIZE) == 0)
		skb->pkt_type = PACKET_BROADCAST;
	else
		skb->pkt_type = PACKET_MULTICAST;

	/* TODO: IB/ipoib: Allow mcast packets from other VFs
	 * 68996a6e760e5c74654723eeb57bf65628ae87f4
	 */

	skb_pull(skb, MLX5_IB_GRH_BYTES);

	skb->protocol = *((__be16 *)(skb->data));

#ifdef CONFIG_COMPAT_LRO_ENABLED_IPOIB
	if (parent_priv->netdev->features & NETIF_F_LRO) {
		skb->ip_summed = CHECKSUM_UNNECESSARY;
	} else {
		skb->ip_summed = CHECKSUM_COMPLETE;
		skb->csum = csum_unfold((__force __sum16)cqe->check_sum);
		stats->csum_complete++;
	}
#else
	skb->ip_summed = CHECKSUM_COMPLETE;
	skb->csum = csum_unfold((__force __sum16)cqe->check_sum);
	stats->csum_complete++;
#endif

	if (unlikely(mlx5e_rx_hw_stamp(tstamp)))
		skb_hwtstamps(skb)->hwtstamp =
				mlx5_timecounter_cyc2time(rq->clock, get_cqe_ts(cqe));

	skb_record_rx_queue(skb, rq->ix);

#ifdef HAVE_NETIF_F_RXHASH
	if (likely(netdev->features & NETIF_F_RXHASH))
		mlx5e_skb_set_hash(cqe, skb);
#endif

	/* 20 bytes of ipoib header and 4 for encap existing */
	pseudo_header = skb_push(skb, MLX5_IPOIB_PSEUDO_LEN);
	memset(pseudo_header, 0, MLX5_IPOIB_PSEUDO_LEN);
	skb_reset_mac_header(skb);
	skb_pull(skb, MLX5_IPOIB_HARD_LEN);

	skb->dev = netdev;

	stats->packets++;
	stats->bytes += cqe_bcnt;
}

void mlx5i_handle_rx_cqe(struct mlx5e_rq *rq, struct mlx5_cqe64 *cqe)
{
#ifdef CONFIG_COMPAT_LRO_ENABLED_IPOIB
	struct mlx5e_priv *priv = mlx5i_epriv(rq->netdev);
#endif
	struct mlx5_wq_cyc *wq = &rq->wqe.wq;
	struct mlx5e_wqe_frag_info *wi;
	struct sk_buff *skb;
	u32 cqe_bcnt;
	u16 ci;

	ci       = mlx5_wq_cyc_ctr2ix(wq, be16_to_cpu(cqe->wqe_counter));
	wi       = get_frag(rq, ci);
	cqe_bcnt = be32_to_cpu(cqe->byte_cnt);

	skb = rq->wqe.skb_from_cqe(rq, cqe, wi, cqe_bcnt);
	if (!skb)
		goto wq_free_wqe;

	mlx5i_complete_rx_cqe(rq, cqe, cqe_bcnt, skb);
	if (unlikely(!skb->dev)) {
		dev_kfree_skb_any(skb);
		goto wq_free_wqe;
	}
#ifdef CONFIG_COMPAT_LRO_ENABLED_IPOIB
	if (priv->netdev->features & NETIF_F_LRO)
		lro_receive_skb(&rq->sw_lro->lro_mgr, skb, NULL);
	else
#endif
	napi_gro_receive(rq->cq.napi, skb);

wq_free_wqe:
	mlx5e_free_rx_wqe(rq, wi, true);
	mlx5_wq_cyc_pop(wq);
}

#endif /* CONFIG_MLX5_CORE_IPOIB */

#ifdef CONFIG_MLX5_EN_IPSEC

void mlx5e_ipsec_handle_rx_cqe(struct mlx5e_rq *rq, struct mlx5_cqe64 *cqe)
{
#ifdef CONFIG_COMPAT_LRO_ENABLED_IPOIB
	struct mlx5e_priv *priv = mlx5i_epriv(rq->netdev);
#endif
	struct mlx5_wq_cyc *wq = &rq->wqe.wq;
	struct mlx5e_wqe_frag_info *wi;
	struct sk_buff *skb;
	u32 cqe_bcnt;
	u16 ci;

	ci       = mlx5_wq_cyc_ctr2ix(wq, be16_to_cpu(cqe->wqe_counter));
	wi       = get_frag(rq, ci);
	cqe_bcnt = be32_to_cpu(cqe->byte_cnt);

	skb = rq->wqe.skb_from_cqe(rq, cqe, wi, cqe_bcnt);
	if (unlikely(!skb)) {
		/* a DROP, save the page-reuse checks */
		mlx5e_free_rx_wqe(rq, wi, true);
		goto wq_cyc_pop;
	}
	skb = mlx5e_ipsec_handle_rx_skb(rq->netdev, skb);
	if (unlikely(!skb)) {
		mlx5e_free_rx_wqe(rq, wi, true);
		goto wq_cyc_pop;
	}

	mlx5e_complete_rx_cqe(rq, cqe, cqe_bcnt, skb);
	napi_gro_receive(rq->cq.napi, skb);

	mlx5e_free_rx_wqe(rq, wi, true);
wq_cyc_pop:
	mlx5_wq_cyc_pop(wq);
}

#endif /* CONFIG_MLX5_EN_IPSEC */
