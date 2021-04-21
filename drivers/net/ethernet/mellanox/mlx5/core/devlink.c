// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/* Copyright (c) 2019 Mellanox Technologies */

#include <devlink.h>

#include "mlx5_core.h"
#ifdef HAVE_DEVLINK_RELOAD_DOWN_SUPPORT_RELOAD_ACTION
#include "fw_reset.h"
#endif
#include "fs_core.h"
#include "eswitch.h"
#include "en/tc_ct.h"

#ifdef HAVE_DEVLINK_DRIVERINIT_VAL
static unsigned int esw_offloads_num_big_groups = ESW_OFFLOADS_DEFAULT_NUM_GROUPS;
#else
unsigned int esw_offloads_num_big_groups = ESW_OFFLOADS_DEFAULT_NUM_GROUPS;
#endif
module_param_named(num_of_groups, esw_offloads_num_big_groups,
                   uint, 0644);
MODULE_PARM_DESC(num_of_groups,
                 "Eswitch offloads number of big groups in FDB table. Valid range 1 - 1024. Default 15");

#ifdef HAVE_DEVLINK_HAS_FLASH_UPDATE
static int mlx5_devlink_flash_update(struct devlink *devlink,
#ifdef HAVE_FLASH_UPDATE_GET_3_PARAMS
				     struct devlink_flash_update_params *params,
#else
				     const char *file_name,
				     const char *component,
#endif
				     struct netlink_ext_ack *extack)
{
	struct mlx5_core_dev *dev = devlink_priv(devlink);
	const struct firmware *fw;
	int err;

#ifdef HAVE_FLASH_UPDATE_GET_3_PARAMS
	if (params->component)
#else
	if (component)
#endif
		return -EOPNOTSUPP;

	err = request_firmware_direct(&fw,
#ifdef HAVE_FLASH_UPDATE_GET_3_PARAMS
#ifdef HAVE_DEVLINK_FLASH_UPDATE_PARAMS_HAS_STRUCT_FW
			params->fw->data,
#else
			params->file_name,
#endif
#else
			file_name,
#endif
			&dev->pdev->dev);
	if (err)
		return err;

	err = mlx5_firmware_flash(dev, fw, extack);
	release_firmware(fw);

	return err;
}
#endif

#ifdef HAVE_DEVLINK_RELOAD_DOWN_SUPPORT_RELOAD_ACTION
static int mlx5_devlink_reload_fw_activate(struct devlink *devlink, struct netlink_ext_ack *extack)
{
	struct mlx5_core_dev *dev = devlink_priv(devlink);
	u8 reset_level, reset_type, net_port_alive;
	int err;

	err = mlx5_fw_reset_query(dev, &reset_level, &reset_type);
	if (err)
		return err;
	if (!(reset_level & MLX5_MFRL_REG_RESET_LEVEL3)) {
		NL_SET_ERR_MSG_MOD(extack, "FW activate requires reboot");
		return -EINVAL;
	}

	net_port_alive = !!(reset_type & MLX5_MFRL_REG_RESET_TYPE_NET_PORT_ALIVE);
	err = mlx5_fw_reset_set_reset_sync(dev, net_port_alive);
	if (err)
		goto out;

	err = mlx5_fw_reset_wait_reset_done(dev);
out:
	if (err)
		NL_SET_ERR_MSG_MOD(extack, "FW activate command failed");
	return err;
}
#endif

#ifdef HAVE_DEVLINK_RELOAD_DOWN_SUPPORT_RELOAD_ACTION
static int mlx5_devlink_trigger_fw_live_patch(struct devlink *devlink,
		struct netlink_ext_ack *extack)
{
	struct mlx5_core_dev *dev = devlink_priv(devlink);
	u8 reset_level;
	int err;

	err = mlx5_fw_reset_query(dev, &reset_level, NULL);
	if (err)
		return err;
	if (!(reset_level & MLX5_MFRL_REG_RESET_LEVEL0)) {
		NL_SET_ERR_MSG_MOD(extack,
				"FW upgrade to the stored FW can't be done by FW live patching");
		return -EINVAL;
	}

