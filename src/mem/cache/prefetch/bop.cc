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

#include "mem/cache/prefetch/bop.hh"

#include "debug/HWPrefetch.hh"
#include "params/BOPPrefetcher.hh"

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

// BOP类的构造函数
// 参数p是BOPPrefetcherParams类型的对象，包含BOP预取器的配置参数
BOP::BOP(const BOPPrefetcherParams &p)
    : Queued(p), // 基类初始化
      scoreMax(p.score_max), // 最大分数
      roundMax(p.round_max), // 最大轮数
      badScore(p.bad_score), // 不良分数
      rrEntries(p.rr_size), // 循环缓冲区大小
      tagMask((1 << p.tag_bits) - 1), // 标签掩码
      delayQueueEnabled(p.delay_queue_enable), // 延迟队列启用状态
      delayQueueSize(p.delay_queue_size), // 延迟队列大小
      delayTicks(cyclesToTicks(p.delay_queue_cycles)), // 延迟周期数
      delayQueueEvent([this]{ delayQueueEventWrapper(); }, name()), // 延迟队列事件
      issuePrefetchRequests(false), // 是否发出预取请求
      bestOffset(1), // 最佳偏移量初始化为1
      phaseBestOffset(0), // 当前阶段最佳偏移量
      bestScore(0), // 最佳分数
      round(0) // 当前轮数
{
    // 确保循环缓冲区的大小是2的幂
    if (!isPowerOf2(rrEntries)) {
        fatal("%s: number of RR entries is not power of 2\n", name());
    }
    // 确保缓存行大小是2的幂
    if (!isPowerOf2(blkSize)) {
        fatal("%s: cache line size is not power of 2\n", name());
    }
    // 如果启用了负偏移量，则确保偏移量列表大小是偶数
    if (!(p.negative_offsets_enable && (p.offset_list_size % 2 == 0))) {
        fatal("%s: negative offsets enabled with odd offset list size\n",
            name());
    }

    // 初始化循环缓冲区
    rrLeft.resize(rrEntries);
    rrRight.resize(rrEntries);

    // 按照论文中的实现，生成指定数量的偏移量
    // 这些偏移量的形式为2^i * 3^j * 5^k，其中i,j,k >= 0
    const int factors[] = { 2, 3, 5 };
    unsigned int i = 0;
    int64_t offset_i = 1;

    // 生成偏移量列表
    while (i < p.offset_list_size)
    {
        int64_t offset = offset_i;

        // 对每个偏移量进行质因数分解
        for (int n : factors) {
            while ((offset % n) == 0) {
                offset /= n;
            }
        }

        // 如果偏移量可以完全分解，则添加到列表中
        if (offset == 1) {
            offsetsList.push_back(OffsetListEntry(offset_i, 0));
            i++;
            // 如果启用了负偏移量，也将其添加到列表中
            if (p.negative_offsets_enable)  {
                offsetsList.push_back(OffsetListEntry(-offset_i, 0));
                i++;
            }
        }

        offset_i++;
    }

    // 设置偏移量列表迭代器的初始位置
    offsetsListIterator = offsetsList.begin();
}

// 处理延迟队列中的事件
void BOP::delayQueueEventWrapper()
{
    // 当延迟队列不为空，且队列头部的事件处理时间小于等于当前时间时，执行事件处理逻辑
    while (!delayQueue.empty() &&
            delayQueue.front().processTick <= curTick())
    {
        // 获取事件关联的地址
        Addr addr_x = delayQueue.front().baseAddr;
        // 将地址插入到RR（快速重试队列）中，使用RRWay::Left方式插入
        insertIntoRR(addr_x, RRWay::Left);
        // 从延迟队列中移除已经处理的事件
        delayQueue.pop_front();
    }

    // 如果延迟队列中还有事件，为下一个事件安排处理时间
    if (!delayQueue.empty()) {
        // 安排下一个事件的处理时间
        schedule(delayQueueEvent, delayQueue.front().processTick);
    }
}

