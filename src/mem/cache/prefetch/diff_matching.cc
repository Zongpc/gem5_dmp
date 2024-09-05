#include "mem/cache/prefetch/diff_matching.hh"
#include "mem/cache/mshr.hh"
#include "mem/cache/base.hh"

#include "debug/HWPrefetch.hh"
#include "debug/DMP.hh"
#include "debug/POINTER.hh"
#include "params/DiffMatchingPrefetcher.hh"

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

// DiffMatching类的构造函数，用于初始化DiffMatching prefetcher。
// 参数p是一个DiffMatchingPrefetcherParams类型的对象，包含了许多配置参数。
DiffMatching::DiffMatching(const DiffMatchingPrefetcherParams &p)
    : Stride(p),
    iddt_ent_num(p.iddt_ent_num),
    tadt_ent_num(p.tadt_ent_num),
    iq_ent_num(p.iq_ent_num),
    rg_ent_num(p.rg_ent_num),
    ics_ent_num(p.ics_ent_num),
    rt_ent_num(p.rt_ent_num),
    range_ahead_dist_level_1(p.range_ahead_dist_level_1),//最后一层预取前瞻度
    range_ahead_dist_level_2(p.range_ahead_dist_level_2),//第一层预取前瞻度
    range_ahead_init_level_1(p.range_ahead_dist_level_1),//初始值
    range_ahead_init_level_2(p.range_ahead_dist_level_2),
    range_ahead_buffer_level_1(0),//一种置信度
    range_ahead_buffer_level_2(0),
    indir_range(p.indir_range),
    // upper_400ca0(0),
    replace_count_level_2(0),
    replace_threshold_level_2(p.replace_threshold_level_2),
    notify_latency(p.notify_latency),
    cur_range_priority(0),
    range_group_size(p.range_group_size),
    range_count(0),
    iddt_diff_num(p.iddt_diff_num),
    tadt_diff_num(p.tadt_diff_num),
    indexDataDeltaTable(p.iddt_ent_num, iddt_ent_t(p.iddt_diff_num, false)),
    targetAddrDeltaTable(p.tadt_ent_num, tadt_ent_t(p.tadt_diff_num, false)),
    iddt_ptr(0), tadt_ptr(0),
    range_unit_param(p.range_unit),
    range_level_param(p.range_level),
    rangeTable(p.rg_ent_num * 6, RangeTableEntry(p.range_unit, p.range_level, false)),
    rg_ptr(0),
    indexQueue(p.iq_ent_num),
    iq_ptr(0),
    indirectCandidateScoreboard(p.ics_ent_num, ICSEntry(p.ics_candidate_num, false)),
    ics_ptr(0),
    checkNewIndexEvent([this] { pickIndexPC(); }, this->name()),
    auto_detect(p.auto_detect),
    detect_period(p.detect_period),
    ics_miss_threshold(p.ics_miss_threshold),
    ics_candidate_num(p.ics_candidate_num),
    relationTable(p.rt_ent_num),
    rt_ptr(0),
    statsDMP(this),
    pf_helper(nullptr)
{
    /**
     * Priority Update Policy: 
     * new range-type priority = cur.priority - range_group_size
     * new single-type priority = parent_rte.priority + 1
     */

    // 初始化cur_range_priority
    cur_range_priority = std::numeric_limits<int32_t>::max();
    cur_range_priority -= cur_range_priority % range_group_size;

    // 如果不自动检测

    if (!p.auto_detect) {
        /**
         * Manual Mode
         */
        std::vector<Addr> pc_list;

        // 初始化IDDT
        if (!p.index_pc_init.empty()) {
            for (auto index_pc : p.index_pc_init) {
                indexDataDeltaTable[iddt_ptr].update(index_pc, 0, 0).validate();
                iddt_ptr++;
            }
            pc_list.insert(
                pc_list.end(), p.index_pc_init.begin(), p.index_pc_init.end()
            );
        }

        // 初始化TADT
        if (!p.target_pc_init.empty()) {
            for (auto target_pc : p.target_pc_init) {
                targetAddrDeltaTable[tadt_ptr].update(target_pc, 0, 0).validate();
                tadt_ptr++;
            }
            pc_list.insert(
                pc_list.end(), p.target_pc_init.begin(), p.target_pc_init.end()
            );
        }

        // 初始化RangeTable
        if (!p.range_pc_init.empty()) {
            for (auto range_pc : p.range_pc_init) {
                for (unsigned int shift_try: shift_v) {
                    rangeTable[rg_ptr].update(range_pc, 0x0, shift_try, 0).validate();
                    rg_ptr++;
                }
            }
        }
        relationTable[rt_ptr].update(
            0x401018,
            0x401050,
            0x7fb84b2058,
            5,
            1, 
            4, // TODO: dynamic detection
            0,
            true,
            false,
            2147479808
        ).validate();
        rt_ptr = (rt_ptr+1) % rt_ent_num;
        relationTable[rt_ptr].update(
            0x401050,
            0x401020,
            0x4,
            0,
            0, 
            4, // TODO: dynamic detection
            0,
            true,
            false,
            2147479809
        ).validate();
        rt_ptr = (rt_ptr+1) % rt_ent_num;
        relationTable[rt_ptr].update(
            0x401050,
            0x40102c,
            0x8,
            0,
            0, 
            4, // TODO: dynamic detection
            0,
            true,
            false,
            2147479809
        ).validate();
        rt_ptr = (rt_ptr+1) % rt_ent_num;
        relationTable[rt_ptr].update(
            0x401050,
            0x401044,
            0x10,
            0,
            0, 
            4, // TODO: dynamic detection
            0,
            true,
            false,
            2147479809
        ).validate();
        rt_ptr = (rt_ptr+1) % rt_ent_num;
        relationTable[rt_ptr].update(
            0x401050,
            0x401050,
            0x18,
            0,
            0, 
            4, // TODO: dynamic detection
            0,
            true,
            true,
            2147479809
        ).validate();
        rt_ptr = (rt_ptr+1) % rt_ent_num;
        // 对pc_list进行排序和去重
        std::sort( pc_list.begin(), pc_list.end() );
        pc_list.erase( std::unique( pc_list.begin(), pc_list.end() ), pc_list.end() );

        // 注册每个PC的统计信息
        statsDMP.regStatsPerPC(pc_list);
        dmp_stats_pc = pc_list;
    }
    // relationTable[rt_ptr].update(
    //     0x401018,
    //     0x401050,
    //     0x7fb84b2058,
    //     5,
    //     1, 
    //     4, // TODO: dynamic detection
    //     0,
    //     true,
    //     2147479808
    // ).validate();
    // rt_ptr = (rt_ptr+1) % rt_ent_num;
    // relationTable[rt_ptr].update(
    //     0x401050,
    //     0x401020,
    //     0x4,
    //     0,
    //     0, 
    //     4, // TODO: dynamic detection
    //     0,
    //     true,
    //     2147479809
    // ).validate();
    // rt_ptr = (rt_ptr+1) % rt_ent_num;
    // relationTable[rt_ptr].update(
    //     0x401050,
    //     0x40102c,
    //     0x8,
    //     0,
    //     0, 
    //     4, // TODO: dynamic detection
    //     0,
    //     true,
    //     2147479809
    // ).validate();
    // rt_ptr = (rt_ptr+1) % rt_ent_num;
    // relationTable[rt_ptr].update(
    //     0x401050,
    //     0x401044,
    //     0x10,
    //     0,
    //     0, 
    //     4, // TODO: dynamic detection
    //     0,
    //     true,
    //     2147479809
    // ).validate();
    // rt_ptr = (rt_ptr+1) % rt_ent_num;
    // relationTable[rt_ptr].update(
    //     0x401050,
    //     0x401050,
    //     0x18,
    //     0,
    //     0, 
    //     4, // TODO: dynamic detection
    //     0,
    //     true,
    //     2147479809
    // ).validate();
    // rt_ptr = (rt_ptr+1) % rt_ent_num;
}

