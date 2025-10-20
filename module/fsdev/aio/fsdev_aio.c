/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2023-2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */
#include "spdk/stdinc.h"
#include "spdk/event.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/config.h"
#include "spdk/util.h"
#include "spdk/thread.h"
#include "spdk/likely.h"
#include "spdk/lut.h"

#include "fsdev_aio.h"
#include <libaio.h>
#ifdef SPDK_CONFIG_URING
#include <liburing.h>
#endif
#include <sys/ioctl.h>
#include <linux/fs.h>

#define FSDEV_AIO_FUSE_KERNEL_MINOR_VERSION 34

#define SPDK_URING_QUEUE_DEPTH 512

#define FILE_PTR_LUT_INIT_SIZE 1000
#define FILE_PTR_LUT_BITS 63
#define FILE_PTR_LUT_BASE (((uint64_t)1) << FILE_PTR_LUT_BITS)
#define FILE_PTR_LUT_MAX_SIZE (1024 * 1024 * 1024) /* 1 billion files maximum */
#define FILE_PTR_LUT_GROWTH_STEP 1000

#define IO_STATUS_ASYNC INT_MIN

#ifndef UNUSED
#define UNUSED(x) (void)(x)
#endif

/* See https://libfuse.github.io/doxygen/structfuse__conn__info.html */
#define MAX_BACKGROUND (100)
#define TIME_GRAN (1)
#define DEFAULT_WRITEBACK_CACHE false
#define DEFAULT_MAX_XFER_SIZE 0x00020000
#define MAXIMUM_MAX_XFER_SIZE 0x00020000
#define DEFAULT_MAX_READAHEAD 0x00020000
#define DEFAULT_XATTR_ENABLED false
#define DEFAULT_SKIP_RW false
#define DEFAULT_ATTR_VALID_MS 0 /* to prevent the attribute caching */
#define DEFAULT_NOTIFY_MAX_DATA_SIZE 4096
#define DEFAULT_ENABLE_NOTIFICATIONS false
#define FANOTIFY_POLLER_PERIOD_US 1000
#ifdef SPDK_CONFIG_COPY_FILE_RANGE
#define DEFAULT_DISABLE_COPY_FILE_RANGE false
#else
#define DEFAULT_DISABLE_COPY_FILE_RANGE true
#endif

#ifdef SPDK_CONFIG_HAVE_STRUCT_STAT_ST_ATIM
/* Linux */
#define ST_ATIM_NSEC(stbuf) ((stbuf)->st_atim.tv_nsec)
#define ST_CTIM_NSEC(stbuf) ((stbuf)->st_ctim.tv_nsec)
#define ST_MTIM_NSEC(stbuf) ((stbuf)->st_mtim.tv_nsec)
#define ST_ATIM_NSEC_SET(stbuf, val) (stbuf)->st_atim.tv_nsec = (val)
#define ST_CTIM_NSEC_SET(stbuf, val) (stbuf)->st_ctim.tv_nsec = (val)
#define ST_MTIM_NSEC_SET(stbuf, val) (stbuf)->st_mtim.tv_nsec = (val)
#elif defined(SPDK_CONFIG_HAVE_STRUCT_STAT_ST_ATIMESPEC)
/* FreeBSD */
#define ST_ATIM_NSEC(stbuf) ((stbuf)->st_atimespec.tv_nsec)
#define ST_CTIM_NSEC(stbuf) ((stbuf)->st_ctimespec.tv_nsec)
#define ST_MTIM_NSEC(stbuf) ((stbuf)->st_mtimespec.tv_nsec)
#define ST_ATIM_NSEC_SET(stbuf, val) (stbuf)->st_atimespec.tv_nsec = (val)
#define ST_CTIM_NSEC_SET(stbuf, val) (stbuf)->st_ctimespec.tv_nsec = (val)
#define ST_MTIM_NSEC_SET(stbuf, val) (stbuf)->st_mtimespec.tv_nsec = (val)
#else
#define ST_ATIM_NSEC(stbuf) 0
#define ST_CTIM_NSEC(stbuf) 0
#define ST_MTIM_NSEC(stbuf) 0
#define ST_ATIM_NSEC_SET(stbuf, val) do { } while (0)
#define ST_CTIM_NSEC_SET(stbuf, val) do { } while (0)
#define ST_MTIM_NSEC_SET(stbuf, val) do { } while (0)
#endif

#define FANOTIFY_MASK (FAN_ATTRIB | FAN_ONDIR | FAN_EVENT_ON_CHILD)

#define MAX_GETXATTR_BUF_SIZE 512 /* TODO: what is the max size? */
#define MAX_SETXATTR_BUF_SIZE 1024 /* TODO: what is the max size? */

/*
 * Example of traditional IOCTL variant of the data that can be
 * get or set by AIO ioctl() implementation, when the structure size
 * is well known in advance.
 */
struct aio_ioctl_data {
	uint32_t width;
	uint32_t height;
};

/*
 * Reading data.
 *
 * The meaning of values:
 * - 'E' - means example.
 * - 44  - cmd number.
 * - data type for the output data (root structure).
 */
#define AIO_IOCTL_GET_DATA_CMD _IOR('E', 44, struct aio_ioctl_data)

/*
 * Setting data.
 *
 * The meaning of values:
 * - 'E' - means example.
 * - 45  - cmd number.
 * - data type for the output data (root structure).
 */
#define AIO_IOCTL_SET_DATA_CMD _IOW('E', 45, struct aio_ioctl_data)

/*
 * Setting and getting data in one blow. The input buffer must be used for poulating
 * internal module data. The output - for returning the old value (before changing).
 *
 * The meaning of values:
 * - 'E' - means example.
 * - 46  - cmd number.
 * - size of the output data (root structure) is sizeof(struct aio_ioctl_unrest)
 */
#define AIO_IOCTL_DATA_CMD _IOWR('E', 46, struct aio_ioctl_data)

/*
 * No data exchange (action) command. Input and output buffers are zero.
 */
#define AIO_IOCTL_ACT_CMD _IO('E', 47)

static struct fsdev_aio_module_opts g_opts = {
	.enable_io_uring = false,
	.max_io_depth = 256,
};

#ifdef SPDK_CONFIG_URING
static int g_io_uring_supported_ops[IORING_OP_LAST] = {0};

static void
fsdev_init_supported_uring_ops(void)
{
	size_t i = 0;
	struct io_uring_probe *probe;

	probe = io_uring_get_probe();
	if (!probe) {
		SPDK_WARNLOG("Failed to get io_uring probe\n");
		return;
	}

	for (i = 0; i < SPDK_COUNTOF(g_io_uring_supported_ops); i++) {
		g_io_uring_supported_ops[i] = io_uring_opcode_supported(probe, i);
	}

	assert(g_io_uring_supported_ops[IORING_OP_WRITEV] == g_io_uring_supported_ops[IORING_OP_READV]);
}

static inline bool
aio_fsdev_use_io_uring_rdwr(void)
{
	return g_opts.enable_io_uring &&
	       g_io_uring_supported_ops[IORING_OP_READV] && g_io_uring_supported_ops[IORING_OP_WRITEV];
}
#else
#define aio_fsdev_use_io_uring_rdwr() false
#endif

struct fsdev_aio_cred {
	uid_t euid;
	gid_t egid;
};

/** Inode number type */
typedef uint64_t spdk_ino_t;

struct fsdev_aio_key {
	ino_t ino;
	dev_t dev;
	RB_ENTRY(fsdev_aio_key) link;
};

struct aio_fsdev_fhdr {
	uint64_t is_fobject : 1;
	uint64_t lut_key : 63;
	uint64_t refcount;
};

/* The sizeof(struct aio_fsdev_fhdr) a multiple of sizeof(uint64_t) */
SPDK_STATIC_ASSERT(sizeof(struct aio_fsdev_fhdr) == 16, "Incorrect size");

struct aio_fsdev_file_handle {
	struct aio_fsdev_fhdr hdr;
	int fd;
	struct {
		DIR *dp;
		struct dirent *entry;
		off_t offset;
	} dir;
	struct aio_fsdev *vfsdev;
	uint64_t fobject_lut_key;
	TAILQ_ENTRY(aio_fsdev_file_handle) link;
};

struct aio_fsdev;

struct aio_fsdev_linux_fh {
	const struct file_handle *fh;
	RB_ENTRY(aio_fsdev_linux_fh) node;
};

static int
aio_fsdev_linux_fh_cmp(struct aio_fsdev_linux_fh *fh1, struct aio_fsdev_linux_fh *fh2)
{
	return memcmp(fh1->fh, fh2->fh, sizeof(*fh1->fh) + fh1->fh->handle_bytes);
}

RB_HEAD(aio_fsdev_linux_fh_tree, aio_fsdev_linux_fh);
RB_GENERATE_STATIC(aio_fsdev_linux_fh_tree, aio_fsdev_linux_fh, node, aio_fsdev_linux_fh_cmp);

#define FOBJECT_FMT "fobj=%p (lut=0x%" PRIx64 " ino=%" PRIu64 " dev=%" PRIu64 ")"
#define FOBJECT_ARGS(fo) (fo), ((uint64_t)(fo)->hdr.lut_key), ((uint64_t)(fo)->key.ino), ((uint64_t)(fo)->key.dev)
struct aio_fsdev_file_object {
	struct aio_fsdev_fhdr hdr;
	mode_t mode;
	int fd;
	char *fd_str;
	struct fsdev_aio_key key;
	union {
		struct file_handle linux_fh;
		char fh_buf[sizeof(struct file_handle) + MAX_HANDLE_SZ];
	};
	struct aio_fsdev_linux_fh linux_fh_entry;
	struct aio_fsdev_file_object *parent_fobject;
	RB_HEAD(aio_fsdev_file_object_tree, fsdev_aio_key) leafs;
	TAILQ_HEAD(, aio_fsdev_file_handle) handles;
	struct aio_fsdev *vfsdev;
};

static inline bool
fsdev_aio_fobject_is_symlink(struct aio_fsdev_file_object *fobject)
{
	return S_ISLNK(fobject->mode);
}

static inline bool
fsdev_aio_fobject_is_dir(struct aio_fsdev_file_object *fobject)
{
	return S_ISDIR(fobject->mode);
}

static int
aio_fsdev_file_object_cmp(struct fsdev_aio_key *fo1, struct fsdev_aio_key *fo2)
{
	if (fo1->dev != fo2->dev) {
		assert(false);
		return -1;
	}

	if (fo1->ino < fo2->ino) {
		return -1;
	} else if (fo1->ino > fo2->ino) {
		return 1;
	} else {
		return 0;
	}
}

RB_GENERATE_STATIC(aio_fsdev_file_object_tree, fsdev_aio_key, link, aio_fsdev_file_object_cmp);

struct aio_fsdev {
	struct spdk_fsdev fsdev;
	struct spdk_fsdev_mount_opts mount_opts;
	struct spdk_fsdev_aio_opts opts;
	char *root_path;
	int proc_self_fd;
	struct aio_fsdev_file_object *root;
	TAILQ_ENTRY(aio_fsdev) tailq;
	struct spdk_lut *lut;
	struct spdk_spinlock lock;
#ifdef SPDK_CONFIG_HAVE_FANOTIFY
	int fanotify_fd;
	struct spdk_poller *fanotify_poller;
	pid_t pid;
	struct aio_fsdev_linux_fh_tree linux_fhs;
#endif
};

struct aio_fsdev_io;

struct aio_fsdev_io {
	struct iocb io;
	uint32_t data_size;
	int status;
	TAILQ_ENTRY(aio_fsdev_io) link;
};

struct aio_io_channel {
	struct spdk_poller *poller;
	TAILQ_HEAD(, aio_fsdev_io) ios_for_submit;
	TAILQ_HEAD(, aio_fsdev_io) ios_in_progress;
	TAILQ_HEAD(, aio_fsdev_io) ios_to_complete;
	union {
		io_context_t io_ctx;
#ifdef SPDK_CONFIG_URING
		struct {
			/* NOTE: once get_sqe fails for the 1st time, we must queue all the following IOs to
			 * preserve the order until the queue is empty
			 */
			uint32_t queue_ios: 1;
			uint32_t reserved: 31;
			uint32_t io_count;
			struct io_uring io_ring;
		} uring;
#endif
	} u;
};

struct aio_fsdev_notify_request {
	struct spdk_fuse_notify_request		base;
	struct iovec				iov;
	struct fuse_out_header			out;
	struct fuse_notify_inval_entry_out	entry;
	char data[];
};

static TAILQ_HEAD(, aio_fsdev) g_aio_fsdev_head = TAILQ_HEAD_INITIALIZER(
			g_aio_fsdev_head);


static inline struct aio_fsdev *
fsdev_to_aio_fsdev(struct spdk_fsdev *fsdev)
{
	return SPDK_CONTAINEROF(fsdev, struct aio_fsdev, fsdev);
}

static inline struct spdk_fsdev_io *
aio_to_fsdev_io(const struct aio_fsdev_io *aio_io)
{
	return SPDK_CONTAINEROF(aio_io, struct spdk_fsdev_io, driver_ctx);
}

static inline struct aio_fsdev_io *
fsdev_to_aio_io(const struct spdk_fsdev_io *fsdev_io)
{
	return (struct aio_fsdev_io *)fsdev_io->driver_ctx;
}

static const char *
fsdev_aio_io_fuse_get_name(struct spdk_fsdev_io *fsdev_io)
{
	assert(spdk_fsdev_io_get_type(fsdev_io) == SPDK_FSDEV_IO_FUSE);
	assert(fsdev_io->u_in.fuse.iov[0].iov_base);

	/* The name will always be at the start of the first in iov.
	 * For commands with 2 names (i.e. RENAME), the caller is
	 * responsible for finding that 2nd name, this function only
	 * takes care of the first one.
	 */
	return fsdev_io->u_in.fuse.iov[0].iov_base;
}

static int clear_suid_sgid(struct aio_fsdev_io *vfsdev_io);

static void
fsdev_aio_cb(struct aio_fsdev_io *aio, long res, long res2)
{
	struct spdk_fsdev_io *fsdev_io = aio_to_fsdev_io(aio);
	struct spdk_io_channel *ioch = spdk_fsdev_io_get_io_channel(fsdev_io);
	struct aio_io_channel *aioch = spdk_io_channel_get_ctx(ioch);

	TAILQ_REMOVE(&aioch->ios_in_progress, aio, link);

	if (res >= 0) {
		struct fuse_in_header *in_hdr = fsdev_io->u_in.fuse.hdr;
		struct fuse_out_header *out_hdr = fsdev_io->u_out.fuse.hdr;

		assert(spdk_fsdev_io_get_type(fsdev_io) == SPDK_FSDEV_IO_FUSE);

		switch (in_hdr->opcode) {
		case FUSE_READ:
			out_hdr->len += res;
			break;
		case FUSE_WRITE:
			out_hdr->len += sizeof(struct fuse_write_out);
			fsdev_io->u_out.fuse.op.write->size = res;

			/* Even though we don't report support for handling this flag, the kernel
			 * will send us on files opened with O_DIRECT, so we still handle it.
			 */
			if (fsdev_io->u_in.fuse.op.write->flags & FUSE_WRITE_KILL_SUIDGID) {
				/* We do not check the return value because
				* failure is not fatal. */
				clear_suid_sgid(aio);
			}
			break;
		default:
			break;
		}
		aio->status = 0;
	} else {
		SPDK_ERRLOG("aio operation failed: %ld\n", res);
		aio->status = res;
	}

	TAILQ_INSERT_TAIL(&aioch->ios_to_complete, aio, link);
}

#define fsdev_io_get_aio_fobject(fsdev_io) \
	fsdev_aio_get_fobject_by_nodeid(fsdev_to_aio_fsdev(fsdev_io->fsdev), fsdev_io->u_in.fuse.hdr->nodeid)

static inline struct aio_fsdev_file_object *
fsdev_aio_get_fobject_by_nodeid(struct aio_fsdev *vfsdev, uint64_t nodeid)
{
	struct aio_fsdev_file_object *fobject;

	if (nodeid == FUSE_ROOT_ID) {
		nodeid = FILE_PTR_LUT_BASE;
	}

	if (nodeid < FILE_PTR_LUT_BASE) {
		SPDK_WARNLOG("0x%" PRIx64 " is not a valid fobject (< 0x%" PRIx64 ")\n", nodeid, FILE_PTR_LUT_BASE);
		return NULL;
	}

	spdk_spin_lock(&vfsdev->lock);
	fobject = spdk_lut_get(vfsdev->lut, nodeid - FILE_PTR_LUT_BASE);
	if (fobject == SPDK_LUT_INVALID_VALUE) {
		spdk_spin_unlock(&vfsdev->lock);
		SPDK_WARNLOG("0x%" PRIx64 " is not a valid fobject\n", nodeid);
		return NULL;
	}

	assert(fobject); /* There shouldn't be NULL fobject in the LUT */

	if (spdk_likely(fobject->hdr.is_fobject)) {
		__atomic_add_fetch(&fobject->hdr.refcount, 1, __ATOMIC_RELAXED); /* ref by caller */
	} else {
		/* Error: the key rather belongs to a fhandle */
		SPDK_WARNLOG("0x%" PRIx64 " is not a fobject\n", nodeid);
		fobject = NULL;
	}
	spdk_spin_unlock(&vfsdev->lock);

	return fobject;
}

static inline uint64_t
fsdev_aio_fobject_to_nodeid(struct aio_fsdev *vfsdev, struct aio_fsdev_file_object *fobject)
{
	assert(fobject);

	if (fobject == vfsdev->root) {
		return FUSE_ROOT_ID;
	}

	return fobject->hdr.lut_key + FILE_PTR_LUT_BASE;
}

static inline struct spdk_fsdev_file_object *
fsdev_aio_get_spdk_fobject(struct aio_fsdev *vfsdev, struct aio_fsdev_file_object *fobject)
{
	return (struct spdk_fsdev_file_object *)(uintptr_t)fsdev_aio_fobject_to_nodeid(vfsdev, fobject);
}

/* The returned fhandle pointer is only valid while the stack frame this pointer is returned to
 * remains valid. You cannot store tihs pointer to be used later. Instead, look it back up again. */
static inline struct aio_fsdev_file_handle *
fsdev_aio_get_fhandle_by_fuse_fh(struct aio_fsdev *vfsdev, uint64_t fh)
{
	struct aio_fsdev_file_handle *fhandle;

	if (fh < FILE_PTR_LUT_BASE) {
		SPDK_WARNLOG("0x%" PRIx64 " is not a valid fhandle (< 0x%" PRIx64 ")\n", fh, FILE_PTR_LUT_BASE);
		return NULL;
	}

	fhandle = spdk_lut_get(vfsdev->lut, fh - FILE_PTR_LUT_BASE);
	if (fhandle == SPDK_LUT_INVALID_VALUE) {
		SPDK_WARNLOG("0x%" PRIx64 " is not a valid fhandle\n", fh);
		return NULL;
	}

	assert(fhandle); /* There shouldn't be NULL fhandle in the LUT */

	if (!fhandle->hdr.is_fobject) {
		/* fhandles either have a refcount of 0 or 1. They do not take extra references. */
		assert(fhandle->hdr.refcount <= 1);
	} else {
		/* Error: the key rather belongs to a fobject */
		SPDK_WARNLOG("0x%" PRIx64 " is not a fhandle\n", fh);
		fhandle = NULL;
	}

	return fhandle;
}

static inline uint64_t
fsdev_aio_get_fuse_fh(struct aio_fsdev *vfsdev, struct aio_fsdev_file_handle *fhandle)
{
	return fhandle->hdr.lut_key + FILE_PTR_LUT_BASE;
}

static inline void
fsdev_aio_io_complete(struct spdk_fsdev_io *fsdev_io, int status)
{
	struct fuse_out_header *out_hdr = fsdev_io->u_out.fuse.hdr;

	assert(spdk_fsdev_io_get_type(fsdev_io) == SPDK_FSDEV_IO_FUSE);

	if (out_hdr != NULL) {
		out_hdr->error = status;
	}

	/* Complete the IO */
	spdk_fsdev_io_complete(fsdev_io, status);
}

static int
is_dot_or_dotdot(const char *name)
{
	return name[0] == '.' && (name[1] == '\0' ||
				  (name[1] == '.' && name[2] == '\0'));
}

/* Is `path` a single path component that is not "." or ".."? */
static int
is_safe_path_component(const char *path)
{
	if (strchr(path, '/')) {
		return 0;
	}

	return !is_dot_or_dotdot(path);
}

static struct aio_fsdev_file_object *
find_leaf_unsafe(struct aio_fsdev_file_object *fobject, ino_t ino, dev_t dev)
{
	struct fsdev_aio_key tmp, *key;

	tmp.ino = ino;
	tmp.dev = dev;
	key = RB_FIND(aio_fsdev_file_object_tree, &fobject->leafs, &tmp);

	if (key != NULL) {
		return SPDK_CONTAINEROF(key, struct aio_fsdev_file_object, key);
	}

	return NULL;
}

#ifdef SPDK_CONFIG_HAVE_FANOTIFY
static int
fsdev_aio_fanotify_add(struct aio_fsdev_file_object *fobject, int parent_fd, const char *name)
{
	struct aio_fsdev *vfsdev = fobject->vfsdev;
	int mount_id;
	int rc;

	fobject->linux_fh.handle_bytes = MAX_HANDLE_SZ;
	rc = name_to_handle_at(parent_fd, name, &fobject->linux_fh, &mount_id, 0);
	if (rc) {
		SPDK_ERRLOG("Failed to get file handle: errno %d, parent fd %d, name %s\n",
			    errno, parent_fd, name);
		return rc;
	}

	rc = fanotify_mark(vfsdev->fanotify_fd, FAN_MARK_ADD | FAN_MARK_ONLYDIR, FANOTIFY_MASK,
			   parent_fd, name);
	if (rc) {
		SPDK_ERRLOG("Failed to add fobject to fanotify: errno %d, fd %d, "
			    "parent fd %d, name %s\n",
			    errno, fobject->fd, parent_fd, name);
		return rc;
	}

	fobject->linux_fh_entry.fh = &fobject->linux_fh;
	RB_INSERT(aio_fsdev_linux_fh_tree, &vfsdev->linux_fhs, &fobject->linux_fh_entry);

	SPDK_DEBUGLOG(fsdev_aio, "Added fobject to fanotify: fd %d, name %s\n",
		      fobject->fd, name);

	return 0;
}

static void
fsdev_aio_fanotify_remove(struct aio_fsdev_file_object *fobject)
{
	struct aio_fsdev *vfsdev = fobject->vfsdev;
	const char *name;
	int fd;
	int rc;

	if (fobject == vfsdev->root) {
		fd = AT_FDCWD;
		name = vfsdev->root_path;
	} else {
		fd = fobject->fd;
		name = ".";
	}

	rc = fanotify_mark(vfsdev->fanotify_fd, FAN_MARK_REMOVE | FAN_MARK_ONLYDIR, FANOTIFY_MASK,
			   fd, name);
	if (rc) {
		SPDK_ERRLOG("Failed to remove fobject from fanotify: errno %d, fd %d, name %s\n",
			    errno, fd, name);
	} else {
		SPDK_DEBUGLOG(fsdev_aio, "Removed fobject from fanotify: fd %d, name %s\n", fd, name);
	}

	spdk_spin_lock(&vfsdev->lock);
	RB_REMOVE(aio_fsdev_linux_fh_tree, &vfsdev->linux_fhs, &fobject->linux_fh_entry);
	spdk_spin_unlock(&vfsdev->lock);
}
#endif

static void
file_object_destroy(struct aio_fsdev_file_object *fobject)
{
	assert(!fobject->hdr.refcount);

#ifdef SPDK_CONFIG_HAVE_FANOTIFY
	/* root is handled on umount */
	if (fobject->vfsdev->fanotify_fd != -1 &&
	    fsdev_aio_fobject_is_dir(fobject) && fobject->parent_fobject) {
		fsdev_aio_fanotify_remove(fobject);
	}
#endif

	close(fobject->fd);
	free(fobject->fd_str);
	free(fobject);
}

