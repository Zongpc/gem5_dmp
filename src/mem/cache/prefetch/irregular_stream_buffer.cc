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

#include "mem/cache/prefetch/irregular_stream_buffer.hh"

#include "debug/HWPrefetch.hh"
#include "mem/cache/prefetch/associative_set_impl.hh"
#include "params/IrregularStreamBufferPrefetcher.hh"

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

// 构造函数，初始化IrregularStreamBuffer对象
IrregularStreamBuffer::IrregularStreamBuffer(
    const IrregularStreamBufferPrefetcherParams &p)
  // 初始化基类Queued和成员变量
    : Queued(p),
    chunkSize(p.chunk_size),
    prefetchCandidatesPerEntry(p.prefetch_candidates_per_entry),
    degree(p.degree),
    // 使用参数初始化trainingUnit
    trainingUnit(p.training_unit_assoc, p.training_unit_entries,
                p.training_unit_indexing_policy,
                p.training_unit_replacement_policy),
    // 使用参数和AddressMappingEntry对象初始化psAddressMappingCache
    psAddressMappingCache(p.address_map_cache_assoc,
                        p.address_map_cache_entries,
                        p.ps_address_map_cache_indexing_policy,
                        p.ps_address_map_cache_replacement_policy,
                        AddressMappingEntry(prefetchCandidatesPerEntry,
                                            p.num_counter_bits)),
    // 使用参数和AddressMappingEntry对象初始化spAddressMappingCache
    spAddressMappingCache(p.address_map_cache_assoc,
                        p.address_map_cache_entries,
                        p.sp_address_map_cache_indexing_policy,
                        p.sp_address_map_cache_replacement_policy,
                        AddressMappingEntry(prefetchCandidatesPerEntry,
                                            p.num_counter_bits)),
    // 初始化structuralAddressCounter为0
    structuralAddressCounter(0)
{
    // 确保prefetchCandidatesPerEntry是2的幂
    assert(isPowerOf2(prefetchCandidatesPerEntry));
}

void IrregularStreamBuffer::calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses, const PacketPtr &pkt) {}
/**
 * @brief 计算预取地址
 * 
 * 此函数根据给定的预取信息计算潜在的预取地址，并将其添加到预取地址向量中。
 * 
 * @param pfi 预取信息，包括PC、访问地址等
 * @param addresses 输出参数，用于存储计算出的预取地址
 */
void IrregularStreamBuffer::calculatePrefetch(const PrefetchInfo &pfi,
    std::vector<AddrPriority> &addresses)
{
    // This prefetcher requires a PC
    if (!pfi.hasPC()) {
        return;
    }
    bool is_secure = pfi.isSecure();
    Addr pc = pfi.getPC();
    Addr addr = blockIndex(pfi.getAddr());

    // Training, if the entry exists, then we found a correlation between
    // the entry lastAddress (named as correlated_addr_A) and the address of
    // the current access (named as correlated_addr_B)
    TrainingUnitEntry *entry = trainingUnit.findEntry(pc, is_secure);
    bool correlated_addr_found = false;
    Addr correlated_addr_A = 0;
    Addr correlated_addr_B = 0;
    if (entry != nullptr && entry->lastAddressSecure == is_secure) {
        trainingUnit.accessEntry(entry);
        correlated_addr_found = true;
        correlated_addr_A = entry->lastAddress;
        correlated_addr_B = addr;
    } else {
        entry = trainingUnit.findVictim(pc);
        assert(entry != nullptr);

        trainingUnit.insertEntry(pc, is_secure, entry);
    }
    // Update the entry
    entry->lastAddress = addr;
    entry->lastAddressSecure = is_secure;

    if (correlated_addr_found) {
        // If a correlation was found, update the Physical-to-Structural
        // table accordingly
        AddressMapping &mapping_A = getPSMapping(correlated_addr_A, is_secure);
        AddressMapping &mapping_B = getPSMapping(correlated_addr_B, is_secure);
        if (mapping_A.counter > 0 && mapping_B.counter > 0) {
            // Entry for A and B
            if (mapping_B.address == (mapping_A.address + 1)) {
                mapping_B.counter++;
            } else {
                if (mapping_B.counter == 1) {
                    // Counter would hit 0, reassign address while keeping
                    // counter at 1
                    mapping_B.address = mapping_A.address + 1;
                    addStructuralToPhysicalEntry(mapping_B.address, is_secure,
                            correlated_addr_B);
                } else {
                    mapping_B.counter--;
                }
            }
        } else {
            if (mapping_A.counter == 0) {
                // if A is not valid, generate a new structural address
                mapping_A.counter++;
                mapping_A.address = structuralAddressCounter;
                structuralAddressCounter += chunkSize;
                addStructuralToPhysicalEntry(mapping_A.address,
                        is_secure, correlated_addr_A);
            }
            mapping_B.counter.reset();
            mapping_B.counter++;
            mapping_B.address = mapping_A.address + 1;
            // update SP-AMC
            addStructuralToPhysicalEntry(mapping_B.address, is_secure,
                    correlated_addr_B);
        }
    }

    // Use the PS mapping to predict future accesses using the current address
    // - Look for the structured address
    // - if it exists, use it to generate prefetches for the subsequent
    //   addresses in ascending order, as many as indicated by the degree
    //   (given the structured address S, prefetch S+1, S+2, .. up to S+degree)
    Addr amc_address = addr / prefetchCandidatesPerEntry;
    Addr map_index   = addr % prefetchCandidatesPerEntry;
    AddressMappingEntry *ps_am = psAddressMappingCache.findEntry(amc_address,
                                                                 is_secure);
    if (ps_am != nullptr) {
        AddressMapping &mapping = ps_am->mappings[map_index];
        if (mapping.counter > 0) {
            Addr sp_address = mapping.address / prefetchCandidatesPerEntry;
            Addr sp_index   = mapping.address % prefetchCandidatesPerEntry;
            AddressMappingEntry *sp_am =
                spAddressMappingCache.findEntry(sp_address, is_secure);
            if (sp_am == nullptr) {
                // The entry has been evicted, can not generate prefetches
                return;
            }
            for (unsigned d = 1;
                    d <= degree && (sp_index + d) < prefetchCandidatesPerEntry;
                    d += 1)
            {
                AddressMapping &spm = sp_am->mappings[sp_index + d];
                //generate prefetch
                if (spm.counter > 0) {
                    Addr pf_addr = spm.address << lBlkSize;
                    addresses.push_back(AddrPriority(pf_addr, 0));
                }
            }
        }
    }
}

