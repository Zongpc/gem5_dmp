/*
 * Copyright (c) 2013-2014 ARM Limited
 * All rights reserved.
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
 * Hardware Prefetcher Definition.
 */

#include "mem/cache/prefetch/base.hh"

#include <cassert>

#include "base/intmath.hh"
#include "mem/cache/base.hh"
#include "params/BasePrefetcher.hh"
#include "debug/HWPrefetch.hh"
#include "debug/DMP.hh"
#include "sim/system.hh"

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

// 定义PrefetchInfo类的构造函数
// 该构造函数负责初始化PrefetchInfo对象，它接收一个数据包指针、一个地址、一个未命中标志作为参数
Base::PrefetchInfo::PrefetchInfo(PacketPtr pkt, Addr addr, bool miss)
    : address(addr), // 初始化地址
    pc(pkt->req->hasPC() ? pkt->req->getPC() : 0), // 初始化程序计数器（PC），如果请求中包含PC则使用它，否则为0
    requestorId(pkt->req->requestorId()), // 初始化请求者ID
    validPC(pkt->req->hasPC()), // 标记请求是否包含有效的PC
    secure(pkt->isSecure()), // 标记请求是否为安全请求
    size(pkt->req->getSize()), // 初始化请求的大小
    write(pkt->isWrite()), // 标记请求是否为写操作
    paddress(pkt->req->getPaddr()), // 初始化物理地址
    cacheMiss(miss), // 标记是否为缓存未命中
    insert_MSHR(pkt->insert_MSHR),
    Cur_tick(pkt->cur_tick), is_cache_hit(!miss) , 
    cID(pkt->req->hasContextId() ? pkt->req->contextId() : 0) // 初始化上下文ID，如果请求中包含上下文ID则使用它，否则为0
{
    unsigned int req_size = pkt->req->getSize();
    // 如果请求不是写操作且缓存未命中，则不存储数据
    if (!write && miss) {
        data = nullptr;
    } else {
        // 否则，根据请求的大小分配数据存储空间，并从数据包中复制相应数据
        data = new uint8_t[req_size];
        Addr offset = pkt->req->getPaddr() - pkt->getAddr();
        std::memcpy(data, &(pkt->getConstPtr<uint8_t>()[offset]), req_size);
    }

    // 初始化触发标志为false，表示未触发填充操作
    fill_trigger = false;
    is_pointer=false;
}

// Base::PrefetchInfo 类的构造函数
// 该构造函数通过复制一个已存在的 PrefetchInfo 对象，并指定一个新的地址，来创建一个新的 PrefetchInfo 对象
Base::PrefetchInfo::PrefetchInfo(PrefetchInfo const &pfi, Addr addr)
    // 初始化列表中的成员变量
    : address(addr), pc(pfi.pc), requestorId(pfi.requestorId),
    validPC(pfi.validPC), secure(pfi.secure), size(pfi.size),
    write(pfi.write), paddress(pfi.paddress), cacheMiss(pfi.cacheMiss),
    cID(pfi.cID), data(nullptr), fill_trigger(pfi.fill_trigger), is_pointer(pfi.is_pointer)
{
    // 构造函数体中无需额外的代码，因为所有需要的初始化都在初始化列表中完成了
}

// Base::PrefetchInfo 类的构造函数
// 该构造函数通过初始化列表对成员变量进行初始化
Base::PrefetchInfo::PrefetchInfo(Addr addr, Addr pc, RequestorID requestorID, ContextID cID)
    // 初始化成员变量
    : address(addr),          // 预取的地址
    pc(pc),                 // 程序计数器值
    requestorId(requestorID),// 请求者ID
    validPC(true),          // PC值是否有效
    secure(false),          // 是否安全模式
    size(0),                // 预取大小
    write(false),           // 是否写操作
    paddress(0x0),          // 物理地址
    cacheMiss(false),       // 是否缓存未命中
    cID(cID),               // 上下文ID
    data(nullptr),          // 数据指针
    fill_trigger(true),      // 触发填充
    is_pointer(false)      // 是否是指针
{
}

