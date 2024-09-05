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

#include "mem/cache/prefetch/signature_path_v2.hh"

#include <cassert>

#include "debug/HWPrefetch.hh"
#include "mem/cache/prefetch/associative_set_impl.hh"
#include "params/SignaturePathPrefetcherV2.hh"

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

// SignaturePathV2 类的构造函数，用于初始化SignaturePathV2对象
// 该构造函数使用了SignaturePathPrefetcherV2Params结构体中的参数来初始化其成员变量
SignaturePathV2::SignaturePathV2(const SignaturePathPrefetcherV2Params &p)
    : SignaturePath(p), // 初始化基类SignaturePath
      // 初始化全局历史寄存器，使用参数中提供的条目、索引策略和替换策略
        globalHistoryRegister(p.global_history_register_entries,
                            p.global_history_register_entries,
                            p.global_history_register_indexing_policy,
                            p.global_history_register_replacement_policy,
                            GlobalHistoryEntry())
{
}

// 处理签名表未命中时的行为
// 当前函数旨在根据全局历史寄存器（GHR）中的条目来更新签名表
// 当签名表中没有当前块的条目时，尝试在GHR中查找，并将找到的签名添加到签名表中
// 如果GHR中没有找到对应的签名，则将当前块作为新签名添加到签名表中
//
// 参数:
// - current_block: 当前处理的块地址
// - new_signature: 引用传递，用于存储新找到或创建的签名
// - new_conf: 引用传递，用于存储新签名的置信度
// - new_stride: 引用传递，用于存储新签名的步长（delta）
void SignaturePathV2::handleSignatureTableMiss(stride_t current_block,
    signature_t &new_signature, double &new_conf, stride_t &new_stride)
{
    // 初始化标志变量，用于指示是否在GHR中找到了对应的签名
    bool found = false;

    // 获取GHR中所有可能的条目，以便在其中搜索匹配的签名
    // 由于GHR是全相联的，因此这里传入的任何值（这里传入0）都会返回所有条目
    std::vector<GlobalHistoryEntry *> all_ghr_entries =
             globalHistoryRegister.getPossibleEntries(0 /* any value works */);

    // 遍历GHR中的所有条目，尝试找到与当前块匹配的签名
    for (auto gh_entry : all_ghr_entries) {
        // 如果GHR条目的上一个块地址加上其步长等于当前块地址，则认为找到了匹配的签名
        if (gh_entry->lastBlock + gh_entry->delta == current_block) {
            // 更新签名、置信度和步长
            new_signature = gh_entry->signature;
            new_conf = gh_entry->confidence;
            new_stride = gh_entry->delta;
            // 标记为已找到
            found = true;
            // 访问GHR中的该条目，以更新其使用情况等信息
            globalHistoryRegister.accessEntry(gh_entry);
            // 找到后立即退出循环
            break;
        }
    }
    // 如果在GHR中没有找到匹配的签名，则将当前块作为新签名添加
    if (!found) {
        // 将当前块地址用作新签名、置信度设为1.0（完全置信），步长设为当前块地址
        new_signature = current_block;
        new_conf = 1.0;
        new_stride = current_block;
    }
}

// 计算预取置信度
// 该函数用于根据签名模式和预取步长模式的匹配程度，计算出预取操作的置信度
// 当签名模式的计数为0时，置信度为0，因为没有足够的数据支持计算
// 置信度的计算公式为：(有效的预取次数 / 发出的预取次数) * (预取步长模式的计数 / 签名模式的计数)
double SignaturePathV2::calculateLookaheadConfidence(
        PatternEntry const &sig,  // 签名模式，包含计数等信息
        PatternStrideEntry const &lookahead) const  // 预取步长模式，包含计数等信息
{
    if (sig.counter == 0) return 0.0;  // 如果签名模式的计数为0，则返回置信度0
    return (((double) usefulPrefetches) / issuedPrefetches) *  // 计算有效的预取比例
            (((double) lookahead.counter) / sig.counter);  // 计算预取步长模式与签名模式计数之比
}

/*
 * 计算预取置信度。
 * 
 * 该函数用于根据签名路径条目和相应的跨距信息计算预取操作的置信度。
 * 置信度是基于模式出现的次数与签名计数的比值来计算的，反映了预取操作的可靠程度。
 * 
 * @param sig 指纹条目，包含计数等信息。
 * @param entry 模式跨距条目，包含计数等具体信息。
 * 
 * @return 返回计算得到的预取置信度。如果签名计数为0，表示无法计算置信度，返回0.0。
 */
double SignaturePathV2::calculatePrefetchConfidence(PatternEntry const &sig,
        PatternStrideEntry const &entry) const
{
    // 检查签名计数是否为0，防止除以0的错误。
    if (sig.counter == 0) return 0.0;
    
    // 计算并返回预取置信度。
    return ((double) entry.counter) / sig.counter;
}

/**
 * 增加模式条目和步长模式条目的计数器。
 * 如果计数器达到饱和状态，则将其减半。
 * 
 * @param pattern_entry 模式条目，包含计数器和步长条目数组。
 * @param pstride_entry 步长模式条目，包含其自身的计数器。
 */
void SignaturePathV2::increasePatternEntryCounter(
        PatternEntry &pattern_entry, PatternStrideEntry &pstride_entry)
{
    // 检查模式条目计数器是否达到饱和状态
    if (pattern_entry.counter.isSaturated()) {
        // 如果达到饱和状态，减半计数器值
        pattern_entry.counter >>= 1;
        // 同样，减半模式条目中所有步长条目的计数器值
        for (auto &entry : pattern_entry.strideEntries) {
            entry.counter >>= 1;
        }
    }
    // 检查步长模式条目计数器是否达到饱和状态
    if (pstride_entry.counter.isSaturated()) {
        // 如果达到饱和状态，同样减半计数器值
        pattern_entry.counter >>= 1;
        // 同样，减半模式条目中所有步长条目的计数器值
        for (auto &entry : pattern_entry.strideEntries) {
            entry.counter >>= 1;
        }
    }
    // 增加模式条目和步长模式条目的计数器值
    pattern_entry.counter++;
    pstride_entry.counter++;
}

// 处理跨页预取的签名路径版本2
// 该函数使用全局历史记录（GHR）来记录和预测跨页的访问模式
// 参数:
//   signature: 访问模式的签名
//   last_offset: 上一个块的偏移量
//   delta: 偏移量的增量
//   path_confidence: 路径置信度，用于评估预测的可靠性
void SignaturePathV2::handlePageCrossingLookahead(signature_t signature,
            stride_t last_offset, stride_t delta, double path_confidence)
{
    // 总是使用替换策略来分配新的条目，因为它们都是唯一的，GHR中没有“命中”
    GlobalHistoryEntry *gh_entry = globalHistoryRegister.findVictim(0);
    assert(gh_entry != nullptr);
    // 任何地址值都可以，因为它从不被使用
    globalHistoryRegister.insertEntry(0, false, gh_entry);

    // 更新全局历史记录条目的信息
    gh_entry->signature = signature;
    gh_entry->lastBlock = last_offset;
    gh_entry->delta = delta;
    gh_entry->confidence = path_confidence;
}

} // namespace prefetch
} // namespace gem5
