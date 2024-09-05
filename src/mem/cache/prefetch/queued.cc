/*
 * Copyright (c) 2014-2015 ARM Limited
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

#include "mem/cache/prefetch/queued.hh"

#include <cassert>

#include "arch/generic/tlb.hh"
#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/HWPrefetch.hh"
#include "debug/HWPrefetchQueue.hh"
#include "mem/cache/base.hh"
#include "mem/request.hh"
#include "params/QueuedPrefetcher.hh"

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

// 创建一个延迟传输数据包，并为其分配请求和包资源。
// 参数:
// paddr: 物理地址。
// blk_size: 块大小。
// requestor_id: 请求者ID。
// tag_prefetch: 是否为预取操作打标签。
// t: 时间周期。
// tag_vaddr: 是否为数据包设置虚拟地址。
void Queued::DeferredPacket::createPkt(Addr paddr, unsigned blk_size,
                                            RequestorID requestor_id,
                                            bool tag_prefetch,
                                            Tick t, 
                                            bool tag_vaddr) {
    // 创建一个预取内存请求
    RequestPtr req = std::make_shared<Request>(paddr, blk_size,
                                                0, requestor_id);

    // 如果预取信息是安全的，将请求标记为安全
    if (pfInfo.isSecure()) {
        req->setFlags(Request::SECURE);
    }
    // 设置任务ID为预取器
    req->taskId(context_switch_task_id::Prefetcher);
    // 创建一个新的数据包并分配空间
    pkt = new Packet(req, MemCmd::HardPFReq, blk_size);
    pkt->allocate();
    // 如果是预取操作并包含PC信息，为数据包打上PC标签
    if (tag_prefetch && pfInfo.hasPC()) {
        pkt->req->setPC(pfInfo.getPC());
        // TODO: 应该也为ContextID打标签吗？
    }
    // 如果需要打上虚拟地址标签，为数据包设置虚拟地址
    if (tag_vaddr) {
        pkt->req->setVaddr(pfInfo.getAddr() + (paddr - owner->blockAddress(paddr)));
    }
    // 如果预取信息是填充触发器，设置数据包的预取标志
    if (pfInfo.isFillTrigger()) {
        pkt->fill_prefetch = true;
    }
    // 设置操作的时间周期
    tick = t;
}

/**
 * 开始对延迟的包进行翻译。
 * 
 * 本函数被设计来处理延迟包的翻译工作。它确保每次只有一個翻译操作在进行，
 * 并利用传输层缓冲区（TLB）来执行翻译请求。翻译操作是通过模拟定时模式下的
 * TLB访问来实现的，这是为了确保翻译过程的正确性和效率。
 * 
 * @param tlb 指向BaseTLB的指针，用于执行翻译操作。
 */
void Queued::DeferredPacket::startTranslation(BaseTLB *tlb)
{
    // 确保翻译请求已经被正确初始化
    assert(translationRequest != nullptr);

    // 如果当前没有翻译操作正在进行
    if (!ongoingTranslation) {
        // 标记翻译操作已经开始
        ongoingTranslation = true;

        // 使用TLB对翻译请求进行定时模式下的翻译
        // 这里之所以使用定时模式是因为它能更准确地模拟实际的翻译过程
        tlb->translateTiming(translationRequest, tc, this, BaseMMU::Read);
    }
}

/// @brief 完成延迟数据包的处理
/// 
/// 根据提供的故障和请求信息，标记当前的翻译任务结束，并通知所有者。
/// 如果提供了故障对象，则表示翻译失败。
/// 
/// @param fault 翻译过程中可能发生的故障，如果为NoFault则表示翻译成功。
/// @param req 请求对象指针，表示触发此翻译任务的请求。
/// @param tc 线程上下文指针，用于在翻译完成时恢复线程状态。
/// @param mode BaseMMU的模式，表示翻译操作的模式。
void Queued::DeferredPacket::finish(const Fault &fault,
    const RequestPtr &req, ThreadContext *tc, BaseMMU::Mode mode)
{
    // 确保当前有一个正在进行的翻译任务
    assert(ongoingTranslation);
    // 将正在进行的翻译任务标记为false，表示翻译任务结束
    ongoingTranslation = false;
    // 如果fault不为NoFault，表示翻译失败
    bool failed = (fault != NoFault);
    // 通知所有者此翻译任务的完成状态
    owner->translationComplete(this, failed);
}