Base::PrefetchInfo::PrefetchInfo(Addr addr, Addr pc, RequestorID requestorID, ContextID cID, bool is_pointer_in)
    // 初始化成员变量
    : address(addr),          // 预取的地址
    pc(pc),                 // 程序计数器值
    requestorId(requestorID),// 请求者ID
    validPC(true),          // PC值是否有效
    secure(false),          // 是否安全模式
    size(0),                // 预取大小
    write(false),           // 是否写操作
    paddress(0x0),          // 物理地址
    cacheMiss(false),       // 是否缓存未命中
    cID(cID),               // 上下文ID
    data(nullptr),          // 数据指针
    fill_trigger(true),      // 触发填充
    is_pointer(is_pointer_in)      // 是否是指针
{
}

// 通知监听器有新的数据包到达
void Base::PrefetchListener::notify(const PacketPtr &pkt)
{
    DPRINTFS(HWPrefetch,(&parent),"startnotify: PC %llx, PAddr %llx\n", 
                    pkt->req->getPC(), pkt->req->getPaddr());
    // 如果是L1请求类型
    if (l1_req) {
        // 通知父类处理L1请求
        parent.notifyL1Req(pkt);
    } else if (l1_resp) {
        // 通知父类处理L1响应
        parent.notifyL1Resp(pkt);
    } else if (isFill) {
        DPRINTFS(HWPrefetch,(&parent),"isFill: PC %llx, PAddr %llx\n", 
                    pkt->req->getPC(), pkt->req->getPaddr());
        // 检查数据包是否包含数据且大小与父类的块大小匹配
        if(pkt->hasData() && (pkt->getSize() == parent.blkSize)) {
            // 获取数据填充的指针
            // if(pkt->hasData())
            {
                DPRINTFS(HWPrefetch,(&parent),"startnotifyfill: PC %llx, PAddr %llx\n", 
                    pkt->req->getPC(), pkt->req->getPaddr());
                const uint8_t* fill_data_ptr = pkt->getConstPtr<u_int8_t>();
                // 通知父类处理数据填充
                parent.notifyFill(pkt, fill_data_ptr);//xymc
            }
        }
    } else {
        // 其他情况，进行探查通知
        parent.probeNotify(pkt, miss);
    }
}

// Base类的构造函数，用于初始化Base对象
Base::Base(const BasePrefetcherParams &p)
    : ClockedObject(p), // 初始化父类ClockedObject
      listeners(), // 初始化listeners，一个用于存储监听器的容器
      cache(nullptr), // 初始化cache指针为nullptr
      blkSize(p.block_size), // 设置块大小
      lBlkSize(floorLog2(blkSize)), // 计算块大小的对数
      onMiss(p.on_miss), // 设置缺失处理参数
      onRead(p.on_read), // 设置读取处理参数
      onWrite(p.on_write), // 设置写入处理参数
      onData(p.on_data), // 设置数据处理参数
      onInst(p.on_inst), // 设置指令处理参数
      requestorId(p.sys->getRequestorId(this)), // 设置请求者ID
      pageBytes(p.page_bytes), // 设置页面大小
      prefetchOnAccess(p.prefetch_on_access), // 设置访问时预取参数
      prefetchOnPfHit(p.prefetch_on_pf_hit), // 设置预取命中时预取参数
      useVirtualAddresses(p.use_virtual_addresses), // 设置是否使用虚拟地址
      prefetchStats(this), // 初始化预取统计信息
      issuedPrefetches(0), // 初始化已发出的预取数
      usefulPrefetches(0), // 初始化有用的预取数
      tlb(nullptr) // 初始化TLB指针为nullptr
{
    // 如果有PC统计列表，则注册并设置列表
    if (!p.stats_pc_list.empty()) {
        prefetchStats.regStatsPerPC(p.stats_pc_list);
        stats_pc_list = p.stats_pc_list;
    }
}

// 设置缓存对象
//
// 参数：
// - _cache: 指向新的缓存对象的指针
//
// 说明：
// 该函数用于将当前对象的缓存替换为新的缓存对象。它首先断言当前对象没有已有的缓存，
// 然后将新的缓存对象指针赋值给成员变量 cache。此外，它还获取新缓存对象的块大小，并
// 将其保存在成员变量 blkSize 中，同时计算并保存块大小的对数（以2为底）到 lBlkSize 中。
// 这些操作确保了对象能够正确地使用新的缓存配置。
void
Base::setCache(BaseCache *_cache)
{
    // 确保当前没有设置缓存
    assert(!cache);
    cache = _cache;

    // 保存缓存的块大小及其对数（以2为底）
    blkSize = cache->getBlockSize();
    lBlkSize = floorLog2(blkSize);
}

