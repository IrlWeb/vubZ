#pragma once
#include <queue>
#include <string>
#include <vector>
#include <lstate.h>
#include <functional>
#include <lstate.h>
#include <string>
#include <queue>
#include <functional>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <shared_mutex>
#include <Windows.h>
#include "Scripting/Bridge/LuauHelper.hpp"
#include <unordered_set>
#include <yarafunk.h>

#define lua_pushoclosure(L, C) setclvalue(L, L->top, C); incr_top(L)
#define lua_tomutclosure(L, idx) const_cast<Closure*>(reinterpret_cast<const Closure*>(lua_topointer(L, idx)))
inline uintptr_t MaxCapabilities = 0xFFFFFFFFFFFFFFFF;

#define OBF(x) (xorstr_(x).crypt_get())
#define ENV_OBF(x) (xorstr_(x).crypt_get())

namespace SharedVariables
{
    inline uintptr_t LastDataModel;
    inline uintptr_t ScriptContext;
    inline lua_State* ExploitThread;
    inline lua_State* RobloxState;

    inline std::atomic<uint64_t> StateGeneration{ 0 };
    inline std::vector<std::string> ExecutionRequests;
    inline std::mutex ExecutionRequestsMutex;
    inline std::atomic<uintptr_t> ExecutorHeartbeatConnection{ 0 };
    inline std::atomic<int64_t> AttachedLuaPlaceId{ -1 };
    inline std::vector<std::string> TeleportQueue;
    inline std::mutex TeleportQueueMutex;

    inline std::queue<std::function<void()>> YieldQueue;
    inline std::mutex YieldQueueMutex;

    inline std::filesystem::path workspace_folder;
    inline std::filesystem::path autoexecute_folder;
    inline std::unordered_set<Closure*> ExecutorClosures;
    inline std::unordered_set<Closure*> HiddenClosures;
    inline std::shared_mutex HiddenClosuresLock;
    inline std::atomic<lua_CFunction> HiddenEntries[8]{};

    inline void AddHiddenEntry(lua_CFunction entry)
    {
        for (auto& slot : HiddenEntries)
        {
            lua_CFunction current = nullptr;
            if (slot.compare_exchange_strong(current, entry, std::memory_order_acq_rel) || current == entry)
                return;
        }
    }

    inline void SetHiddenClosure(Closure* closure, bool hidden = true)
    {
        if (!closure)
            return;
        std::unique_lock lock(HiddenClosuresLock);
        if (hidden)
            HiddenClosures.insert(closure);
        else
            HiddenClosures.erase(closure);
    }

    inline bool IsHiddenClosure(Closure* closure)
    {
        if (!closure)
            return false;
        if (closure->isC)
        {
            for (auto& slot : HiddenEntries)
            {
                lua_CFunction entry = slot.load(std::memory_order_acquire);
                if (!entry)
                    break;
                if (closure->c.f == entry)
                    return true;
            }
        }
        std::shared_lock lock(HiddenClosuresLock);
        return HiddenClosures.count(closure) > 0;
    }

    inline void ClearHiddenClosures()
    {
        std::unique_lock lock(HiddenClosuresLock);
        HiddenClosures.clear();
    }
    inline bool ExecutorFastcallsDisabled = false;

    inline std::atomic_bool JobStepLocked{ true };
    inline std::mutex JobHookMutex;

    inline uintptr_t LastWaitingHybridScriptsJob = 0;
    inline uintptr_t* OriginalHybridJobVtable = nullptr;
    inline uintptr_t* HybridJobVtable = nullptr;


    using JobStep_t = uintptr_t(__fastcall*)(uintptr_t Job, uintptr_t Stats);
    inline JobStep_t OriginalStepFunction = nullptr;
}

inline bool IsExecutorThread(lua_State* L)
{
    if (!L || !L->userdata)
        return false;

    if (SharedVariables::ExploitThread && L == SharedVariables::ExploitThread)
        return true;

    if (!L->userdata->source.expired())
        return false;

    return (L->userdata->capabilities & 0xFFFFFFFFFFFFFF00ULL) == 0xFFFFFFFFFFFFFF00ULL;
}

inline bool IsElevatedExecutorThread(lua_State* L)
{
    return IsExecutorThread(L) && L->userdata->capabilities == MaxCapabilities;
}

inline const char* CodeLoadDeniedReason(lua_State* L)
{
    return IsExecutorThread(L)
        ? "cannot load code while thread identity is lowered; call setthreadidentity(8) first"
        : "The current thread cannot execute code";
}

static LuaTable* getcurrenv(lua_State* L)
{
    if (L->ci == L->base_ci)
        return L->gt;
    else
        return curr_func(L)->env;
}

static LUAU_NOINLINE TValue* pseudo2addr(lua_State* L, int idx)
{
    api_check(L, lua_ispseudo(idx));
    switch (idx)
    {
    case LUA_REGISTRYINDEX:
        return registry(L);
    case LUA_ENVIRONINDEX:
    {
        sethvalue(L, &L->global->pseudotemp, getcurrenv(L));
        return &L->global->pseudotemp;
    }
    case LUA_GLOBALSINDEX:
    {
        sethvalue(L, &L->global->pseudotemp, L->gt);
        return &L->global->pseudotemp;
    }
    default:
    {
        Closure* func = curr_func(L);
        idx = LUA_GLOBALSINDEX - idx;
        return (idx <= func->nupvalues) ? &func->c.upvals[idx - 1] : cast_to(TValue*, luaO_nilobject);
    }
    }
}

static LUAU_FORCEINLINE TValue* index2addr(lua_State* L, int idx)
{
    if (idx > 0)
    {
        TValue* o = L->base + (idx - 1);
        api_check(L, idx <= L->ci->top - L->base);
        if (o >= L->top)
            return cast_to(TValue*, luaO_nilobject);
        else
            return o;
    }
    else if (idx > LUA_REGISTRYINDEX)
    {
        api_check(L, idx != 0 && -idx <= L->top - L->base);
        return L->top + idx;
    }
    else
    {
        return pseudo2addr(L, idx);
    }
}
