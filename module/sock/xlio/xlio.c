/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2020, 2021 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2022-2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/env.h"

#include <sys/epoll.h>
#include <linux/errqueue.h>
#include <infiniband/verbs.h>

#include "spdk/log.h"
#include "spdk/pipe.h"
#include "spdk/sock.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/net.h"
#include "spdk_internal/sock.h"
#include "spdk_internal/event.h"
#include "spdk_internal/xlio.h"

#define MAX_TMPBUF 1024
#define PORTNUMLEN 32
#define XLIO_PACKETS_BUF_SIZE 128

#if defined(SO_ZEROCOPY) && defined(MSG_ZEROCOPY)
#define SPDK_ZEROCOPY
#endif

#ifndef SPDK_ZEROCOPY
#error "XLIO requires zcopy"
#endif

struct xlio_sock_packet {
	struct xlio_socketxtreme_packet_desc_t xlio_packet;
	int refs;
	STAILQ_ENTRY(xlio_sock_packet) link;
};

struct xlio_sock_buf {
	struct spdk_sock_buf sock_buf;
	struct xlio_sock_packet *packet;
};

struct spdk_xlio_ring_fd {
	int			ring_fd;
	int			refs;
	TAILQ_ENTRY(spdk_xlio_ring_fd)	link;
};

struct spdk_xlio_sock {
	struct spdk_sock	base;
	int			fd;
	uint32_t		sendmsg_idx;
	struct ibv_pd *pd;
	struct {
		uint8_t		pending_recv	: 1;
		uint8_t		pending_send	: 1;
		uint8_t		zcopy		: 1;
		uint8_t		recv_zcopy	: 1;
		uint8_t		disconnected	: 1;
		uint8_t		reserved	: 3;
	} flags;
	int			so_priority;

	struct xlio_packets_pool	*xlio_packets_pool;
	STAILQ_HEAD(, xlio_sock_packet)	received_packets;
	struct xlio_buff_t	*cur_xlio_buf;
	size_t			cur_offset;
	uint64_t		batch_start_tsc;
	int			batch_nr;
	struct spdk_xlio_ring_fd	*ring_fd;

	TAILQ_ENTRY(spdk_xlio_sock)	link;
	TAILQ_ENTRY(spdk_xlio_sock)	link_send;
};

struct spdk_xlio_sock_group_impl {
	struct spdk_sock_group_impl	base;
	TAILQ_HEAD(, spdk_xlio_ring_fd)	ring_fd;
	TAILQ_HEAD(, spdk_xlio_sock)	pending_recv;
	TAILQ_HEAD(, spdk_xlio_sock)	pending_send;
	struct xlio_packets_pool	*xlio_packets_pool;
};

static struct spdk_sock_impl_opts g_spdk_xlio_sock_impl_opts = {
	.recv_buf_size = DEFAULT_SO_RCVBUF_SIZE,
	.send_buf_size = DEFAULT_SO_SNDBUF_SIZE,
	.enable_recv_pipe = false,
	.enable_zerocopy_send = true,
	.enable_quickack = false,
	.enable_placement_id = false,
	.enable_zerocopy_send_server = true,
	.enable_zerocopy_send_client = true,
	.enable_zerocopy_recv = true,
	.zerocopy_threshold = 4096,
	.enable_tcp_nodelay = false,
	.buffers_pool_size = 4096,
	.packets_pool_size = 1024,
	.enable_early_init = true
};

/* xlio packets pool for each core */
struct xlio_packets_pool {
	STAILQ_HEAD(, xlio_sock_packet)	free_packets;
	struct xlio_sock_packet	*packets;
	uint32_t		num_free_packets;
	uint32_t		core_id;
	STAILQ_ENTRY(xlio_packets_pool)	link;
};

static pthread_mutex_t g_xlio_pool_mutex = PTHREAD_MUTEX_INITIALIZER;
static STAILQ_HEAD(, xlio_packets_pool) g_xlio_packets_pools = STAILQ_HEAD_INITIALIZER(
			g_xlio_packets_pools);
static struct spdk_mempool *g_xlio_buffers_pool;

static int _sock_flush_ext(struct spdk_sock *sock);
static int xlio_sock_poll_fd(int fd, uint32_t max_events_per_poll);

static void
xlio_sock_free_pools(void)
{
	struct xlio_packets_pool *pool, *tmp;
	STAILQ_FOREACH_SAFE(pool, &g_xlio_packets_pools, link, tmp) {
		STAILQ_REMOVE_HEAD(&g_xlio_packets_pools, link);
		free(pool->packets);
		free(pool);
	}

	spdk_mempool_free(g_xlio_buffers_pool);
}