	return mlx5_fw_reset_set_live_patch(dev);
}
#endif

#ifdef HAVE_DEVLINK_HAS_RELOAD_UP_DOWN
static int mlx5_devlink_reload_down(struct devlink *devlink,
#ifdef HAVE_DEVLINK_RELOAD_DOWN_SUPPORT_RELOAD_ACTION
			     bool netns_change,
			     enum devlink_reload_action action,
			     enum devlink_reload_limit limit,
#elif defined(HAVE_DEVLINK_RELOAD_DOWN_HAS_3_PARAMS)
			     bool netns_change,
#endif
			     struct netlink_ext_ack *extack)
{
	struct mlx5_core_dev *dev = devlink_priv(devlink);
#ifdef CONFIG_MLX5_ESWITCH
	u16 mode = 0;

	if (!mlx5_devlink_eswitch_mode_get(devlink, &mode)) {
		if (mode == DEVLINK_ESWITCH_MODE_SWITCHDEV) {
			NL_SET_ERR_MSG_MOD(extack, "Reload not supported in switchdev mode");
			return -EOPNOTSUPP;
		}
	}
#endif
#ifdef HAVE_DEVLINK_RELOAD_DOWN_SUPPORT_RELOAD_ACTION
	switch (action) {
		case DEVLINK_RELOAD_ACTION_DRIVER_REINIT:
			mlx5_unload_one(dev, false);
			return 0;
		case DEVLINK_RELOAD_ACTION_FW_ACTIVATE:
			if (limit == DEVLINK_RELOAD_LIMIT_NO_RESET)
				return mlx5_devlink_trigger_fw_live_patch(devlink, extack);
			return mlx5_devlink_reload_fw_activate(devlink, extack);
		default:
			/* Unsupported action should not get to this function */
			WARN_ON(1);
			return -EOPNOTSUPP;
	}
#else
	mlx5_unload_one(dev, false);
	return 0;
#endif /* HAVE_DEVLINK_RELOAD_DOWN_SUPPORT_RELOAD_ACTION */
}

static int mlx5_devlink_reload_up(struct devlink *devlink,
#ifdef HAVE_DEVLINK_RELOAD_DOWN_SUPPORT_RELOAD_ACTION
		enum devlink_reload_action action,
		enum devlink_reload_limit limit,
		u32 *actions_performed,
#endif
		struct netlink_ext_ack *extack)
{
	struct mlx5_core_dev *dev = devlink_priv(devlink);
#ifdef HAVE_DEVLINK_RELOAD_DOWN_SUPPORT_RELOAD_ACTION
	*actions_performed = BIT(action);
	switch (action) {
		case DEVLINK_RELOAD_ACTION_DRIVER_REINIT:
			return mlx5_load_one(dev, false);
		case DEVLINK_RELOAD_ACTION_FW_ACTIVATE:
			if (limit == DEVLINK_RELOAD_LIMIT_NO_RESET)
				break;
			/* On fw_activate action, also driver is reloaded and reinit performed */
			*actions_performed |= BIT(DEVLINK_RELOAD_ACTION_DRIVER_REINIT);
			return mlx5_load_one(dev, false);
		default:
			/* Unsupported action should not get to this function */
			WARN_ON(1);
			return -EOPNOTSUPP;
	}

	return 0;
#else
	return mlx5_load_one(dev, false);
#endif /* HAVE_DEVLINK_RELOAD_DOWN_SUPPORT_RELOAD_ACTION */
}

#endif /* HAVE_DEVLINK_HAS_RELOAD_UP_DOWN */

#if defined(HAVE_DEVLINK_HAS_INFO_GET) && defined(HAVE_DEVLINK_INFO_VERSION_FIXED_PUT)
static u8 mlx5_fw_ver_major(u32 version)
{
	return (version >> 24) & 0xff;
}

