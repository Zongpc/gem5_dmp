/*
 * Copyright (c) 2012-2019, 2021 ARM Limited
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
 * Copyright (c) 2006 The Regents of The University of Michigan
 * Copyright (c) 2010,2015 Advanced Micro Devices, Inc.
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
 * Declaration of the Packet class.
 */

#ifndef __MEM_PACKET_HH__
#define __MEM_PACKET_HH__

#include <bitset>
#include <cassert>
#include <initializer_list>
#include <list>

#include "base/addr_range.hh"
#include "base/cast.hh"
#include "base/compiler.hh"
#include "base/flags.hh"
#include "base/logging.hh"
#include "base/printable.hh"
#include "base/types.hh"
#include "mem/htm.hh"
#include "mem/request.hh"
#include "sim/byteswap.hh"

namespace gem5
{

class Packet;
typedef Packet *PacketPtr;
typedef uint8_t* PacketDataPtr;
typedef std::list<PacketPtr> PacketList;
typedef uint64_t PacketId;

class MemCmd
{
    friend class Packet;

    public:
    /**
     * List of all commands associated with a packet.
     */
    enum Command
    {
        InvalidCmd,
        ReadReq,
        ReadResp,
        ReadRespWithInvalidate,
        WriteReq,
        WriteResp,
        WriteCompleteResp,
        WritebackDirty,
        WritebackClean,
        WriteClean,            // writes dirty data below without evicting
        CleanEvict,
        SoftPFReq,
        SoftPFExReq,
        HardPFReq,
        SoftPFResp,
        HardPFResp,
        WriteLineReq,
        UpgradeReq,
        SCUpgradeReq,           // Special "weak" upgrade for StoreCond
        UpgradeResp,
        SCUpgradeFailReq,       // Failed SCUpgradeReq in MSHR (never sent)
        UpgradeFailResp,        // Valid for SCUpgradeReq only
        ReadExReq,
        ReadExResp,
        ReadCleanReq,
        ReadSharedReq,
        LoadLockedReq,
        StoreCondReq,
        StoreCondFailReq,       // Failed StoreCondReq in MSHR (never sent)
        StoreCondResp,
        LockedRMWReadReq,
        LockedRMWReadResp,
        LockedRMWWriteReq,
        LockedRMWWriteResp,
        SwapReq,
        SwapResp,
        // MessageReq and MessageResp are deprecated.
        MemFenceReq = SwapResp + 3,
        MemSyncReq,  // memory synchronization request (e.g., cache invalidate)
        MemSyncResp, // memory synchronization response
        MemFenceResp,
        CleanSharedReq,
        CleanSharedResp,
        CleanInvalidReq,
        CleanInvalidResp,
        // Error responses
        // @TODO these should be classified as responses rather than
        // requests; coding them as requests initially for backwards
        // compatibility
        InvalidDestError,  // packet dest field invalid
        BadAddressError,   // memory address invalid
        ReadError,         // packet dest unable to fulfill read command
        WriteError,        // packet dest unable to fulfill write command
        FunctionalReadError, // unable to fulfill functional read
        FunctionalWriteError, // unable to fulfill functional write
        // Fake simulator-only commands
        PrintReq,       // Print state matching address
        FlushReq,      //request for a cache flush
        InvalidateReq,   // request for address to be invalidated
        InvalidateResp,
        // hardware transactional memory
        HTMReq,
        HTMReqResp,
        HTMAbort,
        // Tlb shootdown
        TlbiExtSync,
        StorePFTrain,
        NUM_MEM_CMDS
    };

    private:
    /**
     * List of command attributes.
     */
    enum Attribute
    {
        IsRead,         //!< Data flows from responder to requester
        IsWrite,        //!< Data flows from requester to responder
        IsUpgrade,
        IsInvalidate,
        IsClean,        //!< Cleans any existing dirty blocks
        NeedsWritable,  //!< Requires writable copy to complete in-cache
        IsRequest,      //!< Issued by requester
        IsResponse,     //!< Issue by responder
        NeedsResponse,  //!< Requester needs response from target
        IsEviction,
        IsSWPrefetch,
        IsHWPrefetch,
        IsLlsc,         //!< Alpha/MIPS LL or SC access
        IsLockedRMW,    //!< x86 locked RMW access
        HasData,        //!< There is an associated payload
        IsError,        //!< Error response
        IsPrint,        //!< Print state matching address (for debugging)
        IsFlush,        //!< Flush the address from caches
        FromCache,      //!< Request originated from a caching agent
        NUM_COMMAND_ATTRIBUTES
    };

    static constexpr unsigned long long
    buildAttributes(std::initializer_list<Attribute> attrs)
    {
        unsigned long long ret = 0;
        for (const auto &attr: attrs)
            ret |= (1ULL << attr);
        return ret;
    }

    /**
     * Structure that defines attributes and other data associated
     * with a Command.
     */
    struct CommandInfo
    {
        /// Set of attribute flags.
        const std::bitset<NUM_COMMAND_ATTRIBUTES> attributes;
        /// Corresponding response for requests; InvalidCmd if no
        /// response is applicable.
        const Command response;
        /// String representation (for printing)
        const std::string str;

        CommandInfo(std::initializer_list<Attribute> attrs,
                Command _response, const std::string &_str) :
            attributes(buildAttributes(attrs)), response(_response), str(_str)
        {}
    };

    /// Array to map Command enum to associated info.
    static const CommandInfo commandInfo[];

    private:

    Command cmd;

    bool
    testCmdAttrib(MemCmd::Attribute attrib) const
    {
        return commandInfo[cmd].attributes[attrib] != 0;
    }

    public:

    bool isRead() const            { return testCmdAttrib(IsRead); }
    bool isWrite() const           { return testCmdAttrib(IsWrite); }
    bool isUpgrade() const         { return testCmdAttrib(IsUpgrade); }
    bool isRequest() const         { return testCmdAttrib(IsRequest); }
    bool isResponse() const        { return testCmdAttrib(IsResponse); }
    bool needsWritable() const     { return testCmdAttrib(NeedsWritable); }
    bool needsResponse() const     { return testCmdAttrib(NeedsResponse); }
    bool isInvalidate() const      { return testCmdAttrib(IsInvalidate); }
    bool isEviction() const        { return testCmdAttrib(IsEviction); }
    bool isClean() const           { return testCmdAttrib(IsClean); }
    bool fromCache() const         { return testCmdAttrib(FromCache); }

    /**
     * A writeback is an eviction that carries data.
     */
    bool isWriteback() const       { return testCmdAttrib(IsEviction) &&
                                            testCmdAttrib(HasData); }

    /**
     * Check if this particular packet type carries payload data. Note
     * that this does not reflect if the data pointer of the packet is
     * valid or not.
     */
    //检查这种特定的数据包类型是否携带负载数据。请注意，这并不反映数据包的数据指针是否有效。
    bool hasData() const        { return testCmdAttrib(HasData); }
    bool isLLSC() const         { return testCmdAttrib(IsLlsc); }
    bool isLockedRMW() const    { return testCmdAttrib(IsLockedRMW); }
    bool isSWPrefetch() const   { return testCmdAttrib(IsSWPrefetch); }
    bool isHWPrefetch() const   { return testCmdAttrib(IsHWPrefetch); }
    bool isPrefetch() const     { return testCmdAttrib(IsSWPrefetch) ||
                                        testCmdAttrib(IsHWPrefetch); }
    bool isError() const        { return testCmdAttrib(IsError); }
    bool isPrint() const        { return testCmdAttrib(IsPrint); }
    bool isFlush() const        { return testCmdAttrib(IsFlush); }

