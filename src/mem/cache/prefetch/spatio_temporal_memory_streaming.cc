/**
 * Copyright (c) 2019 Metempsy Technology Consulting
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

#include "mem/cache/prefetch/spatio_temporal_memory_streaming.hh"

#include "debug/HWPrefetch.hh"
#include "mem/cache/prefetch/associative_set_impl.hh"
#include "params/STeMSPrefetcher.hh"

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

// STeMS 类的构造函数
// 参数 p 为 STeMSPrefetcherParams 类型的引用，用于初始化 STeMS 对象
STeMS::STeMS(const STeMSPrefetcherParams &p)
  // 初始化 Queued 对象，调用 Queued 类的构造函数
    : Queued(p),
    // 初始化空间区域大小
    spatialRegionSize(p.spatial_region_size),
    // 计算空间区域大小的对数，用于后续的位操作
    spatialRegionSizeBits(floorLog2(p.spatial_region_size)),
    // 初始化重建条目数量
    reconstructionEntries(p.reconstruction_entries),
    // 初始化活动代数表，用于记录和管理活跃数据的代数信息
    activeGenerationTable(p.active_generation_table_assoc,
                        p.active_generation_table_entries,
                        p.active_generation_table_indexing_policy,
                        p.active_generation_table_replacement_policy,
                        ActiveGenerationTableEntry(
                            spatialRegionSize / blkSize)),
    // 初始化模式序列表，用于记录和管理数据访问模式信息
    patternSequenceTable(p.pattern_sequence_table_assoc,
                        p.pattern_sequence_table_entries,
                        p.pattern_sequence_table_indexing_policy,
                        p.pattern_sequence_table_replacement_policy,
                        ActiveGenerationTableEntry(
                            spatialRegionSize / blkSize)),
    // 初始化区域缺失顺序缓冲区，用于处理数据缺失的情况
    rmob(p.region_miss_order_buffer_entries),
    // 设置是否向 RMOB 中添加重复条目
    addDuplicateEntriesToRMOB(p.add_duplicate_entries_to_rmob),
    // 初始化触发计数器
    lastTriggerCounter(0)
{
    // 确保空间区域大小是 2 的幂，这对于后续的位操作是必要的
    fatal_if(!isPowerOf2(spatialRegionSize),
        "The spatial region size must be a power of 2.");
}

void
STeMS::checkForActiveGenerationsEnd()
{
    // This prefetcher operates attached to the L1 and it observes all
    // accesses, this guarantees that no evictions are missed

    // Iterate over all entries, if any recorded cacheline has been evicted,
    // the generation finishes, move the entry to the PST
    for (auto &agt_entry : activeGenerationTable) {
        // Check if the entry is valid
        if (agt_entry.isValid()) {
            // Initialize flags and variables for processing
            bool generation_ended = false;
            bool sr_is_secure = agt_entry.isSecure();
            Addr pst_addr = 0;

            // Iterate over all sequence entries in the active generation entry
            for (auto &seq_entry : agt_entry.sequence) {
                // Only process entries with a counter greater than 0
                if (seq_entry.counter > 0) {
                    // Calculate the cache address corresponding to the sequence entry
                    Addr cache_addr =
                        agt_entry.paddress + seq_entry.offset * blkSize;
                    // Check if the cacheline has been evicted
                    if (!inCache(cache_addr, sr_is_secure) &&
                            !inMissQueue(cache_addr, sr_is_secure)) {
                        // If evicted, mark the generation as ended and record the PST address
                        generation_ended = true;
                        pst_addr = (agt_entry.pc << spatialRegionSizeBits)
                                    + seq_entry.offset;
                        break;
                    }
                }
            }
            // If the generation has ended, move the entry to the PST
            if (generation_ended) {
                // Find or allocate an entry in the PST using the calculated PST address
                ActiveGenerationTableEntry *pst_entry =
                    patternSequenceTable.findEntry(pst_addr,
                                                false /*unused*/);
                if (pst_entry == nullptr) {
                    // Typically, an entry will not exist, so find a victim to allocate
                    pst_entry = patternSequenceTable.findVictim(pst_addr);
                    assert(pst_entry != nullptr);
                    patternSequenceTable.insertEntry(pst_addr,
                            false /*unused*/, pst_entry);
                } else {
                    // If the entry exists, access it to update its information
                    patternSequenceTable.accessEntry(pst_entry);
                }
                // Update the PST entry information
                pst_entry->update(agt_entry);
                // Free the AGT entry
                activeGenerationTable.invalidate(&agt_entry);
            }
        }
    }
}

