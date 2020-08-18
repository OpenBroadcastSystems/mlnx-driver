/* SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB */
/* Copyright (c) 2019 Mellanox Technologies. */

#ifndef __MLX5_EN_XSK_TX_H__
#define __MLX5_EN_XSK_TX_H__

#ifdef HAVE_XSK_UMEM_CONSUME_TX_GET_2_PARAMS

#include "en.h"
#ifdef HAVE_NDO_XSK_WAKEUP
#include <net/xdp_sock.h>
#endif
/* TX data path */
int mlx5e_xsk_wakeup(struct net_device *dev, u32 qid, u32 flags);

bool mlx5e_xsk_tx(struct mlx5e_xdpsq *sq, unsigned int budget);

#ifdef HAVE_NDO_XSK_WAKEUP
static inline void mlx5e_xsk_update_tx_wakeup(struct mlx5e_xdpsq *sq)
{
	if (!xsk_umem_uses_need_wakeup(sq->umem))
		return;

	if (sq->pc != sq->cc)
		xsk_clear_tx_need_wakeup(sq->umem);
	else
		xsk_set_tx_need_wakeup(sq->umem);
}
#endif

#endif /* HAVE_XSK_UMEM_CONSUME_TX_GET_2_PARAMS */
#endif /* __MLX5_EN_XSK_TX_H__ */
