/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk_internal/cunit.h"

#include "common/lib/ut_multithread.c"
#define UT_NUM_THREADS 3
#include "common/lib/ut_call.c"
#include "unit/lib/json_mock.c"

#include "spdk/config.h"

#include "spdk/log.h"
#include "spdk/fsdev.h"
#include "spdk/fuse_dispatcher.h"
#include "spdk/rmem.h"
#include "linux/fuse_kernel.h"

#include "lib/fuse_dispatcher/fuse_dispatcher.c"

#define UT_UNIQUE 0xBEADBEAD
#define UT_FSDEV_NAME "utfsdev0"
#define UT_FOBJECT ((struct spdk_fsdev_file_object *)0xDEADDEAD)
#define UT_FHANDLE ((struct spdk_fsdev_file_handle *)0xBEABBEAB)
#define UT_FNAME "ut_test.file"
#define UT_IO_BUFF_SIZE 111

#define UT_RMEM_POOL ((struct spdk_rmem_pool *)0xBEADBEAD)
#define UT_RMEM_POOL_ENTRY ((struct spdk_rmem_entry *)0xDEABDEAB)

DEFINE_STUB_V(spdk_fsdev_close, (struct spdk_fsdev_desc *desc));
DEFINE_STUB(spdk_fsdev_get_supported_fuse_opcodes, uint64_t, (struct spdk_fsdev *fsdev), 0);
DEFINE_STUB(spdk_fsdev_desc_get_fsdev, struct spdk_fsdev *, (struct spdk_fsdev_desc *desc), NULL);
DEFINE_STUB(spdk_fsdev_get_opts, int, (struct spdk_fsdev_opts *opts, size_t opts_size), 0);
DEFINE_STUB(spdk_fsdev_reset, int, (struct spdk_fsdev_desc *desc, spdk_fsdev_reset_completion_cb cb,
				    void *cb_arg), 0);
DEFINE_STUB(spdk_fsdev_reset_supported, bool, (struct spdk_fsdev *fsdev), true);
DEFINE_STUB(spdk_fsdev_get_notify_max_data_size, uint32_t, (const struct spdk_fsdev *fsdev), 0);
DEFINE_STUB(spdk_fsdev_get_name, const char *, (const struct spdk_fsdev *fsdev), NULL);
DEFINE_STUB(spdk_rmem_get_backend_dir, const char *, (void), NULL);
DEFINE_STUB(spdk_rmem_set_backend_dir, int, (const char *backend_dir_name), 0);
DEFINE_STUB(spdk_rmem_pool_create, struct spdk_rmem_pool *, (const char *name, uint32_t entry_size,
		uint32_t num_entries, uint32_t ext_num_entries), UT_RMEM_POOL);
DEFINE_STUB(spdk_rmem_pool_restore, struct spdk_rmem_pool *, (const char *name, uint32_t entry_size,
		spdk_rmem_pool_restore_entry_cb clb, void *ctx), NULL);
DEFINE_STUB(spdk_rmem_pool_get, struct spdk_rmem_entry *, (struct spdk_rmem_pool *pool),
	    UT_RMEM_POOL_ENTRY);
DEFINE_STUB_V(spdk_rmem_entry_write, (struct spdk_rmem_entry *entry, const void *buf));
DEFINE_STUB(spdk_rmem_entry_read, int, (struct spdk_rmem_entry *entry, void *buf), 0);
DEFINE_STUB_V(spdk_rmem_entry_release, (struct spdk_rmem_entry *entry));
DEFINE_STUB_V(spdk_rmem_pool_destroy, (struct spdk_rmem_pool *pool));

static struct spdk_fsdev_desc *g_ut_fsdev_desc = (struct spdk_fsdev_desc *)0xBEADFEAD;