/* This function returns the result reference counter */
static uint64_t
file_object_unref(struct aio_fsdev_file_object *fobject, uint32_t count)
{
	struct aio_fsdev *vfsdev = fobject->vfsdev;
	struct aio_fsdev_file_object *parent_fobject = fobject->parent_fobject;
	uint64_t refcount;

	assert(fobject->hdr.refcount >= count);

	/* IMPORTANT NOTE:
	 * We want keep this function as lightweight and lockless as possible, so we decrease the reference counter
	 * before we take the lock and destroy the object. This is fine in the wast majority of cases, as usually
	 * file operations are performed on a fobject while it's being referenced by the app.
	 * However, there's a race here in cases when the last reference is being removed. The fobject can be
	 * obtained by fsdev_aio_do_lookup after the reference counter has been decreased and checked and before we take
	 * the lock to remove the fobject from its parent's leafs list.
	 * Thus we have to check the value of the reference counter once again to avoid deleting the fobject while
	 * it's in use.
	 */
	refcount = __atomic_sub_fetch(&fobject->hdr.refcount, count, __ATOMIC_RELAXED);
	if (refcount) {
		SPDK_DEBUGLOG(fsdev_aio, "%p urefed (cnt=%" PRIu32 " refcnt=%" PRIu64 ")\n",
			      fobject, count, refcount);
		return refcount;
	}

	if (spdk_unlikely(!parent_fobject)) {
		assert(fobject == fobject->vfsdev->root);

		spdk_spin_lock(&vfsdev->lock);
		refcount = __atomic_load_n(&fobject->hdr.refcount, __ATOMIC_RELAXED);
		if (!refcount) {
			spdk_lut_remove(fobject->vfsdev->lut, fobject->hdr.lut_key);
		}
		spdk_spin_unlock(&vfsdev->lock);

		if (!refcount) {
			SPDK_DEBUGLOG(fsdev_aio, "root fobject removed %p\n", fobject);
			file_object_destroy(fobject);
		}
		return 0;
	}

	spdk_spin_lock(&vfsdev->lock);

	refcount = __atomic_load_n(&fobject->hdr.refcount, __ATOMIC_RELAXED);
	if (!refcount) {
		spdk_lut_remove(fobject->vfsdev->lut, fobject->hdr.lut_key);
		RB_REMOVE(aio_fsdev_file_object_tree, &parent_fobject->leafs, &fobject->key);
	}

	spdk_spin_unlock(&vfsdev->lock);

	if (refcount) {
		return refcount;
	}

	SPDK_DEBUGLOG(fsdev_aio, "%p finally urefed (cnt=%" PRIu32 ")\n",
		      fobject, count);

	file_object_destroy(fobject);

	file_object_unref(parent_fobject, 1); /* unref by the leaf */

	return 0;
}

static inline void
file_object_ref(struct aio_fsdev_file_object *fobject)
{
	/* The fobject is referenced by a caller, so it's' safe just increase the ref count */
	__atomic_add_fetch(&fobject->hdr.refcount, 1, __ATOMIC_RELAXED);
}

static struct aio_fsdev_file_object *
file_object_create_unsafe(struct aio_fsdev *vfsdev, struct aio_fsdev_file_object *parent_fobject,
			  int fd, ino_t ino, dev_t dev, mode_t mode, const char *name)
{
	struct aio_fsdev_file_object *fobject;
	uint64_t lut_key = SPDK_LUT_INVALID_KEY;

	fobject = calloc(1, sizeof(*fobject));
	if (!fobject) {
		SPDK_ERRLOG("Cannot alloc fobject\n");
		return NULL;
	}

	fobject->fd_str = spdk_sprintf_alloc("%d", fd);
	if (!fobject->fd_str) {
		SPDK_ERRLOG("Cannot alloc fd_str\n");
		goto err;
	}

	lut_key = spdk_lut_insert(vfsdev->lut, fobject);
	if (lut_key == SPDK_LUT_INVALID_KEY) {
		SPDK_ERRLOG("Cannot insert fobject into lookup table\n");
		goto err;
	}

	fobject->hdr.is_fobject = true;
	fobject->hdr.lut_key = lut_key;
	fobject->hdr.refcount = 1; /* ref by caller */

	fobject->fd = fd;
	fobject->key.ino = ino;
	fobject->key.dev = dev;
	fobject->vfsdev = vfsdev;
	fobject->mode = mode;

	TAILQ_INIT(&fobject->handles);
	RB_INIT(&fobject->leafs);

#ifdef SPDK_CONFIG_HAVE_FANOTIFY
	/* Root is marked on mount */
	if (vfsdev->fanotify_fd != -1 && fsdev_aio_fobject_is_dir(fobject) && parent_fobject) {
		int rc = fsdev_aio_fanotify_add(fobject, parent_fobject->fd, name);
		if (rc) {
			goto err;
		}
	}
#endif

	if (parent_fobject) {
		fobject->parent_fobject = parent_fobject;
		RB_INSERT(aio_fsdev_file_object_tree, &parent_fobject->leafs, &fobject->key);
		file_object_ref(parent_fobject); /* ref by leaf */
	}

	SPDK_DEBUGLOG(fsdev_aio, "fobject created %p (lut=0x%" PRIx64 ")\n", fobject,
		      (uint64_t)fobject->hdr.lut_key);

	return fobject;

err:
	if (lut_key != SPDK_LUT_INVALID_KEY) {
		spdk_lut_remove(vfsdev->lut, lut_key);
	}

	free(fobject->fd_str);
	free(fobject);
	return NULL;
}

/* Allocate and prepare a new fhandle. This does not commit it to the table, so it is not
 * yet visible to other operations. */
static struct aio_fsdev_file_handle *
file_handle_alloc(struct aio_fsdev_file_object *fobject, int fd)
{
	struct aio_fsdev_file_handle *fhandle;
	struct aio_fsdev *vfsdev;
	uint64_t lut_key;

	assert(fobject != NULL);
	vfsdev = fobject->vfsdev;

	fhandle = calloc(1, sizeof(*fhandle));
	if (!fhandle) {
		SPDK_ERRLOG("Cannot alloc fhandle\n");
		return NULL;
	}

	fhandle->hdr.refcount = 0;
	fhandle->fobject_lut_key = fobject->hdr.lut_key;
	fhandle->vfsdev = vfsdev;
	fhandle->fd = fd;

	spdk_spin_lock(&vfsdev->lock);
	lut_key = spdk_lut_insert(vfsdev->lut, fhandle);
	if (lut_key != SPDK_LUT_INVALID_KEY) {
		fhandle->hdr.lut_key = lut_key;
		fhandle->hdr.refcount = 1;
		__atomic_add_fetch(&fobject->hdr.refcount, 1, __ATOMIC_RELAXED); /* ref by fhandle */
		TAILQ_INSERT_TAIL(&fobject->handles, fhandle, link);
	} else {
		spdk_spin_unlock(&vfsdev->lock);
		SPDK_ERRLOG("Cannot insert fhandle into lookup table\n");
		free(fhandle);
		return NULL;
	}
	spdk_spin_unlock(&vfsdev->lock);

	return fhandle;
}

/* Mark a valid file handle as invalid so future lookups will correctly fail. */
static void
file_handle_invalidate(struct aio_fsdev_file_handle *fhandle)
{
	struct aio_fsdev *vfsdev = fhandle->vfsdev;

	spdk_spin_lock(&vfsdev->lock);
	assert(fhandle->hdr.refcount == 1);
	/* A valid entry in the spdk_lut with a refcount of 0 indicates that it is invalid (but hasn't been destroyed) */
	fhandle->hdr.refcount = 0;

	/* We now immediately remove the handle from the table so future lookups do not find it. */
	spdk_lut_remove(vfsdev->lut, fhandle->hdr.lut_key);

	spdk_spin_unlock(&vfsdev->lock);
}

static void
file_handle_destroy(struct aio_fsdev_file_handle *fhandle)
{
	struct aio_fsdev_file_object *fobject = NULL;
	struct aio_fsdev *vfsdev = fhandle->vfsdev;

	assert(fhandle->hdr.refcount == 0);

	spdk_spin_lock(&vfsdev->lock);

	fobject = spdk_lut_get(vfsdev->lut, fhandle->fobject_lut_key);
	if (fobject == SPDK_LUT_INVALID_VALUE) {
		/* This handle refers to an invalid file object. This should not happen. */
		SPDK_WARNLOG("0x%" PRIx64 " is not a valid fobject\n", fhandle->fobject_lut_key);
		fobject = NULL;
	}

	assert(fobject);

	if (spdk_likely(fobject->hdr.is_fobject)) {
		file_object_ref(fobject);
	} else {
		/* Error: the key rather belongs to a fhandle */
		SPDK_WARNLOG("0x%" PRIx64 " is not a fobject\n", fhandle->fobject_lut_key);
		fobject = NULL;
	}

	assert(fobject); /* There shouldn't be NULL fobject in the LUT and neither of the error conditions above should ever hit */

	if (fobject) {
		TAILQ_REMOVE(&fobject->handles, fhandle, link);
	}

	spdk_spin_unlock(&vfsdev->lock);

	if (fobject) {
		file_object_unref(fobject, 1); /* unref the ref we took above */
		file_object_unref(fobject, 1); /* unref for the fhandle */
	}

	if (fhandle->dir.dp) {
		closedir(fhandle->dir.dp);
	} else {
		close(fhandle->fd);
	}

	free(fhandle);
}

static int
fsdev_aio_fill_attr(struct aio_fsdev_file_object *fobject, struct fuse_attr *attr)
{
	struct stat stbuf;
	int res;

	res = fstatat(fobject->fd, "", &stbuf, AT_EMPTY_PATH);
	if (res == -1) {
		res = -errno;
		SPDK_ERRLOG("fstatat() failed with %d\n", res);
		return res;
	}

	attr->ino = stbuf.st_ino;
	attr->size = stbuf.st_size;
	attr->blocks = stbuf.st_blocks;
	attr->atime = stbuf.st_atime;
	attr->mtime = stbuf.st_mtime;
	attr->ctime = stbuf.st_ctime;
	attr->atimensec = ST_ATIM_NSEC(&stbuf);
	attr->mtimensec = ST_MTIM_NSEC(&stbuf);
	attr->ctimensec = ST_CTIM_NSEC(&stbuf);
	attr->mode = stbuf.st_mode;
	attr->nlink = stbuf.st_nlink;
	attr->uid = stbuf.st_uid;
	attr->gid = stbuf.st_gid;
	attr->rdev = stbuf.st_rdev;
	attr->blksize = stbuf.st_blksize;

	return 0;
}

static inline uint64_t
fsdev_aio_attr_valid_sec(struct aio_fsdev *vfsdev)
{
	return vfsdev->opts.attr_valid_ms / SPDK_SEC_TO_MSEC;
}

static inline uint32_t
fsdev_aio_attr_valid_nsec(struct aio_fsdev *vfsdev)
{
	return (vfsdev->opts.attr_valid_ms % SPDK_SEC_TO_MSEC) * SPDK_MSEC_TO_USEC;
}

static int
fsdev_aio_fill_attr_out(struct aio_fsdev_file_object *fobject, struct fuse_attr_out *attr_out)
{
	int res;

	res = fsdev_aio_fill_attr(fobject, &attr_out->attr);
	if (res) {
		return res;
	}

	attr_out->attr_valid = fsdev_aio_attr_valid_sec(fobject->vfsdev);
	attr_out->attr_valid_nsec = fsdev_aio_attr_valid_nsec(fobject->vfsdev);

	return 0;
}

static int
fsdev_aio_fill_entry_out(struct aio_fsdev_file_object *fobject, struct fuse_entry_out *entry_out)
{
	int res;

	memset(entry_out, 0, sizeof(*entry_out));

	res = fsdev_aio_fill_attr(fobject, &entry_out->attr);
	if (res) {
		return res;
	}

	entry_out->nodeid = fsdev_aio_fobject_to_nodeid(fobject->vfsdev, fobject);
	entry_out->generation = 0;
	entry_out->entry_valid = fsdev_aio_attr_valid_sec(fobject->vfsdev);
	entry_out->entry_valid_nsec = fsdev_aio_attr_valid_nsec(fobject->vfsdev);
	entry_out->attr_valid = entry_out->entry_valid;
	entry_out->attr_valid_nsec = entry_out->entry_valid_nsec;

	return 0;
}

static int
utimensat_empty(struct aio_fsdev *vfsdev, struct aio_fsdev_file_object *fobject,
		const struct timespec *tv)
{
	int res;

	if (fsdev_aio_fobject_is_symlink(fobject)) {
		res = utimensat(fobject->fd, "", tv, AT_EMPTY_PATH);
		if (res == -1 && errno == EINVAL) {
			/* Sorry, no race free way to set times on symlink. */
			errno = EPERM;
		}
	} else {
		res = utimensat(vfsdev->proc_self_fd, fobject->fd_str, tv, 0);
	}

	return res;
}

static void
fsdev_free_leafs(struct aio_fsdev_file_object *fobject, bool unref_fobject)
{
	uint64_t refcount;
	struct fsdev_aio_key *key, *tmp;
	/* ref to make sure it's not deleted when the last reference by a handle or a leaf removed */
	file_object_ref(fobject);

	while (!TAILQ_EMPTY(&fobject->handles)) {
		struct aio_fsdev_file_handle *fhandle = TAILQ_FIRST(&fobject->handles);
		file_handle_invalidate(fhandle);
		file_handle_destroy(fhandle);
#ifdef __clang_analyzer__
		/*
		 * scan-build fails to comprehend that file_handle_destroy() removes the fhandle
		 * from the queue, so it thinks it's remained accessible and throws the "Use of
		 * memory after it is freed" error here.
		 * The loop below "teaches" the scan-build that the freed fhandle is not on the
		 * list anymore and suppresses the error in this way.
		 */
		struct aio_fsdev_file_handle *tmp;
		TAILQ_FOREACH(tmp, &fobject->handles, link) {
			assert(tmp != fhandle);
		}
#endif
	}

	RB_FOREACH_SAFE(key, aio_fsdev_file_object_tree, &fobject->leafs, tmp) {
		struct aio_fsdev_file_object *leaf_fobject = SPDK_CONTAINEROF(key, struct aio_fsdev_file_object,
				key);
		/* We free (unref) the fobject's leafs in any case as the unref_fobject is only related to the fobject */
		fsdev_free_leafs(leaf_fobject, true);
	}

	refcount = file_object_unref(fobject, 1); /* a ref that we took at the beginning of this function */
	if (refcount && unref_fobject) {
		/* if still referenced - unref by refcount */
		refcount = file_object_unref(fobject, refcount);
		assert(refcount == 0);
		UNUSED(refcount);
	}
}

static int
fsdev_aio_op_getattr(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev_file_object *fobject;
	struct fuse_out_header *out_hdr = fsdev_io->u_out.fuse.hdr;
	struct fuse_attr_out *attr_out = fsdev_io->u_out.fuse.op.attr;
	int res;

	fobject = fsdev_io_get_aio_fobject(fsdev_io);
	if (!fobject) {
		SPDK_ERRLOG("Invalid fobject: %p\n", fobject);
		return -EINVAL;
	}

	res = fsdev_aio_fill_attr_out(fobject, attr_out);
	if (res) {
		SPDK_ERRLOG("Cannot fill attr for " FOBJECT_FMT " (err=%d)\n", FOBJECT_ARGS(fobject), res);
		goto fop_failed;
	}

	out_hdr->len += sizeof(struct fuse_attr_out);

	SPDK_DEBUGLOG(fsdev_aio, "GETATTR succeeded for " FOBJECT_FMT "\n", FOBJECT_ARGS(fobject));

fop_failed:
	file_object_unref(fobject, 1);
	return res;
}

static int
fsdev_aio_op_opendir(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	int error = 0;
	int fd;
	DIR *dp = NULL;
	struct aio_fsdev_file_object *fobject;
	uint32_t flags = fsdev_io->u_in.fuse.op.open->flags;
	struct aio_fsdev_file_handle *fhandle = NULL;

	UNUSED(flags);

	fobject = fsdev_io_get_aio_fobject(fsdev_io);
	if (!fobject) {
		SPDK_ERRLOG("Invalid fobject: %p\n", fobject);
		return -EINVAL;
	}

	fd = openat(fobject->fd, ".", O_RDONLY);
	if (fd == -1) {
		error = -errno;
		SPDK_ERRLOG("openat failed for " FOBJECT_FMT " (err=%d)\n", FOBJECT_ARGS(fobject), error);
		goto do_return;
	}

	dp = fdopendir(fd);
	if (dp == NULL) {
		error = -errno;
		SPDK_ERRLOG("fdopendir failed for " FOBJECT_FMT " (err=%d)\n", FOBJECT_ARGS(fobject), error);
		close(fd);
		goto do_return;
	}

	fhandle = file_handle_alloc(fobject, fd);
	if (fhandle == NULL) {
		error = -ENOMEM;
		SPDK_ERRLOG("file_handle_alloc failed for " FOBJECT_FMT " (err=%d)\n", FOBJECT_ARGS(fobject),
			    error);
		closedir(dp);
		goto do_return;
	}

	/* The fd is no longer valid once fdopendir succeeds, so
	 * we just set it to -1 here. It is automatically closed
	 * by closedir in the future. */
	fhandle->fd = -1;
	fhandle->dir.dp = dp;
	fhandle->dir.offset = 0;
	fhandle->dir.entry = NULL;

	SPDK_DEBUGLOG(fsdev_aio, "OPENDIR succeeded for " FOBJECT_FMT " (fh=%p)\n",
		      FOBJECT_ARGS(fobject), fhandle);

	fsdev_io->u_out.fuse.hdr->len += sizeof(struct fuse_open_out);
	memset(fsdev_io->u_out.fuse.op.open, 0, sizeof(*fsdev_io->u_out.fuse.op.open));
	fsdev_io->u_out.fuse.op.open->fh = fsdev_aio_get_fuse_fh(vfsdev, fhandle);

do_return:
	file_object_unref(fobject, 1);
	return error;
}

struct fsdev_aio_release_ctx {
	struct aio_fsdev_file_handle *fhandle;
	struct spdk_fsdev_io *fsdev_io;
};

static void
fsdev_aio_thread_barrier(void *ctx)
{
	/* Do nothing. */
}

static void
fsdev_aio_destroy_fhandle_cpl(void *_ctx)
{
	struct fsdev_aio_release_ctx *ctx = _ctx;
	struct spdk_fsdev_io *fsdev_io = ctx->fsdev_io;
	struct aio_fsdev_file_handle *fhandle = ctx->fhandle;

	file_handle_destroy(fhandle);
	fsdev_aio_io_complete(fsdev_io, 0);

	free(ctx);
}

static int
fsdev_aio_do_release(struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	struct fsdev_aio_release_ctx *ctx;

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		SPDK_ERRLOG("Cannot allocate release context\n");
		return -ENOMEM;
	}
	ctx->fhandle = fsdev_aio_get_fhandle_by_fuse_fh(vfsdev, fsdev_io->u_in.fuse.op.release->fh);
	if (!ctx->fhandle) {
		SPDK_ERRLOG("Invalid fhandle: %p\n", ctx->fhandle);
		free(ctx);
		return -EINVAL;
	}
	ctx->fsdev_io = fsdev_io;

	file_handle_invalidate(ctx->fhandle);
	spdk_for_each_thread(fsdev_aio_thread_barrier, ctx, fsdev_aio_destroy_fhandle_cpl);

	return IO_STATUS_ASYNC;
}

static int
fsdev_aio_op_releasedir(struct spdk_io_channel *_ch, struct spdk_fsdev_io *fsdev_io)
{
	return fsdev_aio_do_release(fsdev_io);
}

static int
fsdev_aio_set_init_opts(struct aio_fsdev *vfsdev, const struct fuse_init_in *init_in,
			struct fuse_init_out *init_out)
{
	uint64_t flags;
	uint32_t flags2;
	uint64_t aio_flags = 0;

	assert(init_out	!= NULL);
	assert(init_in != NULL);
	assert(init_in->major == FUSE_KERNEL_VERSION);
	assert(init_in->minor >= 31);

	UNUSED(vfsdev);

	memset(init_out, 0, sizeof(*init_out));

	init_out->major = FUSE_KERNEL_VERSION;
	init_out->minor = spdk_min(init_in->minor, spdk_min(FSDEV_AIO_FUSE_KERNEL_MINOR_VERSION,
				   FUSE_KERNEL_MINOR_VERSION));
	init_out->max_readahead = vfsdev->mount_opts.max_readahead;
	init_out->max_background = 0xffff;
	init_out->congestion_threshold = 0xffff;
	init_out->max_write = vfsdev->mount_opts.max_xfer_size;
	init_out->time_gran = 1;
	init_out->max_pages = vfsdev->mount_opts.max_xfer_size / 4096;

	if (vfsdev->opts.writeback_cache_enabled && (init_in->flags & FUSE_WRITEBACK_CACHE)) {
		/* The writeback_cache_enabled was enabled upon creation => we follow the opts */
		vfsdev->opts.writeback_cache_enabled = true;
		SPDK_WARNLOG("Enabling writeback cache is unsafe and requires additional "
			     "synchronization from the applications\n");
		aio_flags |= FUSE_WRITEBACK_CACHE;
	}

	flags2 = (init_in->flags & FUSE_INIT_EXT) ? init_in->flags2 : 0;
	flags = ((uint64_t)flags2 << 32) | init_in->flags;
	aio_flags |=
		FUSE_ASYNC_READ | FUSE_BIG_WRITES | FUSE_DONT_MASK |
		FUSE_HAS_IOCTL_DIR | FUSE_DO_READDIRPLUS | FUSE_READDIRPLUS_AUTO | FUSE_ASYNC_DIO |
		FUSE_NO_OPEN_SUPPORT | FUSE_PARALLEL_DIROPS | FUSE_MAX_PAGES | FUSE_CACHE_SYMLINKS |
		FUSE_NO_OPENDIR_SUPPORT | FUSE_SUBMOUNTS | FUSE_INIT_EXT |
		FUSE_EXPORT_SUPPORT | FUSE_AUTO_INVAL_DATA |  FUSE_EXPLICIT_INVAL_DATA | FUSE_POSIX_ACL |
		FUSE_POSIX_LOCKS | FUSE_FLOCK_LOCKS | FUSE_ATOMIC_O_TRUNC | FUSE_NO_EXPORT_SUPPORT |
		FUSE_DIRECT_IO_ALLOW_MMAP;
	if (init_out->minor >= 33) {
		aio_flags |= FUSE_SETXATTR_EXT;
	}
	flags &= aio_flags;
	init_out->flags = (uint32_t)(flags);
	init_out->flags2 = (uint32_t)(flags >> 32);

	vfsdev->mount_opts.flags = flags;

	/* The AIO doesn't apply any additional restrictions, so we just accept the requested opts */

	SPDK_INFOLOG(fsdev_aio, "INIT: %" PRIu32 ".%" PRIu32 "\n", init_out->major, init_out->minor);
	SPDK_INFOLOG(fsdev_aio, "flags: 0x%08" PRIx64 "\n", flags);
	SPDK_INFOLOG(fsdev_aio, "max_readahead: %" PRIu32 "\n", init_out->max_readahead);
	SPDK_INFOLOG(fsdev_aio, "max_write: %" PRIu32 "\n", init_out->max_write);
	SPDK_INFOLOG(fsdev_aio, "max_pages: %" PRIu32 "\n", init_out->max_pages);
	SPDK_INFOLOG(fsdev_aio, "max_background: %" PRIu16 "\n", init_out->max_background);
	SPDK_INFOLOG(fsdev_aio, "congestion_threshold: %" PRIu16 "\n", init_out->congestion_threshold);
	SPDK_INFOLOG(fsdev_aio, "time_gran: %" PRIu32 "\n", init_out->time_gran);

	return 0;
}

static void
fsdev_aio_fanotify_close(struct aio_fsdev *vfsdev)
{
#ifdef SPDK_CONFIG_HAVE_FANOTIFY
	struct aio_fsdev_linux_fh *entry, *tmp_entry;

	RB_FOREACH_SAFE(entry, aio_fsdev_linux_fh_tree, &vfsdev->linux_fhs, tmp_entry) {
		RB_REMOVE(aio_fsdev_linux_fh_tree, &vfsdev->linux_fhs, entry);
	}

	spdk_poller_unregister(&vfsdev->fanotify_poller);
	if (vfsdev->fanotify_fd != -1) {
		close(vfsdev->fanotify_fd);
		vfsdev->fanotify_fd = -1;
	}
#endif
}

#ifdef SPDK_CONFIG_HAVE_FANOTIFY

static struct aio_fsdev_file_object *
fsdev_aio_get_fobject_by_linux_fh(struct aio_fsdev *vfsdev, const struct file_handle *file_handle)
{
	struct aio_fsdev_linux_fh find = { .fh = file_handle };
	struct aio_fsdev_linux_fh *res;
	struct aio_fsdev_file_object *fobject = NULL;

	spdk_spin_lock(&vfsdev->lock);
	res = RB_FIND(aio_fsdev_linux_fh_tree, &vfsdev->linux_fhs, &find);
	if (res) {
		fobject = SPDK_CONTAINEROF(res, struct aio_fsdev_file_object, linux_fh_entry);
		file_object_ref(fobject);
	}
	spdk_spin_unlock(&vfsdev->lock);

	return fobject;
}

