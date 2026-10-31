#pragma once

// Roblox 0.745.0.7451137 (version-5a21b87bbc2641cf).
// Values from the supplied 0.745 Offsets.hpp, cross-checked against the supQ
// runtime dump of that image. Module-only fields keep their names.

#include <cstdint>
#include <memory>
#include <Windows.h>
#include <string>

struct lua_State;
struct YieldState;
struct YieldingLuaThread;

#define REBASE(Address) (Address + reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr)))

struct LookupResult
{
    std::uint32_t index;
    std::uint8_t missing;
    std::uint8_t reserved[3];
};

namespace Offsets
{
    const uintptr_t Print = REBASE(0x1C50690);
    const uintptr_t OpcodeLookupTable = REBASE(0x71CFBA0);
    const uintptr_t ScriptContextResume = REBASE(0x4270490);
    const uintptr_t GetLuaStateForInstance = REBASE(0x41D0080);
    inline uint64_t luaD_throw = REBASE(0x25F6560);

    const uintptr_t TaskDefer = REBASE(0x432E240);

    namespace reflections
    {
        const uintptr_t CastArgs = REBASE(0x4134690);
    }

    namespace Properties
    {
        const uintptr_t CallbackOffsetInDescriptor = 0x88;
        const uintptr_t CallbackOwnerOffset = 0x18;
        const uintptr_t CallbackFunctionOffset = 0x38;
        const uintptr_t LiveThreadRefOffset = 0x18;
        const uintptr_t CallbackThreadOffset = 0x28;
        const uintptr_t LuaStateGlobalOffset = 0x38;
        const uintptr_t FunctionRefIdOffset = 0x34;
        const uintptr_t AlternateHolderAdjustment = 0x1F0;
    }

    namespace Scheduler
    {
        const uintptr_t SchedulerPointer = REBASE(0x8C9D698);
        const uintptr_t TaskScheduler = REBASE(0x8C9D698);
        const uintptr_t JobsStart = 0xC8;
        const uintptr_t JobsEnd = 0xD0;
        const uintptr_t JobName = 0x18;
        const uintptr_t Fps = 0xB0;
    }

    namespace Signals
    {
        const uintptr_t FireTouchInterest = REBASE(0x1498FE0);
        const uintptr_t DisconnectConnect = REBASE(0x419A260);
    }

    namespace Flags
    {
        inline const uintptr_t FireMouseClick = REBASE(0x3C00500);
        inline const uintptr_t FireRightMouseClick = REBASE(0x3C006A0);
        inline const uintptr_t FireMouseHoverEnter = REBASE(0x3C01A70);
        inline const uintptr_t FireMouseHoverLeave = REBASE(0x3C01C10);
        inline const uintptr_t FireProximityPrompt = REBASE(0x30A8980);
    }

    namespace Luau
    {
        const uintptr_t Luau_Execute = REBASE(0x26228C0);
        const uintptr_t LuaO_NilObject = REBASE(0x6735848);
        const uintptr_t LuaH_DummyNode = REBASE(0x67352E8);
        const uintptr_t ScriptContextToLuaState = 0x220;
    }

    namespace DataModel
    {
        const uintptr_t GameLoaded = 0x588;
        const uintptr_t ScriptContext = 0x440;
        const uintptr_t FakeDataModelToDataModel = 0x1F8;
        const uintptr_t FakeDataModelPointer = REBASE(0x8D06600);
        const uintptr_t PlaceId = 0x180;
    }

    namespace FFlags
    {
        inline const uintptr_t GetFFlag = REBASE(0x4A38860);
        inline const uintptr_t FFlagPointer = REBASE(0x83EBA48);

        inline const uintptr_t BooleanType = REBASE(0x7054590);
        inline const uintptr_t IntegerType = REBASE(0x70548D8);

        inline const uintptr_t BooleanValueType = REBASE(0x7054478);
        inline const uintptr_t IntegerValueType = REBASE(0x70547C0);

        inline constexpr uintptr_t ValueLivePointer = 0x20;
        inline constexpr uintptr_t ValueCached = 0x28;
    }

    namespace Extras
    {
        const uintptr_t k_table = REBASE(0x838A130);
    }

    namespace ExtraSpace
    {
        const uintptr_t RequireBypass = 0xB50;
        const uintptr_t ScriptContextToResume = 0xA50;
    }

    namespace Scripts
    {
        const uintptr_t LocalScriptByteCode = 0x178;
        const uintptr_t ModuleScriptByteCode = 0x120;
    }

    namespace InstanceBridge
    {
        const uintptr_t Push = REBASE(0x41B76C0);
    }

    namespace ClassDescriptor
    {
        // 0.745 `Name::lookup(const char*)` wrapper. It lazy-loads the intern
        // table from [0x83E4B78] (constructed by 0x2A27020) and tail-jumps to
        // the hash lookup at 0x2A270E0; 0x2A273F0 is the string-object overload.
        const uintptr_t HashLookup = REBASE(0x2A27430);
        const uintptr_t GetProperty = REBASE(0x1C8E7F0);
    }

