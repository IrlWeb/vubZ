#pragma once

#include <Windows.h>
#include <cstring>
#include <lstate.h>
#include <lobject.h>
#include <lfunc.h>
#include <lstring.h>
#include <ltable.h>
#include <lapi.h>
#include <lgc.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>
#include <lualib.h>

#include <Core/Utilities.hpp>
#include <Core/Context.hpp>
#include <Scripting/Bridge/LuauHelper.hpp>
#include <Scripting/Modules/Closures.hpp>
#include <Scripting/Modules/Oth.hpp>

namespace Metatable {
    inline int getrawmetatable(lua_State* LS) {

        if (LS->userdata == nullptr)
            return 0;

        luaL_checkany(LS, 1);

        if (!lua_getmetatable(LS, 1))
            lua_pushnil(LS);

        return 1;
    };

    int setreadonly(lua_State* L) {
        luaL_checktype(L, 1, LUA_TTABLE);
        luaL_checktype(L, 2, LUA_TBOOLEAN);
        hvalue(luaA_toobject(L, 1))->readonly = lua_toboolean(L, 2);
        return 0;
    }

    int isreadonly(lua_State* L) {
        luaL_checktype(L, 1, LUA_TTABLE);
        lua_pushboolean(L, hvalue(luaA_toobject(L, 1))->readonly);
        return 1;
    }

    int setrawmetatable(lua_State* L)
    {
        luaL_checkany(L, 1);

        const int metatableType = lua_type(L, 2);

        if (metatableType != LUA_TTABLE && metatableType != LUA_TNIL)
            luaL_typeerror(L, 2, "table or nil");

        lua_settop(L, 2);
        lua_setmetatable(L, 1);

        lua_pushvalue(L, 1);
        return 1;
    }

    int makereadonly(lua_State* L) {

        luaL_checktype(L, 1, LUA_TTABLE);

        LuaTable* table = hvalue(luaA_toobject(L, 1));

        table->readonly = true;

        return 0;
    }

    int makewriteable(lua_State* L) {

        luaL_checktype(L, 1, LUA_TTABLE);

        LuaTable* table = hvalue(luaA_toobject(L, 1));

        table->readonly = false;

        return 0;
    }

    int iswritable(lua_State* L) {
        luaL_checktype(L, 1, LUA_TTABLE);
        lua_pushboolean(L, !hvalue(luaA_toobject(L, 1))->readonly);
        return 1;
    }

    inline int getnamecallmethod(lua_State* L)
    {
        return Oth::Namecall(L);
    }

    inline int setnamecallmethod(lua_State* L)
    {
        return Oth::SetNamecall(L);
    }

    inline int hookmetamethod(lua_State* L)
    {
        luaL_checkany(L, 1);
        luaL_checktype(L, 2, LUA_TSTRING);
        luaL_checktype(L, 3, LUA_TFUNCTION);

        const char* metamethod = lua_tostring(L, 2);

        if (!lua_getmetatable(L, 1)) {
            luaL_argerror(L, 1, "object has no metatable");
        }
        int mt_idx = lua_gettop(L);

        bool wasReadonly = lua_getreadonly(L, mt_idx);
        if (wasReadonly) {
            lua_setreadonly(L, mt_idx, false);
        }

        lua_getfield(L, mt_idx, metamethod);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            if (wasReadonly) lua_setreadonly(L, mt_idx, true);
            luaL_argerror(L, 2, "metamethod does not exist");
        }
        int old_method = lua_gettop(L);

        lua_pushvalue(L, 3);
        lua_setfield(L, mt_idx, metamethod);

        if (wasReadonly) {
            lua_setreadonly(L, mt_idx, true);
        }

        lua_pushvalue(L, old_method);
        return 1;
    }

    inline int hookmetamethodV2(lua_State* L)
    {
        luaL_checkany(L, 1);
        luaL_checktype(L, 2, LUA_TSTRING);
        luaL_checktype(L, 3, LUA_TFUNCTION);

        if (!lua_getmetatable(L, 1))
            luaL_argerror(L, 1, "object has no metatable");
        const int mt_idx = lua_gettop(L);

        const char* method = lua_tostring(L, 2);
        lua_rawgetfield(L, mt_idx, method);
        if (!lua_isfunction(L, -1))
        {
            lua_pop(L, 1);
            luaL_argerror(L, 2, "metamethod is not a function");
        }
        const int mm_idx = lua_gettop(L);

        const bool lockedMetatable = lua_getreadonly(L, mt_idx);

        if (!lockedMetatable)
        {
            lua_pushcfunction(L, Closures::hookfunction, "hookmetamethod");
            lua_pushvalue(L, mm_idx);
            lua_pushvalue(L, 3);
            lua_call(L, 2, 1);
            return 1;
        }

        lua_setreadonly(L, mt_idx, false);
        lua_pushvalue(L, 3);
        lua_rawsetfield(L, mt_idx, method);
        lua_setreadonly(L, mt_idx, true);

        lua_pushvalue(L, mm_idx);
        return 1;
    }

    inline int hookmetamethodV3(lua_State* L) {
        luaL_checkany(L, 1);
        luaL_checktype(L, 2, LUA_TSTRING);
        luaL_checktype(L, 3, LUA_TFUNCTION);

        if (!lua_getmetatable(L, 1))
            luaL_argerror(L, 1, "object has no metatable");

        const int mta = lua_gettop(L);
        const char* method = lua_tostring(L, 2);
        lua_rawgetfield(L, mta, method);

        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 2);
            luaL_argerror(L, 2, "metamethod is not a function");
        }

        const int fff = lua_gettop(L);

        {
            Closure* Metamethod = clvalue(index2addr(L, fff));
            if (Metamethod->isC && !Closures::IsExecutorOwnedClosure(Metamethod))
                Closures::mark_hook_plumbing(Metamethod);
        }

        if (strcmp(method, "__namecall") == 0)
        {
            lua_pushcfunction(L, Oth::Hook, "hook");
            lua_pushvalue(L, fff);
            lua_pushvalue(L, 3);
            lua_call(L, 2, 1);
            return 1;
        }

        lua_pushcfunction(L, Closures::hookfunction, "hookmetamethod");
        lua_pushvalue(L, fff);
        lua_pushvalue(L, 3);
        lua_call(L, 2, 1);
        return 1;
    }

    inline void RegisterLibrary(lua_State* L)
    {
        Utils::AddFunction(L, "hookmetamethod", hookmetamethodV3);
        Utils::AddFunction(L, "getnamecallmethod", getnamecallmethod);
        Utils::AddFunction(L, "setnamecallmethod", setnamecallmethod);
        Utils::AddFunction(L, "setreadonly", setreadonly);
        Utils::AddFunction(L, "isreadonly", isreadonly);
        Utils::AddFunction(L, "getrawmetatable", getrawmetatable);
        Utils::AddFunction(L, "setrawmetatable", setrawmetatable);
        Utils::AddFunction(L, "makereadonly", makereadonly);
        Utils::AddFunction(L, "makewriteable", makewriteable);
        Utils::AddFunction(L, "makewritable", makewriteable);
        Utils::AddFunction(L, "iswritable", iswritable);
        Utils::AddFunction(L, "iswriteable", iswritable);
    }
}