DiffMatching::~DiffMatching()
{
}

// DiffMatching::DMPStats 类的构造函数
// 该构造函数初始化了与DMP（数据移动预测）相关的统计变量
DiffMatching::DMPStats::DMPStats(statistics::Group *parent)//加ADD_STAT
    : statistics::Group(parent),  // 初始化父类
    ADD_STAT(dmp_pfIdentified, statistics::units::Count::get(),
             "number of DMP prefetch candidates identified"),  // DMP预取候选总数
    ADD_STAT(dmp_pfIdentifiedPerPfPC, statistics::units::Count::get(),
             "number of DMP prefetch candidates identified per prefetch candidate"),  // 每个预取候选中识别的DMP预取候选数
    ADD_STAT(dmp_noValidDataPerPC, statistics::units::Count::get(),
             "number of prefetch candidates without valid data per prefetch candidate"),  // 每个预取候选中没有有效数据的预取候选数
    ADD_STAT(dmp_dataFill, statistics::units::Count::get(),
             "number of DMP prefetch candidates identified")  // DMP数据填充预取候选数
{
    using namespace statistics;
    
    int max_per_pc = 32;  // 每个预取候选的最大数量

    // 初始化每个预取候选中识别的DMP预取候选数的统计变量
    dmp_pfIdentifiedPerPfPC 
        .init(max_per_pc)
        .flags(total | nozero | nonan)  // 设置统计变量的标志：总数、非零、非NaN（不是一个数字）
        ;
    // 初始化每个预取候选中没有有效数据的预取候选数的统计变量
    dmp_noValidDataPerPC
        .init(max_per_pc)
        .flags(total | nozero | nonan)  // 设置统计变量的标志：总数、非零、非NaN
        ;
}

// 在DiffMatching::DMPStats类中，注册每个PC（程序计数器值）对应的统计信息。
// 该函数接收一个地址列表（stats_pc_list），并为列表中的每个地址注册相应的统计信息。
// 注意：函数内部使用断言确保地址列表的大小不会超过预设的最大值（max_per_pc）。
void DiffMatching::DMPStats::regStatsPerPC(const std::vector<Addr>& stats_pc_list)
{
    // 使用statistics命名空间，以简化后续统计相关操作的调用。
    using namespace statistics;

    // 定义每个PC地址统计信息的最大数量，以确保性能和内存使用量的可控。
    int max_per_pc = 32;
    // 确保传入的PC地址列表大小小于预设的最大值，以防止超出内存分配。
    assert(stats_pc_list.size() < max_per_pc);

    // 遍历PC地址列表，为每个地址注册统计信息。
    for (int i = 0; i < stats_pc_list.size(); i++) {
        // 使用stringstream将PC地址转换为十六进制字符串，以便用作统计信息的子名称。
        std::stringstream stream;
        stream << std::hex << stats_pc_list[i];
        std::string pc_name = stream.str();

        // 为当前PC地址注册“发出预取数量”和“无有效数据”两种统计信息。
        dmp_pfIdentifiedPerPfPC.subname(i, pc_name);
        dmp_noValidDataPerPC.subname(i, pc_name);
    }
}

// 选择一个索引进行处理
void DiffMatching::pickIndexPC()
{
    // 初始化当前最大权重为最小浮点数值
    float cur_weight = std::numeric_limits<float>::min();
    // 用于存储当前选择的队列条目指针
    IndexQueueEntry* choosed_ent = nullptr;

    // 遍历索引队列
    for (auto& iq_ent : indexQueue) {
        // 跳过无效的队列条目
        if (!iq_ent.valid) continue;
        
        // 获取当前队列条目的权重
        float try_weight = iq_ent.getWeight();
        // 如果当前权重大于已记录的最大权重，则更新最大权重和选择的条目
        if (try_weight > cur_weight) {
            cur_weight = try_weight;
            choosed_ent = &iq_ent;
        }
    }

    // 如果有有效的条目被选择
    if (choosed_ent != nullptr) {
        // 增加选择次数
        choosed_ent->tried++;
        // 将选择的索引插入到ICS（候选计分表）中
        insertICS(choosed_ent->index_pc, choosed_ent->cID);
        // 将选择的索引插入到IDDT（索引PC表）中
        insertIDDT(choosed_ent->index_pc, choosed_ent->cID);

        // 打印选择的索引信息
        DPRINTF(
            DMP, "pick for ICS: indexPC %llx cID %d\n", 
            choosed_ent->index_pc, choosed_ent->cID
        );
    }

    // 如果自动检测功能已启用，安排下一次检测
    if (auto_detect) {
        // 在当前时间加上检测周期后触发checkNewIndexEvent事件
        schedule(checkNewIndexEvent, curTick() + clockPeriod() * detect_period);
    }
}

// 更新匹配项或插入新的索引队列条目
//
// 该函数在差分匹配过程中被调用，用于处理匹配更新。
// 它首先尝试在索引队列中找到一个有效的、匹配的条目并增加其匹配计数，
// 如果找不到匹配的条目，则将目标PC和上下文ID作为新的条目插入索引队列。
//
// 参数:
// - index_pc_in: 当前指令的程序计数器值，用于查找匹配的索引队列条目。
// - target_pc_in: 目标指令的程序计数器值，将在未找到匹配项时插入索引队列。
// - cID_in: 上下文ID，与index_pc_in相关联，用于索引队列条目的匹配和插入。
void DiffMatching::matchUpdate(Addr index_pc_in, Addr target_pc_in, ContextID cID_in)
{
    // 遍历索引队列，寻找有效的、匹配的条目
    for (auto& iq_ent : indexQueue) {

        // 忽略无效的队列条目
        if (!iq_ent.valid) continue;

        // 如果找到一个有效的、匹配的条目，则增加其匹配计数，并结束搜索
        if (iq_ent.index_pc == index_pc_in && iq_ent.cID == cID_in) {
            iq_ent.matched++;
            break;
        }
    }

    // 将目标PC和上下文ID作为新的条目插入索引队列，这在未找到匹配项时执行
    insertIndexQueue(target_pc_in, cID_in);
}

bool DiffMatching::ICSEntry::updateMiss(Addr miss_pc, int miss_thred)
{
    // 尝试在miss_count中查找给定的miss_pc
    auto candidate = miss_count.find(miss_pc);

    // 如果找到了miss_pc
    if (candidate != miss_count.end()) {

        // 如果当前miss_pc的计数已经达到或超过了阈值
        if (candidate->second >= miss_thred) {
            // 表示这个miss_pc应该被发送到TADT，返回true
            return true;
        } else {
            // 否则，将miss_pc的计数加一
            candidate->second++;
        }

    } else if (miss_count.size() < candidate_num) {
        // 如果miss_count未满，添加一个新的候选miss_pc，并初始化计数为0
        miss_count.insert({miss_pc, 0});
    } 

    // 如果miss_pc未达到阈值或者miss_count已满，返回false
    return false;
}

// 通知差分匹配器在ICS（间接候选记分板）中出现未命中情况
void DiffMatching::notifyICSMiss(Addr miss_addr, Addr miss_pc_in, ContextID cID_in)
{
    // 遍历间接候选记分板的每个条目
    for (auto& ics_ent : indirectCandidateScoreboard) {
        // 如果条目无效，则跳过
        if (!ics_ent.valid) continue;

        // 如果条目所属的上下文ID与当前不匹配，则跳过
        if (ics_ent.cID != cID_in) continue;

        // 打印调试信息，表示ICS更新未命中
        DPRINTF(DMP, "ICS updateMiss: targetPC %llx Addr %llx cID %d\n", miss_pc_in, miss_addr, cID_in);

        // 如果更新ICS条目的未命中计数达到阈值，即视作候选
        if (ics_ent.updateMiss(miss_pc_in, ics_miss_threshold)) {
            // 尝试向TADT（目标PC表）和RangeTable（范围表）中插入新条目
            // 注意：IDDT（索引PC表）条目应由IndexQueue（索引队列）插入
            insertTADT(miss_pc_in, cID_in);
            insertRG(miss_addr, miss_pc_in, cID_in);

            // 打印调试信息，表示选择ICS条目
            DPRINTF(DMP, "ICS select: targetPC %llx cID %d\n", miss_pc_in, cID_in);

            // 返回，结束函数执行
            return;
        }
    }
}

