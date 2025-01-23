#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) 2022-2025 NVIDIA CORPORATION & AFFILIATES.
#  All rights reserved.

from spdk.rpc.helpers import deprecated_alias


def mlx5_scan_accel_module(client, qp_size=None, cq_size=None, num_requests=None, crypto_split_blocks=None, allowed_devs=None,
                           merge=None, qp_per_domain=None, enable_driver=None, enable_module=None,
                           disable_signature=None, disable_crypto=None):
    """Configure mlx5 accel module. Scans all mlx5 devices which can perform needed operations

    Args:
        qp_size: Qpair size. (optional)
        cq_size: CQ size. (optional)
        num_requests: size of the shared requests pool (optional)
        crypto_split_blocks: number of data blocks to be processed in 1 UMR (optional)
        allowed_devs: comma separated list of allowed device names (optional)
        merge: merge tasks in the sequence when possible (optional)
        qp_per_domain: use dedicated qpair per memory domain per channel (optional)
        enable_driver: enable accel mlx5 platform driver (optional)
        enable_module: enable accel mlx5 module (optional)
        disable_signature: disable signature operations support (optional)
        disable_crypto: disable crypto operations support (optional)
    """
    params = {}

    if qp_size is not None:
        params['qp_size'] = qp_size
    if cq_size is not None:
        params['cq_size'] = cq_size
    if num_requests is not None:
        params['num_requests'] = num_requests
    if crypto_split_blocks is not None:
        params['crypto_split_blocks'] = crypto_split_blocks
    if allowed_devs is not None:
        params['allowed_devs'] = allowed_devs
    if merge is not None:
        print("WARNING: merge param is deprecated and will be removed soon, use --enable-driver instead.")
        params['enable_driver'] = merge
    if qp_per_domain is not None:
        params['qp_per_domain'] = qp_per_domain
    if enable_driver is not None:
        params['enable_driver'] = enable_driver
    if enable_module is not None:
        params['enable_module'] = enable_module
    if disable_signature is not None:
        params['disable_signature'] = disable_signature
    if disable_crypto is not None:
        params['disable_crypto'] = disable_crypto
    return client.call('mlx5_scan_accel_module', params)


def accel_mlx5_dump_stats(client, level=None):

    params = {}

    if level is not None:
        params['level'] = level
    return client.call('accel_mlx5_dump_stats', params)