#define __xlio_sock(sock) (struct spdk_xlio_sock *)sock
#define __xlio_group_impl(group) (struct spdk_xlio_sock_group_impl *)group

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
	rc = xlio_getsockname(sock->fd, (struct sockaddr *) &sa, &salen);
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
		SPDK_ERRLOG("getnameinfo() failed (errno=%d)\n", errno);
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
	rc = xlio_getpeername(sock->fd, (struct sockaddr *) &sa, &salen);
	if (rc != 0) {
		SPDK_ERRLOG("getpeername() failed (errno=%d)\n", errno);
		return -1;
	}

	rc = spdk_net_get_address_string((struct sockaddr *)&sa, caddr, clen);
	if (rc != 0) {
		SPDK_ERRLOG("getnameinfo() failed (errno=%d)\n", errno);
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

enum xlio_sock_create_type {
	SPDK_SOCK_CREATE_LISTEN,
	SPDK_SOCK_CREATE_CONNECT,
};

static int
xlio_sock_set_recvbuf(struct spdk_sock *_sock, int sz)
{
	struct spdk_xlio_sock *sock = __xlio_sock(_sock);
	int min_size;
	int rc;

	assert(sock != NULL);

	/* Set kernel buffer size to be at least MIN_SO_RCVBUF_SIZE and
	 * impl_opts.recv_buf_size. */
	min_size = spdk_max(MIN_SO_RCVBUF_SIZE, g_spdk_xlio_sock_impl_opts.recv_buf_size);

	if (sz < min_size) {
		sz = min_size;
	}

	rc = xlio_setsockopt(sock->fd, SOL_SOCKET, SO_RCVBUF, &sz, sizeof(sz));
	if (rc < 0) {
		return rc;
	}

	return 0;
}

static int
xlio_sock_set_sendbuf(struct spdk_sock *_sock, int sz)
{
	struct spdk_xlio_sock *sock = __xlio_sock(_sock);
	int rc;

	assert(sock != NULL);

	if (sz < MIN_SO_SNDBUF_SIZE) {
		sz = MIN_SO_SNDBUF_SIZE;
	}

	rc = xlio_setsockopt(sock->fd, SOL_SOCKET, SO_SNDBUF, &sz, sizeof(sz));
	if (rc < 0) {
		return rc;
	}

	return 0;
}

static inline struct ibv_pd *
xlio_get_pd(int fd)
{
	struct xlio_pd_attr pd_attr_ptr = {};
	socklen_t len = sizeof(pd_attr_ptr);

	int err = xlio_getsockopt(fd, SOL_SOCKET, SO_XLIO_PD, &pd_attr_ptr, &len);
	if (err < 0) {
		return NULL;
	}
	return pd_attr_ptr.ib_pd;
}

static int
xlio_sock_alloc_buffers_pool(uint32_t buffers_pool_size)
{
	pthread_mutex_lock(&g_xlio_pool_mutex);
	if (g_xlio_buffers_pool) {
		pthread_mutex_unlock(&g_xlio_pool_mutex);
		return 0;
	}

	g_xlio_buffers_pool = spdk_mempool_create("xlio_buffers_pool",
			      buffers_pool_size,
			      sizeof(struct xlio_sock_buf),
			      SPDK_MEMPOOL_DEFAULT_CACHE_SIZE,
			      SPDK_ENV_SOCKET_ID_ANY);
	if (!g_xlio_buffers_pool) {
		SPDK_ERRLOG("Failed to create xlio buffers pool\n");
		pthread_mutex_unlock(&g_xlio_pool_mutex);
		return -ENOMEM;
	}

	pthread_mutex_unlock(&g_xlio_pool_mutex);
	SPDK_NOTICELOG("Create xlio buffers pool, buffers_pool_size %u\n", buffers_pool_size);

	return 0;
}

static struct xlio_packets_pool *
xlio_sock_get_packets_pool(uint32_t packets_pool_size)
{
	struct xlio_packets_pool *pool;
	uint32_t i, current_core = spdk_env_get_current_core();

	pthread_mutex_lock(&g_xlio_pool_mutex);
	STAILQ_FOREACH(pool, &g_xlio_packets_pools, link) {
		if (pool->core_id == current_core) {
			pthread_mutex_unlock(&g_xlio_pool_mutex);
			return pool;
		}
	}

	pool = calloc(1, sizeof(*pool));
	if (!pool) {
		SPDK_ERRLOG("Failed to allocate pool\n");
		goto fail;
	}

	pool->packets = calloc(packets_pool_size,
			       sizeof(struct xlio_sock_packet));
	if (!pool->packets) {
		SPDK_ERRLOG("Failed to allocate packets\n");
		free(pool);
		goto fail;
	}

	STAILQ_INIT(&pool->free_packets);
	for (i = 0; i < packets_pool_size; ++i) {
		STAILQ_INSERT_TAIL(&pool->free_packets, &pool->packets[i], link);
	}

	STAILQ_INSERT_HEAD(&g_xlio_packets_pools, pool, link);
	pool->num_free_packets = packets_pool_size;
	pool->core_id = current_core;
	pthread_mutex_unlock(&g_xlio_pool_mutex);
	SPDK_NOTICELOG("Create xlio pool, packets_pool_size %u on core %u\n",
		       packets_pool_size, current_core);

	return pool;

fail:
	pthread_mutex_unlock(&g_xlio_pool_mutex);
	return NULL;
}

static struct spdk_xlio_sock *
xlio_sock_alloc(int fd, bool enable_zero_copy, enum xlio_sock_create_type type)
{
	struct spdk_xlio_sock *sock;
#if defined(SPDK_ZEROCOPY) || defined(__linux__)
	int flag;
	int rc;
#endif

	sock = calloc(1, sizeof(*sock));
	if (sock == NULL) {
		SPDK_ERRLOG("sock allocation failed\n");
		return NULL;
	}

	sock->fd = fd;

#if defined(SPDK_ZEROCOPY)
	flag = 1;

	if (enable_zero_copy) {

		/* Try to turn on zero copy sends */
		rc = xlio_setsockopt(sock->fd, SOL_SOCKET, SO_ZEROCOPY, &flag, sizeof(flag));
		if (rc == 0) {
			sock->flags.zcopy = true;
		} else {
			SPDK_WARNLOG("Zcopy send is not supported\n");
		}
	}

	if (type != SPDK_SOCK_CREATE_LISTEN) {
		sock->pd = xlio_get_pd(fd);
		if (!sock->pd) {
			SPDK_ERRLOG("Failed to get pd\n");
			goto fail;
		}
	}

#endif
	sock->xlio_packets_pool = xlio_sock_get_packets_pool(g_spdk_xlio_sock_impl_opts.packets_pool_size);
	if (!sock->xlio_packets_pool) {
		SPDK_ERRLOG("Failed to allocated packets pool for socket %d\n", fd);
		goto fail;
	}

	if (g_spdk_xlio_sock_impl_opts.enable_zerocopy_recv) {
		sock->flags.recv_zcopy = true;

		STAILQ_INIT(&sock->received_packets);

		if (xlio_sock_alloc_buffers_pool(g_spdk_xlio_sock_impl_opts.buffers_pool_size)) {
			goto fail;
		}

		if (type != SPDK_SOCK_CREATE_LISTEN) {
			uint64_t user_data = (uintptr_t)&sock->base;
			rc = xlio_setsockopt(sock->fd, SOL_SOCKET, SO_XLIO_USER_DATA,
					     &user_data, sizeof(user_data));
			if (rc != 0) {
				SPDK_ERRLOG("Failed to set socket user data for sock %d: rc %d, errno %d\n",
					    sock->fd, rc, errno);
				goto fail;
			}
		}
	}

#if defined(__linux__)
	flag = 1;

	if (g_spdk_xlio_sock_impl_opts.enable_quickack) {
		rc = xlio_setsockopt(sock->fd, IPPROTO_TCP, TCP_QUICKACK, &flag, sizeof(flag));
		if (rc != 0) {
			SPDK_ERRLOG("quickack was failed to set\n");
		}
	}
#endif

	return sock;

fail:
	free(sock);
	return NULL;
}

static bool
sock_is_loopback(int fd)
{
	struct ifaddrs *addrs, *tmp;
	struct sockaddr_storage sa = {};
	socklen_t salen;
	struct ifreq ifr = {};
	char ip_addr[256], ip_addr_tmp[256];
	int rc;
	bool is_loopback = false;

	salen = sizeof(sa);
	rc = xlio_getsockname(fd, (struct sockaddr *)&sa, &salen);
	if (rc != 0) {
		return is_loopback;
	}

	memset(ip_addr, 0, sizeof(ip_addr));
	rc = spdk_net_get_address_string((struct sockaddr *)&sa, ip_addr, sizeof(ip_addr));
	if (rc != 0) {
		return is_loopback;
	}

	getifaddrs(&addrs);
	for (tmp = addrs; tmp != NULL; tmp = tmp->ifa_next) {
		if (tmp->ifa_addr && (tmp->ifa_flags & IFF_UP) &&
		    (tmp->ifa_addr->sa_family == sa.ss_family)) {
			memset(ip_addr_tmp, 0, sizeof(ip_addr_tmp));
			rc = spdk_net_get_address_string(tmp->ifa_addr, ip_addr_tmp, sizeof(ip_addr_tmp));
			if (rc != 0) {
				continue;
			}

			if (strncmp(ip_addr, ip_addr_tmp, sizeof(ip_addr)) == 0) {
				memcpy(ifr.ifr_name, tmp->ifa_name, sizeof(ifr.ifr_name));
				xlio_ioctl(fd, SIOCGIFFLAGS, &ifr);
				if (ifr.ifr_flags & IFF_LOOPBACK) {
					is_loopback = true;
				}
				goto end;
			}
		}
	}

end:
	freeifaddrs(addrs);
	return is_loopback;
}

static int
xlio_sock_set_nonblock(int fd)
{
	int flag;

	flag = xlio_fcntl(fd, F_GETFL);
	if (xlio_fcntl(fd, F_SETFL, flag | O_NONBLOCK) < 0) {
		SPDK_ERRLOG("fcntl can't set nonblocking mode for socket, fd: %d (%d)\n",
			    fd, errno);
		return -1;
	}

	return 0;
}

static inline const char *
strip_ip(const char *ip, char *buf, size_t buf_size)
{
	char *p;

	if (ip[0] == '[') {
		snprintf(buf, buf_size, "%s", ip + 1);
		p = strchr(buf, ']');
		if (p != NULL) {
			*p = '\0';
		}
		return buf;
	}

	return ip;
}

static inline int
xlio_bind_client_socket(int fd, const char *addr, int port)
{
	char buf[256];
	char portnum[32];
	struct addrinfo hints, *res;
	int rc;

	assert(addr || port);

	if (addr) {
		addr = strip_ip(addr, buf, sizeof(buf));
	}

	snprintf(portnum, sizeof portnum, "%d", port);
	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV | AI_NUMERICHOST;
	rc = xlio_getaddrinfo(addr, portnum, &hints, &res);
	if (rc != 0) {
		SPDK_ERRLOG("Source getaddrinfo() failed %s (%d), address %s, port %d\n",
			    xlio_gai_strerror(rc), rc, addr ? addr : "null", port);
		return -1;
	}

	rc = xlio_bind(fd, res->ai_addr, res->ai_addrlen);
	if (rc != 0) {
		SPDK_ERRLOG("bind() failed at address %s port %d, errno = %d\n",
			    addr ? addr : "null", port, errno);
		xlio_freeaddrinfo(res);
		return -1;
	}

	xlio_freeaddrinfo(res);
	return 0;
}

static struct spdk_sock *
xlio_sock_create(const char *ip, int port,
		 enum xlio_sock_create_type type,
		 struct spdk_sock_opts *opts)
{
	struct spdk_xlio_sock *sock;
	char buf[MAX_TMPBUF];
	char portnum[PORTNUMLEN];
	const char *src_addr;
	uint16_t src_port;
	struct addrinfo hints, *res, *res0;
	int fd;
	int val = 1;
	int rc, sz;
	bool enable_zcopy_user_opts = true;
	bool enable_zcopy_impl_opts = true;

	assert(opts != NULL);

	if (ip == NULL) {
		return NULL;
	}

	ip = strip_ip(ip, buf, sizeof(buf));
	snprintf(portnum, sizeof portnum, "%d", port);
	memset(&hints, 0, sizeof hints);
	hints.ai_family = PF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_NUMERICSERV;
	hints.ai_flags |= AI_PASSIVE;
	hints.ai_flags |= AI_NUMERICHOST;
	rc = xlio_getaddrinfo(ip, portnum, &hints, &res0);
	if (rc != 0) {
		SPDK_ERRLOG("getaddrinfo() failed %s (%d)\n", xlio_gai_strerror(rc), rc);
		return NULL;
	}

	/* try listen */
	fd = -1;
	for (res = res0; res != NULL; res = res->ai_next) {
retry:
		fd = xlio_socket(res->ai_family, res->ai_socktype, res->ai_protocol);
		if (fd < 0) {
			/* error */
			continue;
		}

		sz = g_spdk_xlio_sock_impl_opts.recv_buf_size;
		rc = xlio_setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &sz, sizeof(sz));
		if (rc) {
			/* Not fatal */
		}

		sz = g_spdk_xlio_sock_impl_opts.send_buf_size;
		rc = xlio_setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sz, sizeof(sz));
		if (rc) {
			/* Not fatal */
		}

		rc = xlio_setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof val);
		if (rc != 0) {
			xlio_close(fd);
			/* error */
			continue;
		}

		if (g_spdk_xlio_sock_impl_opts.enable_tcp_nodelay) {
			rc = xlio_setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &val, sizeof val);
			if (rc != 0) {
				xlio_close(fd);
				/* error */
				continue;
			}
		}