// Queued类的构造函数，用于初始化Queued对象
// 参数p：QueuedPrefetcherParams类型的对象，包含了许多预取器的配置参数
Queued::Queued(const QueuedPrefetcherParams &p)
    : Base(p), // 调用基类Base的构造函数
      queueSize(p.queue_size), // 初始化队列大小
      missingTranslationQueueSize(p.max_prefetch_requests_with_pending_translation), // 初始化缺失翻译队列大小
      latency(p.latency), // 初始化延迟
      queueSquash(p.queue_squash), // 初始化队列压缩设置
      queueFilter(p.queue_filter), // 初始化队列过滤设置
      cacheSnoop(p.cache_snoop), // 初始化缓存窃听设置
      tagPrefetch(p.tag_prefetch), // 初始化标签预取设置
      tagVaddr(p.tag_vaddr), // 初始化是否使用虚拟地址进行标签预取
      crossPageCtrl(p.cross_page_ctrl), // 初始化跨页控制设置
      throttleControlPct(p.throttle_control_percentage), // 初始化节流控制百分比
      statsQueued(this) // 初始化统计信息对象
{
    // 确保使用虚拟地址的设置与tagVaddr参数一致
    assert(useVirtualAddresses == tagVaddr);

    // 如果有非空的程序计数器列表，则注册到统计信息对象中
    if (!p.stats_pc_list.empty()) {
        statsQueued.regQueuedPerPC(stats_pc_list);
    }
}

// Queued 类的析构函数
Queued::~Queued()
{
    // 删除预取队列中的所有预取包
    for (DeferredPacket &p : pfq) {
        // 释放预取包的内存
        delete p.pkt;
    }
}

// 打印队列内容的函数
// @param queue 要打印的队列，包含未处理的数据包
void Queued::printQueue(const std::list<DeferredPacket> &queue) const
{
    // 初始化队列位置变量
    int pos = 0;
    // 初始化队列名称变量
    std::string queue_name = "";
    // 根据传入的队列引用设置队列名称
    if (&queue == &pfq) {
        queue_name = "PFQ";
    } else {
        // 断言传入的是正确的队列引用
        assert(&queue == &pfqMissingTranslation);
        queue_name = "PFTransQ";
    }

    // 遍历队列中的每个数据包
    for (const_iterator it = queue.cbegin(); it != queue.cend(); it++, pos++) {
        // 获取数据包的虚拟地址
        Addr vaddr = it->pfInfo.getAddr();
        // 设置物理地址，如果数据包尚未翻译，则物理地址为0
        Addr paddr = it->pkt ? it->pkt->getAddr() : 0;
        // 打印队列位置、虚拟地址、物理地址和优先级
        Addr pc=it->pfInfo.getPC();
        DPRINTF(HWPrefetchQueue, "%s[%d]: Prefetch Req PC:%#x VA: %#x PA: %#x "
                "prio: %3d\n", queue_name, pos, pc, vaddr, paddr, it->priority);
    }
}

/**
 * 打印队列大小信息
 * 
 * 此函数用于调试过程中输出两个队列的当前大小：
 * - pfq：常规预取队列
 * - pfqMissingTranslation：翻译缺失预取队列
 * 
 * 通过DPRINTF宏将队列大小信息输出到日志中，便于开发者了解队列状态
 */
void Queued::printSize() const
{
    DPRINTF(
        HWPrefetch, "pfq size %lu, pfqMissingTranslation size %lu\n", 
        pfq.size(), pfqMissingTranslation.size()
    );
}

