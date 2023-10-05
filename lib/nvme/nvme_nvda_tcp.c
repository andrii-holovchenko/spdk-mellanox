/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation. All rights reserved.
 *   Copyright (c) 2020 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021, 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * NVMe/NVDA_TCP transport
 */

#include "nvme_internal.h"

#include "spdk/endian.h"
#include "spdk/likely.h"
#include "spdk/string.h"
#include "spdk/stdinc.h"
#include "spdk/crc32.h"
#include "spdk/endian.h"
#include "spdk/assert.h"
#include "spdk/string.h"
#include "spdk/thread.h"
#include "spdk/trace.h"
#include "spdk/util.h"
#include "spdk/bit_pool.h"
#include "spdk/dma.h"
#include "spdk/accel.h"

#include "spdk_internal/nvme_nvda_tcp.h"
#include "spdk_internal/trace_defs.h"
#include "spdk_internal/rdma.h"
#include "spdk_internal/rdma_utils.h"
#include "spdk/accel_module.h"

#define NVME_TCP_RW_BUFFER_SIZE 131072
#define NVME_TCP_TIME_OUT_IN_SECONDS 2

#define NVME_TCP_HPDA_DEFAULT			0
#define NVME_TCP_MAX_R2T_DEFAULT		1
#define NVME_TCP_PDU_H2C_MIN_DATA_SIZE		4096

/*
 * Maximum value of transport_ack_timeout used by TCP controller
 */
#define NVME_TCP_CTRLR_MAX_TRANSPORT_ACK_TIMEOUT	31

typedef TAILQ_HEAD(, nvme_tcp_req)	nvme_tcp_req_tailq_t;

/* NVMe TCP transport extensions for spdk_nvme_ctrlr */
struct nvme_tcp_ctrlr {
	struct spdk_nvme_ctrlr			ctrlr;
};

struct nvme_tcp_poll_group {
	struct spdk_nvme_transport_poll_group group;
	struct spdk_sock_group *sock_group;
	uint32_t completions_per_qpair;
	bool in_polling;
	int64_t num_completions;

	void *tcp_reqs;
	TAILQ_HEAD(, nvme_tcp_pdu) free_pdus;
	void *recv_pdus;
	TAILQ_HEAD(, nvme_tcp_qpair) needs_poll;
	struct spdk_nvme_tcp_stat stats;
};

/* NVMe TCP qpair extensions for spdk_nvme_qpair */
struct nvme_tcp_qpair {
	struct spdk_nvme_qpair			qpair;
	struct spdk_sock			*sock;

	nvme_tcp_req_tailq_t			outstanding_reqs;

	TAILQ_HEAD(, nvme_tcp_pdu)		send_queue;
	struct nvme_tcp_pdu			*recv_pdu;
	void					*_recv_pdu;
	struct nvme_tcp_pdu			*send_pdu; /* only for error pdu and init pdu */
	enum nvme_tcp_pdu_recv_state		recv_state;

	struct spdk_bit_pool			*cid_pool;
	struct nvme_tcp_req			**tcp_reqs_lookup;
	struct ibv_pd				*pd;
	struct spdk_rdma_utils_mem_map		*mem_map;
	struct spdk_rdma_memory_domain		*memory_domain;

	struct nvme_tcp_req			*tcp_reqs;
	struct nvme_tcp_req			*reserved_tcp_req;
	struct spdk_nvme_tcp_stat		*stats;

	uint16_t				num_entries;
	uint16_t				async_complete;

	struct {
		uint16_t host_hdgst_enable: 1;
		uint16_t host_ddgst_enable: 1;
		uint16_t icreq_send_ack: 1;
		uint16_t in_connect_poll: 1;
		uint16_t use_poll_group_req_pool: 1;
		uint16_t reserved: 11;
	} flags;

	/** Specifies the maximum number of PDU-Data bytes per H2C Data Transfer PDU */
	uint32_t				maxh2cdata;

	uint32_t				maxr2t;

	/* 0 based value, which is used to guide the padding */
	uint8_t					cpda;

	enum nvme_tcp_qpair_state		state;
	uint32_t				pdus_mkey;
	TAILQ_ENTRY(nvme_tcp_qpair)		link;
	bool					needs_poll;

	uint64_t				icreq_timeout_tsc;

	bool					shared_stats;
};

enum nvme_tcp_req_state {
	NVME_TCP_REQ_FREE,
	NVME_TCP_REQ_ACTIVE,
	NVME_TCP_REQ_ACTIVE_R2T,
};

struct nvme_tcp_req {
	struct nvme_request			req;
	enum nvme_tcp_req_state			state;
	uint16_t				cid;
	uint16_t				ttag;
	uint32_t				datao;
	uint32_t				expected_datao;
	uint32_t				r2tl_remain;
	uint32_t				active_r2ts;
	/* Used to hold a value received from subsequent R2T while we are still
	 * waiting for H2C complete */
	uint16_t				ttag_r2t_next;
	bool					in_capsule_data;
	/* It is used to track whether the req can be safely freed */
	union {
		uint8_t raw;
		struct {
			/* The last send operation completed - kernel released send buffer */
			uint8_t				send_ack : 1;
			/* Data transfer completed - target send resp or last data bit */
			uint8_t				data_recv : 1;
			/* tcp_req is waiting for completion of the previous send operation (buffer reclaim notification
			 * from kernel) to send H2C */
			uint8_t				h2c_send_waiting_ack : 1;
			/* tcp_req received subsequent r2t while it is still waiting for send_ack.
			 * Rare case, actual when dealing with target that can send several R2T requests.
			 * SPDK TCP target sends 1 R2T for the whole data buffer */
			uint8_t				r2t_waiting_h2c_complete : 1;
			uint8_t				in_progress_accel : 1;
			uint8_t				digest_offloaded : 1;
			uint8_t				reserved : 2;
		} bits;
	} ordering;
	struct nvme_tcp_pdu			pdu;
	struct spdk_iobuf_entry			iobuf_entry;
	struct iovec				iobuf_iov;
	uint32_t				iovcnt;
	/* Used to hold a value received from subsequent R2T while we are still
	 * waiting for H2C ack */
	uint32_t				r2tl_remain_next;
	struct nvme_tcp_qpair			*tqpair;
	TAILQ_ENTRY(nvme_tcp_req)		link;
	struct spdk_nvme_cpl			rsp;
	struct spdk_sock_buf			*sock_buf;
	struct iovec				*iov;
	struct iovec				iovs[NVME_TCP_MAX_SGL_DESCRIPTORS];
};

static struct spdk_nvme_tcp_stat g_dummy_stats = {};

static void nvme_tcp_send_h2c_data(struct nvme_tcp_req *tcp_req);
static int64_t nvme_tcp_poll_group_process_completions(struct spdk_nvme_transport_poll_group
		*tgroup, uint32_t completions_per_qpair, spdk_nvme_disconnected_qpair_cb disconnected_qpair_cb);
static void nvme_tcp_icresp_handle(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_pdu *pdu);
static void nvme_tcp_req_complete(struct nvme_tcp_req *tcp_req, struct nvme_tcp_qpair *tqpair,
				  struct spdk_nvme_cpl *rsp, bool print_on_error);
static struct nvme_tcp_req *get_nvme_active_req_by_cid(struct nvme_tcp_qpair *tqpair, uint32_t cid);
static int nvme_tcp_qpair_free_request(struct spdk_nvme_qpair *qpair, struct nvme_request *req);
static int nvme_tcp_qpair_capsule_cmd_send(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_req *tcp_req);
static bool nvme_tcp_memory_domain_enabled(void);

static inline bool
nvme_tcp_pdu_is_zcopy(struct nvme_tcp_pdu *pdu)
{
	struct nvme_tcp_req *tcp_req = pdu->req;
	return (tcp_req &&
		nvme_payload_type(&tcp_req->req.payload) == NVME_PAYLOAD_TYPE_ZCOPY);
}

static inline bool
nvme_tcp_req_with_memory_domain(struct nvme_tcp_req *tcp_req)
{
	return (tcp_req && tcp_req->req.payload.opts &&
		(tcp_req->req.payload.opts->memory_domain ||
		 tcp_req->req.payload.opts->accel_sequence));
}

static inline struct nvme_tcp_req *
nvme_tcp_req(struct nvme_request *req)
{
	return SPDK_CONTAINEROF(req, struct nvme_tcp_req, req);
}

static inline struct nvme_tcp_qpair *
nvme_tcp_qpair(struct spdk_nvme_qpair *qpair)
{
	assert(qpair->trtype == SPDK_NVME_TRANSPORT_CUSTOM_FABRICS);
	return SPDK_CONTAINEROF(qpair, struct nvme_tcp_qpair, qpair);
}

static inline struct nvme_tcp_poll_group *
nvme_tcp_poll_group(struct spdk_nvme_transport_poll_group *group)
{
	return SPDK_CONTAINEROF(group, struct nvme_tcp_poll_group, group);
}

static inline struct nvme_tcp_ctrlr *
nvme_tcp_ctrlr(struct spdk_nvme_ctrlr *ctrlr)
{
	assert(ctrlr->trid.trtype == SPDK_NVME_TRANSPORT_CUSTOM_FABRICS);
	return SPDK_CONTAINEROF(ctrlr, struct nvme_tcp_ctrlr, ctrlr);
}

static int
nvme_tcp_req_get(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_req *tcp_req)
{
	uint32_t cid;

	if (tqpair->flags.use_poll_group_req_pool) {
		cid = spdk_bit_pool_allocate_bit(tqpair->cid_pool);
		if (cid == UINT32_MAX) {
			return -EAGAIN;
		}

		tcp_req->cid = cid;
		tcp_req->tqpair = tqpair;
	}

	tqpair->tcp_reqs_lookup[tcp_req->cid] = tcp_req;

	assert(tcp_req->state == NVME_TCP_REQ_FREE);
	tcp_req->state = NVME_TCP_REQ_ACTIVE;
	tcp_req->datao = 0;
	tcp_req->expected_datao = 0;
	tcp_req->in_capsule_data = false;
	tcp_req->r2tl_remain = 0;
	tcp_req->r2tl_remain_next = 0;
	tcp_req->active_r2ts = 0;
	tcp_req->iovcnt = 0;
	tcp_req->ordering.raw = 0;
	tcp_req->pdu.data_len = 0;
	tcp_req->pdu.data_iovcnt = 0;
	memset(&tcp_req->rsp, 0, sizeof(struct spdk_nvme_cpl));
	tcp_req->iobuf_iov.iov_base = NULL;
	tcp_req->sock_buf = NULL;

	return 0;
}

static void
nvme_tcp_req_put(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_req *tcp_req)
{
	struct spdk_nvme_transport_poll_group *group = tqpair->qpair.poll_group;

	assert(tcp_req->state != NVME_TCP_REQ_FREE);
	tcp_req->state = NVME_TCP_REQ_FREE;

	tqpair->tcp_reqs_lookup[tcp_req->cid] = NULL;

	if (group && tcp_req->iobuf_iov.iov_base) {
		spdk_iobuf_put(group->group->accel_fn_table.get_iobuf_channel(group->group->ctx),
			       tcp_req->iobuf_iov.iov_base,
			       tcp_req->iobuf_iov.iov_len);
	}

	if (tqpair->flags.use_poll_group_req_pool) {
		spdk_bit_pool_free_bit(tqpair->cid_pool, tcp_req->cid);
		tcp_req->cid = UINT16_MAX;
		tcp_req->tqpair = NULL;
	}
}

static struct nvme_tcp_pdu *
nvme_tcp_recv_pdu_get(struct nvme_tcp_qpair *tqpair)
{
	struct nvme_tcp_pdu *pdu;
	struct spdk_nvme_transport_poll_group *group = tqpair->qpair.poll_group;
	struct nvme_tcp_poll_group *tgroup = nvme_tcp_poll_group(group);

	if (spdk_likely(group && tgroup->recv_pdus)) {
		pdu = TAILQ_FIRST(&tgroup->free_pdus);
		if (!pdu) {
			return NULL;
		}

		TAILQ_REMOVE(&tgroup->free_pdus, pdu, tailq);
	} else {
		pdu = tqpair->_recv_pdu;
	}

	return pdu;
}

static void
nvme_tcp_recv_pdu_put(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_pdu *pdu)
{
	struct spdk_nvme_transport_poll_group *group = tqpair->qpair.poll_group;
	struct nvme_tcp_poll_group *tgroup = nvme_tcp_poll_group(group);

	if (spdk_likely(group && tgroup->recv_pdus)) {
		TAILQ_INSERT_HEAD(&tgroup->free_pdus, pdu, tailq);
		tqpair->recv_pdu = NULL;
	}
}

static inline void
nvme_tcp_qpair_set_recv_state(struct nvme_tcp_qpair *tqpair,
			      enum nvme_tcp_pdu_recv_state state)
{
	if (tqpair->recv_state == state) {
		SPDK_ERRLOG("The recv state of tqpair=%p is same with the state(%d) to be set\n",
			    tqpair, state);
		return;
	}

	if (state == NVME_TCP_PDU_RECV_STATE_ERROR) {
		assert(TAILQ_EMPTY(&tqpair->outstanding_reqs));
	}

	tqpair->recv_state = state;
	if ((state == NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY ||
	     state == NVME_TCP_PDU_RECV_STATE_ERROR) && tqpair->recv_pdu) {
		nvme_tcp_recv_pdu_put(tqpair, tqpair->recv_pdu);
	}
}

static int
nvme_tcp_parse_addr(struct sockaddr_storage *sa, int family, const char *addr, const char *service)
{
	struct addrinfo *res;
	struct addrinfo hints;
	int ret;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = family;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = 0;

	ret = getaddrinfo(addr, service, &hints, &res);
	if (ret) {
		SPDK_ERRLOG("getaddrinfo failed: %s (%d)\n", gai_strerror(ret), ret);
		return ret;
	}

	if (res->ai_addrlen > sizeof(*sa)) {
		SPDK_ERRLOG("getaddrinfo() ai_addrlen %zu too large\n", (size_t)res->ai_addrlen);
		ret = -EINVAL;
	} else {
		memcpy(sa, res->ai_addr, res->ai_addrlen);
	}

	freeaddrinfo(res);
	return ret;
}

static void
nvme_tcp_free_reqs(struct nvme_tcp_qpair *tqpair)
{
	spdk_free(tqpair->tcp_reqs);
	tqpair->tcp_reqs = NULL;
	spdk_free(tqpair->reserved_tcp_req);
	tqpair->reserved_tcp_req = NULL;

	spdk_free(tqpair->send_pdu);
	tqpair->send_pdu = NULL;
	spdk_free(tqpair->_recv_pdu);
	tqpair->_recv_pdu = NULL;

	free(tqpair->tcp_reqs_lookup);
	spdk_bit_pool_free(&tqpair->cid_pool);
}

