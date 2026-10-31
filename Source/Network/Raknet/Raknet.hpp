#pragma once

#include <lstate.h>
#include <lualib.h>

#include <cstdint>
#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <atomic>

namespace Raknet
{
    enum PacketPriority
    {
        IMMEDIATE_PRIORITY,
        HIGH_PRIORITY,
        MEDIUM_PRIORITY,
        LOW_PRIORITY,
        NUMBER_OF_PRIORITIES
    };

    enum PacketReliability
    {
        UNRELIABLE,
        UNRELIABLE_SEQUENCED,
        RELIABLE,
        RELIABLE_ORDERED,
        RELIABLE_SEQUENCED,
        UNRELIABLE_WITH_ACK_RECEIPT,
        RELIABLE_WITH_ACK_RECEIPT,
        RELIABLE_ORDERED_WITH_ACK_RECEIPT,
        NUMBER_OF_RELIABILITIES
    };

    enum ItemType : int32_t
    {
        ItemTypeEnd = 0x0,
        ItemTypeDelete = 0x1,
        ItemTypeNew = 0x2,
        ItemTypeChangeProperty = 0x3,
        ItemTypeMarker = 0x4,
        ItemTypePing = 0x5,
        ItemTypePingBack = 0x6,
        ItemTypeEventInvocation = 0x7,
        ItemTypeRequestCharacter = 0x8,
        ItemTypeRocky = 0x9,
        ItemTypeCFrameAcknowledgement = 0xA,
        ItemTypeJoinData = 0xB,
        ItemTypeUpdateClientQuota = 0xC,
        ItemTypeStreamData = 0xD,
        ItemTypeRegionRemoval = 0xE,
        ItemTypeInstanceRemoval = 0xF,
        ItemTypeTag = 0x10,
        ItemTypeStats = 0x11,
        ItemTypeHash = 0x12,
        ItemTypeResolveDeferredTree = 0x13,
        ItemTypeStreamDataInfo = 0x14,
        ItemTypeChangeAttribute = 0x15,
        ItemTypeJoinDataV2 = 0x16,
        ItemTypeStreamTerrain = 0x17,
        ItemTypeStreamPrefetchInfo = 0x18,
        ItemClientQoutaStreamUpdate = 0x19,
        ItemClientQos = 0x1F,
        ItemDeferredSharedString = 0x22,
        ItemTypeISRTimestamp = 0x23,
        ItemStreamPinData = 0x25
    };

    struct LuaCallbacks
    {
        int ref;
        int packetId;
    };

    struct PacketLogEntry
    {
        uint64_t seq;
        uint8_t id;
        uint8_t itemId;
        PacketPriority priority;
        PacketReliability reliability;
        char channel;
        uint32_t size;
        uint32_t bitsUsed;
        bool broadcast;
        uint32_t forceReceipt;
        bool blocked;
        bool truncated;
        bool rbxTransport;
        bool incoming;
        double timeMs;
        std::string summary;
        std::vector<uint8_t> bytes;
    };

    struct PingItem
    {
        bool Valid = false;
        uint8_t ExtraStats = 0;
        uint64_t Time = 0;
        uint32_t FirstSerializeTime = 0;
        float RenderFps = 0.0f;
        float PhysicsFps = 0.0f;
        float HeartbeatFps = 0.0f;
        uint32_t VmMarkers = 0;
        uint32_t MemoryMb = 0;
        uint32_t SendStats = 0;
        bool TracePresent = false;
    };

    struct NetReader
    {
        const uint8_t* d;
        uint32_t n;
        uint32_t pos;

        bool read(void* out, uint32_t len);
        bool u8(uint8_t& v);
        bool u16(uint16_t& v);
        bool u32(uint32_t& v);
        bool u64(uint64_t& v);
        bool f32(float& v);
        bool varint(uint32_t& v);
    };

    inline std::vector<LuaCallbacks> OnSharedSendCallbacks;
    inline std::vector<LuaCallbacks> OnSharedReceiveCallbacks;
    inline std::mutex hooksMutex;

    struct QueuedCallback {
        int ref;
        std::vector<uint8_t> data;
        uint32_t size;
        PacketPriority priority;
        PacketReliability reliability;
        char orderingChannel;
        bool isSend;
    };