static void
fsdev_aio_notify_reply_cb(struct spdk_fuse_notify_request *req, int status)
{
	struct aio_fsdev_notify_request *aio_req = SPDK_CONTAINEROF(req, struct aio_fsdev_notify_request,
			base);

	SPDK_INFOLOG(fsdev_aio, "Notify reply: status %d, ctx %p\n",
		     status, aio_req);
	free(aio_req);
}

static void
fsdev_aio_fanotify_attrib_event_handle(struct aio_fsdev *vfsdev, struct file_handle *file_handle,
				       const char *file_name)
{
	struct aio_fsdev_file_object *fobject;

	fobject = fsdev_aio_get_fobject_by_linux_fh(vfsdev, file_handle);
	if (fobject) {
		struct aio_fsdev_notify_request *req;
		uint64_t parent_nodeid = fsdev_aio_fobject_to_nodeid(fobject->vfsdev, fobject);
		int rc;

		SPDK_INFOLOG(fsdev_aio, "Notify inval entry: parent " FOBJECT_FMT
			     ", parent fd %d, name %s\n",
			     FOBJECT_ARGS(fobject), fobject->fd, file_name);
		file_object_unref(fobject, 1);

		req = calloc(1, sizeof(*req) + strlen(file_name) + 1);
		if (req == NULL) {
			SPDK_ERRLOG("Cannot allocate notify request\n");
			return;
		}

		req->entry.flags = 0;
		req->out.error = FUSE_NOTIFY_INVAL_ENTRY;
		req->out.unique = 0;
		req->base.iovcnt = 1;
		req->base.iovs = &req->iov;
		req->base.fsdev = &vfsdev->fsdev;
		req->base.cb_fn = fsdev_aio_notify_reply_cb;
		req->entry.parent = parent_nodeid;
		req->entry.namelen = strlen(file_name);
		req->out.len = sizeof(req->out) + sizeof(req->entry) + req->entry.namelen + 1;
		memcpy(req->data, file_name, req->entry.namelen + 1);
		req->iov.iov_base = &req->out;
		req->iov.iov_len = sizeof(req->out) + sizeof(req->entry) + req->entry.namelen + 1;
		/* Upper layer guarantees that reply cb is eventually called.
		 * So, we don't store in-flight notifications in fsdev_aio.
		 */
		rc = spdk_fsdev_notify_fuse(&req->base);
		if (rc) {
			SPDK_ERRLOG("spdk_fsdev_notify_fuse failed with %d\n", rc);
			free(req);
			return;
		}
	} else {
		SPDK_INFOLOG(fsdev_aio, "Fobject not found for parent of %s\n", file_name);
	}
}

static void
fsdev_aio_fanotify_event_handle(struct aio_fsdev *vfsdev,
				const struct fanotify_event_metadata *metadata)
{
	struct fanotify_event_info_header *hdr;
	struct file_handle *file_handle = NULL;
	const char *file_name = NULL;
	uint32_t md_len;

	SPDK_DEBUGLOG(fsdev_aio, "Got fanotify event: fd %d, pid %d, mask %016llX\n",
		      metadata->fd, metadata->pid, metadata->mask);

	md_len = metadata->event_len;
	md_len -= sizeof(*metadata);
	hdr = (struct fanotify_event_info_header *)(metadata + 1);
	while (md_len) {
		if (md_len < sizeof(*hdr)) {
			break;
		}

		SPDK_DEBUGLOG(fsdev_aio, "Extra event info of type %u, len %u\n", hdr->info_type, hdr->len);
		assert(md_len >= hdr->len);
		if (hdr->info_type == FAN_EVENT_INFO_TYPE_DFID_NAME) {
			struct fanotify_event_info_fid *dfid_name = (struct fanotify_event_info_fid *)hdr;
			file_handle = (struct file_handle *)dfid_name->handle;
			file_name = file_handle->f_handle + file_handle->handle_bytes;
		}

		md_len -= hdr->len;
		hdr = (struct fanotify_event_info_header *)((char *)hdr + hdr->len);
	}

	if ((metadata->mask & FAN_ATTRIB) && file_name && file_handle) {
		fsdev_aio_fanotify_attrib_event_handle(vfsdev, file_handle, file_name);
	}

	if (metadata->fd != FAN_NOFD) {
		close(metadata->fd);
	}
}

static int
fsdev_aio_fanotify_poller(void *ctx)
{
	struct aio_fsdev *vfsdev = ctx;
	const struct fanotify_event_metadata *metadata;
	struct fanotify_event_metadata buf[256];
	ssize_t len;

	len = read(vfsdev->fanotify_fd, buf, sizeof(buf));
	if (len == -1 && errno == EAGAIN) {
		return SPDK_POLLER_IDLE;
	} else if (len <= 0) {
		SPDK_ERRLOG("Read fanotify_fd failed: len %ld, errno %d\n", len, errno);
		assert(false);
		fsdev_aio_fanotify_close(vfsdev);
		return SPDK_POLLER_IDLE;
	}

	for (metadata = buf; FAN_EVENT_OK(metadata, len); metadata = FAN_EVENT_NEXT(metadata, len)) {
		if (metadata->vers != FANOTIFY_METADATA_VERSION) {
			SPDK_ERRLOG("Mismatch of fanotify metadata version: expected %d, got %d\n",
				    FANOTIFY_METADATA_VERSION, metadata->vers);
			fsdev_aio_fanotify_close(vfsdev);
			break;
		}

		/* Ignore events from our process */
		if (metadata->pid == vfsdev->pid) {
			continue;
		}

		fsdev_aio_fanotify_event_handle(vfsdev, metadata);
	}

	return SPDK_POLLER_BUSY;
}

#endif /* SPDK_CONFIG_HAVE_FANOTIFY */

static void
fsdev_aio_do_destroy(struct aio_fsdev *vfsdev)
{
#ifdef SPDK_CONFIG_HAVE_FANOTIFY
	if (vfsdev->fanotify_fd != -1) {
		fsdev_aio_fanotify_remove(vfsdev->root);
	}
#endif

	fsdev_free_leafs(vfsdev->root, false);

	/* Reset removes the root from the LUT, so we re-insert it after the reset */
	assert(vfsdev->root->hdr.lut_key == 0); /* The root should be the first element in the LUT */
	spdk_lut_reset(vfsdev->lut);
	spdk_lut_insert_at(vfsdev->lut, vfsdev->root, vfsdev->root->hdr.lut_key);
}

static int
fsdev_aio_op_init(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	struct fuse_out_header *out_hdr = fsdev_io->u_out.fuse.hdr;
	struct fuse_init_in *init_in = fsdev_io->u_in.fuse.op.init;
	struct fuse_init_out *init_out = fsdev_io->u_out.fuse.op.init;

	fsdev_aio_do_destroy(vfsdev);

#ifdef SPDK_CONFIG_HAVE_FANOTIFY
	if (vfsdev->fanotify_fd != -1) {
		int rc;
		spdk_spin_lock(&vfsdev->lock);
		rc = fsdev_aio_fanotify_add(vfsdev->root, AT_FDCWD, vfsdev->root_path);
		spdk_spin_unlock(&vfsdev->lock);
		if (rc) {
			return rc;
		}
	}
#endif

	fsdev_aio_set_init_opts(vfsdev, init_in, init_out);
	out_hdr->len += sizeof(*init_out);
	file_object_ref(vfsdev->root);

	return 0;
}

static int
fsdev_aio_op_destroy(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);

	fsdev_aio_do_destroy(vfsdev);
	file_object_unref(vfsdev->root, 1);

	return 0;
}

static int
fsdev_aio_do_lookup(struct aio_fsdev *vfsdev, struct aio_fsdev_file_object *parent_fobject,
		    const char *name, struct aio_fsdev_file_object **pfobject,
		    struct fuse_entry_out *entry_out)
{
	int newfd;
	int res;
	bool is_new;
	struct stat stat;
	struct aio_fsdev_file_object *fobject;

	/* Do not allow escaping root directory */
	if (parent_fobject == vfsdev->root && strcmp(name, "..") == 0) {
		name = ".";
	}

	newfd = openat(parent_fobject->fd, name, O_PATH | O_NOFOLLOW);
	if (newfd == -1) {
		res = -errno;
		SPDK_DEBUGLOG(fsdev_aio, "openat( " FOBJECT_FMT " %s) failed with %d\n",
			      FOBJECT_ARGS(parent_fobject), name, res);
		return res;
	}

	res = fstatat(newfd, "", &stat, AT_EMPTY_PATH);
	if (res == -1) {
		res = -errno;
		SPDK_ERRLOG("fstatat(%s) failed with %d\n", name, res);
		close(newfd);
		return res;
	}

	spdk_spin_lock(&vfsdev->lock);
	fobject = find_leaf_unsafe(parent_fobject, stat.st_ino, stat.st_dev);
	is_new = (fobject == NULL);
	if (fobject) {
		file_object_ref(fobject); /* reference by a fsdev_aio_do_lookup caller */
	} else {
		fobject = file_object_create_unsafe(vfsdev, parent_fobject, newfd, stat.st_ino, stat.st_dev,
						    stat.st_mode, name);
	}
	spdk_spin_unlock(&vfsdev->lock);

	/* Just in case close() can block, let's keep it out of spinlock. */
	if (!fobject) {
		SPDK_ERRLOG("Cannot create file object\n");
		close(newfd);
		return -ENOMEM;
	}
	if (!is_new) {
		close(newfd);
	}

	if (entry_out) {
		res = fsdev_aio_fill_entry_out(fobject, entry_out);
		if (res) {
			SPDK_ERRLOG("fill_entry_out(%s) failed with %d\n", name, res);
			file_object_unref(fobject, 1);
			return res;
		}
	}

	*pfobject = fobject;

	SPDK_DEBUGLOG(fsdev_aio, "lookup(%s) in dir " FOBJECT_FMT ": "  FOBJECT_FMT " fd=%d\n",
		      name, FOBJECT_ARGS(parent_fobject), FOBJECT_ARGS(fobject), fobject->fd);
	return 0;
}

static int
fsdev_aio_op_lookup(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	int err;
	struct aio_fsdev_file_object *parent_fobject;
	struct aio_fsdev_file_object *fobject = NULL;
	const char *name = fsdev_aio_io_fuse_get_name(fsdev_io);
	struct fuse_out_header *out_hdr = fsdev_io->u_out.fuse.hdr;
	struct fuse_entry_out *entry_out = fsdev_io->u_out.fuse.op.entry;

	if (*name == '\0') {
		SPDK_ERRLOG("Empty name\n");
		return -ENOENT;
	}

	/* Don't use is_safe_path_component(), allow "." and ".." for NFS export
	 * support.
	 */
	if (strchr(name, '/')) {
		SPDK_ERRLOG("Invalid name: %s\n", name);
		return -EINVAL;
	}

	parent_fobject = fsdev_io_get_aio_fobject(fsdev_io);
	if (!parent_fobject) {
		return -EINVAL;
	}

	SPDK_DEBUGLOG(fsdev_aio, "  name %s\n", name);

	err = fsdev_aio_do_lookup(vfsdev, parent_fobject, name, &fobject, entry_out);
	if (err) {
		SPDK_DEBUGLOG(fsdev_aio, "fsdev_aio_do_lookup(%s) failed with err=%d\n", name, err);
		goto fop_failed;
	}

	out_hdr->len += sizeof(*entry_out);
	err = 0;

fop_failed:
	file_object_unref(parent_fobject, 1);
	return err;
}

static int
fsdev_aio_op_syncfs(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	struct aio_fsdev_file_object *fobject;
	int fd, res;

	fobject = fsdev_io_get_aio_fobject(fsdev_io);
	if (fobject != vfsdev->root) {
		SPDK_ERRLOG("Syncfs expected root file object but received " FOBJECT_FMT
			    "\n", FOBJECT_ARGS(fobject));
		return -EINVAL;
	}

	/*
	 * We cannot use root's fd that was open with open(O_PATH) because syncfs()
	 * requires any defined permission and O_PATH has none.
	 */
	fd = open(vfsdev->root_path, O_RDONLY);
	if (fd == -1) {
		res = -errno;
		SPDK_ERRLOG("Cannot open root %s (err=%d)\n", vfsdev->root_path, res);
		goto fop_failed;
	}

	res = syncfs(fd);
	if (res == -1) {
		res = -errno;
		SPDK_ERRLOG("Cannot syncfs for " FOBJECT_FMT " (err=%d)\n", FOBJECT_ARGS(fobject), res);
		close(fd);
		goto fop_failed;
	}
	close(fd);

	SPDK_DEBUGLOG(fsdev_aio, "SYNCFS succeeded for " FOBJECT_FMT "\n", FOBJECT_ARGS(fobject));
	res = 0;

fop_failed:
	file_object_unref(fobject, 1);
	return res;
}

static int
fsdev_aio_op_lseek(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	int res;
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	struct aio_fsdev_file_object *fobject;
	struct aio_fsdev_file_handle *fhandle;
	struct fuse_lseek_in *lseek_in = fsdev_io->u_in.fuse.op.lseek;
	off_t offset = lseek_in->offset;
	uint32_t whence = lseek_in->whence;
	struct fuse_lseek_out *lseek_out = fsdev_io->u_out.fuse.op.lseek;

	fobject = fsdev_io_get_aio_fobject(fsdev_io);
	if (!fobject) {
		SPDK_ERRLOG("Invalid fobject: %p\n", fobject);
		return -EINVAL;
	}

	fhandle = fsdev_aio_get_fhandle_by_fuse_fh(vfsdev, lseek_in->fh);
	if (!fhandle) {
		SPDK_ERRLOG("Invalid fhandle: %p\n", fhandle);
		res = -EINVAL;
		goto fop_failed;
	}

	offset = lseek(fhandle->fd, offset, (int)whence);
	if (offset == (off_t) -1) {
		res = -errno;
		SPDK_ERRLOG("Failed to change read/write offset for " FOBJECT_FMT " (err=%d)\n",
			    FOBJECT_ARGS(fobject), res);
		goto fop_failed;
	}

	SPDK_DEBUGLOG(fsdev_aio, "LSEEK succeeded for " FOBJECT_FMT "\n", FOBJECT_ARGS(fobject));

	fsdev_io->u_out.fuse.hdr->len += sizeof(struct fuse_lseek_out);
	lseek_out->offset = offset;
	res = 0;

fop_failed:
	file_object_unref(fobject, 1);
	return res;
}

static int
fsdev_aio_do_poll(struct aio_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	int res;
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	struct aio_fsdev_io *vfsdev_io = fsdev_to_aio_io(fsdev_io);
	struct aio_fsdev_file_object *fobject;
	struct aio_fsdev_file_handle *fhandle;
	struct fuse_poll_in *poll_in = fsdev_io->u_in.fuse.op.poll;
	struct fuse_poll_out *poll_out = fsdev_io->u_out.fuse.op.poll;
	short posix_events = poll_in->events;
	struct pollfd fds;

	fobject = fsdev_io_get_aio_fobject(fsdev_io);
	if (!fobject) {
		SPDK_ERRLOG("Invalid fobject: %p\n", fobject);
		return -EINVAL;
	}

	fhandle = fsdev_aio_get_fhandle_by_fuse_fh(vfsdev, poll_in->fh);
	if (!fhandle) {
		SPDK_ERRLOG("Invalid fhandle: %p\n", fhandle);
		res = -EINVAL;
		goto fop_failed;
	}

	fds.fd = fhandle->fd;
	fds.events = posix_events;
	fds.revents = 0;

	/* Zero timeout - return immediately even if no events available. */
	res = poll(&fds, 1, 0);
	if (res == -1) {
		res = -errno;
		SPDK_ERRLOG("Failed poll for " FOBJECT_FMT " (err=%d)\n",
			    FOBJECT_ARGS(fobject), res);
		goto fop_failed;
	}

	if (res == 0) {
		TAILQ_INSERT_TAIL(&ch->ios_for_submit, vfsdev_io, link);
		res = IO_STATUS_ASYNC;
		goto fop_failed;
	}

	if (res > 0) {
		fsdev_io->u_out.fuse.hdr->len += sizeof(struct fuse_poll_out);
		poll_out->revents = fds.revents;
		res = 0;
	}

	SPDK_DEBUGLOG(fsdev_aio, "POLL succeeded for " FOBJECT_FMT "\n", FOBJECT_ARGS(fobject));

fop_failed:
	file_object_unref(fobject, 1);
	return res;
}

static int
fsdev_aio_op_poll(struct spdk_io_channel *_ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_io_channel *ch = spdk_io_channel_get_ctx(_ch);
	return fsdev_aio_do_poll(ch, fsdev_io);
}

static struct aio_ioctl_data aio_data;

/**
 * Example implemenatation of ioctl.
 *
 * It handles the ioctl cmds that we created to show how to do that properly.
 * - AIO_IOCTL_GET_DATA_CMD - traditional get for some internal struct
 *   which size is known.
 * - AIO_IOCTL_SET_DATA_CMD - same as the previous for setting the local
 *   data.
 * - AIO_IOCTL_DATA_CMD - getting and setting data in same cmd.
 * - AIO_IOCTL_ACT_CMD - no data, just a command.
 */
static int
fsdev_aio_do_aio_ioctl(struct spdk_fsdev_io *fsdev_io, void *in_buf, uint32_t in_bufsz,
		       void *out_buf, uint32_t out_bufsz)
{
	struct fuse_ioctl_in *ioctl_in = fsdev_io->u_in.fuse.op.ioctl;
	struct aio_ioctl_data *rt_data;
	struct aio_ioctl_data saved;

	switch (ioctl_in->cmd) {
	case AIO_IOCTL_GET_DATA_CMD:
		if (ioctl_in->out_size != sizeof(*rt_data)) {
			SPDK_ERRLOG("Invalid out size: %u\n", ioctl_in->out_size);
			return -EINVAL;
		}

		if (out_bufsz < sizeof(*rt_data)) {
			SPDK_ERRLOG("Out iovec is too small: %" PRIu32 "\n", out_bufsz);
			return -EINVAL;
		}

		rt_data = (struct aio_ioctl_data *)out_buf;
		*rt_data = aio_data;
		break;
	case AIO_IOCTL_SET_DATA_CMD:
		if (ioctl_in->in_size != sizeof(*rt_data)) {
			SPDK_ERRLOG("Invalid in size: %u\n", ioctl_in->in_size);
			return -EINVAL;
		}

		if (in_bufsz < sizeof(*rt_data)) {
			SPDK_ERRLOG("In iovec is too small: %" PRIu32 "\n", in_bufsz);
			return -EINVAL;
		}

		rt_data = (struct aio_ioctl_data *)in_buf;
		aio_data = *rt_data;
		break;
	case AIO_IOCTL_DATA_CMD:
		if (ioctl_in->in_size != sizeof(*rt_data) || ioctl_in->out_size != sizeof(*rt_data)) {
			SPDK_ERRLOG("Invalid in or out size: %u or %u\n", ioctl_in->in_size, ioctl_in->out_size);
			return -EINVAL;
		}

		if (in_bufsz < sizeof(*rt_data)) {
			SPDK_ERRLOG("In iovec is too small: %" PRIu32 "\n", in_bufsz);
			return -EINVAL;
		}

		if (out_bufsz < sizeof(*rt_data)) {
			SPDK_ERRLOG("Out iovec is too small: %" PRIu32 "\n", out_bufsz);
			return -EINVAL;
		}

		/*
		 * Input and output buffers can point to the same region of memory. Saving the input.
		 */
		rt_data = (struct aio_ioctl_data *)in_buf;
		saved = *rt_data;

		/*
		 * Populating the data and sending _old_ data back (we decided we want this kind of behavior
		 * for this particular custom ioctl cmd) like a normal get.
		 */
		rt_data = (struct aio_ioctl_data *)out_buf;
		*rt_data = aio_data;
		aio_data = saved;
		break;
	case AIO_IOCTL_ACT_CMD:
		SPDK_DEBUGLOG(fsdev_aio, "Zero-sized ioctl() has been successfully handled.\n");
		break;
	default:
		SPDK_INFOLOG(fsdev_aio, "Unknown ioctl cmd: %u\n", ioctl_in->cmd);
		return -ENOTTY;
	}

	fsdev_io->u_out.fuse.hdr->len += ioctl_in->out_size;

	return 0;
}

static int
fsdev_aio_do_fsioc_ioctl(struct aio_fsdev *vfsdev, struct aio_fsdev_file_object *fobject,
			 uint32_t request, void *buf)
{
	int fd;
	int res;

	fd = openat(vfsdev->proc_self_fd, fobject->fd_str, O_RDWR);
	if (fd == -1) {
		res = -errno;
		SPDK_ERRLOG("Failed to open fd %d\n", fobject->fd);
		return res;
	}

	res = ioctl(fd, request, buf);
	if (res == -1) {
		res = -errno;
		SPDK_ERRLOG("ioctl failed for fd %d\n", fd);
	}
	close(fd);
	return res;
}

static int
fsdev_aio_op_ioctl(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	int res;
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	struct aio_fsdev_file_object *fobject;
	struct fuse_ioctl_in *ioctl_in = fsdev_io->u_in.fuse.op.ioctl;
	struct fuse_ioctl_out *ioctl_out = fsdev_io->u_out.fuse.op.ioctl;
	uint32_t in_bufsz = 0, out_bufsz = 0;
	void *in_buf = NULL, *out_buf = NULL;

	/*
	 * FUSE_IOCTL_COMPAT is used when 32-bit user space app calls ioctl()
	 * on a 64-bit kernel.
	 */
	if (ioctl_in->flags & (FUSE_IOCTL_COMPAT | FUSE_IOCTL_32BIT)) {
		SPDK_ERRLOG("Compat ioctl is not supported.\n");
		return -EINVAL;
	}

	/*
	 * Another compat flag. Not supported.
	 */
	if (ioctl_in->flags & FUSE_IOCTL_COMPAT_X32) {
		SPDK_ERRLOG("Compat x32 ioctl is not supported.\n");
		return -EINVAL;
	}

	/*
	 * Unrestricted flag. Not supported.
	 */
	if (ioctl_in->flags & FUSE_IOCTL_UNRESTRICTED) {
		SPDK_ERRLOG("Unrestricted ioctl is not supported.\n");
		return -EINVAL;
	}

	if (!fsdev_io->u_out.fuse.iovcnt || !fsdev_io->u_out.fuse.iov[0].iov_base) {
		SPDK_ERRLOG("No ioctl out buffers provided (%" PRIu32 ")\n", fsdev_io->u_out.fuse.iovcnt);
		return -EINVAL;
	}

	if (fsdev_io->u_out.fuse.iov[0].iov_len < sizeof(struct fuse_ioctl_out)) {
		SPDK_ERRLOG("Invalid ioctl out buffer size: %" PRIu64 "\n", fsdev_io->u_out.fuse.iov[0].iov_len);
		return -EINVAL;
	}

	fobject = fsdev_io_get_aio_fobject(fsdev_io);
	if (!fobject) {
		SPDK_ERRLOG("Invalid fobject: %p\n", fobject);
		return -EINVAL;
	}

	if (ioctl_in->in_size) {
		struct iovec *in_iovs = fsdev_io->u_in.fuse.iov;

		if (in_iovs[0].iov_len >= ioctl_in->in_size) {
			in_buf = in_iovs[0].iov_base;
			in_bufsz = in_iovs[0].iov_len;
		} else {
			SPDK_ERRLOG("In iovec is too small for %" PRIu32 "\n", ioctl_in->in_size);
			return -EINVAL;
		}
	}

	if (ioctl_in->out_size) {
		struct iovec *out_iovs = fsdev_io->u_out.fuse.iov;

		if (out_iovs[0].iov_len >= sizeof(*ioctl_out) + ioctl_in->out_size) {
			out_buf = out_iovs[0].iov_base + sizeof(*ioctl_out);
			out_bufsz = out_iovs[0].iov_len - sizeof(*ioctl_out);
		} else if (fsdev_io->u_out.fuse.iovcnt > 1 && out_iovs[1].iov_base &&
			   out_iovs[1].iov_len >= ioctl_in->out_size) {
			out_buf = out_iovs[1].iov_base;
			out_bufsz = out_iovs[1].iov_len;
		} else {
			SPDK_ERRLOG("Out iovecs are too small for out size %" PRIu32 "\n", ioctl_in->out_size);
			return -EINVAL;
		}
	}

	memset(ioctl_out, 0, sizeof(*ioctl_out));
	fsdev_io->u_out.fuse.hdr->len += sizeof(struct fuse_ioctl_out);

	switch (ioctl_in->cmd) {
	case AIO_IOCTL_GET_DATA_CMD:
	case AIO_IOCTL_SET_DATA_CMD:
	case AIO_IOCTL_DATA_CMD:
	case AIO_IOCTL_ACT_CMD:
		res = fsdev_aio_do_aio_ioctl(fsdev_io, in_buf, in_bufsz, out_buf, out_bufsz);
		break;
	case FS_IOC_GETFLAGS:
	case FS_IOC_FSGETXATTR:
		res = fsdev_aio_do_fsioc_ioctl(vfsdev, fobject, ioctl_in->cmd, out_buf);
		if (!res) {
			fsdev_io->u_out.fuse.hdr->len += ioctl_in->out_size;
		}
		break;
	case FS_IOC_SETFLAGS:
		res = fsdev_aio_do_fsioc_ioctl(vfsdev, fobject, ioctl_in->cmd, in_buf);
		break;
	default:
		SPDK_INFOLOG(fsdev_aio, "Unknown ioctl cmd: %u\n", ioctl_in->cmd);
		res = -ENOTTY;
		goto fop_failed;
		break;
	}

	SPDK_DEBUGLOG(fsdev_aio, "IOCTL(%u) for " FOBJECT_FMT " handled with result=%d\n",
		      ioctl_in->cmd, FOBJECT_ARGS(fobject), res);

fop_failed:
	file_object_unref(fobject, 1);
	ioctl_out->result = res;
	return 0;
}

