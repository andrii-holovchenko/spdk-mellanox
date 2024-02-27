/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk_internal/xlio.h"
#include <dlfcn.h>
#include "spdk/log.h"

#define DEFAULT_XLIO_PATH "libxlio.so"

enum {
	IOCTL_USER_ALLOC_TX = (1 << 0),
	IOCTL_USER_ALLOC_RX = (1 << 1),
	IOCTL_USER_ALLOC_TX_ZC = (1 << 2)
};

static bool g_initialized;
#ifndef SPDK_CONFIG_STATIC_XLIO
struct spdk_sock_xlio_ops g_xlio_ops;
struct xlio_api_t *g_xlio_api;
static void *g_xlio_handle;
#endif

#ifndef SPDK_CONFIG_STATIC_XLIO
static int
xlio_load(void)
{
	char *xlio_path;

	xlio_path = getenv("SPDK_XLIO_PATH");
	if (!xlio_path) {
		printf("SPDK_XLIO_PATH is not defined. XLIO socket implementation is disabled.\n");
		return -1;
	} else if (strnlen(xlio_path, 1) == 0) {
		xlio_path = NULL;
		printf("SPDK_XLIO_PATH is defined but empty. Using default: %s\n",
		       DEFAULT_XLIO_PATH);
	}

	g_xlio_handle = dlopen(xlio_path ? xlio_path : DEFAULT_XLIO_PATH, RTLD_NOW);
	if (!g_xlio_handle) {
		SPDK_ERRLOG("Failed to load XLIO library: path %s, error %s\n",
			    xlio_path ? xlio_path : DEFAULT_XLIO_PATH, dlerror());
		return -1;
	}

#define GET_SYM(sym) \
	g_xlio_ops.sym = dlsym(g_xlio_handle, #sym); \
	if (!g_xlio_ops.sym) { \
		SPDK_ERRLOG("Failed to find symbol '%s'in XLIO library\n", #sym); \
		dlclose(g_xlio_handle); \
		g_xlio_handle = NULL; \
		return -1; \
	}

	GET_SYM(socket);
	GET_SYM(bind);
	GET_SYM(listen);
	GET_SYM(connect);
	GET_SYM(accept);
	GET_SYM(close);
	GET_SYM(readv);
	GET_SYM(writev);
	GET_SYM(recv);
	GET_SYM(recvmsg);
	GET_SYM(sendmsg);
	GET_SYM(fcntl);
	GET_SYM(ioctl);
	GET_SYM(getsockopt);
	GET_SYM(setsockopt);
	GET_SYM(getsockname);
	GET_SYM(getpeername);
	GET_SYM(getaddrinfo);
	GET_SYM(freeaddrinfo);
	GET_SYM(gai_strerror);
#undef GET_SYM
	return 0;
}

static void
xlio_unload(void)
{
	int rc;

	if (g_xlio_handle) {
		int (*xlio_exit)(void) = dlsym(g_xlio_handle, "xlio_exit");

		if (xlio_exit) {
			xlio_exit();
		}

		memset(&g_xlio_ops, 0, sizeof(g_xlio_ops));
		rc = dlclose(g_xlio_handle);
		if (rc) {
			SPDK_ERRLOG("Closing libxlio failed: rc %d %s\n",
				    rc, dlerror());
		}

		SPDK_NOTICELOG("Unloaded libxlio\n");
		g_xlio_handle = NULL;
	}
}

static struct xlio_api_t *
spdk_xlio_get_api(void)
{
	struct xlio_api_t *api_ptr = NULL;
	socklen_t len = sizeof(api_ptr);

	int err = xlio_getsockopt(-2, SOL_SOCKET, SO_XLIO_GET_API, &api_ptr, &len);
	if (err < 0) {
		return NULL;
	}

	return api_ptr;
}
#endif

static void *
spdk_xlio_alloc(size_t size)
{
	return spdk_zmalloc(size, 0, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
}

static void
spdk_xlio_free(void *buf)
{
	/* For some reason XLIO destructor is not called from dlclose
	 * but is called later after DPDK cleanup. And this leads to a
	 * crash since all DPDK structures are already freed.
	 *
	 * The check below works this around. If XLIO didn't free the
	 * memory while in dlcose(), don't try to free it. DPDK will
	 * do or has already done the cleanup.
	 */
#ifndef SPDK_CONFIG_STATIC_XLIO
	if (g_xlio_handle) {
		spdk_free(buf);
	}
#else
	spdk_free(buf);
#endif
}

int
spdk_xlio_init(void)
{
	int rc;
#pragma pack(push, 1)
	struct {
		uint8_t flags;
		void *(*alloc_func)(size_t);
		void (*free_func)(void *);
	} data;
#pragma pack(pop)
	struct cmsghdr *cmsg;
	char cbuf[CMSG_SPACE(sizeof(data))];
#ifndef SPDK_CONFIG_STATIC_XLIO
	uint64_t required_caps;
#endif

	if (g_initialized) {
		SPDK_ERRLOG("XLIO already initialized\n");
		return -1;
	}

#ifndef SPDK_CONFIG_STATIC_XLIO
	static_assert((sizeof(uint8_t) + sizeof(uintptr_t) +
		       sizeof(uintptr_t)) == sizeof(data),
		      "wrong xlio ioctl data size.");

	/* Before init, g_xlio_api must be NULL */
	assert(g_xlio_api == NULL);

	if (xlio_load() != 0) {
		return -1;
	}

	g_xlio_api = spdk_xlio_get_api();
	if (!g_xlio_api) {
		SPDK_ERRLOG("Failed to get XLIO API\n");
		return -1;
	}
	printf("Got XLIO API %p\n", g_xlio_api);

	if (g_xlio_api->magic != XLIO_MAGIC_NUMBER) {
		SPDK_ERRLOG("Unexpected XLIO API magic number: expected %" PRIx64 ", got %" PRIx64 "\n",
			    (uint64_t)XLIO_MAGIC_NUMBER, g_xlio_api->magic);
		return -1;
	}

	required_caps = XLIO_EXTRA_API_GET_SOCKET_RINGS_FDS |
			XLIO_EXTRA_API_SOCKETXTREME_POLL |
			XLIO_EXTRA_API_SOCKETXTREME_FREE_PACKETS |
			XLIO_EXTRA_API_IOCTL;
	if ((g_xlio_api->cap_mask & required_caps) != required_caps) {
		SPDK_ERRLOG("Required XLIO caps are missing: required %" PRIx64 ", got %" PRIx64 "\n",
			    required_caps, g_xlio_api->cap_mask);
		return -1;
	}
#else
	rc = xlio_init();
	if (rc) {
		SPDK_ERRLOG("xlio_init rc %d (errno=%d)\n", rc, errno);
	}
#endif

	cmsg = (struct cmsghdr *)cbuf;
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = CMSG_XLIO_IOCTL_USER_ALLOC;
	cmsg->cmsg_len = CMSG_LEN(sizeof(data));
	data.flags = IOCTL_USER_ALLOC_RX;
	data.alloc_func = spdk_xlio_alloc;
	data.free_func = spdk_xlio_free;
	memcpy(CMSG_DATA(cmsg), &data, sizeof(data));

	rc = xlio_extra_ioctl(cmsg, cmsg->cmsg_len);
	if (rc < 0) {
		SPDK_ERRLOG("xlio_extra_ioctl rc %d (errno=%d)\n", rc, errno);
		return -1;
	}

	g_initialized = true;
	return rc;
}

void
spdk_xlio_fini(void)
{
	if (g_initialized) {
#ifndef SPDK_CONFIG_STATIC_XLIO
		xlio_unload();
		g_xlio_api = NULL;
#endif
		g_initialized = false;
	} else {
		SPDK_NOTICELOG("XLIO is not initialized\n");
	}
}
