import re
import multiprocessing

def process_chunk(paddr_chunk, prefetch_addresses):
    # 对数据块进行按位与操作并检查缺失的结果
    paddr_and_results = [int(num, 16) & 0xfffffff40 for num in paddr_chunk]
    missing_results = [result for result in paddr_and_results if result not in prefetch_addresses]
    return missing_results

def main():
    print("begin")

    # 读取PAddr文件内容
    with open('miss.txt', 'r') as file:
        paddr_content = file.read()
    print("miss end")

    # 读取prefetch文件内容
    with open('pf.txt', 'r') as file:
        prefetch_content = file.read()
    print("pf end")

    # 提取PAddr后的数字
    paddr_numbers = re.findall(r'PAddr\s([0-9a-fA-F]+)', paddr_content)

    # 提取prefetch后的地址
    prefetch_addresses = re.findall(r'prefetch for\s(0x[0-9a-fA-F]+)', prefetch_content)
    prefetch_addresses = [int(addr, 16) for addr in prefetch_addresses]

    # 去重
    paddr_numbers = list(set(paddr_numbers))
    prefetch_addresses = list(set(prefetch_addresses))

    # 获取CPU核心数
    num_cores = multiprocessing.cpu_count()

    # 将数据分割成多个部分以便并行处理
    chunk_size = len(paddr_numbers) // num_cores
    paddr_chunks = [paddr_numbers[i:i + chunk_size] for i in range(0, len(paddr_numbers), chunk_size)]

    # 创建一个进程池
    with multiprocessing.Pool(processes=num_cores) as pool:
        # 并行处理每个数据块
        results = pool.starmap(process_chunk, [(chunk, prefetch_addresses) for chunk in paddr_chunks])

    # 将结果列表展平
    missing_results = [item for sublist in results for item in sublist]

    # 将缺失结果输出到文件
    with open('missing_results.txt', 'w') as file:
        for result in missing_results:
            file.write(f'{result:x}\n')

    # 计算百分比
    missing_percentage = (len(missing_results) / len(paddr_numbers)) * 100

    # 输出结果
    print(f"按位与后未出现在prefetch地址中的结果数量: {len(missing_results)}")
    print(f"这些结果占总数的百分比: {missing_percentage:.2f}%")

if __name__ == '__main__':
    main()