#if DEBUG
static const char *
posix_lock_type_to_str(uint32_t posix_lock_type)
{
	if (posix_lock_type == F_RDLCK) {
		return "F_RDLCK";
	} else if (posix_lock_type == F_WRLCK) {
		return "F_WRLCK";
	} else if (posix_lock_type == F_UNLCK) {
		return "F_UNLCK";
	} else {
		return "UNKNOWN";
	}
}
#endif

static void
fuse_lk_to_flock(struct fuse_file_lock *lk, struct flock *posix_lock)
{
	memset(posix_lock, 0, sizeof(*posix_lock));

	posix_lock->l_type = lk->type;
	posix_lock->l_whence = SEEK_SET;
	posix_lock->l_start = lk->start;
	if (lk->end == 0) {
		/* 0 means lock to the end of the file in POSIX */
		posix_lock->l_len = 0;
	} else {
		posix_lock->l_len = lk->end - lk->start + 1;
	}

	posix_lock->l_pid = lk->pid;

	SPDK_DEBUGLOG(fsdev_aio, "fsdev -> flock type=%s, start=%lu, len=%lu, pid=%u\n",
		      posix_lock_type_to_str(posix_lock->l_type), posix_lock->l_start,
		      posix_lock->l_len, posix_lock->l_pid);
}

static int
flock_to_fuse_lk(int fd, const struct flock *posix_lock, struct fuse_file_lock *lk)
{
	off_t current_pos;

	memset(lk, 0, sizeof(*lk));

	switch (posix_lock->l_whence) {
	case SEEK_SET:
		lk->start = posix_lock->l_start;
		break;
	case SEEK_CUR:
		current_pos = lseek(fd, 0, SEEK_CUR);
		if (current_pos == (off_t) -1) {
			SPDK_ERRLOG("Failed to get current file pos during "
				    "posix lock conversion with whence=%d!\n",
				    posix_lock->l_whence);
			return -EINVAL;
		}
		lk->start = current_pos + posix_lock->l_start;
		break;
	case SEEK_END:
		current_pos = lseek(fd, 0, SEEK_END);
		if (current_pos == (off_t) -1) {
			SPDK_ERRLOG("Failed to get current file pos during "
				    "posix lock conversion with whence=%d!\n",
				    posix_lock->l_whence);
			return -EINVAL;
		}
		lk->start = current_pos + posix_lock->l_start;
		break;
	default:
		SPDK_ERRLOG("Invalid whence=%d during "
			    "posix lock conversion!\n", posix_lock->l_whence);
		return -EINVAL;
	}

	if (posix_lock->l_len == 0) {
		/* Lock to the end of the file. */
		lk->end = LONG_MAX;
	} else {
		lk->end = posix_lock->l_start + posix_lock->l_len - 1;
	}

	lk->type = posix_lock->l_type;
	lk->pid = posix_lock->l_pid;

	SPDK_DEBUGLOG(fsdev_aio, "flock -> fuse lk type=%x, start=%lu, end=%lu, pid=%u\n",
		      lk->type, lk->start, lk->end, lk->pid);
	return 0;
}

/*
 * This function is not fully functional implementation of getlk() operation.
 * In the enviroment where virtiofs is used the lock pid is usually wrong or 0
 * which needs to be specially handled. Thimnk of it as of an example or
 * tutorial of how it can be implemented.
 */
static int
fsdev_aio_op_getlk(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	int res;
	struct flock posix_lock;
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	struct aio_fsdev_file_object *fobject;
	struct aio_fsdev_file_handle *fhandle;
	struct fuse_lk_in *lk_in = fsdev_io->u_in.fuse.op.lk;
	struct fuse_lk_out *lk_out = fsdev_io->u_out.fuse.op.lk;

	fobject = fsdev_io_get_aio_fobject(fsdev_io);
	if (!fobject) {
		SPDK_ERRLOG("Invalid fobject: %p\n", fobject);
		return -EINVAL;
	}

	fhandle = fsdev_aio_get_fhandle_by_fuse_fh(vfsdev, lk_in->fh);
	if (!fhandle) {
		SPDK_ERRLOG("Invalid fhandle: %p\n", fhandle);
		res = -EINVAL;
		goto fop_failed;
	}

	fuse_lk_to_flock(&lk_in->lk, &posix_lock);

	/*
	 * We're using the input lock and passing it to fcntl(F_GETLK).
	 * This technique is used for checking if a lock of particular
	 * type and the file region can be obtained.
	 */
	res = fcntl(fhandle->fd, F_GETLK, &posix_lock);
	if (res == -1) {
		res = -errno;
		SPDK_ERRLOG("Getlk failed for " FOBJECT_FMT " (err=%d)\n",
			    FOBJECT_ARGS(fobject), res);
		goto fop_failed;
	}

	res = flock_to_fuse_lk(fhandle->fd, &posix_lock, &lk_out->lk);
	if (res) {
		goto fop_failed;
	}

	fsdev_io->u_out.fuse.hdr->len = sizeof(struct fuse_lk_out);
	res = 0;

	SPDK_DEBUGLOG(fsdev_aio, "GETLK succeeded for " FOBJECT_FMT " lock=(type:%d,start:%lu,len:%lu)\n",
		      FOBJECT_ARGS(fobject), posix_lock.l_type, posix_lock.l_start, posix_lock.l_len);

fop_failed:
	file_object_unref(fobject, 1);
	return res;
}

/*
 * This function is not fully functional implementation of setlk() operation.
 * In the environment where fsdev is used the lock pid is usually wrong or 0
 * which needs to be specially handled. Think of it as of an example or
 * tutorial of how it can be implemented.
 */
static int
fsdev_aio_do_setlk(struct aio_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	int res;
	struct flock posix_lock;
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	struct aio_fsdev_io *vfsdev_io = fsdev_to_aio_io(fsdev_io);
	struct fuse_lk_in *lk_in = fsdev_io->u_in.fuse.op.lk;
	struct aio_fsdev_file_object *fobject;
	struct aio_fsdev_file_handle *fhandle;

	fobject = fsdev_io_get_aio_fobject(fsdev_io);
	if (!fobject) {
		SPDK_ERRLOG("Invalid fobject: %p\n", fobject);
		return -EINVAL;
	}

	fhandle = fsdev_aio_get_fhandle_by_fuse_fh(vfsdev, lk_in->fh);
	if (!fhandle) {
		SPDK_ERRLOG("Invalid fhandle: %p\n", fhandle);
		res = -EINVAL;
		goto fop_failed;
	}

	/* if FUSE_LK_FLOCK is requested, handle flock style of the lock. */
	if (lk_in->lk_flags & FUSE_LK_FLOCK) {
		res = flock(fhandle->fd, lk_in->lk.type | LOCK_NB);
		if (res == -1) {
			res = -errno;
			SPDK_ERRLOG("flock failed for " FOBJECT_FMT " (err=%d)\n", FOBJECT_ARGS(fobject), res);
		}

		res = 0;
		SPDK_DEBUGLOG(fsdev_aio, "flock succeeded for " FOBJECT_FMT " fh=%p operation=%d\n",
			      FOBJECT_ARGS(fobject), fhandle, lk_in->lk.type);
		goto fop_failed;

	}

	/* Otherwise, handle POSIX advisory record locking. */
	fuse_lk_to_flock(&lk_in->lk, &posix_lock);

	res = fcntl(fhandle->fd, F_SETLK, &posix_lock);
	if (res == -1) {
		res = -errno;

		/*
		 * Some implementations return -EACCES for conflicting locks. We show
		 * error for the other error codes.
		 */
		if (res != -EACCES && res != -EAGAIN) {
			SPDK_ERRLOG("Fcntl failed for " FOBJECT_FMT " (err=%d)\n",
				    FOBJECT_ARGS(fobject), res);
		} else if (res == -EACCES) {
			res = -EAGAIN;
			goto fop_failed;
		}

		if (res == -EAGAIN && fsdev_io->u_in.fuse.hdr->opcode == FUSE_SETLKW) {
			TAILQ_INSERT_TAIL(&ch->ios_for_submit, vfsdev_io, link);
			res = IO_STATUS_ASYNC;
		}

		goto fop_failed;
	}

	res = 0;
	SPDK_DEBUGLOG(fsdev_aio, "SETLK succeeded for " FOBJECT_FMT " lock=(type:%d,start:%lu,len:%lu)\n",
		      FOBJECT_ARGS(fobject), posix_lock.l_type, posix_lock.l_start, posix_lock.l_len);

fop_failed:
	file_object_unref(fobject, 1);
	return res;
}

static int
fsdev_aio_op_setlk(struct spdk_io_channel *_ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_io_channel *ch = spdk_io_channel_get_ctx(_ch);

	return fsdev_aio_do_setlk(ch, fsdev_io);
}

/*
 * Change to uid/gid of caller so that file is created with ownership of caller.
 */
static int
fsdev_aio_change_cred(const struct fsdev_aio_cred *new, struct fsdev_aio_cred *old)
{
	int res;

	old->euid = geteuid();
	old->egid = getegid();

	res = syscall(SYS_setresgid, -1, new->egid, -1);
	if (res == -1) {
		return -errno;
	}

	res = syscall(SYS_setresuid, -1, new->euid, -1);
	if (res == -1) {
		int errno_save = -errno;

		syscall(SYS_setresgid, -1, old->egid, -1);
		return errno_save;
	}

	return 0;
}

/* Regain Privileges */
static void
fsdev_aio_restore_cred(struct fsdev_aio_cred *old)
{
	int res;

	res = syscall(SYS_setresuid, -1, old->euid, -1);
	if (res == -1) {
		SPDK_ERRLOG("seteuid(%u)", old->euid);
	}

	res = syscall(SYS_setresgid, -1, old->egid, -1);
	if (res == -1) {
		SPDK_ERRLOG("setegid(%u)", old->egid);
	}
}

static inline int
fsdev_aio_fill_dirent(struct spdk_fsdev_io *fsdev_io, char *buf, size_t bufsize,
		      const struct dirent *entry)
{
	size_t namelen;
	size_t entlen;
	size_t entlen_padded;
	struct fuse_dirent *dirent;

	assert(buf != NULL);

	namelen = strlen(entry->d_name);
	entlen = FUSE_NAME_OFFSET + namelen;
	entlen_padded = FUSE_DIRENT_ALIGN(entlen);

	if (entlen_padded > bufsize) {
		return -EAGAIN;
	}

	dirent = (struct fuse_dirent *)buf;
	dirent->ino = entry->d_ino;
	dirent->off = entry->d_off;
	dirent->namelen = namelen;
	dirent->type = entry->d_type;
	memcpy(dirent->name, entry->d_name, namelen);
	memset(dirent->name + namelen, 0, entlen_padded - entlen);

	return (int)entlen_padded;
}

static int
fsdev_aio_add_dirent(struct spdk_fsdev_io *fsdev_io, char *buf, size_t bufsize,
		     const struct dirent *entry)
{
	return fsdev_aio_fill_dirent(fsdev_io, buf, bufsize, entry);
}

static size_t
fsdev_aio_add_direntplus(struct spdk_fsdev_io *fsdev_io, char *buf, size_t bufsize,
			 struct aio_fsdev_file_object *fobject, const struct dirent *entry)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	struct aio_fsdev_file_object *entry_fobject;
	struct fuse_direntplus *direntplus = (struct fuse_direntplus *)buf;
	int res;
	size_t dentry_offs = offsetof(struct fuse_direntplus, dirent);

	/* check that the buffer size is at least enough for the direntplus, so that it's safe to lookup the entry_out */
	if (bufsize <= sizeof(*direntplus)) {
		return -EAGAIN;
	}

	if (is_dot_or_dotdot(entry->d_name)) {
		memset(&direntplus->entry_out, 0, sizeof(direntplus->entry_out));
		goto fill_dentry; /* skip lookup and fill the entry_out */
	}

	res = fsdev_aio_do_lookup(vfsdev, fobject, entry->d_name, &entry_fobject,
				  &direntplus->entry_out);
	if (res) {
		SPDK_DEBUGLOG(fsdev_aio, "fsdev_aio_do_lookup(%s) failed with err=%d\n", entry->d_name, res);
		return res;
	}

fill_dentry:
	res = fsdev_aio_fill_dirent(fsdev_io, buf + dentry_offs, bufsize - dentry_offs, entry);
	if (res < 0) {
		SPDK_DEBUGLOG(fsdev_aio, "fsdev_aio_fill_dirent(%s) failed with err=%d\n", entry->d_name, res);
		return res;
	}

	return dentry_offs + res;
}

static int
fsdev_aio_do_readdir(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io, bool simple)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	struct fuse_read_in *read_in = fsdev_io->u_in.fuse.op.read;
	struct aio_fsdev_file_object *fobject;
	struct aio_fsdev_file_handle *fhandle;
	char *buf = fsdev_io->u_out.fuse.iov[0].iov_base;
	size_t bufsize = fsdev_io->u_out.fuse.iov[0].iov_len;
	uint32_t bytes_written = 0;
	off_t offset;
	int res;

	if (bufsize < read_in->size) {
		SPDK_ERRLOG("Invalid readdir size: %zu < %" PRIu32 "\n", bufsize, read_in->size);
		return -EINVAL;
	}

	fobject = fsdev_io_get_aio_fobject(fsdev_io);
	if (!fobject) {
		SPDK_ERRLOG("Invalid fobject: 0x%" PRIx64 "\n", fsdev_io->u_in.fuse.hdr->nodeid);
		return -EINVAL;
	}

	fhandle = fsdev_aio_get_fhandle_by_fuse_fh(vfsdev, read_in->fh);
	if (!fhandle) {
		SPDK_ERRLOG("Invalid fhandle: 0x%" PRIx64 "\n", read_in->fh);
		res = -EINVAL;
		goto fop_failed;
	}

	offset = ((off_t)read_in->offset);
	if (offset != fhandle->dir.offset) {
		seekdir(fhandle->dir.dp, offset);
		fhandle->dir.entry = NULL;
		fhandle->dir.offset = offset;
	}

	bufsize = read_in->size;
	while (1) {
		if (!fhandle->dir.entry) {
			errno = 0;
			fhandle->dir.entry = readdir(fhandle->dir.dp);
			if (!fhandle->dir.entry) {
				if (errno) {  /* Error */
					res = -errno;
					SPDK_ERRLOG("readdir failed with err=%d", res);
					goto fop_failed;
				} else {  /* End of stream */
					break;
				}
			}
		}

		offset = fhandle->dir.entry->d_off;

		/* Hide root's parent directory */
		if (fobject == vfsdev->root && strcmp(fhandle->dir.entry->d_name, "..") == 0) {
			res = 0;
			goto continue_readdir;
		}

		/* handle t simple readdir first as it doesn't need to lookup the entry */
		if (simple) {
			res = fsdev_aio_add_dirent(fsdev_io, buf, bufsize, fhandle->dir.entry);
			if (res < 0) {
				/* non-zero value returned -> stop the readdir */
				goto fop_failed;
			}

			goto continue_readdir;
		}

		/* normal readdir case handling does lookup and works with fobjects */
		res = fsdev_aio_add_direntplus(fsdev_io, buf, bufsize, fobject, fhandle->dir.entry);
		if (res < 0) {
			/* non-zero value returned -> stop the readdir */
			goto fop_failed;
		}

continue_readdir:
		bytes_written += res;
		buf += res;
		bufsize -= res;

		fhandle->dir.entry = NULL;
		fhandle->dir.offset = offset;
	}

	res = 0;
	SPDK_DEBUGLOG(fsdev_aio,
		      "READDIR succeeded for " FOBJECT_FMT " (simple=%d, sfh=%p, offset=%" PRIu64 " -> %" PRIu64 ")\n",
		      FOBJECT_ARGS(fobject), simple, fhandle, read_in->offset, offset);
fop_failed:
	if (!res || res == -EAGAIN) {
		fsdev_io->u_out.fuse.hdr->len += bytes_written;
		res = 0;
	}
	file_object_unref(fobject, 1);
	return res;
}


static int
fsdev_aio_op_readdirplus(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	return fsdev_aio_do_readdir(ch, fsdev_io, false);
}

static int
fsdev_aio_op_readdir(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	return fsdev_aio_do_readdir(ch, fsdev_io, true);
}

static int
fsdev_aio_do_forget(struct aio_fsdev *vfsdev, uint64_t nodeid, uint64_t nlookup)
{
	struct aio_fsdev_file_object *fobject;

	fobject = fsdev_aio_get_fobject_by_nodeid(vfsdev, nodeid);
	if (!fobject) {
		SPDK_ERRLOG("Invalid fobject: %p\n", fobject);
		return -EINVAL;
	}

	SPDK_DEBUGLOG(fsdev_aio, "FORGET for " FOBJECT_FMT " nlookup=%" PRIu64 "\n",
		      FOBJECT_ARGS(fobject), nlookup);
	file_object_unref(fobject, nlookup + 1 /* + 1 for the fsdev_aio_get_fobject */);

	return 0;
}

static int
fsdev_aio_op_forget(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);

	return fsdev_aio_do_forget(vfsdev, fsdev_io->u_in.fuse.hdr->nodeid,
				   fsdev_io->u_in.fuse.op.forget->nlookup);
}

static int
fsdev_aio_op_batch_forget(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	struct fuse_batch_forget_in *batch = fsdev_io->u_in.fuse.op.batch_forget;
	struct fuse_forget_one *forget = fsdev_io->u_in.fuse.iov[0].iov_base;
	int ret = 0;
	uint32_t i;

	for (i = 0; i < batch->count; i++) {
		int rc;

		rc = fsdev_aio_do_forget(vfsdev, forget[i].nodeid, forget[i].nlookup);
		ret = (ret != 0) ? ret : rc;
	}

	return ret;
}

static uint32_t
update_open_flags(struct aio_fsdev *vfsdev, uint32_t flags)
{
	/*
	 * With writeback cache, kernel may send read requests even
	 * when userspace opened write-only
	 */
	if (vfsdev->opts.writeback_cache_enabled && (flags & O_ACCMODE) == O_WRONLY) {
		flags &= ~O_ACCMODE;
		flags |= O_RDWR;
	}

	/*
	 * With writeback cache, O_APPEND is handled by the kernel.
	 * This breaks atomicity (since the file may change in the
	 * underlying filesystem, so that the kernel's idea of the
	 * end of the file isn't accurate anymore). In this example,
	 * we just accept that. A more rigorous filesystem may want
	 * to return an error here
	 */
	if (vfsdev->opts.writeback_cache_enabled && (flags & O_APPEND)) {
		flags &= ~O_APPEND;
	}

	/*
	 * O_DIRECT in guest should not necessarily mean bypassing page
	 * cache on host as well. If somebody needs that behavior, it
	 * probably should be a configuration knob in daemon.
	 */
	flags &= ~O_DIRECT;

	return flags;
}

static int
fsdev_aio_op_open(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	int fd, res;
	struct aio_fsdev_file_object *fobject;
	uint32_t flags = fsdev_io->u_in.fuse.op.open->flags;
	struct aio_fsdev_file_handle *fhandle;

	fobject = fsdev_io_get_aio_fobject(fsdev_io);
	if (!fobject) {
		SPDK_ERRLOG("Invalid fobject: %p\n", fobject);
		return -EINVAL;
	}

	flags = update_open_flags(vfsdev, flags);

	fd = openat(vfsdev->proc_self_fd, fobject->fd_str, flags & ~O_NOFOLLOW);
	if (fd == -1) {
		res = -errno;
		SPDK_ERRLOG("openat(%d, %s, 0x%08" PRIx32 ") failed with err=%d\n",
			    vfsdev->proc_self_fd, fobject->fd_str, flags, res);
		goto fop_failed;
	}

	fhandle = file_handle_alloc(fobject, fd);
	if (!fhandle) {
		res = -ENOMEM;
		SPDK_ERRLOG("cannot create a file handle (fd=%d)\n", fd);
		close(fd);
		goto fop_failed;
	}

	fsdev_io->u_out.fuse.hdr->len += sizeof(struct fuse_open_out);
	fsdev_io->u_out.fuse.op.open->fh = fsdev_aio_get_fuse_fh(vfsdev, fhandle);
	fsdev_io->u_out.fuse.op.open->open_flags = 0;
	if (fsdev_io->u_in.fuse.op.open->flags & O_DIRECT) {
		fsdev_io->u_out.fuse.op.open->open_flags |= FOPEN_DIRECT_IO;
	}

	res = 0;
	SPDK_DEBUGLOG(fsdev_aio, "OPEN succeeded for " FOBJECT_FMT " (fh=%p, fd=%d)\n",
		      FOBJECT_ARGS(fobject), fhandle, fd);

fop_failed:
	file_object_unref(fobject, 1);
	return res;
}

static int
fsdev_aio_op_flush(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	struct aio_fsdev_file_object *fobject;
	struct aio_fsdev_file_handle *fhandle;
	int res, dup_fd;

	fobject = fsdev_io_get_aio_fobject(fsdev_io);
	if (!fobject) {
		SPDK_ERRLOG("Invalid fobject: %p\n", fobject);
		return -EINVAL;
	}

	fhandle = fsdev_aio_get_fhandle_by_fuse_fh(vfsdev, fsdev_io->u_in.fuse.op.flush->fh);
	if (!fhandle) {
		SPDK_ERRLOG("Invalid fhandle: %p\n", fhandle);
		res = -EINVAL;
		goto fop_failed;
	}

	dup_fd = dup(fhandle->fd);
	if (dup_fd == -1) {
		res = -errno;
		SPDK_ERRLOG("dup(%d) failed for " FOBJECT_FMT " (fh=%p, err=%d)\n",
			    fhandle->fd, FOBJECT_ARGS(fobject), fhandle, res);
		goto fop_failed;
	}
	res = close(dup_fd);
	if (res) {
		res = -errno;
		SPDK_ERRLOG("close(%d) failed for " FOBJECT_FMT " (fh=%p, err=%d)\n",
			    dup_fd, FOBJECT_ARGS(fobject), fhandle, res);
		goto fop_failed;
	}

	res = 0;
	SPDK_DEBUGLOG(fsdev_aio, "FLUSH succeeded for " FOBJECT_FMT " (fh=%p)\n", FOBJECT_ARGS(fobject),
		      fhandle);

fop_failed:
	file_object_unref(fobject, 1);
	return res;
}

static int
fsdev_fchmodat(struct aio_fsdev *vfsdev, struct aio_fsdev_file_object *fobject, uint32_t mode)
{
	int res;

	res = fchmodat(fobject->fd, "", mode, AT_EMPTY_PATH);
	if (res == -1) {
		if (errno == EINVAL) {
			/* Linux only gained support for AT_EMPTY_PATH in fchmodat recently. We'll use a fallback option. */
			res = fchmodat(vfsdev->proc_self_fd, fobject->fd_str, mode, 0);
		}
	}

	return res;
}

