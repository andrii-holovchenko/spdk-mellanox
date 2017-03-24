#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$testdir/../../..
source $rootdir/scripts/autotest_common.sh

DB_BENCH_DIR=/usr/src/rocksdb
DB_BENCH=$DB_BENCH_DIR/db_bench
ROCKSDB_CONF=$testdir/rocksdb.conf

if [ ! -e $DB_BENCH_DIR ]; then
	echo $DB_BENCH_DIR does not exist, skipping rocksdb tests
	exit 0
fi

timing_enter rocksdb

timing_enter db_bench_build

pushd $DB_BENCH_DIR
$MAKE db_bench $MAKEFLAGS $MAKECONFIG DEBUG_LEVEL=0 SPDK_DIR=$rootdir DPDK_DIR=$DPDK_DIR
popd

timing_exit db_bench_build

cp $rootdir/etc/spdk/rocksdb.conf.in $ROCKSDB_CONF
$rootdir/scripts/gen_nvme.sh >> $ROCKSDB_CONF

$rootdir/test/lib/blobfs/mkfs/mkfs $ROCKSDB_CONF Nvme0n1
USE_PERF=0 DURATION=30 NUM_KEYS=50000000 ROCKSDB_CONF=$ROCKSDB_CONF CACHE_SIZE=1024 $testdir/run_tests.sh $DB_BENCH
mkdir $output_dir/rocksdb
cp $testdir/results/last/* $output_dir/rocksdb

trap 'rm -f $ROCKSDB_CONF; exit 1' SIGINT SIGTERM EXIT

trap - SIGINT SIGTERM EXIT

rm -f $ROCKSDB_CONF

timing_exit rocksdb