    namespace RobloxThread
    {
        const uintptr_t IdentityPtr = REBASE(0x83B6678);
        const uintptr_t GetTlsPointer = 0xC700;
    }

    namespace Capabilities
    {
        const uintptr_t GetCapabilities = REBASE(0x1C905D0);
        const uintptr_t Capabilities = 0x30;
    }

    namespace PropertyValue
    {
        const uintptr_t OperatorDelete = REBASE(0x4A03250);
        const uintptr_t NetAssetRefDtor = REBASE(0x1C82180);
        const uintptr_t AssetContentMapDtor = REBASE(0xA6FB10);
    }

    namespace Actor
    {
        const uintptr_t ParallelFlag = 0x53D;
        const uintptr_t Parent = 0x60;
    }

    namespace signal_offsets
    {
        constexpr uintptr_t signature = 0x60;

        namespace connection
        {
            constexpr uintptr_t next = 0x10;
            constexpr uintptr_t enabled = 0x20;
            constexpr uintptr_t slot_wrapper = 0x30;
            constexpr uintptr_t slot_wrapper_self = 0x38;
        }

        namespace slot_wrapper
        {
            constexpr uintptr_t thread = 0x28;
            constexpr uintptr_t function_id = 0x30;
            constexpr uintptr_t function_id_fallback = 0x34;
            constexpr uintptr_t weak_object_ref = 0xA0;
            constexpr uintptr_t weak_object_ref_fallback = 0x90;
        }
    }

    namespace PropertyDescriptor
    {
        const uintptr_t Name = 0x8;
        const uintptr_t ReadCapabilities = 0x38;
        const uintptr_t TypeInfo = 0x70;
        const uintptr_t GetSet = 0xB0;
    }

    namespace Instance
    {
        const uintptr_t PropertyDescriptorBitFlags = 0xAC;
        const uintptr_t Properties = 0x28;
        const uintptr_t ClassDescriptor = 0x18;
        const uintptr_t ClassName = 0x8;
        const uintptr_t Children = 0x70;
        const uintptr_t Name = 0x68;
        const uintptr_t NameString = 0x0;
    }

    namespace Raknet
    {
        const uintptr_t RakPeer = 0x1E0;
        const uintptr_t RakPeerVTable = REBASE(0x6F5F080);
        const uintptr_t RakPeerVTableSize = 0x3E0;
        const uintptr_t idx_Send = 20;
        const uintptr_t NetworkStreamVtable = REBASE(0x6F46C88);
        const size_t AddressOrGuidSize = 0x30;

        const uintptr_t UseRbxTransportClient = REBASE(0x7FE8DD8);
        const uintptr_t FailureFallbackToRbxTransport = REBASE(0x8000A20);
    }

    namespace RbxTransport
    {
        const uintptr_t ClientVTable = REBASE(0x6FC3B30);
        const uintptr_t AppVTable = REBASE(0x6F525C0);
        const size_t AppVTableSlots = 28;
        const size_t idx_AppDestructor = 0;
        const size_t idx_AppSend = 9;
        const size_t idx_AppReceive = 12;
        const size_t idx_AppReceiveMax = 13;

        const uintptr_t ChannelConfig = REBASE(0x3A99470);
        const uintptr_t StreamCreate = REBASE(0x53260A0);
        const uintptr_t StreamCopy = REBASE(0x5326570);
        const uintptr_t StreamAppend = REBASE(0x2126160);
        const uintptr_t StreamDestroy = REBASE(0x20E91F0);

        const uintptr_t NetworkClientReplicator = 0xFA8;
        const uintptr_t ReplicatorConnection = 0x110;
        const uintptr_t NetworkClientConnection = 0xFB0;
        const uintptr_t ClientChannelMask = 0x860;
        const uintptr_t ClientApp = 0x870;

        const size_t StreamSize = 0x58;
        const size_t ReceivedStreamSize = 0x70;
        const uintptr_t StreamVecBegin = 0x0;
        const uintptr_t StreamVecEnd = 0x8;
        const uintptr_t StreamPoolBytes = 0x18;
        const uintptr_t StreamPoolLength = 0x20;
        const uintptr_t StreamPooled = 0x38;
        const uintptr_t StreamPrefix = 0x40;
        const uint32_t ChannelCount = 14;
    }
}

namespace Roblox
{
    inline uintptr_t* k_table = reinterpret_cast<uintptr_t*>(Offsets::Extras::k_table);
    inline auto Print = (uintptr_t(*)(int, const char*, ...))Offsets::Print;
    inline auto Luau_Execute = (void(__fastcall*)(lua_State*))Offsets::Luau::Luau_Execute;
    inline auto GetLuaStateForInstance = (lua_State * (__fastcall*)(uint64_t, uint64_t*, uint64_t*))Offsets::GetLuaStateForInstance;
    inline auto ScriptContextResume = (uint64_t(__fastcall*)(uint64_t, YieldState*, YieldingLuaThread**, uint32_t, uint8_t, uint64_t))Offsets::ScriptContextResume;

