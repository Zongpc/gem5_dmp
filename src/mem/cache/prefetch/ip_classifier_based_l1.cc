// ipclassifier_based_l1.cc
// Created by LuoZhongpei

#include "mem/cache/prefetch/ip_classifier_based_l1.hh"

#include "mem/cache/base.hh"
#include "mem/cache/prefetch/associative_set_impl.hh"
#include "params/IPClassifierBasedL1Prefetcher.hh"

#define LOG2_PAGE_SIZE 64
#define LOG2_BLOCK_SIZE 6

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);

namespace prefetch
{


IPClassifierBasedL1::IPClassifierBasedL1(const IPClassifierBasedL1PrefetcherParams &p) : Queued(p)
{
    if (!prefetchOnAccess) panic("IPCP must be told on every cache access!");
}


//void CACHE::l1d_prefetcher_operate(uint64_t addr, uint64_t ip, uint8_t cache_hit, uint8_t type)
void
IPClassifierBasedL1::calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses)
{
    if (!pfi.hasPC()) { return; }
    uint64_t curr_page = pfi.getAddr() >> LOG2_PAGE_SIZE;
    uint64_t cl_addr = pfi.getAddr() >> LOG2_BLOCK_SIZE;
    uint64_t cl_offset = (pfi.getAddr() >> LOG2_BLOCK_SIZE) & 0x3F;
    uint16_t signature = 0, last_signature = 0;
    int prefetch_degree = 0;
    int spec_nl_threshold = 0;
    int num_prefs = 0;
    uint32_t metadata=0;
    uint16_t ip_tag = (pfi.getPC() >> NUM_IP_INDEX_BITS) & ((1 << NUM_IP_TAG_BITS)-1);

// if(NUM_CPUS == 1){
//     prefetch_degree = 3;
//     spec_nl_threshold = 15; 
// } else {                                    // tightening the degree and MPKC constraints for multi-core
//     prefetch_degree = 2;
//     spec_nl_threshold = 5;
// }
    prefetch_degree = 3;
    spec_nl_threshold = 15; 


// update miss counter
if(pfi.isCacheMiss())
    num_misses += 1;

// update spec nl bit when num misses crosses certain threshold
// if(num_misses == 256){
//     mpkc = ((float) num_misses/(current_core_cycle[cpu]-prev_cpu_cycle))*1000;
//     prev_cpu_cycle = current_core_cycle[cpu];
//     if(mpkc > spec_nl_threshold)
//         spec_nl = 0;
//     else
//         spec_nl = 1;
//     num_misses = 0;
// }

// calculate the index bit
    int index = pfi.getPC() & ((1 << NUM_IP_INDEX_BITS)-1);
    if(trackers_l1[index].ip_tag != ip_tag){               // new/conflict IP
        if(trackers_l1[index].ip_valid == 0){              // if valid bit is zero, update with latest IP info
        trackers_l1[index].ip_tag = ip_tag;
        trackers_l1[index].last_page = curr_page;
        trackers_l1[index].last_cl_offset = cl_offset;
        trackers_l1[index].last_stride = 0;
        trackers_l1[index].signature = 0;
        trackers_l1[index].conf = 0;
        trackers_l1[index].str_valid = 0;
        trackers_l1[index].str_strength = 0;
        trackers_l1[index].str_dir = 0;
        trackers_l1[index].ip_valid = 1;
    } else {                                                    // otherwise, reset valid bit and leave the previous IP as it is
        trackers_l1[index].ip_valid = 0;
    }

    // issue a next line prefetch upon encountering new IP
        uint64_t pf_address = ((pfi.getAddr()>>LOG2_BLOCK_SIZE)+1) << LOG2_BLOCK_SIZE; // BASE NL=1, changing it to 3
        metadata = encode_metadata(1, NL_TYPE, spec_nl);
        //prefetch_line(pfi.getPC(), pfi.getAddr(), pf_address, FILL_L1, metadata);
        addresses.push_back(AddrPriority(pf_address, 0));
        return;
    }
    else {                                                     // if same IP encountered, set valid bit
        trackers_l1[index].ip_valid = 1;
    }
    

    // calculate the stride between the current address and the last address
    int64_t stride = 0;
    // if (cl_offset > trackers_l1[index].last_cl_offset)
    //     stride = cl_offset - trackers_l1[index].last_cl_offset;
    // else {
    //     stride = trackers_l1[index].last_cl_offset - cl_offset;
    //     stride *= -1;
    // }
    stride = cl_offset - trackers_l1[index].last_cl_offset;

    // don't do anything if same address is seen twice in a row
    if (stride == 0)
        return;


// page boundary learning
if(curr_page != trackers_l1[index].last_page){
    if(stride < 0)
        stride += 64;
    else
        stride -= 64;
}

// update constant stride(CS) confidence
trackers_l1[index].conf = update_conf(stride, trackers_l1[index].last_stride, trackers_l1[index].conf);

// update CS only if confidence is zero
if(trackers_l1[index].conf == 0)                      
    trackers_l1[index].last_stride = stride;

last_signature = trackers_l1[index].signature;
// update complex stride(CPLX) confidence
DPT_l1[last_signature].conf = update_conf(stride, DPT_l1[last_signature].delta, DPT_l1[last_signature].conf);

// update CPLX only if confidence is zero
if(DPT_l1[last_signature].conf == 0)
    DPT_l1[last_signature].delta = stride;

// calculate and update new signature in IP table
signature = update_sig_l1(last_signature, stride);
trackers_l1[index].signature = signature;

// check GHB for stream IP
check_for_stream_l1(index, cl_addr);           

// SIG_DP(
// cout << pfi.getPC() << ", " << !pfi.isCacheMiss() << ", " << cl_addr << ", " << pfi.getAddr() << ", " << stride << "; ";
// cout << last_signature<< ", "  << DPT_l1[last_signature].delta<< ", "  << DPT_l1[last_signature].conf << "; ";
// cout << trackers_l1[index].last_stride << ", " << stride << ", " << trackers_l1[index].conf << ", " << "; ";
// );

    if(trackers_l1[index].str_valid == 1){                         // stream IP
        // for stream, prefetch with twice the usual degree
            prefetch_degree = prefetch_degree*2;
        for (int i=0; i<prefetch_degree; i++) {
            uint64_t pf_address = 0;

            if(trackers_l1[index].str_dir == 1){                   // +ve stream
                pf_address = (cl_addr + i + 1) << LOG2_BLOCK_SIZE;
                metadata = encode_metadata(1, S_TYPE, spec_nl);    // stride is 1
            }
            else{                                                       // -ve stream
                pf_address = (cl_addr - i - 1) << LOG2_BLOCK_SIZE;
                metadata = encode_metadata(-1, S_TYPE, spec_nl);   // stride is -1
            }

            // Check if prefetch address is in same 4 KB page
            if ((pf_address >> LOG2_PAGE_SIZE) != (pfi.getAddr() >> LOG2_PAGE_SIZE)){
                break;
            }

            //prefetch_line(pfi.getPC(), pfi.getAddr(), pf_address, FILL_L1, metadata);
            addresses.push_back(AddrPriority(pf_address, 0));
            num_prefs++;
            // SIG_DP(cout << "1, ");
            }

    } else if(trackers_l1[index].conf > 1 && trackers_l1[index].last_stride != 0){            // CS IP  
        for (int i=0; i<prefetch_degree; i++) {
            uint64_t pf_address = (cl_addr + (trackers_l1[index].last_stride*(i+1))) << LOG2_BLOCK_SIZE;

            // Check if prefetch address is in same 4 KB page
            if ((pf_address >> LOG2_PAGE_SIZE) != (pfi.getAddr() >> LOG2_PAGE_SIZE)){
                break;
            }

            metadata = encode_metadata(trackers_l1[index].last_stride, CS_TYPE, spec_nl);
            //prefetch_line(pfi.getPC(), pfi.getAddr(), pf_address, FILL_L1, metadata);
            addresses.push_back(AddrPriority(pf_address, 0));

            num_prefs++;
            // SIG_DP(cout << trackers_l1[index].last_stride << ", ");
        }
    } else if(DPT_l1[signature].conf >= 0 && DPT_l1[signature].delta != 0) {  // if conf>=0, continue looking for delta
        int pref_offset = 0,i=0;                                                        // CPLX IP
        for (i=0; i<prefetch_degree; i++) {
            pref_offset += DPT_l1[signature].delta;
            uint64_t pf_address = ((cl_addr + pref_offset) << LOG2_BLOCK_SIZE);

            // Check if prefetch address is in same 4 KB page
            if (((pf_address >> LOG2_PAGE_SIZE) != (pfi.getAddr() >> LOG2_PAGE_SIZE)) || 
                    (DPT_l1[signature].conf == -1) ||
                    (DPT_l1[signature].delta == 0)){
                // if new entry in DPT or delta is zero, break
                break;
            }

            // we are not prefetching at L2 for CPLX type, so encode delta as 0
            metadata = encode_metadata(0, CPLX_TYPE, spec_nl);
            if(DPT_l1[signature].conf > 0){                                 // prefetch only when conf>0 for CPLX
                //prefetch_line(pfi.getPC(), pfi.getAddr(), pf_address, FILL_L1, metadata);
                addresses.push_back(AddrPriority(pf_address, 0));
                num_prefs++;
                // SIG_DP(cout << pref_offset << ", ");
            }
            signature = update_sig_l1(signature, DPT_l1[signature].delta);
        }
    } 

// if no prefetches are issued till now, speculatively issue a next_line prefetch
// if(num_prefs == 0 && spec_nl == 1){                                        // NL IP
//     uint64_t pf_address = ((addr>>LOG2_BLOCK_SIZE)+1) << LOG2_BLOCK_SIZE;  
//     metadata = encode_metadata(1, NL_TYPE, spec_nl);
//     prefetch_line(ip, addr, pf_address, FILL_L1, metadata);
//     SIG_DP(cout << "1, ");
// }

// SIG_DP(cout << endl);

// update the IP table entries
trackers_l1[index].last_cl_offset = cl_offset;
trackers_l1[index].last_page = curr_page;

// update GHB
// search for matching cl addr
int ghb_index=0;
for(ghb_index = 0; ghb_index < NUM_GHB_ENTRIES; ghb_index++)
    if(cl_addr == ghb_l1[ghb_index])
        break;
// only update the GHB upon finding a new cl address
if(ghb_index == NUM_GHB_ENTRIES){
for(ghb_index=NUM_GHB_ENTRIES-1; ghb_index>0; ghb_index--)
    ghb_l1[ghb_index] = ghb_l1[ghb_index-1];
ghb_l1[0] = cl_addr;
}

return;
}

} // namespace prefetch

} // namespace gem5

