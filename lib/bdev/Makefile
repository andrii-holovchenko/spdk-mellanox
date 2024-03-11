#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  Copyright (c) 2023-2024 NVIDIA CORPORATION & AFFILIATES
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 14
SO_MINOR := 0

C_SRCS = bdev.c bdev_rpc.c bdev_zone.c part.c scsi_nvme.c bdev_reservations.c bdev_qos_limits.c bdev_group_rpc.c
C_SRCS-$(CONFIG_VTUNE) += vtune.c
LIBNAME = bdev

SPDK_MAP_FILE = $(abspath $(CURDIR)/spdk_bdev.map)

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk
