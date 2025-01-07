#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2020 Intel Corporation
#  All rights reserved.
#
# We don't want to tell kernel to include %e or %E since these
# can include whitespaces or other funny characters, and working
# with those on the cmdline would be a nightmare. Use procfs for
# the remaining pieces we want to gather:
# |$rootdir/scripts/core-collector.sh %P %s %t $output_dir

rootdir=$(readlink -f "$(dirname "$0")/../")

maps_to_json() {
	local _maps=("${maps[@]}")
	local mem_regions=() mem

	mem_regions=("/proc/$core_pid/map_files/"*)

	for mem in "${!mem_regions[@]}"; do
		_maps[mem]=\"${_maps[mem]}@${mem_regions[mem]##*/}\"
	done

	local IFS=","
	echo "${_maps[*]}"
}

core_meta() {
	jq . <<- CORE
		{
		  "$exe_comm": {
		    "ts": "$core_time",
		    "size": "$core_size bytes",
		    "PID": $core_pid,
		    "signal": "$core_sig ($core_sig_name)",
		    "path": "$exe_path",
		    "cwd": "$cwd_path",
		    "statm": "$statm",
		    "filter": "$(coredump_filter)",
		    "mapped": [ $(maps_to_json) ]
		  }
		}
	CORE
}

bt() { hash gdb && gdb -batch -ex "thread apply all bt full" "$1" "$2" 2>&1; }

stderr() {
	exec 2> "$core.stderr.txt"
	set -x
}

coredump_filter() {
	local bitmap bit
	local _filter filter

	bitmap[0]=anon-priv-mappings
	bitmap[1]=anon-shared-mappings
	bitmap[2]=file-priv-mappings
	bitmap[3]=file-shared-mappings
	bitmap[4]=elf-headers
	bitmap[5]=priv-hp
	bitmap[6]=shared-hp
	bitmap[7]=priv-DAX
	bitmap[8]=shared-DAX

	_filter=0x$(< "/proc/$core_pid/coredump_filter")

	for bit in "${!bitmap[@]}"; do
		((_filter & 1 << bit)) || continue
		filter=${filter:+$filter,}${bitmap[bit]}
	done

	echo "$filter"
}

filter_process() {
	local excl_binaries=() bin

	# rmem_pool testers simulate crashes to test recovery, so ignore them
	excl_binaries+=("rmem_pool_test")
	excl_binaries+=("rmem_pool_write_crash_test*")
	# Add more if needed

	for bin in "${excl_binaries[@]}"; do
		# The below SC is intentional
		# shellcheck disable=SC2053
		[[ ${exe_path##*/} == $bin ]] && return 1
	done

	# Did the process sit in our repo?
	[[ $cwd_path == "$rootdir"* ]] && return 0

	# Did we load our fio plugins?
	[[ ${maps[*]} == *"$rootdir/build/fio/spdk_nvme"* ]] && return 0
	[[ ${maps[*]} == *"$rootdir/build/fio/spdk_bdev"* ]] && return 0

	# Do we depend on it?
	local crit_binaries=()

	crit_binaries+=("nvme")
	crit_binaries+=("qemu-system*")
	# Add more if needed

	for bin in "${crit_binaries[@]}"; do
		# The below SC is intentional
		# shellcheck disable=SC2053
		[[ ${exe_path##*/} == $bin ]] && return 0
	done

	return 1
}

args+=(core_pid)
args+=(core_sig)
args+=(core_ts)

read -r "${args[@]}" <<< "$*"

exe_path=$(readlink -f "/proc/$core_pid/exe")
cwd_path=$(readlink -f "/proc/$core_pid/cwd")
exe_comm=$(< "/proc/$core_pid/comm")
statm=$(< "/proc/$core_pid/statm")
core_time=$(date -d@"$core_ts")
core_sig_name=$(kill -l "$core_sig")
mapfile -t maps < <(readlink -f "/proc/$core_pid/map_files/"*)

# Filter out processes that we don't care about
filter_process || exit 0

core=$(< "$rootdir/.coredump_path")/${exe_path##*/}_$core_pid.core
stderr

# RLIMIT_CORE is not enforced when core is piped to us. To make
# sure we won't attempt to overload underlying storage, copy
# only the reasonable amount of bytes (systemd defaults to 2G
# so let's follow that).
rlimit=$((1024 * 1024 * 1024 * 2))

# Clear path for lz
rm -f "$core"{,.{bin,bt,gz,json}}

# Slurp the core
head -c "$rlimit" <&0 > "$core"
core_size=$(wc -c < "$core")

# Compress it
gzip -c "$core" > "$core.gz"

# Save the binary
cp "$exe_path" "$core.bin"

# Save the backtrace
bt "$exe_path" "$core" > "$core.bt.txt"

# Save the metadata of the core
core_meta > "$core.json"

# Nuke the original core
rm "$core"