// 根据物理地址和是否安全，获取不规则流缓冲区的地址映射
IrregularStreamBuffer::AddressMapping&
IrregularStreamBuffer::getPSMapping(Addr paddr, bool is_secure)
{
    // 计算AMC地址，用于定位到PS-AMC中的特定条目
    Addr amc_address = paddr / prefetchCandidatesPerEntry;
    // 计算映射索引，用于定位到PS-AMC条目中的特定映射
    Addr map_index   = paddr % prefetchCandidatesPerEntry;
    // 试图在缓存中找到对应的PS-AMC条目
    AddressMappingEntry *ps_entry =
        psAddressMappingCache.findEntry(amc_address, is_secure);
    if (ps_entry != nullptr) {
        // 如果已经存在PS-AMC条目，则访问该条目以更新其访问状态
        psAddressMappingCache.accessEntry(ps_entry);
    } else {
        // 如果不存在，则在缓存中找到一个牺牲品条目以存储新的映射
        ps_entry = psAddressMappingCache.findVictim(amc_address);
        assert(ps_entry != nullptr);

        // 插入新的PS-AMC条目到缓存中
        psAddressMappingCache.insertEntry(amc_address, is_secure, ps_entry);
    }
    // 返回找到或插入的条目中的特定映射
    return ps_entry->mappings[map_index];
}

/// 在结构化到物理地址的映射表中添加一个条目。
///
/// 本函数负责将虚拟地址（结构化地址）映射到物理地址，以便进行有效的内存访问。
/// 它处理了地址映射的细节，包括如何在缓存中找到或插入映射条目。
///
/// @param structural_address 虚拟地址，需要映射的结构化地址。
/// @param is_secure 标识该地址是否属于安全上下文。
/// @param physical_address 物理地址，结构化地址将映射到这个物理地址。
void IrregularStreamBuffer::addStructuralToPhysicalEntry(
    Addr structural_address, bool is_secure, Addr physical_address)
{
    // 计算AMC（Address Mapping Cache）地址，用于索引映射条目。
    Addr amc_address = structural_address / prefetchCandidatesPerEntry;
    // 计算映射条目内的索引。
    Addr map_index   = structural_address % prefetchCandidatesPerEntry;

    // 尝试在AMC中找到对应的条目。
    AddressMappingEntry *sp_entry =
        spAddressMappingCache.findEntry(amc_address, is_secure);

    // 如果找到了条目，则访问它以更新其访问频率。
    if (sp_entry != nullptr) {
        spAddressMappingCache.accessEntry(sp_entry);
    } else {
        // 如果未找到条目，尝试在AMC中找到一个空闲位置（受害者）。
        sp_entry = spAddressMappingCache.findVictim(amc_address);
        // 确保找到了一个空闲位置。
        assert(sp_entry != nullptr);

        // 在AMC中插入新的映射条目。
        spAddressMappingCache.insertEntry(amc_address, is_secure, sp_entry);
    }

    // 获取映射条目以更新。
    AddressMapping &mapping = sp_entry->mappings[map_index];
    // 更新物理地址。
    mapping.address = physical_address;
    // 重置并递增计数器，以跟踪该映射条目的访问频率。
    mapping.counter.reset();
    mapping.counter++;
}

} // namespace prefetch
} // namespace gem5
