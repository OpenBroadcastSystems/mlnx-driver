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

#if defined(HAVE_IRQ_DESC_GET_IRQ_DATA) && defined(HAVE_IRQ_TO_DESC_EXPORTED)
#include <linux/irq.h>
#endif
#include "en.h"


#if defined(CONFIG_NETMAP) || defined(CONFIG_NETMAP_MODULE)
/*
 * mlx5_netmap_linux.h contains functions for netmap support
 * that extend the standard driver.
 */
#include "mlx5_netmap_linux.h"
#endif

#if defined(HAVE_IRQ_DESC_GET_IRQ_DATA) && defined(HAVE_IRQ_TO_DESC_EXPORTED)
static inline bool mlx5e_channel_no_affinity_change(struct mlx5e_channel *c)
{
	int current_cpu = smp_processor_id();
	const struct cpumask *aff;
#ifndef HAVE_IRQ_DATA_AFFINITY
	struct irq_data *idata;

	idata = irq_desc_get_irq_data(c->irq_desc);
	aff = irq_data_get_affinity_mask(idata);
#else
	aff = irq_desc_get_irq_data(c->irq_desc)->affinity;
#endif
	return cpumask_test_cpu(current_cpu, aff);
}
#endif

int mlx5e_napi_poll(struct napi_struct *napi, int budget)
{
	struct mlx5e_channel *c = container_of(napi, struct mlx5e_channel,
					       napi);
	bool busy = false;
	int work_done = 0;
	int i;

#ifndef HAVE_NAPI_COMPLETE_DONE_RET_VALUE
	clear_bit(MLX5E_CHANNEL_NAPI_SCHED, &c->flags);
#endif

#if defined(CONFIG_NETMAP) || defined(CONFIG_NETMAP_MODULE)
	if (nm_netmap_on(NA(c->netdev))) {
		/*
		 * In netmap mode, all the work is done in the context
		 * of the client thread. Interrupt handlers only wake up
		 * clients, which may be sleeping on individual rings
		 * or on a global resource for all rings.
		 */
		struct mlx5e_rq *rq = &c->rq;
		int dummy;

		/* Wake netmap rx client. This results in a call to
		 * mlx5e_netmap_rxsync() which will check for any
		 * received packets and process them
		 */
		netmap_rx_irq(rq->netdev, rq->ix, &dummy);

		for (i = 0; i < c->num_tc; i++) {

			struct mlx5e_cq *scq = &c->sq[i].cq;

			/* Wake netmap tx client. This results in a call to
			 * mlx5e_netmap_txsync()  which will check if a batch
			 * of packets has finished sending and recycle the
			 * buffers
			 */
			netmap_tx_irq(scq->channel->netdev, scq->channel->ix);
		}

		/* cq interrupts are not re-armed until the end of the
		 * mlx5e_netmap_*sync() functions so we don't get more
		 * interrupts if a call to those is already pending or in progress.
		 */
		napi_complete(napi);

		return 0;
	}
#endif

	for (i = 0; i < c->num_tc; i++)
		busy |= mlx5e_poll_tx_cq(&c->sq[i].cq, budget);

#ifdef CONFIG_MLX5_EN_SPECIAL_SQ
	for (i = 0; i < c->num_special_sq; i++)
		busy |= mlx5e_poll_tx_cq(&c->special_sq[i].cq, budget);
#endif

#ifdef HAVE_NETDEV_BPF
	if (c->xdp)
		busy |= mlx5e_poll_xdpsq_cq(&c->rq.xdpsq.cq);
#endif

	if (likely(budget)) { /* budget=0 means: don't poll rx rings */
		work_done = mlx5e_poll_rx_cq(&c->rq.cq, budget);
		busy |= work_done == budget;
	}

	busy |= c->rq.post_wqes(&c->rq);

#if defined(HAVE_IRQ_DESC_GET_IRQ_DATA) && defined(HAVE_IRQ_TO_DESC_EXPORTED)
	if (busy) {
		if (likely(mlx5e_channel_no_affinity_change(c)))
			return budget;
		if (budget && work_done == budget)
			work_done--;
	}
#else
	if (busy)
		return budget;
#endif

#ifdef HAVE_NAPI_COMPLETE_DONE_RET_VALUE
	if (unlikely(!napi_complete_done(napi, work_done)))
		return work_done;
#else
	napi_complete_done(napi, work_done);

	/* avoid losing completion event during/after polling cqs */
	if (test_bit(MLX5E_CHANNEL_NAPI_SCHED, &c->flags)) {
		 napi_schedule(napi);
		 return work_done;
	}
#endif

	for (i = 0; i < c->num_tc; i++)
		mlx5e_cq_arm(&c->sq[i].cq);

#ifdef CONFIG_MLX5_EN_SPECIAL_SQ
	for (i = 0; i < c->num_special_sq; i++)
		mlx5e_cq_arm(&c->special_sq[i].cq);
#endif

	if (MLX5E_TEST_BIT(c->rq.state, MLX5E_RQ_STATE_AM))
		mlx5e_rx_am(&c->rq);

	mlx5e_cq_arm(&c->rq.cq);
	mlx5e_cq_arm(&c->icosq.cq);

	return work_done;
}

void mlx5e_completion_event(struct mlx5_core_cq *mcq)
{
	struct mlx5e_cq *cq = container_of(mcq, struct mlx5e_cq, mcq);

	cq->event_ctr++;
#ifndef HAVE_NAPI_COMPLETE_DONE_RET_VALUE
	set_bit(MLX5E_CHANNEL_NAPI_SCHED, &cq->channel->flags);
#endif
	napi_schedule(cq->napi);
}

void mlx5e_cq_error_event(struct mlx5_core_cq *mcq, enum mlx5_event event)
{
	struct mlx5e_cq *cq = container_of(mcq, struct mlx5e_cq, mcq);
	struct mlx5e_channel *c = cq->channel;
	struct net_device *netdev = c->netdev;

	netdev_err(netdev, "%s: cqn=0x%.6x event=0x%.2x\n",
		   __func__, mcq->cqn, event);
}