static u8 mlx5_fw_ver_minor(u32 version)
{
	return (version >> 16) & 0xff;
}

static u16 mlx5_fw_ver_subminor(u32 version)
{
	return version & 0xffff;
}

#define DEVLINK_FW_STRING_LEN 32
static int
mlx5_devlink_info_get(struct devlink *devlink, struct devlink_info_req *req,
		      struct netlink_ext_ack *extack)
{
	struct mlx5_core_dev *dev = devlink_priv(devlink);
	char version_str[DEVLINK_FW_STRING_LEN];
	u32 running_fw, stored_fw;
	int err;

	err = devlink_info_driver_name_put(req, DRIVER_NAME);
	if (err)
		return err;

	err = devlink_info_version_fixed_put(req, "fw.psid", dev->board_id);
	if (err)
		return err;

	err = mlx5_fw_version_query(dev, &running_fw, &stored_fw);
	if (err)
		return err;

	snprintf(version_str, sizeof(version_str), "%d.%d.%04d",
		 mlx5_fw_ver_major(running_fw), mlx5_fw_ver_minor(running_fw),
		 mlx5_fw_ver_subminor(running_fw));
	err = devlink_info_version_running_put(req, "fw.version", version_str);
	if (err)
		return err;

	/* no pending version, return running (stored) version */
	if (stored_fw == 0)
		stored_fw = running_fw;

	snprintf(version_str, sizeof(version_str), "%d.%d.%04d",
		 mlx5_fw_ver_major(stored_fw), mlx5_fw_ver_minor(stored_fw),
		 mlx5_fw_ver_subminor(stored_fw));
	err = devlink_info_version_stored_put(req, "fw.version", version_str);
	if (err)
		return err;

	return 0;
}
#endif

static const struct devlink_ops mlx5_devlink_ops = {
#ifdef CONFIG_MLX5_ESWITCH
#ifdef HAVE_DEVLINK_HAS_ESWITCH_MODE_GET_SET
	.eswitch_mode_set = mlx5_devlink_eswitch_mode_set,
	.eswitch_mode_get = mlx5_devlink_eswitch_mode_get,
#endif /* HAVE_DEVLINK_HAS_ESWITCH_MODE_GET_SET */
#ifdef HAVE_DEVLINK_HAS_ESWITCH_INLINE_MODE_GET_SET
	.eswitch_inline_mode_set = mlx5_devlink_eswitch_inline_mode_set,
	.eswitch_inline_mode_get = mlx5_devlink_eswitch_inline_mode_get,
#endif /* HAVE_DEVLINK_HAS_ESWITCH_INLINE_MODE_GET_SET */
#ifdef HAVE_DEVLINK_HAS_ESWITCH_ENCAP_MODE_SET
	.eswitch_encap_mode_set = mlx5_devlink_eswitch_encap_mode_set,
	.eswitch_encap_mode_get = mlx5_devlink_eswitch_encap_mode_get,
#endif /* HAVE_DEVLINK_HAS_ESWITCH_ENCAP_MODE_SET */
#endif
#ifdef HAVE_DEVLINK_HAS_PORT_FUNCTION_HW_ADDR_GET
	.port_function_hw_addr_get = mlx5_devlink_port_function_hw_addr_get,
	.port_function_hw_addr_set = mlx5_devlink_port_function_hw_addr_set,
#endif
#ifdef HAVE_DEVLINK_HAS_FLASH_UPDATE
	.flash_update = mlx5_devlink_flash_update,
#endif /* HAVE_DEVLINK_HAS_FLASH_UPDATE */
#ifdef HAVE_DEVLINK_RELOAD_DOWN_SUPPORT_RELOAD_ACTION
	.reload_actions = BIT(DEVLINK_RELOAD_ACTION_DRIVER_REINIT) |
		       	  BIT(DEVLINK_RELOAD_ACTION_FW_ACTIVATE),
	.reload_limits = BIT(DEVLINK_RELOAD_LIMIT_NO_RESET),
#endif
#ifdef HAVE_DEVLINK_HAS_RELOAD_UP_DOWN
	.reload_down = mlx5_devlink_reload_down,
	.reload_up = mlx5_devlink_reload_up,
#endif /* HAVE_DEVLINK_HAS_RELOAD_UP_DOWN */
#if defined(HAVE_DEVLINK_HAS_INFO_GET) && defined(HAVE_DEVLINK_INFO_VERSION_FIXED_PUT)
	.info_get = mlx5_devlink_info_get,
#endif /* HAVE_DEVLINK_HAS_INFO_GET */
};