    bool
    isDemand() const
    {
        return (cmd == ReadReq || cmd == WriteReq ||
                cmd == WriteLineReq || cmd == ReadExReq ||
                cmd == ReadCleanReq || cmd == ReadSharedReq ||
                cmd == StorePFTrain);
    }

    Command
    responseCommand() const
    {
        return commandInfo[cmd].response;
    }

    /// Return the string to a cmd given by idx.
    const std::string &toString() const { return commandInfo[cmd].str; }
    int toInt() const { return (int)cmd; }

    MemCmd(Command _cmd) : cmd(_cmd) { }
    MemCmd(int _cmd) : cmd((Command)_cmd) { }
    MemCmd() : cmd(InvalidCmd) { }

    bool operator==(MemCmd c2) const { return (cmd == c2.cmd); }
    bool operator!=(MemCmd c2) const { return (cmd != c2.cmd); }
};

/**
 * A Packet is used to encapsulate a transfer between two objects in
 * the memory system (e.g., the L1 and L2 cache).  (In contrast, a
 * single Request travels all the way from the requestor to the
 * ultimate destination and back, possibly being conveyed by several
 * different Packets along the way.)
 */
/*数据包用于封装内存系统中两个对象之间的传输（例如，L1 和 L2 缓存）。
（相比之下，一个单独的请求会从请求者一路传输到最终目的地，然后返回，过程中可能由多个不同的数据包进行传递。*/
class Packet : public Printable
{
    public:
    typedef uint32_t FlagsType;
    typedef gem5::Flags<FlagsType> Flags;

    enum : FlagsType
    {
        // Flags to transfer across when copying a packet
        COPY_FLAGS             = 0x000000FF,

        // Flags that are used to create reponse packets
        RESPONDER_FLAGS        = 0x00000009,

        // Does this packet have sharers (which means it should not be
        // considered writable) or not. See setHasSharers below.
        /*这个数据包是否有共享者（这意味着它不应被视为可写）？请参见下面的 setHasSharers。*/
        HAS_SHARERS            = 0x00000001,

        // Special control flags
        /// Special timing-mode atomic snoop for multi-level coherence.
        EXPRESS_SNOOP          = 0x00000002,

        /// Allow a responding cache to inform the cache hierarchy
        /// that it had a writable copy before responding. See
        /// setResponderHadWritable below.
        /*允许响应缓存通知缓存层次结构，在响应之前它有一个可写的副本。请参见下面的 setResponderHadWritable。*/
        RESPONDER_HAD_WRITABLE = 0x00000004,

        // Snoop co-ordination flag to indicate that a cache is
        // responding to a snoop. See setCacheResponding below.
        //嗅探协调标志，用于指示缓存正在响应一个嗅探。请参见下面的 setCacheResponding。
        CACHE_RESPONDING       = 0x00000008,

        // The writeback/writeclean should be propagated further
        // downstream by the receiver
        //写回（writeback）/写干净（writeclean）操作应该由接收者进一步向下游传播。
        WRITE_THROUGH          = 0x00000010,

        // Response co-ordination flag for cache maintenance
        // operations
        //缓存维护操作的响应协调标志
        SATISFIED              = 0x00000020,

        // hardware transactional memory

        // Indicates that this packet/request has returned from the
        // cache hierarchy in a failed transaction. The core is
        // notified like this.
        /*表示这个数据包/请求在缓存层次结构中返回了一个失败的事务。核心会通过这种方式收到通知。*/
        FAILS_TRANSACTION      = 0x00000040,

        // Indicates that this packet/request originates in the CPU executing
        // in transactional mode, i.e. in a transaction.
        //表示这个数据包/请求来源于正在执行事务模式的 CPU，即在一个事务中。
        FROM_TRANSACTION       = 0x00000080,

        /// Are the 'addr' and 'size' fields valid?
        VALID_ADDR             = 0x00000100,
        VALID_SIZE             = 0x00000200,

        /// Is the data pointer set to a value that shouldn't be freed
        /// when the packet is destroyed?
        //数据指针是否被设置为在数据包被销毁时不应释放的值？
        STATIC_DATA            = 0x00001000,
        /// The data pointer points to a value that should be freed when
        /// the packet is destroyed. The pointer is assumed to be pointing
        /// to an array, and delete [] is consequently called
        //数据指针指向的值应在数据包被销毁时释放。假定该指针指向一个数组，因此会调用 delete
        DYNAMIC_DATA           = 0x00002000,

        /// suppress the error if this packet encounters a functional
        /// access failure.
        SUPPRESS_FUNC_ERROR    = 0x00008000,

        // Signal block present to squash prefetch and cache evict packets
        // through express snoop flag
        //通过快速嗅探标志发出信号，指示块存在，以压制预取和缓存驱逐数据包。
        BLOCK_CACHED          = 0x00010000
    };

    Flags flags;

    public:
    bool insert_MSHR = false;

    void setInsertMSHR(){
        insert_MSHR = true;
    }

    Tick cur_tick = 0;

    void setCurTick(Tick tick){
        cur_tick = tick;
    }
    typedef MemCmd::Command Command;

    /// The command field of the packet.
    MemCmd cmd;

    const PacketId id;

    /// A pointer to the original request.
    RequestPtr req;

    bool fill_prefetch;

    private:
    /**
    * A pointer to the data being transferred. It can be different
    * sizes at each level of the hierarchy so it belongs to the
    * packet, not request. This may or may not be populated when a
    * responder receives the packet. If not populated memory should
    * be allocated.
    */
    /*
    指向正在传输的数据的指针。它在层次结构的每个级别可以有不同的大小，因此它属于数据包，而不是请求。
    当响应者接收到数据包时，这个指针可能已被填充，也可能未被填充。如果未填充，则应分配内存。
    */
    PacketDataPtr data;

    /// The address of the request.  This address could be virtual or
    /// physical, depending on the system configuration.
    Addr addr;

    /// True if the request targets the secure memory space.
    bool _isSecure;

    /// The size of the request or transfer.
    unsigned size;

    /**
     * Track the bytes found that satisfy a functional read.
     */
    std::vector<bool> bytesValid;

    // Quality of Service priority value
    uint8_t _qosValue;

    // hardware transactional memory

    /**
     * Holds the return status of the transaction.
     * The default case will be NO_FAIL, otherwise this will specify the
     * reason for the transaction's failure in the memory subsystem.
     */
    /*
    保存事务的返回状态。
    默认情况下为 NO_FAIL，否则将指定内存子系统中事务失败的原因。
    */
    HtmCacheFailure htmReturnReason;

    /**
     * A global unique identifier of the transaction.
     * This is used for correctness/debugging only.
     */
    uint64_t htmTransactionUid;

    public:

    /**
     * The extra delay from seeing the packet until the header is
     * transmitted. This delay is used to communicate the crossbar
     * forwarding latency to the neighbouring object (e.g. a cache)
     * that actually makes the packet wait. As the delay is relative,
     * a 32-bit unsigned should be sufficient.
     */
    /*
    从看到数据包到头部被传输的额外延迟。这个延迟用于将交叉开关转发延迟传达给实际使数据包等待的邻近对象（例如，缓存）。
    由于延迟是相对的，32 位无符号整数应该足够。*/
    uint32_t headerDelay;

    /**
     * Keep track of the extra delay incurred by snooping upwards
     * before sending a request down the memory system. This is used
     * by the coherent crossbar to account for the additional request
     * delay.
     */
    /*
    跟踪在向上嗅探后发送请求到内存系统下游所产生的额外延迟。这用于一致性交叉开关，以考虑额外的请求延迟。*/
    uint32_t snoopDelay;

    /**
     * The extra pipelining delay from seeing the packet until the end of
     * payload is transmitted by the component that provided it (if
     * any). This includes the header delay. Similar to the header
     * delay, this is used to make up for the fact that the
     * crossbar does not make the packet wait. As the delay is
     * relative, a 32-bit unsigned should be sufficient.
     */
    /*
    从看到数据包到由提供数据的组件（如果有的话）传输完负载的额外流水线延迟。
    这包括头部延迟。类似于头部延迟，这用于弥补交叉开关没有让数据包等待的事实。
    由于延迟是相对的，32 位无符号整数应该足够。*/
    uint32_t payloadDelay;

    /**
     * A virtual base opaque structure used to hold state associated
     * with the packet (e.g., an MSHR), specific to a SimObject that
     * sees the packet. A pointer to this state is returned in the
     * packet's response so that the SimObject in question can quickly
     * look up the state needed to process it. A specific subclass
     * would be derived from this to carry state specific to a
     * particular sending device.
     *
     * As multiple SimObjects may add their SenderState throughout the
     * memory system, the SenderStates create a stack, where a
     * SimObject can add a new Senderstate, as long as the
     * predecessing SenderState is restored when the response comes
     * back. For this reason, the predecessor should always be
     * populated with the current SenderState of a packet before
     * modifying the senderState field in the request packet.
     */
    /*
    一个虚拟基类的 opaque 结构，用于保存与数据包相关的状态（例如 MSHR），特定于看到数据包的 SimObject。
    此状态的指针会在数据包的响应中返回，以便相关的 SimObject 可以快速查找处理数据包所需的状态。
    一个具体的子类将从这个基类派生，以携带特定发送设备的状态。
    由于多个 SimObject 可能会在内存系统中添加它们的 SenderState，SenderStates 会形成一个栈，
    其中一个 SimObject 可以添加新的 SenderState，只要在响应返回时前一个 SenderState 被恢复。
    因此，在修改请求数据包中的 senderState 字段之前，前一个 SenderState 应始终用当前的数据包 SenderState 填充。*/
    struct SenderState
    {
        SenderState* predecessor;
        SenderState() : predecessor(NULL) {}
        virtual ~SenderState() {}
    };

    /**
     * Object used to maintain state of a PrintReq.  The senderState
     * field of a PrintReq should always be of this type.
     */
    //用于维护 PrintReq 状态的对象。PrintReq 的 senderState 字段应始终为此类型。
    class PrintReqState : public SenderState
    {
        private:
        /**
         * An entry in the label stack.
         */
        struct LabelStackEntry
        {
            const std::string label;
            std::string *prefix;
            bool labelPrinted;
            LabelStackEntry(const std::string &_label, std::string *_prefix);
        };

        typedef std::list<LabelStackEntry> LabelStack;
        LabelStack labelStack;

        std::string *curPrefixPtr;

        public:
        std::ostream &os;
        const int verbosity;

        PrintReqState(std::ostream &os, int verbosity = 0);
        ~PrintReqState();

        /**
         * Returns the current line prefix.
         */
        const std::string &curPrefix() { return *curPrefixPtr; }

        /**
         * Push a label onto the label stack, and prepend the given
         * prefix string onto the current prefix.  Labels will only be
         * printed if an object within the label's scope is printed.
         */
        /*
        将标签推送到标签栈中，并将给定的前缀字符串添加到当前前缀之前。
        只有在标签范围内的对象被打印时，标签才会被打印。*/
        void pushLabel(const std::string &lbl,
                        const std::string &prefix = "  ");

        /**
         * Pop a label off the label stack.
         */
        void popLabel();

        /**
         * Print all of the pending unprinted labels on the
         * stack. Called by printObj(), so normally not called by
         * users unless bypassing printObj().
         */
        /*
        打印标签栈中所有待打印的标签。由 printObj() 调用，因此通常不会由用户直接调用，除非绕过 printObj()。*/
        void printLabels();

        /**
         * Print a Printable object to os, because it matched the
         * address on a PrintReq.
         */
        void printObj(Printable *obj);
    };

    /**
     * This packet's sender state.  Devices should use dynamic_cast<>
     * to cast to the state appropriate to the sender.  The intent of
     * this variable is to allow a device to attach extra information
     * to a request. A response packet must return the sender state
     * that was attached to the original request (even if a new packet
     * is created).
     */
    /*
    这个数据包的发送者状态。设备应使用 dynamic_cast<> 将其转换为适合发送者的状态。
    这个变量的目的是允许设备将额外的信息附加到请求上。
    响应数据包必须返回附加到原始请求的发送者状态（即使创建了新的数据包）。*/
    SenderState *senderState;

    /**
     * Push a new sender state to the packet and make the current
     * sender state the predecessor of the new one. This should be
     * prefered over direct manipulation of the senderState member
     * variable.
     *
     * @param sender_state SenderState to push at the top of the stack
     */
    /*
    将新的发送者状态推送到数据包中，并将当前的发送者状态设置为新状态的前任。这应优于直接操作 senderState 成员变量。*/
    void pushSenderState(SenderState *sender_state);

    /**
     * Pop the top of the state stack and return a pointer to it. This
     * assumes the current sender state is not NULL. This should be
     * preferred over direct manipulation of the senderState member
     * variable.
     *
     * @return The current top of the stack
     */
    /*
    弹出状态栈的顶部并返回其指针。这假设当前的发送者状态不为 NULL。这应优于直接操作 senderState 成员变量。*/
    SenderState *popSenderState();

    /**
     * Go through the sender state stack and return the first instance
     * that is of type T (as determined by a dynamic_cast). If there
     * is no sender state of type T, NULL is returned.
     *
     * @return The topmost state of type T
     */
    /*
    遍历发送者状态栈，并返回第一个类型为 T 的实例（通过 dynamic_cast 确定）。如果没有类型为 T 的发送者状态，则返回 NULL。*/
    template <typename T>
    T * findNextSenderState() const
    {
        T *t = NULL;
        SenderState* sender_state = senderState;
        while (t == NULL && sender_state != NULL) {
            t = dynamic_cast<T*>(sender_state);
            sender_state = sender_state->predecessor;
        }
        return t;
    }

    /// Return the string name of the cmd field (for debugging and
    /// tracing).
    const std::string &cmdString() const { return cmd.toString(); }

    /// Return the index of this command.
    inline int cmdToIndex() const { return cmd.toInt(); }

