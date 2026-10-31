#pragma once

#include <lstate.h>
#include <lobject.h>
#include <lapi.h>
#include <lgc.h>
#include <lualib.h>

#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <Core/Utilities.hpp>
#include <Security/BrandGuard.hpp>
#include <Scripting/Modules/Closures.hpp>
#include <Scripting/Modules/Crypt.hpp>
#include <Scripting/Modules/Http.hpp>
#include <Scripting/Modules/Instance.hpp>
#include <Scripting/Modules/Miscellaneous.hpp>
#include <Scripting/Modules/Reflection.hpp>
#include <Scripting/Modules/Script.hpp>
#include <Scripting/Modules/Disassembler.hpp>
#include "Update/Offsets.hpp"

namespace Compat
{
    inline char SetNameAnchorKey;

    inline void resolve_level(lua_State* L, int index)
    {
        if (lua_type(L, index) != LUA_TNUMBER)
            return;

        const int level = static_cast<int>(lua_tointeger(L, index));
        lua_Debug ar{};
        if (level < 0 || !lua_getinfo(L, level, "f", &ar))
            luaL_argerror(L, index, "Level out of range");
        lua_replace(L, index);
    }

    inline int comparefunctions(lua_State* L)
    {
        luaL_checktype(L, 1, LUA_TFUNCTION);
        luaL_checktype(L, 2, LUA_TFUNCTION);
        lua_pushboolean(L, lua_topointer(L, 1) == lua_topointer(L, 2));
        return 1;
    }

    inline int ishiddenstack(lua_State* L)
    {
        resolve_level(L, 1);
        luaL_checktype(L, 1, LUA_TFUNCTION);
        lua_pushboolean(L, Closures::is_hidden_cl(clvalue(luaA_toobject(L, 1))));
        return 1;
    }

    inline int getmetafield(lua_State* L)
    {
        luaL_checkany(L, 1);
        luaL_checkstring(L, 2);
        if (!lua_getmetatable(L, 1))
        {
            lua_pushnil(L);
            return 1;
        }
        lua_pushvalue(L, 2);
        lua_rawget(L, -2);
        return 1;
    }

    inline int hasmetafield(lua_State* L)
    {
        luaL_checkany(L, 1);
        luaL_checkstring(L, 2);
        bool present = false;
        if (lua_getmetatable(L, 1))
        {
            lua_pushvalue(L, 2);
            lua_rawget(L, -2);
            present = !lua_isnil(L, -1);
        }
        lua_pushboolean(L, present);
        return 1;
    }

    inline int hasmetatable(lua_State* L)
    {
        luaL_checkany(L, 1);
        lua_pushboolean(L, lua_getmetatable(L, 1) != 0);
        return 1;
    }

    inline int getscriptglobals(lua_State* L)
    {
        lua_createtable(L, 0, 2);
        lua_rawgeti(L, LUA_REGISTRYINDEX, 2);
        lua_setfield(L, -2, "_G");
        lua_rawgeti(L, LUA_REGISTRYINDEX, 4);
        lua_setfield(L, -2, "shared");
        return 1;
    }

    inline int getexecutorversion(lua_State* L)
    {
        lua_pushstring(L, BrandGuard::Version().c_str());
        return 1;
    }

    inline int getfflagtype(lua_State* L)
    {
        const std::string name = luaL_checkstring(L, 1);

        std::uintptr_t flag = Miscellaneous::FindFlagObject(name.c_str());
        std::string resolved = name;
        if (!flag)
        {
            const std::string stripped = Miscellaneous::StripFlagPrefix(name);
            if (!stripped.empty() && (flag = Miscellaneous::FindFlagObject(stripped.c_str())))
                resolved = stripped;
        }

        if (!flag)
        {
            lua_pushstring(L, "unknown");
            return 1;
        }

        uintptr_t identity = 0;
        if (Miscellaneous::ReadIdentity(flag, identity))
        {
            if (identity == Offsets::FFlags::IntegerType || identity == Offsets::FFlags::IntegerValueType)
            {
                lua_pushstring(L, "int");
                return 1;
            }
            if (identity == Offsets::FFlags::BooleanType || identity == Offsets::FFlags::BooleanValueType)
            {
                lua_pushstring(L, "bool");
                return 1;
            }
        }

        std::string value;
        lua_pushstring(L, Miscellaneous::GetFlag(resolved, value) ? "string" : "unknown");
        return 1;
    }