void
DiffMatching::insertIndexQueue(Addr index_pc_in, ContextID cID_in)
{

    if ((index_pc_in & 0xffff800000000000) != 0)
    {
        return;
    }

    // check if already exist
    for (auto& iq_ent : indexQueue) {

        if (!iq_ent.valid) continue;

        if (iq_ent.index_pc == index_pc_in && iq_ent.cID == cID_in) return;
    }

    // insert to position iq_ptr
    indexQueue[iq_ptr].update(index_pc_in, cID_in).validate();
    iq_ptr = (iq_ptr + 1) % iq_ent_num;

    DPRINTF(DMP, "insert indexQueue: indexPC %llx cID %d\n", index_pc_in, cID_in);
}

void
DiffMatching::insertICS(Addr index_pc_in, ContextID cID_in)
{

    if ((index_pc_in & 0xffff800000000000) != 0)
    {
        return;
    }

    // check if already exist
    for (auto& ics_ent : indirectCandidateScoreboard) {

        if (!ics_ent.valid) continue;

        if (ics_ent.index_pc == index_pc_in && ics_ent.cID == cID_in) return;
    }

    // insert to position ics_ptr
    indirectCandidateScoreboard[ics_ptr].update(index_pc_in, cID_in).validate();
    ics_ptr = (ics_ptr + 1) % ics_ent_num;

    DPRINTF(DMP, "insert ICS: indexPC %llx cID %d\n", index_pc_in, cID_in);
}

void
DiffMatching::insertIDDT(Addr index_pc_in, ContextID cID_in)
{

    if ((index_pc_in & 0xffff800000000000) != 0) 
    {
        return;
    }

    // check if already exist
    for (auto& iddt_ent : indexDataDeltaTable) {

        if (!iddt_ent.isValid()) continue;

        if (iddt_ent.getPC() == index_pc_in && iddt_ent.getContextId() == cID_in) return;
    }

    // insert to position iddt_ptr
    indexDataDeltaTable[iddt_ptr].update(index_pc_in, cID_in).validate();
    iddt_ptr = (iddt_ptr + 1) % iddt_ent_num;

    DPRINTF(DMP, "insert IDDT: indexPC %llx cID %d\n", index_pc_in, cID_in);
}

void
DiffMatching::insertTADT(Addr target_pc_in, ContextID cID_in)
{

    if ((target_pc_in & 0xffff800000000000) != 0)
    {
        return;
    }

    // check if already exist
    for (auto& tadt_ent : targetAddrDeltaTable) {

        if (!tadt_ent.isValid()) continue;

        if (tadt_ent.getPC() == target_pc_in && tadt_ent.getContextId() == cID_in) return;
    }

    // insert to position tadt_ptr
    targetAddrDeltaTable[tadt_ptr].update(target_pc_in, cID_in).validate();
    tadt_ptr = (tadt_ptr + 1) % tadt_ent_num;

    DPRINTF(DMP, "insert TADT: targetPC %llx cID %d\n", target_pc_in, cID_in);
}

void
DiffMatching::insertRG(Addr req_addr_in, Addr target_pc_in, ContextID cID_in)
{

    if ((target_pc_in & 0xffff800000000000) != 0) 
    {
        return;
    }

    // check if already exist
    for (auto& rg_ent : rangeTable) {

        if (!rg_ent.valid) continue;

        if (rg_ent.target_pc == target_pc_in && rg_ent.cID == cID_in) return;
    }

    // insert 6 rangeTableRntry for different shift values
    for (auto shift_try : shift_v) {
        rangeTable[rg_ptr].update(
            target_pc_in, req_addr_in, shift_try, cID_in
        ).validate();
        rg_ptr = (rg_ptr+1) % (rg_ent_num * 6);
    }

    DPRINTF(DMP, "insert RG: targetPC %llx cID %d\n", target_pc_in, cID_in);
}

void
DiffMatching::diffMatching(tadt_ent_t& tadt_ent)
{
    // ready flag check 
    assert(tadt_ent.isValid() && tadt_ent.isReady());

    // if((tadt_ent.getPC() & 0x7fbf000000)!=0)
    //     return;
    ContextID tadt_ent_cID = tadt_ent.getContextId();

    // try to match all valid and ready index data diff-sequence 
    for (const auto& iddt_ent : indexDataDeltaTable) {
        if (!iddt_ent.isValid() || !iddt_ent.isReady()) continue;
        if (tadt_ent_cID != iddt_ent.getContextId()) continue;
        
        if(tadt_ent.isFinish())
            DPRINTF(DMP, "diffmatching finish target PC %llx index PC %llx\n", tadt_ent.getPC(), iddt_ent.getPC());
        else
            DPRINTF(DMP, "diffmatching target PC %llx index PC %llx\n", tadt_ent.getPC(), iddt_ent.getPC());
        
        // a specific index data diff-sequence may have multiple matching point  
        for (int i_start = 0; i_start < iddt_diff_num-tadt_diff_num+1; i_start++) {

            // try different shift values
            for (unsigned int shift_try: shift_v) {
                int t_start = 0;
                while (t_start < tadt_diff_num) {
                    if (iddt_ent[i_start+t_start] != (tadt_ent[t_start] >> shift_try)) {
                        break;
                    }
                    t_start++;
                    // DPRINTF(DMP, "diffmatching target PC %llx index PC %llx t_start %d\n", 
                    // tadt_ent.getPC(), iddt_ent.getPC(),t_start);
                }

                if (t_start == tadt_diff_num) {
                    // match success
                    // insert pattern to RelationTable
                    
                    insertRT(iddt_ent, tadt_ent, i_start+tadt_diff_num, shift_try, tadt_ent_cID);
                    
                    // match updata
                    // matchUpdate(iddt_ent.getPC(), tadt_ent.getPC(), tadt_ent.getContextId());
                }

            }
        }

    }
}

bool DiffMatching::findRTE(Addr index_pc, const tadt_ent_t& tadt_ent_match, ContextID cID)
{
    // 根据给定的指令地址、匹配的TADT条目和上下文ID，查找是否存在重复的RTE
    // 在relationTable中寻找是否存在与给定条件匹配的RTE
    // 如果找到匹配的RTE，则返回true；否则，返回false

    // 获取匹配的TADT条目的程序计数器（PC）地址
    Addr target_pc = tadt_ent_match.getPC();
    // 遍历relationTable中的每个RTE
    for (const auto& rte : relationTable)
    {
        // 如果RTE无效，则跳过当前循环
        if (!rte.valid) continue;

        // 检查是否存在环，即当前RTE的index_pc和目标PC与给定的index_pc和目标PC匹配，且上下文ID匹配
        // 如果满足条件，说明找到了一个有效的RTE，返回true
        if (rte.index_pc == target_pc && rte.target_pc == index_pc && rte.cID == cID) {
            return true;
        }

        // 检查是否存在相同目标PC和上下文ID的RTE，但索引PC不同
        // 如果存在，并且给定的index_pc与该RTE的index_pc相同，返回true
        // 如果给定的TADT条目表示一个完成的状态，则继续循环
        // 否则，返回true，表示找到了一个有效的RTE
        if (rte.target_pc == target_pc && rte.cID == cID) {
            if (rte.index_pc == index_pc)
                return true;
            if (tadt_ent_match.isFinish())
                continue;
            return true;
        }
    }
    // 如果遍历完relationTable后没有找到重复的RTE，返回false
    return false;
}