size_t
Queued::getMaxPermittedPrefetches(size_t total) const
{
    /**
     * 根据预取器的准确性来限制生成的预取量。
     * 准确性基于有用预取与已发出预取数量之比计算得出。
     *
     * throttleControlPct 控制预取器生成的候选地址中有多少最终会转换为预取请求：
     * - 如果设置为100，则所有候选者都可以被丢弃（始终允许生成一个请求）
     * - 将其设置为0将禁用节流控制，因此为所有候选者创建请求
     * - 如果设置为60，则40％的候选者将生成请求，剩余的60％将根据当前准确性生成
     */

    // 原始英文注释:
    /**
     * Throttle generated prefetches based in the accuracy of the prefetcher.
     * Accuracy is computed based in the ratio of useful prefetches with
     * respect to the number of issued prefetches.
     *
     * The throttleControlPct controls how many of the candidate addresses
     * generated by the prefetcher will be finally turned into prefetch
     * requests
     * - If set to 100, all candidates can be discarded (one request
     *   will always be allowed to be generated)
     * - Setting it to 0 will disable the throttle control, so requests are
     *   created for all candidates
     * - If set to 60, 40% of candidates will generate a request, and the
     *   remaining 60% will be generated depending on the current accuracy
     */

    size_t max_pfs = total;
    if (total > 0 && issuedPrefetches > 0) {
        size_t throttle_pfs = (total * throttleControlPct) / 100;
        size_t min_pfs = (total - throttle_pfs) == 0 ?
            1 : (total - throttle_pfs);
        max_pfs = min_pfs + (total - min_pfs) *
            usefulPrefetches / issuedPrefetches;
    }
    return max_pfs;
}

// 该函数用于处理缓存未命中时的情况，特别是对于预取队列的操作。
// 主要包括剔除无效的预取请求、计算新的预取地址，并更新预取队列。
void Queued::notify(const PacketPtr &pkt, const PrefetchInfo &pfi)
{
    // 获取当前访问的块地址和安全属性。
    Addr blk_addr = blockAddress(pfi.getAddr());
    bool is_secure = pfi.isSecure();

    // 如果启用了队列剔除功能，则检查并剔除与当前缺失行相同的预取请求。
    if (queueSquash) {
        auto itr = pfq.begin();
        while (itr != pfq.end()) {
            if (itr->pfInfo.getAddr() == blk_addr &&
                itr->pfInfo.isSecure() == is_secure) {
                // 打印调试信息，指示移除的预取请求。
                DPRINTF(HWPrefetch, "Removing pf candidate addr: %#x "
                        "(cl: %#x), demand request going to the same addr\n",
                        itr->pfInfo.getAddr(),
                        blockAddress(itr->pfInfo.getAddr()));
                // 统计被剔除的预取请求数量。
                statsQueued.pfRemovedDemand++;
                // 如果预取请求有关联的程序计数器(PC)，则更新相关统计。
                if (itr->pfInfo.hasPC()) {
                    Addr req_pc = itr->pfInfo.getPC();
                    for (int i = 0; i < stats_pc_list.size(); i++) {
                        if (req_pc == stats_pc_list[i]) {
                            statsQueued.pfRemovedDemandPerPfPC[i]++;
                            break;
                        }
                    }
                }
                // 释放预取请求的内存并从队列中移除。
                delete itr->pkt;
                itr = pfq.erase(itr);
            } else {
                ++itr;
            }
        }
    }

    // 根据当前访问计算新的预取地址。
    std::vector<AddrPriority> addresses;
    calculatePrefetch(pfi, addresses);

    // 获取允许生成的最大预取数量。
    size_t max_pfs = getMaxPermittedPrefetches(addresses.size());

    // 将计算得到的预取地址加入队列。
    size_t num_pfs = 0;
    for (AddrPriority& addr_prio : addresses) {
        // 对预取地址进行块对齐。
        addr_prio.first = blockAddress(addr_prio.first);

        // 如果新的预取地址与当前访问地址不在同一页面，则统计相关数据。
        if (!samePage(addr_prio.first, pfi.getAddr())) {
            statsQueued.pfSpanPage += 1;
            if (hasBeenPrefetched(pkt->getAddr(), pkt->isSecure())) {
                statsQueued.pfUsefulSpanPage += 1;
            }
        }

        // 根据TLB和页面交叉控制标志决定是否可以跨页面预取。
        bool can_cross_page = (tlb != nullptr) && crossPageCtrl;
        if (can_cross_page || samePage(addr_prio.first, pfi.getAddr())) {
            // 创建新的预取信息并加入队列。
            PrefetchInfo new_pfi(pfi,addr_prio.first);
            statsQueued.pfIdentified++;
            DPRINTF(HWPrefetch, "Found a pf candidate addr: %#x, "
                    "inserting into prefetch queue.\n", new_pfi.getAddr());
            insert(pkt, new_pfi, addr_prio.second);
            num_pfs += 1;
            // 如果已经达到最大允许预取数量，则停止添加。
            if (num_pfs == max_pfs) {
                break;
            }
        } else {
            // 打印忽略跨页面预取的情况。
            DPRINTF(HWPrefetch, "Ignoring page crossing prefetch.\n");
        }
    }
}