struct devlink *mlx5_devlink_alloc(void)
{
	return devlink_alloc(&mlx5_devlink_ops, sizeof(struct mlx5_core_dev));
}

void mlx5_devlink_free(struct devlink *devlink)
{
	devlink_free(devlink);
}


#if defined(HAVE_DEVLINK_PARAM) && defined(HAVE_DEVLINK_PARAMS_PUBLISHED)
static int mlx5_devlink_fs_mode_validate(struct devlink *devlink, u32 id,
					 union devlink_param_value val,
					 struct netlink_ext_ack *extack)
{
	struct mlx5_core_dev *dev = devlink_priv(devlink);
	char *value = val.vstr;
	int err = 0;

	if (!strcmp(value, "dmfs")) {
		return 0;
	} else if (!strcmp(value, "smfs")) {
		u8 eswitch_mode;
		bool smfs_cap;

		eswitch_mode = mlx5_eswitch_mode(dev->priv.eswitch);
		smfs_cap = mlx5_fs_dr_is_supported(dev);

		if (!smfs_cap) {
			err = -EOPNOTSUPP;
			NL_SET_ERR_MSG_MOD(extack,
					   "Software managed steering is not supported by current device");
		}

		else if (eswitch_mode == MLX5_ESWITCH_OFFLOADS) {
			NL_SET_ERR_MSG_MOD(extack,
					   "Software managed steering is not supported when eswitch offloads enabled.");
			err = -EOPNOTSUPP;
		}
	} else {
		NL_SET_ERR_MSG_MOD(extack,
				   "Bad parameter: supported values are [\"dmfs\", \"smfs\"]");
		err = -EINVAL;
	}

	return err;
}

static int mlx5_devlink_fs_mode_set(struct devlink *devlink, u32 id,
				    struct devlink_param_gset_ctx *ctx)
{
	struct mlx5_core_dev *dev = devlink_priv(devlink);
	enum mlx5_flow_steering_mode mode;

	if (!strcmp(ctx->val.vstr, "smfs"))
		mode = MLX5_FLOW_STEERING_MODE_SMFS;
	else
		mode = MLX5_FLOW_STEERING_MODE_DMFS;
	dev->priv.steering->mode = mode;

	return 0;
}

static int mlx5_devlink_fs_mode_get(struct devlink *devlink, u32 id,
				    struct devlink_param_gset_ctx *ctx)
{
	struct mlx5_core_dev *dev = devlink_priv(devlink);

	if (dev->priv.steering->mode == MLX5_FLOW_STEERING_MODE_SMFS)
		strcpy(ctx->val.vstr, "smfs");
	else
		strcpy(ctx->val.vstr, "dmfs");
	return 0;
}

#ifdef HAVE_DEVLINK_PARAM_GENERIC_ID_ENABLE_ROCE
static int mlx5_devlink_enable_roce_validate(struct devlink *devlink, u32 id,
					     union devlink_param_value val,
					     struct netlink_ext_ack *extack)
{
	struct mlx5_core_dev *dev = devlink_priv(devlink);
	bool new_state = val.vbool;

	if (new_state && !MLX5_CAP_GEN(dev, roce)) {
		NL_SET_ERR_MSG_MOD(extack, "Device doesn't support RoCE");
		return -EOPNOTSUPP;
	}

	return 0;
}
#endif

