/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2020, 2021 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2022-2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/env.h"

#include <infiniband/verbs.h>

#include "spdk/log.h"
#include "spdk/sock.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/net.h"
#include "spdk/file.h"
#include "spdk/net.h"
#include "spdk/thread.h"

#include "spdk_internal/assert.h"
#include "spdk_internal/sock_module.h"
#include "spdk_internal/xlio.h"

#define MAX_TMPBUF 1024
#define PORTNUMLEN 32

struct spdk_xlio_stream_segment {
	struct xlio_buf				*xlio_buf;
	void					*buf;
	size_t					len;
	STAILQ_ENTRY(spdk_xlio_stream_segment)	link;
};

struct spdk_xlio_sock {
	struct spdk_sock		base;
	xlio_socket_t			xlio_sock;

	struct spdk_xlio_sock_group	*group;

	struct spdk_mem_map		*map;

	/* The current status code of this socket. In practice the following codes will
	 * appear:
	 * ENOTCONN - The socket was disconnected gracefully by the remote side.
	 * ENXIO - The socket is currently connecting.
	 * ECONNABORTED, ECONNRESET, ECONNREFUSED, ETIMEDOUT - The socket is not connected due to an error.
	 * 0 - The socket is connected.
	 */
	int				rc;

	struct {
		uint32_t		accept		: 1;
		uint32_t		rx		: 1;
		uint32_t		reserved	: 26;
	} events;

	STAILQ_ENTRY(spdk_xlio_sock)	link;

	STAILQ_HEAD(, spdk_xlio_stream_segment) pending_stream;
	uint32_t				stream_offset;

	char				interface_name[IFNAMSIZ];
};

struct spdk_xlio_sock_group {
	struct spdk_sock_group_impl	base;
	xlio_poll_group_t		xlio_group;

	STAILQ_HEAD(, spdk_xlio_sock)	pending_accept;
	STAILQ_HEAD(, spdk_xlio_sock)	pending_rx;

	STAILQ_HEAD(, spdk_xlio_stream_segment) segment_pool;
};

static struct spdk_sock_impl_opts g_xlio_impl_opts = {
	.recv_buf_size = DEFAULT_SO_RCVBUF_SIZE,
	.send_buf_size = DEFAULT_SO_SNDBUF_SIZE,
	.enable_recv_pipe = false,
	.enable_quickack = false,
	.enable_placement_id = PLACEMENT_NONE,
	.enable_zerocopy_send_server = true,
	.enable_zerocopy_send_client = true,
	.zerocopy_threshold = 0,
	.tls_version = 0,
	.enable_ktls = false,
	.psk_key = NULL,
	.psk_key_size = 0,
	.psk_identity = NULL,
	.get_key = NULL,
	.get_key_ctx = NULL,
	.tls_cipher_suites = NULL
};

/* TODO: Since this is a PoC, we'll only support one adapter and store this globally. */
static struct spdk_mem_map *g_mem_map = NULL;

static int
spdk_xlio_mem_notify(void *cb_ctx, struct spdk_mem_map *map,
		     enum spdk_mem_map_notify_action action,
		     void *vaddr, size_t size)
{
	struct ibv_pd *pd = cb_ctx;
	struct ibv_mr *mr;
	int rc;

	switch (action) {
	case SPDK_MEM_MAP_NOTIFY_REGISTER:
		mr = ibv_reg_mr(pd, vaddr, size, IBV_ACCESS_LOCAL_WRITE);
		if (mr == NULL) {
			SPDK_ERRLOG("ibv_reg_mr() failed\n");
			return -1;
		} else {
			rc = spdk_mem_map_set_translation(map, (uint64_t)vaddr, size, (uint64_t)mr);
		}
		break;
	case SPDK_MEM_MAP_NOTIFY_UNREGISTER:
		mr = (struct ibv_mr *)spdk_mem_map_translate(map, (uint64_t)vaddr, &size);
		if (mr != NULL) {
			ibv_dereg_mr(mr);
		}
		rc = spdk_mem_map_clear_translation(map, (uint64_t)vaddr, size);
		break;
	default:
		SPDK_UNREACHABLE();
	}

	return rc;
}

const struct spdk_mem_map_ops g_mem_map_ops = {
	.notify_cb = spdk_xlio_mem_notify,
	.are_contiguous = NULL
};

#define __xlio_sock(sock) (struct spdk_xlio_sock *)sock
#define __xlio_group(group) (struct spdk_xlio_sock_group *)group

static void
xlio_sock_copy_impl_opts(struct spdk_sock_impl_opts *dest, const struct spdk_sock_impl_opts *src,
			 size_t len)
{
#define FIELD_OK(field) \
	offsetof(struct spdk_sock_impl_opts, field) + sizeof(src->field) <= len

#define SET_FIELD(field) \
	if (FIELD_OK(field)) { \
		dest->field = src->field; \
	}

	SET_FIELD(recv_buf_size);
	SET_FIELD(send_buf_size);
	SET_FIELD(enable_recv_pipe);
	SET_FIELD(enable_zerocopy_send);
	SET_FIELD(enable_quickack);
	SET_FIELD(enable_placement_id);
	SET_FIELD(enable_zerocopy_send_server);
	SET_FIELD(enable_zerocopy_send_client);
	SET_FIELD(zerocopy_threshold);
	SET_FIELD(tls_version);
	SET_FIELD(enable_ktls);
	SET_FIELD(psk_key);
	SET_FIELD(psk_key_size);
	SET_FIELD(psk_identity);
	SET_FIELD(get_key);
	SET_FIELD(get_key_ctx);
	SET_FIELD(tls_cipher_suites);

#undef SET_FIELD
#undef FIELD_OK
}

