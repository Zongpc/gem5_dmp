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

#include "mem/cache/prefetch/signature_path.hh"

#include <cassert>
#include <climits>

#include "debug/HWPrefetch.hh"
#include "mem/cache/prefetch/associative_set_impl.hh"
#include "params/SignaturePathPrefetcher.hh"

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

// 构造函数：初始化SignaturePath对象
// 参数 p: SignaturePathPrefetcherParams的引用，用于传递配置参数
SignaturePath::SignaturePath(const SignaturePathPrefetcherParams &p)
    : Queued(p),  // 基类初始化
        stridesPerPatternEntry(p.strides_per_pattern_entry),  // 每个模式条目的步数
        signatureShift(p.signature_shift),  // 签名位移
        signatureBits(p.signature_bits),  // 签名位数
        prefetchConfidenceThreshold(p.prefetch_confidence_threshold),  // 预取置信度阈值
        lookaheadConfidenceThreshold(p.lookahead_confidence_threshold),  // 瞻前置信度阈值
        signatureTable(p.signature_table_assoc, p.signature_table_entries,
                        p.signature_table_indexing_policy,
                        p.signature_table_replacement_policy),  // 签名表初始化
        patternTable(p.pattern_table_assoc, p.pattern_table_entries,
                    p.pattern_table_indexing_policy,
                    p.pattern_table_replacement_policy,
                    PatternEntry(stridesPerPatternEntry, p.num_counter_bits))  // 模式表初始化
{
    // 确保预取置信度阈值在0到1之间
    fatal_if(prefetchConfidenceThreshold < 0,
        "The prefetch confidence threshold must be greater than 0\n");
    fatal_if(prefetchConfidenceThreshold > 1,
        "The prefetch confidence threshold must be less than 1\n");

    // 确保瞻前置信度阈值在0到1之间
    fatal_if(lookaheadConfidenceThreshold < 0,
        "The lookahead confidence threshold must be greater than 0\n");
    fatal_if(lookaheadConfidenceThreshold > 1,
        "The lookahead confidence threshold must be less than 1\n");
}

// 从SignaturePath的PatternEntry中获取或选择一个PatternStrideEntry
// 如果给定的stride已经存在，则直接返回对应的PatternStrideEntry
// 如果不存在，则采用特定的替换算法，选择一个counter值最小的条目进行复用
SignaturePath::PatternStrideEntry &
SignaturePath::PatternEntry::getStrideEntry(stride_t stride)
{
    // 尝试查找已存在的stride条目
    PatternStrideEntry *pstride_entry = findStride(stride);
    if (pstride_entry == nullptr) {
        // 具体的替换算法实现
        // 选择counter值最小的条目（victim），并重置所有条目的counter

        // 初始化victim为第一个条目，其counter值设为最大值
        // 这样做是为了确保在所有counter都为最大值时，能够选择第一个条目作为victim
        PatternStrideEntry *victim_pstride_entry = &(strideEntries[0]);

        // 初始化当前最小counter值为最大值，以便在循环中找到真正的最小值
        unsigned long current_counter = ULONG_MAX;
        // 遍历所有stride条目
        for (auto &entry : strideEntries) {
            // 如果找到一个counter更小的条目，则更新victim和当前最小counter值
            if (entry.counter < current_counter) {
                victim_pstride_entry = &entry;
                current_counter = entry.counter;
            }
            // 减少每个条目的counter值，为下次替换做准备
            entry.counter--;
        }
        // 使用选中的victim条目来存储新的stride值
        pstride_entry = victim_pstride_entry;
        // 重置counter，以便新条目可以使用
        pstride_entry->counter.reset();
        // 设置新的stride值
        pstride_entry->stride = stride;
    }
    // 返回获取或替换后的PatternStrideEntry
    return *pstride_entry;
}