// Returns a packet from the queue, handling empty queue situations by attempting to fill it
// Returns nullptr if the queue cannot be filled
PacketPtr Queued::getPacket()
{
    // Log the start of attempting to issue a prefetch
    DPRINTF(HWPrefetch, "Requesting a prefetch to issue.\n");

    // If the queue is empty, try to fill it with requests from the queue of missing translations
    if (pfq.empty()) {
        processMissingTranslations(queueSize);
    }

    // If the queue is still empty after trying to fill it, indicate that no hardware 
    // prefetches are available and return nullptr
    if (pfq.empty()) {
        DPRINTF(HWPrefetch, "No hardware prefetches available.\n");
        return nullptr;
    }

    // Remove the first packet in the queue and prepare to issue it
    PacketPtr pkt = pfq.front().pkt;
    pfq.pop_front();

    // Increment the statistics for issued prefetches
    prefetchStats.pfIssued++;
    issuedPrefetches += 1;

    // If the request contains a program counter (PC), update statistics based on the PC
    if (pkt->req->hasPC()) {
        Addr req_pc = pkt->req->getPC();
        for (int i = 0; i < stats_pc_list.size(); i++) {
            if (req_pc == stats_pc_list[i]) {
                prefetchStats.pfIssuedPerPfPC[i]++;
                break;
            }
        }
    }

    // Ensure the packet is not null before continuing
    assert(pkt != nullptr);

    // Log the generation of a prefetch
    DPRINTF(HWPrefetch, "Generating prefetch for PC:%#x Addr:%#x.\n", pkt->req->getPC(),pkt->getAddr());

    // Attempt to fill the queue with missing translations again, taking into account the current queue size
    processMissingTranslations(queueSize - pfq.size());
    return pkt;
}

// 定义QueuedStats类的构造函数，用于初始化统计相关的数据成员
Queued::QueuedStats::QueuedStats(statistics::Group *parent)
    : statistics::Group(parent), // 调用基类statistics::Group的构造函数
    // 以下统计项用于记录预取操作的各种情况
    ADD_STAT(pfIdentified, statistics::units::Count::get(),
             "number of prefetch candidates identified"), // 识别到的预取候选数量
    ADD_STAT(pfBufferHit, statistics::units::Count::get(),
             "number of redundant prefetches already in prefetch queue"), // 预取队列中已存在的冗余预取数量
    ADD_STAT(pfBufferHitPerPfPC, statistics::units::Count::get(),
             "number of redundant prefetches already in prefetch queue"), // 每预取候选的冗余预取数量
    ADD_STAT(pfInCache, statistics::units::Count::get(),
             "number of redundant prefetches already in cache/mshr dropped"), // 缓存或mshr中已存在的冗余预取数量
    ADD_STAT(pfInCachePerPfPC, statistics::units::Count::get(),
             "number of redundant prefetches already in cache/mshr dropped"), // 每预取候选的冗余预取数量
    ADD_STAT(pfRemovedDemand, statistics::units::Count::get(),
            "number of prefetches dropped due to a demand for the same "
             "address"), // 因相同地址的需求而丢弃的预取数量
    ADD_STAT(pfRemovedDemandPerPfPC, statistics::units::Count::get(),
            "number of prefetches dropped due to a demand for the same "
             "address"), // 每预取候选的丢弃预取数量
    ADD_STAT(pfRemovedFull, statistics::units::Count::get(),
             "number of prefetches dropped due to prefetch queue size"), // 因预取队列大小而丢弃的预取数量
    ADD_STAT(pfRemovedFullPerPfPC, statistics::units::Count::get(),
             "number of prefetches dropped due to prefetch queue size"), // 每预取候选的丢弃预取数量
    ADD_STAT(pfSpanPage, statistics::units::Count::get(),
             "number of prefetches that crossed the page"), // 跨越页面的预取数量
    ADD_STAT(pfUsefulSpanPage, statistics::units::Count::get(),
             "number of prefetches that is useful and crossed the page"), // 有用的且跨越页面的预取数量
    ADD_STAT(pfTransFailed, statistics::units::Count::get(),
            "number of pfq empty and translation not avaliable immediately "
             "when there is a chance for prefetch"), // 预取队列为空且翻译不可用时的预取失败数量
    ADD_STAT(pfTransFailedPerPfPC, statistics::units::Count::get(),
            "number of pfq empty and translation not avaliable immediately "
            "when there is a chance for prefetch") // 每预取候选的预取失败数量
{
    using namespace statistics;

    int max_per_pc = 32; // 每个预取候选的最大统计数量

    // 初始化与预取候选相关的统计项，并设置其属性
    pfBufferHitPerPfPC
        .init(max_per_pc)
        .flags(total | nozero | nonan)
        ;
    pfInCachePerPfPC
        .init(max_per_pc)
        .flags(total | nozero | nonan)
        ;
    pfRemovedDemandPerPfPC
        .init(max_per_pc)
        .flags(total | nozero | nonan)
        ;
    pfRemovedFullPerPfPC
        .init(max_per_pc)
        .flags(total | nozero | nonan)
        ;
    pfTransFailedPerPfPC
        .init(max_per_pc)
        .flags(total | nozero | nonan)
        ;
}