static int
xlio_sock_impl_get_opts(struct spdk_sock_impl_opts *opts, size_t *len)
{
	if (!opts || !len) {
		errno = EINVAL;
		return -1;
	}

	assert(sizeof(*opts) >= *len);
	memset(opts, 0, *len);

	xlio_sock_copy_impl_opts(opts, &g_xlio_impl_opts, *len);
	*len = spdk_min(*len, sizeof(g_xlio_impl_opts));

	return 0;
}

static int
xlio_sock_impl_set_opts(const struct spdk_sock_impl_opts *opts, size_t len)
{
	if (!opts) {
		errno = EINVAL;
		return -1;
	}

	assert(sizeof(*opts) >= len);
	xlio_sock_copy_impl_opts(&g_xlio_impl_opts, opts, len);

	return 0;
}

static void
_opts_get_impl_opts(const struct spdk_sock_opts *opts, struct spdk_sock_impl_opts *dest,
		    const struct spdk_sock_impl_opts *default_impl)
{
	/* Copy the default impl_opts first to cover cases when user's impl_opts is smaller */
	memcpy(dest, default_impl, sizeof(*dest));

	if (opts->impl_opts != NULL) {
		assert(sizeof(*dest) >= opts->impl_opts_size);
		xlio_sock_copy_impl_opts(dest, opts->impl_opts, opts->impl_opts_size);
	}
}

static int
xlio_sock_getaddr(struct spdk_sock *_sock, char *saddr, int slen, uint16_t *sport,
		  char *caddr, int clen, uint16_t *cport)
{
	struct spdk_xlio_sock *sock = __xlio_sock(_sock);
	struct sockaddr_storage sa;
	socklen_t salen;
	int rc;

	assert(sock != NULL);

	memset(&sa, 0, sizeof sa);
	salen = sizeof sa;
	rc = xlio_socket_getsockname(sock->xlio_sock, (struct sockaddr *)&sa, &salen);
	if (rc != 0) {
		SPDK_ERRLOG("getsockname() failed (errno=%d)\n", errno);
		return -1;
	}

	switch (sa.ss_family) {
	case AF_UNIX:
		/* Acceptable connection types that don't have IPs */
		return 0;
	case AF_INET:
	case AF_INET6:
		/* Code below will get IP addresses */
		break;
	default:
		/* Unsupported socket family */
		return -1;
	}

	rc = spdk_net_get_address_string((struct sockaddr *)&sa, saddr, slen);
	if (rc != 0) {
		SPDK_ERRLOG("spdk_net_get_address_string() failed (errno=%d)\n", rc);
		return -1;
	}

	if (sport) {
		if (sa.ss_family == AF_INET) {
			*sport = ntohs(((struct sockaddr_in *) &sa)->sin_port);
		} else if (sa.ss_family == AF_INET6) {
			*sport = ntohs(((struct sockaddr_in6 *) &sa)->sin6_port);
		}
	}

	memset(&sa, 0, sizeof sa);
	salen = sizeof sa;
	rc = xlio_socket_getpeername(sock->xlio_sock, (struct sockaddr *)&sa, &salen);
	if (rc != 0) {
		SPDK_ERRLOG("getpeername() failed (errno=%d)\n", errno);
		return -1;
	}

	rc = spdk_net_get_address_string((struct sockaddr *)&sa, caddr, clen);
	if (rc != 0) {
		SPDK_ERRLOG("spdk_net_get_address_string() failed (errno=%d)\n", rc);
		return -1;
	}

	if (cport) {
		if (sa.ss_family == AF_INET) {
			*cport = ntohs(((struct sockaddr_in *) &sa)->sin_port);
		} else if (sa.ss_family == AF_INET6) {
			*cport = ntohs(((struct sockaddr_in6 *) &sa)->sin6_port);
		}
	}

	return 0;
}

static const char *
xlio_sock_get_interface_name(struct spdk_sock *_sock)
{
	/* TODO */
	return NULL;
}

static int32_t
xlio_sock_get_numa_id(struct spdk_sock *sock)
{
	/* TODO */
	return SPDK_ENV_NUMA_ID_ANY;
}

static int
xlio_sock_set_recvbuf(struct spdk_sock *_sock, int sz)
{
	struct spdk_xlio_sock *sock = __xlio_sock(_sock);
	int min_size;
	int rc;

	assert(sock != NULL);

	/* Set kernel buffer size to be at least MIN_SO_RCVBUF_SIZE and
	 * _sock->impl_opts.recv_buf_size. */
	min_size = spdk_max(MIN_SO_RCVBUF_SIZE, _sock->impl_opts.recv_buf_size);

	if (sz < min_size) {
		sz = min_size;
	}

	rc = xlio_socket_setsockopt(sock->xlio_sock, SOL_SOCKET, SO_RCVBUF, &sz, sizeof(sz));
	if (rc < 0) {
		return rc;
	}

	_sock->impl_opts.recv_buf_size = sz;

	return 0;
}

