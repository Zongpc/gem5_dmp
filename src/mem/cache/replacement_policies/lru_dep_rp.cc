#include "mem/cache/replacement_policies/lru_dep_rp.hh"
#include "mem/cache/cache_blk.hh"
#include "sim/cur_tick.hh"

#include <cassert>

#include "params/LRUDEPRP.hh"

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(ReplacementPolicy, replacement_policy);
namespace replacement_policy
{

LRUDEP::LRUDEP(const LRUDEPRPParams &p) 
  : LRU(p), slope(p.slope), bias(p.bias), pf_gap(p.pf_gap)
{
}

ReplaceableEntry*
LRUDEP::getVictim(const ReplacementCandidates& candidates) const
{
    assert(candidates.size() > 0);

    // // Visit all candidates to find victim
    // ReplaceableEntry* victim = candidates[0];
    // int64_t victim_value = std::static_pointer_cast<LRUReplData>(
    //         victim->replacementData)->lastTouchTick;
    // CacheBlk* victim_blk = dynamic_cast<CacheBlk*>(victim);
    // if (victim_blk && victim_blk->wasPrefetched()) {
    //     victim_value = bias + static_cast<int64_t>((curTick() - victim_value) * slope);
    // }

    // for (const auto& candidate : candidates) {
    //     int64_t candidate_value = std::static_pointer_cast<LRUReplData>(
    //             candidate->replacementData)->lastTouchTick;
    //     CacheBlk* candidate_blk = dynamic_cast<CacheBlk*>(candidate);
    //     if (candidate_blk && candidate_blk->wasPrefetched()) {
    //         candidate_value = bias + static_cast<int64_t>((curTick() - candidate_value) * slope);
    //     }

    //     // Update victim entry if necessary
    //     if (candidate_value < victim_value) {
    //         victim = candidate;
    //     }
    // }

    // // Visit all candidates to find victim
    ReplaceableEntry* victim = candidates[0];
    int64_t victim_value = std::static_pointer_cast<LRUReplData>(
            victim->replacementData)->lastTouchTick;
    CacheBlk* victim_blk = dynamic_cast<CacheBlk*>(victim);
    if (victim_blk && victim_blk->wasPrefetched()) {
        if (victim_value + pf_gap > curTick()) {
            victim_value = 2*(curTick() - pf_gap) - victim_value;
        }
    }

    for (const auto& candidate : candidates) {
        int64_t candidate_value = std::static_pointer_cast<LRUReplData>(
                candidate->replacementData)->lastTouchTick;
        CacheBlk* candidate_blk = dynamic_cast<CacheBlk*>(candidate);
        if (candidate_blk && candidate_blk->wasPrefetched()) {
            if (candidate_value + pf_gap > curTick()) {
                candidate_value = 2*(curTick() - pf_gap) - candidate_value;
            }
        }

        // Update victim entry if necessary
        if (candidate_value < victim_value) {
            victim = candidate;
        } else if (candidate_value == victim_value) {
            if (std::static_pointer_cast<LRUReplData>(
                        candidate->replacementData)->lastTouchTick >
                    std::static_pointer_cast<LRUReplData>(
                        victim->replacementData)->lastTouchTick) {
                victim = candidate;
            }
        }
    }

    // Visit all candidates to find victim
    // ReplaceableEntry* victim = candidates[0];
    // for (const auto& candidate : candidates) {
    //     // Update victim entry if necessary
    //     if (std::static_pointer_cast<LRUReplData>(
    //                 candidate->replacementData)->lastTouchTick <
    //             std::static_pointer_cast<LRUReplData>(
    //                 victim->replacementData)->lastTouchTick) {
    //         victim = candidate;
    //     }

    //     CacheBlk* candidate_blk = dynamic_cast<CacheBlk*>(candidate);
    //     if (candidate_blk && (!candidate_blk->wasPrefetched())) {
    //         Addr cand_pc = candidate_blk->getPC();
    //         if (cand_pc == 0x400ca0) {
    //             victim = candidate;
    //         }
    //     }
    // }

    return victim;
}


} // namespace replacement_policy

} // namespace gem5