// 注册每个程序计数器（PC）对应的统计数据
void Queued::QueuedStats::regQueuedPerPC(const std::vector<Addr>& stats_pc_list) {
    // 使用statistics命名空间
    using namespace statistics;

    // 定义每个PC最大数量
    int max_per_pc = 32;
    // 确保提供的PC地址数量不超过最大值
    assert(stats_pc_list.size() < max_per_pc);

    // 遍历PC地址列表
    for (int i = 0; i < stats_pc_list.size(); i++) {
        // 创建一个字符串流，用于将PC地址（十六进制）转换为字符串
        std::stringstream stream;
        stream << std::hex << stats_pc_list[i];
        std::string pc_name = stream.str();

        // 为每个PC地址设置子名称，用于不同PC地址的统计数据区分
        pfBufferHitPerPfPC.subname(i, pc_name);
        pfInCachePerPfPC.subname(i, pc_name);
        pfRemovedDemandPerPfPC.subname(i, pc_name);
        pfRemovedFullPerPfPC.subname(i, pc_name);
        pfTransFailedPerPfPC.subname(i, pc_name);
    }
}


// 处理缺失的翻译队列
// 该函数旨在处理一批未翻译的包，直到达到最大处理数量限制
// 参数max: 本次处理的最大包数量
// 无返回值
void Queued::processMissingTranslations(unsigned max) {
    // 已处理的包数量
    unsigned count = 0;
    // 获取缺失翻译包的队列迭代器
    iterator it = pfqMissingTranslation.begin();
    // 遍历队列，直到队列结束或达到最大处理数量
    while (it != pfqMissingTranslation.end() && count < max) {
        // 获取当前包的可延迟处理对象
        DeferredPacket &dp = *it;
        // 首先增加迭代器，因为startTranslation可能调用finishTranslation，从而擦除"it"
        it++;
        // 开始翻译当前包
        dp.startTranslation(tlb);
        // 增加已处理包的计数
        count += 1;
    }
}

