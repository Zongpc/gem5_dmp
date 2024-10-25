/*
 * Copyright (c) 2012-2013, 2018-2019 ARM Limited
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
 * Copyright (c) 2003-2005 The Regents of The University of Michigan
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
 * Definition of BaseCache functions.
 */

#include "mem/cache/base.hh"

#include "base/compiler.hh"
#include "base/logging.hh"
#include "debug/Cache.hh"
#include "debug/CacheComp.hh"
#include "debug/CachePort.hh"
#include "debug/CacheRepl.hh"
#include "debug/CacheVerbose.hh"
#include "debug/HWPrefetch.hh"
#include "debug/Bertidebug.hh"
#include "debug/RequestSlot.hh"
#include "mem/cache/compressors/base.hh"
#include "mem/cache/mshr.hh"
#include "mem/cache/prefetch/base.hh"
#include "mem/cache/queue_entry.hh"
#include "mem/cache/tags/compressed_tags.hh"
#include "mem/cache/tags/super_blk.hh"
#include "params/BaseCache.hh"
#include "params/WriteAllocator.hh"
#include "sim/cur_tick.hh"

namespace gem5
{

BaseCache::CacheResponsePort::CacheResponsePort(const std::string &_name,
                                                BaseCache *_cache,
                                                const std::string &_label)
    :   QueuedResponsePort(_name, _cache, queue),
        queue(*_cache, *this, true, _label),
        blocked(false), mustSendRetry(false),
        sendRetryEvent([this]{ processSendRetry(); }, _name)
{
}

// BaseCache类的构造函数
// 参数p为配置参数，blk_size为块大小
BaseCache::BaseCache(const BaseCacheParams &p, unsigned blk_size)
    : ClockedObject(p), // 初始化基类ClockedObject
        cpuSidePort(p.name + ".cpu_side_port", this, "CpuSidePort"), // 初始化CPU侧端口
        memSidePort(p.name + ".mem_side_port", this, "MemSidePort"), // 初始化内存侧端口
        mshrQueue("MSHRs", p.mshrs, 0, p.demand_mshr_reserve, p.name), // 初始化MSHR队列
        writeBuffer("write buffer", p.write_buffers, p.mshrs, p.name), // 初始化写缓冲区
        tags(p.tags), // 初始化标签
        compressor(p.compressor), // 初始化压缩器
        prefetcher(p.prefetcher), // 初始化预取器
        writeAllocator(p.write_allocator), // 初始化写分配器
        writebackClean(p.writeback_clean), // 初始化写回清洁设置
        tempBlockWriteback(nullptr), // 初始化临时写回块
        // 定义一个延迟事件，用于处理临时写回块的原子操作
        writebackTempBlockAtomicEvent([this]{ writebackTempBlockAtomic(); },
                                        name(), false,
                                        EventBase::Delayed_Writeback_Pri),
        blkSize(blk_size), // 块大小
        lookupLatency(p.tag_latency), // 查找延迟
        dataLatency(p.data_latency), // 数据延迟
        forwardLatency(p.tag_latency), // 转发延迟
        fillLatency(p.data_latency), // 填充延迟
        responseLatency(p.response_latency), // 响应延迟
        sequentialAccess(p.sequential_access), // 是否顺序访问
        numTarget(p.tgts_per_mshr), // 每个MSHR的目标数量
        forwardSnoops(true), // 转发探查
        clusivity(p.clusivity), // 独占性设置
        isReadOnly(p.is_read_only), // 是否只读
        replaceExpansions(p.replace_expansions), // 替换扩展设置
        moveContractions(p.move_contractions), // 移动收缩设置
        blocked(0), // 阻塞状态
        order(0), // 顺序
        noTargetMSHR(nullptr), // 无目标MSHR
        missCount(p.max_miss_count), // 缺失计数
        addrRanges(p.addr_ranges.begin(), p.addr_ranges.end()), // 地址范围
        system(p.system), // 系统
        stats(*this),
        event([this]{processEvent();},name())
{
    // MSHR队列没有预留条目，因为我们在每次分配时都会检查MSHR队列
    // 而写队列的预留条目数量与MSHR的数量相同，因为每个MSHR最终可能需要写回
    // 并且我们在提交MSHR之前不会检查写缓冲区

    // forwardSnoops在init()中根据连接的请求方是否实际进行探查来重写

    // 初始化临时缓存块
    tempBlock = new TempCacheBlk(blkSize);

    // 初始化标签
    tags->tagsInit();
    // 如果预取器存在，设置缓存为此缓存
    if (prefetcher)
        prefetcher->setCache(this);

    // 如果启用了压缩，确保标签从CompressedTags派生
    fatal_if(compressor && !dynamic_cast<CompressedTags*>(tags),
        "The tags of compressed cache %s must derive from CompressedTags",
        name());
    // 如果启用了压缩，但标签不是从CompressedTags派生，发出警告
    warn_if(!compressor && dynamic_cast<CompressedTags*>(tags),
        "Compressed cache %s does not have a compression algorithm", name());
    // 如果启用了压缩，设置压缩器的缓存
    if (compressor)
        compressor->setCache(this);

    // 如果配置参数中提供了统计PC列表，则设置统计PC列表
    if (!p.stats_pc_list.empty()) {
        stats_pc_list = p.stats_pc_list;
    }
}

void
BaseCache::processEvent()
{
    int num;
    num = mshrQueue.allocated;

    if(num > 0){
        stats.overallmshrservices += num;
        stats.overallmshrcycles++;
    }

    schedule(event,curTick()+400);
}

void 
BaseCache::startup()
{
    schedule(event,1010);
}

BaseCache::~BaseCache()
{
    delete tempBlock;
}

void
BaseCache::CacheResponsePort::setBlocked()
{
    // 确保当前端口未被阻塞
    assert(!blocked);
    // 输出调试信息，指示端口开始阻塞新请求
    DPRINTF(CachePort, "Port is blocking new requests\n");
    blocked = true;
    // 如果已经在本周期内调度了重试，但重试尚未发生，则取消它
    if (sendRetryEvent.scheduled()) {
        owner.deschedule(sendRetryEvent);
        // 输出调试信息，指示端口取消了重试
        DPRINTF(CachePort, "Port descheduled retry\n");
        mustSendRetry = true;
    }
}

void BaseCache::CacheResponsePort::clearBlocked()
{
    // 断言当前状态是被阻塞的
    assert(blocked);
    // 打印调试信息，表示端口开始接受新请求
    DPRINTF(CachePort, "Port is accepting new requests\n");
    // 设置阻塞状态为false，表示端口不再被阻塞
    blocked = false;
    // 如果必须发送重试请求
    if (mustSendRetry) {
        // TODO: 需要找到一个更好的时间点（比如下一个周期？）
        // 调度发送重试事件，延迟到当前时钟周期的下一个周期执行
        owner.schedule(sendRetryEvent, curTick() + 1);
    }
}

/**
 * @brief BaseCache::CacheResponsePort类的processSendRetry方法
 * 
 * 该方法用于处理重发请求的操作。它首先打印出重发请求的日志信息，
 * 然后重置必须发送重发标志，并调用sendRetryReq方法实际发送重发请求。
 */
void BaseCache::CacheResponsePort::processSendRetry()
{
    DPRINTF(CachePort, "Port is sending retry\n");

    // reset the flag and call retry
    mustSendRetry = false;
    sendRetryReq();
}

/**
 * 生成缓存块地址
 * 
 * 此函数旨在根据输入的缓存块指针，返回对应的块地址
 * 如果输入的块指针不是临时块，则通过标签对象重新生成块地址
 * 如果输入的块指针是临时块，则直接返回临时块的地址
 * 
 * @param blk 指向缓存块的指针
 * @return 返回缓存块的地址
 */
Addr BaseCache::regenerateBlkAddr(CacheBlk* blk)
{
    // 判断输入的缓存块指针是否指向临时块
    if (blk != tempBlock) {
        // 如果不是临时块，调用标签对象的方法重新生成块地址
        return tags->regenerateBlkAddr(blk);
    } else {
        // 如果是临时块，直接返回临时块的地址
        return tempBlock->getAddr();
    }
}

// 初始化基础缓存
void BaseCache::init()
{
    // 确保CPU侧和内存侧端口已连接
    if (!cpuSidePort.isConnected() || !memSidePort.isConnected())
        fatal("Cache ports on %s are not connected\n", name());

    // 发送范围更改通知
    cpuSidePort.sendRangeChange();

    // 根据CPU侧端口是否进行侦听来设置转发侦听
    forwardSnoops = cpuSidePort.isSnooping();
}

Port &
BaseCache::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "mem_side") {
        return memSidePort;
    } else if (if_name == "cpu_side") {
        return cpuSidePort;
    }  else {
        return ClockedObject::getPort(if_name, idx);
    }
}

bool
BaseCache::inRange(Addr addr) const
{
    for (const auto& r : addrRanges) {
        if (r.contains(addr)) {
            return true;
        }
    }
    return false;
}

/*
该函数是 BaseCache 类的一个方法，用于处理在缓存中命中时的请求（timing request）。具体逻辑分为几部分：

处理 LockedRMW 特殊情况：
检查请求 pkt 是否为 LockedRMW 类型。若是，则根据请求类型（读/写）进行处理。
读命中:
检查不存在未完成的访问冲突。
伪造一个 MSHR（缺失状态处理寄存器），因为实际缓存命中，而 LockedRMW 需要 MSHR 处理，在此模拟其状态。
将数据块标记为不可访问，清除读和写权限。
写命中:
恢复之前操作中清除的权限。
查找并清除对应的 MSHR 记录。
伪造一个响应数据包，处理任何挂起的目标。

常规响应处理：
若请求需要响应，则准备响应并调度。
断言请求的延迟应已被消耗。
制造响应并根据请求时间（已考虑交叉开关延迟和访问延迟）调度响应。
若请求不需要响应，则打印日志并安排删除该请求包。
总结来说，该函数主要处理缓存命中情况下的各种请求，特别是针对 LockedRMW 请求的特殊处理，以及处理完请求后的响应和清理工作。*/
void
BaseCache::handleTimingReqHit(PacketPtr pkt, CacheBlk *blk, Tick request_time)
{

    // handle special cases for LockedRMW transactions
    if (pkt->isLockedRMW()) {
        Addr blk_addr = pkt->getBlockAddr(blkSize);

        if (pkt->isRead()) {
            // Read hit for LockedRMW.  Since it requires exclusive
            // permissions, there should be no outstanding access.
            assert(!mshrQueue.findMatch(blk_addr, pkt->isSecure()));
            // The keys to LockedRMW are that (1) we always have an MSHR
            // allocated during the RMW interval to catch snoops and
            // defer them until after the RMW completes, and (2) we
            // clear permissions on the block to turn any upstream
            // access other than the matching write into a miss, causing
            // it to append to the MSHR as well.

            // Because we hit in the cache, we have to fake an MSHR to
            // achieve part (1).  If the read had missed, this MSHR
            // would get allocated as part of normal miss processing.
            // Basically we need to get the MSHR in the same state as if
            // we had missed and just received the response.
            // Request *req2 = new Request(*(pkt->req));
            /*
            LockedRMW 的关键点是 (1) 在 RMW（读取-修改-写入）间隔期间，我们始终分配了一个 MSHR（缺失状态处理寄存器），
            以捕获嗅探请求并将其推迟到 RMW 完成之后处理；
            (2) 我们清除数据块的权限，将任何与写入不匹配的上游访问转为未命中，从而使其也附加到 MSHR 上。
            因为我们在缓存中命中，所以我们必须伪造一个 MSHR 来实现部分 
            (1)。如果读取未命中，此 MSHR 将作为正常未命中处理的一部分进行分配。基本上，
            我们需要使 MSHR 处于与未命中并刚收到响应时相同的状态。
            Request req2 = new Request((pkt->req));*/
            RequestPtr req2 = std::make_shared<Request>(*(pkt->req));
            PacketPtr pkt2 = new Packet(req2, pkt->cmd);
            MSHR *mshr = allocateMissBuffer(pkt2, curTick(), true);
            // Mark the MSHR "in service" (even though it's not) to prevent
            // the cache from sending out a request.
            mshrQueue.markInService(mshr, false);
            // Part (2): mark block inaccessible
            assert(blk);
            blk->clearCoherenceBits(CacheBlk::ReadableBit);
            blk->clearCoherenceBits(CacheBlk::WritableBit);
        } else {
            assert(pkt->isWrite());
            // All LockedRMW writes come here, as they cannot miss.
            // Need to undo the two things described above.  Block
            // permissions were already restored earlier in this
            // function, prior to the access() call.  Now we just need
            // to clear out the MSHR.
            /*
            // 所有 LockedRMW 写操作都会来到这里，因为它们不能未命中。
            // 需要撤销上述两个步骤。块权限已经在此函数的早期，在 access() 调用之前恢复。
            // 现在我们只需要清除 MSHR。*/
            // Read should have already allocated MSHR.
            MSHR *mshr = mshrQueue.findMatch(blk_addr, pkt->isSecure());
            assert(mshr);
            // Fake up a packet and "respond" to the still-pending
            // LockedRMWRead, to process any pending targets and clear
            // out the MSHR
            //伪造一个数据包并“响应”仍处于待处理状态的 LockedRMWRead，以处理任何挂起的目标并清除 MSHR
            PacketPtr resp_pkt =
                new Packet(pkt->req, MemCmd::LockedRMWWriteResp);
            resp_pkt->senderState = mshr;
            recvTimingResp(resp_pkt);
        }
    }

    if (pkt->needsResponse()) {
        // These delays should have been consumed by now
        assert(pkt->headerDelay == 0);
        assert(pkt->payloadDelay == 0);

        pkt->makeTimingResponse();

        // In this case we are considering request_time that takes
        // into account the delay of the xbar, if any, and just
        // lat, neglecting responseLatency, modelling hit latency
        // just as the value of lat overriden by access(), which calls
        // the calculateAccessLatency() function.
        /*在这种情况下，我们考虑了请求时间，即考虑了 xbar（交叉开关）的延迟（如果有的话）和 lat，
        而忽略了响应延迟。命中延迟仅作为由 access() 重写的 lat 值进行建模，
        而 access() 调用了 calculateAccessLatency() 函数。*/
        cpuSidePort.schedTimingResp(pkt, request_time);
    } else {
        DPRINTF(Cache, "%s satisfied %s, no response needed\n", __func__,
                pkt->print());

        // queue the packet for deletion, as the sending cache is
        // still relying on it; if the block is found in access(),
        // CleanEvict and Writeback messages will be deleted
        // here as well
        /*将数据包排队以进行删除，因为发送缓存仍依赖它；如果在 access() 
        中找到数据块，CleanEvict 和 Writeback 消息也将在这里删除。*/
        pendingDelete.reset(pkt);
    }
}

