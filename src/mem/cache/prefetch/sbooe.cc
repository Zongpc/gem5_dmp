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

#include "mem/cache/prefetch/sbooe.hh"

#include "debug/HWPrefetch.hh"
#include "params/SBOOEPrefetcher.hh"

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

// SBOOE 类的构造函数
// 参数:
//   p: SBOOEPrefetcherParams 的实例，包含必要的配置参数
SBOOE::SBOOE(const SBOOEPrefetcherParams &p)
    : Queued(p), // 使用参数初始化基类 Queued
      sequentialPrefetchers(p.sequential_prefetchers), // 初始化顺序预取器的数量
      scoreThreshold((p.sandbox_entries*p.score_threshold_pct)/100), // 计算并初始化分数阈值
      latencyBuffer(p.latency_buffer_size), // 初始化延迟缓冲区
      averageAccessLatency(0), latencyBufferSum(0), // 初始化平均访问延迟及其总和
      bestSandbox(NULL), // 将指向最佳沙箱的指针初始化为 NULL
      accesses(0) // 初始化访问计数为 0
{
    // 为定义的每一个顺序预取器初始化一个沙箱，范围从 -1 到 定义的顺序预取器数量
    for (int i = 0; i < sequentialPrefetchers; i++) {
        sandboxes.push_back(Sandbox(p.sandbox_entries, i-1));
    }
}

// SBOOE::Sandbox 类中的访问方法，用于处理地址访问操作
// 参数:
// Addr addr: 需要访问的地址
// Tick tick: 预期到达时间

void SBOOE::Sandbox::access(Addr addr, Tick tick)
{
    // 在FIFO队列中查找地址以更新分数
    for (const SandboxEntry &entry: entries) {
        if (entry.valid && entry.line == addr) {
            sandboxScore++; // 更新沙盒分数
            if (entry.expectedArrivalTick > curTick()) {
                lateScore++; // 如果预期到达时间晚于当前时间，更新延迟分数
            }
        }
    }

    // 在此沙盒中插入新的访问记录
    SandboxEntry entry; // 创建新的沙盒条目
    entry.valid = true; // 标记条目为有效
    entry.line = addr + stride; // 设置访问的地址（基于步长）
    entry.expectedArrivalTick = tick; // 设置预期到达时间
    entries.push_back(entry); // 将新条目添加到队列中
}

/*
 * SBOOE类的access方法用于处理访问请求。
 * 它遍历所有的sandbox，让每个sandbox处理相同的访问行，
 * 并根据处理结果选择最佳的sandbox。
 * 
 * 参数:
 * access_line - 要访问的行地址。
 * 
 * 返回:
 * 当访问次数达到sandbox数量时返回true，否则返回false。
 */
bool SBOOE::access(Addr access_line)
{
    // 遍历每个sandbox，让其处理访问行并更新最佳sandbox选择。
    for (Sandbox &sb : sandboxes) {
        // 让sandbox处理访问行，参数为当前周期加上平均访问延迟。
        sb.access(access_line, curTick() + averageAccessLatency);

        // 如果当前sandbox的评分为最高，更新最佳sandbox选择。
        if (bestSandbox == NULL || sb.score() > bestSandbox->score()) {
            bestSandbox = &sb;
        }
    }

    // 增加访问计数。
    accesses++;

    // 如果访问次数达到sandbox数量，返回true，表示已完成一轮访问处理。
    return (accesses >= sandboxes.size());
}

void SBOOE::notifyFill(const PacketPtr& pkt)
{
    // 当缓存填充时更新访问延迟统计信息
    // 该函数在缓存行填充时被调用，以便更新平均访问延迟统计信息
    // 它通过跟踪从请求发出到填充完成所经过的周期数来计算延迟

    // (1) 在需求地址列表中查找当前包的地址
    // (2) 计算地址需求到填充完成所经过的周期数（当前周期数减去需求提出时的周期数）
    // (3) 将计算得到的延迟周期数加入到延迟缓冲区（FIFO队列）中
    // (4) 根据新的延迟缓冲区内容，重新计算平均访问延迟

    // 使用地址查找需求列表中对应的提出时间
    auto it = demandAddresses.find(pkt->getAddr());

    // 如果找到了对应的地址需求
    if (it != demandAddresses.end()) {
        // 计算从提出需求到填充完成经过的周期数
        Tick elapsed_ticks = curTick() - it->second;

        // 如果延迟缓冲区已满，移除最旧的延迟值并更新延迟总和
        if (latencyBuffer.full()) {
            latencyBufferSum -= latencyBuffer.front();
        }
        // 将新的延迟值加入缓冲区
        latencyBuffer.push_back(elapsed_ticks);
        // 更新延迟总和
        latencyBufferSum += elapsed_ticks;

        // 根据新的缓冲区内容，更新平均访问延迟
        averageAccessLatency = latencyBufferSum / latencyBuffer.size();

        // 移除已经处理的需求地址
        demandAddresses.erase(it);
    }
}

// 计算预取地址
void SBOOE::calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses) {
    // 获取预取信息中的地址
    const Addr pfi_addr = pfi.getAddr();
    // 计算对应的内存块地址
    const Addr pfi_line = pfi_addr >> lBlkSize;

    // 查找需求地址是否已存在
    auto it = demandAddresses.find(pfi_addr);

    // 如果需求地址不存在，则插入地址及其访问时间
    if (it == demandAddresses.end()) {
        demandAddresses.insert(std::pair<Addr, Tick>(pfi_addr, curTick()));
    }

    // 检查是否完成评估
    const bool evaluationFinished = access(pfi_line);

    // 如果评估完成且最佳沙箱得分超过阈值，则计算预取地址并添加到列表中
    if (evaluationFinished && bestSandbox->score() > scoreThreshold) {
        // 计算预取的内存块地址
        Addr pref_line = pfi_line + bestSandbox->stride;
        // 将预取地址和优先级添加到列表中
        addresses.push_back(AddrPriority(pref_line << lBlkSize, 0));
    }
}

} // namespace prefetch
} // namespace gem5
