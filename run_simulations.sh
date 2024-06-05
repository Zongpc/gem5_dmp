#!/bin/bash

# 定义模拟器路径
GEM5_PATH="build/ARM/gem5.opt"
# 定义配置文件路径
CONFIG_PATH="configs/dmp_pf/fs_L2.py"
# 定义TLB大小数组
TLB_SIZES=(4096)

# 循环TLB大小
for TLB_SIZE in "${TLB_SIZES[@]}"
do
    echo "Running simulation with TLB size: $TLB_SIZE"
    # 运行gem5模拟并筛选 "Insert RelationTable:" 日志信息
    $GEM5_PATH --debug-flags=DMP $CONFIG_PATH \
        --num-cpus=1 \
        --cpu-clock=2.5GHz \
        --cpu-type=O3_ARM_v7a_3 \
        --caches \
        --l2cache \
        --l1i_assoc=8 \
        --l1d_assoc=8 \
        --l2_assoc=4 \
        --l2_mshr_num=32 \
        --l2_repl_policy=LRURP \
        --l1d-hwp-type=DiffMatchingPrefetcher \
        --dmp-notify=l1 \
        --mem-type=SimpleMemory \
        --mem-size=8GB \
        --kernel=../tar/binaries/vmlinux.arm64 \
        --bootloader=../tar/binaries/boot.arm64 \
        --disk-image=../tar/ubuntu-18.04-arm64-docker.img \
        --script=mount_target/mchashjoins.rcS \
        --tlb-size=$TLB_SIZE \
    	| grep -E 'Insert RelationTable:' > m5out/out_pre_tlb_${TLB_SIZE}.txt

    # 检查模拟是否生成了 stats.txt 文件
    if [ -f m5out/stats.txt ]; then
        # 重命名 stats.txt 以反映 TLB 大小
        mv m5out/stats.txt m5out/stats_pre_tlb_${TLB_SIZE}.txt
    else
        echo "Simulation with TLB size $TLB_SIZE did not produce a stats.txt file."
    fi
done

echo "Simulations complete."