void
BaseCache::handleTimingReqMiss(PacketPtr pkt, MSHR *mshr, CacheBlk *blk,
                            Tick forward_time, Tick request_time)
{
    if (writeAllocator &&
        pkt && pkt->isWrite() && !pkt->req->isUncacheable()) {
        writeAllocator->updateMode(pkt->getAddr(), pkt->getSize(),
                                pkt->getBlockAddr(blkSize));
    }

    if (mshr) {
        /// MSHR hit
        /// @note writebacks will be checked in getNextMSHR()
        /// for any conflicting requests to the same block

        //@todo remove hw_pf here

        // Coalesce unless it was a software prefetch (see above).
        if (pkt) {
            assert(!pkt->isWriteback());
            // CleanEvicts corresponding to blocks which have
            // outstanding requests in MSHRs are simply sunk here
            // 对应于在 MSHRs 中有未完成请求的块的 CleanEvicts（清除驱逐）将直接在这里处理。
            if (pkt->cmd == MemCmd::CleanEvict) {
                pendingDelete.reset(pkt);
            } else if (pkt->cmd == MemCmd::WriteClean) {
                // A WriteClean should never coalesce with any
                // outstanding cache maintenance requests.

                // We use forward_time here because there is an
                // uncached memory write, forwarded to WriteBuffer.
                // WriteClean（写干净）不应与任何未完成的缓存维护请求合并。

                // 我们在这里使用 forward_time，因为存在未缓存的内存写操作，被转发到 WriteBuffer。

                allocateWriteBuffer(pkt, forward_time);
            } else {
                DPRINTF(Cache, "%s coalescing MSHR for %s\n", __func__,
                        pkt->print());

                assert(pkt->req->requestorId() < system->maxRequestors());
                stats.cmdStats(pkt).mshrHits[pkt->req->requestorId()]++;

                if (pkt->req->hasPC()) {
                    Addr req_pc = pkt->req->getPC();
                    for (int i = 0; i < stats_pc_list.size(); i++) {
                        if (req_pc == stats_pc_list[i]) {
                            stats.cmdStats(pkt).mshrHitsPerPC[i]++;
                            break;
                        }
                    }
                }

                if (mshr->hasTargets()) {
                    auto first_target = static_cast<MSHR::Target*>(mshr->getTarget());
                    if (first_target->source == MSHR::Target::FromPrefetcher
                        && mshr->getNumTargets() == 1) {

                        stats.cmdStats(pkt).mshrHitsAtPf[pkt->req->requestorId()]++;
                        if (pkt->req->hasPC()) {
                            for (int i = 0; i < stats_pc_list.size(); i++) {
                                Addr req_pc = pkt->req->getPC();
                                if (req_pc == stats_pc_list[i]) {
                                    stats.cmdStats(pkt).mshrHitsAtPfPerPC[i]++;
                                    break;
                                }
                            }
                        }

                        if (prefetcher && pkt->isDemand()) {
                            // should be counted as the prefetch request pc in mshr
                            Addr mshr_pc = first_target->pkt->req->hasPC() ? 
                                            first_target->pkt->req->getPC() : MaxAddr;
                            prefetcher->incrDemandMshrHitsAtPf(mshr_pc);
                        }
                        
                    }
                }

                // We use forward_time here because it is the same
                // considering new targets. We have multiple
                // requests for the same address here. It
                // specifies the latency to allocate an internal
                // buffer and to schedule an event to the queued
                // port and also takes into account the additional
                // delay of the xbar.
                /* 我们在这里使用 forward_time，因为考虑到新的目标时它是相同的。
                我们这里有多个对相同地址的请求。它指定了分配内部缓冲区和调度事件到排队端口的延迟，
                并且还考虑到了 xbar 的额外延迟。*/
                mshr->allocateTarget(pkt, forward_time, order++,
                                    allocOnFill(pkt->cmd));
                if (mshr->getNumTargets() >= numTarget) {
                    noTargetMSHR = mshr;
                    setBlocked(Blocked_NoTargets);
                    /*需要小心处理这一点……如果这个 mshr 还没有准备好（即 time > curTick()），
                    我们不希望将它移到已经准备好的 mshrs 前面
                    mshrQueue.moveToFront(mshr);*/
                    // need to be careful with this... if this mshr isn't
                    // ready yet (i.e. time > curTick()), we don't want to
                    // move it ahead of mshrs that are ready
                    // mshrQueue.moveToFront(mshr);
                }
            }
        }
    } else {
        // no MSHR
        assert(pkt->req->requestorId() < system->maxRequestors());
        stats.cmdStats(pkt).mshrMisses[pkt->req->requestorId()]++;

        if (pkt->req->hasPC()) {
            Addr req_pc = pkt->req->getPC();
            for (int i = 0; i < stats_pc_list.size(); i++) {
                if (req_pc == stats_pc_list[i]) {
                    stats.cmdStats(pkt).mshrMissesPerPC[i]++;
                    break;
                }
            }
        }

        if (prefetcher && pkt->isDemand())
            prefetcher->incrDemandMshrMisses(pkt);

        if (pkt->isEviction() || pkt->cmd == MemCmd::WriteClean) {
            // We use forward_time here because there is an
            // writeback or writeclean, forwarded to WriteBuffer.
            allocateWriteBuffer(pkt, forward_time);
        } else {
            if (blk && blk->isValid()) {
                // If we have a write miss to a valid block, we
                // need to mark the block non-readable.  Otherwise
                // if we allow reads while there's an outstanding
                // write miss, the read could return stale data
                // out of the cache block... a more aggressive
                // system could detect the overlap (if any) and
                // forward data out of the MSHRs, but we don't do
                // that yet.  Note that we do need to leave the
                // block valid so that it stays in the cache, in
                // case we get an upgrade response (and hence no
                // new data) when the write miss completes.
                // As long as CPUs do proper store/load forwarding
                // internally, and have a sufficiently weak memory
                // model, this is probably unnecessary, but at some
                // point it must have seemed like we needed it...
                /*
                如果我们对一个有效的数据块进行写未命中，我们需要将该块标记为不可读。
                否则，如果在有未完成的写未命中时允许读取，读取可能会从缓存块中返回过时的数据……
                一个更具攻击性的系统可以检测重叠（如果有的话）并从 MSHRs 中转发数据，但我们还没有做到这一点。
                请注意，我们确实需要保留块的有效性，以便它留在缓存中，以防在写未命中完成时我们得到一个升级响应（因此没有新数据）。
                只要 CPU 在内部进行适当的存储/加载转发，并且具有足够弱的内存模型，这可能是不必要的，
                但在某个时候它一定看起来是必需的……*/
                assert((pkt->needsWritable() &&
                    !blk->isSet(CacheBlk::WritableBit)) ||
                    pkt->req->isCacheMaintenance());
                blk->clearCoherenceBits(CacheBlk::ReadableBit);
            }
            // Here we are using forward_time, modelling the latency of
            // a miss (outbound) just as forwardLatency, neglecting the
            // lookupLatency component.
            /*在这里，我们使用 forward_time，建模未命中（外向）的延迟，
            就像 forwardLatency 一样，忽略了 lookupLatency 组件。*/
            allocateMissBuffer(pkt, forward_time);
        }
    }
}

void
BaseCache::recvTimingReq(PacketPtr pkt)
{
    // anything that is merely forwarded pays for the forward latency and
    // the delay provided by the crossbar
    //任何只是被转发的操作都需要支付转发延迟和交叉开关（crossbar）提供的延迟。
    Tick forward_time = clockEdge(forwardLatency) + pkt->headerDelay;

    if (pkt->cmd == MemCmd::LockedRMWWriteReq) {
        // For LockedRMW accesses, we mark the block inaccessible after the
        // read (see below), to make sure no one gets in before the write.
        // Now that the write is here, mark it accessible again, so the
        // write will succeed.  LockedRMWReadReq brings the block in in
        // exclusive mode, so we know it was previously writable.
        /*
        对于 LockedRMW 访问，在读取之后我们将块标记为不可访问（见下文），以确保在写操作之前没有人能够访问它。
        现在写操作已经到达，将其重新标记为可访问，以确保写操作成功。
        LockedRMWReadReq 以独占模式将块引入，因此我们知道它之前是可写的。*/
        CacheBlk *blk = tags->findBlock(pkt->getAddr(), pkt->isSecure());
        assert(blk && blk->isValid());
        assert(!blk->isSet(CacheBlk::WritableBit) &&
                !blk->isSet(CacheBlk::ReadableBit));
        blk->setCoherenceBits(CacheBlk::ReadableBit);
        blk->setCoherenceBits(CacheBlk::WritableBit);
    }

    Cycles lat;
    CacheBlk *blk = nullptr;
    bool satisfied = false;
    {
        PacketList writebacks;
        // Note that lat is passed by reference here. The function
        // access() will set the lat value.
        //请注意，lat 是通过引用传递的。函数 access() 将会设置 lat 的值。
        satisfied = access(pkt, blk, lat, writebacks);

        // After the evicted blocks are selected, they must be forwarded
        // to the write buffer to ensure they logically precede anything
        // happening below
        /*
        在选择被驱逐的块之后，它们必须被转发到写缓冲区，以确保它们在逻辑上优先于下面发生的任何操作。*/
        doWritebacks(writebacks, clockEdge(lat + forwardLatency));
    }

    // Here we charge the headerDelay that takes into account the latencies
    // of the bus, if the packet comes from it.
    // The latency charged is just the value set by the access() function.
    // In case of a hit we are neglecting response latency.
    // In case of a miss we are neglecting forward latency.
    /*在这里，我们收取 headerDelay，考虑了总线的延迟（如果数据包来自总线的话）。
    收取的延迟就是由 access() 函数设置的值。
    在命中的情况下，我们忽略了响应延迟。
    在未命中的情况下，我们忽略了转发延迟。*/
    Tick request_time = clockEdge(lat);
    // Here we reset the timing of the packet.
    pkt->headerDelay = pkt->payloadDelay = 0;

    if (satisfied) {
        // notify before anything else as later handleTimingReqHit might turn
        // the packet in a response
        /*在其他操作之前进行通知，因为稍后的 handleTimingReqHit 可能会将数据包转变为响应。*/
        ppHit->notify(pkt);

        if (prefetcher && blk && blk->wasPrefetched()) {
            DPRINTF(Cache, "Hit on prefetch for addr %#x (%s)\n",
                    pkt->getAddr(), pkt->isSecure() ? "s" : "ns");
            prefetcher->prefetchHit(pkt, false);
            blk->clearPrefetched();
            stats.prefetchHits++;
        }

        handleTimingReqHit(pkt, blk, request_time);
    } else {

        handleTimingReqMiss(pkt, blk, forward_time, request_time);

        if (prefetcher) {
            prefetcher->prefetchHit(pkt, true);
        }

        ppMiss->notify(pkt);
    }

    if (prefetcher) {
        // track time of availability of next prefetch, if any
        //跟踪下一个预取的可用时间（如果有的话）。
        Tick next_pf_time = prefetcher->nextPrefetchReadyTime();
        if (next_pf_time != MaxTick) {
            schedMemSideSendEvent(next_pf_time);
        }
    }
}

// 处理不可缓存写响应
void BaseCache::handleUncacheableWriteResp(PacketPtr pkt)
{
    // 计算响应完成时间，包括时钟周期延迟和报头与负载延迟
    Tick completion_time = clockEdge(responseLatency) +
        pkt->headerDelay + pkt->payloadDelay;

    // 重置总线额外时间，因为这些时间现在已被考虑
    pkt->headerDelay = pkt->payloadDelay = 0;

    // 预约响应，将响应包发送到CPU侧端口
    cpuSidePort.schedTimingResp(pkt, completion_time);
}

void
BaseCache::recvTimingResp(PacketPtr pkt)
{
    assert(pkt->isResponse());

    // all header delay should be paid for by the crossbar, unless
    // this is a prefetch response from above
    // 所有的头延迟应该由交叉开关（crossbar）支付，除非这是来自上游的预取响应。
    panic_if(pkt->headerDelay != 0 && pkt->cmd != MemCmd::HardPFResp,
            "%s saw a non-zero packet delay\n", name());

    const bool is_error = pkt->isError();

    if (is_error) {
        DPRINTF(Cache, "%s: Cache received %s with error\n", __func__,
                pkt->print());
    }

    DPRINTF(Cache, "%s: Handling response %s\n", __func__,
            pkt->print());

    // if this is a write, we should be looking at an uncacheable write
    if (pkt->isWrite() && pkt->cmd != MemCmd::LockedRMWWriteResp) {
        assert(pkt->req->isUncacheable());
        handleUncacheableWriteResp(pkt);
        return;
    }

    // we have dealt with any (uncacheable) writes above, from here on
    // we know we are dealing with an MSHR due to a miss or a prefetch
    //我们已经处理了上面任何（不可缓存的）写操作，从这里开始，我们知道我们正在处理的是由于缺失或预取引起的 MSHR。
    MSHR *mshr = dynamic_cast<MSHR*>(pkt->popSenderState());
    assert(mshr);

    if (mshr == noTargetMSHR) {
        // we always clear at least one target
        clearBlocked(Blocked_NoTargets);
        noTargetMSHR = nullptr;
    }

    // Initial target is used just for stats
    const QueueEntry::Target *initial_tgt = mshr->getTarget();
    const Tick miss_latency = curTick() - initial_tgt->recvTime;
    if (pkt->req->isUncacheable()) {
        assert(pkt->req->requestorId() < system->maxRequestors());
        stats.cmdStats(initial_tgt->pkt)
            .mshrUncacheableLatency[pkt->req->requestorId()] += miss_latency;
    } else {
        assert(pkt->req->requestorId() < system->maxRequestors());
        stats.cmdStats(initial_tgt->pkt)
            .mshrMissLatency[pkt->req->requestorId()] += miss_latency;
    }

    PacketList writebacks;

    bool is_fill = !mshr->isForward &&
        (pkt->isRead() || pkt->cmd == MemCmd::UpgradeResp ||
            mshr->wasWholeLineWrite);

    // make sure that if the mshr was due to a whole line write then
    // the response is an invalidation
    // 确保如果 MSHR 是由于整行写入引起的，那么响应应当是无效化操作。
    assert(!mshr->wasWholeLineWrite || pkt->isInvalidate());

    CacheBlk *blk = tags->findBlock(pkt->getAddr(), pkt->isSecure());

    if (is_fill && !is_error) {
        DPRINTF(Cache, "Block for addr %#llx being updated in Cache\n",
                pkt->getAddr());

        const bool allocate = (writeAllocator && mshr->wasWholeLineWrite) ?
            writeAllocator->allocate() : mshr->allocOnFill();
        blk = handleFill(pkt, blk, writebacks, allocate);
        assert(blk != nullptr);
    }

    // Don't want to promote the Locked RMW Read until
    // the locked write comes in
    //  在锁定的写操作到达之前，不希望提升锁定的RMW读操作。
    if (!mshr->hasLockedRMWReadTarget()) {
        if (blk && blk->isValid() && pkt->isClean() && !pkt->isInvalidate()) {
            // The block was marked not readable while there was a pending
            // cache maintenance operation, restore its flag.
            // 当有待处理的缓存维护操作时，该块被标记为不可读，现在恢复其标志。
            blk->setCoherenceBits(CacheBlk::ReadableBit);

            // This was a cache clean operation (without invalidate)
            // and we have a copy of the block already. Since there
            // is no invalidation, we can promote targets that don't
            // require a writable copy
            // 这是一次缓存清理操作（没有无效化），并且我们已经有了该块的副本。由于没有无效化操作，我们可以提升那些不需要可写副本的目标。
            mshr->promoteReadable();
        }

        if (blk && blk->isSet(CacheBlk::WritableBit) &&
            !pkt->req->isCacheInvalidate()) {
            // If at this point the referenced block is writable and the
            // response is not a cache invalidate, we promote targets that
            // were deferred as we couldn't guarrantee a writable copy
            //如果此时引用的块是可写的且响应不是缓存无效化操作，我们可以提升那些因无法保证可写副本而被推迟的目标。
            mshr->promoteWritable();
        }
    }

    serviceMSHRTargets(mshr, pkt, blk);
    // We are stopping servicing targets early for the Locked RMW Read until
    // the write comes.
    // 我们在锁定的RMW读操作完成之前，会提前停止服务目标。
    if (!mshr->hasLockedRMWReadTarget()) {
        if (mshr->promoteDeferredTargets()) {
            // avoid later read getting stale data while write miss is
            // outstanding.. see comment in timingAccess()
            //避免在写操作缺失未完成时，后续读取得到过时数据。请参见 timingAccess() 中的注释。
            if (blk) {
                blk->clearCoherenceBits(CacheBlk::ReadableBit);
            }
            mshrQueue.markPending(mshr);
            schedMemSideSendEvent(clockEdge() + pkt->payloadDelay);
        } else {
            // while we deallocate an mshr from the queue we still have to
            // check the isFull condition before and after as we might
            // have been using the reserved entries already
            // 在从队列中释放一个 MSHR 时，我们仍然需要在释放前后检查是否已满的条件，因为我们可能已经在使用保留的条目。
            const bool was_full = mshrQueue.isFull();
            mshrQueue.deallocate(mshr);
            if (was_full && !mshrQueue.isFull()) {
                clearBlocked(Blocked_NoMSHRs);
            }

            // Request the bus for a prefetch if this deallocation freed enough
            // MSHRs for a prefetch to take place
            // 如果此次释放操作释放了足够的 MSHR 以进行预取，则请求总线进行预取操作。
            if (prefetcher && mshrQueue.canPrefetch() && !isBlocked()) {
                Tick next_pf_time = std::max(
                    prefetcher->nextPrefetchReadyTime(), clockEdge());
                if (next_pf_time != MaxTick)
                    schedMemSideSendEvent(next_pf_time);
            }
        }

        // if we used temp block, check to see if its valid and then clear it
        // 如果我们使用了临时块，检查其是否有效，然后清除它。
        if (blk == tempBlock && tempBlock->isValid()) {
            evictBlock(blk, writebacks);
        }
    }

    const Tick forward_time = clockEdge(forwardLatency) + pkt->headerDelay;
    // copy writebacks to write buffer
    doWritebacks(writebacks, forward_time);

    DPRINTF(CacheVerbose, "%s: Leaving with %s\n", __func__, pkt->print());
    delete pkt;
}