#if defined(SO_PRIORITY)
		if (opts->priority) {
			rc = xlio_setsockopt(fd, SOL_SOCKET, SO_PRIORITY, &opts->priority, sizeof val);
			if (rc != 0) {
				xlio_close(fd);
				/* error */
				continue;
			}
		}
#endif

		if (res->ai_family == AF_INET6) {
			rc = xlio_setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &val, sizeof val);
			if (rc != 0) {
				xlio_close(fd);
				/* error */
				continue;
			}
		}

		if (opts->ack_timeout) {
#if defined(__linux__)
			int to;

			to = opts->ack_timeout;
			rc = xlio_setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &to, sizeof(to));
			if (rc != 0) {
				xlio_close(fd);
				/* error */
				continue;
			}
#else
			SPDK_WARNLOG("TCP_USER_TIMEOUT is not supported.\n");
#endif
		}

		if (type == SPDK_SOCK_CREATE_LISTEN) {
			rc = xlio_bind(fd, res->ai_addr, res->ai_addrlen);
			if (rc != 0) {
				SPDK_ERRLOG("bind() failed at port %d, errno = %d\n", port, errno);
				switch (errno) {
				case EINTR:
					/* interrupted? */
					xlio_close(fd);
					goto retry;
				case EADDRNOTAVAIL:
					SPDK_ERRLOG("IP address %s not available. "
						    "Verify IP address in config file "
						    "and make sure setup script is "
						    "run before starting spdk app.\n", ip);
				/* FALLTHROUGH */
				default:
					/* try next family */
					xlio_close(fd);
					fd = -1;
					continue;
				}
			}
			/* bind OK */
			rc = xlio_listen(fd, 512);
			if (rc != 0) {
				SPDK_ERRLOG("listen() failed, errno = %d\n", errno);
				xlio_close(fd);
				fd = -1;
				break;
			}

			enable_zcopy_impl_opts = g_spdk_xlio_sock_impl_opts.enable_zerocopy_send_server;
		} else if (type == SPDK_SOCK_CREATE_CONNECT) {
			uint64_t user_data = (uintptr_t)NULL;
			rc = xlio_setsockopt(fd, SOL_SOCKET, SO_XLIO_USER_DATA,
					     &user_data, sizeof(user_data));
			if (rc != 0) {
				SPDK_ERRLOG("Failed to set socket user data for sock %d: rc %d, errno %d\n",
					    fd, rc, errno);
				xlio_close(fd);
				fd = -1;
				break;
			}

			src_addr = SPDK_GET_FIELD(opts, src_addr, NULL, opts->opts_size);
			src_port = SPDK_GET_FIELD(opts, src_port, 0, opts->opts_size);
			if (src_addr != NULL || src_port != 0) {
				rc = xlio_bind_client_socket(fd, src_addr, src_port);
				if (rc != 0) {
					xlio_close(fd);
					fd = -1;
					continue;
				}
			}

			rc = xlio_connect(fd, res->ai_addr, res->ai_addrlen);
			if (rc != 0) {
				if (rc != EAGAIN && rc != EWOULDBLOCK && errno != EINPROGRESS) {
					SPDK_ERRLOG("connect() failed, rc %d, errno = %d\n", rc, errno);
					/* try next family */
					xlio_close(fd);
					fd = -1;
					continue;
				}
			}

			enable_zcopy_impl_opts = g_spdk_xlio_sock_impl_opts.enable_zerocopy_send_client;
		}

		if (xlio_sock_set_nonblock(fd)) {
			xlio_close(fd);
			fd = -1;
			break;
		}
		break;
	}
	xlio_freeaddrinfo(res0);

	if (fd < 0) {
		return NULL;
	}

	/* Only enable zero copy for non-loopback sockets. */
	enable_zcopy_user_opts = opts->zcopy && !sock_is_loopback(fd);

	sock = xlio_sock_alloc(fd, enable_zcopy_user_opts && enable_zcopy_impl_opts, type);
	if (sock == NULL) {
		SPDK_ERRLOG("sock allocation failed\n");
		xlio_close(fd);
		return NULL;
	}

	if (opts != NULL) {
		sock->so_priority = opts->priority;
	}

	SPDK_NOTICELOG("Created xlio sock %d: send zcopy %d, recv zcopy %d, pd %p, context %p, dev %s, handle %u\n",
		       fd, sock->flags.zcopy, sock->flags.recv_zcopy, sock->pd,
		       sock->pd ? sock->pd->context : NULL,
		       sock->pd ? sock->pd->context->device->name : "unknown",
		       sock->pd ? sock->pd->handle : 0);
	return &sock->base;
}

static struct spdk_sock *
xlio_sock_listen(const char *ip, int port, struct spdk_sock_opts *opts)
{
	return xlio_sock_create(ip, port, SPDK_SOCK_CREATE_LISTEN, opts);
}

static struct spdk_sock *
xlio_sock_connect(const char *ip, int port, struct spdk_sock_opts *opts)
{
	return xlio_sock_create(ip, port, SPDK_SOCK_CREATE_CONNECT, opts);
}

static struct spdk_sock *
xlio_sock_accept(struct spdk_sock *_sock)
{
	struct spdk_xlio_sock		*sock = __xlio_sock(_sock);
	struct sockaddr_storage		sa;
	socklen_t			salen;
	int				rc, fd;
	struct spdk_xlio_sock		*new_sock;
	int				flag;

	memset(&sa, 0, sizeof(sa));
	salen = sizeof(sa);

	assert(sock != NULL);

	rc = xlio_accept(sock->fd, (struct sockaddr *)&sa, &salen);

	if (rc == -1) {
		return NULL;
	}

	fd = rc;

	flag = xlio_fcntl(fd, F_GETFL);
	if ((!(flag & O_NONBLOCK)) && (xlio_fcntl(fd, F_SETFL, flag | O_NONBLOCK) < 0)) {
		SPDK_ERRLOG("fcntl can't set nonblocking mode for socket, fd: %d (%d)\n", fd, errno);
		xlio_close(fd);
		return NULL;
	}

#if defined(SO_PRIORITY)
	/* The priority is not inherited, so call this function again */
	if (sock->base.opts.priority) {
		rc = xlio_setsockopt(fd, SOL_SOCKET, SO_PRIORITY, &sock->base.opts.priority, sizeof(int));
		if (rc != 0) {
			xlio_close(fd);
			return NULL;
		}
	}
#endif

	/* Inherit the zero copy feature from the listen socket */
	new_sock = xlio_sock_alloc(fd, sock->flags.zcopy, SPDK_SOCK_CREATE_CONNECT);
	if (new_sock == NULL) {
		xlio_close(fd);
		return NULL;
	}
	new_sock->so_priority = sock->base.opts.priority;

	return &new_sock->base;
}

static void xlio_sock_free_packet(struct spdk_xlio_sock *sock, struct xlio_sock_packet *packet);

static int
xlio_sock_close(struct spdk_sock *_sock)
{
	struct spdk_xlio_sock *sock = __xlio_sock(_sock);

	assert(_sock->group_impl == NULL);

	while (!STAILQ_EMPTY(&sock->received_packets)) {
		struct xlio_sock_packet *packet = STAILQ_FIRST(&sock->received_packets);

		STAILQ_REMOVE_HEAD(&sock->received_packets, link);
		if (--packet->refs == 0) {
			xlio_sock_free_packet(sock, packet);
		} else {
			SPDK_ERRLOG("Socket close: received packet with non zero refs %u, fd %d\n",
				    packet->refs, sock->fd);
		}
	}

	assert(TAILQ_EMPTY(&_sock->pending_reqs));

	/* If the socket fails to close, the best choice is to
	 * leak the fd but continue to free the rest of the sock
	 * memory. */
	xlio_close(sock->fd);

	if (sock->ring_fd && --sock->ring_fd->refs == 0) {
		free(sock->ring_fd);
		sock->ring_fd = NULL;
	}
	free(sock);

	return 0;
}

