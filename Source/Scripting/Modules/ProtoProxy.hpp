#pragma once

#include <lstate.h>
#include <lualib.h>
#include <lapi.h>
#include <lobject.h>

#include <cstdint>

namespace ProtoProxy
{
    inline const char* const MetatableName = "ProtoProxy";
    inline char AnchorKey;

    struct Data
    {
        Proto* proto;
    };

    inline Proto* To(lua_State* L, int idx)
    {
        if (lua_type(L, idx) != LUA_TUSERDATA)
            return nullptr;

        idx = lua_absindex(L, idx);
        if (!lua_getmetatable(L, idx))
            return nullptr;

        luaL_getmetatable(L, MetatableName);
        const bool ours = lua_rawequal(L, -1, -2) != 0;
        lua_pop(L, 2);
        if (!ours)
            return nullptr;

        return static_cast<Data*>(lua_touserdata(L, idx))->proto;
    }

    inline void PushAnchors(lua_State* L)
    {
        lua_pushlightuserdata(L, &AnchorKey);
        lua_rawget(L, LUA_REGISTRYINDEX);
    }

    inline void Push(lua_State* L, Proto* proto, int owner)
    {
        owner = lua_absindex(L, owner);

        auto* data = static_cast<Data*>(lua_newuserdata(L, sizeof(Data)));
        data->proto = proto;
        luaL_getmetatable(L, MetatableName);
        lua_setmetatable(L, -2);
        const int proxy = lua_gettop(L);

        PushAnchors(L);
        if (lua_istable(L, -1))
        {
            lua_pushvalue(L, proxy);
            if (To(L, owner))
            {
                lua_pushvalue(L, owner);
                lua_rawget(L, -3);
            }
            else
            {
                lua_pushvalue(L, owner);
            }
            lua_rawset(L, -3);
        }
        lua_pop(L, 1);
    }

    inline int32_t CodeHash(const Proto* proto)
    {
        uint32_t hash = 2166136261u;
        for (int i = 0; proto && i < proto->sizecode; ++i)
        {
            const uint32_t insn = static_cast<uint32_t>(proto->code[i]);
            for (int shift = 0; shift < 32; shift += 8)
            {
                hash ^= (insn >> shift) & 0xFF;
                hash *= 16777619u;
            }
        }
        return static_cast<int32_t>(hash);
    }

    inline int Index(lua_State* L)
    {
        Proto* proto = To(L, 1);
        const char* key = lua_tostring(L, 2);
        if (proto && key && strcmp(key, "CodeHash") == 0)
        {
            lua_pushinteger(L, CodeHash(proto));
            return 1;
        }

        lua_pushnil(L);
        return 1;
    }

    inline int Call(lua_State*)
    {
        return 0;
    }

    inline int ToString(lua_State* L)
    {
        Proto* proto = To(L, 1);
        const char* name = proto && proto->debugname ? getstr(proto->debugname) : "anonymous";
        lua_pushfstring(L, "ProtoProxy: %s", name);
        return 1;
    }

    inline void RegisterLibrary(lua_State* L)
    {
        luaL_newmetatable(L, MetatableName);
        lua_pushstring(L, "ProtoProxy");
        lua_setfield(L, -2, "__type");
        lua_pushcfunction(L, Index, "__index");
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, Call, "__call");
        lua_setfield(L, -2, "__call");
        lua_pushcfunction(L, ToString, "__tostring");
        lua_setfield(L, -2, "__tostring");
        lua_pushstring(L, "The metatable is locked");
        lua_setfield(L, -2, "__metatable");
        lua_pop(L, 1);

        lua_pushlightuserdata(L, &AnchorKey);
        lua_newtable(L);
        lua_createtable(L, 0, 1);
        lua_pushstring(L, "k");
        lua_setfield(L, -2, "__mode");
        lua_setmetatable(L, -2);
        lua_rawset(L, LUA_REGISTRYINDEX);
    }
}
