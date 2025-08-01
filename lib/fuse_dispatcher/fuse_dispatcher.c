/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2024-2025 NVIDIA CORPORATION & AFFILIATES.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/event.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/fsdev.h"
#include "spdk/rpc.h"
#include "spdk/env.h"
#include "spdk/util.h"
#include "spdk/thread.h"
#include "spdk/likely.h"
#include "spdk/fuse_dispatcher.h"
#include "spdk/rmem.h"
#include "linux/fuse_kernel.h"

#ifndef UNUSED
#define UNUSED(x) (void)(x)
#endif

/* TODO: values, see https://libfuse.github.io/doxygen/structfuse__conn__info.html */
#define DEFAULT_TIME_GRAN 1
#define DEFAULT_MAX_BACKGROUND 0xffff
#define DEFAULT_CONGESTION_THRESHOLD 0xffff
#define DEFAULT_MAX_READAHEAD 0x00020000
#define OFFSET_MAX 0x7fffffffffffffffLL

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

/* Size of fuse_init_in in v7.6 - v7.36 */
#define COMPAT_FUSE_INIT_IN_6_SIZE 16
/* Size of fuse_init_in w/ FUSE_INIT_EXT set */
#define COMPAT_FUSE_INIT_IN_EXT_SIZE 64

/* SPDK only supports minor version 34 currently, even though our fuse_kernel.h
 * has a higher number.
 */
#define SPDK_FUSE_KERNEL_MINOR_VERSION 34

/*
 * NOTE: It appeared that the open flags have different values on the different HW architechtures.
 *
 * This code handles the open flags translation in case they're originated from a platform with
 * a different HW architecture.
 *
 * Currently supported:
 *  - X86
 *  - X86_64
 *  - ARM
 *  - ARM64
 */
/* See https://lxr.missinglinkelectronics.com/linux/arch/arm/include/uapi/asm/fcntl.h */
#define ARM_O_DIRECTORY      040000 /* must be a directory */
#define ARM_O_NOFOLLOW      0100000 /* don't follow links */
#define ARM_O_DIRECT        0200000 /* direct disk access hint - currently ignored */
#define ARM_O_LARGEFILE     0400000

/* See https://lxr.missinglinkelectronics.com/linux/include/uapi/asm-generic/fcntl.h */
#define X86_O_DIRECT        00040000        /* direct disk access hint */
#define X86_O_LARGEFILE     00100000
#define X86_O_DIRECTORY     00200000        /* must be a directory */
#define X86_O_NOFOLLOW      00400000        /* don't follow links */

static inline bool
fsdev_d2h_open_flags(enum spdk_fuse_arch fuse_arch, uint32_t flags, uint32_t *translated_flags)
{
	bool res = true;

	*translated_flags = flags;

	/* NOTE: we always check the original flags to avoid situation where the arch and the native flags
	 * overlap and previously set native flag could be interpreted as original arch flag.
	 */
#define REPLACE_FLAG(arch_flag, native_flag) \
	do { \
		if (flags & (arch_flag)) { \
			*translated_flags &= ~(arch_flag); \
			*translated_flags |= (native_flag); \
		} \
	} while(0)

	switch (fuse_arch) {
	case SPDK_FUSE_ARCH_NATIVE:
#if defined(__x86_64__) || defined(__i386__)
	case SPDK_FUSE_ARCH_X86:
	case SPDK_FUSE_ARCH_X86_64:
#endif
#if defined(__aarch64__) || defined(__arm__)
	case SPDK_FUSE_ARCH_ARM:
	case SPDK_FUSE_ARCH_ARM64:
#endif
		/* No translation required */
		break;
#if defined(__x86_64__) || defined(__i386__)
	case SPDK_FUSE_ARCH_ARM:
	case SPDK_FUSE_ARCH_ARM64:
		/* Relace the ARM-specific flags with the native ones */
		REPLACE_FLAG(ARM_O_DIRECTORY, O_DIRECTORY);
		REPLACE_FLAG(ARM_O_NOFOLLOW, O_NOFOLLOW);
		REPLACE_FLAG(ARM_O_DIRECT, O_DIRECT);
		REPLACE_FLAG(ARM_O_LARGEFILE, O_LARGEFILE);
		break;
#endif
#if defined(__aarch64__) || defined(__arm__)
	case SPDK_FUSE_ARCH_X86:
	case SPDK_FUSE_ARCH_X86_64:
		/* Relace the X86-specific flags with the native ones */
		REPLACE_FLAG(X86_O_DIRECTORY, O_DIRECTORY);
		REPLACE_FLAG(X86_O_NOFOLLOW, O_NOFOLLOW);
		REPLACE_FLAG(X86_O_DIRECT, O_DIRECT);
		REPLACE_FLAG(X86_O_LARGEFILE, O_LARGEFILE);
		break;
#endif
	default:
		SPDK_ERRLOG("Unsupported FUSE arch: %d\n", fuse_arch);
		assert(0);
		res = false;
		break;
	}

#undef REPLACE_FLAG

	return res;
}

struct fuse_forget_data {
	uint64_t ino;
	uint64_t nlookup;
};

struct iov_offs {
	size_t iov_offs;
	size_t buf_offs;
};

struct fuse_io {
	/** For SG buffer cases, array of iovecs for input. */
	struct iovec *in_iov;

	/** For SG buffer cases, number of iovecs in in_iov array. */
	int in_iovcnt;

	/** For SG buffer cases, array of iovecs for output. */
	struct iovec *out_iov;

	/** For SG buffer cases, number of iovecs in out_iov array. */
	int out_iovcnt;

	struct iov_offs in_offs;
	struct iov_offs out_offs;

	spdk_fuse_dispatcher_submit_cpl_cb cpl_cb;
	void *cpl_cb_arg;
	struct spdk_io_channel *ch;
	struct spdk_fuse_dispatcher *disp;

	struct fuse_in_header hdr;
	bool in_hdr_with_data;

	uint16_t source_id;
	uint64_t source_unique;

	union {
		struct {
			struct spdk_thread *thread;
			struct fuse_init_in *in;
			size_t out_len;
			int error;
		} init;
		struct {
			bool plus;
			uint32_t size;
			char *writep;
			uint32_t bytes_written;
		} readdir;
		struct {
			uint32_t to_forget;
			struct fuse_forget_data *forgets;
			int status;
			uint32_t idx;
		} batch_forget;

		struct {
			/* File handle of the poll event operation. */
			uint64_t fhandle;

			/* Requested event mask for poll operation. */
			uint32_t events;
		} poll;

		struct {
			int status;
		} fsdev_close;
		struct {
			/* Input lock for setlkw operation. */
			struct spdk_fsdev_file_lock lock;

			/* Input file handle for setlkw operation. */
			uint64_t fhandle;

			/* Input owner for setlkw operation. */
			uint64_t owner;
		} setlkw;
		struct {
			/*
			 * The flags in the ioctl() request. Used in completion
			 * to populate the out that is done differenly for the
			 * "unrestricted".
			 */
			uint32_t flags;

			/*
			 * Saved input out_size and used in compeltion cb
			 * for restricted ioctl().
			 */
			uint32_t out_size;

			/*
			 * Input in and out iovs and counts. These are passed down
			 * to the FSDEV and have to stay alive until the fuse_io
			 * completion.
			 *
			 * Alloctaed in do_ioctl() and freed in the ioctl completion.
			 * when the data is sent back to the kernel.
			 */
			struct iovec *in_iov;
			struct iovec *out_iov;
			uint32_t in_iovcnt;
			uint32_t out_iovcnt;
		} ioctl;
	} u;
};

/* To make sure that the fsdev_io that follows the fuse_io is aligned to 8 */
#define ALIGNED_FUSE_IO_SIZE SPDK_ALIGN_CEIL(sizeof(struct fuse_io), 8)

struct fuse_disp_recovery_data {
	uint32_t proto_major;
	uint32_t proto_minor;
	uint64_t root_fobject;
};

struct spdk_fuse_dispatcher {
	/**
	 * Try to use readdir_simple.
	 */
	uint64_t use_readdir_simple : 1;

	/**
	 * Reserved
	 */
	uint64_t reserved : 63;

	/**
	 * FUSE opcodes supported using SPDK_FSDEV_IO_FUSE.
	 */
	uint64_t supported_fuse_opcodes;

	/**
	 * fsdev descriptor
	 */
	struct spdk_fsdev_desc *desc;

	/**
	 * Major version of the protocol (read-only)
	 */
	uint32_t proto_major;

	/**
	 * Minor version of the protocol (read-only)
	 */
	uint32_t proto_minor;

	/**
	 * FUSE request source's architecture
	 */
	enum spdk_fuse_arch fuse_arch;

	/**
	 * Root file object
	 */
	struct spdk_fsdev_file_object *root_fobject;

	/**
	 * Negotiated mount flags.
	 */
	uint64_t mount_flags;

	/**
	 * Recovery memory pool.
	 */
	struct spdk_rmem_pool *rmem_pool;

	/**
	 * Recovery memory entry (data).
	 */
	struct spdk_rmem_entry *rmem_data;

	/**
	 * Callback to handle FUSE_NOTIFY_REPLY requests.
	 */
	spdk_fuse_dispatcher_notify_reply_cb notify_reply_cb;

	/**
	 * Context for notify_reply_cb.
	 */
	void *notify_reply_cb_arg;
};

struct fuse_notify_reply_in {
	int32_t error; /* 0 on success, negated errno for error */
	uint32_t padding;
};

static inline const char *
fuse_dispatcher_name(struct spdk_fuse_dispatcher *disp)
{
	return spdk_fsdev_get_name(spdk_fsdev_desc_get_fsdev(disp->desc));
}

static inline uint64_t
file_ino(struct spdk_fuse_dispatcher *disp, const struct spdk_fsdev_file_object *fobject)
{
	return (disp->root_fobject == fobject) ? FUSE_ROOT_ID : (uint64_t)(uintptr_t)fobject;
}

static struct spdk_fsdev_file_object *
ino_to_object(struct fuse_io *fuse_io, uint64_t ino)
{
	return (ino == FUSE_ROOT_ID) ?
	       fuse_io->disp->root_fobject :
	       (struct spdk_fsdev_file_object *)(uintptr_t)ino;
}

static struct spdk_fsdev_file_object *
file_object(struct fuse_io *fuse_io)
{
	return ino_to_object(fuse_io, fuse_io->hdr.nodeid);
}

static inline uint64_t
file_fh(const struct spdk_fsdev_file_handle *fhandle)
{
	return (uint64_t)(uintptr_t)fhandle;
}

static struct spdk_fsdev_file_handle *
file_handle(uint64_t fh)
{
	return (struct spdk_fsdev_file_handle *)(uintptr_t)fh;
}

static inline uint16_t
fsdev_io_d2h_u16(struct spdk_fuse_dispatcher *disp, uint16_t v)
{
	return v;
}

static inline uint16_t
fsdev_io_h2d_u16(struct spdk_fuse_dispatcher *disp, uint16_t v)
{
	return v;
}

static inline uint32_t
fsdev_io_d2h_u32(struct spdk_fuse_dispatcher *disp, uint32_t v)
{
	return v;
}

static inline uint32_t
fsdev_io_h2d_u32(struct spdk_fuse_dispatcher *disp, uint32_t v)
{
	return v;
}

static inline int32_t
fsdev_io_d2h_i32(struct spdk_fuse_dispatcher *disp, int32_t v)
{
	return v;
}

static inline int32_t
fsdev_io_h2d_i32(struct spdk_fuse_dispatcher *disp, int32_t v)
{
	return v;
}

static inline uint64_t
fsdev_io_d2h_u64(struct spdk_fuse_dispatcher *disp, uint64_t v)
{
	return v;
}

static inline uint64_t
fsdev_io_h2d_u64(struct spdk_fuse_dispatcher *disp, uint64_t v)
{
	return v;
}

static inline uint32_t
fsdev_io_proto_minor(struct fuse_io *fuse_io)
{
	return fuse_io->disp->proto_minor;
}

static inline void *
_iov_arr_get_buf_info(struct iovec *iovs, size_t cnt, struct iov_offs *offs, size_t *size)
{
	struct iovec *iov;

	assert(offs->iov_offs <= cnt);

	if (offs->iov_offs == cnt) {
		assert(!offs->buf_offs);
		*size = 0;
		return NULL;
	}

	iov = &iovs[offs->iov_offs];

	assert(offs->buf_offs < iov->iov_len);

	*size = iov->iov_len - offs->buf_offs;

	return ((char *)iov->iov_base) + offs->buf_offs;
}

static inline void *
_iov_arr_get_buf(struct iovec *iovs, size_t cnt, struct iov_offs *offs, size_t size,
		 const char *direction)
{
	char *arg_buf;
	size_t arg_size;

	arg_buf = _iov_arr_get_buf_info(iovs, cnt, offs, &arg_size);
	if (!arg_buf) {
		SPDK_INFOLOG(fuse_dispatcher, "Requested %s buffer is already consumed or not existing: "
			     "count=%d, attached=%zu:%zu\n", direction, (int)cnt, offs->iov_offs, offs->buf_offs);
		return NULL;
	}

	if (!arg_size) {
		SPDK_INFOLOG(fuse_dispatcher, "Requested %s buffer attached at %zu:%zu has zero length\n",
			     direction, offs->iov_offs, offs->buf_offs);
		return NULL;
	}

	if (size > arg_size) {
		SPDK_INFOLOG(fuse_dispatcher, "Requested %s buffer is too small (expected size = %zu > actual "
			     "size = %zu) at %zu:%zu\n", direction, size, arg_size, offs->iov_offs, offs->buf_offs);
		return NULL;
	}

	if (size == arg_size) {
		offs->iov_offs++;
		offs->buf_offs = 0;
	} else {
		offs->buf_offs += size;
	}

	return arg_buf;
}

static inline const char *
_fsdev_io_in_arg_get_str(struct fuse_io *fuse_io)
{
	char *arg_buf;
	size_t arg_size, len;

	arg_buf = _iov_arr_get_buf_info(fuse_io->in_iov, fuse_io->in_iovcnt, &fuse_io->in_offs,
					&arg_size);
	if (!arg_buf) {
		SPDK_ERRLOG("Requested IN string buffer is already consumed or not existing: count=%d, attached=%zu:%zu\n",
			    fuse_io->in_iovcnt, fuse_io->in_offs.iov_offs, fuse_io->in_offs.buf_offs);
		return NULL;
	}

	len = strnlen(arg_buf, arg_size);
	if (len == arg_size) {
		SPDK_ERRLOG("No string or bad string attached at %zu:%zu\n", fuse_io->in_offs.iov_offs,
			    fuse_io->in_offs.buf_offs);
		return NULL;
	}

	fuse_io->in_offs.buf_offs += len + 1;

	if (len + 1 == arg_size) {
		fuse_io->in_offs.iov_offs++;
		fuse_io->in_offs.buf_offs = 0;
	}

	return arg_buf;
}

static inline void *
_fsdev_io_in_arg_get_buf(struct fuse_io *fuse_io, size_t size)
{
	return _iov_arr_get_buf(fuse_io->in_iov, fuse_io->in_iovcnt, &fuse_io->in_offs, size, "IN");
}


static inline void *
_fsdev_io_out_arg_get_buf(struct fuse_io *fuse_io, size_t size)
{
	return _iov_arr_get_buf(fuse_io->out_iov, fuse_io->out_iovcnt, &fuse_io->out_offs, size,
				"OUT");
}

static inline struct spdk_fsdev_io *
fuse_to_fsdev_io(struct fuse_io *fuse_io)
{
	return (struct spdk_fsdev_io *)(((char *)fuse_io) + ALIGNED_FUSE_IO_SIZE);
}

static inline struct fuse_io *
fsdev_to_fuse_io(struct spdk_fsdev_io *fsdev_io)
{
	return (struct fuse_io *)(((char *)fsdev_io) - ALIGNED_FUSE_IO_SIZE);
}

static bool
_fuse_op_requires_reply(uint32_t opcode)
{
	switch (opcode) {
	case FUSE_FORGET:
	case FUSE_BATCH_FORGET:
	case FUSE_NOTIFY_REPLY:
	case FUSE_INTERRUPT:
		return false;
	default:
		return true;
	}
}

static void
fuse_dispatcher_update_rmem(struct spdk_fuse_dispatcher *disp)
{
	if (disp->rmem_data) {
		struct fuse_disp_recovery_data data = {
			.proto_major = disp->proto_major,
			.proto_minor = disp->proto_minor,
			.root_fobject = (uint64_t)(uintptr_t)disp->root_fobject,
		};

		spdk_rmem_entry_write(disp->rmem_data, &data);
	}
}

static void
fsdev_attr_to_fuse(struct fuse_io *fuse_io, struct spdk_fsdev_file_object *fobject,
		   const struct spdk_fsdev_file_attr *attr, struct fuse_attr *fattr)
{
	fattr->ino	= fsdev_io_h2d_u64(fuse_io->disp, attr->ino);
	fattr->mode	= fsdev_io_h2d_u32(fuse_io->disp, attr->mode);
	fattr->nlink	= fsdev_io_h2d_u32(fuse_io->disp, attr->nlink);
	fattr->uid	= fsdev_io_h2d_u32(fuse_io->disp, attr->uid);
	fattr->gid	= fsdev_io_h2d_u32(fuse_io->disp, attr->gid);
	fattr->rdev	= fsdev_io_h2d_u32(fuse_io->disp, attr->rdev);
	fattr->size	= fsdev_io_h2d_u64(fuse_io->disp, attr->size);
	fattr->blksize	= fsdev_io_h2d_u32(fuse_io->disp, attr->blksize);
	fattr->blocks	= fsdev_io_h2d_u64(fuse_io->disp, attr->blocks);
	fattr->atime	= fsdev_io_h2d_u64(fuse_io->disp, attr->atime);
	fattr->mtime	= fsdev_io_h2d_u64(fuse_io->disp, attr->mtime);
	fattr->ctime	= fsdev_io_h2d_u64(fuse_io->disp, attr->ctime);
	fattr->atimensec = fsdev_io_h2d_u32(fuse_io->disp, attr->atimensec);
	fattr->mtimensec = fsdev_io_h2d_u32(fuse_io->disp, attr->mtimensec);
	fattr->ctimensec = fsdev_io_h2d_u32(fuse_io->disp, attr->ctimensec);
}

static uint32_t
calc_timeout_sec(uint32_t ms)
{
	return ms / 1000;
}

static uint32_t
calc_timeout_nsec(uint32_t ms)
{
	return (ms % 1000) * 1000000;
}

static void
fill_entry(struct fuse_io *fuse_io, struct fuse_entry_out *arg,
	   struct spdk_fsdev_file_object *fobject, const struct spdk_fsdev_file_attr *attr)
{

	arg->nodeid = fsdev_io_h2d_u64(fuse_io->disp, file_ino(fuse_io->disp, fobject));
	arg->generation = 0;
	arg->entry_valid = fsdev_io_h2d_u64(fuse_io->disp, calc_timeout_sec(attr->valid_ms));
	arg->entry_valid_nsec = fsdev_io_h2d_u32(fuse_io->disp, calc_timeout_nsec(attr->valid_ms));
	arg->attr_valid = fsdev_io_h2d_u64(fuse_io->disp, calc_timeout_sec(attr->valid_ms));
	arg->attr_valid_nsec = fsdev_io_h2d_u32(fuse_io->disp, calc_timeout_nsec(attr->valid_ms));
	fsdev_attr_to_fuse(fuse_io, fobject, attr, &arg->attr);
}

static void
fill_open(struct fuse_io *fuse_io, struct fuse_open_out *arg,
	  struct spdk_fsdev_file_handle *fhandle, uint32_t flags)
{
	uint32_t open_flags = 0;

	if (flags & O_DIRECT) {
		open_flags |= FOPEN_DIRECT_IO;
	}

	arg->fh = fsdev_io_h2d_u64(fuse_io->disp, file_fh(fhandle));
	arg->open_flags = fsdev_io_h2d_u64(fuse_io->disp, open_flags);
}

static void
convert_statfs(struct fuse_io *fuse_io, const struct spdk_fsdev_file_statfs *statfs,
	       struct fuse_kstatfs *kstatfs)
{
	kstatfs->bsize	 = fsdev_io_h2d_u32(fuse_io->disp, statfs->bsize);
	kstatfs->frsize	 = fsdev_io_h2d_u32(fuse_io->disp, statfs->frsize);
	kstatfs->blocks	 = fsdev_io_h2d_u64(fuse_io->disp, statfs->blocks);
	kstatfs->bfree	 = fsdev_io_h2d_u64(fuse_io->disp, statfs->bfree);
	kstatfs->bavail	 = fsdev_io_h2d_u64(fuse_io->disp, statfs->bavail);
	kstatfs->files	 = fsdev_io_h2d_u64(fuse_io->disp, statfs->files);
	kstatfs->ffree	 = fsdev_io_h2d_u64(fuse_io->disp, statfs->ffree);
	kstatfs->namelen = fsdev_io_h2d_u32(fuse_io->disp, statfs->namelen);
}