#ifdef SPDK_ZEROCOPY
static int
_sock_check_zcopy(struct spdk_sock *sock)
{
	struct spdk_xlio_sock *vsock = __xlio_sock(sock);
	struct spdk_xlio_sock_group_impl *group = __xlio_group_impl(sock->group_impl);
	struct msghdr msgh = {};
	uint8_t buf[sizeof(struct cmsghdr) + sizeof(struct sock_extended_err)];
	ssize_t rc;
	struct sock_extended_err *serr;
	struct cmsghdr *cm;
	uint32_t idx;
	struct spdk_sock_request *req, *treq;
	bool found;

	msgh.msg_control = buf;
	msgh.msg_controllen = sizeof(buf);

	while (true) {
		rc = xlio_recvmsg(vsock->fd, &msgh, MSG_ERRQUEUE);

		if (rc < 0) {
			if (errno == EWOULDBLOCK || errno == EAGAIN) {
				return 0;
			}

			if (!TAILQ_EMPTY(&sock->pending_reqs)) {
				SPDK_ERRLOG("Attempting to receive from ERRQUEUE yielded error, but pending list still has orphaned entries\n");
			} else {
				SPDK_WARNLOG("Recvmsg yielded an error!\n");
			}
			return 0;
		}

		cm = CMSG_FIRSTHDR(&msgh);
		if (!cm || cm->cmsg_level != SOL_IP || cm->cmsg_type != IP_RECVERR) {
			SPDK_WARNLOG("Unexpected cmsg level or type!\n");
			return 0;
		}

		serr = (struct sock_extended_err *)CMSG_DATA(cm);
		if (serr->ee_errno != 0 || serr->ee_origin != SO_EE_ORIGIN_ZEROCOPY) {
			SPDK_WARNLOG("Unexpected extended error origin\n");
			return 0;
		}

		/* Most of the time, the pending_reqs array is in the exact
		 * order we need such that all of the requests to complete are
		 * in order, in the front. It is guaranteed that all requests
		 * belonging to the same sendmsg call are sequential, so once
		 * we encounter one match we can stop looping as soon as a
		 * non-match is found.
		 */
		for (idx = serr->ee_info; idx <= serr->ee_data; idx++) {
			found = false;

			TAILQ_FOREACH_SAFE(req, &sock->pending_reqs, internal.link, treq) {
				if (!req->internal.is_zcopy) {
					/* This wasn't a zcopy request. It was just waiting in line to complete */
					rc = spdk_sock_request_put(sock, req, 0);
					if (rc < 0) {
						return rc;
					}
				} else if (req->internal.offset == idx) {
					found = true;

					rc = spdk_sock_request_put(sock, req, 0);
					if (rc < 0) {
						return rc;
					}

				} else if (found) {
					break;
				}
			}

			/* If we reaped buffer reclaim notification and sock is not in pending_recv list yet,
			 * add it now. It allows to call socket callback and process completions */
			if (found && !vsock->flags.pending_recv && group) {
				vsock->flags.pending_recv = true;
				TAILQ_INSERT_TAIL(&group->pending_recv, vsock, link);
			}
		}
	}

	return 0;
}
#endif


static int
xlio_sock_flush(struct spdk_sock *sock)
{
#ifdef SPDK_ZEROCOPY
	struct spdk_xlio_sock *vsock = __xlio_sock(sock);

	if (vsock->flags.zcopy && !TAILQ_EMPTY(&sock->pending_reqs)) {
		_sock_check_zcopy(sock);
	}
#endif

	return _sock_flush_ext(sock);
}

static inline struct xlio_recvfrom_zcopy_packet_t *
next_packet(struct xlio_recvfrom_zcopy_packet_t *packet)
{
	return (struct xlio_recvfrom_zcopy_packet_t *)((char *)packet +
			sizeof(struct xlio_recvfrom_zcopy_packet_t) +
			packet->sz_iov * sizeof(struct iovec));
}

#ifdef DEBUG
static void
dump_packet(struct spdk_xlio_sock *sock, struct xlio_sock_packet *packet)
{
	size_t i;
	struct xlio_buff_t *xlio_buf;

	SPDK_DEBUGLOG(xlio, "Sock %d packet %p: num_bufs %lu, total_len %u, first buf %p\n",
		      sock->fd,
		      packet, packet->xlio_packet.num_bufs,
		      packet->xlio_packet.total_len,
		      packet->xlio_packet.buff_lst);

	for (i = 0, xlio_buf = packet->xlio_packet.buff_lst;
	     xlio_buf;
	     ++i, xlio_buf = xlio_buf->next) {
		SPDK_DEBUGLOG(xlio, "Packet %p[%lu]: payload %p, len %u\n",
			      packet, i, xlio_buf->payload, xlio_buf->len);
	}
}
#endif

static struct xlio_sock_packet *
xlio_sock_get_packet(struct spdk_xlio_sock *sock)
{
	struct xlio_sock_packet *packet = STAILQ_FIRST(&sock->xlio_packets_pool->free_packets);

	assert(packet);
	STAILQ_REMOVE_HEAD(&sock->xlio_packets_pool->free_packets, link);
	assert(sock->xlio_packets_pool->num_free_packets > 0);
	sock->xlio_packets_pool->num_free_packets--;

	return packet;
}

static void
xlio_sock_free_packet(struct spdk_xlio_sock *sock, struct xlio_sock_packet *packet)
{
	int ret;

	SPDK_DEBUGLOG(xlio, "Sock %d: free xlio packet, first buf %p\n",
		      sock->fd, packet->xlio_packet.buff_lst);
	assert(packet->refs == 0);
	/* @todo: How heavy is free_packets()? Maybe batch packets to free? */
	ret = xlio_socketxtreme_free_packets(&packet->xlio_packet, 1);
	if (ret < 0) {
		SPDK_ERRLOG("Free xlio packets failed, ret %d, errno %d\n",
			    ret, errno);
	}

	STAILQ_INSERT_HEAD(&sock->xlio_packets_pool->free_packets, packet, link);
	sock->xlio_packets_pool->num_free_packets++;
}

static void
packets_advance(struct spdk_xlio_sock *sock, size_t len)
{
	SPDK_DEBUGLOG(xlio, "Sock %d: advance packets by %lu bytes\n", sock->fd, len);
	while (len > 0) {
		struct xlio_sock_packet *cur_packet = STAILQ_FIRST(&sock->received_packets);
		/* We don't allow to advance by more than we have data in packets */
		assert(cur_packet != NULL);
		struct xlio_buff_t *cur_xlio_buf = sock->cur_xlio_buf;
		assert(cur_xlio_buf != NULL);
		size_t remaining_buf_len = cur_xlio_buf->len - sock->cur_offset;

		if (len < remaining_buf_len) {
			sock->cur_offset += len;
			len = 0;
		} else {
			len -= remaining_buf_len;

			/* Next iov */
			sock->cur_offset = 0;
			sock->cur_xlio_buf = cur_xlio_buf->next;
			if (!sock->cur_xlio_buf) {
				/* Next packet */
				STAILQ_REMOVE_HEAD(&sock->received_packets, link);
				if (--cur_packet->refs == 0) {
					xlio_sock_free_packet(sock, cur_packet);
				}

				cur_packet = STAILQ_FIRST(&sock->received_packets);
				sock->cur_xlio_buf = cur_packet ? cur_packet->xlio_packet.buff_lst : NULL;
			}
		}
	}

	assert(len == 0);
}

static size_t
packets_next_chunk(struct spdk_xlio_sock *sock,
		   void **buf,
		   struct xlio_sock_packet **packet,
		   size_t max_len)
{
	struct xlio_sock_packet *cur_packet = STAILQ_FIRST(&sock->received_packets);

	if (!sock->cur_xlio_buf && cur_packet) {
		sock->cur_xlio_buf = cur_packet->xlio_packet.buff_lst;
	}

	while (cur_packet) {
		struct xlio_buff_t *cur_xlio_buf = sock->cur_xlio_buf;
		assert(cur_xlio_buf);
		size_t len = cur_xlio_buf->len - sock->cur_offset;

		if (len == 0) {
			/* xlio may return zero length iov. Skip to next in this case */
			SPDK_DEBUGLOG(xlio, "Zero length buffer: len %d, offset %lu\n",
				      cur_xlio_buf->len, sock->cur_offset);
			sock->cur_offset = 0;
			sock->cur_xlio_buf = cur_xlio_buf->next;
			if (!sock->cur_xlio_buf) {
				/* Next packet */
				cur_packet = STAILQ_NEXT(cur_packet, link);
				sock->cur_xlio_buf = cur_packet ? cur_packet->xlio_packet.buff_lst : NULL;
			}
			continue;
		}

		assert(max_len > 0);
		assert(len > 0);
		len = spdk_min(len, max_len);
		*buf = cur_xlio_buf->payload + sock->cur_offset;
		*packet = cur_packet;
		return len;
	}

	return 0;
}

