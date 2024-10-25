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
 * Describes a strided prefetcher.
 */

#ifndef __MEM_CACHE_PREFETCH_BERTI_HH__
#define __MEM_CACHE_PREFETCH_BERTI_HH__

#include <string>
#include <unordered_map>
#include <vector>

#include "base/sat_counter.hh"
#include "base/types.hh"
#include "mem/cache/prefetch/associative_set.hh"
#include "mem/cache/prefetch/queued.hh"
#include "mem/cache/replacement_policies/replaceable_entry.hh"
#include "mem/cache/tags/indexing_policies/set_associative.hh"
#include "mem/packet.hh"
#include "params/BertiPrefetcherHashedSetAssociative.hh"

namespace gem5
{

class BaseIndexingPolicy;
GEM5_DEPRECATED_NAMESPACE(ReplacementPolicy, replacement_policy);
namespace replacement_policy
{
    class Base;
}
struct BertiPrefetcherParams;

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

/**
 * Override the default set associative to apply a specific hash function
 * when extracting a set.
 */
class BertiPrefetcherHashedSetAssociative : public SetAssociative
{
  protected:
    uint32_t extractSet(const Addr addr) const override;
    Addr extractTag(const Addr addr) const override;

  public:
    BertiPrefetcherHashedSetAssociative(
        const BertiPrefetcherHashedSetAssociativeParams &p)
      : SetAssociative(p)
    {
    }
    ~BertiPrefetcherHashedSetAssociative() = default;
};

class Berti : public Queued
{
  protected:

    const bool useRequestorId;

    const int degree;

    /**
     * Create a new history table , elements are same as the paper.
     */
    const struct HistoryTableInfo
    {
        const int assoc;
        const int numEntries;

        BaseIndexingPolicy* const indexingPolicy;
        replacement_policy::Base* const replacementPolicy;

        HistoryTableInfo(int assoc, int num_entries,
            BaseIndexingPolicy* indexing_policy,
            replacement_policy::Base* repl_policy)
          : assoc(assoc), numEntries(num_entries),
            indexingPolicy(indexing_policy), replacementPolicy(repl_policy)
        {
        }
    } HistoryTableInfo;

    /** Tagged by hashed PCs. */
    struct HistoryEntry : public TaggedEntry
    {
        HistoryEntry();
        void invalidate() override;

        int history_set_num;
        struct history_set{
            Addr line_address;  // same as line address in Berti
            Tick timestamp;     // same as timestamp in Berti
        }   history_sets[16];

    };
    typedef AssociativeSet<HistoryEntry> HistoryTable;
    std::unordered_map<int, HistoryTable> HistoryTables;

    /**
     * Create a new delta table , elements are same as the paper.
     */
    const struct DeltaTableInfo
    {
        const int assoc;
        const int numEntries;

        BaseIndexingPolicy* const indexingPolicy;
        replacement_policy::Base* const replacementPolicy;

        DeltaTableInfo(int assoc, int num_entries,
            BaseIndexingPolicy* indexing_policy,
            replacement_policy::Base* repl_policy)
          : assoc(assoc), numEntries(num_entries),
            indexingPolicy(indexing_policy), replacementPolicy(repl_policy)
        {
        }
    } DeltaTableInfo;

    /** Tagged by hashed PCs. */
    struct DeltaEntry : public TaggedEntry
    {

        DeltaEntry();
        void invalidate() override;

        int counter;
        int delta_array_num;
        int access_array_num;

        struct access_array{
            Addr line_address;
            Tick access_tick;
        } access_arrays[16];

        struct delta_array{
            int delta;
            int coverage;
            int status; // status = 1( >65% ) , 2( >30% & <65%)
        }   delta_arrays[16];

    };
    typedef AssociativeSet<DeltaEntry> DeltaTable;
    std::unordered_map<int, DeltaTable> DeltaTables;

    /**
     * Try to find a table of entries for the given context. If none is
     * found, a new table is created.
     *
     * @param context The context to be searched for.
     * @return The table corresponding to the given context.
     */
    HistoryTable* findHistoryTable(int context);

    /**
     * Create a PC table for the given context.
     *
     * @param context The context of the new PC table.
     * @return The new PC table
     */
    HistoryTable* allocateHistoryContext(int context);

    DeltaTable* findDeltaTable(int context);
    
    DeltaTable* allocateDeltaContext(int context);

  public:
    Berti(const BertiPrefetcherParams &p);

    void calculatePrefetch(const PrefetchInfo &pfi,
                           std::vector<AddrPriority> &addresses) override;
};

} // namespace prefetch
} // namespace gem5

#endif // __MEM_CACHE_PREFETCH_STRIDE_HH__
