/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021-2024 NVIDIA CORPORATION & AFFILIATES. All rights
 * reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/log.h"
#include "spdk/util.h"
#include "spdk/bdev_module.h"
#include "bdev_internal.h"
#include "bdev_qos_limits.h"

static bool
bdev_qos_limit_is_read_io(struct spdk_bdev_io *bdev_io)
{
	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_NVME_IO:
	case SPDK_BDEV_IO_TYPE_NVME_IO_MD:
		/* Bit 1 (0x2) set for read operation */
		if (bdev_io->u.nvme_passthru.cmd.opc & SPDK_NVME_OPC_READ) {
			return true;
		} else {
			return false;
		}
	case SPDK_BDEV_IO_TYPE_READ:
		return true;
	case SPDK_BDEV_IO_TYPE_ZCOPY:
		/* Populate to read from disk */
		if (bdev_io->u.bdev.zcopy.populate) {
			return true;
		} else {
			return false;
		}
	default:
		return false;
	}
}

static uint64_t
bdev_qos_limit_get_io_size_in_bytes(struct spdk_bdev_io *bdev_io)
{
	struct spdk_bdev *bdev = bdev_io->bdev;

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_NVME_IO:
	case SPDK_BDEV_IO_TYPE_NVME_IO_MD:
		return bdev_io->u.nvme_passthru.nbytes;
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
		return bdev_io->u.bdev.num_blocks * bdev->blocklen;
	case SPDK_BDEV_IO_TYPE_ZCOPY:
		/* Track the data in the start phase only */
		if (bdev_io->u.bdev.zcopy.start) {
			return bdev_io->u.bdev.num_blocks * bdev->blocklen;
		} else {
			return 0;
		}
	default:
		return 0;
	}
}

static inline bool
bdev_qos_limit_is_iops_rate_limit(enum spdk_bdev_qos_rate_limit_type limit)
{
	assert(limit != SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES);

	switch (limit) {
	case SPDK_BDEV_QOS_RW_IOPS_RATE_LIMIT:
		return true;
	case SPDK_BDEV_QOS_RW_BPS_RATE_LIMIT:
	case SPDK_BDEV_QOS_R_BPS_RATE_LIMIT:
	case SPDK_BDEV_QOS_W_BPS_RATE_LIMIT:
		return false;
	case SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES:
	default:
		return false;
	}
}

static inline bool
bdev_qos_limit_rw_queue_io(struct bdev_qos_limit *limit,
			   struct spdk_bdev_io *io,
			   uint64_t delta)
{
	int64_t remaining_this_timeslice;

	if (!limit->max_per_timeslice) {
		/* The QoS is disabled */
		return false;
	}

	remaining_this_timeslice = __atomic_sub_fetch(&limit->remaining_this_timeslice, delta,
				   __ATOMIC_RELAXED);
	if (remaining_this_timeslice + (int64_t)delta > 0) {
		/* There was still a quota for this delta -> the IO shouldn't be queued
		 *
		 * We allow a slight quota overrun here so an IO bigger than the per-timeslice
		 * quota can be allowed once a while. Such overrun then taken into account in
		 * the QoS poller, where the next timeslice quota is calculated.
		 */
		return false;
	}

	/* There was no quota for this delta -> the IO should be queued
	 * The remaining_this_timeslice must be rewinded so it reflects the real
	 * amount of IOs or bytes allowed.
	 */
	__atomic_add_fetch(
		&limit->remaining_this_timeslice, delta, __ATOMIC_RELAXED);
	return true;
}

static inline void
bdev_qos_limit_rw_rewind_io(struct bdev_qos_limit *limit,
			    struct spdk_bdev_io *io,
			    uint64_t delta)
{
	__atomic_add_fetch(&limit->remaining_this_timeslice, delta, __ATOMIC_RELAXED);
}

static bool
bdev_qos_limit_rw_iops_queue(struct bdev_qos_limit *limit,
			     struct spdk_bdev_io *io)
{
	return bdev_qos_limit_rw_queue_io(limit, io, 1);
}

static void
bdev_qos_limit_rw_iops_rewind_quota(struct bdev_qos_limit *limit,
				    struct spdk_bdev_io *io)
{
	bdev_qos_limit_rw_rewind_io(limit, io, 1);
}

static bool
bdev_qos_limit_rw_bps_queue(struct bdev_qos_limit *limit,
			    struct spdk_bdev_io *io)
{
	return bdev_qos_limit_rw_queue_io(limit, io, bdev_qos_limit_get_io_size_in_bytes(io));
}

static void
bdev_qos_limit_rw_bps_rewind_quota(struct bdev_qos_limit *limit,
				   struct spdk_bdev_io *io)
{
	bdev_qos_limit_rw_rewind_io(limit, io, bdev_qos_limit_get_io_size_in_bytes(io));
}