static int
fsdev_aio_op_setattr(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	int res;
	struct aio_fsdev_file_object *fobject;
	struct aio_fsdev_file_handle *fhandle = NULL;
	struct fuse_setattr_in *setattr;
	uint32_t valid;
	struct fuse_out_header *out_hdr = fsdev_io->u_out.fuse.hdr;
	struct fuse_attr_out *attr_out = fsdev_io->u_out.fuse.op.attr;

	fobject = fsdev_io_get_aio_fobject(fsdev_io);
	if (!fobject) {
		SPDK_ERRLOG("Invalid fobject: %p\n", fobject);
		return -EINVAL;
	}

	setattr = fsdev_io->u_in.fuse.op.setattr;
	valid = setattr->valid;

	if (valid & FATTR_FH) {
		fhandle = fsdev_aio_get_fhandle_by_fuse_fh(vfsdev, setattr->fh);
		if (!fhandle) {
			SPDK_ERRLOG("Invalid fhandle: %" PRIx64 "\n", setattr->fh);
			res = -EINVAL;
			goto fop_failed;
		}
	}

	if (valid & FATTR_MODE) {
		if (fhandle) {
			res = fchmod(fhandle->fd, setattr->mode);
		} else {
			res = fsdev_fchmodat(vfsdev, fobject, setattr->mode);
		}
		if (res == -1) {
			res = -errno;
			SPDK_ERRLOG("fchmod failed for " FOBJECT_FMT " with %d\n", FOBJECT_ARGS(fobject), res);
			goto fop_failed;
		}
		fobject->mode = setattr->mode;
	}

	if (valid & (FATTR_UID | FATTR_GID)) {
		uid_t uid = (valid & FATTR_UID) ? setattr->uid : (uid_t) -1;
		gid_t gid = (valid & FATTR_GID) ? setattr->gid : (gid_t) -1;

		res = fchownat(fobject->fd, "", uid, gid, AT_EMPTY_PATH);
		if (res == -1) {
			res = -errno;
			SPDK_ERRLOG("fchownat failed for " FOBJECT_FMT " with %d\n", FOBJECT_ARGS(fobject), res);
			goto fop_failed;
		}
	}

	if (valid & FATTR_SIZE) {
		int truncfd;

		if (fhandle) {
			truncfd = fhandle->fd;
		} else {
			truncfd = openat(vfsdev->proc_self_fd, fobject->fd_str, O_RDWR);
			if (truncfd < 0) {
				res = -errno;
				SPDK_ERRLOG("openat failed for " FOBJECT_FMT " with %d\n", FOBJECT_ARGS(fobject), res);
				goto fop_failed;
			}
		}

		res = ftruncate(truncfd, setattr->size);
		if (!fhandle) {
			int saverr = errno;
			close(truncfd);
			errno = saverr;
		}
		if (res == -1) {
			res = -errno;
			SPDK_ERRLOG("ftruncate failed for " FOBJECT_FMT " (size=%" PRIu64 ")\n", FOBJECT_ARGS(fobject),
				    setattr->size);
			goto fop_failed;
		}
	}

	if (valid & (FATTR_ATIME | FATTR_MTIME)) {
		struct timespec tv[2];

		tv[0].tv_sec = 0;
		tv[1].tv_sec = 0;
		tv[0].tv_nsec = UTIME_OMIT;
		tv[1].tv_nsec = UTIME_OMIT;

		if (valid & FATTR_ATIME_NOW) {
			tv[0].tv_nsec = UTIME_NOW;
		} else if (valid & FATTR_ATIME) {
			tv[0].tv_sec = setattr->atime;
			tv[0].tv_nsec = setattr->atimensec;
		}

		if (valid & FATTR_MTIME_NOW) {
			tv[1].tv_nsec = UTIME_NOW;
		} else if (valid & FATTR_MTIME) {
			tv[1].tv_sec = setattr->mtime;
			tv[1].tv_nsec = setattr->mtimensec;
		}

		if (fhandle) {
			res = futimens(fhandle->fd, tv);
		} else {
			res = utimensat_empty(vfsdev, fobject, tv);
		}
		if (res == -1) {
			res = -errno;
			SPDK_ERRLOG("futimens/utimensat_empty failed for " FOBJECT_FMT " with %d\n",
				    FOBJECT_ARGS(fobject), res);
			goto fop_failed;
		}
	}

	res = fsdev_aio_fill_attr_out(fobject, attr_out);
	if (res) {
		SPDK_ERRLOG("fsdev_aio_fill_entry_out failed for " FOBJECT_FMT "\n",
			    FOBJECT_ARGS(fobject));
		goto fop_failed;
	}

	out_hdr->len += sizeof(struct fuse_attr_out);
	res = 0;
	SPDK_DEBUGLOG(fsdev_aio, "SETATTR succeeded for " FOBJECT_FMT "\n",
		      FOBJECT_ARGS(fobject));

fop_failed:
	file_object_unref(fobject, 1);
	return res;
}

static int
fsdev_aio_op_create(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	int fd = -1;
	int err;
	struct aio_fsdev_file_object *parent_fobject;
	const char *name = fsdev_aio_io_fuse_get_name(fsdev_io);
	uint32_t mode = fsdev_io->u_in.fuse.op.create->mode;
	uint32_t flags = fsdev_io->u_in.fuse.op.create->flags;
	uint32_t umask = fsdev_io->u_in.fuse.op.create->umask;
	struct fsdev_aio_cred old_cred, new_cred = {
		.euid = fsdev_io->u_in.fuse.hdr->uid,
		.egid = fsdev_io->u_in.fuse.hdr->gid,
	};
	struct aio_fsdev_file_handle *fhandle = NULL;
	struct aio_fsdev_file_object *fobject = NULL;
	struct spdk_fuse_create_out *create_out = fsdev_io->u_out.fuse.op.create;

	if (!is_safe_path_component(name)) {
		SPDK_ERRLOG("CREATE: %s not a safe component\n", name);
		return -EINVAL;
	}

	parent_fobject = fsdev_io_get_aio_fobject(fsdev_io);
	if (!parent_fobject) {
		SPDK_ERRLOG("Invalid parent_fobject: %p\n", parent_fobject);
		return -EINVAL;
	}

	err = fsdev_aio_change_cred(&new_cred, &old_cred);
	if (err) {
		SPDK_ERRLOG("CREATE: cannot change credentials\n");
		goto fop_failed;
	}

	flags = update_open_flags(vfsdev, flags);

	fd = openat(parent_fobject->fd, name, (flags | O_CREAT) & ~O_NOFOLLOW, (mode & ~umask));
	err = fd == -1 ? -errno : 0;
	fsdev_aio_restore_cred(&old_cred);

	if (err) {
		SPDK_ERRLOG("CREATE: openat failed with %d\n", err);
		goto fop_failed;
	}

	/* Fixup mode, openat() ignores some bits important for POSIX compliance. */
	err = fchmod(fd, (mode & ~umask));
	if (err == -1) {
		err = -errno;
		SPDK_ERRLOG("CREATE: lookup failed with %d\n", err);
		goto fop_failed;
	}

	memset(create_out, 0, sizeof(*create_out));
	err = fsdev_aio_do_lookup(vfsdev, parent_fobject, name, &fobject, &create_out->entry);
	if (err) {
		SPDK_ERRLOG("CREATE: lookup failed with %d\n", err);
		goto fop_failed;
	}
	assert(fobject != NULL);

	fhandle = file_handle_alloc(fobject, fd);
	if (!fhandle) {
		file_object_unref(fobject, 1);
		err = -ENOMEM;
		SPDK_ERRLOG("CREATE: failed to create a file handle (fd=%d) with %d\n",
			    fd, err);
		goto fop_failed;
	}

	SPDK_DEBUGLOG(fsdev_aio, "CREATE: succeeded (name=%s " FOBJECT_FMT " fh=%p)\n",
		      name, FOBJECT_ARGS(fobject), fhandle);

	fsdev_io->u_out.fuse.hdr->len += sizeof(*create_out);
	create_out->open.fh = fsdev_aio_get_fuse_fh(vfsdev, fhandle);
	create_out->entry.attr.mode = (mode & ~umask);

	err = 0;

fop_failed:
	if (err && fd != -1) {
		close(fd);
	}
	file_object_unref(parent_fobject, 1);
	return err;
}

static int
fsdev_aio_op_release(struct spdk_io_channel *_ch, struct spdk_fsdev_io *fsdev_io)
{
	return fsdev_aio_do_release(fsdev_io);
}

static inline void
fsdev_aio_prep_read_aio(struct aio_io_channel *ch, struct aio_fsdev_io *vfsdev_io,
			int fd, const struct iovec *iovs, uint32_t iovcnt, uint64_t offs)
{
	io_prep_preadv(&vfsdev_io->io, fd, iovs, iovcnt, offs);
	vfsdev_io->io.data = vfsdev_io;
	TAILQ_INSERT_TAIL(&ch->ios_for_submit, vfsdev_io, link);
}

#ifdef SPDK_CONFIG_URING
static inline int
fsdev_aio_prep_read_io_uring(struct aio_io_channel *ch, struct aio_fsdev_io *vfsdev_io,
			     int fd, const struct iovec *iovs, uint32_t iovcnt, uint64_t offs)
{
	struct io_uring_sqe *sqe;

	sqe = ch->u.uring.queue_ios ? NULL : io_uring_get_sqe(&ch->u.uring.io_ring);
	if (!sqe) {
		SPDK_DEBUGLOG(fsdev_aio, "No SQE available, IO queued for later\n");
		ch->u.uring.queue_ios = 1;
		return -EAGAIN;
	}

	io_uring_prep_readv(sqe, fd, iovs, iovcnt, offs);
	io_uring_sqe_set_data(sqe, vfsdev_io);

	TAILQ_INSERT_TAIL(&ch->ios_in_progress, vfsdev_io, link);
	ch->u.uring.io_count++;

	return 0;
}
#else
#define fsdev_aio_prep_read_io_uring(...)  ({ assert(0); 0; })
#endif

static int
fsdev_aio_op_read(struct spdk_io_channel *_ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	struct aio_io_channel *ch = spdk_io_channel_get_ctx(_ch);
	struct aio_fsdev_io *vfsdev_io = fsdev_to_aio_io(fsdev_io);
	struct aio_fsdev_file_handle *fhandle;
	size_t size = fsdev_io->u_in.fuse.op.read->size;
	uint64_t offs = fsdev_io->u_in.fuse.op.read->offset;
	uint32_t flags = fsdev_io->u_in.fuse.op.read->flags;
	struct iovec *iovs = fsdev_io->u_out.fuse.iov;
	uint32_t iovcnt = fsdev_io->u_out.fuse.iovcnt;

	/* we don't suport the memory domains at the moment */
	assert(!fsdev_io->u_in.fuse.memory_domain);

	UNUSED(size);
	UNUSED(flags);
	UNUSED(size);

	if (!iovs || !iovcnt) {
		SPDK_ERRLOG("bad outvec: iov=%p outcnt=%" PRIu32 "\n", iovs, iovcnt);
		return -EINVAL;
	}

	if (vfsdev->opts.skip_rw) {
		uint32_t i;

		vfsdev_io->status = 0;

		for (i = 0; i < iovcnt; i++, iovs++) {
			fsdev_io->u_out.fuse.hdr->len += iovs->iov_len;
		}

		TAILQ_INSERT_TAIL(&ch->ios_to_complete, vfsdev_io, link);

		return IO_STATUS_ASYNC;
	}

	fhandle = fsdev_aio_get_fhandle_by_fuse_fh(vfsdev, fsdev_io->u_in.fuse.op.read->fh);
	if (!fhandle) {
		SPDK_ERRLOG("Invalid fhandle: %p\n", fhandle);
		return -EINVAL;
	}

	SPDK_DEBUGLOG(fsdev_aio, "read: fd=%d offs=%" PRIu64 " size=%" PRIu64 " iovcnt=%" PRIu32 "\n",
		      fhandle->fd, offs, size, iovcnt);

	vfsdev_io->data_size = 0;

	if (aio_fsdev_use_io_uring_rdwr()) {
		if (fsdev_aio_prep_read_io_uring(ch, vfsdev_io, fhandle->fd, iovs, iovcnt, offs) == -EAGAIN) {
			TAILQ_INSERT_TAIL(&ch->ios_for_submit, vfsdev_io, link);
		}
	} else {
		fsdev_aio_prep_read_aio(ch, vfsdev_io, fhandle->fd, iovs, iovcnt, offs);
	}

	return IO_STATUS_ASYNC;
}

static int
clear_suid_sgid(struct aio_fsdev_io *vfsdev_io)
{
	struct spdk_fsdev_io *fsdev_io = aio_to_fsdev_io(vfsdev_io);
	struct aio_fsdev_file_object *fobject;
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	mode_t new_mode;
	int fd, error;

	fobject = fsdev_aio_get_fobject_by_nodeid(vfsdev, fsdev_io->u_in.fuse.op.write->fh);
	if (!fobject) {
		return 0;
	}

	new_mode = fobject->mode & ~S_ISUID;
	if (fobject->mode & S_IXGRP) {
		new_mode &= ~S_ISGID;
	}

	if (fobject->mode == new_mode) {
		error = 0;
		goto out;
	}

	fd = openat(vfsdev->proc_self_fd, fobject->fd_str, O_RDWR);
	if (fd == -1) {
		error = -errno;
		goto out;
	}

	error = fchmod(fd, new_mode);
	if (error == -1) {
		error = -errno;
		SPDK_ERRLOG("Failed to fchmod(%d, %o) with err=%d\n", fd, new_mode, error);
	}
	close(fd);

out:
	file_object_unref(fobject, 1);
	if (error != 0) {
		SPDK_ERRLOG("Failed to clear suid/sgid on successfull "
			    "write with err=%d - ignoriing\n", error);
	}
	return error;
}

static inline void
fsdev_aio_prep_write_aio(struct aio_io_channel *ch, struct aio_fsdev_io *vfsdev_io,
			 int fd, const struct iovec *iovs, uint32_t iovcnt, uint64_t offs)
{
	io_prep_pwritev(&vfsdev_io->io, fd, iovs, iovcnt, offs);
	vfsdev_io->io.data = vfsdev_io;
	TAILQ_INSERT_TAIL(&ch->ios_for_submit, vfsdev_io, link);
}

#ifdef SPDK_CONFIG_URING
static inline int
fsdev_aio_prep_write_io_uring(struct aio_io_channel *ch, struct aio_fsdev_io *vfsdev_io,
			      int fd, const struct iovec *iovs, uint32_t iovcnt, uint64_t offs)
{
	struct io_uring_sqe *sqe;

	sqe = ch->u.uring.queue_ios ? NULL : io_uring_get_sqe(&ch->u.uring.io_ring);
	if (!sqe) {
		SPDK_DEBUGLOG(fsdev_aio, "No SQE available, IO queued for later\n");
		ch->u.uring.queue_ios = 1;
		return -EAGAIN;
	}

	io_uring_prep_writev(sqe, fd, iovs, iovcnt, offs);
	io_uring_sqe_set_data(sqe, vfsdev_io);

	TAILQ_INSERT_TAIL(&ch->ios_in_progress, vfsdev_io, link);
	ch->u.uring.io_count++;

	return 0;
}
#else
#define fsdev_aio_prep_write_io_uring(...) ({ assert(0); 0; })
#endif

static int
fsdev_aio_op_write(struct spdk_io_channel *_ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	struct aio_io_channel *ch = spdk_io_channel_get_ctx(_ch);
	struct aio_fsdev_io *vfsdev_io = fsdev_to_aio_io(fsdev_io);
	struct aio_fsdev_file_handle *fhandle;
	size_t size = fsdev_io->u_in.fuse.op.write->size;
	uint64_t offs = fsdev_io->u_in.fuse.op.write->offset;
	uint32_t flags = fsdev_io->u_in.fuse.op.write->flags;
	const struct iovec *iovs = fsdev_io->u_in.fuse.iov;
	uint32_t iovcnt =  fsdev_io->u_in.fuse.iovcnt;

	/* we don't suport the memory domains at the moment */
	assert(!fsdev_io->u_in.fuse.memory_domain);

	UNUSED(size);
	UNUSED(flags);
	UNUSED(size);

	if (!iovcnt || !iovs) { /* there should be at least one iovec with data */
		SPDK_ERRLOG("bad invec: iov=%p cnt=%" PRIu32 "\n", iovs, iovcnt);
		return -EINVAL;
	}

	if (vfsdev->opts.skip_rw) {
		uint32_t i;
		struct fuse_out_header *out_hdr = fsdev_io->u_out.fuse.hdr;

		out_hdr->len += sizeof(struct fuse_write_out);
		fsdev_io->u_out.fuse.op.write->size = 0;
		vfsdev_io->status = 0;

		for (i = 0; i < iovcnt; i++, iovs++) {
			fsdev_io->u_out.fuse.op.write->size += iovs->iov_len;
		}

		TAILQ_INSERT_TAIL(&ch->ios_to_complete, vfsdev_io, link);

		return IO_STATUS_ASYNC;
	}

	fhandle = fsdev_aio_get_fhandle_by_fuse_fh(vfsdev, fsdev_io->u_in.fuse.op.write->fh);
	if (!fhandle) {
		SPDK_ERRLOG("Invalid fhandle: %p\n", fhandle);
		return -EINVAL;
	}

	SPDK_DEBUGLOG(fsdev_aio, "write: fd=%d offs=%" PRIu64 " size=%" PRIu64 " iovcnt=%" PRIu32 "\n",
		      fhandle->fd, offs, size, iovcnt);

	vfsdev_io->data_size = 0;

	if (aio_fsdev_use_io_uring_rdwr()) {
		if (fsdev_aio_prep_write_io_uring(ch, vfsdev_io, fhandle->fd, iovs, iovcnt, offs) == -EAGAIN) {
			TAILQ_INSERT_TAIL(&ch->ios_for_submit, vfsdev_io, link);
		}
	} else {
		fsdev_aio_prep_write_aio(ch, vfsdev_io, fhandle->fd, iovs, iovcnt, offs);
	}

	return IO_STATUS_ASYNC;
}

static int
fsdev_aio_op_readlink(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	int res;
	struct aio_fsdev_file_object *fobject;
	struct iovec *out_iov = &fsdev_io->u_out.fuse.iov[0];
	char *buf = out_iov->iov_base;

	fobject = fsdev_io_get_aio_fobject(fsdev_io);
	if (!fobject) {
		SPDK_ERRLOG("Invalid fobject: %p\n", fobject);
		return -EINVAL;
	}

	res = readlinkat(fobject->fd, "", buf, out_iov->iov_len);
	if (res == -1) {
		res = -errno;
		SPDK_ERRLOG("readlinkat failed for " FOBJECT_FMT " with %d\n",
			    FOBJECT_ARGS(fobject), res);
		goto fop_failed;
	}

	if (((uint32_t)res) == out_iov->iov_len) {
		SPDK_ERRLOG("buffer is too short\n");
		res = -ENAMETOOLONG;
		goto fop_failed;
	}

	buf[res] = 0;
	fsdev_io->u_out.fuse.hdr->len += res + 1;
	res = 0;

fop_failed:
	file_object_unref(fobject, 1);
	return res;
}

static int
fsdev_aio_op_statfs(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	int res;
	struct aio_fsdev_file_object *fobject;
	struct statvfs stbuf;
	struct fuse_statfs_out *statfs_out = fsdev_io->u_out.fuse.op.statfs;
	struct fuse_out_header *out_hdr = fsdev_io->u_out.fuse.hdr;

	fobject = fsdev_io_get_aio_fobject(fsdev_io);
	if (!fobject) {
		SPDK_ERRLOG("Invalid fobject: %p\n", fobject);
		return -EINVAL;
	}

	res = fstatvfs(fobject->fd, &stbuf);
	if (res == -1) {
		res = -errno;
		SPDK_ERRLOG("fstatvfs failed with %d\n", res);
		goto fop_failed;
	}

	memset(statfs_out, 0, sizeof(*statfs_out));
	statfs_out->st.blocks = stbuf.f_blocks;
	statfs_out->st.bfree = stbuf.f_bfree;
	statfs_out->st.bavail = stbuf.f_bavail;
	statfs_out->st.files = stbuf.f_files;
	statfs_out->st.ffree = stbuf.f_ffree;
	statfs_out->st.bsize = stbuf.f_bsize;
	statfs_out->st.namelen = stbuf.f_namemax;
	statfs_out->st.frsize = stbuf.f_frsize;

	out_hdr->len += sizeof(*statfs_out);
	res = 0;

fop_failed:
	file_object_unref(fobject, 1);
	return res;
}

static int
fsdev_aio_mknod_symlink(struct spdk_fsdev_io *fsdev_io, const char *name, mode_t mode, dev_t rdev,
			const char *link, uint32_t umask)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	struct aio_fsdev_file_object *parent_fobject;
	struct aio_fsdev_file_object *fobject = NULL;
	int res;
	int saverr;
	struct fsdev_aio_cred old_cred, new_cred = {
		.euid = fsdev_io->u_in.fuse.hdr->uid,
		.egid = fsdev_io->u_in.fuse.hdr->gid,
	};
	struct fuse_out_header *out_hdr = fsdev_io->u_out.fuse.hdr;
	struct fuse_entry_out *entry_out = fsdev_io->u_out.fuse.op.entry;

	if (!is_safe_path_component(name)) {
		SPDK_ERRLOG("%s isn't safe\n", name);
		return -EINVAL;
	}

	parent_fobject = fsdev_io_get_aio_fobject(fsdev_io);
	if (!parent_fobject) {
		SPDK_ERRLOG("Invalid parent_fobject: %p\n", parent_fobject);
		return -EINVAL;
	}

	res = fsdev_aio_change_cred(&new_cred, &old_cred);
	if (res) {
		SPDK_ERRLOG("cannot change cred (err=%d)\n", res);
		goto fop_failed;
	}

	if (S_ISDIR(mode)) {
		res = mkdirat(parent_fobject->fd, name, (mode & ~umask));
	} else if (S_ISLNK(mode)) {
		if (link) {
			res = symlinkat(link, parent_fobject->fd, name);
		} else {
			SPDK_ERRLOG("NULL link pointer\n");
			errno = EINVAL;
		}
	} else if (S_ISFIFO(mode)) {
		res = mkfifoat(parent_fobject->fd, name, (mode & ~umask));
	} else {
		res = mknodat(parent_fobject->fd, name, (mode & ~umask), rdev);
	}
	saverr = -errno;

	fsdev_aio_restore_cred(&old_cred);

	if (res == -1) {
		SPDK_ERRLOG("cannot mkdirat/symlinkat/mknodat (err=%d)\n", saverr);
		res = saverr;
		goto fop_failed;
	}

	res = fsdev_aio_do_lookup(vfsdev, parent_fobject, name, &fobject, entry_out);
	if (res) {
		SPDK_ERRLOG("lookup failed (err=%d)\n", res);
		goto fop_failed;
	}
	assert(fobject != NULL);
	/*
	 * Fixup the mode, functions creating files above ignore some bits important
	 * for POSIX compliance.
	 */
	if (!S_ISLNK(mode)) {
		res = fsdev_fchmodat(vfsdev, fobject, (mode & ~umask));
		if (res == -1) {
			res = -errno;
			SPDK_ERRLOG("fsdev_aio_mknod_symlink mode fixup failed with %d\n", res);
			file_object_unref(fobject, 1);
			goto fop_failed;
		}
		entry_out->attr.mode = (mode & ~umask);
	}

	SPDK_DEBUGLOG(fsdev_aio, "fsdev_aio_mknod_symlink(%s " FOBJECT_FMT ") -> " FOBJECT_FMT ")\n",
		      name, FOBJECT_ARGS(parent_fobject), FOBJECT_ARGS(fobject));

	out_hdr->len += sizeof(*entry_out);
	res = 0;

fop_failed:
	file_object_unref(parent_fobject, 1);
	return res;
}

static int
fsdev_aio_op_mknod(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	const char *name = fsdev_aio_io_fuse_get_name(fsdev_io);
	mode_t mode = fsdev_io->u_in.fuse.op.mknod->mode;
	uint32_t umask = fsdev_io->u_in.fuse.op.mknod->umask;
	dev_t rdev = fsdev_io->u_in.fuse.op.mknod->rdev;

	if (strnlen(name, PATH_MAX + 1) == PATH_MAX + 1) {
		return -ENAMETOOLONG;
	}

	return fsdev_aio_mknod_symlink(fsdev_io, name, mode, rdev, NULL, umask);
}