static int
poll_no_group_socket(struct spdk_xlio_sock *sock)
{
	int ret;
	uint32_t max_events_per_poll;

	/* For sockets not bound to group we have to poll here.
	 * Polling may find events for other sockets but not for this one.
	 * So, we need to check if new packets were added for this socket.
	 */
	if (!sock->ring_fd) {
		int ring_fds[2];
		int num_rings;

		ret = xlio_get_socket_rings_fds(sock->fd, ring_fds, 2);
		if (ret < 0) {
			SPDK_ERRLOG("Failed to get ring FDs for socket %d: rc %d, errno %d\n",
				    sock->fd, ret, errno);
			return ret;
		}

		num_rings = ret;
		/* @todo: add support for multiple rings */
		assert(num_rings == 1);
		sock->ring_fd = calloc(1, sizeof(struct spdk_xlio_ring_fd));
		if (!sock->ring_fd) {
			SPDK_ERRLOG("Failed to allocate ring_fd\n");
			return -1;
		}
		sock->ring_fd->ring_fd = ring_fds[0];
		sock->ring_fd->refs = 1;
		SPDK_NOTICELOG("Discovered ring fd %d for socket %d, num_rings %d\n",
			       sock->ring_fd->ring_fd, sock->fd, num_rings);
	}

	if (sock->xlio_packets_pool->num_free_packets) {
		max_events_per_poll = spdk_min(sock->xlio_packets_pool->num_free_packets, MAX_EVENTS_PER_POLL);

		ret = xlio_sock_poll_fd(sock->ring_fd->ring_fd, max_events_per_poll);
		if (ret < 0) {
			return -1;
		}
	} else {
		SPDK_DEBUGLOG(xlio, "no free packets\n");
	}

	if (STAILQ_EMPTY(&sock->received_packets)) {
		errno = EAGAIN;
		return -1;
	}

	return 0;
}

static int
readv_wrapper(struct spdk_xlio_sock *sock, struct iovec *iovs, int iovcnt)
{
	int ret;

	if (sock->flags.recv_zcopy) {
		int i;
		size_t offset = 0;

		if (STAILQ_EMPTY(&sock->received_packets)) {
			if (spdk_unlikely(!sock->base.group_impl)) {
				ret = poll_no_group_socket(sock);
				if (ret < 0) {
					if (sock->flags.disconnected) {
						return 0;
					}
					return ret;
				}
			} else {
				/* @todo: should we try to poll here? */
				if (sock->flags.disconnected) {
					return 0;
				}
				errno = EAGAIN;
				return -1;
			}
		}

		assert(!STAILQ_EMPTY(&sock->received_packets));
		ret = 0;
		i = 0;
		while (i < iovcnt) {
			void *buf;
			size_t len;
			struct iovec *iov = &iovs[i];
			size_t iov_len = iov->iov_len - offset;
			struct xlio_sock_packet *packet;

			len = packets_next_chunk(sock, &buf, &packet, iov_len);
			if (len == 0) {
				/* No more data */
				SPDK_DEBUGLOG(xlio, "Sock %d: readv_wrapper ret %d\n", sock->fd, ret);
				return ret;
			}

			memcpy(iov->iov_base + offset, buf, len);
			packets_advance(sock, len);
			ret += len;
			offset += len;
			assert(offset <= iov->iov_len);
			if (offset == iov->iov_len) {
				offset = 0;
				i++;
			}
		}

		SPDK_DEBUGLOG(xlio, "Sock %d: readv_wrapper ret %d\n", sock->fd, ret);
	} else {
		ret = xlio_readv(sock->fd, iovs, iovcnt);
		SPDK_DEBUGLOG(xlio, "Sock %d: readv_wrapper ret %d, errno %d\n", sock->fd, ret, errno);
	}

	return ret;
}

static ssize_t
xlio_sock_readv(struct spdk_sock *_sock, struct iovec *iov, int iovcnt)
{
	struct spdk_xlio_sock *sock = __xlio_sock(_sock);

	return readv_wrapper(sock, iov, iovcnt);
}

static ssize_t
xlio_sock_recv(struct spdk_sock *sock, void *buf, size_t len)
{
	struct iovec iov[1];

	iov[0].iov_base = buf;
	iov[0].iov_len = len;

	return xlio_sock_readv(sock, iov, 1);
}

static ssize_t
xlio_sock_writev(struct spdk_sock *_sock, struct iovec *iov, int iovcnt)
{
	struct spdk_xlio_sock *sock = __xlio_sock(_sock);
	int rc;

	/* In order to process a writev, we need to flush any asynchronous writes
	 * first. */
	rc = _sock_flush_ext(_sock);
	if (rc < 0) {
		return rc;
	}

	if (!TAILQ_EMPTY(&_sock->queued_reqs)) {
		/* We weren't able to flush all requests */
		errno = EAGAIN;
		return -1;
	}

	return xlio_writev(sock->fd, iov, iovcnt);
}

union _mkeys_container {
	char buf[CMSG_SPACE(sizeof(struct xlio_pd_key) * IOV_BATCH_SIZE)];
	struct cmsghdr align;
};

static inline size_t
xlio_sock_prep_reqs(struct spdk_sock *_sock, struct iovec *iovs, struct msghdr *msg,
		    union _mkeys_container *mkeys_container, uint32_t *_total)
{
	size_t iovcnt = 0;
	int i;
	struct spdk_sock_request *req;
	unsigned int offset;
	struct cmsghdr *cmsg;
	struct xlio_pd_key *mkeys = NULL;
	uint32_t total = 0;
	bool first_req_mkey;

	req = TAILQ_FIRST(&_sock->queued_reqs);
	assert(req);
	first_req_mkey = req->mkeys != NULL;

	msg->msg_control = mkeys_container->buf;
	msg->msg_controllen = sizeof(mkeys_container->buf);
	cmsg = CMSG_FIRSTHDR(msg);

	cmsg->cmsg_len = CMSG_LEN(sizeof(struct xlio_pd_key) * IOV_BATCH_SIZE);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_XLIO_PD;

	mkeys = (struct xlio_pd_key *)CMSG_DATA(cmsg);

	while (req && iovcnt < IOV_BATCH_SIZE) {
		offset = req->internal.offset;

		if (first_req_mkey == !req->mkeys) {
			/* mkey setting or zcopy threshold is different with the first req */
			break;
		}

		for (i = 0; i < req->iovcnt && iovcnt < IOV_BATCH_SIZE; i++) {
			/* Consume any offset first */
			if (offset >= SPDK_SOCK_REQUEST_IOV(req, i)->iov_len) {
				offset -= SPDK_SOCK_REQUEST_IOV(req, i)->iov_len;
				continue;
			}
			if (first_req_mkey) {
				assert(req->mkeys);
				mkeys[iovcnt].mkey = req->mkeys[i];
				mkeys[iovcnt].flags = 0;
			}
			iovs[iovcnt].iov_base = SPDK_SOCK_REQUEST_IOV(req, i)->iov_base + offset;
			iovs[iovcnt].iov_len = SPDK_SOCK_REQUEST_IOV(req, i)->iov_len - offset;
			total += (uint32_t)iovs[iovcnt].iov_len;
			iovcnt++;

			offset = 0;
		}

		req = TAILQ_NEXT(req, internal.link);
	}

	if (first_req_mkey) {
		msg->msg_controllen = CMSG_SPACE(sizeof(struct xlio_pd_key) * iovcnt);
		cmsg->cmsg_len = CMSG_LEN(sizeof(struct xlio_pd_key) * iovcnt);
	} else {
		msg->msg_control = NULL;
		msg->msg_controllen = 0;
	}

	*_total = total;

	return iovcnt;
}

static bool
xlio_sock_flush_now(struct spdk_sock *sock, uint32_t qlen_bytes)
{
	struct spdk_xlio_sock *vsock = __xlio_sock(sock);

	if (g_spdk_xlio_sock_impl_opts.flush_batch_timeout) {
		uint64_t now = spdk_get_ticks();
		if (qlen_bytes >= g_spdk_xlio_sock_impl_opts.flush_batch_bytes_threshold) {
			/* Flush now */
		} else if (vsock->batch_start_tsc &&
			   (now - vsock->batch_start_tsc) * SPDK_SEC_TO_USEC / spdk_get_ticks_hz() >
			   g_spdk_xlio_sock_impl_opts.flush_batch_timeout) {
			/* batch timeout */
			if (sock->queued_iovcnt < vsock->batch_nr) {
				vsock->batch_nr = spdk_max(vsock->batch_nr >> 1, 1);
			}
		} else if (sock->queued_iovcnt >= vsock->batch_nr) {
			/* Try to flush socket before timeout, so could batch more */
			vsock->batch_nr = spdk_min(vsock->batch_nr + 1,
						   g_spdk_xlio_sock_impl_opts.flush_batch_iovcnt_threshold);
		} else {
			/* Not flush socket, try to batch more requests */
			if (!vsock->batch_start_tsc) {
				vsock->batch_start_tsc = now;
			}
			return false;
		}
		vsock->batch_start_tsc = 0;
	}

	return true;
}

