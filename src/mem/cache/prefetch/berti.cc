/*
 * Copyright (c) 2023 XJTU
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

#include "mem/cache/prefetch/berti.hh"

#include <cassert>

#include "base/intmath.hh"
#include "base/logging.hh"
#include "base/random.hh"
#include "base/trace.hh"
#include "debug/HWPrefetch.hh"
#include "mem/cache/prefetch/associative_set_impl.hh"
#include "mem/cache/replacement_policies/base.hh"
#include "params/BertiPrefetcher.hh"

namespace gem5
{

    GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
    namespace prefetch
    {

        Berti::HistoryEntry::HistoryEntry()
            : TaggedEntry()
        {
            invalidate();
        }

        void
        Berti::HistoryEntry::invalidate()
        {
            TaggedEntry::invalidate();
            history_set_num = 0;
            for (int i = 0; i < 16; i++)
            {
                history_sets[i].line_address = 0;
                history_sets[i].timestamp = 0;
            }
        }

        Berti::DeltaEntry::DeltaEntry()
            : TaggedEntry()
        {
            invalidate();
        }

        void
        Berti::DeltaEntry::invalidate()
        {
            TaggedEntry::invalidate();
            delta_array_num = 0;
            access_array_num = 0;
            counter = 0;
            for (int i = 0; i < 16; i++)
            {
                delta_arrays[i].delta = 0;
                delta_arrays[i].coverage = 0;
                delta_arrays[i].status = 0;
                access_arrays[i].line_address = 0;
                access_arrays[i].access_tick = 0;
            }
        }

        Berti::Berti(const BertiPrefetcherParams &p)
            : Queued(p),
              useRequestorId(p.use_requestor_id),
              degree(p.degree),
              HistoryTableInfo(p.history_table_assoc, p.history_table_entries, p.history_table_indexing_policy,
                               p.history_table_replacement_policy),
              DeltaTableInfo(p.delta_table_assoc, p.delta_table_entries, p.delta_table_indexing_policy,
                             p.delta_table_replacement_policy)
        {
        }

        Berti::HistoryTable *
        Berti::findHistoryTable(int context)
        {
            // Check if table for given context exists
            auto it = HistoryTables.find(context);
            if (it != HistoryTables.end())
                return &it->second;

            // If table does not exist yet, create one
            return allocateHistoryContext(context);
        }

        Berti::HistoryTable *
        Berti::allocateHistoryContext(int context)
        {
            // Create new table
            auto insertion_result = HistoryTables.insert(std::make_pair(context,
                                                                        HistoryTable(HistoryTableInfo.assoc, HistoryTableInfo.numEntries,
                                                                                     HistoryTableInfo.indexingPolicy, HistoryTableInfo.replacementPolicy)));
            // ,HistoryEntry(initConfidence))));

            DPRINTF(HWPrefetch, "Adding context %i with stride entries\n", context);

            // Get iterator to new pc table, and then return a pointer to the new table
            return &(insertion_result.first->second);
        }

        Berti::DeltaTable *
        Berti::findDeltaTable(int context)
        {
            // Check if table for given context exists
            auto it = DeltaTables.find(context);
            if (it != DeltaTables.end())
                return &it->second;

            // If table does not exist yet, create one
            return allocateDeltaContext(context);
        }

        Berti::DeltaTable *
        Berti::allocateDeltaContext(int context)
        {
            // Create new table
            auto insertion_result = DeltaTables.insert(std::make_pair(context,
                                                                      DeltaTable(DeltaTableInfo.assoc, DeltaTableInfo.numEntries,
                                                                                 DeltaTableInfo.indexingPolicy, DeltaTableInfo.replacementPolicy)));
            // ,HistoryEntry(initConfidence))));

            DPRINTF(HWPrefetch, "Adding context %i with stride entries\n", context);

            // Get iterator to new pc table, and then return a pointer to the new table
            return &(insertion_result.first->second);
        }

        void
        Berti::calculatePrefetch(const PrefetchInfo &pfi,
                                 std::vector<AddrPriority> &addresses)
        {
            if (!pfi.hasPC())
            {
                DPRINTF(HWPrefetch, "Ignoring request with no PC.\n");
                return;
            }

            // Get required packet info
            Addr pf_addr = pfi.getAddr();
            Addr pc = pfi.getPC();
            bool is_secure = pfi.isSecure();
            Tick cur_tick = pfi.getCurTick(); // Tick when insert into MSHR
            RequestorID requestor_id = useRequestorId ? pfi.getRequestorId() : 0;

            // update History Table when insert MSHR , latency are included using pfi.getCurTick()
            if (pfi.getInsertMSHR())
            {
                // printf("pc : %#x , addr : %#x , get insert MSHR\n", pc, pf_addr);
                HistoryTable *history_table = findHistoryTable(requestor_id);
                HistoryEntry *history_entry = history_table->findEntry(pc, is_secure);
                if (history_entry != nullptr)
                { // find HistoryEntry
                    if (history_entry->history_set_num <= 16)
                    { // berti records 16 ways for the same pc
                        (history_entry->history_set_num == 16) ?: history_entry->history_set_num++;
                        for (int i = history_entry->history_set_num - 1; i > 0; i--)
                        {
                            history_entry->history_sets[i].line_address =
                                history_entry->history_sets[i - 1].line_address;
                            history_entry->history_sets[i].timestamp =
                                history_entry->history_sets[i - 1].timestamp;
                        }
                        history_entry->history_sets[0].line_address = pf_addr;
                        history_entry->history_sets[0].timestamp = cur_tick;
                    }
                    else
                        history_entry->history_set_num = 0; // exception
                }
                else
                {
                    HistoryEntry *entry = history_table->findVictim(pc);
                    // Insert new entry's data
                    entry->history_sets[0].line_address = pf_addr;
                    entry->history_sets[0].timestamp = cur_tick;
                    entry->history_set_num = 1;
                    history_table->insertEntry(pc, is_secure, entry);
                }
            }

            // update Delta Table when there is a cache hit
            if (pfi.getCacheHit())
            {
                // printf("pc : %#x , addr : %#x , get cache hit\n", pc, pf_addr);
                DeltaTable *delta_table = findDeltaTable(requestorId);
                DeltaEntry *delta_entry = delta_table->findEntry(pc, is_secure);
                if (delta_entry != nullptr)
                {
                    if (delta_entry->access_array_num <= 16) // record at max 16 access arrays , same as Berti
                    {
                        (delta_entry->access_array_num == 16) ?: delta_entry->access_array_num++;
                        for (int i = delta_entry->access_array_num - 1; i > 0; i--)
                        {
                            delta_entry->access_arrays[i].line_address =
                                delta_entry->access_arrays[i - 1].line_address;
                            delta_entry->access_arrays[i].access_tick =
                                delta_entry->access_arrays[i - 1].access_tick;
                        }
                        delta_entry->access_arrays[0].line_address = pf_addr;
                        delta_entry->access_arrays[0].access_tick = curTick();
                    }
                    else
                        delta_entry->access_array_num = 0; // exception

                    if (delta_entry->delta_array_num < 16) // start computing the coverage of deltas
                    {
                        HistoryTable *history_table = findHistoryTable(requestor_id);
                        HistoryEntry *history_entry = history_table->findEntry(pc, is_secure);
                        if (history_entry != nullptr)
                        {
                            for (int i = 0; i < history_entry->history_set_num; i++)
                            {
                                if (history_entry->history_sets[i].line_address == pf_addr)
                                {
                                    for (int j = 0; j < delta_entry->access_array_num; j++)
                                    {
                                        if (delta_entry->access_arrays[j].access_tick < history_entry->history_sets[i].timestamp)
                                        {
                                            int delta = - delta_entry->access_arrays[j].line_address + history_entry->history_sets[i].line_address;
                                            if (!delta)
                                                continue;
                                            for (int k = 0; k < delta_entry->delta_array_num; k++)
                                            {
                                                // match in delta_arrays
                                                if (delta_entry->delta_arrays[k].delta == delta)
                                                {
                                                    delta_entry->delta_arrays[k].coverage++;
                                                    break;
                                                }
                                                // not found match in delta_arrays , fill new delta_array
                                                if (k == delta_entry->delta_array_num - 1)
                                                {
                                                    if (delta_entry->delta_array_num < 16)
                                                    {
                                                        delta_entry->delta_arrays[delta_entry->delta_array_num].delta = delta;
                                                        delta_entry->delta_arrays[delta_entry->delta_array_num].coverage++;
                                                    }
                                                    // delta_entry full , replace delta_array based on coverage
                                                    else if (delta_entry->delta_array_num == 16)
                                                    {
                                                        // update delta_array both FIFO and < 50% coverage , remember to reset status.
                                                        for (int l = 0; l < delta_entry->delta_array_num; l++)
                                                        {
                                                            if (delta_entry->delta_arrays[l].status > 0)
                                                                continue;
                                                            for (int m = l; m < delta_entry->delta_array_num - 1; m++)
                                                            {
                                                                delta_entry->delta_arrays[m].coverage = delta_entry->delta_arrays[m + 1].coverage;
                                                                delta_entry->delta_arrays[m].delta = delta_entry->delta_arrays[m + 1].delta;
                                                                delta_entry->delta_arrays[m].status = delta_entry->delta_arrays[m + 1].status;
                                                            }
                                                            delta_entry->delta_arrays[delta_entry->delta_array_num - 1].coverage = 1;
                                                            delta_entry->delta_arrays[delta_entry->delta_array_num - 1].delta = delta;
                                                            delta_entry->delta_arrays[delta_entry->delta_array_num - 1].status = 0;
                                                            break;
                                                        }
                                                    }
                                                }
                                            }
                                            (delta_entry->delta_array_num == 16) ?: delta_entry->delta_array_num++;
                                            delta_entry->counter++;
                                            // calculate the most possible delta
                                            if (delta_entry->counter >= 16)
                                            {
                                                double sum = 0;
                                                for (int k = 0; k < delta_entry->delta_array_num; k++)
                                                {
                                                    sum += delta_entry->delta_arrays[k].coverage;
                                                }
                                                for (int k = 0; k < delta_entry->delta_array_num; k++)
                                                {
                                                    double temp = delta_entry->delta_arrays[k].coverage / sum;
                                                    if (temp > 0.3)
                                                        delta_entry->delta_arrays[k].status = 1;
                                                    else if (temp > 0.1)
                                                        delta_entry->delta_arrays[k].status = 2;
                                                    else
                                                        delta_entry->delta_arrays[k].status = 0;
                                                }
                                                delta_entry->counter = 0;
                                            }
                                        }
                                    }
                                    break;
                                }
                            }
                        }
                    }
                    else
                        delta_entry->delta_array_num = 0; // exception
                }
                else
                {
                    DeltaEntry *entry = delta_table->findVictim(pc);
                    // Insert new entry's data
                    entry->access_array_num = 1;
                    entry->access_arrays[0].line_address = pf_addr;
                    entry->access_arrays[0].access_tick = curTick();
                    delta_table->insertEntry(pc, is_secure, entry);
                }
            }
            // prefetch generation based on delta entry
            DeltaTable *delta_table = findDeltaTable(requestorId);
            DeltaEntry *delta_entry = delta_table->findEntry(pc, is_secure);
            if (delta_entry != nullptr){
                for (int i = 0; i < delta_entry->delta_array_num; i++)
                {
                    if (delta_entry->delta_arrays[i].status == 1)
                    {
                        // for (int d = 1; d <= degree; d++) {
                        //     // Round strides up to atleast 1 cacheline
                        //     int prefetch_delta = delta_entry->delta_arrays[i].delta;
                        //     if (abs(prefetch_delta) < blkSize) {
                        //         prefetch_delta = (prefetch_delta < 0) ? -blkSize : blkSize;
                        //     }

                        //     Addr new_addr = pf_addr + d * prefetch_delta;
                        //     addresses.push_back(AddrPriority(new_addr, 0));
                        // }
                        for (int d = 1; d <= degree; d++){
                            int prefetch_delta = delta_entry->delta_arrays[i].delta;
                            Addr new_addr = pf_addr + d*prefetch_delta;
                            addresses.push_back(AddrPriority(new_addr, 0));
                        }
                    }
                }
            }

        }

        uint32_t
        BertiPrefetcherHashedSetAssociative::extractSet(const Addr pc) const
        {
            const Addr hash1 = pc >> 1;
            const Addr hash2 = hash1 >> tagShift;
            return (hash1 ^ hash2) & setMask;
        }

        Addr
        BertiPrefetcherHashedSetAssociative::extractTag(const Addr addr) const
        {
            return addr;
        }

    } // namespace prefetch
} // namespace gem5