static int
fsdev_aio_op_mkdir(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	const char *name = fsdev_aio_io_fuse_get_name(fsdev_io);
	mode_t mode = fsdev_io->u_in.fuse.op.mkdir->mode;
	uint32_t umask = fsdev_io->u_in.fuse.op.mkdir->umask;

	if (strnlen(name, PATH_MAX + 1) == PATH_MAX + 1) {
		return -ENAMETOOLONG;
	}

	return fsdev_aio_mknod_symlink(fsdev_io, name, S_IFDIR | mode, 0, NULL, umask);
}

static int
fsdev_aio_op_symlink(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	const char *target = fsdev_aio_io_fuse_get_name(fsdev_io);
	size_t len;
	const char *linkpath;

	len = strnlen(target, PATH_MAX + 1);
	if (len == PATH_MAX + 1) {
		return -ENAMETOOLONG;
	}
	linkpath = target + len + 1;
	if (strnlen(linkpath, PATH_MAX + 1) == PATH_MAX + 1) {
		return -ENAMETOOLONG;
	}

	return fsdev_aio_mknod_symlink(fsdev_io, target, S_IFLNK, 0, linkpath, 0);
}

static int
fsdev_aio_do_unlink(struct aio_fsdev *vfsdev, struct aio_fsdev_file_object *parent_fobject,
		    const char *name, bool is_dir)
{
	/* fobject must be initialized to avoid a scan-build false positive */
	struct aio_fsdev_file_object *fobject = NULL;
	int res;

	if (!parent_fobject) {
		SPDK_ERRLOG("Invalid parent_fobject: %p\n", parent_fobject);
		return -EINVAL;
	}

	if (!is_safe_path_component(name)) {
		SPDK_ERRLOG("%s isn't safe\n", name);
		return -EINVAL;
	}

	res = fsdev_aio_do_lookup(vfsdev, parent_fobject, name, &fobject, NULL);
	if (res) {
		SPDK_ERRLOG("can't find '%s' under " FOBJECT_FMT "\n", name, FOBJECT_ARGS(parent_fobject));
		return -EIO;
	}

	res = unlinkat(parent_fobject->fd, name, is_dir ? AT_REMOVEDIR : 0);
	if (res) {
		res = -errno;
		SPDK_WARNLOG("unlinkat(" FOBJECT_FMT " %s) failed (err=%d)\n",
			     FOBJECT_ARGS(parent_fobject), name, res);
	}

	file_object_unref(fobject, 1);
	return res;
}

static int
fsdev_aio_op_unlink(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	struct aio_fsdev_file_object *parent_fobject;
	const char *name = fsdev_aio_io_fuse_get_name(fsdev_io);
	int res;

	parent_fobject = fsdev_io_get_aio_fobject(fsdev_io);
	if (!parent_fobject) {
		SPDK_ERRLOG("Invalid fobject: %p\n", parent_fobject);
		return -EINVAL;
	}

	res = fsdev_aio_do_unlink(vfsdev, parent_fobject, name, false);
	file_object_unref(parent_fobject, 1);
	return res;
}

static int
fsdev_aio_op_rmdir(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	struct aio_fsdev_file_object *parent_fobject;
	const char *name = fsdev_aio_io_fuse_get_name(fsdev_io);
	int res;

	parent_fobject = fsdev_io_get_aio_fobject(fsdev_io);
	if (!parent_fobject) {
		SPDK_ERRLOG("Invalid fobject: %p\n", parent_fobject);
		return -EINVAL;
	}

	res = fsdev_aio_do_unlink(vfsdev, parent_fobject, name, true);
	file_object_unref(parent_fobject, 1);
	return res;
}

static int
fsdev_aio_do_rename(struct spdk_fsdev_io *fsdev_io, uint64_t newdir, uint32_t flags)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	int res;
	/* old_fobject must be initialized to avoid a scan-build false positive */
	struct aio_fsdev_file_object *old_fobject = NULL;
	struct aio_fsdev_file_object *parent_fobject;
	const char *name = fsdev_aio_io_fuse_get_name(fsdev_io);
	struct aio_fsdev_file_object *new_parent_fobject;
	const char *new_name;
	size_t namelen;

	namelen = strnlen(name, PATH_MAX + 1);
	if (namelen == PATH_MAX + 1) {
		return -ENAMETOOLONG;
	}

	new_name = name + namelen + 1;
	if (strnlen(new_name, PATH_MAX + 1) == PATH_MAX + 1) {
		return -ENAMETOOLONG;
	}

	if (!is_safe_path_component(name)) {
		SPDK_ERRLOG("name '%s' isn't safe\n", name);
		return -EINVAL;
	}

	if (!is_safe_path_component(new_name)) {
		SPDK_ERRLOG("newname '%s' isn't safe\n", new_name);
		return -EINVAL;
	}

	parent_fobject = fsdev_io_get_aio_fobject(fsdev_io);
	if (!parent_fobject) {
		SPDK_ERRLOG("Invalid parent_fobject\n");
		return -EINVAL;
	}

	new_parent_fobject = fsdev_aio_get_fobject_by_nodeid(vfsdev, newdir);
	if (!new_parent_fobject) {
		SPDK_ERRLOG("Invalid new_parent_fobject\n");
		res = -EINVAL;
		goto bad_new_parent_fobject;
	}

	res = fsdev_aio_do_lookup(vfsdev, parent_fobject, name, &old_fobject, NULL);
	if (res) {
		SPDK_ERRLOG("can't find '%s' under " FOBJECT_FMT "\n", name, FOBJECT_ARGS(parent_fobject));
		res = -EIO;
		goto fop_failed;
	}

	if (flags) {
#ifndef SYS_renameat2
		SPDK_ERRLOG("flags are not supported\n");
		res = -ENOTSUP;
		goto fop_failed;
#else
		res = syscall(SYS_renameat2, parent_fobject->fd, name, new_parent_fobject->fd,
			      new_name, flags);
		if (res == -1 && errno == ENOSYS) {
			SPDK_ERRLOG("SYS_renameat2 returned ENOSYS\n");
			res = -ENOSYS;
			goto fop_failed;
		} else if (res == -1) {
			res = -errno;
			SPDK_ERRLOG("SYS_renameat2 failed (err=%d))\n", res);
			goto fop_failed;
		}
#endif
	} else {
		res = renameat(parent_fobject->fd, name, new_parent_fobject->fd, new_name);
		if (res == -1) {
			res = -errno;
			SPDK_ERRLOG("renameat failed (err=%d)\n", res);
			goto fop_failed;
		}
	}

	file_object_unref(old_fobject, 1);
	res = 0;

fop_failed:
	file_object_unref(new_parent_fobject, 1);
bad_new_parent_fobject:
	file_object_unref(parent_fobject, 1);
	return res;
}

static int
fsdev_aio_op_rename(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	uint64_t newdir = fsdev_io->u_in.fuse.op.rename->newdir;

	return fsdev_aio_do_rename(fsdev_io, newdir, 0);
}

static int
fsdev_aio_op_rename2(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	uint32_t flags = fsdev_io->u_in.fuse.op.rename2->flags;
	uint64_t newdir = fsdev_io->u_in.fuse.op.rename2->newdir;

	return fsdev_aio_do_rename(fsdev_io, newdir, flags);
}

static int
fsdev_aio_op_link(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	int res;
	struct aio_fsdev_file_object *fobject;
	struct aio_fsdev_file_object *new_parent_fobject;
	const char *name = fsdev_aio_io_fuse_get_name(fsdev_io);
	struct aio_fsdev_file_object *link_fobject = NULL;
	uint64_t oldnodeid = fsdev_io->u_in.fuse.op.link->oldnodeid;
	struct fuse_out_header *out_hdr = fsdev_io->u_out.fuse.hdr;
	struct fuse_entry_out *entry_out = fsdev_io->u_out.fuse.op.entry;

	if (!is_safe_path_component(name)) {
		SPDK_ERRLOG("%s is not a safe component\n", name);
		return -EINVAL;
	}

	fobject = fsdev_aio_get_fobject_by_nodeid(vfsdev, oldnodeid);
	if (!fobject) {
		SPDK_ERRLOG("Invalid fobject: %p\n", fobject);
		return -EINVAL;
	}

	new_parent_fobject = fsdev_io_get_aio_fobject(fsdev_io);
	if (!new_parent_fobject) {
		SPDK_ERRLOG("Invalid new_parent_fobject: %p\n", new_parent_fobject);
		res = -EINVAL;
		goto bad_new_parent_fobject;
	}

	res = linkat(fobject->fd, "", new_parent_fobject->fd, name, AT_EMPTY_PATH);
	if (res == -1) {
		res = -errno;
		SPDK_ERRLOG("linkat failed " FOBJECT_FMT " -> " FOBJECT_FMT " name=%s (err=%d)\n",
			    FOBJECT_ARGS(fobject), FOBJECT_ARGS(new_parent_fobject), name, res);
		goto fop_failed;
	}

	res = fsdev_aio_do_lookup(vfsdev, new_parent_fobject, name, &link_fobject, entry_out);
	if (res) {
		SPDK_ERRLOG("lookup failed (err=%d)\n", res);
		goto fop_failed;
	}

	out_hdr->len += sizeof(*entry_out);
	res = 0;

	SPDK_DEBUGLOG(fsdev_aio, "LINK succeeded for " FOBJECT_FMT " -> " FOBJECT_FMT " name=%s\n",
		      FOBJECT_ARGS(fobject), FOBJECT_ARGS(link_fobject), name);

fop_failed:
	file_object_unref(new_parent_fobject, 1);
bad_new_parent_fobject:
	file_object_unref(fobject, 1);
	return res;
}

static int
fsdev_aio_op_fsync(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	int res;
	struct aio_fsdev_file_object *fobject;
	struct aio_fsdev_file_handle *fhandle;
	struct fuse_fsync_in *fsync_in = fsdev_io->u_in.fuse.op.fsync;
	bool datasync = fsync_in->fsync_flags & FUSE_FSYNC_FDATASYNC;

	fobject = fsdev_io_get_aio_fobject(fsdev_io);
	if (!fobject) {
		SPDK_ERRLOG("Invalid fobject: %p\n", fobject);
		return -EINVAL;
	}

	fhandle = fsdev_aio_get_fhandle_by_fuse_fh(vfsdev, fsync_in->fh);
	if (!fhandle) {
		SPDK_ERRLOG("Invalid fhandle: 0x%" PRIx64 "\n", fsync_in->fh);
		res = -EINVAL;
		goto fop_failed;
	}

	if (datasync) {
		res = fdatasync(fhandle->fd);
	} else {
		res = fsync(fhandle->fd);
	}

	if (res == -1) {
		res = -errno;
		SPDK_ERRLOG("fdatasync/fsync failed for " FOBJECT_FMT " fh=%p (err=%d)\n",
			    FOBJECT_ARGS(fobject), fhandle, res);
		goto fop_failed;
	}

	SPDK_DEBUGLOG(fsdev_aio, "FSYNC succeeded for " FOBJECT_FMT " fh=%p\n",
		      FOBJECT_ARGS(fobject), fhandle);

	res = 0;

fop_failed:
	file_object_unref(fobject, 1);

	return res;
}

static int
fsdev_aio_op_setxattr(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	int res, fd;
	struct aio_fsdev_file_object *fobject;
	struct fuse_setxattr_in *setxattr_in = fsdev_io->u_in.fuse.op.setxattr;
	size_t setxattr_in_size;
	const char *name;
	size_t namelen;
	const char *value;
	uint32_t size = setxattr_in->size;
	uint64_t flags = setxattr_in->flags;
	static const char *acl_access_name = "system.posix_acl_access";

	if (!vfsdev->opts.xattr_enabled) {
		SPDK_INFOLOG(fsdev_aio, "xattr is disabled by config\n");
		return -ENOSYS;
	}

	setxattr_in_size = (vfsdev->mount_opts.flags & FUSE_SETXATTR_EXT) ?
			   sizeof(*setxattr_in) : FUSE_COMPAT_SETXATTR_IN_SIZE;

	name = (char *)(setxattr_in) + setxattr_in_size;
	namelen = strnlen(name, MAX_SETXATTR_BUF_SIZE); /* TODO: what is the max name length? */
	if (namelen == MAX_SETXATTR_BUF_SIZE) {
		return -ENAMETOOLONG;
	}

	value = name + namelen + 1;
	if (strnlen(value, MAX_SETXATTR_BUF_SIZE - namelen - 1) == MAX_SETXATTR_BUF_SIZE - namelen - 1) {
		return -ENAMETOOLONG;
	}

	fobject = fsdev_io_get_aio_fobject(fsdev_io);
	if (!fobject) {
		SPDK_ERRLOG("Invalid fobject: %p\n", fobject);
		return -EINVAL;
	}

	fd = openat(vfsdev->proc_self_fd, fobject->fd_str, O_RDONLY);
	if (fd == -1) {
		res = -errno;
		SPDK_ERRLOG("openat failed (errno=%d)\n", -res);
		goto fop_failed;
	}

	res = fsetxattr(fd, name, value, size, flags);
	if (res == -1) {
		res = -errno;
		if (res == -ENOTSUP) {
			SPDK_INFOLOG(fsdev_aio, "fsetxattr: extended attributes are not supported or disabled\n");
		} else {
			SPDK_ERRLOG("fsetxattr failed with errno=%d\n", res);
		}
		goto fop_failed;
	}

	/* Clear SGID when system.posix_acl_access is set. */
	if ((vfsdev->mount_opts.flags & FUSE_SETXATTR_EXT) && (flags & FUSE_SETXATTR_ACL_KILL_SGID) &&
	    !strcmp(name, acl_access_name)) {
		struct fuse_entry_out entry_out = {};
		mode_t new_mode;

		res = fsdev_aio_fill_entry_out(fobject, &entry_out);
		if (res) {
			SPDK_ERRLOG("Failed to get file attrs for cleaning SGID on behalf of changed "
				    "\"%s\" with error=%d - ignoring.\n", acl_access_name, res);
			goto fop_failed;
		}

		new_mode = entry_out.attr.mode & ~S_ISGID;
		res = fchmod(fobject->fd, new_mode);
		if (res == -1) {
			SPDK_WARNLOG("Failed to clean SGID on behalf of changed '%s' with errno=%d - ignoring.\n",
				     acl_access_name, -errno);
		}
	}

	res = 0;

	SPDK_DEBUGLOG(fsdev_aio,
		      "SETXATTR succeeded for " FOBJECT_FMT " name=%s value=%s size=%" PRIu32 " flags=0x%lx" PRIx64 "\n",
		      FOBJECT_ARGS(fobject), name, value, size, flags);

fop_failed:
	if (fd != -1) {
		close(fd);
	}
	file_object_unref(fobject, 1);
	return res;
}

static int
fsdev_aio_op_getxattr(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	int res, fd;
	struct aio_fsdev_file_object *fobject;
	struct fuse_getxattr_in *getxattr_in = fsdev_io->u_in.fuse.op.getxattr;
	const char *name;
	struct iovec *out_iov = &fsdev_io->u_out.fuse.iov[0];
	void *buffer = out_iov->iov_base;
	size_t size = getxattr_in->size;
	ssize_t value_size;

	if (!vfsdev->opts.xattr_enabled) {
		SPDK_INFOLOG(fsdev_aio, "xattr is disabled by config\n");
		return -ENOSYS;
	}

	name = (char *)(getxattr_in) + sizeof(*getxattr_in);
	if (strnlen(name, MAX_GETXATTR_BUF_SIZE) == MAX_GETXATTR_BUF_SIZE) {
		SPDK_ERRLOG("Invalid name\n");
		return -EINVAL;
	}

	fobject = fsdev_io_get_aio_fobject(fsdev_io);
	if (!fobject) {
		SPDK_ERRLOG("Invalid fobject: %p\n", fobject);
		return -EINVAL;
	}

	fd = openat(vfsdev->proc_self_fd, fobject->fd_str, O_RDONLY);
	if (fd == -1) {
		res = -errno;
		SPDK_ERRLOG("openat failed (errno=%d)\n", -res);
		goto fop_failed;
	}

	value_size = fgetxattr(fd, name, buffer, size);
	if (value_size == -1) {
		res = -errno;
		if (res == -ENODATA) {
			SPDK_INFOLOG(fsdev_aio, "getxattr: no extended attribute '%s' found\n", name);
		} else if (res == -ENOTSUP) {
			SPDK_INFOLOG(fsdev_aio, "getxattr: extended attributes are not supported or disabled\n");
		} else {
			SPDK_ERRLOG("getxattr failed with errno=%d\n", res);
		}

		goto fop_failed;
	}

	fsdev_io->u_out.fuse.hdr->len += value_size;
	res = 0;
	SPDK_DEBUGLOG(fsdev_aio,
		      "GETXATTR succeeded for " FOBJECT_FMT " name=%s value=%s value_size=%zd\n",
		      FOBJECT_ARGS(fobject), name, (char *)buffer, value_size);

fop_failed:
	if (fd != -1) {
		close(fd);
	}
	file_object_unref(fobject, 1);
	return res;
}

static int
fsdev_aio_op_listxattr(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	ssize_t data_size;
	int res, fd;
	struct aio_fsdev_file_object *fobject;
	struct fuse_getxattr_in *getxattr_in = fsdev_io->u_in.fuse.op.getxattr;
	struct iovec *out_iov = &fsdev_io->u_out.fuse.iov[0];
	void *buffer = out_iov->iov_base;
	size_t size = getxattr_in->size;

	if (!vfsdev->opts.xattr_enabled) {
		SPDK_INFOLOG(fsdev_aio, "xattr is disabled by config\n");
		return -ENOSYS;
	}

	fobject = fsdev_io_get_aio_fobject(fsdev_io);
	if (!fobject) {
		SPDK_ERRLOG("Invalid fobject: %p\n", fobject);
		return -EINVAL;
	}

	fd = openat(vfsdev->proc_self_fd, fobject->fd_str, O_RDONLY);
	if (fd == -1) {
		res = -errno;
		SPDK_ERRLOG("openat failed (errno=%d)\n", -res);
		goto fop_failed;
	}

	data_size = flistxattr(fd, buffer, size);
	if (data_size == -1) {
		res = -errno;
		if (res == -ENOTSUP) {
			SPDK_INFOLOG(fsdev_aio, "listxattr: extended attributes are not supported or disabled\n");
		} else {
			SPDK_ERRLOG("listxattr failed with errno=%d\n", res);
		}
		goto fop_failed;
	}

	fsdev_io->u_out.fuse.hdr->len += data_size;
	res = 0;
	SPDK_DEBUGLOG(fsdev_aio, "LISTXATTR succeeded for " FOBJECT_FMT " data_size=%zu\n",
		      FOBJECT_ARGS(fobject), data_size);

fop_failed:
	if (fd != -1) {
		close(fd);
	}
	file_object_unref(fobject, 1);
	return res;
}

static int
fsdev_aio_op_removexattr(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	int res, fd;
	struct aio_fsdev_file_object *fobject;
	const char *name = fsdev_aio_io_fuse_get_name(fsdev_io);

	if (!vfsdev->opts.xattr_enabled) {
		SPDK_INFOLOG(fsdev_aio, "xattr is disabled by config\n");
		return -ENOSYS;
	}

	fobject = fsdev_io_get_aio_fobject(fsdev_io);
	if (!fobject) {
		SPDK_ERRLOG("Invalid fobject: %p\n", fobject);
		return -EINVAL;
	}

	fd = openat(vfsdev->proc_self_fd, fobject->fd_str, O_RDONLY);
	if (fd == -1) {
		res = -errno;
		SPDK_ERRLOG("openat failed (errno=%d)\n", -res);
		goto fop_failed;
	}

	res = fremovexattr(fd, name);
	if (res == -1) {
		res = -errno;
		if (res == -ENODATA) {
			SPDK_INFOLOG(fsdev_aio, "removexattr: no extended attribute '%s' found\n", name);
		} else if (res == -ENOTSUP) {
			SPDK_INFOLOG(fsdev_aio, "removexattr: extended attributes are not supported or disabled\n");
		} else {
			SPDK_ERRLOG("removexattr failed with errno=%d\n", res);
		}
		goto fop_failed;
	}

	res = 0;
	SPDK_DEBUGLOG(fsdev_aio, "REMOVEXATTR succeeded for " FOBJECT_FMT " name=%s\n",
		      FOBJECT_ARGS(fobject), name);

fop_failed:
	if (fd != -1) {
		close(fd);
	}
	file_object_unref(fobject, 1);
	return res;
}

static int
fsdev_aio_do_fallocate(struct aio_fsdev_file_handle *fhandle, uint32_t mode,
		       uint64_t offset, uint64_t length)
{
	int res;

#ifdef __linux__
	res = fallocate(fhandle->fd, mode, offset, length);

	/* Standard errno-based error handling. */
	if (res == -1) {
		res = -errno;
	}
#else
	res = posix_fallocate(fhandle->fd, offset, length);

	/*
	 * posix_fallocate() returns positive error without
	 * setting errno.
	 */
	if (res) {
		res = -res;
	}
#endif
	return res;
}

static int
fsdev_aio_op_fallocate(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	int res;
	struct fuse_fallocate_in *fallocate_in = fsdev_io->u_in.fuse.op.fallocate;
	struct aio_fsdev_file_object *fobject;
	struct aio_fsdev_file_handle *fhandle;
	uint32_t mode = fallocate_in->mode;
	uint64_t offset  = fallocate_in->offset;
	uint64_t length = fallocate_in->length;

#ifndef __linux__
	if (mode) {
		SPDK_ERRLOG("non-zero mode is not suppored\n");
		return -EINVAL;
	}
#endif
	fobject = fsdev_io_get_aio_fobject(fsdev_io);
	if (!fobject) {
		SPDK_ERRLOG("Invalid fobject: %p\n", fobject);
		return -EINVAL;
	}

	fhandle = fsdev_aio_get_fhandle_by_fuse_fh(vfsdev, fallocate_in->fh);
	if (!fhandle) {
		SPDK_ERRLOG("Invalid fhandle: %p\n", fhandle);
		res = -EINVAL;
		goto fop_failed;
	}

	res = fsdev_aio_do_fallocate(fhandle, mode, offset, length);
	if (res) {
		SPDK_ERRLOG("fallocate failed for fh=%p with err=%d\n",
			    fhandle, res);
		goto fop_failed;
	}

	SPDK_DEBUGLOG(fsdev_aio,
		      "FALLOCATE returns %d for " FOBJECT_FMT " fh=%p offset=%" PRIu64 " length=%" PRIu64 "\n",
		      res, FOBJECT_ARGS(fobject), fhandle, offset, length);
	res = 0;

fop_failed:
	file_object_unref(fobject, 1);
	return res;
}

static int
fsdev_aio_op_copy_file_range(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
#ifdef SPDK_CONFIG_COPY_FILE_RANGE
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	ssize_t res;
	int saverr = 0;
	struct fuse_copy_file_range_in *copy_file_range_in = fsdev_io->u_in.fuse.op.copy_file_range;
	struct aio_fsdev_file_object *fobject_in;
	struct aio_fsdev_file_handle *fhandle_in;
	off_t off_in = copy_file_range_in->off_in;
	struct aio_fsdev_file_object *fobject_out;
	struct aio_fsdev_file_handle *fhandle_out;
	off_t off_out = copy_file_range_in->off_out;
	size_t len = copy_file_range_in->len;
	uint32_t flags = copy_file_range_in->flags;
	struct fuse_out_header *out_hdr = fsdev_io->u_out.fuse.hdr;
	struct fuse_write_out *write_out = fsdev_io->u_out.fuse.op.write;

	if (vfsdev->opts.disable_copy_file_range) {
		SPDK_ERRLOG("copy_file_range is disabled by config\n");
		return -ENOSYS;
	}

	fobject_in = fsdev_io_get_aio_fobject(fsdev_io);
	if (!fobject_in) {
		SPDK_ERRLOG("Invalid fobject_in\n");
		return -EINVAL;
	}

	fhandle_in = fsdev_aio_get_fhandle_by_fuse_fh(vfsdev, copy_file_range_in->fh_in);
	if (!fhandle_in) {
		SPDK_ERRLOG("Invalid fhandle_in: %p\n", fhandle_in);
		res = -EINVAL;
		goto bad_fobject_in;
	}

	fobject_out = fsdev_aio_get_fobject_by_nodeid(vfsdev, copy_file_range_in->nodeid_out);
	if (!fobject_out) {
		SPDK_ERRLOG("Invalid fobject_out\n");
		res = -EINVAL;
		goto bad_fobject_in;
	}

	fhandle_out = fsdev_aio_get_fhandle_by_fuse_fh(vfsdev, copy_file_range_in->fh_out);
	if (!fhandle_out) {
		SPDK_ERRLOG("Invalid fhandle_out: %p\n", fhandle_out);
		res = -EINVAL;
		goto fop_failed;
	}

	res = copy_file_range(fhandle_in->fd, &off_in, fhandle_out->fd, &off_out, len, flags);
	if (res < 0) {
		res = -errno;
		SPDK_ERRLOG("copy_file_range failed with err=%d\n", saverr);
		goto fop_failed;
	}

	SPDK_DEBUGLOG(fsdev_aio,
		      "COPY_FILE_RANGE returned %zd for " FOBJECT_FMT " fh=%p offset=%" PRIu64 " -> " FOBJECT_FMT
		      " fh=%p offset=%" PRIu64 " (len=%zu flags=0x%" PRIx32 ")\n",
		      res, FOBJECT_ARGS(fobject_in), fhandle_in, (uint64_t)off_in, FOBJECT_ARGS(fobject_out), fhandle_out,
		      (uint64_t)off_out, len, flags);


	out_hdr->len += sizeof(struct fuse_write_out);
	write_out->size = res;
	res = 0;

fop_failed:
	file_object_unref(fobject_out, 1);
bad_fobject_in:
	file_object_unref(fobject_in, 1);
	return res;
#else
	return -ENOSYS;
#endif
}