static bool
bdev_qos_limit_r_bps_queue(struct bdev_qos_limit *limit,
			   struct spdk_bdev_io *io)
{
	if (bdev_qos_limit_is_read_io(io) == false) {
		return false;
	}

	return bdev_qos_limit_rw_bps_queue(limit, io);
}

static void
bdev_qos_limit_r_bps_rewind_quota(struct bdev_qos_limit *limit,
				  struct spdk_bdev_io *io)
{
	if (bdev_qos_limit_is_read_io(io) != false) {
		bdev_qos_limit_rw_rewind_io(limit, io, bdev_qos_limit_get_io_size_in_bytes(io));
	}
}

static bool
bdev_qos_limit_w_bps_queue(struct bdev_qos_limit *limit,
			   struct spdk_bdev_io *io)
{
	if (bdev_qos_limit_is_read_io(io) == true) {
		return false;
	}

	return bdev_qos_limit_rw_bps_queue(limit, io);
}

static void
bdev_qos_limit_w_bps_rewind_quota(struct bdev_qos_limit *limit,
				  struct spdk_bdev_io *io)
{
	if (bdev_qos_limit_is_read_io(io) != true) {
		bdev_qos_limit_rw_rewind_io(limit, io, bdev_qos_limit_get_io_size_in_bytes(io));
	}
}

static void
bdev_qos_limit_set_ops(struct bdev_qos_limit *limit, enum spdk_bdev_qos_rate_limit_type type)
{
	if (limit->limit == SPDK_BDEV_QOS_LIMIT_NOT_DEFINED) {
		limit->queue_io = NULL;
		return;
	}

	switch (type) {
	case SPDK_BDEV_QOS_RW_IOPS_RATE_LIMIT:
		limit->queue_io = bdev_qos_limit_rw_iops_queue;
		limit->rewind_quota = bdev_qos_limit_rw_iops_rewind_quota;
		break;
	case SPDK_BDEV_QOS_RW_BPS_RATE_LIMIT:
		limit->queue_io = bdev_qos_limit_rw_bps_queue;
		limit->rewind_quota = bdev_qos_limit_rw_bps_rewind_quota;
		break;
	case SPDK_BDEV_QOS_R_BPS_RATE_LIMIT:
		limit->queue_io = bdev_qos_limit_r_bps_queue;
		limit->rewind_quota = bdev_qos_limit_r_bps_rewind_quota;
		break;
	case SPDK_BDEV_QOS_W_BPS_RATE_LIMIT:
		limit->queue_io = bdev_qos_limit_w_bps_queue;
		limit->rewind_quota = bdev_qos_limit_w_bps_rewind_quota;
		break;
	default:
		break;
	}
}

static void
bdev_qos_limits_set_ops(struct bdev_qos_limits *limits)
{
	int i;

	for (i = 0; i < SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES; i++) {
		bdev_qos_limit_set_ops(&limits->rate_limits[i], i);
	}
}

static void
bdev_qos_limit_init(struct bdev_qos_limit *limit, enum spdk_bdev_qos_rate_limit_type type)
{
	if (bdev_qos_limit_is_iops_rate_limit(type) == true) {
		limit->min_per_timeslice = SPDK_BDEV_QOS_MIN_IO_PER_TIMESLICE;
	} else {
		limit->min_per_timeslice = SPDK_BDEV_QOS_MIN_BYTE_PER_TIMESLICE;
	}

	limit->limit = SPDK_BDEV_QOS_LIMIT_NOT_DEFINED;
}

void
bdev_qos_limits_init(struct bdev_qos_limits *limits)
{
	int i;

	for (i = 0; i < SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES; i++) {
		bdev_qos_limit_init(&limits->rate_limits[i], i);
	}
}

static void
bdev_qos_limit_set(struct bdev_qos_limit *limit, enum spdk_bdev_qos_rate_limit_type type,
		   uint64_t value)
{
	uint32_t limit_set_complement;
	uint64_t min_limit_per_sec;

	if (bdev_qos_limit_is_iops_rate_limit(type) == true) {
		limit->limit = value;
		min_limit_per_sec = SPDK_BDEV_QOS_MIN_IOS_PER_SEC;
	} else {
		/* Change from megabyte to byte rate limit */
		limit->limit = value * 1024 * 1024;
		min_limit_per_sec = SPDK_BDEV_QOS_MIN_BYTES_PER_SEC;
	}

	limit_set_complement = limit->limit % min_limit_per_sec;
	if (limit_set_complement) {
		SPDK_ERRLOG("Requested rate limit %" PRIu64
			    " is not a multiple of %" PRIu64 "\n",
			    value,
			    min_limit_per_sec);
		limit->limit += min_limit_per_sec - limit_set_complement;
		SPDK_ERRLOG("Round up the rate limit to %" PRIu64 "\n", value);
	}
}

void
bdev_qos_limits_set(struct bdev_qos_limits *limits, const uint64_t *values)
{
	int i;

	for (i = 0; i < SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES; i++) {
		bdev_qos_limit_set(&limits->rate_limits[i], i, values[i]);
	}
}