    namespace Signal
    {
        inline auto FireTouchInterest = (void(__fastcall*)(uintptr_t, uintptr_t, uintptr_t, bool, bool))Offsets::Signals::FireTouchInterest;
    }

    namespace InstanceBridge
    {
        inline auto UintPtr = (void(__fastcall*)(lua_State*, std::uintptr_t))Offsets::InstanceBridge::Push;
        inline auto Shared = (void(__fastcall*)(lua_State*, std::shared_ptr<std::uintptr_t*>))Offsets::InstanceBridge::Push;
        inline auto Void = (void(__fastcall*)(lua_State*, void**))Offsets::InstanceBridge::Push;
        inline auto Void2 = (void(__fastcall*)(lua_State*, void*))Offsets::InstanceBridge::Push;
        inline auto Weak = (void(__fastcall*)(lua_State*, std::weak_ptr<std::uintptr_t>))Offsets::InstanceBridge::Push;
    }

    using fireclick_t = void(__fastcall*)(void* clickDetector, float distance, void* player);
    inline auto FireMouseClick = reinterpret_cast<fireclick_t>(Offsets::Flags::FireMouseClick);
    inline auto FireRightMouseClick = reinterpret_cast<fireclick_t>(Offsets::Flags::FireRightMouseClick);

    using firehover_t = void(__fastcall*)(void* clickDetector, void* player);
    inline auto FireMouseHover = reinterpret_cast<firehover_t>(Offsets::Flags::FireMouseHoverEnter);
    inline auto FireMouseHoverLeave = reinterpret_cast<firehover_t>(Offsets::Flags::FireMouseHoverLeave);

    inline auto CastArgs = reinterpret_cast<uintptr_t(__fastcall*)(lua_State*, int, void*, bool, int)>(Offsets::reflections::CastArgs);
    inline auto GetCapabilities = (__int64(__fastcall*)(int*))Offsets::Capabilities::GetCapabilities;

    using TGetIdentityStruct = uintptr_t(__fastcall*)(uintptr_t);
    inline auto GetIdentityStruct = reinterpret_cast<TGetIdentityStruct>(Offsets::RobloxThread::GetTlsPointer);

    inline auto FireProximityPrompt = (void(__thiscall*)(uintptr_t))Offsets::Flags::FireProximityPrompt;
    inline auto FireTouchInterest = (void(__fastcall*)(uintptr_t, uintptr_t, uintptr_t, bool, bool))Offsets::Signals::FireTouchInterest;

    inline auto PushInstance = (void(__fastcall*)(lua_State*, void*))Offsets::InstanceBridge::Push;
    inline auto DisconnectConnection = (void(__fastcall*)(uintptr_t*))Offsets::Signals::DisconnectConnect;

    using GetValueFn = bool(__fastcall*)(void* registry, const std::string& name, std::string* output, bool typecheck);
    inline auto GetFFlag = reinterpret_cast<GetValueFn>(Offsets::FFlags::GetFFlag);

    using HashLookupT = uintptr_t(*)(const char*);
    using GetPropertyT = uintptr_t(*)(uintptr_t, uintptr_t*);

    inline GetPropertyT GetProperty = reinterpret_cast<GetPropertyT>(Offsets::ClassDescriptor::GetProperty);
    inline HashLookupT HashLookup = reinterpret_cast<HashLookupT>(Offsets::ClassDescriptor::HashLookup);

    inline auto Void2 = reinterpret_cast<void(__fastcall*)(lua_State*, void*)>(Offsets::InstanceBridge::Push);

    using TaskDeferT = int(__fastcall*)(lua_State*);
    inline TaskDeferT TaskDefer = reinterpret_cast<TaskDeferT>(Offsets::TaskDefer);

    using LuaD_ThrowT = void(__fastcall*)(lua_State*, int);
    inline LuaD_ThrowT LuaD_Throw = reinterpret_cast<LuaD_ThrowT>(Offsets::luaD_throw);

    using InternT = std::uintptr_t(__fastcall*)(const char*);
    using FindBucketT = LookupResult * (__fastcall*)(std::uintptr_t, LookupResult*, const std::uintptr_t*, std::uint32_t);

    inline std::uintptr_t InternName(const char* name)
    {
        const auto function = reinterpret_cast<InternT>(Offsets::ClassDescriptor::HashLookup);
        return function(name);
    }

    inline LookupResult* FindBucket(std::uintptr_t table, LookupResult* output, const std::uintptr_t* key, std::uint32_t mode)
    {
        const auto function = reinterpret_cast<FindBucketT>(Offsets::ClassDescriptor::GetProperty);
        return function(table, output, key, mode);
    }
}
