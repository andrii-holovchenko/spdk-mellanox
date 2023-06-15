#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) 2022, NVIDIA CORPORATION & AFFILIATES.
#  All rights reserved.

from spdk.rpc.helpers import deprecated_alias


def mlx5_scan_accel_module(client, qp_size=None, num_requests=None, split_mb_blocks=None, allowed_crypto_devs=None,
                           siglast=None, enable_crc=None, merge=None):
    """Enable mlx5 accel module. Scans all mlx5 devices which can perform needed operations

    Args:
        qp_size: Qpair size. (optional)
        num_requests: size of a global requests pool per mlx5 device (optional)
        enable_crc: enable CRC32C and COPY_CRC32C operations (optional)
        merge: merge tasks in the sequence when possible (optional)
    """
    params = {}

    if qp_size is not None:
        params['qp_size'] = qp_size
    if num_requests is not None:
        params['num_requests'] = num_requests
    if split_mb_blocks is not None:
        params['split_mb_blocks'] = split_mb_blocks
    if allowed_crypto_devs is not None:
        params['allowed_crypto_devs'] = allowed_crypto_devs
    if siglast is not None:
        params['siglast'] = siglast
    if enable_crc is not None:
        params['enable_crc'] = enable_crc
    if merge is not None:
        params['merge'] = merge
    return client.call('mlx5_scan_accel_module', params)
