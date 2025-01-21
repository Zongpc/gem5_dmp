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

#include "mem/cache/prefetch/access_map_pattern_matching.hh"

#include "debug/HWPrefetch.hh"
#include "mem/cache/prefetch/associative_set_impl.hh"
#include "params/AMPMPrefetcher.hh"
#include "params/AccessMapPatternMatching.hh"

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

// 构造函数，初始化访问模式匹配的访问映射对象
AccessMapPatternMatching::AccessMapPatternMatching(
    const AccessMapPatternMatchingParams &p)
    : ClockedObject(p), // 初始化基类ClockedObject
        blkSize(p.block_size), // 块大小
        limitStride(p.limit_stride), // 限制 stride
        startDegree(p.start_degree), // 初始度数
        hotZoneSize(p.hot_zone_size), // 热区大小
        highCoverageThreshold(p.high_coverage_threshold), // 高覆盖率阈值
        lowCoverageThreshold(p.low_coverage_threshold), // 低覆盖率阈值
        highAccuracyThreshold(p.high_accuracy_threshold), // 高精度阈值
        lowAccuracyThreshold(p.low_accuracy_threshold), // 低精度阈值
        highCacheHitThreshold(p.high_cache_hit_threshold), // 高缓存命中率阈值
        lowCacheHitThreshold(p.low_cache_hit_threshold), // 低缓存命中率阈值
        epochCycles(p.epoch_cycles), // 一个周期的周期数
        offChipMemoryLatency(p.offchip_memory_latency), // 离片内存延迟
        accessMapTable(p.access_map_table_assoc, p.access_map_table_entries,
                        p.access_map_table_indexing_policy,
                        p.access_map_table_replacement_policy,
                        AccessMapEntry(hotZoneSize / blkSize)), // 初始化访问映射表
        numGoodPrefetches(0), // 有效预取次数，初始化为0
        numTotalPrefetches(0), // 总预取次数，初始化为0
        numRawCacheMisses(0), // 原始缓存未命中次数，初始化为0
        numRawCacheHits(0), // 原始缓存命中次数，初始化为0
        degree(startDegree), // 当前度数，初始化为startDegree
        usefulDegree(startDegree), // 有用的度数，初始化为startDegree
        epochEvent([this]{ processEpochEvent(); }, name()) // 注册周期事件
{
    // 确保热区大小是2的幂
    fatal_if(!isPowerOf2(hotZoneSize),
        "the hot zone size must be a power of 2");
}

// AccessMapPatternMatching 类的启动函数，在对象创建时调用。
void AccessMapPatternMatching::startup()
{
    // 安排 epochEvent 事件在 epochCycles 时钟周期后执行。
    schedule(epochEvent, clockEdge(epochCycles));
}

// 处理每个周期事件，计算访问模式匹配的指标并调整预取策略
void AccessMapPatternMatching::processEpochEvent()
{
    // 在下一个周期调度周期事件
    schedule(epochEvent, clockEdge(epochCycles));

    // 计算预取准确率：正确预取数占总预取数的比例
    double prefetch_accuracy =
        ((double) numGoodPrefetches) / ((double) numTotalPrefetches);

    // 计算预取覆盖率：正确预取数占原始缓存未命中数的比例
    double prefetch_coverage =
        ((double) numGoodPrefetches) / ((double) numRawCacheMisses);

    // 计算缓存命中率：原始缓存命中数占总请求数的比例
    double cache_hit_ratio = ((double) numRawCacheHits) /
        ((double) (numRawCacheHits + numRawCacheMisses));

    // 计算总请求数：原始缓存未命中数减去正确预取数加上总预取数
    double num_requests = (double) (numRawCacheMisses - numGoodPrefetches +
        numTotalPrefetches);

    // 计算内存带宽：总请求数乘以离片内存延迟除以周期的周期数
    double memory_bandwidth = num_requests * offChipMemoryLatency /
        cyclesToTicks(epochCycles);

    // 根据预取覆盖率、准确率和缓存命中率调整预取策略
    if (prefetch_coverage > highCoverageThreshold &&
        (prefetch_accuracy > highAccuracyThreshold ||
        cache_hit_ratio < lowCacheHitThreshold)) {
        // 高覆盖率和准确率下，增加预取度
        usefulDegree += 1;
    } else if ((prefetch_coverage < lowCoverageThreshold &&
            (prefetch_accuracy < lowAccuracyThreshold ||
                cache_hit_ratio > highCacheHitThreshold)) ||
            (prefetch_accuracy < lowAccuracyThreshold &&
                cache_hit_ratio > highCacheHitThreshold)) {
        // 低覆盖率或准确率下，减少预取度
        usefulDegree -= 1;
    }

    // 根据内存带宽和调整后的预取度设置最终的预取度
    degree = std::min((unsigned) memory_bandwidth, usefulDegree);

    // 重置周期统计信息
    numGoodPrefetches = 0.0;
    numTotalPrefetches = 0.0;
    numRawCacheMisses = 0.0;
    numRawCacheHits = 0.0;
}