static int
_sock_flush_ext(struct spdk_sock *sock)
{
	struct iovec iovs[IOV_BATCH_SIZE];
	struct msghdr msg = {};
	union _mkeys_container mkeys_container;
	struct spdk_xlio_sock *vsock = __xlio_sock(sock);
	size_t iovcnt;
	struct spdk_sock_request *req;
	ssize_t rc;
	size_t len;
	int flags = 0;
	int retval;
	int i;
	unsigned int offset;
	uint32_t zerocopy_threshold;
	uint32_t total;
	bool is_zcopy = false;

	/* Can't flush from within a callback or we end up with recursive calls */
	if (sock->cb_cnt > 0) {
		return 0;
	}

	if (spdk_unlikely(TAILQ_EMPTY(&sock->queued_reqs))) {
		return 0;
	}

	iovcnt = xlio_sock_prep_reqs(sock, iovs, &msg, &mkeys_container, &total);
	if (spdk_unlikely(iovcnt == 0)) {
		return 0;
	}

	assert(!(!vsock->flags.zcopy && msg.msg_controllen > 0));

	zerocopy_threshold = g_spdk_xlio_sock_impl_opts.zerocopy_threshold;

	if (!xlio_sock_flush_now(sock, total)) {
		return 0;
	}

	/* Allow zcopy if enabled on socket and either the data needs to be sent,
	 * which is reported by xlio_sock_prep_reqs() with setting msg.msg_controllen
	 * or the msg size is bigger than the threshold configured. */
	if (vsock->flags.zcopy && (msg.msg_controllen || total >= zerocopy_threshold)) {
		flags = MSG_ZEROCOPY;
		is_zcopy = true;
	}
	/* Perform the vectored write */
	msg.msg_iov = iovs;
	msg.msg_iovlen = iovcnt;

	rc = xlio_sendmsg(vsock->fd, &msg, flags);
	if (rc <= 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK || (errno == ENOBUFS && vsock->flags.zcopy)) {
			return 0;
		}

		SPDK_ERRLOG("sendmsg error %zd\n", rc);
		return rc;
	}

	if (is_zcopy) {
		/* Handling overflow case, because we use vsock->sendmsg_idx - 1 for the
		 * req->internal.offset, so sendmsg_idx should not be zero  */
		if (spdk_unlikely(vsock->sendmsg_idx == UINT32_MAX)) {
			vsock->sendmsg_idx = 1;
		} else {
			vsock->sendmsg_idx++;
		}
	}

	/* Consume the requests that were actually written */
	req = TAILQ_FIRST(&sock->queued_reqs);
	while (req) {
		offset = req->internal.offset;

		/* req->internal.is_zcopy is true when the whole req or part of it is
		 * sent with zerocopy */
		req->internal.is_zcopy = is_zcopy;

		for (i = 0; i < req->iovcnt; i++) {
			/* Advance by the offset first */
			if (offset >= SPDK_SOCK_REQUEST_IOV(req, i)->iov_len) {
				offset -= SPDK_SOCK_REQUEST_IOV(req, i)->iov_len;
				continue;
			}

			/* Calculate the remaining length of this element */
			len = SPDK_SOCK_REQUEST_IOV(req, i)->iov_len - offset;

			if (len > (size_t)rc) {
				/* This element was partially sent. */
				req->internal.offset += rc;
				return 0;
			}

			offset = 0;
			req->internal.offset += len;
			rc -= len;
		}

		/* Handled a full request. */
		spdk_sock_request_pend(sock, req);

		/* Ordering control. */
		if (!req->internal.is_zcopy && req == TAILQ_FIRST(&sock->pending_reqs)) {
			/* The sendmsg syscall above isn't currently asynchronous,
			* so it's already done. */
			retval = spdk_sock_request_put(sock, req, 0);
			if (spdk_unlikely(retval)) {
				break;
			}
		} else {
			/* Re-use the offset field to hold the sendmsg call index. The
			 * index is 0 based, so subtract one here because we've already
			 * incremented above. */
			req->internal.offset = vsock->sendmsg_idx - 1;
		}

		if (rc == 0) {
			break;
		}

		req = TAILQ_FIRST(&sock->queued_reqs);
	}

	return 0;
}


static void
xlio_sock_writev_async(struct spdk_sock *sock, struct spdk_sock_request *req)
{
	struct spdk_xlio_sock *vsock = __xlio_sock(sock);
	struct spdk_xlio_sock_group_impl *group = __xlio_group_impl(sock->group_impl);
	int rc;

	spdk_sock_request_queue(sock, req);

	/* If there are a sufficient number queued, just flush them out immediately. */
	if (sock->queued_iovcnt >= IOV_BATCH_SIZE) {
		rc = _sock_flush_ext(sock);
		if (spdk_likely(rc == 0)) {
			if (TAILQ_EMPTY(&sock->queued_reqs) && vsock->flags.pending_send && sock->group_impl) {
				TAILQ_REMOVE(&group->pending_send, vsock, link_send);
				vsock->flags.pending_send = false;
			}
		} else {
			spdk_sock_abort_requests(sock);
		}
	} else if (!vsock->flags.pending_send && sock->group_impl) {
		TAILQ_INSERT_TAIL(&group->pending_send, vsock, link_send);
		vsock->flags.pending_send = true;
	}
}

static int
xlio_sock_set_recvlowat(struct spdk_sock *_sock, int nbytes)
{
	struct spdk_xlio_sock *sock = __xlio_sock(_sock);
	int val;
	int rc;

	assert(sock != NULL);

	val = nbytes;
	rc = xlio_setsockopt(sock->fd, SOL_SOCKET, SO_RCVLOWAT, &val, sizeof val);
	if (rc != 0) {
		SPDK_DEBUGLOG(xlio, "Set SO_RECVLOWAT failed: rc %d\n", rc);
	}
	return 0;
}

static bool
xlio_sock_is_ipv6(struct spdk_sock *_sock)
{
	struct spdk_xlio_sock *sock = __xlio_sock(_sock);
	struct sockaddr_storage sa;
	socklen_t salen;
	int rc;

	assert(sock != NULL);

	memset(&sa, 0, sizeof sa);
	salen = sizeof sa;
	rc = xlio_getsockname(sock->fd, (struct sockaddr *) &sa, &salen);
	if (rc != 0) {
		SPDK_ERRLOG("getsockname() failed (errno=%d)\n", errno);
		return false;
	}

	return (sa.ss_family == AF_INET6);
}

static bool
xlio_sock_is_ipv4(struct spdk_sock *_sock)
{
	struct spdk_xlio_sock *sock = __xlio_sock(_sock);
	struct sockaddr_storage sa;
	socklen_t salen;
	int rc;

	assert(sock != NULL);

	memset(&sa, 0, sizeof sa);
	salen = sizeof sa;
	rc = xlio_getsockname(sock->fd, (struct sockaddr *) &sa, &salen);
	if (rc != 0) {
		SPDK_ERRLOG("getsockname() failed (errno=%d)\n", errno);
		return false;
	}

	return (sa.ss_family == AF_INET);
}

static bool
xlio_sock_is_connected(struct spdk_sock *_sock)
{
	struct spdk_xlio_sock *sock = __xlio_sock(_sock);
	uint8_t byte;
	int rc;

	rc = xlio_recv(sock->fd, &byte, 1, MSG_PEEK);
	if (rc == 0) {
		return false;
	}

	if (rc < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			return true;
		}

		return false;
	}

	return true;
}

static struct spdk_sock_group_impl *
xlio_sock_group_impl_create(void)
{
	struct spdk_xlio_sock_group_impl *group_impl;
	uint32_t num_packets = g_spdk_xlio_sock_impl_opts.packets_pool_size;
	uint32_t num_buffers = g_spdk_xlio_sock_impl_opts.buffers_pool_size;

	group_impl = calloc(1, sizeof(*group_impl));
	if (group_impl == NULL) {
		SPDK_ERRLOG("group_impl allocation failed\n");
		return NULL;
	}

	TAILQ_INIT(&group_impl->ring_fd);
	TAILQ_INIT(&group_impl->pending_recv);
	TAILQ_INIT(&group_impl->pending_send);

	if (num_packets) {
		group_impl->xlio_packets_pool = xlio_sock_get_packets_pool(num_packets);
		if (!group_impl->xlio_packets_pool) {
			goto fail;
		}
	}

	if (num_buffers && xlio_sock_alloc_buffers_pool(num_buffers)) {
		SPDK_ERRLOG("Failed to allocated buffers pool for group %p\n", group_impl);
		goto fail;
	}

	return &group_impl->base;

fail:
	free(group_impl);
	return NULL;
}

