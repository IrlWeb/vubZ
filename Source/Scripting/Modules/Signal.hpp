#pragma once

#include <lstate.h>
#include <lualib.h>
#include <Core/Utilities.hpp>

namespace LuaSignal
{
    inline char SignalStateKey;
    inline char ConnectionStateKey;

    inline char SharedKey;

    inline void PushShared(lua_State* L, const char* field)
    {
        lua_pushlightuserdata(L, &SharedKey);
        lua_rawget(L, LUA_REGISTRYINDEX);
        if (!lua_istable(L, -1))
            luaL_error(L, "Signal library is not initialized");

        lua_getfield(L, -1, field);
        lua_remove(L, -2);
    }

    inline int CheckState(lua_State* L, int idx, void* key, const char* kind)
    {
        idx = lua_absindex(L, idx);
        if (lua_type(L, idx) == LUA_TUSERDATA && lua_getmetatable(L, idx))
        {
            lua_pushlightuserdata(L, key);
            lua_rawget(L, -2);
            lua_remove(L, -2);
            if (lua_istable(L, -1))
                return lua_gettop(L);

            lua_pop(L, 1);
        }

        luaL_typeerrorL(L, idx, kind);
        return 0;
    }

    inline void MakeObjectMetatable(lua_State* L, void* key, int state, int index, const char* tostringField)
    {
        lua_createtable(L, 0, 4);

        lua_pushvalue(L, index);
        lua_setfield(L, -2, "__index");

        PushShared(L, tostringField);
        lua_setfield(L, -2, "__tostring");

        lua_pushstring(L, "The metatable is locked");
        lua_setfield(L, -2, "__metatable");

        lua_pushlightuserdata(L, key);
        lua_pushvalue(L, state);
        lua_rawset(L, -3);
    }

    inline bool DisconnectState(lua_State* L, int conn)
    {
        lua_getfield(L, conn, "index");
        lua_getfield(L, -1, "Connected");
        const bool connected = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);

        if (!connected)
        {
            lua_pop(L, 1);
            return false;
        }

        lua_pushboolean(L, 0);
        lua_setfield(L, -2, "Connected");
        lua_pop(L, 1);

        lua_getfield(L, conn, "signal");
        const int signal = lua_gettop(L);
        const int count = lua_objlen(L, signal);

        for (int i = 1; i <= count; ++i)
        {
            lua_rawgeti(L, signal, i);
            if (!lua_getmetatable(L, -1))
            {
                lua_pop(L, 1);
                continue;
            }

            lua_pushlightuserdata(L, &ConnectionStateKey);
            lua_rawget(L, -2);
            const bool self = lua_rawequal(L, -1, conn) != 0;
            lua_pop(L, 3);

            if (!self)
                continue;

            for (int j = i; j < count; ++j)
            {
                lua_rawgeti(L, signal, j + 1);
                lua_rawseti(L, signal, j);
            }

            lua_pushnil(L);
            lua_rawseti(L, signal, count);
            break;
        }

        lua_pop(L, 1);