void
spdk_fsdev_io_init(struct spdk_fsdev_io *fsdev_io, struct spdk_fsdev_desc *desc,
		   struct spdk_io_channel *ch,
		   uint64_t unique, enum spdk_fsdev_io_type type,
		   uint16_t source_id, uint64_t source_unique,
		   spdk_fsdev_cpl_cb *cb_fn, void *cb_arg)
{
	ut_call_record_begin(spdk_fsdev_io_init);
	ut_call_record_param_ptr(fsdev_io);
	ut_call_record_param_ptr(desc);
	ut_call_record_param_ptr(ch);
	ut_call_record_param_int(unique);
	ut_call_record_param_int(type);
	ut_call_record_param_int(source_id);
	ut_call_record_param_int(source_unique);
	ut_call_record_param_ptr(cb_fn);
	ut_call_record_param_ptr(cb_arg);
	ut_call_record_end();
}

void
spdk_fsdev_io_submit(struct spdk_fsdev_io *fsdev_io)
{
	ut_call_record_begin(spdk_fsdev_io_submit);
	ut_call_record_param_ptr(fsdev_io);
	ut_call_record_end();
}

int
spdk_fsdev_get_io_ctx_size(void)
{
	ut_call_record_begin(spdk_fsdev_get_io_ctx_size);
	ut_call_record_end();
	return sizeof(struct spdk_fsdev_io) +  UT_IO_BUFF_SIZE;
}

static void
request_cb(void *cb_arg, int error)
{
	ut_call_record_begin(request_cb);
	ut_call_record_param_ptr(cb_arg);
	ut_call_record_param_int(error);
	ut_call_record_end();
}

static void
notify_reply_cb(void *cb_arg,
		const struct spdk_fsdev_notify_reply_data *notify_reply_data,
		uint64_t unique_id)
{
	ut_call_record_begin(notify_reply_cb);
	ut_call_record_param_ptr(cb_arg);
	ut_call_record_param_hash(notify_reply_data, sizeof(*notify_reply_data));
	ut_call_record_param_int(unique_id);
	ut_call_record_end();
}

static void
ut_fuse_disp_test_create_delete(void)
{
	struct spdk_fuse_dispatcher *disp;

	disp = spdk_fuse_dispatcher_create(g_ut_fsdev_desc, false, notify_reply_cb, NULL);
	CU_ASSERT(disp != NULL);

	spdk_fuse_dispatcher_delete(disp);
}

struct fuse_in {
	struct fuse_in_header hdr;
	union {
		struct fuse_init_in init;
		struct fuse_notify_reply_in notify_reply;
	};
};

struct fuse_out {
	struct fuse_out_header hdr;
	union {
		struct fuse_init_out init;
		struct fuse_notify_poll_wakeup_out poll;
		struct fuse_notify_inval_inode_out inval_inode;
		struct {
			struct fuse_notify_inval_entry_out fuse;
			char name[UT_CALL_REC_MAX_STR_SIZE];
		} inval_entry;
		struct fuse_notify_delete_out delete;
		struct fuse_notify_store_out store;
		struct fuse_notify_retrieve_out retrieve;
	};
};