    inline std::mutex callbackQueueMutex;
    inline std::vector<QueuedCallback> callbackQueue;

    inline std::mutex logMutex;
    inline std::deque<PacketLogEntry> packetLog;
    inline uint64_t packetSeq = 0;
    inline std::atomic<bool> capturePaused{ false };
    inline std::atomic<bool> desyncing{ false };
    inline std::atomic<int> desyncPacketId{ 0xA1 };
    inline std::atomic<uint64_t> desyncDropped{ 0 };

    static constexpr size_t MaxPacketLogEntries = 200;
    static constexpr size_t MaxStoredPacketBytes = 512;
    static constexpr size_t MaxQueuedCallbacks = 1024;

    using send_fn_t = uint32_t(__fastcall*)(int64_t*, int64_t*, PacketPriority, PacketReliability, char, int64_t*, bool, uint32_t);
    inline std::atomic<send_fn_t> original_raknet_send{ nullptr };

    using receive_fn_t = void*(__fastcall*)(int64_t* _this);
    inline receive_fn_t original_raknet_receive = nullptr;

    inline std::atomic<uintptr_t> RakPeer_instance{ 0 };

    struct LuaBitStream {
        std::vector<uint8_t> buffer;
        bool sent = false;
    };

    const char* ItemTypeName(uint8_t id);
    std::string DecodeReplicationItem(const uint8_t* b, uint32_t n);
    void RecordPacket(uint8_t id, uint8_t itemId, uint32_t bitsUsed, PacketPriority priority, PacketReliability reliability, char channel, uint32_t size, const uint8_t* bytes, bool broadcast, uint32_t forceReceipt, bool rbxTransport = false, bool incoming = false);
    std::vector<PacketLogEntry> SnapshotLog();
    void ClearLog();
    void MarkLastPacketBlocked();
    void pushPacketUserdata(lua_State* L, uint8_t* bytes, uint32_t size,
                            PacketPriority priority, PacketReliability reliability, char orderingChannel);

    void ProcessCallbacks(lua_State* L);

    uint32_t __fastcall rakpeerSendSharedHook(int64_t* _this, int64_t* bitStream, PacketPriority priority, PacketReliability reliability, char orderingChannel, int64_t* systemIdentifier, bool Broadcast, uint32_t ForceReceiptNumber);
    void* __fastcall rakpeerReceiveHook(int64_t* _this);

    bool Disabled();

    bool InitializeHook(uintptr_t NetPeerSendJob);

    void StartWatcher();

    int raknet_desync(lua_State* L);
    int raknet_add_send_hook(lua_State* L);
    int raknet_remove_send_hook(lua_State* L);
    int raknet_add_receive_hook(lua_State* L);
    int raknet_remove_receive_hook(lua_State* L);
    int raknet_getpacketlog(lua_State* L);
    int raknet_clearpacketlog(lua_State* L);
    int raknet_capturepaused(lua_State* L);
    int raknet_desyncping(lua_State* L);
    int raknet_status(lua_State* L);

    int raknet_send_raw(lua_State* L);
    int raknet_bitstream_new(lua_State* L);
    int raknet_bitstream_write(lua_State* L);
    int raknet_bitstream_writestring(lua_State* L);
    int raknet_bitstream_writeu8(lua_State* L);
    int raknet_bitstream_writeu16(lua_State* L);
    int raknet_bitstream_writeu32(lua_State* L);
    int raknet_bitstream_writef32(lua_State* L);
    int raknet_bitstream_writef64(lua_State* L);
    int raknet_bitstream_clear(lua_State* L);
    int raknet_bitstream_getlength(lua_State* L);
    int raknet_bitstream_send(lua_State* L);
    int raknet_bitstream_tostring(lua_State* L);
    int rakensend_bitstream(lua_State* L);

    bool SendRawPacket(const uint8_t* data, uint32_t size, PacketPriority priority = HIGH_PRIORITY,
                       PacketReliability reliability = RELIABLE_ORDERED, char channel = 0);

    void RegisterLibrary(lua_State* L);
}