Tick
BaseCache::recvAtomic(PacketPtr pkt)
{
    // should assert here that there are no outstanding MSHRs or
    // writebacks... that would mean that someone used an atomic
    // access in timing mode
    // 应该在这里断言没有未完成的 MSHR 或写回操作... 这意味着可能有人在时序模式下使用了原子访问。
    // We use lookupLatency here because it is used to specify the latency
    // to access.
    // 我们在这里使用 lookupLatency，因为它用于指定访问延迟。
    Cycles lat = lookupLatency;

    CacheBlk *blk = nullptr;
    PacketList writebacks;
    bool satisfied = access(pkt, blk, lat, writebacks);

    if (pkt->isClean() && blk && blk->isSet(CacheBlk::DirtyBit)) {
        // A cache clean opearation is looking for a dirty
        // block. If a dirty block is encountered a WriteClean
        // will update any copies to the path to the memory
        // until the point of reference.
        // 缓存清理操作会寻找一个脏块。如果遇到脏块，WriteClean 操作将更新到内存路径上的任何副本，直到引用点为止。
        DPRINTF(CacheVerbose, "%s: packet %s found block: %s\n",
                __func__, pkt->print(), blk->print());
        PacketPtr wb_pkt = writecleanBlk(blk, pkt->req->getDest(), pkt->id);
        writebacks.push_back(wb_pkt);
        pkt->setSatisfied();
    }

    // handle writebacks resulting from the access here to ensure they
    // logically precede anything happening below
    // 在这里处理由于访问操作引起的写回，以确保它们在逻辑上先于下面发生的任何操作。
    doWritebacksAtomic(writebacks);
    assert(writebacks.empty());

    if (!satisfied) {
        lat += handleAtomicReqMiss(pkt, blk, writebacks);
    }

    // Note that we don't invoke the prefetcher at all in atomic mode.
    // It's not clear how to do it properly, particularly for
    // prefetchers that aggressively generate prefetch candidates and
    // rely on bandwidth contention to throttle them; these will tend
    // to pollute the cache in atomic mode since there is no bandwidth
    // contention.  If we ever do want to enable prefetching in atomic
    // mode, though, this is the place to do it... see timingAccess()
    // for an example (though we'd want to issue the prefetch(es)
    // immediately rather than calling requestMemSideBus() as we do
    // there).
    /*
    // 请注意，我们在原子模式下完全不调用预取器。
    // 目前还不清楚如何正确地执行这操作，特别是对于那些积极生成预取候选并依赖带宽争用来限制它们的预取器；
    由于在原子模式下没有带宽争用，这些预取器往往会污染缓存。
    // 如果将来我们想要在原子模式下启用预取，这里是执行的地方……参见 timingAccess() 中的示例
    （尽管我们希望立即发出预取请求，而不是像在那里那样调用 requestMemSideBus()）。*/

    // do any writebacks resulting from the response handling
    // 执行响应处理过程中产生的任何写回操作。
    doWritebacksAtomic(writebacks);

    // if we used temp block, check to see if its valid and if so
    // clear it out, but only do so after the call to recvAtomic is
    // finished so that any downstream observers (such as a snoop
    // filter), first see the fill, and only then see the eviction
    /*
    如果我们使用了临时块，检查其是否有效，如果有效，则清除它，
    但只在 recvAtomic 调用完成后进行，以便任何下游观察者（如窥探过滤器）首先看到填充操作，然后才看到驱逐操作。*/
    if (blk == tempBlock && tempBlock->isValid()) {
        // the atomic CPU calls recvAtomic for fetch and load/store
        // sequentuially, and we may already have a tempBlock
        // writeback from the fetch that we have not yet sent
        // 原子 CPU 依次调用 recvAtomic 进行取值和加载/存储操作，我们可能已经有了来自取值操作的临时块写回，但尚未发送。
        if (tempBlockWriteback) {
            // if that is the case, write the prevoius one back, and
            // do not schedule any new event
            // 如果是这种情况，将之前的写回操作完成，并且不要安排任何新的事件。
            writebackTempBlockAtomic();
        } else {
            // the writeback/clean eviction happens after the call to
            // recvAtomic has finished (but before any successive
            // calls), so that the response handling from the fill is
            // allowed to happen first
            // 写回/清理驱逐操作在 recvAtomic 调用完成后（但在任何后续调用之前）进行，以便填充操作的响应处理能够优先发生。
            schedule(writebackTempBlockAtomicEvent, curTick());
        }

        tempBlockWriteback = evictBlock(blk);
    }

    if (pkt->needsResponse()) {
        pkt->makeAtomicResponse();
    }

    return lat * clockPeriod();
}

void
BaseCache::functionalAccess(PacketPtr pkt, bool from_cpu_side)
{
    Addr blk_addr = pkt->getBlockAddr(blkSize);
    bool is_secure = pkt->isSecure();
    CacheBlk *blk = tags->findBlock(pkt->getAddr(), is_secure);
    MSHR *mshr = mshrQueue.findMatch(blk_addr, is_secure);

    pkt->pushLabel(name());

    CacheBlkPrintWrapper cbpw(blk);

    // Note that just because an L2/L3 has valid data doesn't mean an
    // L1 doesn't have a more up-to-date modified copy that still
    // needs to be found.  As a result we always update the request if
    // we have it, but only declare it satisfied if we are the owner.

    // see if we have data at all (owned or otherwise)
    bool have_data = blk && blk->isValid()
        && pkt->trySatisfyFunctional(&cbpw, blk_addr, is_secure, blkSize,
                                    blk->data);

    // data we have is dirty if marked as such or if we have an
    // in-service MSHR that is pending a modified line
    bool have_dirty =
        have_data && (blk->isSet(CacheBlk::DirtyBit) ||
                    (mshr && mshr->inService && mshr->isPendingModified()));

    bool done = have_dirty ||
        cpuSidePort.trySatisfyFunctional(pkt) ||
        mshrQueue.trySatisfyFunctional(pkt) ||
        writeBuffer.trySatisfyFunctional(pkt) ||
        memSidePort.trySatisfyFunctional(pkt);

    DPRINTF(CacheVerbose, "%s: %s %s%s%s\n", __func__,  pkt->print(),
            (blk && blk->isValid()) ? "valid " : "",
            have_data ? "data " : "", done ? "done " : "");

    // We're leaving the cache, so pop cache->name() label
    pkt->popLabel();

    if (done) {
        pkt->makeResponse();
    } else {
        // if it came as a request from the CPU side then make sure it
        // continues towards the memory side
        if (from_cpu_side) {
            memSidePort.sendFunctional(pkt);
        } else if (cpuSidePort.isSnooping()) {
            // if it came from the memory side, it must be a snoop request
            // and we should only forward it if we are forwarding snoops
            cpuSidePort.sendFunctionalSnoop(pkt);
        }
    }
}

// 更新缓存块数据的函数
// 参数:
// - blk: 指向需要更新的缓存块的指针
// - cpkt: 指向包含新数据的包的指针，可能为nullptr
// - has_old_data: 标志位，指示缓存块中是否之前有数据
void
BaseCache::updateBlockData(CacheBlk *blk, const PacketPtr cpkt,
    bool has_old_data)
{
    // 创建数据更新对象，包含新生成的块地址和安全属性
    DataUpdate data_update(regenerateBlkAddr(blk), blk->isSecure());

    // 如果有数据更新监听器
    if (ppDataUpdate->hasListeners()) {
        // 如果缓存块中有旧数据
        if (has_old_data) {
            // 将旧数据复制到data_update的oldData中
            data_update.oldData = std::vector<uint64_t>(blk->data,
                blk->data + (blkSize / sizeof(uint64_t)));
        }
    }

    // 实际执行数据更新操作
    if (cpkt) {
        // 如果cpkt不为空，将包中的数据写入缓存块
        cpkt->writeDataToBlock(blk->data, blkSize);
    }

    // 如果有数据更新监听器
    if (ppDataUpdate->hasListeners()) {
        // 如果cpkt不为空
        if (cpkt) {
            // 将新数据复制到data_update的newData中
            data_update.newData = std::vector<uint64_t>(blk->data,
                blk->data + (blkSize / sizeof(uint64_t)));
        }
        // 通知所有监听器数据更新事件
        ppDataUpdate->notify(data_update);
    }
}

void
BaseCache::cmpAndSwap(CacheBlk *blk, PacketPtr pkt)
{
    assert(pkt->isRequest());

    uint64_t overwrite_val;
    bool overwrite_mem;
    uint64_t condition_val64;
    uint32_t condition_val32;

    int offset = pkt->getOffset(blkSize);
    uint8_t *blk_data = blk->data + offset;

    assert(sizeof(uint64_t) >= pkt->getSize());

    // Get a copy of the old block's contents for the probe before the update
    DataUpdate data_update(regenerateBlkAddr(blk), blk->isSecure());
    if (ppDataUpdate->hasListeners()) {
        data_update.oldData = std::vector<uint64_t>(blk->data,
            blk->data + (blkSize / sizeof(uint64_t)));
    }

    overwrite_mem = true;
    // keep a copy of our possible write value, and copy what is at the
    // memory address into the packet
    pkt->writeData((uint8_t *)&overwrite_val);
    pkt->setData(blk_data);

    if (pkt->req->isCondSwap()) {
        if (pkt->getSize() == sizeof(uint64_t)) {
            condition_val64 = pkt->req->getExtraData();
            overwrite_mem = !std::memcmp(&condition_val64, blk_data,
                                        sizeof(uint64_t));
        } else if (pkt->getSize() == sizeof(uint32_t)) {
            condition_val32 = (uint32_t)pkt->req->getExtraData();
            overwrite_mem = !std::memcmp(&condition_val32, blk_data,
                                        sizeof(uint32_t));
        } else
            panic("Invalid size for conditional read/write\n");
    }

    if (overwrite_mem) {
        std::memcpy(blk_data, &overwrite_val, pkt->getSize());
        blk->setCoherenceBits(CacheBlk::DirtyBit);

        if (ppDataUpdate->hasListeners()) {
            data_update.newData = std::vector<uint64_t>(blk->data,
                blk->data + (blkSize / sizeof(uint64_t)));
            ppDataUpdate->notify(data_update);
        }
    }
}

QueueEntry*
BaseCache::getNextQueueEntry()
{
    // Check both MSHR queue and write buffer for potential requests,
    // note that null does not mean there is no request, it could
    // simply be that it is not ready
    MSHR *miss_mshr  = mshrQueue.getNext();
    WriteQueueEntry *wq_entry = writeBuffer.getNext();

    // If we got a write buffer request ready, first priority is a
    // full write buffer, otherwise we favour the miss requests
    if (wq_entry && (writeBuffer.isFull() || !miss_mshr)) {
        // need to search MSHR queue for conflicting earlier miss.
        MSHR *conflict_mshr = mshrQueue.findPending(wq_entry);

        if (conflict_mshr && conflict_mshr->order < wq_entry->order) {
            // Service misses in order until conflict is cleared.
            DPRINTF(RequestSlot, "[Ready] WB full and conflicted with MSHR: %s\n", conflict_mshr->print());
            return conflict_mshr;

            // @todo Note that we ignore the ready time of the conflict here
        }

        // No conflicts; issue write
        DPRINTF(RequestSlot, "[Ready] WB full and no conflicted MSHR: %s\n", wq_entry->print());
        return wq_entry;
    } else if (miss_mshr) {
        // need to check for conflicting earlier writeback
        WriteQueueEntry *conflict_mshr = writeBuffer.findPending(miss_mshr);
        if (conflict_mshr) {
            // not sure why we don't check order here... it was in the
            // original code but commented out.

            // The only way this happens is if we are
            // doing a write and we didn't have permissions
            // then subsequently saw a writeback (owned got evicted)
            // We need to make sure to perform the writeback first
            // To preserve the dirty data, then we can issue the write

            // should we return wq_entry here instead?  I.e. do we
            // have to flush writes in order?  I don't think so... not
            // for Alpha anyway.  Maybe for x86?
            DPRINTF(RequestSlot, "[Ready] MSHR conflicted with WB: %s\n", conflict_mshr->print());
            return conflict_mshr;

            // @todo Note that we ignore the ready time of the conflict here
        }

        // No conflicts; issue read
        DPRINTF(RequestSlot, "[Ready] MSHR fine with WB: %s\n", miss_mshr->print());
        return miss_mshr;
    }

    // fall through... no pending requests.  Try a prefetch.
    assert(!miss_mshr && !wq_entry);

    // for debug trace
    if (!prefetcher) 
        DPRINTF(RequestSlot, "[Failed] No available prefetcher\n");
    else if (isBlocked()) 
        DPRINTF(RequestSlot, "[Failed] Cache Blocked\n");
    else if (!mshrQueue.canPrefetch()) 
        DPRINTF(RequestSlot, "[Failed] MSHR resource not enough\n");

    // do prefetch try
    if (prefetcher && mshrQueue.canPrefetch() && !isBlocked()) {
        // If we have a miss queue slot, we can try a prefetch
        PacketPtr pkt = prefetcher->getPacket();
        Tick cur_tick = curTick();
        if (pkt) {
            Addr pf_addr = pkt->getBlockAddr(blkSize);
            if (tags->findBlock(pf_addr, pkt->isSecure())) {
                DPRINTF(HWPrefetch, "Prefetch %#x has hit in cache, "
                        "dropped.\n", pf_addr);
                DPRINTF(RequestSlot, "[Failed] Prefetch droped\n");
                prefetcher->pfHitInCache(pkt);
                
                CacheBlk* try_cache_blk = getCacheBlk(pf_addr, pkt->isSecure());

                if (try_cache_blk != nullptr && try_cache_blk->data) {
                    DPRINTF(HWPrefetch, "getNextQueueEntry: PC %llx, PAddr %llx\n", 
                    pkt->req->getPC(), pkt->req->getPaddr());
                    prefetcher->notifyFill(pkt, try_cache_blk->data);
                }
                // CacheBlk* try_cache_blk = getCacheBlk(pkt->getAddr(), pkt->isSecure());
                // Cycles abandoned_lat;
                // CacheBlk* try_cache_blk = tags->accessBlock(pkt, abandoned_lat);
                // if (try_cache_blk && try_cache_blk->data) {//xymc
                //     prefetcher->hitTrigger(
                //         pkt->req->hasPC() ? pkt->req->getPC() : MaxAddr,
                //         pkt->req->getPaddr(), try_cache_blk->data,
                //         false
                //     );
                // }
                // free the request and packet
                delete pkt;
            } else if (mshrQueue.findMatch(pf_addr, pkt->isSecure())) {
                DPRINTF(HWPrefetch, "Prefetch %#x has hit in a MSHR, "
                        "dropped.\n", pf_addr);
                DPRINTF(RequestSlot, "[Failed] Prefetch droped\n");
                prefetcher->pfHitInMSHR(pkt);
                // free the request and packet
                delete pkt;
            } else if (writeBuffer.findMatch(pf_addr, pkt->isSecure())) {
                DPRINTF(HWPrefetch, "Prefetch %#x has hit in the "
                        "Write Buffer, dropped.\n", pf_addr);
                DPRINTF(RequestSlot, "[Failed] Prefetch droped\n");
                prefetcher->pfHitInWB(pkt);
                // free the request and packet
                delete pkt;
            } else {
                // Update statistic on number of prefetches issued
                // (hwpf_mshr_misses)
                assert(pkt->req->requestorId() < system->maxRequestors());
                stats.cmdStats(pkt).mshrMisses[pkt->req->requestorId()]++;

                // allocate an MSHR and return it, note
                // that we send the packet straight away, so do not
                // schedule the send
                DPRINTF(RequestSlot, "[Ready] Prefetch chance: may drop, check debug::HWPrefetch\n");
                return allocateMissBuffer(pkt, curTick(), false);
            }
        } else {
            //break;
            DPRINTF(RequestSlot, "[Failed] No available prefetch pkt\n");
        }
    }

    return nullptr;
}