Base::StatGroup::StatGroup(statistics::Group *parent)
    : statistics::Group(parent),
    ADD_STAT(demandMshrMisses, statistics::units::Count::get(),
        "demands not covered by prefetchs"),
    ADD_STAT(demandMshrMissesPerPC, statistics::units::Count::get(),
        "demands not covered by prefetchs"),
    ADD_STAT(demandMshrHitsAtPf, statistics::units::Count::get(),
        "demands hit in mshr allocated by prefetchs"),
    ADD_STAT(demandMshrHitsAtPfPerPfPC, statistics::units::Count::get(),
        "demands hit in mshr allocated by prefetchs"),
    ADD_STAT(pfIssued, statistics::units::Count::get(),
        "number of hwpf issued"),
    ADD_STAT(pfIssuedPerPfPC, statistics::units::Count::get(),
        "number of hwpf issued"),
    ADD_STAT(pfUnused, statistics::units::Count::get(),
            "number of HardPF blocks evicted w/o reference"),
    ADD_STAT(pfUnusedPerPfPC, statistics::units::Count::get(),
            "number of HardPF blocks evicted w/o reference"),
    ADD_STAT(pfUseful, statistics::units::Count::get(),
        "number of useful prefetch"),
    ADD_STAT(pfUsefulPerPfPC, statistics::units::Count::get(),
        "number of useful prefetch"),
    ADD_STAT(pfUsefulButMiss, statistics::units::Count::get(),
        "number of hit on prefetch but cache block is not in an usable "
        "state"),
    // ADD_STAT(accuracy, statistics::units::Count::get(),
    //     "accuracy of the prefetcher"),
    // ADD_STAT(accuracyPerPC, statistics::units::Count::get(),
    //     "accuracy of the prefetcher"),
    // ADD_STAT(timely_accuracy, statistics::units::Count::get(),
    //     "timely accuracy of the prefetcher"),
    // ADD_STAT(timely_accuracy_perPfPC, statistics::units::Count::get(),
    //     "timely accuracy of the prefetcher"),
    // ADD_STAT(coverage, statistics::units::Count::get(),
    // "coverage brought by this prefetcher"),
    // ADD_STAT(coveragePerPC, statistics::units::Count::get(),
    // "coverage brought by this prefetcher"),
    ADD_STAT(pf_cosumed, statistics::units::Count::get(),
        "pf_cosumed of the prefetcher"),
    ADD_STAT(pf_cosumed_perPfPC, statistics::units::Count::get(),
        "pf_cosumed of the prefetcher"),
    ADD_STAT(pf_effective, statistics::units::Count::get(),
        "pf_effective of the prefetcher"),
    ADD_STAT(pf_effective_perPfPC, statistics::units::Count::get(),
        "pf_effective of the prefetcher"),
    ADD_STAT(pf_timely, statistics::units::Count::get(),
        "pf_timely of the prefetcher"),
    ADD_STAT(pf_timely_perPfPC, statistics::units::Count::get(),
        "pf_timely of the prefetcher"),
    ADD_STAT(accuracy_cache, statistics::units::Count::get(),
        "accuracy_cache of the prefetcher"),
    ADD_STAT(accuracy_cache_perPfPC, statistics::units::Count::get(),
        "accuracy_cache of the prefetcher"),
    ADD_STAT(accuracy_prefetcher, statistics::units::Count::get(),
        "accuracy_prefetcher of the prefetcher"),
    ADD_STAT(accuracy_prefetcher_perPfPC, statistics::units::Count::get(),
        "accuracy_prefetcher of the prefetcher"),
    ADD_STAT(pfHitInCache, statistics::units::Count::get(),
        "number of prefetches hitting in cache"),
    ADD_STAT(pfHitInCachePerPfPC, statistics::units::Count::get(),
        "number of prefetches hitting in cache"),
    ADD_STAT(pfHitInMSHR, statistics::units::Count::get(),
        "number of prefetches hitting in a MSHR"),
    ADD_STAT(pfHitInMSHRPerPfPC, statistics::units::Count::get(),
        "number of prefetches hitting in a MSHR"),
    ADD_STAT(pfHitInWB, statistics::units::Count::get(),
        "number of prefetches hit in the Write Buffer"),
    ADD_STAT(pfHitInWBPerPfPC, statistics::units::Count::get(),
        "number of prefetches hit in the Write Buffer"),
    ADD_STAT(pfLate, statistics::units::Count::get(),
        "number of late prefetches (hitting in cache, MSHR or WB)"),
    ADD_STAT(pfLatePerPfPC, statistics::units::Count::get(),
        "number of late prefetches (hitting in cache, MSHR or WB)"),
    ADD_STAT(pfLateRate, statistics::units::Count::get(),
        "number of late prefetches (hitting in cache, MSHR or WB)"),
    ADD_STAT(pfLateRatePerPfPC, statistics::units::Count::get(),
        "number of late prefetches (hitting in cache, MSHR or WB)")
{
    using namespace statistics;

    pfUnused.flags(nozero);

    // timely_accuracy.flags(total);
    // timely_accuracy = pfUseful / pfIssued;

    // coverage.flags(total | nonan );
    // coverage = pfUseful / (pfUseful + demandMshrMisses);

    pfLate = pfHitInCache + pfHitInMSHR + pfHitInWB;

    pfLateRate.flags(nozero | nonan);
    pfLateRate = pfLate / pfIssued;

    // accuracy.flags(total | nonan);
    // accuracy = pfUseful / (pfIssued - pfHitInCache - pfHitInMSHR - pfHitInWB);

    /* pf_cosumed = hwpf_mshr_miss / overall_mshr_miss */
    pf_cosumed.flags(total | nonan);
    pf_cosumed = (pfIssued - pfLate) / (pfIssued - pfLate + demandMshrMisses);

    /* pf_effective = (pfUseful + demandMSHRHitAtPf) / hwpf_mshr_miss */
    pf_effective.flags(total | nonan);
    pf_effective = (pfUseful + demandMshrHitsAtPf) / (pfIssued - pfLate);

    /* pf_timely = pfUseful / (pfUseful + demandMshrHitsAtPf) */
    pf_timely.flags(total | nonan);
    pf_timely = pfUseful / (pfUseful + demandMshrHitsAtPf);

    /** NOTE: if pfLate, cache will reschedule memside's send event.
     * If next ready time is later than curTick(), 
     * cache will try schedule it at curTick()+1.
     * So pfLate will not delay the timing of the whole cache, mostly.
     */

    /* accuracy_cache = pf_effective * pf_timely */
    /*                = pfUseful / hwpf_mshr_miss            */
    accuracy_cache.flags(total | nonan);
    accuracy_cache = pfUseful / (pfIssued - pfLate);

    /** NOTE: May unfair, since we don't know the prefetch part of pfUnused,
     * not to mention how many of these are accuracy. 
     * So ignore the pfUnused part.
     */
    accuracy_prefetcher.flags(total | nonan);
    accuracy_prefetcher = (pfLate + pfUseful + demandMshrHitsAtPf) / pfIssued;

    int max_per_pc = 32;

    demandMshrMissesPerPC
        .init(max_per_pc)
        .flags(total | nozero | nonan)
        ;

    demandMshrHitsAtPfPerPfPC
        .init(max_per_pc)
        .flags(total | nozero | nonan)
        ;

    pfIssuedPerPfPC
        .init(max_per_pc)
        .flags(total | nozero | nonan)
        ;
    pfUnusedPerPfPC
        .init(max_per_pc)
        .flags(total | nozero | nonan)
        ;
    pfUsefulPerPfPC
        .init(max_per_pc)
        .flags(total | nozero | nonan)
        ;
    pfHitInCachePerPfPC
        .init(max_per_pc)
        .flags(total | nozero | nonan)
        ;
    pfHitInMSHRPerPfPC
        .init(max_per_pc)
        .flags(total | nozero | nonan)
        ;
    pfHitInWBPerPfPC
        .init(max_per_pc)
        .flags(total | nozero | nonan)
        ;
    // accuracyPerPC.flags(nozero | nonan);
    // accuracyPerPC = pfUsefulPerPfPC / 
    //     (pfIssuedPerPfPC - pfHitInCachePerPfPC - pfHitInMSHRPerPfPC - pfHitInWBPerPfPC);

    // timely_accuracy_perPfPC.flags(nozero | nonan);
    // timely_accuracy_perPfPC = pfUsefulPerPfPC / pfIssuedPerPfPC;

    // coveragePerPC.flags(nozero | nonan);
    // coveragePerPC = pfUsefulPerPfPC / (pfUsefulPerPfPC + demandMshrMissesPerPC);

    pfLatePerPfPC.flags(total | nozero | nonan);
    pfLatePerPfPC = pfHitInCachePerPfPC + pfHitInMSHRPerPfPC + pfHitInWBPerPfPC;

    pfLateRatePerPfPC.flags(nozero | nonan);
    pfLateRatePerPfPC = pfLatePerPfPC / pfIssuedPerPfPC;

    pf_cosumed_perPfPC.flags(total | nonan);
    pf_cosumed_perPfPC = (pfIssuedPerPfPC - pfLatePerPfPC) / (pfIssuedPerPfPC - pfLatePerPfPC + demandMshrMissesPerPC);

    pf_effective_perPfPC.flags(total | nonan);
    pf_effective_perPfPC = (pfUsefulPerPfPC + demandMshrHitsAtPfPerPfPC) / (pfIssuedPerPfPC - pfLatePerPfPC);

    pf_timely_perPfPC.flags(total | nonan);
    pf_timely_perPfPC = pfUsefulPerPfPC / (pfUsefulPerPfPC + demandMshrHitsAtPfPerPfPC);

    accuracy_cache_perPfPC.flags(total | nonan);
    accuracy_cache_perPfPC = pfUsefulPerPfPC / (pfIssuedPerPfPC - pfLatePerPfPC);

    accuracy_prefetcher_perPfPC.flags(total | nonan);
    accuracy_prefetcher_perPfPC = (pfLatePerPfPC + pfUsefulPerPfPC + demandMshrHitsAtPfPerPfPC) / pfIssuedPerPfPC;
}