static int
nvme_tcp_alloc_reqs(struct nvme_tcp_qpair *tqpair)
{
	size_t req_size_padded;
	uint16_t i;
	struct nvme_tcp_req	*tcp_req;

	req_size_padded = SPDK_ALIGN_CEIL(sizeof(struct nvme_tcp_req), 64);

	tqpair->tcp_reqs = spdk_zmalloc(tqpair->num_entries * req_size_padded, 64, NULL,
					SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	if (tqpair->tcp_reqs == NULL) {
		SPDK_ERRLOG("Failed to allocate tcp_reqs on tqpair=%p\n", tqpair);
		goto fail;
	}

	tqpair->reserved_tcp_req = spdk_zmalloc(req_size_padded, 64, NULL,
						SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	if (tqpair->reserved_tcp_req == NULL) {
		goto fail;
	}

	tqpair->send_pdu = spdk_zmalloc(sizeof(struct nvme_tcp_pdu), 0x1000, NULL,
					SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	if (tqpair->send_pdu == NULL) {
		goto fail;
	}

	tqpair->_recv_pdu = spdk_zmalloc(sizeof(struct nvme_tcp_pdu), 0x1000, NULL,
					 SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	if (tqpair->_recv_pdu == NULL) {
		goto fail;
	}

	tqpair->cid_pool = spdk_bit_pool_create(tqpair->num_entries);
	tqpair->tcp_reqs_lookup = calloc(tqpair->num_entries, sizeof(*tqpair->tcp_reqs_lookup));
	TAILQ_INIT(&tqpair->send_queue);
	TAILQ_INIT(&tqpair->outstanding_reqs);
	for (i = 0; i < tqpair->num_entries; i++) {
		tcp_req = &tqpair->tcp_reqs[i];
		tcp_req->cid = i;
		tcp_req->tqpair = tqpair;
		tcp_req->req.qpair = &tqpair->qpair;
		STAILQ_INSERT_HEAD(&tqpair->qpair.free_req, &tcp_req->req, stailq);
		tcp_req->pdu.sock_req.mkeys = tcp_req->pdu.mkeys;
	}

	tcp_req = tqpair->reserved_tcp_req;

	tcp_req->tqpair = tqpair;
	tcp_req->req.qpair = &tqpair->qpair;

	tqpair->qpair.reserved_req = &tcp_req->req;

	tqpair->send_pdu->sock_req.mkeys = tqpair->send_pdu->mkeys;

	tqpair->qpair.active_free_req = &tqpair->qpair.free_req;

	return 0;
fail:
	nvme_tcp_free_reqs(tqpair);
	return -ENOMEM;
}

static void nvme_tcp_qpair_abort_reqs(struct spdk_nvme_qpair *qpair, uint32_t dnr);

static void
nvme_tcp_ctrlr_disconnect_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(qpair);
	struct nvme_tcp_pdu *pdu;
	int rc;
	struct nvme_tcp_poll_group *group;

	if (tqpair->needs_poll) {
		group = nvme_tcp_poll_group(qpair->poll_group);
		TAILQ_REMOVE(&group->needs_poll, tqpair, link);
		tqpair->needs_poll = false;
	}

	if (qpair->outstanding_zcopy_reqs == 0) {
		rc = spdk_sock_close(&tqpair->sock);

		if (tqpair->sock != NULL) {
			SPDK_ERRLOG("tqpair=%p, errno=%d, rc=%d\n", tqpair, errno, rc);
			/* Set it to NULL manually */
			tqpair->sock = NULL;
		}
	} else {
		SPDK_NOTICELOG("Cannot close socket for qpair %u because %d zcopy reqs is pending.\n",
			       qpair->id, qpair->outstanding_zcopy_reqs);
	}

	/* clear the send_queue */
	while (!TAILQ_EMPTY(&tqpair->send_queue)) {
		pdu = TAILQ_FIRST(&tqpair->send_queue);
		/* Remove the pdu from the send_queue to prevent the wrong sending out
		 * in the next round connection
		 */
		TAILQ_REMOVE(&tqpair->send_queue, pdu, tailq);
	}

	nvme_tcp_qpair_abort_reqs(qpair, 0);
	nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_QUIESCING);
}

static int
nvme_tcp_ctrlr_delete_io_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	struct nvme_tcp_qpair *tqpair;

	assert(qpair != NULL);
	tqpair = nvme_tcp_qpair(qpair);
	nvme_tcp_qpair_abort_reqs(qpair, 0);
	assert(TAILQ_EMPTY(&tqpair->outstanding_reqs));
	assert(tqpair->qpair.num_outstanding_reqs == 0);
	qpair->reserved_req = NULL;
	nvme_qpair_deinit(qpair);
	nvme_tcp_free_reqs(tqpair);
	if (!tqpair->shared_stats) {
		free(tqpair->stats);
	}
	spdk_rdma_utils_free_mem_map(&tqpair->mem_map);
	spdk_rdma_put_memory_domain(tqpair->memory_domain);
	free(tqpair);

	return 0;
}

static int
nvme_tcp_ctrlr_enable(struct spdk_nvme_ctrlr *ctrlr)
{
	return 0;
}

static int
nvme_tcp_ctrlr_destruct(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_tcp_ctrlr *tctrlr = nvme_tcp_ctrlr(ctrlr);

	if (ctrlr->adminq) {
		nvme_tcp_ctrlr_delete_io_qpair(ctrlr, ctrlr->adminq);
	}

	nvme_ctrlr_destruct_finish(ctrlr);

	free(tctrlr);

	return 0;
}

static void
_pdu_write_done(void *cb_arg, int err)
{
	struct nvme_tcp_pdu *pdu = cb_arg;
	struct nvme_tcp_qpair *tqpair = pdu->qpair;
	struct nvme_tcp_poll_group *pgroup;

	/* If there are queued requests, we assume they are queued because they are waiting
	 * for resources to be released. Those resources are almost certainly released in
	 * response to a PDU completing here. However, to attempt to make forward progress
	 * the qpair needs to be polled and we can't rely on another network event to make
	 * that happen. Add it to a list of qpairs to poll regardless of network activity
	 * here.
	 * Besides, when tqpair state is NVME_TCP_QPAIR_STATE_FABRIC_CONNECT_POLL or
	 * NVME_TCP_QPAIR_STATE_INITIALIZING, need to add it to needs_poll list too to make
	 * forward progress in case that the resources are released after icreq's or CONNECT's
	 * resp is processed. */
	if (tqpair->qpair.poll_group && !tqpair->needs_poll && (!STAILQ_EMPTY(&tqpair->qpair.queued_req) ||
			tqpair->state == NVME_TCP_QPAIR_STATE_FABRIC_CONNECT_POLL ||
			tqpair->state == NVME_TCP_QPAIR_STATE_INITIALIZING)) {
		pgroup = nvme_tcp_poll_group(tqpair->qpair.poll_group);

		TAILQ_INSERT_TAIL(&pgroup->needs_poll, tqpair, link);
		tqpair->needs_poll = true;
	}

	TAILQ_REMOVE(&tqpair->send_queue, pdu, tailq);

	if (err != 0) {
		nvme_transport_ctrlr_disconnect_qpair(tqpair->qpair.ctrlr, &tqpair->qpair);
		return;
	}

	assert(pdu->cb_fn != NULL);
	pdu->cb_fn(pdu->cb_arg);
}

static void
_tcp_write_pdu(struct nvme_tcp_pdu *pdu)
{
	uint32_t mapped_length = 0;
	struct nvme_tcp_qpair *tqpair = pdu->qpair;

	pdu->sock_req.iovcnt = nvme_tcp_build_iovs(pdu->iov, SPDK_COUNTOF(pdu->iov), pdu,
			       (bool)tqpair->flags.host_hdgst_enable, (bool)tqpair->flags.host_ddgst_enable,
			       &mapped_length);
	pdu->sock_req.cb_fn = _pdu_write_done;
	pdu->sock_req.cb_arg = pdu;
	TAILQ_INSERT_TAIL(&tqpair->send_queue, pdu, tailq);
	tqpair->stats->submitted_requests++;
	spdk_sock_writev_async(tqpair->sock, &pdu->sock_req);
}

static void
data_crc32_accel_done(void *cb_arg, int status)
{
	struct nvme_tcp_pdu *pdu = cb_arg;

	if (spdk_unlikely(status)) {
		SPDK_ERRLOG("Failed to compute the data digest for pdu =%p\n", pdu);
		_pdu_write_done(pdu, status);
		return;
	}

	pdu->data_digest_crc32 ^= SPDK_CRC32C_XOR;
	MAKE_DIGEST_WORD(pdu->data_digest, pdu->data_digest_crc32);

	_tcp_write_pdu(pdu);
}

static uint32_t
nvme_tcp_pdu_calc_data_digest_with_iov(struct nvme_tcp_pdu *pdu, struct iovec *iovs, int iovcnt)
{
	uint32_t crc32c = SPDK_CRC32C_XOR;
	uint32_t mod;

	assert(pdu->data_len != 0);

	crc32c = spdk_crc32c_iov_update(iovs, iovcnt, crc32c);
	mod = pdu->data_len % SPDK_NVME_TCP_DIGEST_ALIGNMENT;
	if (mod != 0) {
		uint32_t pad_length = SPDK_NVME_TCP_DIGEST_ALIGNMENT - mod;
		uint8_t pad[3] = {0, 0, 0};

		assert(pad_length > 0);
		assert(pad_length <= sizeof(pad));
		crc32c = spdk_crc32c_update(pad, pad_length, crc32c);
	}
	crc32c = crc32c ^ SPDK_CRC32C_XOR;
	return crc32c;
}

static uint32_t
nvme_tcp_pdu_calc_data_digest_with_sock_buf(struct nvme_tcp_pdu *pdu)
{
	struct nvme_tcp_req *tcp_req = pdu->req;
	struct spdk_sock_buf *sock_buf = tcp_req->sock_buf;
	uint32_t crc32c = SPDK_CRC32C_XOR;
	uint32_t mod;

	assert(pdu->data_len != 0);
	while (sock_buf) {
		crc32c = spdk_crc32c_iov_update(&sock_buf->iov, 1, crc32c);
		sock_buf = sock_buf->next;
	}

	mod = pdu->data_len % SPDK_NVME_TCP_DIGEST_ALIGNMENT;
	if (mod != 0) {
		uint32_t pad_length = SPDK_NVME_TCP_DIGEST_ALIGNMENT - mod;
		uint8_t pad[3] = {0, 0, 0};

		assert(pad_length > 0);
		assert(pad_length <= sizeof(pad));
		crc32c = spdk_crc32c_update(pad, pad_length, crc32c);
	}

	crc32c = crc32c ^ SPDK_CRC32C_XOR;
	return crc32c;
}

static void
pdu_data_crc32_compute(struct nvme_tcp_pdu *pdu)
{
	struct nvme_tcp_qpair *tqpair = pdu->qpair;
	uint32_t crc32c = 0;
	struct nvme_tcp_poll_group *tgroup = nvme_tcp_poll_group(tqpair->qpair.poll_group);

	/* Data Digest */
	if (pdu->data_len > 0 && g_nvme_tcp_ddgst[pdu->hdr.common.pdu_type] &&
	    tqpair->flags.host_ddgst_enable) {
		/* Only support this limited case for the first step */
		/* @todo: add support for crc32 accelerator in zcopy flow */
		if ((nvme_qpair_get_state(&tqpair->qpair) >= NVME_QPAIR_CONNECTED) &&
		    (tgroup != NULL && tgroup->group.group->accel_fn_table.submit_accel_crc32c) &&
		    spdk_likely(pdu->data_len % SPDK_NVME_TCP_DIGEST_ALIGNMENT == 0) &&
		    !nvme_tcp_pdu_is_zcopy(pdu)) {
			tgroup->group.group->accel_fn_table.submit_accel_crc32c(tgroup->group.group->ctx,
					&pdu->data_digest_crc32, pdu->data_iov,
					pdu->data_iovcnt, 0, data_crc32_accel_done, pdu);
			return;
		}

		if (spdk_unlikely(nvme_tcp_pdu_is_zcopy(pdu))) {
			/* zcopy write does not support yet */
			assert(0);
		} else {
			crc32c = nvme_tcp_pdu_calc_data_digest_with_iov(pdu, pdu->data_iov, pdu->data_iovcnt);
		}
		MAKE_DIGEST_WORD(pdu->data_digest, crc32c);
		tqpair->stats->send_ddgsts++;
	}

	_tcp_write_pdu(pdu);
}

static int
nvme_tcp_qpair_write_pdu(struct nvme_tcp_qpair *tqpair,
			 struct nvme_tcp_pdu *pdu,
			 nvme_tcp_qpair_xfer_complete_cb cb_fn,
			 void *cb_arg)
{
	int hlen;
	uint32_t crc32c;

	hlen = pdu->hdr.common.hlen;
	pdu->cb_fn = cb_fn;
	pdu->cb_arg = cb_arg;
	pdu->qpair = tqpair;

	/* Header Digest */
	if (g_nvme_tcp_hdgst[pdu->hdr.common.pdu_type] && tqpair->flags.host_hdgst_enable) {
		crc32c = nvme_tcp_pdu_calc_header_digest(pdu);
		MAKE_DIGEST_WORD((uint8_t *)pdu->hdr.raw + hlen, crc32c);
	}

	pdu_data_crc32_compute(pdu);

	return 0;
}

/*
 * Build SGL describing contiguous payload buffer.
 */
static int
nvme_tcp_build_contig_request(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_req *tcp_req)
{
	struct nvme_request *req = &tcp_req->req;

	tcp_req->iov = tcp_req->iovs;
	tcp_req->iov[0].iov_base = req->payload.contig_or_cb_arg + req->payload_offset;
	tcp_req->iov[0].iov_len = req->payload_size;
	tcp_req->iovcnt = 1;

	SPDK_DEBUGLOG(nvme, "enter\n");

	assert(nvme_payload_type(&req->payload) == NVME_PAYLOAD_TYPE_CONTIG);

	return 0;
}

/*
 * Build SGL describing scattered payload buffer.
 */
static int
nvme_tcp_build_sgl_request(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_req *tcp_req)
{
	int rc;
	uint32_t length, remaining_size, iovcnt = 0, max_num_sgl;
	struct nvme_request *req = &tcp_req->req;

	SPDK_DEBUGLOG(nvme, "enter\n");

	assert(req->payload_size != 0);
	assert(nvme_payload_type(&req->payload) == NVME_PAYLOAD_TYPE_SGL);
	assert(req->payload.reset_sgl_fn != NULL);
	assert(req->payload.next_sge_fn != NULL);
	req->payload.reset_sgl_fn(req->payload.contig_or_cb_arg, req->payload_offset);

	max_num_sgl = spdk_min(req->qpair->ctrlr->max_sges, NVME_TCP_MAX_SGL_DESCRIPTORS);
	remaining_size = req->payload_size;

	tcp_req->iov = tcp_req->iovs;
	do {
		rc = req->payload.next_sge_fn(req->payload.contig_or_cb_arg, &tcp_req->iov[iovcnt].iov_base,
					      &length);
		if (rc) {
			return -1;
		}

		length = spdk_min(length, remaining_size);
		tcp_req->iov[iovcnt].iov_len = length;
		remaining_size -= length;
		iovcnt++;
	} while (remaining_size > 0 && iovcnt < max_num_sgl);


	/* Should be impossible if we did our sgl checks properly up the stack, but do a sanity check here. */
	if (remaining_size > 0) {
		SPDK_ERRLOG("Failed to construct tcp_req=%p, and the iovcnt=%u, remaining_size=%u\n",
			    tcp_req, iovcnt, remaining_size);
		return -1;
	}

	tcp_req->iovcnt = iovcnt;

	return 0;
}

static int
nvme_tcp_build_sgl_passthru_request(struct nvme_tcp_req *tcp_req)
{
	struct nvme_request *req = &tcp_req->req;

	SPDK_DEBUGLOG(nvme, "enter\n");

	assert(nvme_payload_type(&req->payload) == NVME_PAYLOAD_TYPE_SGL);
	assert(req->payload.opts != NULL);
	assert(req->payload.opts->iov != NULL);
	assert(req->payload.opts->iovcnt != 0);

	tcp_req->iov = req->payload.opts->iov;
	tcp_req->iovcnt = req->payload.opts->iovcnt;

	return 0;
}

static inline int
nvme_tcp_build_zcopy_request(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_req *tcp_req)
{
#ifdef DEBUG
	struct nvme_request *req = &tcp_req->req;
#endif

	assert(nvme_payload_type(&req->payload) == NVME_PAYLOAD_TYPE_ZCOPY);
	tcp_req->iov = tcp_req->iovs;
	tcp_req->iovcnt = 0;
	return 0;
}

static int
nvme_tcp_req_build(struct nvme_tcp_req *tcp_req)
{
	struct nvme_request *req = &tcp_req->req;
	struct nvme_tcp_qpair *tqpair = tcp_req->tqpair;
	int rc;
	enum nvme_payload_type payload_type = nvme_payload_type(&req->payload);
	enum spdk_nvme_data_transfer xfer;

	req->cmd.psdt = SPDK_NVME_PSDT_SGL_MPTR_CONTIG;
	req->cmd.dptr.sgl1.unkeyed.type = SPDK_NVME_SGL_TYPE_TRANSPORT_DATA_BLOCK;
	req->cmd.dptr.sgl1.unkeyed.subtype = SPDK_NVME_SGL_SUBTYPE_TRANSPORT;
	req->cmd.dptr.sgl1.unkeyed.length = req->payload_size;

	switch (payload_type) {
	case NVME_PAYLOAD_TYPE_CONTIG:
		rc = nvme_tcp_build_contig_request(tqpair, tcp_req);
		break;
	case NVME_PAYLOAD_TYPE_SGL:
		if (req->payload.opts != NULL && req->payload.opts->iov != NULL) {
			rc = nvme_tcp_build_sgl_passthru_request(tcp_req);
		} else {
			rc = nvme_tcp_build_sgl_request(tqpair, tcp_req);
		}
		break;
	case NVME_PAYLOAD_TYPE_ZCOPY:
		rc = nvme_tcp_build_zcopy_request(tqpair, tcp_req);
		break;
	default:
		rc = -1;
	}

	if (rc) {
		return rc;
	}

	xfer = spdk_nvmf_cmd_get_data_transfer(&req->cmd);
	if (xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER) {
		struct spdk_nvme_ctrlr *ctrlr = tqpair->qpair.ctrlr;
		uint32_t max_in_capsule_data_size = ctrlr->ioccsz_bytes;
		if ((req->cmd.opc == SPDK_NVME_OPC_FABRIC) || nvme_qpair_is_admin_queue(&tqpair->qpair)) {
			max_in_capsule_data_size = SPDK_NVME_TCP_IN_CAPSULE_DATA_MAX_SIZE;
		}

		if (req->payload_size <= max_in_capsule_data_size) {
			req->cmd.dptr.sgl1.unkeyed.type = SPDK_NVME_SGL_TYPE_DATA_BLOCK;
			req->cmd.dptr.sgl1.unkeyed.subtype = SPDK_NVME_SGL_SUBTYPE_OFFSET;
			req->cmd.dptr.sgl1.address = 0;
			tcp_req->in_capsule_data = true;
		}
	}

	return 0;
}

static void
_nvme_tcp_accel_finished_in_capsule(void *cb_arg, int status)
{
	struct nvme_tcp_req *tcp_req = cb_arg;
	struct nvme_tcp_qpair *tqpair = tcp_req->tqpair;
	struct spdk_nvme_cpl cpl;
	enum spdk_nvme_generic_command_status_code sc;
	int rc;

	SPDK_DEBUGLOG(nvme, "accel cpl, req %p, status %d\n", tcp_req, status);
	assert(tcp_req->ordering.bits.in_progress_accel);
	tcp_req->ordering.bits.in_progress_accel = 0;

	if (spdk_unlikely(status)) {
		SPDK_ERRLOG("tqair %p, req %p, accel sequence status %d\n", tqpair, tcp_req, status);
		sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		goto fail_req;
	}
	if (spdk_unlikely(tqpair->recv_state == NVME_TCP_PDU_RECV_STATE_QUIESCING ||
		!spdk_nvme_qpair_is_connected(&tqpair->qpair))) {
		SPDK_DEBUGLOG(nvme, "tqpair %p, req %p accel cpl in disconnecting, outstanding %u\n",
			    tqpair, tcp_req, tqpair->qpair.num_outstanding_reqs);
		sc = SPDK_NVME_SC_ABORTED_SQ_DELETION;
		goto fail_req;
	}

	/* Once copy task is finished, we use a single staging buffer.
	 * To reuse existing functions to build a capsule, remove reset_sgl_fn since
	 * it is not needed any more and overwrite contig_or_cb_arg with address of the
	 * staging buffer */
	tcp_req->req.payload.reset_sgl_fn = NULL;
	tcp_req->req.payload.contig_or_cb_arg = tcp_req->iobuf_iov.iov_base;
	tcp_req->req.payload_offset = 0;
	/* Buffer is in local memory, clear memory domain pointer */
	tcp_req->req.payload.opts->memory_domain = NULL;

	/* At this point tcp_req->iovs point to outdated values */
	nvme_tcp_build_contig_request(tcp_req->tqpair, tcp_req);

	tqpair->stats->outstanding_reqs++;
	rc = nvme_tcp_qpair_capsule_cmd_send(tqpair, tcp_req);
	if (spdk_unlikely(rc)) {
		SPDK_ERRLOG("tqpair %p, req %p, failed to send cmd rc %d\n", tqpair, tcp_req, rc);
		sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		goto fail_req;
	}

	return;

fail_req:
	memset(&cpl, 0, sizeof(cpl));
	cpl.status.sc = sc;
	cpl.status.sct = SPDK_NVME_SCT_GENERIC;
	cpl.status.dnr = 0;
	nvme_tcp_req_complete(tcp_req, tqpair, &cpl, true);
}

static inline int
nvme_tcp_apply_accel_sequence_in_capsule(struct nvme_tcp_req *tcp_req)
{
	struct nvme_request *req = &tcp_req->req;
	struct spdk_nvme_poll_group *group = tcp_req->tqpair->qpair.poll_group->group;
	struct spdk_accel_sequence *accel_seq;
	struct spdk_io_channel *accel_ch;
	struct spdk_accel_task *task;
	bool skip_copy = false;
	int rc;

	SPDK_DEBUGLOG(nvme, "Write request with accel sequence: tcp_req %p\n", tcp_req);
	assert(req->payload.opts);
	accel_seq = req->payload.opts->accel_sequence;
	if (accel_seq) {
		task = spdk_accel_sequence_first_task(accel_seq);
		if (task->op_code == ACCEL_OPC_ENCRYPT && spdk_accel_sequence_next_task(task) == NULL) {
			task->dst_domain = NULL;
			task->dst_domain_ctx = NULL;
			task->d.iovs = &tcp_req->iobuf_iov;
			task->d.iovcnt = 1;
			skip_copy = true;
		}
	}

	accel_ch = group->accel_fn_table.get_accel_channel(group->ctx);
	assert(accel_ch);
	if (tcp_req->tqpair->flags.host_ddgst_enable) {
		if (!skip_copy) {
			rc = spdk_accel_append_copy_crc32c(&accel_seq, accel_ch,
							   (uint32_t *)tcp_req->pdu.data_digest,
							   &tcp_req->iobuf_iov, 1, NULL, NULL,
							   tcp_req->iov, tcp_req->iovcnt,
							   req->payload.opts->memory_domain,
							   req->payload.opts->memory_domain_ctx,
							   SPDK_CRC32C_XOR, NULL, NULL);
			skip_copy = true;
		} else {
			rc = spdk_accel_append_crc32c(&accel_seq, accel_ch, (uint32_t *)tcp_req->pdu.data_digest,
						      &tcp_req->iobuf_iov, 1, NULL, NULL, SPDK_CRC32C_XOR, NULL, NULL);
		}
		if (spdk_unlikely(rc)) {
			SPDK_ERRLOG("Failed to append crc32 accel task, rc %d\n", rc);
			return rc;
		}
		tcp_req->ordering.bits.digest_offloaded = 1;
	}

	if (!skip_copy) {
		rc = spdk_accel_append_copy(&accel_seq, accel_ch, &tcp_req->iobuf_iov, 1, NULL, NULL, tcp_req->iov,
					    tcp_req->iovcnt, req->payload.opts->memory_domain,
					    req->payload.opts->memory_domain_ctx, 0, NULL, NULL);
		if (spdk_unlikely(rc)) {
			return rc;
		}
	}

	spdk_accel_sequence_finish(accel_seq, _nvme_tcp_accel_finished_in_capsule, tcp_req);
	TAILQ_INSERT_TAIL(&tcp_req->tqpair->outstanding_reqs, tcp_req, link);
	tcp_req->ordering.bits.in_progress_accel = 1;
	return -EINPROGRESS;
}

static void
nvme_tcp_iobuf_get_cb(struct spdk_iobuf_entry *entry, void *buf)
{
	struct nvme_tcp_req *tcp_req = SPDK_CONTAINEROF(entry, struct nvme_tcp_req, iobuf_entry);
	int rc;

	tcp_req->iobuf_iov.iov_base = buf;
	rc = nvme_tcp_apply_accel_sequence_in_capsule(tcp_req);

	if (spdk_unlikely(rc != -EINPROGRESS)) {
		struct spdk_nvme_cpl cpl;

		SPDK_ERRLOG("failed to apply sequence, rc %d\n", rc);
		assert(rc != 0);

		cpl.status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		cpl.status.sct = SPDK_NVME_SCT_GENERIC;
		cpl.status.dnr = 1;
		nvme_tcp_req_complete(tcp_req, tcp_req->tqpair, &cpl, true);
	}
}

static int
nvme_tcp_req_init(struct nvme_tcp_qpair *tqpair, struct nvme_request *req,
		  struct nvme_tcp_req *tcp_req)
{
	req->cmd.cid = tcp_req->cid;

	return nvme_tcp_req_build(tcp_req);
}

static inline bool
nvme_tcp_req_complete_safe(struct nvme_tcp_req *tcp_req)
{
	if (!(tcp_req->ordering.bits.send_ack && tcp_req->ordering.bits.data_recv)) {
		return false;
	}

	assert(tcp_req->state == NVME_TCP_REQ_ACTIVE);
	assert(tcp_req->tqpair != NULL);

	SPDK_DEBUGLOG(nvme, "complete tcp_req(%p) on tqpair=%p\n", tcp_req, tcp_req->tqpair);

	if (!tcp_req->tqpair->qpair.in_completion_context) {
		tcp_req->tqpair->async_complete++;
	}

	nvme_tcp_req_complete(tcp_req, tcp_req->tqpair, &tcp_req->rsp, true);
	return true;
}

static void
nvme_tcp_qpair_cmd_send_complete(void *cb_arg)
{
	struct nvme_tcp_req *tcp_req = cb_arg;

	SPDK_DEBUGLOG(nvme, "tcp req %p, cid %u, qid %u\n", tcp_req, tcp_req->cid,
		      tcp_req->tqpair->qpair.id);
	tcp_req->ordering.bits.send_ack = 1;
	/* Handle the r2t case */
	if (spdk_unlikely(tcp_req->ordering.bits.h2c_send_waiting_ack)) {
		SPDK_DEBUGLOG(nvme, "tcp req %p, send H2C data\n", tcp_req);
		nvme_tcp_send_h2c_data(tcp_req);
	} else {
		nvme_tcp_req_complete_safe(tcp_req);
	}
}

static inline void
nvme_tcp_qpair_prepare_pdu(struct nvme_tcp_qpair *tqpair,
			   struct nvme_tcp_pdu *pdu,
			   nvme_tcp_qpair_xfer_complete_cb cb_fn,
			   void *cb_arg)
{
	struct nvme_tcp_req *tcp_req = cb_arg;
	int hlen;
	uint32_t crc32c;
	uint32_t mapped_length = 0;

	hlen = pdu->hdr.common.hlen;

	/* Header Digest */
	if (tqpair->flags.host_hdgst_enable && g_nvme_tcp_hdgst[pdu->hdr.common.pdu_type]) {
		crc32c = nvme_tcp_pdu_calc_header_digest(pdu);
		MAKE_DIGEST_WORD((uint8_t *)pdu->hdr.raw + hlen, crc32c);
	}

	/* Data Digest */
	if (pdu->data_len > 0 && tqpair->flags.host_ddgst_enable && g_nvme_tcp_ddgst[pdu->hdr.common.pdu_type]) {
		if (!tcp_req->ordering.bits.digest_offloaded) {
			crc32c = nvme_tcp_pdu_calc_data_digest_with_iov(pdu, pdu->data_iov, pdu->data_iovcnt);
			MAKE_DIGEST_WORD(pdu->data_digest, crc32c);
		}
		tqpair->stats->send_ddgsts++;
	}

	pdu->cb_fn = cb_fn;
	pdu->cb_arg = cb_arg;

	pdu->sock_req.iovcnt = nvme_tcp_build_iovs(pdu->iov, SPDK_COUNTOF(pdu->iov), pdu,
			       (bool)tqpair->flags.host_hdgst_enable, (bool)tqpair->flags.host_ddgst_enable,
			       &mapped_length);
	pdu->qpair = tqpair;
	pdu->sock_req.cb_fn = _pdu_write_done;
	pdu->sock_req.cb_arg = pdu;
	TAILQ_INSERT_TAIL(&tqpair->send_queue, pdu, tailq);
}

static inline int
nvme_tcp_get_memory_translation(struct nvme_request *req, struct nvme_tcp_qpair *tqpair,
				struct spdk_rdma_memory_translation_ctx *_ctx)
{
	struct spdk_memory_domain_translation_result dma_translation;
	struct spdk_rdma_utils_memory_translation rdma_translation;
	int rc;

	if (req && req->payload.opts && req->payload.opts->memory_domain) {
		assert(_ctx);
		struct ibv_qp dst_qp = {
			.pd = tqpair->pd
		};
		struct spdk_memory_domain_translation_ctx dst_domain_ctx = {
				.size = sizeof(struct spdk_memory_domain_translation_ctx),
				.rdma.ibv_qp = &dst_qp
		};
		dma_translation.size = sizeof(struct spdk_memory_domain_translation_result);

		rc = spdk_memory_domain_translate_data(req->payload.opts->memory_domain,
						       req->payload.opts->memory_domain_ctx,
						       tqpair->memory_domain->domain, &dst_domain_ctx,
						       _ctx->addr, _ctx->length, &dma_translation);
		if (spdk_unlikely(rc)) {
			SPDK_ERRLOG("DMA memory translation failed, rc %d\n", rc);
			return rc;
		} else if (spdk_unlikely(dma_translation.iov_count != 1)) {
			SPDK_ERRLOG("Translation to multiple iovs is not supported, iov count %u\n",
				    dma_translation.iov_count);
			return -ENOTSUP;
		}

		_ctx->lkey = dma_translation.rdma.lkey;
		_ctx->rkey = dma_translation.rdma.rkey;
		_ctx->addr = dma_translation.iov.iov_base;
		_ctx->length = dma_translation.iov.iov_len;
	} else {
		rc = spdk_rdma_utils_get_translation(tqpair->mem_map, _ctx->addr, _ctx->length, &rdma_translation);
		if (spdk_unlikely(rc)) {
			SPDK_ERRLOG("RDMA memory translation failed, rc %d\n", rc);
			return rc;
		}
		if (rdma_translation.translation_type == SPDK_RDMA_UTILS_TRANSLATION_MR) {
			_ctx->lkey = rdma_translation.mr_or_key.mr->lkey;
			_ctx->rkey = rdma_translation.mr_or_key.mr->rkey;
		} else {
			_ctx->lkey = _ctx->rkey = (uint32_t)rdma_translation.mr_or_key.key;
		}
	}

	return 0;
}

static inline int
nvme_tcp_fill_mkeys(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_req *tcp_req,
		    struct nvme_tcp_pdu *pdu)
{
	if (!tqpair->mem_map) {
		return 0;
	}

	int sock_iovs, rc;
	int data_iovcnt = pdu->sock_req.iovcnt;

	/* the first element is always a capsule cmd allocated from huge pages, use standard
	 * memory translation */
	pdu->mkeys[0] = tqpair->pdus_mkey;

	if (pdu->hdr.common.flags & SPDK_NVME_TCP_CH_FLAGS_DDGSTF) {
		/* The last element is a data digest which is located in the pdu structure. */
		pdu->mkeys[data_iovcnt - 1] = tqpair->pdus_mkey;
		data_iovcnt--;
	}

	for (sock_iovs = 1; sock_iovs < data_iovcnt; sock_iovs++) {
		struct spdk_rdma_memory_translation_ctx ctx = {
				. addr = pdu->iov[sock_iovs].iov_base,
				.length = pdu->iov[sock_iovs].iov_len};

		assert(tcp_req != NULL);
		rc = nvme_tcp_get_memory_translation(&tcp_req->req, tqpair, &ctx);

		if (spdk_unlikely(rc)) {
			SPDK_ERRLOG("Memory translation failed, rc %d", rc);
			return rc;
		}

		pdu->mkeys[sock_iovs] = ctx.lkey;
		assert(ctx.lkey);
	}

	return 0;
}

static int
nvme_tcp_qpair_capsule_cmd_send(struct nvme_tcp_qpair *tqpair,
				struct nvme_tcp_req *tcp_req)
{
	struct nvme_tcp_pdu *pdu;
	struct spdk_nvme_tcp_cmd *capsule_cmd;
	uint32_t plen = 0, alignment;
	bool has_memory_domain;
	uint8_t pdo;

	SPDK_DEBUGLOG(nvme, "enter\n");
	pdu = &tcp_req->pdu;

	capsule_cmd = &pdu->hdr.capsule_cmd;
	capsule_cmd->common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_CAPSULE_CMD;
	plen = capsule_cmd->common.hlen = sizeof(*capsule_cmd);
	capsule_cmd->ccsqe = tcp_req->req.cmd;

	SPDK_DEBUGLOG(nvme, "capsule_cmd cid=%u on tqpair(%p)\n", tcp_req->req.cmd.cid, tqpair);

	if (tqpair->flags.host_hdgst_enable) {
		SPDK_DEBUGLOG(nvme, "Header digest is enabled for capsule command on tcp_req=%p\n",
			      tcp_req);
		capsule_cmd->common.flags |= SPDK_NVME_TCP_CH_FLAGS_HDGSTF;
		plen += SPDK_NVME_TCP_DIGEST_LEN;
	}

	if ((tcp_req->req.payload_size == 0) || !tcp_req->in_capsule_data) {
		goto end;
	}

	pdo = plen;
	pdu->padding_len = 0;
	if (tqpair->cpda) {
		alignment = (tqpair->cpda + 1) << 2;
		if (alignment > plen) {
			pdu->padding_len = alignment - plen;
			pdo = alignment;
			plen = alignment;
		}
	}

	capsule_cmd->common.pdo = pdo;
	plen += tcp_req->req.payload_size;
	if (tqpair->flags.host_ddgst_enable) {
		capsule_cmd->common.flags |= SPDK_NVME_TCP_CH_FLAGS_DDGSTF;
		plen += SPDK_NVME_TCP_DIGEST_LEN;
	}

	tcp_req->datao = 0;
	nvme_tcp_pdu_set_data_buf(pdu, tcp_req->iov, tcp_req->iovcnt,
				  0, tcp_req->req.payload_size);
end:
	capsule_cmd->common.plen = plen;

	nvme_tcp_qpair_prepare_pdu(tqpair, pdu, nvme_tcp_qpair_cmd_send_complete, tcp_req);
	if (nvme_tcp_fill_mkeys(tqpair, tcp_req, pdu) != 0) {
		return -1;
	}

	/* Let socket layer know we have memory domain and data that has to be sent with
	 * full zcopy. */
	has_memory_domain = tcp_req->req.payload.opts && tcp_req->req.payload.opts->memory_domain;
	pdu->sock_req.has_memory_domain_data = has_memory_domain && tcp_req->in_capsule_data;
	tqpair->stats->submitted_requests++;
	spdk_sock_writev_async(tqpair->sock, &pdu->sock_req);

	return 0;
}

static int
nvme_tcp_qpair_submit_request(struct spdk_nvme_qpair *qpair,
			      struct nvme_request *req)
{
	struct nvme_tcp_qpair *tqpair;
	struct nvme_tcp_req *tcp_req;
	int rc;
	enum spdk_nvme_data_transfer xfer;

	tqpair = nvme_tcp_qpair(qpair);
	assert(tqpair != NULL);

	tcp_req = nvme_tcp_req(req);
	assert(tcp_req != NULL);

	rc = nvme_tcp_req_get(tqpair, tcp_req);
	if (rc != 0) {
		tqpair->stats->queued_requests++;
		/* Inform the upper layer to try again later. */
		return rc;
	}

	rc = nvme_tcp_req_init(tqpair, req, tcp_req);
	if (rc) {
		SPDK_ERRLOG("nvme_tcp_req_init() failed, rc %d\n", rc);
		nvme_tcp_req_put(tqpair, tcp_req);
		return -1;
	}

	xfer = spdk_nvmf_cmd_get_data_transfer(&req->cmd);
	if (xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER && req->payload.opts &&
	    (req->payload.opts->accel_sequence || (tcp_req->tqpair->flags.host_ddgst_enable &&
					      req->payload.opts->memory_domain && tcp_req->in_capsule_data))) {
		struct spdk_iobuf_channel *iobuf_ch;
		struct spdk_nvme_poll_group *group;

		/* Request contains accel sequence, we need to finish the sequence before
		 * continue to build the request */
		group = tqpair->qpair.poll_group->group;
		if (spdk_unlikely(!group)) {
			SPDK_ERRLOG("accel_seq is only supported with poll groups\n");
			return -ENOTSUP;
		}
		iobuf_ch = group->accel_fn_table.get_iobuf_channel(group->ctx);
		assert(iobuf_ch);
		tcp_req->iobuf_iov.iov_len = req->payload_size;
		tcp_req->iobuf_iov.iov_base = spdk_iobuf_get(iobuf_ch, tcp_req->iobuf_iov.iov_len,
							     &tcp_req->iobuf_entry, nvme_tcp_iobuf_get_cb);
		if (spdk_unlikely(!tcp_req->iobuf_iov.iov_base)) {
			/* Finish accel sequence once buffer is allocated */
			SPDK_WARNLOG("no buffer, in progress\n");
			return 0;
		}
		rc = nvme_tcp_apply_accel_sequence_in_capsule(tcp_req);
		return (rc == -EINPROGRESS) ? 0 : rc;
	}

	spdk_trace_record(TRACE_NVME_TCP_SUBMIT, qpair->id, 0, (uintptr_t)req, req->cb_arg,
			  (uint32_t)req->cmd.cid, (uint32_t)req->cmd.opc,
			  req->cmd.cdw10, req->cmd.cdw11, req->cmd.cdw12);
	TAILQ_INSERT_TAIL(&tqpair->outstanding_reqs, tcp_req, link);
	tqpair->stats->outstanding_reqs++;
	return nvme_tcp_qpair_capsule_cmd_send(tqpair, tcp_req);
}

static int
nvme_tcp_qpair_free_request(struct spdk_nvme_qpair *qpair,
			    struct nvme_request *req)
{
	struct nvme_tcp_qpair *tqpair;
	struct nvme_tcp_req *tcp_req;
	int rc = 0;

	assert(nvme_payload_type(&req->payload) == NVME_PAYLOAD_TYPE_ZCOPY);

	assert(qpair != NULL);
	tqpair = nvme_tcp_qpair(qpair);
	tcp_req = get_nvme_active_req_by_cid(tqpair, req->cmd.cid);
	if (tcp_req) {
		spdk_sock_free_bufs(tqpair->sock, tcp_req->sock_buf);
		tcp_req->iovcnt = 0;
		tcp_req->sock_buf = NULL;
		nvme_tcp_req_put(tcp_req->tqpair, tcp_req);
	} else {
		rc = -EINVAL;
		SPDK_ERRLOG("Failed to find request to free: cid %u\n", req->cmd.cid);
	}

	req->zcopy.iovs = NULL;
	req->zcopy.iovcnt = 0;
	nvme_free_request(req);

	/* Zcopy requests may be queued for waiting resources, so set needs_poll
	 * and increase async_complete to trigger a resubmission of queued requests.
	 */
	if (tqpair->qpair.poll_group && !STAILQ_EMPTY(&tqpair->qpair.queued_req) &&
	    !tqpair->needs_poll) {
		struct nvme_tcp_poll_group *pgroup;
		pgroup = nvme_tcp_poll_group(tqpair->qpair.poll_group);

		TAILQ_INSERT_TAIL(&pgroup->needs_poll, tqpair, link);
		tqpair->needs_poll = true;
	}
	tqpair->async_complete++;

	return rc;
}

static int
nvme_tcp_qpair_reset(struct spdk_nvme_qpair *qpair)
{
	return 0;
}

static inline void
_nvme_tcp_req_complete(struct nvme_tcp_req *tcp_req,
		      struct nvme_tcp_qpair *tqpair,
		      struct spdk_nvme_cpl *rsp)
{
	struct spdk_nvme_cpl	cpl;
	spdk_nvme_cmd_cb	user_cb;
	void			*user_cb_arg;
	struct nvme_request	*req;
	struct spdk_nvme_qpair	*qpair;

	req = &tcp_req->req;
	qpair = req->qpair;

	TAILQ_REMOVE(&tqpair->outstanding_reqs, tcp_req, link);
	tqpair->stats->outstanding_reqs--;
	/* Cache arguments to be passed to nvme_complete_request since tcp_req can be zeroed when released */
	memcpy(&cpl, rsp, sizeof(cpl));
	user_cb		= req->cb_fn;
	user_cb_arg	= req->cb_arg;

	if (nvme_payload_type(&req->payload) == NVME_PAYLOAD_TYPE_ZCOPY) {
		nvme_complete_request_zcopy(req->zcopy.zcopy_cb_fn, user_cb_arg, qpair, req, &cpl);
	} else {
		nvme_tcp_req_put(tqpair, tcp_req);
		nvme_free_request(req);
		nvme_complete_request(user_cb, user_cb_arg, qpair, req, &cpl);
	}
}

static void
nvme_tcp_req_accel_seq_complete_cb(void *arg, int status)
{
	struct nvme_tcp_req	*tcp_req = arg;
	struct nvme_tcp_qpair	*tqpair = tcp_req->tqpair;
	struct nvme_request	*req;

	SPDK_DEBUGLOG(nvme, "Accel sequence completed: tcp_req %p, status %d\n", tcp_req, status);

	req = &tcp_req->req;

	assert(tcp_req->ordering.bits.in_progress_accel);
	tcp_req->ordering.bits.in_progress_accel = 0;
	spdk_nvme_request_put_zcopy_iovs(&req->zcopy);
	spdk_sock_free_bufs(tqpair->sock, tcp_req->sock_buf);
	tcp_req->iovcnt = 0;
	tcp_req->sock_buf = NULL;

	if (spdk_unlikely(status)) {
		SPDK_ERRLOG("tqair %p, req %p, accel sequence status %d\n", tqpair, tcp_req, status);
		tcp_req->rsp.status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		tcp_req->rsp.status.sct = SPDK_NVME_SCT_GENERIC;
		tcp_req->rsp.status.dnr = 0;
		goto complete;
	}
	if (spdk_unlikely(tqpair->recv_state == NVME_TCP_PDU_RECV_STATE_QUIESCING ||
		!spdk_nvme_qpair_is_connected(&tqpair->qpair))) {
		SPDK_DEBUGLOG(nvme, "tqpair %p, req %p accel cpl in disconnecting, outstanding %u\n",
			    tqpair, tcp_req, tqpair->qpair.num_outstanding_reqs);
		tcp_req->rsp.status.sc = SPDK_NVME_SC_ABORTED_SQ_DELETION;
		tcp_req->rsp.status.sct = SPDK_NVME_SCT_GENERIC;
		tcp_req->rsp.status.dnr = 0;
	}

complete:
	_nvme_tcp_req_complete(tcp_req, tqpair, &tcp_req->rsp);
}

static void
nvme_tcp_req_complete_memory_domain(struct nvme_tcp_req *tcp_req,
				    struct nvme_tcp_qpair *tqpair,
				    struct spdk_nvme_cpl *rsp)
{
	struct nvme_request	*req;
	enum spdk_nvme_data_transfer xfer;
	bool			error;
	int			rc = 0;
	struct spdk_accel_task	*task;
	struct spdk_accel_sequence *accel_seq;
	bool skip_copy = false;

	req = &tcp_req->req;

	assert(req->cmd.opc != SPDK_NVME_OPC_FABRIC);
	xfer = spdk_nvme_opc_get_data_transfer(req->cmd.opc);
	error = spdk_nvme_cpl_is_error(rsp);

	if (xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
		struct spdk_io_channel	*accel_ch;
		struct spdk_nvme_poll_group *group = tqpair->qpair.poll_group->group;
		struct spdk_sock_buf *sock_buf;

		assert(group != 0);

		/* @todo: check if we ever need to deliver data for error completion, e.g. with data digest error */
		if (spdk_unlikely(error)) {
			goto out;
		}

		if (tcp_req->ordering.bits.digest_offloaded) {
			goto out;
		}

		assert(req->zcopy.iovcnt == 0);
		sock_buf = tcp_req->sock_buf;
		while (sock_buf) {
			req->zcopy.iovcnt++;
			sock_buf = sock_buf->next;
		}

		rc = spdk_nvme_request_get_zcopy_iovs(&req->zcopy);
		if (spdk_unlikely(rc)) {
			SPDK_ERRLOG("Failed to allocate zcopy iovs count\n");
			goto out;
		}

		req->zcopy.iovcnt = 0;
		sock_buf = tcp_req->sock_buf;
		while (sock_buf) {
			req->zcopy.iovs[req->zcopy.iovcnt++] = sock_buf->iov;
			sock_buf = sock_buf->next;
		}

		tqpair->stats->received_data_pdus++;
		tqpair->stats->received_data_iovs += req->zcopy.iovcnt;
		if (req->zcopy.iovcnt > (int)tqpair->stats->max_data_iovs_per_pdu) {
			tqpair->stats->max_data_iovs_per_pdu = req->zcopy.iovcnt;
		}

		accel_ch = group->accel_fn_table.get_accel_channel(group->ctx);
		if (spdk_unlikely(!accel_ch)) {
			SPDK_ERRLOG("Failed to get accel io channel\n");
		}

		accel_seq = req->payload.opts->accel_sequence;
		if (accel_seq) {
			task = spdk_accel_sequence_first_task(accel_seq);
			if (task->op_code == ACCEL_OPC_DECRYPT && spdk_accel_sequence_next_task(task) == NULL) {
				skip_copy = true;
				task->src_domain = NULL;
				task->src_domain_ctx = NULL;
				task->s.iovs = req->zcopy.iovs;
				task->s.iovcnt = req->zcopy.iovcnt;
			}
		}
		if (!skip_copy) {
			rc = spdk_accel_append_copy(&accel_seq, accel_ch,
						    tcp_req->iov, tcp_req->iovcnt,
						    req->payload.opts->memory_domain,
						    req->payload.opts->memory_domain_ctx,
						    req->zcopy.iovs, req->zcopy.iovcnt,
						    NULL, NULL, 0, NULL, NULL);
			if (spdk_unlikely(rc)) {
				SPDK_ERRLOG("Failed to append copy accel task, rc %d\n", rc);
				spdk_nvme_request_put_zcopy_iovs(&req->zcopy);
				goto out;
			}

		}
		spdk_accel_sequence_reverse(accel_seq);
		spdk_accel_sequence_finish(accel_seq,
						nvme_tcp_req_accel_seq_complete_cb,
						tcp_req);
		tcp_req->ordering.bits.in_progress_accel = 1;
		return;
	}

 out:
	if (xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
		spdk_nvme_request_put_zcopy_iovs(&req->zcopy);
		spdk_sock_free_bufs(tqpair->sock, tcp_req->sock_buf);
		tcp_req->iovcnt = 0;
		tcp_req->sock_buf = NULL;
	}

	_nvme_tcp_req_complete(tcp_req, tqpair, rsp);
}

static inline void
nvme_tcp_req_complete(struct nvme_tcp_req *tcp_req,
		      struct nvme_tcp_qpair *tqpair,
		      struct spdk_nvme_cpl *rsp,
		      bool print_on_error)
{
	struct spdk_nvme_qpair	*qpair;
	struct nvme_request	*req;
	bool			error, print_error;

	req = &tcp_req->req;
	qpair = req->qpair;

	error = spdk_nvme_cpl_is_error(rsp);
	spdk_trace_record(TRACE_NVME_TCP_COMPLETE, qpair->id, 0, (uintptr_t)req, req->cb_arg,
			  (uint32_t)req->cmd.cid, (uint32_t)rsp->status_raw);

	if (spdk_unlikely(error)) {
		print_error = print_on_error && !qpair->ctrlr->opts.disable_error_logging;
		if (spdk_unlikely(print_error)) {
			spdk_nvme_qpair_print_command(qpair, &req->cmd);
		}
		if (spdk_unlikely(print_error || SPDK_DEBUGLOG_FLAG_ENABLED("nvme"))) {
			spdk_nvme_qpair_print_completion(qpair, rsp);
		}
	}

	if (spdk_likely(nvme_tcp_req_with_memory_domain(tcp_req))) {
		nvme_tcp_req_complete_memory_domain(tcp_req, tqpair, rsp);
		return;
	}

	_nvme_tcp_req_complete(tcp_req, tqpair, rsp);
}

static void
nvme_tcp_qpair_abort_reqs(struct spdk_nvme_qpair *qpair, uint32_t dnr)
{
	struct nvme_tcp_req *tcp_req, *tmp;
	struct spdk_nvme_cpl cpl = {};
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(qpair);

	cpl.status.sc = SPDK_NVME_SC_ABORTED_SQ_DELETION;
	cpl.status.sct = SPDK_NVME_SCT_GENERIC;
	cpl.status.dnr = dnr;

	TAILQ_FOREACH_SAFE(tcp_req, &tqpair->outstanding_reqs, link, tmp) {
		if (tcp_req->ordering.bits.in_progress_accel) {
			continue;
		}
		spdk_nvme_request_put_zcopy_iovs(&tcp_req->req.zcopy);
		if (tcp_req->sock_buf) {
			spdk_sock_free_bufs(tqpair->sock, tcp_req->sock_buf);
			tcp_req->sock_buf = NULL;
		}
		nvme_tcp_req_complete(tcp_req, tqpair, &cpl, true);
	}
}

static void
nvme_tcp_qpair_send_h2c_term_req_complete(void *cb_arg)
{
	struct nvme_tcp_qpair *tqpair = cb_arg;

	tqpair->state = NVME_TCP_QPAIR_STATE_EXITING;
}

static void
nvme_tcp_qpair_send_h2c_term_req(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_pdu *pdu,
				 enum spdk_nvme_tcp_term_req_fes fes, uint32_t error_offset)
{
	struct nvme_tcp_pdu *rsp_pdu;
	struct spdk_nvme_tcp_term_req_hdr *h2c_term_req;
	uint32_t h2c_term_req_hdr_len = sizeof(*h2c_term_req);
	uint8_t copy_len;

	rsp_pdu = tqpair->send_pdu;
	memset(rsp_pdu, 0, sizeof(*rsp_pdu));
	h2c_term_req = &rsp_pdu->hdr.term_req;
	h2c_term_req->common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_H2C_TERM_REQ;
	h2c_term_req->common.hlen = h2c_term_req_hdr_len;

	if ((fes == SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD) ||
	    (fes == SPDK_NVME_TCP_TERM_REQ_FES_INVALID_DATA_UNSUPPORTED_PARAMETER)) {
		DSET32(&h2c_term_req->fei, error_offset);
	}

	copy_len = pdu->hdr.common.hlen;
	if (copy_len > SPDK_NVME_TCP_TERM_REQ_ERROR_DATA_MAX_SIZE) {
		copy_len = SPDK_NVME_TCP_TERM_REQ_ERROR_DATA_MAX_SIZE;
	}

	/* Copy the error info into the buffer */
	memcpy((uint8_t *)rsp_pdu->hdr.raw + h2c_term_req_hdr_len, pdu->hdr.raw, copy_len);
	nvme_tcp_pdu_set_data(rsp_pdu, (uint8_t *)rsp_pdu->hdr.raw + h2c_term_req_hdr_len, copy_len);

	/* Contain the header len of the wrong received pdu */
	h2c_term_req->common.plen = h2c_term_req->common.hlen + copy_len;
	nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_QUIESCING);
	nvme_tcp_qpair_write_pdu(tqpair, rsp_pdu, nvme_tcp_qpair_send_h2c_term_req_complete, tqpair);
}