static int
xlio_sock_set_sendbuf(struct spdk_sock *_sock, int sz)
{
	struct spdk_xlio_sock *sock = __xlio_sock(_sock);
	int min_size;
	int rc;

	assert(sock != NULL);

	/* Set kernel buffer size to be at least MIN_SO_SNDBUF_SIZE and
	 * _sock->impl_opts.send_buf_size. */
	min_size = spdk_max(MIN_SO_SNDBUF_SIZE, _sock->impl_opts.send_buf_size);

	if (sz < min_size) {
		sz = min_size;
	}

	rc = xlio_socket_setsockopt(sock->xlio_sock, SOL_SOCKET, SO_SNDBUF, &sz, sizeof(sz));
	if (rc < 0) {
		return rc;
	}

	_sock->impl_opts.send_buf_size = sz;

	return 0;
}

static struct spdk_sock *
xlio_sock_listen(const char *ip, int port, struct spdk_sock_group_impl *_group,
		 struct spdk_sock_opts *opts)
{
	struct spdk_xlio_sock_group *group = __xlio_group(_group);
	struct spdk_xlio_sock *sock;
	struct xlio_socket_attr attr;
	struct spdk_sock_impl_opts impl_opts;
	char buf[MAX_TMPBUF];
	char portnum[PORTNUMLEN];
	char *p;
	struct addrinfo hints, *res, *res0;
	int rc;

	if (ip == NULL) {
		return NULL;
	}

	assert(opts != NULL);
	_opts_get_impl_opts(opts, &impl_opts, &g_xlio_impl_opts);

	sock = calloc(1, sizeof(*sock));
	if (sock == NULL) {
		SPDK_ERRLOG("sock allocation failed\n");
		return NULL;
	}

	sock->group = group;
	STAILQ_INIT(&sock->pending_stream);

	attr.flags = 0;
	attr.domain = AF_INET;
	attr.group = group->xlio_group;
	attr.userdata_sq = (uintptr_t)sock;

	rc = xlio_socket_create(&attr, &sock->xlio_sock);
	if (rc != 0) {
		free(sock);
		return NULL;
	}

	if (ip[0] == '[') {
		snprintf(buf, sizeof(buf), "%s", ip + 1);
		p = strchr(buf, ']');
		if (p != NULL) {
			*p = '\0';
		}
		ip = (const char *) &buf[0];
	}

	snprintf(portnum, sizeof portnum, "%d", port);
	memset(&hints, 0, sizeof hints);
	hints.ai_family = PF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_NUMERICSERV;
	hints.ai_flags |= AI_PASSIVE;
	hints.ai_flags |= AI_NUMERICHOST;
	rc = getaddrinfo(ip, portnum, &hints, &res0);
	if (rc != 0) {
		SPDK_ERRLOG("getaddrinfo() failed %s (%d)\n", gai_strerror(rc), rc);
		xlio_socket_destroy(sock->xlio_sock);
		free(sock);
		return NULL;
	}

	rc = -1;
	for (res = res0; res != NULL; res = res->ai_next) {
retry:
		rc = xlio_socket_bind(sock->xlio_sock, res->ai_addr, res->ai_addrlen);
		if (rc != 0) {
			SPDK_ERRLOG("xlio_socket_bind() failed at port %d, errno = %d\n", port, errno);
			switch (errno) {
			case EINTR:
				/* interrupted? */
				goto retry;
			case EADDRNOTAVAIL:
				SPDK_ERRLOG("IP address %s not available. "
					    "Verify IP address in config file "
					    "and make sure setup script is "
					    "run before starting spdk app.\n", ip);
			/* FALLTHROUGH */
			default:
				/* try next family */
				continue;
			}
		}

		rc = xlio_socket_listen(sock->xlio_sock);
		if (rc != 0) {
			SPDK_ERRLOG("xlio_socket_listen() failed, errno = %d\n", errno);
		}

		break;
	}
	freeaddrinfo(res0);

	if (rc < 0) {
		xlio_socket_destroy(sock->xlio_sock);
		free(sock);
		return NULL;
	}

	return &sock->base;
}

static struct spdk_sock *
xlio_sock_connect(const char *ip, int port, struct spdk_sock_group_impl *_group,
		  struct spdk_sock_opts *opts)
{
	struct spdk_xlio_sock_group *group = __xlio_group(_group);
	struct spdk_xlio_sock *sock;
	struct xlio_socket_attr attr;
	struct spdk_sock_impl_opts impl_opts;
	char buf[MAX_TMPBUF];
	char portnum[PORTNUMLEN];
	char *p;
	const char *src_addr;
	uint16_t src_port;
	struct addrinfo hints, *res, *res0, *src_ai;
	int rc;

	if (ip == NULL) {
		return NULL;
	}

	assert(opts != NULL);
	_opts_get_impl_opts(opts, &impl_opts, &g_xlio_impl_opts);

	sock = calloc(1, sizeof(*sock));
	if (sock == NULL) {
		return NULL;
	}

	sock->group = group;
	STAILQ_INIT(&sock->pending_stream);