    bool isStorePFTrain() const     { return cmd == MemCmd::StorePFTrain;  }
    bool isRead() const              { return cmd.isRead(); }
    bool isWrite() const             { return cmd.isWrite(); }
    bool isDemand() const            { return cmd.isDemand(); }
    bool isUpgrade()  const          { return cmd.isUpgrade(); }
    bool isRequest() const           { return cmd.isRequest(); }
    bool isResponse() const          { return cmd.isResponse(); }
    bool needsWritable() const
    {
        // we should never check if a response needsWritable, the
        // request has this flag, and for a response we should rather
        // look at the hasSharers flag (if not set, the response is to
        // be considered writable)
        /*
        我们不应该检查响应是否需要 writable，应该检查请求是否具有这个标志，
        而对于响应，我们应该查看 hasSharers 标志（如果未设置，则应认为响应是可写的）。*/
        assert(isRequest());
        return cmd.needsWritable();
    }
    bool needsResponse() const       { return cmd.needsResponse(); }
    bool isInvalidate() const        { return cmd.isInvalidate(); }
    bool isEviction() const          { return cmd.isEviction(); }
    bool isClean() const             { return cmd.isClean(); }
    bool fromCache() const           { return cmd.fromCache(); }
    bool isWriteback() const         { return cmd.isWriteback(); }
    bool hasData() const             { return cmd.hasData(); }
    bool hasRespData() const
    {
        MemCmd resp_cmd = cmd.responseCommand();
        return resp_cmd.hasData();
    }
    bool isLLSC() const              { return cmd.isLLSC(); }
    bool isLockedRMW() const         { return cmd.isLockedRMW(); }
    bool isError() const             { return cmd.isError(); }
    bool isPrint() const             { return cmd.isPrint(); }
    bool isFlush() const             { return cmd.isFlush(); }

    bool isWholeLineWrite(unsigned blk_size)
    {
        return (cmd == MemCmd::WriteReq || cmd == MemCmd::WriteLineReq) &&
            getOffset(blk_size) == 0 && getSize() == blk_size;
    }

    //@{
    /// Snoop flags
    /**
     * Set the cacheResponding flag. This is used by the caches to
     * signal another cache that they are responding to a request. A
     * cache will only respond to snoops if it has the line in either
     * Modified or Owned state. Note that on snoop hits we always pass
     * the line as Modified and never Owned. In the case of an Owned
     * line we proceed to invalidate all other copies.
     *
     * On a cache fill (see Cache::handleFill), we check hasSharers
     * first, ignoring the cacheResponding flag if hasSharers is set.
     * A line is consequently allocated as:
     *
     * hasSharers cacheResponding state
     * true       false           Shared
     * true       true            Shared
     * false      false           Exclusive独占
     * false      true            Modified
     */
    /*
    缓存用这个标志通知另一个缓存它们正在响应一个请求。
    缓存仅在行处于 Modified 或 Owned 状态时才会响应嗅探。
    请注意，在嗅探命中时，我们总是将行标记为 Modified，而不是 Owned。
    在 Owned 行的情况下，我们会继续使所有其他副本失效。
    */
    /*
    在缓存填充（见 Cache::handleFill）时，我们首先检查 hasSharers，如果 hasSharers 被设置，
    则忽略 cacheResponding 标志。因此，一行会被分配为：
    */
    void setCacheResponding()
    {
        assert(isRequest());
        assert(!flags.isSet(CACHE_RESPONDING));
        flags.set(CACHE_RESPONDING);
    }
    uint64_t promisingResponder{0};
    void setCacheRespondingBy(uint64_t by)
    {
        promisingResponder = by;
    }
    uint64_t getCacheRespondingBy()
    {
        return promisingResponder;
    }
    bool cacheResponding() const { return flags.isSet(CACHE_RESPONDING); }
    /**
     * On fills, the hasSharers flag is used by the caches in
     * combination with the cacheResponding flag, as clarified
     * above. If the hasSharers flag is not set, the packet is passing
     * writable. Thus, a response from a memory passes the line as
     * writable by default.
     *
     * The hasSharers flag is also used by upstream caches to inform a
     * downstream cache that they have the block (by calling
     * setHasSharers on snoop request packets that hit in upstream
     * cachs tags or MSHRs). If the snoop packet has sharers, a
     * downstream cache is prevented from passing a dirty line upwards
     * if it was not explicitly asked for a writable copy. See
     * Cache::satisfyCpuSideRequest.
     *
     * The hasSharers flag is also used on writebacks, in
     * combination with the WritbackClean or WritebackDirty commands,
     * to allocate the block downstream either as:
     *
     * command        hasSharers state
     * WritebackDirty false      Modified
     * WritebackDirty true       Owned
     * WritebackClean false      Exclusive
     * WritebackClean true       Shared
     */
    /*
    在填充操作中，缓存使用 hasSharers 标志与 cacheResponding 标志结合，如上所述。
    如果 hasSharers 标志未设置，则数据包被视为可写。因此，来自内存的响应默认将行视为可写。
    hasSharers 标志还被上游缓存用于通知下游缓存它们拥有该块（通过在命中的上游缓存标签或 MSHR 上调用 setHasSharers）。
    如果嗅探数据包有共享者，则下游缓存会被阻止将脏行传递到上游，除非它明确请求了一个可写副本。见 Cache::satisfyCpuSideRequest。
    hasSharers 标志也用于写回操作，与 WritebackClean 或 WritebackDirty 命令结合使用，以将块分配给下游，具体如下：*/
    void setHasSharers()    { flags.set(HAS_SHARERS); }
    bool hasSharers() const { return flags.isSet(HAS_SHARERS); }
    //@}

    /**
     * The express snoop flag is used for two purposes. Firstly, it is
     * used to bypass flow control for normal (non-snoop) requests
     * going downstream in the memory system. In cases where a cache
     * is responding to a snoop from another cache (it had a dirty
     * line), but the line is not writable (and there are possibly
     * other copies), the express snoop flag is set by the downstream
     * cache to invalidate all other copies in zero time. Secondly,
     * the express snoop flag is also set to be able to distinguish
     * snoop packets that came from a downstream cache, rather than
     * snoop packets from neighbouring caches.
     */
    /*
    express snoop 标志有两个用途。首先，它用于绕过内存系统中正常（非嗅探）请求的流量控制。
    在缓存响应另一个缓存的嗅探请求（它有一个脏行）的情况下，但该行不可写（并且可能有其他副本），
    下游缓存会设置 express snoop 标志以在零时间内使所有其他副本无效。
    其次，express snoop 标志也用于区分来自下游缓存的嗅探数据包，而不是来自邻近缓存的嗅探数据包。*/
    void setExpressSnoop()      { flags.set(EXPRESS_SNOOP); }
    bool isExpressSnoop() const { return flags.isSet(EXPRESS_SNOOP); }

    /**
     * On responding to a snoop request (which only happens for
     * Modified or Owned lines), make sure that we can transform an
     * Owned response to a Modified one. If this flag is not set, the
     * responding cache had the line in the Owned state, and there are
     * possibly other Shared copies in the memory system. A downstream
     * cache helps in orchestrating the invalidation of these copies
     * by sending out the appropriate express snoops.
     */
    /*
    在响应嗅探请求时（这只发生在 Modified 或 Owned 行的情况下），
    确保我们可以将 Owned 响应转换为 Modified 响应。如果未设置此标志，
    则响应缓存中的行处于 Owned 状态，并且内存系统中可能还有其他 Shared 副本。
    下游缓存通过发送适当的 express snoops 来帮助协调这些副本的无效化。*/
    void setResponderHadWritable()
    {
        assert(cacheResponding());
        assert(!responderHadWritable());
        flags.set(RESPONDER_HAD_WRITABLE);
    }
    bool responderHadWritable() const
    { return flags.isSet(RESPONDER_HAD_WRITABLE); }