// 检查新的RTE是否会被现有的RTE预取
// 这个函数用于确定新加入的RTE条目是否与已存在的RTE条目有冗余，
// 即新RTE是否会被其他RTE预取到。
// 参数:
//   index_pc: 当前RTE的程序计数器索引
//   target_base_addr: 目标地址的基础地址
//   cID: 上下文标识符
// 返回值:
//   如果存在冗余的RTE，则返回true，否则返回false
bool DiffMatching::checkRedundantRTE(Addr index_pc, Addr target_base_addr, ContextID cID)
{
    // 计算块地址掩码，用于判断两个地址是否指向同一个缓存块
    Addr block_addr_mask = ~(Addr(blkSize - 1));

    // 遍历关系表中的所有RTE条目
    for (const auto& rte : relationTable) 
    {
        // 如果RTE无效，则跳过本次循环
        if (!rte.valid) continue;

        // 检查当前RTE条目的index_pc、target_base_addr和cID是否与传入的参数相匹配，
        // 并且目标地址是否指向相同的缓存块
        if ( rte.index_pc == index_pc && 
            ((rte.target_base_addr ^ target_base_addr) & block_addr_mask) == 0 &&
            rte.cID == cID ) {
            // 如果所有条件都满足，说明存在冗余的RTE，返回true
            return true;
        }
    }

    // 如果遍历结束没有找到冗余的RTE，返回false
    return false;
}

void
DiffMatching::insertRT(
    const iddt_ent_t& iddt_ent_match, 
    tadt_ent_t& tadt_ent_match,
    int iddt_match_point, unsigned int shift, ContextID cID)
{
    Addr new_index_pc = iddt_ent_match.getPC();
    Addr new_target_pc = tadt_ent_match.getPC();
    bool is_pointer_in=false;
    // check if pattern already exist
    if (findRTE(new_index_pc, tadt_ent_match, cID)) return;
    
    // match updata
    matchUpdate(iddt_ent_match.getPC(), tadt_ent_match.getPC(), tadt_ent_match.getContextId());
    

    if(tadt_ent_match.isPointer())
    {
        tadt_ent_match.update_finish();
        if(new_index_pc==new_target_pc)
            is_pointer_in=true;
    }
    
    // calculate the target base address
    IndexData data_match = iddt_ent_match.getLast();
    for (int i = iddt_match_point; i < iddt_diff_num; i++) {
        data_match -= iddt_ent_match[i];
    }
    
    TargetAddr addr_match = tadt_ent_match.getLast();

    
    int64_t base_addr_tmp = addr_match - (data_match << shift);

    DPRINTF(DMP, "indexPC %llx targetPC %llx Matched: LastData %llx Data %llx Addr %llx Shift %d\n", 
            new_index_pc, new_target_pc,iddt_ent_match.getLast(), data_match, addr_match, shift);
    
    // assert(base_addr_tmp <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()));
    Addr target_base_addr = static_cast<uint64_t>(base_addr_tmp);

    // if (checkRedundantRTE(new_index_pc, target_base_addr, cID)) return;

    /* get indexPC Range type */
    bool new_range_type;

    // Stride PC should be classified as Range
    // search for all requestor
    if(pf_helper) {
        new_range_type = pf_helper->checkStride(new_index_pc);
    } else {
        new_range_type = this->checkStride(new_index_pc);

        // tmp: only for test
        //new_range_type = true;
    }

    // try tadt's range detection
    if (!new_range_type) {

        // check rangeTable for range type
        for (auto range_ent : rangeTable) {
            if (range_ent.target_pc != new_index_pc || range_ent.cID != cID) continue;
    
            new_range_type = new_range_type || range_ent.getRangeType();
            
            if (new_range_type == true) break; 
        } 

    }

    /* get priority */
    int32_t priority = 0;
    if (new_range_type) {
        priority = cur_range_priority;
        cur_range_priority -= range_group_size;
        range_count++;

        for (auto& rt_ent : relationTable) {
            if (rt_ent.valid && rt_ent.index_pc == new_target_pc) {
                rt_ent.priority = priority + 1;
            }
        }
    } else {
        priority = getPriority(new_index_pc, cID) + 1;
        assert(priority % range_group_size > 0);
    }

    DPRINTF(DMP, "Insert RelationTable: "
        "indexPC %llx targetPC %llx target_addr %llx shift %d cID %d rangeType %d priority %d\n",
        new_index_pc, new_target_pc, target_base_addr, shift, cID, new_range_type, priority
    );

    relationTable[rt_ptr].update(
        new_index_pc,
        new_target_pc,
        target_base_addr,
        shift,
        new_range_type, 
        indir_range, // TODO: dynamic detection
        cID,
        true,
        is_pointer_in,
        priority
    ).validate();
    rt_ptr = (rt_ptr+1) % rt_ent_num;
}

/**
 * @brief 根据给定的目标程序计数器(PC)地址和上下文ID，获取优先级
 * 
 * 本函数通过搜索关系表(relationTable)，查找与给定目标PC地址和上下文ID匹配的条目。
 * 如果找到匹配条目，则返回该条目的优先级；如果没有找到，则返回0。
 * 
 * @param target_pc_in 目标程序计数器(PC)地址
 * @param cID_in 上下文ID，如果cID_in不为-1，则同时根据cID_in进行过滤
 * @return int32_t 匹配条目的优先级，如果没有匹配则返回0
 */
int32_t DiffMatching::getPriority(Addr target_pc_in, ContextID cID_in)
{
    // 初始化优先级为0，这是默认返回值
    int32_t priority = 0;

    // 遍历关系表中的每个条目
    for (auto& rt_ent : relationTable) {
        // 如果条目无效，则跳过当前循环，不过这段代码被注释掉了
        // if (!rt_ent.valid()) continue;

        // 如果当前条目的目标PC地址与输入的目标PC地址不匹配，则跳过当前循环
        if (rt_ent.target_pc != target_pc_in) continue;

        // 如果上下文ID不为-1且当前条目的上下文ID与输入的上下文ID不匹配，则跳过当前循环
        if (cID_in != -1 && rt_ent.cID != cID_in) continue;

        // 如果找到匹配的条目，则更新优先级并终止循环
        priority = rt_ent.priority;
        break;
    }

    // 返回计算得到的优先级
    return priority;
}

/**
 * @brief 根据给定的索引和上下文ID获取范围类型
 * 
 * 该函数通过查找关系表，寻找与给定索引和上下文ID匹配的条目。
 * 如果找到匹配项，则返回该匹配项的优先级；如果没有找到，则返回-1。
 * 
 * @param index_pc_in 索引PC（程序计数器）值，用于标识特定的指令或数据位置
 * @param cID_in 上下文ID，用于区分不同的上下文
 * @return int32_t 匹配项的优先级，如果没有匹配项则返回-1
 */
int32_t DiffMatching::getRangeType(Addr index_pc_in, ContextID cID_in) //xymc
{
    // 遍历关系表中的每个条目
    for (auto& rt_ent : relationTable) {
        
        // 检查条目的有效性，如果无效则跳过当前循环
        // if (!rt_ent.valid()) continue;

        // 检查当前条目的索引PC和上下文ID是否与输入参数匹配
        if (rt_ent.index_pc == index_pc_in && rt_ent.cID == cID_in) {
            // 如果匹配，则返回匹配项的优先级
            return rt_ent.priority;
        }
    }
    // 如果没有找到匹配项，则返回-1
    return -1;
}