/**
 * 根据访问映射地址和安全属性获取或插入访问映射条目
 * 
 * @param am_addr 访问映射的地址
 * @param is_secure 指示是否是安全模式下的访问
 * @return 访问映射条目的指针
 * 
 * 此函数尝试从访问映射表中查找给定地址和安全属性的条目如果找到，
 * 它更新该条目的访问时间如果未找到，它在表中插入一个新的条目
 * 
 * 注意：函数假定访问映射表至少有一个空闲的受害者条目，用于新条目的插入
 */
AccessMapPatternMatching::AccessMapEntry *
AccessMapPatternMatching::getAccessMapEntry(Addr am_addr,
                bool is_secure)
{
    // 尝试从访问映射表中查找指定的条目
    AccessMapEntry *am_entry = accessMapTable.findEntry(am_addr, is_secure);
    
    // 如果找到了条目，则更新其访问时间
    if (am_entry != nullptr) {
        accessMapTable.accessEntry(am_entry);
    } else {
        // 如果未找到，尝试找到一个受害者条目用于替换
        am_entry = accessMapTable.findVictim(am_addr);
        // 确保找到了受害者条目，因为表应该有至少一个空闲条目
        assert(am_entry != nullptr);
        
        // 在表中插入新的访问映射条目
        accessMapTable.insertEntry(am_addr, is_secure, am_entry);
    }
    // 返回找到或插入的条目指针
    return am_entry;
}

// 设置访问映射条目的状态
void AccessMapPatternMatching::setEntryState(AccessMapEntry &entry,
    Addr block, enum AccessMapState state)
{
    // 获取原始状态
    enum AccessMapState old = entry.states[block];
    // 设置新的状态
    entry.states[block] = state;

    // 初始化时不需要更新统计信息
    if (state == AM_INIT) return;

    // 根据旧状态更新统计信息
    switch (old) {
        // 初始状态
        case AM_INIT:
            // 如果新状态是预取，则增加总预取次数
            if (state == AM_PREFETCH) {
                numTotalPrefetches += 1;
            } else if (state == AM_ACCESS) {
                // 如果新状态是访问，则增加原始缓存未命中次数
                numRawCacheMisses += 1;
            }
            break;
        // 预取状态
        case AM_PREFETCH:
            // 如果新状态是访问，则增加有效预取和原始缓存未命中次数
            if (state == AM_ACCESS) {
                numGoodPrefetches += 1;
                numRawCacheMisses += 1;
            }
            break;
        // 访问状态
        case AM_ACCESS:
            // 如果状态仍然是访问，则增加原始缓存命中次数
            if (state == AM_ACCESS) {
                numRawCacheHits += 1;
            }
            break;
        // 未知状态，触发断言
        default:
            panic("Impossible path\n");
            break;
    }
}