#ifdef CONFIG_MLX5_ESWITCH
static int mlx5_devlink_large_group_num_validate(struct devlink *devlink, u32 id,
						 union devlink_param_value val,
						 struct netlink_ext_ack *extack)
{
	int group_num = val.vu32;

	if (group_num < 1 || group_num > 1024) {
		NL_SET_ERR_MSG_MOD(extack,
				   "Unsupported group number, supported range is 1-1024");
		return -EOPNOTSUPP;
	}

	return 0;
}

#if IS_ENABLED(CONFIG_NET_CLS_E2E_CACHE)
static int mlx5_devlink_e2e_cache_size_validate(struct devlink *devlink, u32 id,
						union devlink_param_value val,
						struct netlink_ext_ack *extack)
{
	int size = val.vu32;

	if (size < 0) {
		NL_SET_ERR_MSG_MOD(extack, "Unsupported e2e cache size");
		return -EOPNOTSUPP;
	}

	return 0;
}
#endif
static int mlx5_devlink_esw_pet_insert_set(struct devlink *devlink, u32 id,
					   struct devlink_param_gset_ctx *ctx)
{
	struct mlx5_core_dev *dev = devlink_priv(devlink);

	if (!MLX5_ESWITCH_MANAGER(dev))
		return -EOPNOTSUPP;

	return mlx5_esw_offloads_pet_insert_set(dev->priv.eswitch, ctx->val.vbool);
}

static int mlx5_devlink_esw_pet_insert_get(struct devlink *devlink, u32 id,
					   struct devlink_param_gset_ctx *ctx)
{
	struct mlx5_core_dev *dev = devlink_priv(devlink);

	if (!MLX5_ESWITCH_MANAGER(dev))
		return -EOPNOTSUPP;

	ctx->val.vbool = mlx5_eswitch_pet_insert_allowed(dev->priv.eswitch);
	return 0;
}

static int mlx5_devlink_esw_pet_insert_validate(struct devlink *devlink, u32 id,
						union devlink_param_value val,
						struct netlink_ext_ack *extack)
{
	struct mlx5_core_dev *dev = devlink_priv(devlink);
	u8 esw_mode;

	if (!MLX5_ESWITCH_MANAGER(dev)) {
		NL_SET_ERR_MSG_MOD(extack, "E-Switch is unsupported");
		return -EOPNOTSUPP;
	}

	esw_mode = mlx5_eswitch_mode(dev->priv.eswitch);
	if (esw_mode == MLX5_ESWITCH_OFFLOADS) {
		NL_SET_ERR_MSG_MOD(extack,
				   "E-Switch must either disabled or non switchdev mode");
		return -EBUSY;
	}

	if (!mlx5e_esw_offloads_pet_supported(dev->priv.eswitch))
		return -EOPNOTSUPP;

	if (!mlx5_core_is_ecpf(dev))
		return -EOPNOTSUPP;

	return 0;
}
#endif /* CONFIG_MLX5_ESWITCH */

static int mlx5_devlink_ct_max_offloaded_conns_set(struct devlink *devlink, u32 id,
						   struct devlink_param_gset_ctx *ctx)
{
	struct mlx5_core_dev *dev = devlink_priv(devlink);

	mlx5_tc_ct_max_offloaded_conns_set(dev, ctx->val.vu32);
	return 0;
}

static int mlx5_devlink_ct_max_offloaded_conns_get(struct devlink *devlink, u32 id,
						   struct devlink_param_gset_ctx *ctx)
{
	struct mlx5_core_dev *dev = devlink_priv(devlink);

	ctx->val.vu32 = mlx5_tc_ct_max_offloaded_conns_get(dev);
	return 0;
}

#ifdef HAVE_DEVLINK_RELOAD_DOWN_SUPPORT_RELOAD_ACTION
static int mlx5_devlink_enable_remote_dev_reset_set(struct devlink *devlink, u32 id,
		struct devlink_param_gset_ctx *ctx)
{
	struct mlx5_core_dev *dev = devlink_priv(devlink);