// 在Base类的StatGroup子类中注册每个PC（程序计数器）的统计信息
// 该函数接收一个地址（PC值）的列表，并根据这些地址注册一系列的性能统计变量
// 参数:
//   stats_pc_list: 一个包含需要注册统计信息的PC地址的向量
void 
Base::StatGroup::regStatsPerPC(const std::vector<Addr> &stats_pc_list)
{
    // 使用statistics命名空间内的元素
    using namespace statistics;

    // 定义每个PC最多可以注册的统计变量数量
    int max_per_pc = 32;
    // 确保传入的PC地址数量不超过最大值
    assert(stats_pc_list.size() < max_per_pc);
    
    // 遍历PC地址列表
    for (int i = 0; i < stats_pc_list.size(); i++) {
        // 将PC地址转换为十六进制字符串，用于统计变量的命名
        std::stringstream stream;
        stream << std::hex << stats_pc_list[i];
        std::string pc_name = stream.str();

        // 为当前PC地址注册各种统计变量，使用转换后的PC名称作为子名称
        demandMshrMissesPerPC.subname(i, pc_name);
        demandMshrHitsAtPfPerPfPC.subname(i, pc_name);
        pfIssuedPerPfPC.subname(i, pc_name);
        pfUnusedPerPfPC.subname(i, pc_name); 
        pfUsefulPerPfPC.subname(i, pc_name);
        pfHitInCachePerPfPC.subname(i, pc_name);
        pfHitInMSHRPerPfPC.subname(i, pc_name);
        pfHitInWBPerPfPC.subname(i, pc_name);

        // 以下统计变量被注释掉，可能是因为它们在当前场景下不需要被注册
        // accuracyPerPC.subname(i, pc_name);
        // timely_accuracy_perPfPC.subname(i, pc_name);
        // coveragePerPC.subname(i, pc_name);
        pfLatePerPfPC.subname(i, pc_name);
        pfLateRatePerPfPC.subname(i, pc_name);

        // 这里注册更多的统计变量，反映了各种预测性能的细分统计
        pf_cosumed_perPfPC.subname(i, pc_name);
        pf_effective_perPfPC.subname(i, pc_name);
        pf_timely_perPfPC.subname(i, pc_name);
        accuracy_cache_perPfPC.subname(i, pc_name);
        accuracy_prefetcher_perPfPC.subname(i, pc_name);
    }
}