bool
BaseCache::handleEvictions(std::vector<CacheBlk*> &evict_blks,
    PacketList &writebacks)
{
    bool replacement = false;
    for (const auto& blk : evict_blks) {
        if (blk->isValid()) {
            replacement = true;

            const MSHR* mshr =
                mshrQueue.findMatch(regenerateBlkAddr(blk), blk->isSecure());
            if (mshr) {
                // Must be an outstanding upgrade or clean request on a block
                // we're about to replace
                assert((!blk->isSet(CacheBlk::WritableBit) &&
                    mshr->needsWritable()) || mshr->isCleaning());
                return false;
            }
        }
    }

    // The victim will be replaced by a new entry, so increase the replacement
    // counter if a valid block is being replaced
    if (replacement) {
        stats.replacements++;

        // Evict valid blocks associated to this victim block
        for (auto& blk : evict_blks) {
            if (blk->isValid()) {
                evictBlock(blk, writebacks);
            }
        }
    }

    return true;
}

bool
BaseCache::updateCompressionData(CacheBlk *&blk, const uint64_t* data,
                                PacketList &writebacks)
{
    // tempBlock does not exist in the tags, so don't do anything for it.
    if (blk == tempBlock) {
        return true;
    }

    // The compressor is called to compress the updated data, so that its
    // metadata can be updated.
    Cycles compression_lat = Cycles(0);
    Cycles decompression_lat = Cycles(0);
    const auto comp_data =
        compressor->compress(data, compression_lat, decompression_lat);
    std::size_t compression_size = comp_data->getSizeBits();

    // Get previous compressed size
    CompressionBlk* compression_blk = static_cast<CompressionBlk*>(blk);
    [[maybe_unused]] const std::size_t prev_size =
        compression_blk->getSizeBits();

    // If compressed size didn't change enough to modify its co-allocatability
    // there is nothing to do. Otherwise we may be facing a data expansion
    // (block passing from more compressed to less compressed state), or a
    // data contraction (less to more).
    bool is_data_expansion = false;
    bool is_data_contraction = false;
    const CompressionBlk::OverwriteType overwrite_type =
        compression_blk->checkExpansionContraction(compression_size);
    std::string op_name = "";
    if (overwrite_type == CompressionBlk::DATA_EXPANSION) {
        op_name = "expansion";
        is_data_expansion = true;
    } else if ((overwrite_type == CompressionBlk::DATA_CONTRACTION) &&
        moveContractions) {
        op_name = "contraction";
        is_data_contraction = true;
    }

    // If block changed compression state, it was possibly co-allocated with
    // other blocks and cannot be co-allocated anymore, so one or more blocks
    // must be evicted to make room for the expanded/contracted block
    std::vector<CacheBlk*> evict_blks;
    if (is_data_expansion || is_data_contraction) {
        std::vector<CacheBlk*> evict_blks;
        bool victim_itself = false;
        CacheBlk *victim = nullptr;
        if (replaceExpansions || is_data_contraction) {
            victim = tags->findVictim(regenerateBlkAddr(blk),
                blk->isSecure(), compression_size, evict_blks);

            // It is valid to return nullptr if there is no victim
            if (!victim) {
                return false;
            }

            // If the victim block is itself the block won't need to be moved,
            // and the victim should not be evicted
            if (blk == victim) {
                victim_itself = true;
                auto it = std::find_if(evict_blks.begin(), evict_blks.end(),
                    [&blk](CacheBlk* evict_blk){ return evict_blk == blk; });
                evict_blks.erase(it);
            }

            // Print victim block's information
            DPRINTF(CacheRepl, "Data %s replacement victim: %s\n",
                op_name, victim->print());
        } else {
            // If we do not move the expanded block, we must make room for
            // the expansion to happen, so evict every co-allocated block
            const SuperBlk* superblock = static_cast<const SuperBlk*>(
                compression_blk->getSectorBlock());
            for (auto& sub_blk : superblock->blks) {
                if (sub_blk->isValid() && (blk != sub_blk)) {
                    evict_blks.push_back(sub_blk);
                }
            }
        }

        // Try to evict blocks; if it fails, give up on update
        if (!handleEvictions(evict_blks, writebacks)) {
            return false;
        }

        DPRINTF(CacheComp, "Data %s: [%s] from %d to %d bits\n",
                op_name, blk->print(), prev_size, compression_size);

        if (!victim_itself && (replaceExpansions || is_data_contraction)) {
            // Move the block's contents to the invalid block so that it now
            // co-allocates with the other existing superblock entry
            tags->moveBlock(blk, victim);
            blk = victim;
            compression_blk = static_cast<CompressionBlk*>(blk);
        }
    }

    // Update the number of data expansions/contractions
    if (is_data_expansion) {
        stats.dataExpansions++;
    } else if (is_data_contraction) {
        stats.dataContractions++;
    }

    compression_blk->setSizeBits(compression_size);
    compression_blk->setDecompressionLatency(decompression_lat);

    return true;
}

void
BaseCache::satisfyRequest(PacketPtr pkt, CacheBlk *blk, bool, bool)
{
    assert(pkt->isRequest());

    assert(blk && blk->isValid());
    // Occasionally this is not true... if we are a lower-level cache
    // satisfying a string of Read and ReadEx requests from
    // upper-level caches, a Read will mark the block as shared but we
    // can satisfy a following ReadEx anyway since we can rely on the
    // Read requestor(s) to have buffered the ReadEx snoop and to
    // invalidate their blocks after receiving them.
    // assert(!pkt->needsWritable() || blk->isSet(CacheBlk::WritableBit));
    assert(pkt->getOffset(blkSize) + pkt->getSize() <= blkSize);

    // Check RMW operations first since both isRead() and
    // isWrite() will be true for them
    if (pkt->cmd == MemCmd::SwapReq) {
        if (pkt->isAtomicOp()) {
            // Get a copy of the old block's contents for the probe before
            // the update
            DataUpdate data_update(regenerateBlkAddr(blk), blk->isSecure());
            if (ppDataUpdate->hasListeners()) {
                data_update.oldData = std::vector<uint64_t>(blk->data,
                    blk->data + (blkSize / sizeof(uint64_t)));
            }

            // extract data from cache and save it into the data field in
            // the packet as a return value from this atomic op
            int offset = tags->extractBlkOffset(pkt->getAddr());
            uint8_t *blk_data = blk->data + offset;
            pkt->setData(blk_data);

            // execute AMO operation
            (*(pkt->getAtomicOp()))(blk_data);

            // Inform of this block's data contents update
            if (ppDataUpdate->hasListeners()) {
                data_update.newData = std::vector<uint64_t>(blk->data,
                    blk->data + (blkSize / sizeof(uint64_t)));
                ppDataUpdate->notify(data_update);
            }

            // set block status to dirty
            blk->setCoherenceBits(CacheBlk::DirtyBit);
        } else {
            cmpAndSwap(blk, pkt);
        }
    } else if (pkt->isWrite()) {
        // we have the block in a writable state and can go ahead,
        // note that the line may be also be considered writable in
        // downstream caches along the path to memory, but always
        // Exclusive, and never Modified
        assert(blk->isSet(CacheBlk::WritableBit));
        // Write or WriteLine at the first cache with block in writable state
        if (blk->checkWrite(pkt)) {
            updateBlockData(blk, pkt, true);
        }
        // Always mark the line as dirty (and thus transition to the
        // Modified state) even if we are a failed StoreCond so we
        // supply data to any snoops that have appended themselves to
        // this cache before knowing the store will fail.
        blk->setCoherenceBits(CacheBlk::DirtyBit);
        DPRINTF(CacheVerbose, "%s for %s (write)\n", __func__, pkt->print());
    } else if (pkt->isRead()) {
        if (pkt->isLLSC()) {
            blk->trackLoadLocked(pkt);
        }

        // all read responses have a data payload
        assert(pkt->hasRespData());
        pkt->setDataFromBlock(blk->data, blkSize);
    } else if (pkt->isUpgrade()) {
        // sanity check
        assert(!pkt->hasSharers());

        if (blk->isSet(CacheBlk::DirtyBit)) {
            // we were in the Owned state, and a cache above us that
            // has the line in Shared state needs to be made aware
            // that the data it already has is in fact dirty
            pkt->setCacheResponding();
            blk->clearCoherenceBits(CacheBlk::DirtyBit);
        }
    } else if (pkt->isClean()) {
        blk->clearCoherenceBits(CacheBlk::DirtyBit);
    } else {
        assert(pkt->isInvalidate());
        invalidateBlock(blk);
        DPRINTF(CacheVerbose, "%s for %s (invalidation)\n", __func__,
                pkt->print());
    }
}

/////////////////////////////////////////////////////
//
// Access path: requests coming in from the CPU side
//
/////////////////////////////////////////////////////
Cycles
BaseCache::calculateTagOnlyLatency(const uint32_t delay,
                                const Cycles lookup_lat) const
{
    // A tag-only access has to wait for the packet to arrive in order to
    // perform the tag lookup.
    return ticksToCycles(delay) + lookup_lat;
}

Cycles
BaseCache::calculateAccessLatency(const CacheBlk* blk, const uint32_t delay,
                                const Cycles lookup_lat) const
{
    Cycles lat(0);

    if (blk != nullptr) {
        // As soon as the access arrives, for sequential accesses first access
        // tags, then the data entry. In the case of parallel accesses the
        // latency is dictated by the slowest of tag and data latencies.
        if (sequentialAccess) {
            lat = ticksToCycles(delay) + lookup_lat + dataLatency;
        } else {
            lat = ticksToCycles(delay) + std::max(lookup_lat, dataLatency);
        }

        // Check if the block to be accessed is available. If not, apply the
        // access latency on top of when the block is ready to be accessed.
        const Tick tick = curTick() + delay;
        const Tick when_ready = blk->getWhenReady();
        if (when_ready > tick &&
            ticksToCycles(when_ready - tick) > lat) {
            lat += ticksToCycles(when_ready - tick);
        }
    } else {
        // In case of a miss, we neglect the data access in a parallel
        // configuration (i.e., the data access will be stopped as soon as
        // we find out it is a miss), and use the tag-only latency.
        lat = calculateTagOnlyLatency(delay, lookup_lat);
    }

    return lat;
}

