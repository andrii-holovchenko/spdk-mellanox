/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2024-2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/** \file
 * Filesystem device internal APIs
 */

#ifndef SPDK_FSDEV_INT_H
#define SPDK_FSDEV_INT_H

#include "spdk/thread.h"

#define __io_ch_to_fsdev_ch(io_ch)	((struct spdk_fsdev_channel *)spdk_io_channel_get_ctx(io_ch))

typedef TAILQ_HEAD(, spdk_fsdev_io) fsdev_io_tailq_t;

struct spdk_fsdev_channel {
	struct spdk_fsdev	*fsdev;

	/* The channel for the underlying device */
	struct spdk_io_channel	*channel;

	/* Per io_device per thread data */
	struct spdk_fsdev_shared_resource *shared_resource;

	/*
	 * Count of I/O submitted to the underlying dev module through this channel
	 * and waiting for completion.
	 */
	uint64_t		io_outstanding;

	/*
	 * List of all submitted I/Os.
	 */
	fsdev_io_tailq_t	io_submitted;

	/* Channel flags */
	uint32_t		flags;

	/* Trace ID */
	uint16_t		trace_id;

	/* Stat */
	struct spdk_fsdev_io_stat *stat;

	/* Poller for delayed IO handling */
	struct spdk_poller	*poller;

	/* TAILQs for holding delayed IOs */
	fsdev_io_tailq_t	delayed_submit;
	fsdev_io_tailq_t	delayed_complete;
	fsdev_io_tailq_t	delayed_99_complete;
	uint32_t		delayed_99_count;
};

const char *fsdev_notify_type_get_name(enum spdk_fsdev_notify_type type);

#endif /* SPDK_FSDEV_INT_H */