static void
ut_fuse_disp_test_init_destroy(void)
{
	struct spdk_fuse_dispatcher *disp;
	const size_t io_ctx_size = spdk_fuse_dispatcher_get_io_ctx_size();
	uint8_t io_ctx[io_ctx_size];
	struct spdk_io_channel *io_channel = (struct spdk_io_channel *)0x12345678;
	int request_cb_arg;
	struct fuse_in init_in;
	struct iovec in_iov = { .iov_base = &init_in };
	struct fuse_out init_out;
	struct iovec out_iov = { .iov_base = &init_out };
	spdk_fsdev_cpl_cb *mount_cb_fn;
	spdk_fsdev_cpl_cb *umount_cb_fn;
	struct spdk_fsdev_io *fsdev_io;
	void *cb_arg;
	int rc;

	disp = spdk_fuse_dispatcher_create(g_ut_fsdev_desc, false, notify_reply_cb, NULL);
	CU_ASSERT(disp != NULL);

	/* FUSE_INIT 7.34 */
	ut_calls_reset();
	memset(&init_in, 0, sizeof(init_in));
	init_in.hdr.len = sizeof(init_in.hdr) + sizeof(init_in.init);
	init_in.hdr.opcode = FUSE_INIT;
	init_in.hdr.unique = 1;
	init_in.init.major = 7;
	init_in.init.minor = 34;
	init_in.init.max_readahead = 16384;
	init_in.init.flags = FUSE_ASYNC_READ | FUSE_POSIX_LOCKS | FUSE_MAX_PAGES |
			     FUSE_EXPORT_SUPPORT | FUSE_AUTO_INVAL_DATA | FUSE_WRITEBACK_CACHE | FUSE_POSIX_ACL;
	in_iov.iov_len = init_in.hdr.len;
	out_iov.iov_len = sizeof(init_out.hdr) + sizeof(init_out.init);
	rc = spdk_fuse_dispatcher_submit_request(disp, io_channel, &in_iov, 1, &out_iov, 1, io_ctx,
			100, 1000, request_cb, &request_cb_arg);
	CU_ASSERT(rc == 0);
	CU_ASSERT(ut_calls_get_call_count() == 2);

	CU_ASSERT(ut_calls_get_func(0) == spdk_fsdev_io_init);
	fsdev_io = ut_calls_param_get_ptr(0, 0);
	CU_ASSERT((uintptr_t)fsdev_io > (uintptr_t)io_ctx);
	CU_ASSERT((uintptr_t)fsdev_io - (uintptr_t)io_ctx < io_ctx_size);
	CU_ASSERT(ut_calls_param_get_ptr(0, 1) == g_ut_fsdev_desc);
	CU_ASSERT(ut_calls_param_get_ptr(0, 2) == io_channel);
	CU_ASSERT(ut_calls_param_get_int(0, 3) == 1); /* unique */
	CU_ASSERT(ut_calls_param_get_int(0, 4) == SPDK_FSDEV_IO_MOUNT); /* type */
	CU_ASSERT(ut_calls_param_get_int(0, 5) == 100); /* source_id */
	CU_ASSERT(ut_calls_param_get_int(0, 6) == 1000); /* source_unique */
	mount_cb_fn = ut_calls_param_get_ptr(0, 7);
	cb_arg = ut_calls_param_get_ptr(0, 8);

	CU_ASSERT(ut_calls_get_func(1) == spdk_fsdev_io_submit);
	CU_ASSERT(ut_calls_param_get_ptr(0, 0) == fsdev_io);

	ut_calls_reset();
	fsdev_io->u_out.mount.opts.opts_size = sizeof(fsdev_io->u_out.mount.opts);
	fsdev_io->u_out.mount.opts.max_xfer_size = 131072;
	fsdev_io->u_out.mount.opts.max_readahead = 16384;
	/* Unset SPDK_FSDEV_MOUNT_POSIX_ACL */
	fsdev_io->u_out.mount.opts.flags = SPDK_FSDEV_MOUNT_DOT_PATH_LOOKUP |
					   SPDK_FSDEV_MOUNT_AUTO_INVAL_DATA |
					   SPDK_FSDEV_MOUNT_WRITEBACK_CACHE | SPDK_FSDEV_MOUNT_POSIX_LOCKS;

	mount_cb_fn(cb_arg, 0, fsdev_io);
	CU_ASSERT(ut_calls_get_func(0) == request_cb);
	CU_ASSERT(ut_calls_param_get_ptr(0, 0) == &request_cb_arg);
	CU_ASSERT(ut_calls_param_get_int(0, 1) == 0);
	CU_ASSERT(init_out.hdr.len == sizeof(init_out.hdr) + sizeof(init_out.init));
	CU_ASSERT(init_out.hdr.error == 0);
	CU_ASSERT(init_out.hdr.unique == 1);
	CU_ASSERT(init_out.init.major == 7);
	CU_ASSERT(init_out.init.minor == 34);
	CU_ASSERT(init_out.init.max_readahead == 16384);
	CU_ASSERT(init_out.init.flags == (FUSE_ASYNC_READ | FUSE_POSIX_LOCKS | FUSE_MAX_PAGES |
					  FUSE_EXPORT_SUPPORT | FUSE_AUTO_INVAL_DATA |
					  FUSE_WRITEBACK_CACHE));
	CU_ASSERT(init_out.init.max_background == 0xFFFF);
	CU_ASSERT(init_out.init.congestion_threshold == 0xFFFF);
	CU_ASSERT(init_out.init.max_write == 131072);
	CU_ASSERT(init_out.init.time_gran == 1);
	CU_ASSERT(init_out.init.max_pages == 131072 / 4096);
	CU_ASSERT(init_out.init.map_alignment == 0);

	/* FUSE_DESTROY */
	ut_calls_reset();
	memset(&init_in, 0, sizeof(init_in));
	init_in.hdr.len = sizeof(init_in.hdr);
	init_in.hdr.opcode = FUSE_DESTROY;
	init_in.hdr.unique = 2;
	in_iov.iov_len = init_in.hdr.len;
	out_iov.iov_len = sizeof(init_out.hdr);
	rc = spdk_fuse_dispatcher_submit_request(disp, io_channel, &in_iov, 1, &out_iov, 1, io_ctx,
			20, 200, request_cb, &request_cb_arg);
	CU_ASSERT(rc == 0);

	CU_ASSERT(rc == 0);
	CU_ASSERT(ut_calls_get_call_count() == 2);

	CU_ASSERT(ut_calls_get_func(0) == spdk_fsdev_io_init);
	CU_ASSERT(ut_calls_param_get_ptr(0, 0) == fsdev_io);
	CU_ASSERT(ut_calls_param_get_ptr(0, 1) == g_ut_fsdev_desc);
	CU_ASSERT(ut_calls_param_get_ptr(0, 2) == io_channel);
	CU_ASSERT(ut_calls_param_get_int(0, 3) == 2); /* unique */
	CU_ASSERT(ut_calls_param_get_int(0, 4) == SPDK_FSDEV_IO_UMOUNT); /* type */
	CU_ASSERT(ut_calls_param_get_int(0, 5) == 20); /* source_id */
	CU_ASSERT(ut_calls_param_get_int(0, 6) == 200); /* source_unique */
	umount_cb_fn = ut_calls_param_get_ptr(0, 7);
	cb_arg = ut_calls_param_get_ptr(0, 8);

	CU_ASSERT(ut_calls_get_func(1) == spdk_fsdev_io_submit);
	CU_ASSERT(ut_calls_param_get_ptr(0, 0) == fsdev_io);

	ut_calls_reset();
	umount_cb_fn(cb_arg, 0, fsdev_io);
	CU_ASSERT(ut_calls_get_func(0) == request_cb);
	CU_ASSERT(ut_calls_param_get_ptr(0, 0) == &request_cb_arg);
	CU_ASSERT(ut_calls_param_get_int(0, 1) == 0);
	CU_ASSERT(init_out.hdr.len == sizeof(init_out.hdr));
	CU_ASSERT(init_out.hdr.error == 0);
	CU_ASSERT(init_out.hdr.unique == 2);

	spdk_fuse_dispatcher_delete(disp);
}