/*
 * 观察访问请求的处理逻辑
 * 
 * 此函数用于判断是否对当前的访问请求进行观察处理它根据请求包（Packet）的类型（如指令加载、读、写、失效等）
 * 以及是否为未命中（miss）来决定是否进行观察处理该函数的目的是根据缓存的行为和配置条件，决定是否需要
 * 对当前的内存访问进行观察处理，例如，是否需要预取指令或数据，是否处理读、写、失效等操作
 * 
 * @param pkt 指向请求包的智能指针，包含访问请求的所有信息
 * @param miss 布尔值，指示当前访问是否为未命中
 * @return 返回布尔值，指示当前访问是否被观察处理
 */
bool Base::observeAccess(const PacketPtr &pkt, bool miss) const
{
    // 判断当前访问是否为指令加载
    bool fetch = pkt->req->isInstFetch();
    // 判断当前访问是否为读操作
    bool read = pkt->isRead();
    // 判断当前访问是否为失效操作
    bool inv = pkt->isInvalidate();

    // 如果当前访问不是未命中
    if (!miss) {
        // 如果配置了在预取命中时进行观察处理，并且当前包已被预取，则返回true
        if (prefetchOnPfHit)
            return hasBeenPrefetched(pkt->getAddr(), pkt->isSecure());
        // 如果没有配置在访问时进行预取，则返回false
        if (!prefetchOnAccess)
            return false;
    }
    // 如果当前访问是未缓存的请求，则不进行观察处理
    if (pkt->req->isUncacheable()) return false;
    // 如果是指令加载且不观察指令，则不进行观察处理
    if (fetch && !onInst) return false;
    // 如果不是指令加载且不观察数据，则不进行观察处理
    if (!fetch && !onData) return false;
    // 如果不是指令加载且是读操作，且不观察读操作，则不进行观察处理
    if (!fetch && read && !onRead) return false;
    // 如果不是指令加载且不是读操作，且不观察写操作，则不进行观察处理
    if (!fetch && !read && !onWrite) return false;
    // 如果不是指令加载且不是读操作，且是失效操作，且不观察失效操作，则不进行观察处理
    if (!fetch && !read && inv) return false;
    // 如果当前请求是CleanEvict命令，则不进行观察处理
    if (pkt->cmd == MemCmd::CleanEvict) return false;

    // 如果配置了在未命中时进行观察处理，则根据miss的值返回结果
    if (onMiss) {
        return miss;
    }

    // 默认情况下，如果以上条件都不满足，则进行观察处理
    return true;
}