bool
BaseCache::access(PacketPtr pkt, CacheBlk *&blk, Cycles &lat,
                PacketList &writebacks)
{
    // sanity check
    assert(pkt->isRequest());

    gem5_assert(!(isReadOnly && pkt->isWrite()),
                "Should never see a write in a read-only cache %s\n",
                name());

    // Access block in the tags
    Cycles tag_latency(0);
    blk = tags->accessBlock(pkt, tag_latency);

    DPRINTF(Cache, "%s for %s %s\n", __func__, pkt->print(),
            blk ? "hit " + blk->print() : "miss");

    if (pkt->req->isCacheMaintenance()) {
        // A cache maintenance operation is always forwarded to the
        // memory below even if the block is found in dirty state.

        // We defer any changes to the state of the block until we
        // create and mark as in service the mshr for the downstream
        // packet.

        // Calculate access latency on top of when the packet arrives. This
        // takes into account the bus delay.
        lat = calculateTagOnlyLatency(pkt->headerDelay, tag_latency);

        return false;
    }

    if (pkt->isEviction()) {
        // We check for presence of block in above caches before issuing
        // Writeback or CleanEvict to write buffer. Therefore the only
        // possible cases can be of a CleanEvict packet coming from above
        // encountering a Writeback generated in this cache peer cache and
        // waiting in the write buffer. Cases of upper level peer caches
        // generating CleanEvict and Writeback or simply CleanEvict and
        // CleanEvict almost simultaneously will be caught by snoops sent out
        // by crossbar.
        WriteQueueEntry *wb_entry = writeBuffer.findMatch(pkt->getAddr(),
                                                        pkt->isSecure());
        if (wb_entry) {
            assert(wb_entry->getNumTargets() == 1);
            PacketPtr wbPkt = wb_entry->getTarget()->pkt;
            assert(wbPkt->isWriteback());

            if (pkt->isCleanEviction()) {
                // The CleanEvict and WritebackClean snoops into other
                // peer caches of the same level while traversing the
                // crossbar. If a copy of the block is found, the
                // packet is deleted in the crossbar. Hence, none of
                // the other upper level caches connected to this
                // cache have the block, so we can clear the
                // BLOCK_CACHED flag in the Writeback if set and
                // discard the CleanEvict by returning true.
                wbPkt->clearBlockCached();

                // A clean evict does not need to access the data array
                lat = calculateTagOnlyLatency(pkt->headerDelay, tag_latency);

                return true;
            } else {
                assert(pkt->cmd == MemCmd::WritebackDirty);
                // Dirty writeback from above trumps our clean
                // writeback... discard here
                // Note: markInService will remove entry from writeback buffer.
                markInService(wb_entry);
                delete wbPkt;
            }
        }
    }

    // The critical latency part of a write depends only on the tag access
    if (pkt->isWrite()) {
        lat = calculateTagOnlyLatency(pkt->headerDelay, tag_latency);
    }

    // Writeback handling is special case.  We can write the block into
    // the cache without having a writeable copy (or any copy at all).
    if (pkt->isWriteback()) {
        assert(blkSize == pkt->getSize());

        // we could get a clean writeback while we are having
        // outstanding accesses to a block, do the simple thing for
        // now and drop the clean writeback so that we do not upset
        // any ordering/decisions about ownership already taken
        if (pkt->cmd == MemCmd::WritebackClean &&
            mshrQueue.findMatch(pkt->getAddr(), pkt->isSecure())) {
            DPRINTF(Cache, "Clean writeback %#llx to block with MSHR, "
                    "dropping\n", pkt->getAddr());

            // A writeback searches for the block, then writes the data.
            // As the writeback is being dropped, the data is not touched,
            // and we just had to wait for the time to find a match in the
            // MSHR. As of now assume a mshr queue search takes as long as
            // a tag lookup for simplicity.
            return true;
        }

        const bool has_old_data = blk && blk->isValid();
        if (!blk) {
            // need to do a replacement
            blk = allocateBlock(pkt, writebacks);
            if (!blk) {
                // no replaceable block available: give up, fwd to next level.
                incMissCount(pkt);
                return false;
            }

            blk->setCoherenceBits(CacheBlk::ReadableBit);
        } else if (compressor) {
            // This is an overwrite to an existing block, therefore we need
            // to check for data expansion (i.e., block was compressed with
            // a smaller size, and now it doesn't fit the entry anymore).
            // If that is the case we might need to evict blocks.
            if (!updateCompressionData(blk, pkt->getConstPtr<uint64_t>(),
                writebacks)) {
                invalidateBlock(blk);
                return false;
            }
        }

        // only mark the block dirty if we got a writeback command,
        // and leave it as is for a clean writeback
        if (pkt->cmd == MemCmd::WritebackDirty) {
            // TODO: the coherent cache can assert that the dirty bit is set
            blk->setCoherenceBits(CacheBlk::DirtyBit);
        }
        // if the packet does not have sharers, it is passing
        // writable, and we got the writeback in Modified or Exclusive
        // state, if not we are in the Owned or Shared state
        if (!pkt->hasSharers()) {
            blk->setCoherenceBits(CacheBlk::WritableBit);
        }
        // nothing else to do; writeback doesn't expect response
        assert(!pkt->needsResponse());

        updateBlockData(blk, pkt, has_old_data);
        DPRINTF(Cache, "%s new state is %s\n", __func__, blk->print());
        incHitCount(pkt);

        // When the packet metadata arrives, the tag lookup will be done while
        // the payload is arriving. Then the block will be ready to access as
        // soon as the fill is done
        blk->setWhenReady(clockEdge(fillLatency) + pkt->headerDelay +
            std::max(cyclesToTicks(tag_latency), (uint64_t)pkt->payloadDelay));

        return true;
    } else if (pkt->cmd == MemCmd::CleanEvict) {
        // A CleanEvict does not need to access the data array
        lat = calculateTagOnlyLatency(pkt->headerDelay, tag_latency);

        if (blk) {
            // Found the block in the tags, need to stop CleanEvict from
            // propagating further down the hierarchy. Returning true will
            // treat the CleanEvict like a satisfied write request and delete
            // it.
            return true;
        }
        // We didn't find the block here, propagate the CleanEvict further
        // down the memory hierarchy. Returning false will treat the CleanEvict
        // like a Writeback which could not find a replaceable block so has to
        // go to next level.
        return false;
    } else if (pkt->cmd == MemCmd::WriteClean) {
        // WriteClean handling is a special case. We can allocate a
        // block directly if it doesn't exist and we can update the
        // block immediately. The WriteClean transfers the ownership
        // of the block as well.
        assert(blkSize == pkt->getSize());

        const bool has_old_data = blk && blk->isValid();
        if (!blk) {
            if (pkt->writeThrough()) {
                // if this is a write through packet, we don't try to
                // allocate if the block is not present
                return false;
            } else {
                // a writeback that misses needs to allocate a new block
                blk = allocateBlock(pkt, writebacks);
                if (!blk) {
                    // no replaceable block available: give up, fwd to
                    // next level.
                    incMissCount(pkt);
                    return false;
                }

                blk->setCoherenceBits(CacheBlk::ReadableBit);
            }
        } else if (compressor) {
            // This is an overwrite to an existing block, therefore we need
            // to check for data expansion (i.e., block was compressed with
            // a smaller size, and now it doesn't fit the entry anymore).
            // If that is the case we might need to evict blocks.
            if (!updateCompressionData(blk, pkt->getConstPtr<uint64_t>(),
                writebacks)) {
                invalidateBlock(blk);
                return false;
            }
        }

        // at this point either this is a writeback or a write-through
        // write clean operation and the block is already in this
        // cache, we need to update the data and the block flags
        assert(blk);
        // TODO: the coherent cache can assert that the dirty bit is set
        if (!pkt->writeThrough()) {
            blk->setCoherenceBits(CacheBlk::DirtyBit);
        }
        // nothing else to do; writeback doesn't expect response
        assert(!pkt->needsResponse());

        updateBlockData(blk, pkt, has_old_data);
        DPRINTF(Cache, "%s new state is %s\n", __func__, blk->print());

        incHitCount(pkt);

        // When the packet metadata arrives, the tag lookup will be done while
        // the payload is arriving. Then the block will be ready to access as
        // soon as the fill is done
        blk->setWhenReady(clockEdge(fillLatency) + pkt->headerDelay +
            std::max(cyclesToTicks(tag_latency), (uint64_t)pkt->payloadDelay));

        // If this a write-through packet it will be sent to cache below
        return !pkt->writeThrough();
    } else if (blk && (pkt->needsWritable() ?
            blk->isSet(CacheBlk::WritableBit) :
            blk->isSet(CacheBlk::ReadableBit))) {
        // OK to satisfy access
        incHitCount(pkt);

        // Calculate access latency based on the need to access the data array
        if (pkt->isRead()) {
            lat = calculateAccessLatency(blk, pkt->headerDelay, tag_latency);

            // When a block is compressed, it must first be decompressed
            // before being read. This adds to the access latency.
            if (compressor) {
                lat += compressor->getDecompressionLatency(blk);
            }
        } else {
            lat = calculateTagOnlyLatency(pkt->headerDelay, tag_latency);
        }

        satisfyRequest(pkt, blk);
        maintainClusivity(pkt->fromCache(), blk);

        return true;
    }

    // Can't satisfy access normally... either no block (blk == nullptr)
    // or have block but need writable

    incMissCount(pkt);

    lat = calculateAccessLatency(blk, pkt->headerDelay, tag_latency);

    if (!blk && pkt->isLLSC() && pkt->isWrite()) {
        // complete miss on store conditional... just give up now
        pkt->req->setExtraData(0);
        return true;
    }

    return false;
}

void
BaseCache::maintainClusivity(bool from_cache, CacheBlk *blk)
{
    if (from_cache && blk && blk->isValid() &&
        !blk->isSet(CacheBlk::DirtyBit) && clusivity == enums::mostly_excl) {
        // if we have responded to a cache, and our block is still
        // valid, but not dirty, and this cache is mostly exclusive
        // with respect to the cache above, drop the block
        invalidateBlock(blk);
    }
}

CacheBlk*
BaseCache::handleFill(PacketPtr pkt, CacheBlk *blk, PacketList &writebacks,
                    bool allocate)
{
    assert(pkt->isResponse());
    Addr addr = pkt->getAddr();
    bool is_secure = pkt->isSecure();
    const bool has_old_data = blk && blk->isValid();
    const std::string old_state = (debug::Cache && blk) ? blk->print() : "";

    // When handling a fill, we should have no writes to this line.
    assert(addr == pkt->getBlockAddr(blkSize));
    assert(!writeBuffer.findMatch(addr, is_secure));

    if (!blk) {
        // better have read new data...
        assert(pkt->hasData() || pkt->cmd == MemCmd::InvalidateResp);

        // need to do a replacement if allocating, otherwise we stick
        // with the temporary storage
        blk = allocate ? allocateBlock(pkt, writebacks) : nullptr;

        if (!blk) {
            // No replaceable block or a mostly exclusive
            // cache... just use temporary storage to complete the
            // current request and then get rid of it
            blk = tempBlock;
            tempBlock->insert(addr, is_secure);
            DPRINTF(Cache, "using temp block for %#llx (%s)\n", addr,
                    is_secure ? "s" : "ns");
        }
    } else {
        // existing block... probably an upgrade
        // don't clear block status... if block is already dirty we
        // don't want to lose that
    }

    // Block is guaranteed to be valid at this point
    assert(blk->isValid());
    assert(blk->isSecure() == is_secure);
    assert(regenerateBlkAddr(blk) == addr);

    blk->setCoherenceBits(CacheBlk::ReadableBit);

    // sanity check for whole-line writes, which should always be
    // marked as writable as part of the fill, and then later marked
    // dirty as part of satisfyRequest
    //对整行写入进行合理性检查，整行写入应始终在填充时标记为可写，然后在满足请求时标记为脏。
    if (pkt->cmd == MemCmd::InvalidateResp) {
        assert(!pkt->hasSharers());
    }

    // here we deal with setting the appropriate state of the line,
    // and we start by looking at the hasSharers flag, and ignore the
    // cacheResponding flag (normally signalling dirty data) if the
    // packet has sharers, thus the line is never allocated as Owned
    // (dirty but not writable), and always ends up being either
    // Shared, Exclusive or Modified, see Packet::setCacheResponding
    // for more details
    // 在这里，我们处理设置该行的适当状态，首先查看 `hasSharers` 标志。
    // 如果数据包有共享者，则忽略 `cacheResponding` 标志（通常表示脏数据），
    // 因此该行永远不会被分配为“已拥有”（脏但不可写），
    // 最终状态总是“共享”、“独占”或“已修改”。
    // 详细信息请参见 `Packet::setCacheResponding`。
    if (!pkt->hasSharers()) {
        // we could get a writable line from memory (rather than a
        // cache) even in a read-only cache, note that we set this bit
        // even for a read-only cache, possibly revisit this decision
        // 即使在只读缓存中，我们也可能从内存中获得一条可写行。
        // 注意，即使对于只读缓存，我们也设置了这个标志，
        // 这个决定可能需要重新考虑。

        blk->setCoherenceBits(CacheBlk::WritableBit);

        // check if we got this via cache-to-cache transfer (i.e., from a
        // cache that had the block in Modified or Owned state)
        // 检查我们是否通过缓存到缓存的传输获得了这个块
        // （即从一个处于已修改或已拥有状态的缓存中获得的块）。

        if (pkt->cacheResponding()) {
            // we got the block in Modified state, and invalidated the
            // owners copy
            // 我们获取了处于已修改状态的块，并使所有者的副本无效。
            blk->setCoherenceBits(CacheBlk::DirtyBit);

            gem5_assert(!isReadOnly, "Should never see dirty snoop response "
                        "in read-only cache %s\n", name());

        }
    }

    DPRINTF(Cache, "Block addr %#llx (%s) moving from %s to %s\n",
            addr, is_secure ? "s" : "ns", old_state, blk->print());

    // if we got new data, copy it in (checking for a read response
    // and a response that has data is the same in the end)
    // 如果我们得到了新数据，将其复制进去
    // （检查读取响应和有数据的响应最终是一样的）。

    if (pkt->isRead()) {
        // sanity checks
        assert(pkt->hasData());
        assert(pkt->getSize() == blkSize);

        updateBlockData(blk, pkt, has_old_data);
        // DPRINTF(HWPrefetch, "ppFill base: PC %llx, PAddr %llx\n", 
        //             pkt->req->hasPC()? pkt->req->getPC(): 0, pkt->req->hasPaddr()? pkt->req->getPaddr(): 0);
        // ppFill->notify(pkt);//xymc
    }
    // ppFill->notify(pkt);//xymc
    // The block will be ready when the payload arrives and the fill is done
    // 当有效载荷到达并且填充完成时，该块将准备就绪。
    blk->setWhenReady(clockEdge(fillLatency) + pkt->headerDelay +
                        pkt->payloadDelay);

    return blk;
}

CacheBlk*
BaseCache::allocateBlock(const PacketPtr pkt, PacketList &writebacks)
{
    // Get address
    const Addr addr = pkt->getAddr();

    // Get secure bit
    const bool is_secure = pkt->isSecure();

    // Block size and compression related access latency. Only relevant if
    // using a compressor, otherwise there is no extra delay, and the block
    // is fully sized
    std::size_t blk_size_bits = blkSize*8;
    Cycles compression_lat = Cycles(0);
    Cycles decompression_lat = Cycles(0);

    // If a compressor is being used, it is called to compress data before
    // insertion. Although in Gem5 the data is stored uncompressed, even if a
    // compressor is used, the compression/decompression methods are called to
    // calculate the amount of extra cycles needed to read or write compressed
    // blocks.
    if (compressor && pkt->hasData()) {
        const auto comp_data = compressor->compress(
            pkt->getConstPtr<uint64_t>(), compression_lat, decompression_lat);
        blk_size_bits = comp_data->getSizeBits();
    }

    // Find replacement victim
    std::vector<CacheBlk*> evict_blks;
    CacheBlk *victim = tags->findVictim(addr, is_secure, blk_size_bits,
                                        evict_blks);

    // It is valid to return nullptr if there is no victim
    if (!victim)
        return nullptr;

    // Print victim block's information
    DPRINTF(CacheRepl, "Replacement victim: %s\n", victim->print());

    // Try to evict blocks; if it fails, give up on allocation
    if (!handleEvictions(evict_blks, writebacks)) {
        return nullptr;
    }

    // Insert new block at victimized entry
    tags->insertBlock(pkt, victim);

    // If using a compressor, set compression data. This must be done after
    // insertion, as the compression bit may be set.
    if (compressor) {
        compressor->setSizeBits(victim, blk_size_bits);
        compressor->setDecompressionLatency(victim, decompression_lat);
    }

    return victim;
}

void
BaseCache::invalidateBlock(CacheBlk *blk)
{
    // If block is still marked as prefetched, then it hasn't been used
    if (blk->wasPrefetched()) {
        prefetcher->prefetchUnused(blk->getPC());
    }

    // Notify that the data contents for this address are no longer present
    updateBlockData(blk, nullptr, blk->isValid());

    // If handling a block present in the Tags, let it do its invalidation
    // process, which will update stats and invalidate the block itself
    if (blk != tempBlock) {
        tags->invalidate(blk);
    } else {
        tempBlock->invalidate();
    }
}

void
BaseCache::evictBlock(CacheBlk *blk, PacketList &writebacks)
{
    PacketPtr pkt = evictBlock(blk);
    if (pkt) {
        writebacks.push_back(pkt);
    }
}

PacketPtr
BaseCache::writebackBlk(CacheBlk *blk)
{
    gem5_assert(!isReadOnly || writebackClean,
                "Writeback from read-only cache");
    assert(blk && blk->isValid() &&
        (blk->isSet(CacheBlk::DirtyBit) || writebackClean));

    stats.writebacks[Request::wbRequestorId]++;

    RequestPtr req = std::make_shared<Request>(
        regenerateBlkAddr(blk), blkSize, 0, Request::wbRequestorId);

    if (blk->isSecure())
        req->setFlags(Request::SECURE);

    req->taskId(blk->getTaskId());

    PacketPtr pkt =
        new Packet(req, blk->isSet(CacheBlk::DirtyBit) ?
                   MemCmd::WritebackDirty : MemCmd::WritebackClean);

    DPRINTF(Cache, "Create Writeback %s writable: %d, dirty: %d\n",
        pkt->print(), blk->isSet(CacheBlk::WritableBit),
        blk->isSet(CacheBlk::DirtyBit));

    if (blk->isSet(CacheBlk::WritableBit)) {
        // not asserting shared means we pass the block in modified
        // state, mark our own block non-writeable
        blk->clearCoherenceBits(CacheBlk::WritableBit);
    } else {
        // we are in the Owned state, tell the receiver
        pkt->setHasSharers();
    }

    // make sure the block is not marked dirty
    blk->clearCoherenceBits(CacheBlk::DirtyBit);

    pkt->allocate();
    pkt->setDataFromBlock(blk->data, blkSize);

    // When a block is compressed, it must first be decompressed before being
    // sent for writeback.
    if (compressor) {
        pkt->payloadDelay = compressor->getDecompressionLatency(blk);
    }

    return pkt;
}