	mlx5_fw_reset_enable_remote_dev_reset_set(dev, ctx->val.vbool);
	return 0;
}

static int mlx5_devlink_enable_remote_dev_reset_get(struct devlink *devlink, u32 id,
		struct devlink_param_gset_ctx *ctx)
{
	struct mlx5_core_dev *dev = devlink_priv(devlink);

	ctx->val.vbool = mlx5_fw_reset_enable_remote_dev_reset_get(dev);
	return 0;
}
#endif

static const struct devlink_param mlx5_devlink_params[] = {
	DEVLINK_PARAM_DRIVER(MLX5_DEVLINK_PARAM_ID_CT_ACTION_ON_NAT_CONNS,
			     "ct_action_on_nat_conns", DEVLINK_PARAM_TYPE_BOOL,
			     BIT(DEVLINK_PARAM_CMODE_RUNTIME),
			     mlx5_devlink_ct_action_on_nat_conns_get,
			     mlx5_devlink_ct_action_on_nat_conns_set,
			     NULL),
	DEVLINK_PARAM_DRIVER(MLX5_DEVLINK_PARAM_ID_FLOW_STEERING_MODE,
			     "flow_steering_mode", DEVLINK_PARAM_TYPE_STRING,
			     BIT(DEVLINK_PARAM_CMODE_RUNTIME),
			     mlx5_devlink_fs_mode_get, mlx5_devlink_fs_mode_set,
			     mlx5_devlink_fs_mode_validate),
#ifdef HAVE_DEVLINK_PARAM_GENERIC_ID_ENABLE_ROCE
	DEVLINK_PARAM_GENERIC(ENABLE_ROCE, BIT(DEVLINK_PARAM_CMODE_DRIVERINIT),
			      NULL, NULL, mlx5_devlink_enable_roce_validate),
#endif
#ifdef HAVE_DEVLINK_RELOAD_DOWN_SUPPORT_RELOAD_ACTION
	DEVLINK_PARAM_GENERIC(ENABLE_REMOTE_DEV_RESET, BIT(DEVLINK_PARAM_CMODE_RUNTIME),
			mlx5_devlink_enable_remote_dev_reset_get,
			mlx5_devlink_enable_remote_dev_reset_set, NULL),
#endif
	DEVLINK_PARAM_DRIVER(MLX5_DEVLINK_PARAM_ID_CT_MAX_OFFLOADED_CONNS,
			     "ct_max_offloaded_conns", DEVLINK_PARAM_TYPE_U32,
			     BIT(DEVLINK_PARAM_CMODE_RUNTIME),
			     mlx5_devlink_ct_max_offloaded_conns_get,
			     mlx5_devlink_ct_max_offloaded_conns_set,
			     NULL),
#ifdef CONFIG_MLX5_ESWITCH
	DEVLINK_PARAM_DRIVER(MLX5_DEVLINK_PARAM_ID_ESW_LARGE_GROUP_NUM,
			     "fdb_large_groups", DEVLINK_PARAM_TYPE_U32,
			     BIT(DEVLINK_PARAM_CMODE_DRIVERINIT),
			     NULL, NULL,
			     mlx5_devlink_large_group_num_validate),
#if IS_ENABLED(CONFIG_NET_CLS_E2E_CACHE)
	DEVLINK_PARAM_DRIVER(MLX5_DEVLINK_PARAM_ID_ESW_E2E_CACHE_SIZE,
			     "e2e_cache_size", DEVLINK_PARAM_TYPE_U32,
			     BIT(DEVLINK_PARAM_CMODE_DRIVERINIT),
			     NULL, NULL,
			     mlx5_devlink_e2e_cache_size_validate),
#endif
	DEVLINK_PARAM_DRIVER(MLX5_DEVLINK_PARAM_ID_ESW_PET_INSERT,
			     "esw_pet_insert", DEVLINK_PARAM_TYPE_BOOL,
			     BIT(DEVLINK_PARAM_CMODE_RUNTIME),
			     mlx5_devlink_esw_pet_insert_get,
			     mlx5_devlink_esw_pet_insert_set,
			     mlx5_devlink_esw_pet_insert_validate),
#endif /* CONFIG_MLX5_ESWITCH */
};