/**
 * 检查给定地址是否在缓存中。
 * 
 * 此函数委托给cache对象的inCache方法，用于判断特定的地址是否已经存在于缓存中。
 * 它考虑了地址的安全性，根据is_secure参数决定是否是安全上下文。
 * 
 * @param addr 要检查的内存地址。
 * @param is_secure 指示是否在安全上下文中运行的布尔值。
 * @return 如果地址存在于缓存中，返回true；否则返回false。
 */
bool
Base::inCache(Addr addr, bool is_secure) const
{
    return cache->inCache(addr, is_secure);
}

/**
 * 检查给定地址是否在缺失队列中。
 * 
 * 此函数委托给cache对象来查询指定地址是否当前在缺失队列中。
 * 缺失队列是用于追踪那些在缓存中未命中且正在等待从内存中取回的数据地址。
 * 
 * @param addr 要检查的内存地址。
 * @param is_secure 指示地址是否属于安全上下文的标志。
 * @return 如果地址在缺失队列中，则返回true；否则返回false。
 */
bool
Base::inMissQueue(Addr addr, bool is_secure) const
{
    return cache->inMissQueue(addr, is_secure);
}

/**
 * 检查给定地址是否已被预取
 * 
 * @param addr 要检查的地址
 * @param is_secure 指示是否是安全指令的布尔值
 * @return 如果地址已被预取，则返回true；否则返回false
 * 
 * 此函数委托给cache对象来获取结果，目的是为了确定是否对给定地址执行了预取操作
 * 预取操作有助于提高内存访问效率，在系统中可能涉及到缓存管理或者预取机制
 */