    inline int getobjects(lua_State* L)
    {
        if (lua_type(L, 1) == LUA_TNUMBER)
        {
            const std::string url = "rbxassetid://" + std::to_string(static_cast<long long>(lua_tonumber(L, 1)));
            lua_pushstring(L, url.c_str());
            lua_replace(L, 1);
        }
        if (lua_type(L, 1) == LUA_TSTRING)
        {
            lua_getglobal(L, "game");
            lua_insert(L, 1);
        }
        return Miscellaneous::GetObjects(L);
    }

    inline int info(lua_State* L)
    {
        const int count = lua_gettop(L);
        std::string message;
        for (int i = 1; i <= count; ++i)
        {
            size_t length = 0;
            const char* text = luaL_tolstring(L, i, &length);
            if (i > 1)
                message.push_back('\t');
            message.append(text, length);
            lua_pop(L, 1);
        }
        Roblox::Print(1, "%s", message.c_str());
        return 0;
    }

    inline int setname(lua_State* L)
    {
        resolve_level(L, 1);
        luaL_checktype(L, 1, LUA_TFUNCTION);
        luaL_checktype(L, 2, LUA_TSTRING);

        Closure* cl = clvalue(luaA_toobject(L, 1));
        TString* name = tsvalue(luaA_toobject(L, 2));

        lua_pushlightuserdata(L, &SetNameAnchorKey);
        lua_rawget(L, LUA_REGISTRYINDEX);
        if (!lua_istable(L, -1))
        {
            lua_pop(L, 1);
            lua_newtable(L);
            lua_createtable(L, 0, 1);
            lua_pushstring(L, "k");
            lua_setfield(L, -2, "__mode");
            lua_setmetatable(L, -2);
            lua_pushlightuserdata(L, &SetNameAnchorKey);
            lua_pushvalue(L, -2);
            lua_rawset(L, LUA_REGISTRYINDEX);
        }
        lua_pushvalue(L, 1);
        lua_pushvalue(L, 2);
        lua_rawset(L, -3);
        lua_pop(L, 1);

        if (cl->isC)
        {
            cl->c.debugname = name;
            luaC_objbarrier(L, cl, name);
        }
        else if (Proto* p = cl->l.p)
        {
            p->debugname = name;
            luaC_objbarrier(L, p, name);
        }
        return 0;
    }

    inline LuaTable* safeenv_target(lua_State* L, int index)
    {
        switch (lua_type(L, index))
        {
        case LUA_TFUNCTION:
            return clvalue(luaA_toobject(L, index))->env;
        case LUA_TTABLE:
            return hvalue(luaA_toobject(L, index));
        case LUA_TTHREAD:
            return thvalue(luaA_toobject(L, index))->gt;
        case LUA_TNONE:
        case LUA_TNIL:
        {
            lua_Debug ar{};
            if (lua_getinfo(L, 1, "f", &ar))
            {
                const Closure* caller = clvalue(luaA_toobject(L, -1));
                LuaTable* env = caller->isC ? L->gt : caller->env;
                lua_pop(L, 1);
                return env;
            }
            return L->gt;
        }
        default:
            luaL_typeerrorL(L, index, "function, table or thread");
            return nullptr;
        }
    }

    inline int getsafeenv(lua_State* L)
    {
        LuaTable* env = safeenv_target(L, 1);
        lua_pushboolean(L, env && env->safeenv != 0);
        return 1;
    }

