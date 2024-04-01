#ifndef __MEM_CACHE_REPLACEMENT_POLICIES_LRU_RP_DEP_HH__
#define __MEM_CACHE_REPLACEMENT_POLICIES_LRU_RP_DEP_HH__

#include <memory>
#include <cassert>

#include "mem/cache/replacement_policies/lru_rp.hh"

namespace gem5
{

struct LRUDEPRPParams;

GEM5_DEPRECATED_NAMESPACE(ReplacementPolicy, replacement_policy);
namespace replacement_policy
{

class LRUDEP : public LRU
{
    float slope;
    int bias;
    Tick pf_gap;
  public:  
    LRUDEP(const LRUDEPRPParams &p);
    ~LRUDEP() = default;

    ReplaceableEntry* getVictim(const ReplacementCandidates& candidates) 
                                                                const override;
};

} // namespace replacement_policy
    
} // namespace gem5

#endif // __MEM_CACHE_REPLACEMENT_POLICIES_LRU_RP_DEP_HH__ 