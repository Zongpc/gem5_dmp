/**
 * Copyright (c) 2018 Metempsy Technology Consulting
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "mem/cache/prefetch/indirect_memory.hh"

#include "mem/cache/base.hh"
#include "mem/cache/prefetch/associative_set_impl.hh"
#include "params/IndirectMemoryPrefetcher.hh"

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

// 构造函数，用于初始化IndirectMemory对象
IndirectMemory::IndirectMemory(const IndirectMemoryPrefetcherParams &p)
  // 初始化基类Queued和成员变量
    : Queued(p),
    maxPrefetchDistance(p.max_prefetch_distance), // 设置最大预取距离
    shiftValues(p.shift_values), // 初始化移位值
    prefetchThreshold(p.prefetch_threshold), // 设置预取阈值
    streamCounterThreshold(p.stream_counter_threshold), // 设置流计数器阈值
    streamingDistance(p.streaming_distance), // 设置流距离
    // 初始化预取表，用于存储和管理预取信息
    prefetchTable(p.pt_table_assoc, p.pt_table_entries,
                p.pt_table_indexing_policy, p.pt_table_replacement_policy,
                PrefetchTableEntry(p.num_indirect_counter_bits)),
    // 初始化间接模式检测器，用于识别和跟踪间接访问模式
    ipd(p.ipd_table_assoc, p.ipd_table_entries, p.ipd_table_indexing_policy,
        p.ipd_table_replacement_policy,
        IndirectPatternDetectorEntry(p.addr_array_len, shiftValues.size())),
    ipdEntryTrackingMisses(nullptr), // 初始化间接模式检测器条目跟踪未命中指针
    byteOrder(p.sys->getGuestByteOrder()) // 设置字节序
{
}

// 计算预取地址
void IndirectMemory::calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses)
{
    // 必须有PC（程序计数器）才能进行预取
    if (!pfi.hasPC()) {
        return;
    }

    // 获取安全模式状态、程序计数器、访问地址和缓存未命中状态
    bool is_secure = pfi.isSecure();
    Addr pc = pfi.getPC();
    Addr addr = pfi.getAddr();
    bool miss = pfi.isCacheMiss();

    // 检查活跃条目的访问匹配情况
    checkAccessMatchOnActiveEntries(addr);

    // 如果预取器正在跟踪未命中情况，并且当前是未命中
    if (ipdEntryTrackingMisses != nullptr && miss) {
        // 检查跟踪未命中的条目是否已设置第二个索引
        if (!ipdEntryTrackingMisses->secondIndexSet) {
            trackMissIndex1(addr);
        } else {
            trackMissIndex2(addr);
        }
    } else {
        // 如果未跟踪未命中，尝试检测流访问
        PrefetchTableEntry *pt_entry = prefetchTable.findEntry(pc, false /* unused */);
        if (pt_entry != nullptr) {
            prefetchTable.accessEntry(pt_entry);

            // 如果当前访问地址与条目中的地址不同，可能是流访问
            if (pt_entry->address != addr) {
                // 流访问检测到，增加流计数器
                pt_entry->streamCounter += 1;
                // 如果流计数器超过阈值，进行预取
                if (pt_entry->streamCounter >= streamCounterThreshold) {
                    // 计算偏移量，用于生成预取地址
                    int64_t delta = addr - pt_entry->address;
                    // 生成并添加预取地址
                    for (unsigned int i = 1; i <= streamingDistance; i += 1) {
                        addresses.push_back(AddrPriority(addr + delta * i, 0));
                    }
                }
                // 更新条目中的地址和安全模式状态
                pt_entry->address = addr;
                pt_entry->secure = is_secure;

                // 如果是读操作且数据大小不超过8字节，尝试读取索引
                if (!miss && !pfi.isWrite() && pfi.getSize() <= 8) {
                    int64_t index = 0;
                    bool read_index = true;
                    // 根据数据大小读取索引
                    switch(pfi.getSize()) {
                        case sizeof(uint8_t):
                            index = pfi.get<uint8_t>(byteOrder);
                            break;
                        case sizeof(uint16_t):
                            index = pfi.get<uint16_t>(byteOrder);
                            break;
                        case sizeof(uint32_t):
                            index = pfi.get<uint32_t>(byteOrder);
                            break;
                        case sizeof(uint64_t):
                            index = pfi.get<uint64_t>(byteOrder);
                            break;
                        default:
                            read_index = false; // 忽略非2的幂大小
                    }
                    // 如果成功读取索引且条目未启用，分配或更新IPD条目并开始跟踪未命中
                    if (read_index && !pt_entry->enabled) {
                        allocateOrUpdateIPDEntry(pt_entry, index);
                    } else if (read_index) {
                        // 启用的条目，更新索引和间接计数器
                        pt_entry->index = index;
                        if (!pt_entry->increasedIndirectCounter) {
                            pt_entry->indirectCounter--;
                        } else {
                            pt_entry->increasedIndirectCounter = false;
                        }

                        // 如果间接计数器足够高，开始预取
                        if (pt_entry->indirectCounter > prefetchThreshold) {
                            unsigned distance = maxPrefetchDistance * pt_entry->indirectCounter.calcSaturation();
                            for (int delta = 1; delta < distance; delta += 1) {
                                Addr pf_addr = pt_entry->baseAddr + (pt_entry->index << pt_entry->shift);
                                addresses.push_back(AddrPriority(pf_addr, 0));
                            }
                        }
                    }
                }
            }
        } else {
            // 如果没有找到条目，找到一个牺牲品并插入新条目
            pt_entry = prefetchTable.findVictim(pc);
            assert(pt_entry != nullptr);
            prefetchTable.insertEntry(pc, false /* unused */, pt_entry);
            pt_entry->address = addr;
            pt_entry->secure = is_secure;
        }
    }
}