        lua_pushnil(L);
        lua_setfield(L, conn, "fn");
        return true;
    }

    inline int New(lua_State* L)
    {
        lua_newuserdata(L, 1);
        const int object = lua_gettop(L);

        lua_createtable(L, 0, 1);
        lua_newtable(L);
        lua_setfield(L, -2, "waiters");
        const int state = lua_gettop(L);

        PushShared(L, "methods");
        const int methods = lua_gettop(L);

        MakeObjectMetatable(L, &SignalStateKey, state, methods, "signaltostring");
        lua_setmetatable(L, object);

        lua_settop(L, object);
        return 1;
    }

    inline int ConnectWith(lua_State* L, bool once)
    {
        const int signal = CheckState(L, 1, &SignalStateKey, "Signal");
        luaL_checktype(L, 2, LUA_TFUNCTION);

        lua_newuserdata(L, 1);
        const int object = lua_gettop(L);

        lua_createtable(L, 0, 2);
        const int index = lua_gettop(L);
        lua_pushboolean(L, 1);
        lua_setfield(L, index, "Connected");
        PushShared(L, "disconnect");
        lua_setfield(L, index, "Disconnect");

        lua_createtable(L, 0, 4);
        const int state = lua_gettop(L);
        lua_pushvalue(L, 2);
        lua_setfield(L, state, "fn");
        lua_pushboolean(L, once);
        lua_setfield(L, state, "once");
        lua_pushvalue(L, signal);
        lua_setfield(L, state, "signal");
        lua_pushvalue(L, index);
        lua_setfield(L, state, "index");

        MakeObjectMetatable(L, &ConnectionStateKey, state, index, "conntostring");
        lua_setmetatable(L, object);

        lua_pushvalue(L, object);
        lua_rawseti(L, signal, lua_objlen(L, signal) + 1);

        lua_settop(L, object);
        return 1;
    }

    inline int Connect(lua_State* L)
    {
        return ConnectWith(L, false);
    }

    inline int Once(lua_State* L)
    {
        return ConnectWith(L, true);
    }

    inline int Disconnect(lua_State* L)
    {
        const int state = CheckState(L, 1, &ConnectionStateKey, "Connection");
        DisconnectState(L, state);
        return 0;
    }

    inline void SpawnWith(lua_State* L, int spawn, int target, int argCount)
    {
        lua_checkstack(L, argCount + 3);
        lua_pushvalue(L, spawn);
        lua_pushvalue(L, target);
        for (int i = 0; i < argCount; ++i)
            lua_pushvalue(L, 2 + i);

        if (lua_pcall(L, argCount + 1, 0, 0) != LUA_OK)
            lua_pop(L, 1);
    }

    inline int Fire(lua_State* L)
    {
        const int argCount = lua_gettop(L) - 1;
        const int signal = CheckState(L, 1, &SignalStateKey, "Signal");

        PushShared(L, "spawn");
        const int spawn = lua_gettop(L);

        const int count = lua_objlen(L, signal);
        lua_createtable(L, count, 0);
        const int connections = lua_gettop(L);
        for (int i = 1; i <= count; ++i)
        {
            lua_rawgeti(L, signal, i);
            lua_rawseti(L, connections, i);
        }

        lua_getfield(L, signal, "waiters");
        const int waiters = lua_gettop(L);
        lua_newtable(L);
        lua_setfield(L, signal, "waiters");

        for (int i = 1; i <= count; ++i)
        {
            lua_rawgeti(L, connections, i);
            const int object = lua_gettop(L);
            const int state = CheckState(L, object, &ConnectionStateKey, "Connection");

            lua_getfield(L, state, "fn");
            const int fn = lua_gettop(L);

            lua_getfield(L, state, "once");
            const bool once = lua_toboolean(L, -1) != 0;
            lua_pop(L, 1);

            if (lua_isfunction(L, fn))
            {
                if (once)
                    DisconnectState(L, state);

                SpawnWith(L, spawn, fn, argCount);
            }

            lua_settop(L, object - 1);
        }

        const int waiting = lua_objlen(L, waiters);
        for (int i = 1; i <= waiting; ++i)
        {
            lua_rawgeti(L, waiters, i);
            if (lua_isthread(L, -1))
                SpawnWith(L, spawn, lua_gettop(L), argCount);
            lua_pop(L, 1);
        }

        return 0;
    }

    inline int Wait(lua_State* L)
    {
        const int signal = CheckState(L, 1, &SignalStateKey, "Signal");

        if (!lua_isyieldable(L))
            luaL_error(L, "Signal:Wait cannot yield here");

        lua_getfield(L, signal, "waiters");
        lua_pushthread(L);
        lua_rawseti(L, -2, lua_objlen(L, -2) + 1);

        lua_settop(L, 0);
        return lua_yield(L, 0);
    }

    inline int DisconnectAll(lua_State* L)
    {
        const int signal = CheckState(L, 1, &SignalStateKey, "Signal");

        while (const int count = lua_objlen(L, signal))
        {
            lua_rawgeti(L, signal, count);
            const int state = CheckState(L, -1, &ConnectionStateKey, "Connection");
            if (!DisconnectState(L, state))
            {
                lua_pushnil(L);
                lua_rawseti(L, signal, count);
            }
            lua_settop(L, signal);
        }

        return 0;
    }

    inline int SignalToString(lua_State* L)
    {
        lua_pushstring(L, "Signal");
        return 1;
    }

    inline int ConnectionToString(lua_State* L)
    {
        lua_pushstring(L, "Connection");
        return 1;
    }

    inline void RegisterLibrary(lua_State* L)
    {
        if (!L)
            return;

        lua_pushlightuserdata(L, &SharedKey);
        lua_createtable(L, 0, 5);

        lua_createtable(L, 0, 7);
        Utils::AddTableFunction(L, "Connect", Connect);
        Utils::AddTableFunction(L, "Once", Once);
        Utils::AddTableFunction(L, "Wait", Wait);
        Utils::AddTableFunction(L, "Fire", Fire);
        Utils::AddTableFunction(L, "DisconnectAll", DisconnectAll);
        Utils::AddTableFunction(L, "Destroy", DisconnectAll);
        lua_setreadonly(L, -1, true);
        lua_setfield(L, -2, "methods");

        Utils::PushGuardedFunction(L, "Disconnect", Disconnect);
        lua_setfield(L, -2, "disconnect");
        Utils::PushGuardedFunction(L, "__tostring", SignalToString);
        lua_setfield(L, -2, "signaltostring");
        Utils::PushGuardedFunction(L, "__tostring", ConnectionToString);
        lua_setfield(L, -2, "conntostring");

        lua_getglobal(L, "task");
        if (lua_istable(L, -1))
            lua_getfield(L, -1, "spawn");
        else
            lua_pushnil(L);
        lua_remove(L, -2);
        lua_setfield(L, -2, "spawn");

        lua_rawset(L, LUA_REGISTRYINDEX);

        lua_createtable(L, 0, 1);
        Utils::AddTableFunction(L, "new", New);
        lua_setreadonly(L, -1, true);
        lua_pushvalue(L, -1);
        lua_setglobal(L, "VoltSignal");
        lua_pushvalue(L, -1);
        lua_setglobal(L, "PsmSignal");
        lua_pushvalue(L, -1);
        lua_setglobal(L, "RealSignal");
        lua_pushvalue(L, -1);
        lua_setglobal(L, "supQSignal");
        lua_setglobal(L, "Signal");
    }

    inline void PushNew(lua_State* L)
    {
        New(L);
    }

    inline void FireFromC(lua_State* L, int signal, int argCount)
    {
        signal = lua_absindex(L, signal);
        lua_pushcfunction(L, Fire, "Fire");
        lua_pushvalue(L, signal);
        for (int i = 0; i < argCount; ++i)
            lua_pushvalue(L, -(argCount + 2));
        if (lua_pcall(L, argCount + 1, 0, 0) != LUA_OK)
            lua_pop(L, 1);
        lua_pop(L, argCount);
    }
}
