#!/bin/bash

#build/ARM/gem5.opt --debug-flags=DMP     configs/dmp_pf/fs_L2.py --num-cpus 1 --cpu-clock 2.5GHz --cpu-type O3_ARM_v7a_3     --caches --l2cache --l1i_assoc 8 --l1d_assoc 8 --l2_assoc 4 --l2_mshr_num 32     --l2_repl_policy LRURP --l1d-hwp-type DiffMatchingPrefetcher --dmp-notify l1     --mem-type SimpleMemory --mem-size 8GB --kernel=/home/zongpc/work/qiang/os/binaries/vmlinux.arm64     --bootloader=/home/zongpc/work/qiang/os/binaries/boot.arm64 --disk-image=/home/zongpc/work/qiang/os/ubuntu-18.04-arm64-docker.img     --script=/home/zongpc/work/qiang/gem5_study/mount_target/mchashjoin.rcS --checkpoint-dir=/home/zongpc/work/qiang/gem5_study/m5out/hashjoin

# 定义 -r 参数的值
r_values=("graph_bfs_cg10")

# Prefetcher选择器
pf_values=("StridePrefetcher")

# 遍历 r_values 数组并执行命令
for r in "${r_values[@]}"; do
    for p in "${pf_values[@]}"; do
        if [ -d "m5out/${r}_${p}" ]; then
            if [ -d "m5out/${r}_${p}_bak" ]; then
                rm -rf m5out/${r}_${p}_bak
            fi
            mv m5out/${r}_${p} m5out/${r}_${p}_bak
            mkdir -p m5out/${r}_${p}
        else
            mkdir -p m5out/${r}_${p}
        fi

        # 定义第一个指令
        first_command="build/ARM/gem5.opt --outdir=./m5out/${r}_${p} --debug-flags=HWPrefetch \
        configs/dmp_pf/fs_L2.py --num-cpus 1 --cpu-clock 2.5GHz --cpu-type O3_ARM_v7a_3 \
        --caches --l2cache --l1i_assoc 8 --l1d_assoc 8 --l2_assoc 4 --l2_mshr_num 32 \
        --l2_repl_policy LRURP --l1d-hwp-type ${p} \
        --mem-type SimpleMemory --mem-size 8GB --kernel=/home/zongpc/work/qiang/os/binaries/vmlinux.arm64 \
        --bootloader=/home/zongpc/work/qiang/os/binaries/boot.arm64 --disk-image=/home/zongpc/work/qiang/os/ubuntu-18.04-arm64-docker.img \
        --script=./mount_target/${r}.rcS \
        "
        #--checkpoint-dir=./m5out/${r} "

        # 执行第一个指令
        echo "执行指令: $first_command"
        eval $first_command  > tmplog_${r}_${p}.txt

        # 获取前300,000行和后100,000行，并将其写入 1.txt
        # {
        #     head -n 300000 temp_output.txt
        #     grep -E "Insert RelationTable:" temp_output.txt
        #     tail -n 100000 temp_output.txt
        # } > "result_size_4096_change/debug_${size}.txt"| grep "Insert RelationTable:"> ${r}.txt

        # 检查第一个指令是否成功执行
        if [ $? -ne 0 ]; then
            echo "第一个指令执行失败，终止脚本执行。"
        fi

        # 移动并重命名 stats.txt 文件
        #mv "m5out/stats.txt" "m5out/${r}_${p}/stats.txt"
        #mv "m5out/system.terminal" "m5out/${r}_${p}/system.terminal"
    done
done

echo "所有指令成功执行。"