PacketPtr
BaseCache::writecleanBlk(CacheBlk *blk, Request::Flags dest, PacketId id)
{
    RequestPtr req = std::make_shared<Request>(
        regenerateBlkAddr(blk), blkSize, 0, Request::wbRequestorId);

    if (blk->isSecure()) {
        req->setFlags(Request::SECURE);
    }
    req->taskId(blk->getTaskId());

    PacketPtr pkt = new Packet(req, MemCmd::WriteClean, blkSize, id);

    if (dest) {
        req->setFlags(dest);
        pkt->setWriteThrough();
    }

    DPRINTF(Cache, "Create %s writable: %d, dirty: %d\n", pkt->print(),
            blk->isSet(CacheBlk::WritableBit), blk->isSet(CacheBlk::DirtyBit));

    if (blk->isSet(CacheBlk::WritableBit)) {
        // not asserting shared means we pass the block in modified
        // state, mark our own block non-writeable
        blk->clearCoherenceBits(CacheBlk::WritableBit);
    } else {
        // we are in the Owned state, tell the receiver
        pkt->setHasSharers();
    }

    // make sure the block is not marked dirty
    blk->clearCoherenceBits(CacheBlk::DirtyBit);

    pkt->allocate();
    pkt->setDataFromBlock(blk->data, blkSize);

    // When a block is compressed, it must first be decompressed before being
    // sent for writeback.
    if (compressor) {
        pkt->payloadDelay = compressor->getDecompressionLatency(blk);
    }

    return pkt;
}


void
BaseCache::memWriteback()
{
    tags->forEachBlk([this](CacheBlk &blk) { writebackVisitor(blk); });
}

void
BaseCache::memInvalidate()
{
    tags->forEachBlk([this](CacheBlk &blk) { invalidateVisitor(blk); });
}

bool
BaseCache::isDirty() const
{
    return tags->anyBlk([](CacheBlk &blk) {
        return blk.isSet(CacheBlk::DirtyBit); });
}

bool
BaseCache::coalesce() const
{
    return writeAllocator && writeAllocator->coalesce();
}

void
BaseCache::writebackVisitor(CacheBlk &blk)
{
    if (blk.isSet(CacheBlk::DirtyBit)) {
        assert(blk.isValid());

        RequestPtr request = std::make_shared<Request>(
            regenerateBlkAddr(&blk), blkSize, 0, Request::funcRequestorId);

        request->taskId(blk.getTaskId());
        if (blk.isSecure()) {
            request->setFlags(Request::SECURE);
        }

        Packet packet(request, MemCmd::WriteReq);
        packet.dataStatic(blk.data);

        memSidePort.sendFunctional(&packet);

        blk.clearCoherenceBits(CacheBlk::DirtyBit);
    }
}

void
BaseCache::invalidateVisitor(CacheBlk &blk)
{
    if (blk.isSet(CacheBlk::DirtyBit))
        warn_once("Invalidating dirty cache lines. " \
                  "Expect things to break.\n");

    if (blk.isValid()) {
        assert(!blk.isSet(CacheBlk::DirtyBit));
        invalidateBlock(&blk);
    }
}

Tick
BaseCache::nextQueueReadyTime() const
{
    Tick nextReady = std::min(mshrQueue.nextReadyTime(),
                            writeBuffer.nextReadyTime());

    // Don't signal prefetch ready time if no MSHRs available
    // Will signal once enoguh MSHRs are deallocated
    if (prefetcher && mshrQueue.canPrefetch() && !isBlocked()) {
        nextReady = std::min(nextReady,
                            prefetcher->nextPrefetchReadyTime());
    }

    DPRINTF(
        RequestSlot, 
        "[Schedule] Next send time: %lld MSHR: %lld WB: %lld PF: %lld\n", 
        nextReady, 
        mshrQueue.nextReadyTime(), 
        writeBuffer.nextReadyTime(), 
        prefetcher ? prefetcher->nextPrefetchReadyTime() : MaxTick
    );

    return nextReady;
}


bool
BaseCache::sendMSHRQueuePacket(MSHR* mshr)
{
    assert(mshr);

    // use request from 1st target
    PacketPtr tgt_pkt = mshr->getTarget()->pkt;

    DPRINTF(Cache, "%s: MSHR %s\n", __func__, tgt_pkt->print());

    // if the cache is in write coalescing mode or (additionally) in
    // no allocation mode, and we have a write packet with an MSHR
    // that is not a whole-line write (due to incompatible flags etc),
    // then reset the write mode
    if (writeAllocator && writeAllocator->coalesce() && tgt_pkt->isWrite()) {
        if (!mshr->isWholeLineWrite()) {
            // if we are currently write coalescing, hold on the
            // MSHR as many cycles extra as we need to completely
            // write a cache line
            if (writeAllocator->delay(mshr->blkAddr)) {
                Tick delay = blkSize / tgt_pkt->getSize() * clockPeriod();
                DPRINTF(CacheVerbose, "Delaying pkt %s %llu ticks to allow "
                        "for write coalescing\n", tgt_pkt->print(), delay);
                mshrQueue.delay(mshr, delay);
                return false;
            } else {
                writeAllocator->reset();
            }
        } else {
            writeAllocator->resetDelay(mshr->blkAddr);
        }
    }

    CacheBlk *blk = tags->findBlock(mshr->blkAddr, mshr->isSecure);

    // either a prefetch that is not present upstream, or a normal
    // MSHR request, proceed to get the packet to send downstream
    PacketPtr pkt = createMissPacket(tgt_pkt, blk, mshr->needsWritable(),
                                     mshr->isWholeLineWrite());

    mshr->isForward = (pkt == nullptr);

    if (mshr->isForward) {
        // not a cache block request, but a response is expected
        // make copy of current packet to forward, keep current
        // copy for response handling
        pkt = new Packet(tgt_pkt, false, true);
        assert(!pkt->isWrite());
    }

    // play it safe and append (rather than set) the sender state,
    // as forwarded packets may already have existing state
    pkt->pushSenderState(mshr);

    if (pkt->isClean() && blk && blk->isSet(CacheBlk::DirtyBit)) {
        // A cache clean opearation is looking for a dirty block. Mark
        // the packet so that the destination xbar can determine that
        // there will be a follow-up write packet as well.
        pkt->setSatisfied();
    }

    if (!memSidePort.sendTimingReq(pkt)) {
        // we are awaiting a retry, but we
        // delete the packet and will be creating a new packet
        // when we get the opportunity
        delete pkt;

        // note that we have now masked any requestBus and
        // schedSendEvent (we will wait for a retry before
        // doing anything), and this is so even if we do not
        // care about this packet and might override it before
        // it gets retried
        return true;
    } else {
        // As part of the call to sendTimingReq the packet is
        // forwarded to all neighbouring caches (and any caches
        // above them) as a snoop. Thus at this point we know if
        // any of the neighbouring caches are responding, and if
        // so, we know it is dirty, and we can determine if it is
        // being passed as Modified, making our MSHR the ordering
        // point
        bool pending_modified_resp = !pkt->hasSharers() &&
            pkt->cacheResponding();
        markInService(mshr, pending_modified_resp);

        if (pkt->isClean() && blk && blk->isSet(CacheBlk::DirtyBit)) {
            // A cache clean opearation is looking for a dirty
            // block. If a dirty block is encountered a WriteClean
            // will update any copies to the path to the memory
            // until the point of reference.
            DPRINTF(CacheVerbose, "%s: packet %s found block: %s\n",
                    __func__, pkt->print(), blk->print());
            PacketPtr wb_pkt = writecleanBlk(blk, pkt->req->getDest(),
                                             pkt->id);
            PacketList writebacks;
            writebacks.push_back(wb_pkt);
            doWritebacks(writebacks, 0);
        }

        return false;
    }
}

bool
BaseCache::sendWriteQueuePacket(WriteQueueEntry* wq_entry)
{
    assert(wq_entry);

    // always a single target for write queue entries
    PacketPtr tgt_pkt = wq_entry->getTarget()->pkt;

    DPRINTF(Cache, "%s: write %s\n", __func__, tgt_pkt->print());

    // forward as is, both for evictions and uncacheable writes
    if (!memSidePort.sendTimingReq(tgt_pkt)) {
        // note that we have now masked any requestBus and
        // schedSendEvent (we will wait for a retry before
        // doing anything), and this is so even if we do not
        // care about this packet and might override it before
        // it gets retried
        return true;
    } else {
        markInService(wq_entry);
        return false;
    }
}

void
BaseCache::serialize(CheckpointOut &cp) const
{
    bool dirty(isDirty());

    if (dirty) {
        warn("*** The cache still contains dirty data. ***\n");
        warn("    Make sure to drain the system using the correct flags.\n");
        warn("    This checkpoint will not restore correctly " \
             "and dirty data in the cache will be lost!\n");
    }

    // Since we don't checkpoint the data in the cache, any dirty data
    // will be lost when restoring from a checkpoint of a system that
    // wasn't drained properly. Flag the checkpoint as invalid if the
    // cache contains dirty data.
    bool bad_checkpoint(dirty);
    SERIALIZE_SCALAR(bad_checkpoint);
}

void
BaseCache::unserialize(CheckpointIn &cp)
{
    bool bad_checkpoint;
    UNSERIALIZE_SCALAR(bad_checkpoint);
    if (bad_checkpoint) {
        fatal("Restoring from checkpoints with dirty caches is not "
              "supported in the classic memory system. Please remove any "
              "caches or drain them properly before taking checkpoints.\n");
    }
}


BaseCache::CacheCmdStats::CacheCmdStats(BaseCache &c,
                                        const std::string &name)
    : statistics::Group(&c, name.c_str()), cache(c),
      ADD_STAT(hits, statistics::units::Count::get(),
               ("number of " + name + " hits").c_str()),
      ADD_STAT(hitsPerPC, statistics::units::Count::get(),
               ("number of " + name + " hits").c_str()),
      ADD_STAT(hitsAtPf, statistics::units::Count::get(),
               ("number of " + name + " hits").c_str()),
      ADD_STAT(hitsAtPfPerPC, statistics::units::Count::get(),
               ("number of " + name + " hits").c_str()),
      ADD_STAT(hitsAtPfAlloc, statistics::units::Count::get(),
               ("number of " + name + " hits").c_str()),
      ADD_STAT(hitsAtPfAllocPerPC, statistics::units::Count::get(),
               ("number of " + name + " hits").c_str()),
      ADD_STAT(misses, statistics::units::Count::get(),
               ("number of " + name + " misses").c_str()),
      ADD_STAT(missesPerPC, statistics::units::Count::get(),
               ("number of " + name + " misses").c_str()),
      ADD_STAT(hitLatency, statistics::units::Tick::get(),
               ("number of " + name + " hit ticks").c_str()),
      ADD_STAT(missLatency, statistics::units::Tick::get(),
               ("number of " + name + " miss ticks").c_str()),
      ADD_STAT(accesses, statistics::units::Count::get(),
               ("number of " + name + " accesses(hits+misses)").c_str()),
      ADD_STAT(accessesPerPC, statistics::units::Count::get(),
               ("number of " + name + " accesses(hits+misses)").c_str()),
      ADD_STAT(missRate, statistics::units::Ratio::get(),
               ("miss rate for " + name + " accesses").c_str()),
      ADD_STAT(avgMissLatency, statistics::units::Rate<
                    statistics::units::Tick, statistics::units::Count>::get(),
               ("average " + name + " miss latency").c_str()),
      ADD_STAT(mshrHits, statistics::units::Count::get(),
               ("number of " + name + " MSHR hits").c_str()),
      ADD_STAT(mshrHitsPerPC, statistics::units::Count::get(),
               ("number of " + name + " MSHR hits").c_str()),
      ADD_STAT(mshrHitsAtPf, statistics::units::Count::get(),
               ("number of " + name + " MSHR hits at pfMSHR").c_str()),
      ADD_STAT(mshrHitsAtPfPerPC, statistics::units::Count::get(),
               ("number of " + name + " MSHR hits at pfMSHR").c_str()),
      ADD_STAT(mshrMisses, statistics::units::Count::get(),
               ("number of " + name + " MSHR misses").c_str()),
      ADD_STAT(mshrMissesPerPC, statistics::units::Count::get(),
               ("number of " + name + " MSHR misses").c_str()),
      ADD_STAT(mshrUncacheable, statistics::units::Count::get(),
               ("number of " + name + " MSHR uncacheable").c_str()),
      ADD_STAT(mshrUncacheablePerPC, statistics::units::Count::get(),
               ("number of " + name + " MSHR uncacheable").c_str()),
      ADD_STAT(mshrMissLatency, statistics::units::Tick::get(),
               ("number of " + name + " MSHR miss ticks").c_str()),
      ADD_STAT(mshrUncacheableLatency, statistics::units::Tick::get(),
               ("number of " + name + " MSHR uncacheable ticks").c_str()),
      ADD_STAT(mshrMissRate, statistics::units::Ratio::get(),
               ("mshr miss rate for " + name + " accesses").c_str()),
      ADD_STAT(avgMshrMissLatency, statistics::units::Rate<
                    statistics::units::Tick, statistics::units::Count>::get(),
               ("average " + name + " mshr miss latency").c_str()),
      ADD_STAT(avgMshrUncacheableLatency, statistics::units::Rate<
                    statistics::units::Tick, statistics::units::Count>::get(),
               ("average " + name + " mshr uncacheable latency").c_str())
{
}

