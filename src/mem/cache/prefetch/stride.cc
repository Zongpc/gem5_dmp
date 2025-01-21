/*
 * Copyright (c) 2018 Inria
 * Copyright (c) 2012-2013, 2015 ARM Limited
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
 * Stride Prefetcher template instantiations.
 */

#include "mem/cache/prefetch/stride.hh"

#include <cassert>

#include "base/intmath.hh"
#include "base/logging.hh"
#include "base/random.hh"
#include "base/trace.hh"
#include "debug/HWPrefetch.hh"
#include "mem/cache/prefetch/associative_set_impl.hh"
#include "mem/cache/replacement_policies/base.hh"
#include "params/StridePrefetcher.hh"

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

// Stride::StrideEntry类的构造函数
// 参数init_confidence：初始化置信度，类型为SatCounter8
// 该构造函数首先调用基类TaggedEntry的构造函数进行初始化，
// 然后将init_confidence赋值给成员变量confidence，
// 最后调用invalidate函数将当前条目设置为无效状态
Stride::StrideEntry::StrideEntry(const SatCounter8& init_confidence)
    : TaggedEntry(), confidence(init_confidence)
{
    invalidate();
}

// 重置缓存行的状态，包括清空地址、步长和置信度信息
void Stride::StrideEntry::invalidate()
{
    // 调用基类的invalidate方法，进行初始状态重置
    TaggedEntry::invalidate();
    // 重置最后访问的地址为0
    lastAddr = 0;
    // 重置步长为0
    stride = 0;
    // 重置置信度信息
    confidence.reset();
}

// Stride构造函数的定义
Stride::Stride(const StridePrefetcherParams &p)
  // 初始化基类Queued和成员变量
    : Queued(p),  // 初始化基类Queued
    initConfidence(p.confidence_counter_bits, p.initial_confidence),  // 初始化初始置信度
    threshConf(p.confidence_threshold/100.0),  // 初始化置信度阈值
    useRequestorId(p.use_requestor_id),  // 初始化是否使用请求者ID
    degree(p.degree),  // 初始化预取度
    pcTableInfo(p.table_assoc, p.table_entries, p.table_indexing_policy,  // 初始化PC表信息
        p.table_replacement_policy)
{
}

// The findTable function is used to find or create a PCTable for a given context.
// Parameters:
// - context: The context for which to find or create the PCTable.
// Return value:
// - Returns a pointer to the found or newly created PCTable.
Stride::PCTable*
Stride::findTable(int context)
{
    // Check if table for given context exists
    auto it = pcTables.find(context);
    if (it != pcTables.end())
        // If found, return a pointer to the existing table
        return &it->second;

    // If table does not exist yet, create one
    return allocateNewContext(context);
}

// Allocate a new context table
Stride::PCTable*
Stride::allocateNewContext(int context)
{
    // Create new table
    // Insert a new context-table pair into the pcTables map, where the key is the context,
    // and the value is the corresponding PCTable object.
    // The PCTable is initialized with the configuration information from pcTableInfo, and the
    // StrideEntry uses the initConfidence value for initialization.
    auto insertion_result = pcTables.insert(std::make_pair(context,
        PCTable(pcTableInfo.assoc, pcTableInfo.numEntries,
        pcTableInfo.indexingPolicy, pcTableInfo.replacementPolicy,
        StrideEntry(initConfidence))));

    // Debug output: log the addition of a new context with stride entries
    DPRINTF(HWPrefetch, "Adding context %i with stride entries\n", context);

    // Get iterator to new pc table, and then return a pointer to the new table
    return &(insertion_result.first->second);
}

/**
 * 检查给定地址是否符合 stride 模式。
 * 
 * 此函数通过检查程序计数器（PC）表格中的条目来判断给定地址是否遵循预期的 stride 模式。
 * 如果任何一个条目的信心值达到或超过预设阈值，则认为该地址符合 stride 模式。
 * 
 * @param addr 要检查的内存地址。
 * @return 如果地址符合 stride 模式则返回 true，否则返回 false。
 */
bool
Stride::checkStride(Addr addr) const
{
    // 遍历 PC 表格
    for (auto it = pcTables.begin(); it != pcTables.end(); ++it) {
        // 在当前 PC 表项中查找给定地址的条目
        StrideEntry* entry = it->second.findEntry(addr, false);

        // 如果找到条目且其信心值达到或超过阈值，则认为地址符合 stride 模式
        if (entry && entry->confidence.calcSaturation() >= threshConf) {
            DPRINTF(HWPrefetch, "Stride Chcek Pass: PC %x, confidence %.3f.\n", addr,entry->confidence.calcSaturation());
            return true;
        } else if (entry) {
            DPRINTF(HWPrefetch, "Stride Chcek Fail: PC %x, confidence %.3f.\n", addr,entry->confidence.calcSaturation());
        }
    }
    // 如果没有找到符合要求的条目，则地址不符合 stride 模式
    return false;
}

uint64_t 
Stride::catchRspData(const PacketPtr &pkt) const
{
    // 如果包中不包含程序计数器（PC），则打印调试信息并返回
    if (!pkt->req->hasPC()) {
        DPRINTF(HWPrefetch, "catchRspData: no PC\n");
        return 0;
    }
        
    // 如果包中的数据无效，则打印调试信息并返回
    if (!pkt->validData()) {
        DPRINTF(HWPrefetch, "catchRspData: PC %llx, PAddr %llx, no Data, %s\n", 
                                pkt->req->getPC(), pkt->req->getPaddr(), pkt->cmdString());
        return 0;
    }

    // 假设响应数据的最大长度为 8 字节（int64）
    // 由于包只保留请求需要的数据，因此我们需要解析所有数据。
    const int data_stride = 8;
    const int byte_width = 8;

    // 如果包的大小超过 8 字节，则返回
    if (pkt->getSize() > 8) return 0; 
    uint8_t data[8] = {0};
    pkt->writeData(data); 
    uint64_t resp_data = 0;
    // 从数据中构造响应数据
    for (int i_st = data_stride-1; i_st >= 0; i_st--) {
        resp_data = resp_data << byte_width;
        resp_data += static_cast<uint64_t>(data[i_st]);
    }

    // 如果响应数据可能导致整数溢出，则返回
    // assert(resp_data <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()));//xymc
    if (resp_data > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) return 0;

    return resp_data;
}

