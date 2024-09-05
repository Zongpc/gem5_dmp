#!/bin/bash

# 定义 -r 参数的值
r_values=(1 2 3 4 5 6 7 8)

# 遍历 r_values 数组并执行命令
for r in "${r_values[@]}"; do
    # 定义第一个指令
    first_command="build/ARM/gem5.opt \
    configs/dmp_pf/fs_L2.py --num-cpus 1 --cpu-clock 2.5GHz \
    --cpu-type O3_ARM_v7a_3 --caches --l2cache --l1i_assoc 8 --l1d_assoc 8 \
    --l2_assoc 4 --l2_mshr_num 32 --l2_repl_policy LRURP \
    --mem-type SimpleMemory --mem-size 8GB --kernel=../tar/binaries/vmlinux.arm64 \
    --bootloader=../tar/binaries/boot.arm64 --disk-image=../tar/ubuntu-18.04-arm64-docker.img \
    --script=mount_target/mchashjoins.rcS --checkpoint-dir=/home/luoqiang/xymc/gem5_dda/m5out/tlb_65536_change \
    -r $r --restore-with-cpu=O3_ARM_v7a_3
"

    # 执行第一个指令
    echo "执行指令: $first_command"
    eval $first_command 

    # 检查第一个指令是否成功执行
    if [ $? -ne 0 ]; then
        echo "第一个指令执行失败，终止脚本执行。"
        exit 1
    fi

    # 移动并重命名 stats.txt 文件
    mv "m5out/stats.txt" "m5out/noprefetch_${r}_ssize_${r}.txt"

done

echo "所有指令成功执行。"