static bool
nvme_tcp_qpair_recv_state_valid(struct nvme_tcp_qpair *tqpair)
{
	switch (tqpair->state) {
	case NVME_TCP_QPAIR_STATE_FABRIC_CONNECT_SEND:
	case NVME_TCP_QPAIR_STATE_FABRIC_CONNECT_POLL:
	case NVME_TCP_QPAIR_STATE_RUNNING:
		return true;
	default:
		return false;
	}
}

static void
nvme_tcp_pdu_ch_handle(struct nvme_tcp_qpair *tqpair)
{
	struct nvme_tcp_pdu *pdu;
	uint32_t error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes;
	uint32_t expected_hlen, hd_len = 0;
	bool plen_error = false;

	pdu = tqpair->recv_pdu;

	SPDK_DEBUGLOG(nvme, "pdu type = %d\n", pdu->hdr.common.pdu_type);
	if (spdk_unlikely(pdu->hdr.common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_IC_RESP)) {
		if (tqpair->state != NVME_TCP_QPAIR_STATE_INVALID) {
			SPDK_ERRLOG("Already received IC_RESP PDU, and we should reject this pdu=%p\n", pdu);
			fes = SPDK_NVME_TCP_TERM_REQ_FES_PDU_SEQUENCE_ERROR;
			goto err;
		}
		expected_hlen = sizeof(struct spdk_nvme_tcp_ic_resp);
		if (pdu->hdr.common.plen != expected_hlen) {
			plen_error = true;
		}
	} else {
		if (spdk_unlikely(!nvme_tcp_qpair_recv_state_valid(tqpair))) {
			SPDK_ERRLOG("The TCP/IP tqpair connection is not negotiated\n");
			fes = SPDK_NVME_TCP_TERM_REQ_FES_PDU_SEQUENCE_ERROR;
			goto err;
		}

		switch (pdu->hdr.common.pdu_type) {
		case SPDK_NVME_TCP_PDU_TYPE_CAPSULE_RESP:
			expected_hlen = sizeof(struct spdk_nvme_tcp_rsp);
			if (pdu->hdr.common.flags & SPDK_NVME_TCP_CH_FLAGS_HDGSTF) {
				hd_len = SPDK_NVME_TCP_DIGEST_LEN;
			}

			if (pdu->hdr.common.plen != (expected_hlen + hd_len)) {
				plen_error = true;
			}
			break;
		case SPDK_NVME_TCP_PDU_TYPE_C2H_DATA:
			expected_hlen = sizeof(struct spdk_nvme_tcp_c2h_data_hdr);
			if (pdu->hdr.common.plen < pdu->hdr.common.pdo) {
				plen_error = true;
			}
			break;
		case SPDK_NVME_TCP_PDU_TYPE_C2H_TERM_REQ:
			expected_hlen = sizeof(struct spdk_nvme_tcp_term_req_hdr);
			if ((pdu->hdr.common.plen <= expected_hlen) ||
			    (pdu->hdr.common.plen > SPDK_NVME_TCP_TERM_REQ_PDU_MAX_SIZE)) {
				plen_error = true;
			}
			break;
		case SPDK_NVME_TCP_PDU_TYPE_R2T:
			expected_hlen = sizeof(struct spdk_nvme_tcp_r2t_hdr);
			if (pdu->hdr.common.flags & SPDK_NVME_TCP_CH_FLAGS_HDGSTF) {
				hd_len = SPDK_NVME_TCP_DIGEST_LEN;
			}

			if (pdu->hdr.common.plen != (expected_hlen + hd_len)) {
				plen_error = true;
			}
			break;

		default:
			SPDK_ERRLOG("Unexpected PDU type 0x%02x\n", tqpair->recv_pdu->hdr.common.pdu_type);
			fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
			error_offset = offsetof(struct spdk_nvme_tcp_common_pdu_hdr, pdu_type);
			goto err;
		}
	}

	if (pdu->hdr.common.hlen != expected_hlen) {
		SPDK_ERRLOG("Expected PDU header length %u, got %u\n",
			    expected_hlen, pdu->hdr.common.hlen);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_common_pdu_hdr, hlen);
		goto err;

	} else if (plen_error) {
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_common_pdu_hdr, plen);
		goto err;
	} else {
		nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PSH);
		nvme_tcp_pdu_calc_psh_len(tqpair->recv_pdu, tqpair->flags.host_hdgst_enable);
		return;
	}