// 添加预取地址到指定路径
// 
// 本函数用于根据给定的起始地址(ppn)、上一个块地址(last_block)、步长(delta)、路径置信度(path_confidence)、
// 签名(signature)和是否安全(is_secure)来计算下一个预取地址，并将其添加到地址向量(addresses)中。
// 
// 参数:
// - ppn: 预取页号
// - last_block: 上一个块的偏移地址
// - delta: 地址变化的步长
// - path_confidence: 路径置信度，用于决定预取的可靠性
// - signature: 本次预取的签名，用于标识预取模式
// - is_secure: 标识本次预取是否是安全的
// - addresses: 存储预取地址的向量，本函数将新的预取地址添加到该向量中
void SignaturePath::addPrefetch(Addr ppn, stride_t last_block, stride_t delta, double path_confidence, signature_t signature, bool is_secure, std::vector<AddrPriority> &addresses)
{
    // 计算下一个预取块的偏移地址
    stride_t block = last_block + delta;

    // 定义预取页号和块偏移变量
    Addr pf_ppn;
    stride_t pf_block;

    // 处理跨页情况
    if (block < 0) {
        // 计算向负方向跨过的页数
        stride_t num_cross_pages = 1 + (-block) / (pageBytes/blkSize);
        // 如果跨过的页数超过了起始页号，忽略本次预取
        if (num_cross_pages > ppn) {
            return;
        }
        // 更新预取的页号和块偏移
        pf_ppn = ppn - num_cross_pages;
        pf_block = block + (pageBytes/blkSize) * num_cross_pages;
        // 处理跨页预取的特殊情况
        handlePageCrossingLookahead(signature, last_block, delta, path_confidence);
    } else if (block >= (pageBytes/blkSize)) {
        // 计算向正方向跨过的页数
        stride_t num_cross_pages = block / (pageBytes/blkSize);
        // 如果跨过的页数超过了最大地址，忽略本次预取
        if (MaxAddr/pageBytes < (ppn + num_cross_pages)) {
            return;
        }
        // 更新预取的页号和块偏移
        pf_ppn = ppn + num_cross_pages;
        pf_block = block - (pageBytes/blkSize) * num_cross_pages;
        // 处理跨页预取的特殊情况
        handlePageCrossingLookahead(signature, last_block, delta, path_confidence);
    } else {
        // 如果没有跨页，直接更新预取的页号和块偏移
        pf_ppn = ppn;
        pf_block = block;
    }

    // 根据预取的页号和块偏移计算得到新的预取地址
    Addr new_addr = pf_ppn * pageBytes;
    new_addr += pf_block * (Addr)blkSize;

    // 打印预取地址的调试信息
    DPRINTF(HWPrefetch, "Queuing prefetch to %#x.\n", new_addr);
    // 将新的预取地址添加到地址向量中，优先级为0
    addresses.push_back(AddrPriority(new_addr, 0));
}

// 处理签名表未命中的情况
// 当前函数的目的是在签名表中找不到对应的签名时，用当前块作为新的签名，并初始化其置信度和步长。
// 参数:
//   current_block - 当前的内存块，将被用作新的签名。
//   new_signature - 引用传递，将被设置为新的签名（即current_block）。
//   new_conf - 引用传递，将被设置为新的置信度，初始化为1.0，表示完全置信。
//   new_stride - 引用传递，将被设置为新的步长，初始化为current_block的大小，表示下一次访问的间隔。
void SignaturePath::handleSignatureTableMiss(stride_t current_block,
    signature_t &new_signature, double &new_conf, stride_t &new_stride)
{
    // 将当前块作为新的签名
    new_signature = current_block;
    // 初始化新的置信度为1.0
    new_conf = 1.0;
    // 初始化新的步长为当前块的大小
    new_stride = current_block;
}

/**
 * 增加模式条目计数器
 * 
 * 该函数的目的是为了增加与特定模式条目相关联的计数器值
 * 它通过引用接收两个参数：一个PatternEntry对象和一个PatternStrideEntry对象
 * 通过增加PatternStrideEntry对象中的计数器，函数实现了其功能
 * 
 * @param pattern_entry 模式条目对象的引用，用于访问和修改条目相关数据
 * @param pstride_entry PatternStrideEntry对象的引用，其计数器将被增加
 */
void SignaturePath::increasePatternEntryCounter(
        PatternEntry &pattern_entry, PatternStrideEntry &pstride_entry)
{
    pstride_entry.counter++;
}

/*
 * 更新模式表以反映给定签名和步长的新出现。
 * 此函数用于更新模式表中特定签名的条目，记录其新出现的步长。
 *
 * 参数:
 * - signature: 签名值，用于定位模式表中的对应条目。
 * - stride: 步长值，表示从当前地址到下一个签名的偏移量。
 */
void SignaturePath::updatePatternTable(Addr signature, stride_t stride)
{
    // 确认步长不为零，因为零步长在地址预测中没有实际意义。
    assert(stride != 0);

    // 根据签名访问并获取模式表中的条目。
    PatternEntry &p_entry = getPatternEntry(signature);
    // 根据步长访问并获取模式条目中的步长条目。
    PatternStrideEntry &ps_entry = p_entry.getStrideEntry(stride);
    // 增加模式条目的计数，反映该模式的出现。
    increasePatternEntryCounter(p_entry, ps_entry);
}