// 在STeMS类中添加一个条目到Region Miss Order Buffer (RMOB) 中。
//
// 参数:
// - sr_addr: 服务请求的地址
// - pst_addr: 处理服务请求的处理单元的地址
// - delta: 与服务请求相关联的代价值
//
// 该函数创建一个新的RMOB条目并将其添加到RMOB中。如果在RMOB中已存在相同的条目，则根据配置决定是否添加。
void
STeMS::addToRMOB(Addr sr_addr, Addr pst_addr, unsigned int delta)
{
    // 创建一个新的RMOB条目并初始化它的成员变量。
    RegionMissOrderBufferEntry rmob_entry;
    rmob_entry.srAddress = sr_addr;
    rmob_entry.pstAddress = pst_addr;
    rmob_entry.delta = delta;

    // 如果配置为不添加重复条目，则检查RMOB中是否已存在相同的条目。
    if (!addDuplicateEntriesToRMOB) {
        // 遍历RMOB中的每个条目，检查是否存在相同的服务请求地址、处理单元地址和代价值。
        for (const auto& entry : rmob) {
            if (entry.srAddress == sr_addr &&
                entry.pstAddress == pst_addr &&
                entry.delta == delta) {
                // 如果找到相同的条目，则不添加新条目并返回。
                return;
            }
        }
    }

    // 将新的RMOB条目添加到RMOB中。
    rmob.push_back(rmob_entry);
}

void STeMS::calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses, const PacketPtr &pkt) {}
// 计算预取地址
void STeMS::calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses) {
    // 如果没有PC（程序计数器），则忽略该请求
    if (!pfi.hasPC()) {
        DPRINTF(HWPrefetch, "Ignoring request with no PC.\n");
        return;
    }

    // 获取PC、是否安全模式、空间区域地址和物理地址
    Addr pc = pfi.getPC();
    bool is_secure = pfi.isSecure();
    Addr sr_addr = pfi.getAddr() / spatialRegionSize;
    Addr paddr = pfi.getPaddr();

    // 计算空间区域内的偏移量
    Addr sr_offset = (pfi.getAddr() % spatialRegionSize) / blkSize;

    // 检查是否有活动的生成已经结束
    checkForActiveGenerationsEnd();

    // 在活动生成表中查找条目
    ActiveGenerationTableEntry *agt_entry = activeGenerationTable.findEntry(sr_addr, is_secure);
    if (agt_entry != nullptr) {
        // 找到条目，正在记录中，添加偏移量
        activeGenerationTable.accessEntry(agt_entry);
        agt_entry->addOffset(sr_offset);
        lastTriggerCounter += 1;
    } else {
        // 未找到，这是第一次访问（触发访问）

        // 将条目添加到RMOB（最近使用移动缓冲区）
        Addr pst_addr = (pc << spatialRegionSizeBits) + sr_offset;
        addToRMOB(sr_addr, pst_addr, lastTriggerCounter);
        // 重置最后触发计数器
        lastTriggerCounter = 0;

        // 分配一个新的AGT（活动生成表）条目
        agt_entry = activeGenerationTable.findVictim(sr_addr);
        assert(agt_entry != nullptr);
        activeGenerationTable.insertEntry(sr_addr, is_secure, agt_entry);
        agt_entry->pc = pc;
        agt_entry->paddress = paddr;
        agt_entry->addOffset(sr_offset);
    }

    // 增加其他条目的序列计数器
    for (auto &agt_e : activeGenerationTable) {
        if (agt_e.isValid() && agt_entry != &agt_e) {
            agt_e.seqCounter += 1;
        }
    }

    // 如果是缓存未命中，搜索RMOB中最近的条目，并重建注册的访问序列
    if (pfi.isCacheMiss()) {
        auto it = rmob.end();
        while (it != rmob.begin()) {
            --it;
            if (it->srAddress == sr_addr) {
                // 重建访问序列
                reconstructSequence(it, addresses);
                break;
            }
        }
    }
}