    /**
     * Copy the reponse flags from an input packet to this packet. The
     * reponse flags determine whether a responder has been found and
     * the state at which the block will be at the destination.
     *
     * @pkt The packet that we will copy flags from
     */
    /*
    将响应标志从输入数据包复制到当前数据包。响应标志确定是否找到了响应者以及块在目标处的状态。*/
    void copyResponderFlags(const PacketPtr pkt);

    /**
     * A writeback/writeclean cmd gets propagated further downstream
     * by the receiver when the flag is set.
     */
    /*
    当设置了该标志时，写回/写清命令会由接收者进一步传播到下游。*/
    void setWriteThrough()
    {
        assert(cmd.isWrite() &&
                (cmd.isEviction() || cmd == MemCmd::WriteClean));
        flags.set(WRITE_THROUGH);
    }
    void clearWriteThrough() { flags.clear(WRITE_THROUGH); }
    bool writeThrough() const { return flags.isSet(WRITE_THROUGH); }

    /**
     * Set when a request hits in a cache and the cache is not going
     * to respond. This is used by the crossbar to coordinate
     * responses for cache maintenance operations.
     */
    /*
    当请求在缓存中命中且缓存不打算响应时设置此标志。这由交叉开关用于协调缓存维护操作的响应。*/
    void setSatisfied()
    {
        assert(cmd.isClean());
        assert(!flags.isSet(SATISFIED));
        flags.set(SATISFIED);
    }
    bool satisfied() const { return flags.isSet(SATISFIED); }

    void setSuppressFuncError()     { flags.set(SUPPRESS_FUNC_ERROR); }
    bool suppressFuncError() const  { return flags.isSet(SUPPRESS_FUNC_ERROR); }
    void setBlockCached()          { flags.set(BLOCK_CACHED); }
    bool isBlockCached() const     { return flags.isSet(BLOCK_CACHED); }
    void clearBlockCached()        { flags.clear(BLOCK_CACHED); }

    /**
     * QoS Value getter
     * Returns 0 if QoS value was never set (constructor default).
     *
     * @return QoS priority value of the packet
     */
    inline uint8_t qosValue() const { return _qosValue; }

    /**
     * QoS Value setter
     * Interface for setting QoS priority value of the packet.
     *
     * @param qos_value QoS priority value
     */
    inline void qosValue(const uint8_t qos_value)
    { _qosValue = qos_value; }

    inline RequestorID requestorId() const { return req->requestorId(); }

    // Network error conditions... encapsulate them as methods since
    // their encoding keeps changing (from result field to command
    // field, etc.)
    void
    setBadAddress()
    {
        assert(isResponse());
        cmd = MemCmd::BadAddressError;
    }

    // Command error conditions. The request is sent to target but the target
    // cannot make it.
    void
    setBadCommand()
    {
        assert(isResponse());
        if (isWrite()) {
            cmd = MemCmd::WriteError;
        } else {
            cmd = MemCmd::ReadError;
        }
    }

    void copyError(Packet *pkt) { assert(pkt->isError()); cmd = pkt->cmd; }

    Addr getAddr() const { assert(flags.isSet(VALID_ADDR)); return addr; }
    /**
     * Update the address of this packet mid-transaction. This is used
     * by the address mapper to change an already set address to a new
     * one based on the system configuration. It is intended to remap
     * an existing address, so it asserts that the current address is
     * valid.
     */
    /*
    在事务进行中更新此数据包的地址。这由地址映射器用于根据系统配置将已设置的地址更改为新地址。
    它旨在重新映射现有地址，因此它断言当前地址是有效的。*/
    void setAddr(Addr _addr) { assert(flags.isSet(VALID_ADDR)); addr = _addr; }

    unsigned getSize() const  { assert(flags.isSet(VALID_SIZE)); return size; }

    /**
     * Get address range to which this packet belongs.
     *
     * @return Address range of this packet.
     */
    AddrRange getAddrRange() const;

    Addr getOffset(unsigned int blk_size) const
    {
        return getAddr() & Addr(blk_size - 1);
    }

    Addr getBlockAddr(unsigned int blk_size) const
    {
        return getAddr() & ~(Addr(blk_size - 1));
    }

    bool isSecure() const
    {
        assert(flags.isSet(VALID_ADDR));
        return _isSecure;
    }

    /**
     * Accessor function to atomic op.
     */
    AtomicOpFunctor *getAtomicOp() const { return req->getAtomicOpFunctor(); }
    bool isAtomicOp() const { return req->isAtomic(); }

    /**
     * It has been determined that the SC packet should successfully update
     * memory. Therefore, convert this SC packet to a normal write.
     */
    /*
    已确定 SC 数据包应该成功更新内存。因此，将此 SC 数据包转换为正常的写操作。*/
    void
    convertScToWrite()
    {
        assert(isLLSC());
        assert(isWrite());
        cmd = MemCmd::WriteReq;
    }

    /**
     * When ruby is in use, Ruby will monitor the cache line and the
     * phys memory should treat LL ops as normal reads.
     */
    void
    convertLlToRead()
    {
        assert(isLLSC());
        assert(isRead());
        cmd = MemCmd::ReadReq;
    }

    /**
     * Constructor. Note that a Request object must be constructed
     * first, but the Requests's physical address and size fields need
     * not be valid. The command must be supplied.
     */
    /*
    构造函数。请注意，必须首先构造一个 Request 对象，但 Request 的物理地址和大小字段不需要有效。必须提供命令。*/
    Packet(const RequestPtr &_req, MemCmd _cmd)
        :  cmd(_cmd), id((PacketId)_req.get()), req(_req),
            fill_prefetch(false),
            data(nullptr), addr(0), _isSecure(false), size(0),
            _qosValue(0),
            htmReturnReason(HtmCacheFailure::NO_FAIL),
            htmTransactionUid(0),
            headerDelay(0), snoopDelay(0),
            payloadDelay(0), senderState(NULL)
    {
        flags.clear();
        if (req->hasPaddr()) {
            addr = req->getPaddr();
            flags.set(VALID_ADDR);
            _isSecure = req->isSecure();
        }

        /**
         * hardware transactional memory
         *
         * This is a bit of a hack!
         * Technically the address of a HTM command is set to zero
         * but is not valid. The reason that we pretend it's valid is
         * to void the getAddr() function from failing. It would be
         * cumbersome to add control flow in many places to check if the
         * packet represents a HTM command before calling getAddr().
         */
        /*
        从技术上讲，HTM 命令的地址设置为零，但无效。我们之所以假装它有效，是为了避免 getAddr() 函数失败。
        在许多地方添加控制流以检查数据包是否表示 HTM 命令，然后再调用 getAddr()，会非常麻烦。*/
        if (req->isHTMCmd()) {
            flags.set(VALID_ADDR);
            assert(addr == 0x0);
        }
        if (req->hasSize()) {
            size = req->getSize();
            flags.set(VALID_SIZE);
        }
    }

