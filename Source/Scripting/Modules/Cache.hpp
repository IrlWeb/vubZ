#include <cstring>
#include <lstate.h>
#include <lualib.h>
#include <lmem.h>
#include <Core/Utilities.hpp>
#include <Core/Context.hpp>

namespace Cache
{
    inline void* checkinstance(lua_State* L, int index)
    {
        luaL_checktype(L, index, LUA_TUSERDATA);

        if (strcmp(luaL_typename(L, index), "Instance") != 0)
            luaL_typeerrorL(L, index, "Instance");

        void** slot = static_cast<void**>(lua_touserdata(L, index));
        if (!slot)
            luaL_argerrorL(L, index, "Instance is not valid");

        return *slot;
    }

    inline void luaL_trimstack(lua_State* L, int n) {
        lua_settop(L, n);
    }

    inline int invalidate(lua_State* L)
    {
        const auto rawUserdata = checkinstance(L, 1);

        lua_pushlightuserdata(L, (void*)Roblox::PushInstance);
        lua_gettable(L, LUA_REGISTRYINDEX);

        lua_pushlightuserdata(L, reinterpret_cast<void*>(rawUserdata));
        lua_pushnil(L);
        lua_settable(L, -3);

        return 0;
    }

    inline int replace(lua_State* L)
    {
        const auto rawUserdata = checkinstance(L, 1);
        checkinstance(L, 2);

        lua_pushlightuserdata(L, (void*)Roblox::PushInstance);
        lua_gettable(L, LUA_REGISTRYINDEX);

        lua_pushlightuserdata(L, rawUserdata);
        lua_pushvalue(L, 2);
        lua_settable(L, -3);

        return 0;
    }

    inline int iscached(lua_State* L)
    {
        const auto rawUserdata = checkinstance(L, 1);

        lua_pushlightuserdata(L, (void*)Roblox::PushInstance);
        lua_gettable(L, LUA_REGISTRYINDEX);

        lua_pushlightuserdata(L, rawUserdata);
        lua_gettable(L, -2);

        lua_pushboolean(L, lua_rawequal(L, -1, 1));

        return 1;
    }

    int getreg(lua_State* L) {
        Cache::luaL_trimstack(L, (int)0);

        lua_rawcheckstack(L, 1);
        luaC_threadbarrier(L);

        lua_pushvalue(L, LUA_REGISTRYINDEX);
        return 1;
    };

    inline int compareinstances(lua_State* L)
    {
        uintptr_t instance_one = reinterpret_cast<uintptr_t>(checkinstance(L, 1));
        uintptr_t instance_two = reinterpret_cast<uintptr_t>(checkinstance(L, 2));

        lua_pushboolean(L, instance_one == instance_two);

        return 1;
    }

    inline int getinstancecache(lua_State* L)
    {
        lua_pushlightuserdata(L, (void*)Roblox::PushInstance);
        lua_rawget(L, LUA_REGISTRYINDEX);
        if (!lua_istable(L, -1))
        {
            lua_pop(L, 1);
            lua_newtable(L);
        }
        return 1;
    }

    inline void RegisterLibrary(lua_State* L)
    {
        if (!L) return;

        lua_newtable(L);

        Utils::AddTableFunction(L, "invalidate", invalidate);
        Utils::AddTableFunction(L, "iscached", iscached);
        Utils::AddTableFunction(L, "replace", replace);

        lua_setglobal(L, "cache");

        Utils::AddFunction(L, "getinstancecache", getinstancecache);
        Utils::AddFunction(L, "getreg", getreg);
        Utils::AddFunction(L, "getregistry", getreg);
        Utils::AddFunction(L, "compareinstances", compareinstances);
    }
}