static void mlx5_devlink_set_params_init_values(struct devlink *devlink)
{
	struct mlx5_core_dev *dev = devlink_priv(devlink);
	union devlink_param_value value;

	if (dev->priv.steering->mode == MLX5_FLOW_STEERING_MODE_DMFS)
		strcpy(value.vstr, "dmfs");
	else
		strcpy(value.vstr, "smfs");
	devlink_param_driverinit_value_set(devlink,
					   MLX5_DEVLINK_PARAM_ID_FLOW_STEERING_MODE,
					   value);

#ifdef HAVE_DEVLINK_PARAM_GENERIC_ID_ENABLE_ROCE
	value.vbool = MLX5_CAP_GEN(dev, roce);
	devlink_param_driverinit_value_set(devlink,
					   DEVLINK_PARAM_GENERIC_ID_ENABLE_ROCE,
					   value);
#endif

#ifdef CONFIG_MLX5_ESWITCH
	value.vu32 = esw_offloads_num_big_groups;
	devlink_param_driverinit_value_set(devlink,
					   MLX5_DEVLINK_PARAM_ID_ESW_LARGE_GROUP_NUM,
					   value);

	value.vu32 = ESW_DEFAULT_E2E_CACHE_SIZE;
	devlink_param_driverinit_value_set(devlink,
					   MLX5_DEVLINK_PARAM_ID_ESW_E2E_CACHE_SIZE,
					   value);

	if (MLX5_ESWITCH_MANAGER(dev)) {
		value.vbool = false;
		devlink_param_driverinit_value_set(devlink,
						   MLX5_DEVLINK_PARAM_ID_ESW_PET_INSERT,
						   value);
	}
#endif
}
#endif /* HAVE_DEVLINK_HAS_INFO_GET && HAVE_DEVLINK_INFO_VERSION_FIXED_PUT */

int mlx5_devlink_register(struct devlink *devlink, struct device *dev)
{
	int err;

	err = devlink_register(devlink, dev);
	if (err)
		return err;

#if defined(HAVE_DEVLINK_PARAM) && defined(HAVE_DEVLINK_PARAMS_PUBLISHED)
	err = devlink_params_register(devlink, mlx5_devlink_params,
				      ARRAY_SIZE(mlx5_devlink_params));
	if (err)
		goto params_reg_err;
	mlx5_devlink_set_params_init_values(devlink);
#ifdef HAVE_DEVLINK_PARAMS_PUBLISHED
	devlink_params_publish(devlink);
#endif /* HAVE_DEVLINK_PARAMS_PUBLISHED */
	return 0;

params_reg_err:
	devlink_unregister(devlink);
#endif
	return err;
}

void mlx5_devlink_unregister(struct devlink *devlink)
{
#if defined(HAVE_DEVLINK_PARAM) && defined(HAVE_DEVLINK_PARAMS_PUBLISHED)
	devlink_params_unregister(devlink, mlx5_devlink_params,
				  ARRAY_SIZE(mlx5_devlink_params));
#endif
	devlink_unregister(devlink);
}

int
mlx5_devlink_ct_action_on_nat_conns_set(struct devlink *devlink, u32 id,
					struct devlink_param_gset_ctx *ctx)
{
	struct mlx5_core_dev *dev = devlink_priv(devlink);

	dev->mlx5e_res.ct.ct_action_on_nat_conns = ctx->val.vbool;
	return 0;
}

int
mlx5_devlink_ct_action_on_nat_conns_get(struct devlink *devlink, u32 id,
					struct devlink_param_gset_ctx *ctx)
{
	struct mlx5_core_dev *dev = devlink_priv(devlink);

	ctx->val.vbool = dev->mlx5e_res.ct.ct_action_on_nat_conns;
	return 0;
}