    /**
     * Alternate constructor if you are trying to create a packet with
     * a request that is for a whole block, not the address from the
     * req.  this allows for overriding the size/addr of the req.
     */
    /*
    如果你尝试创建一个用于整个块的请求数据包，而不是使用 req 的地址，这个备用构造函数可以使用。它允许覆盖 req 的大小/地址。*/
    Packet(const RequestPtr &_req, MemCmd _cmd, int _blkSize, PacketId _id = 0)
        :  cmd(_cmd), id(_id ? _id : (PacketId)_req.get()), req(_req),
            fill_prefetch(false),
            data(nullptr), addr(0), _isSecure(false),
            _qosValue(0),
            htmReturnReason(HtmCacheFailure::NO_FAIL),
            htmTransactionUid(0),
            headerDelay(0),
            snoopDelay(0), payloadDelay(0), senderState(NULL)
    {
        flags.clear();
        if (req->hasPaddr()) {
            addr = req->getPaddr() & ~(_blkSize - 1);
            flags.set(VALID_ADDR);
            _isSecure = req->isSecure();
        }
        size = _blkSize;
        flags.set(VALID_SIZE);
    }

    /**
     * Alternate constructor for copying a packet.  Copy all fields
     * *except* if the original packet's data was dynamic, don't copy
     * that, as we can't guarantee that the new packet's lifetime is
     * less than that of the original packet.  In this case the new
     * packet should allocate its own data.
     */
    /*
    备用构造函数用于复制数据包。复制所有字段 除了 原始数据包的动态数据，不要复制这些数据，
    因为我们无法保证新数据包的生命周期比原始数据包短。在这种情况下，新数据包应分配自己的数据。*/
    Packet(const PacketPtr pkt, bool clear_flags, bool alloc_data)
        :  cmd(pkt->cmd), id(pkt->id), req(pkt->req),
            fill_prefetch(pkt->fill_prefetch),
            data(nullptr),
            addr(pkt->addr), _isSecure(pkt->_isSecure), size(pkt->size),
            bytesValid(pkt->bytesValid),
            _qosValue(pkt->qosValue()),
            htmReturnReason(HtmCacheFailure::NO_FAIL),
            htmTransactionUid(0),
            headerDelay(pkt->headerDelay),
            snoopDelay(0),
            payloadDelay(pkt->payloadDelay),
            senderState(pkt->senderState)
    {
        if (!clear_flags)
            flags.set(pkt->flags & COPY_FLAGS);

        flags.set(pkt->flags & (VALID_ADDR|VALID_SIZE));

        if (pkt->isHtmTransactional())
            setHtmTransactional(pkt->getHtmTransactionUid());

        if (pkt->htmTransactionFailedInCache()) {
            setHtmTransactionFailedInCache(
                pkt->getHtmTransactionFailedInCacheRC()
            );
        }

        // should we allocate space for data, or not, the express
        // snoops do not need to carry any data as they only serve to
        // co-ordinate state changes
        //我们是否应该为数据分配空间，或者不分配，express snoops 不需要携带任何数据，因为它们仅用于协调状态变化
        if (alloc_data) {
            // even if asked to allocate data, if the original packet
            // holds static data, then the sender will not be doing
            // any memcpy on receiving the response, thus we simply
            // carry the pointer forward
            //即使要求分配数据空间，如果原始数据包包含静态数据，则在接收响应时，
            //发送方不会进行任何 memcpy 操作，因此我们只需将指针传递下去
            if (pkt->flags.isSet(STATIC_DATA)) {
                data = pkt->data;
                flags.set(STATIC_DATA);
            } else {
                allocate();
            }
        }
    }

    /**
     * Generate the appropriate read MemCmd based on the Request flags.
     */
    /**
     * 根据请求类型构建内存命令
     * 
     * @param req 请求对象的指针
     * @return 返回与请求类型相应的内存命令
     * 
     * 此函数根据传入的请求对象指针，判断请求的类型，并返回相应的内存命令
     * 如果请求是HTML命令、锁定加载请求、预取请求等，都会返回对应的内存命令
     * 如果请求是锁定的原子操作请求，则返回对应的内存命令
     * 如果请求是普通的读请求，则返回对应的内存命令
     */
    static MemCmd
    makeReadCmd(const RequestPtr &req)
    {
        // 判断请求是否为HTML命令
        if (req->isHTMCmd()) {
            // 如果是HTML中止命令，则返回对应的内存命令
            if (req->isHTMAbort())
                return MemCmd::HTMAbort;
            else
                // 否则，返回HTML请求对应的内存命令
                return MemCmd::HTMReq;
        } else if (req->isLLSC())
            // 如果请求是锁定加载或存储比较交换命令，则返回对应的内存命令
            return MemCmd::LoadLockedReq;
        else if (req->isPrefetchEx())
            // 如果请求是预取（独占）命令，则返回对应的内存命令
            return MemCmd::SoftPFExReq;
        else if (req->isPrefetch())
            // 如果请求是预取命令，则返回对应的内存命令
            return MemCmd::SoftPFReq;
        else if (req->isLockedRMW())
            // 如果请求是锁定的原子操作命令，则返回对应的内存命令
            return MemCmd::LockedRMWReadReq;
        else
            // 默认情况下，返回普通读请求对应的内存命令
            return MemCmd::ReadReq;
    }

    /**
     * Generate the appropriate write MemCmd based on the Request flags.
     */
    /**
     * 根据请求类型生成相应的内存命令
     * 
     * @param req 请求对象的指针，用于判断其类型并生成对应的内存命令
     * @return 返回生成的内存命令
     */
    static MemCmd
    makeWriteCmd(const RequestPtr &req)
    {
        // 判断请求是否为条件存储类型
        if (req->isLLSC())
            return MemCmd::StoreCondReq;
        // 判断请求是否为交换或原子操作类型
        else if (req->isSwap() || req->isAtomic())
            return MemCmd::SwapReq;
        // 判断请求是否为缓存失效类型
        else if (req->isCacheInvalidate()) {
            // 根据缓存是否干净进一步确定命令类型
            return req->isCacheClean() ? MemCmd::CleanInvalidReq :
                MemCmd::InvalidateReq;
        } else if (req->isCacheClean()) {
            // 如果请求表示缓存是干净的，则生成共享清洁请求
            return MemCmd::CleanSharedReq;
        } else if (req->isLockedRMW()) {
            // 如果请求为带有锁的读写操作，则生成带有锁的RMW写请求
            return MemCmd::LockedRMWWriteReq;
        } else
            // 默认情况下生成普通写请求
            return MemCmd::WriteReq;
    }
    
    static MemCmd
    makePFtrainCmd(const RequestPtr& req) {
        assert(req->isStorePFTrain());
        return MemCmd::StorePFTrain;
    }

    /**
     * Constructor-like methods that return Packets based on Request objects.
     * Fine-tune the MemCmd type if it's not a vanilla read or write.
     */
    static PacketPtr
    createRead(const RequestPtr &req)
    {
        return new Packet(req, makeReadCmd(req));
    }

    static PacketPtr
    createWrite(const RequestPtr &req)
    {
        return new Packet(req, makeWriteCmd(req));
    }