// 当队列中的请求完成翻译时调用此函数
void Queued::translationComplete(DeferredPacket *dp, bool failed) {
    // 在缺失翻译的队列中找到对应的 DeferredPacket
    auto it = pfqMissingTranslation.begin();
    while (it != pfqMissingTranslation.end()) {
        if (&(*it) == dp) {
            break;
        }
        it++;
    }
    // 确保找到了对应的 DeferredPacket
    assert(it != pfqMissingTranslation.end());
    // 处理翻译成功的情况
    if (!failed) {
        // 打印翻译成功的调试信息
        DPRINTF(HWPrefetch, "%s Translation of vaddr %#x succeeded: "
                "paddr %#x \n", tlb->name(),
                it->translationRequest->getVaddr(),
                it->translationRequest->getPaddr());
        Addr target_paddr = it->translationRequest->getPaddr();
        // 检查这个预取是否已经是多余的
        if (cacheSnoop && (inCache(target_paddr, it->pfInfo.isSecure()) ||
                    inMissQueue(target_paddr, it->pfInfo.isSecure()))) {
            // 统计预取到的地址已经在缓存中的情况
            statsQueued.pfInCache++;
            if (it->pfInfo.hasPC()) {
                Addr req_pc = it->pfInfo.getPC();
                for (int i = 0; i < stats_pc_list.size(); i++) {
                    if (req_pc == stats_pc_list[i]) {
                        statsQueued.pfInCachePerPfPC[i]++;
                        break;
                    }
                }
            }

            // 打印放弃多余的预取请求的调试信息
            DPRINTF(HWPrefetch, "Dropping redundant in "
                    "cache/MSHR prefetch addr:%#x\n", target_paddr);
        } else {
            // 计算预取请求的到达时间，并创建包，然后加入队列
            Tick pf_time = curTick() + clockPeriod() * latency;
            it->createPkt(target_paddr, blkSize, requestorId, tagPrefetch,
                            pf_time, tagVaddr);
            addToQueue(pfq, *it);
        }
    } else {
        // 处理翻译失败的情况
        DPRINTF(HWPrefetch, "%s Translation of vaddr %#x failed, dropping "
                "prefetch request %#x \n", tlb->name(),
                it->translationRequest->getVaddr());

        // 统计翻译失败的情况
        statsQueued.pfTransFailed += 1;
        if (it->pfInfo.hasPC()) {
            Addr req_pc = it->pfInfo.getPC();
            for (int i = 0; i < stats_pc_list.size(); i++) {
                if (req_pc == stats_pc_list[i]) {
                    statsQueued.pfTransFailedPerPfPC[i]++;
                    break;
                }
            }
        }
    }
    // 从缺失翻译的队列中移除处理过的请求
    pfqMissingTranslation.erase(it);
}

// 检查是否已在队列中，并根据优先级进行处理
bool Queued::alreadyInQueue(std::list<DeferredPacket> &queue,
                            const PrefetchInfo &pfi, int32_t priority) {
    // 初始化找到标志为false
    bool found = false;
    iterator it;
    // 使用iterator遍历队列
    for (it = queue.begin(); it != queue.end(); it++) {
        // 检查当前元素的pfInfo是否与传入的pfi有相同的地址
        found = it->pfInfo.sameAddr(pfi);
        if(found)
            break;
    }

    // 如果在队列中找到了相同的地址
    if (found) {
        // 增加缓冲区命中计数
        statsQueued.pfBufferHit++;
        // 如果pfi包含程序计数器(PC)
        if (pfi.hasPC()) {
            Addr req_pc = pfi.getPC();
            // 遍历PC列表，增加对应PF PC的命中计数
            for (int i = 0; i < stats_pc_list.size(); i++) {
                if (req_pc == stats_pc_list[i]) {
                    statsQueued.pfBufferHitPerPfPC[i]++;
                    break;
                }
            }
            if(pfi.isPointer())
            {
                delete it->pkt;
                queue.erase(it);
                return false;
                // DPRINTF(HWPrefetch, "old pc:%llx new pc:%llx\n", it->pfInfo.getPC(), req_pc);
                // it->pfInfo.setPC(req_pc);
                // it->pkt->req->setPC(req_pc);
            }
        }

        // 如果当前元素的优先级小于新传入的优先级
        if (it->priority < priority) {
            // 更新优先级和队列中的位置
            it->priority = priority;
            auto prev = it;
            // 向前遍历，找到合适的位置
            while (prev != queue.begin()) {
                prev--;
                // 如果当前包的优先级更高，则交换位置
                if (*it > *prev) {
                    std::swap(*it, *prev);
                    it = prev;
                }
            }
            // 打印调试信息，地址已在队列中，优先级已更新
            DPRINTF(HWPrefetch, "Prefetch addr already in "
                "prefetch queue, priority updated\n");
        } else {
            // 打印调试信息，地址已在队列中，但优先级未更新
            DPRINTF(HWPrefetch, "Prefetch addr already in "
                "prefetch queue\n");
        }
    }
    // 返回是否找到
    return found;
}