	attr.flags = 0;
	attr.domain = AF_INET;
	attr.group = group->xlio_group;
	attr.userdata_sq = (uintptr_t)sock;

	rc = xlio_socket_create(&attr, &sock->xlio_sock);
	if (rc != 0) {
		free(sock);
		return NULL;
	}

	src_addr = SPDK_GET_FIELD(opts, src_addr, NULL, opts->opts_size);
	src_port = SPDK_GET_FIELD(opts, src_port, 0, opts->opts_size);
	if (src_addr != NULL || src_port != 0) {
		snprintf(portnum, sizeof(portnum), "%"PRIu16, src_port);
		memset(&hints, 0, sizeof hints);
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_flags = AI_NUMERICSERV | AI_NUMERICHOST | AI_PASSIVE;
		rc = getaddrinfo(src_addr, src_port > 0 ? portnum : NULL,
				 &hints, &src_ai);
		if (rc != 0 || src_ai == NULL) {
			SPDK_ERRLOG("getaddrinfo() failed %s (%d)\n",
				    rc != 0 ? gai_strerror(rc) : "", rc);
			xlio_socket_destroy(sock->xlio_sock);
			free(sock);
			return NULL;
		}

		rc = xlio_socket_bind(sock->xlio_sock, src_ai->ai_addr, src_ai->ai_addrlen);
		if (rc != 0) {
			SPDK_ERRLOG("xlio_socket_bind() failed errno %d (%s:%s)\n", errno,
				    src_addr ? src_addr : "", portnum);
			freeaddrinfo(src_ai);
			xlio_socket_destroy(sock->xlio_sock);
			free(sock);
			return NULL;
		}

		freeaddrinfo(src_ai);
		src_ai = NULL;
	}

	if (ip[0] == '[') {
		snprintf(buf, sizeof(buf), "%s", ip + 1);
		p = strchr(buf, ']');
		if (p != NULL) {
			*p = '\0';
		}
		ip = (const char *) &buf[0];
	}

	snprintf(portnum, sizeof portnum, "%d", port);
	memset(&hints, 0, sizeof hints);
	hints.ai_family = PF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_NUMERICSERV;
	hints.ai_flags |= AI_PASSIVE;
	hints.ai_flags |= AI_NUMERICHOST;
	rc = getaddrinfo(ip, portnum, &hints, &res0);
	if (rc != 0) {
		SPDK_ERRLOG("getaddrinfo() failed %s (%d)\n", gai_strerror(rc), rc);
		xlio_socket_destroy(sock->xlio_sock);
		free(sock);
		return NULL;
	}

	rc = -1;
	for (res = res0; res != NULL; res = res->ai_next) {
		sock->rc = ENXIO;
		rc = xlio_socket_connect(sock->xlio_sock, res->ai_addr, res->ai_addrlen);
		if (rc != 0) {
			SPDK_ERRLOG("xlio_socket_connect() failed, errno = %d\n", errno);
			/* try next family */
			continue;
		}

		/* In xlio, connect is asynchronous. We need to sit here and spin until
		 * it is done. */
		while (sock->rc == ENXIO) {
			xlio_poll_group_poll(group->xlio_group);
		}

		break;
	}
	freeaddrinfo(res0);

	if (rc < 0) {
		xlio_socket_destroy(sock->xlio_sock);
		free(sock);
		return NULL;
	}

	return &sock->base;
}

static struct spdk_sock *
xlio_sock_accept(struct spdk_sock *_sock)
{
	struct spdk_xlio_sock_group *group;
	struct spdk_xlio_sock *listen_sock = __xlio_sock(_sock);
	struct spdk_xlio_sock *sock;

	assert(listen_sock != NULL);

	group = listen_sock->group;
	assert(group != NULL);

	sock = STAILQ_FIRST(&group->pending_accept);
	if (sock == NULL) {
		if (listen_sock->events.rx) {
			STAILQ_REMOVE(&group->pending_rx, listen_sock, spdk_xlio_sock, link);
			listen_sock->events.rx = false;
		}
		return NULL;
	}

	STAILQ_REMOVE_HEAD(&group->pending_accept, link);
	sock->events.accept = false;

	if (STAILQ_EMPTY(&group->pending_accept)) {
		if (listen_sock->events.rx) {
			STAILQ_REMOVE(&group->pending_rx, listen_sock, spdk_xlio_sock, link);
			listen_sock->events.rx = false;
		}
	}

	/* The user is accepting this socket. While we were waiting for
	 * them to do that, we may have already had an rx event. If we did,
	 * put this socket on the correct list now. */
	if (sock->events.rx) {
		STAILQ_INSERT_TAIL(&group->pending_rx, sock, link);
	}

	return &sock->base;
}

static int
xlio_sock_close(struct spdk_sock_group_impl *_group, struct spdk_sock *_sock)
{
	struct spdk_xlio_sock *sock = __xlio_sock(_sock);
	struct spdk_xlio_sock_group *group = __xlio_group(_group);
	struct spdk_xlio_stream_segment *segment, *tsegment;
	int rc;

	if (sock->rc == 0) {
		xlio_socket_flush(sock->xlio_sock);
	}

	/* Dump all data that is waiting to be received. This is safe. */
	STAILQ_FOREACH_SAFE(segment, &sock->pending_stream, link, tsegment) {
		STAILQ_REMOVE_HEAD(&sock->pending_stream, link);
		xlio_poll_group_buf_free(group->xlio_group, segment->xlio_buf);
		STAILQ_INSERT_HEAD(&group->segment_pool, segment, link);
	}

	/* This is actually asynchronous. The remainder of the process will occur
	 * in the event callback. */
	rc = xlio_socket_destroy(sock->xlio_sock);

	return rc;
}