    inline int setsafeenv(lua_State* L)
    {
        LuaTable* env = nullptr;
        bool safe = false;
        if (lua_type(L, 1) == LUA_TBOOLEAN)
        {
            safe = lua_toboolean(L, 1) != 0;
            lua_pushnil(L);
            env = safeenv_target(L, lua_gettop(L));
            lua_pop(L, 1);
        }
        else
        {
            env = safeenv_target(L, 1);
            safe = lua_toboolean(L, 2) != 0;
        }

        if (env)
            env->safeenv = safe ? 1 : 0;
        return 0;
    }

    inline bool read_hidden(lua_State* L, int instance, const char* name)
    {
        instance = lua_absindex(L, instance);
        lua_pushcfunction(L, Reflection::gethiddenproperty, "gethiddenproperty");
        lua_pushvalue(L, instance);
        lua_pushstring(L, name);
        if (lua_pcall(L, 2, 1, 0) != LUA_OK)
        {
            lua_pop(L, 1);
            return false;
        }
        return true;
    }

    inline int getbspval(lua_State* L)
    {
        luaL_checktype(L, 1, LUA_TUSERDATA);
        const char* property = luaL_checkstring(L, 2);
        const bool encode = lua_toboolean(L, 3) != 0;

        if (!read_hidden(L, 1, property) || lua_type(L, -1) != LUA_TSTRING)
            luaL_error(L, "getbspval: '%s' is not a readable BinaryString property", property);

        if (encode)
        {
            size_t length = 0;
            const char* raw = lua_tolstring(L, -1, &length);
            const std::string encoded = Crypt::base64_encode_bytes(reinterpret_cast<const uint8_t*>(raw), length);
            lua_pushlstring(L, encoded.data(), encoded.size());
        }
        return 1;
    }

    inline int getpcd(lua_State* L)
    {
        luaL_checktype(L, 1, LUA_TUSERDATA);
        if (!read_hidden(L, 1, "PhysicalConfigData"))
            luaL_error(L, "getpcd: PhysicalConfigData is unavailable on this instance");

        std::string data;
        if (lua_type(L, -1) == LUA_TSTRING)
        {
            size_t length = 0;
            const char* raw = lua_tolstring(L, -1, &length);
            data.assign(raw, length);
        }
        lua_pop(L, 1);

        std::string hex;
        std::string hash;
        if (Crypt::cryptoapi_hash(CALG_MD5, data.data(), data.size(), hex) && hex.size() == 32)
        {
            hash.resize(16);
            for (size_t i = 0; i < 16; ++i)
                hash[i] = static_cast<char>(std::stoi(hex.substr(i * 2, 2), nullptr, 16));
        }

        lua_pushlstring(L, hash.data(), hash.size());
        lua_pushlstring(L, data.data(), data.size());
        return 2;
    }

    inline int getreferencedby(lua_State* L)
    {
        luaL_checktype(L, 1, LUA_TUSERDATA);
        void* ud = lua_touserdata(L, 1);
        const uintptr_t target = ud ? Reflection::seh_read_ptr(reinterpret_cast<uintptr_t>(ud)) : 0;
        if (!target)
            luaL_argerror(L, 1, "Instance expected");

        const uintptr_t parentDescriptor = Reflection::get_instance_prop_by_name(L, target, "Parent");
        const int refType = parentDescriptor ? Reflection::get_property_type(parentDescriptor) : -1;

        lua_newtable(L);
        const int result = lua_gettop(L);
        if (refType < 0)
            return 1;

        lua_pushcfunction(L, Instance::getinstances, "getinstances");
        lua_call(L, 0, 1);
        const int all = lua_gettop(L);

        std::unordered_map<uintptr_t, std::vector<std::string>> refProperties;
        int count = 0;

        const int total = lua_objlen(L, all);
        for (int i = 1; i <= total; ++i)
        {
            lua_rawgeti(L, all, i);
            const int item = lua_gettop(L);
            void* itemUd = lua_touserdata(L, item);
            const uintptr_t instance = itemUd ? Reflection::seh_read_ptr(reinterpret_cast<uintptr_t>(itemUd)) : 0;
            const uintptr_t classDescriptor = instance
                ? Reflection::seh_read_ptr(instance + Offsets::Instance::ClassDescriptor) : 0;
            if (!classDescriptor || instance == target)
            {
                lua_settop(L, all);
                continue;
            }

            auto cached = refProperties.find(classDescriptor);
            if (cached == refProperties.end())
            {
                std::vector<std::string> names;
                std::unordered_set<std::string> seen;
                for (uintptr_t descriptor : Reflection::get_all_properties_for_instance(instance))
                {
                    if (Reflection::get_property_type(descriptor) != refType)
                        continue;
                    const char* name = Reflection::get_property_name(descriptor);
                    if (!name || !name[0] || std::strcmp(name, "Parent") == 0 || !seen.insert(name).second)
                        continue;
                    names.emplace_back(name);
                }
                cached = refProperties.emplace(classDescriptor, std::move(names)).first;
            }

            for (const std::string& name : cached->second)
            {
                if (!read_hidden(L, item, name.c_str()))
                    continue;
                void* valueUd = lua_type(L, -1) == LUA_TUSERDATA ? lua_touserdata(L, -1) : nullptr;
                const bool match = valueUd &&
                    Reflection::seh_read_ptr(reinterpret_cast<uintptr_t>(valueUd)) == target;
                lua_pop(L, 1);
                if (match)
                {
                    lua_pushvalue(L, item);
                    lua_rawseti(L, result, ++count);
                    break;
                }
            }
            lua_settop(L, all);
        }

        lua_settop(L, result);
        return 1;
    }