err:
	nvme_tcp_qpair_send_h2c_term_req(tqpair, pdu, fes, error_offset);
}

static struct nvme_tcp_req *
get_nvme_active_req_by_cid(struct nvme_tcp_qpair *tqpair, uint32_t cid)
{
	assert(tqpair != NULL);
	if (spdk_unlikely(cid >= tqpair->num_entries)) {
		return NULL;
	}

	return tqpair->tcp_reqs_lookup[cid];
}

static void
nvme_tcp_c2h_data_payload_handle(struct nvme_tcp_qpair *tqpair,
				 struct nvme_tcp_pdu *pdu, uint32_t *reaped)
{
	struct nvme_tcp_req *tcp_req;
	struct spdk_nvme_tcp_c2h_data_hdr *c2h_data;
	uint8_t flags;

	tcp_req = pdu->req;
	assert(tcp_req != NULL);

	SPDK_DEBUGLOG(nvme, "enter\n");
	c2h_data = &pdu->hdr.c2h_data;
	tcp_req->datao += pdu->data_len;
	flags = c2h_data->common.flags;

	if (flags & SPDK_NVME_TCP_C2H_DATA_FLAGS_LAST_PDU) {
		if (tcp_req->datao == tcp_req->req.payload_size) {
			tcp_req->rsp.status.p = 0;
		} else {
			tcp_req->rsp.status.p = 1;
		}

		tcp_req->rsp.cid = tcp_req->cid;
		tcp_req->rsp.sqid = tqpair->qpair.id;
		if (flags & SPDK_NVME_TCP_C2H_DATA_FLAGS_SUCCESS) {
			tcp_req->ordering.bits.data_recv = 1;
			if (nvme_tcp_req_complete_safe(tcp_req)) {
				(*reaped)++;
			}
		}
	}
}

static const char *spdk_nvme_tcp_term_req_fes_str[] = {
	"Invalid PDU Header Field",
	"PDU Sequence Error",
	"Header Digest Error",
	"Data Transfer Out of Range",
	"Data Transfer Limit Exceeded",
	"Unsupported parameter",
};

static void
nvme_tcp_c2h_term_req_dump(struct spdk_nvme_tcp_term_req_hdr *c2h_term_req)
{
	SPDK_ERRLOG("Error info of pdu(%p): %s\n", c2h_term_req,
		    spdk_nvme_tcp_term_req_fes_str[c2h_term_req->fes]);
	if ((c2h_term_req->fes == SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD) ||
	    (c2h_term_req->fes == SPDK_NVME_TCP_TERM_REQ_FES_INVALID_DATA_UNSUPPORTED_PARAMETER)) {
		SPDK_DEBUGLOG(nvme, "The offset from the start of the PDU header is %u\n",
			      DGET32(c2h_term_req->fei));
	}
	/* we may also need to dump some other info here */
}

static void
nvme_tcp_c2h_term_req_payload_handle(struct nvme_tcp_qpair *tqpair,
				     struct nvme_tcp_pdu *pdu)
{
	nvme_tcp_c2h_term_req_dump(&pdu->hdr.term_req);
	nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_QUIESCING);
}

static void
_nvme_tcp_pdu_payload_handle(struct nvme_tcp_qpair *tqpair, uint32_t *reaped)
{
	struct nvme_tcp_pdu *pdu;

	assert(tqpair != NULL);
	pdu = tqpair->recv_pdu;

	switch (pdu->hdr.common.pdu_type) {
	case SPDK_NVME_TCP_PDU_TYPE_C2H_DATA:
		nvme_tcp_c2h_data_payload_handle(tqpair, pdu, reaped);
		nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY);
		break;

	case SPDK_NVME_TCP_PDU_TYPE_C2H_TERM_REQ:
		nvme_tcp_c2h_term_req_payload_handle(tqpair, pdu);
		break;

	default:
		/* The code should not go to here */
		SPDK_ERRLOG("The code should not go to here\n");
		break;
	}
}

static void
tcp_data_recv_crc32_done(void *cb_arg, int status)
{
	struct nvme_tcp_req *tcp_req = cb_arg;
	struct nvme_tcp_pdu *pdu;
	struct nvme_tcp_qpair *tqpair;
	int rc;
	struct nvme_tcp_poll_group *pgroup;
	int dummy_reaped = 0;

	pdu = &tcp_req->pdu;

	tqpair = tcp_req->tqpair;
	assert(tqpair != NULL);

	if (tqpair->qpair.poll_group && !tqpair->needs_poll) {
		pgroup = nvme_tcp_poll_group(tqpair->qpair.poll_group);
		TAILQ_INSERT_TAIL(&pgroup->needs_poll, tqpair, link);
		tqpair->needs_poll = true;
	}

	if (spdk_unlikely(status)) {
		SPDK_ERRLOG("Failed to compute the data digest for pdu =%p\n", pdu);
		tcp_req->rsp.status.sc = SPDK_NVME_SC_COMMAND_TRANSIENT_TRANSPORT_ERROR;
		goto end;
	}

	pdu->data_digest_crc32 ^= SPDK_CRC32C_XOR;
	tqpair->stats->recv_ddgsts++;
	rc = MATCH_DIGEST_WORD(pdu->data_digest, pdu->data_digest_crc32);
	if (rc == 0) {
		SPDK_ERRLOG("data digest error on tqpair=(%p) with pdu=%p\n", tqpair, pdu);
		tcp_req->rsp.status.sc = SPDK_NVME_SC_COMMAND_TRANSIENT_TRANSPORT_ERROR;
	}

end:
	nvme_tcp_c2h_data_payload_handle(tqpair, pdu, &dummy_reaped);
}

static void
nvme_tcp_req_accel_seq_complete_crc_c2h_cb(void *arg, int status)
{
	struct nvme_tcp_req *tcp_req = arg;
	struct nvme_tcp_qpair *tqpair;
	struct nvme_tcp_poll_group *pgroup;
	int dummy_reaped = 0;

	SPDK_DEBUGLOG(nvme, "Accel sequence completed: tcp_req %p, status %d\n", tcp_req, status);
	assert(tcp_req->ordering.bits.in_progress_accel);
	tcp_req->ordering.bits.in_progress_accel = 0;

	assert(tcp_req->tqpair != NULL);
	tqpair = tcp_req->tqpair;

	if (tqpair->qpair.poll_group && !STAILQ_EMPTY(&tqpair->qpair.queued_req) && !tqpair->needs_poll) {
		pgroup = nvme_tcp_poll_group(tqpair->qpair.poll_group);
		TAILQ_INSERT_TAIL(&pgroup->needs_poll, tqpair, link);
		tqpair->needs_poll = true;
	}

	if (spdk_unlikely(status)) {
		SPDK_ERRLOG("Failed to compute the data digest for pdu =%p\n", &tcp_req->pdu);
		tcp_req->rsp.status.sc = SPDK_NVME_SC_COMMAND_TRANSIENT_TRANSPORT_ERROR;

		/* Prevent aborting this sequence in nvme_tcp_req_complete_memory_domain(). */
		tcp_req->req.payload.opts->accel_sequence = NULL;
		goto complete;
	}
	if (spdk_unlikely(tqpair->recv_state == NVME_TCP_PDU_RECV_STATE_QUIESCING ||
			  !spdk_nvme_qpair_is_connected(&tqpair->qpair))) {
		SPDK_DEBUGLOG(nvme, "tqpair %p, req %p accel cpl in disconnecting, outstanding %u\n",
			      tqpair, tcp_req, tqpair->qpair.num_outstanding_reqs);
		tcp_req->rsp.status.sc = SPDK_NVME_SC_ABORTED_SQ_DELETION;
		tcp_req->rsp.status.sct = SPDK_NVME_SCT_GENERIC;
		tcp_req->rsp.status.dnr = 0;
	}
complete:
	nvme_tcp_c2h_data_payload_handle(tqpair, &tcp_req->pdu, &dummy_reaped);
}

static inline int
nvme_tcp_apply_accel_sequence_c2h(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_pdu *pdu)
{
	struct nvme_tcp_req *tcp_req = pdu->req;
	struct nvme_request *req;
	struct spdk_nvme_poll_group *group = tqpair->qpair.poll_group->group;
	struct spdk_io_channel *accel_ch;
	struct spdk_accel_sequence *accel_seq;
	struct spdk_sock_buf *sock_buf;
	bool skip_copy = false;
	int rc;

	req = &tcp_req->req;

	assert(group != 0);
	assert(req->zcopy.iovcnt == 0);
	sock_buf = tcp_req->sock_buf;
	while (sock_buf) {
		req->zcopy.iovcnt++;
		sock_buf = sock_buf->next;
	}

	rc = spdk_nvme_request_get_zcopy_iovs(&req->zcopy);
	if (spdk_unlikely(rc)) {
		SPDK_ERRLOG("Failed to allocate zcopy iovs count\n");
		return rc;
	}

	req->zcopy.iovcnt = 0;
	sock_buf = tcp_req->sock_buf;
	while (sock_buf) {
		req->zcopy.iovs[req->zcopy.iovcnt++] = sock_buf->iov;
		sock_buf = sock_buf->next;
	}

	tqpair->stats->received_data_pdus++;
	tqpair->stats->received_data_iovs += req->zcopy.iovcnt;
	if (req->zcopy.iovcnt > (int)tqpair->stats->max_data_iovs_per_pdu) {
		tqpair->stats->max_data_iovs_per_pdu = req->zcopy.iovcnt;
	}

	accel_ch = group->accel_fn_table.get_accel_channel(group->ctx);
	if (spdk_unlikely(!accel_ch)) {
		SPDK_ERRLOG("Failed to get accel io channel\n");
		return -EIO;
	}

	assert(req->payload.opts);
	accel_seq = req->payload.opts->accel_sequence;
	if (accel_seq) {
		struct spdk_accel_task *task = spdk_accel_sequence_first_task(accel_seq);
		if (task->op_code == ACCEL_OPC_DECRYPT && spdk_accel_sequence_next_task(task) == NULL) {
			skip_copy = true;
			task->src_domain = NULL;
			task->src_domain_ctx = NULL;
			task->s.iovs = req->zcopy.iovs;
			task->s.iovcnt = req->zcopy.iovcnt;
		}
	}
	if (!skip_copy) {
		rc = spdk_accel_append_copy(&accel_seq, accel_ch, tcp_req->iov, tcp_req->iovcnt,
					    req->payload.opts->memory_domain, req->payload.opts->memory_domain_ctx,
					    req->zcopy.iovs, req->zcopy.iovcnt, NULL, NULL, 0, NULL, NULL);
		if (spdk_unlikely(rc)) {
			SPDK_ERRLOG("Failed to append copy accel task, rc %d\n", rc);
			return rc;
		}
	}
	rc = spdk_accel_append_check_crc32c(&accel_seq, accel_ch, (uint32_t *)&pdu->data_digest,
					    req->zcopy.iovs, req->zcopy.iovcnt, NULL, NULL, SPDK_CRC32C_XOR, NULL,
					    NULL);
	if (spdk_unlikely(rc)) {
		SPDK_ERRLOG("Failed to append check crc accel task, rc %d\n", rc);
		goto abort_sequence;
	}

	spdk_accel_sequence_reverse(accel_seq);
	spdk_accel_sequence_finish(accel_seq, nvme_tcp_req_accel_seq_complete_crc_c2h_cb, tcp_req);
	tcp_req->ordering.bits.in_progress_accel = 1;

	return 0;
abort_sequence:
	if (!req->payload.opts->accel_sequence) {
		spdk_accel_sequence_abort(accel_seq);
	}
	return rc;
}