// 根据给定的参数获取或更新签名条目。
// 如果找到了对应的签名条目，则更新其访问信息并计算步长；
// 如果未找到，则插入新的签名条目并处理未命中情况。
// 参数:
// - ppn: 物理页面号
// - is_secure: 是否为安全模式
// - block: 当前块地址
// - miss: 是否发生未命中的标志
// - stride: 计算得到的步长
// - initial_confidence: 初始置信度
// 返回:
// - 访问或插入的签名条目引用
SignaturePath::SignatureEntry &
SignaturePath::getSignatureEntry(Addr ppn, bool is_secure,
        stride_t block, bool &miss, stride_t &stride,
        double &initial_confidence)
{
    // 尝试从签名表中查找对应的签名条目
    SignatureEntry* signature_entry = signatureTable.findEntry(ppn, is_secure);
    if (signature_entry != nullptr) {
        // 如果找到签名条目，访问它以更新其访问信息
        signatureTable.accessEntry(signature_entry);
        // 未命中标志设为false
        miss = false;
        // 计算当前块地址与上一个块地址的差值作为步长
        stride = block - signature_entry->lastBlock;
    } else {
        // 如果未找到签名条目，从签名表中找一个空闲位置（牺牲者）
        signature_entry = signatureTable.findVictim(ppn);
        assert(signature_entry != nullptr);

        // 处理签名表未命中的情况，设置签名、初始置信度和步长
        handleSignatureTableMiss(block, signature_entry->signature,
            initial_confidence, stride);

        // 在签名表中插入新的签名条目
        signatureTable.insertEntry(ppn, is_secure, signature_entry);
        // 未命中标志设为true
        miss = true;
    }
    // 更新签名条目的最后一个块地址
    signature_entry->lastBlock = block;
    // 返回访问或插入的签名条目引用
    return *signature_entry;
}

// 根据签名获取PatternEntry对象的引用
// 如果签名在模式表中已存在，则访问该表项并返回其引用
// 如果签名在模式表中不存在，则在模式表中找到一个牺牲品，插入新的表项，并返回其引用
SignaturePath::PatternEntry &
SignaturePath::getPatternEntry(Addr signature)
{
    // 尝试在模式表中查找对应的PatternEntry
    PatternEntry* pattern_entry = patternTable.findEntry(signature, false);
    
    if (pattern_entry != nullptr) {
        // 签名找到
        // 访问表项以更新其访问频率或其它状态信息
        patternTable.accessEntry(pattern_entry);
    } else {
        // 签名未找到，需要找到一个牺牲品以插入新的表项
        pattern_entry = patternTable.findVictim(signature);
        // 确保找到了一个可用的牺牲品
        assert(pattern_entry != nullptr);

        // 插入新的表项到模式表中
        patternTable.insertEntry(signature, false, pattern_entry);
    }
    // 返回PatternEntry对象的引用
    return *pattern_entry;
}

// 计算预取置信度
// 
// 该函数用于根据签名和步长信息计算预取操作的置信度置信度反映了预取操作的可靠程度，
// 帮助系统决定是否执行预取操作高置信度的预取可以有效提升性能，而低置信度的预取
// 可能会导致性能下降或产生其他不利影响
// 
// 参数:
// sig - 签名条目，用于标识特定的模式或特征
// entry - 包含步长信息和计数器信息的条目，用于计算饱和度
// 
// 返回值:
// 返回计算得到的预取置信度，该值由条目中的计数器饱和度决定饱和度越高，置信度越高
double SignaturePath::calculatePrefetchConfidence(PatternEntry const &sig,
        PatternStrideEntry const &entry) const
{
    // 根据entry中的计数器信息计算饱和度，作为预取置信度的值
    return entry.counter.calcSaturation();
}

/**
 * 计算前瞻置信度
 * 
 * 该函数根据特征条目和前瞻步长信息计算前瞻置信度
 * 前瞻置信度是评估当前特征与下一个特征匹配的可能性
 * 
 * @param sig 特征条目，包含当前特征的信息
 * @param lookahead 前瞻步长信息，包含前瞻特征的计数信息
 * @return 返回计算得到的前瞻置信度
 */
double SignaturePath::calculateLookaheadConfidence(PatternEntry const &sig,
        PatternStrideEntry const &lookahead) const
{
    // 初始化前瞻置信度为前瞻特征的计数饱和度
    double lookahead_confidence = lookahead.counter.calcSaturation();
    
    // 如果前瞻置信度超过0.95，则将其限制为0.95
    // 这是为了确保当前的置信度最终会低于阈值
    if (lookahead_confidence > 0.95) {
        lookahead_confidence = 0.95;
    }
    
    // 返回最终的前瞻置信度
    return lookahead_confidence;
}

