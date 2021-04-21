EXTRA_CFLAGS += $(OPENIB_KERNEL_EXTRA_CFLAGS) \
		$(KERNEL_MEMTRACK_CFLAGS) \
		$(KERNEL_SYSTUNE_CFLAGS) \
		-I$(CWD)/include \
		-I$(CWD)/drivers/net/ethernet/mellanox/mlx5 \
		-I$(CWD)/drivers/net/ethernet/mellanox/mlxfw \

ifneq (,$(CFLAGS_RETPOLINE))
# This is x86 and kernel has no retpoline support.
# Now we need to check for gcc support
ifneq (,$(shell $(CC) --target-help 2>/dev/null | grep -- -mindirect-branch=))
# The compiler supports it. Set the proper flags (inline or extern):
subdir-ccflags-y += $(CFLAGS_RETPOLINE)
endif
endif

obj-y := compat$(CONFIG_COMPAT_VERSION)/
obj-$(CONFIG_MLX5_CORE)         += drivers/net/ethernet/mellanox/mlx5/core/
obj-$(CONFIG_MLX5_CORE)         += drivers/infiniband/hw/mlx5/
obj-$(CONFIG_MLXFW)             += drivers/net/ethernet/mellanox/mlxfw/
obj-$(CONFIG_MEMTRACK)          += drivers/net/ethernet/mellanox/debug/
