// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/* Copyright (c) 2020, Mellanox Technologies inc.  All rights reserved. */

#include "en_rep.h"
#include "en/devlink.h"

#ifdef HAVE_DEVLINK_PORT_ATRRS_SET_GET_SUPPORT
int mlx5e_devlink_port_register(struct mlx5e_priv *priv)
{
	struct devlink *devlink = priv_to_devlink(priv->mdev);
#ifdef HAVE_DEVLINK_PORT_ATRRS_SET_GET_2_PARAMS
	struct devlink_port_attrs attrs = {};

	if (mlx5_core_is_pf(priv->mdev)) {
		attrs.flavour = DEVLINK_PORT_FLAVOUR_PHYSICAL;
		attrs.phys.port_number = PCI_FUNC(priv->mdev->pdev->devfn);
	} else {
		attrs.flavour = DEVLINK_PORT_FLAVOUR_VIRTUAL;
	}

	devlink_port_attrs_set(&priv->dl_port, &attrs);

#else
	if (mlx5_core_is_pf(priv->mdev))
		devlink_port_attrs_set(&priv->dl_port,
				DEVLINK_PORT_FLAVOUR_PHYSICAL,
				PCI_FUNC(priv->mdev->pdev->devfn),
				false, 0
#ifdef HAVE_DEVLINK_PORT_ATRRS_SET_GET_7_PARAMS
				,NULL, 0);
#else
	);
#endif
	else
		devlink_port_attrs_set(&priv->dl_port,
				DEVLINK_PORT_FLAVOUR_VIRTUAL,
				0, false , 0
#ifdef HAVE_DEVLINK_PORT_ATRRS_SET_GET_7_PARAMS
				,NULL, 0);
#else
	);
#endif

#endif
	return devlink_port_register(devlink, &priv->dl_port, 1);
}

void mlx5e_devlink_port_type_eth_set(struct mlx5e_priv *priv)
{
	devlink_port_type_eth_set(&priv->dl_port, priv->netdev);
}

void mlx5e_devlink_port_unregister(struct mlx5e_priv *priv)
{
	devlink_port_unregister(&priv->dl_port);
	memset(&priv->dl_port, 0, sizeof(priv->dl_port));
}

struct devlink_port *mlx5e_get_devlink_port(struct net_device *dev)
{
	struct mlx5e_priv *priv = netdev_priv(dev);

	if (!netif_device_present(dev))
		return NULL;

	if (mlx5e_is_uplink_rep(priv))
		return mlx5e_rep_get_devlink_port(dev);

	return &priv->dl_port;
}
#endif