static void
ut_fuse_disp_test_encode_notify(void)
{
	struct spdk_fuse_dispatcher *disp;
	struct spdk_fsdev_notify_data fsdev_notify_data;
	struct fuse_out fuse_notify_data;
	struct iovec iov = { .iov_base = &fuse_notify_data, .iov_len = sizeof(fuse_notify_data) };
	bool has_reply;
	int rc;

	disp = spdk_fuse_dispatcher_create(g_ut_fsdev_desc, false, notify_reply_cb, NULL);
	CU_ASSERT(disp != NULL);

	/* INVAL INODE notification */
	memset(&fsdev_notify_data, 0, sizeof(fsdev_notify_data));
	fsdev_notify_data.type = SPDK_FSDEV_NOTIFY_INVAL_DATA;
	fsdev_notify_data.inval_data.fobject = UT_FOBJECT;
	fsdev_notify_data.inval_data.offset = 100500;
	fsdev_notify_data.inval_data.size = 4096;
	rc = spdk_fuse_dispatcher_encode_notify(disp, &iov, 1, &fsdev_notify_data, 1, &has_reply);
	CU_ASSERT(rc == 0);
	CU_ASSERT(fuse_notify_data.hdr.len == sizeof(fuse_notify_data.hdr) + sizeof(
			  fuse_notify_data.inval_inode));
	CU_ASSERT(fuse_notify_data.hdr.error == FUSE_NOTIFY_INVAL_INODE);
	CU_ASSERT(fuse_notify_data.hdr.unique == 1);
	CU_ASSERT(fuse_notify_data.inval_inode.ino == (uint64_t)(uintptr_t)UT_FOBJECT);
	CU_ASSERT(fuse_notify_data.inval_inode.off == 100500);
	CU_ASSERT(fuse_notify_data.inval_inode.len == 4096);
	CU_ASSERT(has_reply == true);

	/* INVAL ENTRY notification */
	memset(&fsdev_notify_data, 0, sizeof(fsdev_notify_data));
	fsdev_notify_data.type = SPDK_FSDEV_NOTIFY_INVAL_ENTRY;
	fsdev_notify_data.inval_entry.parent_fobject = UT_FOBJECT;
	fsdev_notify_data.inval_entry.name = UT_FNAME;
	rc = spdk_fuse_dispatcher_encode_notify(disp, &iov, 1, &fsdev_notify_data, 2, &has_reply);
	CU_ASSERT(rc == 0);
	CU_ASSERT(fuse_notify_data.hdr.len == sizeof(fuse_notify_data.hdr) + sizeof(
			  fuse_notify_data.inval_entry.fuse) +
		  strlen(UT_FNAME));
	CU_ASSERT(fuse_notify_data.hdr.error == FUSE_NOTIFY_INVAL_ENTRY);
	CU_ASSERT(fuse_notify_data.hdr.unique == 2);
	CU_ASSERT(fuse_notify_data.inval_entry.fuse.parent == (uint64_t)(uintptr_t)UT_FOBJECT);
	CU_ASSERT(fuse_notify_data.inval_entry.fuse.namelen == strlen(UT_FNAME));
	CU_ASSERT(memcmp(fuse_notify_data.inval_entry.name, UT_FNAME, strlen(UT_FNAME)) == 0);
	CU_ASSERT(has_reply == true);

	/* "Empty" notification */
	rc = spdk_fuse_dispatcher_encode_notify(disp, &iov, 1, NULL, 3, &has_reply);
	CU_ASSERT(rc == 0);
	CU_ASSERT(fuse_notify_data.hdr.len == sizeof(fuse_notify_data.hdr));
	CU_ASSERT(fuse_notify_data.hdr.error == 0);
	CU_ASSERT(fuse_notify_data.hdr.unique == 0);
	CU_ASSERT(has_reply == false);

	spdk_fuse_dispatcher_delete(disp);
}

