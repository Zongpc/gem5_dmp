#!/bin/bash

# 定义调试标志数组
DEBUG_FLAG_ARR=(
#    "TLB,TLBVerbose"
    "HWPrefetch,HWPrefetchQueue"
    "DMP"
    "Cache,CacheVerbose,CachePort"
#    "RequestSlot"
#    "PacketQueue"
#    "CoherentXBar"
)

# 确保 gem5.opt 文件有执行权限
chmod +x build/ARM/gem5.opt

# 定义 gem5 运行函数
function gem5_run() {
    local RUN_LABEL
    IFS=',' read -r RUN_LABEL _ <<< $1
    local RUN_LABEL="DEBUG_"$RUN_LABEL
    echo "Running with debug flags: $1"  # 打印当前标志
    # 删除已有的运行目录
    if [ -d ${RUN_LABEL} ]; then
        rm -r $RUN_LABEL
    fi
    
    # 创建新的运行目录并切换到该目录
    mkdir $RUN_LABEL
    cd $RUN_LABEL
    
    # 执行 gem5 命令
    ../build/ARM/gem5.opt --debug-flags=$1 \
        ../configs/dmp_pf/fs_L2.py \
        --num-cpus 1 --cpu-clock 2.5GHz --cpu-type O3_ARM_v7a_3 --caches --l2cache \
        --l1i_assoc 8 --l1d_assoc 8 --l2_assoc 4 --l2_mshr_num 32 --l2_repl_policy LRURP \
        --l1d-hwp-type DiffMatchingPrefetcher --dmp-notify l1 --mem-type SimpleMemory \
        --mem-size 8GB --kernel=../../tar/binaries/vmlinux.arm64 --bootloader=../../tar/binaries/boot.arm64 \
        --disk-image=../../tar/ubuntu-18.04-arm64-docker.img --script=../mount_target/mchashjoins.rcS \
        --checkpoint-dir /home/luoqiang/xymc/gem5_dda/m5out -r 1 --restore-with-cpu=O3_ARM_v7a_3 \
        > ${RUN_LABEL}.log

    # 检查命令是否成功执行
    if [ $? -ne 0 ]; then
        echo "Error running gem5 with debug flags $1"
    fi

    # 返回上一级目录
    cd ..
}

# 循环遍历调试标志数组并运行 gem5
for label in ${DEBUG_FLAG_ARR[@]}; do
    gem5_run $label
done
