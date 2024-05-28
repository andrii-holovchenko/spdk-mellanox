#!/bin/bash -e
# Dependency on package:
#   libhugetlbfs-utils @ CentOS
#   hugepages @ Ubuntu

min_hugemem=${MIN_HUGEMEM:-2G}
Hugetlb=$(grep Hugetlb /proc/meminfo | awk '{ print $2 }')

case $(echo ${min_hugemem: -1}) in
    M|m)
        unit=m
        ;;
    G|g)
        unit=g
        ;;
    K|k)
        unit=k
        ;;
    *)
        echo "[ERROR]: Unsupported unit format for hugepages!"
        exit 1
        ;;
esac

if [ $Hugetlb -gt 0 ]; then
    if [ $unit = "k" ]; then
        required_size=${min_hugemem%?}
	hp_size_mb=$((${min_hugemem%?} // 1024))
    elif [ $unit = "m" ]; then
	required_size=$((${min_hugemem%?} * 1024))
	hp_size_mb=${min_hugemem%?}
    elif [ $unit = "g" ]; then
	required_size=$((${min_hugemem%?} * 1024 * 1024))
	hp_size_mb=$((${min_hugemem%?} * 1024))
    fi

    if [ $Hugetlb -ge $required_size ]; then
        exit 0
    fi
fi

if [ -e /usr/bin/hugeadm ]; then
    exec /usr/bin/hugeadm --pool-pages-min DEFAULT:${min_hugemem}
else
    exec env HUGEMEM="$hp_size_mb" PCI_ALLOWED="none" /usr/share/spdk/scripts/setup.sh
fi