bool
DiffMatching::RangeTableEntry::updateSample(Addr addr_in)
{
    // 检查连续性：确保输入地址与当前跟踪的地址序列保持一致性
    // assert(target_PC == PC_in);

    // 对输入地址进行位移操作，用于后续的重复性和连续性检查
    Addr addr_shifted = addr_in >> shift_times;

    // 检查重复性：如果输入地址与当前尾部地址相同，则不更新并返回false
    if (addr_shifted == cur_tail[0] || addr_shifted == cur_tail[1])
    {
        return false;
    } 

    // 检查连续性：如果输入地址是当前序列的下一地址，则增加计数并更新序列
    if (addr_shifted == cur_tail[0] + 1) {
        cur_count++;

        cur_tail[1] = cur_tail[0];
        cur_tail[0] = addr_shifted;

        return false;
    } 

    // 如果存在连续地址序列，则根据序列长度生成区间样本，并重置计数
    if (cur_count > 0) { //这里是为了统计各个范围段的样本数量
        // 根据当前序列长度选择合适的样本级别
        int sampled_level;
        if (cur_count >= range_quant_level * range_quant_unit) {
            sampled_level = range_quant_level;
        } else {
            sampled_level = (cur_count + range_quant_unit) / range_quant_unit;
        }
        // 更新对应级别的样本计数
        sample_count[sampled_level-1]++;
        cur_count = 0;
    }

    // 更新当前地址序列的尾部地址
    cur_tail[1] = cur_tail[0];
    cur_tail[0] = addr_shifted;

    // 成功更新返回true
    return true;
}

bool DiffMatching::RangeTableEntry::getRangeType() const {
    // 计算样本总数
    int sum = 0;
    for (int sample : sample_count) {
        sum += sample;
    }
    // 判断是否有样本大于0
    return (sum > 0);
}

bool DiffMatching::rangeFilter(Addr pc_in, Addr addr_in, ContextID cID_in)
{
    // 初始化返回值为true，表示默认情况下过滤条件满足
    bool ret = true;

    // 遍历范围表中的每个条目
    for (auto& range_ent : rangeTable) {
        
        // 如果当前条目无效，则跳过
        if (!range_ent.valid) continue;
        
        // 如果当前条目的目标PC和上下文ID与输入不匹配，则跳过
        if (range_ent.target_pc != pc_in || range_ent.cID != cID_in) continue;

        // 打印调试信息，显示当前样本的PC、地址和当前尾指针位置
        DPRINTF(DMP, "updateSample: pc %llx addr %llx cur_tail %llx\n", 
                    pc_in, addr_in, range_ent.cur_tail);
        
        // 调用当前条目的updateSample方法更新样本，并保存返回值
        bool update_ret = range_ent.updateSample(addr_in);
        
        // 更新返回值，确保所有相关条目的updateSample调用都成功
        ret = ret && update_ret;
    }

    // 返回最终的返回值
    return ret;
}

/**
 * @brief 判断请求地址是否满足偏移过滤条件
 * 
 * 本函数用于在差分匹配过程中判断请求地址是否与已有的条目值接近。如果请求地址与条目值的差值小于4096，
 * 则认为这是一个需要继续追踪的地址，更新指针追踪信息并返回false。否则，表示过滤不通过，返回true。
 * 
 * @param tadt_ent 差分匹配条目引用，用于存储和更新匹配过程中的信息
 * @param req_addr 请求地址，用于判断是否满足过滤条件
 * @return true表示请求地址不满足过滤条件，即与已有条目值相差较大
 *         false表示请求地址满足过滤条件，即与已有条目值接近
 */
bool DiffMatching::offsetFilter(tadt_ent_t& tadt_ent,Addr req_addr)
{
    // 如果差分匹配条目已经完成，则直接返回true，表示不再进行过滤
    if(tadt_ent.isFinish())
        return true;
    
    // 如果请求地址与差分匹配条目的值相差小于4096，则更新指针追踪信息并返回false
    if(req_addr-tadt_ent.getValue()<4096)
    {
        tadt_ent.update_pointer_chase();
        // 打印偏移过滤的相关信息，包括程序计数器值、请求地址和条目值
        DPRINTF(POINTER,"offsetfilter: pc %llx addr %llx value %llx\n",tadt_ent.getPC(),req_addr,tadt_ent.getValue());
        return false;
    }
    
    // 如果请求地址与条目值相差大于等于4096，表示过滤不通过，返回true
    return true;
}

// 处理 L1 请求通知，主要用于读请求，并更新差分匹配表
void DiffMatching::notifyL1Req(const PacketPtr &pkt) 
{  
    // 仅处理读请求
    if (!pkt->isRead()) return;

    // 更新 TADT 前检查 PC 和虚拟地址是否有效
    if (!pkt->req->hasPC() || !pkt->req->hasVaddr()) {
        return;
    }

    Addr req_addr = pkt->req->getVaddr();

    // 检查请求地址是否可能引起溢出
    if (req_addr > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) return;

    // 遍历目标地址差分表，寻找匹配项
    for (auto& tadt_ent: targetAddrDeltaTable) {

        Addr target_pc = tadt_ent.getPC();
        
        // 检查 TADT 项是否与当前请求的 PC 相匹配，且 TADT 项有效
        if (target_pc != pkt->req->getPC() || !tadt_ent.isValid()) continue;

        // 范围过滤，确保地址在有效范围内
        if (!rangeFilter(target_pc, req_addr, 
                        pkt->req->hasContextId() ? pkt->req->contextId() : 0))
            continue;

        // 偏移过滤，进一步精确匹配条件
        if(!offsetFilter(tadt_ent,req_addr))
            continue;

        // 打印过滤通过的请求信息
        DPRINTF(DMP, "notifyL1Req: [filter pass] PC %llx, cID %d, Addr %llx, PAddr %llx, VAddr %llx\n",
                            pkt->req->hasPC() ? pkt->req->getPC() : 0x0,
                            pkt->req->hasContextId() ? pkt->req->contextId() : 0,
                            pkt->getAddr(), 
                            pkt->req->getPaddr(), 
                            pkt->req->hasVaddr() ? pkt->req->getVaddr() : 0x0 );

        // 更新 TADT 项的相关信息
        tadt_ent.fill(
            static_cast<TargetAddr>(pkt->req->getVaddr()),
            pkt->req->hasContextId() ? pkt->req->contextId() : 0
        ); 

        // 如果 TADT 项准备就绪，则尝试进行差分匹配
        if (tadt_ent.isReady()) {
            DPRINTF(DMP, "try diffMatching for target PC: %llx\n", target_pc);
            diffMatching(tadt_ent);
        }
    }

    // 记录 L1 请求通知的日志信息
    DPRINTF(HWPrefetch, "notifyL1Req: PC %llx, Addr %llx, PAddr %llx, VAddr %llx\n",
                        pkt->req->hasPC() ? pkt->req->getPC() : 0x0,
                        pkt->getAddr(), 
                        pkt->req->getPaddr(), 
                        pkt->req->hasVaddr() ? pkt->req->getVaddr() : 0x0 );
}

