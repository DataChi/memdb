#!/bin/bash

#
# IMPORTANT!!! In order for this script to work, execute the following as root before running it:
#
# $ echo 0 > /proc/sys/kernel/yama/ptrace_scope
# 
# This ensures that the OS allows Pin to attach to the process.
# For more information, regarding child injection, see Injection section in the Pin User Manual.
#

# Check that the ptrace_scope file has the correct value
value=$(</proc/sys/kernel/yama/ptrace_scope)
echo 

if [ "$value" -ne 0 ]; then
    echo "In order to run this script execute the following as root: "
    echo "     $ echo 0 > /proc/sys/kernel/yama/ptrace_scope "
    echo "This ensures that the OS allows Pin to attach to the process"
    exit 1
fi


MEMTRACKER_HOME=/cs/systems/home/fedorova/Work/VIVIDPERF/vividperf/pintools/scripts
MEMVIS_HOME=/cs/systems/home/fedorova/Work/DATACHI/memvis

# Create the DB
env LD_LIBRARY_PATH=../wt-dev/build_posix/.libs:../wt-dev/build_posix/ext/compressors/snappy/.libs/ ./db_bench_wiredtiger --cache_size=534217728 --threads=1 --use_lsm=1 --db=/tmpfs/leveldb --benchmarks=fillseq --value_size=62 

env LD_LIBRARY_PATH=../wt-dev/build_posix/.libs:../wt-dev/build_posix/ext/compressors/snappy/.libs/  pin.sh   -t $CUSTOM_PINTOOLS_HOME/obj-intel64/memtracker.so -- ./db_bench_wiredtiger --cache_size=534217728  --use_existing_db=1 --threads=8 --use_lsm=1 --db=/tmpfs/leveldb --reads=1000 --benchmarks=readseq --value_size=62 | tee readseq-with-stats.txt | $MEMTRACKER_HOME/memtracker2json.py > readseq-with-stats.json


cat readseq-with-stats.json |  $MEMVIS_HOME/analyzer -p "LevelDB readseq on WT with stats (full)" -d $MEMVIS_HOME/server/data