void
BaseCache::CacheCmdStats::regStatsFromParent()
{
    using namespace statistics;

    statistics::Group::regStats();
    System *system = cache.system;
    const auto max_requestors = system->maxRequestors();
      
    const int max_per_pc = 32;
    std::vector<Addr> stats_pc_list = cache.stats_pc_list;

    assert(stats_pc_list.size() < max_per_pc);

    hits
        .init(max_requestors)
        .flags(total | nozero | nonan)
        ;
    for (int i = 0; i < max_requestors; i++) {
        hits.subname(i, system->getRequestorName(i));
    }

    hitsAtPf
        .init(max_requestors)
        .flags(total | nozero | nonan)
        ;
    for (int i = 0; i < max_requestors; i++) {
        hitsAtPf.subname(i, system->getRequestorName(i));
    }

    hitsAtPfAlloc
        .init(max_requestors)
        .flags(total | nozero | nonan)
        ;
    for (int i = 0; i < max_requestors; i++) {
        hitsAtPfAlloc.subname(i, system->getRequestorName(i));
    }

    // Miss statistics
    misses
        .init(max_requestors)
        .flags(total | nozero | nonan)
        ;
    for (int i = 0; i < max_requestors; i++) {
        misses.subname(i, system->getRequestorName(i));
    }

    // Hit latency statistics
    hitLatency
        .init(max_requestors)
        .flags(total | nozero | nonan)
        ;
    for (int i = 0; i < max_requestors; i++) {
        hitLatency.subname(i, system->getRequestorName(i));
    }

    // Miss latency statistics
    missLatency
        .init(max_requestors)
        .flags(total | nozero | nonan)
        ;
    for (int i = 0; i < max_requestors; i++) {
        missLatency.subname(i, system->getRequestorName(i));
    }

    // access formulas
    accesses.flags(total | nozero | nonan);
    accesses = hits + misses;
    for (int i = 0; i < max_requestors; i++) {
        accesses.subname(i, system->getRequestorName(i));
    }

    // miss rate formulas
    missRate.flags(total | nozero | nonan);
    missRate = misses / accesses;
    for (int i = 0; i < max_requestors; i++) {
        missRate.subname(i, system->getRequestorName(i));
    }

    // miss latency formulas
    avgMissLatency.flags(total | nozero | nonan);
    avgMissLatency = missLatency / misses;
    for (int i = 0; i < max_requestors; i++) {
        avgMissLatency.subname(i, system->getRequestorName(i));
    }

    // MSHR statistics
    // MSHR hit statistics
    mshrHits
        .init(max_requestors)
        .flags(total | nozero | nonan)
        ;
    for (int i = 0; i < max_requestors; i++) {
        mshrHits.subname(i, system->getRequestorName(i));
    }

    mshrHitsAtPf
        .init(max_requestors)
        .flags(total | nozero | nonan)
        ;
    for (int i = 0; i < max_requestors; i++) {
        mshrHits.subname(i, system->getRequestorName(i));
    }

    // MSHR miss statistics
    mshrMisses
        .init(max_requestors)
        .flags(total | nozero | nonan)
        ;
    for (int i = 0; i < max_requestors; i++) {
        mshrMisses.subname(i, system->getRequestorName(i));
    }

    // MSHR miss latency statistics
    mshrMissLatency
        .init(max_requestors)
        .flags(total | nozero | nonan)
        ;
    for (int i = 0; i < max_requestors; i++) {
        mshrMissLatency.subname(i, system->getRequestorName(i));
    }

    // MSHR uncacheable statistics
    mshrUncacheable
        .init(max_requestors)
        .flags(total | nozero | nonan)
        ;
    for (int i = 0; i < max_requestors; i++) {
        mshrUncacheable.subname(i, system->getRequestorName(i));
    }

    // MSHR miss latency statistics
    mshrUncacheableLatency
        .init(max_requestors)
        .flags(total | nozero | nonan)
        ;
    for (int i = 0; i < max_requestors; i++) {
        mshrUncacheableLatency.subname(i, system->getRequestorName(i));
    }

    // MSHR miss rate formulas
    mshrMissRate.flags(total | nozero | nonan);
    mshrMissRate = mshrMisses / accesses;

    for (int i = 0; i < max_requestors; i++) {
        mshrMissRate.subname(i, system->getRequestorName(i));
    }

    // mshrMiss latency formulas
    avgMshrMissLatency.flags(total | nozero | nonan);
    avgMshrMissLatency = mshrMissLatency / mshrMisses;
    for (int i = 0; i < max_requestors; i++) {
        avgMshrMissLatency.subname(i, system->getRequestorName(i));
    }

    // mshrUncacheable latency formulas
    avgMshrUncacheableLatency.flags(total | nozero | nonan);
    avgMshrUncacheableLatency = mshrUncacheableLatency / mshrUncacheable;
    for (int i = 0; i < max_requestors; i++) {
        avgMshrUncacheableLatency.subname(i, system->getRequestorName(i));
    }

    // PerPC stats

    hitsPerPC.init(max_per_pc).flags(nozero | nonan);
    hitsAtPfPerPC.init(max_per_pc).flags(nozero | nonan);
    hitsAtPfAllocPerPC.init(max_per_pc).flags(nozero | nonan);
    missesPerPC.init(max_per_pc).flags(nozero | nonan);
    mshrHitsPerPC.init(max_per_pc).flags(nozero | nonan);
    mshrHitsAtPfPerPC.init(max_per_pc).flags(nozero | nonan);
    mshrMissesPerPC.init(max_per_pc).flags(nozero | nonan);
    mshrUncacheablePerPC.init(max_per_pc).flags(nozero | nonan);

    accessesPerPC.flags(nozero | nonan);
    accessesPerPC = hitsPerPC + missesPerPC;

    for (int i = 0; i < stats_pc_list.size(); i++) {
        std::stringstream stream;
        stream << std::hex << stats_pc_list[i];
        std::string pc_hex = stream.str();

        hitsPerPC.subname(i, pc_hex);
        hitsAtPfPerPC.subname(i, pc_hex);
        hitsAtPfAllocPerPC.subname(i, pc_hex);
        missesPerPC.subname(i, pc_hex);
        accessesPerPC.subname(i, pc_hex);
        mshrHitsPerPC.subname(i, pc_hex);
        mshrHitsAtPfPerPC.subname(i, pc_hex);
        mshrMissesPerPC.subname(i, pc_hex);
        mshrUncacheablePerPC.subname(i, pc_hex);
    }

}

BaseCache::CacheStats::CacheStats(BaseCache &c)
    : statistics::Group(&c), cache(c),

    ADD_STAT(demandHits, statistics::units::Count::get(),
             "number of demand (read+write) hits"),
    ADD_STAT(demandHitsPerPC, statistics::units::Count::get(),
             "number of demand (read+write) hits"),
    ADD_STAT(demandHitsAtPf, statistics::units::Count::get(),
             "number of demand (read+write) hits"),
    ADD_STAT(demandHitsAtPfPerPC, statistics::units::Count::get(),
             "number of demand (read+write) hits"),
    ADD_STAT(demandHitsAtPfAlloc, statistics::units::Count::get(),
             "number of demand (read+write) hits"),
    ADD_STAT(demandHitsAtPfAllocPerPC, statistics::units::Count::get(),
             "number of demand (read+write) hits"),
    ADD_STAT(hitsAtPfCoverAccess, statistics::units::Count::get(),
             "hits at prefetch / demand accesses"),
    ADD_STAT(hitsAtPfCoverAccessPerPC, statistics::units::Count::get(),
             "hits at prefetch / demand accesses"),
    ADD_STAT(hitsAtPfAllocCoverAccess, statistics::units::Count::get(),
             "hits at prefetch alloc / demand accesses"),
    ADD_STAT(hitsAtPfAllocCoverAccessPerPC, statistics::units::Count::get(),
             "hits at prefetch alloc / demand accesses"),
    ADD_STAT(hitsPfRatio, statistics::units::Count::get(),
             "hits at prefetch alloc / hits at prefetch"),
    ADD_STAT(hitsPfRatioPerPC, statistics::units::Count::get(),
             "hits at prefetch alloc / hits at prefetch"),
    ADD_STAT(overallHits, statistics::units::Count::get(),
             "number of overall hits"),
    ADD_STAT(demandHitLatency, statistics::units::Tick::get(),
             "number of demand (read+write) hit ticks"),
    ADD_STAT(overallHitLatency, statistics::units::Tick::get(),
            "number of overall hit ticks"),
    ADD_STAT(demandMisses, statistics::units::Count::get(),
             "number of demand (read+write) misses"),
    ADD_STAT(demandMissesPerPC, statistics::units::Count::get(),
             "number of demand (read+write) misses"),
    ADD_STAT(overallMisses, statistics::units::Count::get(),
             "number of overall misses"),
    ADD_STAT(demandMissLatency, statistics::units::Tick::get(),
             "number of demand (read+write) miss ticks"),
    ADD_STAT(overallMissLatency, statistics::units::Tick::get(),
             "number of overall miss ticks"),
    ADD_STAT(demandAccesses, statistics::units::Count::get(),
             "number of demand (read+write) accesses"),
    ADD_STAT(demandAccessesPerPC, statistics::units::Count::get(),
             "number of demand (read+write) accesses"),
    ADD_STAT(overallAccesses, statistics::units::Count::get(),
             "number of overall (read+write) accesses"),
    ADD_STAT(demandMissRate, statistics::units::Ratio::get(),
             "miss rate for demand accesses"),
    ADD_STAT(demandMissRatePerPC, statistics::units::Ratio::get(),
             "miss rate for demand accesses"),
    ADD_STAT(overallMissRate, statistics::units::Ratio::get(),
             "miss rate for overall accesses"),
    ADD_STAT(demandAvgMissLatency, statistics::units::Rate<
                statistics::units::Tick, statistics::units::Count>::get(),
             "average overall miss latency in ticks"),
    ADD_STAT(overallAvgMissLatency, statistics::units::Rate<
                statistics::units::Tick, statistics::units::Count>::get(),
             "average overall miss latency"),
    ADD_STAT(blockedCycles, statistics::units::Cycle::get(),
            "number of cycles access was blocked"),
    ADD_STAT(blockedCauses, statistics::units::Count::get(),
            "number of times access was blocked"),
    ADD_STAT(avgBlocked, statistics::units::Rate<
                statistics::units::Cycle, statistics::units::Count>::get(),
             "average number of cycles each access was blocked"),
    ADD_STAT(writebacks, statistics::units::Count::get(),
             "number of writebacks"),
    ADD_STAT(demandMshrHits, statistics::units::Count::get(),
             "number of demand (read+write) MSHR hits"),
    ADD_STAT(demandMshrHitsPerPC, statistics::units::Count::get(),
             "number of demand (read+write) MSHR hits"),
    ADD_STAT(overallMshrHits, statistics::units::Count::get(),
             "number of overall MSHR hits"),
    ADD_STAT(demandMshrHitsAtPf, statistics::units::Count::get(),
             "number of demand (read+write) MSHR hits"),
    ADD_STAT(demandMshrHitsAtPfPerPC, statistics::units::Count::get(),
             "number of demand (read+write) MSHR hits"),
    ADD_STAT(demandMshrMisses, statistics::units::Count::get(),
             "number of demand (read+write) MSHR misses"),
    ADD_STAT(demandMshrMissesPerPC, statistics::units::Count::get(),
             "number of demand (read+write) MSHR misses"),
    ADD_STAT(overallMshrMisses, statistics::units::Count::get(),
            "number of overall MSHR misses"),
    ADD_STAT(overallMshrUncacheable, statistics::units::Count::get(),
             "number of overall MSHR uncacheable misses"),
    ADD_STAT(demandMshrMissLatency, statistics::units::Tick::get(),
             "number of demand (read+write) MSHR miss ticks"),
    ADD_STAT(overallMshrMissLatency, statistics::units::Tick::get(),
             "number of overall MSHR miss ticks"),
    ADD_STAT(overallMshrUncacheableLatency, statistics::units::Tick::get(),
             "number of overall MSHR uncacheable ticks"),
    ADD_STAT(demandMshrMissRate, statistics::units::Ratio::get(),
             "mshr miss ratio for demand accesses"),
    ADD_STAT(overallMshrMissRate, statistics::units::Ratio::get(),
             "mshr miss ratio for overall accesses"),
    ADD_STAT(demandAvgMshrMissLatency, statistics::units::Rate<
                statistics::units::Tick, statistics::units::Count>::get(),
             "average overall mshr miss latency"),
    ADD_STAT(overallAvgMshrMissLatency, statistics::units::Rate<
                statistics::units::Tick, statistics::units::Count>::get(),
             "average overall mshr miss latency"),
    ADD_STAT(overallAvgMshrUncacheableLatency, statistics::units::Rate<
                statistics::units::Tick, statistics::units::Count>::get(),
             "average overall mshr uncacheable latency"),
    ADD_STAT(replacements, statistics::units::Count::get(),
             "number of replacements"),
    ADD_STAT(prefetchFills, statistics::units::Count::get(),
             "number of prefetch fills"),
    ADD_STAT(prefetchHits, statistics::units::Count::get(),
             "number of prefetch hits"),
    ADD_STAT(dataExpansions, statistics::units::Count::get(),
             "number of data expansions"),
    ADD_STAT(dataContractions, statistics::units::Count::get(),
             "number of data contractions"),
    ADD_STAT(overallmshrcycles, statistics::units::Count::get(),
             "number of cycles when mshr is not empty"),
    ADD_STAT(overallmshrservices, statistics::units::Count::get(),
             "number of services when mshr is not empty"),
    cmd(MemCmd::NUM_MEM_CMDS)
{
    for (int idx = 0; idx < MemCmd::NUM_MEM_CMDS; ++idx)
        cmd[idx].reset(new CacheCmdStats(c, MemCmd(idx).toString()));
}