// 处理 L1 响应通知
// 该函数主要负责处理与包（pkt）相关的逻辑，包括数据有效性的检查，
// 数据解析，以及更新 TADT（目标PC表）和 IDDT（索引PC表）。
void DiffMatching::notifyL1Resp(const PacketPtr &pkt) 
{
    // 如果包中不包含程序计数器（PC），则打印调试信息并返回
    if (!pkt->req->hasPC()) {
        DPRINTF(HWPrefetch, "notifyL1Resp: no PC\n");
        return;
    }
        
    // 如果包中的数据无效，则打印调试信息并返回
    if (!pkt->validData()) {
        DPRINTF(HWPrefetch, "notifyL1Resp: PC %llx, PAddr %llx, no Data, %s\n", 
                                pkt->req->getPC(), pkt->req->getPaddr(), pkt->cmdString());
        return;
    }

    // 假设响应数据的最大长度为 8 字节（int64）
    // 由于包只保留请求需要的数据，因此我们需要解析所有数据。
    const int data_stride = 8;
    const int byte_width = 8;

    // 如果包的大小超过 8 字节，则返回
    if (pkt->getSize() > 8) return; 
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
    if (resp_data > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) return;

    // 更新 TADT（目标PC表）以检测指针追查
    for (auto& tadt_ent: targetAddrDeltaTable) {

        Addr target_pc = tadt_ent.getPC();
        
        // 验证 TADT 条目是否有效
        if (target_pc != pkt->req->getPC() || !tadt_ent.isValid()) continue;
        // 如果 TADT 条目已完成，则跳过
        if(tadt_ent.isFinish())
            continue;
        IndexData new_data;
        std::memcpy(&new_data, &resp_data, sizeof(int64_t));
        // 更新 TADT 条目的最后值  xymc:这里是否要加个重复过滤器呢
        tadt_ent.update_last_value(new_data);
    }

    // 更新 IDDT（索引数据增量表）
    for (auto& iddt_ent: indexDataDeltaTable) {
        // 如果 IDDT 条目的 PC 与包中的 PC 匹配且条目有效
        if (iddt_ent.getPC() == pkt->req->getPC() && iddt_ent.isValid()) {

            IndexData new_data;
            std::memcpy(&new_data, &resp_data, sizeof(int64_t));

            // 如果新数据与上一次的数据相同，则跳过
            if (iddt_ent.getLast() == new_data) continue;

            // 填充 IDDT 条目
            iddt_ent.fill(new_data, pkt->req->hasContextId() ? pkt->req->contextId() : 0);
        }
    }
        
    // 旧代码段，用于处理特定程序计数器（PC）的响应数据
    // 目前已注释掉，未使用
    // if (pkt->req->getPC() == 0x400c80) {
    //     for (const auto& rt_ent : relationTable) {
    //         if (rt_ent.valid && rt_ent.index_pc == 0x400c7c) {
    //             upper_400ca0 = (resp_data << rt_ent.shift) + rt_ent.target_base_addr;
    //         }
    //     }
    // }

    // 打印处理过的响应数据的详细信息
    // DPRINTF(HWPrefetch, "notifyL1Resp: PC %llx, PAddr %llx, VAddr %llx, Size %d, Data %llx\n", 
    //                     pkt->req->getPC(), pkt->req->getPaddr(), 
    //                     pkt->req->hasVaddr() ? pkt->req->getVaddr() : 0x0,
    //                     pkt->getSize(), resp_data);
}

// DiffMatching::notifyFill 接收填充通知并处理
// 参数:
// - pkt: 包指针，包含请求和响应信息
// - data_ptr: 指向接收到的数据的指针
void DiffMatching::notifyFill(const PacketPtr &pkt, const uint8_t* data_ptr)
{
    // 使用虚拟地址进行预取
    assert(tlb != nullptr);

    // 检查包是否有PC（程序计数器）信息
    if (!pkt->req->hasPC()) {
        DPRINTF(HWPrefetch, "notifyFill: no PC\n");
        return;
    }

    // 检查包数据是否有效
    if (!pkt->validData()) {
        DPRINTF(HWPrefetch, "notifyFill: PC %llx, PAddr %llx, no Data, %s\n", 
                    pkt->req->getPC(), pkt->req->getPaddr(), pkt->cmdString());
        statsDMP.dmp_noValidData++;
        for (int i = 0; i < dmp_stats_pc.size(); i++) {
            Addr req_pc = pkt->req->getPC();
            if (req_pc == dmp_stats_pc[i]) {
                statsDMP.dmp_noValidDataPerPC[i]++;
                break;
            }
        }
        return;
    }

    // 获取响应数据
    uint8_t fill_data[blkSize];
    std::memcpy(fill_data, data_ptr, blkSize);

    // 打印响应数据（调试用）
    // if (debug::HWPrefetch) {
    //     unsigned data_offset_debug = pkt->req->getPaddr() & (blkSize-1);
    //     do {
    //     int64_t resp_data = (int64_t) ((uint64_t)fill_data[data_offset_debug]
    //                             + (((uint64_t)fill_data[data_offset_debug+1]) << 8)
    //                             + (((uint64_t)fill_data[data_offset_debug+2]) << 16)
    //                             + (((uint64_t)fill_data[data_offset_debug+3]) << 24));
    //     DPRINTF(HWPrefetch, "notifyFill: PC %llx, PAddr %llx, DataOffset %d, Data %llx\n", 
    //                     pkt->req->getPC(), pkt->req->getPaddr(), data_offset_debug, resp_data);
    //     data_offset_debug += 4;
    //     } while (data_offset_debug < blkSize);
    // }

    // 假设响应数据为整数，总是占用8字节
    const int data_stride = 8;
    const int byte_width = 8;

    // 获取PC和数据偏移量
    Addr pc = pkt->req->getPC();
    unsigned data_offset = pkt->req->getPaddr() & (blkSize-1);
    // 遍历关系表
    for (const auto& rt_ent: relationTable) { 

        // 跳过无效的表项
        if (!rt_ent.valid) continue;

        // 跳过与当前PC不匹配的表项
        if (rt_ent.index_pc != pc) continue;

        // 设置范围结束位置，如果不是范围类型，则只处理一个数据
        unsigned range_start;
        unsigned range_end;
        if (rt_ent.range) {
            // continue;
            // range_end = std::min(data_offset + data_stride * rt_ent.range_degree, blkSize);
            range_start = data_offset+data_stride * (rt_ent.range_degree-1);
            range_end = data_offset + data_stride * rt_ent.range_degree;
        } else {
            range_start = data_offset;
            range_end = data_offset + data_stride;
        }

        // 循环进行范围预取
        for (unsigned i_of = range_start; i_of < range_end; i_of += data_stride)
        {
            // 将fill_data[]整合到resp_data中（视为无符号数）
            uint64_t resp_data = 0;
            for (int i_st = data_stride-1; i_st >= 0; i_st--) {
                resp_data = resp_data << byte_width;
                resp_data += static_cast<uint64_t>(fill_data[i_of + i_st]);
            }

            // 计算目标预取地址
            Addr pf_addr = (resp_data << rt_ent.shift) + rt_ent.target_base_addr;
            DPRINTF(HWPrefetch, 
                    "notifyFill: IndexPC %llx, TargetPC %llx, PAddr %llx, pkt_addr %llx, pkt_offset %llx, pkt_data %llx, pf_addr %llx\n", 
                    pc, rt_ent.target_pc, pkt->req->getPaddr(), pkt->getAddr(), data_offset, resp_data, pf_addr);
            if(resp_data == 0)
                continue;
            // 插入到缺失翻译队列中
            insertIndirectPrefetch(pf_addr, rt_ent.target_pc, rt_ent.cID, rt_ent.priority, rt_ent.is_pointer);
            
            // if (rt_ent.target_pc == 0x400ca0) {
            //     for (int i = 1; i <= range_ahead_dist; i++) {
            //         insertIndirectPrefetch(pf_addr + blkSize * i, rt_ent.target_pc, rt_ent.cID, rt_ent.priority);
            //     }
            // }
        }

        // 尝试立即进行翻译
        processMissingTranslations(queueSize - pfq.size());
    }

    // 更新统计数据
    statsDMP.dmp_dataFill++;
}

