/*
 * Copyright (c) 2005 The Regents of The University of Michigan
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

/**
 * @file
 * Describes a tagged prefetcher based on template policies.
 */

#include "mem/cache/prefetch/tagged.hh"

#include "params/TaggedPrefetcher.hh"

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

// Tagged构造函数
// 参数:
//   p - 一个TaggedPrefetcherParams类型的引用，用于初始化参数
// 
// 说明:
//   - 使用Queued类的构造函数和degree成员变量初始化本类
//   - degree根据传入的参数p中的degree成员进行初始化
Tagged::Tagged(const TaggedPrefetcherParams &p)
    : Queued(p), degree(p.degree)
{

}

/**
 * 根据给定的预取信息计算并添加预取地址
 *
 * 本函数基于给定的预取信息（PFI），计算出一系列预取地址，并将它们添加到地址列表中
 * 预取地址通过块地址加上一个基于预取程度（degree）和块大小（blkSize）的偏移量来计算
 *
 * @param pfi 预取信息对象，包含需要预取数据的相关信息
 * @param addresses 一个地址和优先级的列表，用于存储计算出的预取地址
 */
void Tagged::calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses)
{
    // 计算块地址，即预取信息指定地址对应的缓存块地址
    Addr blkAddr = blockAddress(pfi.getAddr());

    // 循环遍历预取程度范围内的每个偏移量
    for (int d = 1; d <= degree; d++) {
        // 计算新的预取地址
        Addr newAddr = blkAddr + d * blkSize;
        // 将新的预取地址及其优先级（0表示最低优先级）添加到地址列表中
        addresses.push_back(AddrPriority(newAddr, 0));
    }
}

} // namespace prefetch
} // namespace gem5