void
IndirectMemory::allocateOrUpdateIPDEntry(
    const PrefetchTableEntry *pt_entry, int64_t index)
{
    // 使用pt_entry的地址对IPD进行索引
    Addr ipd_entry_addr = (Addr) pt_entry;
    // 在IPD中查找对应的条目，第二个参数为false表示不需要更新
    IndirectPatternDetectorEntry *ipd_entry = ipd.findEntry(ipd_entry_addr,
                                                            false/* unused */);
    if (ipd_entry != nullptr) {
        // 访问IPD条目
        ipd.accessEntry(ipd_entry);
        if (!ipd_entry->secondIndexSet) {
            // 第二次看到索引时，填充idx2
            ipd_entry->idx2 = index;
            ipd_entry->secondIndexSet = true;
            // 将跟踪缺失的IPD条目设置为当前条目
            ipdEntryTrackingMisses = ipd_entry;
        } else {
            // 第三次访问，但到目前为止还没有发现模式，释放IPD条目
            ipd.invalidate(ipd_entry);
            // 清除跟踪缺失的IPD条目
            ipdEntryTrackingMisses = nullptr;
        }
    } else {
        // 找到一个牺牲品条目以存储新的IPD条目
        ipd_entry = ipd.findVictim(ipd_entry_addr);
        // 确保找到了牺牲品条目
        assert(ipd_entry != nullptr);
        // 在IPD中插入新的条目，第二个参数为false表示不需要更新
        ipd.insertEntry(ipd_entry_addr, false /* unused */, ipd_entry);
        // 第一次看到索引，填充idx1
        ipd_entry->idx1 = index;
        // 将跟踪缺失的IPD条目设置为当前条目
        ipdEntryTrackingMisses = ipd_entry;
    }
}

/**
 * 在间接内存访问中跟踪未命中索引1
 * 
 * 此函数用于在间接模式检测器条目中跟踪缺失的索引1，它通过更新基础地址向量来实现，
 * 基于未命中的地址和当前的索引值
 * 
 * @param miss_addr 未命中的地址，这是计算间接地址的基础
 */
