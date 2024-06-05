#!/bin/bash

# 定期刷新 sudo 缓存的函数
keep_sudo_alive() {
    while true; do
        sudo -v
        sleep 60
    done
}

# 启动后台进程定期刷新 sudo 缓存
keep_sudo_alive &

# 获取后台进程的 PID 以便脚本结束时终止它
KEEP_SUDO_ALIVE_PID=$!

# 定义需要测试的参数
sizes=(12800 64000 128000 256000 512000 1280000 2560000)

# 定义其他固定参数
script_path="mount_target/mchashjoins.rcS"
command="build/ARM/gem5.opt configs/dmp_pf/fs_L2.py --num-cpus 1 --cpu-clock 2.5GHz --cpu-type O3_ARM_v7a_3 --caches --l2cache --l1i_assoc 8 --l1d_assoc 8 --l2_assoc 4 --l2_mshr_num 32 --l2_repl_policy LRURP  --mem-type SimpleMemory --mem-size 8GB --kernel=../tar/binaries/vmlinux.arm64 --bootloader=../tar/binaries/boot.arm64 --disk-image=../tar/ubuntu-18.04-arm64-docker.img --script=${script_path}"

# 循环遍历每一个参数组合
for size in "${sizes[@]}"; do
    # 修改mchashjoins.rcS文件内容
    sudo sh -c "echo '#!/bin/sh\n\n\
    echo \"hash join Computing!\"\n\
    ./mchashjoins --algo=NPO_st --r-size=${size} --s-size=${size}\n\
    echo \"hash join Done.\"\n\n\
    /sbin/m5 exit' > ${script_path}"

    # 运行测试
    eval "${command}"

    # 重命名生成的m5out/stats.txt文件
    if [ -f "m5out/stats.txt" ]; then
        mv "m5out/stats.txt" "m5out/stats_rsize_${size}_ssize_${size}.txt"
    else
        echo "Simulation with r-size and s-size $size did not produce a stats.txt file."
    fi
done

# 脚本结束时终止后台进程
kill $KEEP_SUDO_ALIVE_PID