static struct fuse_out_header *
fuse_dispatcher_fill_out_hdr(struct fuse_io *fuse_io, size_t out_len, int error)
{
	struct fuse_out_header *hdr;
	struct iovec *out;
	uint32_t len;

	assert(fuse_io->out_iovcnt >= 1);
	assert(error <= 0);

	out = fuse_io->out_iov;

	if (out->iov_len < sizeof(*hdr)) {
		SPDK_ERRLOG("Bad out header len: %zu < %zu\n", out->iov_len, sizeof(*hdr));
		return NULL;
	}

	if (error < -1000) {
		SPDK_ERRLOG("Bad completion error value: %" PRIu32 "\n", error);
		return NULL;
	}

	len = sizeof(*hdr);
	if (error == 0) {
		len += out_len;
	}

	hdr = out->iov_base;
	memset(hdr, 0, sizeof(*hdr));


	hdr->unique = fsdev_io_h2d_u64(fuse_io->disp, fuse_io->hdr.unique);
	hdr->error = fsdev_io_h2d_i32(fuse_io->disp, error);
	hdr->len = fsdev_io_h2d_u32(fuse_io->disp, len);

	return hdr;
}

static void
fuse_dispatcher_io_complete_final(struct fuse_io *fuse_io, int error)
{
	spdk_fuse_dispatcher_submit_cpl_cb cpl_cb = fuse_io->cpl_cb;
	void *cpl_cb_arg = fuse_io->cpl_cb_arg;

	cpl_cb(cpl_cb_arg, error);
}

static void
fuse_dispatcher_io_complete(struct fuse_io *fuse_io, uint32_t out_len, int error)
{
	struct fuse_out_header *hdr = fuse_dispatcher_fill_out_hdr(fuse_io, out_len, error);

	assert(_fuse_op_requires_reply(fuse_io->hdr.opcode));

	if (!hdr) {
		SPDK_ERRLOG("Completion failed: cannot fill out header\n");
		return;
	}

	SPDK_DEBUGLOG(fuse_dispatcher,
		      "Completing IO#%" PRIu64 " (err=%d, out_len=%" PRIu32 ")\n",
		      fuse_io->hdr.unique, error, out_len);

	fuse_dispatcher_io_complete_final(fuse_io, error);
}

static void
fuse_dispatcher_io_copy_and_complete(struct fuse_io *fuse_io, const void *out, uint32_t out_len,
				     int error)
{
	if (out && out_len) {
		void *buf = _fsdev_io_out_arg_get_buf(fuse_io, out_len);
		if (buf) {
			memcpy(buf, out, out_len);
		} else {
			SPDK_ERRLOG("Completion failed: cannot get buf to copy %" PRIu32 " bytes\n", out_len);
			error = -EINVAL;
			out_len = 0;
		}
	}

	fuse_dispatcher_io_complete(fuse_io, out_len, error);
}

static void
fuse_dispatcher_io_complete_none(struct fuse_io *fuse_io, int err)
{
	SPDK_DEBUGLOG(fuse_dispatcher, "Completing IO#%" PRIu64 " (err=%d)\n",
		      fuse_io->hdr.unique, err);
	fuse_dispatcher_io_complete_final(fuse_io, err);
}

static void
fuse_dispatcher_io_complete_ok(struct fuse_io *fuse_io, uint32_t out_len)
{
	fuse_dispatcher_io_complete(fuse_io, out_len, 0);
}

static void
fuse_dispatcher_io_complete_err(struct fuse_io *fuse_io, int err)
{
	fuse_dispatcher_io_complete(fuse_io, 0, err);
}

static void
fuse_dispatcher_io_complete_entry(struct fuse_io *fuse_io, struct spdk_fsdev_file_object *fobject,
				  const struct spdk_fsdev_file_attr *attr)
{
	struct fuse_entry_out arg;
	size_t size = fsdev_io_proto_minor(fuse_io) < 9 ?
		      FUSE_COMPAT_ENTRY_OUT_SIZE : sizeof(arg);

	memset(&arg, 0, sizeof(arg));
	fill_entry(fuse_io, &arg, fobject, attr);

	fuse_dispatcher_io_copy_and_complete(fuse_io, &arg, size, 0);
}

static void
fuse_dispatcher_io_complete_open(struct fuse_io *fuse_io, struct spdk_fsdev_file_handle *fhandle,
				 uint32_t flags)
{
	struct fuse_open_out *arg;

	arg = _fsdev_io_out_arg_get_buf(fuse_io, sizeof(*arg));
	if (!arg) {
		SPDK_ERRLOG("Cannot get fuse_open_out\n");
		fuse_dispatcher_io_complete_err(fuse_io, -EINVAL);
		return;
	}

	fill_open(fuse_io, arg, fhandle, flags);

	fuse_dispatcher_io_complete_ok(fuse_io, sizeof(*arg));
}

static void
fuse_dispatcher_io_complete_create(struct fuse_io *fuse_io, struct spdk_fsdev_file_object *fobject,
				   const struct spdk_fsdev_file_attr *attr,
				   struct spdk_fsdev_file_handle *fhandle, uint32_t flags)
{
	char buf[sizeof(struct fuse_entry_out) + sizeof(struct fuse_open_out)];
	size_t entrysize = fsdev_io_proto_minor(fuse_io) < 9 ?
			   FUSE_COMPAT_ENTRY_OUT_SIZE : sizeof(struct fuse_entry_out);
	struct fuse_entry_out *earg = (struct fuse_entry_out *) buf;
	struct fuse_open_out *oarg = (struct fuse_open_out *)(buf + entrysize);

	memset(buf, 0, sizeof(buf));
	fill_entry(fuse_io, earg, fobject, attr);
	fill_open(fuse_io, oarg, fhandle, flags);

	fuse_dispatcher_io_copy_and_complete(fuse_io, buf, entrysize + sizeof(struct fuse_open_out), 0);
}

static void
fuse_dispatcher_io_complete_xattr(struct fuse_io *fuse_io, uint32_t count)
{
	struct fuse_getxattr_out *arg;

	arg = _fsdev_io_out_arg_get_buf(fuse_io, sizeof(*arg));
	if (!arg) {
		SPDK_ERRLOG("Cannot get fuse_getxattr_out\n");
		fuse_dispatcher_io_complete_err(fuse_io, -EINVAL);
		return;
	}

	arg->size = fsdev_io_h2d_i32(fuse_io->disp, count);

	fuse_dispatcher_io_complete_ok(fuse_io, sizeof(*arg));
}

static void
fuse_dispatcher_io_complete_write(struct fuse_io *fuse_io, uint32_t data_size, int error)
{
	struct fuse_write_out *arg;

	arg = _fsdev_io_out_arg_get_buf(fuse_io, sizeof(*arg));
	if (!arg) {
		SPDK_ERRLOG("Cannot get fuse_write_out\n");
		fuse_dispatcher_io_complete_err(fuse_io, -EINVAL);
		return;
	}

	arg->size = fsdev_io_d2h_u32(fuse_io->disp, data_size);

	fuse_dispatcher_io_complete(fuse_io, sizeof(*arg), error);
}

static void
fuse_dispatcher_io_complete_statfs(struct fuse_io *fuse_io,
				   const struct spdk_fsdev_file_statfs *statfs)
{
	struct fuse_statfs_out arg;
	size_t size = fsdev_io_proto_minor(fuse_io) < 4 ?
		      FUSE_COMPAT_STATFS_SIZE : sizeof(arg);

	memset(&arg, 0, sizeof(arg));
	convert_statfs(fuse_io, statfs, &arg.st);

	return fuse_dispatcher_io_copy_and_complete(fuse_io, &arg, size, 0);
}

static void
fuse_dispatcher_io_complete_attr(struct fuse_io *fuse_io, const struct spdk_fsdev_file_attr *attr)
{
	struct fuse_attr_out arg;
	size_t size = fsdev_io_proto_minor(fuse_io) < 9 ?
		      FUSE_COMPAT_ATTR_OUT_SIZE : sizeof(arg);

	memset(&arg, 0, sizeof(arg));
	arg.attr_valid = fsdev_io_h2d_u64(fuse_io->disp, calc_timeout_sec(attr->valid_ms));
	arg.attr_valid_nsec = fsdev_io_h2d_u32(fuse_io->disp, calc_timeout_nsec(attr->valid_ms));
	fsdev_attr_to_fuse(fuse_io, file_object(fuse_io), attr, &arg.attr);

	fuse_dispatcher_io_copy_and_complete(fuse_io, &arg, size, 0);
}

static void
fuse_dispatcher_io_complete_lseek(struct fuse_io *fuse_io, off_t offset)
{
	struct fuse_lseek_out arg;
	size_t size = sizeof(arg);

	arg.offset = fsdev_io_h2d_u64(fuse_io->disp, offset);

	fuse_dispatcher_io_copy_and_complete(fuse_io, &arg, size, 0);
}

static uint32_t
fsdev_events_to_fuse(uint32_t spdk_events)
{
	uint32_t result = 0;

	if (spdk_events & SPDK_FSDEV_POLLIN) {
		result |= POLLIN;
	}
	if (spdk_events & SPDK_FSDEV_POLLOUT) {
		result |= POLLOUT;
	}
	if (spdk_events & SPDK_FSDEV_POLLPRI) {
		result |= POLLPRI;
	}
	if (spdk_events & SPDK_FSDEV_POLLERR) {
		result |= POLLERR;
	}
	if (spdk_events & SPDK_FSDEV_POLLHUP) {
		result |= POLLHUP;
	}
	if (spdk_events & SPDK_FSDEV_POLLNVAL) {
		result |= POLLNVAL;
	}
	if (spdk_events & SPDK_FSDEV_POLLRDNORM) {
		result |= POLLRDNORM;
	}
	if (spdk_events & SPDK_FSDEV_POLLRDBAND) {
		result |= POLLRDBAND;
	}
	if (spdk_events & SPDK_FSDEV_POLLWRNORM) {
		result |= POLLWRNORM;
	}
	if (spdk_events & SPDK_FSDEV_POLLWRBAND) {
		result |= POLLWRBAND;
	}

	return result;
}

static uint32_t
fuse_events_to_fsdev(uint32_t events)
{
	uint32_t result = 0;

	if (events & POLLIN) {
		result |= SPDK_FSDEV_POLLIN;
	}
	if (events & POLLOUT) {
		result |= SPDK_FSDEV_POLLOUT;
	}
	if (events & POLLPRI) {
		result |= SPDK_FSDEV_POLLPRI;
	}
	if (events & POLLERR) {
		result |= SPDK_FSDEV_POLLERR;
	}
	if (events & POLLHUP) {
		result |= SPDK_FSDEV_POLLHUP;
	}
	if (events & POLLNVAL) {
		result |= SPDK_FSDEV_POLLNVAL;
	}
	if (events & POLLRDNORM) {
		result |= SPDK_FSDEV_POLLRDNORM;
	}
	if (events & POLLRDBAND) {
		result |= SPDK_FSDEV_POLLRDBAND;
	}
	if (events & POLLWRNORM) {
		result |= SPDK_FSDEV_POLLWRNORM;
	}
	if (events & POLLWRBAND) {
		result |= SPDK_FSDEV_POLLWRBAND;
	}

	return result;
}

static void
fuse_dispatcher_io_complete_poll(struct fuse_io *fuse_io, uint32_t revents)
{
	struct fuse_poll_out *arg = _fsdev_io_out_arg_get_buf(fuse_io, sizeof(*arg));

	if (!arg) {
		SPDK_ERRLOG("Cannot get fuse_poll_out\n");
		fuse_dispatcher_io_complete_err(fuse_io, -EINVAL);
		return;
	}
	arg->revents = fsdev_io_h2d_u32(fuse_io->disp, fsdev_events_to_fuse(revents));

	fuse_dispatcher_io_complete_ok(fuse_io, sizeof(*arg));
}
#if DEBUG
static const char *
fuse_lock_type_to_str(uint32_t fuse_lock_type)
{
	if (fuse_lock_type == F_RDLCK) {
		return "F_RDLCK";
	} else if (fuse_lock_type == F_WRLCK) {
		return "F_WRLCK";
	} else if (fuse_lock_type == F_UNLCK) {
		return "F_UNLCK";
	} else {
		return "UNKNOWN";
	}
}
#endif

static int
fuse_to_fsdev_file_lock(struct fuse_io *fuse_io, const struct fuse_file_lock *fuse_lock,
			struct spdk_fsdev_file_lock *fsdev_lock)
{
	switch (fsdev_io_d2h_u32(fuse_io->disp, fuse_lock->type)) {
	case F_RDLCK:
		fsdev_lock->type = SPDK_FSDEV_RDLCK;
		break;
	case F_WRLCK:
		fsdev_lock->type = SPDK_FSDEV_WRLCK;
		break;
	case F_UNLCK:
		fsdev_lock->type = SPDK_FSDEV_UNLCK;
		break;
	default:
		SPDK_ERRLOG("Invalid lock type %d during fuse to fsdev lock conversion.\n",
			    fsdev_io_d2h_u32(fuse_io->disp, fuse_lock->type));
		return -EINVAL;
	}
	fsdev_lock->start = fsdev_io_d2h_u64(fuse_io->disp, fuse_lock->start);
	fsdev_lock->end = fsdev_io_d2h_u64(fuse_io->disp, fuse_lock->end);
	if (fsdev_lock->end == 0) {
		fsdev_lock->end = SPDK_FSDEV_FILE_LOCK_END_OF_FILE;
	}
	fsdev_lock->pid = fsdev_io_d2h_u32(fuse_io->disp, fuse_lock->pid);

	SPDK_DEBUGLOG(fuse_dispatcher, "fuse -> fsdev lock type=%x, start=%lu, end=%lu, pid=%u\n",
		      fsdev_lock->type, fsdev_lock->start, fsdev_lock->end, fsdev_lock->pid);
	return 0;
}

static int
fsdev_file_lock_to_fuse(struct fuse_io *fuse_io, const struct spdk_fsdev_file_lock *fsdev_lock,
			struct fuse_file_lock *fuse_lock)
{
	switch (fsdev_lock->type) {
	case SPDK_FSDEV_RDLCK:
		fuse_lock->type = fsdev_io_h2d_u32(fuse_io->disp, F_RDLCK);
		break;
	case SPDK_FSDEV_WRLCK:
		fuse_lock->type = fsdev_io_h2d_u32(fuse_io->disp, F_WRLCK);
		break;
	case SPDK_FSDEV_UNLCK:
		fuse_lock->type = fsdev_io_h2d_u32(fuse_io->disp, F_UNLCK);
		break;
	default:
		SPDK_ERRLOG("Invalid lock type %d encountered during fsdev to fuse "
			    "locks conversion.\n", fsdev_lock->type);
		return -EINVAL;
	}

	fuse_lock->start = fsdev_io_h2d_u64(fuse_io->disp, fsdev_lock->start);
	fuse_lock->end = fsdev_io_h2d_u64(fuse_io->disp, fsdev_lock->end);
	fuse_lock->pid = fsdev_io_h2d_u32(fuse_io->disp, fsdev_lock->pid);

	SPDK_DEBUGLOG(fuse_dispatcher, "fsdev -> fuse lock type=%s, start=%lu, len=%lu, pid=%u\n",
		      fuse_lock_type_to_str(fsdev_io_d2h_u32(fuse_io->disp, fsdev_lock->type)),
		      fuse_lock->start, fuse_lock->end, fuse_lock->pid);
	return 0;
}

static void
fuse_dispatcher_io_complete_getlk(struct fuse_io *fuse_io,
				  const struct spdk_fsdev_file_lock *fsdev_lock)
{
	struct fuse_lk_out *arg;
	int err;

	arg = _fsdev_io_out_arg_get_buf(fuse_io, sizeof(*arg));
	if (!arg) {
		SPDK_ERRLOG("Cannot get fuse_lk_out\n");
		fuse_dispatcher_io_complete_err(fuse_io, -EINVAL);
		return;
	}

	err = fsdev_file_lock_to_fuse(fuse_io, fsdev_lock, &arg->lk);
	if (!err) {
		fuse_dispatcher_io_complete_ok(fuse_io, sizeof(*arg));
	} else {
		fuse_dispatcher_io_complete_err(fuse_io, err);
	}
}

/* `buf` is allowed to be empty so that the proper size may be
   allocated by the caller */
static size_t
fuse_dispatcher_add_direntry(struct fuse_io *fuse_io, char *buf, size_t bufsize, const char *name,
			     uint64_t ino, uint32_t type, off_t off)
{
	size_t namelen;
	size_t entlen;
	size_t entlen_padded;
	struct fuse_dirent *dirent;

	namelen = strlen(name);
	entlen = FUSE_NAME_OFFSET + namelen;
	entlen_padded = FUSE_DIRENT_ALIGN(entlen);

	if ((buf == NULL) || (entlen_padded > bufsize)) {
		return entlen_padded;
	}

	dirent = (struct fuse_dirent *) buf;
	dirent->ino = fsdev_io_h2d_u64(fuse_io->disp, ino);
	dirent->off = fsdev_io_h2d_u64(fuse_io->disp, off);
	dirent->namelen = fsdev_io_h2d_u32(fuse_io->disp, namelen);
	dirent->type = fsdev_io_h2d_u32(fuse_io->disp, type);
	memcpy(dirent->name, name, namelen);
	memset(dirent->name + namelen, 0, entlen_padded - entlen);

	return entlen_padded;
}

/* `buf` is allowed to be empty so that the proper size may be
   allocated by the caller */
static size_t
fuse_dispatcher_add_direntry_plus(struct fuse_io *fuse_io, char *buf, size_t bufsize,
				  const char *name, struct spdk_fsdev_file_object *fobject, const struct spdk_fsdev_file_attr *attr,
				  off_t off)
{
	size_t namelen;
	size_t entlen;
	size_t entlen_padded;

	namelen = strlen(name);
	entlen = FUSE_NAME_OFFSET_DIRENTPLUS + namelen;
	entlen_padded = FUSE_DIRENT_ALIGN(entlen);
	if ((buf == NULL) || (entlen_padded > bufsize)) {
		return entlen_padded;
	}

	struct fuse_direntplus *dp = (struct fuse_direntplus *) buf;
	memset(&dp->entry_out, 0, sizeof(dp->entry_out));
	fill_entry(fuse_io, &dp->entry_out, fobject, attr);

	struct fuse_dirent *dirent = &dp->dirent;
	dirent->ino = fsdev_io_h2d_u64(fuse_io->disp, attr->ino);
	dirent->off = fsdev_io_h2d_u64(fuse_io->disp, off);
	dirent->namelen = fsdev_io_h2d_u32(fuse_io->disp, namelen);
	dirent->type = fsdev_io_h2d_u32(fuse_io->disp, (attr->mode & 0170000) >> 12);
	memcpy(dirent->name, name, namelen);
	memset(dirent->name + namelen, 0, entlen_padded - entlen);

	return entlen_padded;
}

static void
fuse_dispatcher_no_resp_cpl_cb(void *cb_arg, int status, struct spdk_fsdev_io *fsdev_io)
{
	struct fuse_io *fuse_io = cb_arg;

	/* FUSE_FORGET and FUSE_INTERRUPT require no response */
	switch (spdk_fsdev_io_get_type(fsdev_io)) {
	case SPDK_FSDEV_IO_ABORT:
	case SPDK_FSDEV_IO_FORGET:
		fuse_dispatcher_io_complete_none(fuse_io, status);
		break;
	default:
		/* No other IOs should be completed thru this callback */
		assert(false);
		fuse_dispatcher_io_complete_err(fuse_io, -EINVAL);
		break;
	}
}

static void fuse_dispatcher_readdir_fallback_and_retry(struct spdk_fsdev_io *fsdev_io);

static void
fuse_dispatcher_readdir_cpl_cb(void *cb_arg, int status, struct spdk_fsdev_io *fsdev_io)
{
	struct fuse_io *fuse_io = cb_arg;
	enum spdk_fsdev_io_type type = spdk_fsdev_io_get_type(fsdev_io);
	struct spdk_fuse_dispatcher *disp = fuse_io->disp;

	switch (type) {
	case SPDK_FSDEV_IO_READDIR:
		break;
	case SPDK_FSDEV_IO_READDIR_SIMPLE:
		if (disp->use_readdir_simple && status == -ENOSYS) {
			/* if SPDK_FSDEV_IO_READDIR_SIMPLE is not supported by the underlying fsdev,
			 * we fallback to SPDK_FSDEV_IO_READDIR and retry.
			 */
			fuse_dispatcher_readdir_fallback_and_retry(fsdev_io);
			return;
		}
		break;
	default:
		assert(false);
		fuse_dispatcher_io_complete_err(fuse_io, -EINVAL);
		return;
	}

	if (status && status != -EAGAIN) {
		fuse_dispatcher_io_complete_err(fuse_io, status);
		return;
	}

	fuse_dispatcher_io_complete_ok(fuse_io, fuse_io->u.readdir.bytes_written);
}