void
BaseCache::CacheStats::regStats()
{
    using namespace statistics;

    statistics::Group::regStats();

    System *system = cache.system;
    const auto max_requestors = system->maxRequestors();

    const int max_per_pc = 32;
    std::vector<Addr> stats_pc_list = cache.stats_pc_list;

    assert(stats_pc_list.size() < max_per_pc);

    for (auto &cs : cmd)
        cs->regStatsFromParent();

// These macros make it easier to sum the right subset of commands and
// to change the subset of commands that are considered "demand" vs
// "non-demand"
#define SUM_DEMAND(s)                                                   \
    (cmd[MemCmd::ReadReq]->s + cmd[MemCmd::WriteReq]->s +               \
     cmd[MemCmd::WriteLineReq]->s + cmd[MemCmd::ReadExReq]->s +         \
     cmd[MemCmd::ReadCleanReq]->s + cmd[MemCmd::ReadSharedReq]->s)

// should writebacks be included here?  prior code was inconsistent...
#define SUM_NON_DEMAND(s)                                       \
    (cmd[MemCmd::SoftPFReq]->s + cmd[MemCmd::HardPFReq]->s +    \
     cmd[MemCmd::SoftPFExReq]->s)

    demandHits.flags(total | nozero | nonan);
    demandHits = SUM_DEMAND(hits);
    for (int i = 0; i < max_requestors; i++) {
        demandHits.subname(i, system->getRequestorName(i));
    }

    demandHitsAtPf.flags(total | nozero | nonan);
    demandHitsAtPf = SUM_DEMAND(hitsAtPf);
    for (int i = 0; i < max_requestors; i++) {
        demandHitsAtPf.subname(i, system->getRequestorName(i));
    }

    demandHitsAtPfAlloc.flags(total | nozero | nonan);
    demandHitsAtPfAlloc = SUM_DEMAND(hitsAtPfAlloc);
    for (int i = 0; i < max_requestors; i++) {
        demandHitsAtPfAlloc.subname(i, system->getRequestorName(i));
    }

    demandHitsPerPC.flags(total | nozero | nonan);
    demandHitsPerPC = SUM_DEMAND(hitsPerPC);

    demandHitsAtPfPerPC.flags(total | nozero | nonan);
    demandHitsAtPfPerPC = SUM_DEMAND(hitsAtPfPerPC);

    demandHitsAtPfAllocPerPC.flags(total | nozero | nonan);
    demandHitsAtPfAllocPerPC = SUM_DEMAND(hitsAtPfAllocPerPC);

    overallHits.flags(total | nozero | nonan);
    overallHits = demandHits + SUM_NON_DEMAND(hits);
    for (int i = 0; i < max_requestors; i++) {
        overallHits.subname(i, system->getRequestorName(i));
    }

    demandMisses.flags(total | nozero | nonan);
    demandMisses = SUM_DEMAND(misses);
    for (int i = 0; i < max_requestors; i++) {
        demandMisses.subname(i, system->getRequestorName(i));
    }

    demandMissesPerPC.flags(total | nozero | nonan);
    demandMissesPerPC = SUM_DEMAND(missesPerPC);

    overallMisses.flags(total | nozero | nonan);
    overallMisses = demandMisses + SUM_NON_DEMAND(misses);
    for (int i = 0; i < max_requestors; i++) {
        overallMisses.subname(i, system->getRequestorName(i));
    }

    demandMissLatency.flags(total | nozero | nonan);
    demandMissLatency = SUM_DEMAND(missLatency);
    for (int i = 0; i < max_requestors; i++) {
        demandMissLatency.subname(i, system->getRequestorName(i));
    }

    overallMissLatency.flags(total | nozero | nonan);
    overallMissLatency = demandMissLatency + SUM_NON_DEMAND(missLatency);
    for (int i = 0; i < max_requestors; i++) {
        overallMissLatency.subname(i, system->getRequestorName(i));
    }

    demandHitLatency.flags(total | nozero | nonan);
    demandHitLatency = SUM_DEMAND(hitLatency);
    for (int i = 0; i < max_requestors; i++) {
        demandHitLatency.subname(i, system->getRequestorName(i));
    }
    overallHitLatency.flags(total | nozero | nonan);
    overallHitLatency = demandHitLatency + SUM_NON_DEMAND(hitLatency);
    for (int i = 0; i < max_requestors; i++) {
        overallHitLatency.subname(i, system->getRequestorName(i));
    }

    demandAccesses.flags(total | nozero | nonan);
    demandAccesses = demandHits + demandMisses;
    for (int i = 0; i < max_requestors; i++) {
        demandAccesses.subname(i, system->getRequestorName(i));
    }

    demandAccessesPerPC.flags(total | nozero | nonan);
    demandAccessesPerPC = demandHitsPerPC + demandMissesPerPC;

    overallAccesses.flags(total | nozero | nonan);
    overallAccesses = overallHits + overallMisses;
    for (int i = 0; i < max_requestors; i++) {
        overallAccesses.subname(i, system->getRequestorName(i));
    }

    demandMissRate.flags(total | nozero | nonan);
    demandMissRate = demandMisses / demandAccesses;
    for (int i = 0; i < max_requestors; i++) {
        demandMissRate.subname(i, system->getRequestorName(i));
    }

    demandMissRatePerPC.flags(nozero | nonan);
    demandMissRatePerPC = demandMissesPerPC / demandAccessesPerPC;

    overallMissRate.flags(total | nozero | nonan);
    overallMissRate = overallMisses / overallAccesses;
    for (int i = 0; i < max_requestors; i++) {
        overallMissRate.subname(i, system->getRequestorName(i));
    }

    demandAvgMissLatency.flags(total | nozero | nonan);
    demandAvgMissLatency = demandMissLatency / demandMisses;
    for (int i = 0; i < max_requestors; i++) {
        demandAvgMissLatency.subname(i, system->getRequestorName(i));
    }

    overallAvgMissLatency.flags(total | nozero | nonan);
    overallAvgMissLatency = overallMissLatency / overallMisses;
    for (int i = 0; i < max_requestors; i++) {
        overallAvgMissLatency.subname(i, system->getRequestorName(i));
    }

    blockedCycles.init(NUM_BLOCKED_CAUSES);
    blockedCycles
        .subname(Blocked_NoMSHRs, "no_mshrs")
        .subname(Blocked_NoTargets, "no_targets")
        ;


    blockedCauses.init(NUM_BLOCKED_CAUSES);
    blockedCauses
        .subname(Blocked_NoMSHRs, "no_mshrs")
        .subname(Blocked_NoTargets, "no_targets")
        ;

    avgBlocked
        .subname(Blocked_NoMSHRs, "no_mshrs")
        .subname(Blocked_NoTargets, "no_targets")
        ;
    avgBlocked = blockedCycles / blockedCauses;

    writebacks
        .init(max_requestors)
        .flags(total | nozero | nonan)
        ;
    for (int i = 0; i < max_requestors; i++) {
        writebacks.subname(i, system->getRequestorName(i));
    }

    demandMshrHits.flags(total | nozero | nonan);
    demandMshrHits = SUM_DEMAND(mshrHits);
    for (int i = 0; i < max_requestors; i++) {
        demandMshrHits.subname(i, system->getRequestorName(i));
    }

    demandMshrHitsPerPC.flags(total | nozero | nonan);
    demandMshrHitsPerPC = SUM_DEMAND(mshrHitsPerPC);

    demandMshrHitsAtPf.flags(total | nozero | nonan);
    demandMshrHitsAtPf = SUM_DEMAND(mshrHitsAtPf);
    for (int i = 0; i < max_requestors; i++) {
        demandMshrHitsAtPf.subname(i, system->getRequestorName(i));
    }

    demandMshrHitsAtPfPerPC.flags(total | nozero | nonan);
    demandMshrHitsAtPfPerPC = SUM_DEMAND(mshrHitsAtPfPerPC);

    overallMshrHits.flags(total | nozero | nonan);
    overallMshrHits = demandMshrHits + SUM_NON_DEMAND(mshrHits);
    for (int i = 0; i < max_requestors; i++) {
        overallMshrHits.subname(i, system->getRequestorName(i));
    }

    demandMshrMisses.flags(total | nozero | nonan);
    demandMshrMisses = SUM_DEMAND(mshrMisses);
    for (int i = 0; i < max_requestors; i++) {
        demandMshrMisses.subname(i, system->getRequestorName(i));
    }

    demandMshrMissesPerPC.flags(total | nozero | nonan);
    demandMshrMissesPerPC = SUM_DEMAND(mshrMissesPerPC);

    overallMshrMisses.flags(total | nozero | nonan);
    overallMshrMisses = demandMshrMisses + SUM_NON_DEMAND(mshrMisses);
    for (int i = 0; i < max_requestors; i++) {
        overallMshrMisses.subname(i, system->getRequestorName(i));
    }

    demandMshrMissLatency.flags(total | nozero | nonan);
    demandMshrMissLatency = SUM_DEMAND(mshrMissLatency);
    for (int i = 0; i < max_requestors; i++) {
        demandMshrMissLatency.subname(i, system->getRequestorName(i));
    }

    overallMshrMissLatency.flags(total | nozero | nonan);
    overallMshrMissLatency =
        demandMshrMissLatency + SUM_NON_DEMAND(mshrMissLatency);
    for (int i = 0; i < max_requestors; i++) {
        overallMshrMissLatency.subname(i, system->getRequestorName(i));
    }

    overallMshrUncacheable.flags(total | nozero | nonan);
    overallMshrUncacheable =
        SUM_DEMAND(mshrUncacheable) + SUM_NON_DEMAND(mshrUncacheable);
    for (int i = 0; i < max_requestors; i++) {
        overallMshrUncacheable.subname(i, system->getRequestorName(i));
    }

    overallMshrUncacheableLatency.flags(total | nozero | nonan);
    overallMshrUncacheableLatency =
        SUM_DEMAND(mshrUncacheableLatency) +
        SUM_NON_DEMAND(mshrUncacheableLatency);
    for (int i = 0; i < max_requestors; i++) {
        overallMshrUncacheableLatency.subname(i, system->getRequestorName(i));
    }

    demandMshrMissRate.flags(total | nozero | nonan);
    demandMshrMissRate = demandMshrMisses / demandAccesses;
    for (int i = 0; i < max_requestors; i++) {
        demandMshrMissRate.subname(i, system->getRequestorName(i));
    }

    overallMshrMissRate.flags(total | nozero | nonan);
    overallMshrMissRate = overallMshrMisses / overallAccesses;
    for (int i = 0; i < max_requestors; i++) {
        overallMshrMissRate.subname(i, system->getRequestorName(i));
    }

    demandAvgMshrMissLatency.flags(total | nozero | nonan);
    demandAvgMshrMissLatency = demandMshrMissLatency / demandMshrMisses;
    for (int i = 0; i < max_requestors; i++) {
        demandAvgMshrMissLatency.subname(i, system->getRequestorName(i));
    }

    overallAvgMshrMissLatency.flags(total | nozero | nonan);
    overallAvgMshrMissLatency = overallMshrMissLatency / overallMshrMisses;
    for (int i = 0; i < max_requestors; i++) {
        overallAvgMshrMissLatency.subname(i, system->getRequestorName(i));
    }

    overallAvgMshrUncacheableLatency.flags(total | nozero | nonan);
    overallAvgMshrUncacheableLatency =
        overallMshrUncacheableLatency / overallMshrUncacheable;
    for (int i = 0; i < max_requestors; i++) {
        overallAvgMshrUncacheableLatency.subname(i,
            system->getRequestorName(i));
    }

    dataExpansions.flags(nozero | nonan);
    dataContractions.flags(nozero | nonan);
	
	overallmshrcycles.flags(nozero);
    overallmshrservices.flags(nozero);

    hitsAtPfCoverAccess.flags(total | nozero | nonan);
    hitsAtPfCoverAccess = demandHitsAtPf / demandAccesses;

    hitsAtPfCoverAccessPerPC.flags(total | nozero | nonan);
    hitsAtPfCoverAccessPerPC = demandHitsAtPfPerPC / demandAccessesPerPC;

    hitsAtPfAllocCoverAccess.flags(total | nozero | nonan);
    hitsAtPfAllocCoverAccess = demandHitsAtPfAlloc / demandAccesses;

    hitsAtPfAllocCoverAccessPerPC.flags(total | nozero | nonan);
    hitsAtPfAllocCoverAccessPerPC = demandHitsAtPfAllocPerPC / demandAccessesPerPC;

    hitsPfRatio.flags(total | nozero | nonan);
    hitsPfRatio = demandHitsAtPfAlloc / demandHitsAtPf;

    hitsPfRatioPerPC.flags(total | nozero | nonan);
    hitsPfRatioPerPC = demandHitsAtPfAllocPerPC / demandHitsAtPfPerPC;

    // PerPC stats
    for (int i = 0; i < stats_pc_list.size(); i++) {
        std::stringstream stream;
        stream << std::hex << stats_pc_list[i];
        std::string pc_hex = stream.str();

        demandHitsPerPC.subname(i, pc_hex);
        demandHitsAtPfPerPC.subname(i, pc_hex);
        demandHitsAtPfAllocPerPC.subname(i, pc_hex);
        hitsAtPfCoverAccessPerPC.subname(i, pc_hex);
        hitsAtPfAllocCoverAccessPerPC.subname(i, pc_hex);
        hitsPfRatioPerPC.subname(i, pc_hex);
        demandMissesPerPC.subname(i, pc_hex);
        demandAccessesPerPC.subname(i, pc_hex);
        demandMissRatePerPC.subname(i, pc_hex);
        demandMshrHitsPerPC.subname(i, pc_hex);
        demandMshrHitsAtPfPerPC.subname(i, pc_hex);
        demandMshrMissesPerPC.subname(i, pc_hex);
    }

}

void
BaseCache::regProbePoints()
{
    ppHit = new ProbePointArg<PacketPtr>(this->getProbeManager(), "Hit");
    ppMiss = new ProbePointArg<PacketPtr>(this->getProbeManager(), "Miss");
    ppFill = new ProbePointArg<PacketPtr>(this->getProbeManager(), "Fill");
    ppDataUpdate =
        new ProbePointArg<DataUpdate>(this->getProbeManager(), "Data Update");

    
    ppL1Req = new ProbePointArg<PacketPtr>(this->getProbeManager(), "Request");
    ppL1Resp = new ProbePointArg<PacketPtr>(this->getProbeManager(), "Response");
}

///////////////
//
// CpuSidePort
//
///////////////
bool
BaseCache::CpuSidePort::recvTimingSnoopResp(PacketPtr pkt)
{
    // Snoops shouldn't happen when bypassing caches
    assert(!cache->system->bypassCaches());

    assert(pkt->isResponse());

    // Express snoop responses from requestor to responder, e.g., from L1 to L2
    cache->recvTimingSnoopResp(pkt);
    return true;
}


bool
BaseCache::CpuSidePort::tryTiming(PacketPtr pkt)
{
    if (cache->system->bypassCaches() || pkt->isExpressSnoop()) {
        // always let express snoop packets through even if blocked
        return true;
    } else if (blocked || mustSendRetry) {
        // either already committed to send a retry, or blocked
        DPRINTF(
            CachePort, "tryTiming failed [blocked:%d] [mustSendRetry:%d]",
                        blocked ? 1 : 0,
                        mustSendRetry ? 1 : 0
        );
        mustSendRetry = true;
        return false;
    }
    mustSendRetry = false;
    return true;
}

bool
BaseCache::CpuSidePort::recvTimingReq(PacketPtr pkt)
{
    assert(pkt->isRequest());

    cache->ppL1Req->notify(pkt);

    if (cache->system->bypassCaches()) {
        // Just forward the packet if caches are disabled.
        // @todo This should really enqueue the packet rather
        [[maybe_unused]] bool success = cache->memSidePort.sendTimingReq(pkt);
        assert(success);
        return true;
    } else if (tryTiming(pkt)) {
        cache->recvTimingReq(pkt);
        return true;
    }
    return false;
}

Tick
BaseCache::CpuSidePort::recvAtomic(PacketPtr pkt)
{
    if (cache->system->bypassCaches()) {
        // Forward the request if the system is in cache bypass mode.
        return cache->memSidePort.sendAtomic(pkt);
    } else {
        return cache->recvAtomic(pkt);
    }
}

void
BaseCache::CpuSidePort::recvFunctional(PacketPtr pkt)
{
    if (cache->system->bypassCaches()) {
        // The cache should be flushed if we are in cache bypass mode,
        // so we don't need to check if we need to update anything.
        cache->memSidePort.sendFunctional(pkt);
        return;
    }

    // functional request
    cache->functionalAccess(pkt, true);
}

AddrRangeList
BaseCache::CpuSidePort::getAddrRanges() const
{
    return cache->getAddrRanges();
}


BaseCache::
CpuSidePort::CpuSidePort(const std::string &_name, BaseCache *_cache,
                         const std::string &_label)
    : CacheResponsePort(_name, _cache, _label), cache(_cache)
{
}

///////////////
//
// MemSidePort
//
///////////////
bool
BaseCache::MemSidePort::recvTimingResp(PacketPtr pkt)
{
    cache->recvTimingResp(pkt);
    return true;
}

// Express snooping requests to memside port
void
BaseCache::MemSidePort::recvTimingSnoopReq(PacketPtr pkt)
{
    // Snoops shouldn't happen when bypassing caches
    assert(!cache->system->bypassCaches());

    // handle snooping requests
    cache->recvTimingSnoopReq(pkt);
}

Tick
BaseCache::MemSidePort::recvAtomicSnoop(PacketPtr pkt)
{
    // Snoops shouldn't happen when bypassing caches
    assert(!cache->system->bypassCaches());

    return cache->recvAtomicSnoop(pkt);
}

void
BaseCache::MemSidePort::recvFunctionalSnoop(PacketPtr pkt)
{
    // Snoops shouldn't happen when bypassing caches
    assert(!cache->system->bypassCaches());

    // functional snoop (note that in contrast to atomic we don't have
    // a specific functionalSnoop method, as they have the same
    // behaviour regardless)
    cache->functionalAccess(pkt, false);
}

void
BaseCache::CacheReqPacketQueue::sendDeferredPacket()
{
    // sanity check
    assert(!waitingOnRetry);

    // there should never be any deferred request packets in the
    // queue, instead we resly on the cache to provide the packets
    // from the MSHR queue or write queue
    assert(deferredPacketReadyTime() == MaxTick);

    // check for request packets (requests & writebacks)
    QueueEntry* entry = cache.getNextQueueEntry();

    if (!entry) {
        // can happen if e.g. we attempt a writeback and fail, but
        // before the retry, the writeback is eliminated because
        // we snoop another cache's ReadEx.
    } else {
        // let our snoop responses go first if there are responses to
        // the same addresses
        if (checkConflictingSnoop(entry->getTarget()->pkt)) {
            return;
        }
        waitingOnRetry = entry->sendPacket(cache);
    }

    // if we succeeded and are not waiting for a retry, schedule the
    // next send considering when the next queue is ready, note that
    // snoop responses have their own packet queue and thus schedule
    // their own events
    if (!waitingOnRetry) {
        schedSendEvent(cache.nextQueueReadyTime());
    } else {
        DPRINTF(RequestSlot, "[Failed] Waiting on retry\n");
    }
}

BaseCache::MemSidePort::MemSidePort(const std::string &_name,
                                    BaseCache *_cache,
                                    const std::string &_label)
    : CacheRequestPort(_name, _cache, _reqQueue, _snoopRespQueue),
      _reqQueue(*_cache, *this, _snoopRespQueue, _label),
      _snoopRespQueue(*_cache, *this, true, _label), cache(_cache)
{
}

void
WriteAllocator::updateMode(Addr write_addr, unsigned write_size,
                           Addr blk_addr)
{
    // check if we are continuing where the last write ended
    if (nextAddr == write_addr) {
        delayCtr[blk_addr] = delayThreshold;
        // stop if we have already saturated
        if (mode != WriteMode::NO_ALLOCATE) {
            byteCount += write_size;
            // switch to streaming mode if we have passed the lower
            // threshold
            if (mode == WriteMode::ALLOCATE &&
                byteCount > coalesceLimit) {
                mode = WriteMode::COALESCE;
                DPRINTF(Cache, "Switched to write coalescing\n");
            } else if (mode == WriteMode::COALESCE &&
                       byteCount > noAllocateLimit) {
                // and continue and switch to non-allocating mode if we
                // pass the upper threshold
                mode = WriteMode::NO_ALLOCATE;
                DPRINTF(Cache, "Switched to write-no-allocate\n");
            }
        }
    } else {
        // we did not see a write matching the previous one, start
        // over again
        byteCount = write_size;
        mode = WriteMode::ALLOCATE;
        resetDelay(blk_addr);
    }
    nextAddr = write_addr + write_size;
}

} // namespace gem5