static void
fsdev_aio_op_abort_aio(struct aio_io_channel *ch, struct aio_fsdev_io *vfsdev_io)
{
	int res;
	struct io_event result;

	res = io_cancel(ch->u.io_ctx, &vfsdev_io->io, &result);
	if (res) {
		TAILQ_REMOVE(&ch->ios_in_progress, vfsdev_io, link);
		SPDK_DEBUGLOG(fsdev_aio, "aio=%p cancelled\n", vfsdev_io);
		fsdev_aio_cb(vfsdev_io, ECANCELED, 0);
	} else {
		SPDK_WARNLOG("aio=%p cancellation failed with err=%d\n", vfsdev_io, res);
	}
}

#ifdef SPDK_CONFIG_URING
static void
fsdev_aio_op_abort_io_uring(struct aio_io_channel *ch, struct aio_fsdev_io *vfsdev_io)
{
	/* I/O abortion is optional - we do our best to abort the I/O when possible. */
#ifdef IORING_ASYNC_CANCEL_USERDATA
	int res;
	struct io_uring_sqe *sqe;

	if (!g_io_uring_supported_ops[IORING_OP_ASYNC_CANCEL]) {
		SPDK_DEBUGLOG(fsdev_aio, "IORING_OP_ASYNC_CANCEL is not supported by kernel\n");
		return;
	}

	sqe = io_uring_get_sqe(&ch->u.uring.io_ring);
	if (sqe == NULL) {
		SPDK_WARNLOG("No SQE available for cancellation\n");
		return;
	}

	io_uring_prep_cancel(sqe, vfsdev_io, IORING_ASYNC_CANCEL_USERDATA);
	io_uring_sqe_set_data(sqe, NULL);
	res = io_uring_submit(&ch->u.uring.io_ring);
	if (res > 0) {
		SPDK_DEBUGLOG(fsdev_aio, "Cancellation submitted\n");
	} else {
		SPDK_WARNLOG("Cancellation submission failed with err=%d\n", res);
	}
#else
	SPDK_INFOLOG(fsdev_aio, "IORING_OP_ASYNC_CANCEL is not supported\n");
#endif
}
#else
#define fsdev_aio_op_abort_io_uring(ch, vfsdev_io) assert(0)
#endif

static void
fsdev_aio_op_abort_io(struct aio_io_channel *ch, struct aio_fsdev_io *vfsdev_io)
{
	if (aio_fsdev_use_io_uring_rdwr()) {
		fsdev_aio_op_abort_io_uring(ch, vfsdev_io);
	} else {
		fsdev_aio_op_abort_aio(ch, vfsdev_io);
	}
}


static int
fsdev_aio_op_interrupt(struct spdk_io_channel *_ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_io_channel *ch = spdk_io_channel_get_ctx(_ch);
	struct aio_fsdev_io *vfsdev_io, *tmp;
	struct fuse_interrupt_in *interrupt_in = fsdev_io->u_in.fuse.op.interrupt;
	uint64_t unique_to_abort = interrupt_in->unique;

	TAILQ_FOREACH_SAFE(vfsdev_io, &ch->ios_for_submit, link, tmp) {
		struct spdk_fsdev_io *_fsdev_io = aio_to_fsdev_io(vfsdev_io);

		if (fsdev_io->fsdev != _fsdev_io->fsdev) {
			continue;
		}

		if (spdk_fsdev_io_get_unique(_fsdev_io) == unique_to_abort) {
			TAILQ_REMOVE(&ch->ios_for_submit, vfsdev_io, link);
			fsdev_aio_io_complete(fsdev_io, -ECANCELED);
			return 0; /* we found the IO to abort, no need to continue */
		}
	}

	TAILQ_FOREACH_SAFE(vfsdev_io, &ch->ios_in_progress, link, tmp) {
		struct spdk_fsdev_io *_fsdev_io = aio_to_fsdev_io(vfsdev_io);

		if (fsdev_io->fsdev != _fsdev_io->fsdev) {
			continue;
		}

		if (spdk_fsdev_io_get_unique(_fsdev_io) == unique_to_abort) {
			fsdev_aio_op_abort_io(ch, vfsdev_io);
			return 0; /* we found the IO to abort, no need to continue */
		}
	}

	return 0;
}

static int
aio_io_poll_common(struct aio_io_channel *ch)
{
	int res = SPDK_POLLER_IDLE;
	struct aio_fsdev_io *vfsdev_io, *tmp;
	TAILQ_HEAD(aio_tmp_list, aio_fsdev_io) ios = TAILQ_HEAD_INITIALIZER(ios);

	TAILQ_SWAP(&ch->ios_to_complete, &ios, aio_fsdev_io, link);
	TAILQ_FOREACH_SAFE(vfsdev_io, &ios, link, tmp) {
		struct spdk_fsdev_io *fsdev_io = aio_to_fsdev_io(vfsdev_io);

		TAILQ_REMOVE(&ios, vfsdev_io, link);
		fsdev_aio_io_complete(fsdev_io, vfsdev_io->status);
		res = SPDK_POLLER_BUSY;
	}

	return res;
}

static int
aio_io_poll_aio(void *arg)
{
	struct aio_io_channel *ch = arg;
	struct aio_fsdev_io *vfsdev_io, *tmp;
	TAILQ_HEAD(aio_tmp_list, aio_fsdev_io) ios = TAILQ_HEAD_INITIALIZER(ios);
	struct io_event events[32];
	struct iocb *iocbs[32];
	long to_submit = 0;
	int i, rc, res;
	struct timespec timeout;

	timeout.tv_sec = 0;
	timeout.tv_nsec = 0;

	rc = io_getevents(ch->u.io_ctx, 0, 32, events, &timeout);
	if (rc < 0) {
		SPDK_ERRLOG("%s\n", strerror(-rc));
		rc = 0;
	}

	for (i = 0; i < rc; i++) {
		fsdev_aio_cb(events[i].data, events[i].res, events[i].res2);
	}

	res = rc > 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;

	TAILQ_SWAP(&ch->ios_for_submit, &ios, aio_fsdev_io, link);
	TAILQ_FOREACH_SAFE(vfsdev_io, &ios, link, tmp) {
		struct spdk_fsdev_io *fsdev_io = aio_to_fsdev_io(vfsdev_io);
		enum spdk_fsdev_io_type type = spdk_fsdev_io_get_type(fsdev_io);

		rc = -EOPNOTSUPP;
		res = SPDK_POLLER_BUSY;

		switch (type) {
		case SPDK_FSDEV_IO_FUSE:
			switch (fsdev_io->u_in.fuse.hdr->opcode) {
			case FUSE_READ:
			case FUSE_WRITE:
				iocbs[to_submit++] = &vfsdev_io->io;
				rc = IO_STATUS_ASYNC;
				break;
			case FUSE_SETLKW:
				TAILQ_REMOVE(&ios, vfsdev_io, link);
				rc = fsdev_aio_do_setlk(ch, fsdev_io);
				break;
			case FUSE_POLL:
				TAILQ_REMOVE(&ios, vfsdev_io, link);
				rc = fsdev_aio_do_poll(ch, fsdev_io);
				break;
			default:
				SPDK_ERRLOG("Unsupported FUSE IO type: %d\n", type);
				assert(0);
				rc = -EINVAL;
				break;
			}
			break;
		case SPDK_FSDEV_IO_POLL:
		case SPDK_FSDEV_IO_SETLK:
		case SPDK_FSDEV_IO_READ:
		case SPDK_FSDEV_IO_WRITE:
			SPDK_ERRLOG("Operation type %d has been converted to SPDK_FSDEV_IO_FUSE\n", (int)type);
		/* FALLTHROUGH */
		default:
			TAILQ_REMOVE(&ios, vfsdev_io, link);
			break;
		}

		if (rc != IO_STATUS_ASYNC) {
			vfsdev_io->status = rc;
			TAILQ_INSERT_TAIL(&ch->ios_to_complete, vfsdev_io, link);
		}

		if (to_submit == SPDK_COUNTOF(iocbs)) {
			break;
		}
	}

	if (to_submit > 0) {
		rc = io_submit(ch->u.io_ctx, to_submit, iocbs);
		if (rc < 0) {
			res = rc;
			rc = 0; /* 0 were submitted. This will get them all queued back up. */

			if (res != -EAGAIN) {
				/* The failures typically apply to the first iocb. Fail that one, but let the others
				 * queue back up to be resubmitted. */
				SPDK_ERRLOG("Failed io_submit: %s (%d)\n", spdk_strerror(-res), res);
				vfsdev_io = TAILQ_FIRST(&ios);
				TAILQ_REMOVE(&ios, vfsdev_io, link);
				vfsdev_io->status = res;
				TAILQ_INSERT_TAIL(&ch->ios_to_complete, vfsdev_io, link);
			}
		}

		/* For each request actually submitted, shift it into the in progress list. */
		i = 0;
		TAILQ_FOREACH_SAFE(vfsdev_io, &ios, link, tmp) {
			if (i == rc) {
				break;
			}

			assert(&vfsdev_io->io == iocbs[i]);

			TAILQ_REMOVE(&ios, vfsdev_io, link);
			TAILQ_INSERT_TAIL(&ch->ios_in_progress, vfsdev_io, link);

			i++;
		}

		/* For all remaining requests, put them back into the ios_for_submit list */
		vfsdev_io = TAILQ_LAST(&ios, aio_tmp_list);
		while (vfsdev_io != NULL) {
			TAILQ_REMOVE(&ios, vfsdev_io, link);
			TAILQ_INSERT_HEAD(&ch->ios_for_submit, vfsdev_io, link);
			vfsdev_io = TAILQ_LAST(&ios, aio_tmp_list);
		}
	}

	if (aio_io_poll_common(ch) == SPDK_POLLER_BUSY) {
		res = SPDK_POLLER_BUSY;
	}

	return res;
}

#ifdef SPDK_CONFIG_URING

static int
aio_retry_io_uring_read(struct aio_io_channel *ch, struct aio_fsdev_io *vfsdev_io)
{
	struct spdk_fsdev_io *fsdev_io = aio_to_fsdev_io(vfsdev_io);
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	struct aio_fsdev_file_handle *fhandle;
	size_t size = fsdev_io->u_in.fuse.op.read->size;
	uint64_t offs = fsdev_io->u_in.fuse.op.read->offset;
	struct iovec *iovs = fsdev_io->u_out.fuse.iov;
	uint32_t iovcnt = fsdev_io->u_out.fuse.iovcnt;

	fhandle = fsdev_aio_get_fhandle_by_fuse_fh(vfsdev, fsdev_io->u_in.fuse.op.read->fh);
	if (!fhandle) {
		SPDK_ERRLOG("Invalid fhandle: %p\n", fhandle);
		return -EINVAL;
	}

	UNUSED(size);

	SPDK_DEBUGLOG(fsdev_aio, "read: fd=%d offs=%" PRIu64 " size=%" PRIu64 " iovcnt=%" PRIu32 "\n",
		      fhandle->fd, offs, size, iovcnt);

	return fsdev_aio_prep_read_io_uring(ch, vfsdev_io, fhandle->fd, iovs, iovcnt, offs);
}

static int
aio_retry_io_uring_write(struct aio_io_channel *ch, struct aio_fsdev_io *vfsdev_io)
{
	struct spdk_fsdev_io *fsdev_io = aio_to_fsdev_io(vfsdev_io);
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	struct aio_fsdev_file_handle *fhandle;
	size_t size = fsdev_io->u_in.fuse.op.write->size;
	uint64_t offs = fsdev_io->u_in.fuse.op.write->offset;
	const struct iovec *iovs = fsdev_io->u_in.fuse.iov;
	uint32_t iovcnt = fsdev_io->u_in.fuse.iovcnt;

	UNUSED(size);

	fhandle = fsdev_aio_get_fhandle_by_fuse_fh(vfsdev, fsdev_io->u_in.fuse.op.write->fh);
	if (!fhandle) {
		SPDK_ERRLOG("Invalid fhandle: %p\n", fhandle);
		return -EINVAL;
	}

	SPDK_DEBUGLOG(fsdev_aio, "write: fd=%d offs=%" PRIu64 " size=%" PRIu64 " iovcnt=%" PRIu32 "\n",
		      fhandle->fd, offs, size, iovcnt);

	return fsdev_aio_prep_write_io_uring(ch, vfsdev_io, fhandle->fd, iovs, iovcnt, offs);
}

static int
aio_io_poll_io_uring(void *arg)
{
	struct aio_io_channel *ch = arg;
	int res = SPDK_POLLER_IDLE;
	unsigned cqes_count = 0;
	struct io_uring_cqe *cqe;
	unsigned head;
	TAILQ_HEAD(aio_tmp_list, aio_fsdev_io) ios = TAILQ_HEAD_INITIALIZER(ios);

	/* Handle completions */
	io_uring_for_each_cqe(&ch->u.uring.io_ring, head, cqe) {
		struct aio_fsdev_io *vfsdev_io = io_uring_cqe_get_data(cqe);

#ifdef IORING_ASYNC_CANCEL_USERDATA
		if (!vfsdev_io) {
			/* This is a cancellation request */
			continue;
		}
#endif

		fsdev_aio_cb(vfsdev_io, cqe->res, (cqe->res < 0) ? cqe->res : 0);
		res = SPDK_POLLER_BUSY;

		cqes_count++;
	}

	io_uring_cq_advance(&ch->u.uring.io_ring, cqes_count);

	/* Submit more I/O */
	if (ch->u.uring.io_count) {
		res = io_uring_submit(&ch->u.uring.io_ring);
		if (res < 0) {
			SPDK_ERRLOG("io_uring_submit failed with err=%d\n", res);
		} else {
			assert((uint32_t)res <= ch->u.uring.io_count);
			ch->u.uring.io_count -= res;
		}
	}

	/* Handle submit queue */
	ch->u.uring.queue_ios = 0; /* reset the flag */
	while (!TAILQ_EMPTY(&ch->ios_for_submit)) {
		struct aio_fsdev_io *vfsdev_io = TAILQ_FIRST(&ch->ios_for_submit);
		struct spdk_fsdev_io *fsdev_io = aio_to_fsdev_io(vfsdev_io);
		enum spdk_fsdev_io_type type = spdk_fsdev_io_get_type(fsdev_io);
		int rc = IO_STATUS_ASYNC;

		TAILQ_REMOVE(&ch->ios_for_submit, vfsdev_io, link);

		res = SPDK_POLLER_BUSY;

		switch (type) {
		case SPDK_FSDEV_IO_FUSE:
			switch (fsdev_io->u_in.fuse.hdr->opcode) {
			case FUSE_READ:
				/* If this or one of the previous IOs failed due to no SQE available, we need to retry it */
				if (!TAILQ_EMPTY(&ios) || aio_retry_io_uring_read(ch, vfsdev_io) == -EAGAIN) {
					TAILQ_INSERT_TAIL(&ios, vfsdev_io, link);
				}
				break;
			case FUSE_WRITE:
				/* If this or one of the previous IOs failed due to no SQE available, we need to retry it */
				if (!TAILQ_EMPTY(&ios) || aio_retry_io_uring_write(ch, vfsdev_io) == -EAGAIN) {
					TAILQ_INSERT_TAIL(&ios, vfsdev_io, link);
				}
				break;
			case FUSE_SETLKW:
				rc = fsdev_aio_do_setlk(ch, fsdev_io);
				break;
			case FUSE_POLL:
				rc = fsdev_aio_do_poll(ch, fsdev_io);
				break;
			default:
				SPDK_ERRLOG("Unsupported FUSE IO type: %d\n", type);
				assert(0);
				rc = -EINVAL;
				break;
			}
			break;
		default:
			SPDK_ERRLOG("Unsupported IO type: %d\n", type);
			assert(0);
			rc = -EINVAL;
			break;
		}

		if (rc != IO_STATUS_ASYNC) {
			vfsdev_io->status = rc;
			TAILQ_INSERT_TAIL(&ch->ios_to_complete, vfsdev_io, link);
		}
	}

	if (!TAILQ_EMPTY(&ios)) {
		/* The queue is not empty, so add the remaining IOs to the submit queue */
		assert(ch->u.uring.queue_ios); /* If this is the case, the flag has been set by the retry logic */
		TAILQ_CONCAT(&ch->ios_for_submit, &ios, link);
	} else {
		/* The queue is empty, so we've successfully submitted all queued IOs */
		assert(!ch->u.uring.queue_ios);
	}

	/* Handle common part of the poll */
	if (aio_io_poll_common(ch) == SPDK_POLLER_BUSY) {
		res = SPDK_POLLER_BUSY;
	}

	return res;
}
#endif

static int
fsdev_aio_op_fuse(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct fuse_in_header *in_hdr = fsdev_io->u_in.fuse.hdr;
	struct fuse_out_header *out_hdr = fsdev_io->u_out.fuse.hdr;
	int status;

	assert(spdk_fsdev_io_get_type(fsdev_io) == SPDK_FSDEV_IO_FUSE);
	if (out_hdr != NULL) {
		out_hdr->unique = fsdev_io->u_in.fuse.hdr->unique;
		out_hdr->len = sizeof(*out_hdr);
	}

	switch (in_hdr->opcode) {
	case FUSE_READ:
		status = fsdev_aio_op_read(ch, fsdev_io);
		break;
	case FUSE_WRITE:
		status = fsdev_aio_op_write(ch, fsdev_io);
		break;
	case FUSE_GETATTR:
		status = fsdev_aio_op_getattr(ch, fsdev_io);
		break;
	case FUSE_SETATTR:
		status = fsdev_aio_op_setattr(ch, fsdev_io);
		break;
	case FUSE_OPEN:
		status = fsdev_aio_op_open(ch, fsdev_io);
		break;
	case FUSE_OPENDIR:
		status = fsdev_aio_op_opendir(ch, fsdev_io);
		break;
	case FUSE_CREATE:
		status = fsdev_aio_op_create(ch, fsdev_io);
		break;
	case FUSE_RELEASE:
		status = fsdev_aio_op_release(ch, fsdev_io);
		break;
	case FUSE_RELEASEDIR:
		status = fsdev_aio_op_releasedir(ch, fsdev_io);
		break;
	case FUSE_LOOKUP:
		status = fsdev_aio_op_lookup(ch, fsdev_io);
		break;
	case FUSE_FORGET:
		status = fsdev_aio_op_forget(ch, fsdev_io);
		break;
	case FUSE_BATCH_FORGET:
		status = fsdev_aio_op_batch_forget(ch, fsdev_io);
		break;
	case FUSE_SYMLINK:
		status = fsdev_aio_op_symlink(ch, fsdev_io);
		break;
	case FUSE_MKNOD:
		status = fsdev_aio_op_mknod(ch, fsdev_io);
		break;
	case FUSE_MKDIR:
		status = fsdev_aio_op_mkdir(ch, fsdev_io);
		break;
	case FUSE_UNLINK:
		status = fsdev_aio_op_unlink(ch, fsdev_io);
		break;
	case FUSE_RMDIR:
		status = fsdev_aio_op_rmdir(ch, fsdev_io);
		break;
	case FUSE_INIT:
		status = fsdev_aio_op_init(ch, fsdev_io);
		break;
	case FUSE_DESTROY:
		status = fsdev_aio_op_destroy(ch, fsdev_io);
		break;
	case FUSE_RENAME:
		status = fsdev_aio_op_rename(ch, fsdev_io);
		break;
	case FUSE_RENAME2:
		status = fsdev_aio_op_rename2(ch, fsdev_io);
		break;
	case FUSE_READLINK:
		status = fsdev_aio_op_readlink(ch, fsdev_io);
		break;
	case FUSE_LINK:
		status = fsdev_aio_op_link(ch, fsdev_io);
		break;
	case FUSE_STATFS:
		status = fsdev_aio_op_statfs(ch, fsdev_io);
		break;
	case FUSE_FSYNC:
	case FUSE_FSYNCDIR:
		status = fsdev_aio_op_fsync(ch, fsdev_io);
		break;
	case FUSE_SETXATTR:
		status = fsdev_aio_op_setxattr(ch, fsdev_io);
		break;
	case FUSE_GETXATTR:
		status = fsdev_aio_op_getxattr(ch, fsdev_io);
		break;
	case FUSE_LISTXATTR:
		status = fsdev_aio_op_listxattr(ch, fsdev_io);
		break;
	case FUSE_REMOVEXATTR:
		status = fsdev_aio_op_removexattr(ch, fsdev_io);
		break;
	case FUSE_FLUSH:
		status = fsdev_aio_op_flush(ch, fsdev_io);
		break;
	case FUSE_READDIRPLUS:
		status = fsdev_aio_op_readdirplus(ch, fsdev_io);
		break;
	case FUSE_READDIR:
		status = fsdev_aio_op_readdir(ch, fsdev_io);
		break;
	case FUSE_INTERRUPT:
		status = fsdev_aio_op_interrupt(ch, fsdev_io);
		break;
	case FUSE_FALLOCATE:
		status = fsdev_aio_op_fallocate(ch, fsdev_io);
		break;
	case FUSE_COPY_FILE_RANGE:
		status = fsdev_aio_op_copy_file_range(ch, fsdev_io);
		break;
	case FUSE_SYNCFS:
		status = fsdev_aio_op_syncfs(ch, fsdev_io);
		break;
	case FUSE_LSEEK:
		status = fsdev_aio_op_lseek(ch, fsdev_io);
		break;
	case FUSE_GETLK:
		status = fsdev_aio_op_getlk(ch, fsdev_io);
		break;
	case FUSE_SETLK:
	case FUSE_SETLKW:
		status = fsdev_aio_op_setlk(ch, fsdev_io);
		break;
	case FUSE_POLL:
		status = fsdev_aio_op_poll(ch, fsdev_io);
		break;
	case FUSE_IOCTL:
		status = fsdev_aio_op_ioctl(ch, fsdev_io);
		break;
	default:
		SPDK_ERRLOG("Unsupported opcode: %" PRIu32 "\n", in_hdr->opcode);
		status = -ENOSYS;
		break;
	}

	return status;
}

static int
aio_fsdev_create_cb(void *io_device, void *ctx_buf)
{
	struct aio_io_channel *ch = ctx_buf;
	struct spdk_thread *thread = spdk_get_thread();
	spdk_poller_fn poller_fn;
	int res;

	UNUSED(thread);

	if (!aio_fsdev_use_io_uring_rdwr()) {
		res = io_queue_init(g_opts.max_io_depth, &ch->u.io_ctx);
		if (res) {
			SPDK_ERRLOG("io_setup(%" PRIu32 ") failed with %d\n", g_opts.max_io_depth, res);
			return -ENOMEM;
		}

		poller_fn = aio_io_poll_aio;
	}
#ifdef SPDK_CONFIG_URING
	else {
		res = io_uring_queue_init(g_opts.max_io_depth, &ch->u.uring.io_ring,
					  IORING_SETUP_COOP_TASKRUN | IORING_SETUP_SINGLE_ISSUER);
		if (res) {
			SPDK_ERRLOG("io_uring_queue_init(%" PRIu32 ") failed with %d\n", g_opts.max_io_depth, res);
			io_queue_release(ch->u.io_ctx);
			return -res;
		}

		poller_fn = aio_io_poll_io_uring;
	}
#endif

	TAILQ_INIT(&ch->ios_for_submit);
	TAILQ_INIT(&ch->ios_in_progress);
	TAILQ_INIT(&ch->ios_to_complete);

	ch->poller = SPDK_POLLER_REGISTER(poller_fn, ch, 0);

	UNUSED(thread);

	SPDK_DEBUGLOG(fsdev_aio,
		      "Created aio fsdev IO channel: thread %s, thread id %" PRIu64 " (io_uring=%d)\n",
		      spdk_thread_get_name(thread), spdk_thread_get_id(thread), aio_fsdev_use_io_uring_rdwr());
	return 0;
}