// 插入间接预取请求到预取队列中
// pf_addr: 预取地址
// target_pc: 触发预取的程序计数器
// cID: 上下文ID
// priority: 预取的优先级
void DiffMatching::insertIndirectPrefetch(Addr pf_addr, Addr target_pc, ContextID cID, int32_t priority, bool is_pointer)
{
    // 获取块对齐的预取地址
    Addr blk_pf_addr = blockAddress(pf_addr);

    // 创建一个假的预取信息，用于链式触发，使用target_pc作为生成器PC
    // 使用块对齐地址用于重复检查
    PrefetchInfo fake_pfi(blk_pf_addr, target_pc, requestorId, cID, is_pointer);
    
    // 统计识别到的预取数量
    statsDMP.dmp_pfIdentified++;
    // 按PC地址统计识别到的预取数量
    for (int i = 0; i < dmp_stats_pc.size(); i++) {
        if (target_pc == dmp_stats_pc[i]) {
            statsDMP.dmp_pfIdentifiedPerPfPC[i]++;
            break;
        }
    }

    // 过滤重复请求
    if (queueFilter) {
        // 如果已经在队列中存在相同的预取地址，则返回
        if (alreadyInQueue(pfq, fake_pfi, priority)) {
            // DPRINTF(HWPrefetch, "xymc\n");
            return;
        }
        // 如果已经在缺失翻译队列中存在相同的预取地址，则返回
        if (alreadyInQueue(pfqMissingTranslation, fake_pfi, priority)) {
            return;
        }
    }

    // 为DPP创建包和请求，用于后续翻译
    DeferredPacket dpp(this, fake_pfi, 0, priority);

    // 设置目标PC和目标虚拟地址，进行块对齐
    dpp.pfInfo.setPC(target_pc);
    dpp.pfInfo.setAddr(blk_pf_addr);

    // TODO: 应设置ContextID

    // 创建用于翻译的请求，并设置PREFETCH标志
    RequestPtr translation_req = std::make_shared<Request>(
        pf_addr, blkSize, Request::PREFETCH, requestorId, 
        target_pc, cID);

    // 设置待翻译的请求，并添加到缺失翻译队列中
    dpp.setTranslationRequest(translation_req);
    dpp.tc = cache->system->threads[translation_req->contextId()];

    // pf_time将在翻译完成时设置

    addToQueue(pfqMissingTranslation, dpp);
}

// 当命中触发时，处理差异匹配
// 参数:
//   pc: 当前指令的程序计数器
//   addr: 访问的地址
//   data_ptr: 指向响应数据的指针
//   from_access: 表示是否来自访问的布尔值
void DiffMatching::hitTrigger(Addr pc, Addr addr, const uint8_t* data_ptr, bool from_access)
{
    // 获取响应数据
    uint8_t fill_data[blkSize];
    std::memcpy(fill_data, data_ptr, blkSize);

    // 假设响应数据是一个int型，总是占用4字节
    const int data_stride = 8;
    const int byte_width = 8;

    // 计算数据偏移量
    unsigned data_offset = addr & (blkSize-1);
    // 遍历关系表
    for (const auto& rt_ent: relationTable) { 

        // 如果关系表中的指令地址与当前pc不匹配，跳过
        if (rt_ent.index_pc != pc) continue;

        // 如果不是来自访问且有范围，则跳过
        if (!from_access && rt_ent.range) continue;

        /*
        // 设置范围结束位置，如果不是范围类型只处理一个数据
        unsigned range_end;
        if (rt_ent.range) {
            range_end = std::min(data_offset + data_stride * rt_ent.range_degree, blkSize);
        } else {
            range_end = data_offset + data_stride;
        }
        */

        /*
        // 对范围预取进行循环
        for (unsigned i_of = data_offset; i_of < range_end; i_of += data_stride)
        */

        // 将fill_data[]整合到resp_data中（视为无符号数）
        uint64_t resp_data = 0;
        for (int i_st = data_stride-1; i_st >= 0; i_st--) {
            resp_data = resp_data << byte_width;
            resp_data += static_cast<uint64_t>(fill_data[data_offset + i_st]);
        }

        // 计算目标预取地址
        Addr pf_addr = (resp_data << rt_ent.shift) + rt_ent.target_base_addr;
        DPRINTF(HWPrefetch, 
                "hitTrigger: IndexPC %llx, TargetPC %llx, Addr %llx, data_offset %llx, data %llx, pf_addr %llx\n", 
                pc, rt_ent.target_pc, addr, data_offset, resp_data, pf_addr);

        // 插入到缺失翻译队列中
        insertIndirectPrefetch(pf_addr, rt_ent.target_pc, rt_ent.cID, rt_ent.priority, rt_ent.is_pointer);
        
        /*
        // 如果目标pc为特定值，进行额外的预取地址插入
        if (rt_ent.target_pc == 0x400ca0) {
            for (int i = 1; i <= range_ahead_dist; i++) {
                insertIndirectPrefetch(pf_addr + blkSize * i, rt_ent.target_pc, rt_ent.cID, rt_ent.priority);
            }
        }
        */

        // 尝试立即进行翻译
        processMissingTranslations(queueSize - pfq.size());
    }
}

// 在差分匹配过程中，用于处理预测失效的钩子函数
// 该函数根据程序计数器（PC）地址计算一个优先级，并据此更新预测失效的缓冲级别
// 参数pc：当前指令的程序计数器地址
void DiffMatching::dmdCatchPfHook(Addr pc)
{
    // 获取当前PC地址的预测失效优先级，-1表示默认值
    Addr mshr_pf_prio = getPriority(pc, -1);

    // 如果预测失效优先级为0，则不进行任何操作并返回
    if (mshr_pf_prio == 0) return;

    // 计算处于哪个范围级别的预测失效，通过减去最大int值并除以预定义的组大小得到
    int range_level = 
        (std::numeric_limits<int32_t>::max() - mshr_pf_prio) / range_group_size;

    // 根据预测失效的范围级别，更新相应的预测失效缓冲区
    // 如果范围级别为0，则增加第1级缓冲区的计数
    if (range_level == 0) {
        range_ahead_buffer_level_1 += 1;
    } else {
        // 否则，增加第2级缓冲区的计数
        range_ahead_buffer_level_2 += 1;
    }
}

// 在DiffMatching类中，处理预取失效时的替换策略
void DiffMatching::pfReplaceHook(Addr pc)
{
    // 获取当前预取地址的优先级，-1表示不指定特定的MSHR（Multiple Source Hazard Register）
    Addr mshr_pf_prio = getPriority(pc, -1);

    // 如果优先级为0，表示当前预取不具有足够的优先级，直接返回
    if (mshr_pf_prio == 0) return;

    // 计算当前预取地址所属的范围级别，用于区分不同优先级的预取
    int range_level = 
        (std::numeric_limits<int32_t>::max() - mshr_pf_prio) / range_group_size;
    
    // 如果范围级别大于0，表示当前预取地址的优先级较高，需要进行相应的替换策略调整
    if (range_level > 0) {
        // 增加第二级别的替换计数
        replace_count_level_2++;

        // 如果第二级别的替换计数超过设定的阈值
        if (replace_count_level_2 > replace_threshold_level_2) {
            // 重置第二级别的预取距离计数器
            range_ahead_dist_level_2 = range_ahead_init_level_2;
            // 注释掉的代码原本用于重置第一级别的预取距离计数器，但在此场景下可能不需要
            // range_ahead_dist_level_1 = range_ahead_init_level_1;
        }
    }
}