static void
nvme_tcp_pdu_payload_handle(struct nvme_tcp_qpair *tqpair,
			    uint32_t *reaped)
{
	int rc = 0;
	struct nvme_tcp_pdu *pdu = tqpair->recv_pdu;
	uint32_t crc32c;
	struct nvme_tcp_poll_group *tgroup;
	struct nvme_tcp_req *tcp_req = pdu->req;

	assert(tqpair->recv_state == NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD);
	SPDK_DEBUGLOG(nvme, "enter\n");

	/* The request can be NULL, e.g. in case of C2HTermReq */
	if (spdk_likely(tcp_req != NULL)) {
		tcp_req->expected_datao += pdu->data_len;
	}

	/* check data digest if need */
	if (pdu->ddgst_enable) {
		/* But if the data digest is enabled, tcp_req cannot be NULL */
		assert(tcp_req != NULL);
		tgroup = nvme_tcp_poll_group(tqpair->qpair.poll_group);
		/* Only support this limitated case that the request has only one c2h pdu */
		if ((nvme_qpair_get_state(&tqpair->qpair) >= NVME_QPAIR_CONNECTED) && tgroup != NULL &&
		    spdk_likely(pdu->data_len % SPDK_NVME_TCP_DIGEST_ALIGNMENT == 0
				&& tcp_req->req.payload_size == pdu->data_len) &&
		    !nvme_tcp_pdu_is_zcopy(pdu)) {
			tcp_req->pdu.hdr = pdu->hdr;
			tcp_req->pdu.req = tcp_req;
			memcpy(tcp_req->pdu.data_digest, pdu->data_digest, sizeof(pdu->data_digest));
			tcp_req->pdu.data_len = pdu->data_len;

			nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY);

			if (nvme_tcp_req_with_memory_domain(tcp_req)) {
				tqpair->stats->recv_ddgsts++;
				tcp_req->ordering.bits.digest_offloaded = 1;
				rc = nvme_tcp_apply_accel_sequence_c2h(tqpair, &tcp_req->pdu);
				if (spdk_unlikely(rc)) {
					goto transient_error;
				}
				return;
			} else if (tgroup->group.group->accel_fn_table.submit_accel_crc32c) {
				memcpy(tcp_req->pdu.data_iov, pdu->data_iov,
				       sizeof(pdu->data_iov[0]) * pdu->data_iovcnt);
				tcp_req->pdu.data_iovcnt = pdu->data_iovcnt;

				tgroup->group.group->accel_fn_table.submit_accel_crc32c(
					tgroup->group.group->ctx, &tcp_req->pdu.data_digest_crc32,
					tcp_req->pdu.data_iov, tcp_req->pdu.data_iovcnt, 0, tcp_data_recv_crc32_done,
					tcp_req);
				return;
			}
		}

		if (nvme_tcp_pdu_is_zcopy(pdu)) {
			crc32c = nvme_tcp_pdu_calc_data_digest_with_iov(pdu, tcp_req->req.zcopy.iovs, tcp_req->iovcnt);
		} else if (nvme_tcp_req_with_memory_domain(pdu->req)) {
			crc32c = nvme_tcp_pdu_calc_data_digest_with_sock_buf(pdu);
		} else {
			crc32c = nvme_tcp_pdu_calc_data_digest_with_iov(pdu, pdu->data_iov, pdu->data_iovcnt);
		}

		tqpair->stats->recv_ddgsts++;
		rc = MATCH_DIGEST_WORD(pdu->data_digest, crc32c);
		if (rc == 0) {
transient_error:
			SPDK_ERRLOG("data digest error on tqpair=(%p) with pdu=%p\n", tqpair, pdu);
			tcp_req = pdu->req;
			assert(tcp_req != NULL);
			tcp_req->rsp.status.sc = SPDK_NVME_SC_COMMAND_TRANSIENT_TRANSPORT_ERROR;
		}
	}

	_nvme_tcp_pdu_payload_handle(tqpair, reaped);
}

static void
nvme_tcp_send_icreq_complete(void *cb_arg)
{
	struct nvme_tcp_qpair *tqpair = cb_arg;

	SPDK_DEBUGLOG(nvme, "Complete the icreq send for tqpair=%p %u\n", tqpair, tqpair->qpair.id);

	tqpair->flags.icreq_send_ack = true;

	if (tqpair->state == NVME_TCP_QPAIR_STATE_INITIALIZING) {
		SPDK_DEBUGLOG(nvme, "tqpair %p %u, finalize icresp\n", tqpair, tqpair->qpair.id);
		tqpair->state = NVME_TCP_QPAIR_STATE_FABRIC_CONNECT_SEND;
	}
}

static void
nvme_tcp_icresp_handle(struct nvme_tcp_qpair *tqpair,
		       struct nvme_tcp_pdu *pdu)
{
	struct spdk_nvme_tcp_ic_resp *ic_resp = &pdu->hdr.ic_resp;
	uint32_t error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes;
	int recv_buf_size;

	/* Only PFV 0 is defined currently */
	if (ic_resp->pfv != 0) {
		SPDK_ERRLOG("Expected ICResp PFV %u, got %u\n", 0u, ic_resp->pfv);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_ic_resp, pfv);
		goto end;
	}

	if (ic_resp->maxh2cdata < NVME_TCP_PDU_H2C_MIN_DATA_SIZE) {
		SPDK_ERRLOG("Expected ICResp maxh2cdata >=%u, got %u\n", NVME_TCP_PDU_H2C_MIN_DATA_SIZE,
			    ic_resp->maxh2cdata);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_ic_resp, maxh2cdata);
		goto end;
	}
	tqpair->maxh2cdata = ic_resp->maxh2cdata;

	if (ic_resp->cpda > SPDK_NVME_TCP_CPDA_MAX) {
		SPDK_ERRLOG("Expected ICResp cpda <=%u, got %u\n", SPDK_NVME_TCP_CPDA_MAX, ic_resp->cpda);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_ic_resp, cpda);
		goto end;
	}
	tqpair->cpda = ic_resp->cpda;

	tqpair->flags.host_hdgst_enable = ic_resp->dgst.bits.hdgst_enable ? true : false;
	tqpair->flags.host_ddgst_enable = ic_resp->dgst.bits.ddgst_enable ? true : false;
	SPDK_DEBUGLOG(nvme, "host_hdgst_enable: %u\n", tqpair->flags.host_hdgst_enable);
	SPDK_DEBUGLOG(nvme, "host_ddgst_enable: %u\n", tqpair->flags.host_ddgst_enable);

	/* Now that we know whether digests are enabled, properly size the receive buffer to
	 * handle several incoming 4K read commands according to SPDK_NVMF_TCP_RECV_BUF_SIZE_FACTOR
	 * parameter. */
	recv_buf_size = 0x1000 + sizeof(struct spdk_nvme_tcp_c2h_data_hdr);

	if (tqpair->flags.host_hdgst_enable) {
		recv_buf_size += SPDK_NVME_TCP_DIGEST_LEN;
	}

	if (tqpair->flags.host_ddgst_enable) {
		recv_buf_size += SPDK_NVME_TCP_DIGEST_LEN;
	}

	if (spdk_sock_set_recvbuf(tqpair->sock, recv_buf_size * SPDK_NVMF_TCP_RECV_BUF_SIZE_FACTOR) < 0) {
		SPDK_WARNLOG("Unable to allocate enough memory for receive buffer on tqpair=%p with size=%d\n",
			     tqpair,
			     recv_buf_size);
		/* Not fatal. */
	}

	nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY);

	if (!tqpair->flags.icreq_send_ack) {
		tqpair->state = NVME_TCP_QPAIR_STATE_INITIALIZING;
		SPDK_DEBUGLOG(nvme, "tqpair %p %u, waiting icreq ack\n", tqpair, tqpair->qpair.id);
		return;
	}

	tqpair->state = NVME_TCP_QPAIR_STATE_FABRIC_CONNECT_SEND;
	return;
end:
	nvme_tcp_qpair_send_h2c_term_req(tqpair, pdu, fes, error_offset);
}

static void
nvme_tcp_capsule_resp_hdr_handle(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_pdu *pdu,
				 uint32_t *reaped)
{
	struct nvme_tcp_req *tcp_req;
	struct spdk_nvme_tcp_rsp *capsule_resp = &pdu->hdr.capsule_resp;
	uint32_t cid, error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes;

	SPDK_DEBUGLOG(nvme, "enter\n");
	cid = capsule_resp->rccqe.cid;
	tcp_req = get_nvme_active_req_by_cid(tqpair, cid);

	if (!tcp_req) {
		SPDK_ERRLOG("no tcp_req is found with cid=%u for tqpair=%p\n", cid, tqpair);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_rsp, rccqe);
		goto end;
	}

	tcp_req->rsp = capsule_resp->rccqe;
	tcp_req->ordering.bits.data_recv = 1;

	/* Recv the pdu again */
	nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY);

	if (nvme_tcp_req_complete_safe(tcp_req)) {
		(*reaped)++;
	}

	return;

end:
	nvme_tcp_qpair_send_h2c_term_req(tqpair, pdu, fes, error_offset);
}

static void
nvme_tcp_c2h_term_req_hdr_handle(struct nvme_tcp_qpair *tqpair,
				 struct nvme_tcp_pdu *pdu)
{
	struct spdk_nvme_tcp_term_req_hdr *c2h_term_req = &pdu->hdr.term_req;
	uint32_t error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes;

	if (c2h_term_req->fes > SPDK_NVME_TCP_TERM_REQ_FES_INVALID_DATA_UNSUPPORTED_PARAMETER) {
		SPDK_ERRLOG("Fatal Error Status(FES) is unknown for c2h_term_req pdu=%p\n", pdu);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_term_req_hdr, fes);
		goto end;
	}

	/* set the data buffer */
	nvme_tcp_pdu_set_data(pdu, (uint8_t *)pdu->hdr.raw + c2h_term_req->common.hlen,
			      c2h_term_req->common.plen - c2h_term_req->common.hlen);
	nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD);
	return;
end:
	nvme_tcp_qpair_send_h2c_term_req(tqpair, pdu, fes, error_offset);
}

static void
nvme_tcp_c2h_data_hdr_handle(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_pdu *pdu)
{
	struct nvme_tcp_req *tcp_req;
	struct spdk_nvme_tcp_c2h_data_hdr *c2h_data = &pdu->hdr.c2h_data;
	uint32_t error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes;
	int flags = c2h_data->common.flags;

	SPDK_DEBUGLOG(nvme, "enter\n");
	SPDK_DEBUGLOG(nvme, "c2h_data info on tqpair(%p): datao=%u, datal=%u, cccid=%d\n",
		      tqpair, c2h_data->datao, c2h_data->datal, c2h_data->cccid);
	tcp_req = get_nvme_active_req_by_cid(tqpair, c2h_data->cccid);
	if (!tcp_req) {
		SPDK_ERRLOG("no tcp_req found for c2hdata cid=%d\n", c2h_data->cccid);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_c2h_data_hdr, cccid);
		goto end;

	}

	SPDK_DEBUGLOG(nvme, "tcp_req(%p) on tqpair(%p): expected_datao=%u, payload_size=%u\n",
		      tcp_req, tqpair, tcp_req->expected_datao, tcp_req->req.payload_size);

	if (spdk_unlikely((flags & SPDK_NVME_TCP_C2H_DATA_FLAGS_SUCCESS) &&
			  !(flags & SPDK_NVME_TCP_C2H_DATA_FLAGS_LAST_PDU))) {
		SPDK_ERRLOG("Invalid flag flags=%d in c2h_data=%p\n", flags, c2h_data);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_c2h_data_hdr, common);
		goto end;
	}

	if (c2h_data->datal > tcp_req->req.payload_size) {
		SPDK_ERRLOG("Invalid datal for tcp_req(%p), datal(%u) exceeds payload_size(%u)\n",
			    tcp_req, c2h_data->datal, tcp_req->req.payload_size);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_DATA_TRANSFER_OUT_OF_RANGE;
		goto end;
	}

	if (tcp_req->expected_datao != c2h_data->datao) {
		SPDK_ERRLOG("Invalid datao for tcp_req(%p), received datal(%u) != expected datao(%u) in tcp_req\n",
			    tcp_req, c2h_data->datao, tcp_req->expected_datao);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_c2h_data_hdr, datao);
		goto end;
	}

	if ((c2h_data->datao + c2h_data->datal) > tcp_req->req.payload_size) {
		SPDK_ERRLOG("Invalid data range for tcp_req(%p), received (datao(%u) + datal(%u)) > datao(%u) in tcp_req\n",
			    tcp_req, c2h_data->datao, c2h_data->datal, tcp_req->req.payload_size);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_DATA_TRANSFER_OUT_OF_RANGE;
		error_offset = offsetof(struct spdk_nvme_tcp_c2h_data_hdr, datal);
		goto end;

	}

	nvme_tcp_pdu_set_data_buf(pdu, tcp_req->iov, tcp_req->iovcnt,
				  c2h_data->datao, c2h_data->datal);
	pdu->req = tcp_req;

	nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD);
	return;

end:
	nvme_tcp_qpair_send_h2c_term_req(tqpair, pdu, fes, error_offset);
}

static void
nvme_tcp_qpair_h2c_data_send_complete(void *cb_arg)
{
	struct nvme_tcp_req *tcp_req = cb_arg;

	assert(tcp_req != NULL);

	tcp_req->ordering.bits.send_ack = 1;
	if (tcp_req->r2tl_remain) {
		nvme_tcp_send_h2c_data(tcp_req);
	} else {
		assert(tcp_req->active_r2ts > 0);
		tcp_req->active_r2ts--;
		tcp_req->state = NVME_TCP_REQ_ACTIVE;

		if (tcp_req->ordering.bits.r2t_waiting_h2c_complete) {
			tcp_req->ordering.bits.r2t_waiting_h2c_complete = 0;
			SPDK_DEBUGLOG(nvme, "tcp_req %p: continue r2t\n", tcp_req);
			assert(tcp_req->active_r2ts > 0);
			tcp_req->ttag = tcp_req->ttag_r2t_next;
			tcp_req->r2tl_remain = tcp_req->r2tl_remain_next;
			tcp_req->state = NVME_TCP_REQ_ACTIVE_R2T;
			nvme_tcp_send_h2c_data(tcp_req);
			return;
		}

		/* Need also call this function to free the resource */
		nvme_tcp_req_complete_safe(tcp_req);
	}
}

static void
nvme_tcp_accel_seq_finished_h2c_cb(void *cb_arg, int status)
{
	struct nvme_tcp_req *tcp_req = cb_arg;
	struct nvme_tcp_qpair *tqpair = tcp_req->tqpair;
	struct nvme_tcp_pdu *rsp_pdu;
	struct spdk_nvme_tcp_h2c_data_hdr *h2c_data;
	bool has_memory_domain;
	enum spdk_nvme_generic_command_status_code sc;
	struct spdk_nvme_cpl cpl;

	SPDK_DEBUGLOG(nvme, "accel cpl, req %p, status %d\n", tcp_req, status);
	assert(tcp_req->ordering.bits.in_progress_accel);
	tcp_req->ordering.bits.in_progress_accel = 0;

	if (spdk_unlikely(status)) {
		SPDK_ERRLOG("tqair %p, req %p, accel sequence status %d\n", tqpair, tcp_req, status);
		sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		goto fail_req;
	}
	if (spdk_unlikely(tqpair->recv_state == NVME_TCP_PDU_RECV_STATE_QUIESCING ||
			  !spdk_nvme_qpair_is_connected(&tqpair->qpair))) {
		SPDK_DEBUGLOG(nvme, "tqpair %p, req %p accel cpl in disconnecting, outstanding %u\n",
			      tqpair, tcp_req, tqpair->qpair.num_outstanding_reqs);
		sc = SPDK_NVME_SC_ABORTED_SQ_DELETION;
		goto fail_req;
	}

	/* Once copy task is finished, we use a single staging buffer.
	 * To reuse existing functions to build a capsule, remove reset_sgl_fn since
	 * it is not needed any more and overwrite contig_or_cb_arg with address of the
	 * staging buffer
	 */
	tcp_req->req.payload.reset_sgl_fn = NULL;
	tcp_req->req.payload.contig_or_cb_arg = tcp_req->iobuf_iov.iov_base;
	tcp_req->req.payload_offset = 0;
	/* Buffer is in local memory, clear memory domain pointer */
	tcp_req->req.payload.opts->memory_domain = NULL;

	/* At this point tcp_req->iovs point to outdated values */
	nvme_tcp_build_contig_request(tcp_req->tqpair, tcp_req);
	rsp_pdu = &tcp_req->pdu;
	h2c_data = &rsp_pdu->hdr.h2c_data;
	nvme_tcp_pdu_set_data_buf(rsp_pdu, tcp_req->iov, tcp_req->iovcnt, h2c_data->datao, h2c_data->datal);

	nvme_tcp_qpair_prepare_pdu(tqpair, rsp_pdu, nvme_tcp_qpair_h2c_data_send_complete, tcp_req);
	if (nvme_tcp_fill_mkeys(tqpair, tcp_req, rsp_pdu) != 0) {
		SPDK_ERRLOG("Failed to fill mkeys\n");
		sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		goto fail_req;
	}

	/* Always has domain data if memory domains active. */
	has_memory_domain = tcp_req->req.payload.opts && tcp_req->req.payload.opts->memory_domain;
	rsp_pdu->sock_req.has_memory_domain_data = has_memory_domain;
	tqpair->stats->submitted_requests++;
	spdk_sock_writev_async(tqpair->sock, &rsp_pdu->sock_req);

	return;

fail_req:
	memset(&cpl, 0, sizeof(cpl));
	cpl.status.sc = sc;
	cpl.status.sct = SPDK_NVME_SCT_GENERIC;
	cpl.status.dnr = 0;
	nvme_tcp_req_complete(tcp_req, tqpair, &cpl, true);
}

static inline int
nvme_tcp_apply_accel_sequence_h2c(struct nvme_tcp_req *tcp_req)
{
	struct nvme_request *req = &tcp_req->req;
	struct spdk_nvme_poll_group *group = tcp_req->tqpair->qpair.poll_group->group;
	struct spdk_accel_sequence *accel_seq;
	struct spdk_io_channel *accel_ch;
	struct spdk_accel_task *task;
	bool skip_copy = false;
	int rc;

	SPDK_DEBUGLOG(nvme, "Write request with accel sequence h2c: tcp_req %p\n", tcp_req);

	assert(req->payload.opts);
	accel_seq = req->payload.opts->accel_sequence;
	accel_ch = group->accel_fn_table.get_accel_channel(group->ctx);
	assert(accel_ch);

	if (accel_seq) {
		task = spdk_accel_sequence_first_task(accel_seq);
		if (task->op_code == ACCEL_OPC_ENCRYPT && spdk_accel_sequence_next_task(task) == NULL) {
			task->dst_domain = NULL;
			task->dst_domain_ctx = NULL;
			task->d.iovs = &tcp_req->iobuf_iov;
			task->d.iovcnt = 1;
			skip_copy = true;
		}
	}

	/*
	 * Ddigest offload is not supported when the data are split into two and more PDUs. The SW will
	 * handle ddigest later.
	 */
	if (tcp_req->tqpair->flags.host_ddgst_enable && !tcp_req->r2tl_remain) {
		if (!skip_copy) {
			rc = spdk_accel_append_copy_crc32c(&accel_seq, accel_ch,
							   (uint32_t *)tcp_req->pdu.data_digest,
							   &tcp_req->iobuf_iov, 1, NULL, NULL,
							   tcp_req->iov, tcp_req->iovcnt,
							   req->payload.opts->memory_domain,
							   req->payload.opts->memory_domain_ctx,
							   SPDK_CRC32C_XOR, NULL, NULL);
			skip_copy = true;
		} else {
			rc = spdk_accel_append_crc32c(&accel_seq, accel_ch,
						      (uint32_t *)tcp_req->pdu.data_digest, &tcp_req->iobuf_iov, 1,
						      NULL, NULL, SPDK_CRC32C_XOR, NULL, NULL);
		}
		if (spdk_unlikely(rc)) {
			SPDK_ERRLOG("Failed to append crc32 accel task, rc %d\n", rc);
			return rc;
		}
		tcp_req->ordering.bits.digest_offloaded = 1;
	}

	if (!skip_copy) {
		rc = spdk_accel_append_copy(&accel_seq, accel_ch, &tcp_req->iobuf_iov, 1, NULL, NULL,
					    tcp_req->iov, tcp_req->iovcnt, req->payload.opts->memory_domain,
					    req->payload.opts->memory_domain_ctx, 0, NULL, NULL);
		if (spdk_unlikely(rc)) {
			return rc;
		}
	}

	spdk_accel_sequence_finish(accel_seq, nvme_tcp_accel_seq_finished_h2c_cb, tcp_req);
	tcp_req->ordering.bits.in_progress_accel = 1;

	return rc;
}

