#pragma once

#include <Windows.h>
#include <lstate.h>
#include <lualib.h>
#include <Core/Utilities.hpp>
#include <Scripting/Environment.hpp>
#include <Runtime/Scheduler/TaskScheduler.hpp>
#include <Runtime/Execution/ScriptExecution.hpp>
#include <Scripting/Modules/Signals.hpp>
#include <Scripting/Modules/Signal.hpp>
#include <Scripting/Modules/Trampoline.hpp>
#include <Scripting/Modules/Compat.hpp>
#include "Update/Offsets.hpp"
#include <Scripting/Modules/Drawing.h>
#include <Scripting/Modules/Console.h>
#include <Scripting/Modules/Bit.h>
#include <Scripting/Modules/ModuleLib.h> 
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <cstring>
#include <algorithm>

inline std::mutex g_actor_scan_mutex;
inline std::unordered_set<lua_State*> g_tracked_actor_threads;
inline std::vector<std::shared_ptr<uintptr_t>> g_tracked_actors;
inline std::unordered_map<lua_State*, bool> g_ParallelStates;

inline bool IsValidPtr(uintptr_t Address) {
    constexpr uintptr_t MinUserAddress = 0x10000;
    constexpr uintptr_t MaxUserAddress = 0x7FFFFFFF0000ULL;

    if (!Address || Address < MinUserAddress || Address > MaxUserAddress)
        return false;

    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<LPCVOID>(Address), &mbi, sizeof(mbi)))
        return false;

    if (mbi.State != MEM_COMMIT)
        return false;

    return mbi.Protect != PAGE_NOACCESS && mbi.Protect != PAGE_GUARD;
}

inline void TrackActor(uintptr_t actorPointer)
{
    if (!actorPointer || !IsValidPtr(actorPointer))
        return;

    std::lock_guard<std::mutex> lock(g_actor_scan_mutex);
    for (const auto& tracked : g_tracked_actors) {
        if (tracked && *tracked == actorPointer)
            return;
    }

    g_tracked_actors.emplace_back(std::make_shared<uintptr_t>(actorPointer));
}

inline void RegisterActorThread(lua_State* thread)
{
    if (!thread)
        return;

    std::lock_guard<std::mutex> lock(g_actor_scan_mutex);
    g_tracked_actor_threads.insert(thread);
}

namespace Actors
{
    inline int abs_index(lua_State* L, int idx) {
        return (idx < 0 && idx > LUA_REGISTRYINDEX) ? (lua_gettop(L) + idx + 1) : idx;
    }

    struct ActorEntry {
        int id;
        lua_State* thread;
        uintptr_t actorPtr;
        bool alive;
    };

    inline std::mutex g_actor_map_mutex;
    inline std::unordered_map<int, ActorEntry> g_actor_map;
    inline int g_next_actor_id = 1;

    inline const void* COMM_CACHE_KEY = reinterpret_cast<const void*>(0xCA4C4E);
    inline const void* ONMESSAGE_CALLBACK_KEY = reinterpret_cast<const void*>(0x004D5347);

    static void deepCopyInner(lua_State* src, lua_State* dst, int idx,
        std::unordered_map<const void*, int>& seen, std::vector<int>& refs, int depth)
    {
        const int absIdx = abs_index(src, idx);

        if (depth > 64 || !lua_checkstack(dst, 4)) {
            lua_pushnil(dst);
            return;
        }

        switch (lua_type(src, absIdx)) {
        case LUA_TTABLE: {
            const void* ptr = lua_topointer(src, absIdx);
            if (ptr) {
                const auto it = seen.find(ptr);
                if (it != seen.end()) {
                    lua_getref(dst, it->second);
                    return;
                }
            }

            lua_newtable(dst);
            if (ptr) {
                const int ref = lua_ref(dst, -1);
                seen[ptr] = ref;
                refs.push_back(ref);
            }

            lua_pushnil(src);
            while (lua_next(src, absIdx)) {
                deepCopyInner(src, dst, -2, seen, refs, depth + 1);
                deepCopyInner(src, dst, -1, seen, refs, depth + 1);

                if (lua_isnil(dst, -2))
                    lua_pop(dst, 2);
                else
                    lua_settable(dst, -3);

                lua_pop(src, 1);
            }
            break;
        }
        case LUA_TUSERDATA: {
            if (strcmp(luaL_typename(src, absIdx), OBF("Instance")) == 0)
                Roblox::PushInstance(dst, lua_touserdata(src, absIdx));
            else
                lua_pushnil(dst);
            break;
        }
        case LUA_TSTRING: {
            size_t len = 0;
            const char* str = lua_tolstring(src, absIdx, &len);
            if (str)
                lua_pushlstring(dst, str, len);
            else
                lua_pushnil(dst);
            break;
        }
        case LUA_TNUMBER:
            lua_pushnumber(dst, lua_tonumber(src, absIdx));
            break;
        case LUA_TBOOLEAN:
            lua_pushboolean(dst, lua_toboolean(src, absIdx));
            break;
        case LUA_TVECTOR: {
            const float* v = lua_tovector(src, absIdx);
            if (v)
#if LUA_VECTOR_SIZE == 4
                lua_pushvector(dst, v[0], v[1], v[2], v[3]);
#else
                lua_pushvector(dst, v[0], v[1], v[2]);
#endif
            else
                lua_pushnil(dst);
            break;
        }
        default:
            lua_pushnil(dst);
            break;
        }
    }

    static void deepCopy(lua_State* src, lua_State* dst, int idx) {
        std::unordered_map<const void*, int> seen;
        std::vector<int> refs;

        deepCopyInner(src, dst, idx, seen, refs, 0);

        for (const int ref : refs)
            lua_unref(dst, ref);
    }