void SignaturePath::calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses, const PacketPtr &pkt) {}
// 根据给定的预取信息计算预取地址
void SignaturePath::calculatePrefetch(const PrefetchInfo &pfi,
                                    std::vector<AddrPriority> &addresses)
{
    // 获取请求的地址
    Addr request_addr = pfi.getAddr();
    // 计算请求地址的页内偏移
    Addr ppn = request_addr / pageBytes;
    // 计算当前块内偏移
    stride_t current_block = (request_addr % pageBytes) / blkSize;
    // 定义当前步长变量
    stride_t stride;
    // 获取是否是安全模式
    bool is_secure = pfi.isSecure();
    // 初始置信度设置为1.0
    double initial_confidence = 1.0;

    // 获取页的SignatureEntry，用于：
    // - 计算当前步长
    // - 获取当前访问签名
    bool miss;
    // 获取签名条目并计算签名信息
    SignatureEntry &signature_entry = getSignatureEntry(ppn, is_secure,
            current_block, miss, stride, initial_confidence);

    // 如果没有该页的历史记录，则无法继续处理
    if (miss) {
        return;
    }

    // 如果步长为0，则无法继续处理
    if (stride == 0) {
        return;
    }

    // 更新当前签名的置信度
    updatePatternTable(signature_entry.signature, stride);

    // 更新当前SignatureEntry的签名
    signature_entry.signature =
        updateSignature(signature_entry.signature, stride);

    // 准备进行预取地址的生成
    signature_t current_signature = signature_entry.signature;
    double current_confidence = initial_confidence;
    stride_t current_stride = signature_entry.lastBlock;

    // 当前路径置信度足够高时，寻找预取候选地址
    while (current_confidence > lookaheadConfidenceThreshold) {
        // 使用更新后的签名生成预取地址
        // - 在PatternTable中查找所有置信度足够的条目，这些是预取候选地址
        // - 选择计数最高的条目作为"lookahead"
        PatternEntry *current_pattern_entry =
            patternTable.findEntry(current_signature, false);
        PatternStrideEntry const *lookahead = nullptr;
        if (current_pattern_entry != nullptr) {
            unsigned long max_counter = 0;
            // 遍历所有步长条目，选择计数最高的作为lookahead
            for (auto const &entry : current_pattern_entry->strideEntries) {
                if (max_counter < entry.counter) {
                    max_counter = entry.counter;
                    lookahead = &entry;
                }
                // 计算预取置信度
                double prefetch_confidence =
                    calculatePrefetchConfidence(*current_pattern_entry, entry);

                // 如果预取置信度足够高，则添加到候选列表
                if (prefetch_confidence >= prefetchConfidenceThreshold) {
                    assert(entry.stride != 0);
                    addPrefetch(ppn, current_stride, entry.stride,
                                current_confidence, current_signature,
                                is_secure, addresses);
                }
            }
        }

        // 更新置信度和签名，准备下一步预取计算
        if (lookahead != nullptr) {
            current_confidence *= calculateLookaheadConfidence(
                    *current_pattern_entry, *lookahead);
            current_signature =
                updateSignature(current_signature, lookahead->stride);
            current_stride += lookahead->stride;
        } else {
            current_confidence = 0.0;
        }
    }

    // 辅助预取器处理
    auxiliaryPrefetcher(ppn, current_block, is_secure, addresses);
}

/// 辅助预取器函数，用于SignaturePath类
/// 此函数的主要目的是根据给定的参数预取指令或数据，以提升系统性能
/// 参数:
/// - ppn: 物理页号，用于定位物理内存中的特定页
/// - current_block: 当前块内的偏移量，用于定位页内的具体位置
/// - is_secure: 布尔值，指示所访问的数据是否安全，用于控制不同的预取策略
/// - addresses: 包含预取地址及其优先级信息的向量，指导预取器应预取哪些地址
void SignaturePath::auxiliaryPrefetcher(Addr ppn, stride_t current_block,
        bool is_secure, std::vector<AddrPriority> &addresses)
{
    /// 如果addresses向量为空，意味着没有找到预取候选者
    /// 在这种情况下，启用下一行预取器以预取下一区块的内存
    /// 这是一种简单的预取策略，主要用于处理未找到其他预取候选者的情况
    if (addresses.empty()) {
        addPrefetch(ppn, current_block, 1, 0.0 /* 未使用 */, 0 /* 未使用 */,
                    is_secure, addresses);
    }
}

} // namespace prefetch
} // namespace gem5
