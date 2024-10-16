#!/bin/bash

#build/ARM/gem5.opt --debug-flags=DMP     configs/dmp_pf/fs_L2.py --num-cpus 1 --cpu-clock 2.5GHz --cpu-type O3_ARM_v7a_3     --caches --l2cache --l1i_assoc 8 --l1d_assoc 8 --l2_assoc 4 --l2_mshr_num 32     --l2_repl_policy LRURP --l1d-hwp-type DiffMatchingPrefetcher --dmp-notify l1     --mem-type SimpleMemory --mem-size 8GB --kernel=/home/zongpc/work/qiang/os/binaries/vmlinux.arm64     --bootloader=/home/zongpc/work/qiang/os/binaries/boot.arm64 --disk-image=/home/zongpc/work/qiang/os/ubuntu-18.04-arm64-docker.img     --script=/home/zongpc/work/qiang/gem5_study/mount_target/mchashjoin.rcS --checkpoint-dir=/home/zongpc/work/qiang/gem5_study/m5out/hashjoin

# 定义 -r 参数的值
r_values=("graph_bfs_am0302" "graph_bfs_ws")

# DMP开关
dmp_flag="--l1d-hwp-type DiffMatchingPrefetcher"

# 遍历 r_values 数组并执行命令
for r in "${r_values[@]}"; do
    if [ -d "m5out/${r}" ]; then
        if [ -d "m5out/${r}_bak" ]; then
            rm -rf m5out/${r}_bak
        fi
        mv m5out/${r} m5out/${r}_bak
        mkdir -p m5out/${r}
    else
        mkdir -p m5out/${r}
    fi

    # 定义第一个指令
    first_command="build/ARM/gem5.opt --debug-flags=DMP \
    configs/dmp_pf/fs_L2.py --num-cpus 1 --cpu-clock 2.5GHz --cpu-type O3_ARM_v7a_3 \
    --caches --l2cache --l1i_assoc 8 --l1d_assoc 8 --l2_assoc 4 --l2_mshr_num 32 \
    --l2_repl_policy LRURP ${dmp_flag} --dmp-notify l1 \
    --mem-type SimpleMemory --mem-size 8GB --kernel=/home/zongpc/work/qiang/os/binaries/vmlinux.arm64 \
    --bootloader=/home/zongpc/work/qiang/os/binaries/boot.arm64 --disk-image=/home/zongpc/work/qiang/os/ubuntu-18.04-arm64-docker.img \
    --script=/home/zongpc/work/qiang/gem5_study/mount_target/${r}.rcS --checkpoint-dir=/home/zongpc/work/qiang/gem5_study/m5out/${r} "

    # 执行第一个指令
    echo "执行指令: $first_command"
    eval $first_command  > tmplog_${r}.txt

    # 获取前300,000行和后100,000行，并将其写入 1.txt
    # {
    #     head -n 300000 temp_output.txt
    #     grep -E "Insert RelationTable:" temp_output.txt
    #     tail -n 100000 temp_output.txt
    # } > "result_size_4096_change/debug_${size}.txt"| grep "Insert RelationTable:"> ${r}.txt

    # 检查第一个指令是否成功执行
    if [ $? -ne 0 ]; then
        echo "第一个指令执行失败，终止脚本执行。"
        exit 1
    fi

    # 移动并重命名 stats.txt 文件
    mv "m5out/stats.txt" "m5out/${r}/stats_${r}.txt"
    mv "m5out/system.terminal" "m5out/${r}/${r}.terminal"

    #--------------------------------------------------------------no dmp------------------------------------------------------------------
    if [ -d "m5out/${r}" ]; then
        # 定义第一个指令
        first_command="build/ARM/gem5.opt --debug-flags=DMP \
        configs/dmp_pf/fs_L2.py --num-cpus 1 --cpu-clock 2.5GHz --cpu-type O3_ARM_v7a_3 \
        --caches --l2cache --l1i_assoc 8 --l1d_assoc 8 --l2_assoc 4 --l2_mshr_num 32 \
        --l2_repl_policy LRURP --dmp-notify l1 \
        --mem-type SimpleMemory --mem-size 8GB --kernel=/home/zongpc/work/qiang/os/binaries/vmlinux.arm64 \
        --bootloader=/home/zongpc/work/qiang/os/binaries/boot.arm64 --disk-image=/home/zongpc/work/qiang/os/ubuntu-18.04-arm64-docker.img \
        --script=/home/zongpc/work/qiang/gem5_study/mount_target/${r}.rcS --checkpoint-dir=/home/zongpc/work/qiang/gem5_study/m5out/${r} \
        -r 1 --restore-with-cpu=O3_ARM_v7a_3"
    else
        echo "Sorry, no checkpoint of ${r} found!"
        exit 1
    fi

    # 执行第一个指令
    echo "执行指令: $first_command"
    eval $first_command  > tmplog_${r}_no_dmp.txt

    # 检查第一个指令是否成功执行
    if [ $? -ne 0 ]; then
        echo "第一个指令执行失败，终止脚本执行。"
        exit 1
    fi

    # 移动并重命名 stats.txt 文件
    mv "m5out/stats.txt" "m5out/${r}/stats_${r}_no_dmp.txt"
    mv "m5out/system.terminal" "m5out/${r}/${r}_no_dmp.terminal"
done

echo "所有指令成功执行。"