    static void push_comm_cache(lua_State* L) {
        lua_pushlightuserdata(L, const_cast<void*>(COMM_CACHE_KEY));
        lua_rawget(L, LUA_REGISTRYINDEX);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            lua_newtable(L);
            lua_pushlightuserdata(L, const_cast<void*>(COMM_CACHE_KEY));
            lua_pushvalue(L, -2);
            lua_rawset(L, LUA_REGISTRYINDEX);
        }
    }

    static void push_comm_folder(lua_State* L) {
        lua_getglobal(L, OBF("game"));
        lua_getfield(L, -1, OBF("GetService"));
        lua_pushvalue(L, -2);
        lua_pushstring(L, OBF("CoreGui"));
        if (lua_pcall(L, 2, 1, 0) != LUA_OK || lua_isnil(L, -1)) {
            luaL_error(L, OBF("CoreGui indisponible"));
        }
        lua_remove(L, -2);

        lua_getfield(L, -1, OBF("FindFirstChild"));
        lua_pushvalue(L, -2);
        lua_pushstring(L, "__comm_channels__");
        lua_pcall(L, 2, 1, 0);

        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);

            lua_getglobal(L, OBF("Instance"));
            lua_getfield(L, -1, OBF("new"));
            lua_pushstring(L, "Folder");
            lua_pcall(L, 1, 1, 0);
            lua_remove(L, -2);

            lua_pushstring(L, "__comm_channels__");
            lua_setfield(L, -2, OBF("Name"));

            lua_pushvalue(L, -2);
            lua_setfield(L, -2, OBF("Parent"));
        }

        lua_remove(L, -2);
    }

    static void push_event_object(lua_State* L, int bindable_idx) {
        int abs_bindable = abs_index(L, bindable_idx);

        lua_newtable(L);
        int obj_idx = lua_gettop(L);

        lua_getfield(L, abs_bindable, OBF("Event"));
        lua_setfield(L, obj_idx, OBF("Event"));

        lua_pushvalue(L, abs_bindable);
        lua_pushcclosure(L, [](lua_State* L) -> int {
            int top = lua_gettop(L);
            lua_getfield(L, lua_upvalueindex(1), OBF("Fire"));
            lua_pushvalue(L, lua_upvalueindex(1));

            int start_arg = (top >= 1 && lua_istable(L, 1)) ? 2 : 1;
            for (int i = start_arg; i <= top; i++) {
                lua_pushvalue(L, i);
            }

            int nargs = 1 + (top >= start_arg ? (top - start_arg + 1) : 0);
            lua_pcall(L, nargs, 0, 0);
            return 0;
            }, OBF("eventObject_Fire"), 1);
        lua_setfield(L, obj_idx, OBF("Fire"));

        lua_pushcfunction(L, [](lua_State* L) -> int {
            luaL_checktype(L, 1, LUA_TTABLE);
            luaL_checktype(L, 2, LUA_TFUNCTION);

            lua_getfield(L, 1, OBF("Event"));
            if (lua_isnil(L, -1)) return 0;

            lua_getfield(L, -1, OBF("Once"));
            lua_insert(L, -2);
            lua_pushvalue(L, 2);

            lua_pcall(L, 2, 0, 0);
            return 0;
            }, OBF("eventObject_Once"));
        lua_setfield(L, obj_idx, OBF("Once"));

        lua_pushcfunction(L, [](lua_State* L) -> int {
            luaL_checktype(L, 1, LUA_TTABLE);
            luaL_checktype(L, 2, LUA_TFUNCTION);

            lua_getfield(L, 1, OBF("Event"));
            if (lua_isnil(L, -1)) return 0;

            lua_getfield(L, -1, OBF("Connect"));
            lua_insert(L, -2);
            lua_pushvalue(L, 2);

            lua_call(L, 2, 1);
            return 1;
            }, OBF("eventObject_Connect"));
        lua_pushvalue(L, -1);
        lua_setfield(L, obj_idx, OBF("Connect"));
        lua_setfield(L, obj_idx, OBF("connect"));
    }

    static int push_comm_event_for_id(lua_State* L, int id) {
        push_comm_folder(L);
        char idStr[32];
        snprintf(idStr, sizeof(idStr), "%d", id);

        lua_getfield(L, -1, OBF("FindFirstChild"));
        lua_pushvalue(L, -2);
        lua_pushstring(L, idStr);
        lua_pcall(L, 2, 1, 0);

        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);

            lua_getglobal(L, OBF("Instance"));
            lua_getfield(L, -1, OBF("new"));
            lua_pushstring(L, OBF("BindableEvent"));
            lua_pcall(L, 1, 1, 0);
            lua_remove(L, -2);

            lua_pushstring(L, idStr);
            lua_setfield(L, -2, OBF("Name"));

            lua_pushvalue(L, -2);
            lua_setfield(L, -2, OBF("Parent"));
        }

        lua_remove(L, -2);
        return 1;
    }

    void RegisterLibrary(lua_State* L);

    inline int actor_spawn(lua_State* L) {
        if (!IsElevatedExecutorThread(L))
            luaL_error(L, "%s", CodeLoadDeniedReason(L));

        luaL_checktype(L, 1, LUA_TSTRING);
        const char* code = lua_tostring(L, 1);

        lua_getglobal(L, OBF("Instance"));
        lua_getfield(L, -1, OBF("new"));
        lua_pushstring(L, "Actor");
        if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
            luaL_error(L, "Impossible de creer l'instance Actor");
            return 0;
        }
        lua_remove(L, -2);

        int actor_obj_idx = lua_gettop(L);
        uintptr_t actorPtr = *reinterpret_cast<uintptr_t*>(lua_touserdata(L, actor_obj_idx));

        push_comm_folder(L);
        lua_pushvalue(L, actor_obj_idx);
        lua_setfield(L, -2, OBF("Parent"));
        lua_pop(L, 1);

        lua_getglobal(L, OBF("Instance"));
        lua_getfield(L, -1, OBF("new"));
        lua_pushstring(L, OBF("LocalScript"));
        if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
            luaL_error(L, OBF("Impossible de creer le LocalScript"));
            return 0;
        }
        lua_remove(L, -2);

        uintptr_t localScript = *reinterpret_cast<uintptr_t*>(lua_touserdata(L, -1));
        *reinterpret_cast<uintptr_t*>(localScript + Offsets::Actor::Parent) = actorPtr;

        const uintptr_t ScriptContext = (uintptr_t)L->userdata->shared->script_context;
        uintptr_t unused = 0;
        lua_State* actorL = reinterpret_cast<lua_State*>(TaskScheduler::GetLuaState(ScriptContext, unused, localScript));

        lua_State* NL = lua_newthread(actorL);
        lua_pop(actorL, 1);

        RegisterActorThread(NL);

        TrackActor(actorPtr);
        std::shared_ptr<uintptr_t> actorShared;
        {
            std::lock_guard<std::mutex> lock(g_actor_scan_mutex);
            for (const auto& tracked : g_tracked_actors) {
                if (tracked && *tracked == actorPtr) {
                    actorShared = tracked;
                    break;
                }
            }
        }
        if (!actorShared) {
            actorShared = std::make_shared<uintptr_t>(actorPtr);
        }
        NL->userdata->actor = actorShared;

        {
            std::lock_guard<std::mutex> map_lock(g_actor_map_mutex);
            std::lock_guard<std::mutex> scan_lock(g_actor_scan_mutex);
            g_ParallelStates[NL] = true;
        }

        TaskScheduler::SetThreadCapabilities(NL, 8, MaxCapabilities);
        luaL_sandboxthread(NL);
        lua_settop(NL, 0);

        lua_newtable(NL);
        lua_setglobal(NL, "_G");

        lua_newtable(NL);
        lua_setglobal(NL, OBF("shared"));

        Http::RegisterLibrary(NL);
        Crypt::RegisterLibrary(NL);
        Input::RegisterLibrary(NL);
        Debug::RegisterLibrary(NL);
        Cache::RegisterLibrary(NL);
        Signals::RegisterLibrary(NL);
        LuaSignal::RegisterLibrary(NL);
        Script::RegisterLibrary(NL);
        Closures::RegisterLibrary(NL);
        Instance::RegisterLibrary(NL);
        Websocket::Start(NL);
        Metatable::RegisterLibrary(NL);
        Filesystem::RegisterLibrary(NL);
        Reflection::RegisterLibrary(NL);
        Miscellaneous::RegisterLibrary(NL);
        Regex::RegisterLibrary(NL);
        FileDialog::RegisterLibrary(NL);
        Actors::RegisterLibrary(NL);
        Drawing::RegisterLibrary(NL);
        Console::RegisterLibrary(NL);
        supQLib::RegisterLibrary(NL);
        Bit::RegisterLibrary(NL);
        int id;
        {
            std::lock_guard<std::mutex> lock(g_actor_map_mutex);
            id = g_next_actor_id++;
            g_actor_map[id] = { id, NL, actorPtr, true };
        }

        lua_pushinteger(NL, id);
        lua_setglobal(NL, "_ACTOR_ID");

        push_comm_event_for_id(NL, id);
        lua_getfield(NL, -1, OBF("Event"));
        lua_getfield(NL, -1, OBF("Connect"));
        lua_pushvalue(NL, -2);

        lua_pushcclosure(NL, [](lua_State* L) -> int {
            lua_pushlightuserdata(L, const_cast<void*>(ONMESSAGE_CALLBACK_KEY));
            lua_rawget(L, LUA_REGISTRYINDEX);

            if (lua_isfunction(L, -1)) {
                int argc = lua_gettop(L) - 1;
                lua_insert(L, 1);
                lua_pcall(L, argc, 0, 0);
            }
            return 0;
            }, OBF("actor_event_dispatch"), 0);

        lua_pcall(NL, 2, 1, 0);
        lua_pop(NL, 3);

        std::string bytecode = Execution::CompileScript(code);
        int loadRes = luau_load(NL, "=actor_spawn", bytecode.c_str(), bytecode.size(), 0);

        if (loadRes != LUA_OK) {
            const char* err = lua_tostring(NL, -1);
            lua_pop(NL, 1);

            {
                std::lock_guard<std::mutex> map_lock(g_actor_map_mutex);
                std::lock_guard<std::mutex> scan_lock(g_actor_scan_mutex);
                g_actor_map.erase(id);
                g_ParallelStates.erase(NL);
            }

            lua_pushnil(L);
            lua_pushstring(L, err);
            return 2;
        }

        if (Closure* Cl = clvalue(luaA_toobject(NL, -1))) {
            if (Cl && Cl->l.p)
                TaskScheduler::SetProtoCapabilities(Cl->l.p, &MaxCapabilities);
        }

        Roblox::TaskDefer(NL);

        push_comm_event_for_id(L, id);
        push_event_object(L, -1);
        lua_remove(L, -2);

        lua_pushinteger(L, id);
        lua_insert(L, -2);
        return 2;
    }

    inline int actor_kill(lua_State* L) {
        luaL_checktype(L, 1, LUA_TNUMBER);
        int id = static_cast<int>(lua_tointeger(L, 1));

        {
            std::lock_guard<std::mutex> map_lock(g_actor_map_mutex);
            std::lock_guard<std::mutex> scan_lock(g_actor_scan_mutex);

            auto it = g_actor_map.find(id);
            if (it == g_actor_map.end() || !it->second.alive) {
                lua_pushboolean(L, false);
                return 1;
            }

            it->second.alive = false;
            g_ParallelStates.erase(it->second.thread);
            g_actor_map.erase(it);
        }

        lua_settop(L, 0);
        push_comm_folder(L);
        char idStr[32];
        snprintf(idStr, sizeof(idStr), "%d", id);

        lua_getfield(L, -1, OBF("FindFirstChild"));
        lua_pushvalue(L, 1);
        lua_pushstring(L, idStr);
        lua_pcall(L, 2, 1, 0);

        if (!lua_isnil(L, -1)) {
            lua_getfield(L, -1, OBF("Destroy"));
            lua_pushvalue(L, -2);
            lua_pcall(L, 1, 0, 0);
        }

        lua_settop(L, 0);
        lua_pushboolean(L, true);
        return 1;
    }

    inline int actor_send(lua_State* L) {
        luaL_checktype(L, 1, LUA_TNUMBER);
        luaL_checktype(L, 2, LUA_TSTRING);

        int id = static_cast<int>(lua_tointeger(L, 1));
        int total_top = lua_gettop(L);
        int extra_args = total_top - 2;

        push_comm_event_for_id(L, id);
        lua_getfield(L, -1, OBF("Fire"));
        lua_pushvalue(L, -2);
        lua_pushvalue(L, 2);

        for (int i = 3; i <= total_top; i++) {
            lua_pushvalue(L, i);
        }

        lua_pcall(L, 2 + (extra_args > 0 ? extra_args : 0), 0, 0);
        lua_pop(L, 1);
        return 0;
    }

    inline int actor_onmessage(lua_State* L) {
        luaL_checktype(L, 1, LUA_TFUNCTION);

        lua_pushlightuserdata(L, const_cast<void*>(ONMESSAGE_CALLBACK_KEY));
        lua_pushvalue(L, 1);
        lua_rawset(L, LUA_REGISTRYINDEX);

        return 0;
    }

    inline int actor_getid(lua_State* L) {
        lua_getglobal(L, "_ACTOR_ID");
        return 1;
    }

    inline int actor_isparallel(lua_State* L) {
        lua_State* target = L;
        if (lua_gettop(L) >= 1 && lua_isthread(L, 1)) {
            target = lua_tothread(L, 1);
        }

        if (!target) {
            lua_pushboolean(L, false);
            return 1;
        }

        bool parallel = false;
        lua_State* mainL = (target->global && target->global->mainthread) ? target->global->mainthread : target;

        {
            std::lock_guard<std::mutex> lock(g_actor_scan_mutex);
            auto it = g_ParallelStates.find(target);
            if (it != g_ParallelStates.end()) {
                parallel = it->second;
            }
            else {
                auto itMain = g_ParallelStates.find(mainL);
                if (itMain != g_ParallelStates.end()) {
                    parallel = itMain->second;
                }
                else if (g_tracked_actor_threads.find(target) != g_tracked_actor_threads.end() ||
                    g_tracked_actor_threads.find(mainL) != g_tracked_actor_threads.end()) {
                    parallel = true;
                }
            }
        }

        if (!parallel && target->userdata) {
            rbxextraspace* extra = target->userdata;
            auto actorPtr = extra->actor.lock();
            if (!actorPtr && mainL->userdata) {
                actorPtr = mainL->userdata->actor.lock();
            }

            if (actorPtr && *actorPtr != 0) {
                uintptr_t actor = *actorPtr;
                uintptr_t inner = 0;
                if (signal_safe_read(reinterpret_cast<const void*>(actor + 0x18), &inner, sizeof(inner)) && inner) {
                    uintptr_t v8 = 0;
                    if (signal_safe_read(reinterpret_cast<const void*>(inner + 0x28), &v8, sizeof(v8)) && v8) {
                        uint8_t parallelByte = 0;
                        if (signal_safe_read(reinterpret_cast<const void*>(v8 + Offsets::Actor::ParallelFlag), &parallelByte, sizeof(parallelByte))) {
                            parallel = (parallelByte != 0);
                        }
                        else {
                            parallel = true;
                        }
                    }
                    else {
                        parallel = true;
                    }
                }
                else {
                    parallel = true;
                }
            }
        }

        lua_pushboolean(L, parallel);
        return 1;
    }

    inline int getactors(lua_State* L) {
        lua_newtable(L);
        int idx = 1;

        lua_getglobal(L, OBF("game"));
        if (lua_isuserdata(L, -1)) {
            lua_getfield(L, -1, OBF("GetDescendants"));
            if (lua_isfunction(L, -1)) {
                lua_pushvalue(L, -2);
                if (lua_pcall(L, 1, 1, 0) == LUA_OK && lua_istable(L, -1)) {
                    lua_pushnil(L);
                    while (lua_next(L, -2)) {
                        if (lua_isuserdata(L, -1)) {
                            lua_getfield(L, -1, OBF("ClassName"));
                            if (lua_isstring(L, -1) && strcmp(lua_tostring(L, -1), "Actor") == 0) {
                                lua_pop(L, 1);
                                lua_pushvalue(L, -1);
                                lua_rawseti(L, 1, idx++);
                            }
                            else {
                                lua_pop(L, 1);
                            }
                        }
                        lua_pop(L, 1);
                    }
                }
                lua_pop(L, 1);
            }
            else {
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
        return 1;
    }

    inline int get_comm_channel(lua_State* L) {
        if (!lua_isnumber(L, 1)) {
            lua_pushnil(L);
            return 1;
        }

        int id = static_cast<int>(lua_tointeger(L, 1));

        push_comm_cache(L);
        lua_pushinteger(L, id);
        lua_rawget(L, -2);
        if (!lua_isnil(L, -1)) {
            return 1;
        }
        lua_pop(L, 2);

        push_comm_folder(L);
        char idStr[32];
        snprintf(idStr, sizeof(idStr), "%d", id);

        lua_getfield(L, -1, OBF("FindFirstChild"));
        lua_pushvalue(L, -2);
        lua_pushstring(L, idStr);
        lua_pcall(L, 2, 1, 0);
        lua_remove(L, -2);

        if (lua_isnil(L, -1)) {
            return 1;
        }

        push_event_object(L, -1);
        int obj_idx = lua_gettop(L);

        push_comm_cache(L);
        lua_pushinteger(L, id);
        lua_pushvalue(L, obj_idx);
        lua_rawset(L, -3);
        lua_pop(L, 1);

        lua_pushvalue(L, obj_idx);
        return 1;
    }

    inline int create_comm_channel(lua_State* L) {
        lua_settop(L, 0);

        push_comm_folder(L);

        lua_getfield(L, 1, OBF("GetAttribute"));
        lua_pushvalue(L, 1);
        lua_pushstring(L, "__next_id");
        lua_pcall(L, 2, 1, 0);

        int current = lua_isnil(L, -1) ? 0 : static_cast<int>(lua_tointeger(L, -1));
        lua_pop(L, 1);
        int next_id = current + 1;

        lua_getfield(L, 1, OBF("SetAttribute"));
        lua_pushvalue(L, 1);
        lua_pushstring(L, "__next_id");
        lua_pushinteger(L, next_id);
        lua_pcall(L, 3, 0, 0);

        lua_getglobal(L, OBF("Instance"));
        lua_getfield(L, -1, OBF("new"));
        lua_pushstring(L, OBF("BindableEvent"));
        lua_pcall(L, 1, 1, 0);
        lua_remove(L, -2);

        char idStr[32];
        snprintf(idStr, sizeof(idStr), "%d", next_id);
        lua_pushstring(L, idStr);
        lua_setfield(L, 2, OBF("Name"));

        lua_pushvalue(L, 1);
        lua_setfield(L, 2, OBF("Parent"));

        push_event_object(L, 2);

        push_comm_cache(L);
        lua_pushinteger(L, next_id);
        lua_pushvalue(L, 3);
        lua_rawset(L, -3);
        lua_pop(L, 1);

        lua_pushinteger(L, next_id);
        lua_pushvalue(L, 3);
        return 2;
    }

    inline int run_on_actor(lua_State* L) {
        if (!IsElevatedExecutorThread(L))
            luaL_error(L, "%s", CodeLoadDeniedReason(L));

        luaL_checktype(L, 1, LUA_TUSERDATA);
        luaL_checktype(L, 2, LUA_TSTRING);

        lua_getfield(L, 1, OBF("ClassName"));
        const auto class_name = std::string_view(lua_tolstring(L, -1, nullptr));
        lua_pop(L, 1);

        if (class_name != "Actor") {
            luaL_argerrorL(L, 1, OBF("Actor expected"));
            return 0;
        }

        void* actorUd = lua_touserdata(L, 1);
        if (!actorUd)
            luaL_error(L, "run_on_actor: invalid Actor");

        auto actor = *reinterpret_cast<__int64*>(actorUd);
        if (!actor)
            luaL_error(L, OBF("run_on_actor: Actor is not live"));

        TrackActor(static_cast<uintptr_t>(actor));

        size_t codeLen = 0;
        const char* codePtr = lua_tolstring(L, 2, &codeLen);
        const std::string Code(codePtr ? codePtr : "", codePtr ? codeLen : 0);

        lua_remove(L, 2);
        lua_remove(L, 1);

        int Args = lua_gettop(L);

        lua_getglobal(L, OBF("Instance"));
        if (!lua_istable(L, -1))
            luaL_error(L, OBF("run_on_actor: Instance is unavailable"));

        lua_getfield(L, -1, OBF("new"));
        lua_pushstring(L, OBF("LocalScript"));
        if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
            const char* err = lua_tostring(L, -1);
            luaL_error(L, "run_on_actor: %s", err ? err : OBF("Instance.new failed"));
        }

        void* scriptUd = lua_isuserdata(L, -1) ? lua_touserdata(L, -1) : nullptr;
        const uintptr_t LocalScript = scriptUd ? *reinterpret_cast<uintptr_t*>(scriptUd) : 0;
        if (!LocalScript)
            luaL_error(L, OBF("run_on_actor: failed to create the actor script"));

        *reinterpret_cast<std::uintptr_t*>(LocalScript + Offsets::Actor::Parent) = actor;

        std::uintptr_t ScriptContext = 0;
        if (L->userdata && L->userdata->shared)
            ScriptContext = reinterpret_cast<std::uintptr_t>(L->userdata->shared->script_context);
        if (!ScriptContext)
            ScriptContext = SharedVariables::ScriptContext;
        if (!ScriptContext)
            luaL_error(L, OBF("run_on_actor: no ScriptContext"));

        uintptr_t unused = 0;
        lua_State* ActorState = reinterpret_cast<lua_State*>(TaskScheduler::GetLuaState(ScriptContext, unused, LocalScript));
        if (!ActorState)
            luaL_error(L, "run_on_actor: could not reach the actor's Lua state");

        auto NL = lua_newthread(ActorState);
        if (!NL || !NL->userdata) {
            lua_pop(ActorState, 1);
            luaL_error(L, "run_on_actor: could not create a thread on the actor");
        }
        RegisterActorThread(NL);
        lua_pop(ActorState, 1);

        std::shared_ptr<uintptr_t> actorShared;
        {
            std::lock_guard<std::mutex> lock(g_actor_scan_mutex);
            for (const auto& tracked : g_tracked_actors) {
                if (tracked && *tracked == static_cast<uintptr_t>(actor)) {
                    actorShared = tracked;
                    break;
                }
            }
        }
        if (!actorShared) {
            actorShared = std::make_shared<uintptr_t>(static_cast<uintptr_t>(actor));
        }
        NL->userdata->actor = actorShared;

        {
            std::lock_guard<std::mutex> map_lock(g_actor_map_mutex);
            std::lock_guard<std::mutex> scan_lock(g_actor_scan_mutex);
            g_ParallelStates[NL] = true;
        }

        TaskScheduler::SetThreadCapabilities(NL, 8, MaxCapabilities);
        luaL_sandboxthread(NL);
        lua_settop(NL, 0);

        lua_newtable(NL);
        lua_setglobal(NL, "_G");

        lua_newtable(NL);
        lua_setglobal(NL, OBF("shared"));

        Http::RegisterLibrary(NL);
        Crypt::RegisterLibrary(NL);
        Input::RegisterLibrary(NL);
        Debug::RegisterLibrary(NL);
        Cache::RegisterLibrary(NL);
        Signals::RegisterLibrary(NL);
        LuaSignal::RegisterLibrary(NL);
        Script::RegisterLibrary(NL);
        Closures::RegisterLibrary(NL);
        Instance::RegisterLibrary(NL);
        Websocket::Start(NL);
        Metatable::RegisterLibrary(NL);
        Filesystem::RegisterLibrary(NL);
        Reflection::RegisterLibrary(NL);
        Miscellaneous::RegisterLibrary(NL);
        Regex::RegisterLibrary(NL);
        FileDialog::RegisterLibrary(NL);
        Actors::RegisterLibrary(NL);

        std::string Bytecode = Execution::CompileScript(Code);
        int LoadResult = luau_load(NL, OBF("=actor"), Bytecode.c_str(), Bytecode.size(), 0);
        if (LoadResult == LUA_OK) {
            if (Closure* Cl = clvalue(luaA_toobject(NL, -1))) {
                if (Cl && Cl->l.p) {
                    TaskScheduler::SetProtoCapabilities(Cl->l.p, &MaxCapabilities);
                }
            }
            for (int i{ 1 }; i <= Args; i++) {
                deepCopy(L, NL, i);
            }
            Roblox::TaskDefer(NL);
        }
        else {
            const char* err = lua_tostring(NL, -1);
            lua_pop(NL, 1);
            luaL_error(L, "%s", err);
            return 0;
        }
        return 0;
    }

    inline int run_on_thread(lua_State* L) {
        if (!IsElevatedExecutorThread(L))
            luaL_error(L, "%s", CodeLoadDeniedReason(L));

        luaL_checktype(L, 1, LUA_TSTRING);
        const char* Code = lua_tostring(L, 1);

        lua_State* NL = lua_newthread(L);
        lua_pop(L, 1);

        TaskScheduler::SetThreadCapabilities(NL, 8, MaxCapabilities);
        luaL_sandboxthread(NL);
        lua_settop(NL, 0);

        std::string Bytecode = Execution::CompileScript(Code);
        int LoadResult = luau_load(NL, OBF("=actor"), Bytecode.c_str(), Bytecode.size(), 0);

        if (LoadResult != LUA_OK) {
            const char* err = lua_tostring(NL, -1);
            lua_pop(NL, 1);
            luaL_error(L, "%s", err);
            return 0;
        }

        if (Closure* Cl = clvalue(luaA_toobject(NL, -1))) {
            if (Cl && Cl->l.p)
                TaskScheduler::SetProtoCapabilities(Cl->l.p, &MaxCapabilities);
        }

        lua_pushlightuserdata(L, reinterpret_cast<void*>(NL));
        lua_pushcclosure(L, [](lua_State* innerL) -> int {
            lua_State* NL = reinterpret_cast<lua_State*>(lua_tolightuserdata(innerL, lua_upvalueindex(1)));
            Roblox::TaskDefer(NL);
            return 0;
            }, OBF("run_on_thread_closure"), 1);

        return 1;
    }

    inline void RegisterLibrary(lua_State* L);

    struct LuaVm
    {
        int id = 0;
        bool isActor = false;
        lua_State* executor = nullptr;
        std::unordered_set<uintptr_t> actors;
    };

    inline std::mutex g_vm_mutex;
    inline std::unordered_map<global_State*, LuaVm> g_vms;
    inline int g_next_vm_id = 1;

    inline std::unordered_map<int, std::unordered_set<global_State*>> g_event_subscribers;

    inline const char* const StateProxyName = "LuaStateProxy";
    inline const char* const StateEventName = "LuaStateEvent";
    inline char LocalEventsKey;
    inline char StateCreatedKey;
    inline char ActorWatchKey;

    struct StateRef
    {
        int id;
    };

    inline LuaVm* find_vm_by_id(int id)
    {
        for (auto& [vm, entry] : g_vms)
        {
            if (entry.id == id)
                return &entry;
        }
        return nullptr;
    }

    inline global_State* vm_of_id(int id)
    {
        for (auto& [vm, entry] : g_vms)
        {
            if (entry.id == id)
                return vm;
        }
        return nullptr;
    }

    inline void RegisterGameState(lua_State* L)
    {
        std::lock_guard<std::mutex> lock(g_vm_mutex);
        g_vms.clear();
        g_event_subscribers.clear();
        g_next_vm_id = 1;

        LuaVm& game = g_vms[L->global];
        game.id = g_next_vm_id++;
        game.isActor = false;
        game.executor = L;
    }

    inline lua_State* build_actor_executor(lua_State* actorState, uintptr_t actor)
    {
        lua_State* NL = lua_newthread(actorState);
        lua_ref(actorState, -1);
        lua_pop(actorState, 1);
        if (!NL || !NL->userdata)
            return nullptr;

        RegisterActorThread(NL);
        TrackActor(actor);
        std::shared_ptr<uintptr_t> actorShared;
        {
            std::lock_guard<std::mutex> lock(g_actor_scan_mutex);
            for (const auto& tracked : g_tracked_actors)
            {
                if (tracked && *tracked == actor)
                {
                    actorShared = tracked;
                    break;
                }
            }
            g_ParallelStates[NL] = true;
        }
        if (!actorShared)
            actorShared = std::make_shared<uintptr_t>(actor);
        NL->userdata->actor = actorShared;

        TaskScheduler::SetThreadCapabilities(NL, 8, MaxCapabilities);
        luaL_sandboxthread(NL);
        lua_settop(NL, 0);

        lua_newtable(NL);
        lua_setglobal(NL, "_G");
        lua_newtable(NL);
        lua_setglobal(NL, OBF("shared"));

        Http::RegisterLibrary(NL);
        Crypt::RegisterLibrary(NL);
        Input::RegisterLibrary(NL);
        Debug::RegisterLibrary(NL);
        Cache::RegisterLibrary(NL);
        Signals::RegisterLibrary(NL);
        LuaSignal::RegisterLibrary(NL);
        Script::RegisterLibrary(NL);
        Closures::RegisterLibrary(NL);
        Instance::RegisterLibrary(NL);
        Websocket::Start(NL);
        Metatable::RegisterLibrary(NL);
        Filesystem::RegisterLibrary(NL);
        Reflection::RegisterLibrary(NL);
        Miscellaneous::RegisterLibrary(NL);
        Regex::RegisterLibrary(NL);
        FileDialog::RegisterLibrary(NL);
        Actors::RegisterLibrary(NL);
        Drawing::RegisterLibrary(NL);
        Console::RegisterLibrary(NL);
        Bit::RegisterLibrary(NL);
        Trampoline::RegisterLibrary(NL);
        Compat::RegisterLibrary(NL);
        return NL;
    }

    inline lua_State* resolve_actor_vm(lua_State* L, uintptr_t actor)
    {
        const int top = lua_gettop(L);
        lua_getglobal(L, OBF("Instance"));
        lua_getfield(L, -1, OBF("new"));
        lua_pushstring(L, OBF("LocalScript"));
        if (lua_pcall(L, 1, 1, 0) != LUA_OK)
        {
            lua_settop(L, top);
            return nullptr;
        }

        void* scriptUd = lua_touserdata(L, -1);
        const uintptr_t localScript = scriptUd ? *static_cast<uintptr_t*>(scriptUd) : 0;
        if (!localScript)
        {
            lua_settop(L, top);
            return nullptr;
        }
        *reinterpret_cast<uintptr_t*>(localScript + Offsets::Actor::Parent) = actor;

        uintptr_t scriptContext = 0;
        if (L->userdata && L->userdata->shared)
            scriptContext = reinterpret_cast<uintptr_t>(L->userdata->shared->script_context);
        if (!scriptContext)
            scriptContext = SharedVariables::ScriptContext;

        uintptr_t unused = 0;
        lua_State* state = scriptContext
            ? reinterpret_cast<lua_State*>(TaskScheduler::GetLuaState(scriptContext, unused, localScript))
            : nullptr;

        lua_settop(L, top);
        return state;
    }

    inline int ensure_actor_state(lua_State* L, int index, bool* created = nullptr)
    {
        if (created)
            *created = false;

        void* ud = lua_touserdata(L, index);
        const uintptr_t actor = ud ? *static_cast<uintptr_t*>(ud) : 0;
        if (!actor)
            return 0;

        {
            std::lock_guard<std::mutex> lock(g_vm_mutex);
            for (auto& [vm, entry] : g_vms)
            {
                if (entry.actors.count(actor))
                    return entry.id;
            }
        }

        lua_State* actorState = resolve_actor_vm(L, actor);
        if (!actorState)
            return 0;

        {
            std::lock_guard<std::mutex> lock(g_vm_mutex);
            auto it = g_vms.find(actorState->global);
            if (it != g_vms.end())
            {
                it->second.actors.insert(actor);
                return it->second.id;
            }
        }

        lua_State* executor = build_actor_executor(actorState, actor);
        if (!executor)
            return 0;

        std::lock_guard<std::mutex> lock(g_vm_mutex);
        if (auto it = g_vms.find(actorState->global); it != g_vms.end())
        {
            it->second.actors.insert(actor);
            return it->second.id;
        }
        LuaVm& entry = g_vms[actorState->global];
        entry.id = g_next_vm_id++;
        entry.isActor = true;
        entry.executor = executor;
        entry.actors.insert(actor);
        if (created)
            *created = true;
        return entry.id;
    }

    inline void push_state(lua_State* L, int id)
    {
        auto* ref = static_cast<StateRef*>(lua_newuserdata(L, sizeof(StateRef)));
        ref->id = id;
        luaL_getmetatable(L, StateProxyName);
        lua_setmetatable(L, -2);
    }

    inline int current_state_id(lua_State* L)
    {
        std::lock_guard<std::mutex> lock(g_vm_mutex);
        auto it = g_vms.find(L->global);
        if (it != g_vms.end())
            return it->second.id;

        LuaVm& entry = g_vms[L->global];
        entry.id = g_next_vm_id++;
        entry.isActor = L->userdata && L->userdata->actor.lock() != nullptr;
        entry.executor = L;
        return entry.id;
    }

    inline int check_state(lua_State* L, int index)
    {
        if (lua_type(L, index) == LUA_TUSERDATA && lua_getmetatable(L, index))
        {
            luaL_getmetatable(L, StateProxyName);
            const bool ours = lua_rawequal(L, -1, -2) != 0;
            lua_pop(L, 2);
            if (ours)
                return static_cast<StateRef*>(lua_touserdata(L, index))->id;
        }
        luaL_typeerrorL(L, index, StateProxyName);
        return 0;
    }

    inline void push_actor_instances(lua_State* L, const std::unordered_set<uintptr_t>& actors)
    {
        lua_newtable(L);
        const int result = lua_gettop(L);
        if (actors.empty())
            return;

        lua_pushcfunction(L, getactors, "getactors");
        lua_call(L, 0, 1);
        const int all = lua_gettop(L);
        int count = 0;
        const int total = lua_objlen(L, all);
        for (int i = 1; i <= total; ++i)
        {
            lua_rawgeti(L, all, i);
            void* ud = lua_touserdata(L, -1);
            if (ud && actors.count(*static_cast<uintptr_t*>(ud)))
                lua_rawseti(L, result, ++count);
            else
                lua_pop(L, 1);
        }
        lua_settop(L, result);
    }

    inline void push_local_event(lua_State* L, int id)
    {
        lua_pushlightuserdata(L, &LocalEventsKey);
        lua_rawget(L, LUA_REGISTRYINDEX);
        if (!lua_istable(L, -1))
        {
            lua_pop(L, 1);
            lua_newtable(L);
            lua_pushlightuserdata(L, &LocalEventsKey);
            lua_pushvalue(L, -2);
            lua_rawset(L, LUA_REGISTRYINDEX);
        }
        lua_rawgeti(L, -1, id);
        if (lua_isnil(L, -1))
        {
            lua_pop(L, 1);
            LuaSignal::PushNew(L);
            lua_pushvalue(L, -1);
            lua_rawseti(L, -3, id);

            std::lock_guard<std::mutex> lock(g_vm_mutex);
            g_event_subscribers[id].insert(L->global);
        }
        lua_remove(L, -2);
    }

    inline int check_event(lua_State* L, int index)
    {
        if (lua_type(L, index) == LUA_TUSERDATA && lua_getmetatable(L, index))
        {
            luaL_getmetatable(L, StateEventName);
            const bool ours = lua_rawequal(L, -1, -2) != 0;
            lua_pop(L, 2);
            if (ours)
                return static_cast<StateRef*>(lua_touserdata(L, index))->id;
        }
        luaL_typeerrorL(L, index, StateEventName);
        return 0;
    }

    inline int deliver_event(lua_State* L)
    {
        const int id = static_cast<int>(lua_tointeger(L, lua_upvalueindex(1)));
        const int argc = lua_gettop(L);
        push_local_event(L, id);
        lua_insert(L, 1);
        LuaSignal::FireFromC(L, 1, argc);
        return 0;
    }

    inline int event_fire(lua_State* L)
    {
        const int id = check_event(L, 1);
        const int argc = lua_gettop(L) - 1;

        std::vector<std::pair<global_State*, lua_State*>> targets;
        {
            std::lock_guard<std::mutex> lock(g_vm_mutex);
            for (global_State* vm : g_event_subscribers[id])
            {
                auto it = g_vms.find(vm);
                if (it != g_vms.end() && it->second.executor)
                    targets.emplace_back(vm, it->second.executor);
            }
        }

        for (auto& [vm, executor] : targets)
        {
            if (vm == L->global)
            {
                push_local_event(L, id);
                for (int i = 2; i <= argc + 1; ++i)
                    lua_pushvalue(L, i);
                LuaSignal::FireFromC(L, -(argc + 1), argc);
                lua_pop(L, 1);
                continue;
            }

            lua_State* thread = lua_newthread(executor);
            lua_pop(executor, 1);
            lua_pushinteger(thread, id);
            lua_pushcclosure(thread, deliver_event, "LuaStateEvent.deliver", 1);
            for (int i = 2; i <= argc + 1; ++i)
                deepCopy(L, thread, i);
            Roblox::TaskDefer(thread);
        }
        return 0;
    }

    inline int event_forward(lua_State* L, const char* method)
    {
        const int id = check_event(L, 1);
        push_local_event(L, id);
        lua_replace(L, 1);
        lua_getfield(L, 1, method);
        lua_insert(L, 1);
        lua_call(L, lua_gettop(L) - 1, LUA_MULTRET);
        return lua_gettop(L);
    }

    inline int event_connect(lua_State* L) { return event_forward(L, "Connect"); }
    inline int event_once(lua_State* L) { return event_forward(L, "Once"); }

    inline int event_wait(lua_State* L)
    {
        const int id = check_event(L, 1);
        lua_settop(L, 0);
        push_local_event(L, id);
        return LuaSignal::Wait(L);
    }

    inline int event_index(lua_State* L)
    {
        const char* key = luaL_checkstring(L, 2);
        if (strcmp(key, "Connect") == 0) { lua_pushcfunction(L, event_connect, "Connect"); return 1; }
        if (strcmp(key, "Once") == 0) { lua_pushcfunction(L, event_once, "Once"); return 1; }
        if (strcmp(key, "Wait") == 0) { lua_pushcfunction(L, event_wait, "Wait"); return 1; }
        if (strcmp(key, "Fire") == 0) { lua_pushcfunction(L, event_fire, "Fire"); return 1; }
        luaL_error(L, "%s is not a valid member of %s", key, StateEventName);
        return 0;
    }

    inline int state_execute(lua_State* L)
    {
        if (!IsElevatedExecutorThread(L))
            luaL_error(L, "%s", CodeLoadDeniedReason(L));

        const int id = check_state(L, 1);
        size_t length = 0;
        const char* source = luaL_checklstring(L, 2, &length);
        const std::string code(source, length);
        const int argc = lua_gettop(L) - 2;

        lua_State* executor = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_vm_mutex);
            if (LuaVm* vm = find_vm_by_id(id))
                executor = vm->executor;
        }
        if (!executor)
            luaL_error(L, "Execute: this Lua state is no longer available");

        lua_State* thread = lua_newthread(executor);
        lua_pop(executor, 1);

        const std::string bytecode = Execution::CompileScript(code);
        if (luau_load(thread, OBF("=actor"), bytecode.c_str(), bytecode.size(), 0) != LUA_OK)
        {
            const char* err = lua_tostring(thread, -1);
            const std::string message = err ? err : "failed to load";
            lua_pop(thread, 1);
            luaL_error(L, "Execute: %s", message.c_str());
        }
        if (Closure* cl = clvalue(luaA_toobject(thread, -1)); cl && cl->l.p)
            TaskScheduler::SetProtoCapabilities(cl->l.p, &MaxCapabilities);

        for (int i = 3; i <= argc + 2; ++i)
        {
            if (executor->global == L->global)
            {
                lua_pushvalue(L, i);
                lua_xmove(L, thread, 1);
            }
            else
            {
                deepCopy(L, thread, i);
            }
        }

        Roblox::TaskDefer(thread);
        return 0;
    }

    inline int state_getactors(lua_State* L)
    {
        const int id = check_state(L, 1);
        std::unordered_set<uintptr_t> actors;
        {
            std::lock_guard<std::mutex> lock(g_vm_mutex);
            if (LuaVm* vm = find_vm_by_id(id))
                actors = vm->actors;
        }
        push_actor_instances(L, actors);
        return 1;
    }

    inline int state_index(lua_State* L)
    {
        const int id = check_state(L, 1);
        const char* key = luaL_checkstring(L, 2);

        if (strcmp(key, "Id") == 0)
        {
            lua_pushinteger(L, id);
            return 1;
        }
        if (strcmp(key, "IsActorState") == 0)
        {
            std::lock_guard<std::mutex> lock(g_vm_mutex);
            LuaVm* vm = find_vm_by_id(id);
            lua_pushboolean(L, vm && vm->isActor);
            return 1;
        }
        if (strcmp(key, "Event") == 0)
        {
            auto* ref = static_cast<StateRef*>(lua_newuserdata(L, sizeof(StateRef)));
            ref->id = id;
            luaL_getmetatable(L, StateEventName);
            lua_setmetatable(L, -2);
            return 1;
        }
        if (strcmp(key, "Execute") == 0)
        {
            lua_pushcfunction(L, state_execute, "Execute");
            return 1;
        }
        if (strcmp(key, "GetActors") == 0)
        {
            lua_pushcfunction(L, state_getactors, "GetActors");
            return 1;
        }

        luaL_error(L, "%s is not a valid member of %s", key, StateProxyName);
        return 0;
    }

    inline int state_eq(lua_State* L)
    {
        lua_pushboolean(L, check_state(L, 1) == check_state(L, 2));
        return 1;
    }

    inline int state_tostring(lua_State* L)
    {
        lua_pushfstring(L, "LuaStateProxy(%d)", check_state(L, 1));
        return 1;
    }

    inline int getluastate(lua_State* L)
    {
        if (lua_isnoneornil(L, 1))
        {
            push_state(L, current_state_id(L));
            return 1;
        }

        if (!lua_isuserdata(L, 1) || strcmp(luaL_typename(L, 1), "Instance") != 0)
            luaL_typeerrorL(L, 1, "Actor or script");

        lua_getfield(L, 1, OBF("IsA"));
        lua_pushvalue(L, 1);
        lua_pushstring(L, "Actor");
        lua_call(L, 2, 1);
        const bool isActor = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);

        int actorIndex = 1;
        if (!isActor)
        {
            lua_getfield(L, 1, OBF("FindFirstAncestorOfClass"));
            lua_pushvalue(L, 1);
            lua_pushstring(L, "Actor");
            lua_call(L, 2, 1);
            if (lua_isnil(L, -1))
            {
                std::lock_guard<std::mutex> lock(g_vm_mutex);
                for (auto& [vm, entry] : g_vms)
                {
                    if (!entry.isActor)
                    {
                        push_state(L, entry.id);
                        return 1;
                    }
                }
                lua_pushnil(L);
                return 1;
            }
            actorIndex = lua_gettop(L);
        }

        const int id = ensure_actor_state(L, actorIndex);
        if (!id)
        {
            lua_pushnil(L);
            return 1;
        }
        push_state(L, id);
        return 1;
    }

    inline int getgamestate(lua_State* L)
    {
        std::lock_guard<std::mutex> lock(g_vm_mutex);
        for (auto& [vm, entry] : g_vms)
        {
            if (!entry.isActor)
            {
                push_state(L, entry.id);
                return 1;
            }
        }
        lua_pushnil(L);
        return 1;
    }

    inline int getactorstates(lua_State* L)
    {
        lua_pushcfunction(L, getactors, "getactors");
        lua_call(L, 0, 1);
        const int all = lua_gettop(L);
        const int total = lua_objlen(L, all);
        for (int i = 1; i <= total; ++i)
        {
            lua_rawgeti(L, all, i);
            ensure_actor_state(L, lua_gettop(L));
            lua_pop(L, 1);
        }

        lua_newtable(L);
        int count = 0;
        std::vector<int> ids;
        {
            std::lock_guard<std::mutex> lock(g_vm_mutex);
            for (auto& [vm, entry] : g_vms)
            {
                if (entry.isActor)
                    ids.push_back(entry.id);
            }
        }
        std::sort(ids.begin(), ids.end());
        for (int id : ids)
        {
            push_state(L, id);
            lua_rawseti(L, -2, ++count);
        }
        return 1;
    }

    inline int get_current_actor(lua_State* L)
    {
        std::shared_ptr<uintptr_t> actor;
        if (L->userdata)
            actor = L->userdata->actor.lock();
        if (!actor && L->global && L->global->mainthread && L->global->mainthread->userdata)
            actor = L->global->mainthread->userdata->actor.lock();
        if (!actor || !*actor)
        {
            lua_pushnil(L);
            return 1;
        }

        push_actor_instances(L, std::unordered_set<uintptr_t>{ *actor });
        lua_rawgeti(L, -1, 1);
        return 1;
    }

    inline int getactorthreads(lua_State* L)
    {
        return getactorstates(L);
    }

    inline std::unordered_set<uintptr_t> live_actor_pointers(lua_State* L)
    {
        std::unordered_set<uintptr_t> live;
        lua_pushcfunction(L, getactors, "getactors");
        lua_call(L, 0, 1);
        const int all = lua_gettop(L);
        const int total = lua_objlen(L, all);
        for (int i = 1; i <= total; ++i)
        {
            lua_rawgeti(L, all, i);
            if (void* ud = lua_touserdata(L, -1))
                live.insert(*static_cast<uintptr_t*>(ud));
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        return live;
    }

    inline int getdeletedactors(lua_State* L)
    {
        const std::unordered_set<uintptr_t> live = live_actor_pointers(L);

        std::vector<int> ids;
        {
            std::lock_guard<std::mutex> lock(g_vm_mutex);
            for (auto& [vm, entry] : g_vms)
            {
                if (!entry.isActor || entry.actors.empty())
                    continue;
                bool alive = false;
                for (uintptr_t actor : entry.actors)
                {
                    if (live.count(actor))
                    {
                        alive = true;
                        break;
                    }
                }
                if (!alive)
                    ids.push_back(entry.id);
            }
        }
        std::sort(ids.begin(), ids.end());

        lua_createtable(L, static_cast<int>(ids.size()), 0);
        int count = 0;
        for (int id : ids)
        {
            push_state(L, id);
            lua_rawseti(L, -2, ++count);
        }
        return 1;
    }

    inline int getactorfromthread(lua_State* L)
    {
        std::unordered_set<uintptr_t> actors;
        if (lua_type(L, 1) == LUA_TUSERDATA)
        {
            const int id = check_state(L, 1);
            std::lock_guard<std::mutex> lock(g_vm_mutex);
            if (LuaVm* vm = find_vm_by_id(id))
                actors = vm->actors;
        }
        else
        {
            luaL_checktype(L, 1, LUA_TTHREAD);
            lua_State* target = lua_tothread(L, 1);
            std::shared_ptr<uintptr_t> actor;
            if (target && target->userdata)
                actor = target->userdata->actor.lock();
            if (!actor && target && target->global && target->global->mainthread &&
                target->global->mainthread->userdata)
                actor = target->global->mainthread->userdata->actor.lock();
            if (actor && *actor)
                actors.insert(*actor);
        }

        if (actors.empty())
        {
            lua_pushnil(L);
            return 1;
        }
        push_actor_instances(L, actors);
        lua_rawgeti(L, -1, 1);
        return 1;
    }

    inline int run_on_thread_ex(lua_State* L)
    {
        if (lua_type(L, 1) == LUA_TSTRING)
            return run_on_thread(L);
        if (lua_type(L, 1) == LUA_TUSERDATA)
            return state_execute(L);

        if (!IsElevatedExecutorThread(L))
            luaL_error(L, "%s", CodeLoadDeniedReason(L));

        luaL_checktype(L, 1, LUA_TTHREAD);
        size_t length = 0;
        const char* source = luaL_checklstring(L, 2, &length);
        if (length == 0)
            luaL_argerror(L, 2, "source must not be empty");
        const std::string bytecode = Execution::CompileScript(std::string(source, length));
        const int argc = lua_gettop(L) - 2;

        lua_State* thread = lua_newthread(L);
        const int threadIndex = lua_gettop(L);
        lua_getfenv(L, 1);
        lua_setfenv(L, threadIndex);
        TaskScheduler::SetThreadCapabilities(thread, 8, MaxCapabilities);

        if (luau_load(thread, "=run_on_thread", bytecode.c_str(), bytecode.size(), 0) != LUA_OK)
        {
            const char* err = lua_tostring(thread, -1);
            const std::string message = err ? err : "failed to load";
            lua_pop(thread, 1);
            luaL_error(L, "run_on_thread: %s", message.c_str());
        }
        if (Closure* cl = clvalue(luaA_toobject(thread, -1)); cl && cl->l.p)
            TaskScheduler::SetProtoCapabilities(cl->l.p, &MaxCapabilities);

        for (int i = 3; i <= argc + 2; ++i)
        {
            lua_pushvalue(L, i);
            lua_xmove(L, thread, 1);
        }

        Roblox::TaskDefer(thread);
        lua_settop(L, threadIndex - 1);
        return 0;
    }

    inline int luastateproxy_new(lua_State* L)
    {
        push_state(L, current_state_id(L));
        return 1;
    }

    inline int watch_descendant(lua_State* L)
    {
        if (!lua_isuserdata(L, 1))
            return 0;
        lua_getfield(L, 1, OBF("ClassName"));
        const bool actor = lua_isstring(L, -1) && strcmp(lua_tostring(L, -1), "Actor") == 0;
        lua_pop(L, 1);
        if (!actor)
            return 0;

        bool created = false;
        if (!ensure_actor_state(L, 1, &created) || !created)
            return 0;

        lua_pushlightuserdata(L, &StateCreatedKey);
        lua_rawget(L, LUA_REGISTRYINDEX);
        lua_pushvalue(L, 1);
        LuaSignal::FireFromC(L, -2, 1);
        lua_pop(L, 1);
        return 0;
    }

    inline int start_actor_watch(lua_State* L)
    {
        lua_pushcfunction(L, getactors, "getactors");
        lua_call(L, 0, 1);
        const int all = lua_gettop(L);
        const int total = lua_objlen(L, all);
        for (int i = 1; i <= total; ++i)
        {
            lua_pushcfunction(L, watch_descendant, "watch_descendant");
            lua_rawgeti(L, all, i);
            if (lua_pcall(L, 1, 0, 0) != LUA_OK)
                lua_pop(L, 1);
        }
        lua_settop(L, 0);

        lua_getglobal(L, "game");
        lua_getfield(L, -1, "DescendantAdded");
        lua_getfield(L, -1, "Connect");
        lua_pushvalue(L, -2);
        Utils::PushGuardedFunction(L, "on_actor_state_created", watch_descendant);
        lua_call(L, 2, 1);
        lua_pushlightuserdata(L, &ActorWatchKey);
        lua_insert(L, -2);
        lua_rawset(L, LUA_REGISTRYINDEX);
        lua_settop(L, 0);
        return 0;
    }

    inline void RegisterStateTypes(lua_State* L)
    {
        luaL_newmetatable(L, StateProxyName);
        lua_pushcfunction(L, state_index, "__index");
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, state_eq, "__eq");
        lua_setfield(L, -2, "__eq");
        lua_pushcfunction(L, state_tostring, "__tostring");
        lua_setfield(L, -2, "__tostring");
        lua_pushstring(L, StateProxyName);
        lua_setfield(L, -2, "__type");
        lua_pushstring(L, "The metatable is locked");
        lua_setfield(L, -2, "__metatable");
        lua_pop(L, 1);

        luaL_newmetatable(L, StateEventName);
        lua_pushcfunction(L, event_index, "__index");
        lua_setfield(L, -2, "__index");
        lua_pushstring(L, StateEventName);
        lua_setfield(L, -2, "__type");
        lua_pushstring(L, "The metatable is locked");
        lua_setfield(L, -2, "__metatable");
        lua_pop(L, 1);

        lua_newtable(L);
        Utils::AddTableFunction(L, "new", luastateproxy_new);
        lua_setreadonly(L, -1, true);
        lua_setglobal(L, StateProxyName);
    }

    inline void RegisterGameEnvironment(lua_State* L)
    {
        RegisterGameState(L);

        LuaSignal::PushNew(L);
        lua_pushlightuserdata(L, &StateCreatedKey);
        lua_pushvalue(L, -2);
        lua_rawset(L, LUA_REGISTRYINDEX);
        lua_setglobal(L, "on_actor_state_created");

        lua_getglobal(L, "task");
        lua_getfield(L, -1, "defer");
        lua_remove(L, -2);
        Utils::PushGuardedFunction(L, "on_actor_state_created", start_actor_watch);
        if (lua_pcall(L, 1, 0, 0) != LUA_OK)
            lua_pop(L, 1);
    }

    inline void RegisterLibrary(lua_State* L) {
        if (!L) return;
        RegisterStateTypes(L);
        Utils::AddFunction(L, OBF("getluastate"), getluastate);
        Utils::AddFunction(L, OBF("getgamestate"), getgamestate);
        Utils::AddFunction(L, OBF("getactorstates"), getactorstates);
        Utils::AddFunction(L, OBF("get_current_actor"), get_current_actor);
        Utils::AddFunction(L, OBF("getactors"), getactors);
        Utils::AddFunction(L, OBF("isparallel"), actor_isparallel);
        Utils::AddFunction(L, OBF("checkparallel"), actor_isparallel);
        Utils::AddFunction(L, OBF("get_comm_channel"), get_comm_channel);
        Utils::AddFunction(L, OBF("create_comm_channel"), create_comm_channel);
        Utils::AddFunction(L, OBF("run_on_actor"), run_on_actor);
        Utils::AddFunction(L, OBF("runonactor"), run_on_actor);
        Utils::AddFunction(L, OBF("runonthread"), run_on_thread_ex);
        Utils::AddFunction(L, OBF("run_on_thread"), run_on_thread_ex);
        Utils::AddFunction(L, OBF("getcurrentactor"), get_current_actor);
        Utils::AddFunction(L, OBF("is_parallel"), actor_isparallel);
        Utils::AddFunction(L, OBF("getactorthreads"), getactorthreads);
        Utils::AddFunction(L, OBF("get_actor_threads"), getactorthreads);
        Utils::AddFunction(L, OBF("getactorfromthread"), getactorfromthread);
        Utils::AddFunction(L, OBF("getactorfromstate"), getactorfromthread);
        Utils::AddFunction(L, OBF("getdeletedactors"), getdeletedactors);
    }
}