// 计算给定地址和方式的哈希值
// 该函数用于生成一个基于地址和路数的哈希值，用于在一组条目中选择一个条目
// 参数:
//   addr: 需要进行哈希计算的地址
//   way: 需要进行右移的位数，用于调整哈希值的生成
// 返回值:
//   返回计算出的哈希值，用于索引选择
unsigned int
BOP::hash(Addr addr, unsigned int way) const
{
    // 第一次右移，根据路数调整地址，产生初步哈希值
    Addr hash1 = addr >> way;
    
    // 第二次右移，基于条目数量的对数，进一步调整哈希值
    Addr hash2 = hash1 >> floorLog2(rrEntries);
    
    // 通过异或操作合并两次哈希结果，并与条目数量减一的结果进行与操作，确保哈希值在有效范围内
    return (hash1 ^ hash2) & (Addr)(rrEntries - 1);
}

// 在BOP类中插入一个地址到RR的特定方式中
void BOP::insertIntoRR(Addr addr, unsigned int way)
{
    // 根据提供的方式选择相应的RR进行插入
    switch (way) {
        // 如果方式为Left，则插入到rrLeft中
        case RRWay::Left:
            rrLeft[hash(addr, RRWay::Left)] = addr;
            break;
        // 如果方式为Right，则插入到rrRight中
        case RRWay::Right:
            rrRight[hash(addr, RRWay::Right)] = addr;
            break;
    }
}

// 在BOP类中插入一个地址到延迟队列中
// 该函数的目的是将地址加入到延迟队列中，以便在指定的延迟周期后进行处理
// 参数:
//   x - 需要插入到延迟队列中的地址
// 返回值: 无
void BOP::insertIntoDelayQueue(Addr x)
{
    // 检查延迟队列是否已满
    if (delayQueue.size() == delayQueueSize) {
        // 如果队列已满，则不执行任何操作，直接返回
        return;
    }

    // 计算处理周期，当前周期加上延迟周期
    Tick process_tick = curTick() + delayTicks;

    // 将地址和处理周期添加到延迟队列条目中
    delayQueue.push_back(DelayQueueEntry(x, process_tick));

    // 如果延迟队列事件未被调度，则调度该事件
    if (!delayQueueEvent.scheduled()) {
        schedule(delayQueueEvent, process_tick);
    }
}

// 重置分数
void BOP::resetScores()
{
    // 遍历偏移量列表，将每个成员的分数重置为0
    for (auto& it : offsetsList) {
        it.second = 0;
    }
}

/**
 * 计算给定地址的标签部分。
 * 
 * 标签部分是通过将地址右移块大小（blkSize）的位数，并与标签掩码（tagMask）进行位与操作得到的。
 * 这个操作用于从一个地址中提取出标签值，该标签值用于缓存的索引或其他目的。
 * 
 * @param addr 要计算标签的地址。
 * @return 返回计算得到的标签值。
 */
inline Addr
BOP::tag(Addr addr) const
{
    return (addr >> blkSize) & tagMask;
}

/**
 * 在左或右集合中测试给定地址是否存在。
 * 
 * 该函数通过检查给定地址是否存在于`rrLeft`或`rrRight`集合中，
 * 来判断地址是否已经通过“或”操作符被记录。
 * 
 * @param addr 要测试的地址。
 * @return 如果地址存在于`rrLeft`或`rrRight`中，返回true；否则返回false。
 */
bool BOP::testRR(Addr addr) const {
    // 遍历`rrLeft`集合，检查地址是否存在。
    for (auto& it : rrLeft) {
        if (it == addr) {
            return true;
        }
    }

    // 遍历`rrRight`集合，检查地址是否存在。
    for (auto& it : rrRight) {
        if (it == addr) {
            return true;
        }
    }

    // 如果地址既不存在于`rrLeft`也不存在于`rrRight`，返回false。
    return false;
}

