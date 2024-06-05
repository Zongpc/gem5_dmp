#!/bin/bash



# 第一个指令
build/ARM/gem5.opt configs/dmp_pf/fs_L2.py --num-cpus 1 --cpu-clock 2.5GHz --cpu-type O3_ARM_v7a_3 --caches --l2cache --l1i_assoc 8 --l1d_assoc 8 --l2_assoc 4 --l2_mshr_num 32 --l2_repl_policy LRURP --mem-type SimpleMemory --mem-size 8GB --kernel=../tar/binaries/vmlinux.arm64 --bootloader=../tar/binaries/boot.arm64 --disk-image=../tar/ubuntu-18.04-arm64-docker.img --script=mount_target/mchashjoins.rcS --checkpoint-dir=/home/xymc/Desktop/gem5_dda/m5out -r 24 --restore-with-cpu=O3_ARM_v7a_3

# 检查第一个指令是否成功执行
if [ $? -ne 0 ]; then
    echo "第一个指令执行失败，终止脚本执行。"
    exit 1
fi

# 移动并重命名 stats.txt 文件
mv "m5out/stats.txt" "m5out/stats_rsize_2560000_ssize_2560000.txt"

# 第二个指令
build/ARM/gem5.opt --debug-flags=DMP configs/dmp_pf/fs_L2.py --num-cpus 1 --cpu-clock 2.5GHz --cpu-type O3_ARM_v7a_3 --caches --l2cache --l1i_assoc 8 --l1d_assoc 8 --l2_assoc 4 --l2_mshr_num 32 --l2_repl_policy LRURP --l1d-hwp-type StridePrefetcher --dmp-notify l1 --mem-type SimpleMemory --mem-size 8GB --kernel=../tar/binaries/vmlinux.arm64 --bootloader=../tar/binaries/boot.arm64 --disk-image=../tar/ubuntu-18.04-arm64-docker.img --script=mount_target/mchashjoins.rcS --checkpoint-dir=/home/xymc/Desktop/gem5_dda/m5out -r 23 --restore-with-cpu=O3_ARM_v7a_3

# 检查第二个指令是否成功执行
if [ $? -ne 0 ]; then
    echo "第二个指令执行失败。"
    exit 1
fi

echo "所有指令成功执行。"
