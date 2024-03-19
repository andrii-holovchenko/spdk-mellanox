#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

DIRS-y = accel.c
DIRS-$(CONFIG_CRYPTO) += dpdk_cryptodev.c
DIRS-$(CONFIG_DPDK_COMPRESSDEV) += dpdk_compressdev.c
ifeq ($(CONFIG_RDMA_PROV),mlx5_dv)
DIRS-y += accel_mlx5.c
endif

.PHONY: all clean $(DIRS-y)

all: $(DIRS-y)
clean: $(DIRS-y)

include $(SPDK_ROOT_DIR)/mk/spdk.subdirs.mk
