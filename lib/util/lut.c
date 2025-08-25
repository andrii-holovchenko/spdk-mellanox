/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/barrier.h"
#include "spdk/log.h"
#include "spdk/assert.h"
#include "spdk/util.h"
#include "spdk/lut.h"

#define SPDK_LUT_TWO_MB (2 * 1024 * 1024)

struct spdk_lut_addr {
	uint64_t valid : 1;
	uint64_t value: 63;
};

SPDK_STATIC_ASSERT(sizeof(uintptr_t) == sizeof(struct spdk_lut_addr),
		   "This implementation assumes 64 bit arch");

struct spdk_lut_node {
	uint64_t key;
	/* NOTE: the current implementation never uses both link and ptr simultaneously.
	 * The link is only used while a node is in the free_nodes list, while the ptr is only used on insert, after
	 * we remove it from the free_nodes list and mark as valid.
	 * We utilize this understanding here, putting both the ptr and the link data members under the same union
	 * that allows us to make the node as small as 16 bytes and therefore improve the cache utilization.
	 */
	union {
		struct spdk_lut_addr addr;
		STAILQ_ENTRY(spdk_lut_node) link;
	} u;
};

/* The sizeof(struct spdk_lut_node) must be exactly 16 bytes  */
SPDK_STATIC_ASSERT(sizeof(struct spdk_lut_node) == 16, "Incorrect size");

struct spdk_lut {
	struct spdk_lut_node *nodes;
	uint64_t num_nodes;
	uint64_t init_size;
	uint64_t growth_step;
	uint64_t max_size;
	STAILQ_HEAD(, spdk_lut_node) free_nodes;
};

static int
lut_extend(struct spdk_lut *lut, uint64_t size)
{
	struct spdk_lut_node *node;
	void *placement, *ptr;
	uint64_t i;
	int rc;

	/* This is the address where we need to populate */
	placement = (void *)(lut->nodes + lut->num_nodes);

	ptr = mmap(placement, size * sizeof(struct spdk_lut_node),
		   PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE | MAP_FIXED,
		   -1, 0);
	if (ptr == MAP_FAILED) {
		rc = errno;
		SPDK_ERRLOG("Unable to populate lut memory. rc: %d\n", rc);
		return -rc;
	}

	for (i = lut->num_nodes; i < lut->num_nodes + size; i++) {
		node = &lut->nodes[i];
		node->key = i;
		STAILQ_INSERT_TAIL(&lut->free_nodes, node, u.link);
		assert(node->u.addr.valid == 0);
	}

	/* Barrier here so that 'num_nodes' isn't increased before
	 * the above nodes have been populated with their keys. */
	spdk_wmb();

	lut->num_nodes += size;

	return 0;
}

static uint64_t
lut_make_2mb_multiple(uint64_t size)
{
	size *= sizeof(struct spdk_lut_node);
	size = spdk_divide_round_up(size, SPDK_LUT_TWO_MB) * SPDK_LUT_TWO_MB;
	size /= sizeof(struct spdk_lut_node);

	return size;
}

static int
lut_init(struct spdk_lut *lut)
{
	int rc;

	assert(STAILQ_EMPTY(&lut->free_nodes));
	assert(lut->nodes == NULL);

	/* Reserve address space to handle the maximum size, but do not populate it with memory yet. */
	lut->nodes = mmap(NULL, lut->max_size * sizeof(struct spdk_lut_node),
			  PROT_NONE,
			  MAP_PRIVATE | MAP_ANONYMOUS,
			  -1, 0);
	if (lut->nodes == MAP_FAILED) {
		rc = errno;
		SPDK_ERRLOG("Failed to create memory mapping for lut. rc: %d\n", rc);
		return -rc;
	}

	/* Populate the initial part of the array. */
	rc = lut_extend(lut, lut->init_size);
	if (rc != 0) {
		munmap(lut->nodes, lut->max_size * sizeof(struct spdk_lut_node));
		lut->nodes = NULL;
		return rc;
	}

	return 0;
}