    /**
     * clean up packet variables
     */
    ~Packet()
    {
        deleteData();
    }

    /**
     * Take a request packet and modify it in place to be suitable for
     * returning as a response to that request.
     */
    void
    makeResponse()
    {
        assert(needsResponse());
        assert(isRequest());
        cmd = cmd.responseCommand();

        // responses are never express, even if the snoop that
        // triggered them was
        flags.clear(EXPRESS_SNOOP);
    }

    void
    makeAtomicResponse()
    {
        makeResponse();
    }

    void
    makeTimingResponse()
    {
        makeResponse();
    }

    /// 设置功能响应状态
    /// 
    /// 该函数用于根据操作的成功与否来设置当前命令的状态。如果操作失败，会根据是读操作还是写操作设置不同的错误命令。
    /// 
    /// @param success 操作是否成功的标志，true表示成功，false表示失败。
    void setFunctionalResponseStatus(bool success)
    {
        // 当操作失败时，根据当前是否为写操作来决定设置的错误命令
        if (!success) {
            if (isWrite()) {
                cmd = MemCmd::FunctionalWriteError; // 如果是写操作失败，则设置为功能写错误
            } else {
                cmd = MemCmd::FunctionalReadError; // 如果是读操作失败，则设置为功能读错误
            }
        }
    }

    void
    setSize(unsigned size)
    {
        assert(!flags.isSet(VALID_SIZE));

        this->size = size;
        flags.set(VALID_SIZE);
    }

    /**
     * Check if packet corresponds to a given block-aligned address and
     * address space.
     * 检查数据包是否对应于给定的块对齐地址和地址空间
     *
     * @param addr The address to compare against.
     * @param is_secure Whether addr belongs to the secure address space.
     * @param blk_size Block size in bytes.
     * @return Whether packet matches description.
     */
    bool matchBlockAddr(const Addr addr, const bool is_secure,
                        const int blk_size) const;

    /**
     * Check if this packet refers to the same block-aligned address and
     * address space as another packet.
     * 检查此数据包是否与另一个数据包引用相同的块对齐地址和地址空间。
     *
     * @param pkt The packet to compare against.
     * @param blk_size Block size in bytes.
     * @return Whether packet matches description.
     */
    bool matchBlockAddr(const PacketPtr pkt, const int blk_size) const;

    /**
     * Check if packet corresponds to a given address and address space.
     *
     * @param addr The address to compare against.
     * @param is_secure Whether addr belongs to the secure address space.
     * @return Whether packet matches description.
     */
    bool matchAddr(const Addr addr, const bool is_secure) const;

    /**
     * Check if this packet refers to the same address and address space as
     * another packet.
     *
     * @param pkt The packet to compare against.
     * @return Whether packet matches description.
     */
    bool matchAddr(const PacketPtr pkt) const;

    public:
    /**
     * @{
     * @name Data accessor mehtods
     */

    /**
     * Set the data pointer to the following value that should not be
     * freed. Static data allows us to do a single memcpy even if
     * multiple packets are required to get from source to destination
     * and back. In essence the pointer is set calling dataStatic on
     * the original packet, and whenever this packet is copied and
     * forwarded the same pointer is passed on. When a packet
     * eventually reaches the destination holding the data, it is
     * copied once into the location originally set. On the way back
     * to the source, no copies are necessary.
     */
    /*
    将数据指针设置为不应释放的以下值。静态数据允许我们进行一次 memcpy 操作，即使需要多个数据包才能从源到目的地再返回。
    实质上，该指针是通过在原始数据包上调用 dataStatic 设置的，并且每当此数据包被复制和转发时，都会传递相同的指针。
    当数据包最终到达持有数据的目的地时，它会被复制一次到最初设置的位置。在返回源的过程中，不需要进行任何复制。*/
    template <typename T>
    void
    dataStatic(T *p)
    {
        assert(flags.noneSet(STATIC_DATA|DYNAMIC_DATA));
        data = (PacketDataPtr)p;
        flags.set(STATIC_DATA);
    }

    /**
     * Set the data pointer to the following value that should not be
     * freed. This version of the function allows the pointer passed
     * to us to be const. To avoid issues down the line we cast the
     * constness away, the alternative would be to keep both a const
     * and non-const data pointer and cleverly choose between
     * them. Note that this is only allowed for static data.
     */
    template <typename T>
    void
    dataStaticConst(const T *p)
    {
        assert(flags.noneSet(STATIC_DATA|DYNAMIC_DATA));
        data = const_cast<PacketDataPtr>(p);
        flags.set(STATIC_DATA);
    }

    /**
     * Set the data pointer to a value that should have delete []
     * called on it. Dynamic data is local to this packet, and as the
     * packet travels from source to destination, forwarded packets
     * will allocate their own data. When a packet reaches the final
     * destination it will populate the dynamic data of that specific
     * packet, and on the way back towards the source, memcpy will be
     * invoked in every step where a new packet was created e.g. in
     * the caches. Ultimately when the response reaches the source a
     * final memcpy is needed to extract the data from the packet
     * before it is deallocated.
     */
    template <typename T>
    void
    dataDynamic(T *p)
    {
        assert(flags.noneSet(STATIC_DATA|DYNAMIC_DATA));
        data = (PacketDataPtr)p;
        flags.set(DYNAMIC_DATA);
    }

    /**
     * get a pointer to the data ptr.
     */
    template <typename T>
    T*
    getPtr()
    {
        assert(flags.isSet(STATIC_DATA|DYNAMIC_DATA));
        assert(!isMaskedWrite());
        return (T*)data;
    }

    template <typename T>
    const T*
    getConstPtr() const
    {
        assert(flags.isSet(STATIC_DATA|DYNAMIC_DATA));
        return (const T*)data;
    }

    /**
     * Get the data in the packet byte swapped from big endian to
     * host endian.
     */
    template <typename T>
    T getBE() const;

    /**
     * Get the data in the packet byte swapped from little endian to
     * host endian.
     */
    template <typename T>
    T getLE() const;

    /**
     * Get the data in the packet byte swapped from the specified
     * endianness.
     */
    template <typename T>
    T get(ByteOrder endian) const;

    /** Set the value in the data pointer to v as big endian. */
    template <typename T>
    void setBE(T v);

    /** Set the value in the data pointer to v as little endian. */
    template <typename T>
    void setLE(T v);

    /**
     * Set the value in the data pointer to v using the specified
     * endianness.
     */
    template <typename T>
    void set(T v, ByteOrder endian);

    /**
     * Get the data in the packet byte swapped from the specified
     * endianness and zero-extended to 64 bits.
     */
    uint64_t getUintX(ByteOrder endian) const;

    /**
     * Set the value in the word w after truncating it to the length
     * of the packet and then byteswapping it to the desired
     * endianness.
     */
    void setUintX(uint64_t w, ByteOrder endian);

    /**
     * Copy data into the packet from the provided pointer.
     */
    void
    setData(const uint8_t *p)
    {
        // we should never be copying data onto itself, which means we
        // must idenfity packets with static data, as they carry the
        // same pointer from source to destination and back
        assert(p != getPtr<uint8_t>() || flags.isSet(STATIC_DATA));

        if (p != getPtr<uint8_t>()) {
            // for packet with allocated dynamic data, we copy data from
            // one to the other, e.g. a forwarded response to a response
            std::memcpy(getPtr<uint8_t>(), p, getSize());
        }
    }

