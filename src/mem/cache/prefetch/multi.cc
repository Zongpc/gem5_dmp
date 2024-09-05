/*
 * Copyright (c) 2014, 2019 ARM Limited
 * All rights reserved
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
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

#include "mem/cache/prefetch/multi.hh"

#include "params/MultiPrefetcher.hh"

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

// Multi类的构造函数，用于初始化Multi对象
// 该构造函数接收一个MultiPrefetcherParams对象p作为参数，用以配置Multi对象
// 参数:
//   p - 一个MultiPrefetcherParams对象，包含了一系列的预取器配置信息
Multi::Multi(const MultiPrefetcherParams &p)
    : Base(p), // 调用基类构造函数，用参数p进行初始化
        prefetchers(p.prefetchers.begin(), p.prefetchers.end()), // 初始化prefetchers成员，将参数p中的预取器列表复制过来
        lastChosenPf(0) // 初始化lastChosenPf成员为0，表示尚未选择任何预取器
{
}

// 为 Multi 类型的实例设置缓存
// 
// 参数：
//   _cache: 指向一个新的 BaseCache 对象，用于替换当前的缓存
//
// 说明：
//   本函数通过迭代当前所有预取器（prefetchers），并为它们每一个设置新的缓存(_cache)。
//   这样做是为了确保所有关联的预取器都能使用新的缓存策略，从而提升整体性能或满足新的业务需求。
void Multi::setCache(BaseCache *_cache)
{
    // 遍历预取器列表，为每个预取器设置新的缓存
    for (auto pf : prefetchers)
        pf->setCache(_cache);
}

/**
 * 计算下一个预取就绪时间的最小值
 * 
 * 此函数通过遍历所有的预取器（prefetchers），找出下一个预取操作就绪的最小时间点
 * 它用于确保系统能够在下一个预取操作就绪时及时处理，从而提高性能
 * 
 * @return Tick 返回下一个预取就绪时间的最小值
 */
Tick
Multi::nextPrefetchReadyTime() const
{
    // 初始化下一个就绪时间为最大值，以确保第一次比较时能正确设置最小值
    Tick next_ready = MaxTick;

    // 遍历预取器列表，寻找下一个预取就绪时间的最小值
    for (auto pf : prefetchers)
        // 更新下一个预取就绪时间的最小值
        next_ready = std::min(next_ready, pf->nextPrefetchReadyTime());

    // 返回计算出的下一个预取就绪时间的最小值
    return next_ready;
}

// 循环选择下一个预取器，直到找到一个准备好发出预取的预取器，或循环一周后无预取器准备好
PacketPtr
Multi::getPacket()
{
    // 更新最后选择的预取器索引，采用轮询策略
    lastChosenPf = (lastChosenPf + 1) % prefetchers.size();
    uint8_t pf_turn = lastChosenPf;

    // 遍历所有预取器，寻找当前有预取数据包准备好的预取器
    for (int pf = 0 ;  pf < prefetchers.size(); pf++) {
        // 检查当前预取器是否已经准备好进行下一次预取
        if (prefetchers[pf_turn]->nextPrefetchReadyTime() <= curTick()) {
            // 如果预取器准备好，获取其预取的数据包
            PacketPtr pkt = prefetchers[pf_turn]->getPacket();
            // 如果获取的数据包为空，抛出异常，因为预取器已经准备好但未能返回一个数据包
            panic_if(!pkt, "Prefetcher is ready but didn't return a packet.");
            // 增加预取统计信息
            prefetchStats.pfIssued++;
            issuedPrefetches++;
            // 返回获取到的预取数据包
            return pkt;
        }
        // 继续检查下一个预取器
        pf_turn = (pf_turn + 1) % prefetchers.size();
    }

    // 如果遍历完所有预取器后没有找到准备好发出预取的预取器，返回nullptr
    return nullptr;
}

} // namespace prefetch
} // namespace gem5
