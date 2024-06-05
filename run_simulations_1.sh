#!/bin/bash

# 定义模拟器路径
GEM5_PATH="build/ARM/gem5.opt"
# 定义配置文件路径
CONFIG_PATH="configs/dmp_pf/fs_L2.py"
# 定义dmp-indir-range数组
INDIR_RANGES=(1 2 3 4 5 6 7 8)

# 循环dmp-indir-range的值
for INDIR_RANGE in "${INDIR_RANGES[@]}"
do
    echo "Running simulation with dmp-indir-range: $INDIR_RANGE"
    # 运行gem5模拟
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
        --dmp-indir-range=$INDIR_RANGE \
        --checkpoint-dir=/home/xymc/Desktop/gem5_dda/m5out -r 1 --restore-with-cpu=O3_ARM_v7a_3 \
   	| grep -E 'Insert RelationTable:' > m5out/out_pre_range_${INDIR_RANGE}.txt

    # 检查模拟是否生成了stats.txt文件
    if [ -f /home/xymc/Desktop/gem5_dda/m5out/stats.txt ]; then
        # 重命名stats.txt以反映dmp-indir-range大小
        mv /home/xymc/Desktop/gem5_dda/m5out/stats.txt /home/xymc/Desktop/gem5_dda/m5out/stats_indir_range_${INDIR_RANGE}.txt
    else
        echo "Simulation with dmp-indir-range $INDIR_RANGE did not produce a stats.txt file."
    fi
done

echo "Simulations complete."