static int
xlio_sock_group_impl_add_sock(struct spdk_sock_group_impl *_group, struct spdk_sock *_sock)
{
	struct spdk_xlio_sock_group_impl *group = __xlio_group_impl(_group);
	struct spdk_xlio_sock *sock = __xlio_sock(_sock);
	struct spdk_xlio_ring_fd *ring_fd;
	int ring_fds[2];
	int rc;

	rc = xlio_get_socket_rings_fds(sock->fd, ring_fds, 2);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to get ring FDs for socket %d\n", sock->fd);
		return rc;
	}

	/* @todo: add support for multiple rings */
	assert(rc == 1);
	SPDK_DEBUGLOG(xlio, "Sock %d ring %d\n", sock->fd, ring_fds[0]);

	TAILQ_FOREACH(ring_fd, &group->ring_fd, link) {
		if (ring_fd->ring_fd == ring_fds[0]) {
			sock->ring_fd = ring_fd;
			ring_fd->refs++;
			return 0;
		}
	}

	if (!sock->ring_fd) {
		sock->ring_fd = calloc(1, sizeof(struct spdk_xlio_ring_fd));
		if (!sock->ring_fd) {
			SPDK_ERRLOG("Failed to allocate ring_fd\n");
			return -1;
		}
	}
	sock->ring_fd->ring_fd = ring_fds[0];
	sock->ring_fd->refs = 1;
	TAILQ_INSERT_TAIL(&group->ring_fd, sock->ring_fd, link);

	return 0;
}

static int
xlio_sock_group_impl_remove_sock(struct spdk_sock_group_impl *_group, struct spdk_sock *_sock)
{
	struct spdk_xlio_sock_group_impl *group = __xlio_group_impl(_group);
	struct spdk_xlio_sock *sock = __xlio_sock(_sock);

	spdk_sock_abort_requests(_sock);
	if (sock->flags.pending_send) {
		TAILQ_REMOVE(&group->pending_send, sock, link_send);
		sock->flags.pending_send = false;
	}
	if (sock->flags.pending_recv) {
		TAILQ_REMOVE(&group->pending_recv, sock, link);
		sock->flags.pending_recv = false;
	}
	if (--sock->ring_fd->refs == 0) {
		TAILQ_REMOVE(&group->ring_fd, sock->ring_fd, link);
		free(sock->ring_fd);
	}
	sock->ring_fd = NULL;

	return 0;
}

static int
xlio_sock_poll_fd(int fd, uint32_t max_events_per_poll)
{
	struct spdk_sock *sock;
	struct spdk_xlio_sock *vsock;
	int num_events, i, rc;
	struct xlio_socketxtreme_completion_t comps[MAX_EVENTS_PER_POLL];

	num_events = xlio_socketxtreme_poll(fd, comps, max_events_per_poll, SOCKETXTREME_POLL_TX);
	if (num_events < 0) {
		SPDK_ERRLOG("Socket extreme poll failed for fd %d: fd, result %d, errno %d\n", fd, num_events,
			    errno);
		return -1;
	}

	for (i = 0; i < num_events; i++) {
		struct xlio_socketxtreme_completion_t *comp = &comps[i];
		struct xlio_sock_packet *packet;

		sock = (struct spdk_sock *)comp->user_data;
		if (!sock) {
			continue;
		}

		vsock = __xlio_sock(sock);
		SPDK_DEBUGLOG(xlio, "XLIO completion[%d]: ring fd %d, events %" PRIx64
			      ", user_data %p, listen_fd %d\n",
			      i, fd, comp->events, (void *)comp->user_data, comp->listen_fd);

		if (comp->events & EPOLLHUP) {
			SPDK_ERRLOG("Got EPOLLHUP event on socket %d, events %" PRIx64 "\n",
				    vsock->fd, comp->events);
			vsock->flags.disconnected = true;
		}

#ifdef SPDK_ZEROCOPY
		if (comp->events & EPOLLERR) {
			rc = _sock_check_zcopy(sock);
			/* If the socket was closed or removed from
			 * the group in response to a send ack, don't
			 * add it to the array here. */
			if (rc || sock->cb_fn == NULL) {
				continue;
			}
		}
#endif
		if (comp->events & XLIO_SOCKETXTREME_PACKET) {
			packet = xlio_sock_get_packet(vsock);
			packet->xlio_packet = comp->packet;
			/*
			 * While the packet is in received list there is data
			 * to read from it.  To avoid free of packets with
			 * unread data we intialize reference counter to 1.
			 */
			packet->refs = 1;
			STAILQ_INSERT_TAIL(&vsock->received_packets, packet, link);
#ifdef DEBUG
			dump_packet(vsock, packet);
#endif
		}

		/* If the socket does not already have recv pending, add it now */
		if ((comp->events & (XLIO_SOCKETXTREME_PACKET | EPOLLHUP)) &&
		    spdk_likely(sock->group_impl) && !vsock->flags.pending_recv) {
			struct spdk_xlio_sock_group_impl *group = __xlio_group_impl(sock->group_impl);

			vsock->flags.pending_recv = true;
			TAILQ_INSERT_TAIL(&group->pending_recv, vsock, link);
		}
	}

	return num_events;
}

static int
xlio_sock_group_impl_poll(struct spdk_sock_group_impl *_group, int max_events,
			  struct spdk_sock **socks)
{
	struct spdk_xlio_sock_group_impl *group = __xlio_group_impl(_group);
	int num_events, i, rc;
	struct spdk_xlio_sock *vsock, *ptmp;
	uint32_t max_events_per_poll;
	struct spdk_xlio_ring_fd *ring_fd;

	/*
	 * Important to use TAILQ_FOREACH_SAFE() here bacause of the following:
	 * - spdk_sock_abort_requests() can lead to removing from ->pending_send
	 *   in xlio_sock_group_impl_remove_sock()
	 * - we remove from ->pending_send if no more ->queued_reqs left.
	 */
	TAILQ_FOREACH_SAFE(vsock, &group->pending_send, link_send, ptmp) {
		assert(vsock->flags.pending_send);

		rc = _sock_flush_ext(&vsock->base);
		if (spdk_likely(rc == 0)) {
			/*
			 * Removing from pendings only in no-error case because
			 * spdk_sock_abort_requests() can cause removing from group,
			 * removing from pendign and kill vsock itself underneath.
			 * In any case we will crash in that case.
			 */
			if (TAILQ_EMPTY(&vsock->base.queued_reqs)) {
				TAILQ_REMOVE(&group->pending_send, vsock, link_send);
				vsock->flags.pending_send = false;
			}
		} else {
			/*
			 * Aborting requests leads to removing from group
			 * and socket close. Removing from group also
			 * removes @vsock from all group pending lists
			 * in xlio_sock_group_impl_remove_sock().
			 */
			spdk_sock_abort_requests(&vsock->base);
		}
	}

	TAILQ_FOREACH(ring_fd, &group->ring_fd, link) {
		if (group->xlio_packets_pool->num_free_packets) {
			max_events_per_poll = spdk_min(group->xlio_packets_pool->num_free_packets, MAX_EVENTS_PER_POLL);
			num_events = xlio_sock_poll_fd(ring_fd->ring_fd, max_events_per_poll);
			if (num_events < 0) {
				/* @todo: what if we have a problem with just one ring and another one is good? */
				return -1;
			}
		} else {
			SPDK_DEBUGLOG(xlio, "no free packets\n");
			break;
		}
	}

	num_events = 0;
	TAILQ_FOREACH_SAFE(vsock, &group->pending_recv, link, ptmp) {
		if (num_events == max_events) {
			break;
		}

		/* If the socket's cb_fn is NULL, just remove it from the
		 * list and do not add it to socks array */
		if (spdk_unlikely(vsock->base.cb_fn == NULL)) {
			vsock->flags.pending_recv = false;
			TAILQ_REMOVE(&group->pending_recv, vsock, link);
			continue;
		}

		socks[num_events++] = &vsock->base;
	}

	/* Cycle the pending_recv list so that each time we poll things aren't
	 * in the same order. */
	for (i = 0; i < num_events; i++) {
		vsock = __xlio_sock(socks[i]);

		TAILQ_REMOVE(&group->pending_recv, vsock, link);
		vsock->flags.pending_recv = false;
	}

	return num_events;
}

static int
xlio_sock_group_impl_close(struct spdk_sock_group_impl *_group)
{
	struct spdk_xlio_sock_group_impl *group = __xlio_group_impl(_group);
	struct spdk_xlio_ring_fd *ring_fd, *ptmp;

	/* All ring_fds should have already been removed while removing sockets from group */
	assert(TAILQ_EMPTY(&group->ring_fd));
	TAILQ_FOREACH_SAFE(ring_fd, &group->ring_fd, link, ptmp) {
		TAILQ_REMOVE(&group->ring_fd, ring_fd, link);
		free(ring_fd);
	}

	free(group);
	return 0;
}