static void
aio_fsdev_destroy_cb(void *io_device, void *ctx_buf)
{
	struct aio_io_channel *ch = ctx_buf;
	struct spdk_thread *thread = spdk_get_thread();

	UNUSED(thread);

	spdk_poller_unregister(&ch->poller);

	assert(TAILQ_EMPTY(&ch->ios_in_progress));

	if (!aio_fsdev_use_io_uring_rdwr()) {
		io_queue_release(ch->u.io_ctx);
	}
#ifdef SPDK_CONFIG_URING
	else {
		io_uring_queue_exit(&ch->u.uring.io_ring);
	}
#endif

	SPDK_DEBUGLOG(fsdev_aio,
		      "Destroyed aio fsdev IO channel: thread %s, thread id %" PRIu64 "\n",
		      spdk_thread_get_name(thread), spdk_thread_get_id(thread));
}

static int
fsdev_aio_config_json(struct spdk_json_write_ctx *w)
{
	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "method", "fsdev_aio_set_options");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_uint32(w, "max_io_depth", g_opts.max_io_depth);
	spdk_json_write_named_bool(w, "enable_io_uring", g_opts.enable_io_uring);
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);

	return 0;
}

static int
fsdev_aio_initialize(void)
{
	/*
	 * We need to pick some unique address as our "io device" - so just use the
	 *  address of the global tailq.
	 */
	spdk_io_device_register(&g_aio_fsdev_head,
				aio_fsdev_create_cb, aio_fsdev_destroy_cb,
				sizeof(struct aio_io_channel), "aio_fsdev");

#ifdef SPDK_CONFIG_URING
	fsdev_init_supported_uring_ops();
#endif

	return 0;
}

static void
fsdev_aio_finish(void)
{
	spdk_io_device_unregister(&g_aio_fsdev_head, NULL);
}

static int
fsdev_aio_get_ctx_size(void)
{
	return sizeof(struct aio_fsdev_io);
}

static struct spdk_fsdev_module aio_fsdev_module = {
	.name = "aio",
	.config_json = fsdev_aio_config_json,
	.module_init = fsdev_aio_initialize,
	.module_fini = fsdev_aio_finish,
	.get_ctx_size	= fsdev_aio_get_ctx_size,
};

SPDK_FSDEV_MODULE_REGISTER(aio, &aio_fsdev_module);

static void
fsdev_aio_free(struct aio_fsdev *vfsdev)
{
	if (vfsdev->proc_self_fd != -1) {
		close(vfsdev->proc_self_fd);
	}

	if (vfsdev->root) {
		uint64_t refcount = file_object_unref(vfsdev->root, 1);
		assert(refcount == 0);
		UNUSED(refcount);

	}

	fsdev_aio_fanotify_close(vfsdev);

	if (vfsdev->lut) {
		spdk_lut_free(vfsdev->lut);
		spdk_spin_destroy(&vfsdev->lock);
	}

	free(vfsdev->fsdev.name);
	free(vfsdev->root_path);

	free(vfsdev);
}

static int
fsdev_aio_destruct(void *ctx)
{
	struct aio_fsdev *vfsdev = ctx;

	TAILQ_REMOVE(&g_aio_fsdev_head, vfsdev, tailq);

	fsdev_free_leafs(vfsdev->root, true);
	vfsdev->root = NULL;

	fsdev_aio_free(vfsdev);
	return 0;
}

static void
fsdev_aio_submit_request(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	int status;
	enum spdk_fsdev_io_type type = spdk_fsdev_io_get_type(fsdev_io);

	if (spdk_likely(type == SPDK_FSDEV_IO_FUSE)) {
		status = fsdev_aio_op_fuse(ch, fsdev_io);
		if (status != IO_STATUS_ASYNC) {
			fsdev_aio_io_complete(fsdev_io, status);
		}
	} else {
		SPDK_DEBUGLOG(fsdev_aio, "Operation type %d is not implemented!\n", (int)type);
		spdk_fsdev_io_complete(fsdev_io, -ENOSYS);
	}
}

static struct spdk_io_channel *
fsdev_aio_get_io_channel(void *ctx)
{
	/* We don't create an spdk_io_channel per aio_fsdev. Rather we share it among all the aio fsdevs. */
	return spdk_get_io_channel(&g_aio_fsdev_head);
}

static void
fsdev_aio_write_config_json(struct spdk_fsdev *fsdev, struct spdk_json_write_ctx *w)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev);

	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "method", "fsdev_aio_create");
	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "name", spdk_fsdev_get_name(&vfsdev->fsdev));
	spdk_json_write_named_string(w, "root_path", vfsdev->root_path);
	spdk_json_write_named_bool(w, "enable_xattr", vfsdev->opts.xattr_enabled);
	spdk_json_write_named_bool(w, "enable_writeback_cache",
				   vfsdev->opts.writeback_cache_enabled);
	spdk_json_write_named_uint32(w, "max_xfer_size", vfsdev->opts.max_xfer_size);
	spdk_json_write_named_uint32(w, "max_readahead", vfsdev->opts.max_readahead);

	spdk_json_write_named_bool(w, "skip_rw", vfsdev->opts.skip_rw);
	spdk_json_write_named_bool(w, "enable_notifications", vfsdev->opts.enable_notifications);
	spdk_json_write_named_uint32(w, "attr_valid_ms", vfsdev->opts.attr_valid_ms);
	spdk_json_write_named_bool(w, "disable_copy_file_range", vfsdev->opts.disable_copy_file_range);
	spdk_json_write_object_end(w); /* params */
	spdk_json_write_object_end(w);
}

static int
fsdev_aio_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct aio_fsdev *vfsdev = ctx;

	spdk_json_write_named_string(w, "root_path", vfsdev->root_path);
	spdk_json_write_named_bool(w, "enable_xattr", vfsdev->opts.xattr_enabled);
	spdk_json_write_named_bool(w, "enable_writeback_cache",
				   vfsdev->opts.writeback_cache_enabled);
	spdk_json_write_named_uint32(w, "max_xfer_size", vfsdev->opts.max_xfer_size);
	spdk_json_write_named_uint32(w, "max_readahead", vfsdev->opts.max_readahead);
	spdk_json_write_named_bool(w, "skip_rw", vfsdev->opts.skip_rw);
	spdk_json_write_named_bool(w, "enable_notifications", vfsdev->opts.enable_notifications);
	spdk_json_write_named_uint32(w, "attr_valid_ms", vfsdev->opts.attr_valid_ms);
	spdk_json_write_named_bool(w, "disable_copy_file_range", vfsdev->opts.disable_copy_file_range);
	spdk_json_write_named_object_begin(w, "io_uring");
	spdk_json_write_named_bool(w, "rdwr", aio_fsdev_use_io_uring_rdwr());
	spdk_json_write_object_end(w);

	return 0;
}

struct fsdev_aio_reset_ctx {
	struct aio_fsdev *vfsdev;
	spdk_fsdev_reset_done_cb cb;
	void *cb_arg;
	struct spdk_poller *poller;
	bool has_outstanding_ios;
};

static void
fsdev_aio_reset_done(struct fsdev_aio_reset_ctx *ctx, int status)
{
	fsdev_free_leafs(ctx->vfsdev->root, false);

	ctx->cb(ctx->cb_arg, status);

	spdk_poller_unregister(&ctx->poller);

	free(ctx);
}

static void
fsdev_aio_reset_check_outstanding_io_msg_cb(struct spdk_io_channel_iter *i)
{
	struct spdk_io_channel *_ch = spdk_io_channel_iter_get_channel(i);
	struct aio_io_channel *ch = spdk_io_channel_get_ctx(_ch);
	struct fsdev_aio_reset_ctx *ctx = spdk_io_channel_iter_get_ctx(i);
	struct aio_fsdev_io *vfsdev_io;
	bool ios_in_progress = false;

	/* Check whether some IOs remained in progress */
	TAILQ_FOREACH(vfsdev_io, &ch->ios_in_progress, link) {
		/* We only check the IOs which belong to our aio_fsdev. */
		struct spdk_fsdev_io *fsdev_io = aio_to_fsdev_io(vfsdev_io);
		if (fsdev_io->fsdev == &ctx->vfsdev->fsdev) {
			ios_in_progress = true;
			break;
		}
	}

	if (ios_in_progress) {
		__atomic_test_and_set(&ctx->has_outstanding_ios, __ATOMIC_RELAXED);
	}

	spdk_for_each_channel_continue(i, 0);
}

static void
fsdev_aio_reset_check_outstanding_io_done_cb(struct spdk_io_channel_iter *i, int status)
{
	struct fsdev_aio_reset_ctx *ctx = spdk_io_channel_iter_get_ctx(i);
	struct spdk_fsdev *fsdev = &ctx->vfsdev->fsdev;
	bool has_outstanding_ios;

	if (status) {
		SPDK_ERRLOG("%s: outstanding IOs check failed with %d\n", spdk_fsdev_get_name(fsdev), status);
		fsdev_aio_reset_done(ctx, status);
		return;
	}

	/* Get the has_outstanding_ios and reset it so the poller can set it again if resumed */
	has_outstanding_ios = __atomic_exchange_n(&ctx->has_outstanding_ios, 0, __ATOMIC_RELAXED);
	if (has_outstanding_ios) {
		/* We still have uncompleted IOs, so resume the poller */
		SPDK_DEBUGLOG(fsdev_aio, "%s: some IOs are still uncompleted\n", spdk_fsdev_get_name(fsdev));
		spdk_poller_resume(ctx->poller);
		return;
	}

	/* All IOs have been completed -> finish the reset */
	SPDK_DEBUGLOG(fsdev_aio, "%s: all IOs have been completed. Reset is done!\n",
		      spdk_fsdev_get_name(fsdev));

	fsdev_aio_reset_done(ctx, 0);
}

static int
fsdev_aio_reset_poller_cb(void *_ctx)
{
	struct fsdev_aio_reset_ctx *ctx = _ctx;

	spdk_poller_pause(ctx->poller); /* We'll pause the poller until the current check is done */

	/* Check whether all the IOs has been completed */
	spdk_for_each_channel(&g_aio_fsdev_head, fsdev_aio_reset_check_outstanding_io_msg_cb, ctx,
			      fsdev_aio_reset_check_outstanding_io_done_cb);

	return SPDK_POLLER_BUSY;
}

static void
fsdev_aio_reset_msg_cb(struct spdk_io_channel_iter *i)
{
	struct spdk_io_channel *_ch = spdk_io_channel_iter_get_channel(i);
	struct aio_io_channel *ch = spdk_io_channel_get_ctx(_ch);
	struct fsdev_aio_reset_ctx *ctx = spdk_io_channel_iter_get_ctx(i);
	struct aio_fsdev_io *vfsdev_io, *tmp;

	TAILQ_FOREACH_SAFE(vfsdev_io, &ch->ios_for_submit, link, tmp) {
		struct spdk_fsdev_io *fsdev_io = aio_to_fsdev_io(vfsdev_io);

		if (fsdev_io->fsdev == &ctx->vfsdev->fsdev) {
			TAILQ_REMOVE(&ch->ios_for_submit, vfsdev_io, link);
			fsdev_aio_io_complete(fsdev_io, -ECANCELED);
		}
	}

	TAILQ_FOREACH_SAFE(vfsdev_io, &ch->ios_in_progress, link, tmp) {
		struct spdk_fsdev_io *fsdev_io = aio_to_fsdev_io(vfsdev_io);

		if (fsdev_io->fsdev == &ctx->vfsdev->fsdev) {
			fsdev_aio_op_abort_io(ch, vfsdev_io);
		}
	}

	spdk_for_each_channel_continue(i, 0);
}

static void
fsdev_aio_reset_done_cb(struct spdk_io_channel_iter *i, int status)
{
	struct fsdev_aio_reset_ctx *ctx = spdk_io_channel_iter_get_ctx(i);
	struct spdk_fsdev *fsdev = &ctx->vfsdev->fsdev;

	if (status) {
		SPDK_ERRLOG("%s: IO cancellation failed with %d\n", spdk_fsdev_get_name(fsdev), status);
		fsdev_aio_reset_done(ctx, status);
		return;
	}

	SPDK_DEBUGLOG(fsdev_aio, "%s: all the outstanding IOs have been cancelled\n",
		      spdk_fsdev_get_name(fsdev));

	/* Resume the poller, so it'll wait until the completion of all the IOs */
	spdk_poller_resume(ctx->poller);
}

static int
fsdev_aio_reset(void *_ctx, spdk_fsdev_reset_done_cb cb, void *cb_arg)
{
	struct aio_fsdev *vfsdev = _ctx;
	struct fsdev_aio_reset_ctx *ctx;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		SPDK_ERRLOG("Cannot allocate the reset object\n");
		return -ENOMEM;
	}

	ctx->vfsdev = vfsdev;
	ctx->cb = cb;
	ctx->cb_arg = cb_arg;
	ctx->poller = SPDK_POLLER_REGISTER(fsdev_aio_reset_poller_cb, ctx, 0);
	if (!ctx->poller) {
		free(ctx);
		SPDK_ERRLOG("Cannot register reset poller\n");
		return -ENOMEM;
	}

	spdk_poller_pause(ctx->poller); /* We'll start it once the IOs are cancelled */

	/* First, we'll cancel all the async IOs */
	spdk_for_each_channel(&g_aio_fsdev_head, fsdev_aio_reset_msg_cb, ctx, fsdev_aio_reset_done_cb);
	return 0;
}

#ifdef SPDK_CONFIG_HAVE_FANOTIFY
static int
fsdev_aio_enable_notifications(struct aio_fsdev *vfsdev)
{
	int rc;

	vfsdev->fanotify_fd = fanotify_init(FAN_NONBLOCK | FAN_REPORT_FID | FAN_REPORT_DFID_NAME,
					    O_RDONLY | O_LARGEFILE);
	if (vfsdev->fanotify_fd == -1) {
		SPDK_ERRLOG("Failed to create fanotify, errno %d\n", errno);
		rc = -errno;
		goto err;
	}

	vfsdev->pid = getpid();
	vfsdev->fanotify_poller = SPDK_POLLER_REGISTER(fsdev_aio_fanotify_poller, vfsdev,
				  FANOTIFY_POLLER_PERIOD_US);
	if (!vfsdev->fanotify_poller) {
		SPDK_ERRLOG("Failed to create fanotify poller\n");
		rc = -ENOMEM;
		goto err;
	}

	SPDK_NOTICELOG("Started fanotify poller: fanotify fd %d\n", vfsdev->fanotify_fd);
	return 0;

err:
	fsdev_aio_fanotify_close(vfsdev);
	return rc;
}

static int
fsdev_aio_disable_notifications(struct aio_fsdev *vfsdev)
{
	fsdev_aio_fanotify_close(vfsdev);
	return 0;
}

static int
fsdev_aio_set_notifications(void *ctx, bool enabled)
{
	struct aio_fsdev *vfsdev = ctx;

	if (enabled && vfsdev->opts.enable_notifications && vfsdev->fanotify_fd == -1) {
		return fsdev_aio_enable_notifications(vfsdev);
	} else if (enabled && !vfsdev->opts.enable_notifications) {
		SPDK_ERRLOG("Notifications are disabled in fsdev_aio\n");
		return -EOPNOTSUPP;
	} else if (!enabled && vfsdev->fanotify_fd != -1) {
		return fsdev_aio_disable_notifications(vfsdev);
	}

	return 0;
}
#else
static int
fsdev_aio_set_notifications(void *ctx, bool enabled)
{
	struct aio_fsdev *vfsdev = ctx;

	if (enabled && !vfsdev->opts.enable_notifications) {
		SPDK_ERRLOG("Notifications are disabled in fsdev_aio\n");
		return -EOPNOTSUPP;
	}

	return 0;
}
#endif

static const struct spdk_fsdev_fn_table aio_fn_table = {
	.destruct		= fsdev_aio_destruct,
	.submit_request		= fsdev_aio_submit_request,
	.get_io_channel		= fsdev_aio_get_io_channel,
	.write_config_json	= fsdev_aio_write_config_json,
	.reset			= fsdev_aio_reset,
	.dump_info_json		= fsdev_aio_dump_info_json,
	.set_notifications	= fsdev_aio_set_notifications
};

static int
setup_root(struct aio_fsdev *vfsdev)
{
	int fd, res;
	struct stat stat;

	fd = open(vfsdev->root_path, O_PATH);
	if (fd == -1) {
		res = -errno;
		SPDK_ERRLOG("Cannot open root %s (err=%d)\n", vfsdev->root_path, res);
		return res;
	}

	res = fstatat(fd, "", &stat, AT_EMPTY_PATH);
	if (res == -1) {
		res = -errno;
		SPDK_ERRLOG("Cannot get root fstatat of %s (err=%d)\n", vfsdev->root_path, res);
		close(fd);
		return res;
	}

	vfsdev->root = file_object_create_unsafe(vfsdev, NULL, fd, stat.st_ino, stat.st_dev, stat.st_mode,
			"/");
	if (!vfsdev->root) {
		SPDK_ERRLOG("Cannot alloc root\n");
		close(fd);
		return -ENOMEM;
	}

	SPDK_INFOLOG(fsdev_aio, "root (%s) fd=%d\n", vfsdev->root_path, fd);
	return 0;
}

static int
setup_proc_self_fd(struct aio_fsdev *vfsdev)
{
	vfsdev->proc_self_fd = open("/proc/self/fd", O_PATH);
	if (vfsdev->proc_self_fd == -1) {
		int saverr = -errno;
		SPDK_ERRLOG("Failed to open procfs fd dir with %d\n", saverr);
		return saverr;
	}

	SPDK_DEBUGLOG(fsdev_aio, "procfs fd dir opened (fd=%d)\n", vfsdev->proc_self_fd);
	return 0;
}

void
spdk_fsdev_aio_get_default_opts(struct spdk_fsdev_aio_opts *opts)
{
	assert(opts);

	memset(opts, 0, sizeof(*opts));

	opts->xattr_enabled = DEFAULT_XATTR_ENABLED;
	opts->writeback_cache_enabled = DEFAULT_WRITEBACK_CACHE;
	opts->max_xfer_size = DEFAULT_MAX_XFER_SIZE;
	opts->max_readahead = DEFAULT_MAX_READAHEAD;
	opts->skip_rw = DEFAULT_SKIP_RW;
	opts->enable_notifications = DEFAULT_ENABLE_NOTIFICATIONS;
	opts->attr_valid_ms = DEFAULT_ATTR_VALID_MS;
	opts->disable_copy_file_range = DEFAULT_DISABLE_COPY_FILE_RANGE;
}

int
spdk_fsdev_aio_create(struct spdk_fsdev **fsdev, const char *name, const char *root_path,
		      const struct spdk_fsdev_aio_opts *opts)
{
	struct aio_fsdev *vfsdev;
	int rc;

	if (opts->max_xfer_size > MAXIMUM_MAX_XFER_SIZE) {
		SPDK_ERRLOG("max_xfer_size %" PRIu32 " bigger than maximum allowed %" PRIu32 "\n",
			    opts->max_xfer_size, MAXIMUM_MAX_XFER_SIZE);
		return -EINVAL;
	}

	vfsdev = calloc(1, sizeof(*vfsdev));
	if (!vfsdev) {
		SPDK_ERRLOG("Could not allocate aio_fsdev\n");
		return -ENOMEM;
	}

	vfsdev->proc_self_fd = -1;

	vfsdev->fsdev.name = strdup(name);
	if (!vfsdev->fsdev.name) {
		SPDK_ERRLOG("Could not strdup fsdev name: %s\n", name);
		fsdev_aio_free(vfsdev);
		return -ENOMEM;
	}

	vfsdev->root_path = strdup(root_path);
	if (!vfsdev->root_path) {
		SPDK_ERRLOG("Could not strdup root path: %s\n", root_path);
		fsdev_aio_free(vfsdev);
		return -ENOMEM;
	}

	vfsdev->lut = spdk_lut_create(FILE_PTR_LUT_INIT_SIZE, FILE_PTR_LUT_GROWTH_STEP,
				      FILE_PTR_LUT_MAX_SIZE);
	if (!vfsdev->lut) {
		SPDK_ERRLOG("Could not create lookup table\n");
		fsdev_aio_free(vfsdev);
		return -ENOMEM;
	}

#ifdef SPDK_CONFIG_HAVE_FANOTIFY
	vfsdev->fanotify_fd = -1;
	RB_INIT(&vfsdev->linux_fhs);
#endif

	spdk_spin_init(&vfsdev->lock);

	rc = setup_root(vfsdev);
	if (rc) {
		SPDK_ERRLOG("Could not setup root: %s (err=%d)\n", root_path, rc);
		fsdev_aio_free(vfsdev);
		return rc;
	}

	rc = setup_proc_self_fd(vfsdev);
	if (rc) {
		SPDK_ERRLOG("Could not setup proc_self_fd (err=%d)\n", rc);
		fsdev_aio_free(vfsdev);
		return rc;
	}

	vfsdev->opts = *opts;
	vfsdev->fsdev.ctxt = vfsdev;
	vfsdev->fsdev.fn_table = &aio_fn_table;
	vfsdev->fsdev.module = &aio_fsdev_module;
#ifdef SPDK_CONFIG_HAVE_FANOTIFY
	if (vfsdev->opts.enable_notifications) {
		struct statvfs stat = {};

		/* Check if filesystem supports fanotify. According to fanotify_mark(2)
		 * it doesn't work for filesystems that report fsid 0.
		 */
		rc = fstatvfs(vfsdev->root->fd, &stat);
		if (rc) {
			rc = -errno;
			SPDK_ERRLOG("Failed to get filesystem statistics, fsdev %s, root_fd %d, errno %d\n",
				    vfsdev->fsdev.name, vfsdev->root->fd, errno);
			fsdev_aio_free(vfsdev);
			return rc;
		}

		if (stat.f_fsid == 0) {
			SPDK_ERRLOG("Fsdev %s does not support fanotify\n", vfsdev->fsdev.name);
			fsdev_aio_free(vfsdev);
			return -EINVAL;
		}
		vfsdev->fsdev.notify_max_data_size = DEFAULT_NOTIFY_MAX_DATA_SIZE;
	}
#endif

	vfsdev->fsdev.supported_fuse_opcodes = UINT64_MAX;

	rc = spdk_fsdev_register(&vfsdev->fsdev);
	if (rc) {
		fsdev_aio_free(vfsdev);
		return rc;
	}

	vfsdev->mount_opts.max_xfer_size = opts->max_xfer_size;
	vfsdev->mount_opts.max_readahead = opts->max_readahead;

	*fsdev = &(vfsdev->fsdev);
	TAILQ_INSERT_TAIL(&g_aio_fsdev_head, vfsdev, tailq);
	SPDK_DEBUGLOG(fsdev_aio, "Created aio filesystem %s: (xattr_enabled=%" PRIu8 " writeback_cache=%"
		      PRIu8 " max_xfer_size=%" PRIu32 " max_readahead=%" PRIu32 " skip_rw=%" PRIu8 " io_uring=%d)\n",
		      vfsdev->fsdev.name, vfsdev->opts.xattr_enabled, vfsdev->opts.writeback_cache_enabled,
		      vfsdev->opts.max_xfer_size, vfsdev->opts.max_readahead, vfsdev->opts.skip_rw,
		      aio_fsdev_use_io_uring_rdwr());
	return rc;
}
void
spdk_fsdev_aio_delete(const char *name,
		      spdk_delete_aio_fsdev_complete cb_fn, void *cb_arg)
{
	int rc;

	rc = spdk_fsdev_unregister_by_name(name, &aio_fsdev_module, cb_fn, cb_arg);
	if (rc != 0) {
		cb_fn(cb_arg, rc);
	}

	SPDK_DEBUGLOG(fsdev_aio, "Deleted aio filesystem %s\n", name);
}

void
fsdev_aio_get_opts(struct fsdev_aio_module_opts *opts)
{
	*opts = g_opts;
}

int
fsdev_aio_set_opts(const struct fsdev_aio_module_opts *opts)
{
	if (opts->max_io_depth == 0) {
		return -EINVAL;
	}

	g_opts = *opts;
	return 0;
}
SPDK_LOG_REGISTER_COMPONENT(fsdev_aio)