// 创建预取请求
/*
 * @param addr 预取的地址
 * @param pfi 预取信息的引用，包含预取的相关信息如程序计数器等
 * @param pkt 包含了原始请求的Packet指针
 * 
 * @return 返回创建的预取请求指针
 * 
 * 该函数用于根据给定的地址、预取信息和Packet指针创建一个新的预取请求。
 * 预取请求是用于提前加载数据到缓存中，以提高数据访问性能。
 * 通过设置请求标志为预取，使得该请求在处理时能够被识别为预取请求。
 */
RequestPtr Queued::createPrefetchRequest(Addr addr, PrefetchInfo const &pfi, PacketPtr pkt)
{
    // 创建并返回一个新的预取请求对象
    RequestPtr translation_req = std::make_shared<Request>(
            addr, blkSize, pkt->req->getFlags(), requestorId, pfi.getPC(),
            pkt->req->contextId());
    translation_req->setFlags(Request::PREFETCH);
    return translation_req;
}

/*
 * 在队列中插入一个预取请求。
 * 如果预取信息已经存在于队列中，则不执行插入操作。
 * 根据是否使用虚拟地址来计算目标物理地址；如果预取跨越页面边界，则创建翻译请求并将其排队。
 *
 * 参数:
 *   pkt: 包含请求的Packet指针。
 *   new_pfi: 预取信息。
 *   priority: 预取的优先级。
 */

void Queued::insert(const PacketPtr &pkt, PrefetchInfo &new_pfi,
                        int32_t priority)
{
    // 检查预取信息是否已存在于队列中
    if (queueFilter) {
        if (alreadyInQueue(pfq, new_pfi, priority)) {
            return;
        }
        if (alreadyInQueue(pfqMissingTranslation, new_pfi, priority)) {
            return;
        }
    }

    /*
     * Physical address computation
     * if the prefetch is within the same page
     *   using VA: add the computed stride to the original PA
     *   using PA: no actions needed
     * if we are page crossing
     *   using VA: Create a translaion request and enqueue the corresponding
     *       deferred packet to the queue of pending translations
     *   using PA: use the provided VA to obtain the target VA, then attempt to
     *     translate the resulting address
     */

    // 获取原始地址，基于是否使用虚拟地址
    Addr orig_addr = useVirtualAddresses ?
        pkt->req->getVaddr() : pkt->req->getPaddr();
    // 确定预取地址与原始地址的关系，并计算步长(stride)
    bool positive_stride = new_pfi.getAddr() >= orig_addr;
    Addr stride = positive_stride ?
        (new_pfi.getAddr() - orig_addr) : (orig_addr - new_pfi.getAddr());

    // 初始化目标物理地址和相关变量
    Addr target_paddr;
    bool has_target_pa = false;
    RequestPtr translation_req = nullptr;

    // 判断预取是否在同一个页面内
    if (samePage(orig_addr, new_pfi.getAddr())) {
        if (useVirtualAddresses) {
            // if we trained with virtual addresses,
            // compute the target PA using the original PA and adding the
            // prefetch stride (difference between target VA and original VA)
            // 如果使用虚拟地址训练，计算目标物理地址
            target_paddr = positive_stride ? (pkt->req->getPaddr() + stride) :
                (pkt->req->getPaddr() - stride);
        } else {
            // 如果使用物理地址训练，目标物理地址即为预取地址
            target_paddr = new_pfi.getAddr();
        }
        has_target_pa = true;
    } else {
        // Page crossing reference

        // ContextID is needed for translation
        // 预取跨越页面边界的情况
        if (!pkt->req->hasContextId()) {
            return;
        }
        if (useVirtualAddresses) {
            // 使用虚拟地址进行预取，创建翻译请求
            has_target_pa = false;
            translation_req = createPrefetchRequest(new_pfi.getAddr(), new_pfi,
                                                    pkt);
        } else if (pkt->req->hasVaddr()) {
            // Compute the target VA using req->getVaddr + stride
            // 使用物理地址进行预取，计算目标虚拟地址并创建翻译请求
            has_target_pa = false;
            Addr target_vaddr = positive_stride ?
                (pkt->req->getVaddr() + stride) :
                (pkt->req->getVaddr() - stride);
            translation_req = createPrefetchRequest(target_vaddr, new_pfi,
                                                    pkt);
        } else {
            // Using PA for training but the request does not have a VA,
            // unable to process this page crossing prefetch.
            // 使用物理地址训练，但是请求中没有虚拟地址，无法处理这个跨页预取
            return;
        }
    }

    // 如果目标物理地址已知且启用了缓存窥探，检查该地址是否已在缓存或缺失队列中
    if (has_target_pa && cacheSnoop &&
            (inCache(target_paddr, new_pfi.isSecure()) ||
            inMissQueue(target_paddr, new_pfi.isSecure()))) {
        statsQueued.pfInCache++;
        if (new_pfi.hasPC()) {
            Addr req_pc = new_pfi.getPC();
            for (int i = 0; i < stats_pc_list.size(); i++) {
                if (req_pc == stats_pc_list[i]) {
                    statsQueued.pfInCachePerPfPC[i]++;
                    break;
                }
            }
        }

        DPRINTF(HWPrefetch, "Dropping redundant in "
                "cache/MSHR prefetch addr:%#x\n", target_paddr);
        return;
    }

    /* 创建预取数据包并找到插入位置 */
    /* Create the packet and find the spot to insert it */
    DeferredPacket dpp(this, new_pfi, 0, priority);
    if (has_target_pa) {
        // 当前时间加上延迟周期数得到预取时间
        Tick pf_time = curTick() + clockPeriod() * latency;
        dpp.createPkt(target_paddr, blkSize, requestorId, tagPrefetch,
                    pf_time, tagVaddr);
        DPRINTF(HWPrefetch, "Prefetch queued. "
                "addr:%#x priority: %3d tick:%lld.\n",
                new_pfi.getAddr(), priority, pf_time);
        // 将预取数据包添加到队列中
        addToQueue(pfq, dpp);
    } else {
        // Add the translation request and try to resolve it later
        // 如果没有目标物理地址，将翻译请求添加到队列中，并尝试稍后解决
        dpp.setTranslationRequest(translation_req);
        dpp.tc = cache->system->threads[translation_req->contextId()];
        DPRINTF(HWPrefetch, "Prefetch queued with no translation. "
                "addr:%#x priority: %3d\n", new_pfi.getAddr(), priority);
        // 将带有翻译请求的预取数据包添加到缺失翻译队列中
        addToQueue(pfqMissingTranslation, dpp);
    }
}