void Stride::calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses) {}

// 计算并预测取址，输入pkt同时进行linked list地筛选
void Stride::calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses, const PacketPtr &pkt) {
    // 如果没有PC（程序计数器），则忽略该请求
    if (!pfi.hasPC()) {
        DPRINTF(HWPrefetch, "Ignoring request with no PC.\n");
        return;
    }

    // 获取必要包信息
    Addr pf_addr = pfi.getAddr();
    Addr pc = pfi.getPC();
    bool is_secure = pfi.isSecure();
    RequestorID requestor_id = useRequestorId ? pfi.getRequestorId() : 0;

    // 获取对应的PC表
    PCTable* pcTable = findTable(requestor_id);

    // 在PC表中查找条目
    StrideEntry *entry = pcTable->findEntry(pc, is_secure);

    // 如果找到条目
    if (entry != nullptr) {
        pcTable->accessEntry(entry);

        // 表格命中
        int new_stride = pf_addr - entry->lastAddr;
        bool stride_match = (new_stride == entry->stride);

        // link_detector 找到addr-data的offset稳定的PC
        int64_t rspData = 0;
        bool loffset_match = 0;
        rspData = catchRspData(pkt);
        if(rspData != 0) {
            int64_t new_loffset = pf_addr - entry->rspData;
            loffset_match = (new_loffset == entry->loffset) && (new_loffset < 4096) && (new_stride != 0);
            entry->rspData = rspData;
            entry->loffset = new_loffset;
        }

        // 调整步长条目的置信度
        if (stride_match && new_stride != 0) {
            entry->confidence++;
        } else {
            entry->confidence--;
            // 如果置信度低于阈值，则训练新步长
            if (entry->confidence.calcSaturation() < threshConf) {
                entry->stride = new_stride;
            }
        }

        DPRINTF(HWPrefetch, "Hit: PC %x pkt_addr %x (%s) stride %d (%s), "
                "conf %d, link (%s)\n", pc, pf_addr, is_secure ? "s" : "ns",
                new_stride, stride_match ? "match" : "change",
                (int)entry->confidence, loffset_match ? "match" : "miss");

        entry->lastAddr = pf_addr;

        // 如果置信度低于阈值，则中止预测生成
        if (entry->confidence.calcSaturation() < threshConf) {
            if (loffset_match == 1) { //zongpc
                callReadytoIssue(pfi, true);
            }
            return;
        }

        if (loffset_match == 1) { //zongpc
            callReadytoIssue(pfi, true);
        } else {
            //if(pc < 0x407210) { //debug_zongpc
                callReadytoIssue(pfi, false);
            //}
        }

        // 生成多达degree个预测地址
        for (int d = 1; d <= degree; d++) {
            // 将步长向上取至少为1个缓存行
            int prefetch_stride = new_stride;
            if (abs(new_stride) < blkSize) {
                prefetch_stride = (new_stride < 0) ? -blkSize : blkSize;
            }

            Addr new_addr = pf_addr + d * prefetch_stride;
            addresses.push_back(AddrPriority(new_addr, 0));
        }
    } else {
        // 表格未命中
        DPRINTF(HWPrefetch, "Miss: PC %x pkt_addr %x (%s)\n", pc, pf_addr,
                is_secure ? "s" : "ns");

        // 找到一个牺牲者条目并插入新条目的数据
        StrideEntry* entry = pcTable->findVictim(pc);
        entry->lastAddr = pf_addr;
        int64_t rspData = 0;
        rspData = catchRspData(pkt);
        if(rspData != 0) {
            entry->rspData = rspData;
        }
        pcTable->insertEntry(pc, is_secure, entry);
    }
}

/**
 * @brief 从给定的程序计数器（PC）地址中提取出组索引。
 *
 * 该函数通过两级哈希处理来计算组索引，以实现Stride Prefetcher的Hashed Set Associative策略。
 * 它首先对PC地址进行右移操作以生成两个哈希值，然后通过异或操作来计算最终的组索引。
 * 这种方法旨在通过简单的位操作提供足够的随机性和扩散性，以支持高效的数据访问预测。
 *
 * @param pc 程序计数器地址，用作计算组索引的输入。
 * @return 返回计算得到的组索引，该值在setMask的范围内有效。
 */
uint32_t StridePrefetcherHashedSetAssociative::extractSet(const Addr pc) const
{
    // 第一级哈希：对PC地址进行右移操作，这是哈希处理的一部分。
    const Addr hash1 = pc >> 1;
    // 第二级哈希：再次对结果进行右移操作，为最终的组索引计算做准备。
    const Addr hash2 = hash1 >> tagShift;
    // 计算最终的组索引：通过异或操作合并两个哈希值，并与setMask进行与操作，以确保结果在有效范围内。
    return (hash1 ^ hash2) & setMask;
}

Addr
StridePrefetcherHashedSetAssociative::extractTag(const Addr addr) const
{
    return addr;
}

} // namespace prefetch
} // namespace gem5
