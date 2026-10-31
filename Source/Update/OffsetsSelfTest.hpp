#pragma once

#include "Update/Offsets.hpp"
#include <lua.h>
#include <lstate.h>
#include <lobject.h>

#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>
#include <Windows.h>
#include <Psapi.h>

namespace OffsetsSelfTest
{
    enum class Sev { Pass, Warn, Fail };

    struct Result
    {
        const char* group;
        std::string name;
        Sev         sev;
        std::string detail;
    };

    struct Report
    {
        std::vector<Result> results;
        int passed = 0, warned = 0, failed = 0;

        void add(const char* group, std::string name, Sev sev, std::string detail)
        {
            results.push_back({ group, std::move(name), sev, std::move(detail) });
            if (sev == Sev::Pass) ++passed;
            else if (sev == Sev::Warn) ++warned;
            else ++failed;
        }
    };

    namespace detail
    {
        inline std::string fmt(const char* f, ...)
        {
            char buf[512];
            va_list ap; va_start(ap, f);
            _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, f, ap);
            va_end(ap);
            return std::string(buf);
        }

        inline bool SafeReadBuf(uintptr_t addr, void* out, size_t n)
        {
            if (!addr || !n) return false;
            SIZE_T got = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(addr), out, n, &got))
                return false;
            return got == n;
        }

        template <typename T> inline bool SafeRead(uintptr_t addr, T& out)
        {
            return SafeReadBuf(addr, &out, sizeof(T));
        }

        template <typename T> inline T SafeReadOr(uintptr_t addr, T fallback)
        {
            T v{};
            return SafeRead(addr, v) ? v : fallback;
        }

        inline bool LooksLikePointer(uintptr_t p)
        {
            if (p < 0x10000 || p > 0x7FFFFFFFFFFFull) return false;
            if (p & 7) return false;
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery(reinterpret_cast<LPCVOID>(p), &mbi, sizeof(mbi))) return false;
            if (mbi.State != MEM_COMMIT) return false;
            return !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
        }

        enum class SecKind { Missing, Code, Data, Other };

        inline uintptr_t ImageBase()
        {
            return reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
        }

        inline uint32_t ImageSize()
        {
            MODULEINFO mi{};
            if (!GetModuleInformation(GetCurrentProcess(), GetModuleHandleA(nullptr), &mi, sizeof(mi)))
                return 0;
            return mi.SizeOfImage;
        }

        inline SecKind ClassifyRva(uintptr_t rva)
        {
            const uintptr_t base = ImageBase();
            const uint32_t  size = ImageSize();
            if (!base || !size || !rva || rva >= size) return SecKind::Missing;

            auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE) return SecKind::Missing;
            auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE) return SecKind::Missing;

            auto sec = IMAGE_FIRST_SECTION(nt);
            for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
            {
                const uintptr_t lo = sec->VirtualAddress;
                const uintptr_t hi = lo + (sec->Misc.VirtualSize ? sec->Misc.VirtualSize : sec->SizeOfRawData);
                if (rva < lo || rva >= hi) continue;
                if (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) return SecKind::Code;
                if (sec->Characteristics & (IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_CNT_UNINITIALIZED_DATA))
                    return SecKind::Data;
                return SecKind::Other;
            }
            return SecKind::Missing;
        }

        inline const char* KindName(SecKind k)
        {
            switch (k)
            {
            case SecKind::Code:  return "code";
            case SecKind::Data:  return "data";
            case SecKind::Other: return "other";
            default:             return "outside-image";
            }
        }

        inline bool ReadStdString(uintptr_t str, std::string& out, size_t capLimit = 0x4000)
        {
            if (!LooksLikePointer(str)) return false;
            uint64_t len = 0, cap = 0;
            if (!SafeRead(str + 0x10, len)) return false;
            if (!SafeRead(str + 0x18, cap)) return false;
            if (cap > capLimit || len > cap) return false;

            uintptr_t buf = str;
            if (cap >= 0x10 && !SafeRead(buf, buf)) return false;
            if (!len) { out.clear(); return true; }
            out.assign(static_cast<size_t>(len), '\0');
            return SafeReadBuf(buf, out.data(), static_cast<size_t>(len));
        }

        inline bool ReadRbxString(uintptr_t holder, std::string& out, size_t capLimit = 0x4000)
        {
            return LooksLikePointer(holder) && ReadStdString(holder + 0x8, out, capLimit);
        }

        inline bool IsPrintable(const std::string& s)
        {
            if (s.empty() || s.size() > 256) return false;
            return std::all_of(s.begin(), s.end(),
                               [](unsigned char c) { return c >= 0x20 && c < 0x7F; });
        }

        inline bool ClassNameOf(uintptr_t inst, std::string& out)
        {
            uintptr_t cd = 0;
            if (!SafeRead(inst + Offsets::Instance::ClassDescriptor, cd)) return false;
            if (!LooksLikePointer(cd)) return false;
            uintptr_t node = 0;
            if (!SafeRead(cd + Offsets::Instance::ClassName, node)) return false;
            return ReadStdString(node, out);
        }
    }

    inline void CheckAddresses(Report& r)
    {
        using namespace detail;
        const char* G = "address";

        if (!ImageBase()) { r.add(G, "main module", Sev::Fail, "GetModuleHandle(nullptr) failed"); return; }

        struct Entry { const char* name; uintptr_t abs; SecKind want; };
        const uintptr_t base = ImageBase();

        const Entry table[] = {
            { "Print",                    Offsets::Print,                            SecKind::Code },
            { "ScriptContextResume",      Offsets::ScriptContextResume,              SecKind::Code },
            { "GetLuaStateForInstance",   Offsets::GetLuaStateForInstance,           SecKind::Code },
            { "luaD_throw",               (uintptr_t)Offsets::luaD_throw,            SecKind::Code },
            { "TaskDefer",                Offsets::TaskDefer,                        SecKind::Code },
            { "reflections::CastArgs",    Offsets::reflections::CastArgs,            SecKind::Code },
            { "Luau::Luau_Execute",       Offsets::Luau::Luau_Execute,               SecKind::Code },
            { "Signals::FireTouchInterest", Offsets::Signals::FireTouchInterest,     SecKind::Code },
            { "Signals::DisconnectConnect", Offsets::Signals::DisconnectConnect,     SecKind::Code },
            { "Flags::FireMouseClick",    Offsets::Flags::FireMouseClick,            SecKind::Code },
            { "Flags::FireRightMouseClick", Offsets::Flags::FireRightMouseClick,     SecKind::Code },
            { "Flags::FireMouseHoverEnter", Offsets::Flags::FireMouseHoverEnter,     SecKind::Code },
            { "Flags::FireMouseHoverLeave", Offsets::Flags::FireMouseHoverLeave,     SecKind::Code },
            { "Flags::FireProximityPrompt", Offsets::Flags::FireProximityPrompt,     SecKind::Code },
            { "FFlags::GetFFlag",         Offsets::FFlags::GetFFlag,                 SecKind::Code },
            { "InstanceBridge::Push",     Offsets::InstanceBridge::Push,             SecKind::Code },
            { "ClassDescriptor::GetProperty", Offsets::ClassDescriptor::GetProperty, SecKind::Code },
            { "ClassDescriptor::HashLookup",  Offsets::ClassDescriptor::HashLookup,  SecKind::Code },
            { "Capabilities::GetCapabilities", Offsets::Capabilities::GetCapabilities, SecKind::Code },
            { "PropertyValue::OperatorDelete", Offsets::PropertyValue::OperatorDelete, SecKind::Code },
            { "PropertyValue::NetAssetRefDtor", Offsets::PropertyValue::NetAssetRefDtor, SecKind::Code },
            { "PropertyValue::AssetContentMapDtor", Offsets::PropertyValue::AssetContentMapDtor, SecKind::Code },
            { "RobloxThread::GetTlsPointer",  Offsets::RobloxThread::GetTlsPointer,  SecKind::Code },

            { "OpcodeLookupTable",        Offsets::OpcodeLookupTable,                SecKind::Data },
            { "Luau::LuaO_NilObject",     Offsets::Luau::LuaO_NilObject,             SecKind::Data },
            { "Luau::LuaH_DummyNode",     Offsets::Luau::LuaH_DummyNode,             SecKind::Data },
            { "Extras::k_table",          Offsets::Extras::k_table,                  SecKind::Data },
            { "FFlags::FFlagPointer",     Offsets::FFlags::FFlagPointer,             SecKind::Data },
            { "FFlags::BooleanType",      Offsets::FFlags::BooleanType,              SecKind::Data },
            { "FFlags::IntegerType",      Offsets::FFlags::IntegerType,              SecKind::Data },
            { "FFlags::BooleanValueType", Offsets::FFlags::BooleanValueType,         SecKind::Data },
            { "FFlags::IntegerValueType", Offsets::FFlags::IntegerValueType,         SecKind::Data },
            { "RobloxThread::IdentityPtr", Offsets::RobloxThread::IdentityPtr,       SecKind::Data },
            { "Scheduler::SchedulerPointer", Offsets::Scheduler::SchedulerPointer,   SecKind::Data },
            { "DataModel::FakeDataModelPointer", Offsets::DataModel::FakeDataModelPointer, SecKind::Data },
            { "Raknet::RakPeerVTable",    Offsets::Raknet::RakPeerVTable,            SecKind::Data },
            { "Raknet::NetworkStreamVtable", Offsets::Raknet::NetworkStreamVtable,   SecKind::Data },
            { "Raknet::UseRbxTransportClient", Offsets::Raknet::UseRbxTransportClient, SecKind::Data },
            { "Raknet::FailureFallbackToRbxTransport", Offsets::Raknet::FailureFallbackToRbxTransport, SecKind::Data },
            { "RbxTransport::ClientVTable", Offsets::RbxTransport::ClientVTable,    SecKind::Data },
            { "RbxTransport::AppVTable",  Offsets::RbxTransport::AppVTable,          SecKind::Data },
            { "RbxTransport::ChannelConfig", Offsets::RbxTransport::ChannelConfig,   SecKind::Code },
            { "RbxTransport::StreamCreate", Offsets::RbxTransport::StreamCreate,     SecKind::Code },
            { "RbxTransport::StreamCopy", Offsets::RbxTransport::StreamCopy,         SecKind::Code },
            { "RbxTransport::StreamAppend", Offsets::RbxTransport::StreamAppend,     SecKind::Code },
            { "RbxTransport::StreamDestroy", Offsets::RbxTransport::StreamDestroy,   SecKind::Code },
        };

        for (const auto& e : table)
        {
            if (!e.abs)
            {
                r.add(G, e.name, Sev::Warn, "unresolved in the 0.745 dump");
                continue;
            }

            const uintptr_t rva = e.abs - base;
            const SecKind got = ClassifyRva(rva);
            if (got == e.want)
                r.add(G, e.name, Sev::Pass, fmt("+0x%llX in %s", (unsigned long long)rva, KindName(got)));
            else
                r.add(G, e.name, Sev::Fail,
                      fmt("+0x%llX is %s, expected %s -- stale offset or wrong client build",
                          (unsigned long long)rva, KindName(got), KindName(e.want)));
        }

        if (Offsets::Scheduler::SchedulerPointer == Offsets::Scheduler::TaskScheduler)
            r.add(G, "[xcheck] SchedulerPointer == TaskScheduler", Sev::Pass, "in sync");
        else
            r.add(G, "[xcheck] SchedulerPointer == TaskScheduler", Sev::Fail,
                  "the two scheduler aliases disagree; one was updated without the other");
    }

    inline void CheckStructs(Report& r)
    {
        using namespace detail;
        const char* G = "struct";

        auto expect = [&](const char* what, bool ok, const char* why)
        {
            r.add(G, what, ok ? Sev::Pass : Sev::Fail, why);
        };

        expect("sizeof(lua_State) == 0x88", sizeof(lua_State) == 0x88, "thread state size");
        expect("sizeof(RobloxExtraSpace) == 0xB8", sizeof(RobloxExtraSpace) == 0xB8, "extra space size");
        expect("sizeof(RobloxExtraSpace::Shared) == 0x38", sizeof(RobloxExtraSpace::Shared) == 0x38, "shared block size (engineContext added in 742)");
        expect("sizeof(LuaTable) == 0x30", sizeof(LuaTable) == 0x30, "table size");
        expect("sizeof(Closure) == 0x40", sizeof(Closure) == 0x40, "closure size");
        expect("sizeof(Proto) == 0xE8", sizeof(Proto) == 0xE8, "proto size");
        expect("sizeof(CallInfo) == 0x30", sizeof(CallInfo) == 0x30, "callinfo size");
        expect("sizeof(TString) == 0x20", sizeof(TString) == 0x20, "string size");
        expect("sizeof(stringtable) == 0x10", sizeof(stringtable) == 0x10, "stringtable size");

        expect("lua_State::global == 0x38", offsetof(lua_State, global) == 0x38,
               "0.745 moved global back to 0x38");
        expect("lua_State::userdata == 0x20", offsetof(lua_State, userdata) == 0x20,
               "0.745 moved userdata to 0x20");
        expect("LuaTable::metatable == 0x10", offsetof(LuaTable, metatable) == 0x10,
               "moved from 0x20 in 743");
        expect("Closure::isC == 0x03", offsetof(Closure, isC) == 0x03,
               "unchanged in 743");
        expect("Proto::code == 0x88", offsetof(Proto, code) == 0x88,
               "0.745 reordered the Proto pointer block");
        expect("GCheader::memcat == 0x02", offsetof(GCheader, memcat) == 0x02,
               "0.745 CommonHeader is marked, tt, memcat");

        expect("[xcheck] Properties::LuaStateGlobalOffset",
               Offsets::Properties::LuaStateGlobalOffset == offsetof(lua_State, global),
               "Reflection.hpp reads L->global through this constant");
    }

    inline void CheckLive(Report& r)
    {
        using namespace detail;
        const char* G = "live";

        uintptr_t fake = 0;
        if (!SafeRead(Offsets::DataModel::FakeDataModelPointer, fake) || !LooksLikePointer(fake))
        {
            r.add(G, "FakeDataModelPointer", Sev::Fail, "did not yield a usable pointer");
            return;
        }
        r.add(G, "FakeDataModelPointer", Sev::Pass, fmt("0x%llX", (unsigned long long)fake));

        uintptr_t dm = 0;
        if (!SafeRead(fake + Offsets::DataModel::FakeDataModelToDataModel, dm) || !LooksLikePointer(dm))
        {
            r.add(G, "DataModel::FakeDataModelToDataModel", Sev::Fail,
                  fmt("+0x%llX did not yield a real DataModel",
                      (unsigned long long)Offsets::DataModel::FakeDataModelToDataModel));
            return;
        }
        r.add(G, "DataModel::FakeDataModelToDataModel", Sev::Pass, fmt("0x%llX", (unsigned long long)dm));

        std::string cls;
        if (ClassNameOf(dm, cls) && IsPrintable(cls))
            r.add(G, "Instance::ClassDescriptor + ClassName", Sev::Pass, fmt("\"%s\"", cls.c_str()));
        else
            r.add(G, "Instance::ClassDescriptor + ClassName", Sev::Fail,
                  "class name did not decode -- 0x18 / 0x8 is wrong");

        {
            uintptr_t ct = 0;
            SafeRead(dm + Offsets::Instance::Children, ct);
            uintptr_t begin = 0, end = 0;
            bool ok = LooksLikePointer(ct)
                   && SafeRead(ct + 0x0, begin) && SafeRead(ct + 0x8, end)
                   && begin && end >= begin && ((end - begin) % 0x10) == 0
                   && ((end - begin) / 0x10) < 4096;
            r.add(G, "DataModel children (Instance::Children)", ok ? Sev::Pass : Sev::Fail,
                  ok ? fmt("%llu children", (unsigned long long)((end - begin) / 0x10))
                     : fmt("+0x%llX is not a valid container on the DataModel",
                           (unsigned long long)Offsets::Instance::Children));
        }

        {
            const uintptr_t sc = SafeReadOr<uintptr_t>(dm + Offsets::DataModel::ScriptContext, 0);
            r.add(G, "DataModel::ScriptContext", LooksLikePointer(sc) ? Sev::Pass : Sev::Fail,
                  LooksLikePointer(sc) ? fmt("0x%llX", (unsigned long long)sc)
                                       : "+0x440 did not yield a pointer");

            const uint64_t placeId = SafeReadOr<uint64_t>(dm + Offsets::DataModel::PlaceId, 0);
            if (!placeId)
                r.add(G, "DataModel::PlaceId", Sev::Warn, "zero -- probably still on the menu");
            else if (placeId < (1ull << 40))
                r.add(G, "DataModel::PlaceId", Sev::Pass, fmt("%llu", (unsigned long long)placeId));
            else
                r.add(G, "DataModel::PlaceId", Sev::Fail,
                      fmt("%llu is not a plausible place id", (unsigned long long)placeId));

            const uint8_t loaded = SafeReadOr<uint8_t>(dm + Offsets::DataModel::GameLoaded, 0xFF);
            r.add(G, "DataModel::GameLoaded", loaded <= 1 ? Sev::Pass : Sev::Fail,
                  loaded <= 1 ? fmt("%u", loaded) : fmt("%u is not a bool -- GameLoaded (0x5C8) is wrong", loaded));
        }

        {
            uintptr_t sched = 0;
            SafeRead(Offsets::Scheduler::SchedulerPointer, sched);
            if (!LooksLikePointer(sched))
            {
                r.add(G, "Scheduler", Sev::Fail, "scheduler pointer is not usable");
            }
            else
            {
                uintptr_t begin = 0, end = 0;
                SafeRead(sched + Offsets::Scheduler::JobsStart, begin);
                SafeRead(sched + Offsets::Scheduler::JobsEnd, end);

                if (!LooksLikePointer(begin) || end < begin || ((end - begin) % 0x10))
                {
                    r.add(G, "Scheduler JobsStart/JobsEnd", Sev::Fail,
                          "job array is not a sane 0x10-strided range");
                }
                else
                {
                    const size_t n = static_cast<size_t>((end - begin) / 0x10);
                    r.add(G, "Scheduler JobsStart/JobsEnd", (n && n < 512) ? Sev::Pass : Sev::Warn,
                          fmt("%llu jobs", (unsigned long long)n));

                    size_t named = 0;
                    for (size_t i = 0; i < n && i < 128; ++i)
                    {
                        uintptr_t job = 0;
                        if (!SafeRead(begin + i * 0x10, job) || !LooksLikePointer(job)) continue;
                        std::string jn;
                        if (ReadRbxString(job + Offsets::Scheduler::JobName, jn) && IsPrintable(jn))
                            ++named;
                    }
                    r.add(G, "Scheduler::JobName", named ? Sev::Pass : Sev::Fail,
                          named ? fmt("+0x18 decoded %llu job names", (unsigned long long)named)
                                : "+0x18 decoded no job names -- JobName is wrong");
                }

                const double fps = SafeReadOr<double>(sched + Offsets::Scheduler::Fps, -1.0);
                r.add(G, "Scheduler::Fps", (fps > 0.0 && fps <= 1000.0) ? Sev::Pass : Sev::Warn,
                      fmt("%.2f", fps));
            }
        }

        {
            const uintptr_t vt = Offsets::Raknet::RakPeerVTable;
            uintptr_t slot = 0;
            const bool ok = SafeRead(vt + Offsets::Raknet::idx_Send * sizeof(void*), slot)
                         && ClassifyRva(slot - ImageBase()) == SecKind::Code;
            r.add(G, "Raknet::RakPeerVTable slot 20 (SendShared)", ok ? Sev::Pass : Sev::Fail,
                  ok ? fmt("0x%llX is code", (unsigned long long)slot)
                     : "slot 20 does not point at code -- vtable address or index is wrong");

            uintptr_t nsSlot = 0;
            const bool nsOk = SafeRead(Offsets::Raknet::NetworkStreamVtable, nsSlot)
                           && ClassifyRva(nsSlot - ImageBase()) == SecKind::Code;
            r.add(G, "Raknet::NetworkStreamVtable", nsOk ? Sev::Pass : Sev::Fail,
                  nsOk ? "slot 0 is code" : "slot 0 does not point at code");

            uintptr_t appSend = 0;
            const bool appOk = SafeRead(Offsets::RbxTransport::AppVTable + Offsets::RbxTransport::idx_AppSend * sizeof(void*), appSend)
                            && ClassifyRva(appSend - ImageBase()) == SecKind::Code;
            r.add(G, "RbxTransport::AppVTable slot 9 (Send)", appOk ? Sev::Pass : Sev::Fail,
                  appOk ? fmt("0x%llX is code", (unsigned long long)appSend)
                        : "slot 9 does not point at code -- vtable address or index is wrong");
        }
    }

    inline void CheckLuaState(Report& r, lua_State* L)
    {
        using namespace detail;
        const char* G = "luau";

        if (!L) { r.add(G, "lua_State", Sev::Warn, "none supplied -- Luau checks skipped"); return; }

        const uintptr_t Lp = reinterpret_cast<uintptr_t>(L);
        if (!LooksLikePointer(Lp)) { r.add(G, "lua_State", Sev::Fail, "not readable"); return; }

        const uint8_t tt = SafeReadOr<uint8_t>(Lp + offsetof(GCheader, tt), 0xFF);
        r.add(G, "lua_State::tt", tt == 8 ? Sev::Pass : Sev::Fail,
              tt == 8 ? "LUA_TTHREAD" : fmt("tt == %u, expected 8", tt));

        const auto stack = SafeReadOr<uintptr_t>(Lp + offsetof(lua_State, stack), 0);
        const auto base  = SafeReadOr<uintptr_t>(Lp + offsetof(lua_State, base), 0);
        const auto top   = SafeReadOr<uintptr_t>(Lp + offsetof(lua_State, top), 0);
        const auto last  = SafeReadOr<uintptr_t>(Lp + offsetof(lua_State, stack_last), 0);
        const bool stackOk = stack && base && top && last && stack <= base && base <= top && top <= last;
        r.add(G, "lua_State stack ordering", stackOk ? Sev::Pass : Sev::Fail,
              stackOk ? "stack <= base <= top <= stack_last"
                      : fmt("stack=0x%llX base=0x%llX top=0x%llX last=0x%llX -- layout is wrong",
                            (unsigned long long)stack, (unsigned long long)base,
                            (unsigned long long)top, (unsigned long long)last));

        const auto baseci = SafeReadOr<uintptr_t>(Lp + offsetof(lua_State, base_ci), 0);
        const auto ci     = SafeReadOr<uintptr_t>(Lp + offsetof(lua_State, ci), 0);
        const auto endci  = SafeReadOr<uintptr_t>(Lp + offsetof(lua_State, end_ci), 0);
        const bool ciOk = baseci && ci && endci && baseci <= ci && ci <= endci;
        r.add(G, "lua_State callinfo ordering", ciOk ? Sev::Pass : Sev::Fail,
              ciOk ? "base_ci <= ci <= end_ci" : "callinfo range is not ordered");

        const auto g = SafeReadOr<uintptr_t>(Lp + offsetof(lua_State, global), 0);
        r.add(G, "lua_State::global", LooksLikePointer(g) ? Sev::Pass : Sev::Fail,
              LooksLikePointer(g) ? fmt("0x%llX", (unsigned long long)g) : "+0x18 is not a global_State");

        const auto es = SafeReadOr<uintptr_t>(Lp + offsetof(lua_State, userdata), 0);
        if (!LooksLikePointer(es)) { r.add(G, "ExtraSpace", Sev::Fail, "+0x80 is not a RobloxExtraSpace"); return; }
        r.add(G, "ExtraSpace", Sev::Pass, fmt("0x%llX", (unsigned long long)es));

        const auto identity = SafeReadOr<uint32_t>(es + offsetof(RobloxExtraSpace, identity), 0xFFFFFFFF);
        r.add(G, "ExtraSpace::identity", identity <= 9 ? Sev::Pass : Sev::Fail,
              identity <= 9 ? fmt("%u", identity)
                            : fmt("%u is not a valid identity -- 0x58 is wrong", identity));

        const auto caps = SafeReadOr<uint64_t>(es + offsetof(RobloxExtraSpace, capabilities), 0);
        r.add(G, "ExtraSpace::capabilities", caps ? Sev::Pass : Sev::Warn,
              fmt("0x%llX%s", (unsigned long long)caps, caps ? "" : " (zero -- unprivileged thread?)"));

        const auto idSc = SafeReadOr<uintptr_t>(es + offsetof(RobloxExtraSpace, identity_script_context), 0);
        const auto shared = SafeReadOr<uintptr_t>(es + offsetof(RobloxExtraSpace, shared), 0);
        if (LooksLikePointer(shared))
        {
            const auto sharedSc = SafeReadOr<uintptr_t>(
                shared + offsetof(RobloxExtraSpace::Shared, script_context), 0);
            const auto threads = SafeReadOr<int>(
                shared + offsetof(RobloxExtraSpace::Shared, threadCount), -1);

            if (idSc && sharedSc && idSc == sharedSc)
                r.add(G, "[xcheck] identity vs Shared ScriptContext", Sev::Pass, "both agree");
            else
                r.add(G, "[xcheck] identity vs Shared ScriptContext", Sev::Fail,
                      fmt("identity 0x%llX vs shared 0x%llX -- 0x68 or Shared+0x28 is wrong",
                          (unsigned long long)idSc, (unsigned long long)sharedSc));

            r.add(G, "Shared::threadCount", (threads > 0 && threads < 100000) ? Sev::Pass : Sev::Warn,
                  fmt("%d", threads));
        }
        else
        {
            r.add(G, "ExtraSpace::shared", Sev::Fail, "+0x18 is not a Shared block");
        }
    }

    inline Report RunAll(lua_State* L = nullptr)
    {
        Report r;
        CheckAddresses(r);
        CheckStructs(r);
        CheckLive(r);
        CheckLuaState(r, L);
        return r;
    }

    inline std::string Format(const Report& r)
    {
        std::string out;
        out += detail::fmt("offsets self-test -- %d passed, %d warned, %d FAILED\n",
                           r.passed, r.warned, r.failed);
        out += "--------------------------------------------------------------\n";

        const char* lastGroup = nullptr;
        for (const auto& e : r.results)
        {
            if (lastGroup != e.group)
            {
                out += detail::fmt("\n[%s]\n", e.group);
                lastGroup = e.group;
            }
            const char* tag = e.sev == Sev::Pass ? "ok  " : e.sev == Sev::Warn ? "warn" : "FAIL";
            out += detail::fmt("  %s  %-42s %s\n", tag, e.name.c_str(), e.detail.c_str());
        }

        out += "\n--------------------------------------------------------------\n";
        if (!r.failed && !r.warned)
            out += "all checks passed\n";
        else if (!r.failed)
            out += "no failures; review the warnings above\n";
        else
        {
            out += detail::fmt("%d WRONG OFFSET/STRUCT FIELD(S):\n", r.failed);
            for (const auto& e : r.results)
                if (e.sev == Sev::Fail)
                    out += detail::fmt("  * %s::%s -- %s\n", e.group, e.name.c_str(), e.detail.c_str());
        }
        return out;
    }

    inline bool WriteReport(const Report& r, const char* path)
    {
        const std::string text = Format(r);
        OutputDebugStringA(text.c_str());
        FILE* f = nullptr;
        if (fopen_s(&f, path, "wb") != 0 || !f) return false;
        fwrite(text.data(), 1, text.size(), f);
        fclose(f);
        return true;
    }
}