// 将包添加到队列中
void Queued::addToQueue(std::list<DeferredPacket> &queue, DeferredPacket &dpp) {
    // 验证请求的预取缓冲区空间
    if (queue.size() == queueSize) {
        statsQueued.pfRemovedFull++;
        if (dpp.pfInfo.hasPC()) {
            Addr req_pc = dpp.pfInfo.getPC();
            for (int i = 0; i < stats_pc_list.size(); i++) {
                if (req_pc == stats_pc_list[i]) {
                    statsQueued.pfRemovedFullPerPfPC[i]++;
                    break;
                }
            }
        }
        // 最低优先级包
        iterator it = queue.end();
        panic_if(it == queue.begin(), "预取队列已满且为空！");
        --it;
        // 查找该优先级中最旧的包
        panic_if(it == queue.begin(), "预取队列已满，只有一个元素！");
        iterator prev = it;
        bool cont = true;
        // 当未到达队列头部时
        while (cont && prev != queue.begin()) {
            prev--;
            // 当处于相同的优先级时
            cont = prev->priority == it->priority;
            if (cont)
                // 更新指针
                it = prev;
        }
        DPRINTF(HWPrefetch, "预取队列已满，移除最低优先级的最旧包, 地址: %#x\n", it->pfInfo.getAddr());
        delete it->pkt;
        queue.erase(it);
    }

    // 如果队列为空，或者新包的优先级不高于队列中最后的包
    if ((queue.size() == 0) || (dpp <= queue.back())) {
        queue.emplace_back(dpp);
    } else {
        iterator it = queue.end();
        // 从队列末尾开始向前查找合适的位置
        do {
            --it;
        } while (it != queue.begin() && dpp > *it);
        // 如果到达队列头部，需要判断新包是否应该成为新的头部
        if (it == queue.begin() && dpp <= *it)
            it++;
        queue.insert(it, dpp);
    }

    // 如果启用了调试信息输出
    if (debug::HWPrefetchQueue)
        printQueue(queue);

    if (debug::HWPrefetch)
        printSize();
}

} // namespace prefetch
} // namespace gem5