static void
nvme_tcp_h2c_iobuf_get_cb(struct spdk_iobuf_entry *entry, void *buf)
{
	struct nvme_tcp_req *tcp_req = SPDK_CONTAINEROF(entry, struct nvme_tcp_req, iobuf_entry);
	int rc;

	tcp_req->iobuf_iov.iov_base = buf;

	rc = nvme_tcp_apply_accel_sequence_h2c(tcp_req);
	if (spdk_unlikely(rc != -EINPROGRESS)) {
		struct spdk_nvme_cpl cpl;

		SPDK_ERRLOG("failed to apply sequence, rc %d\n", rc);
		assert(rc != 0);

		cpl.status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		cpl.status.sct = SPDK_NVME_SCT_GENERIC;
		cpl.status.dnr = 1;
		nvme_tcp_req_complete(tcp_req, tcp_req->tqpair, &cpl, true);
	}
}

static void
nvme_tcp_send_h2c_data(struct nvme_tcp_req *tcp_req)
{
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(tcp_req->req.qpair);
	struct nvme_tcp_pdu *rsp_pdu;
	struct spdk_nvme_tcp_h2c_data_hdr *h2c_data;
	uint32_t plen, pdo, alignment;
	struct nvme_request *req;
	bool has_memory_domain;
	struct spdk_nvme_cpl cpl;

	/* Reinit the send_ack and h2c_send_waiting_ack bits */
	tcp_req->ordering.bits.send_ack = 0;
	tcp_req->ordering.bits.h2c_send_waiting_ack = 0;
	rsp_pdu = &tcp_req->pdu;
	memset(rsp_pdu, 0, sizeof(*rsp_pdu));
	rsp_pdu->sock_req.mkeys = rsp_pdu->mkeys;
	h2c_data = &rsp_pdu->hdr.h2c_data;

	h2c_data->common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_H2C_DATA;
	plen = h2c_data->common.hlen = sizeof(*h2c_data);
	h2c_data->cccid = tcp_req->cid;
	h2c_data->ttag = tcp_req->ttag;
	h2c_data->datao = tcp_req->datao;

	h2c_data->datal = spdk_min(tcp_req->r2tl_remain, tqpair->maxh2cdata);
	nvme_tcp_pdu_set_data_buf(rsp_pdu, tcp_req->iov, tcp_req->iovcnt,
				  h2c_data->datao, h2c_data->datal);
	tcp_req->r2tl_remain -= h2c_data->datal;

	if (tqpair->flags.host_hdgst_enable) {
		h2c_data->common.flags |= SPDK_NVME_TCP_CH_FLAGS_HDGSTF;
		plen += SPDK_NVME_TCP_DIGEST_LEN;
	}

	rsp_pdu->padding_len = 0;
	pdo = plen;
	if (tqpair->cpda) {
		alignment = (tqpair->cpda + 1) << 2;
		if (alignment > plen) {
			rsp_pdu->padding_len = alignment - plen;
			pdo = plen = alignment;
		}
	}

	h2c_data->common.pdo = pdo;
	plen += h2c_data->datal;
	if (tqpair->flags.host_ddgst_enable) {
		h2c_data->common.flags |= SPDK_NVME_TCP_CH_FLAGS_DDGSTF;
		plen += SPDK_NVME_TCP_DIGEST_LEN;
	}

	h2c_data->common.plen = plen;
	tcp_req->datao += h2c_data->datal;
	if (!tcp_req->r2tl_remain) {
		h2c_data->common.flags |= SPDK_NVME_TCP_H2C_DATA_FLAGS_LAST_PDU;
	}

	SPDK_DEBUGLOG(nvme, "h2c_data info: datao=%u, datal=%u, pdu_len=%u for tqpair=%p\n",
		      h2c_data->datao, h2c_data->datal, h2c_data->common.plen, tqpair);

	req = &tcp_req->req;

	/* Allocate an IO buffer and copy data to it if this H2CData PDU is the first. */
	if (tqpair->flags.host_ddgst_enable && h2c_data->datao == 0 && tcp_req->r2tl_remain == 0 &&
	    nvme_tcp_req_with_memory_domain(tcp_req)) {
		struct spdk_nvme_poll_group *group;
		struct spdk_iobuf_channel *iobuf_ch;
		int rc;

		group = tqpair->qpair.poll_group->group;
		if (spdk_unlikely(!group)) {
			SPDK_ERRLOG("accel_seq is only supported with poll groups\n");
			goto fail_req;
		}

		iobuf_ch = group->accel_fn_table.get_iobuf_channel(group->ctx);
		assert(iobuf_ch);
		tcp_req->iobuf_iov.iov_len = req->payload_size;
		tcp_req->iobuf_iov.iov_base = spdk_iobuf_get(iobuf_ch, tcp_req->iobuf_iov.iov_len,
							     &tcp_req->iobuf_entry, nvme_tcp_h2c_iobuf_get_cb);
		if (spdk_unlikely(!tcp_req->iobuf_iov.iov_base)) {
			/* Finish accel sequence once buffer is allocated */
			SPDK_WARNLOG("no buffer, in progress\n");
			return;
		}
		rc = nvme_tcp_apply_accel_sequence_h2c(tcp_req);
		if (spdk_unlikely(rc)) {
			SPDK_ERRLOG("Failed to apply sequence\n");
			goto fail_req;
		}
		return;
	}

	nvme_tcp_qpair_prepare_pdu(tqpair, rsp_pdu, nvme_tcp_qpair_h2c_data_send_complete, tcp_req);
	if (nvme_tcp_fill_mkeys(tqpair, tcp_req, rsp_pdu) != 0) {
		SPDK_ERRLOG("Failed to fill mkeys\n");
		goto fail_req;
	}

	/* Always has domain data if memory domains active. */
	has_memory_domain = tcp_req->req.payload.opts && tcp_req->req.payload.opts->memory_domain;
	rsp_pdu->sock_req.has_memory_domain_data = has_memory_domain;
	tqpair->stats->submitted_requests++;
	spdk_sock_writev_async(tqpair->sock, &rsp_pdu->sock_req);

	return;
fail_req:
	cpl.status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
	cpl.status.sct = SPDK_NVME_SCT_GENERIC;
	cpl.status.dnr = 1;
	nvme_tcp_req_complete(tcp_req, tcp_req->tqpair, &cpl, true);
}

static void
nvme_tcp_r2t_hdr_handle(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_pdu *pdu)
{
	struct nvme_tcp_req *tcp_req;
	struct spdk_nvme_tcp_r2t_hdr *r2t = &pdu->hdr.r2t;
	uint32_t cid, error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes;

	SPDK_DEBUGLOG(nvme, "enter\n");
	cid = r2t->cccid;
	tcp_req = get_nvme_active_req_by_cid(tqpair, cid);
	if (!tcp_req) {
		SPDK_ERRLOG("Cannot find tcp_req for tqpair=%p\n", tqpair);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_r2t_hdr, cccid);
		goto end;
	}

	SPDK_DEBUGLOG(nvme, "r2t info: r2to=%u, r2tl=%u for tqpair=%p\n", r2t->r2to, r2t->r2tl,
		      tqpair);

	if (tcp_req->state == NVME_TCP_REQ_ACTIVE) {
		assert(tcp_req->active_r2ts == 0);
		tcp_req->state = NVME_TCP_REQ_ACTIVE_R2T;
	}

	if (tcp_req->datao != r2t->r2to) {
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_r2t_hdr, r2to);
		goto end;

	}

	if ((r2t->r2tl + r2t->r2to) > tcp_req->req.payload_size) {
		SPDK_ERRLOG("Invalid R2T info for tcp_req=%p: (r2to(%u) + r2tl(%u)) exceeds payload_size(%u)\n",
			    tcp_req, r2t->r2to, r2t->r2tl, tqpair->maxh2cdata);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_DATA_TRANSFER_OUT_OF_RANGE;
		error_offset = offsetof(struct spdk_nvme_tcp_r2t_hdr, r2tl);
		goto end;
	}

	tcp_req->active_r2ts++;
	if (spdk_unlikely(tcp_req->active_r2ts > tqpair->maxr2t)) {
		if (tcp_req->state == NVME_TCP_REQ_ACTIVE_R2T && !tcp_req->ordering.bits.send_ack) {
			/* We receive a subsequent R2T while we are waiting for H2C transfer to complete */
			SPDK_DEBUGLOG(nvme, "received a subsequent R2T\n");
			assert(tcp_req->active_r2ts == tqpair->maxr2t + 1);
			tcp_req->ttag_r2t_next = r2t->ttag;
			tcp_req->r2tl_remain_next = r2t->r2tl;
			tcp_req->ordering.bits.r2t_waiting_h2c_complete = 1;
			nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY);
			return;
		} else {
			fes = SPDK_NVME_TCP_TERM_REQ_FES_R2T_LIMIT_EXCEEDED;
			SPDK_ERRLOG("Invalid R2T: Maximum number of R2T exceeded! Max: %u for tqpair=%p\n", tqpair->maxr2t,
				    tqpair);
			goto end;
		}
	}

	tcp_req->ttag = r2t->ttag;
	tcp_req->r2tl_remain = r2t->r2tl;
	nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY);

	if (spdk_likely(tcp_req->ordering.bits.send_ack)) {
		nvme_tcp_send_h2c_data(tcp_req);
	} else {
		tcp_req->ordering.bits.h2c_send_waiting_ack = 1;
	}

	return;

end:
	nvme_tcp_qpair_send_h2c_term_req(tqpair, pdu, fes, error_offset);

}

static void
nvme_tcp_pdu_psh_handle(struct nvme_tcp_qpair *tqpair, uint32_t *reaped)
{
	struct nvme_tcp_pdu *pdu;
	int rc;
	uint32_t crc32c, error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes;

	assert(tqpair->recv_state == NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PSH);
	pdu = tqpair->recv_pdu;

	SPDK_DEBUGLOG(nvme, "enter: pdu type =%u\n", pdu->hdr.common.pdu_type);
	/* check header digest if needed */
	if (pdu->has_hdgst) {
		crc32c = nvme_tcp_pdu_calc_header_digest(pdu);
		rc = MATCH_DIGEST_WORD((uint8_t *)pdu->hdr.raw + pdu->hdr.common.hlen, crc32c);
		if (rc == 0) {
			SPDK_ERRLOG("header digest error on tqpair=(%p) with pdu=%p\n", tqpair, pdu);
			fes = SPDK_NVME_TCP_TERM_REQ_FES_HDGST_ERROR;
			nvme_tcp_qpair_send_h2c_term_req(tqpair, pdu, fes, error_offset);
			return;

		}
	}

	switch (pdu->hdr.common.pdu_type) {
	case SPDK_NVME_TCP_PDU_TYPE_IC_RESP:
		nvme_tcp_icresp_handle(tqpair, pdu);
		break;
	case SPDK_NVME_TCP_PDU_TYPE_CAPSULE_RESP:
		nvme_tcp_capsule_resp_hdr_handle(tqpair, pdu, reaped);
		break;
	case SPDK_NVME_TCP_PDU_TYPE_C2H_DATA:
		nvme_tcp_c2h_data_hdr_handle(tqpair, pdu);
		break;

	case SPDK_NVME_TCP_PDU_TYPE_C2H_TERM_REQ:
		nvme_tcp_c2h_term_req_hdr_handle(tqpair, pdu);
		break;
	case SPDK_NVME_TCP_PDU_TYPE_R2T:
		nvme_tcp_r2t_hdr_handle(tqpair, pdu);
		break;

	default:
		SPDK_ERRLOG("Unexpected PDU type 0x%02x\n", tqpair->recv_pdu->hdr.common.pdu_type);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = 1;
		nvme_tcp_qpair_send_h2c_term_req(tqpair, pdu, fes, error_offset);
		break;
	}

}

static int
nvme_tcp_read_digest(struct spdk_sock *sock, struct nvme_tcp_pdu *pdu, uint32_t len)
{
	struct iovec iov;

	assert(len <= SPDK_NVME_TCP_DIGEST_LEN);

	iov.iov_base = pdu->data_digest + SPDK_NVME_TCP_DIGEST_LEN - len;
	iov.iov_len = len;

	return nvme_tcp_readv_data(sock, &iov, 1);
}

static int
nvme_tcp_read_payload_data_zcopy(struct spdk_sock *sock, struct nvme_tcp_pdu *pdu)
{
	struct nvme_tcp_req *tcp_req = pdu->req;
	struct spdk_sock_buf *sock_buf;
	int ret = 0;

	if (pdu->data_len > pdu->rw_offset) {
		int rc = 0;
		size_t len = pdu->data_len - pdu->rw_offset;

		ret = spdk_sock_recv_zcopy(sock, len, &sock_buf);
		if (ret <= 0) {
			goto fail;
		}

		SPDK_DEBUGLOG(nvme, "Got %d bytes from socket layer\n", ret);

		if (tcp_req->sock_buf) {
			struct spdk_sock_buf *cur_buf = tcp_req->sock_buf;
			while (cur_buf->next) { cur_buf = cur_buf->next; }
			cur_buf->next = sock_buf;
		} else {
			tcp_req->sock_buf = sock_buf;
		}

		if ((size_t)ret != len) {
			/* Part of data is not received, so return directly */
			return ret;
		}

		/* We got all the data. Setup iovs */
		sock_buf = tcp_req->sock_buf;

		assert(tcp_req->req.zcopy.iovcnt == 0);
		while (sock_buf) {
			tcp_req->req.zcopy.iovcnt++;
			sock_buf = sock_buf->next;
		}

		if (spdk_unlikely(tcp_req->req.zcopy.iovcnt > NVME_MAX_ZCOPY_IOVS)) {
			/* fallback memcopy */
			tcp_req->req.zcopy.iovcnt = 0;
			rc = spdk_nvme_request_get_zcopy_buffers(&tcp_req->req, pdu->data_len);
			if (rc == 0) {
				size_t dst_offset = 0;

				tcp_req->iovcnt = tcp_req->req.zcopy.iovcnt;
				sock_buf = tcp_req->sock_buf;
				while (sock_buf) {
					dst_offset += spdk_copy_iov_with_offset(&sock_buf->iov, 1,
										tcp_req->req.zcopy.iovs,
										tcp_req->req.zcopy.iovcnt,
										dst_offset);

					sock_buf = sock_buf->next;
				}

				spdk_sock_free_bufs(sock, tcp_req->sock_buf);
				tcp_req->sock_buf = NULL;

				SPDK_DEBUGLOG(nvme, "Payload is split into %d iovs\n", tcp_req->iovcnt);
			}
		} else {
			if (tcp_req->req.zcopy.iovcnt <= NVME_TCP_MAX_SGL_DESCRIPTORS) {
				tcp_req->req.zcopy.iovs = tcp_req->iov;
			} else {
				rc = spdk_nvme_request_get_zcopy_iovs(&tcp_req->req.zcopy);
			}
			if (rc == 0) {
				assert(tcp_req->iovcnt == 0);
				sock_buf = tcp_req->sock_buf;
				while (sock_buf) {
					tcp_req->req.zcopy.iovs[tcp_req->iovcnt++] = sock_buf->iov;
					sock_buf = sock_buf->next;
				}
				SPDK_DEBUGLOG(nvme, "Payload is split into %d iovs\n", tcp_req->iovcnt);
			}
		}

		if (rc != 0) {
			SPDK_ERRLOG("Failed to set zcopy iov\n");
			spdk_sock_free_bufs(sock, tcp_req->sock_buf);
			tcp_req->sock_buf = NULL;
			tcp_req->rsp.status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		}
	}

	if (pdu->ddgst_enable) {
		int ret_dgst = nvme_tcp_read_digest(sock, pdu, SPDK_NVME_TCP_DIGEST_LEN +
						    pdu->data_len - pdu->rw_offset - ret);
		if (ret_dgst < 0) {
			ret = ret_dgst;
			goto fail;
		}

		ret += ret_dgst;
	}
	return ret;

fail:
	if (ret < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			return 0;
		}

		/* For connect reset issue, do not output error log */
		if (errno != ECONNRESET) {
			SPDK_ERRLOG("spdk_sock_readv() failed, errno %d: %s\n",
				    errno, spdk_strerror(errno));
		}
	}

	/* connection closed */
	return NVME_TCP_CONNECTION_FATAL;
}

static int
nvme_tcp_read_payload_data_memory_domain(struct spdk_sock *sock, struct nvme_tcp_pdu *pdu)
{
	struct nvme_tcp_req *tcp_req = pdu->req;
	struct spdk_sock_buf *sock_buf;
	int ret = 0;

	if (pdu->data_len > pdu->rw_offset) {
		size_t len = pdu->data_len - pdu->rw_offset;

		ret = spdk_sock_recv_zcopy(sock, len, &sock_buf);
		if (ret <= 0) {
			goto fail;
		}

		SPDK_DEBUGLOG(nvme, "Got %d bytes from socket layer\n", ret);

		if (tcp_req->sock_buf) {
			struct spdk_sock_buf *cur_buf = tcp_req->sock_buf;
			while (cur_buf->next) { cur_buf = cur_buf->next; }
			cur_buf->next = sock_buf;
		} else {
			tcp_req->sock_buf = sock_buf;
		}

		if ((size_t)ret != len) {
			/* Part of data is not received, so return directly */
			return ret;
		}
	}

	if (pdu->ddgst_enable) {
		int ret_dgst = nvme_tcp_read_digest(sock, pdu, SPDK_NVME_TCP_DIGEST_LEN +
						    pdu->data_len - pdu->rw_offset - ret);
		if (ret_dgst < 0) {
			ret = ret_dgst;
			goto fail;
		}

		ret += ret_dgst;
	}
	return ret;

fail:
	if (ret < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			return 0;
		}

		/* For connect reset issue, do not output error log */
		if (errno != ECONNRESET) {
			SPDK_ERRLOG("spdk_sock_readv() failed, errno %d: %s\n",
				    errno, spdk_strerror(errno));
		}
	}

	/* connection closed */
	return NVME_TCP_CONNECTION_FATAL;
}

static int
nvme_tcp_read_pdu(struct nvme_tcp_qpair *tqpair, uint32_t *reaped, uint32_t max_completions)
{
	int rc = 0;
	struct nvme_tcp_pdu *pdu;
	uint32_t data_len;
	enum nvme_tcp_pdu_recv_state prev_state;

	*reaped = tqpair->async_complete;
	tqpair->async_complete = 0;

	/* The loop here is to allow for several back-to-back state changes. */
	do {
		if (*reaped >= max_completions) {
			break;
		}

		prev_state = tqpair->recv_state;
		pdu = tqpair->recv_pdu;
		switch (tqpair->recv_state) {
		/* If in a new state */
		case NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY:
			if (pdu) {
				pdu->ch_valid_bytes = 0;
				pdu->psh_valid_bytes = 0;
				pdu->has_hdgst = 0;
				pdu->rw_offset = 0;
				pdu->ddgst_enable = 0;
			}

			nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_CH);
			break;
		/* Wait for the pdu common header */
		case NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_CH:
			if (!pdu) {
				struct spdk_nvme_tcp_common_pdu_hdr common_hdr;

				rc = nvme_tcp_read_data(tqpair->sock,
							sizeof(struct spdk_nvme_tcp_common_pdu_hdr),
							(uint8_t *)&common_hdr);
				if (rc < 0) {
					nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_QUIESCING);
					break;
				} else if (rc == 0) {
					return NVME_TCP_PDU_IN_PROGRESS;
				}

				pdu = tqpair->recv_pdu = nvme_tcp_recv_pdu_get(tqpair);
				if (spdk_unlikely(!pdu)) {
					SPDK_ERRLOG("Failed to get recv pdu\n");
					nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_QUIESCING);
					break;
				}

				pdu->ch_valid_bytes = 0;
				pdu->psh_valid_bytes = 0;
				pdu->has_hdgst = 0;
				pdu->rw_offset = 0;
				pdu->ddgst_enable = 0;

				memcpy(&pdu->hdr.common, &common_hdr, rc);
				pdu->ch_valid_bytes = rc;
			} else {
				assert(pdu->ch_valid_bytes < sizeof(struct spdk_nvme_tcp_common_pdu_hdr));
				rc = nvme_tcp_read_data(tqpair->sock,
							sizeof(struct spdk_nvme_tcp_common_pdu_hdr) - pdu->ch_valid_bytes,
							(uint8_t *)&pdu->hdr.common + pdu->ch_valid_bytes);
				if (rc < 0) {
					nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_QUIESCING);
					break;
				}
				pdu->ch_valid_bytes += rc;
			}

			if (pdu->ch_valid_bytes < sizeof(struct spdk_nvme_tcp_common_pdu_hdr)) {
				return NVME_TCP_PDU_IN_PROGRESS;
			}

			/* The command header of this PDU has now been read from the socket. */
			nvme_tcp_pdu_ch_handle(tqpair);
			break;
		/* Wait for the pdu specific header  */
		case NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PSH:
			assert(pdu->psh_valid_bytes < pdu->psh_len);
			rc = nvme_tcp_read_data(tqpair->sock,
						pdu->psh_len - pdu->psh_valid_bytes,
						(uint8_t *)&pdu->hdr.raw + sizeof(struct spdk_nvme_tcp_common_pdu_hdr) + pdu->psh_valid_bytes);
			if (rc < 0) {
				nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_QUIESCING);
				break;
			}

			pdu->psh_valid_bytes += rc;
			if (pdu->psh_valid_bytes < pdu->psh_len) {
				return NVME_TCP_PDU_IN_PROGRESS;
			}

			/* All header(ch, psh, head digist) of this PDU has now been read from the socket. */
			nvme_tcp_pdu_psh_handle(tqpair, reaped);
			break;
		case NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD:
			/* check whether the data is valid, if not we just return */
			if (!pdu->data_len) {
				return NVME_TCP_PDU_IN_PROGRESS;
			}

			data_len = pdu->data_len;
			/* data digest */
			if (spdk_unlikely((pdu->hdr.common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_C2H_DATA) &&
					  tqpair->flags.host_ddgst_enable)) {
				data_len += SPDK_NVME_TCP_DIGEST_LEN;
				pdu->ddgst_enable = 1;
			}

			if (nvme_tcp_pdu_is_zcopy(pdu)) {
				rc = nvme_tcp_read_payload_data_zcopy(tqpair->sock, pdu);
			} else if (nvme_tcp_req_with_memory_domain(pdu->req)) {
				rc = nvme_tcp_read_payload_data_memory_domain(tqpair->sock, pdu);
			} else {
				rc = nvme_tcp_read_payload_data(tqpair->sock, pdu);
			}
			if (rc < 0) {
				nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_QUIESCING);
				break;
			}

			pdu->rw_offset += rc;
			if (pdu->rw_offset < data_len) {
				return NVME_TCP_PDU_IN_PROGRESS;
			}

			assert(pdu->rw_offset == data_len);
			/* All of this PDU has now been read from the socket. */
			nvme_tcp_pdu_payload_handle(tqpair, reaped);
			break;
		case NVME_TCP_PDU_RECV_STATE_QUIESCING:
			if (TAILQ_EMPTY(&tqpair->outstanding_reqs)) {
				if (nvme_qpair_get_state(&tqpair->qpair) == NVME_QPAIR_DISCONNECTING) {
					nvme_transport_ctrlr_disconnect_qpair_done(&tqpair->qpair);
				}
				nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_ERROR);
			}
			break;
		case NVME_TCP_PDU_RECV_STATE_ERROR:
			if (pdu) {
				memset(pdu, 0, sizeof(struct nvme_tcp_pdu));
			}
			return NVME_TCP_PDU_FATAL;
		default:
			assert(0);
			break;
		}
	} while (prev_state != tqpair->recv_state);

	return rc > 0 ? 0 : rc;
}