static int
xlio_sock_flush(struct spdk_sock *_sock)
{
	struct spdk_xlio_sock *sock = __xlio_sock(_sock);

	if (sock->rc != 0) {
		errno = sock->rc;
		return -1;
	}

	xlio_socket_flush(sock->xlio_sock);

	return 0;
}

static ssize_t
xlio_sock_readv(struct spdk_sock *_sock, struct iovec *iov, int iovcnt)
{
	struct spdk_xlio_sock *sock = __xlio_sock(_sock);
	struct spdk_xlio_sock_group *group;
	struct spdk_xlio_stream_segment *segment;
	struct spdk_iov_xfer ix;
	size_t len;
	ssize_t total;

	if (sock->rc != 0) {
		errno = sock->rc;
		return -1;
	}

	if (!sock->events.rx) {
		errno = EAGAIN;
		return -1;
	}

	segment = STAILQ_FIRST(&sock->pending_stream);
	if (segment == NULL) {
		errno = EAGAIN;
		return -1;
	}

	group = sock->group;

	spdk_iov_xfer_init(&ix, iov, iovcnt);

	total = 0;
	while (segment != NULL) {
		len = spdk_iov_xfer_from_buf(&ix, segment->buf + sock->stream_offset,
					     segment->len - sock->stream_offset);
		sock->stream_offset += len;
		total += len;

		if (sock->stream_offset < segment->len) {
			break;
		}

		/* We've read to the end of this segment, so we can release it. */
		sock->stream_offset = 0;
		STAILQ_REMOVE_HEAD(&sock->pending_stream, link);

		xlio_poll_group_buf_free(group->xlio_group, segment->xlio_buf);
		STAILQ_INSERT_HEAD(&group->segment_pool, segment, link);

		segment = STAILQ_FIRST(&sock->pending_stream);
	}

	if (STAILQ_EMPTY(&sock->pending_stream)) {
		STAILQ_REMOVE(&group->pending_rx, sock, spdk_xlio_sock, link);
		sock->events.rx = false;
	}

	return total;
}

static ssize_t
xlio_sock_recv(struct spdk_sock *_sock, void *buf, size_t len)
{
	struct iovec iov = {
		.iov_base = buf,
		.iov_len = len
	};

	return xlio_sock_readv(_sock, &iov, 1);
}

static ssize_t
xlio_sock_writev(struct spdk_sock *_sock, struct iovec *iov, int iovcnt)
{
	struct spdk_xlio_sock *sock = __xlio_sock(_sock);
	int rc, i;
	ssize_t total;
	struct xlio_socket_send_attr attr = {
		.flags = XLIO_SOCKET_SEND_FLAG_INLINE, /* We don't own this memory, so xlio needs to copy it internally */
		.mkey = 0, /* xlio will presumably handle this */
		.userdata_op = 0 /* we will not get a completion notification for this operation */
	};

	if (sock->rc != 0) {
		errno = sock->rc;
		return -1;
	}

	rc = xlio_socket_sendv(sock->xlio_sock, iov, iovcnt, &attr);
	if (rc < 0) {
		errno = -rc;
		return -1;
	}

	/* Unfortunately, the API expects to return the total amount of data being sent because
	 * other APIs do partial sends. There's no other way to calculate it other than loop.
	 */
	total = 0;
	for (i = 0; i < iovcnt; i++) {
		total += iov[i].iov_len;
	}

	return total;
}

static int
xlio_sock_recv_next(struct spdk_sock *_sock, void **buf, struct spdk_sock_buf_token **token)
{
	struct spdk_xlio_sock *sock = __xlio_sock(_sock);
	struct spdk_xlio_sock_group *group;
	struct spdk_xlio_stream_segment *segment;

	if (sock->rc != 0) {
		errno = sock->rc;
		return -1;
	}

	if (!sock->events.rx) {
		errno = EAGAIN;
		return -1;
	}

	segment = STAILQ_FIRST(&sock->pending_stream);
	if (segment == NULL) {
		errno = EAGAIN;
		return -1;
	}

	group = sock->group;

	/* We are going to give this segment over to the user, but some of it may have already
	 * been read from using the copying APIs (readv, recv). We need to adjust the pointer
	 * and the length. */
	segment->buf += sock->stream_offset;
	segment->len -= sock->stream_offset;

	*buf = segment->buf;
	*token = (struct spdk_sock_buf_token *)segment;

	sock->stream_offset = 0;
	STAILQ_REMOVE_HEAD(&sock->pending_stream, link);

	if (STAILQ_EMPTY(&sock->pending_stream)) {
		STAILQ_REMOVE(&group->pending_rx, sock, spdk_xlio_sock, link);
		sock->events.rx = false;
	}

	return segment->len;
}