    /**
     * Copy data into the packet from the provided block pointer,
     * which is aligned to the given block size.
     */
    void
    setDataFromBlock(const uint8_t *blk_data, int blkSize)
    {
        setData(blk_data + getOffset(blkSize));
    }

    /**
     * Copy data from the packet to the memory at the provided pointer.
     * @param p Pointer to which data will be copied.
     */
    void
    writeData(uint8_t *p) const
    {
        if (!isMaskedWrite()) {
            std::memcpy(p, getConstPtr<uint8_t>(), getSize());
        } else {
            assert(req->getByteEnable().size() == getSize());
            // Write only the enabled bytes
            const uint8_t *base = getConstPtr<uint8_t>();
            for (unsigned int i = 0; i < getSize(); i++) {
                if (req->getByteEnable()[i]) {
                    p[i] = *(base + i);
                }
                // Disabled bytes stay untouched
            }
        }
    }

    /**
     * Copy data from the packet to the provided block pointer, which
     * is aligned to the given block size.
     * @param blk_data Pointer to block to which data will be copied.
     * @param blkSize Block size in bytes.
     */
    void
    writeDataToBlock(uint8_t *blk_data, int blkSize) const
    {
        writeData(blk_data + getOffset(blkSize));
    }

    /**
     * delete the data pointed to in the data pointer. Ok to call to
     * matter how data was allocted.
     */
    void
    deleteData()
    {
        if (flags.isSet(DYNAMIC_DATA))
            delete [] data;

        flags.clear(STATIC_DATA|DYNAMIC_DATA);
        data = NULL;
    }

    /** Allocate memory for the packet. */
    void
    allocate()
    {
        // if either this command or the response command has a data
        // payload, actually allocate space
        if (hasData() || hasRespData()) {
            assert(flags.noneSet(STATIC_DATA|DYNAMIC_DATA));
            flags.set(DYNAMIC_DATA);
            data = new uint8_t[getSize()];
        }
    }

    /** @} */

    /** Get the data in the packet without byte swapping. */
    template <typename T>
    T getRaw() const;

    /** Set the value in the data pointer to v without byte swapping. */
    template <typename T>
    void setRaw(T v);

  public:
    /**
     * Check a functional request against a memory value stored in
     * another packet (i.e. an in-transit request or
     * response). Returns true if the current packet is a read, and
     * the other packet provides the data, which is then copied to the
     * current packet. If the current packet is a write, and the other
     * packet intersects this one, then we update the data
     * accordingly.
     */
    bool
    trySatisfyFunctional(PacketPtr other)
    {
        if (other->isMaskedWrite()) {
            // Do not forward data if overlapping with a masked write
            if (_isSecure == other->isSecure() &&
                getAddr() <= (other->getAddr() + other->getSize() - 1) &&
                other->getAddr() <= (getAddr() + getSize() - 1)) {
                warn("Trying to check against a masked write, skipping."
                     " (addr: 0x%x, other addr: 0x%x)", getAddr(),
                     other->getAddr());
            }
            return false;
        }
        // all packets that are carrying a payload should have a valid
        // data pointer
        return trySatisfyFunctional(other, other->getAddr(), other->isSecure(),
                                    other->getSize(),
                                    other->hasData() ?
                                    other->getPtr<uint8_t>() : NULL);
    }

    /**
     * Does the request need to check for cached copies of the same block
     * in the memory hierarchy above.
     **/
    bool
    mustCheckAbove() const
    {
        return cmd == MemCmd::HardPFReq || isEviction();
    }

    /**
     * Is this packet a clean eviction, including both actual clean
     * evict packets, but also clean writebacks.
     */
    bool
    isCleanEviction() const
    {
        return cmd == MemCmd::CleanEvict || cmd == MemCmd::WritebackClean;
    }

    bool
    isMaskedWrite() const
    {
        return (cmd == MemCmd::WriteReq && req->isMasked());
    }

    /**
     * Check a functional request against a memory value represented
     * by a base/size pair and an associated data array. If the
     * current packet is a read, it may be satisfied by the memory
     * value. If the current packet is a write, it may update the
     * memory value.
     */
    bool
    trySatisfyFunctional(Printable *obj, Addr base, bool is_secure, int size,
                         uint8_t *_data);

    /**
     * Push label for PrintReq (safe to call unconditionally).
     */
    void
    pushLabel(const std::string &lbl)
    {
        if (isPrint())
            safe_cast<PrintReqState*>(senderState)->pushLabel(lbl);
    }

    /**
     * Pop label for PrintReq (safe to call unconditionally).
     */
    void
    popLabel()
    {
        if (isPrint())
            safe_cast<PrintReqState*>(senderState)->popLabel();
    }

    void print(std::ostream &o, int verbosity = 0,
               const std::string &prefix = "") const;

    /**
     * A no-args wrapper of print(std::ostream...)
     * meant to be invoked from DPRINTFs
     * avoiding string overheads in fast mode
     * @return string with the request's type and start<->end addresses
     */
    std::string print() const;

    // hardware transactional memory

    /**
     * Communicates to the core that a packet was processed by the memory
     * subsystem while running in transactional mode.
     * It may happen that the transaction has failed at the memory subsystem
     * and this needs to be communicated to the core somehow.
     * This function decorates the response packet with flags to indicate
     * such a situation has occurred.
     */
    void makeHtmTransactionalReqResponse(const HtmCacheFailure ret_code);

    /**
     * Stipulates that this packet/request originates in the CPU executing
     * in transactional mode, i.e. within a transaction.
     */
    void setHtmTransactional(uint64_t val);

    /**
     * Returns whether or not this packet/request originates in the CPU
     * executing in transactional mode, i.e. within a transaction.
     */
    bool isHtmTransactional() const;

    /**
     * If a packet/request originates in a CPU executing in transactional
     * mode, i.e. within a transaction, this function returns the unique ID
     * of the transaction. This is used for verifying correctness
     * and debugging.
     */
    uint64_t getHtmTransactionUid() const;

    /**
     * Stipulates that this packet/request has returned from the
     * cache hierarchy in a failed transaction. The core is
     * notified like this.
     */
    void setHtmTransactionFailedInCache(const HtmCacheFailure ret_code);

    /**
     * Returns whether or not this packet/request has returned from the
     * cache hierarchy in a failed transaction. The core is
     * notified liked this.
     */
    bool htmTransactionFailedInCache() const;

    /**
     * If a packet/request has returned from the cache hierarchy in a
     * failed transaction, this function returns the failure reason.
     */
    HtmCacheFailure getHtmTransactionFailedInCacheRC() const;

    /** Check if a packet has valid data */
    bool validData() const;
    
    bool missOnLatePf{false};

    bool coalescingMSHR{false};

    int pfSource{PrefetchSourceType::PF_NONE};

    int pfDepth = 0;

    bool fromBOP() const { return pfSource == PrefetchSourceType::HWP_BOP; }
    
    PrefetchSourceType getPFSource() const { return static_cast<PrefetchSourceType>(pfSource); }
    int getPFDepth() const { return pfDepth; }
};

} // namespace gem5

#endif //__MEM_PACKET_HH
