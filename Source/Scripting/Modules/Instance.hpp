#pragma once

#include <unordered_set>

#define lua_torawuserdata(L, idx) lua_touserdata(L, idx)

#include <Windows.h>
#include <lstate.h>
#include <lgc.h>
#include <lualib.h>
#include <string>

#include <Core/Utilities.hpp>
#include <Core/Context.hpp>
#include <bitset>

inline void luaL_trimstack(lua_State* L, int n) {
    lua_settop(L, n);
}

namespace Instance
{
    static bool can_read(uintptr_t address, size_t size = 1) noexcept
    {
        if (address < 0x10000)
            return false;

        MEMORY_BASIC_INFORMATION mbi = {};
        if (VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)) == 0)
            return false;

        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            return false;

        uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        return address + size >= address && address + size <= regionEnd;
    }

    static bool IsRobloxInstance(lua_State* L, int idx)
    {
        if (!lua_isuserdata(L, idx)) return false;
        lua_getglobal(L, "typeof");
        lua_pushvalue(L, idx);
        if (lua_pcall(L, 1, 1, 0) != LUA_OK) { lua_pop(L, 1); return false; }
        bool ok = lua_isstring(L, -1) && strcmp(lua_tostring(L, -1), "Instance") == 0;
        lua_pop(L, 1);
        return ok;
    }

    inline int getinstances(lua_State* L)
    {
        luaL_trimstack(L, 0);

        lua_newtable(L);
        int resultIdx = lua_gettop(L);
        int index = 0;

        lua_pushvalue(L, LUA_REGISTRYINDEX);
        lua_pushlightuserdata(L, reinterpret_cast<void*>(Roblox::InstanceBridge::UintPtr));
        lua_rawget(L, -2);

        if (lua_istable(L, -1))
        {
            lua_pushnil(L);
            while (lua_next(L, -2) != 0)
            {
                if (!lua_isnil(L, -1))
                {
                    lua_getglobal(L, "typeof");
                    lua_pushvalue(L, -2);
                    lua_pcall(L, 1, 1, 0);
                    bool isInst = lua_isstring(L, -1) &&
                        strcmp(lua_tostring(L, -1), "Instance") == 0;
                    lua_pop(L, 1);

                    if (isInst)
                    {
                        lua_pushinteger(L, ++index);
                        lua_pushvalue(L, -2);
                        lua_settable(L, resultIdx);
                    }
                }
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 2);

        lua_getglobal(L, "game");
        if (!lua_isnil(L, -1))
        {
            bool found = false;
            for (int i = 1; i <= index; ++i)
            {
                lua_rawgeti(L, resultIdx, i);
                if (lua_rawequal(L, -1, -2)) { found = true; lua_pop(L, 1); break; }
                lua_pop(L, 1);
            }
            if (!found)
            {
                lua_pushinteger(L, ++index);
                lua_pushvalue(L, -2);
                lua_settable(L, resultIdx);
            }
        }
        lua_pop(L, 1);

        lua_pushvalue(L, resultIdx);
        return 1;
    }

    inline int getnilinstances(lua_State* L)
    {
        luaL_trimstack(L, 0);

        lua_pushvalue(L, LUA_REGISTRYINDEX);
        lua_pushlightuserdata(L, reinterpret_cast<void*>(Roblox::InstanceBridge::UintPtr));
        lua_rawget(L, -2);

        if (!lua_istable(L, -1)) { lua_pop(L, 1); lua_pushnil(L); return 1; };

        lua_newtable(L);

        int index = 0;

        lua_pushnil(L);
        while (lua_next(L, -3) != 0) {

            if (!lua_isnil(L, -1)) {
                lua_getglobal(L, "typeof");
                lua_pushvalue(L, -2);
                lua_pcall(L, 1, 1, 0);

                const char* rawType = lua_tostring(L, -1);
                std::string type = rawType ? rawType : "";
                lua_pop(L, 1);

                if (type == "Instance") {
                    lua_getfield(L, -1, "Parent");
                    int parentType = lua_type(L, -1);
                    lua_pop(L, 1);

                    if (parentType == LUA_TNIL) {
                        lua_pushinteger(L, ++index);

                        lua_pushvalue(L, -2);
                        lua_settable(L, -5);
                    }
                }
            }

            lua_pop(L, 1);
        }

        lua_remove(L, -2);

        return 1;
    }

    inline int isnetworkowner(lua_State* L)
    {
        if (!L || !lua_isuserdata(L, 1)) {
            lua_pushboolean(L, 0);
            return 1;
        }

        lua_getfield(L, 1, "Name");
        if (lua_isstring(L, -1)) {
            const char* name = lua_tostring(L, -1);
            if (name && strcmp(name, "HumanoidRootPart") == 0) {
                lua_pop(L, 1);
                lua_pushboolean(L, 1);
                return 1;
            }
        }
        lua_pop(L, 1);

        lua_getfield(L, 1, "ClassName");
        if (!lua_isstring(L, -1)) {
            lua_pop(L, 1);
            lua_pushboolean(L, 0);
            return 1;
        }

        const char* cn = lua_tostring(L, -1);
        if (!cn) {
            lua_pop(L, 1);
            lua_pushboolean(L, 0);
            return 1;
        }
        lua_pop(L, 1);

        if (strcmp(cn, "BasePart") != 0 && strcmp(cn, "Part") != 0 &&
            strcmp(cn, "MeshPart") != 0 && strcmp(cn, "UnionOperation") != 0) {
            lua_pushboolean(L, 0);
            return 1;
        }

        lua_getfield(L, 1, "GetNetworkOwner");
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 1);
            lua_pushboolean(L, 0);
            return 1;
        }

        lua_pushvalue(L, 1);
        if (lua_pcall(L, 1, 1, 0) != 0) {
            lua_pop(L, 1);
            lua_pushboolean(L, 0);
            return 1;
        }

        lua_getglobal(L, "game");
        lua_getfield(L, -1, "GetService");
        lua_pushvalue(L, -2);
        lua_pushstring(L, "Players");

        if (lua_pcall(L, 2, 1, 0) != 0) {
            lua_pop(L, 3);
            lua_pushboolean(L, 0);
            return 1;
        }

        lua_getfield(L, -1, "LocalPlayer");
        bool is_owner = lua_rawequal(L, -1, -4);

        lua_pop(L, 4);
        lua_pushboolean(L, is_owner ? 1 : 0);
        return 1;
    }

    inline int getspecialinfo(lua_State* L)
    {
        if (!lua_isuserdata(L, 1))
        {
            lua_pushnil(L);
            return 1;
        }

        lua_newtable(L);
        int T = lua_gettop(L);

        lua_getfield(L, 1, "Name");
        lua_setfield(L, T, "Name");

        lua_getfield(L, 1, "ClassName");
        lua_setfield(L, T, "ClassName");

        lua_getfield(L, 1, "GetFullName");
        if (lua_isfunction(L, -1))
        {
            lua_pushvalue(L, 1);
            if (lua_pcall(L, 1, 1, 0) == LUA_OK)
                lua_setfield(L, T, "FullName");
            else
                lua_pop(L, 1);
        }
        else { lua_pop(L, 1); }

        void* ud = lua_touserdata(L, 1);
        uintptr_t instance = 0;
        if (ud)
        {
            instance = *reinterpret_cast<uintptr_t*>(ud);
            char buf[32];
            snprintf(buf, sizeof(buf), "0x%llX", (unsigned long long)instance);
            lua_pushstring(L, buf);
            lua_setfield(L, T, "Handle");
        }

        lua_getfield(L, 1, "Parent");
        lua_pushboolean(L, lua_isnil(L, -1) ? 1 : 0);
        lua_setfield(L, T, "IsNil");
        lua_pop(L, 1);

        lua_getfield(L, 1, "Parent");
        if (lua_isuserdata(L, -1))
        {
            lua_getfield(L, -1, "ClassName");
            bool isService = lua_isstring(L, -1) &&
                strcmp(lua_tostring(L, -1), "DataModel") == 0;
            lua_pop(L, 1);
            lua_pushboolean(L, isService ? 1 : 0);
            lua_setfield(L, T, "IsService");
        }
        else
        {
            lua_pushboolean(L, 0);
            lua_setfield(L, T, "IsService");
        }
        lua_pop(L, 1);

        if (instance && !IsBadReadPtr(reinterpret_cast<void*>(instance + 0x30), 16))
        {
            uint64_t lo = *reinterpret_cast<uint64_t*>(instance + 0x30);
            uint64_t hi = *reinterpret_cast<uint64_t*>(instance + 0x38);
            char uid[48];
            snprintf(uid, sizeof(uid), "%016llX%016llX",
                (unsigned long long)hi, (unsigned long long)lo);
            lua_pushstring(L, uid);
            lua_setfield(L, T, "UniqueId");
        }

        lua_getfield(L, 1, "Archivable");
        lua_setfield(L, T, "Archivable");

        lua_getfield(L, 1, "GetChildren");
        if (lua_isfunction(L, -1))
        {
            lua_pushvalue(L, 1);
            if (lua_pcall(L, 1, 1, 0) == LUA_OK && lua_istable(L, -1))
            {
                lua_pushinteger(L, (int)lua_objlen(L, -1));
                lua_setfield(L, T, "ChildCount");
                lua_pop(L, 1);
            }
            else lua_pop(L, 1);
        }
        else lua_pop(L, 1);

        lua_getfield(L, 1, "GetTags");
        if (lua_isfunction(L, -1))
        {
            lua_pushvalue(L, 1);
            if (lua_pcall(L, 1, 1, 0) == LUA_OK)
                lua_setfield(L, T, "Tags");
            else
                lua_pop(L, 1);
        }
        else lua_pop(L, 1);

        lua_pushvalue(L, T);
        return 1;
    }

    inline int getthreads(lua_State* L)
    {
        lua_newtable(L);
        int T = lua_gettop(L);
        int index = 0;

        global_State* g = L->global;
        if (!g) return 1;

        lua_State* main_thread = g->mainthread;
        if (!main_thread) return 1;

        lua_pushvalue(L, LUA_REGISTRYINDEX);
        int reg = lua_gettop(L);

        lua_pushnil(L);
        while (lua_next(L, reg) != 0)
        {
            if (lua_type(L, -1) == LUA_TTHREAD)
            {
                lua_State* thread = lua_tothread(L, -1);
                if (thread && thread != L)
                {
                    lua_pushvalue(L, -1);
                    lua_rawseti(L, T, ++index);
                }
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);

        if (main_thread != L)
        {
            lua_pushthread(main_thread);
            lua_xmove(main_thread, L, 1);
            lua_rawseti(L, T, ++index);
        }

        lua_pushvalue(L, T);
        return 1;
    }

    inline int getproperties(lua_State* L)
    {
        luaL_checktype(L, 1, LUA_TUSERDATA);

        if (strcmp(luaL_typename(L, 1), "Instance") != 0) {
            lua_pushnil(L);
            return 1;
        }

        lua_newtable(L);
        int resultIdx = lua_gettop(L);

        lua_getfield(L, 1, "GetAttributes");
        if (lua_isfunction(L, -1)) {
            lua_pushvalue(L, 1);
            if (lua_pcall(L, 1, 1, 0) == LUA_OK && lua_istable(L, -1)) {
                lua_pushnil(L);
                while (lua_next(L, -2) != 0) {
                    lua_pushvalue(L, -2);
                    lua_pushvalue(L, -2);
                    lua_settable(L, resultIdx);
                    lua_pop(L, 1);
                }
            }
            lua_pop(L, 1);
        }
        else {
            lua_pop(L, 1);
        }

        void* ud = lua_touserdata(L, 1);
        if (!ud) {
            lua_pushvalue(L, resultIdx);
            return 1;
        }

        uintptr_t instance = *reinterpret_cast<uintptr_t*>(ud);
        if (!instance) {
            lua_pushvalue(L, resultIdx);
            return 1;
        }

        const uintptr_t classDescriptor = *reinterpret_cast<uintptr_t*>(instance + 0x18);
        if (!classDescriptor) {
            lua_pushvalue(L, resultIdx);
            return 1;
        }

        const uintptr_t propertiesStart = *reinterpret_cast<uintptr_t*>(classDescriptor + 0x28);
        if (!propertiesStart) {
            lua_pushvalue(L, resultIdx);
            return 1;
        }

        for (uint32_t i = 0; i < 256; ++i) {
            const uintptr_t entry = propertiesStart + (i * 0x10);
            if (IsBadReadPtr(reinterpret_cast<void*>(entry), sizeof(uintptr_t))) break;

            const uintptr_t prop = *reinterpret_cast<uintptr_t*>(entry);
            if (!prop) continue;

            const uintptr_t namePtr = *reinterpret_cast<uintptr_t*>(prop + 0x8);
            if (!namePtr || IsBadReadPtr(reinterpret_cast<void*>(namePtr), 1)) continue;

            const char* name = reinterpret_cast<const char*>(namePtr);
            if (!name || name[0] == '\0') continue;

            lua_pushstring(L, name);
            lua_gettable(L, 1);

            if (!lua_isnil(L, -1)) {
                lua_setfield(L, resultIdx, name);
            }
            else {
                lua_pop(L, 1);
            }
        }

        lua_pushvalue(L, resultIdx);
        return 1;
    }

    inline int getsimulationradius(lua_State* L)
    {
        if (!L) {
            lua_pushnumber(L, 0);
            lua_pushnumber(L, 0);
            return 2;
        }

        lua_getglobal(L, "game");
        if (!lua_isuserdata(L, -1)) {
            lua_pop(L, 1);
            lua_pushnumber(L, 0);
            lua_pushnumber(L, 0);
            return 2;
        }

        lua_getfield(L, -1, "GetService");
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 2);
            lua_pushnumber(L, 0);
            lua_pushnumber(L, 0);
            return 2;
        }

        lua_pushvalue(L, -2);
        lua_pushstring(L, "Players");

        if (lua_pcall(L, 2, 1, 0) != 0) {
            lua_pop(L, 2);
            lua_pushnumber(L, 0);
            lua_pushnumber(L, 0);
            return 2;
        }

        lua_getfield(L, -1, "LocalPlayer");
        if (lua_isuserdata(L, -1)) {
            lua_pushnumber(L, 1000.0);
            lua_pushnumber(L, 1000.0);
            lua_remove(L, -3);
            lua_remove(L, -3);
            return 2;
        }

        lua_pop(L, 2);
        lua_pushnumber(L, 0);
        lua_pushnumber(L, 0);
        return 2;
    }

    inline int setsimulationradius(lua_State* L)
    {
        double radius = luaL_checknumber(L, 1);
        double maxRadius = luaL_optnumber(L, 2, radius);

        lua_getglobal(L, "game");
        lua_getfield(L, -1, "GetService");
        lua_pushvalue(L, -2);
        lua_pushstring(L, "Players");

        if (lua_pcall(L, 2, 1, 0) == 0) {
            lua_getfield(L, -1, "LocalPlayer");
            if (lua_isuserdata(L, -1)) {
                lua_pushnumber(L, radius);
                lua_setfield(L, -2, "SimulationRadius");

                lua_pushnumber(L, maxRadius);
                lua_setfield(L, -2, "MaximumSimulationRadius");
            }
        }

        return 0;
    }

    int getcallbackvalue(lua_State* L)
    {
        luaL_checktype(L, 1, LUA_TUSERDATA);
        luaL_checktype(L, 2, LUA_TSTRING);

        const char* prop = lua_tostring(L, 2);

        lua_pushstring(L, "__callbacks");
        lua_rawget(L, LUA_REGISTRYINDEX);
        if (lua_isnil(L, -1))
        {
            lua_pushnil(L);
            return 1;
        }

        lua_pushvalue(L, 1);
        lua_rawget(L, -2);
        if (lua_isnil(L, -1))
        {
            lua_pushnil(L);
            return 1;
        }

        lua_pushstring(L, prop);
        lua_rawget(L, -2);

        return 1;
    }

    inline uintptr_t get_instance_prop_by_name(lua_State* L, uintptr_t instance, const char* propName)
    {
        if (!instance || !can_read(instance + 0x18, sizeof(uintptr_t)))
            return 0;

        uintptr_t classDescriptor = *reinterpret_cast<uintptr_t*>(instance + 0x18);
        if (!classDescriptor)
            return 0;

        uintptr_t super = classDescriptor;
        while (super) {
            if (!can_read(super + 0x28, sizeof(uintptr_t)))
                break;

            uintptr_t propertiesStart = *reinterpret_cast<uintptr_t*>(super + 0x28);
            if (propertiesStart) {
                for (uint32_t i = 0; i < 512; ++i) {
                    uintptr_t entry = propertiesStart + (i * 0x10);
                    if (!can_read(entry, sizeof(uintptr_t)))
                        break;

                    uintptr_t prop = *reinterpret_cast<uintptr_t*>(entry);
                    if (!prop)
                        continue;

                    uintptr_t namePtr = *reinterpret_cast<uintptr_t*>(prop + 0x8);
                    if (!namePtr || !can_read(namePtr, 1))
                        continue;

                    const char* name = reinterpret_cast<const char*>(namePtr);
                    if (name && _stricmp(name, propName) == 0)
                        return prop;
                }
            }

            if (!can_read(super + 0x8, sizeof(uintptr_t)))
                break;
            uintptr_t parent = *reinterpret_cast<uintptr_t*>(super + 0x8);
            if (!parent || parent == super)
                break;
            super = parent;
        }

        return 0;
    }

    inline int gethiddenproperty(lua_State* L)
    {
        luaL_checktype(L, 1, LUA_TUSERDATA);
        luaL_checktype(L, 2, LUA_TSTRING);

        if (!Instance::IsRobloxInstance(L, 1)) {
            luaL_error(L, "gethiddenproperty requires an Instance");
            return 0;
        }

        const uintptr_t instance = *static_cast<uintptr_t*>(lua_touserdata(L, 1));
        const char* propName = lua_tostring(L, 2);

        uintptr_t property = get_instance_prop_by_name(L, instance, propName);
        if (!property) {
            lua_pushnil(L);
            lua_pushboolean(L, false);
            return 2;
        }

        bool wasHidden = true;
        if (can_read(property + 0x8C, sizeof(uint32_t))) {
            uint32_t oldFlags = *reinterpret_cast<uint32_t*>(property + 0x8C);
            wasHidden = (oldFlags & 0x10) == 0;
            *reinterpret_cast<uint32_t*>(property + 0x8C) = oldFlags | 0x10;
        }

        lua_getfield(L, 1, propName);

        if (can_read(property + 0x8C, sizeof(uint32_t))) {
            uint32_t curFlags = *reinterpret_cast<uint32_t*>(property + 0x8C);
            *reinterpret_cast<uint32_t*>(property + 0x8C) = curFlags & ~0x10;
        }

        lua_pushboolean(L, wasHidden);
        return 2;
    }

    inline int getmodules(lua_State* L)
    {
        lua_newtable(L);
        const int result = lua_gettop(L);
        int count = 0;
        std::unordered_set<uintptr_t> seen;

        lua_pushlightuserdata(L, reinterpret_cast<void*>(Roblox::InstanceBridge::UintPtr));
        lua_rawget(L, LUA_REGISTRYINDEX);
        if (lua_istable(L, -1))
        {
            const int cache = lua_gettop(L);
            lua_pushnil(L);
            while (lua_next(L, cache) != 0)
            {
                if (lua_isuserdata(L, -1) && strcmp(luaL_typename(L, -1), "Instance") == 0)
                {
                    lua_getfield(L, -1, "ClassName");
                    const bool module = lua_isstring(L, -1) && strcmp(lua_tostring(L, -1), "ModuleScript") == 0;
                    lua_pop(L, 1);

                    const uintptr_t instance = *static_cast<uintptr_t*>(lua_touserdata(L, -1));
                    if (module && instance && seen.insert(instance).second)
                    {
                        lua_pushvalue(L, -1);
                        lua_rawseti(L, result, ++count);
                    }
                }
                lua_pop(L, 1);
            }
        }

        lua_settop(L, result);
        return 1;
    }

    inline void RegisterLibrary(lua_State* L)
    {
        if (!L) return;

        Utils::AddFunction(L, "getmodules", getmodules);

        Utils::AddFunction(L, "getnilinstances", getnilinstances);
        Utils::AddFunction(L, "getinstances", getinstances);
        Utils::AddFunction(L, "isnetworkowner", isnetworkowner);
        Utils::AddFunction(L, "getspecialinfo", getspecialinfo);
        Utils::AddFunction(L, "getthreads", getthreads);
        Utils::AddFunction(L, "getsimulationradius", getsimulationradius);
        Utils::AddFunction(L, "setsimulationradius", setsimulationradius);
    }
}
