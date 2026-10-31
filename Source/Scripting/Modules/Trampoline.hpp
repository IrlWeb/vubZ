#pragma once

#include <lstate.h>
#include <lualib.h>
#include <lapi.h>
#include <ldo.h>
#include <ldebug.h>
#include <lfunc.h>
#include <lgc.h>

#include <climits>
#include <cstdlib>
#include <memory>
#include <vector>

#include <Core/Utilities.hpp>
#include <Scripting/Modules/Miscellaneous.hpp>

namespace Trampoline
{
    struct Frame
    {
        int funcIndex;
        int line;
    };

    struct LinePatch
    {
        int* slot;
        int value;
        int saved;
    };

    inline const Instruction* pc_for_line(Proto* p, int line)
    {
        if (!p || p->sizecode <= 0)
            return p ? p->code : nullptr;

        int best = 0;
        int bestDistance = INT_MAX;
        for (int pc = 0; pc < p->sizecode; ++pc)
        {
            const int at = luaG_getline(p, pc);
            if (at < 0)
                break;
            const int distance = std::abs(at - line);
            if (distance < bestDistance)
            {
                best = pc;
                bestDistance = distance;
                if (distance == 0)
                    break;
            }
        }
        return p->code + best + 1;
    }

    inline bool read_tls_identity(uint64_t& identity, uint64_t& caps)
    {
        __try
        {
            const uintptr_t descriptor = *reinterpret_cast<uintptr_t*>(Offsets::RobloxThread::IdentityPtr);
            if (!descriptor)
                return false;
            const uintptr_t p = Roblox::GetIdentityStruct(descriptor);
            if (!p)
                return false;
            identity = *reinterpret_cast<uint64_t*>(p);
            caps = *reinterpret_cast<uint64_t*>(p + Offsets::Capabilities::Capabilities);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    inline int trampoline_call(lua_State* L)
    {
        if (!Miscellaneous::CalledFromExecutorCode(L))
            luaL_error(L, "The current thread cannot set thread identity");

        luaL_checktype(L, 1, LUA_TFUNCTION);
        luaL_checktype(L, 2, LUA_TTABLE);
        luaL_checktype(L, 3, LUA_TTABLE);
        const int argc = lua_gettop(L) - 3;

        std::vector<Frame> frames;
        const int frameCount = lua_objlen(L, 2);
        for (int i = 1; i <= frameCount; ++i)
        {
            lua_rawgeti(L, 2, i);
            if (!lua_istable(L, -1))
                luaL_error(L, "trampoline_call: callstack[%d] must be a table", i);
            lua_getfield(L, -1, "func");
            if (!lua_isfunction(L, -1))
                luaL_error(L, "trampoline_call: callstack[%d].func must be a function", i);
            lua_getfield(L, -2, "currentline");
            int line = -1;
            if (lua_isnumber(L, -1))
                line = static_cast<int>(lua_tointeger(L, -1));
            else if (lua_iscfunction(L, -2))
                luaL_error(L, "trampoline_call: callstack[%d].currentline is required for a C function", i);
            lua_pop(L, 1);
            lua_remove(L, -2);
            frames.push_back({ lua_gettop(L), line });
        }

        lua_getfield(L, 3, "thread");
        lua_State* parent = lua_isthread(L, -1) ? lua_tothread(L, -1) : nullptr;
        const int parentIndex = lua_gettop(L);
        lua_getfield(L, 3, "script");
        const int scriptIndex = lua_gettop(L);
        lua_getfield(L, 3, "identity");
        const int identityIndex = lua_gettop(L);
        lua_getfield(L, 3, "env");
        const int envIndex = lua_gettop(L);

        if (!lua_isnil(L, scriptIndex) &&
            (!lua_isuserdata(L, scriptIndex) || strcmp(luaL_typename(L, scriptIndex), "Instance") != 0))
            luaL_error(L, "trampoline_call: thread_options.script must be a LuaSourceContainer");
        if (!lua_isnil(L, scriptIndex))
        {
            lua_getfield(L, scriptIndex, "IsA");
            lua_pushvalue(L, scriptIndex);
            lua_pushstring(L, "LuaSourceContainer");
            lua_call(L, 2, 1);
            const bool isScript = lua_toboolean(L, -1) != 0;
            lua_pop(L, 1);
            if (!isScript)
                luaL_error(L, "trampoline_call: thread_options.script must be a LuaSourceContainer");
        }
        if (!lua_isnil(L, identityIndex) && !lua_isnumber(L, identityIndex))
            luaL_error(L, "trampoline_call: thread_options.identity must be a number");
        if (!lua_isnil(L, envIndex) && !lua_istable(L, envIndex))
            luaL_error(L, "trampoline_call: thread_options.env must be a table");

        // The env option becomes a sandbox: a copy of the given table that falls
        // back to our globals, with _G pointing at itself. Lua closures resolve
        // globals through cl->env, not the thread's globals, so the sandbox is
        // also swapped onto the target closure for the duration of the call.
        int sandboxIndex = 0;
        if (lua_istable(L, envIndex))
        {
            lua_rawcheckstack(L, 6);
            lua_createtable(L, 0, 0);
            sandboxIndex = lua_gettop(L);

            lua_pushnil(L);
            while (lua_next(L, envIndex) != 0)
            {
                lua_pushvalue(L, -2);
                lua_pushvalue(L, -2);
                lua_rawset(L, sandboxIndex);
                lua_pop(L, 1);
            }

            lua_createtable(L, 0, 1);
            lua_pushvalue(L, LUA_GLOBALSINDEX);
            lua_setfield(L, -2, "__index");
            lua_setmetatable(L, sandboxIndex);

            if (lua_rawgetfield(L, sandboxIndex, "_G") == LUA_TNIL)
            {
                lua_pushvalue(L, sandboxIndex);
                lua_rawsetfield(L, sandboxIndex, "_G");
            }
            lua_pop(L, 1);
            lua_setsafeenv(L, sandboxIndex, false);
        }

        lua_State* T = parent;
        if (T)
        {
            if (T == L || lua_status(T) != LUA_OK || T->ci != T->base_ci)
                luaL_error(L, "trampoline_call: thread_options.thread must be a coroutine that has not started");
        }
        else
        {
            T = lua_newthread(L);
            if (T->userdata && L->userdata)
            {
                if (lua_isnumber(L, identityIndex))
                {
                    int identity = static_cast<int>(lua_tointeger(L, identityIndex));
                    uint64_t caps = identity == 8
                        ? MaxCapabilities
                        : (0xFFFFFFFFFFFFFF00ull | Roblox::GetCapabilities(&identity));
                    T->userdata->identity = identity;
                    T->userdata->capabilities = caps;
                }
                if (!lua_isnil(L, scriptIndex))
                {
                    auto* instance = static_cast<std::shared_ptr<uintptr_t>*>(lua_touserdata(L, scriptIndex));
                    T->userdata->source = *instance;
                }
            }
            if (sandboxIndex)
            {
                lua_pushvalue(L, sandboxIndex);
                lua_xmove(L, T, 1);
                lua_replace(T, LUA_GLOBALSINDEX);
            }
        }

        uint64_t savedIdentity = 0;
        uint64_t savedCaps = 0;
        const bool restoreIdentity = T->userdata && read_tls_identity(savedIdentity, savedCaps);
        if (restoreIdentity)
        {
            uintptr_t unused = 0;
            Miscellaneous::TryWriteTlsIdentity(static_cast<int>(T->userdata->identity), T->userdata->capabilities, unused);
        }

        const ptrdiff_t savedCi = saveci(T, T->ci);
        const ptrdiff_t savedBase = savestack(T, T->base);
        const ptrdiff_t savedTop = savestack(T, T->top);

        std::vector<LinePatch> linePatches;
        for (auto it = frames.rbegin(); it != frames.rend(); ++it)
        {
            Closure* cl = clvalue(luaA_toobject(L, it->funcIndex));
            const int size = cl->isC ? LUA_MINSTACK : cl->l.p->maxstacksize;
            luaD_checkstack(T, size + 2);

            lua_pushvalue(L, it->funcIndex);
            lua_xmove(L, T, 1);
            StkId func = T->top - 1;

            CallInfo* ci = incr_ci(T);
            ci->func = func;
            ci->base = func + 1;
            ci->top = ci->base + size;
            ci->nresults = 0;
            ci->flags = 0;
            ci->p = cl->isC ? nullptr : cl->l.p;
            ci->savedpc = cl->isC
                ? nullptr
                : pc_for_line(cl->l.p, it->line >= 0 ? it->line : cl->l.p->linedefined);

            // A requested line that the proto has no instruction for can't be
            // reached through savedpc alone; rebase that pc's abslineinfo entry
            // so the frame reports it. Applied only around the call itself.
            Proto* p = ci->p;
            if (p && it->line >= 0 && p->sizecode > 0 && p->lineinfo)
            {
                const int rel = static_cast<int>(ci->savedpc - p->code) - 1;
                if (luaG_getline(p, rel) != it->line)
                {
                    int* abs = p->abslineinfo;
                    const unsigned char* li = p->lineinfo;
                    linePatches.push_back({ &abs[rel >> p->linegaplog2], it->line - li[rel], 0 });
                }
            }

            for (StkId slot = ci->base; slot < ci->top; ++slot)
                setnilvalue(slot);
            T->base = ci->base;
            T->top = ci->top;
        }

        luaD_checkstack(T, argc + 2);
        const int resultBase = lua_gettop(T);
        lua_pushvalue(L, 1);
        lua_xmove(L, T, 1);
        for (int i = 4; i <= argc + 3; ++i)
        {
            lua_pushvalue(L, i);
            lua_xmove(L, T, 1);
        }

        int savedEnvIndex = 0;
        if (sandboxIndex && !clvalue(luaA_toobject(L, 1))->isC)
        {
            lua_rawcheckstack(L, 2);
            lua_getfenv(L, 1);
            savedEnvIndex = lua_gettop(L);
            lua_pushvalue(L, sandboxIndex);
            lua_setfenv(L, 1);
        }
        for (LinePatch& patch : linePatches)
        {
            patch.saved = *patch.slot;
            *patch.slot = patch.value;
        }

        const int status = lua_pcall(T, argc, LUA_MULTRET, 0);

        for (auto it = linePatches.rbegin(); it != linePatches.rend(); ++it)
            *it->slot = it->saved;
        if (savedEnvIndex)
        {
            lua_pushvalue(L, savedEnvIndex);
            lua_setfenv(L, 1);
        }

        const int results = lua_gettop(T) - resultBase;

        lua_rawcheckstack(L, results + 1);
        lua_pushboolean(L, status == LUA_OK);
        lua_xmove(T, L, results);

        T->ci = restoreci(T, savedCi);
        T->base = restorestack(T, savedBase);
        T->top = restorestack(T, savedTop);

        if (restoreIdentity)
        {
            uintptr_t unused = 0;
            Miscellaneous::TryWriteTlsIdentity(static_cast<int>(savedIdentity), savedCaps, unused);
        }

        (void)parentIndex;
        return results + 1;
    }

    inline void RegisterLibrary(lua_State* L)
    {
        Utils::AddFunction(L, "trampoline_call", trampoline_call);
    }
}