static void
fuse_dispatcher_cpl_cb(void *cb_arg, int status, struct spdk_fsdev_io *fsdev_io)
{
	struct fuse_io *fuse_io = cb_arg;

	if (status) {
		fuse_dispatcher_io_complete_err(fuse_io, status);
		return;
	}

	switch (spdk_fsdev_io_get_type(fsdev_io)) {
	case SPDK_FSDEV_IO_MOUNT:
	case SPDK_FSDEV_IO_UMOUNT:
	case SPDK_FSDEV_IO_IOCTL:
	case SPDK_FSDEV_IO_ABORT:
	case SPDK_FSDEV_IO_FORGET:
	case SPDK_FSDEV_IO_READDIR:
		/* We use a different completion callback for these because the completion path is more complex */
		assert(false);
		fuse_dispatcher_io_complete_err(fuse_io, -EINVAL);
		return;
	case SPDK_FSDEV_IO_LOOKUP:
		fuse_dispatcher_io_complete_entry(fuse_io, fsdev_io->u_out.lookup.fobject,
						  &fsdev_io->u_out.lookup.attr);
		break;
	case SPDK_FSDEV_IO_GETATTR:
		fuse_dispatcher_io_complete_attr(fuse_io, &fsdev_io->u_out.getattr.attr);
		break;
	case SPDK_FSDEV_IO_SETATTR:
		fuse_dispatcher_io_complete_attr(fuse_io, &fsdev_io->u_out.setattr.attr);
		break;
	case SPDK_FSDEV_IO_READLINK:
		fuse_dispatcher_io_copy_and_complete(fuse_io, fsdev_io->u_out.readlink.linkname,
						     strlen(fsdev_io->u_out.readlink.linkname) + 1, 0);
		break;
	case SPDK_FSDEV_IO_SYMLINK:
		fuse_dispatcher_io_complete_entry(fuse_io, fsdev_io->u_out.symlink.fobject,
						  &fsdev_io->u_out.symlink.attr);
		break;
	case SPDK_FSDEV_IO_MKNOD:
		fuse_dispatcher_io_complete_entry(fuse_io, fsdev_io->u_out.mknod.fobject,
						  &fsdev_io->u_out.mknod.attr);
		break;
	case SPDK_FSDEV_IO_MKDIR:
		fuse_dispatcher_io_complete_entry(fuse_io, fsdev_io->u_out.mkdir.fobject,
						  &fsdev_io->u_out.mkdir.attr);
		break;
	case SPDK_FSDEV_IO_LINK:
		fuse_dispatcher_io_complete_entry(fuse_io, fsdev_io->u_out.link.fobject,
						  &fsdev_io->u_out.link.attr);
		break;
	case SPDK_FSDEV_IO_OPEN:
		fuse_dispatcher_io_complete_open(fuse_io, fsdev_io->u_out.open.fhandle,
						 fsdev_io->u_in.open.flags);
		break;
	case SPDK_FSDEV_IO_READ:
		fuse_dispatcher_io_complete(fuse_io, fsdev_io->u_out.read.data_size, status);
		break;
	case SPDK_FSDEV_IO_WRITE:
		fuse_dispatcher_io_complete_write(fuse_io, fsdev_io->u_out.write.data_size, status);
		break;
	case SPDK_FSDEV_IO_STATFS:
		fuse_dispatcher_io_complete_statfs(fuse_io, &fsdev_io->u_out.statfs.statfs);
		break;
	case SPDK_FSDEV_IO_GETXATTR:
		fuse_dispatcher_io_complete_xattr(fuse_io, fsdev_io->u_out.getxattr.value_size);
		break;
	case SPDK_FSDEV_IO_LISTXATTR:
		if (fsdev_io->u_out.listxattr.size_only) {
			fuse_dispatcher_io_complete_xattr(fuse_io, fsdev_io->u_out.listxattr.data_size);
		} else {
			fuse_dispatcher_io_complete_ok(fuse_io, fsdev_io->u_out.listxattr.data_size);
		}
		break;
	case SPDK_FSDEV_IO_OPENDIR:
		fuse_dispatcher_io_complete_open(fuse_io, fsdev_io->u_out.opendir.fhandle,
						 fsdev_io->u_in.opendir.flags);
		break;
	case SPDK_FSDEV_IO_CREATE:
		fuse_dispatcher_io_complete_create(fuse_io, fsdev_io->u_out.create.fobject,
						   &fsdev_io->u_out.create.attr,
						   fsdev_io->u_out.create.fhandle,
						   fsdev_io->u_in.create.flags);
		break;
	case SPDK_FSDEV_IO_COPY_FILE_RANGE:
		fuse_dispatcher_io_complete_write(fuse_io, fsdev_io->u_out.copy_file_range.data_size, status);
		break;
	case SPDK_FSDEV_IO_LSEEK:
		fuse_dispatcher_io_complete_lseek(fuse_io, fsdev_io->u_out.lseek.offset);
		break;
	case SPDK_FSDEV_IO_POLL:
		fuse_dispatcher_io_complete_poll(fuse_io, fsdev_io->u_out.poll.revents);
		break;
	case SPDK_FSDEV_IO_GETLK:
		fuse_dispatcher_io_complete_getlk(fuse_io, &fsdev_io->u_out.getlk.lock);
		break;
	default:
		fuse_dispatcher_io_complete_err(fuse_io, 0);
		break;
	}
}

/*
 * Static FUSE commands handlers
 */
static inline struct spdk_fsdev_desc *
fuse_io_desc(struct fuse_io *fuse_io)
{
	return fuse_io->disp->desc;
}

static inline void
fuse_init_fsdev_io_ex(struct fuse_io *fuse_io, enum spdk_fsdev_io_type type,
		      spdk_fsdev_cpl_cb *cb_fn)
{
	struct spdk_fuse_dispatcher *disp = fuse_io->disp;
	struct spdk_fsdev_io *fsdev_io = fuse_to_fsdev_io(fuse_io);

	spdk_fsdev_io_init(fsdev_io, disp->desc, fuse_io->ch, fuse_io->hdr.unique, type, fuse_io->source_id,
			   fuse_io->source_unique, cb_fn, fuse_io);
}

static inline void
fuse_init_fsdev_io(struct fuse_io *fuse_io, enum spdk_fsdev_io_type type)
{
	fuse_init_fsdev_io_ex(fuse_io, type, fuse_dispatcher_cpl_cb);
}

static int
fuse_dispatcher_fill_lookup(struct fuse_io *fuse_io)
{
	struct spdk_fsdev_io *fsdev_io = fuse_to_fsdev_io(fuse_io);
	const char *name = _fsdev_io_in_arg_get_str(fuse_io);
	if (!name) {
		SPDK_ERRLOG("No name or bad name attached\n");
		return -EINVAL;
	}

	fuse_init_fsdev_io(fuse_io, SPDK_FSDEV_IO_LOOKUP);

	fsdev_io->u_in.lookup.parent_fobject = file_object(fuse_io);
	fsdev_io->u_in.lookup.name = name;

	return 0;
}

static int
fuse_dispatcher_fill_forget(struct fuse_io *fuse_io)
{
	struct spdk_fsdev_io *fsdev_io = fuse_to_fsdev_io(fuse_io);
	struct fuse_forget_in *arg;

	arg = _fsdev_io_in_arg_get_buf(fuse_io, sizeof(*arg));
	if (!arg) {
		SPDK_ERRLOG("Cannot get fuse_forget_in\n");
		return -EINVAL;
	}

	/* FUSE_FORGET requires no response */
	fuse_init_fsdev_io_ex(fuse_io, SPDK_FSDEV_IO_FORGET, fuse_dispatcher_no_resp_cpl_cb);

	fsdev_io->u_in.forget.fobject = file_object(fuse_io);
	fsdev_io->u_in.forget.nlookup = fsdev_io_d2h_u64(fuse_io->disp, arg->nlookup);

	return 0;
}

static int
fuse_dispatcher_fill_getattr(struct fuse_io *fuse_io)
{
	struct spdk_fsdev_io *fsdev_io = fuse_to_fsdev_io(fuse_io);
	uint64_t fh = 0;

	if (fsdev_io_proto_minor(fuse_io) >= 9) {
		struct fuse_getattr_in *arg;

		arg = _fsdev_io_in_arg_get_buf(fuse_io, sizeof(*arg));
		if (!arg) {
			SPDK_ERRLOG("Cannot get fuse_getattr_in\n");
			return -EINVAL;
		}

		if (fsdev_io_d2h_u64(fuse_io->disp, arg->getattr_flags) & FUSE_GETATTR_FH) {
			fh = fsdev_io_d2h_u64(fuse_io->disp, arg->fh);
		}
	}

	fuse_init_fsdev_io(fuse_io, SPDK_FSDEV_IO_GETATTR);

	fsdev_io->u_in.getattr.fobject = file_object(fuse_io);
	fsdev_io->u_in.getattr.fhandle = file_handle(fh);

	return 0;
}

#define FATTR_FLAGS_MAP \
	FATTR_FLAG(ATTR_MODE)		\
	FATTR_FLAG(ATTR_UID)		\
	FATTR_FLAG(ATTR_GID)		\
	FATTR_FLAG(ATTR_SIZE)		\
	FATTR_FLAG(ATTR_ATIME)		\
	FATTR_FLAG(ATTR_MTIME)		\
	FATTR_FLAG(ATTR_ATIME_NOW)	\
	FATTR_FLAG(ATTR_MTIME_NOW)	\
	FATTR_FLAG(ATTR_CTIME)

static uint32_t
fuse_fattr_flags_to_fsdev(uint32_t flags)
{
	uint32_t result = 0;

#define FATTR_FLAG(name) \
	if (flags & F##name) {			\
		result |= SPDK_FSDEV_##name;	\
	}

	FATTR_FLAGS_MAP;

#undef FXATTR_FLAG

	return result;
}

static int
fuse_dispatcher_fill_setattr(struct fuse_io *fuse_io)
{
	struct spdk_fsdev_io *fsdev_io = fuse_to_fsdev_io(fuse_io);
	struct fuse_setattr_in *arg;
	uint64_t fh = 0;
	uint32_t valid;
	struct spdk_fsdev_file_attr *attr = &fsdev_io->u_in.setattr.attr;

	arg = _fsdev_io_in_arg_get_buf(fuse_io, sizeof(*arg));
	if (!arg) {
		SPDK_ERRLOG("Cannot get fuse_setattr_in\n");
		return -EINVAL;
	}

	fuse_init_fsdev_io(fuse_io, SPDK_FSDEV_IO_SETATTR);

	memset(attr, 0, sizeof(*attr));
	attr->mode      = fsdev_io_d2h_u32(fuse_io->disp, arg->mode);
	attr->uid       = fsdev_io_d2h_u32(fuse_io->disp, arg->uid);
	attr->gid       = fsdev_io_d2h_u32(fuse_io->disp, arg->gid);
	attr->size      = fsdev_io_d2h_u64(fuse_io->disp, arg->size);
	attr->atime     = fsdev_io_d2h_u64(fuse_io->disp, arg->atime);
	attr->mtime     = fsdev_io_d2h_u64(fuse_io->disp, arg->mtime);
	attr->ctime     = fsdev_io_d2h_u64(fuse_io->disp, arg->ctime);
	attr->atimensec = fsdev_io_d2h_u32(fuse_io->disp, arg->atimensec);
	attr->mtimensec = fsdev_io_d2h_u32(fuse_io->disp, arg->mtimensec);
	attr->ctimensec = fsdev_io_d2h_u32(fuse_io->disp, arg->ctimensec);

	valid = fsdev_io_d2h_u64(fuse_io->disp, arg->valid);
	if (valid & FATTR_FH) {
		valid &= ~FATTR_FH;
		fh = fsdev_io_d2h_u64(fuse_io->disp, arg->fh);
	}

	fsdev_io->u_in.setattr.fobject = file_object(fuse_io);
	fsdev_io->u_in.setattr.fhandle = file_handle(fh);
	fsdev_io->u_in.setattr.to_set = fuse_fattr_flags_to_fsdev(valid);
	fsdev_io->u_in.setattr.to_set &=
		SPDK_FSDEV_ATTR_MODE |
		SPDK_FSDEV_ATTR_UID |
		SPDK_FSDEV_ATTR_GID |
		SPDK_FSDEV_ATTR_SIZE |
		SPDK_FSDEV_ATTR_ATIME |
		SPDK_FSDEV_ATTR_MTIME |
		SPDK_FSDEV_ATTR_ATIME_NOW |
		SPDK_FSDEV_ATTR_MTIME_NOW |
		SPDK_FSDEV_ATTR_CTIME;

	return 0;
}

static int
fuse_dispatcher_fill_readlink(struct fuse_io *fuse_io)
{
	struct spdk_fsdev_io *fsdev_io = fuse_to_fsdev_io(fuse_io);

	fuse_init_fsdev_io(fuse_io, SPDK_FSDEV_IO_READLINK);

	fsdev_io->u_in.readlink.fobject = file_object(fuse_io);

	return 0;
}

static int
fuse_dispatcher_fill_symlink(struct fuse_io *fuse_io)
{
	struct spdk_fsdev_io *fsdev_io = fuse_to_fsdev_io(fuse_io);
	const char *name, *linkname;

	name = _fsdev_io_in_arg_get_str(fuse_io);
	if (!name) {
		SPDK_ERRLOG("Cannot get name\n");
		return -EINVAL;
	}

	linkname = _fsdev_io_in_arg_get_str(fuse_io);
	if (!linkname) {
		SPDK_ERRLOG("Cannot get linkname\n");
		return -EINVAL;
	}

	fuse_init_fsdev_io(fuse_io, SPDK_FSDEV_IO_SYMLINK);

	fsdev_io->u_in.symlink.parent_fobject = file_object(fuse_io);
	fsdev_io->u_in.symlink.target = name;
	fsdev_io->u_in.symlink.linkpath = linkname;
	fsdev_io->u_in.symlink.euid = fuse_io->hdr.uid;
	fsdev_io->u_in.symlink.egid = fuse_io->hdr.gid;

	return 0;
}

static int
fuse_dispatcher_fill_mknod(struct fuse_io *fuse_io)
{
	struct spdk_fsdev_io *fsdev_io = fuse_to_fsdev_io(fuse_io);
	bool compat = fsdev_io_proto_minor(fuse_io) < 12;
	struct fuse_mknod_in *arg;
	const char *name;

	arg = _fsdev_io_in_arg_get_buf(fuse_io, compat ? FUSE_COMPAT_MKNOD_IN_SIZE : sizeof(*arg));
	if (!arg) {
		SPDK_ERRLOG("Cannot get fuse_mknod_in (compat=%d)\n", compat);
		return EINVAL;
	}

	name = _fsdev_io_in_arg_get_str(fuse_io);
	if (!name) {
		SPDK_ERRLOG("Cannot get name (compat=%d)\n", compat);
		return -EINVAL;
	}

	fuse_init_fsdev_io(fuse_io, SPDK_FSDEV_IO_MKNOD);

	fsdev_io->u_in.mknod.parent_fobject = file_object(fuse_io);
	fsdev_io->u_in.mknod.name = name;
	fsdev_io->u_in.mknod.mode = fsdev_io_d2h_u32(fuse_io->disp, arg->mode);
	fsdev_io->u_in.mknod.umask = fsdev_io_d2h_u32(fuse_io->disp, arg->umask);
	fsdev_io->u_in.mknod.rdev = fsdev_io_d2h_u32(fuse_io->disp, arg->rdev);
	fsdev_io->u_in.mknod.euid = fuse_io->hdr.uid;
	fsdev_io->u_in.mknod.egid = fuse_io->hdr.gid;

	return 0;
}

static int
fuse_dispatcher_fill_mkdir(struct fuse_io *fuse_io)
{
	struct spdk_fsdev_io *fsdev_io = fuse_to_fsdev_io(fuse_io);
	bool compat = fsdev_io_proto_minor(fuse_io) < 12;
	struct fuse_mkdir_in *arg;
	const char *name;

	arg = _fsdev_io_in_arg_get_buf(fuse_io, compat ? sizeof(uint32_t) : sizeof(*arg));
	if (!arg) {
		SPDK_ERRLOG("Cannot get fuse_mkdir_in (compat=%d)\n", compat);
		return -EINVAL;
	}

	name = _fsdev_io_in_arg_get_str(fuse_io);
	if (!name) {
		SPDK_ERRLOG("Cannot get name (compat=%d)\n", compat);
		return -EINVAL;
	}

	fuse_init_fsdev_io(fuse_io, SPDK_FSDEV_IO_MKDIR);

	fsdev_io->u_in.mkdir.parent_fobject = file_object(fuse_io);
	fsdev_io->u_in.mkdir.name = name;
	fsdev_io->u_in.mkdir.mode = fsdev_io_d2h_u32(fuse_io->disp, arg->mode);
	fsdev_io->u_in.mkdir.umask = fsdev_io_d2h_u32(fuse_io->disp, arg->umask);
	fsdev_io->u_in.mkdir.euid = fuse_io->hdr.uid;
	fsdev_io->u_in.mkdir.egid = fuse_io->hdr.gid;

	return 0;
}

static int
fuse_dispatcher_fill_unlink(struct fuse_io *fuse_io)
{
	struct spdk_fsdev_io *fsdev_io = fuse_to_fsdev_io(fuse_io);
	const char *name;

	name = _fsdev_io_in_arg_get_str(fuse_io);
	if (!name) {
		SPDK_ERRLOG("Cannot get name\n");
		return -EINVAL;
	}

	fuse_init_fsdev_io(fuse_io, SPDK_FSDEV_IO_UNLINK);

	fsdev_io->u_in.unlink.parent_fobject = file_object(fuse_io);
	fsdev_io->u_in.unlink.name = name;

	return 0;
}

static int
fuse_dispatcher_fill_rmdir(struct fuse_io *fuse_io)
{
	struct spdk_fsdev_io *fsdev_io = fuse_to_fsdev_io(fuse_io);
	const char *name;

	name = _fsdev_io_in_arg_get_str(fuse_io);
	if (!name) {
		SPDK_ERRLOG("Cannot get name\n");
		return -EINVAL;
	}

	fuse_init_fsdev_io(fuse_io, SPDK_FSDEV_IO_RMDIR);

	fsdev_io->u_in.rmdir.parent_fobject = file_object(fuse_io);
	fsdev_io->u_in.rmdir.name = name;

	return 0;
}

#define RENAME2_FLAGS_MAP \
	RENAME2_FLAG(EXCHANGE)	\
	RENAME2_FLAG(NOREPLACE)	\
	RENAME2_FLAG(WHITEOUT)

static uint32_t
fuse_rename2_flags_to_fsdev(uint32_t flags)
{
	uint32_t result = 0;

#define RENAME2_FLAG(name) \
	if (flags & RENAME_##name) {			\
		result |= SPDK_FSDEV_RENAME_##name;	\
	}

	RENAME2_FLAGS_MAP;

#undef RENAME2_FLAG

	return result;
}

static int
fuse_dispatcher_fill_rename_common(struct fuse_io *fuse_io, bool version2)
{
	struct spdk_fsdev_io *fsdev_io = fuse_to_fsdev_io(fuse_io);
	uint64_t newdir;
	const char *oldname;
	const char *newname;
	uint32_t flags = 0;

	if (!version2) {
		struct fuse_rename_in *arg;
		arg = _fsdev_io_in_arg_get_buf(fuse_io, sizeof(*arg));
		if (!arg) {
			SPDK_ERRLOG("Cannot get fuse_rename_in\n");
			return -EINVAL;
		}
		newdir = fsdev_io_d2h_u64(fuse_io->disp, arg->newdir);
	} else {
		struct fuse_rename2_in *arg;
		arg = _fsdev_io_in_arg_get_buf(fuse_io, sizeof(*arg));
		if (!arg) {
			SPDK_ERRLOG("Cannot get fuse_rename2_in\n");
			return -EINVAL;
		}
		newdir = fsdev_io_d2h_u64(fuse_io->disp, arg->newdir);
		flags = fsdev_io_d2h_u64(fuse_io->disp, arg->flags);
		flags = fuse_rename2_flags_to_fsdev(flags);
	}

	oldname = _fsdev_io_in_arg_get_str(fuse_io);
	if (!oldname) {
		SPDK_ERRLOG("Cannot get oldname\n");
		return -EINVAL;
	}

	newname = _fsdev_io_in_arg_get_str(fuse_io);
	if (!newname) {
		SPDK_ERRLOG("Cannot get newname\n");
		return -EINVAL;
	}

	fuse_init_fsdev_io(fuse_io, SPDK_FSDEV_IO_RENAME);

	fsdev_io->u_in.rename.parent_fobject = file_object(fuse_io);
	fsdev_io->u_in.rename.name = oldname;
	fsdev_io->u_in.rename.new_parent_fobject = ino_to_object(fuse_io, newdir);
	fsdev_io->u_in.rename.new_name = newname;
	fsdev_io->u_in.rename.flags = flags;

	return 0;
}

static int
fuse_dispatcher_fill_rename(struct fuse_io *fuse_io)
{
	return fuse_dispatcher_fill_rename_common(fuse_io, false);
}

static int
fuse_dispatcher_fill_rename2(struct fuse_io *fuse_io)
{
	return fuse_dispatcher_fill_rename_common(fuse_io, true);
}

