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

#include "mem/cache/prefetch/slim_ampm.hh"

#include "params/SlimAMPMPrefetcher.hh"

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

// 构造函数：初始化SlimAMPM类的实例
// 参数 p: SlimAMPMPrefetcherParams类型的引用，包含必要的参数用于初始化
SlimAMPM::SlimAMPM(const SlimAMPMPrefetcherParams &p)
  : Queued(p), // 初始化基类Queued
    ampm(*p.ampm), // 初始化ampm成员变量
    dcpt(*p.dcpt) // 初始化dcpt成员变量
{
}

// SlimAMPM类中的预取地址计算函数
// 函数首先尝试通过dcpt成员进行预取计算，若未获得预取地址，则利用ampm成员进行二次计算
// 结合两种预取机制，以提升预取的准确度与范围

void
SlimAMPM::calculatePrefetch(const PrefetchInfo &pfi,
                  std::vector<AddrPriority> &addresses)
{
    // 使用dcpt策略计算预取地址
    dcpt.calculatePrefetch(pfi, addresses);
    
    // 若dcpt未能找到预取地址，尝试使用ampm策略进行计算
    if (addresses.empty()) {
        ampm.calculatePrefetch(pfi, addresses);
    }
}

} // namespace prefetch
} // namespace gem5