static void
ut_fuse_disp_test_notify_reply(void)
{
	struct spdk_fuse_dispatcher *disp;
	const size_t io_ctx_size = spdk_fuse_dispatcher_get_io_ctx_size();
	uint8_t io_ctx[io_ctx_size];
	struct spdk_io_channel *io_channel = (struct spdk_io_channel *)0x12345678;
	int request_cb_arg;
	int notify_cb_arg;
	struct spdk_fsdev_notify_reply_data fsdev_notify_reply_data;
	struct iovec in_iov;
	struct fuse_in notify_reply;
	int rc;

	disp = spdk_fuse_dispatcher_create(g_ut_fsdev_desc, false, notify_reply_cb, &notify_cb_arg);
	CU_ASSERT(disp != NULL);

	/* Successful reply */
	ut_calls_reset();
	memset(&notify_reply, 0, sizeof(notify_reply));
	notify_reply.hdr.len = sizeof(notify_reply);
	notify_reply.hdr.opcode = FUSE_NOTIFY_REPLY;
	notify_reply.hdr.unique = 1;
	in_iov.iov_base = &notify_reply;
	in_iov.iov_len = notify_reply.hdr.len;
	rc = spdk_fuse_dispatcher_submit_request(disp, io_channel, &in_iov, 1, NULL, 0, io_ctx,
			0, 0, request_cb, &request_cb_arg);
	CU_ASSERT(rc == 0);
	fsdev_notify_reply_data.status = 0;
	CU_ASSERT(ut_calls_get_func(0) == notify_reply_cb);
	CU_ASSERT(ut_calls_param_get_ptr(0, 0) == &notify_cb_arg);
	CU_ASSERT(ut_calls_param_get_hash(0, 1) ==
		  ut_hash(&fsdev_notify_reply_data, sizeof(fsdev_notify_reply_data)));
	CU_ASSERT(ut_calls_param_get_int(0, 2) == 1);
	CU_ASSERT(ut_calls_get_func(1) == request_cb);
	CU_ASSERT(ut_calls_param_get_ptr(1, 0) == &request_cb_arg);
	CU_ASSERT(ut_calls_param_get_int(1, 1) == 0);

	/* Reply with error */
	ut_calls_reset();
	notify_reply.hdr.unique = 2;
	notify_reply.notify_reply.error = -EINVAL;
	rc = spdk_fuse_dispatcher_submit_request(disp, io_channel, &in_iov, 1, NULL, 0, io_ctx,
			0, 0, request_cb, &request_cb_arg);
	CU_ASSERT(rc == 0);
	fsdev_notify_reply_data.status = -EINVAL;
	CU_ASSERT(ut_calls_get_func(0) == notify_reply_cb);
	CU_ASSERT(ut_calls_param_get_ptr(0, 0) == &notify_cb_arg);
	CU_ASSERT(ut_calls_param_get_hash(0, 1) ==
		  ut_hash(&fsdev_notify_reply_data, sizeof(fsdev_notify_reply_data)));
	CU_ASSERT(ut_calls_param_get_int(0, 2) == 2);
	CU_ASSERT(ut_calls_get_func(1) == request_cb);
	CU_ASSERT(ut_calls_param_get_ptr(1, 0) == &request_cb_arg);
	CU_ASSERT(ut_calls_param_get_int(1, 1) == 0);

	spdk_fuse_dispatcher_delete(disp);
}

static int
fuse_disp_ut(int argc, char **argv)
{
	CU_pSuite		suite = NULL;
	unsigned int		num_failures;

	suite = CU_add_suite("fuse_dispatcher", NULL, NULL);

	CU_ADD_TEST(suite, ut_fuse_disp_test_create_delete);
	CU_ADD_TEST(suite, ut_fuse_disp_test_init_destroy);
	CU_ADD_TEST(suite, ut_fuse_disp_test_encode_notify);
	CU_ADD_TEST(suite, ut_fuse_disp_test_notify_reply);

	allocate_cores(1);
	allocate_threads(1);
	set_thread(0);

	num_failures = spdk_ut_run_tests(argc, argv, NULL);

	poll_thread(0);

	free_threads();
	free_cores();

	return num_failures;
}

int
main(int argc, char **argv)
{
	unsigned int num_failures;

	CU_initialize_registry();

	num_failures = fuse_disp_ut(argc, argv);

	CU_cleanup_registry();
	return num_failures;
}
