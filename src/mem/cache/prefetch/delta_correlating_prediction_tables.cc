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

#include "mem/cache/prefetch/delta_correlating_prediction_tables.hh"

#include "debug/HWPrefetch.hh"
#include "mem/cache/prefetch/associative_set_impl.hh"
#include "params/DCPTPrefetcher.hh"
#include "params/DeltaCorrelatingPredictionTables.hh"

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

// 构造函数：初始化DeltaCorrelatingPredictionTables对象
DeltaCorrelatingPredictionTables::DeltaCorrelatingPredictionTables(
    const DeltaCorrelatingPredictionTablesParams &p) : SimObject(p),
   // 初始化成员变量deltaBits和deltaMaskBits
    deltaBits(p.delta_bits), deltaMaskBits(p.delta_mask_bits),
   // 使用给定的参数初始化预测表
    table(p.table_assoc, p.table_entries, p.table_indexing_policy,
        p.table_replacement_policy, DCPTEntry(p.deltas_per_entry))
{
}

// 无效化DCPT条目。
//
// 该函数用于将DeltaCorrelatingPredictionTables中的DCPT条目置为无效状态。这包括：
// 1. 调用基类TaggedEntry的invalidate方法，进行初始无效化处理。
// 2. 清空并重置deltas数组，确保所有存储的差异值被清除，并通过填充零至满来预设其状态。
// 3. 将lastAddress重置为0，清除之前的相关地址信息。
void
DeltaCorrelatingPredictionTables::DCPTEntry::invalidate()
{
    // 调用基类方法进行初始无效化处理。
    TaggedEntry::invalidate();

    // 清空deltas数组，确保移除之前的所有数据。
    deltas.flush();
    // 通过不断填充零，直到deltas数组满为止，预设其状态。
    while (!deltas.full()) {
        deltas.push_back(0);
    }
    // 将lastAddress重置为0，清除之前的地址信息。
    lastAddress = 0;
}

// 在DeltaCorrelatingPredictionTables命名空间中定义DCPTEntry类的addAddress成员函数
void DeltaCorrelatingPredictionTables::DCPTEntry::addAddress(Addr address, unsigned int delta_bits)
{
    // 检查当前地址与上一个地址是否不同
    if ((address - lastAddress) != 0) {
        // 计算当前地址与上一个地址的差异（delta）
        Addr delta = address - lastAddress;

        // 考虑delta的符号位，计算最大的正向delta值
        Addr max_positive_delta = (1 << (delta_bits-1)) - 1;

        // 如果当前地址大于上一个地址，检查正向delta溢出
        if (address > lastAddress) {
            if (delta > max_positive_delta) {
                // 如果delta超过最大值，则设置为0，表示溢出
                delta = 0;
            }
        } else {
            // 如果上一个地址大于当前地址，检查负向delta溢出
            if (lastAddress - address > (max_positive_delta + 1)) {
                // 如果delta超过最大负值，则设置为0，表示溢出
                delta = 0;
            }
        }

        // 将计算得到的delta值添加到列表中，并更新lastAddress为当前地址
        deltas.push_back(delta);
        lastAddress = address;
    }
}

/**
 * 从DeltaCorrelatingPredictionTables的DCPTEntry类中获取候选地址
 * 
 * @param pfs 一个存储地址优先级对的向量，将被填充以预测地址
 * @param mask 用于地址匹配的掩码，用于比较两个最近的delta值的特定部分
 * 
 * 该函数通过查找历史delta值中的模式来生成候选地址，如果找到匹配的模式，
 * 则使用该模式之后的delta值进行地址预测
 */
void DeltaCorrelatingPredictionTables::DCPTEntry::getCandidates(
    std::vector<Queued::AddrPriority> &pfs, unsigned int mask) const
{
    // 确保deltas队列已满，以便有足够的数据进行预测
    assert(deltas.full());

    // 获取最近两个delta值
    const int delta_penultimate = *(deltas.end() - 2);
    const int delta_last = *(deltas.end() - 1);

    // 如果最近的delta值中有0，表示溢出，无法进行匹配，直接返回
    if (delta_last == 0 || delta_penultimate == 0) {
        return;
    }

    // 在deltas队列的前面部分查找与最近两个delta值匹配的模式
    auto it = deltas.begin();
    for (; it != (deltas.end() - 2); ++it) {
        const int prev_delta_penultimate = *it;
        const int prev_delta_last = *(it + 1);
        // 如果找到匹配的模式
        if ((prev_delta_penultimate >> mask) == (delta_penultimate >> mask) &&
            (prev_delta_last >> mask) == (delta_last >> mask)) {
            // 跳过匹配的delta对，使用之后的delta值进行地址预测
            it += 2;
            Addr addr = lastAddress;
            // 使用剩余的delta值逐个生成预测地址，并添加到pfs中
            while (it != deltas.end()) {
                const int pf_delta = *(it++);
                addr += pf_delta;
                pfs.push_back(Queued::AddrPriority(addr, 0));
            }
            break;
        }
    }
}
// 在DeltaCorrelatingPredictionTables类中，计算预取地址的函数
// 该函数根据提供的PrefetchInfo对象计算一组预取地址
// 参数:
//   - pfi: 一个const引用类型的PrefetchInfo对象，包含预取请求的信息
//   - addresses: 一个Queued::AddrPriority类型的vector，用于存储计算得到的预取地址
void DeltaCorrelatingPredictionTables::calculatePrefetch(
    const Base::PrefetchInfo &pfi,
    std::vector<Queued::AddrPriority> &addresses)
{
    // 检查PrefetchInfo对象中是否包含程序计数器（PC），如果没有，则打印调试信息并返回
    if (!pfi.hasPC()) {
        DPRINTF(HWPrefetch, "Ignoring request with no PC.\n");
        return;
    }

    // 从PrefetchInfo对象中获取请求的地址和程序计数器（PC）
    Addr address = pfi.getAddr();
    Addr pc = pfi.getPC();

    // 查找表项，这里的is_secure参数在findEntry函数中未使用，因为我们使用pc进行索引
    DCPTEntry *entry = table.findEntry(pc, false /* unused */);

    // 如果找到了表项，则添加地址并计算候选地址
    if (entry != nullptr) {
        entry->addAddress(address, deltaBits);
        // Delta关联操作，将地址添加到entry中，并使用delta掩码位获取候选地址
        entry->getCandidates(addresses, deltaMaskBits);
    } else {
        // 如果没有找到表项，则查找一个牺牲品（victim）并插入新的表项
        entry = table.findVictim(pc);

        // 插入新的表项，这里的false /* unused */参数未使用
        table.insertEntry(pc, false /* unused */, entry);

        // 初始化新插入表项的最后地址为当前地址
        entry->lastAddress = address;
    }
}

// DCPT构造函数
// 参数 p: DCPTPrefetcherParams对象的引用，用于初始化DCPT
// 说明: 该构造函数使用参数p来初始化Queued和dcpt成员变量
DCPT::DCPT(const DCPTPrefetcherParams &p)
    : Queued(p), dcpt(*p.dcpt)
{
}

// DCPT类的成员函数，用于计算预取地址和其优先级
//
// @param pfi PrefetchInfo的引用，提供预取操作的相关信息
// @param addresses 引用类型参数，存储计算得到的地址及其优先级
void DCPT::calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses)
{
    // 调用dcpt对象的calculatePrefetch方法，进行预取地址计算
    dcpt.calculatePrefetch(pfi, addresses);
}

void DCPT::calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses, const PacketPtr &pkt) {}


} // namespace prefetch
} // namespace gem5