static void
nvme_tcp_qpair_check_timeout(struct spdk_nvme_qpair *qpair)
{
	uint64_t t02;
	struct nvme_tcp_req *tcp_req, *tmp;
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(qpair);
	struct spdk_nvme_ctrlr *ctrlr = qpair->ctrlr;
	struct spdk_nvme_ctrlr_process *active_proc;

	/* Don't check timeouts during controller initialization. */
	if (ctrlr->state != NVME_CTRLR_STATE_READY) {
		return;
	}

	if (nvme_qpair_is_admin_queue(qpair)) {
		active_proc = nvme_ctrlr_get_current_process(ctrlr);
	} else {
		active_proc = qpair->active_proc;
	}

	/* Only check timeouts if the current process has a timeout callback. */
	if (active_proc == NULL || active_proc->timeout_cb_fn == NULL) {
		return;
	}

	t02 = spdk_get_ticks();
	TAILQ_FOREACH_SAFE(tcp_req, &tqpair->outstanding_reqs, link, tmp) {
		if (nvme_request_check_timeout(&tcp_req->req, tcp_req->cid, active_proc, t02)) {
			/*
			 * The requests are in order, so as soon as one has not timed out,
			 * stop iterating.
			 */
			break;
		}
	}
}

static int nvme_tcp_ctrlr_connect_qpair_poll(struct spdk_nvme_ctrlr *ctrlr,
		struct spdk_nvme_qpair *qpair);

static int
nvme_tcp_qpair_process_completions(struct spdk_nvme_qpair *qpair, uint32_t max_completions)
{
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(qpair);
	uint32_t reaped;
	int rc;

	if (qpair->poll_group == NULL) {
		rc = spdk_sock_flush(tqpair->sock);
		if (rc < 0) {
			SPDK_ERRLOG("Failed to flush tqpair=%p (%d): %s\n", tqpair,
				    errno, spdk_strerror(errno));
			if (spdk_unlikely(tqpair->qpair.ctrlr->timeout_enabled)) {
				nvme_tcp_qpair_check_timeout(qpair);
			}
			if (nvme_qpair_get_state(qpair) == NVME_QPAIR_DISCONNECTING) {
				if (TAILQ_EMPTY(&tqpair->outstanding_reqs)) {
					nvme_transport_ctrlr_disconnect_qpair_done(qpair);
				}
				return 0;
			}
			return rc;
		}
	}

	if (max_completions == 0) {
		max_completions = spdk_max(tqpair->num_entries, 1);
	} else {
		max_completions = spdk_min(max_completions, tqpair->num_entries);
	}

	reaped = 0;
	rc = nvme_tcp_read_pdu(tqpair, &reaped, max_completions);
	if (rc < 0) {
		SPDK_DEBUGLOG(nvme, "Error polling CQ! qpair %d, rc %d(%d): %s\n",
			      tqpair->qpair.id, rc, errno, spdk_strerror(errno));
		goto fail;
	}

	if (spdk_unlikely(tqpair->qpair.ctrlr->timeout_enabled)) {
		nvme_tcp_qpair_check_timeout(qpair);
	}

	if (spdk_unlikely(nvme_qpair_get_state(qpair) == NVME_QPAIR_CONNECTING)) {
		rc = nvme_tcp_ctrlr_connect_qpair_poll(qpair->ctrlr, qpair);
		if (rc != 0 && rc != -EAGAIN) {
			SPDK_ERRLOG("Failed to connect tqpair=%p\n", tqpair);
			goto fail;
		} else if (rc == 0) {
			/* Once the connection is completed, we can submit queued requests */
			nvme_qpair_resubmit_requests(qpair, tqpair->num_entries);
		}
	}

	return reaped;
fail:

	/*
	 * Since admin queues take the ctrlr_lock before entering this function,
	 * we can call nvme_transport_ctrlr_disconnect_qpair. For other qpairs we need
	 * to call the generic function which will take the lock for us.
	 */
	qpair->transport_failure_reason = SPDK_NVME_QPAIR_FAILURE_UNKNOWN;

	if (nvme_qpair_is_admin_queue(qpair)) {
		nvme_transport_ctrlr_disconnect_qpair(qpair->ctrlr, qpair);
	} else {
		nvme_ctrlr_disconnect_qpair(qpair);
	}
	return -ENXIO;
}

static void
nvme_tcp_qpair_sock_cb(void *ctx, struct spdk_sock_group *group, struct spdk_sock *sock)
{
	struct spdk_nvme_qpair *qpair = ctx;
	struct nvme_tcp_poll_group *pgroup = nvme_tcp_poll_group(qpair->poll_group);
	int32_t num_completions;
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(qpair);

	if (tqpair->needs_poll) {
		TAILQ_REMOVE(&pgroup->needs_poll, tqpair, link);
		tqpair->needs_poll = false;
	}

	num_completions = spdk_nvme_qpair_process_completions(qpair, pgroup->completions_per_qpair);

	if (pgroup->num_completions >= 0 && num_completions >= 0) {
		pgroup->num_completions += num_completions;
		pgroup->stats.nvme_completions += num_completions;
	} else {
		pgroup->num_completions = -ENXIO;
	}
}

static int
nvme_tcp_qpair_icreq_send(struct nvme_tcp_qpair *tqpair)
{
	struct spdk_nvme_tcp_ic_req *ic_req;
	struct nvme_tcp_pdu *pdu;

	pdu = tqpair->send_pdu;
	memset(tqpair->send_pdu, 0, sizeof(*tqpair->send_pdu));
	ic_req = &pdu->hdr.ic_req;

	ic_req->common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_IC_REQ;
	ic_req->common.hlen = ic_req->common.plen = sizeof(*ic_req);
	ic_req->pfv = 0;
	ic_req->maxr2t = NVME_TCP_MAX_R2T_DEFAULT - 1;
	ic_req->hpda = NVME_TCP_HPDA_DEFAULT;

	ic_req->dgst.bits.hdgst_enable = tqpair->qpair.ctrlr->opts.header_digest;
	ic_req->dgst.bits.ddgst_enable = tqpair->qpair.ctrlr->opts.data_digest;

	nvme_tcp_qpair_write_pdu(tqpair, pdu, nvme_tcp_send_icreq_complete, tqpair);

	tqpair->icreq_timeout_tsc = spdk_get_ticks() + (NVME_TCP_TIME_OUT_IN_SECONDS * spdk_get_ticks_hz());
	return 0;
}

static int
nvme_tcp_qpair_connect_sock(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	struct sockaddr_storage dst_addr;
	struct sockaddr_storage src_addr;
	int rc;
	struct nvme_tcp_qpair *tqpair;
	int family;
	long int port;
	char *sock_impl_name;
	struct spdk_sock_impl_opts impl_opts;
	size_t impl_opts_size = sizeof(impl_opts);
	struct spdk_sock_opts opts;

	tqpair = nvme_tcp_qpair(qpair);

	switch (ctrlr->trid.adrfam) {
	case SPDK_NVMF_ADRFAM_IPV4:
		family = AF_INET;
		break;
	case SPDK_NVMF_ADRFAM_IPV6:
		family = AF_INET6;
		break;
	default:
		SPDK_ERRLOG("Unhandled ADRFAM %d\n", ctrlr->trid.adrfam);
		rc = -1;
		return rc;
	}

	SPDK_DEBUGLOG(nvme, "adrfam %d ai_family %d\n", ctrlr->trid.adrfam, family);

	memset(&dst_addr, 0, sizeof(dst_addr));

	SPDK_DEBUGLOG(nvme, "trsvcid is %s\n", ctrlr->trid.trsvcid);
	rc = nvme_tcp_parse_addr(&dst_addr, family, ctrlr->trid.traddr, ctrlr->trid.trsvcid);
	if (rc != 0) {
		SPDK_ERRLOG("dst_addr nvme_tcp_parse_addr() failed\n");
		return rc;
	}

	if (ctrlr->opts.src_addr[0] || ctrlr->opts.src_svcid[0]) {
		memset(&src_addr, 0, sizeof(src_addr));
		rc = nvme_tcp_parse_addr(&src_addr, family, ctrlr->opts.src_addr, ctrlr->opts.src_svcid);
		if (rc != 0) {
			SPDK_ERRLOG("src_addr nvme_tcp_parse_addr() failed\n");
			return rc;
		}
	}

	port = spdk_strtol(ctrlr->trid.trsvcid, 10);
	if (port <= 0 || port >= INT_MAX) {
		SPDK_ERRLOG("Invalid port: %s\n", ctrlr->trid.trsvcid);
		rc = -1;
		return rc;
	}

	sock_impl_name = ctrlr->opts.psk[0] ? "ssl" : NULL;
	SPDK_DEBUGLOG(nvme, "sock_impl_name is %s\n", sock_impl_name);

	spdk_sock_impl_get_opts(sock_impl_name, &impl_opts, &impl_opts_size);
	impl_opts.enable_ktls = false;
	impl_opts.tls_version = SPDK_TLS_VERSION_1_3;
	/* TODO: Change current PSK HEX string format to TLS PSK Interchange Format */
	impl_opts.psk_key = ctrlr->opts.psk;
	/* TODO: generate identity from hostnqn instead */
	impl_opts.psk_identity = "psk.spdk.io";

	opts.opts_size = sizeof(opts);
	spdk_sock_get_default_opts(&opts);
	opts.priority = ctrlr->trid.priority;
	opts.zcopy = !nvme_qpair_is_admin_queue(qpair);
	if (ctrlr->opts.transport_ack_timeout) {
		opts.ack_timeout = 1ULL << ctrlr->opts.transport_ack_timeout;
	}
	if (sock_impl_name) {
		opts.impl_opts = &impl_opts;
		opts.impl_opts_size = sizeof(impl_opts);
	}
	tqpair->sock = spdk_sock_connect_ext(ctrlr->trid.traddr, port, sock_impl_name, &opts);
	if (!tqpair->sock) {
		SPDK_ERRLOG("sock connection error of tqpair=%p with addr=%s, port=%ld\n",
			    tqpair, ctrlr->trid.traddr, port);
		rc = -1;
		return rc;
	}

	return 0;
}

static int
nvme_tcp_ctrlr_connect_qpair_poll(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	struct nvme_tcp_qpair *tqpair;
	int rc;

	tqpair = nvme_tcp_qpair(qpair);

	/* Prevent this function from being called recursively, as it could lead to issues with
	 * nvme_fabric_qpair_connect_poll() if the connect response is received in the recursive
	 * call.
	 */
	if (tqpair->flags.in_connect_poll) {
		return -EAGAIN;
	}

	tqpair->flags.in_connect_poll = 1;

	switch (tqpair->state) {
	case NVME_TCP_QPAIR_STATE_INVALID:
	case NVME_TCP_QPAIR_STATE_INITIALIZING:
		if (spdk_get_ticks() > tqpair->icreq_timeout_tsc) {
			SPDK_ERRLOG("Failed to construct the tqpair=%p via correct icresp\n", tqpair);
			rc = -ETIMEDOUT;
			break;
		}
		rc = -EAGAIN;
		break;
	case NVME_TCP_QPAIR_STATE_FABRIC_CONNECT_SEND:
		rc = nvme_fabric_qpair_connect_async(&tqpair->qpair, tqpair->num_entries + 1);
		if (rc < 0) {
			SPDK_ERRLOG("Failed to send an NVMe-oF Fabric CONNECT command\n");
			break;
		}
		tqpair->state = NVME_TCP_QPAIR_STATE_FABRIC_CONNECT_POLL;
		rc = -EAGAIN;
		break;
	case NVME_TCP_QPAIR_STATE_FABRIC_CONNECT_POLL:
		rc = nvme_fabric_qpair_connect_poll(&tqpair->qpair);
		if (rc == 0) {
			tqpair->state = NVME_TCP_QPAIR_STATE_RUNNING;
			nvme_qpair_set_state(qpair, NVME_QPAIR_CONNECTED);
		} else if (rc != -EAGAIN) {
			SPDK_ERRLOG("Failed to poll NVMe-oF Fabric CONNECT command\n");
		}
		break;
	case NVME_TCP_QPAIR_STATE_RUNNING:
		rc = 0;
		break;
	default:
		assert(false);
		rc = -EINVAL;
		break;
	}

	tqpair->flags.in_connect_poll = 0;
	return rc;
}

static int
nvme_tcp_ctrlr_connect_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	int rc = 0;
	struct nvme_tcp_qpair *tqpair;
	struct nvme_tcp_poll_group *tgroup = NULL;;
	struct spdk_sock_caps sock_caps = {};
	void *tcp_reqs;

	tqpair = nvme_tcp_qpair(qpair);

	if (!tqpair->sock) {
		rc = nvme_tcp_qpair_connect_sock(ctrlr, qpair);
		if (rc < 0) {
			return rc;
		}
	}

	if (qpair->poll_group) {
		rc = nvme_poll_group_connect_qpair(qpair);
		if (rc) {
			SPDK_ERRLOG("Unable to activate the tcp qpair.\n");
			return rc;
		}
		tgroup = nvme_tcp_poll_group(qpair->poll_group);
	} else if (!tqpair->stats) {
		tqpair->stats = calloc(1, sizeof(*tqpair->stats));
		if (!tqpair->stats) {
			SPDK_ERRLOG("tcp stats memory allocation failed\n");
			return -ENOMEM;
		}
	}

	rc = spdk_sock_get_caps(tqpair->sock, &sock_caps);
	if (rc == 0) {
		struct spdk_rdma_utils_memory_translation mem_translation = {};

		tqpair->pd = sock_caps.ibv_pd;
		if (tqpair->pd) {
			char *tcp_mem_domain = getenv("SPDK_NVDA_TCP_USE_TCP_MEM_DOMAIN");
			if (tcp_mem_domain) {
				SPDK_NOTICELOG("Using TCP memory domain\n");
				tqpair->memory_domain = spdk_rdma_get_tcp_memory_domain(tqpair->pd);
			} else {
				SPDK_NOTICELOG("Using RDMA memory domain\n");
				tqpair->memory_domain = spdk_rdma_get_memory_domain(tqpair->pd);
			}

			if (!tqpair->memory_domain) {
				SPDK_ERRLOG("Failed to get memory domain\n");
				return -ENOTSUP;
			}

			if (!nvme_qpair_is_admin_queue(&tqpair->qpair)) {
				SPDK_NOTICELOG("TCP qpair %p %u, PD %p\n", tqpair, tqpair->qpair.id, tqpair->pd);

				tqpair->mem_map = spdk_rdma_utils_create_mem_map(tqpair->pd, NULL,
										 IBV_ACCESS_LOCAL_WRITE);
				if (!tqpair->mem_map) {
					SPDK_ERRLOG("Failed to create memory map\n");
					return -ENOTSUP;
				}

				tcp_reqs = (qpair->poll_group && tgroup->tcp_reqs) ?
					   tgroup->tcp_reqs : tqpair->tcp_reqs;
				rc = spdk_rdma_utils_get_translation(tqpair->mem_map, tcp_reqs,
							       tqpair->num_entries * sizeof(struct nvme_tcp_req),
							       &mem_translation);
				if (rc) {
					SPDK_ERRLOG("Failed to get mkey for PDUs\n");
					return -ENOTSUP;
				}
				tqpair->pdus_mkey = spdk_rdma_utils_memory_translation_get_lkey(&mem_translation);
			} else {
				if (nvme_tcp_memory_domain_enabled() &&
				    getenv("SPDK_NVDA_TCP_DISABLE_ACCEL_SEQ") == NULL) {
					ctrlr->flags |= SPDK_NVME_CTRLR_ACCEL_SEQUENCE_SUPPORTED;
				} else {
					SPDK_NOTICELOG("Accel sequence support disabled\n");
				}
			}
		}
	} else {
		SPDK_NOTICELOG("Socket layer doesn't report capabilities. Full zero-copy write is disabled.\n");
	}

	tqpair->maxr2t = NVME_TCP_MAX_R2T_DEFAULT;
	/* Explicitly set the state and recv_state of tqpair */
	tqpair->state = NVME_TCP_QPAIR_STATE_INVALID;
	if (tqpair->recv_state != NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY) {
		nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY);
	}
	rc = nvme_tcp_qpair_icreq_send(tqpair);
	if (rc != 0) {
		SPDK_ERRLOG("Unable to connect the tqpair\n");
		return rc;
	}

	return rc;
}

static struct spdk_nvme_qpair *
nvme_tcp_ctrlr_create_qpair(struct spdk_nvme_ctrlr *ctrlr,
			    uint16_t qid, uint32_t qsize,
			    enum spdk_nvme_qprio qprio,
			    uint32_t num_requests, bool async)
{
	struct nvme_tcp_qpair *tqpair;
	struct spdk_nvme_qpair *qpair;
	int rc;

	if (qsize < SPDK_NVME_QUEUE_MIN_ENTRIES) {
		SPDK_ERRLOG("Failed to create qpair with size %u. Minimum queue size is %d.\n",
			    qsize, SPDK_NVME_QUEUE_MIN_ENTRIES);
		return NULL;
	}

	tqpair = calloc(1, sizeof(struct nvme_tcp_qpair));
	if (!tqpair) {
		SPDK_ERRLOG("failed to get create tqpair\n");
		return NULL;
	}

	/* Set num_entries one less than queue size. According to NVMe
	 * and NVMe-oF specs we can not submit queue size requests,
	 * one slot shall always remain empty.
	 */
	tqpair->num_entries = qsize - 1;
	qpair = &tqpair->qpair;

	rc = nvme_qpair_init(qpair, qid, ctrlr, 1, num_requests, async);
	if (rc != 0) {
		free(tqpair);
		return NULL;
	}

	rc = nvme_tcp_alloc_reqs(tqpair);
	if (rc) {
		nvme_tcp_ctrlr_delete_io_qpair(ctrlr, qpair);
		return NULL;
	}

	/* spdk_nvme_qpair_get_optimal_poll_group needs socket information.
	 * So create the socket first when creating a qpair. */
	rc = nvme_tcp_qpair_connect_sock(ctrlr, qpair);
	if (rc) {
		nvme_tcp_ctrlr_delete_io_qpair(ctrlr, qpair);
		return NULL;
	}

	return qpair;
}

static struct spdk_nvme_qpair *
nvme_tcp_ctrlr_create_io_qpair(struct spdk_nvme_ctrlr *ctrlr, uint16_t qid,
			       const struct spdk_nvme_io_qpair_opts *opts)
{
	return nvme_tcp_ctrlr_create_qpair(ctrlr, qid, opts->io_queue_size, opts->qprio,
					   opts->io_queue_requests, opts->async_mode);
}

