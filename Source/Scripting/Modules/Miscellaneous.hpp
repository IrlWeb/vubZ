#pragma once

#include <Core/CallTrace.hpp>

#include <Scripting/Obfuscation.hpp>
#include <shlobj.h>

#include <Core/DebugLog.hpp>

#include <lgc.h>
#include <lstate.h>
#include <lualib.h>

#include <Core/Utilities.hpp>
#include <Core/Context.hpp>
#include <Security/BrandGuard.hpp>
#include <Security/CloneGuard.hpp>
#include <Security/VMProtectShim.h>
#include <Runtime/Execution/ScriptExecution.hpp>
#include <Scripting/Modules/Closures.hpp>
#include <Scripting/Coroutines/Yielding.hpp>
#include "Update/FFlags.hpp"

#include <Windows.h>
#include <cstdio>
#include <cstdint>
#include <string>
#include <charconv>

namespace Miscellaneous
{

    inline bool TryWriteTlsIdentity(int identity, uint64_t caps, uintptr_t& outPtr)
    {
        __try
        {
            const uintptr_t descriptor =
                *reinterpret_cast<uintptr_t*>(Offsets::RobloxThread::IdentityPtr);
            if (!descriptor)
                return false;

            const uintptr_t p = Roblox::GetIdentityStruct(descriptor);
            outPtr = p;
            if (!p)
                return false;

            *reinterpret_cast<uint64_t*>(p) = static_cast<uint64_t>(identity);
            *reinterpret_cast<uint64_t*>(p + Offsets::Capabilities::Capabilities) = caps;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    inline bool CalledFromExecutorCode(lua_State* L)
    {
        if (IsExecutorThread(L))
            return true;

        for (CallInfo* ci = L->ci - 1; ci > L->base_ci; --ci)
        {
            if (!ttisfunction(ci->func))
                continue;
            Closure* cl = clvalue(ci->func);
            if (cl->isC)
                continue;
            return Closures::IsExecutorProto(cl->l.p);
        }
        return false;
    }

    int setthreadidentity(lua_State* L)
    {
        luaL_checktype(L, 1, LUA_TNUMBER);
        int identity = (int)lua_tointeger(L, 1);

        if (!IsExecutorThread(L))
        {
            luaL_error(L, "The current thread cannot set thread identity");
            return 0;
        }


        if (!CalledFromExecutorCode(L))
        {
            luaL_error(L, "The current thread cannot set thread identity");
            return 0;
        }

        uint64_t caps = 0xFFFFFFFFFFFFFF00ULL | Roblox::GetCapabilities(&identity);

        if (identity == 8)
        {
            caps = MaxCapabilities;
        }

        L->userdata->identity = identity;
        L->userdata->capabilities = caps;

        uintptr_t identity_ptr = 0;
        if (!TryWriteTlsIdentity(identity, caps, identity_ptr))
        {
            static bool Warned = false;
            if (!Warned)
            {
                Warned = true;
                SUPQ_LOG_ERR("setthreadidentity: TLS identity write failed "
                    "(IdentityPtr=0x%llX descriptor->0x%llX). Extra-space identity "
                    "and capabilities were still set, so most checks still pass.",
                    (unsigned long long)Offsets::RobloxThread::IdentityPtr,
                    (unsigned long long)identity_ptr);
            }
        }

        return 0;
    }

    int SetIdentity(lua_State* L) {
        return setthreadidentity(L);
    }

    int getthreadidentity(lua_State* L)
    {
        if (!L->userdata)
        {
            lua_pushinteger(L, 0);
            return 1;
        }
        lua_pushinteger(L, L->userdata->identity);
        return 1;
    }

    inline void CheckRivalGlobals(lua_State* L)
    {
        const int table = lua_gettop(L);
        if (!lua_istable(L, table))
            return;

        lua_rawcheckstack(L, 1);

        const std::string* keys[] = {
            &BrandGuard::RivalConsoleGlobal(),
            &BrandGuard::RivalProbeGlobal(),
        };

        for (const std::string* key : keys)
        {
            lua_rawgetfield(L, table, key->c_str());
            const bool present = !lua_isnil(L, -1);
            lua_pop(L, 1);

            if (present)
                CloneGuard::Flag(CloneGuard::Signal::RivalGlobals, *key);
        }
    }

    int getgenv(lua_State* L)
    {
        if (!IsExecutorThread(L))
        {
            luaL_error(L, "The current thread cannot access executor globals");
            return 0;
        }

        if (SharedVariables::ExploitThread == L)
        {
            lua_pushvalue(L, LUA_GLOBALSINDEX);
            CheckRivalGlobals(L);
            return 1;
        }

        lua_rawcheckstack(L, 1);
        luaC_threadbarrier(L);
        luaC_threadbarrier(SharedVariables::ExploitThread);
        lua_pushvalue(SharedVariables::ExploitThread, LUA_GLOBALSINDEX);
        lua_xmove(SharedVariables::ExploitThread, L, 1);

        CheckRivalGlobals(L);
        return 1;
    }

    int identifyexecutor(lua_State* L)
    {
        VMProtectBeginUltra("brand.identifyexecutor");
        lua_pushstring(L, BrandGuard::Name().c_str());
        lua_pushstring(L, BrandGuard::Version().c_str());
        VMProtectEnd();
        return 2;
    }

    void EnforceIdentityGlobals(lua_State* L);

    int getexecutorname(lua_State* L)
    {
        VMProtectBeginUltra("brand.getexecutorname");
        lua_pushstring(L, BrandGuard::Name().c_str());
        VMProtectEnd();
        return 1;
    }

    int GetObjects(lua_State* L)
    {
        luaL_checktype(L, 1, LUA_TUSERDATA);
        luaL_checktype(L, 2, LUA_TSTRING);

        lua_getglobal(L, "game");
        lua_getfield(L, -1, "GetService");
        if (!lua_isfunction(L, -1))
        {
            lua_pop(L, 2);
            luaL_error(L, "game:GetService is unavailable");
            return 0;
        }
        lua_pushvalue(L, -2);
        lua_pushstring(L, "InsertService");
        if (lua_pcall(L, 2, 1, 0) != LUA_OK)
        {
            const char* err = lua_tostring(L, -1);
            char message[512];
            strncpy_s(message, err ? err : "failed to get InsertService", _TRUNCATE);
            luaL_error(L, "%s", message);
            return 0;
        }
        lua_remove(L, -2);

        lua_getfield(L, -1, "LoadLocalAsset");
        if (!lua_isfunction(L, -1))
        {
            lua_pop(L, 2);
            luaL_error(L, "InsertService:LoadLocalAsset is unavailable");
            return 0;
        }

        lua_pushvalue(L, -2);
        lua_pushvalue(L, 2);
        if (lua_pcall(L, 2, 1, NULL) != LUA_OK)
        {
            const char* err = lua_tostring(L, -1);
            char message[512];
            strncpy_s(message, err ? err : "LoadLocalAsset failed", _TRUNCATE);
            luaL_error(L, "%s", message);
            return 0;
        }

        if (lua_type(L, -1) == LUA_TSTRING)
            luaL_error(L, lua_tostring(L, -1));

        lua_createtable(L, 1, NULL);
        lua_pushvalue(L, -2);
        lua_rawseti(L, -2, 1);

        lua_remove(L, -3);
        lua_remove(L, -2);

        return 1;
    }

    int setfpscap(lua_State* L) {
        double fps = luaL_checknumber(L, 1);

        if (!std::isfinite(fps) || fps <= 0.0 || fps > 250.0) fps = 250.0;

        uintptr_t scheduler = *reinterpret_cast<uintptr_t*>(Offsets::Scheduler::TaskScheduler);
        if (!scheduler) return 0;

        *reinterpret_cast<double*>(scheduler + Offsets::Scheduler::Fps) = 1.0 / fps;

        return 0;
    }

    inline int getfpscap(lua_State* L)
    {
        uintptr_t scheduler = *reinterpret_cast<uintptr_t*>(Offsets::Scheduler::TaskScheduler);

        double fps = *reinterpret_cast<double*>(scheduler + Offsets::Scheduler::Fps);

        if (!std::isfinite(fps) || fps <= 0.0)
            lua_pushnumber(L, 250.0);
        else
            lua_pushnumber(L, 1.0 / fps);

        return 1;
    }

    int queueonteleport(lua_State* L)
    {
        size_t length = 0;
        const char* code = luaL_checklstring(L, 1, &length);
        std::lock_guard<std::mutex> lock(SharedVariables::TeleportQueueMutex);
        SharedVariables::TeleportQueue.emplace_back(code, length);
        return 0;
    }

    int clearteleportqueue(lua_State*)
    {
        std::lock_guard<std::mutex> lock(SharedVariables::TeleportQueueMutex);
        SharedVariables::TeleportQueue.clear();
        return 0;
    }

    inline char GethuiKey;

    // One cloneref of CoreGui per DataModel: sUNC requires gethui() to return the same reference
    // on every call. The cached clone is reused while it still wraps the current CoreGui, and
    // rebuilt after a teleport swaps the DataModel.
    inline int resolve_gethui(lua_State* L)
    {
        lua_getglobal(L, "game");
        lua_getfield(L, -1, "GetService");
        lua_pushvalue(L, -2);
        lua_pushstring(L, "CoreGui");
        lua_call(L, 2, 1);
        const int coreGui = lua_gettop(L);
        if (lua_type(L, coreGui) != LUA_TUSERDATA)
            luaL_error(L, "CoreGui is unavailable");
        void* const current = *static_cast<void**>(lua_touserdata(L, coreGui));

        lua_pushlightuserdata(L, &GethuiKey);
        lua_rawget(L, LUA_REGISTRYINDEX);
        if (lua_type(L, -1) == LUA_TUSERDATA && *static_cast<void**>(lua_touserdata(L, -1)) == current)
            return 1;
        lua_pop(L, 1);

        lua_pushcfunction(L, Closures::cloneref, "cloneref");
        lua_pushvalue(L, coreGui);
        lua_call(L, 1, 1);

        lua_pushlightuserdata(L, &GethuiKey);
        lua_pushvalue(L, -2);
        lua_rawset(L, LUA_REGISTRYINDEX);
        return 1;
    }
    inline int gethuiV2(lua_State* L)
    {
        lua_settop(L, 0);
        lua_pushcfunction(L, resolve_gethui, "gethui");
        if (lua_pcall(L, 0, 1, 0) != LUA_OK)
        {
            const char* message = lua_tostring(L, -1);
            luaL_error(L, "gethui: %s", message ? message : "failed to resolve the hidden container");
        }

        return 1;
    }

    int gethui(lua_State* L)
    {
        lua_getglobal(L, ENV_OBF("game"));
        if (!lua_isuserdata(L, -1)) { lua_pop(L, 1); lua_pushnil(L); return 1; }

        lua_getfield(L, -1, ENV_OBF("GetService"));
        if (!lua_isfunction(L, -1)) { lua_pop(L, 2); lua_pushnil(L); return 1; }

        lua_pushvalue(L, -2);
        lua_pushstring(L, ENV_OBF("CoreGui"));
        if (lua_pcall(L, 2, 1, 0) != LUA_OK) {
            lua_pop(L, 1);
            lua_pushnil(L);
            return 1;
        }

        if (!lua_isuserdata(L, -1)) { lua_pop(L, 1); lua_pushnil(L); return 1; }
        int coreGuiIdx = lua_gettop(L);

        lua_getfield(L, -1, ENV_OBF("FindFirstChildOfClass"));
        if (!lua_isfunction(L, -1)) { lua_pop(L, 1); lua_pushnil(L); return 1; }

        lua_pushvalue(L, coreGuiIdx);
        lua_pushstring(L, ENV_OBF("ScreenGui"));
        if (lua_pcall(L, 2, 1, 0) == LUA_OK && lua_isuserdata(L, -1)) {
            return 1;
        }

        lua_getglobal(L, ENV_OBF("Instance"));
        lua_getfield(L, -1, ENV_OBF("new"));
        lua_pushstring(L, ENV_OBF("ScreenGui"));
        lua_call(L, 1, 1);
        lua_remove(L, -2);

        if (lua_isuserdata(L, -1)) {
            lua_pushvalue(L, -1);
            lua_setfield(L, coreGuiIdx, ENV_OBF("Name"));
            lua_pushvalue(L, -1);
            lua_setfield(L, coreGuiIdx, ENV_OBF("Parent"));
            return 1;
        }

        lua_pop(L, 1);
        lua_pushnil(L);
        return 1;
    }

    static std::mutex CommChannelMutex;
    static int NextCommChannelId = 1;

    struct CommChannelData {
        int id;
        int selfRef;
    };
    static std::unordered_map<int, CommChannelData> CommChannels;

    int get_comm_channel(lua_State* L) {
        if (!lua_isnumber(L, 1)) { lua_pushnil(L); return 1; }
        int id = (int)lua_tointeger(L, 1);

        std::lock_guard<std::mutex> lock(CommChannelMutex);
        auto it = CommChannels.find(id);
        if (it == CommChannels.end()) { lua_pushnil(L); return 1; }

        lua_rawgeti(L, LUA_REGISTRYINDEX, it->second.selfRef);
        return 1;
    }

    inline std::string StripFlagPrefix(const std::string& name)
    {
        static const char* const prefixes[] = {
            "DFString", "SFString", "FString",
            "DFFlag", "SFFlag", "FFlag",
            "DFInt", "SFInt", "FInt",
            "DFLog", "SFLog", "FLog",
        };

        for (const char* prefix : prefixes)
        {
            const std::size_t length = std::strlen(prefix);
            if (name.size() > length && name.compare(0, length, prefix) == 0)
                return name.substr(length);
        }

        return {};
    }

    inline std::uintptr_t FindFlagRaw(void* reg, const char* name, std::size_t length) noexcept
    {
        __try
        {
            const auto registry = reinterpret_cast<std::uintptr_t>(reg);

            if (!registry)
                return 0;

            const uintptr_t list =*reinterpret_cast<const std::uintptr_t*>(registry + 0x8);

            if (!list)
                return 0;

            uintptr_t node = *reinterpret_cast<const uintptr_t*>(list);

            const uintptr_t end = *reinterpret_cast<const uintptr_t*>(list + 0x8);

            for (std::size_t i = 0; node && node != end && i < 20000; ++i)
            {
                const int32_t nameLength = *reinterpret_cast<const std::int32_t*>(node + 0x20);

                if (nameLength >= 0 && static_cast<std::size_t>(nameLength) == length)
                {
                    const char* nodeName = reinterpret_cast<const char*>(node + 0x10);

                    if (nameLength >= 0x10)
                    {
                        nodeName = *reinterpret_cast<const char* const*>(node + 0x10);
                    }

                    if (nodeName &&
                        std::memcmp(nodeName, name, length) == 0)
                    {
                        return *reinterpret_cast<const std::uintptr_t*>(node + 0x30);
                    }
                }

                node = *reinterpret_cast<const std::uintptr_t*>(node);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }

        return 0;
    }

    inline std::uintptr_t FindFlagObject(const char* name)
    {
        void* registry = *reinterpret_cast<void* const*>(Offsets::FFlags::FFlagPointer);

        if (!registry || !name)
            return 0;

        return FindFlagRaw(registry ,name, std::strlen(name));
    }

    inline bool GetFlag(const std::string& name, std::string& output)
    {
        output.clear();

        void* registry = *reinterpret_cast<void* const*>(Offsets::FFlags::FFlagPointer);
        if (!registry)
            return false;

        __try
        {
            return Roblox::GetFFlag(registry, name, &output, false);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            output.clear();
            return false;
        }
    }

    inline bool ReadIdentity(uintptr_t flag, uintptr_t& identity) noexcept
    {
        __try
        {
            identity = *reinterpret_cast<const std::uintptr_t*>(flag);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            identity = 0;
            return false;
        }
    }

    inline bool WriteFlagInteger(uintptr_t flag, int32_t value) noexcept
    {
        __try
        {
            const std::uintptr_t live =*reinterpret_cast<const uintptr_t*>(flag + Offsets::FFlags::ValueLivePointer);

            if (!live)
                return false;

            *reinterpret_cast<int32_t*>(live) = value;

            *reinterpret_cast<std::int32_t*>(flag + Offsets::FFlags::ValueCached) = value;

            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    inline bool WriteFlagBoolean(uintptr_t flag, bool value) noexcept
    {
        __try
        {
            const std::uintptr_t live =*reinterpret_cast<const uintptr_t*>(flag + Offsets::FFlags::ValueLivePointer);

            if (!live)
                return false;

            *reinterpret_cast<bool*>(live) = value;

            *reinterpret_cast<bool*>(flag + Offsets::FFlags::ValueCached) = value;

            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    inline bool SetFlag(const std::string& name, const std::string& value)
    {
        std::uintptr_t flag = FindFlagObject(name.c_str());
        if (!flag)
        {
            const std::string stripped = StripFlagPrefix(name);
            if (!stripped.empty())
                flag = FindFlagObject(stripped.c_str());
        }

        if (!flag)
        {
            Roblox::Print(3, "Flag not found");
            return false;
        }

        uintptr_t identity = 0;

        if (!ReadIdentity(flag, identity))
        {
            return false;
        }

        const bool isInteger = identity == Offsets::FFlags::IntegerType || identity == Offsets::FFlags::IntegerValueType;

        if (isInteger)
        {
            int32_t parsed = 0;
            const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);

            if (result.ec != std::errc{} || result.ptr != value.data() + value.size())
            {
                return false;
            }

            const bool written = WriteFlagInteger(flag, parsed);
            return written;
        }

        const bool isBoolean = identity == Offsets::FFlags::BooleanType || identity == Offsets::FFlags::BooleanValueType;

        if (isBoolean)
        {
            if (_stricmp(value.c_str(), "true") == 0)
                return WriteFlagBoolean(flag, true);

            if (_stricmp(value.c_str(), "false") == 0)
                return WriteFlagBoolean(flag, false);

            return false;
        }

        return false;
    }

    int getfflag(lua_State* L)
    {
        const std::string name = luaL_checkstring(L, 1);

        std::string value;
        if (!GetFlag(name, value))
        {
            const std::string stripped = StripFlagPrefix(name);
            if (stripped.empty() || !GetFlag(stripped, value))
            {
                lua_pushnil(L);
                return 1;
            }
        }

        lua_pushlstring(L, value.data(), value.size());
        return 1;
    }

    int setfflag(lua_State* L)
    {
        const char* name = luaL_checkstring(L, 1);
        std::string value;

        if (lua_type(L, 2) == LUA_TBOOLEAN)
            value = lua_toboolean(L, 2) ? "true" : "false";
        else
            value = luaL_checkstring(L, 2);

        lua_pushboolean(L, SetFlag(name, value));
        return 1;
    }

    inline int calltrace(lua_State* L)
    {
        if (!lua_isnoneornil(L, 1))
        {
            luaL_checktype(L, 1, LUA_TBOOLEAN);
            CallTrace::SetEnabled(lua_toboolean(L, 1) != 0);
        }
        lua_pushboolean(L, CallTrace::IsEnabled());
        return 1;
    }

    inline int calltracemark(lua_State* L)
    {
        CallTrace::Note("mark", luaL_optstring(L, 1, ""), static_cast<int>(L->nCcalls));
        CallTrace::Flush();
        return 0;
    }

    inline int get_process_identifier(lua_State* L)
    {
        lua_pushinteger(L, static_cast<int>(GetCurrentProcessId()));
        return 1;
    }

    void RegisterLibrary(lua_State* L)
    {

        Utils::AddFunction(L, "getgenv", Miscellaneous::getgenv);
        Utils::AddFunction(L, "get_process_identifier", Miscellaneous::get_process_identifier);
        Utils::AddFunction(L, "calltrace", Miscellaneous::calltrace);
        Utils::AddFunction(L, "calltracemark", Miscellaneous::calltracemark);
        Utils::AddFunction(L, "identifyexecutor", Miscellaneous::identifyexecutor);
        Utils::AddFunction(L, "getexecutorname", Miscellaneous::getexecutorname);
        EnforceIdentityGlobals(L);
        Utils::AddFunction(L, "setthreadidentity", Miscellaneous::setthreadidentity);
        Utils::AddFunction(L, "setidentity", Miscellaneous::setthreadidentity);
        Utils::AddFunction(L, "setthreadcontext", Miscellaneous::setthreadidentity);
        Utils::AddFunction(L, "set_thread_identity", Miscellaneous::setthreadidentity);
        Utils::AddFunction(L, "getthreadidentity", Miscellaneous::getthreadidentity);
        Utils::AddFunction(L, "getidentity", Miscellaneous::getthreadidentity);
        Utils::AddFunction(L, "getthreadcontext", Miscellaneous::getthreadidentity);
        Utils::AddFunction(L, "get_thread_identity", Miscellaneous::getthreadidentity);
        Utils::AddFunction(L, "setfpscap", Miscellaneous::setfpscap);
        Utils::AddFunction(L, "getfpscap", Miscellaneous::getfpscap);
        Utils::AddFunction(L, "GetObjects", Miscellaneous::GetObjects);
        Utils::AddFunction(L, "queueonteleport", Miscellaneous::queueonteleport);
        Utils::AddFunction(L, "queue_on_teleport", Miscellaneous::queueonteleport);
        Utils::AddFunction(L, "queueteleport", Miscellaneous::queueonteleport);
        Utils::AddFunction(L, "clearteleportqueue", Miscellaneous::clearteleportqueue);
        Utils::AddFunction(L, "clear_teleport_queue", Miscellaneous::clearteleportqueue);
        Utils::AddFunction(L, "clearqueueonteleport", Miscellaneous::clearteleportqueue);
        Utils::AddFunction(L, "gethui", Miscellaneous::gethuiV2);
        Utils::AddFunction(L, "get_hidden_ui", Miscellaneous::gethuiV2);
        Utils::AddFunction(L, "get_hidden_gui", Miscellaneous::gethuiV2);
        Utils::AddFunction(L, "getfflag", Miscellaneous::getfflag);
        Utils::AddFunction(L, "setfflag", Miscellaneous::setfflag);

    }
}

namespace Miscellaneous
{
    void EnforceIdentityGlobals(lua_State* L)
    {
        if (!L)
            return;

        Utils::AddFunction(L, SUPQ2_OBF("identifyexecutor"), Miscellaneous::identifyexecutor);
        Utils::AddFunction(L, SUPQ2_OBF("getexecutorname"), Miscellaneous::getexecutorname);
    }
}