bool
Base::hasBeenPrefetched(Addr addr, bool is_secure) const
{
    return cache->hasBeenPrefetched(addr, is_secure);
}

/**
 * 判断两个地址是否在同一个页面内。
 * 
 * 页面是内存管理的基本单位，同一个页面内的地址访问可以提高内存访问效率。
 * 本函数通过将两个地址分别向下取整到其所在页面的起始地址，并比较这两个起始地址是否相同，
 * 来判断这两个地址是否在同一个页面内。
 * 
 * @param a 第一个地址。
 * @param b 第二个地址。
 * 
 * @return 如果两个地址在同一个页面内，返回true；否则返回false。
 */
bool
Base::samePage(Addr a, Addr b) const
{
    return roundDown(a, pageBytes) == roundDown(b, pageBytes);
}

/**
 * 获取给定地址的块地址
 * 
 * 该函数通过将输入地址与块大小的补数减1进行按位与操作，来计算该地址所属的块的起始地址
 * 这样可以确保任何不符合块边界要求的地址部分都被清除，使得返回的地址符合块的起始对齐要求
 * 
 * @param a 输入的地址，期望通过该函数获取其所属块的起始地址
 * @return 返回输入地址所属块的起始地址
 */
Addr
Base::blockAddress(Addr a) const
{
    return a & ~((Addr)blkSize-1);
}

// 根据给定的地址计算并返回对应的块索引
//
// 参数:
//   a - 要计算块索引的地址
//
// 返回值:
//   对应的块索引，通过将地址右移块大小的位数来计算得到
Addr Base::blockIndex(Addr a) const {
    return a >> lBlkSize;
}

/**
 * 获取给定地址的页面地址
 *
 * 此函数通过将给定的地址向下取整到页面大小的倍数来计算页面地址，
 * 这意味着结果地址将是小于或等于给定地址的最大页面边界地址。
 *
 * @param a 给定的地址
 * @return 返回计算得到的页面地址
 */
Addr
Base::pageAddress(Addr a) const
{
    return roundDown(a, pageBytes);
}

/**
 * 获取给定地址的页内偏移量。
 *
 * 本函数通过将输入地址与页大小减一的值进行按位与操作，计算出地址在所在页内的偏移量。
 * 这种方法利用了地址的二进制表示中，页内偏移部分的位是与其和页大小减一的值的按位与结果相同的特性。
 *
 * @param a 输入的地址。
 * @return 返回输入地址在所在页内的偏移量。
 */
Addr
Base::pageOffset(Addr a) const
{
    return a & (pageBytes - 1);
}

/**
 * 计算页面中特定块的地址。
 * 
 * 本函数用于根据页面地址和块索引计算特定块的地址。它是只读操作，不修改任何状态。
 * 
 * @param page 页面的起始地址。
 * @param blockIndex 块在页面中的索引，从0开始。
 * @return 返回特定块的地址。
 */
Addr
Base::pageIthBlockAddress(Addr page, uint32_t blockIndex) const
{
    // 通过将块索引左移块大小来计算块地址，并加到页面起始地址上
    return page + (blockIndex << lBlkSize);
}

// 在Base类中处理预取命中情况
void Base::prefetchHit(PacketPtr pkt, bool miss) {
    // 检查当前包的地址是否已被预取，如果是，则增加有用预取计数
    if (hasBeenPrefetched(pkt->getAddr(), pkt->isSecure())) {
        usefulPrefetches += 1;
        prefetchStats.pfUseful++;

        // 获取当前请求的程序计数器地址
        Addr req_pc = cache->getCacheBlk(pkt->getAddr(), pkt->isSecure())->getPC();
        DPRINTF(DMP, "pfuseful: PC %llx, PAddr %llx\n", 
                    req_pc, pkt->req->getPaddr());
        // 遍历程序计数器地址列表，找到对应项增加其有用预取计数
        for (int i = 0; i < stats_pc_list.size(); i++) {
            if (req_pc == stats_pc_list[i]) {
                prefetchStats.pfUsefulPerPfPC[i]++;
                break;
            }
        }

        // 如果当前请求是未命中的，增加有用预取但未命中计数
        if (miss)
            prefetchStats.pfUsefulButMiss++;
    }
}