// 重建序列
// 该函数通过处理区域缺失顺序缓冲区（RMOB）的条目来重建地址序列
// 参数:
// - rmob_it: RMOB的迭代器，用于指示处理的起始位置
// - addresses: 重建后的地址序列，按优先级排序
void
STeMS::reconstructSequence(
    CircularQueue<RegionMissOrderBufferEntry>::iterator rmob_it,
    std::vector<AddrPriority> &addresses)
{
    // 初始化重建数组，大小为reconstructionEntries，元素初始化为MaxAddr
    std::vector<Addr> reconstruction(reconstructionEntries, MaxAddr);
    unsigned int idx = 0;

    // 处理RMOB条目，从最近的地址（sr_addr）开始，到最新的条目
    for (auto it = rmob_it; it != rmob.end() && (idx < reconstructionEntries);
        it++) {
        // 根据空间区域大小和RMOB条目的srAddress计算重建地址
        reconstruction[idx] = it->srAddress * spatialRegionSize;
        // 更新索引值，考虑delta值
        idx += (it+1)->delta + 1;
    }

    // 使用每个RMOB条目的PC查询PST（模式序列表）
    idx = 0;
    for (auto it = rmob_it; it != rmob.end() && (idx < reconstructionEntries);
        it++) {
        // 查找PST条目
        ActiveGenerationTableEntry *pst_entry =
            patternSequenceTable.findEntry(it->pstAddress, false /* unused */);
        if (pst_entry != nullptr) {
            // 访问PST条目
            patternSequenceTable.accessEntry(pst_entry);
            // 处理PST条目的序列信息
            for (auto &seq_entry : pst_entry->sequence) {
                if (seq_entry.counter > 1) {
                    // 计算重建地址
                    Addr rec_addr = it->srAddress * spatialRegionSize +
                        seq_entry.offset;
                    unsigned ridx = idx + seq_entry.delta;
                    // 尝试使用对应位置，如果已被使用，则查找周围位置
                    if (ridx < reconstructionEntries &&
                        reconstruction[ridx] == MaxAddr) {
                        reconstruction[ridx] = rec_addr;
                    } else if ((ridx + 1) < reconstructionEntries &&
                        reconstruction[ridx + 1] == MaxAddr) {
                        reconstruction[ridx + 1] = rec_addr;
                    } else if ((ridx + 2) < reconstructionEntries &&
                        reconstruction[ridx + 2] == MaxAddr) {
                        reconstruction[ridx + 2] = rec_addr;
                    } else if ((ridx > 0) &&
                        ((ridx - 1) < reconstructionEntries) &&
                        reconstruction[ridx - 1] == MaxAddr) {
                        reconstruction[ridx - 1] = rec_addr;
                    } else if ((ridx > 1) &&
                        ((ridx - 2) < reconstructionEntries) &&
                        reconstruction[ridx - 2] == MaxAddr) {
                        reconstruction[ridx - 2] = rec_addr;
                    }
                }
            }
        }
        // 更新索引值，考虑delta值
        idx += (it+1)->delta + 1;
    }

    // 将重建的地址添加到输出地址序列中，忽略未使用的地址（MaxAddr）
    for (Addr pf_addr : reconstruction) {
        if (pf_addr != MaxAddr) {
            addresses.push_back(AddrPriority(pf_addr, 0));
        }
    }
}

} // namespace prefetch
} // namespace gem5
