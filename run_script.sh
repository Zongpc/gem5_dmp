#!/bin/bash

# 从文件读取密码
PASSWORD=$(cat password.txt)

# 定期刷新 sudo 缓存的函数
keep_sudo_alive() {
    while true; do
        echo "$PASSWORD" | sudo -S -v
        sleep 60
    done
}

# 启动后台进程定期刷新 sudo 缓存
keep_sudo_alive &
# 获取后台进程的 PID 以便脚本结束时终止它
KEEP_SUDO_ALIVE_PID=$!

# 定义需要测试的参数
sizes=(12800 64000 128000 256000 512000 1280000 2560000 5120000)
# sizes=(512000 1280000 2560000 5120000)
# 定义其他固定参数
script_path="mount_target/mchashjoins.rcS"
command="build/ARM/gem5.opt --debug-flags=DMP \
configs/dmp_pf/fs_L2.py \
--num-cpus 1 --cpu-clock 2.5GHz --cpu-type O3_ARM_v7a_3 --caches --l2cache \
--l1i_assoc 8 --l1d_assoc 8 --l2_assoc 4 --l2_mshr_num 32 --l2_repl_policy LRURP \
--l1d-hwp-type DiffMatchingPrefetcher --dmp-notify l1 --mem-type SimpleMemory \
--mem-size 8GB --kernel=../tar/binaries/vmlinux.arm64 --bootloader=../tar/binaries/boot.arm64 \
--disk-image=../tar/ubuntu-18.04-arm64-docker.img --script=${script_path}"

# 确保结果目录存在
mkdir -p result_size_4096_change

# 循环遍历每一个参数组合
for size in "${sizes[@]}"; do
    # 修改 mchashjoins.rcS 文件内容
    echo "$PASSWORD" | sudo -S sh -c "cat <<EOF > ${script_path}
#!/bin/sh

echo \"hash join Computing!\"
./mchashjoins --algo=NPO_st --r-size=${size} --s-size=${size}
echo \"hash join Done.\"

/sbin/m5 exit
EOF"

    # 确保你有对 mchashjoins.rcS 的写权限
    echo "$PASSWORD" | sudo -S chmod 644 ${script_path}

    # 运行测试并将输出写入临时文件
    eval "${command}" > temp_output.txt

    # 获取前300,000行和后100,000行，并将其写入 1.txt
    {
        head -n 300000 temp_output.txt
        grep -E "Insert RelationTable:" temp_output.txt
        tail -n 100000 temp_output.txt
    } > "result_size_4096_change/debug_${size}.txt"

    # 确保你有对目标文件的写权限
    chmod 644 "result_size_4096_change/debug_${size}.txt"

    # 重命名生成的 m5out/stats.txt 文件
    if [ -f "m5out/stats.txt" ]; then
        mv "m5out/stats.txt" "result_size_4096_change/prefetch_${size}.txt"
        # 确保你有对目标文件的写权限
        chmod 644 "result_size_4096_change/prefetch_${size}.txt"
    else
        echo "Simulation with r-size and s-size $size did not produce a stats.txt file."
    fi

    # 删除临时文件
    rm temp_output.txt
done

# 脚本结束时终止后台进程
kill $KEEP_SUDO_ALIVE_PID