static int
fuse_dispatcher_fill_link(struct fuse_io *fuse_io)
{
	struct spdk_fsdev_io *fsdev_io = fuse_to_fsdev_io(fuse_io);
	struct fuse_link_in *arg;
	const char *name;
	uint64_t oldnodeid;

	arg = _fsdev_io_in_arg_get_buf(fuse_io, sizeof(*arg));
	if (!arg) {
		SPDK_ERRLOG("Cannot get fuse_link_in\n");
		return -EINVAL;
	}

	name = _fsdev_io_in_arg_get_str(fuse_io);
	if (!name) {
		SPDK_ERRLOG("Cannot get name\n");
		return -EINVAL;
	}

	fuse_init_fsdev_io(fuse_io, SPDK_FSDEV_IO_LINK);

	oldnodeid = fsdev_io_d2h_u64(fuse_io->disp, arg->oldnodeid);
	fsdev_io->u_in.link.fobject = ino_to_object(fuse_io, oldnodeid);
	fsdev_io->u_in.link.new_parent_fobject = file_object(fuse_io);
	fsdev_io->u_in.link.name = name;

	return 0;
}

static int
fuse_dispatcher_fill_open(struct fuse_io *fuse_io)
{
	struct spdk_fuse_dispatcher *disp = fuse_io->disp;
	struct spdk_fsdev_io *fsdev_io = fuse_to_fsdev_io(fuse_io);
	struct fuse_open_in *arg;
	uint32_t flags;

	arg = _fsdev_io_in_arg_get_buf(fuse_io, sizeof(*arg));
	if (!arg) {
		SPDK_ERRLOG("Cannot get fuse_forget_in\n");
		return -EINVAL;
	}

	if (!fsdev_d2h_open_flags(disp->fuse_arch, fsdev_io_d2h_u32(fuse_io->disp, arg->flags),
				  &flags)) {
		SPDK_ERRLOG("Cannot translate flags\n");
		return -EINVAL;
	}

	fuse_init_fsdev_io(fuse_io, SPDK_FSDEV_IO_OPEN);

	fsdev_io->u_in.open.fobject = file_object(fuse_io);
	fsdev_io->u_in.open.flags = flags;

	return 0;
}

static int
fuse_dispatcher_fill_read(struct fuse_io *fuse_io)
{
	struct spdk_fsdev_io *fsdev_io = fuse_to_fsdev_io(fuse_io);
	bool compat = fsdev_io_proto_minor(fuse_io) < 9;
	struct fuse_read_in *arg;
	uint64_t fh;
	uint32_t flags = 0;

	arg = _fsdev_io_in_arg_get_buf(fuse_io,
				       compat ? offsetof(struct fuse_read_in, lock_owner) : sizeof(*arg));
	if (!arg) {
		SPDK_ERRLOG("Cannot get fuse_read_in\n");
		return -EINVAL;
	}

	if (!compat) {
		flags = fsdev_io_d2h_u32(fuse_io->disp, arg->flags);
	}

	fh = fsdev_io_d2h_u64(fuse_io->disp, arg->fh);

	fuse_init_fsdev_io(fuse_io, SPDK_FSDEV_IO_READ);

	fsdev_io->u_in.read.fobject = file_object(fuse_io);
	fsdev_io->u_in.read.fhandle = file_handle(fh);
	fsdev_io->u_in.read.size = fsdev_io_d2h_u32(fuse_io->disp, arg->size);
	fsdev_io->u_in.read.offs = fsdev_io_d2h_u64(fuse_io->disp, arg->offset);
	fsdev_io->u_in.read.flags = flags;
	fsdev_io->u_in.read.iov = fuse_io->out_iov + 1;
	fsdev_io->u_in.read.iovcnt = fuse_io->out_iovcnt - 1;
	fsdev_io->u_in.read.opts = NULL;

	return 0;
}

static inline void
fuse_dispatcher_map_write_fuse_flags(struct spdk_fsdev_io *fsdev_io, uint32_t fuse_write_flags)
{
	assert(((fsdev_io->u_in.write.flags >> 32) & UINT32_MAX) == 0);

	if (fuse_write_flags & FUSE_WRITE_CACHE) {
		fsdev_io->u_in.write.flags |= SPDK_FSDEV_WRITE_CACHE;
	}
	if (fuse_write_flags & FUSE_WRITE_LOCKOWNER) {
		fsdev_io->u_in.write.flags |= SPDK_FSDEV_WRITE_LOCKOWNER;
	}
	if (fuse_write_flags & FUSE_WRITE_KILL_SUIDGID) {
		fsdev_io->u_in.write.flags |= SPDK_FSDEV_WRITE_KILL_SUIDGID;
	}
}

static int
fuse_dispatcher_fill_write(struct fuse_io *fuse_io)
{
	struct spdk_fsdev_io *fsdev_io = fuse_to_fsdev_io(fuse_io);
	bool compat = fsdev_io_proto_minor(fuse_io) < 9;
	struct fuse_write_in *arg;
	uint64_t fh;
	uint64_t flags = 0, write_flags = 0;

	arg = _fsdev_io_in_arg_get_buf(fuse_io,
				       compat ? FUSE_COMPAT_WRITE_IN_SIZE : sizeof(*arg));
	if (!arg) {
		SPDK_ERRLOG("Cannot get fuse_write_in\n");
		return -EINVAL;
	}

	if (fuse_io->in_offs.buf_offs) {
		SPDK_ERRLOG("Data IOVs should be separate from the header IOV\n");
		return -EINVAL;
	}

	if (!compat) {
		flags = fsdev_io_d2h_u32(fuse_io->disp, arg->flags);
	}
	write_flags = fsdev_io_d2h_u32(fuse_io->disp, arg->write_flags);

	fh = fsdev_io_d2h_u64(fuse_io->disp, arg->fh);

	fuse_init_fsdev_io(fuse_io, SPDK_FSDEV_IO_WRITE);

	fsdev_io->u_in.write.fobject = file_object(fuse_io);
	fsdev_io->u_in.write.fhandle = file_handle(fh);
	fsdev_io->u_in.write.size = fsdev_io_d2h_u32(fuse_io->disp, arg->size);
	fsdev_io->u_in.write.offs = fsdev_io_d2h_u64(fuse_io->disp, arg->offset);
	fsdev_io->u_in.write.flags = flags;
	fuse_dispatcher_map_write_fuse_flags(fsdev_io, write_flags);
	fsdev_io->u_in.write.iov = fuse_io->in_iov + fuse_io->in_offs.iov_offs;
	fsdev_io->u_in.write.iovcnt = fuse_io->in_iovcnt - fuse_io->in_offs.iov_offs;
	fsdev_io->u_in.write.opts = NULL;

	return 0;
}

static int
fuse_dispatcher_fill_statfs(struct fuse_io *fuse_io)
{
	struct spdk_fsdev_io *fsdev_io = fuse_to_fsdev_io(fuse_io);

	fuse_init_fsdev_io(fuse_io, SPDK_FSDEV_IO_STATFS);

	fsdev_io->u_in.statfs.fobject = file_object(fuse_io);

	return 0;
}

static int
fuse_dispatcher_fill_release(struct fuse_io *fuse_io)
{
	struct spdk_fsdev_io *fsdev_io = fuse_to_fsdev_io(fuse_io);
	bool compat = fsdev_io_proto_minor(fuse_io) < 8;
	struct fuse_release_in *arg;
	uint64_t fh;

	arg = _fsdev_io_in_arg_get_buf(fuse_io,
				       compat ? offsetof(struct fuse_release_in, lock_owner) : sizeof(*arg));
	if (!arg) {
		SPDK_ERRLOG("Cannot get fuse_release_in\n");
		return -EINVAL;
	}

	fh = fsdev_io_d2h_u64(fuse_io->disp, arg->fh);

	fuse_init_fsdev_io(fuse_io, SPDK_FSDEV_IO_RELEASE);

	fsdev_io->u_in.release.fobject = file_object(fuse_io);
	fsdev_io->u_in.release.fhandle = file_handle(fh);

	return 0;
}

static int
fuse_dispatcher_fill_fsync(struct fuse_io *fuse_io)
{
	struct spdk_fsdev_io *fsdev_io = fuse_to_fsdev_io(fuse_io);
	struct fuse_fsync_in *arg;
	uint64_t fh;
	bool datasync;

	arg = _fsdev_io_in_arg_get_buf(fuse_io, sizeof(*arg));
	if (!arg) {
		SPDK_ERRLOG("Cannot get fuse_fsync_in\n");
		fuse_dispatcher_io_complete_err(fuse_io, -EINVAL);
		return -EINVAL;
	}

	fh = fsdev_io_d2h_u64(fuse_io->disp, arg->fh);
	datasync = (fsdev_io_d2h_u32(fuse_io->disp, arg->fsync_flags) & 1) ? true : false;

	fuse_init_fsdev_io(fuse_io, SPDK_FSDEV_IO_FSYNC);

	fsdev_io->u_in.fsync.fobject = file_object(fuse_io);
	fsdev_io->u_in.fsync.fhandle = file_handle(fh);
	fsdev_io->u_in.fsync.datasync = datasync;

	return 0;
}

#define XATTR_FLAGS_MAP \
	XATTR_FLAG(XATTR_CREATE) \
	XATTR_FLAG(XATTR_REPLACE)

static uint64_t
fuse_xattr_flags_to_fsdev(uint32_t flags)
{
	uint64_t result = 0;

#define XATTR_FLAG(name) \
	if (flags & name) {			\
		result |= SPDK_FSDEV_##name;	\
	}

	XATTR_FLAGS_MAP;

#undef XATTR_FLAG

	return result;
}

#define XATTR_EXT_FLAGS_MAP \
	XATTR_EXT_FLAG(SETXATTR_ACL_KILL_SGID)

static uint64_t
fuse_xattr_ext_flags_to_fsdev(uint32_t flags)
{
	uint64_t result = 0;

#define XATTR_EXT_FLAG(name) \
	if (flags & FUSE_##name) {		\
		result |= SPDK_FSDEV_##name;	\
	}

	XATTR_EXT_FLAGS_MAP;

#undef XATTR_EXT_FLAG

	return result;
}

static int
fuse_dispatcher_fill_setxattr(struct fuse_io *fuse_io)
{
	struct spdk_fuse_dispatcher *disp = fuse_io->disp;
	struct spdk_fsdev_io *fsdev_io = fuse_to_fsdev_io(fuse_io);
	bool xattr_ext = !!(disp->mount_flags & FUSE_SETXATTR_EXT);
	struct fuse_setxattr_in *arg;
	const char *name;
	const char *value;
	uint32_t size;
	uint64_t flags;

	size = xattr_ext ? sizeof(*arg) : FUSE_COMPAT_SETXATTR_IN_SIZE;
	arg = _fsdev_io_in_arg_get_buf(fuse_io, size);
	if (!arg) {
		SPDK_ERRLOG("Cannot get fuse_setxattr_in\n");
		return -EINVAL;
	}

	name = _fsdev_io_in_arg_get_str(fuse_io);
	if (!name) {
		SPDK_ERRLOG("Cannot get name\n");
		return -EINVAL;
	}

	size = fsdev_io_d2h_u32(fuse_io->disp, arg->size);
	value = _fsdev_io_in_arg_get_buf(fuse_io, size);
	if (!value) {
		SPDK_ERRLOG("Cannot get value of %" PRIu32 " bytes\n", size);
		return -EINVAL;
	}

	flags = fuse_xattr_flags_to_fsdev(fsdev_io_d2h_u32(fuse_io->disp, arg->flags));
	if (xattr_ext) {
		flags |= fuse_xattr_ext_flags_to_fsdev(fsdev_io_d2h_u32(fuse_io->disp, arg->setxattr_flags));
	}

	fuse_init_fsdev_io(fuse_io, SPDK_FSDEV_IO_SETXATTR);

	fsdev_io->u_in.setxattr.fobject = file_object(fuse_io);
	fsdev_io->u_in.setxattr.name = name;
	fsdev_io->u_in.setxattr.value = value;
	fsdev_io->u_in.setxattr.size = size;
	fsdev_io->u_in.setxattr.flags = flags;

	return 0;
}

static int
fuse_dispatcher_fill_getxattr(struct fuse_io *fuse_io)
{
	struct spdk_fsdev_io *fsdev_io = fuse_to_fsdev_io(fuse_io);
	struct fuse_getxattr_in *arg;
	const char *name;
	char *buff = NULL;
	uint32_t size;
	struct iov_offs out_offs_bu;

	arg = _fsdev_io_in_arg_get_buf(fuse_io, sizeof(*arg));
	if (!arg) {
		SPDK_ERRLOG("Cannot get fuse_getxattr_in\n");
		return -EINVAL;
	}

	name = _fsdev_io_in_arg_get_str(fuse_io);
	if (!name) {
		SPDK_ERRLOG("Cannot get name\n");
		return -EINVAL;
	}

	if (fuse_io->out_iovcnt < 2) {
		SPDK_ERRLOG("No buffer to getxattr\n");
		return -EINVAL;
	}

	size = fsdev_io_d2h_u32(fuse_io->disp, arg->size);

	/* Zero size means requesting size of the xattr value. No need to go further. */
	if (size > 0) {
		/*
		 * NOTE: we want to avoid an additional allocation and copy and put the xattr
		 * directly to the buffer provided in out_iov. In order to do so we have to
		 * preserve the out_offs, advance it to get the buffer pointer and then restore
		 * to allow the fuse_dispatcher_io_complete_xattr() to fill the fuse_getxattr_out
		 * which precedes this buffer.
		 */
		out_offs_bu = fuse_io->out_offs; /* Preserve the out offset */

		/* Skip the fuse_getxattr_out */
		_fsdev_io_out_arg_get_buf(fuse_io, sizeof(struct fuse_getxattr_out));
		if (size < sizeof(struct fuse_getxattr_out)) {
			SPDK_ERRLOG("Invalid size=%u smaller than the size of fuse_getxattr_out=%lu "
				    "in getxattr request.\n", size, sizeof(struct fuse_getxattr_out));
			return -EINVAL;
		}
		size -= sizeof(struct fuse_getxattr_out);

		buff = _fsdev_io_out_arg_get_buf(fuse_io, size); /* Get the buffer for the xattr */
		if (!buff) {
			/*
			 * Should not happen at this point but let's ignore it. Null buff and zero
			 * size are valid inputs for spdk_fsdev_getxattr().
			 */
			size = 0;
		}
		fuse_io->out_offs = out_offs_bu; /* Restore the out offset */
	}

	fuse_init_fsdev_io(fuse_io, SPDK_FSDEV_IO_GETXATTR);

	fsdev_io->u_in.getxattr.fobject = file_object(fuse_io);
	fsdev_io->u_in.getxattr.name = name;
	fsdev_io->u_in.getxattr.buffer = buff;
	fsdev_io->u_in.getxattr.size = size;

	return 0;
}

static int
fuse_dispatcher_fill_listxattr(struct fuse_io *fuse_io)
{
	struct spdk_fsdev_io *fsdev_io = fuse_to_fsdev_io(fuse_io);
	struct fuse_getxattr_in *arg;
	struct iovec *iov;
	uint32_t size;

	arg = _fsdev_io_in_arg_get_buf(fuse_io, sizeof(*arg));
	if (!arg) {
		SPDK_ERRLOG("Cannot get fuse_getxattr_in\n");
		return -EINVAL;
	}

	size = fsdev_io_d2h_u32(fuse_io->disp, arg->size);
	iov = fuse_io->out_iov + 1;
	if (iov->iov_len < size) {
		SPDK_ERRLOG("Wrong iov len (%zu < %" PRIu32")\n", iov->iov_len, size);
		return -EINVAL;
	}

	fuse_init_fsdev_io(fuse_io, SPDK_FSDEV_IO_LISTXATTR);

	fsdev_io->u_in.listxattr.fobject = file_object(fuse_io);
	fsdev_io->u_in.listxattr.buffer = iov->iov_base;
	fsdev_io->u_in.listxattr.size = size;

	return 0;
}

static int
fuse_dispatcher_fill_removexattr(struct fuse_io *fuse_io)
{
	struct spdk_fsdev_io *fsdev_io = fuse_to_fsdev_io(fuse_io);
	const char *name = _fsdev_io_in_arg_get_str(fuse_io);

	if (!name) {
		SPDK_ERRLOG("Cannot get name\n");
		return -EINVAL;
	}

	fuse_init_fsdev_io(fuse_io, SPDK_FSDEV_IO_REMOVEXATTR);

	fsdev_io->u_in.removexattr.fobject = file_object(fuse_io);
	fsdev_io->u_in.removexattr.name = name;

	return 0;
}

static int
fuse_dispatcher_fill_flush(struct fuse_io *fuse_io)
{
	struct spdk_fsdev_io *fsdev_io = fuse_to_fsdev_io(fuse_io);
	bool compat = fsdev_io_proto_minor(fuse_io) < 7;
	struct fuse_flush_in *arg;
	uint64_t fh;

	arg = _fsdev_io_in_arg_get_buf(fuse_io,
				       compat ? offsetof(struct fuse_flush_in, lock_owner) : sizeof(*arg));
	if (!arg) {
		SPDK_ERRLOG("Cannot get fuse_flush_in\n");
		return -EINVAL;
	}

	fh = fsdev_io_d2h_u64(fuse_io->disp, arg->fh);

	fuse_init_fsdev_io(fuse_io, SPDK_FSDEV_IO_FLUSH);

	fsdev_io->u_in.flush.fobject = file_object(fuse_io);
	fsdev_io->u_in.flush.fhandle = file_handle(fh);

	return 0;
}

static void
do_mount_rollback_cpl_clb(void *cb_arg, int status, struct spdk_fsdev_io *fsdev_io)
{
	struct fuse_io *fuse_io = cb_arg;
	struct spdk_fuse_dispatcher *disp = fuse_io->disp;

	UNUSED(disp);

	SPDK_DEBUGLOG(fuse_dispatcher, "%s unmounted\n", fuse_dispatcher_name(disp));

	/* The IO is FUSE_INIT, so we complete it with the appropriate error */
	fuse_dispatcher_io_complete_err(fuse_io, fuse_io->u.init.error);
}

static void
fuse_dispatcher_mount_rollback(struct fuse_io *fuse_io)
{
	fuse_init_fsdev_io_ex(fuse_io, SPDK_FSDEV_IO_UMOUNT, do_mount_rollback_cpl_clb);

	spdk_fsdev_io_submit(fuse_to_fsdev_io(fuse_io));
}

#define FUSE_DOT_PATH_LOOKUP FUSE_EXPORT_SUPPORT
#define FUSE_O_TRUNC FUSE_ATOMIC_O_TRUNC

#define MNT_FLAGS_MAP \
	MNT_FLAG(DOT_PATH_LOOKUP)	\
	MNT_FLAG(AUTO_INVAL_DATA)	\
	MNT_FLAG(EXPLICIT_INVAL_DATA)	\
	MNT_FLAG(WRITEBACK_CACHE)	\
	MNT_FLAG(POSIX_ACL)		\
	MNT_FLAG(POSIX_LOCKS)		\
	MNT_FLAG(FLOCK_LOCKS)		\
	MNT_FLAG(O_TRUNC)		\
	MNT_FLAG(NO_EXPORT_SUPPORT)	\
	MNT_FLAG(HANDLE_KILLPRIV)	\
	MNT_FLAG(HANDLE_KILLPRIV_V2)	\
	MNT_FLAG(DIRECT_IO_ALLOW_MMAP)

static uint64_t
fuse_mount_flags_to_fsdev(uint64_t flags)
{
	uint64_t result = 0;

#define MNT_FLAG(name) \
	if (flags & FUSE_##name) {			\
		result |= SPDK_FSDEV_MOUNT_##name;	\
	}

	MNT_FLAGS_MAP;

#undef MNT_FLAG

	return result;
}

static uint64_t
fsdev_mount_flags_to_fuse(uint64_t flags)
{
	uint64_t result = 0;

#define MNT_FLAG(name) \
	if (flags & SPDK_FSDEV_MOUNT_##name) {	\
		result |= FUSE_##name;		\
	}

	MNT_FLAGS_MAP;

#undef MNT_FLAG

	return result;
}

/* Maximal number of pages for unlimited max_xfer_size. Using FUSE page limit value. */
#define SPDK_FSDEV_PAGE_LIMIT 256