struct spdk_lut *
spdk_lut_create(uint64_t init_size, uint64_t growth_step, uint64_t max_size)
{
	struct spdk_lut *lut;
	int rc;

	if (max_size < init_size) {
		SPDK_ERRLOG("Invalid sizes: init=%" PRIu64 " max=%" PRIu64 "\n", init_size, max_size);
		return NULL;
	}

	/* Instead of failing, reduce the max size to our internal maximum. Eventually all code will be updated
	 * to provide a reasonable maximum value and we can instead fail here if it is too target. */
	max_size = spdk_min(max_size, SPDK_LUT_MAX_SIZE);

	init_size = lut_make_2mb_multiple(init_size);
	growth_step = lut_make_2mb_multiple(growth_step);
	max_size = lut_make_2mb_multiple(max_size);

	lut = calloc(1, sizeof(*lut));
	if (!lut) {
		SPDK_ERRLOG("Cannot alloc array object\n");
		return NULL;
	}

	STAILQ_INIT(&lut->free_nodes);
	lut->init_size = init_size;
	lut->max_size = max_size;
	lut->growth_step = growth_step;

	rc = lut_init(lut);
	if (rc != 0) {
		free(lut);
		return NULL;
	}

	return lut;
}

uint64_t
spdk_lut_insert(struct spdk_lut *lut, void *value)
{
	struct spdk_lut_node *node;
	struct spdk_lut_addr addr = {
		.valid = 1,
		.value = (uint64_t)value
	};

	if (STAILQ_EMPTY(&lut->free_nodes) && lut_extend(lut, lut->growth_step)) {
		return SPDK_LUT_INVALID_KEY;
	}

	node = STAILQ_FIRST(&lut->free_nodes);
	STAILQ_REMOVE_HEAD(&lut->free_nodes, u.link);

	node->u.addr = addr;

	return node->key;
}

int
spdk_lut_insert_at(struct spdk_lut *lut, void *value, uint64_t key)
{
	struct spdk_lut_node *node;
	struct spdk_lut_addr addr;

	assert(key < lut->max_size);

	while (key >= lut->num_nodes) {
		if (lut_extend(lut, lut->growth_step)) {
			return -ENOMEM;
		}
	}

	node = &lut->nodes[key];

	/* Copy the address out of the node in a single load instruction */
	addr = node->u.addr;

	if (addr.valid) {
		return -EALREADY;
	}

	STAILQ_REMOVE(&lut->free_nodes, node, spdk_lut_node, u.link);

	addr.valid = 1;
	addr.value = (uint64_t)value;

	node->u.addr = addr;

	return 0;
}


void *
spdk_lut_get(struct spdk_lut *lut, uint64_t key)
{
	struct spdk_lut_node *node;
	struct spdk_lut_addr addr;

	if (key >= lut->num_nodes) {
		return SPDK_LUT_INVALID_VALUE;
	}

	node = &lut->nodes[key];

	/* Copy the address out of the node in a single load instruction */
	addr = node->u.addr;

	if (addr.valid) {
		/* This cast is ugly, but it tells the compiler that I do in fact
		 * know what I'm doing. I want to cast a 63 bit value to a 64 bit
		 * value, and then treat that as a pointer. */
		return (void *)(uint64_t)addr.value;
	}

	return SPDK_LUT_INVALID_VALUE;
}

int
spdk_lut_foreach(struct spdk_lut *lut, spdk_lut_foreach_cb cb_fn, void *cb_arg)
{
	struct spdk_lut_node *node;
	struct spdk_lut_addr addr;
	uint64_t key;
	int rc = 0;

	for (key = 0; key < lut->num_nodes ; key++) {
		node = &lut->nodes[key];

		/* Copy the address out of the node in a single load instruction */
		addr = node->u.addr;

		if (!addr.valid) {
			continue;
		}

		rc = cb_fn(cb_arg, key, (void *)(uint64_t)addr.value);
		if (rc) {
			break;
		}
	}

	return rc;
}

int
spdk_lut_reset(struct spdk_lut *lut)
{
	/* Reset the free nodes list */
	STAILQ_INIT(&lut->free_nodes);

	/* Unmap the current nodes */
	munmap(lut->nodes, lut->max_size * sizeof(struct spdk_lut_node));
	lut->nodes = NULL;
	lut->num_nodes = 0;

	return lut_init(lut);
}

int
spdk_lut_remove(struct spdk_lut *lut, uint64_t key)
{
	struct spdk_lut_node *node;
	struct spdk_lut_addr addr;
	bool rc = -ENOENT;

	if (key < lut->num_nodes) {
		node = &lut->nodes[key];

		/* Copy the address out of the node in a single load instruction */
		addr = node->u.addr;

		if (addr.valid) {
			STAILQ_INSERT_HEAD(&lut->free_nodes, node, u.link);
			assert(node->u.addr.valid == 0);
			rc = 0;
		}
	}

	return rc;
}

void
spdk_lut_free(struct spdk_lut *lut)
{
	munmap(lut->nodes, lut->max_size * sizeof(struct spdk_lut_node));
	free(lut);
}