    inline void RegisterDebugExtras(lua_State* L)
    {
        lua_getglobal(L, "debug");
        if (!lua_istable(L, -1))
        {
            lua_pop(L, 1);
            return;
        }
        const bool wasReadonly = lua_getreadonly(L, -1) != 0;
        lua_setreadonly(L, -1, false);
        Utils::AddTableFunction(L, "setname", setname);
        Utils::AddTableFunction(L, "getsafeenv", getsafeenv);
        Utils::AddTableFunction(L, "isuntouched", getsafeenv);
        Utils::AddTableFunction(L, "setsafeenv", setsafeenv);
        Utils::AddTableFunction(L, "setuntouched", setsafeenv);
        lua_setreadonly(L, -1, wasReadonly);
        lua_pop(L, 1);
    }

    inline void RegisterLibrary(lua_State* L)
    {
        Utils::AddFunction(L, "comparefunctions", comparefunctions);
        Utils::AddFunction(L, "comparefunction", comparefunctions);
        Utils::AddFunction(L, "ishiddenstack", ishiddenstack);
        Utils::AddFunction(L, "isstackhidden", ishiddenstack);
        Utils::AddFunction(L, "sethiddenstack", Closures::setstackhidden);
        Utils::AddFunction(L, "isourthread", Script::isexecutorthread);
        Utils::AddFunction(L, "is_our_thread", Script::isexecutorthread);

        Utils::AddFunction(L, "getmetafield", getmetafield);
        Utils::AddFunction(L, "hasmetafield", hasmetafield);
        Utils::AddFunction(L, "hasmetatable", hasmetatable);

        Utils::AddFunction(L, "getscriptglobals", getscriptglobals);
        Utils::AddFunction(L, "getexecutorversion", getexecutorversion);
        Utils::AddFunction(L, "getfflagtype", getfflagtype);
        Utils::AddFunction(L, "getfastflagtype", getfflagtype);
        Utils::AddFunction(L, "getobjects", getobjects);
        Utils::AddFunction(L, "httpget", Http::HttpGet);
        Utils::AddFunction(L, "info", info);

        Utils::AddFunction(L, "setname", setname);
        RegisterDebugExtras(L);

        Utils::AddFunction(L, "getbspval", getbspval);
        Utils::AddFunction(L, "getpcd", getpcd);
        Utils::AddFunction(L, "getpcdprop", getpcd);
        Utils::AddFunction(L, "getreferencedby", getreferencedby);
        Utils::AddFunction(L, "getreferences", getreferencedby);

        Utils::AddFunction(L, "disassemble", Disassembler::disassemble);
    }
}
