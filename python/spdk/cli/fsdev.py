#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2016 Intel Corporation
#  All rights reserved.
#  Copyright (c) 2022 Dell Inc, or its subsidiaries.
#  Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#

import sys
import spdk.rpc as rpc  # noqa
from spdk.rpc.client import print_dict, print_json, print_array  # noqa


def add_parser(subparsers):

    def fsdev_get_opts(args):
        print_json(rpc.fsdev.fsdev_get_opts(args.client))

    p = subparsers.add_parser('fsdev_get_opts', help='Get the fsdev subsystem options')
    p.set_defaults(func=fsdev_get_opts)

    def fsdev_set_opts(args):
        print(rpc.fsdev.fsdev_set_opts(args.client, max_source_id=args.max_source_id,
                                       disable_recovery=args.disable_recovery))

    p = subparsers.add_parser('fsdev_set_opts', help='Set the fsdev subsystem options')
    p.add_argument('-s', '--max-source-id', help='Max source ID (1-4096)',
                   type=int, choices=range(1, 4096), metavar="[1-4096]")
    recovery_parser = p.add_mutually_exclusive_group(required=False)
    recovery_parser.add_argument('--disable-recovery', help='Disable recovery', action='store_true',
                                 default=None)
    recovery_parser.add_argument('--enable-recovery', help='Enable recovery',
                                 dest='disable_recovery', action='store_false')
    p.set_defaults(func=fsdev_set_opts)

    def fsdev_get_fsdevs(args):
        print_dict(rpc.fsdev.fsdev_get_fsdevs(args.client, name=args.name))

    p = subparsers.add_parser('fsdev_get_fsdevs',
                              help='Display current fsdev list or required fsdev')
    p.add_argument('-f', '--name', help="Name of the fsdev. Example: aio0", required=False)
    p.set_defaults(func=fsdev_get_fsdevs)

    def fsdev_get_iostat(args):
        print_json(rpc.fsdev.fsdev_get_iostat(args.client, name=args.name, per_channel=args.per_channel))

    p = subparsers.add_parser('fsdev_get_iostat',
                              help='Display current I/O statistics of all the fsdevs or specified fsdev.')
    p.add_argument('-f', '--name', help="Name of the fsdev. Example: aio0", required=False)
    p.add_argument('-c', '--per-channel', default=False, dest='per_channel', help='Display per channel IO stats for specified device',
                   action='store_true', required=False)
    p.set_defaults(func=fsdev_get_iostat)

    def fsdev_reset_iostat(args):
        print(rpc.fsdev.fsdev_reset_iostat(args.client, name=args.name))

    p = subparsers.add_parser('fsdev_reset_iostat', help='Reset the I/O statictics for all the fsdevs or specified fsdev')
    p.add_argument('-f', '--name', help="Name of the fsdev. Example: aio0", required=False)
    p.set_defaults(func=fsdev_reset_iostat)

    def fsdev_set_delays(args):
        print_json(rpc.fsdev.fsdev_set_delays(args.client, name=args.name, submit=args.submit,
                                              complete=args.complete, complete_99=args.complete_99))

    p = subparsers.add_parser('fsdev_set_delays',
                              help='Set submission and/or completion delays for fsdev I/O')
    p.add_argument('name', help="Name of the fsdev. Example: aio0")
    p.add_argument('-s', '--submit', help='Submission delay in microseconds (default 0: disabled)',
                   required=False, type=int)
    p.add_argument('-c', '--complete', help='Completion delay in microseconds (default 0: disabled)',
                   required=False, type=int)
    p.add_argument('-n', '--complete-99', help='99%% Completion delay in microseconds (default 0: disabled)',
                   required=False, type=int)
    p.set_defaults(func=fsdev_set_delays)

    def fsdev_aio_set_options(args):
        print(rpc.fsdev.fsdev_aio_set_options(args.client, max_io_depth=args.max_io_depth, enable_io_uring=args.enable_io_uring))

    p = subparsers.add_parser('fsdev_aio_set_options', help='Set the aio filesystem options')
    p.add_argument('-m', '--max-io-depth', help='Max IO depth', type=int)
    p.add_argument('-e', '--enable-io-uring', help='Enable IO uring', action='store_true', default=None)
    p.set_defaults(func=fsdev_aio_set_options)

    def fsdev_aio_get_options(args):
        print_json(rpc.fsdev.fsdev_aio_get_options(args.client))

    p = subparsers.add_parser('fsdev_aio_get_options', help='Get the aio filesystem options')
    p.set_defaults(func=fsdev_aio_get_options)

    def fsdev_aio_create(args):
        print(rpc.fsdev.fsdev_aio_create(args.client, name=args.name, root_path=args.root_path,
                                         enable_xattr=args.enable_xattr, enable_writeback_cache=args.enable_writeback_cache,
                                         max_xfer_size=args.max_xfer_size, skip_rw=args.skip_rw,
                                         max_readahead=args.max_readahead,
                                         enable_notifications=args.enable_notifications,
                                         attr_valid_ms=args.attr_valid_ms,
                                         disable_copy_file_range=args.disable_copy_file_range))

    p = subparsers.add_parser('fsdev_aio_create', help='Create a aio filesystem')
    p.add_argument('name', help='Filesystem name. Example: aio0.')
    p.add_argument('root_path', help='Path on the system fs to expose as SPDK filesystem')

    group = p.add_mutually_exclusive_group()
    group.add_argument('--enable-xattr', help='Enable extended attributes', action='store_true', default=None)
    group.add_argument('--disable-xattr', help='Disable extended attributes', dest='enable_xattr', action='store_false', default=None)

    group = p.add_mutually_exclusive_group()
    group.add_argument('--enable-writeback-cache', help='Enable writeback cache', action='store_true', default=None)
    group.add_argument('--disable-writeback-cache', help='Disable writeback cache', dest='enable_writeback_cache', action='store_false',
                       default=None)

    p.add_argument('-w', '--max-xfer-size', help='Max data transfer size in bytes', type=int)
    p.add_argument('-r', '--max-readahead', help='Max readahead size in bytes', type=int)
    p.add_argument('--skip-rw', dest='skip_rw', help="Do not process read or write commands. This is used for testing.",
                   action='store_true', default=None)
    p.add_argument('--enable-notifications', help="Enable notifications.", action='store_true', default=None)
    p.add_argument('-a', '--attr-valid-ms', help='File attributes validity time in miliseconds. Used for entry cache.', type=int)
    p.add_argument('--disable-copy-file-range', help='Disable copy_file_range', action='store_true', default=None)
    p.set_defaults(func=fsdev_aio_create)

    def fsdev_aio_delete(args):
        print(rpc.fsdev.fsdev_aio_delete(args.client, name=args.name))

    p = subparsers.add_parser('fsdev_aio_delete', help='Delete a aio filesystem')
    p.add_argument('name', help='Filesystem name. Example: aio0.')
    p.set_defaults(func=fsdev_aio_delete)