// 在BOP类中，根据给定的地址x进行最佳偏移量学习。
// 该函数通过迭代偏移量列表，查找与地址x相关的偏移量，并根据找到的情况更新偏移量的分数。
// 如果满足一定的条件，会更新最佳偏移量，并重置相关参数以开始新的学习阶段。
void BOP::bestOffsetLearning(Addr x) {
    // 当前迭代到的偏移量地址
    Addr offset_addr = (*offsetsListIterator).first;
    // 用于查找的地址，通过从输入地址x中减去当前偏移量地址获得
    Addr lookup_addr = x - offset_addr;

    // 如果在RR表中找到了对应的地址，增加这个偏移量的分数
    if (testRR(lookup_addr)) {
        DPRINTF(HWPrefetch, "Address %#lx found in the RR table\n", x);
        (*offsetsListIterator).second++;
        // 如果当前偏移量的分数超过了之前的最佳分数，更新最佳分数和对应的偏移量
        if ((*offsetsListIterator).second > bestScore) {
            bestScore = (*offsetsListIterator).second;
            phaseBestOffset = (*offsetsListIterator).first;
            DPRINTF(HWPrefetch, "New best score is %lu\n", bestScore);
        }
    }

    // 继续迭代下一个偏移量
    offsetsListIterator++;

    // 所有偏移量都被访问了一遍，意味着一个学习阶段结束。检查并更新相关参数。
    if (offsetsListIterator == offsetsList.end()) {
        offsetsListIterator = offsetsList.begin();
        round++;

        // 检查是否需要更新最佳偏移量：
        // (1) 如果任一分数达到SCORE_MAX
        // (2) 如果迭代轮数达到ROUND_MAX
        if ((bestScore >= scoreMax) || (round == roundMax)) {
            bestOffset = phaseBestOffset;
            round = 0;
            bestScore = 0;
            phaseBestOffset = 0;
            resetScores();
            issuePrefetchRequests = true;
        } else if (bestScore <= badScore) {
            issuePrefetchRequests = false;
        }
    }
}

// 计算并添加预取地址到指定的数据结构中
// 
// @param pfi 预取信息对象，包含访问地址等信息
// @param addresses 用于存储待预取地址的向量，每个元素包含地址和优先级
void BOP::calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses)
{
    // 获取当前访问地址
    Addr addr = pfi.getAddr();
    // 提取地址的标记部分
    Addr tag_x = tag(addr);

    // 根据配置决定是否使用延迟队列
    if (delayQueueEnabled) {
        // 将标记插入到延迟队列中
        insertIntoDelayQueue(tag_x);
    } else {
        // 否则，插入到RR（循环替换）队列中，指定插入位置为左侧
        insertIntoRR(tag_x, RRWay::Left);
    }

    // 更新最佳偏移量的学习逻辑
    // 这里通过遍历特定偏移量来更新分数、最佳分数和当前最佳偏移量
    bestOffsetLearning(tag_x);

    // 由于这是一个一级预取器，它每次访问最多生成一个预取请求
    if (issuePrefetchRequests) {
        // 计算预取地址，基于最佳偏移量和块大小
        Addr prefetch_addr = addr + (bestOffset << lBlkSize);
        // 将计算出的预取地址添加到待预取地址向量中，优先级设为0
        addresses.push_back(AddrPriority(prefetch_addr, 0));
        // 调试打印生成的预取地址
        DPRINTF(HWPrefetch, "Generated prefetch %#lx\n", prefetch_addr);
    }
}

// 通知填充操作
void BOP::notifyFill(const PacketPtr& pkt)
{
    // 仅当pkt是硬件预取时才进行后续操作
    if (!pkt->cmd.isHWPrefetch()) return;

    // 计算pkt地址对应的tag
    Addr tag_y = tag(pkt->getAddr());

    // 如果允许发出预取请求
    if (issuePrefetchRequests) {
        // 将tag插入到重置寄存器的正确位置
        insertIntoRR(tag_y - bestOffset, RRWay::Right);
    }
}

} // namespace prefetch
} // namespace gem5
