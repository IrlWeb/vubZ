#include "Network/Raknet/Raknet.hpp"

#include <Core/DebugLog.hpp>
#include <Core/Context.hpp>
#include <Core/Utilities.hpp>
#include "Update/Offsets.hpp"

#include <Windows.h>
#include <ShlObj.h>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <map>
#include <memory>
#include <thread>
#include <unordered_map>

namespace Raknet
{
    namespace
    {
        constexpr size_t RakPeerSlots = Offsets::Raknet::RakPeerVTableSize / sizeof(void*);
        alignas(16) void* g_shadowBlock[RakPeerSlots + 1]{};
        void** const g_shadow = g_shadowBlock + 1;
        bool g_shadowBuilt = false;

        std::mutex g_hookMutex;
        std::unordered_map<uintptr_t, uintptr_t> g_hookedPeers;

        bool JobNameEquals(uintptr_t job, const char* want)
        {
            __try
            {
                const uintptr_t text = job + Offsets::Scheduler::JobName;
                const size_t length = *reinterpret_cast<size_t*>(text + 0x10);
                const size_t capacity = *reinterpret_cast<size_t*>(text + 0x18);
                if (length == 0 || length > 256 || capacity < length)
                    return false;

                const char* data = capacity >= 0x10
                    ? *reinterpret_cast<const char**>(text)
                    : reinterpret_cast<const char*>(text);
                if (!data)
                    return false;

                return strncmp(data, want, length) == 0 && want[length] == 0;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        uintptr_t FindJob(const char* name)
        {
            __try
            {
                const uintptr_t scheduler = *reinterpret_cast<uintptr_t*>(Offsets::Scheduler::SchedulerPointer);
                if (!scheduler)
                    return 0;

                const uintptr_t start = *reinterpret_cast<uintptr_t*>(scheduler + Offsets::Scheduler::JobsStart);
                const uintptr_t end = *reinterpret_cast<uintptr_t*>(scheduler + Offsets::Scheduler::JobsEnd + sizeof(uintptr_t));
                if (!start || !end || end <= start)
                    return 0;

                for (uintptr_t entry = start; entry < end; entry += 0x10)
                {
                    const uintptr_t job = *reinterpret_cast<uintptr_t*>(entry);
                    if (job && JobNameEquals(job, name))
                        return job;
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
            return 0;
        }

        uintptr_t ReadPeer(uintptr_t job)
        {
            __try
            {
                return *reinterpret_cast<uintptr_t*>(job + Offsets::Raknet::RakPeer);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return 0;
            }
        }

        void** ReadVtable(uintptr_t object)
        {
            __try
            {
                return *reinterpret_cast<void***>(object);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return nullptr;
            }
        }

        bool ReadStream(const int64_t* sharedPtr, uint8_t** data, uint32_t* size)
        {
            __try
            {
                if (!sharedPtr)
                    return false;
                void* stream = reinterpret_cast<void*>(sharedPtr[0]);
                if (!stream)
                    return false;
                void** vt = *reinterpret_cast<void***>(stream);
                *data = reinterpret_cast<uint8_t* (__fastcall*)(void*)>(vt[1])(stream);
                *size = reinterpret_cast<uint32_t(__fastcall*)(void*)>(vt[2])(stream);
                return *data && *size;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        std::mutex g_addressMutex;
        alignas(16) uint8_t g_address[Offsets::Raknet::AddressOrGuidSize]{};
        bool g_haveAddress = false;

        bool CopyAddress(const void* from, void* to)
        {
            __try
            {
                memcpy(to, from, Offsets::Raknet::AddressOrGuidSize);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        void CacheAddress(const int64_t* systemIdentifier)
        {
            if (!systemIdentifier)
                return;
            alignas(16) uint8_t copy[Offsets::Raknet::AddressOrGuidSize];
            if (!systemIdentifier || !CopyAddress(systemIdentifier, copy))
                return;
            std::lock_guard<std::mutex> lock(g_addressMutex);
            memcpy(g_address, copy, sizeof(copy));
            g_haveAddress = true;
        }

        struct FakeNetworkStream
        {
            void* vftable;
            uint8_t flag;
            uint8_t pad0[7];
            uint8_t* data;
            uint64_t vecBegin;
            uint64_t vecMid;
            uint64_t vecEnd;
            uint32_t bytesUsed;
            uint32_t bytesAlloc;
            uint32_t readOffset;
            uint8_t b3c, b3d;
            uint8_t owned;
            uint8_t b3f;
        };
        static_assert(sizeof(FakeNetworkStream) == 0x40, "NetworkStream layout");

        struct OwnedStream
        {
            FakeNetworkStream stream{};
            std::vector<uint8_t> storage;
        };

        std::shared_ptr<void> MakeOwnedStream(const uint8_t* data, uint32_t size)
        {
            auto owned = std::make_shared<OwnedStream>();
            owned->storage.assign(data, data + size);
            FakeNetworkStream& s = owned->stream;
            s.vftable = reinterpret_cast<void*>(Offsets::Raknet::NetworkStreamVtable);
            s.data = owned->storage.data();
            s.bytesUsed = size;
            s.bytesAlloc = size;
            s.owned = 0;

            return std::shared_ptr<void>(owned, &owned->stream);
        }
    }

    bool NetReader::read(void* out, uint32_t len)
    {
        if (pos + len > n) return false;
        memcpy(out, d + pos, len);
        pos += len;
        return true;
    }

    bool NetReader::u8(uint8_t& v) { return read(&v, 1); }
    bool NetReader::u16(uint16_t& v) { return read(&v, 2); }
    bool NetReader::u32(uint32_t& v) { return read(&v, 4); }
    bool NetReader::u64(uint64_t& v) { return read(&v, 8); }
    bool NetReader::f32(float& v) { return read(&v, 4); }

    bool NetReader::varint(uint32_t& v)
    {
        v = 0;
        uint32_t shift = 0;
        for (int i = 0; i < 10; i++)
        {
            uint8_t b;
            if (!u8(b)) return false;
            v |= (uint32_t)(b & 0x7F) << shift;
            if (!(b & 0x80)) return true;
            shift += 7;
        }
        return false;
    }

    static bool DecodePingItem(NetReader& r, PingItem& out)
    {
        if (!r.u8(out.ExtraStats) || !r.u64(out.Time) || !r.u32(out.FirstSerializeTime))
            return false;

        if (!r.f32(out.RenderFps) || !r.f32(out.PhysicsFps) || !r.f32(out.HeartbeatFps))
        {
            out.Valid = true;
            return true;
        }

        uint32_t encoded = 0;
        if (!r.u32(encoded))
        {
            out.Valid = true;
            return true;
        }

        out.VmMarkers = encoded ^ static_cast<uint32_t>(out.Time);

        if (r.u32(out.MemoryMb) && r.u32(out.SendStats))
        {
            uint8_t trace = 0;
            if (r.u8(trace))
                out.TracePresent = (trace != 0);
        }

        out.Valid = true;
        return true;
    }

    const char* ItemTypeName(uint8_t id)
    {
        switch ((ItemType)id)
        {
        case ItemTypeEnd: return "End";
        case ItemTypeDelete: return "Delete";
        case ItemTypeNew: return "New";
        case ItemTypeChangeProperty: return "ChangeProperty";
        case ItemTypeMarker: return "Marker";
        case ItemTypePing: return "Ping";
        case ItemTypePingBack: return "PingBack";
        case ItemTypeEventInvocation: return "EventInvocation";
        case ItemTypeRequestCharacter: return "RequestCharacter";
        case ItemTypeRocky: return "Rocky";
        case ItemTypeCFrameAcknowledgement: return "CFrameAck";
        case ItemTypeJoinData: return "JoinData";
        case ItemTypeUpdateClientQuota: return "UpdateClientQuota";
        case ItemTypeStreamData: return "StreamData";
        case ItemTypeRegionRemoval: return "RegionRemoval";
        case ItemTypeInstanceRemoval: return "InstanceRemoval";
        case ItemTypeTag: return "Tag";
        case ItemTypeStats: return "Stats";
        case ItemTypeHash: return "Hash";
        case ItemTypeResolveDeferredTree: return "ResolveDeferredTree";
        case ItemTypeStreamDataInfo: return "StreamDataInfo";
        case ItemTypeChangeAttribute: return "ChangeAttribute";
        case ItemTypeJoinDataV2: return "JoinDataV2";
        case ItemTypeStreamTerrain: return "StreamTerrain";
        case ItemTypeStreamPrefetchInfo: return "StreamPrefetchInfo";
        case ItemClientQoutaStreamUpdate: return "ClientQuotaStreamUpdate";
        case ItemClientQos: return "ClientQos";
        case ItemDeferredSharedString: return "DeferredSharedString";
        case ItemTypeISRTimestamp: return "ISRTimestamp";
        case ItemStreamPinData: return "StreamPinData";
        default: return "Unknown";
        }
    }

    std::string DecodeReplicationItem(const uint8_t* b, uint32_t n)
    {
        if (n < 2)
            return ItemTypeName(0);

        NetReader r{ b + 2, n - 2, 0 };
        char buf[256];

        switch ((ItemType)b[1])
        {
        case ItemTypeDelete:
        {
            uint32_t id = 0;
            if (r.varint(id)) { sprintf_s(buf, "Delete obj=0x%X", id); return buf; }
            break;
        }
        case ItemTypeMarker:
        {
            uint32_t ts = 0;
            if (r.u32(ts)) { sprintf_s(buf, "Marker ts=%u", ts); return buf; }
            break;
        }
        case ItemTypePing:
        case ItemTypePingBack:
        {
            PingItem ping{};
            if (DecodePingItem(r, ping))
            {
                sprintf_s(buf, "%s time=%llu fps=%.1f/%.1f/%.1f vm=0x%X mem=%uMB",
                    b[1] == ItemTypePing ? "Ping" : "PingBack",
                    (unsigned long long)ping.Time,
                    ping.RenderFps, ping.PhysicsFps, ping.HeartbeatFps,
                    ping.VmMarkers, ping.MemoryMb);
                return buf;
            }
            break;
        }
        case ItemTypeEventInvocation:
        {
            uint32_t obj = 0, extra = 0;
            uint16_t ev = 0;
            if (r.varint(obj) && r.u32(extra) && r.u16(ev))
            {
                sprintf_s(buf, "EventInvocation obj=0x%X ev=0x%X (%d args left=%u)", obj, ev, ev, (uint32_t)(r.n - r.pos));
                return buf;
            }
            break;
        }
        case ItemTypeISRTimestamp:
        {
            uint32_t ts = 0;
            if (r.u32(ts)) { sprintf_s(buf, "ISRTimestamp ts=%u", ts); return buf; }
            break;
        }
        default:
            break;
        }
        return ItemTypeName(b[1]);
    }

    // Seq of the last entry this thread recorded, so a held packet can find its log row
    // again when a send hook blocks it.
    static thread_local uint64_t lastRecordedSeq = 0;

    void RecordPacket(uint8_t id, uint8_t itemId, uint32_t bitsUsed, PacketPriority priority, PacketReliability reliability, char channel, uint32_t size, const uint8_t* bytes, bool broadcast, uint32_t forceReceipt, bool rbxTransport, bool incoming)
    {
        FILETIME now{};
        GetSystemTimeAsFileTime(&now);
        const uint64_t ticks = (static_cast<uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;

        PacketLogEntry e;
        e.incoming = incoming;
        e.timeMs = static_cast<double>((ticks - 116444736000000000ULL) / 10000ULL);
        e.id = id;
        e.itemId = itemId;
        e.priority = priority;
        e.reliability = reliability;
        e.channel = channel;
        e.size = size;
        e.bitsUsed = bitsUsed;
        e.broadcast = broadcast;
        e.forceReceipt = forceReceipt;
        e.blocked = false;
        e.truncated = size > MaxStoredPacketBytes;
        e.rbxTransport = rbxTransport;
        e.bytes.assign(bytes, bytes + (std::min)((size_t)size, MaxStoredPacketBytes));
        e.summary = (id == 0x83 && size >= 2) ? DecodeReplicationItem(bytes, size) : "";

        std::lock_guard<std::mutex> lock(logMutex);
        e.seq = ++packetSeq;
        lastRecordedSeq = e.seq;
        packetLog.push_back(std::move(e));
        if (packetLog.size() > MaxPacketLogEntries)
            packetLog.pop_front();
    }

    std::vector<PacketLogEntry> SnapshotLog()
    {
        std::lock_guard<std::mutex> lock(logMutex);
        return std::vector<PacketLogEntry>(packetLog.begin(), packetLog.end());
    }

    void ClearLog()
    {
        std::lock_guard<std::mutex> lock(logMutex);
        packetLog.clear();
    }

    void MarkLastPacketBlocked()
    {
        std::lock_guard<std::mutex> lock(logMutex);
        if (!packetLog.empty())
            packetLog.back().blocked = true;
    }

    static void MarkPacketBlocked(uint64_t seq)
    {
        if (!seq)
            return;
        std::lock_guard<std::mutex> lock(logMutex);
        for (auto it = packetLog.rbegin(); it != packetLog.rend(); ++it) {
            if (it->seq == seq) {
                it->blocked = true;
                break;
            }
            if (it->seq < seq)
                break;
        }
    }

    static void SetPacketPayload(lua_State* L, int table, const uint8_t* bytes, uint32_t size)
    {
        table = lua_absindex(L, table);

        lua_pushinteger(L, (int)size);
        lua_setfield(L, table, "Size");

        if (size >= 1)
            lua_pushinteger(L, (int)bytes[0]);
        else
            lua_pushnil(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, table, "Id");
        lua_pushvalue(L, -1);
        lua_setfield(L, table, "ID");
        lua_pushvalue(L, -1);
        lua_setfield(L, table, "PacketID");
        lua_setfield(L, table, "PacketId");

        if (size >= 2)
            lua_pushinteger(L, (int)bytes[1]);
        else
            lua_pushnil(L);
        lua_setfield(L, table, "ItemId");

        void* buf = lua_newbuffer(L, size);
        memcpy(buf, bytes, size);
        lua_pushvalue(L, -1);
        lua_setfield(L, table, "Data");
        lua_setfield(L, table, "AsBuffer");

        lua_createtable(L, (int)size, 0);
        for (uint32_t i = 0; i < size; i++) {
            lua_pushinteger(L, (int)bytes[i]);
            lua_rawseti(L, -2, i + 1);
        }
        lua_setfield(L, table, "AsArray");

        lua_pushlstring(L, reinterpret_cast<const char*>(bytes), size);
        lua_setfield(L, table, "AsString");
    }

    void pushPacketUserdata(lua_State* L, uint8_t* bytes, uint32_t size,
                            PacketPriority priority, PacketReliability reliability, char orderingChannel)
    {
        lua_newtable(L);

        lua_pushinteger(L, (int)priority);
        lua_setfield(L, -2, "Priority");

        lua_pushinteger(L, (int)reliability);
        lua_setfield(L, -2, "Reliability");

        lua_pushinteger(L, (int)(unsigned char)orderingChannel);
        lua_setfield(L, -2, "OrderingChannel");

        SetPacketPayload(L, -1, bytes, size);
    }

    static void PumpHeldPackets(lua_State* L);

    void ProcessCallbacks(lua_State* L)
    {
        if (!L) return;

        PumpHeldPackets(L);

        std::vector<QueuedCallback> batch;
        {
            std::lock_guard<std::mutex> lock(callbackQueueMutex);
            if (callbackQueue.empty()) return;
            batch.swap(callbackQueue);
        }

        if (!lua_checkstack(L, 2))
            return;

        const int top = lua_gettop(L);

        lua_State* T = lua_newthread(L);
        if (!T || !lua_checkstack(T, 24)) {
            lua_settop(L, top);
            return;
        }

        lua_getglobal(T, "task");
        if (lua_type(T, -1) != LUA_TTABLE) {
            lua_settop(L, top);
            return;
        }
        lua_getfield(T, -1, "defer");
        lua_remove(T, -2);
        if (lua_type(T, 1) != LUA_TFUNCTION) {
            lua_settop(L, top);
            return;
        }

        for (auto& qc : batch) {
            lua_pushvalue(T, 1);

            lua_getref(T, qc.ref);
            if (lua_type(T, -1) != LUA_TFUNCTION) {
                lua_settop(T, 1);
                continue;
            }

            pushPacketUserdata(T, qc.data.data(), qc.size, qc.priority, qc.reliability, qc.orderingChannel);

            if (lua_pcall(T, 2, 0, 0) != LUA_OK) {
                const char* err = lua_tostring(T, -1);
                SUPQ_LOG_ERR("[Raknet] task.defer failed: %s", err ? err : "unknown");
            }
            lua_settop(T, 1);
        }

        lua_settop(L, top);
    }

    static std::atomic<bool> hasSendHooks{ false };
    static std::atomic<bool> hasReceiveHooks{ false };

    static bool ObservePacket(const uint8_t* packetBytes, uint32_t packetSize, PacketPriority priority,
                              PacketReliability reliability, char orderingChannel, const int64_t* systemIdentifier,
                              bool Broadcast, uint32_t ForceReceiptNumber, bool rbxTransport = false) noexcept
    {
        const uint8_t packetId = packetBytes[0];
        const uint8_t itemId = (packetSize > 1) ? packetBytes[1] : (uint8_t)ItemTypeEnd;
        lastRecordedSeq = 0;
        const bool drop = desyncing && packetId == (uint8_t)desyncPacketId.load();

        try {
            if (!Broadcast)
                CacheAddress(systemIdentifier);

            if (!capturePaused) {
                RecordPacket(packetId, itemId, packetSize * 8, priority, reliability, orderingChannel, packetSize, packetBytes, Broadcast, ForceReceiptNumber, rbxTransport);
                if (drop)
                    MarkLastPacketBlocked();
            }
        }
        catch (...) {
        }

        return drop;
    }

    namespace
    {
        struct HeldPacket
        {
            std::shared_ptr<void> stream;
            uintptr_t peer = 0;
            PacketPriority priority = HIGH_PRIORITY;
            PacketReliability reliability = RELIABLE_ORDERED;
            char channel = 0;
            bool broadcast = false;
            uint32_t forceReceipt = 0;
            alignas(16) uint8_t address[Offsets::Raknet::AddressOrGuidSize]{};
            std::vector<uint8_t> bytes;
            bool replaced = false;
            bool blocked = false;
            bool rbx = false;
            uint32_t rbxChannel = 0;
            uint64_t logSeq = 0;
        };

        struct HeldBatch
        {
            std::vector<HeldPacket> packets;
            ULONGLONG handedAt = 0;
        };

        std::mutex g_heldMutex;
        std::vector<HeldPacket> g_held;
        std::atomic<bool> g_heldPending{ false };

        std::map<uint64_t, HeldBatch> g_batches;
        uint64_t g_nextBatchId = 1;

        uint64_t g_dispatchingBatch = 0;

        constexpr size_t MaxHeldPackets = 8192;
        constexpr ULONGLONG StaleBatchMs = 2000;
    }

    static bool HoldPacket(int64_t* peer, const int64_t* bitStream, const uint8_t* bytes, uint32_t size,
                           PacketPriority priority, PacketReliability reliability, char channel,
                           const int64_t* systemIdentifier, bool broadcast, uint32_t forceReceipt) noexcept
    {
        try {
            if (!bitStream || !reinterpret_cast<void* const*>(bitStream)[1])
                return false;

            HeldPacket held;
            if (!systemIdentifier || !CopyAddress(systemIdentifier, held.address))
                return false;

            held.stream = *reinterpret_cast<const std::shared_ptr<void>*>(bitStream);
            held.peer = reinterpret_cast<uintptr_t>(peer);
            held.priority = priority;
            held.reliability = reliability;
            held.channel = channel;
            held.broadcast = broadcast;
            held.forceReceipt = forceReceipt;
            held.bytes.assign(bytes, bytes + size);
            held.logSeq = lastRecordedSeq;

            std::lock_guard<std::mutex> lock(g_heldMutex);
            if (g_held.size() >= MaxHeldPackets)
                return false;
            g_held.push_back(std::move(held));
            g_heldPending.store(true, std::memory_order_relaxed);
            return true;
        }
        catch (...) {
            return false;
        }
    }

    namespace
    {
        namespace RT = Offsets::RbxTransport;

        struct alignas(8) RtStream { uint8_t raw[RT::StreamSize]; };
        struct alignas(8) RtSendResult { uint64_t sendId; uint8_t accepted; uint8_t pad[7]; uint64_t detail; };
        struct alignas(8) RtReceiveBatch { uint8_t* first; uint8_t* last; uint8_t* limit; uint8_t meta[16]; };
        struct alignas(8) RtChannelConfig { uint32_t reliability; uint32_t priority; uint8_t receipt; uint8_t pad[7]; };
        static_assert(sizeof(RtSendResult) == 0x18, "RbxTransport send result");
        static_assert(sizeof(RtReceiveBatch) == 0x28, "RbxTransport receive batch");

        using RtDestroyFn = void* (__fastcall*)(void*, unsigned int);
        using RtSendFn = RtSendResult* (__fastcall*)(void*, RtSendResult*, uint32_t, RtStream*);
        using RtReceiveFn = RtReceiveBatch* (__fastcall*)(void*, RtReceiveBatch*, uint32_t);
        using RtReceiveMaxFn = RtReceiveBatch* (__fastcall*)(void*, RtReceiveBatch*, uint32_t, int32_t);
        using RtChannelConfigFn = RtChannelConfig* (__fastcall*)(RtChannelConfig*, uint32_t);
        using RtStreamCreateFn = RtStream* (__fastcall*)(RtStream*, size_t, size_t);
        using RtStreamCopyFn = RtStream* (__fastcall*)(RtStream*, const RtStream*);
        using RtStreamAppendFn = void(__fastcall*)(RtStream*, const uint8_t*, size_t);
        using RtStreamDestroyFn = void(__fastcall*)(RtStream*);

        alignas(16) void* g_rtShadowBlock[RT::AppVTableSlots + 1]{};
        void** const g_rtShadow = g_rtShadowBlock + 1;
        bool g_rtShadowBuilt = false;

        RtDestroyFn g_rtDestroy = nullptr;
        RtSendFn g_rtSend = nullptr;
        RtReceiveFn g_rtReceive = nullptr;
        RtReceiveMaxFn g_rtReceiveMax = nullptr;

        std::atomic<uintptr_t> g_rtApp{ 0 };
        std::atomic<uintptr_t> g_rtClient{ 0 };

        std::atomic<uint8_t> g_rtChannelForId[256]{};

        struct RtOutPacket
        {
            uintptr_t app = 0;
            uint32_t channel = 0;
            std::vector<uint8_t> bytes;
        };

        std::mutex g_rtOutMutex;
        std::vector<RtOutPacket> g_rtOut;
        std::atomic<bool> g_rtOutPending{ false };
        constexpr size_t MaxOutPackets = 8192;

        RtChannelConfig RtConfig(uint32_t channel)
        {
            RtChannelConfig config{};
            if (channel < RT::ChannelCount)
                reinterpret_cast<RtChannelConfigFn>(RT::ChannelConfig)(&config, channel);
            return config;
        }

        PacketPriority RtPriority(const RtChannelConfig& config)
        {
            return config.priority >= 2 && config.priority <= 4
                ? static_cast<PacketPriority>(config.priority - 1) : MEDIUM_PRIORITY;
        }

        PacketReliability RtReliability(const RtChannelConfig& config)
        {
            if (config.reliability == 1)
                return config.receipt ? UNRELIABLE_WITH_ACK_RECEIPT : UNRELIABLE_SEQUENCED;
            return config.receipt ? RELIABLE_ORDERED_WITH_ACK_RECEIPT : RELIABLE_ORDERED;
        }

        bool RtPayload(const void* stream, const uint8_t** data, uint32_t* size)
        {
            __try
            {
                const auto* s = static_cast<const uint8_t*>(stream);
                const uint8_t* bytes = nullptr;
                size_t used = 0;
                if (s[RT::StreamPooled] != 0)
                {
                    bytes = *reinterpret_cast<const uint8_t* const*>(s + RT::StreamPoolBytes);
                    used = *reinterpret_cast<const size_t*>(s + RT::StreamPoolLength);
                }
                else
                {
                    bytes = *reinterpret_cast<const uint8_t* const*>(s + RT::StreamVecBegin);
                    const auto* end = *reinterpret_cast<const uint8_t* const*>(s + RT::StreamVecEnd);
                    used = end >= bytes ? static_cast<size_t>(end - bytes) : 0;
                }
                const size_t prefix = *reinterpret_cast<const size_t*>(s + RT::StreamPrefix);
                if (!bytes || used > 16 * 1024 * 1024 || prefix >= used)
                    return false;

                *data = bytes + prefix;
                *size = static_cast<uint32_t>(used - prefix);
                volatile uint8_t probe = (*data)[0];
                probe = (*data)[*size - 1];
                (void)probe;
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        void RtTransmit(void* app, uint32_t channel, const std::vector<uint8_t>& bytes)
        {
            const auto create = reinterpret_cast<RtStreamCreateFn>(RT::StreamCreate);
            const auto copy = reinterpret_cast<RtStreamCopyFn>(RT::StreamCopy);
            const auto append = reinterpret_cast<RtStreamAppendFn>(RT::StreamAppend);
            const auto destroy = reinterpret_cast<RtStreamDestroyFn>(RT::StreamDestroy);

            const size_t prefix = RtConfig(channel).reliability == 1 ? 32 : 8;

            RtStream scratch{};
            create(&scratch, bytes.size(), prefix);
            RtStream stream{};
            try {
                copy(&stream, &scratch);
            }
            catch (...) {
                destroy(&scratch);
                throw;
            }
            destroy(&scratch);

            try {
                append(&stream, bytes.data(), bytes.size());
                RtSendResult result{};
                g_rtSend(app, &result, channel, &stream);
            }
            catch (...) {
                destroy(&stream);
                throw;
            }
            destroy(&stream);
        }

        void RtFlushOut(void* app)
        {
            if (!g_rtOutPending.load(std::memory_order_acquire))
                return;

            std::vector<RtOutPacket> out;
            {
                std::lock_guard<std::mutex> lock(g_rtOutMutex);
                out.swap(g_rtOut);
                g_rtOutPending.store(false, std::memory_order_release);
            }

            for (const RtOutPacket& packet : out) {
                if (packet.app != reinterpret_cast<uintptr_t>(app) || packet.bytes.empty())
                    continue;
                try {
                    RtTransmit(app, packet.channel, packet.bytes);
                }
                catch (...) {
                }
            }
        }

        bool RtChannelReady(uint32_t channel)
        {
            const uintptr_t client = g_rtClient.load();
            if (!client || channel >= RT::ChannelCount)
                return false;
            __try
            {
                const uint64_t mask = *reinterpret_cast<const uint64_t*>(client + RT::ClientChannelMask);
                return (mask & (1ull << channel)) != 0;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        bool RtQueueOut(uintptr_t app, uint32_t channel, std::vector<uint8_t> bytes)
        {
            if (!app || bytes.empty() || !RtChannelReady(channel))
                return false;

            std::lock_guard<std::mutex> lock(g_rtOutMutex);
            if (g_rtOut.size() >= MaxOutPackets)
                return false;
            g_rtOut.push_back({ app, channel, std::move(bytes) });
            g_rtOutPending.store(true, std::memory_order_release);
            return true;
        }

        bool RtHoldPacket(void* app, uint32_t channel, const RtChannelConfig& config,
                          const uint8_t* bytes, uint32_t size) noexcept
        {
            try {
                HeldPacket held;
                held.rbx = true;
                held.rbxChannel = channel;
                held.peer = reinterpret_cast<uintptr_t>(app);
                held.priority = RtPriority(config);
                held.reliability = RtReliability(config);
                held.channel = static_cast<char>(channel);
                held.bytes.assign(bytes, bytes + size);
                held.logSeq = lastRecordedSeq;

                std::lock_guard<std::mutex> lock(g_heldMutex);
                if (g_held.size() >= MaxHeldPackets)
                    return false;
                g_held.push_back(std::move(held));
                g_heldPending.store(true, std::memory_order_relaxed);
                return true;
            }
            catch (...) {
                return false;
            }
        }

        void QueueReceiveCallbacks(const uint8_t* bytes, uint32_t size, PacketPriority priority,
                                   PacketReliability reliability, char channel)
        {
            std::lock_guard<std::mutex> hlock(hooksMutex);
            std::lock_guard<std::mutex> lock(callbackQueueMutex);
            for (auto& cb : OnSharedReceiveCallbacks) {
                if (cb.packetId >= 0 && cb.packetId != (int)bytes[0])
                    continue;
                if (callbackQueue.size() >= MaxQueuedCallbacks)
                    return;

                QueuedCallback qc;
                qc.ref = cb.ref;
                qc.data.assign(bytes, bytes + size);
                qc.size = size;
                qc.priority = priority;
                qc.reliability = reliability;
                qc.orderingChannel = channel;
                qc.isSend = false;
                callbackQueue.push_back(std::move(qc));
            }
        }

        bool RtReadBatch(const RtReceiveBatch* batch, uint8_t** first, uint8_t** last)
        {
            __try
            {
                *first = batch->first;
                *last = batch->last;
                return *first && *last >= *first
                    && static_cast<size_t>(*last - *first) % RT::ReceivedStreamSize == 0
                    && static_cast<size_t>(*last - *first) <= 4096 * RT::ReceivedStreamSize;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        void RtCaptureBatch(uint32_t channel, const RtReceiveBatch* batch)
        {
            uint8_t* first = nullptr;
            uint8_t* last = nullptr;
            if (channel >= RT::ChannelCount || !RtReadBatch(batch, &first, &last))
                return;

            const RtChannelConfig config = RtConfig(channel);
            for (uint8_t* item = first; item != last; item += RT::ReceivedStreamSize) {
                const uint8_t* data = nullptr;
                uint32_t size = 0;
                if (RtPayload(item, &data, &size))
                    QueueReceiveCallbacks(data, size, RtPriority(config), RtReliability(config), static_cast<char>(channel));
            }
        }

        void* __fastcall RtDestroyHook(void* app, unsigned int flags)
        {
            uintptr_t expected = reinterpret_cast<uintptr_t>(app);
            if (g_rtApp.compare_exchange_strong(expected, 0))
                g_rtClient = 0;
            return g_rtDestroy(app, flags);
        }

        RtSendResult* __fastcall RtSendHook(void* app, RtSendResult* result, uint32_t channel, RtStream* stream)
        {
            try {
                RtFlushOut(app);
            }
            catch (...) {
            }

            const uint8_t* data = nullptr;
            uint32_t size = 0;
            if (channel < RT::ChannelCount && RtPayload(stream, &data, &size)) {
                g_rtChannelForId[data[0]].store(static_cast<uint8_t>(channel + 1), std::memory_order_relaxed);

                const RtChannelConfig config = RtConfig(channel);
                if (ObservePacket(data, size, RtPriority(config), RtReliability(config), static_cast<char>(channel),
                                  nullptr, false, 0, true)) {
                    desyncDropped.fetch_add(1, std::memory_order_relaxed);
                    memset(result, 0, sizeof(*result));
                    return result;
                }

                if ((hasSendHooks.load(std::memory_order_relaxed) || g_heldPending.load(std::memory_order_relaxed))
                    && !config.receipt
                    && reinterpret_cast<uintptr_t>(app) == g_rtApp.load()
                    && RtHoldPacket(app, channel, config, data, size)) {
                    memset(result, 0, sizeof(*result));
                    return result;
                }
            }

            return g_rtSend(app, result, channel, stream);
        }

        RtReceiveBatch* __fastcall RtReceiveHook(void* app, RtReceiveBatch* result, uint32_t channel)
        {
            RtReceiveBatch* returned = g_rtReceive(app, result, channel);
            if (hasReceiveHooks.load(std::memory_order_relaxed)) {
                try {
                    RtCaptureBatch(channel, result);
                }
                catch (...) {
                }
            }
            return returned;
        }

        RtReceiveBatch* __fastcall RtReceiveMaxHook(void* app, RtReceiveBatch* result, uint32_t channel, int32_t maxBytes)
        {
            RtReceiveBatch* returned = g_rtReceiveMax(app, result, channel, maxBytes);
            if (hasReceiveHooks.load(std::memory_order_relaxed)) {
                try {
                    RtCaptureBatch(channel, result);
                }
                catch (...) {
                }
            }
            return returned;
        }

        bool RtClassIs(uintptr_t instance, const char* want)
        {
            __try
            {
                const uintptr_t descriptor = *reinterpret_cast<uintptr_t*>(instance + Offsets::Instance::ClassDescriptor);
                const uintptr_t text = descriptor ? *reinterpret_cast<uintptr_t*>(descriptor + Offsets::Instance::ClassName) : 0;
                if (!text)
                    return false;
                const size_t length = *reinterpret_cast<size_t*>(text + 0x10);
                const size_t capacity = *reinterpret_cast<size_t*>(text + 0x18);
                if (length == 0 || length > 64 || capacity < length)
                    return false;
                const char* chars = capacity >= 0x10 ? *reinterpret_cast<const char**>(text) : reinterpret_cast<const char*>(text);
                return strncmp(chars, want, length) == 0 && want[length] == 0;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        enum RtStage : int
        {
            RtNoDataModel, RtNoChildren, RtNoNetworkClient, RtNoClient, RtClientVtable,
            RtNoApp, RtAppVtable, RtSwapFailed, RtFault, RtBound,
        };
        std::atomic<int> g_rtStage{ RtNoDataModel };
        std::atomic<uintptr_t> g_rtSeen{ 0 };

        RtStage RtFindApp(uintptr_t* clientOut, uintptr_t* appOut)
        {
            __try
            {
                const uintptr_t fake = *reinterpret_cast<uintptr_t*>(Offsets::DataModel::FakeDataModelPointer);
                const uintptr_t dataModel = fake ? *reinterpret_cast<uintptr_t*>(fake + Offsets::DataModel::FakeDataModelToDataModel) : 0;
                if (!dataModel)
                    return RtNoDataModel;
                const uintptr_t container = *reinterpret_cast<uintptr_t*>(dataModel + Offsets::Instance::Children);
                if (!container)
                    return RtNoChildren;

                const uintptr_t begin = *reinterpret_cast<uintptr_t*>(container);
                const uintptr_t end = *reinterpret_cast<uintptr_t*>(container + 8);
                if (!begin || end <= begin || end - begin > 0x10000 || (end - begin) % 0x10)
                    return RtNoChildren;

                for (uintptr_t entry = begin; entry < end; entry += 0x10) {
                    const uintptr_t child = *reinterpret_cast<uintptr_t*>(entry);
                    if (!child || !RtClassIs(child, "NetworkClient"))
                        continue;

                    const uintptr_t replicator = *reinterpret_cast<uintptr_t*>(child + RT::NetworkClientReplicator);
                    const uintptr_t client = replicator
                        ? *reinterpret_cast<uintptr_t*>(replicator + RT::ReplicatorConnection)
                        : *reinterpret_cast<uintptr_t*>(child + RT::NetworkClientConnection);
                    g_rtSeen = child;
                    if (!client)
                        return RtNoClient;
                    g_rtSeen = *reinterpret_cast<uintptr_t*>(client);
                    if (g_rtSeen != RT::ClientVTable)
                        return RtClientVtable;
                    const uintptr_t app = *reinterpret_cast<uintptr_t*>(client + RT::ClientApp);
                    g_rtSeen = client;
                    if (!app)
                        return RtNoApp;
                    *clientOut = client;
                    *appOut = app;
                    return RtBound;
                }
                return RtNoNetworkClient;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return RtFault;
            }
        }

        bool RtBind()
        {
            uintptr_t client = 0;
            uintptr_t app = 0;
            const RtStage found = RtFindApp(&client, &app);
            if (found != RtBound) {
                g_rtStage = found;
                g_rtApp = 0;
                g_rtClient = 0;
                return false;
            }

            void** live = ReadVtable(app);
            if (live == g_rtShadow) {
                g_rtStage = RtBound;
                g_rtClient = client;
                g_rtApp = app;
                return true;
            }

            void** const genuine = reinterpret_cast<void**>(RT::AppVTable);
            if (live != genuine) {
                g_rtStage = RtAppVtable;
                g_rtSeen = reinterpret_cast<uintptr_t>(live);
                return false;
            }

            std::lock_guard<std::mutex> lock(g_hookMutex);
            if (!g_rtShadowBuilt) {
                memcpy(g_rtShadowBlock, genuine - 1, sizeof(g_rtShadowBlock));
                g_rtDestroy = reinterpret_cast<RtDestroyFn>(genuine[RT::idx_AppDestructor]);
                g_rtSend = reinterpret_cast<RtSendFn>(genuine[RT::idx_AppSend]);
                g_rtReceive = reinterpret_cast<RtReceiveFn>(genuine[RT::idx_AppReceive]);
                g_rtReceiveMax = reinterpret_cast<RtReceiveMaxFn>(genuine[RT::idx_AppReceiveMax]);
                g_rtShadow[RT::idx_AppDestructor] = reinterpret_cast<void*>(&RtDestroyHook);
                g_rtShadow[RT::idx_AppSend] = reinterpret_cast<void*>(&RtSendHook);
                g_rtShadow[RT::idx_AppReceive] = reinterpret_cast<void*>(&RtReceiveHook);
                g_rtShadow[RT::idx_AppReceiveMax] = reinterpret_cast<void*>(&RtReceiveMaxHook);
                g_rtShadowBuilt = true;
            }

            if (InterlockedCompareExchangePointer(reinterpret_cast<PVOID volatile*>(app), g_rtShadow, genuine) != genuine) {
                g_rtStage = RtSwapFailed;
                return false;
            }

            g_rtStage = RtBound;
            g_rtClient = client;
            g_rtApp = app;
            SUPQ_LOG_OK("[Raknet] hooked RbxTransport app=0x%llX", (unsigned long long)app);
            return true;
        }
    }

    static bool PeerStillOurs(uintptr_t peer)
    {
        return peer && peer == RakPeer_instance.load() && ReadVtable(peer) == g_shadow;
    }

    static void ResendHeld(const HeldPacket& packet)
    {
        // Hook blocks count the same as desync drops, so status().dropped and the log's
        // Blocked flag show them.
        if (packet.blocked) {
            desyncDropped.fetch_add(1, std::memory_order_relaxed);
            MarkPacketBlocked(packet.logSeq);
            return;
        }

        if (packet.rbx) {
            if (!packet.blocked && packet.peer == g_rtApp.load())
                RtQueueOut(packet.peer, packet.rbxChannel, packet.bytes);
            return;
        }

        const send_fn_t original = original_raknet_send.load();
        if (packet.blocked || packet.bytes.empty() || !original || !PeerStillOurs(packet.peer))
            return;

        std::shared_ptr<void> stream = packet.replaced
            ? MakeOwnedStream(packet.bytes.data(), (uint32_t)packet.bytes.size())
            : packet.stream;

        alignas(16) uint8_t address[Offsets::Raknet::AddressOrGuidSize];
        memcpy(address, packet.address, sizeof(address));

        original(reinterpret_cast<int64_t*>(packet.peer), reinterpret_cast<int64_t*>(&stream),
                 packet.priority, packet.reliability, packet.channel,
                 reinterpret_cast<int64_t*>(address), packet.broadcast, packet.forceReceipt);
    }

    static HeldPacket* PacketMethodTarget(lua_State* L)
    {
        const uint64_t batchId = (uint64_t)lua_tonumber(L, lua_upvalueindex(1));
        const size_t index = (size_t)lua_tonumber(L, lua_upvalueindex(2));

        auto batch = g_batches.find(batchId);
        if (batchId != g_dispatchingBatch || batch == g_batches.end() || index >= batch->second.packets.size())
            luaL_error(L, "the packet has already been sent; change it inside its send hook");

        return &batch->second.packets[index];
    }

    static int PacketSetData(lua_State* L)
    {
        HeldPacket* packet = PacketMethodTarget(L);

        const int self = lua_istable(L, 1) && lua_gettop(L) >= 2 ? 1 : 0;
        const int arg = self ? 2 : 1;

        std::vector<uint8_t> data;
        if (lua_type(L, arg) == LUA_TSTRING) {
            size_t length = 0;
            const char* text = lua_tolstring(L, arg, &length);
            data.assign(text, text + length);
        }
        else if (lua_type(L, arg) == LUA_TBUFFER) {
            size_t length = 0;
            const auto* raw = static_cast<const uint8_t*>(lua_tobuffer(L, arg, &length));
            data.assign(raw, raw + length);
        }
        else if (lua_type(L, arg) == LUA_TTABLE) {
            const int count = lua_objlen(L, arg);
            data.reserve(count);
            for (int i = 1; i <= count; ++i) {
                lua_rawgeti(L, arg, i);
                const double byte = lua_tonumber(L, -1);
                if (!lua_isnumber(L, -1) || byte < 0 || byte > 255 || byte != (double)(int)byte)
                    luaL_error(L, "SetData: byte %d must be an integer from 0 to 255", i);
                data.push_back((uint8_t)byte);
                lua_pop(L, 1);
            }
        }
        else {
            luaL_typeerrorL(L, arg, "buffer, string or {number}");
        }

        if (data.empty())
            luaL_argerrorL(L, arg, "packet data must not be empty");

        packet->bytes = std::move(data);
        packet->replaced = true;

        if (self)
            SetPacketPayload(L, 1, packet->bytes.data(), (uint32_t)packet->bytes.size());
        return 0;
    }

    static int PacketBlock(lua_State* L)
    {
        PacketMethodTarget(L)->blocked = true;
        return 0;
    }

    static int DispatchHeldBatch(lua_State* L)
    {
        const uint64_t batchId = (uint64_t)luaL_checknumber(L, 1);
        if (g_batches.find(batchId) == g_batches.end())
            return 0;

        std::vector<int> refs;
        {
            std::lock_guard<std::mutex> lock(hooksMutex);
            for (auto& cb : OnSharedSendCallbacks)
                refs.push_back(cb.ref);
        }

        lua_settop(L, 0);
        g_dispatchingBatch = batchId;

        const size_t count = g_batches[batchId].packets.size();
        for (size_t i = 0; i < count && !refs.empty(); ++i) {
            HeldPacket& packet = g_batches[batchId].packets[i];

            pushPacketUserdata(L, packet.bytes.data(), (uint32_t)packet.bytes.size(),
                               packet.priority, packet.reliability, packet.channel);
            for (const char* method : { "SetData", "Block", "Ignore" }) {
                lua_pushnumber(L, (double)batchId);
                lua_pushnumber(L, (double)i);
                lua_pushcclosure(L, method[0] == 'S' ? PacketSetData : PacketBlock, method, 2);
                lua_setfield(L, -2, method);
            }
            const int table = lua_gettop(L);

            for (int ref : refs) {
                lua_getref(L, ref);
                if (!lua_isfunction(L, -1)) {
                    lua_pop(L, 1);
                    continue;
                }
                lua_pushvalue(L, table);
                if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
                    const char* error = lua_tostring(L, -1);
                    Roblox::Print(3, "raknet send hook error: %s", error ? error : "unknown error");
                    lua_pop(L, 1);
                }
            }

            lua_settop(L, 0);
        }

        g_dispatchingBatch = 0;

        auto batch = g_batches.find(batchId);
        if (batch != g_batches.end()) {
            for (const HeldPacket& packet : batch->second.packets)
                ResendHeld(packet);
            g_batches.erase(batch);
        }
        return 0;
    }

    static bool DeferCall(lua_State* L, lua_CFunction fn, const char* name, double arg)
    {
        if (!lua_checkstack(L, 2))
            return false;

        const int top = lua_gettop(L);
        bool ok = false;

        lua_State* T = lua_newthread(L);
        if (T && lua_checkstack(T, 4)) {
            lua_getglobal(T, "task");
            if (lua_istable(T, -1)) {
                lua_getfield(T, -1, "defer");
                if (lua_isfunction(T, -1)) {
                    lua_pushcfunction(T, fn, name);
                    lua_pushnumber(T, arg);
                    ok = lua_pcall(T, 2, 0, 0) == LUA_OK;
                }
            }
        }

        lua_settop(L, top);
        return ok;
    }

    static void PumpHeldPackets(lua_State* L)
    {
        const ULONGLONG now = GetTickCount64();

        for (auto it = g_batches.begin(); it != g_batches.end();) {
            if (now - it->second.handedAt < StaleBatchMs)
                break;
            for (const HeldPacket& packet : it->second.packets)
                ResendHeld(packet);
            it = g_batches.erase(it);
        }

        if (!g_heldPending.load(std::memory_order_relaxed))
            return;

        HeldBatch batch;
        {
            std::lock_guard<std::mutex> lock(g_heldMutex);
            batch.packets.swap(g_held);
            g_heldPending.store(false, std::memory_order_relaxed);
        }
        if (batch.packets.empty())
            return;

        if (!hasSendHooks.load(std::memory_order_relaxed) && g_batches.empty()) {
            for (const HeldPacket& packet : batch.packets)
                ResendHeld(packet);
            return;
        }

        const uint64_t batchId = g_nextBatchId++;
        batch.handedAt = now;
        g_batches.emplace(batchId, std::move(batch));

        if (!DeferCall(L, DispatchHeldBatch, "raknet_dispatch", (double)batchId))
            g_batches[batchId].handedAt = 0;
    }

    uint32_t __fastcall rakpeerSendSharedHook(int64_t* _this, int64_t* bitStream, PacketPriority priority, PacketReliability reliability, char orderingChannel, int64_t* systemIdentifier, bool Broadcast, uint32_t ForceReceiptNumber)
    {
        const send_fn_t original = original_raknet_send.load();
        if (!original)
            return 0;

        uint8_t* packetBytes = nullptr;
        uint32_t packetSize = 0;
        if (ReadStream(bitStream, &packetBytes, &packetSize)) {
            if (ObservePacket(packetBytes, packetSize, priority, reliability, orderingChannel, systemIdentifier, Broadcast, ForceReceiptNumber)) {
                desyncDropped.fetch_add(1, std::memory_order_relaxed);
                return 0;
            }

            if ((hasSendHooks.load(std::memory_order_relaxed) || g_heldPending.load(std::memory_order_relaxed))
                && reinterpret_cast<uintptr_t>(_this) == RakPeer_instance.load()
                && HoldPacket(_this, bitStream, packetBytes, packetSize, priority, reliability,
                              orderingChannel, systemIdentifier, Broadcast, ForceReceiptNumber))
                return 0;
        }

        return original(_this, bitStream, priority, reliability, orderingChannel,
                        systemIdentifier, Broadcast, ForceReceiptNumber);
    }

    void* __fastcall rakpeerReceiveHook(int64_t* _this)
    {
        if (!original_raknet_receive)
            return nullptr;

        void* packet = original_raknet_receive(_this);
        if (!packet)
            return nullptr;

        uint8_t* packetBytes = *reinterpret_cast<uint8_t**>(reinterpret_cast<uintptr_t>(packet));
        uint32_t packetSize = *reinterpret_cast<uint32_t*>(reinterpret_cast<uintptr_t>(packet) + 0x08);

        if (!packetBytes || packetSize == 0)
            return packet;

        uint8_t packetId = packetBytes[0];
        ItemType itemId = (packetSize > 1) ? (ItemType)packetBytes[1] : ItemTypeEnd;

        if (!capturePaused)
            RecordPacket(packetId, (uint8_t)itemId, 0, MEDIUM_PRIORITY, RELIABLE_ORDERED, 0, packetSize, packetBytes, false, 0, false, true);

        if (hasReceiveHooks.load(std::memory_order_relaxed)) {
            std::lock_guard<std::mutex> hlock(hooksMutex);
            std::lock_guard<std::mutex> lock(callbackQueueMutex);
            for (auto& cb : OnSharedReceiveCallbacks) {
                if (cb.packetId >= 0 && cb.packetId != (int)packetId)
                    continue;

                QueuedCallback qc;
                qc.ref = cb.ref;
                qc.data.assign(packetBytes, packetBytes + packetSize);
                qc.size = packetSize;
                qc.priority = MEDIUM_PRIORITY;
                qc.reliability = RELIABLE_ORDERED;
                qc.orderingChannel = 0;
                qc.isSend = false;
                callbackQueue.push_back(std::move(qc));
            }
        }

        return packet;
    }

    bool Disabled()
    {
        static int cached = -1;
        if (cached >= 0)
            return cached != 0;

        cached = 0;
        PWSTR appData = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData)) && appData)
        {
            wchar_t path[MAX_PATH];
            if (swprintf_s(path, L"%s\\supQ\\raknet.off", appData) > 0
                && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
            {
                cached = 1;
                SUPQ_LOG_OK("[Raknet] disabled by user setting");
            }
            CoTaskMemFree(appData);
        }
        return cached != 0;
    }

    bool InitializeHook(uintptr_t NetPeerSendJob)
    {
        if (!NetPeerSendJob || Disabled())
            return false;

        const uintptr_t RakPeer = ReadPeer(NetPeerSendJob);
        if (!RakPeer)
            return false;

        void** live = ReadVtable(RakPeer);
        if (!live)
            return false;

        std::lock_guard<std::mutex> lock(g_hookMutex);

        if (live == g_shadow) {
            RakPeer_instance = RakPeer;
            return true;
        }

        void** const genuine = reinterpret_cast<void**>(Offsets::Raknet::RakPeerVTable);
        if (live != genuine)
            return false;

        const auto seen = g_hookedPeers.find(RakPeer);
        if (seen != g_hookedPeers.end() && seen->second == NetPeerSendJob)
            return false;

        if (!g_shadowBuilt) {
            memcpy(g_shadowBlock, genuine - 1, sizeof(g_shadowBlock));
            g_shadow[Offsets::Raknet::idx_Send] = reinterpret_cast<void*>(&rakpeerSendSharedHook);
            original_raknet_send = reinterpret_cast<send_fn_t>(genuine[Offsets::Raknet::idx_Send]);
            g_shadowBuilt = true;
        }

        g_hookedPeers[RakPeer] = NetPeerSendJob;
        *reinterpret_cast<void***>(RakPeer) = g_shadow;
        RakPeer_instance = RakPeer;

        SUPQ_LOG_OK("[Raknet] hooked RakPeer=0x%llX", (unsigned long long)RakPeer);
        return true;
    }

    void StartWatcher()
    {
        if (Disabled())
            return;

        std::thread([] {
            for (;;)
            {
                RtBind();

                const uintptr_t job = FindJob("Net Peer Send");

                if (!job || !InitializeHook(job))
                    RakPeer_instance = 0;

                Sleep(250);
            }
        }).detach();
    }

    int raknet_desync(lua_State* L)
    {
        const int id = (int)luaL_optinteger(L, 2, desyncPacketId.load());
        luaL_argcheck(L, id >= 0 && id <= 0xFF, 2, "packet id must be 0-255");
        desyncPacketId = id;
        desyncing = lua_toboolean(L, 1) != 0;
        return 0;
    }

    static bool RakNetHooked()
    {
        return RakPeer_instance.load() != 0 && original_raknet_send.load() != nullptr;
    }

    int raknet_is_enabled(lua_State* L)
    {
        lua_pushboolean(L, g_rtApp.load() != 0 || RakNetHooked());
        return 1;
    }

    int raknet_status(lua_State* L)
    {
        lua_newtable(L);

        const bool rbxTransport = g_rtApp.load() != 0;
        lua_pushboolean(L, rbxTransport || RakNetHooked());
        lua_setfield(L, -2, "hooked");

        lua_pushstring(L, rbxTransport ? "RbxTransport" : RakNetHooked() ? "RakNet" : "none");
        lua_setfield(L, -2, "transport");

        {
            static const char* const stages[] = {
                "no DataModel", "no DataModel children", "no NetworkClient child",
                "NetworkClient has no connection", "client vtable mismatch", "client+0x890 app is null",
                "app vtable mismatch", "app vptr swap lost a race", "fault while walking", "bound",
            };
            const int stage = g_rtStage.load();
            const uintptr_t seen = g_rtSeen.load();
            const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
            char text[160];
            sprintf_s(text, "%s (seen 0x%llX, rva 0x%llX)", stage >= 0 && stage <= RtBound ? stages[stage] : "?",
                      (unsigned long long)seen, (unsigned long long)(seen >= base ? seen - base : 0));
            lua_pushstring(L, text);
        }
        lua_setfield(L, -2, "rbxBind");

        char peer[32];
        sprintf_s(peer, "0x%llX", (unsigned long long)RakPeer_instance.load());
        lua_pushstring(L, peer);
        lua_setfield(L, -2, "peer");

        lua_pushboolean(L, *reinterpret_cast<const uint8_t*>(Offsets::Raknet::UseRbxTransportClient) != 0);
        lua_setfield(L, -2, "rbxTransportClient");

        lua_pushboolean(L, desyncing.load());
        lua_setfield(L, -2, "desync");

        lua_pushinteger(L, desyncPacketId.load());
        lua_setfield(L, -2, "desyncId");

        lua_pushnumber(L, (double)desyncDropped.load());
        lua_setfield(L, -2, "dropped");

        {
            std::lock_guard<std::mutex> lock(g_addressMutex);
            lua_pushboolean(L, g_haveAddress);
        }
        lua_setfield(L, -2, "haveAddress");

        {
            std::lock_guard<std::mutex> lock(callbackQueueMutex);
            lua_pushinteger(L, (int)callbackQueue.size());
        }
        lua_setfield(L, -2, "queued");

        size_t held = 0;
        {
            std::lock_guard<std::mutex> lock(g_heldMutex);
            held = g_held.size();
        }
        for (auto& [id, batch] : g_batches)
            held += batch.packets.size();
        lua_pushinteger(L, (int)held);
        lua_setfield(L, -2, "held");

        return 1;
    }

    int raknet_add_send_hook(lua_State* L)
    {
        luaL_checktype(L, 1, LUA_TFUNCTION);

        const int ref = lua_ref(L, 1);
        {
            std::lock_guard<std::mutex> lock(hooksMutex);
            OnSharedSendCallbacks.push_back({ ref, -1 });
            hasSendHooks.store(true, std::memory_order_relaxed);
        }

        lua_pushvalue(L, 1);
        return 1;
    }

    static bool RemoveHook(lua_State* L, std::vector<LuaCallbacks>& hooks, std::atomic<bool>& flag)
    {
        if (lua_type(L, 1) != LUA_TFUNCTION)
            return false;

        const void* targetFn = lua_topointer(L, 1);

        std::lock_guard<std::mutex> lock(hooksMutex);
        for (size_t i = 0; i < hooks.size(); i++) {
            lua_getref(L, hooks[i].ref);
            const void* refFn = lua_topointer(L, -1);
            lua_pop(L, 1);

            if (refFn && refFn == targetFn) {
                lua_unref(L, hooks[i].ref);
                hooks.erase(hooks.begin() + i);
                flag.store(!hooks.empty(), std::memory_order_relaxed);
                return true;
            }
        }
        return false;
    }

    int raknet_remove_send_hook(lua_State* L)
    {
        lua_pushboolean(L, RemoveHook(L, OnSharedSendCallbacks, hasSendHooks));
        return 1;
    }

    int raknet_add_receive_hook(lua_State* L)
    {
        luaL_checktype(L, 1, LUA_TFUNCTION);
        const int packetId = (int)luaL_optinteger(L, 2, -1);

        const int ref = lua_ref(L, 1);
        {
            std::lock_guard<std::mutex> lock(hooksMutex);
            OnSharedReceiveCallbacks.push_back({ ref, packetId });
            hasReceiveHooks.store(true, std::memory_order_relaxed);
        }

        lua_pushvalue(L, 1);
        return 1;
    }

    int raknet_remove_receive_hook(lua_State* L)
    {
        lua_pushboolean(L, RemoveHook(L, OnSharedReceiveCallbacks, hasReceiveHooks));
        return 1;
    }

    int raknet_getpacketlog(lua_State* L)
    {
        const double afterSeq = luaL_optnumber(L, 1, 0);
        const size_t maxBytes = static_cast<size_t>((std::clamp)(luaL_optinteger(L, 2, 64), 0, static_cast<int>(MaxStoredPacketBytes)));

        auto snap = SnapshotLog();
        lua_newtable(L);
        int idx = 1;

        for (auto& e : snap) {
            if (static_cast<double>(e.seq) <= afterSeq)
                continue;

            lua_newtable(L);

            lua_pushinteger(L, (int)e.seq);
            lua_setfield(L, -2, "Seq");

            lua_pushinteger(L, (int)e.id);
            lua_setfield(L, -2, "Id");

            lua_pushinteger(L, (int)e.itemId);
            lua_setfield(L, -2, "ItemId");

            lua_pushstring(L, ItemTypeName(e.itemId));
            lua_setfield(L, -2, "ItemName");

            lua_pushinteger(L, (int)e.priority);
            lua_setfield(L, -2, "Priority");

            lua_pushinteger(L, (int)e.reliability);
            lua_setfield(L, -2, "Reliability");

            lua_pushinteger(L, (int)(unsigned char)e.channel);
            lua_setfield(L, -2, "Channel");

            lua_pushinteger(L, (int)e.size);
            lua_setfield(L, -2, "Size");

            lua_pushboolean(L, e.blocked);
            lua_setfield(L, -2, "Blocked");

            lua_pushstring(L, e.rbxTransport ? "RbxTransport" : "RakNet");
            lua_setfield(L, -2, "Transport");

            lua_pushstring(L, e.incoming ? "in" : "out");
            lua_setfield(L, -2, "Direction");

            lua_pushnumber(L, e.timeMs);
            lua_setfield(L, -2, "Time");

            lua_pushboolean(L, e.truncated);
            lua_setfield(L, -2, "Truncated");

            lua_pushstring(L, e.summary.empty() ? ItemTypeName(e.itemId) : e.summary.c_str());
            lua_setfield(L, -2, "Summary");

            lua_newtable(L);
            const uint32_t showSize = static_cast<uint32_t>((std::min)(e.bytes.size(), maxBytes));
            for (uint32_t i = 0; i < showSize; i++) {
                lua_pushinteger(L, (int)e.bytes[i]);
                lua_rawseti(L, -2, i + 1);
            }
            lua_setfield(L, -2, "Bytes");

            if (e.id == 0x83 && (e.itemId == ItemTypePing || e.itemId == ItemTypePingBack) && e.bytes.size() >= 2)
            {
                NetReader pr{ e.bytes.data() + 2, static_cast<uint32_t>(e.bytes.size() - 2), 0 };
                PingItem ping{};
                if (DecodePingItem(pr, ping))
                {
                    lua_newtable(L);
                    lua_pushinteger(L, ping.ExtraStats); lua_setfield(L, -2, "ExtraStats");
                    lua_pushnumber(L, static_cast<double>(ping.Time)); lua_setfield(L, -2, "Time");
                    lua_pushinteger(L, static_cast<int>(ping.FirstSerializeTime)); lua_setfield(L, -2, "FirstSerializeTime");
                    lua_pushnumber(L, ping.RenderFps); lua_setfield(L, -2, "RenderFps");
                    lua_pushnumber(L, ping.PhysicsFps); lua_setfield(L, -2, "PhysicsFps");
                    lua_pushnumber(L, ping.HeartbeatFps); lua_setfield(L, -2, "HeartbeatFps");
                    lua_pushinteger(L, static_cast<int>(ping.VmMarkers)); lua_setfield(L, -2, "VmMarkers");
                    lua_pushinteger(L, static_cast<int>(ping.MemoryMb)); lua_setfield(L, -2, "MemoryMb");
                    lua_pushinteger(L, static_cast<int>(ping.SendStats)); lua_setfield(L, -2, "SendStats");
                    lua_pushboolean(L, ping.TracePresent); lua_setfield(L, -2, "TracePresent");
                    lua_setfield(L, -2, "Ping");
                }
            }

            lua_rawseti(L, -2, idx++);
        }

        return 1;
    }

    int raknet_read(lua_State* L)
    {
        const int limit = static_cast<int>(luaL_optinteger(L, 1, 50));
        auto snap = SnapshotLog();
        const size_t total = snap.size();
        const size_t count = limit > 0 ? (std::min)(total, static_cast<size_t>(limit)) : total;
        const size_t start = total - count;

        lua_createtable(L, static_cast<int>(count), 0);
        int idx = 1;
        for (size_t i = start; i < total; ++i)
        {
            const auto& e = snap[i];
            lua_newtable(L);
            lua_pushinteger(L, (int)e.seq); lua_setfield(L, -2, "Seq");
            lua_pushinteger(L, (int)e.id); lua_setfield(L, -2, "Id");
            lua_pushinteger(L, (int)e.itemId); lua_setfield(L, -2, "ItemId");
            lua_pushstring(L, ItemTypeName(e.itemId)); lua_setfield(L, -2, "ItemName");
            lua_pushinteger(L, (int)e.size); lua_setfield(L, -2, "Size");
            lua_pushstring(L, e.incoming ? "in" : "out"); lua_setfield(L, -2, "Direction");
            lua_pushstring(L, e.summary.empty() ? ItemTypeName(e.itemId) : e.summary.c_str()); lua_setfield(L, -2, "Summary");
            SetPacketPayload(L, -1, e.bytes.data(), static_cast<uint32_t>(e.bytes.size()));
            lua_rawseti(L, -2, idx++);
        }
        return 1;
    }

    int raknet_inspect(lua_State* L)
    {
        const bool hasCallback = lua_isfunction(L, 1);
        const int limit = static_cast<int>(luaL_optinteger(L, hasCallback ? 2 : 1, 50));

        lua_pushcfunction(L, raknet_read, "read");
        lua_pushinteger(L, limit);
        lua_call(L, 1, 1);

        if (hasCallback)
        {
            const int tableIdx = lua_gettop(L);
            const int len = lua_objlen(L, tableIdx);
            for (int i = 1; i <= len; ++i)
            {
                lua_pushvalue(L, 1);
                lua_rawgeti(L, tableIdx, i);
                lua_call(L, 1, 0);
            }
        }

        return 1;
    }

    int raknet_clearpacketlog(lua_State* L)
    {
        ClearLog();
        return 0;
    }

    int raknet_capturepaused(lua_State* L)
    {
        capturePaused = lua_toboolean(L, 1) != 0;
        return 0;
    }

    int raknet_desyncping(lua_State* L)
    {
        return 0;
    }

    bool SendRawPacket(const uint8_t* data, uint32_t size, PacketPriority priority,
                       PacketReliability reliability, char channel)
    {
        if (const uintptr_t app = g_rtApp.load()) {
            if (!data || size == 0)
                return false;
            const uint8_t seen = g_rtChannelForId[data[0]].load(std::memory_order_relaxed);
            const uint32_t rtChannel = seen ? seen - 1u : 1u;
            const bool queued = RtQueueOut(app, rtChannel, std::vector<uint8_t>(data, data + size));
            if (!queued)
                SUPQ_LOG_ERR("[Raknet] SendRawPacket: RbxTransport channel %u not ready", rtChannel);
            return queued;
        }

        const uintptr_t peer = RakPeer_instance.load();
        const send_fn_t original = original_raknet_send.load();
        if (!peer || !original) {
            SUPQ_LOG_ERR("[Raknet] SendRawPacket: RakPeer not hooked yet");
            return false;
        }

        if (!data || size == 0)
            return false;

        alignas(16) uint8_t address[Offsets::Raknet::AddressOrGuidSize];
        {
            std::lock_guard<std::mutex> lock(g_addressMutex);
            if (!g_haveAddress) {
                SUPQ_LOG_ERR("[Raknet] SendRawPacket: no server address captured yet");
                return false;
            }
            memcpy(address, g_address, sizeof(address));
        }

        std::shared_ptr<void> stream = MakeOwnedStream(data, size);

        const uint32_t result = original(
            reinterpret_cast<int64_t*>(peer),
            reinterpret_cast<int64_t*>(&stream),
            priority, reliability, channel,
            reinterpret_cast<int64_t*>(address), false, 0);

        return result != 0;
    }

    int raknet_send_raw(lua_State* L)
    {
        size_t dataLen = 0;
        const char* data = luaL_checklstring(L, 1, &dataLen);

        const int priority = (int)luaL_optinteger(L, 2, HIGH_PRIORITY);
        const int reliability = (int)luaL_optinteger(L, 3, RELIABLE_ORDERED);
        const int channel = (int)luaL_optinteger(L, 4, 0);

        if (dataLen == 0)
            luaL_error(L, "raknet.send: empty packet data");

        lua_pushboolean(L, SendRawPacket(
            reinterpret_cast<const uint8_t*>(data), (uint32_t)dataLen,
            (PacketPriority)priority, (PacketReliability)reliability, (char)channel));
        return 1;
    }

    static LuaBitStream* checkBitStream(lua_State* L, int idx = 1)
    {
        return (LuaBitStream*)luaL_checkudata(L, idx, "RakBitStream");
    }

    int raknet_bitstream_new(lua_State* L)
    {
        void* mem = lua_newuserdatadtor(L, sizeof(LuaBitStream), [](lua_State*, void* p) {
            static_cast<LuaBitStream*>(p)->~LuaBitStream();
        });
        new (mem) LuaBitStream();

        luaL_getmetatable(L, "RakBitStream");
        lua_setmetatable(L, -2);
        return 1;
    }

    int raknet_bitstream_write(lua_State* L)
    {
        LuaBitStream* bs = checkBitStream(L);
        size_t len = 0;
        const char* data = luaL_checklstring(L, 2, &len);
        bs->buffer.insert(bs->buffer.end(), data, data + len);
        return 0;
    }

    int raknet_bitstream_writestring(lua_State* L)
    {
        LuaBitStream* bs = checkBitStream(L);
        size_t len = 0;
        const char* data = luaL_checklstring(L, 2, &len);

        uint32_t val = (uint32_t)len;
        while (val >= 0x80) {
            bs->buffer.push_back((uint8_t)(val | 0x80));
            val >>= 7;
        }
        bs->buffer.push_back((uint8_t)val);

        bs->buffer.insert(bs->buffer.end(), data, data + len);
        return 0;
    }

    int raknet_bitstream_writeu8(lua_State* L)
    {
        LuaBitStream* bs = checkBitStream(L);
        bs->buffer.push_back((uint8_t)luaL_checkinteger(L, 2));
        return 0;
    }

    int raknet_bitstream_writeu16(lua_State* L)
    {
        LuaBitStream* bs = checkBitStream(L);
        uint16_t v = (uint16_t)luaL_checkinteger(L, 2);
        bs->buffer.push_back((uint8_t)(v & 0xFF));
        bs->buffer.push_back((uint8_t)((v >> 8) & 0xFF));
        return 0;
    }

    int raknet_bitstream_writeu32(lua_State* L)
    {
        LuaBitStream* bs = checkBitStream(L);
        uint32_t v = (uint32_t)luaL_checknumber(L, 2);
        for (int i = 0; i < 4; i++)
            bs->buffer.push_back((uint8_t)(v >> (i * 8)));
        return 0;
    }

    int raknet_bitstream_writef32(lua_State* L)
    {
        LuaBitStream* bs = checkBitStream(L);
        float v = (float)luaL_checknumber(L, 2);
        uint32_t iv;
        memcpy(&iv, &v, 4);
        for (int i = 0; i < 4; i++)
            bs->buffer.push_back((uint8_t)(iv >> (i * 8)));
        return 0;
    }

    int raknet_bitstream_writef64(lua_State* L)
    {
        LuaBitStream* bs = checkBitStream(L);
        double v = luaL_checknumber(L, 2);
        uint64_t iv;
        memcpy(&iv, &v, 8);
        for (int i = 0; i < 8; i++)
            bs->buffer.push_back((uint8_t)((iv >> (i * 8)) & 0xFF));
        return 0;
    }

    int raknet_bitstream_clear(lua_State* L)
    {
        LuaBitStream* bs = checkBitStream(L);
        bs->buffer.clear();
        bs->sent = false;
        return 0;
    }

    int raknet_bitstream_getlength(lua_State* L)
    {
        LuaBitStream* bs = checkBitStream(L);
        lua_pushinteger(L, (int)bs->buffer.size());
        return 1;
    }

    int raknet_bitstream_send(lua_State* L)
    {
        LuaBitStream* bs = checkBitStream(L);
        if (bs->buffer.empty())
            luaL_error(L, "BitStream:Send - buffer is empty");

        const int priority = (int)luaL_optinteger(L, 2, HIGH_PRIORITY);
        const int reliability = (int)luaL_optinteger(L, 3, RELIABLE_ORDERED);
        const int channel = (int)luaL_optinteger(L, 4, 0);

        const bool ok = SendRawPacket(bs->buffer.data(), (uint32_t)bs->buffer.size(),
            (PacketPriority)priority, (PacketReliability)reliability, (char)channel);
        bs->sent = ok;

        lua_pushboolean(L, ok);
        return 1;
    }

    int raknet_bitstream_tostring(lua_State* L)
    {
        LuaBitStream* bs = checkBitStream(L);
        lua_pushlstring(L, reinterpret_cast<const char*>(bs->buffer.data()), bs->buffer.size());
        return 1;
    }

    int rakensend_bitstream(lua_State* L)
    {
        return raknet_bitstream_send(L);
    }

    void RegisterLibrary(lua_State* L)
    {
        if (Disabled())
            return;

        desyncing = false;

        {
            std::lock_guard<std::mutex> lock(callbackQueueMutex);
            callbackQueue.clear();
        }
        {
            std::lock_guard<std::mutex> lock(g_heldMutex);
            g_held.clear();
            g_heldPending.store(false, std::memory_order_relaxed);
        }
        g_batches.clear();
        g_dispatchingBatch = 0;
        {
            std::lock_guard<std::mutex> lock(hooksMutex);
            OnSharedSendCallbacks.clear();
            OnSharedReceiveCallbacks.clear();
            hasSendHooks.store(false, std::memory_order_relaxed);
            hasReceiveHooks.store(false, std::memory_order_relaxed);
        }

        luaL_newmetatable(L, "RakBitStream");
        lua_newtable(L);
        Utils::AddTableFunction(L, "Write", raknet_bitstream_write);
        Utils::AddTableFunction(L, "WriteString", raknet_bitstream_writestring);
        Utils::AddTableFunction(L, "WriteU8", raknet_bitstream_writeu8);
        Utils::AddTableFunction(L, "WriteU16", raknet_bitstream_writeu16);
        Utils::AddTableFunction(L, "WriteU32", raknet_bitstream_writeu32);
        Utils::AddTableFunction(L, "WriteFloat", raknet_bitstream_writef32);
        Utils::AddTableFunction(L, "WriteDouble", raknet_bitstream_writef64);
        Utils::AddTableFunction(L, "Clear", raknet_bitstream_clear);
        Utils::AddTableFunction(L, "GetLength", raknet_bitstream_getlength);
        Utils::AddTableFunction(L, "Send", raknet_bitstream_send);
        Utils::AddTableFunction(L, "ToString", raknet_bitstream_tostring);
        lua_setfield(L, -2, "__index");
        lua_pop(L, 1);

        lua_newtable(L);

        Utils::AddTableFunction(L, "desync", raknet_desync);
        Utils::AddTableFunction(L, "add_send_hook", raknet_add_send_hook);
        Utils::AddTableFunction(L, "remove_send_hook", raknet_remove_send_hook);
        Utils::AddTableFunction(L, "add_receive_hook", raknet_add_receive_hook);
        Utils::AddTableFunction(L, "remove_receive_hook", raknet_remove_receive_hook);
        Utils::AddTableFunction(L, "getpacketlog", raknet_getpacketlog);
        Utils::AddTableFunction(L, "read", raknet_read);
        Utils::AddTableFunction(L, "inspect", raknet_inspect);
        Utils::AddTableFunction(L, "clearpacketlog", raknet_clearpacketlog);
        Utils::AddTableFunction(L, "capturepaused", raknet_capturepaused);
        Utils::AddTableFunction(L, "desyncping", raknet_desyncping);
        Utils::AddTableFunction(L, "status", raknet_status);
        Utils::AddTableFunction(L, "is_enabled", raknet_is_enabled);
        Utils::AddTableFunction(L, "send", raknet_send_raw);
        Utils::AddTableFunction(L, "Send", rakensend_bitstream);

        lua_newtable(L);
        Utils::AddTableFunction(L, "new", raknet_bitstream_new);
        lua_setfield(L, -2, "BitStream");

        lua_setglobal(L, "raknet");

        Utils::AddFunction(L, "add_send_hook", raknet_add_send_hook);
        Utils::AddFunction(L, "remove_send_hook", raknet_remove_send_hook);
        Utils::AddFunction(L, "add_receive_hook", raknet_add_receive_hook);
        Utils::AddFunction(L, "remove_receive_hook", raknet_remove_receive_hook);
    }
}