// 在基类中处理探查通知。此函数决定是否通知预取器有关缓存未命中或其他情况的信息。
// 参数pkt：一个PacketPtr对象，指向需要探查的包。
// 参数miss：一个布尔值，指示是否存在未命中情况。
void
Base::probeNotify(const PacketPtr &pkt, bool miss)
{
    // 不在软件预取、缓存维护操作或我们正在合并的写操作时通知预取器。
    if (pkt->cmd.isSWPrefetch()) return;
    if (pkt->req->isCacheMaintenance()) return;
    if (pkt->isWrite() && cache != nullptr && cache->coalesce()) return;

    // 确保请求具有物理地址，否则引发恐慌。
    if (!pkt->req->hasPaddr()) {
        panic("Request must have a physical address");
    }

    // 如果预取器无法观察到此访问，则记录信息并返回。
    if (!observeAccess(pkt, miss)) {
        DPRINTF(HWPrefetch, "Prefetcher can't observe this access, dropped.\n");
    }

    // 验证此访问类型是否被预取器观察到。
    if (observeAccess(pkt, miss)) {
        // 如果使用虚拟地址并且包请求具有虚拟地址，则使用虚拟地址创建预取信息。
        if (useVirtualAddresses && pkt->req->hasVaddr()) {
            PrefetchInfo pfi(pkt, pkt->req->getVaddr(), miss);
            notify(pkt, pfi);
        } else if (!useVirtualAddresses) {
            // 如果不使用虚拟地址，则使用物理地址创建预取信息。
            PrefetchInfo pfi(pkt, pkt->req->getPaddr(), miss);
            notify(pkt, pfi);
        }
    }
}

void Base::regProbeListeners() {
    /**
     * 如果配置脚本没有添加任何探针，则使用“Miss”探针连接到父缓存。
     * 如果缓存配置为在访问时预取，则还连接到“Hit”探针。
     */
    if (listeners.empty() && cache != nullptr) {
        ProbeManager *pm(cache->getProbeManager());
        listeners.push_back(new PrefetchListener(*this, pm, "Miss", false, true));
        listeners.push_back(new PrefetchListener(*this, pm, "Fill", true, false));
        listeners.push_back(new PrefetchListener(*this, pm, "Hit", false, false));
    }
}

// void
// Base::addEventProbe(SimObject *obj, const char *name)
// {
//     ProbeManager *pm(obj->getProbeManager());
//     listeners.push_back(new PrefetchListener(*this, pm, name));
// }

// 向基类对象添加一个事件探查器
void
Base::addEventProbe(SimObject *obj, const char *name,
                    bool isFill=false, bool isMiss=false, bool l1_req=false, bool l1_resp=false)
{
    // 获取对象的探查器管理器
    ProbeManager *pm(obj->getProbeManager());
    // 创建并添加一个新的预取监听器到监听器列表
    listeners.push_back(new PrefetchListener(*this, pm, name, isFill, isMiss, l1_req, l1_resp));
}

/**
 * 为 Base 类添加一个 TLB（Translation Lookaside Buffer）。
 * 
 * 此函数用于将一个 BaseTLB 对象注册到 Base 类中。每个 Base 对象只允许注册一个 TLB，
 * 如果尝试添加第二个 TLB，将会触发断言错误。这是为了确保 TLB 的唯一性，避免出现冲突或重复的情况。
 * 
 * @param t 指向要注册的 BaseTLB 对象的指针。传递的所有权将归 Base 类管理，即 Base 类负责其生命周期。
 * 
 * @note 此函数使用了断言来确保只能添加一个 TLB。在发布版本中，应该使用更健壮的检查机制来替代断言。
 */
void
Base::addTLB(BaseTLB *t)
{
    fatal_if(tlb != nullptr, "Only one TLB can be registered");
    tlb = t;
}

} // namespace prefetch
} // namespace gem5