void IndirectMemory::trackMissIndex1(Addr miss_addr)
{
    // 获取当前用于跟踪未命中事件的间接模式检测器条目
    IndirectPatternDetectorEntry *entry = ipdEntryTrackingMisses;
    
    // 确保未命中的次数小于基础地址向量的大小，否则会发生越界
    assert(entry->numMisses < entry->baseAddr.size());
    
    // 引用基础地址向量，该向量存储针对不同位移值计算的基础地址
    std::vector<Addr> &ba_array = entry->baseAddr[entry->numMisses];
    
    // 初始化向量索引
    int idx = 0;
    
    // 遍历位移值数组，计算基础地址并更新基础地址向量
    for (int shift : shiftValues) {
        // 计算基础地址并存储到相应的向量位置
        ba_array[idx] = miss_addr - (entry->idx1 << shift);
        // 更新向量索引
        idx += 1;
    }
    
    // 增加未命中的计数
    entry->numMisses += 1;
    
    // 如果未命中的次数等于基础地址向量的大小，表示已跟踪完足够的未命中事件
    if (entry->numMisses == entry->baseAddr.size()) {
        // 设置跟踪未命中事件的条目为nullptr，停止跟踪
        ipdEntryTrackingMisses = nullptr;
    }
}
// 在间接内存访问中跟踪未命中索引2
// 该函数用于处理间接内存访问中的未命中情况，通过跟踪和匹配之前的未命中地址
// 来填充预测表(PT)的额外字段，从而优化内存访问模式的预测。
// 参数:
//   miss_addr: 当前未命中的地址
void IndirectMemory::trackMissIndex2(Addr miss_addr)
{
    // 获取正在跟踪未命中情况的IPD条目
    IndirectPatternDetectorEntry *entry = ipdEntryTrackingMisses;
    
    // 第二个索引已被填充，将之前未命中时（使用idx1）生成的地址
    // 与新生成的地址（使用idx2）进行比较，如果找到匹配项，
    // 则填充PT条目的附加字段。
    for (int midx = 0; midx < entry->numMisses; midx += 1)
    {
        // 获取当前未命中情况下的基地址数组
        std::vector<Addr> &ba_array = entry->baseAddr[midx];
        
        // 遍历基地址数组，寻找匹配项
        int idx = 0;
        for (int shift : shiftValues) {
            // 如果找到匹配项
            if (ba_array[idx] == (miss_addr - (entry->idx2 << shift))) {
                // 填充相应的PT条目
                PrefetchTableEntry *pt_entry =
                    (PrefetchTableEntry *) entry->getTag();
                pt_entry->baseAddr = ba_array[idx];
                pt_entry->shift = shift;
                pt_entry->enabled = true;
                pt_entry->indirectCounter.reset();
                
                // 释放当前的IPD条目
                ipd.invalidate(entry);
                
                // 不再跟踪更多未命中
                ipdEntryTrackingMisses = nullptr;
                return;
            }
            idx += 1;
        }
    }
}

/**
 * 检查并更新预取表中活跃条目的访问匹配度
 * 
 * 此函数遍历预取表中的每一个条目，如果条目处于启用状态且给定地址与条目的基地址及索引匹配，
 * 则增加该条目的间接计数器，并标记该计数器已增加。这用于优化内存访问预测机制的准确性。
 * 
 * @param addr 要检查的内存地址
 */
void IndirectMemory::checkAccessMatchOnActiveEntries(Addr addr)
{
    // 遍历预取表中的每一个条目
    for (auto &pt_entry : prefetchTable) {
        // 检查条目是否处于启用状态
        if (pt_entry.enabled) {
            // 检查给定地址是否与当前条目的基地址和索引偏移地址匹配
            if (addr == pt_entry.baseAddr + (pt_entry.index << pt_entry.shift)) {
                // 如果匹配，增加间接计数器的值
                pt_entry.indirectCounter++;
                // 标记间接计数器已增加
                pt_entry.increasedIndirectCounter = true;
            }
        }
    }
}

} // namespace prefetch
} // namespace gem5