static void
bdev_qos_limit_update_max_quota_per_timeslice(struct bdev_qos_limit *limit,
		enum spdk_bdev_qos_rate_limit_type type)
{
	uint64_t max_per_timeslice;

	if (bdev_qos_limit_is_iops_rate_limit(type) == true) {
		limit->min_per_timeslice = SPDK_BDEV_QOS_MIN_IO_PER_TIMESLICE;
	} else {
		limit->min_per_timeslice = SPDK_BDEV_QOS_MIN_BYTE_PER_TIMESLICE;
	}

	if (limit->limit == SPDK_BDEV_QOS_LIMIT_NOT_DEFINED) {
		limit->max_per_timeslice = 0;
		return;
	}

	max_per_timeslice = limit->limit * SPDK_BDEV_QOS_TIMESLICE_IN_USEC / SPDK_SEC_TO_USEC;

	limit->max_per_timeslice = spdk_max(max_per_timeslice, limit->min_per_timeslice);

	__atomic_store_n(&limit->remaining_this_timeslice, limit->max_per_timeslice,
			 __ATOMIC_RELEASE);
}

void
bdev_qos_limits_update_max_quota_per_timeslice(struct bdev_qos_limits *limits)
{
	int i;

	for (i = 0; i < SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES; i++) {
		bdev_qos_limit_update_max_quota_per_timeslice(&limits->rate_limits[i], i);
	}

	bdev_qos_limits_set_ops(limits);
}

static inline void
bdev_qos_limit_rewind(struct bdev_qos_limit *limit, struct spdk_bdev_io *bdev_io)
{
	if (!limit->queue_io) {
		return;
	}

	limit->rewind_quota(limit, bdev_io);
}

void
bdev_qos_limits_rewind(struct bdev_qos_limits *limits, struct spdk_bdev_io *bdev_io)
{
	int i;

	for (i = 0; i < SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES; i++) {
		bdev_qos_limit_rewind(&limits->rate_limits[i], bdev_io);
	}
}

static inline bool
bdev_qos_limit_queue_io(struct bdev_qos_limit *limit, struct spdk_bdev_io *bdev_io)
{
	if (!limit->queue_io) {
		return false;
	}

	return limit->queue_io(limit, bdev_io);
}

bool
bdev_qos_limits_queue_io(struct bdev_qos_limits *limits, struct spdk_bdev_io *bdev_io)
{
	int i;

	for (i = 0; i < SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES; i++) {
		if (bdev_qos_limit_queue_io(&limits->rate_limits[i], bdev_io) == true) {
			for (i -= 1; i >= 0 ; i--) {
				bdev_qos_limit_rewind(&limits->rate_limits[i], bdev_io);
			}
			return true;
		}
	}

	return false;
}

static inline void
bdev_qos_limit_reset_quota(struct bdev_qos_limit *limit, int timeslice_count)
{
	int64_t remaining_last_timeslice;

	/* We may have allowed the IOs or bytes to slightly overrun in the last
	 * timeslice. remaining_this_timeslice is signed, so if it's negative
	 * here, we'll account for the overrun so that the next timeslice will
	 * be appropriately reduced.
	 */
	remaining_last_timeslice = __atomic_exchange_n(&limit->remaining_this_timeslice,
				   0, __ATOMIC_RELAXED);
	if (remaining_last_timeslice < 0) {
		/* There could be a race condition here as both bdev_qos_rw_queue_io() and bdev_channel_poll_qos()
		 * potentially use 2 atomic ops each, so they can intertwine.
		 * This race can potentialy cause the limits to be a little fuzzy but won't cause any real damage.
		 */
		__atomic_store_n(&limit->remaining_this_timeslice, remaining_last_timeslice,
				 __ATOMIC_RELAXED);
	}

	if (timeslice_count > 0) {
		__atomic_add_fetch(&limit->remaining_this_timeslice,
				   limit->max_per_timeslice * timeslice_count,
				   __ATOMIC_RELAXED);
	}
}

void
bdev_qos_limits_reset_quota(struct bdev_qos_limits *limits,
			    uint64_t now,
			    uint64_t timeslice_size,
			    uint64_t *last_timeslice)
{
	int timeslice_count = 0;
	int i;

	while (now >= (*last_timeslice + timeslice_size)) {
		*last_timeslice += timeslice_size;
		timeslice_count++;
	}

	for (i = 0; i < SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES; i++) {
		bdev_qos_limit_reset_quota(&limits->rate_limits[i], timeslice_count);
	}
}

bool
bdev_qos_limits_check_disabled(const uint64_t *limits)
{
	int i;

	for (i = 0; i < SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES; i++) {
		if (limits[i] != SPDK_BDEV_QOS_LIMIT_NOT_DEFINED) {
			return false;
		}
	}
	return true;
}
