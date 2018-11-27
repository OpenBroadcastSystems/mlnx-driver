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

#include "en.h"
#include <linux/irq.h>
#include <linux/prefetch.h>

#if defined(CONFIG_NETMAP) || defined(CONFIG_NETMAP_MODULE)
/*
 * mlx5_netmap_linux.h contains functions for netmap support
 * that extend the standard driver.
 */
#include "mlx5_netmap_linux.h"
#endif

void mlx5e_prefetch_cqe(struct mlx5e_cq *cq)
{
	struct mlx5_cqwq *wq = &cq->wq;
	u32 ci = mlx5_cqwq_get_ci(wq);
	struct mlx5_cqe64 *cqe = mlx5_cqwq_get_wqe(wq, ci);

	prefetch(cqe);
}

struct mlx5_cqe64 *mlx5e_get_cqe(struct mlx5e_cq *cq)
{
	struct mlx5_cqwq *wq = &cq->wq;
	u32 ci = mlx5_cqwq_get_ci(wq);
	struct mlx5_cqe64 *cqe = mlx5_cqwq_get_wqe(wq, ci);
	int cqe_ownership_bit = cqe->op_own & MLX5_CQE_OWNER_MASK;
	int sw_ownership_val = mlx5_cqwq_get_wrap_cnt(wq) & 1;

	if (cqe_ownership_bit != sw_ownership_val)
		return NULL;

	/* ensure cqe content is read after cqe ownership bit */
	rmb();

	return cqe;
}

static inline bool mlx5e_no_channel_affinity_change(struct mlx5e_channel *c)
{
#if defined(HAVE_IRQ_DESC_GET_IRQ_DATA) && defined(HAVE_IRQ_TO_DESC_EXPORTED)
	int current_cpu = smp_processor_id();
#ifdef HAVE_IRQ_DATA_AFFINITY
	struct cpumask *aff = irq_desc_get_irq_data(c->irq_desc)->affinity;
#else
	struct irq_data *idata;
	const struct cpumask *aff;
	idata = irq_desc_get_irq_data(c->irq_desc);
	aff = irq_data_get_affinity_mask(idata);
#endif

	return cpumask_test_cpu(current_cpu, aff);
#else
	if (c->tot_rx < MLX5_EN_MIN_RX_ARM)
		return true;

	c->tot_rx = 0;
	return false;
#endif
}

int mlx5e_napi_poll(struct napi_struct *napi, int budget)
{
	struct mlx5e_channel *c = container_of(napi, struct mlx5e_channel,
					       napi);
	bool busy = false;
	int i;

	clear_bit(MLX5E_CHANNEL_NAPI_SCHED, &c->flags);

#ifdef DEV_NETMAP
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

		/* avoid losing completion event during/after polling cqs */
		if (test_bit(MLX5E_CHANNEL_NAPI_SCHED, &c->flags)) {
			napi_schedule(napi); /* request another call to this func */
		}

		return 0;
	}
#endif

	busy |= mlx5e_poll_rx_cq(&c->rq.cq, budget);

	busy |= mlx5e_post_rx_wqes(&c->rq);

	for (i = 0; i < c->num_tx; i++)
		busy |= mlx5e_poll_tx_cq(&c->sq[i].cq);

#if !(defined(HAVE_IRQ_DESC_GET_IRQ_DATA) && defined(HAVE_IRQ_TO_DESC_EXPORTED))
	c->tot_rx += budget;
#endif
	if (busy && likely(mlx5e_no_channel_affinity_change(c)))
		return budget;

	napi_complete(napi);

	/* avoid losing completion event during/after polling cqs */
	if (test_bit(MLX5E_CHANNEL_NAPI_SCHED, &c->flags)) {
		napi_schedule(napi);
		return 0;
	}

	for (i = 0; i < c->num_tx; i++)
		mlx5e_cq_arm(&c->sq[i].cq);
	mlx5e_cq_arm(&c->rq.cq);

	return 0;
}

void mlx5e_completion_event(struct mlx5_core_cq *mcq)
{
	struct mlx5e_cq *cq = container_of(mcq, struct mlx5e_cq, mcq);

	set_bit(MLX5E_CHANNEL_NAPI_SCHED, &cq->channel->flags);
	barrier();
	napi_schedule(cq->napi);
}

void mlx5e_cq_error_event(struct mlx5_core_cq *mcq, enum mlx5_event event)
{
	struct mlx5e_cq *cq = container_of(mcq, struct mlx5e_cq, mcq);
	struct mlx5e_channel *c = cq->channel;
	struct mlx5e_priv *priv = c->priv;
	struct net_device *netdev = priv->netdev;

	netdev_err(netdev, "%s: cqn=0x%.6x event=0x%.2x\n",
		   __func__, mcq->cqn, event);
}