// DiffMatching::notify - 通知差分匹配模块有请求命中或未命中缓存
// 参数:
//   pkt: 请求包的指针
//   pfi: 预取信息的引用
void DiffMatching::notify (const PacketPtr &pkt, const PrefetchInfo &pfi)
{
    // 检测是否为缓存未命中
    if (pfi.isCacheMiss()) {
        // 未命中情况
        DPRINTF(HWPrefetch, "notify::CacheMiss: PC %llx, Addr %llx, PAddr %llx, VAddr %llx\n", 
                            pkt->req->hasPC() ? pkt->req->getPC() : 0x0,
                            pkt->getAddr(), 
                            pkt->req->getPaddr(), 
                            pkt->req->hasVaddr() ? pkt->req->getVaddr() : 0x0);
        // 通知ICS未命中
        notifyICSMiss(
            pkt->req->hasVaddr() ? pkt->req->getVaddr() : 0x0,
            pkt->req->hasPC() ? pkt->req->getPC() : 0x0,
            pkt->req->hasContextId () ? pkt->req->contextId() : 0
        );        

    } else {
        // 命中情况
        DPRINTF(HWPrefetch, "notify::CacheHit: PC %llx, Addr %llx, PAddr %llx, VAddr %llx\n", 
                            pkt->req->hasPC() ? pkt->req->getPC() : 0x0,
                            pkt->getAddr(), 
                            pkt->req->getPaddr(), 
                            pkt->req->hasVaddr() ? pkt->req->getVaddr() : 0x0);
    }

    // 断言pkt确实代表一个请求
    assert(pkt->isRequest());

    // TODO: 是否应该对高级缓存预取进行进一步预取？
    // 例如，L1预取请求访问并命中L2。
    // 目前，L1硬件预取请求将在L2被转换为读共享请求。

    //if (pfi.isCacheMiss) { 
    // DMP仅观察DCache未命中（访问L2），意图减少分支预测未命中影响

    // if (!pkt->req->isPrefetch()) {

        // 在发送预取的缓存中再次测试，以防ppMiss->notify()来自其他位置。
        // 当此函数由ppHit->notify()调用时，我们使用缓存块数据进行预取。

        // 检查请求是否包含PC和上下文ID
        if (pkt->req->hasPC() && pkt->req->hasContextId()) {
            Addr pc = pkt->req->getPC();
            ContextID cid = pkt->req->contextId();
            // 计算访问优先级
            int32_t access_prio = getRangeType(pc, cid);

            // 检查是否达到新的范围级别
            if (access_prio % range_group_size == 0) {

                // 计算范围级别
                int range_level = 
                    (std::numeric_limits<int32_t>::max() - access_prio) / range_group_size;

                // 打印当前PC、访问优先级和范围级别
                DPRINTF(HWPrefetch, "pc %llx access_prio %d range_level : %d\n", pc, access_prio, range_level); 

                // 循环变量和距离变量
                int i,d;
                // d = 0;
                // 根据范围级别设置预取距离和缓冲区
                // if (range_level == 0) {
                //     i = range_ahead_dist_level_1;
                //     d = (range_ahead_buffer_level_1 > 0) ? 8 : 0;
                //     range_ahead_buffer_level_1 = 0;
                //     range_ahead_dist_level_1 += d;
                //     range_ahead_dist_level_1 = (range_ahead_dist_level_1 > 64) ? 64 : range_ahead_dist_level_1;

                //     // 重置高级别
                //     replace_count_level_2 = 0;
                // } else {
                //     i = range_ahead_dist_level_2;
                //     d = (range_ahead_buffer_level_2 > 0) ? 8 : 0;
                //     range_ahead_buffer_level_2 = 0;
                //     range_ahead_dist_level_2 += d;
                //     range_ahead_dist_level_2 = (range_ahead_dist_level_2 > 64) ? 64 : range_ahead_dist_level_2;

                //     // 重置零级别
                //     range_ahead_dist_level_1 = range_ahead_init_level_1;
                // }
                // int ahead_i=1;
                // 预取循环
                // DPRINTF(DMP, "pc %llx qianzhan %d range_level : %d rangecount %d\n", pc, i, range_level,range_count);
                i=32;
                d=8;
                for (int ahead =i; ahead <= i+d; ahead += 8) {
                    // 尝试获取缓存块
                    CacheBlk* try_cache_blk = cache->getCacheBlk(pkt->getAddr()+ahead, pkt->isSecure());
                    if (try_cache_blk != nullptr && try_cache_blk->data ) {
                        // notifyFill(pkt, try_cache_blk->data);
                        // 如果缓存块有效，则触发命中
                        Addr ahead_addr = pkt->req->getVaddr() + ahead;
                        hitTrigger(pc, ahead_addr, try_cache_blk->data, true);
                        // if (range_level == 0 && range_count > 1) {
                        //     // if (ahead_addr < upper_400ca0) {
                        //         hitTrigger(pc, ahead_addr, try_cache_blk->data, true);
                        //     // }
                        // } else {
                        //     hitTrigger(pc, ahead_addr, try_cache_blk->data, true);
                        // }

                    } else {
                        // 如果缓存块无效，则插入间接预取
                        insertIndirectPrefetch(
                            pkt->getAddr()+ahead, 
                            pc, cid,
                            getPriority(pc, cid),
                            false
                        );
                        // 处理缺失的翻译
                        processMissingTranslations(queueSize - pfq.size());
                    }
                }

                // assert(try_cache_blk && try_cache_blk->data);

            }
            else
            {
                CacheBlk* try_cache_blk = cache->getCacheBlk(pkt->getAddr(), pkt->isSecure());

                // assert(try_cache_blk && try_cache_blk->data);

                if (try_cache_blk != nullptr && try_cache_blk->data) {
                    DPRINTF(HWPrefetch, "Diaoyong: PC %llx, PAddr %llx\n", 
                    pkt->req->getPC(), pkt->req->getPaddr());
                    notifyFill(pkt, try_cache_blk->data);
                }
            }
        }
    
    //}
    // 如果是未命中且不在未命中队列中，则通知队列
    // if (pfi.isCacheMiss() && !(cache->inMissQueue(pkt->getAddr(), pkt->isSecure()))) {
        // 通知队列
        Queued::notify(pkt, pfi);
    // }
}

// 函数: callReadytoIssue
// 功能: 调用准备就绪以发布事件，根据预取信息判断是否将指令加入索引队列。
// 参数:
//   pfi - 预取信息引用，包含PC（程序计数器）和其他预取相关数据。

void DiffMatching::callReadytoIssue(const PrefetchInfo& pfi)
{
    // 获取预取信息中的程序计数器值。
    Addr pc = pfi.getPC();

    // 掩码内核空间地址，检查PC是否属于用户空间。
    // 只有当PC处于用户空间时才插入索引队列。
    if ((pc & 0xffff800000000000) == 0)
    {
        // 将PC和预取信息中的核心ID插入到索引队列中。
        insertIndexQueue(pc, pfi.getcID());
    }

    // 当自动检测模式开启并且没有已安排的新索引检查事件时。
    if (auto_detect && !checkNewIndexEvent.scheduled())
    {
        // 安排下一个新索引检查事件。
        // 事件将在当前时钟周期加上检测周期的时钟周期后触发。
        schedule(checkNewIndexEvent, curTick() + clockPeriod() * detect_period);
    }
}

/**
 * 注册一个步进辅助对象到DiffMatching类中。
 * 
 * 该函数用于将一个步进辅助对象（Stride*类型的指针）注册到DiffMatching类的实例中。
 * 它确保了只能注册一个步进辅助对象，如果尝试注册第二个，将会触发断言。
 * 
 * @param s 指向要注册的步进辅助对象的指针。
 */
void DiffMatching::addPfHelper(Stride* s)
{
    // 检查是否已经存在pf_helper，如果存在则抛出异常，确保只能添加一个PfHelper
    fatal_if(pf_helper != nullptr, "Only one PfHelper can be registered");
    pf_helper = s;
}

// 在DiffMatching类中，计算预取地址并设置优先级
void DiffMatching::calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses) 
{
    // 如果pf_helper存在，则使用虚拟地址来丢弃跨步预取，同时继续更新pcTables
    if (pf_helper) {
        std::vector<AddrPriority> fake_addresses;

        // 调用Stride类的calculatePrefetch方法，将结果存储在fake_addresses中
        Stride::calculatePrefetch(pfi, fake_addresses);
    } else {
        // 如果pf_helper不存在，直接调用Stride类的calculatePrefetch方法，将结果存储在addresses中
        Stride::calculatePrefetch(pfi, addresses);
    }

    // 为跨步预取设置优先级，以防跨步pc与rt_ent的目标pc相同
    int32_t priority = 0;
    // 如果pfi包含PC，则根据PC计算优先级
    if (pfi.hasPC()) {
        priority = getPriority(pfi.getPC(), -1);
    }

    // 遍历addresses，设置每个地址的优先级
    for (auto& addr : addresses) {
        addr.second = priority;
    }
}

} // namespace prefetch

} // namespace gem5