static void
xlio_sock_writev_async(struct spdk_sock *_sock, struct spdk_sock_request *req)
{
	struct spdk_xlio_sock *sock = __xlio_sock(_sock);
	int rc, i;
	struct xlio_socket_send_attr attr = {
		.flags = 0, /* The default is zero copy */
		.mkey = 0, /* This gets filled in based on the lookup below */
		.userdata_op = 0
	};
	struct ibv_mr *mr, *mr_tmp;
	struct iovec *iov;

	if (sock->rc != 0) {
		spdk_sock_request_complete(&sock->base, req, sock->rc);
		return;
	}

	mr = NULL;
	mr_tmp = NULL;
	for (i = 0; i < req->iovcnt; i++) {
		uint64_t size;

		iov = SPDK_SOCK_REQUEST_IOV(req, i);
		size = (uint64_t)iov->iov_len;

		mr_tmp = (struct ibv_mr *)spdk_mem_map_translate(sock->map, (uint64_t)iov->iov_base, &size);
		assert(mr_tmp != NULL);

		if (mr == NULL) {
			mr = mr_tmp;
		} else if (mr->lkey != mr_tmp->lkey) {
			mr = NULL;
			break;
		}
	}

	if (mr != NULL) {
		attr.mkey = mr->lkey;
		attr.userdata_op = (uintptr_t)req; /* We'll get a completion notification when this finishes */
		iov = SPDK_SOCK_REQUEST_IOV(req, 0);
		rc = xlio_socket_sendv(sock->xlio_sock, iov, req->iovcnt, &attr);
		if (rc < 0) {
			spdk_sock_request_complete(&sock->base, req, rc);
			return;
		}

		return;
	}

	/* Split memory keys so we need to do a separate send for each iov. */
	for (i = 0; i < req->iovcnt; i++) {
		uint64_t size;

		iov = SPDK_SOCK_REQUEST_IOV(req, i);
		size = (uint64_t)iov->iov_len;

		mr = (struct ibv_mr *)spdk_mem_map_translate(sock->map, (uint64_t)iov->iov_base, &size);
		assert(mr != NULL);
		assert(size == iov->iov_len);

		attr.mkey = mr->lkey;

		if (i == req->iovcnt - 1) {
			attr.userdata_op = (uintptr_t)req; /* We'll get a completion notification when this finishes */
		}

		rc = xlio_socket_send(sock->xlio_sock, iov->iov_base, iov->iov_len, &attr);
		if (rc < 0) {
			spdk_sock_request_complete(&sock->base, req, rc);
			return;
		}
	}
}

static int
xlio_sock_set_recvlowat(struct spdk_sock *_sock, int nbytes)
{
	struct spdk_xlio_sock *sock = __xlio_sock(_sock);
	int val;
	int rc;

	if (sock->rc != 0) {
		errno = sock->rc;
		return -1;
	}

	val = nbytes;
	rc = xlio_socket_setsockopt(sock->xlio_sock, SOL_SOCKET, SO_RCVLOWAT, &val, sizeof val);
	if (rc != 0) {
		return -1;
	}

	return 0;
}

static bool
xlio_sock_is_ipv6(struct spdk_sock *_sock)
{
	struct spdk_xlio_sock *sock = __xlio_sock(_sock);

	if (sock->rc != 0) {
		errno = sock->rc;
		return -1;
	}

	/* TODO */
	return false;
}

static bool
xlio_sock_is_ipv4(struct spdk_sock *_sock)
{
	struct spdk_xlio_sock *sock = __xlio_sock(_sock);

	if (sock->rc != 0) {
		errno = sock->rc;
		return -1;
	}

	/* TODO */
	return true;
}

static bool
xlio_sock_is_connected(struct spdk_sock *_sock)
{
	struct spdk_xlio_sock *sock = __xlio_sock(_sock);

	return sock->rc == 0;
}

static struct spdk_sock_group_impl *
xlio_sock_group_get_optimal(struct spdk_sock *sock, struct spdk_sock_group_impl *hint)
{
	return NULL;
}

static void
spdk_xlio_socket_event_cb(xlio_socket_t xlio_sock, uintptr_t userdata_sq, int event, int value)
{
	struct spdk_xlio_sock *sock = (struct spdk_xlio_sock *)userdata_sq;

	switch (event) {
	case XLIO_SOCKET_EVENT_ESTABLISHED:
		sock->rc = 0;
		break;
	case XLIO_SOCKET_EVENT_TERMINATED:
		/* This is the last event we'll get. */
		free(sock);
		break;
	case XLIO_SOCKET_EVENT_CLOSED:
		/* The remote side closed the connection. Set an error here
		 * and wait for the user to close the socket for clean up. */
		sock->rc = ENOTCONN;
		break;
	case XLIO_SOCKET_EVENT_ERROR:
		/* There was an error. Set the error here and wait for the
		 * user to close the socket for clean up. */
		sock->rc = value;
		break;

	}
}

static void
spdk_xlio_socket_comp_cb(xlio_socket_t xlio_sock, uintptr_t userdata_sq, uintptr_t userdata_op)
{
	struct spdk_xlio_sock *sock = (struct spdk_xlio_sock *)userdata_sq;
	struct spdk_sock_request *req = (struct spdk_sock_request *)userdata_op;

	spdk_sock_request_complete(&sock->base, req, 0);
}

