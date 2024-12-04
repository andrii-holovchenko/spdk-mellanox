#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2019 Intel Corporation
#  All rights reserved.
#
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh

# simply check if rpc commands have any effect on spdk
function rpc_integrity() {
	bdevs=$($rpc bdev_get_bdevs)
	[ "$(jq length <<< "$bdevs")" == "0" ]

	malloc=$($rpc bdev_malloc_create 8 512)
	bdevs=$($rpc bdev_get_bdevs)
	[ "$(jq length <<< "$bdevs")" == "1" ]

	$rpc bdev_passthru_create -b "$malloc" -p Passthru0
	bdevs=$($rpc bdev_get_bdevs)
	[ "$(jq length <<< "$bdevs")" == "2" ]

	$rpc bdev_passthru_delete Passthru0
	$rpc bdev_malloc_delete $malloc
	bdevs=$($rpc bdev_get_bdevs)
	[ "$(jq length <<< "$bdevs")" == "0" ]
}

function rpc_plugins() {
	malloc=$($rpc --plugin rpc_plugin create_malloc)
	bdevs=$($rpc bdev_get_bdevs)
	[ "$(jq length <<< "$bdevs")" == "1" ]

	$rpc --plugin rpc_plugin delete_malloc $malloc
	bdevs=$($rpc bdev_get_bdevs)
	[ "$(jq length <<< "$bdevs")" == "0" ]

	# Multiple --plugin options
	$rootdir/scripts/rpc.py --plugin rpc_plugin --plugin scheduler_plugin create_malloc --help
	$rootdir/scripts/rpc.py --plugin rpc_plugin --plugin scheduler_plugin scheduler_thread_create --help

	# Multiple plugins in SPDK_RPC_PLUGIN environment variable
	SPDK_RPC_PLUGIN="rpc_plugin:scheduler_plugin" $rootdir/scripts/rpc.py create_malloc --help
	SPDK_RPC_PLUGIN="rpc_plugin:scheduler_plugin" $rootdir/scripts/rpc.py scheduler_thread_create --help
}

function rpc_trace_cmd_test() {
	local info

	info=$($rpc trace_get_info)
	[ "$(jq length <<< "$info")" -gt 2 ]
	[ "$(jq 'has("tpoint_group_mask")' <<< "$info")" = "true" ]
	[ "$(jq 'has("tpoint_shm_path")' <<< "$info")" = "true" ]
	[ "$(jq 'has("bdev")' <<< "$info")" = "true" ]
	[ "$(jq -r .bdev.tpoint_mask <<< "$info")" != "0x0" ]
}

function go_rpc() {
	bdevs=$($rootdir/build/examples/hello_gorpc)
	[ "$(jq length <<< "$bdevs")" == "0" ]

	malloc=$($rpc bdev_malloc_create 8 512)

	bdevs=$($rootdir/build/examples/hello_gorpc)
	[ "$(jq length <<< "$bdevs")" == "1" ]

	$rpc bdev_malloc_delete $malloc
	bdevs=$($rootdir/build/examples/hello_gorpc)
	[ "$(jq length <<< "$bdevs")" == "0" ]
}

$SPDK_BIN_DIR/spdk_tgt -e bdev &
spdk_pid=$!
trap 'killprocess $spdk_pid; exit 1' SIGINT SIGTERM EXIT
waitforlisten $spdk_pid

export PYTHONPATH=$PYTHONPATH:$testdir

# basic integrity test
rpc=rpc_cmd
run_test "rpc_integrity" rpc_integrity
run_test "rpc_plugins" rpc_plugins
run_test "rpc_trace_cmd_test" rpc_trace_cmd_test
if [[ $SPDK_JSONRPC_GO_CLIENT -eq 1 ]]; then
	run_test "go_rpc" go_rpc
fi
# same integrity test, but with rpc_cmd() instead
rpc="rpc_cmd"
run_test "rpc_daemon_integrity" rpc_integrity

trap - SIGINT SIGTERM EXIT
killprocess $spdk_pid