// 计算预取地址
void AccessMapPatternMatching::calculatePrefetch(const Base::PrefetchInfo &pfi,
    std::vector<Queued::AddrPriority> &addresses)
{
    // 确保初始时地址列表为空
    assert(addresses.empty());

    // 获取安全级别
    bool is_secure = pfi.isSecure();
    // 计算访问图地址
    Addr am_addr = pfi.getAddr() / hotZoneSize;//hotZoneSize:2Kib
    // 计算当前块地址
    Addr current_block = (pfi.getAddr() % hotZoneSize) / blkSize;//blkSize:64
    // 每个区域的行数
    uint64_t lines_per_zone = hotZoneSize / blkSize;

    // 获取当前、前一个和后一个块的访问图条目
    AccessMapEntry *am_entry_curr = getAccessMapEntry(am_addr, is_secure);
    AccessMapEntry *am_entry_prev = (am_addr > 0) ?
        getAccessMapEntry(am_addr-1, is_secure) : nullptr;
    AccessMapEntry *am_entry_next = (am_addr < (MaxAddr/hotZoneSize)) ?
        getAccessMapEntry(am_addr+1, is_secure) : nullptr;
    // 确保条目之间互不相同，且当前条目不为空
    assert(am_entry_curr != am_entry_prev);
    assert(am_entry_curr != am_entry_next);
    assert(am_entry_prev != am_entry_next);
    assert(am_entry_curr != nullptr);

    // 标记当前访问状态为已访问
    setEntryState(*am_entry_curr, current_block, AM_ACCESS);

    // 创建3个条目状态的连续副本，避免在寻找预取候选时进行边界检查
    std::vector<AccessMapState> states(3 * lines_per_zone);
    for (unsigned idx = 0; idx < lines_per_zone; idx += 1) {
        states[idx] =
            am_entry_prev != nullptr ? am_entry_prev->states[idx] : AM_INVALID;
        states[idx + lines_per_zone] = am_entry_curr->states[idx];
        states[idx + 2 * lines_per_zone] =
            am_entry_next != nullptr ? am_entry_next->states[idx] : AM_INVALID;
    }

    // 当前块在新向量中的索引
    Addr states_current_block = current_block + lines_per_zone;
    // 考虑步长1到lines_per_zone/2
    int max_stride = limitStride == 0 ? lines_per_zone / 2 : limitStride + 1;
    for (int stride = 1; stride < max_stride; stride += 1) {
        // 测试正向步长
        if (checkCandidate(states, states_current_block, stride)) {
            // 找到候选，当前块 - 步长
            Addr pf_addr;
            if (stride > current_block) {
                // 索引（current_block - stride）落在前一个区域，调整地址
                Addr blk = states_current_block - stride;
                pf_addr = (am_addr - 1) * hotZoneSize + blk * blkSize;
                setEntryState(*am_entry_prev, blk, AM_PREFETCH);
            } else {
                // 索引（current_block - stride）落在当前条目内
                Addr blk = current_block - stride;
                pf_addr = am_addr * hotZoneSize + blk * blkSize;
                setEntryState(*am_entry_curr, blk, AM_PREFETCH);
            }
            // 添加预取地址
            addresses.push_back(Queued::AddrPriority(pf_addr, 0));
            // 达到预取程度，退出循环
            if (addresses.size() == degree) {
                break;
            }
        }

        // 测试负向步长
        if (checkCandidate(states, states_current_block, -stride)) {
            // 找到候选，当前块 + 步长
            Addr pf_addr;
            if (current_block + stride >= lines_per_zone) {
                // 索引（current_block + stride）落在后一个区域，调整地址
                Addr blk = (states_current_block + stride) % lines_per_zone;
                pf_addr = (am_addr + 1) * hotZoneSize + blk * blkSize;
                setEntryState(*am_entry_next, blk, AM_PREFETCH);
            } else {
                // 索引（current_block + stride）落在当前条目内
                Addr blk = current_block + stride;
                pf_addr = am_addr * hotZoneSize + blk * blkSize;
                setEntryState(*am_entry_curr, blk, AM_PREFETCH);
            }
            // 添加预取地址
            addresses.push_back(Queued::AddrPriority(pf_addr, 0));
            // 达到预取程度，退出循环
            if (addresses.size() == degree) {
                break;
            }
        }
    }
}

AMPM::AMPM(const AMPMPrefetcherParams &p)
    : Queued(p), ampm(*p.ampm)
{
}

void
AMPM::calculatePrefetch(const PrefetchInfo &pfi,
    std::vector<AddrPriority> &addresses)
{
    ampm.calculatePrefetch(pfi, addresses);
}

void
AMPM::calculatePrefetch(const PrefetchInfo &pfi,
    std::vector<AddrPriority> &addresses, const PacketPtr &pkt)
{
}

} // namespace prefetch
} // namespace gem5