static void
spdk_xlio_socket_rx_cb(xlio_socket_t xlio_sock, uintptr_t userdata_sq, void *data, size_t len,
		       struct xlio_buf *buf)
{
	struct spdk_xlio_sock *sock = (struct spdk_xlio_sock *)userdata_sq;
	struct spdk_xlio_sock_group *group;
	struct spdk_xlio_stream_segment *segment;

	group = sock->group;
	assert(group != NULL);

	segment = STAILQ_FIRST(&group->segment_pool);
	if (segment == NULL) {
		/* TODO: I guess just allocate more. The only other option is to disconnect. */
		segment = calloc(1, sizeof(*segment));
		if (segment == NULL) {
			xlio_socket_destroy(xlio_sock);
			return;
		}
	} else {
		STAILQ_REMOVE_HEAD(&group->segment_pool, link);
	}

	segment->buf = data;
	segment->len = len;
	segment->xlio_buf = buf;
	STAILQ_INSERT_TAIL(&sock->pending_stream, segment, link);

	if (sock->events.rx) {
		/* This socket is already in the pending rx list, so do nothing. */
		return;
	}

	sock->events.rx = true;

	if (sock->events.accept) {
		/* This socket is already in the pending accept list, so do nothing. */
		return;
	}

	STAILQ_INSERT_TAIL(&group->pending_rx, sock, link);
}

static void
spdk_xlio_socket_accept_cb(xlio_socket_t xlio_sock, xlio_socket_t parent,
			   uintptr_t parent_userdata_sq)
{
	struct spdk_xlio_sock *listen_sock = (struct spdk_xlio_sock *)parent_userdata_sq;
	struct spdk_xlio_sock_group *group;
	struct spdk_xlio_sock *sock;
	struct ibv_pd *pd;
	int rc;

	group = listen_sock->group;

	sock = calloc(1, sizeof(*sock));
	if (sock == NULL) {
		xlio_socket_destroy(xlio_sock);
		return;
	}

	sock->xlio_sock = xlio_sock;
	sock->group = group;
	STAILQ_INIT(&sock->pending_stream);

	rc = xlio_socket_update(xlio_sock, 0, (uintptr_t)sock);
	if (rc != 0) {
		xlio_socket_destroy(xlio_sock);
		free(sock);
		return;
	}

	if (g_mem_map == NULL) {
		/* This is the first accepted socket. We can finally discover the protection domain. */
		pd = xlio_socket_get_pd(xlio_sock);
		assert(pd != NULL);

		g_mem_map = spdk_mem_map_alloc(0, &g_mem_map_ops, pd);
		assert(g_mem_map != NULL);
	}

	sock->map = g_mem_map;

	sock->events.accept = true;
	STAILQ_INSERT_TAIL(&group->pending_accept, sock, link);

	/* We put the listen socket into the pending rx queue as well */
	if (!listen_sock->events.rx) {
		STAILQ_INSERT_TAIL(&group->pending_rx, listen_sock, link);
		listen_sock->events.rx = true;
	}
}

static struct spdk_sock_group_impl *
xlio_sock_group_create(void)
{
	int rc;
	struct spdk_xlio_sock_group *group;
	struct xlio_poll_group_attr attr = {
		.flags = XLIO_GROUP_FLAG_DIRTY,
		.socket_event_cb = spdk_xlio_socket_event_cb,
		.socket_comp_cb = spdk_xlio_socket_comp_cb,
		.socket_rx_cb = spdk_xlio_socket_rx_cb,
		.socket_accept_cb = spdk_xlio_socket_accept_cb,
	};

	group = calloc(1, sizeof(*group));
	if (group == NULL) {
		return NULL;
	}

	rc = xlio_poll_group_create(&attr, &group->xlio_group);
	if (rc != 0) {
		free(group);
		return NULL;
	}

	STAILQ_INIT(&group->pending_accept);
	STAILQ_INIT(&group->pending_rx);

	STAILQ_INIT(&group->segment_pool);

	/* TODO: How many of these do we allocate? Should it be configurable? */
	for (int i = 0; i < 256; i++) {
		struct spdk_xlio_stream_segment *segment;

		segment = calloc(1, sizeof(*segment));
		if (segment == NULL) {
			break;
		}

		STAILQ_INSERT_TAIL(&group->segment_pool, segment, link);
	}

	return &group->base;
}

static int
xlio_sock_group_add_sock(struct spdk_sock_group_impl *_group, struct spdk_sock *_sock)
{
	struct spdk_xlio_sock_group *group = __xlio_group(_group);
	struct spdk_xlio_sock *sock = __xlio_sock(_sock);

	if (sock->events.rx) {
		STAILQ_INSERT_TAIL(&group->pending_rx, sock, link);
	}

	if (sock->events.accept) {
		STAILQ_INSERT_TAIL(&group->pending_accept, sock, link);
	}

	sock->group = group;

	return xlio_socket_attach_group(sock->xlio_sock, group->xlio_group);
}