static int
do_mount_prepare_completion(struct fuse_io *fuse_io,
			    const struct spdk_fsdev_mount_opts *negotiated_opts)
{
	uint64_t requested_flags;
	struct spdk_fuse_dispatcher *disp = fuse_io->disp;
	struct fuse_init_out outarg;
	size_t outargsize = sizeof(outarg);
	uint64_t supported = 0;
	uint32_t max_xfer_size;
	void *out_buf;

	assert(disp->desc);

	memset(&outarg, 0, sizeof(outarg));
	outarg.major = fsdev_io_h2d_u32(fuse_io->disp, disp->proto_major);
	outarg.minor = fsdev_io_h2d_u32(fuse_io->disp, disp->proto_minor);

	if (disp->proto_minor < 5) {
		outargsize = FUSE_COMPAT_INIT_OUT_SIZE;
	} else if (disp->proto_minor < 23) {
		outargsize = FUSE_COMPAT_22_INIT_OUT_SIZE;
	}

	requested_flags = fsdev_io_d2h_u32(fuse_io->disp, fuse_io->u.init.in->flags);
	if (requested_flags & FUSE_INIT_EXT) {
		requested_flags |= (uint64_t)fsdev_io_d2h_u32(fuse_io->disp,
				   fuse_io->u.init.in->flags2) << 32;
	}

#define SET_MOUNT_FLAG(requested_flags, stage, flag)	\
	if (requested_flags & (FUSE_##flag)) {		\
		stage |= (FUSE_##flag);			\
	}
	/* Always supported if requested by the FUSE. */
	SET_MOUNT_FLAG(requested_flags, supported, ASYNC_READ);
	SET_MOUNT_FLAG(requested_flags, supported, BIG_WRITES);
	SET_MOUNT_FLAG(requested_flags, supported, DONT_MASK);
	SET_MOUNT_FLAG(requested_flags, supported, DO_READDIRPLUS);
	SET_MOUNT_FLAG(requested_flags, supported, READDIRPLUS_AUTO);
	SET_MOUNT_FLAG(requested_flags, supported, ASYNC_DIO);
	SET_MOUNT_FLAG(requested_flags, supported, NO_OPEN_SUPPORT);
	SET_MOUNT_FLAG(requested_flags, supported, PARALLEL_DIROPS);
	SET_MOUNT_FLAG(requested_flags, supported, CACHE_SYMLINKS);
	SET_MOUNT_FLAG(requested_flags, supported, NO_OPENDIR_SUPPORT);
	SET_MOUNT_FLAG(requested_flags, supported, SUBMOUNTS);
	SET_MOUNT_FLAG(requested_flags, supported, MAX_PAGES);
	SET_MOUNT_FLAG(requested_flags, supported, SETXATTR_EXT);
	SET_MOUNT_FLAG(requested_flags, supported, HAS_IOCTL_DIR);
	SET_MOUNT_FLAG(requested_flags, supported, INIT_EXT);

#undef SET_MOUNT_FLAG

	/* Sending back the fsdev negotiated mount opts. */
	supported |= fsdev_mount_flags_to_fuse(negotiated_opts->flags);
	outarg.flags = fsdev_io_h2d_u32(fuse_io->disp, (uint32_t)supported);
	if (requested_flags & FUSE_INIT_EXT) {
		outarg.flags2 = fsdev_io_h2d_u32(fuse_io->disp, (uint32_t)(supported >> 32));
	}
	disp->mount_flags = supported;

	outarg.max_readahead = fsdev_io_h2d_u32(fuse_io->disp, negotiated_opts->max_readahead);

	max_xfer_size = negotiated_opts->max_xfer_size;

	if (max_xfer_size == 0) {
		/*
		 * The number of pages used by FUSE (and controlled when parsing max_pages) is
		 * limited to 256 pags. Let's use this value for unlimited case.
		 */
		max_xfer_size = SPDK_FSDEV_PAGE_LIMIT * PAGE_SIZE;
		SPDK_WARNLOG("FSDEV reported max_xfer_size = 0 (unlimited). Setting max_xfer_size = %u.\n",
			     max_xfer_size);
	}

	/*
	 * If max_xfer_size returned from the fsdev is <= 4k and we send max_write of
	 * this value to the FUSE it will set its own default = 4k as for today.
	 */
	outarg.max_write = fsdev_io_h2d_u32(fuse_io->disp, max_xfer_size);

	/*
	 * Sending max_pages == 0 to the FUSE will result into setting it to default
	 * value == 1.
	 */
	outarg.max_pages = max_xfer_size / PAGE_SIZE;
	outarg.max_pages = fsdev_io_h2d_u32(fuse_io->disp, outarg.max_pages);

	if (fsdev_io_proto_minor(fuse_io) >= 13) {
		outarg.max_background = fsdev_io_h2d_u16(fuse_io->disp, DEFAULT_MAX_BACKGROUND);
		outarg.congestion_threshold = fsdev_io_h2d_u16(fuse_io->disp, DEFAULT_CONGESTION_THRESHOLD);
	}

	if (fsdev_io_proto_minor(fuse_io) >= 23) {
		outarg.time_gran = fsdev_io_h2d_u32(fuse_io->disp, DEFAULT_TIME_GRAN);
	}

	SPDK_INFOLOG(fuse_dispatcher, "INIT: %" PRIu32 ".%" PRIu32 "\n",
		     fsdev_io_d2h_u32(fuse_io->disp, outarg.major), fsdev_io_d2h_u32(fuse_io->disp, outarg.minor));
	SPDK_INFOLOG(fuse_dispatcher, "flags: 0x%08" PRIx64 "\n",
		     ((uint64_t)fsdev_io_d2h_u32(fuse_io->disp, outarg.flags2) << 32) |
		     fsdev_io_d2h_u32(fuse_io->disp, outarg.flags));
	SPDK_INFOLOG(fuse_dispatcher, "max_readahead: %" PRIu32 "\n",
		     fsdev_io_d2h_u32(fuse_io->disp, outarg.max_readahead));
	SPDK_INFOLOG(fuse_dispatcher, "max_write: %" PRIu32 "\n",
		     fsdev_io_d2h_u32(fuse_io->disp, outarg.max_write));
	SPDK_INFOLOG(fuse_dispatcher, "max_pages: %" PRIu32 "\n",
		     fsdev_io_d2h_u32(fuse_io->disp, outarg.max_pages));
	SPDK_INFOLOG(fuse_dispatcher, "max_background: %" PRIu16 "\n",
		     fsdev_io_d2h_u16(fuse_io->disp, outarg.max_background));
	SPDK_INFOLOG(fuse_dispatcher, "congestion_threshold: %" PRIu16 "\n",
		     fsdev_io_d2h_u16(fuse_io->disp, outarg.congestion_threshold));
	SPDK_INFOLOG(fuse_dispatcher, "time_gran: %" PRIu32 "\n",
		     fsdev_io_d2h_u32(fuse_io->disp, outarg.time_gran));

	out_buf = _fsdev_io_out_arg_get_buf(fuse_io, outargsize);
	if (!out_buf) {
		SPDK_ERRLOG("Cannot get buf to copy fuse_init_out of %zu bytes\n", outargsize);
		return -EINVAL;
	}

	memcpy(out_buf, &outarg, outargsize);

	fuse_io->u.init.out_len = outargsize;
	return 0;
}

static void
fuse_dispatcher_mount_cpl_clb(void *cb_arg, int status, struct spdk_fsdev_io *fsdev_io)
{
	struct fuse_io *fuse_io = cb_arg;
	struct spdk_fuse_dispatcher *disp = fuse_io->disp;
	int rc;

	if (status) {
		SPDK_ERRLOG("%s: spdk_fsdev_mount failed (err=%d)\n", fuse_dispatcher_name(disp), status);
		fuse_dispatcher_io_complete_err(fuse_io, status);
		return;
	}

	SPDK_DEBUGLOG(fuse_dispatcher, "%s: spdk_fsdev_mount succeeded\n", fuse_dispatcher_name(disp));
	disp->root_fobject = fsdev_io->u_out.mount.root_fobject;
	rc = do_mount_prepare_completion(fuse_io, &fsdev_io->u_out.mount.opts);
	if (rc) {
		SPDK_ERRLOG("%s: mount completion preparation failed with %d\n", fuse_dispatcher_name(disp), rc);
		fuse_io->u.init.error = rc;
		disp->root_fobject = NULL;
		fuse_dispatcher_mount_rollback(fuse_io);
		return;
	}

	/* Save the negotiated state */
	fuse_dispatcher_update_rmem(disp);

	fuse_dispatcher_io_complete_ok(fuse_io, fuse_io->u.init.out_len);
}

static void
fuse_dispatcher_init_complete_again(struct fuse_io *fuse_io)
{
	struct fuse_init_out outarg;
	size_t outargsize = sizeof(outarg);

	memset(&outarg, 0, sizeof(outarg));
	outarg.major = fsdev_io_h2d_u32(fuse_io->disp, FUSE_KERNEL_VERSION);
	outarg.minor = fsdev_io_h2d_u32(fuse_io->disp, SPDK_FUSE_KERNEL_MINOR_VERSION);

	fuse_dispatcher_io_copy_and_complete(fuse_io, &outarg, outargsize, 0);
}

static int
fuse_dispatcher_fill_init(struct fuse_io *fuse_io)
{
	struct spdk_fsdev_io *fsdev_io = fuse_to_fsdev_io(fuse_io);
	size_t compat_size = offsetof(struct fuse_init_in, max_readahead);
	struct spdk_fuse_dispatcher *disp = fuse_io->disp;
	uint32_t max_readahead = DEFAULT_MAX_READAHEAD;
	uint64_t requested_flags = 0;

	/* First try to read the legacy header */
	fuse_io->u.init.in = _fsdev_io_in_arg_get_buf(fuse_io, compat_size);
	if (!fuse_io->u.init.in) {
		SPDK_ERRLOG("Cannot get fuse_init_in\n");
		return -EBADR;
	}

	disp->proto_major = fsdev_io_d2h_u32(fuse_io->disp, fuse_io->u.init.in->major);
	disp->proto_minor = fsdev_io_d2h_u32(fuse_io->disp, fuse_io->u.init.in->minor);

	SPDK_DEBUGLOG(fuse_dispatcher, "Proto version: %" PRIu32 ".%" PRIu32 "\n",
		      disp->proto_major,
		      disp->proto_minor);

	if (disp->proto_major < FUSE_KERNEL_VERSION) {
		SPDK_ERRLOG("INIT: unsupported major protocol version: %" PRIu32 "\n",
			    disp->proto_major);
		return -EPROTO;
	}

	if (disp->supported_fuse_opcodes != 0 && disp->proto_minor < 31) {
		SPDK_ERRLOG("INIT: unsupported minor protocol version: %" PRIu32 " for using fuse\n",
			    disp->proto_minor);
		return -EPROTO;
	}

	/* NOTE: it seems that libfuse has made assumption that FUSE_KERNEL_VERSION will never be changed again,
	 * and proto_minor will determine all versioning forever (see do_init() in libfuse/lib/fuse_lowlevel.c).
	 * As libfuse is considered the de facto standard, we just follow the same logic here.
	 */
	if (disp->proto_major > FUSE_KERNEL_VERSION) {
		return -EAGAIN;
	}

	if (disp->proto_minor > SPDK_FUSE_KERNEL_MINOR_VERSION) {
		SPDK_DEBUGLOG(fuse_dispatcher, "INIT: proto_minor adjusted: %" PRIu32 " -> %" PRIu32 "\n",
			      disp->proto_minor, SPDK_FUSE_KERNEL_MINOR_VERSION);
		disp->proto_minor = SPDK_FUSE_KERNEL_MINOR_VERSION;
	}

	if (disp->proto_minor >= 6) {
		/* Read the rest of struct fuse_init_in (w/o INIT_EXT) */
		void *arg_extra = _fsdev_io_in_arg_get_buf(fuse_io, COMPAT_FUSE_INIT_IN_6_SIZE -
				  compat_size);
		if (!arg_extra) {
			SPDK_ERRLOG("INIT: protocol version: %" PRIu32 ".%" PRIu32 " but legacy data found\n",
				    disp->proto_major, disp->proto_minor);
			return -EINVAL;
		}

		compat_size += COMPAT_FUSE_INIT_IN_6_SIZE;
		requested_flags = fsdev_io_d2h_u32(fuse_io->disp, fuse_io->u.init.in->flags);
		max_readahead = fsdev_io_d2h_u32(fuse_io->disp, fuse_io->u.init.in->max_readahead);
		/* Make sure we can safely read the extended portion */
		if (requested_flags & FUSE_INIT_EXT) {
			arg_extra = _fsdev_io_in_arg_get_buf(fuse_io, COMPAT_FUSE_INIT_IN_EXT_SIZE -
							     compat_size);
			if (!arg_extra) {
				SPDK_ERRLOG("INIT: FUSE_INIT_EXT flag set but no INIT_EXT data found\n");
				return -EINVAL;
			}
			requested_flags |= (uint64_t)fsdev_io_d2h_u32(fuse_io->disp,
					   fuse_io->u.init.in->flags2) << 32;
		}
		SPDK_INFOLOG(fuse_dispatcher, "requested: flags=0x%" PRIx64 " max_readahead=%" PRIu32 "\n",
			     requested_flags, max_readahead);
	}

	fuse_io->u.init.thread = spdk_get_thread();

	fuse_init_fsdev_io_ex(fuse_io, SPDK_FSDEV_IO_MOUNT, fuse_dispatcher_mount_cpl_clb);

	memset(&fsdev_io->u_in.mount.opts, 0, sizeof(fsdev_io->u_in.mount.opts));
	fsdev_io->u_in.mount.opts.opts_size = sizeof(fsdev_io->u_in.mount.opts);

	/* Passing for negotiation only few flags. The rest are always supported. */
	fsdev_io->u_in.mount.opts.flags = fuse_mount_flags_to_fsdev(requested_flags);
	fsdev_io->u_in.mount.opts.max_readahead = max_readahead;

	return 0;
}

static int
fuse_dispatcher_fill_opendir(struct fuse_io *fuse_io)
{
	struct spdk_fsdev_io *fsdev_io = fuse_to_fsdev_io(fuse_io);
	struct fuse_open_in *arg;

	arg = _fsdev_io_in_arg_get_buf(fuse_io, sizeof(*arg));
	if (!arg) {
		SPDK_ERRLOG("Cannot get fuse_open_in\n");
		return -EINVAL;
	}

	fuse_init_fsdev_io(fuse_io, SPDK_FSDEV_IO_OPENDIR);

	fsdev_io->u_in.opendir.fobject = file_object(fuse_io);
	fsdev_io->u_in.opendir.flags = fsdev_io_d2h_u32(fuse_io->disp, arg->flags);

	return 0;
}

static int
fuse_dispatcher_readdir_usr_entry_clb(void *cb_arg, struct spdk_fsdev_io *fsdev_io,
				      const char *name,
				      struct spdk_fsdev_file_object *fobject, const struct spdk_fsdev_file_attr *attr,
				      off_t offset, bool *forget)
{
	struct fuse_io *fuse_io = cb_arg;
	size_t bytes_remained = fuse_io->u.readdir.size - fuse_io->u.readdir.bytes_written;
	size_t direntry_bytes;

	direntry_bytes = fuse_io->u.readdir.plus ?
			 fuse_dispatcher_add_direntry_plus(fuse_io, fuse_io->u.readdir.writep, bytes_remained,
					 name, fobject, attr, offset) :
			 fuse_dispatcher_add_direntry(fuse_io, fuse_io->u.readdir.writep, bytes_remained,
					 name, attr->ino, (attr->mode & 0170000) >> 12, offset);

	if (direntry_bytes > bytes_remained) {
		return -EAGAIN;
	}

	fuse_io->u.readdir.writep += direntry_bytes;
	fuse_io->u.readdir.bytes_written += direntry_bytes;

	*forget = fuse_io->u.readdir.plus ? false : true;

	return 0;
}

static int
fuse_dispatcher_readdir_simple_usr_entry_clb(void *cb_arg, struct spdk_fsdev_io *fsdev_io,
		const char *name, uint64_t ino, uint32_t type, off_t offset)
{
	struct fuse_io *fuse_io = cb_arg;
	struct spdk_fuse_dispatcher *disp = fuse_io->disp;
	size_t bytes_remained = fuse_io->u.readdir.size - fuse_io->u.readdir.bytes_written;
	size_t direntry_bytes;

	UNUSED(disp);
	assert(disp->use_readdir_simple);
	assert(!fuse_io->u.readdir.plus);

	direntry_bytes = fuse_dispatcher_add_direntry(fuse_io, fuse_io->u.readdir.writep, bytes_remained,
			 name, ino, type, offset);

	if (direntry_bytes > bytes_remained) {
		return -EAGAIN;
	}

	fuse_io->u.readdir.writep += direntry_bytes;
	fuse_io->u.readdir.bytes_written += direntry_bytes;

	return 0;
}

static void
fuse_dispatcher_readdir_fallback_and_retry(struct spdk_fsdev_io *fsdev_io)
{
	struct fuse_io *fuse_io = fsdev_to_fuse_io(fsdev_io);
	struct spdk_fuse_dispatcher *disp = fuse_io->disp;
	/* We copy the data fields aside first as fsdev_io->u_in is a union. */
	struct spdk_fsdev_file_object *fobject = fsdev_io->u_in.readdir_simple.fobject;
	struct spdk_fsdev_file_handle *fhandle = fsdev_io->u_in.readdir_simple.fhandle;
	uint64_t offset = fsdev_io->u_in.readdir_simple.offset;

	/* The underlying fsdev does not support SPDK_FSDEV_IO_READDIR_SIMPLE simple, so we fallback to SPDK_FSDEV_IO_READDIR. */
	disp->use_readdir_simple = 0;

	SPDK_NOTICELOG("%s: READDIR_SIMPLE not supported, falling back to READDIR\n",
		       fuse_dispatcher_name(disp));

	/* Re-init the fsdev_io with SPDK_FSDEV_IO_READDIR. */
	fuse_init_fsdev_io_ex(fuse_io, SPDK_FSDEV_IO_READDIR, fuse_dispatcher_readdir_cpl_cb);

	/* All the relevant data fields are already set to readdir_simple, so all we need is to copy the them to readdir. */
	fsdev_io->u_in.readdir.fobject = fobject;
	fsdev_io->u_in.readdir.fhandle = fhandle;
	fsdev_io->u_in.readdir.offset = offset;
	fsdev_io->u_in.readdir.entry_cb_fn = fuse_dispatcher_readdir_usr_entry_clb;

	/* Submit the fsdev_io again. */
	spdk_fsdev_io_submit(fsdev_io);
}

static int
fuse_dispatcher_fill_readdir_common(struct fuse_io *fuse_io, bool plus)
{
	struct spdk_fsdev_io *fsdev_io = fuse_to_fsdev_io(fuse_io);
	struct fuse_read_in *arg;
	uint64_t fh;
	uint32_t size;

	arg = _fsdev_io_in_arg_get_buf(fuse_io, sizeof(*arg));
	if (!arg) {
		SPDK_ERRLOG("Cannot get fuse_read_in\n");
		return -EINVAL;
	}

	size = fsdev_io_d2h_u32(fuse_io->disp, arg->size);

	fuse_io->u.readdir.writep = _fsdev_io_out_arg_get_buf(fuse_io, size);
	if (!fuse_io->u.readdir.writep) {
		SPDK_ERRLOG("Cannot get buffer of %" PRIu32 " bytes\n", size);
		return -EINVAL;
	}

	fuse_io->u.readdir.plus = plus;
	fuse_io->u.readdir.size = size;
	fuse_io->u.readdir.bytes_written = 0;

	fh = fsdev_io_d2h_u64(fuse_io->disp, arg->fh);

	if (plus || !fuse_io->disp->use_readdir_simple) {
		/* For READDIRPLUS or if SPDK_FSDEV_IO_READDIR_SIMPLE is not supported by the underlying fsdev, we use SPDK_FSDEV_IO_READDIR. */
		fuse_init_fsdev_io_ex(fuse_io, SPDK_FSDEV_IO_READDIR, fuse_dispatcher_readdir_cpl_cb);

		fsdev_io->u_in.readdir.fobject = file_object(fuse_io);
		fsdev_io->u_in.readdir.fhandle = file_handle(fh);
		fsdev_io->u_in.readdir.offset = fsdev_io_d2h_u64(fuse_io->disp, arg->offset);
		fsdev_io->u_in.readdir.entry_cb_fn = fuse_dispatcher_readdir_usr_entry_clb;
	} else {
		/* For READDIR, we first try SPDK_FSDEV_IO_READDIR_SIMPLE. */
		fuse_init_fsdev_io_ex(fuse_io, SPDK_FSDEV_IO_READDIR_SIMPLE, fuse_dispatcher_readdir_cpl_cb);

		fsdev_io->u_in.readdir_simple.fobject = file_object(fuse_io);
		fsdev_io->u_in.readdir_simple.fhandle = file_handle(fh);
		fsdev_io->u_in.readdir_simple.offset = fsdev_io_d2h_u64(fuse_io->disp, arg->offset);
		fsdev_io->u_in.readdir_simple.entry_cb_fn = fuse_dispatcher_readdir_simple_usr_entry_clb;
	}

	return 0;
}

static int
fuse_dispatcher_fill_readdir(struct fuse_io *fuse_io)
{
	return fuse_dispatcher_fill_readdir_common(fuse_io, false);
}

static int
fuse_dispatcher_fill_readdirplus(struct fuse_io *fuse_io)
{
	return fuse_dispatcher_fill_readdir_common(fuse_io, true);
}

static int
fuse_dispatcher_fill_releasedir(struct fuse_io *fuse_io)
{
	struct spdk_fsdev_io *fsdev_io = fuse_to_fsdev_io(fuse_io);
	struct fuse_release_in *arg;
	uint64_t fh;

	arg = _fsdev_io_in_arg_get_buf(fuse_io, sizeof(*arg));
	if (!arg) {
		SPDK_ERRLOG("Cannot get fuse_release_in\n");
		return -EINVAL;
	}

	fh = fsdev_io_d2h_u64(fuse_io->disp, arg->fh);

	fuse_init_fsdev_io(fuse_io, SPDK_FSDEV_IO_RELEASEDIR);

	fsdev_io->u_in.releasedir.fobject = file_object(fuse_io);
	fsdev_io->u_in.releasedir.fhandle = file_handle(fh);

	return 0;
}

static int
fuse_dispatcher_fill_fsyncdir(struct fuse_io *fuse_io)
{
	struct spdk_fsdev_io *fsdev_io = fuse_to_fsdev_io(fuse_io);
	struct fuse_fsync_in *arg;
	uint64_t fh;
	bool datasync;

	arg = _fsdev_io_in_arg_get_buf(fuse_io, sizeof(*arg));
	if (!arg) {
		SPDK_ERRLOG("Cannot get fuse_fsync_in\n");
		return -EINVAL;
	}

	fh = fsdev_io_d2h_u64(fuse_io->disp, arg->fh);
	datasync = (fsdev_io_d2h_u32(fuse_io->disp, arg->fsync_flags) & 1) ? true : false;

	fuse_init_fsdev_io(fuse_io, SPDK_FSDEV_IO_FSYNCDIR);

	fsdev_io->u_in.fsyncdir.fobject = file_object(fuse_io);
	fsdev_io->u_in.fsyncdir.fhandle = file_handle(fh);
	fsdev_io->u_in.fsyncdir.datasync = datasync;

	return 0;
}

static int
fuse_dispatcher_fill_getlk(struct fuse_io *fuse_io)
{
	struct spdk_fsdev_io *fsdev_io = fuse_to_fsdev_io(fuse_io);
	struct fuse_lk_in *arg;
	uint64_t fh;
	int err;

	arg = _fsdev_io_in_arg_get_buf(fuse_io, sizeof(*arg));
	if (!arg) {
		SPDK_ERRLOG("Cannot get fuse_lk_in\n");
		return -EINVAL;
	}

	fh = fsdev_io_d2h_u64(fuse_io->disp, arg->fh);

	fuse_init_fsdev_io(fuse_io, SPDK_FSDEV_IO_GETLK);

	err = fuse_to_fsdev_file_lock(fuse_io, &arg->lk, &fsdev_io->u_in.getlk.lock);
	if (err) {
		return err;
	}

	fsdev_io->u_in.getlk.fobject = file_object(fuse_io);
	fsdev_io->u_in.getlk.fhandle = file_handle(fh);
	fsdev_io->u_in.getlk.owner = fsdev_io_d2h_u64(fuse_io->disp, arg->owner);

	return 0;
}

static int
fuse_dispatcher_fill_setlk(struct fuse_io *fuse_io)
{
	struct spdk_fsdev_io *fsdev_io = fuse_to_fsdev_io(fuse_io);
	struct fuse_lk_in *arg;
	uint64_t fh;
	uint32_t lk_flags;
	uint64_t owner;

	arg = _fsdev_io_in_arg_get_buf(fuse_io, sizeof(*arg));
	if (!arg) {
		SPDK_ERRLOG("Cannot get fuse_lk_in\n");
		return -EINVAL;
	}

	fh = fsdev_io_d2h_u64(fuse_io->disp, arg->fh);
	owner = fsdev_io_d2h_u64(fuse_io->disp, arg->owner);
	lk_flags = fsdev_io_d2h_u32(fuse_io->disp, arg->lk_flags);

	/* Handling flock style of the lock. */
	if (lk_flags & FUSE_LK_FLOCK) {
		enum spdk_fsdev_file_lock_op op;

		switch (fsdev_io_d2h_u32(fuse_io->disp, arg->lk.type)) {
		case F_RDLCK:
			op = SPDK_FSDEV_LOCK_SH;
			break;
		case F_WRLCK:
			op = SPDK_FSDEV_LOCK_EX;
			break;
		case F_UNLCK:
			op = SPDK_FSDEV_LOCK_UN;
			break;
		default:
			SPDK_ERRLOG("Invalid lock type %d in fuse_lk_in\n",
				    fsdev_io_d2h_u32(fuse_io->disp, arg->lk.type));
			return -EINVAL;
		}

		fuse_init_fsdev_io(fuse_io, SPDK_FSDEV_IO_FLOCK);

		fsdev_io->u_in.flock.fobject = file_object(fuse_io);
		fsdev_io->u_in.flock.fhandle = file_handle(fh);
		fsdev_io->u_in.flock.operation = op;
	} else {
		int err;

		fuse_init_fsdev_io(fuse_io, SPDK_FSDEV_IO_SETLK);

		err = fuse_to_fsdev_file_lock(fuse_io, &arg->lk, &fsdev_io->u_in.setlk.lock);
		if (err) {
			return err;
		}

		fsdev_io->u_in.setlk.fobject = file_object(fuse_io);
		fsdev_io->u_in.setlk.fhandle = file_handle(fh);
		fsdev_io->u_in.setlk.owner = owner;
		fsdev_io->u_in.setlk.wait = false;
	}

	return 0;
}

static int
fuse_dispatcher_fill_setlkw(struct fuse_io *fuse_io)
{
	struct spdk_fsdev_io *fsdev_io = fuse_to_fsdev_io(fuse_io);
	int err;
	struct fuse_lk_in *arg;

	arg = _fsdev_io_in_arg_get_buf(fuse_io, sizeof(*arg));
	if (!arg) {
		SPDK_ERRLOG("Cannot get fuse_lk_in\n");
		return -EINVAL;
	}

	fuse_io->u.setlkw.fhandle = fsdev_io_d2h_u64(fuse_io->disp, arg->fh);
	fuse_io->u.setlkw.owner = fsdev_io_d2h_u64(fuse_io->disp, arg->owner);

	fuse_init_fsdev_io(fuse_io, SPDK_FSDEV_IO_SETLK);

	err = fuse_to_fsdev_file_lock(fuse_io, &arg->lk, &fuse_io->u.setlkw.lock);
	if (err) {
		return err;
	}

	fsdev_io->u_in.setlk.fobject = file_object(fuse_io);
	fsdev_io->u_in.setlk.fhandle = file_handle(fuse_io->u.setlkw.fhandle);
	fsdev_io->u_in.setlk.lock = fuse_io->u.setlkw.lock;
	fsdev_io->u_in.setlk.owner = fuse_io->u.setlkw.owner;
	fsdev_io->u_in.setlk.wait = true;

	return 0;
}

static int
fuse_dispatcher_fill_access(struct fuse_io *fuse_io)
{
	struct spdk_fsdev_io *fsdev_io = fuse_to_fsdev_io(fuse_io);
	struct fuse_access_in *arg;
	uint32_t mask;

	arg = _fsdev_io_in_arg_get_buf(fuse_io, sizeof(*arg));
	if (!arg) {
		SPDK_ERRLOG("Cannot get fuse_access_in\n");
		return -EINVAL;
	}

	mask = fsdev_io_h2d_u32(fuse_io->disp, arg->mask);

	fuse_init_fsdev_io(fuse_io, SPDK_FSDEV_IO_ACCESS);

	fsdev_io->u_in.access.fobject = file_object(fuse_io);
	fsdev_io->u_in.access.mask = mask;
	fsdev_io->u_in.access.uid = geteuid();
	fsdev_io->u_in.access.gid = getegid();

	return 0;
}

static int
fuse_dispatcher_fill_create(struct fuse_io *fuse_io)
{
	struct spdk_fuse_dispatcher *disp = fuse_io->disp;
	struct spdk_fsdev_io *fsdev_io = fuse_to_fsdev_io(fuse_io);
	bool compat = fsdev_io_proto_minor(fuse_io) < 12;
	struct fuse_create_in *arg;
	const char *name;
	uint32_t flags, mode, umask = 0;
	size_t arg_size = compat ? sizeof(struct fuse_open_in) : sizeof(*arg);

	arg = _fsdev_io_in_arg_get_buf(fuse_io, arg_size);
	if (!arg) {
		SPDK_ERRLOG("Cannot get fuse_create_in (compat=%d)\n", compat);
		return -EINVAL;
	}

	name = _fsdev_io_in_arg_get_str(fuse_io);
	if (!name) {
		SPDK_ERRLOG("Cannot get name (compat=%d)\n", compat);
		return -EINVAL;
	}

	mode =  fsdev_io_d2h_u32(fuse_io->disp, arg->mode);
	if (!compat) {
		umask = fsdev_io_d2h_u32(fuse_io->disp, arg->umask);
	}

	if (!fsdev_d2h_open_flags(disp->fuse_arch, fsdev_io_d2h_u32(fuse_io->disp, arg->flags), &flags)) {
		SPDK_ERRLOG("Cannot translate flags\n");
		return -EINVAL;
	}

	fuse_init_fsdev_io(fuse_io, SPDK_FSDEV_IO_CREATE);

	fsdev_io->u_in.create.parent_fobject = file_object(fuse_io);
	fsdev_io->u_in.create.name = name;
	fsdev_io->u_in.create.mode = mode;
	fsdev_io->u_in.create.flags = flags;
	fsdev_io->u_in.create.umask = umask;
	fsdev_io->u_in.create.euid = fuse_io->hdr.uid;
	fsdev_io->u_in.create.egid = fuse_io->hdr.gid;

	return 0;
}

static int
fuse_dispatcher_fill_interrupt(struct fuse_io *fuse_io)
{
	struct spdk_fsdev_io *fsdev_io = fuse_to_fsdev_io(fuse_io);
	struct fuse_interrupt_in *arg;
	uint64_t unique;

	arg = _fsdev_io_in_arg_get_buf(fuse_io, sizeof(*arg));
	if (!arg) {
		SPDK_ERRLOG("Cannot get fuse_access_in\n");
		return -EINVAL;
	}

	unique = fsdev_io_d2h_u64(fuse_io->disp, arg->unique);

	SPDK_DEBUGLOG(fuse_dispatcher, "INTERRUPT: %" PRIu64 "\n", unique);

	/* FUSE_INTERRUPT requires no response */
	fuse_init_fsdev_io_ex(fuse_io, SPDK_FSDEV_IO_ABORT, fuse_dispatcher_no_resp_cpl_cb);

	fsdev_io->u_in.abort.unique_to_abort = unique;

	return 0;
}

static int
fuse_dispatcher_fill_bmap(struct fuse_io *fuse_io)
{
	SPDK_ERRLOG("BMAP is not supported\n");
	return -ENOSYS;
}

static struct fuse_ioctl_iovec *
fsdev_ioctl_iovec_to_fuse_copy(struct fuse_io *fuse_io, const struct iovec *iov, size_t count)
{
	struct fuse_ioctl_iovec *fiov;
	size_t i;

	fiov = calloc(1, sizeof(struct fuse_ioctl_iovec) * count);
	if (!fiov) {
		return NULL;
	}

	for (i = 0; i < count; i++) {
		fiov[i].base = fsdev_io_h2d_u64(fuse_io->disp, (uint64_t)(uintptr_t)iov[i].iov_base);
		fiov[i].len = fsdev_io_h2d_u64(fuse_io->disp, (uint64_t)(uintptr_t)iov[i].iov_len);
	}

	return fiov;
}

static struct iovec *
fuse_ioctl_iovec_copy(const struct iovec *iov, size_t count)
{
	size_t size = sizeof(*iov) * count;
	struct iovec *result;

	assert(iov && count);

	result = calloc(1, size);
	if (!result) {
		return NULL;
	}
	memcpy(result, iov, size);
	return result;
}

typedef void (*fuse_dispatcher_ioctl_cpl_cb)(struct fuse_io *fuse_io, size_t size,
		int32_t out_flags, int32_t result,
		struct iovec *in_iov, uint32_t in_iovcnt,
		struct iovec *out_iov, uint32_t out_iovcnt);
/**
 * Unrestricted version of ioctl() completion callback.
 *
 * It returns ioctl() result in a set of fuse_ioctl_iovec and though it is
 * primarily used for FUSE_IOCTL_RETRY case it seems nothing stops it from being
 * used for a traditional ioctl() that gets/sets internal data tha size of which
 * is known and FUSE_IOCTL_RETRY is not required.
 */
static void
fuse_dispatcher_io_complete_unrestricted_ioctl(struct fuse_io *fuse_io, size_t size,
		int32_t out_flags, int32_t result,
		struct iovec *in_iov, uint32_t in_iovcnt,
		struct iovec *out_iov, uint32_t out_iovcnt)
{
	struct fuse_ioctl_iovec *fiov = NULL;
	struct fuse_ioctl_iovec *in_fiov = NULL;
	struct fuse_ioctl_iovec *out_fiov = NULL;

	if (in_iovcnt) {
		size_t in_size = sizeof(*fiov) * in_iovcnt;

		fiov = _fsdev_io_out_arg_get_buf(fuse_io, in_size);
		if (!fiov) {
			SPDK_ERRLOG("Cannot get ioctl iovec out buffer\n");
			fuse_dispatcher_io_complete_err(fuse_io, -EINVAL);
			return;
		}

		/* Converting to struct fuse_ioctl_iovec with uint64_t fields. */
		in_fiov = fsdev_ioctl_iovec_to_fuse_copy(fuse_io, in_iov, in_iovcnt);
		if (!in_fiov) {
			fuse_dispatcher_io_complete_err(fuse_io, -ENOMEM);
			return;
		}
		memcpy(fiov, in_fiov, in_size);
		size += in_size;
		free(in_fiov);
	}
	if (out_iovcnt) {
		size_t out_size = sizeof(*fiov) * out_iovcnt;

		fiov = _fsdev_io_out_arg_get_buf(fuse_io, out_size);
		if (!fiov) {
			SPDK_ERRLOG("Cannot get ioctl iovec out buffer\n");
			fuse_dispatcher_io_complete_err(fuse_io, -EINVAL);
			return;
		}

		/* Converting to struct fuse_ioctl_iovec with uint64_t fields. */
		out_fiov = fsdev_ioctl_iovec_to_fuse_copy(fuse_io, out_iov, out_iovcnt);
		if (!out_fiov) {
			fuse_dispatcher_io_complete_err(fuse_io, -ENOMEM);
			return;
		}
		memcpy(fiov, out_fiov, out_size);
		size += out_size;
		free(out_fiov);
	}

	fuse_dispatcher_io_complete_ok(fuse_io, size);
}

static void
fuse_dispatcher_io_complete_restricted_ioctl(struct fuse_io *fuse_io, size_t size,
		int32_t out_flags, int32_t result,
		struct iovec *in_iov, uint32_t in_iovcnt,
		struct iovec *out_iov, uint32_t out_iovcnt)
{
	if (in_iovcnt) {
		SPDK_ERRLOG("Got unexpected for restricted ioctl() input "
			    "buffer to be returned to the FUSE - ignoring.\n");
	}
	if (out_iovcnt) {
		SPDK_ERRLOG("Got unexpected for restricted ioctl() output "
			    "buffer to be returned to the FUSE - ignoring.\n");
	}

	/*
	 * The out buffer has already been populated (if any). Make sure to have
	 * correct size in the header.
	 */
	size += fuse_io->u.ioctl.out_size;

	fuse_dispatcher_io_complete_ok(fuse_io, size);
}

static void
fuse_dispatcher_io_complete_ioctl(struct fuse_io *fuse_io,
				  bool retry, int32_t result,
				  struct iovec *in_iov, uint32_t in_iovcnt,
				  struct iovec *out_iov, uint32_t out_iovcnt)
{
	struct fuse_ioctl_out *arg;
	fuse_dispatcher_ioctl_cpl_cb ioctl_cpl_cb;
	uint32_t in_flags = fuse_io->u.ioctl.flags;
	uint32_t out_flags = retry ? FUSE_IOCTL_RETRY : 0;

	arg = _fsdev_io_out_arg_get_buf(fuse_io, sizeof(*arg));
	if (!arg) {
		SPDK_ERRLOG("Cannot get fuse_ioctl_out\n");
		fuse_dispatcher_io_complete_err(fuse_io, -EINVAL);
		return;
	}

	arg->result = fsdev_io_h2d_u32(fuse_io->disp, (uint32_t)result);
	arg->flags = fsdev_io_h2d_u32(fuse_io->disp, out_flags);
	arg->in_iovs = fsdev_io_h2d_u32(fuse_io->disp, in_iovcnt);
	arg->out_iovs = fsdev_io_h2d_u32(fuse_io->disp, out_iovcnt);

	if (in_flags & FUSE_IOCTL_UNRESTRICTED) {
		ioctl_cpl_cb = fuse_dispatcher_io_complete_unrestricted_ioctl;
	} else {
		ioctl_cpl_cb = fuse_dispatcher_io_complete_restricted_ioctl;
	}

	ioctl_cpl_cb(fuse_io, sizeof(*arg), out_flags, result, in_iov, in_iovcnt,
		     out_iov, out_iovcnt);
}

static void
fuse_dispatcher_ioctl_cpl_clb(void *cb_arg, int status, struct spdk_fsdev_io *fsdev_io)
{
	struct fuse_io *fuse_io = cb_arg;
	bool retry = (status == -EAGAIN);
	uint32_t in_flags = fuse_io->u.ioctl.flags;

	/*
	 * We get -EAGAIN on retry requested by the fsdev, this is not an error.
	 */
	if (retry) {
		/*
		 * Retry without FUSE_IOCTL_UNRESTRICTED is not allowed.
		 */
		status = (in_flags & FUSE_IOCTL_UNRESTRICTED) ? 0 : -EIO;
	} else if (!status && (fsdev_io->u_out.ioctl.in_iovcnt || fsdev_io->u_out.ioctl.out_iovcnt)) {
		/*
		 * The final stage (no retry case) should populate the data into
		 * the buffers. Retruning iovecs is not allowed and will corrupt
		 * the data.
		 */
		SPDK_ERRLOG("The FSDEV module populated some iovecs with in_iovcnt=%u "
			    "and out_iovcnt=%u for non-retry case, when it was supposed "
			    "to populate the data buffers only.\n",
			    fsdev_io->u_out.ioctl.in_iovcnt, fsdev_io->u_out.ioctl.out_iovcnt);
		status = -EIO;
	}

	if (!status) {
		fuse_dispatcher_io_complete_ioctl(fuse_io, retry, fsdev_io->u_out.ioctl.result,
						  fsdev_io->u_out.ioctl.in_iov, fsdev_io->u_out.ioctl.in_iovcnt,
						  fsdev_io->u_out.ioctl.out_iov, fsdev_io->u_out.ioctl.out_iovcnt);
	} else {
		fuse_dispatcher_io_complete_err(fuse_io, status);
	}

	/* Allocated in fuse_dispatcher_fill_ioctl(). */
	free(fuse_io->u.ioctl.in_iov);
	free(fuse_io->u.ioctl.out_iov);
}

static int
fuse_dispatcher_fill_ioctl(struct fuse_io *fuse_io)
{
	struct spdk_fsdev_io *fsdev_io = fuse_to_fsdev_io(fuse_io);
	int err;
	struct fuse_ioctl_in *in;
	uint64_t fh;
	uint32_t flags;
	uint32_t request;
	uint64_t arg;
	uint32_t in_size;
	uint32_t out_size;
	struct iovec in_iov[1];
	struct iovec out_iov[1];

	in = _fsdev_io_in_arg_get_buf(fuse_io, sizeof(*in));
	if (!in) {
		SPDK_ERRLOG("Cannot get fuse_ioctl_in\n");
		return -EINVAL;
	}

	flags = fsdev_io_d2h_u32(fuse_io->disp, in->flags);

	/*
	 * FUSE_IOCTL_COMPAT is used when 32-bit user space app calls ioctl()
	 * on a 64-bit kernel.
	 */
	if (flags & (FUSE_IOCTL_COMPAT | FUSE_IOCTL_32BIT)) {
		SPDK_ERRLOG("Compat ioctl is not supported.\n");
		return -EINVAL;
	}

	/*
	 * Another compat flag. Not supported.
	 */
	if (flags & FUSE_IOCTL_COMPAT_X32) {
		SPDK_ERRLOG("Compat x32 ioctl is not supported.\n");
		return -EINVAL;
	}

	fh = fsdev_io_d2h_u64(fuse_io->disp, in->fh);
	request = fsdev_io_d2h_u32(fuse_io->disp, in->cmd);
	in_size = fsdev_io_d2h_u32(fuse_io->disp, in->in_size);
	out_size = fsdev_io_d2h_u32(fuse_io->disp, in->out_size);
	arg = fsdev_io_d2h_u64(fuse_io->disp, in->arg);

	if (in_size) {
		in_iov[0].iov_base = _fsdev_io_in_arg_get_buf(fuse_io, in_size);
		if (!in_iov[0].iov_base) {
			SPDK_ERRLOG("Failed to get input buf of size=%u\n", in_size);
			return -EINVAL;
		}
		in_iov[0].iov_len = in_size;

		fuse_io->u.ioctl.in_iov = fuse_ioctl_iovec_copy(in_iov, 1);
		if (!fuse_io->u.ioctl.in_iov) {
			SPDK_ERRLOG("Cannot alloc ioctl iovecs.\n");
			return -EINVAL;
		}
		fuse_io->u.ioctl.in_iovcnt = 1;
	} else {
		fuse_io->u.ioctl.in_iov = NULL;
		fuse_io->u.ioctl.in_iovcnt = 0;
	}

	/*
	 * Getting out buffer to avoid copying allow the fsdev to use it directly
	 * for any returned data.
	 */
	if (out_size) {
		char *buff;

		/* Preserve the out offset. */
		struct iov_offs out_offs_bu = fuse_io->out_offs;

		/* Skip the fuse_ioctl_out. */
		_fsdev_io_out_arg_get_buf(fuse_io, sizeof(struct fuse_ioctl_out));

		/* Get the buffer for the out iovec. */
		buff = _fsdev_io_out_arg_get_buf(fuse_io, out_size);
		if (!buff) {
			SPDK_INFOLOG(fuse_dispatcher, "Got NULL ioctl out buffer.\n");
			err = -EINVAL;
			goto out_err;
		}

		/*
		 * Restore the out offset so it works on populating the output in
		 * comeption cb.
		 */
		fuse_io->out_offs = out_offs_bu;

		out_iov[0].iov_base = buff;
		out_iov[0].iov_len = out_size;

		fuse_io->u.ioctl.out_iov = fuse_ioctl_iovec_copy(out_iov, 1);
		if (!fuse_io->u.ioctl.out_iov) {
			SPDK_ERRLOG("Cannot alloc ioctl iovecs.\n");
			err = -ENOMEM;
			goto out_err;
		}
		fuse_io->u.ioctl.out_iovcnt = 1;
	} else {
		fuse_io->u.ioctl.out_iov = NULL;
		fuse_io->u.ioctl.out_iovcnt = 0;
	}

	/* Used in the completion cb for checking UNRESTRICTED & RETRY flags. */
	fuse_io->u.ioctl.flags = flags;
	fuse_io->u.ioctl.out_size = out_size;

	fuse_init_fsdev_io_ex(fuse_io, SPDK_FSDEV_IO_IOCTL, fuse_dispatcher_ioctl_cpl_clb);

	fsdev_io->u_in.ioctl.fobject = file_object(fuse_io);
	fsdev_io->u_in.ioctl.fhandle = file_handle(fh);
	fsdev_io->u_in.ioctl.request = request;
	fsdev_io->u_in.ioctl.arg = arg;

	fsdev_io->u_in.ioctl.in_iov = fuse_io->u.ioctl.in_iov;
	fsdev_io->u_in.ioctl.in_iovcnt = fuse_io->u.ioctl.in_iovcnt;

	fsdev_io->u_in.ioctl.out_iov = fuse_io->u.ioctl.out_iov;
	fsdev_io->u_in.ioctl.out_iovcnt = fuse_io->u.ioctl.out_iovcnt;

	/* Zero out the out values so we know what to free in fuse_dispatcher_ioctl_cpl_clb() */
	fsdev_io->u_out.ioctl.in_iov = NULL;
	fsdev_io->u_out.ioctl.in_iovcnt = 0;
	fsdev_io->u_out.ioctl.out_iov = NULL;
	fsdev_io->u_out.ioctl.out_iovcnt = 0;

	return 0;

out_err:
	free(fuse_io->u.ioctl.in_iov);
	free(fuse_io->u.ioctl.out_iov);

	return err;
}

static int
fuse_dispatcher_fill_poll(struct fuse_io *fuse_io)
{
	struct spdk_fsdev_io *fsdev_io = fuse_to_fsdev_io(fuse_io);
	struct fuse_poll_in *arg;

	arg = _fsdev_io_in_arg_get_buf(fuse_io, sizeof(*arg));
	if (!arg) {
		SPDK_ERRLOG("Cannot get fuse_poll_in\n");
		return -EINVAL;
	}

	fuse_io->u.poll.fhandle = fsdev_io_d2h_u64(fuse_io->disp, arg->fh);
	fuse_io->u.poll.events = fuse_events_to_fsdev(fsdev_io_d2h_u32(fuse_io->disp, arg->events));

	fuse_init_fsdev_io(fuse_io, SPDK_FSDEV_IO_POLL);

	fsdev_io->u_in.poll.fobject = file_object(fuse_io);
	fsdev_io->u_in.poll.fhandle = file_handle(fuse_io->u.poll.fhandle);
	fsdev_io->u_in.poll.events = fuse_io->u.poll.events;
	fsdev_io->u_in.poll.wait = true;

	return 0;
}

#define FALLOC_FLAGS_MAP \
	FALLOC_FLAG(FL_KEEP_SIZE)	\
	FALLOC_FLAG(FL_PUNCH_HOLE)	\
	FALLOC_FLAG(FL_NO_HIDE_STALE)	\
	FALLOC_FLAG(FL_COLLAPSE_RANGE)	\
	FALLOC_FLAG(FL_ZERO_RANGE)	\
	FALLOC_FLAG(FL_INSERT_RANGE)	\
	FALLOC_FLAG(FL_UNSHARE_RANGE)

static uint32_t
fuse_falloc_flags_to_fsdev(uint32_t flags)
{
	uint32_t result = 0;

#define FALLOC_FLAG(name) \
	if (flags & FALLOC_##name) {			\
		result |= SPDK_FSDEV_FALLOC_##name;	\
	}

	FALLOC_FLAGS_MAP;

#undef FALLOC_FLAG

	return result;
}

static int
fuse_dispatcher_fill_fallocate(struct fuse_io *fuse_io)
{
	struct spdk_fsdev_io *fsdev_io = fuse_to_fsdev_io(fuse_io);
	struct fuse_fallocate_in *arg;
	uint32_t mode;
	uint64_t fh;

	arg = _fsdev_io_in_arg_get_buf(fuse_io, sizeof(*arg));
	if (!arg) {
		SPDK_ERRLOG("Cannot get fuse_fallocate_in\n");
		return -EINVAL;

	}

	fh = fsdev_io_d2h_u64(fuse_io->disp, arg->fh);
	mode = fuse_falloc_flags_to_fsdev(fsdev_io_d2h_u32(fuse_io->disp, arg->mode));

	fuse_init_fsdev_io(fuse_io, SPDK_FSDEV_IO_FALLOCATE);

	fsdev_io->u_in.fallocate.fobject = file_object(fuse_io);
	fsdev_io->u_in.fallocate.fhandle = file_handle(fh);
	fsdev_io->u_in.fallocate.mode = mode;
	fsdev_io->u_in.fallocate.offset = fsdev_io_d2h_u64(fuse_io->disp, arg->offset);
	fsdev_io->u_in.fallocate.length = fsdev_io_d2h_u64(fuse_io->disp, arg->length);

	return 0;
}

static void
fuse_dispatcher_fill_umount_cpl_clb(void *cb_arg, int status, struct spdk_fsdev_io *fsdev_io)
{
	struct fuse_io *fuse_io = cb_arg;
	struct spdk_fuse_dispatcher *disp = fuse_io->disp;

	disp->proto_major = disp->proto_minor = 0;
	disp->root_fobject = NULL;
	SPDK_DEBUGLOG(fuse_dispatcher, "%s unmounted\n", fuse_dispatcher_name(disp));

	/* Save the state */
	fuse_dispatcher_update_rmem(disp);

	fuse_dispatcher_io_complete_err(fuse_io, 0);
}

static int
fuse_dispatcher_fill_destroy(struct fuse_io *fuse_io)
{
	fuse_init_fsdev_io_ex(fuse_io, SPDK_FSDEV_IO_UMOUNT, fuse_dispatcher_fill_umount_cpl_clb);

	return 0;
}

static void fuse_dispatcher_batch_forget_next(struct fuse_io *fuse_io);

static void
fuse_dispatcher_batch_forget_cpl_cb(void *cb_arg, int status, struct spdk_fsdev_io *fsdev_io)
{
	struct fuse_io *fuse_io = cb_arg;

	if (status) {
		fuse_io->u.batch_forget.status = status;
	}

	fuse_io->u.batch_forget.idx++;
	if (fuse_io->u.batch_forget.idx < fuse_io->u.batch_forget.to_forget) {
		fuse_dispatcher_batch_forget_next(fuse_io);
		return;
	}

	SPDK_DEBUGLOG(fuse_dispatcher, "%s: all nodes forgotten\n",
		      fuse_dispatcher_name(fuse_io->disp));

	/* FUSE_BATCH_FORGET requires no response */
	fuse_dispatcher_io_complete_none(fuse_io, fuse_io->u.batch_forget.status);
}

static void
fuse_dispatcher_batch_forget_next(struct fuse_io *fuse_io)
{
	struct spdk_fsdev_io *fsdev_io = fuse_to_fsdev_io(fuse_io);
	struct fuse_forget_data *forget;
	uint64_t ino;
	uint64_t nlookup;

	forget = &fuse_io->u.batch_forget.forgets[fuse_io->u.batch_forget.idx];
	ino = fsdev_io_d2h_u64(fuse_io->disp, forget->ino);
	nlookup = fsdev_io_d2h_u64(fuse_io->disp, forget->nlookup);

	fuse_init_fsdev_io_ex(fuse_io, SPDK_FSDEV_IO_FORGET, fuse_dispatcher_batch_forget_cpl_cb);

	fsdev_io->u_in.forget.fobject = ino_to_object(fuse_io, ino);
	fsdev_io->u_in.forget.nlookup = nlookup;

	SPDK_DEBUGLOG(fuse_dispatcher, "%s: forget issued (nodeid=0x%" PRIx64 " nlookup=%" PRIu64 "\n",
		      fuse_dispatcher_name(fuse_io->disp), ino, nlookup);

	spdk_fsdev_io_submit(fsdev_io);
}

static int
fuse_dispatcher_batch_forget(struct fuse_io *fuse_io)
{
	struct fuse_batch_forget_in *arg;
	struct fuse_forget_data *forgets;
	size_t scount;
	uint32_t count;

	arg = _fsdev_io_in_arg_get_buf(fuse_io, sizeof(*arg));
	if (!arg) {
		SPDK_ERRLOG("Cannot get fuse_batch_forget_in\n");
		return -EINVAL;
	}

	/* Prevent integer overflow.  The compiler emits the following warning
	 * unless we use the scount local variable:
	 *
	 * error: comparison is always false due to limited range of data type
	 * [-Werror=type-limits]
	 *
	 * This may be true on 64-bit hosts but we need this check for 32-bit
	 * hosts.
	 */
	scount = fsdev_io_d2h_u32(fuse_io->disp, arg->count);
	if (scount > SIZE_MAX / sizeof(forgets[0])) {
		SPDK_WARNLOG("Too many forgets (%zu >= %zu)\n", scount,
			     SIZE_MAX / sizeof(forgets[0]));
		/* FUSE_BATCH_FORGET requires no response */
		return -EINVAL;
	}

	count = scount;
	if (!count) {
		SPDK_WARNLOG("0 forgets requested\n");
		/* FUSE_BATCH_FORGET requires no response */
		return -EINVAL;
	}

	fuse_io->u.batch_forget.forgets = _fsdev_io_in_arg_get_buf(fuse_io, count * sizeof(forgets[0]));
	if (!fuse_io->u.batch_forget.forgets) {
		SPDK_WARNLOG("Cannot get expected forgets (%" PRIu32 ")\n", count);
		/* FUSE_BATCH_FORGET requires no response */
		return -EINVAL;
	}

	fuse_io->u.batch_forget.to_forget = count;
	fuse_io->u.batch_forget.status = 0;
	fuse_io->u.batch_forget.idx = 0;

	SPDK_DEBUGLOG(fuse_dispatcher, "%s: %" PRIu32 " fobjects to forget\n",
		      fuse_dispatcher_name(fuse_io->disp), count);

	fuse_dispatcher_batch_forget_next(fuse_io);

	return 0; /* some forgets are still in progress, wait for completions */
}

static int
fuse_dispatcher_fill_lseek(struct fuse_io *fuse_io)
{
	struct spdk_fsdev_io *fsdev_io = fuse_to_fsdev_io(fuse_io);
	struct fuse_lseek_in *arg;
	uint64_t fh;
	uint64_t offset;
	enum spdk_fsdev_seek_whence whence;

	arg = _fsdev_io_in_arg_get_buf(fuse_io, sizeof(*arg));
	if (!arg) {
		SPDK_ERRLOG("Cannot get fuse_lseek_in\n");
		return -EINVAL;
	}

	fh = fsdev_io_d2h_u64(fuse_io->disp, arg->fh);
	offset = fsdev_io_d2h_u64(fuse_io->disp, arg->offset);

	switch (fsdev_io_d2h_u32(fuse_io->disp, arg->whence)) {
	case SEEK_SET:
		whence = SPDK_FSDEV_SEEK_SET;
		break;
	case SEEK_CUR:
		whence = SPDK_FSDEV_SEEK_CUR;
		break;
	case SEEK_END:
		whence = SPDK_FSDEV_SEEK_END;
		break;
	case SEEK_DATA:
		whence = SPDK_FSDEV_SEEK_DATA;
		break;
	case SEEK_HOLE:
		whence = SPDK_FSDEV_SEEK_HOLE;
		break;
	default:
		SPDK_ERRLOG("Invalid whence %d in fuse_lseek_in\n", fsdev_io_d2h_u32(fuse_io->disp, arg->whence));
		return -EINVAL;
	}

	fuse_init_fsdev_io(fuse_io, SPDK_FSDEV_IO_LSEEK);
	fsdev_io->u_in.lseek.fobject = file_object(fuse_io);
	fsdev_io->u_in.lseek.fhandle = file_handle(fh);
	fsdev_io->u_in.lseek.offset = offset;
	fsdev_io->u_in.lseek.whence = whence;

	return 0;
}

static int
fuse_dispatcher_fill_copy_file_range(struct fuse_io *fuse_io)
{
	struct spdk_fsdev_io *fsdev_io = fuse_to_fsdev_io(fuse_io);
	struct fuse_copy_file_range_in *arg;
	uint64_t fh_in, fh_out, nodeid_out;

	arg = _fsdev_io_in_arg_get_buf(fuse_io, sizeof(*arg));
	if (!arg) {
		SPDK_ERRLOG("Cannot get fuse_copy_file_range_in\n");
		return -EINVAL;
	}

	fh_in = fsdev_io_d2h_u64(fuse_io->disp, arg->fh_in);
	nodeid_out = fsdev_io_d2h_u64(fuse_io->disp, arg->nodeid_out);
	fh_out = fsdev_io_d2h_u64(fuse_io->disp, arg->fh_out);

	fuse_init_fsdev_io(fuse_io, SPDK_FSDEV_IO_COPY_FILE_RANGE);

	fsdev_io->u_in.copy_file_range.fobject_in = file_object(fuse_io);
	fsdev_io->u_in.copy_file_range.fhandle_in = file_handle(fh_in);
	fsdev_io->u_in.copy_file_range.off_in = fsdev_io_d2h_u64(fuse_io->disp, arg->off_in);
	fsdev_io->u_in.copy_file_range.fobject_out = ino_to_object(fuse_io, nodeid_out);
	fsdev_io->u_in.copy_file_range.fhandle_out = file_handle(fh_out);
	fsdev_io->u_in.copy_file_range.off_out = fsdev_io_d2h_u64(fuse_io->disp, arg->off_out);
	fsdev_io->u_in.copy_file_range.len = fsdev_io_d2h_u64(fuse_io->disp, arg->len);
	fsdev_io->u_in.copy_file_range.flags = fsdev_io_d2h_u64(fuse_io->disp, arg->flags);

	return 0;
}

static int
fuse_dispatcher_fill_setupmapping(struct fuse_io *fuse_io)
{
	SPDK_ERRLOG("SETUPMAPPING is not supported\n");
	return -ENOSYS;
}

static int
fuse_dispatcher_fill_removemapping(struct fuse_io *fuse_io)
{
	SPDK_ERRLOG("REMOVEMAPPING is not supported\n");
	return -ENOSYS;
}

static int
fuse_dispatcher_fill_syncfs(struct fuse_io *fuse_io)
{
	struct spdk_fsdev_io *fsdev_io = fuse_to_fsdev_io(fuse_io);
	struct fuse_syncfs_in *arg;

	arg = _fsdev_io_in_arg_get_buf(fuse_io, sizeof(*arg));
	if (!arg) {
		SPDK_ERRLOG("Cannot get fuse_syncfs_in\n");
		return -EINVAL;
	}

	fuse_init_fsdev_io(fuse_io, SPDK_FSDEV_IO_SYNCFS);
	fsdev_io->u_in.syncfs.fobject = file_object(fuse_io);

	return 0;
}

static int
fuse_dispatcher_notify_reply(struct fuse_io *fuse_io)
{
	struct fuse_notify_reply_in *arg;
	struct spdk_fsdev_notify_reply_data notify_reply_data;

	arg = _fsdev_io_in_arg_get_buf(fuse_io, sizeof(*arg));
	if (!arg) {
		SPDK_ERRLOG("Cannot get virtiofs_reply_in: unique %" PRIu64", len %u\n",
			    fuse_io->hdr.unique, fuse_io->hdr.len);
		return -EINVAL;
	}

	SPDK_INFOLOG(fuse_dispatcher, "FUSE_NOTIFY_REPLY: unique %" PRIu64 ", len %u, error %d\n",
		     fuse_io->hdr.unique, fuse_io->hdr.len, arg->error);
	if (fuse_io->disp->notify_reply_cb) {
		notify_reply_data.status = fsdev_io_h2d_i32(fuse_io->disp, arg->error);
		fuse_io->disp->notify_reply_cb(fuse_io->disp->notify_reply_cb_arg, &notify_reply_data,
					       fuse_io->hdr.unique);
	}

	return 0;
}

static const struct {
	const char *name;
} fuse_op_names[] = {
	[FUSE_LOOKUP]		= { "LOOKUP" },
	[FUSE_FORGET]		= { "FORGET" },
	[FUSE_GETATTR]		= { "GETATTR" },
	[FUSE_SETATTR]		= { "SETATTR" },
	[FUSE_READLINK]		= { "READLINK" },
	[FUSE_SYMLINK]		= { "SYMLINK" },
	[FUSE_MKNOD]		= { "MKNOD" },
	[FUSE_MKDIR]		= { "MKDIR" },
	[FUSE_UNLINK]		= { "UNLINK" },
	[FUSE_RMDIR]		= { "RMDIR" },
	[FUSE_RENAME]		= { "RENAME" },
	[FUSE_LINK]		= { "LINK" },
	[FUSE_OPEN]		= { "OPEN" },
	[FUSE_READ]		= { "READ" },
	[FUSE_WRITE]		= { "WRITE" },
	[FUSE_STATFS]		= { "STATFS" },
	[FUSE_RELEASE]		= { "RELEASE" },
	[FUSE_FSYNC]		= { "FSYNC" },
	[FUSE_SETXATTR]		= { "SETXATTR" },
	[FUSE_GETXATTR]		= { "GETXATTR" },
	[FUSE_LISTXATTR]	= { "LISTXATTR" },
	[FUSE_REMOVEXATTR]	= { "REMOVEXATTR" },
	[FUSE_FLUSH]		= { "FLUSH" },
	[FUSE_INIT]		= { "INIT" },
	[FUSE_OPENDIR]		= { "OPENDIR" },
	[FUSE_READDIR]		= { "READDIR" },
	[FUSE_RELEASEDIR]	= { "RELEASEDIR" },
	[FUSE_FSYNCDIR]		= { "FSYNCDIR" },
	[FUSE_GETLK]		= { "GETLK" },
	[FUSE_SETLK]		= { "SETLK" },
	[FUSE_SETLKW]		= { "SETLKW" },
	[FUSE_ACCESS]		= { "ACCESS" },
	[FUSE_CREATE]		= { "CREATE" },
	[FUSE_INTERRUPT]	= { "INTERRUPT" },
	[FUSE_BMAP]		= { "BMAP" },
	[FUSE_DESTROY]		= { "DESTROY" },
	[FUSE_IOCTL]		= { "IOCTL" },
	[FUSE_POLL]		= { "POLL" },
	[FUSE_NOTIFY_REPLY]	= { "NOTIFY_REPLY" },
	[FUSE_BATCH_FORGET]	= { "BATCH_FORGET" },
	[FUSE_FALLOCATE]	= { "FALLOCATE" },
	[FUSE_READDIRPLUS]	= { "READDIRPLUS" },
	[FUSE_RENAME2]		= { "RENAME2" },
	[FUSE_LSEEK]		= { "LSEEK" },
	[FUSE_COPY_FILE_RANGE]	= { "COPY_FILE_RANGE" },
	[FUSE_SETUPMAPPING]	= { "SETUPMAPPING" },
	[FUSE_REMOVEMAPPING]	= { "REMOVEMAPPING" },
	[FUSE_SYNCFS]		= { "SYNCFS" },
};

const char *
spdk_fuse_dispatcher_get_operation_name(uint32_t opcode)
{
	if (opcode >= SPDK_COUNTOF(fuse_op_names)) {
		return "UNKNOWN";
	}

	return fuse_op_names[opcode].name;
}

static int
fuse_dispatcher_submit_io(struct fuse_io *fuse_io)
{
	int rc;

	switch (fuse_io->hdr.opcode) {
	case FUSE_LOOKUP:
		rc = fuse_dispatcher_fill_lookup(fuse_io);
		break;
	case FUSE_FORGET:
		rc = fuse_dispatcher_fill_forget(fuse_io);
		break;
	case FUSE_GETATTR:
		rc = fuse_dispatcher_fill_getattr(fuse_io);
		break;
	case FUSE_SETATTR:
		rc = fuse_dispatcher_fill_setattr(fuse_io);
		break;
	case FUSE_READLINK:
		rc = fuse_dispatcher_fill_readlink(fuse_io);
		break;
	case FUSE_SYMLINK:
		rc = fuse_dispatcher_fill_symlink(fuse_io);
		break;
	case FUSE_MKNOD:
		rc = fuse_dispatcher_fill_mknod(fuse_io);
		break;
	case FUSE_MKDIR:
		rc = fuse_dispatcher_fill_mkdir(fuse_io);
		break;
	case FUSE_UNLINK:
		rc = fuse_dispatcher_fill_unlink(fuse_io);
		break;
	case FUSE_RMDIR:
		rc = fuse_dispatcher_fill_rmdir(fuse_io);
		break;
	case FUSE_RENAME:
		rc = fuse_dispatcher_fill_rename(fuse_io);
		break;
	case FUSE_LINK:
		rc = fuse_dispatcher_fill_link(fuse_io);
		break;
	case FUSE_OPEN:
		rc = fuse_dispatcher_fill_open(fuse_io);
		break;
	case FUSE_READ:
		rc = fuse_dispatcher_fill_read(fuse_io);
		break;
	case FUSE_WRITE:
		rc = fuse_dispatcher_fill_write(fuse_io);
		break;
	case FUSE_STATFS:
		rc = fuse_dispatcher_fill_statfs(fuse_io);
		break;
	case FUSE_RELEASE:
		rc = fuse_dispatcher_fill_release(fuse_io);
		break;
	case FUSE_FSYNC:
		rc = fuse_dispatcher_fill_fsync(fuse_io);
		break;
	case FUSE_SETXATTR:
		rc = fuse_dispatcher_fill_setxattr(fuse_io);
		break;
	case FUSE_GETXATTR:
		rc = fuse_dispatcher_fill_getxattr(fuse_io);
		break;
	case FUSE_LISTXATTR:
		rc = fuse_dispatcher_fill_listxattr(fuse_io);
		break;
	case FUSE_REMOVEXATTR:
		rc = fuse_dispatcher_fill_removexattr(fuse_io);
		break;
	case FUSE_FLUSH:
		rc = fuse_dispatcher_fill_flush(fuse_io);
		break;
	case FUSE_INIT:
		rc = fuse_dispatcher_fill_init(fuse_io);
		if (rc == -EAGAIN) {
			fuse_dispatcher_init_complete_again(fuse_io);
			return 0;
		}
		break;
	case FUSE_OPENDIR:
		rc = fuse_dispatcher_fill_opendir(fuse_io);
		break;
	case FUSE_READDIR:
		rc = fuse_dispatcher_fill_readdir(fuse_io);
		break;
	case FUSE_RELEASEDIR:
		rc = fuse_dispatcher_fill_releasedir(fuse_io);
		break;
	case FUSE_FSYNCDIR:
		rc = fuse_dispatcher_fill_fsyncdir(fuse_io);
		break;
	case FUSE_GETLK:
		rc = fuse_dispatcher_fill_getlk(fuse_io);
		break;
	case FUSE_SETLK:
		rc = fuse_dispatcher_fill_setlk(fuse_io);
		break;
	case FUSE_SETLKW:
		rc = fuse_dispatcher_fill_setlkw(fuse_io);
		break;
	case FUSE_ACCESS:
		rc = fuse_dispatcher_fill_access(fuse_io);
		break;
	case FUSE_CREATE:
		rc = fuse_dispatcher_fill_create(fuse_io);
		break;
	case FUSE_INTERRUPT:
		rc = fuse_dispatcher_fill_interrupt(fuse_io);
		break;
	case FUSE_BMAP:
		rc = fuse_dispatcher_fill_bmap(fuse_io);
		break;
	case FUSE_DESTROY:
		rc = fuse_dispatcher_fill_destroy(fuse_io);
		break;
	case FUSE_IOCTL:
		rc = fuse_dispatcher_fill_ioctl(fuse_io);
		break;
	case FUSE_POLL:
		rc = fuse_dispatcher_fill_poll(fuse_io);
		break;
	case FUSE_NOTIFY_REPLY:
		rc = fuse_dispatcher_notify_reply(fuse_io);
		fuse_dispatcher_io_complete_none(fuse_io, rc);
		return 0;
		break;
	case FUSE_BATCH_FORGET:
		rc = fuse_dispatcher_batch_forget(fuse_io);
		if (rc < 0) {
			/* FUSE_BATCH_FORGET requires no response */
			fuse_dispatcher_io_complete_none(fuse_io, rc);
		}
		return 0;
		break;
	case FUSE_FALLOCATE:
		rc = fuse_dispatcher_fill_fallocate(fuse_io);
		break;
	case FUSE_READDIRPLUS:
		rc = fuse_dispatcher_fill_readdirplus(fuse_io);
		break;
	case FUSE_RENAME2:
		rc = fuse_dispatcher_fill_rename2(fuse_io);
		break;
	case FUSE_LSEEK:
		rc = fuse_dispatcher_fill_lseek(fuse_io);
		break;
	case FUSE_COPY_FILE_RANGE:
		rc = fuse_dispatcher_fill_copy_file_range(fuse_io);
		break;
	case FUSE_SETUPMAPPING:
		rc = fuse_dispatcher_fill_setupmapping(fuse_io);
		break;
	case FUSE_REMOVEMAPPING:
		rc = fuse_dispatcher_fill_removemapping(fuse_io);
		break;
	case FUSE_SYNCFS:
		rc = fuse_dispatcher_fill_syncfs(fuse_io);
		break;
	default:
		SPDK_ERRLOG("Unsupported opcode: %" PRIu32 "\n", fuse_io->hdr.opcode);
		rc = -ENOSYS;
		break;
	}

	if (rc) {
		if (_fuse_op_requires_reply(fuse_io->hdr.opcode)) {
			fuse_dispatcher_io_complete_err(fuse_io, rc);
		} else {
			fuse_dispatcher_io_complete_none(fuse_io, rc);
		}
		return 0;
	}

	spdk_fsdev_io_submit(fuse_to_fsdev_io(fuse_io));
	return 0;
}

static int
fuse_dispatcher_handle_fuse_req(struct spdk_fuse_dispatcher *disp, struct fuse_io *fuse_io)
{
	struct fuse_in_header *hdr;

	if (!fuse_io->in_iovcnt || !fuse_io->in_iov) {
		SPDK_ERRLOG("Bad IO: no IN iov (%d, %p)\n", fuse_io->in_iovcnt, fuse_io->in_iov);
		goto exit;
	}

	hdr = _fsdev_io_in_arg_get_buf(fuse_io, sizeof(*hdr));
	if (!hdr) {
		SPDK_ERRLOG("Bad IO: cannot get fuse_in_header\n");
		goto exit;
	}

	fuse_io->hdr.opcode = fsdev_io_d2h_u32(fuse_io->disp, hdr->opcode);
	fuse_io->hdr.unique = fsdev_io_d2h_u64(fuse_io->disp, hdr->unique);

	if (spdk_unlikely(!fuse_io->ch)) {
		/* The fsdev is not currently active. Complete this request. */
		SPDK_ERRLOG("IO (%" PRIu32 ") arrived while there's no channel\n", fuse_io->hdr.opcode);
		goto exit;
	}

	if (spdk_likely(_fuse_op_requires_reply(hdr->opcode))) {
		struct fuse_out_header *out_hdr = _fsdev_io_out_arg_get_buf(fuse_io, sizeof(*out_hdr));
		if (!out_hdr) {
			SPDK_ERRLOG("Bad IO: cannot get out_hdr\n");
			goto exit;
		}

		UNUSED(out_hdr); /* We don't need it here, we just made a check and a reservation */
	}

	fuse_io->hdr.len = fsdev_io_d2h_u32(fuse_io->disp, hdr->len);
	fuse_io->hdr.nodeid = fsdev_io_d2h_u64(fuse_io->disp, hdr->nodeid);
	fuse_io->hdr.uid = fsdev_io_d2h_u32(fuse_io->disp, hdr->uid);
	fuse_io->hdr.gid = fsdev_io_d2h_u32(fuse_io->disp, hdr->gid);
	fuse_io->hdr.pid = fsdev_io_d2h_u32(fuse_io->disp, hdr->pid);

	SPDK_DEBUGLOG(fuse_dispatcher, "IO arrived: %" PRIu32 " (%s) len=%" PRIu32 " unique=%" PRIu64
		      " nodeid=0x%" PRIx64 " uid=%" PRIu32 " gid=%" PRIu32 " pid=%" PRIu32
		      " core_id=%" PRIu32 " source_id=%" PRIu16 " source_unique=%" PRIu64 "\n",
		      fuse_io->hdr.opcode, spdk_fuse_dispatcher_get_operation_name(fuse_io->hdr.opcode),
		      fuse_io->hdr.len, fuse_io->hdr.unique, fuse_io->hdr.nodeid, fuse_io->hdr.uid,
		      fuse_io->hdr.gid, fuse_io->hdr.pid, spdk_env_get_current_core(),
		      fuse_io->source_id, fuse_io->source_unique);

	return fuse_dispatcher_submit_io(fuse_io);

exit:
	return -EINVAL;
}

static bool
fuse_dispatcher_create_rmem(struct spdk_fuse_dispatcher *disp, char *rmem_pool_name)
{
	disp->rmem_pool = spdk_rmem_pool_create(rmem_pool_name, sizeof(struct fuse_disp_recovery_data),
						1, 1);
	if (!disp->rmem_pool) {
		SPDK_ERRLOG("%s: failed to create rmem pool\n", rmem_pool_name);
		return false;
	}

	disp->rmem_data = spdk_rmem_pool_get(disp->rmem_pool);
	if (!disp->rmem_data) {
		SPDK_ERRLOG("%s: failed to get rmem_data\n", rmem_pool_name);
		spdk_rmem_pool_destroy(disp->rmem_pool);
		return false;
	}

	/* Save the initial state */
	fuse_dispatcher_update_rmem(disp);

	SPDK_NOTICELOG("%s: rmem pool created succesfully\n", rmem_pool_name);
	return true;
}

static int
fuse_dispatcher_rmem_restore_block_cb(struct spdk_rmem_entry *entry, void *ctx)
{
	struct fuse_disp_recovery_data data;
	struct spdk_fuse_dispatcher *disp = ctx;
	int rc;

	if (disp->rmem_data) {
		SPDK_ERRLOG("%s: data has already been restored. Duplicated entry?\n",
			    fuse_dispatcher_name(disp));
		return -EIO;
	}

	rc = spdk_rmem_entry_read(entry, &data);
	if (rc) {
		SPDK_ERRLOG("%s: failed to read restored entry (err=%d)\n",
			    fuse_dispatcher_name(disp), rc);
		return rc;
	}

	disp->rmem_data = entry;
	disp->proto_major = data.proto_major;
	disp->proto_minor = data.proto_minor;
	disp->root_fobject = (struct spdk_fsdev_file_object *)(uintptr_t)data.root_fobject;

	SPDK_NOTICELOG("%s: data restored: proto_major=%u proto_minor=%u root_fobject=0x%p\n",
		       fuse_dispatcher_name(disp), disp->proto_major, disp->proto_minor, disp->root_fobject);

	return 0;
}

static bool
fuse_dispatcher_recover_rmem(struct spdk_fuse_dispatcher *disp, char *rmem_pool_name)
{
	disp->rmem_pool = spdk_rmem_pool_restore(rmem_pool_name, sizeof(struct fuse_disp_recovery_data),
			  fuse_dispatcher_rmem_restore_block_cb, disp);
	if (!disp->rmem_pool) {
		SPDK_ERRLOG("%s: failed to restore rmem pool\n", rmem_pool_name);
		return false;
	}

	SPDK_NOTICELOG("%s: rmem pool restored successfully\n", rmem_pool_name);
	return true;
}

static bool
fuse_dispatcher_init_rmem(struct spdk_fuse_dispatcher *disp, bool recovery_mode)
{
	bool res = false;
	char *rmem_pool_name;

	rmem_pool_name = spdk_sprintf_alloc("fuse_disp_%s", fuse_dispatcher_name(disp));
	if (!rmem_pool_name) {
		SPDK_ERRLOG("could not allocate pool name for %s\n", fuse_dispatcher_name(disp));
		return false;
	}

	res = recovery_mode ? fuse_dispatcher_recover_rmem(disp, rmem_pool_name) :
	      fuse_dispatcher_create_rmem(disp, rmem_pool_name);

	free(rmem_pool_name);

	return res;
}

struct spdk_fuse_dispatcher *
spdk_fuse_dispatcher_create(struct spdk_fsdev_desc *desc, bool recovery_mode,
			    spdk_fuse_dispatcher_notify_reply_cb notify_reply_cb,
			    void *notify_reply_cb_arg)
{
	struct spdk_fuse_dispatcher *disp;
	struct spdk_fsdev *fsdev;

	disp = calloc(1, sizeof(*disp));
	if (!disp) {
		SPDK_ERRLOG("could not allocate disp\n");
		return NULL;
	}

	disp->fuse_arch = SPDK_FUSE_ARCH_NATIVE;
	fsdev = spdk_fsdev_desc_get_fsdev(desc);
	disp->supported_fuse_opcodes = spdk_fsdev_get_supported_fuse_opcodes(fsdev);
	disp->desc = desc;
	disp->notify_reply_cb = notify_reply_cb;
	disp->notify_reply_cb_arg = notify_reply_cb_arg;
	disp->use_readdir_simple = 1;

	if (!fuse_dispatcher_init_rmem(disp, recovery_mode)) {
		SPDK_ERRLOG("could not create or restore rmem pool for %s\n", fuse_dispatcher_name(disp));
		free(disp);
		return NULL;
	}

	return disp;
}

int
spdk_fuse_dispatcher_set_arch(struct spdk_fuse_dispatcher *disp, enum spdk_fuse_arch fuse_arch)
{
	switch (fuse_arch) {
	case SPDK_FUSE_ARCH_NATIVE:
	case SPDK_FUSE_ARCH_X86:
	case SPDK_FUSE_ARCH_X86_64:
	case SPDK_FUSE_ARCH_ARM:
	case SPDK_FUSE_ARCH_ARM64:
		SPDK_NOTICELOG("FUSE arch set to %d\n", fuse_arch);
		disp->fuse_arch = fuse_arch;
		return 0;
	default:
		return -EINVAL;
	}
}

size_t
spdk_fuse_dispatcher_get_io_ctx_size(void)
{
	return ALIGNED_FUSE_IO_SIZE + spdk_fsdev_get_io_ctx_size();
}

int
spdk_fuse_dispatcher_submit_request(struct spdk_fuse_dispatcher *disp,
				    struct spdk_io_channel *ch,
				    struct iovec *in_iov, int in_iovcnt,
				    struct iovec *out_iov, int out_iovcnt, void *io_ctx,
				    uint16_t source_id, uint64_t source_unique,
				    spdk_fuse_dispatcher_submit_cpl_cb clb, void *cb_arg)
{
	struct fuse_io *fuse_io = (struct fuse_io *) io_ctx;

	if (!fuse_io) {
		SPDK_ERRLOG("Invalid argument, fuse_io is NULL\n");
		return -ENOBUFS;
	}

	fuse_io->disp = disp;
	fuse_io->ch = ch;
	fuse_io->in_iov = in_iov;
	fuse_io->in_iovcnt = in_iovcnt;
	fuse_io->out_iov = out_iov;
	fuse_io->out_iovcnt = out_iovcnt;
	fuse_io->cpl_cb = clb;
	fuse_io->cpl_cb_arg = cb_arg;

	fuse_io->in_offs.iov_offs = 0;
	fuse_io->in_offs.buf_offs = 0;
	fuse_io->out_offs.iov_offs = 0;
	fuse_io->out_offs.buf_offs = 0;

	fuse_io->source_id = source_id;
	fuse_io->source_unique = source_unique;

	return fuse_dispatcher_handle_fuse_req(disp, fuse_io);
}

void
spdk_fuse_dispatcher_delete(struct spdk_fuse_dispatcher *disp)
{
	if (disp->rmem_data) {
		assert(disp->rmem_pool != NULL);
		spdk_rmem_entry_release(disp->rmem_data);
		spdk_rmem_pool_destroy(disp->rmem_pool);
	}
	free(disp);
}

static int
fuse_dispatcher_encode_notify_inval_inode(struct spdk_fuse_dispatcher *disp,
		struct fuse_out_header *out_hdr,
		size_t buf_size,
		const struct spdk_fsdev_notify_data *notify_data)
{
	struct fuse_notify_inval_inode_out *inval_inode;

	out_hdr->error = fsdev_io_d2h_i32(disp, FUSE_NOTIFY_INVAL_INODE);
	out_hdr->len = fsdev_io_d2h_u32(disp,
					sizeof(struct fuse_out_header) + sizeof(struct fuse_notify_inval_inode_out));

	if (out_hdr->len > buf_size) {
		SPDK_ERRLOG("Buffer is too small for notification, buf_size %lu, notify_size %d\n",
			    buf_size, out_hdr->len);
		return -ENOMEM;
	}

	inval_inode = (struct fuse_notify_inval_inode_out *)(out_hdr + 1);
	inval_inode->ino = fsdev_io_d2h_u64(disp, file_ino(disp, notify_data->inval_data.fobject));
	inval_inode->off = fsdev_io_d2h_u64(disp, notify_data->inval_data.offset);
	inval_inode->len = fsdev_io_d2h_u64(disp, notify_data->inval_data.size);
	return 0;
}

static int
fuse_dispatcher_encode_notify_inval_entry(struct spdk_fuse_dispatcher *disp,
		struct fuse_out_header *out_hdr,
		size_t buf_size,
		const struct spdk_fsdev_notify_data *notify_data)
{
	struct fuse_notify_inval_entry_out *inval_entry;
	char *name;
	size_t namelen;

	namelen = strlen(notify_data->inval_entry.name);
	out_hdr->error = fsdev_io_d2h_i32(disp, FUSE_NOTIFY_INVAL_ENTRY);
	out_hdr->len = fsdev_io_d2h_u32(disp,
					sizeof(struct fuse_out_header) + sizeof(struct fuse_notify_inval_entry_out) + namelen);

	if (out_hdr->len > buf_size) {
		SPDK_ERRLOG("Buffer is too small for notification, buf_size %lu, notify_size %d\n",
			    buf_size, out_hdr->len);
		return -ENOMEM;
	}

	inval_entry = (struct fuse_notify_inval_entry_out *)(out_hdr + 1);
	inval_entry->parent =
		fsdev_io_d2h_u64(disp, file_ino(disp, notify_data->inval_entry.parent_fobject));
	inval_entry->namelen = fsdev_io_d2h_u32(disp, namelen);
	name = (char *)(out_hdr + 1) + sizeof(*inval_entry);
	memcpy(name, notify_data->inval_entry.name, namelen);
	return 0;
}

int
spdk_fuse_dispatcher_encode_notify(struct spdk_fuse_dispatcher *disp,
				   struct iovec *iov, int iovcnt,
				   const struct spdk_fsdev_notify_data *notify_data,
				   uint64_t unique_id,
				   bool *has_reply)
{
	struct fuse_out_header *out_hdr;
	size_t buf_size;
	bool tmp_has_reply = false;
	int i;
	int rc = 0;

	for (i = 0, buf_size = 0; i < iovcnt; buf_size += iov[i].iov_len, ++i);
	assert(buf_size >= sizeof(struct fuse_out_header));
	out_hdr = malloc(buf_size);
	if (!out_hdr) {
		SPDK_ERRLOG("Failed to allocate bounce buffer for fuse notification, buf_size %lu\n", buf_size);
		return -ENOMEM;
	}

	if (notify_data) {
		out_hdr->unique = fsdev_io_d2h_u64(disp, unique_id);
		switch (notify_data->type) {
		case SPDK_FSDEV_NOTIFY_INVAL_DATA:
			rc = fuse_dispatcher_encode_notify_inval_inode(disp, out_hdr, buf_size, notify_data);
			tmp_has_reply = true;
			break;
		case SPDK_FSDEV_NOTIFY_INVAL_ENTRY:
			rc = fuse_dispatcher_encode_notify_inval_entry(disp, out_hdr, buf_size, notify_data);
			tmp_has_reply = true;
			break;
		default:
			SPDK_ERRLOG("Unsupported notify type %d\n", notify_data->type);
			rc = -EINVAL;
			break;
		}
	} else {
		/* error and unique set to zero indicate device reset to driver */
		out_hdr->len = fsdev_io_d2h_u32(disp, sizeof(*out_hdr));
		out_hdr->error = 0;
		out_hdr->unique = 0;
	}

	if (rc == 0) {
		*has_reply = tmp_has_reply;
		spdk_copy_buf_to_iovs(iov, iovcnt, out_hdr, out_hdr->len);
	}

	free(out_hdr);
	return rc;
}

uint32_t
spdk_fuse_dispatcher_get_notify_buf_size(struct spdk_fuse_dispatcher *disp)
{
	const uint32_t max_header_size = sizeof(struct fuse_out_header) +
					 sizeof(struct fuse_notify_retrieve_out);
	uint32_t buf_size = spdk_fsdev_get_notify_max_data_size(spdk_fsdev_desc_get_fsdev(disp->desc));

	if (buf_size) {
		buf_size += max_header_size;
	}

	return buf_size;
}

SPDK_LOG_REGISTER_COMPONENT(fuse_dispatcher)