static int
xlio_sock_impl_get_opts(struct spdk_sock_impl_opts *opts, size_t *len)
{
	if (!opts || !len) {
		errno = EINVAL;
		return -1;
	}
	memset(opts, 0, *len);

#define FIELD_OK(field) \
	offsetof(struct spdk_sock_impl_opts, field) + sizeof(opts->field) <= *len

#define GET_FIELD(field) \
	if (FIELD_OK(field)) { \
		opts->field = g_spdk_xlio_sock_impl_opts.field; \
	}

	GET_FIELD(recv_buf_size);
	GET_FIELD(send_buf_size);
	GET_FIELD(enable_recv_pipe);
	GET_FIELD(enable_zerocopy_send);
	GET_FIELD(enable_quickack);
	GET_FIELD(enable_placement_id);
	GET_FIELD(enable_zerocopy_send_server);
	GET_FIELD(enable_zerocopy_send_client);
	GET_FIELD(enable_zerocopy_recv);
	GET_FIELD(zerocopy_threshold);
	GET_FIELD(enable_tcp_nodelay);
	GET_FIELD(buffers_pool_size);
	GET_FIELD(packets_pool_size);
	GET_FIELD(flush_batch_timeout);
	GET_FIELD(flush_batch_iovcnt_threshold);
	GET_FIELD(flush_batch_bytes_threshold);
	GET_FIELD(enable_early_init);

#undef GET_FIELD
#undef FIELD_OK

	*len = spdk_min(*len, sizeof(g_spdk_xlio_sock_impl_opts));
	return 0;
}

static int
xlio_sock_impl_set_opts(const struct spdk_sock_impl_opts *opts, size_t len)
{
	if (!opts) {
		errno = EINVAL;
		return -1;
	}

#define FIELD_OK(field) \
	offsetof(struct spdk_sock_impl_opts, field) + sizeof(opts->field) <= len

#define SET_FIELD(field) \
	if (FIELD_OK(field)) { \
		g_spdk_xlio_sock_impl_opts.field = opts->field; \
	}

	SET_FIELD(recv_buf_size);
	SET_FIELD(send_buf_size);
	SET_FIELD(enable_recv_pipe);
	SET_FIELD(enable_zerocopy_send);
	SET_FIELD(enable_quickack);
	SET_FIELD(enable_placement_id);
	SET_FIELD(enable_zerocopy_send_server);
	SET_FIELD(enable_zerocopy_send_client);
	SET_FIELD(enable_zerocopy_recv);
	SET_FIELD(zerocopy_threshold);
	SET_FIELD(enable_tcp_nodelay);
	SET_FIELD(buffers_pool_size);
	SET_FIELD(packets_pool_size);
	SET_FIELD(flush_batch_timeout);
	SET_FIELD(flush_batch_iovcnt_threshold);
	SET_FIELD(flush_batch_bytes_threshold);
	SET_FIELD(enable_early_init);

#undef SET_FIELD
#undef FIELD_OK

	return 0;
}

static int
xlio_sock_get_caps(struct spdk_sock *sock, struct spdk_sock_caps *caps)
{
	struct spdk_xlio_sock *vsock = __xlio_sock(sock);

	caps->zcopy_send = vsock->flags.zcopy;
	caps->ibv_pd = vsock->pd;
	caps->zcopy_recv = vsock->flags.recv_zcopy;

	return 0;
}

static ssize_t
xlio_sock_recv_zcopy(struct spdk_sock *_sock, size_t len, struct spdk_sock_buf **sock_buf)
{
	struct spdk_xlio_sock *sock = __xlio_sock(_sock);
	struct xlio_sock_buf *prev_buf = NULL;
	int ret;

	SPDK_DEBUGLOG(xlio, "Sock %d: zcopy recv %lu bytes\n", sock->fd, len);
	assert(sock->flags.recv_zcopy);
	*sock_buf = NULL;

	if (STAILQ_EMPTY(&sock->received_packets)) {
		if (spdk_unlikely(!_sock->group_impl)) {
			ret = poll_no_group_socket(sock);
			if (ret < 0) {
				if (sock->flags.disconnected) {
					return 0;
				}
				return ret;
			}
		} else {
			if (sock->flags.disconnected) {
				return 0;
			}
			errno = EAGAIN;
			return -1;
		}
	}

	assert(!STAILQ_EMPTY(&sock->received_packets));
	ret = 0;
	while (len > 0) {
		void *data;
		size_t chunk_len;
		struct xlio_sock_buf *buf;
		struct xlio_sock_packet *packet;

		chunk_len = packets_next_chunk(sock, &data, &packet, len);
		if (chunk_len == 0) {
			/* No more data */
			break;
		}

		assert(chunk_len <= len);
		buf = spdk_mempool_get(g_xlio_buffers_pool);
		if (spdk_unlikely(!buf)) {
			SPDK_DEBUGLOG(xlio, "Sock %d: no more buffers, total_len %d\n", sock->fd, ret);
			if (spdk_unlikely(_sock->group_impl && !sock->flags.pending_recv)) {
				struct spdk_xlio_sock_group_impl *group =
					__xlio_group_impl(sock->base.group_impl);
				sock->flags.pending_recv = true;
				SPDK_DEBUGLOG(xlio, "Sock %d, insert to pending_recv\n", sock->fd);
				TAILQ_INSERT_TAIL(&group->pending_recv, sock, link);
			}
			if (ret == 0) {
				ret = -1;
				errno = EAGAIN;
			}
			break;
		}

		buf->sock_buf.iov.iov_base = data;
		buf->sock_buf.iov.iov_len = chunk_len;
		buf->sock_buf.next = NULL;
		buf->packet = packet;
		packet->refs++;
		if (prev_buf) {
			prev_buf->sock_buf.next = &buf->sock_buf;
		} else {
			*sock_buf = &buf->sock_buf;
		}

		packets_advance(sock, chunk_len);
		len -= chunk_len;
		ret += chunk_len;
		prev_buf = buf;
		SPDK_DEBUGLOG(xlio, "Sock %d: add buffer %p, len %lu, total_len %d\n",
			      sock->fd, buf, buf->sock_buf.iov.iov_len, ret);
	}

	SPDK_DEBUGLOG(xlio, "Sock %d: recv_zcopy ret %d\n", sock->fd, ret);
	return ret;
}

static int
xlio_sock_free_bufs(struct spdk_sock *_sock, struct spdk_sock_buf *sock_buf)
{
	struct spdk_xlio_sock *sock = __xlio_sock(_sock);

	while (sock_buf) {
		struct xlio_sock_buf *buf = SPDK_CONTAINEROF(sock_buf,
					    struct xlio_sock_buf,
					    sock_buf);
		struct xlio_sock_packet *packet = buf->packet;
		struct spdk_sock_buf *next = buf->sock_buf.next;

		spdk_mempool_put(g_xlio_buffers_pool, buf);
		if (--packet->refs == 0) {
			xlio_sock_free_packet(sock, packet);
		}

		sock_buf = next;
	}

	return 0;
}

static struct spdk_sock_group_impl *
xlio_sock_group_impl_get_optimal(struct spdk_sock *_sock, struct spdk_sock_group_impl *hint)
{
	return NULL;
}

static void
xlop_sock_deinit(void)
{
	xlio_sock_free_pools();
}

static struct spdk_net_impl g_xlio_net_impl = {
	.name		= "xlio",
	.getaddr	= xlio_sock_getaddr,
	.connect	= xlio_sock_connect,
	.listen		= xlio_sock_listen,
	.accept		= xlio_sock_accept,
	.close		= xlio_sock_close,
	.recv		= xlio_sock_recv,
	.readv		= xlio_sock_readv,
	.writev		= xlio_sock_writev,
	.writev_async	= xlio_sock_writev_async,
	.flush		= xlio_sock_flush,
	.set_recvlowat	= xlio_sock_set_recvlowat,
	.set_recvbuf	= xlio_sock_set_recvbuf,
	.set_sendbuf	= xlio_sock_set_sendbuf,
	.is_ipv6	= xlio_sock_is_ipv6,
	.is_ipv4	= xlio_sock_is_ipv4,
	.is_connected	= xlio_sock_is_connected,
	.group_impl_get_optimal	= xlio_sock_group_impl_get_optimal,
	.group_impl_create	= xlio_sock_group_impl_create,
	.group_impl_add_sock	= xlio_sock_group_impl_add_sock,
	.group_impl_remove_sock = xlio_sock_group_impl_remove_sock,
	.group_impl_poll	= xlio_sock_group_impl_poll,
	.group_impl_close	= xlio_sock_group_impl_close,
	.get_opts	= xlio_sock_impl_get_opts,
	.set_opts	= xlio_sock_impl_set_opts,
	.get_caps	= xlio_sock_get_caps,
	.recv_zcopy	= xlio_sock_recv_zcopy,
	.free_bufs	= xlio_sock_free_bufs,
	.deinit		= xlop_sock_deinit,
};

SPDK_NET_IMPL_REGISTER(xlio, &g_xlio_net_impl);
SPDK_LOG_REGISTER_COMPONENT(xlio)