static int
xlio_sock_group_remove_sock(struct spdk_sock_group_impl *_group, struct spdk_sock *_sock)
{
	struct spdk_xlio_sock_group *group = __xlio_group(_group);
	struct spdk_xlio_sock *sock = __xlio_sock(_sock);
	int rc;

	if (sock->events.rx) {
		STAILQ_REMOVE(&group->pending_rx, sock, spdk_xlio_sock, link);
	}

	if (sock->events.accept) {
		STAILQ_REMOVE(&group->pending_accept, sock, spdk_xlio_sock, link);
	}

	sock->group = NULL;

	if (sock->rc != 0) {
		/* sockets with errors are already removed */
		return 0;
	}

	rc = xlio_socket_detach_group(sock->xlio_sock);
	if (rc < 0) {
		return rc;
	}

	return rc;
}

static int
xlio_sock_group_poll(struct spdk_sock_group_impl *_group, int max_events, struct spdk_sock **socks)
{
	struct spdk_xlio_sock_group *group = __xlio_group(_group);
	struct spdk_xlio_sock *sock;
	int count;

	xlio_poll_group_flush(group->xlio_group);

	xlio_poll_group_poll(group->xlio_group);

	count = 0;
	STAILQ_FOREACH(sock, &group->pending_rx, link) {
		if (count >= max_events) {
			break;
		}

		socks[count++] = &sock->base;
	}

	/* TODO: We should shuffle the pending lists for fairness */

	return count;
}

static int
xlio_sock_group_impl_get_interruptfd(struct spdk_sock_group_impl *_group)
{
	return -ENOTSUP;
}

static int
xlio_sock_group_release_buf(struct spdk_sock_group_impl *_group, void *buf,
			    struct spdk_sock_buf_token *token)
{
	struct spdk_xlio_sock_group *group = __xlio_group(_group);
	struct spdk_xlio_stream_segment *segment;

	segment = (struct spdk_xlio_stream_segment *)token;
	if (segment == NULL) {
		return -EINVAL;
	}

	xlio_poll_group_buf_free(group->xlio_group, segment->xlio_buf);
	STAILQ_INSERT_HEAD(&group->segment_pool, segment, link);

	return 0;
}

static int
xlio_sock_group_close(struct spdk_sock_group_impl *_group)
{
	struct spdk_xlio_sock_group *group = __xlio_group(_group);
	struct spdk_xlio_sock *sock, *tmp;

	if (!STAILQ_EMPTY(&group->pending_rx)) {
		return -EBUSY;
	}

	STAILQ_FOREACH_SAFE(sock, &group->pending_accept, link, tmp) {
		STAILQ_REMOVE_HEAD(&group->pending_accept, link);
		xlio_socket_destroy(sock->xlio_sock);
		free(sock);
	}

	return xlio_poll_group_destroy(group->xlio_group);
}

static void *
spdk_xlio_alloc(size_t size)
{
	return spdk_zmalloc(size, 0, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
}

static void
spdk_xlio_free(void *buf)
{
	spdk_free(buf);
}

static int
xlio_sock_initialize(void)
{
	int rc;
	struct xlio_init_attr iattr = {
		.flags = 0,
		.memory_alloc = &spdk_xlio_alloc,
		.memory_free = &spdk_xlio_free,
	};

	rc = xlio_init_ex(&iattr);
	if (rc) {
		SPDK_ERRLOG("xlio_init rc %d (errno=%d)\n", rc, errno);
	}

	return rc;
}

static struct spdk_net_impl g_xlio_net_impl = {
	.name		= "xlio",
	.init		= xlio_sock_initialize,
	.getaddr	= xlio_sock_getaddr,
	.get_interface_name = xlio_sock_get_interface_name,
	.get_numa_id	= xlio_sock_get_numa_id,
	.connect	= xlio_sock_connect,
	.listen		= xlio_sock_listen,
	.accept		= xlio_sock_accept,
	.close		= xlio_sock_close,
	.recv		= xlio_sock_recv,
	.readv		= xlio_sock_readv,
	.writev		= xlio_sock_writev,
	.recv_next	= xlio_sock_recv_next,
	.writev_async	= xlio_sock_writev_async,
	.flush		= xlio_sock_flush,
	.set_recvlowat	= xlio_sock_set_recvlowat,
	.set_recvbuf	= xlio_sock_set_recvbuf,
	.set_sendbuf	= xlio_sock_set_sendbuf,
	.is_ipv6	= xlio_sock_is_ipv6,
	.is_ipv4	= xlio_sock_is_ipv4,
	.is_connected	= xlio_sock_is_connected,
	.group_impl_get_optimal	= xlio_sock_group_get_optimal,
	.group_impl_create	= xlio_sock_group_create,
	.group_impl_add_sock	= xlio_sock_group_add_sock,
	.group_impl_remove_sock = xlio_sock_group_remove_sock,
	.group_impl_poll	= xlio_sock_group_poll,
	.group_impl_get_interruptfd    = xlio_sock_group_impl_get_interruptfd,
	.group_impl_close	= xlio_sock_group_close,
	.group_impl_release_buf = xlio_sock_group_release_buf,
	.get_opts	= xlio_sock_impl_get_opts,
	.set_opts	= xlio_sock_impl_set_opts,
};

SPDK_NET_IMPL_REGISTER_DEFAULT(xlio, &g_xlio_net_impl);
SPDK_LOG_REGISTER_COMPONENT(sock_xlio)