/* We have to use the typedef in the function declaration to appease astyle. */
typedef struct spdk_nvme_ctrlr spdk_nvme_ctrlr_t;

static spdk_nvme_ctrlr_t *
nvme_tcp_ctrlr_construct(const struct spdk_nvme_transport_id *trid,
			 const struct spdk_nvme_ctrlr_opts *opts,
			 void *devhandle)
{
	struct nvme_tcp_ctrlr *tctrlr;
	struct nvme_tcp_qpair *tqpair;
	struct spdk_sock_caps sock_caps = {};
	int rc;

	tctrlr = calloc(1, sizeof(*tctrlr));
	if (tctrlr == NULL) {
		SPDK_ERRLOG("could not allocate ctrlr\n");
		return NULL;
	}

	tctrlr->ctrlr.opts = *opts;
	tctrlr->ctrlr.trid = *trid;

	if (opts->transport_ack_timeout > NVME_TCP_CTRLR_MAX_TRANSPORT_ACK_TIMEOUT) {
		SPDK_NOTICELOG("transport_ack_timeout exceeds max value %d, use max value\n",
			       NVME_TCP_CTRLR_MAX_TRANSPORT_ACK_TIMEOUT);
		tctrlr->ctrlr.opts.transport_ack_timeout = NVME_TCP_CTRLR_MAX_TRANSPORT_ACK_TIMEOUT;
	}

	rc = nvme_ctrlr_construct(&tctrlr->ctrlr);
	if (rc != 0) {
		free(tctrlr);
		return NULL;
	}

	tctrlr->ctrlr.adminq = nvme_tcp_ctrlr_create_qpair(&tctrlr->ctrlr, 0,
			       tctrlr->ctrlr.opts.admin_queue_size, 0,
			       tctrlr->ctrlr.opts.admin_queue_size + 1, true);
	if (!tctrlr->ctrlr.adminq) {
		SPDK_ERRLOG("failed to create admin qpair\n");
		nvme_tcp_ctrlr_destruct(&tctrlr->ctrlr);
		return NULL;
	}

	tqpair = nvme_tcp_qpair(tctrlr->ctrlr.adminq);
	rc = spdk_sock_get_caps(tqpair->sock, &sock_caps);
	if (rc == 0 && sock_caps.zcopy_recv) {
		tctrlr->ctrlr.flags |= SPDK_NVME_CTRLR_ZCOPY_SUPPORTED;
		SPDK_NOTICELOG("Controller supports zero copy API\n");
	}

	if (nvme_ctrlr_add_process(&tctrlr->ctrlr, 0) != 0) {
		SPDK_ERRLOG("nvme_ctrlr_add_process() failed\n");
		nvme_ctrlr_destruct(&tctrlr->ctrlr);
		return NULL;
	}

	return &tctrlr->ctrlr;
}

static uint32_t
nvme_tcp_ctrlr_get_max_xfer_size(struct spdk_nvme_ctrlr *ctrlr)
{
	/* TCP transport doesn't limit maximum IO transfer size. */
	return UINT32_MAX;
}

static uint16_t
nvme_tcp_ctrlr_get_max_sges(struct spdk_nvme_ctrlr *ctrlr)
{
	/*
	 * We do not support >1 SGE in the initiator currently,
	 *  so we can only return 1 here.  Once that support is
	 *  added, this should return ctrlr->cdata.nvmf_specific.msdbd
	 *  instead.
	 */
	return NVME_TCP_MAX_SGL_DESCRIPTORS;
}

static int
nvme_tcp_qpair_iterate_requests(struct spdk_nvme_qpair *qpair,
				int (*iter_fn)(struct nvme_request *req, void *arg),
				void *arg)
{
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(qpair);
	struct nvme_tcp_req *tcp_req, *tmp;
	int rc;

	assert(iter_fn != NULL);

	TAILQ_FOREACH_SAFE(tcp_req, &tqpair->outstanding_reqs, link, tmp) {
		rc = iter_fn(&tcp_req->req, arg);
		if (rc != 0) {
			return rc;
		}
	}

	return 0;
}

static void
nvme_tcp_admin_qpair_abort_aers(struct spdk_nvme_qpair *qpair)
{
	struct nvme_tcp_req *tcp_req, *tmp;
	struct spdk_nvme_cpl cpl = {};
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(qpair);

	cpl.status.sc = SPDK_NVME_SC_ABORTED_SQ_DELETION;
	cpl.status.sct = SPDK_NVME_SCT_GENERIC;

	TAILQ_FOREACH_SAFE(tcp_req, &tqpair->outstanding_reqs, link, tmp) {
		if (tcp_req->req.cmd.opc != SPDK_NVME_OPC_ASYNC_EVENT_REQUEST) {
			continue;
		}

		nvme_tcp_req_complete(tcp_req, tqpair, &cpl, false);
	}
}

static struct spdk_nvme_transport_poll_group *
nvme_tcp_poll_group_create(void)
{
	struct nvme_tcp_poll_group *group = calloc(1, sizeof(*group));
	struct nvme_tcp_req	*tcp_req;
	struct nvme_tcp_pdu	*pdu;
	size_t req_size_padded, pdu_size_padded;
	uint16_t num_requests, i;
	int rc;

	if (group == NULL) {
		SPDK_ERRLOG("Unable to allocate poll group.\n");
		return NULL;
	}

	TAILQ_INIT(&group->needs_poll);

	rc = nvme_transport_poll_group_init(&group->group, 0);
	if (rc != 0) {
		free(group);
		return NULL;
	}

	num_requests = g_spdk_nvme_transport_opts.poll_group_requests;

	if (num_requests != 0) {
		req_size_padded = SPDK_ALIGN_CEIL(sizeof(struct nvme_tcp_req), 64);
		group->tcp_reqs = spdk_zmalloc(num_requests * req_size_padded, 64, NULL,
					       SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
		if (group->tcp_reqs == NULL) {
			SPDK_ERRLOG("Failed to allocate tcp_reqs on poll group %p\n", group);
			goto fail;
		}

		for (i = 0; i < num_requests; i++) {
			tcp_req = group->tcp_reqs + i * req_size_padded;
			tcp_req->cid = UINT16_MAX;
			STAILQ_INSERT_HEAD(&group->group.free_req, &tcp_req->req, stailq);
			tcp_req->pdu.sock_req.mkeys = tcp_req->pdu.mkeys;
		}

		pdu_size_padded = SPDK_ALIGN_CEIL(sizeof(struct nvme_tcp_pdu), 64);

		/* @todo: what should be the size of recv pdus pool? */
		group->recv_pdus = spdk_zmalloc(num_requests * pdu_size_padded, 0x1000, NULL,
						SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);

		if (group->recv_pdus == NULL) {
			SPDK_ERRLOG("Failed to allocate recv_pdus on poll group %p\n", group);
			goto fail;
		}

		TAILQ_INIT(&group->free_pdus);
		for (i = 0; i < num_requests; i++) {
			pdu = group->recv_pdus + i * pdu_size_padded;
			TAILQ_INSERT_TAIL(&group->free_pdus, pdu, tailq);
		}
	}

	group->sock_group = spdk_sock_group_create(group);
	if (group->sock_group == NULL) {
		SPDK_ERRLOG("Unable to allocate sock group.\n");
		goto fail;
	}

	return &group->group;

 fail:
	nvme_transport_poll_group_deinit(&group->group);
	spdk_free(group->tcp_reqs);
	spdk_free(group->recv_pdus);
	free(group);
	return NULL;
}

static struct spdk_nvme_transport_poll_group *
nvme_tcp_qpair_get_optimal_poll_group(struct spdk_nvme_qpair *qpair)
{
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(qpair);
	struct spdk_sock_group *group = NULL;
	int rc;

	rc = spdk_sock_get_optimal_sock_group(tqpair->sock, &group, NULL);
	if (!rc && group != NULL) {
		return spdk_sock_group_get_ctx(group);
	}

	return NULL;
}

static int
nvme_tcp_poll_group_connect_qpair(struct spdk_nvme_qpair *qpair)
{
	struct nvme_tcp_poll_group *group = nvme_tcp_poll_group(qpair->poll_group);
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(qpair);

	if (spdk_sock_group_add_sock(group->sock_group, tqpair->sock, nvme_tcp_qpair_sock_cb, qpair)) {
		return -EPROTO;
	}
	return 0;
}

static int
nvme_tcp_poll_group_disconnect_qpair(struct spdk_nvme_qpair *qpair)
{
	struct nvme_tcp_poll_group *group = nvme_tcp_poll_group(qpair->poll_group);
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(qpair);

	if (tqpair->needs_poll) {
		TAILQ_REMOVE(&group->needs_poll, tqpair, link);
		tqpair->needs_poll = false;
	}

	if (tqpair->sock && group->sock_group) {
		if (spdk_sock_group_remove_sock(group->sock_group, tqpair->sock)) {
			return -EPROTO;
		}
	}
	return 0;
}

static int
nvme_tcp_poll_group_add(struct spdk_nvme_transport_poll_group *tgroup,
			struct spdk_nvme_qpair *qpair)
{
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(qpair);
	struct nvme_tcp_poll_group *group = nvme_tcp_poll_group(tgroup);

	/* disconnected qpairs won't have a sock to add. */
	if (nvme_qpair_get_state(qpair) >= NVME_QPAIR_CONNECTED) {
		if (spdk_sock_group_add_sock(group->sock_group, tqpair->sock, nvme_tcp_qpair_sock_cb, qpair)) {
			return -EPROTO;
		}
	}

	tqpair->recv_pdu = NULL;
	tqpair->stats = &group->stats;
	tqpair->shared_stats = true;

	if (group->tcp_reqs != NULL) {
		tqpair->flags.use_poll_group_req_pool = 1;
		qpair->active_free_req = &tgroup->free_req;
	}

	return 0;
}

static int
nvme_tcp_poll_group_remove(struct spdk_nvme_transport_poll_group *tgroup,
			   struct spdk_nvme_qpair *qpair)
{
	struct nvme_tcp_qpair *tqpair;
	struct nvme_tcp_poll_group *group;

	assert(qpair->poll_group_tailq_head == &tgroup->disconnected_qpairs);

	tqpair = nvme_tcp_qpair(qpair);
	group = nvme_tcp_poll_group(tgroup);

	assert(tqpair->shared_stats == true);
	tqpair->stats = &g_dummy_stats;

	if (tqpair->needs_poll) {
		TAILQ_REMOVE(&group->needs_poll, tqpair, link);
		tqpair->needs_poll = false;
	}

	return 0;
}

static int64_t
nvme_tcp_poll_group_process_completions(struct spdk_nvme_transport_poll_group *tgroup,
					uint32_t completions_per_qpair, spdk_nvme_disconnected_qpair_cb disconnected_qpair_cb)
{
	struct nvme_tcp_poll_group *group = nvme_tcp_poll_group(tgroup);
	struct spdk_nvme_qpair *qpair, *tmp_qpair;
	struct nvme_tcp_qpair *tqpair, *tmp_tqpair;
	int num_events;

	if (group->in_polling) {
		return 0;
	}

	group->in_polling = true;
	group->completions_per_qpair = completions_per_qpair;
	group->num_completions = 0;
	group->stats.polls++;

	num_events = spdk_sock_group_poll(group->sock_group);

	STAILQ_FOREACH_SAFE(qpair, &tgroup->disconnected_qpairs, poll_group_stailq, tmp_qpair) {
		if (qpair->outstanding_zcopy_reqs > 0) {
			SPDK_DEBUGLOG(nvme, "Cannot destroy qpair %u because %d zcopy reqs is pending.\n",
				      qpair->id, qpair->outstanding_zcopy_reqs);
			continue;
		}

		tqpair = nvme_tcp_qpair(qpair);

		if (tqpair->sock != NULL) {
			int rc = spdk_sock_close(&tqpair->sock);
			if (tqpair->sock != NULL) {
				SPDK_ERRLOG("tqpair=%p, errno=%d, rc=%d\n", tqpair, errno, rc);
				/* Set it to NULL manually */
				tqpair->sock = NULL;
			}
		}
		if (nvme_qpair_get_state(qpair) == NVME_QPAIR_DISCONNECTING) {
			if (TAILQ_EMPTY(&tqpair->outstanding_reqs)) {
				nvme_transport_ctrlr_disconnect_qpair_done(qpair);
			}
		}

		if (nvme_qpair_get_state(qpair) == NVME_QPAIR_DISCONNECTED) {
			disconnected_qpair_cb(qpair, tgroup->group->ctx);
		}
	}

	/* If any qpairs were marked as needing to be polled due to an asynchronous write completion
	 * and they weren't polled as a consequence of calling spdk_sock_group_poll above, poll them now. */
	TAILQ_FOREACH_SAFE(tqpair, &group->needs_poll, link, tmp_tqpair) {
		nvme_tcp_qpair_sock_cb(&tqpair->qpair, group->sock_group, tqpair->sock);
	}

	group->in_polling = false;

	if (spdk_unlikely(num_events < 0)) {
		return num_events;
	}

	group->stats.idle_polls += !num_events;
	group->stats.socket_completions += num_events;

	return group->num_completions;
}

static int
nvme_tcp_poll_group_destroy(struct spdk_nvme_transport_poll_group *tgroup)
{
	int rc;
	struct nvme_tcp_poll_group *group = nvme_tcp_poll_group(tgroup);

	if (!STAILQ_EMPTY(&tgroup->connected_qpairs) || !STAILQ_EMPTY(&tgroup->disconnected_qpairs)) {
		return -EBUSY;
	}

	rc = spdk_sock_group_close(&group->sock_group);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to close the sock group for a tcp poll group.\n");
		assert(false);
	}

	nvme_transport_poll_group_deinit(&group->group);
	spdk_free(group->tcp_reqs);
	spdk_free(group->recv_pdus);
	free(tgroup);

	return 0;
}

static int
nvme_tcp_poll_group_get_stats(struct spdk_nvme_transport_poll_group *tgroup,
			      struct spdk_nvme_transport_poll_group_stat **_stats)
{
	struct nvme_tcp_poll_group *group;
	struct spdk_nvme_transport_poll_group_stat *stats;

	if (tgroup == NULL || _stats == NULL) {
		SPDK_ERRLOG("Invalid stats or group pointer\n");
		return -EINVAL;
	}

	group = nvme_tcp_poll_group(tgroup);

	stats = calloc(1, sizeof(*stats));
	if (!stats) {
		SPDK_ERRLOG("Can't allocate memory for TCP stats\n");
		return -ENOMEM;
	}
	stats->trtype = SPDK_NVME_TRANSPORT_CUSTOM_FABRICS;
	snprintf(stats->trname, SPDK_NVMF_TRSTRING_MAX_LEN, "%s", "NVDA_TCP");
	memcpy(&stats->tcp, &group->stats, sizeof(group->stats));

	*_stats = stats;

	return 0;
}

static void
nvme_tcp_poll_group_free_stats(struct spdk_nvme_transport_poll_group *tgroup,
			       struct spdk_nvme_transport_poll_group_stat *stats)
{
	free(stats);
}

static bool
nvme_tcp_memory_domain_enabled(void)
{
	const char *module_name;

	return getenv("SPDK_NVDA_TCP_DISABLE_MEM_DOMAIN") == NULL &&
		spdk_accel_get_opc_module_name(ACCEL_OPC_COPY, &module_name) == 0 &&
		strcmp(module_name, "mlx5") == 0;
}

static int
nvme_tcp_ctrlr_get_memory_domains(const struct spdk_nvme_ctrlr *ctrlr,
				  struct spdk_memory_domain **domains, int array_size)
{
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(ctrlr->adminq);

	if (!tqpair->memory_domain || !nvme_tcp_memory_domain_enabled()) {
		SPDK_NOTICELOG("Memory domain support disabled\n");
		return 0;
	} else if (domains && array_size > 0) {
		domains[0] = tqpair->memory_domain->domain;
	}

	return 1;
}

static const struct spdk_nvme_transport_ops tcp_ops = {
	.name = "NVDA_TCP",
	.type = SPDK_NVME_TRANSPORT_CUSTOM_FABRICS,
	.ctrlr_construct = nvme_tcp_ctrlr_construct,
	.ctrlr_scan = nvme_fabric_ctrlr_scan,
	.ctrlr_destruct = nvme_tcp_ctrlr_destruct,
	.ctrlr_enable = nvme_tcp_ctrlr_enable,

	.ctrlr_set_reg_4 = nvme_fabric_ctrlr_set_reg_4,
	.ctrlr_set_reg_8 = nvme_fabric_ctrlr_set_reg_8,
	.ctrlr_get_reg_4 = nvme_fabric_ctrlr_get_reg_4,
	.ctrlr_get_reg_8 = nvme_fabric_ctrlr_get_reg_8,
	.ctrlr_set_reg_4_async = nvme_fabric_ctrlr_set_reg_4_async,
	.ctrlr_set_reg_8_async = nvme_fabric_ctrlr_set_reg_8_async,
	.ctrlr_get_reg_4_async = nvme_fabric_ctrlr_get_reg_4_async,
	.ctrlr_get_reg_8_async = nvme_fabric_ctrlr_get_reg_8_async,

	.ctrlr_get_max_xfer_size = nvme_tcp_ctrlr_get_max_xfer_size,
	.ctrlr_get_max_sges = nvme_tcp_ctrlr_get_max_sges,

	.ctrlr_create_io_qpair = nvme_tcp_ctrlr_create_io_qpair,
	.ctrlr_delete_io_qpair = nvme_tcp_ctrlr_delete_io_qpair,
	.ctrlr_connect_qpair = nvme_tcp_ctrlr_connect_qpair,
	.ctrlr_disconnect_qpair = nvme_tcp_ctrlr_disconnect_qpair,

	.ctrlr_get_memory_domains = nvme_tcp_ctrlr_get_memory_domains,

	.qpair_abort_reqs = nvme_tcp_qpair_abort_reqs,
	.qpair_reset = nvme_tcp_qpair_reset,
	.qpair_submit_request = nvme_tcp_qpair_submit_request,
	.qpair_process_completions = nvme_tcp_qpair_process_completions,
	.qpair_iterate_requests = nvme_tcp_qpair_iterate_requests,
	.admin_qpair_abort_aers = nvme_tcp_admin_qpair_abort_aers,

	.poll_group_create = nvme_tcp_poll_group_create,
	.qpair_get_optimal_poll_group = nvme_tcp_qpair_get_optimal_poll_group,
	.poll_group_connect_qpair = nvme_tcp_poll_group_connect_qpair,
	.poll_group_disconnect_qpair = nvme_tcp_poll_group_disconnect_qpair,
	.poll_group_add = nvme_tcp_poll_group_add,
	.poll_group_remove = nvme_tcp_poll_group_remove,
	.poll_group_process_completions = nvme_tcp_poll_group_process_completions,
	.poll_group_destroy = nvme_tcp_poll_group_destroy,
	.poll_group_get_stats = nvme_tcp_poll_group_get_stats,
	.poll_group_free_stats = nvme_tcp_poll_group_free_stats,

	.qpair_free_request = nvme_tcp_qpair_free_request,
};

SPDK_NVME_TRANSPORT_REGISTER(tcp, &tcp_ops);

SPDK_TRACE_REGISTER_FN(nvme_nvda_tcp, "nvme_nvda_tcp", TRACE_GROUP_NVME_NVDA_TCP)
{
	struct spdk_trace_tpoint_opts opts[] = {
		{
			"NVME_NVDA_TCP_SUBMIT", TRACE_NVME_NVDA_TCP_SUBMIT,
			OWNER_NVME_NVDA_TCP_QP, OBJECT_NVME_NVDA_TCP_REQ, 1,
			{	{ "ctx", SPDK_TRACE_ARG_TYPE_PTR, 8 },
				{ "cid", SPDK_TRACE_ARG_TYPE_INT, 4 },
				{ "opc", SPDK_TRACE_ARG_TYPE_INT, 4 },
				{ "dw10", SPDK_TRACE_ARG_TYPE_PTR, 4 },
				{ "dw11", SPDK_TRACE_ARG_TYPE_PTR, 4 },
				{ "dw12", SPDK_TRACE_ARG_TYPE_PTR, 4 }
			}
		},
		{
			"NVME_NVDA_TCP_COMPLETE", TRACE_NVME_NVDA_TCP_COMPLETE,
			OWNER_NVME_NVDA_TCP_QP, OBJECT_NVME_NVDA_TCP_REQ, 0,
			{	{ "ctx", SPDK_TRACE_ARG_TYPE_PTR, 8 },
				{ "cid", SPDK_TRACE_ARG_TYPE_INT, 4 },
				{ "cpl", SPDK_TRACE_ARG_TYPE_PTR, 4 }
			}
		},
	};

	spdk_trace_register_object(OBJECT_NVME_NVDA_TCP_REQ, 'p');
	spdk_trace_register_owner(OWNER_NVME_NVDA_TCP_QP, 'q');
	spdk_trace_register_description_ext(opts, SPDK_COUNTOF(opts));
}
