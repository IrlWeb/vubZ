#pragma once

#include <cstring>
#include <string>

#include <lapi.h>
#include <lstring.h>
#include <lstate.h>
#include <lualib.h>
#include <lgc.h>
#include <lmem.h>
#include <lobject.h>
#include <lfunc.h>
#include <ltable.h>
#include <ldebug.h>
#include <ldo.h>

#include <Runtime/Execution/ScriptExecution.hpp>
#include <Core/Utilities.hpp>
#include <Scripting/Modules/Closures.hpp>
#include <Scripting/Modules/ProtoProxy.hpp>
#include <Scripting/Modules/Crypt.hpp>
#include <Scripting/Modules/Oth.hpp>

class debug_library
{
public:
    static void initialize(lua_State* L);
};
namespace Debug
{
    Closure* header_get_function(lua_State* L, bool allowCclosure = false, bool popcl = true)
    {
        luaL_checkany(L, 1);

        if (!(lua_isfunction(L, 1) || lua_isnumber(L, 1)))
        {
            luaL_argerror(L, 1, "function or number");
        }

        int level = 0;
        if (lua_isnumber(L, 1))
        {
            level = lua_tointeger(L, 1);

            if (level <= 0)
            {
                luaL_argerror(L, 1, "level out of range");
            }
        }
        else if (lua_isfunction(L, 1))
        {
            level = -lua_gettop(L);
        }

        lua_Debug ar;
        if (!lua_getinfo(L, level, "f", &ar))
        {
            luaL_argerror(L, 1, "invalid level");
        }

        if (!lua_isfunction(L, -1))
            luaL_argerror(L, 1, "level does not point to a function");
        if (lua_iscfunction(L, -1) && !allowCclosure)
            luaL_argerror(L, 1, "level points to c function");

        if (!allowCclosure && lua_iscfunction(L, -1))
        {
            luaL_argerror(L, 1, "luau function expected");
        }

        auto function = clvalue(luaA_toobject(L, -1));
        if (popcl)
            lua_pop(L, 1);

        return function;
    }

    inline const char* aux_upvalue_2(Closure* f, int n, TValue** val)
    {
        if (f->isC)
        {
            if (!(1 <= n && n <= f->nupvalues))
                return NULL;
            *val = &f->c.upvals[n - 1];
            return "";
        }
        else
        {
            Proto* p = f->l.p;
            if (!(1 <= n && n <= p->nups))
                return NULL;
            TValue* r = &f->l.uprefs[n - 1];
            *val = ttisupval(r) ? upvalue(r)->v : r;
            if (!(1 <= n && n <= p->sizeupvalues))
                return "";
            return getstr(p->upvalues[n - 1]);
        }
    }

    inline Proto* header_get_proto(lua_State* L)
    {
        if (Proto* proto = ProtoProxy::To(L, 1))
            return proto;

        return header_get_function(L)->l.p;
    }

    inline void push_closure_value(lua_State* L, Closure* cl);

    inline void push_inert_closure(lua_State* L, Proto* proto)
    {
        Closure* cl = luaF_newLclosure(L, proto->nups, L->gt, proto);
        push_closure_value(L, cl);
    }

    int debug_getupvalues(lua_State* L)
    {
        luaL_checkany(L, 1);

        int FunctionIndex = 0;
        bool TemporaryFunction = false;

        if (lua_isfunction(L, 1))
        {
            FunctionIndex = 1;
        }
        else if (lua_isnumber(L, 1))
        {
            const int level = static_cast<int>(lua_tointeger(L, 1));

            if (level <= 0)
                luaL_argerror(L, 1, "level must be greater than 0");

            lua_Debug ar{};

            if (!lua_getinfo(L, level, "f", &ar))
                luaL_argerror(L, 1, "level out of range");

            if (!lua_isfunction(L, -1))
            {
                lua_pop(L, 1);
                luaL_argerror(L, 1, "stack level does not contain a function");
            }

            FunctionIndex = lua_gettop(L);
            TemporaryFunction = true;
        }
        else
        {
            luaL_argerror(L, 1, "function or stack level expected");
        }

        if (lua_iscfunction(L, FunctionIndex))
        {
            if (TemporaryFunction)
                lua_pop(L, 1);

            luaL_argerror(L, 1, "Luau function expected");
        }

        lua_newtable(L);
        const int resultTable = lua_gettop(L);

        for (int index = 1;; ++index)
        {
            const char* name = lua_getupvalue(L, FunctionIndex, index);

            if (!name)
                break;

            lua_rawseti(L, resultTable, index);
        }

        if (TemporaryFunction)
            lua_remove(L, FunctionIndex);

        return 1;
    }

    int debug_getupvalue(lua_State* L)
    {
        luaL_checkany(L, 1);
        const int upvalueIndex = static_cast<int>(luaL_checkinteger(L, 2));

        if (upvalueIndex < 1)
            luaL_argerror(L, 2, "upvalue index must be greater than 0");

        int FunctionIndex = 0;
        bool TemporaryFunction = false;

        if (lua_isfunction(L, 1))
        {
            FunctionIndex = 1;
        }
        else if (lua_isnumber(L, 1))
        {
            const int level = static_cast<int>(lua_tointeger(L, 1));

            if (level <= 0)
                luaL_argerror(L, 1, "level must be greater than 0");

            lua_Debug ar{};

            if (!lua_getinfo(L, level, "f", &ar))
                luaL_argerror(L, 1, "level out of range");

            if (!lua_isfunction(L, -1))
            {
                lua_pop(L, 1);
                luaL_argerror(L, 1, "stack level does not contain a function");
            }

            FunctionIndex = lua_gettop(L);
            TemporaryFunction = true;
        }
        else
        {
            luaL_argerror(L, 1, "function or stack level expected");
        }

        if (lua_iscfunction(L, FunctionIndex))
        {
            if (TemporaryFunction)
                lua_pop(L, 1);

            luaL_argerror(L, 1, "Luau function expected");
        }

        const char* name = lua_getupvalue(L, FunctionIndex, upvalueIndex);

        if (!name)
        {
            if (TemporaryFunction)
                lua_pop(L, 1);

            luaL_argerror(L, 2, "upvalue index out of range");
        }

        if (TemporaryFunction)
            lua_remove(L, FunctionIndex);

        return 1;
    }

    inline std::vector<int> collect_import_slots(const Proto* proto)
    {
        if (!proto || proto->sizek <= 0)
            return {};

        std::vector<int> slots(static_cast<size_t>(proto->sizek), -1);
        const uint8_t* opcodes = Crypt::detail_hash::opcode_decode_table();

        for (int pc = 0; pc < proto->sizecode; ++pc)
        {
            const Instruction word = proto->code[pc];
            const uint8_t op = opcodes[LUAU_INSN_OP(word)];
            const int skip = Crypt::detail_hash::aux_words(op);

            if (op == LOP_GETIMPORT)
            {
                const int slot = LUAU_INSN_D(word);
                if (slot >= 0 && slot < proto->sizek && (pc + 1) < proto->sizecode)
                    slots[slot] = static_cast<int>(proto->code[pc + 1]);
            }

            pc += skip;
        }

        return slots;
    }

    int debug_setupvalue(lua_State* L)
    {
        const auto Func = header_get_function(L, false, false);
        const auto idx = lua_tointeger(L, 2);

        if (Func->nupvalues <= 0)
        {
            luaL_argerror(L, 1, "function has no upvalues");
        }

        if (!(idx >= 1 && idx <= Func->nupvalues))
        {
            luaL_argerror(L, 2, "index out of range");
        }

        lua_pushvalue(L, 3);
        lua_setupvalue(L, -2, idx);

        if (!Func->isC)
        {
            TValue* ref = &Func->l.uprefs[idx - 1];
            if (ttisupval(ref) && upisopen(upvalue(ref)))
            {
                StkId slot = upvalue(ref)->v;
                luaM_visitgco(L, slot, [](void* context, lua_Page* page, GCObject* gco) -> bool {
                    (void)page;
                    if (gco->gch.tt != LUA_TTHREAD)
                        return false;
                    lua_State* th = gco2th(gco);
                    auto target = static_cast<StkId>(context);
                    if (th->stack && th->stack_last && target >= th->stack && target < th->stack_last)
                        luaC_threadbarrier(th);
                    return false;
                });
            }
        }

        return 1;
    }

    int debug_getconstants(lua_State* L)
    {
        Proto* p = header_get_proto(L);
        const std::vector<int> imports = collect_import_slots(p);

        lua_createtable(L, p->sizek, 0);

        for (int i = 0; i < p->sizek; i++)
        {
            TValue k = p->k[i];
            const bool isImport = (i < static_cast<int>(imports.size()) && imports[i] != -1);

            if (isImport || k.tt == LUA_TNIL || k.tt == LUA_TFUNCTION || k.tt == LUA_TTABLE)
            {
                lua_pushnil(L);
            }
            else
            {
                luaC_threadbarrier(L);
                luaA_pushvalue(L, &k);
            }
            lua_rawseti(L, -2, i + 1);
        }

        return 1;
    }

    int debug_getconstant(lua_State* L)
    {
        Proto* p = header_get_proto(L);
        const auto idx = luaL_checkinteger(L, 2);

        if (p->sizek <= 0)
        {
            luaL_argerror(L, 1, "function has no constants");
        }

        if (!(idx >= 1 && idx <= p->sizek))
        {
            luaL_argerror(L, 2, "index out of range");
        }

        const std::vector<int> imports = collect_import_slots(p);
        const auto k = &p->k[idx - 1];
        const bool isImport = ((idx - 1) < static_cast<int>(imports.size()) && imports[idx - 1] != -1);

        if (isImport || k->tt == LUA_TNIL || k->tt == LUA_TTABLE || k->tt == LUA_TFUNCTION)
        {
            lua_pushnil(L);
            return 1;
        }

        luaC_threadbarrier(L);
        luaA_pushvalue(L, k);
        return 1;
    }

    int debug_setconstant(lua_State* L)
    {
        luaL_checkany(L, 3);

        Proto* p = header_get_proto(L);
        const auto idx = luaL_checkinteger(L, 2);

        if (p->sizek <= 0)
        {
            luaL_argerror(L, 1, "function has no constants");
        }

        if (!(idx >= 1 && idx <= p->sizek))
        {
            luaL_argerror(L, 2, "index out of range");
        }

        const std::vector<int> imports = collect_import_slots(p);
        if ((idx - 1) < static_cast<int>(imports.size()) && imports[idx - 1] != -1)
        {
            return 0;
        }

        TValue* k = &p->k[idx - 1];

        if (k->tt == LUA_TFUNCTION || k->tt == LUA_TTABLE)
        {
            return 0;
        }

        const TValue* src = luaA_toobject(L, 3);
        if (k->tt == src->tt)
        {
            if (p->execdata && L->global->ecb.disable)
                L->global->ecb.disable(L, p);

            setobj2n(L, k, src);
            luaC_barrier(L, p, k);

            const int targetIdx = static_cast<int>(idx - 1);
            for (int item = 0; item < p->sizek && item < static_cast<int>(imports.size()); ++item)
            {
                const int id = imports[item];
                if (id == -1)
                    continue;

                const int count = static_cast<int>(static_cast<unsigned int>(id) >> 30);
                const int first = count > 0 ? static_cast<int>((id >> 20) & 1023) : -1;
                const int second = count > 1 ? static_cast<int>((id >> 10) & 1023) : -1;
                const int third = count > 2 ? static_cast<int>(id & 1023) : -1;

                if (first == targetIdx || second == targetIdx || third == targetIdx)
                    setnilvalue(&p->k[item]);
            }
        }

        return 0;
    }

    inline void push_closure_value(lua_State* L, Closure* cl)
    {
        luaD_checkstack(L, 1);
        luaC_threadbarrier(L);
        setclvalue(L, L->top, cl);
        L->top++;
    }

    inline Proto* get_parent_proto(lua_State* L, int& owner)
    {
        if (Proto* proto = ProtoProxy::To(L, 1))
        {
            lua_pushvalue(L, 1);
            owner = lua_gettop(L);
            return proto;
        }

        Closure* closure = header_get_function(L, false, false);
        owner = lua_gettop(L);
        if (!closure || closure->isC || !closure->l.p)
            luaL_argerrorL(L, 1, "luau function expected");
        return closure->l.p;
    }

    int debug_getprotos(lua_State* L)
    {
        int owner = 0;
        Proto* p = get_parent_proto(L, owner);

        lua_createtable(L, p->sizep, 0);
        for (int i = 0; i < p->sizep; i++)
        {
            ProtoProxy::Push(L, p->p[i], owner);
            lua_rawseti(L, -2, i + 1);
        }

        return 1;
    }

    int debug_getproto(lua_State* L)
    {
        luaL_checktype(L, 2, LUA_TNUMBER);
        const int index = static_cast<int>(lua_tointeger(L, 2));
        const bool active = luaL_optboolean(L, 3, false);

        int owner = 0;
        Proto* p = get_parent_proto(L, owner);

        if (index <= 0 || index > p->sizep)
            luaL_argerrorL(L, 2, "index out of bounds");

        Proto* wanted = p->p[index - 1];

        if (!active)
        {
            ProtoProxy::Push(L, wanted, owner);
            return 1;
        }

        const global_State* g = L->global;
        int count = 0;
        lua_newtable(L);

        lua_gc(L, LUA_GCSTOP, 0);
        for (lua_Page* page = g->allgcopages; page; page = luaM_getnextpage(page))
        {
            char* start;
            char* end;
            int busyBlocks;
            int blockSize;
            luaM_getpagewalkinfo(page, &start, &end, &busyBlocks, &blockSize);

            for (char* pos = start; pos != end; pos += blockSize)
            {
                GCObject* object = reinterpret_cast<GCObject*>(pos);
                if (object->gch.tt != LUA_TFUNCTION || isdead(g, object))
                    continue;

                Closure* closure = reinterpret_cast<Closure*>(pos);
                if (closure->isC || closure->l.p != wanted)
                    continue;

                push_closure_value(L, closure);
                lua_rawseti(L, -2, ++count);
            }
        }
        lua_gc(L, LUA_GCRESTART, 0);

        return 1;
    }

    bool is_internal_stack_frame(CallInfo* ci)
    {
        if (!ci || !ci->func || !ttisfunction(ci->func))
            return true;

        Closure* closure = clvalue(ci->func);
        if (!closure)
            return true;

        if (Closures::is_hidden_cl(closure))
            return true;

        if (closure->isC)
        {
            const char* debugName = closure->c.debugname ? getstr(closure->c.debugname) : nullptr;
            return debugName &&
                (strcmp(debugName, "hookfunction") == 0 ||
                    strcmp(debugName, "newcclosure") == 0 ||
                    strcmp(debugName, "__supq_hookfunction_dispatch") == 0);
        }

        Proto* proto = closure->l.p;
        const char* source = proto && proto->source ? getstr(proto->source) : nullptr;
        return source && strcmp(source, "=hookfunction") == 0;
    }

    CallInfo* resolve_visible_stack_level(lua_State* target, int level)
    {
        if (!target || level <= 0)
            return nullptr;

        int visibleLevel = 0;
        for (CallInfo* ci = target->ci; ci > target->base_ci;)
        {
            --ci;

            if (is_internal_stack_frame(ci))
                continue;

            if (++visibleLevel == level)
                return ci;
        }

        return nullptr;
    }

    int stack_frame_size(lua_State* target, CallInfo* ci, Closure* closure)
    {
        if (!target || !ci || !ci->base)
            return 0;

        int size = 0;
        if (closure && !closure->isC && closure->l.p)
            size = closure->l.p->maxstacksize;

        if (size <= 0 && ci->top && ci->top > ci->base)
            size = static_cast<int>(ci->top - ci->base);

        if (size <= 0 && target->top > ci->base)
            size = static_cast<int>(target->top - ci->base);

        const int stackAvailable = static_cast<int>((target->stack + target->stacksize) - ci->base);
        if (stackAvailable < size)
            size = stackAvailable;

        return size > 0 ? size : 0;
    }

    bool resolve_stack_target(lua_State* L, int argPos, int& out_level, CallInfo*& out_ci, Closure*& out_cl)
    {
        out_level = 0;
        out_ci = nullptr;
        out_cl = nullptr;

        const int stack_size = static_cast<int>(L->ci - L->base_ci);

        if (lua_isnumber(L, argPos))
        {
            out_level = lua_tointeger(L, argPos);
            out_ci = resolve_visible_stack_level(L, out_level);
            if (!out_ci)
            {
                luaL_argerror(L, argPos, "level out of range");
                return false;
            }
        }
        else if (lua_isfunction(L, argPos))
        {
            for (int i = 1; i < stack_size; ++i)
            {
                CallInfo* ci = L->ci - i;
                if (is_internal_stack_frame(ci))
                    continue;

                if (!ci->func || !ttisfunction(ci->func))
                    continue;

                Closure* active = clvalue(ci->func);
                Closure* wanted = clvalue(luaA_toobject(L, argPos));
                if (active == wanted)
                {
                    out_level = i;
                    out_ci = ci;
                    break;
                }
            }

            if (out_level == 0)
            {
                luaL_argerror(L, argPos, "function not found in call stack");
                return false;
            }
        }
        else
        {
            luaL_argerror(L, argPos, "function or number expected");
            return false;
        }

        if (!out_ci)
        {
            out_ci = resolve_visible_stack_level(L, out_level);
            if (!out_ci)
            {
                luaL_argerror(L, argPos, "invalid level");
                return false;
            }
        }

        if (!out_ci->func || !ttisfunction(out_ci->func))
        {
            luaL_argerror(L, argPos, "not a function");
            return false;
        }

        out_cl = clvalue(out_ci->func);
        if (!out_cl)
        {
            luaL_argerror(L, argPos, "not a function");
            return false;
        }

        if (out_cl->isC)
        {
            luaL_argerror(L, argPos, "luau function expected");
            return false;
        }

        if (out_ci < L->base_ci || out_ci > L->ci)
        {
            luaL_error(L, "invalid callinfo");
            return false;
        }

        if (!out_ci->base || !out_ci->top)
        {
            luaL_error(L, "callinfo base/top is null");
            return false;
        }

        return true;
    }

    int debug_getstack(lua_State* L)
    {
        int arg = 0;
        lua_State* target = L;

        if (lua_isthread(L, 1))
        {
            target = lua_tothread(L, 1);
            if (!target)
                luaL_argerror(L, 1, "invalid thread");

            arg = 1;
        }

        const int level = luaL_checkinteger(L, arg + 1);
        CallInfo* ci = nullptr;
        if (Oth::OnHookThread(target))
            ci = Oth::Frame(target, level);
        else
            ci = resolve_visible_stack_level(target, level);
        if (!ci)
            luaL_argerror(L, arg + 1, "level out of range");

        if (ci < target->base_ci || ci > target->ci)
            luaL_error(L, "invalid callinfo");

        if (!ci->func || ci->func->tt != LUA_TFUNCTION)
            luaL_error(L, "invalid stack frame");

        Closure* closure = clvalue(ci->func);
        if (closure && closure->isC)
            luaL_error(L, "cannot get stack of a C function");

        if (!ci->base)
            luaL_error(L, "callinfo base is null");

        const int frame_size = stack_frame_size(target, ci, closure);

        if (lua_isnumber(L, arg + 2))
        {
            const int idx = lua_tointeger(L, arg + 2) - 1;

            if (idx < 0 || idx >= frame_size)
            {
                lua_pushnil(L);
                return 1;
            }

            StkId slot = ci->base + idx;
            if (slot < target->stack || slot >= target->stack + target->stacksize)
            {
                lua_pushnil(L);
                return 1;
            }

            luaC_threadbarrier(L);
            luaA_pushvalue(L, slot);
        }
        else
        {
            lua_newtable(L);

            for (int i = 0; i < frame_size; ++i)
            {
                StkId slot = ci->base + i;
                if (slot < target->stack || slot >= target->stack + target->stacksize)
                    break;

                luaC_threadbarrier(L);
                luaA_pushvalue(L, slot);
                lua_rawseti(L, -2, i + 1);
            }
        }

        return 1;
    }

    int debug_setstack(lua_State* L)
    {
        int level = 0;
        CallInfo* ci = nullptr;
        Closure* closure = nullptr;
        if (!resolve_stack_target(L, 1, level, ci, closure))
            return 0;

        const auto idx = luaL_checkinteger(L, 2) - 1;
        const int frame_size = stack_frame_size(L, ci, closure);
        if (idx >= frame_size || idx < 0)
        {
            luaL_argerror(L, 2, "index out of range");
        }

        const TValue* new_value = luaA_toobject(L, 3);
        if (!new_value)
        {
            luaL_argerror(L, 3, "invalid value");
        }

        StkId slot = ci->base + idx;
        for (CallInfo* active = ci + 1; active <= L->ci; ++active)
        {
            if (slot == active->func)
                luaL_argerror(L, 2, "cannot replace an active call frame");
        }

        setobj2s(L, slot, new_value);
        luaC_threadbarrier(L);
        return 0;
    }

    int debug_getinfo(lua_State* L)
    {
        luaL_checkany(L, 1);

        int proxy = 0;
        if (Proto* proto = ProtoProxy::To(L, 1))
        {
            lua_settop(L, 1);
            lua_pushvalue(L, 1);
            proxy = 2;
            push_inert_closure(L, proto);
            lua_replace(L, 1);
        }

        if (!(lua_isfunction(L, 1) || lua_isnumber(L, 1)))
        {
            luaL_argerror(L, 1, "function or number");
        }

        int level;
        if (lua_isnumber(L, 1))
        {
            level = lua_tointeger(L, 1);
        }
        else
        {
            level = -lua_gettop(L);
        }

        const char* desc = "sluanf";

        lua_Debug ar;
        if (!lua_getinfo(L, level, desc, &ar))
        {
            luaL_argerror(L, 1, "invalid level");
        }

        if (!lua_isfunction(L, -1))
        {
            luaL_argerror(L, 1, "level does not point to a function.");
        }

        lua_newtable(L);
        {
            if (std::strchr(desc, 's'))
            {
                lua_pushstring(L, ar.source);
                lua_setfield(L, -2, "source");

                lua_pushstring(L, ar.short_src);
                lua_setfield(L, -2, "short_src");

                lua_pushstring(L, ar.what);
                lua_setfield(L, -2, "what");

                lua_pushinteger(L, ar.linedefined);
                lua_setfield(L, -2, "linedefined");
            }

            if (std::strchr(desc, 'l'))
            {
                lua_pushinteger(L, ar.currentline);
                lua_setfield(L, -2, "currentline");
            }

            if (std::strchr(desc, 'u'))
            {
                lua_pushinteger(L, ar.nupvals);
                lua_setfield(L, -2, "nups");
            }

            if (std::strchr(desc, 'a'))
            {
                lua_pushinteger(L, ar.isvararg);
                lua_setfield(L, -2, "is_vararg");

                lua_pushinteger(L, ar.nparams);
                lua_setfield(L, -2, "numparams");
            }

            if (std::strchr(desc, 'n'))
            {
                lua_pushstring(L, ar.name ? ar.name : "");
                lua_setfield(L, -2, "name");
            }

            if (std::strchr(desc, 'f'))
            {
                lua_pushvalue(L, proxy ? proxy : -2);
                if (!proxy)
                    lua_remove(L, -3);
                lua_setfield(L, -2, "func");
            }
        }

        return 1;
    }

    inline bool is_hidden_stack_frame(CallInfo* ci)
    {
        if (!ci || !ci->func || !ttisfunction(ci->func))
            return false;

        return Closures::is_hidden_cl(clvalue(ci->func));
    }

    inline int resolve_hidden_aware_level(lua_State* target, int level, bool ownFrame = true)
    {
        if (!target || level < 0)
            return -1;

        const int depth = static_cast<int>(target->ci - target->base_ci);
        const int first = ownFrame ? 1 : 0;

        if (level < first)
            return level < depth ? level : -1;

        int real = level;
        for (int i = first; i < depth && is_hidden_stack_frame(target->ci - i); ++i)
            ++real;

        while (real < depth && is_hidden_stack_frame(target->ci - real))
            ++real;

        return real < depth ? real : -1;
    }

    int debug_info(lua_State* L)
    {
        int arg = 0;
        lua_State* target = L;

        if (lua_isthread(L, 1))
        {
            target = lua_tothread(L, 1);
            if (!target)
                luaL_argerror(L, 1, "invalid thread");

            arg = 1;
        }

        int l1top = 0;
        if (target != L)
        {
            lua_rawcheckstack(target, 1);
            l1top = lua_gettop(target);
        }

        int level;
        bool proxyArg = false;
        if (lua_isnumber(L, arg + 1))
        {
            level = static_cast<int>(lua_tointeger(L, arg + 1));
            luaL_argcheck(L, level >= 0, arg + 1, "level can't be negative");

            if (Oth::OnHookThread(target))
            {
                CallInfo* frame = Oth::Frame(target, level);
                if (!frame)
                    return 0;
                level = static_cast<int>(target->ci - frame);
            }
            else
            {
                level = resolve_hidden_aware_level(target, level, target == L);
                if (level < 0)
                    return 0;
            }
        }
        else if (arg == 0 && lua_isfunction(L, 1))
        {
            level = -lua_gettop(L);
        }
        else if (Proto* proto = arg == 0 ? ProtoProxy::To(L, 1) : nullptr)
        {
            luaL_checkstring(L, 2);
            lua_settop(L, 2);
            push_inert_closure(L, proto);
            level = -1;
            proxyArg = true;
        }
        else
        {
            luaL_argerror(L, arg + 1, "function or level expected");
            return 0;
        }

        const char* options = luaL_checkstring(L, arg + 2);

        lua_Debug ar;
        if (!lua_getinfo(target, level, options, &ar))
            return 0;

        int results = 0;
        bool occurs[26] = {};

        for (const char* it = options; *it; ++it)
        {
            if (unsigned(*it - 'a') < 26)
            {
                if (occurs[*it - 'a'])
                {
                    if (target != L)
                        lua_settop(target, l1top);

                    luaL_argerror(L, arg + 2, "duplicate option");
                }

                occurs[*it - 'a'] = true;
            }

            switch (*it)
            {
            case 's':
                lua_pushstring(L, ar.short_src);
                results++;
                break;

            case 'l':
                lua_pushinteger(L, ar.currentline);
                results++;
                break;

            case 'n':
                lua_pushstring(L, ar.name ? ar.name : "");
                results++;
                break;

            case 'f':
                if (proxyArg)
                    lua_pushvalue(L, 1);
                else if (target == L)
                    lua_pushvalue(L, -1 - results);
                else
                    lua_xmove(target, L, 1);
                results++;
                break;

            case 'a':
                lua_pushinteger(L, ar.nparams);
                lua_pushboolean(L, ar.isvararg);
                results += 2;
                break;

            default:
                if (target != L)
                    lua_settop(target, l1top);

                luaL_argerror(L, arg + 2, "invalid option");
            }
        }

        return results;
    }

    static int coroutine_running(lua_State* L)
    {
        lua_State* target = Oth::Original(L);
        lua_pushthread(target);
        if (target != L)
            lua_xmove(target, L, 1);
        return 1;
    }

    int debug_traceback(lua_State* L)
    {
        int arg = 0;
        lua_State* target = L;

        if (lua_isthread(L, 1))
        {
            target = lua_tothread(L, 1);
            if (!target)
                luaL_argerror(L, 1, "invalid thread");

            arg = 1;
        }

        const char* msg = luaL_optstring(L, arg + 1, nullptr);
        const int level = luaL_optinteger(L, arg + 2, (L == target) ? 1 : 0);
        luaL_argcheck(L, level >= 0, arg + 2, "level can't be negative");

        luaL_Strbuf buf;
        luaL_buffinit(L, &buf);

        if (msg)
        {
            luaL_addstring(&buf, msg);
            luaL_addstring(&buf, "\n");
        }

        if (Oth::OnHookThread(target))
        {
            for (int i = level; ; ++i)
            {
                lua_State* state = target;
                CallInfo* frame = Oth::Frame(state, i);
                if (!frame)
                    break;

                int real = static_cast<int>(state->ci - frame);
                lua_Debug ar;
                if (!lua_getinfo(state, real, "sln", &ar))
                    break;

                if (strcmp(ar.what, "C") == 0)
                    continue;

                if (ar.source)
                    luaL_addstring(&buf, ar.short_src);

                if (ar.currentline > 0)
                {
                    char line[32];
                    std::snprintf(line, sizeof(line), ":%d", ar.currentline);
                    luaL_addstring(&buf, line);
                }

                if (ar.name && ar.name[0] != '\0')
                {
                    luaL_addstring(&buf, " function ");
                    luaL_addstring(&buf, ar.name);
                }

                luaL_addstring(&buf, "\n");
            }

            luaL_pushresult(&buf);
            return 1;
        }

        const int start = resolve_hidden_aware_level(target, level, target == L);
        const int depth = static_cast<int>(target->ci - target->base_ci);

        lua_Debug ar;
        for (int real = start; start >= 0 && real < depth; ++real)
        {
            if (real > 0 && is_hidden_stack_frame(target->ci - real))
                continue;

            if (!lua_getinfo(target, real, "sln", &ar))
                break;

            if (strcmp(ar.what, "C") == 0)
                continue;

            if (ar.source)
                luaL_addstring(&buf, ar.short_src);

            if (ar.currentline > 0)
            {
                char line[32];
                char* lineend = line + sizeof(line);
                char* lineptr = lineend;
                for (unsigned int r = ar.currentline; r > 0; r /= 10)
                    *--lineptr = '0' + (r % 10);

                luaL_addchar(&buf, ':');
                luaL_addlstring(&buf, lineptr, lineend - lineptr);
            }

            if (ar.name)
            {
                luaL_addstring(&buf, " function ");
                luaL_addstring(&buf, ar.name);
            }

            luaL_addchar(&buf, '\n');
        }

        luaL_pushresult(&buf);
        return 1;
    }

    int debug_error(lua_State* L)
    {
        const int level = luaL_optinteger(L, 2, 1);
        lua_settop(L, 1);

        if (lua_isstring(L, 1) && level > 0)
        {
            lua_State* state = L;
            int real = -1;
            if (Oth::OnHookThread(state))
            {
                CallInfo* frame = Oth::Frame(state, level);
                if (frame)
                    real = static_cast<int>(state->ci - frame);
            }
            else
            {
                real = resolve_hidden_aware_level(L, level);
            }

            lua_Debug ar;
            if (real >= 0 && lua_getinfo(state, real, "sl", &ar) && ar.currentline > 0)
            {
                lua_pushfstring(L, "%s:%d: ", ar.short_src, ar.currentline);
            }
            else
            {
                lua_rawcheckstack(L, 1);
                lua_pushliteral(L, "");
            }

            lua_pushvalue(L, 1);
            lua_concat(L, 2);
        }

        lua_error(L);
        return 0;
    }

    inline int base_setfenv(lua_State* L)
    {
        luaL_checktype(L, 2, LUA_TTABLE);

        if (lua_isfunction(L, 1))
        {
            lua_pushvalue(L, 1);
        }
        else
        {
            const int level = static_cast<int>(luaL_checkinteger(L, 1));
            luaL_argcheck(L, level >= 0, 1, "level must be non-negative");

            lua_State* state = L;
            int real = -1;
            if (Oth::OnHookThread(state))
            {
                CallInfo* frame = Oth::Frame(state, level);
                if (frame)
                    real = static_cast<int>(state->ci - frame);
            }
            else
            {
                real = resolve_hidden_aware_level(L, level);
            }

            lua_Debug ar;
            if (real < 0 || lua_getinfo(state, real, "f", &ar) == 0)
                luaL_argerror(L, 1, "invalid level");

            if (state != L)
                lua_xmove(state, L, 1);

            if (lua_isnil(L, -1))
                luaL_error(L, "no function environment for tail call at level %d", level);
        }

        lua_pushvalue(L, 2);
        lua_setsafeenv(L, -1, false);

        if (lua_isnumber(L, 1) && lua_tonumber(L, 1) == 0)
        {
            lua_pushthread(L);
            lua_insert(L, -2);
            lua_setfenv(L, -2);
            return 0;
        }

        if (lua_iscfunction(L, -2) || lua_setfenv(L, -2) == 0)
            luaL_error(L, "'setfenv' cannot change environment of given object");

        return 1;
    }

    template <lua_CFunction Impl>
    int ShieldedBuiltin(lua_State* L)
    {
        const int top = lua_gettop(L);
        unsigned long Fault = 0;
        const int Results = SafeCall::Invoke(Impl, L, &Fault);
        if (Fault != 0)
        {
            if (Fault == EXCEPTION_STACK_OVERFLOW)
                _resetstkoflw();

            lua_settop(L, top);
            luaL_error(L, "%s (0x%08X)", SafeCall::FaultName(Fault), static_cast<unsigned int>(Fault));
        }
        return Results;
    }

    inline bool RedirectEngineBuiltin(lua_State* L, const char* library, const char* name, lua_CFunction impl)
    {
        lua_State* main = L->global->mainthread;
        if (!main || !main->gt)
            return false;

        const int top = lua_gettop(L);
        lua_rawcheckstack(L, 2);
        luaC_threadbarrier(L);
        sethvalue(L, L->top, main->gt);
        incr_top(L);

        if (library)
        {
            lua_rawgetfield(L, -1, library);
            lua_remove(L, -2);
            if (!lua_istable(L, -1))
            {
                lua_settop(L, top);
                return false;
            }
        }

        lua_rawgetfield(L, -1, name);
        lua_remove(L, -2);
        if (!lua_iscfunction(L, -1))
        {
            lua_settop(L, top);
            return false;
        }

        Closure* builtin = clvalue(index2addr(L, -1));
        if (builtin->c.f == SafeCall::Guard)
        {
            lua_settop(L, top);
            return false;
        }

        builtin->c.f = impl;
        builtin->c.cont = nullptr;
        return true;
    }

    int debug_setinfo(lua_State* L)
    {
        luaL_checktype(L, 2, LUA_TTABLE);

        Proto* proto = ProtoProxy::To(L, 1);
        if (!proto)
        {
            if (lua_isfunction(L, 1) && lua_iscfunction(L, 1))
                luaL_argerror(L, 1, "Luau function expected");
            proto = header_get_function(L)->l.p;
        }
        if (!proto)
            luaL_argerror(L, 1, "Luau function expected");

        lua_getfield(L, 2, "source");
        lua_getfield(L, 2, "short_src");
        lua_getfield(L, 2, "name");
        lua_getfield(L, 2, "currentline");
        const int source = lua_gettop(L) - 3;
        const int shortSrc = source + 1;
        const int name = source + 2;
        const int line = source + 3;

        if (lua_isstring(L, source) || lua_isstring(L, shortSrc))
        {
            std::string value;
            if (lua_isstring(L, source))
            {
                size_t length = 0;
                const char* text = lua_tolstring(L, source, &length);
                value.assign(text, length);
            }
            else
            {
                const char* old = proto->source ? getstr(proto->source) : "";
                if (old[0] == '=' || old[0] == '@')
                    value.push_back(old[0]);
                size_t length = 0;
                const char* text = lua_tolstring(L, shortSrc, &length);
                value.append(text, length);
            }

            TString* str = luaS_newlstr(L, value.data(), value.size());
            proto->source.Set(str);
            luaC_objbarrier(L, proto, str);
        }

        if (lua_isstring(L, name))
        {
            size_t length = 0;
            const char* text = lua_tolstring(L, name, &length);
            TString* str = luaS_newlstr(L, text, length);
            proto->debugname.Set(str);
            luaC_objbarrier(L, proto, str);
        }

        if (lua_isnumber(L, line))
            proto->linedefined = static_cast<int>(lua_tointeger(L, line));

        return 0;
    }

    int debug_validlevel(lua_State* L)
    {
        const int level = static_cast<int>(luaL_checkinteger(L, 1));
        lua_State* target = lua_isthread(L, 2) ? lua_tothread(L, 2) : L;

        lua_Debug ar{};
        lua_pushboolean(L, target && level >= 0 && lua_getinfo(target, level, "", &ar) != 0);
        return 1;
    }

    int debug_getcallstack(lua_State* L)
    {
        lua_State* target = lua_isthread(L, 1) ? lua_tothread(L, 1) : L;

        lua_newtable(L);
        const int out = lua_gettop(L);
        int count = 0;

        int level = target == L ? 1 : 0;

        for (int hops = 0; target && hops < 16; ++hops)
        {
            for (;; ++level)
            {
                lua_Debug ar{};
                lua_rawcheckstack(target, 1);
                if (!lua_getinfo(target, level, "fl", &ar))
                    break;

                if (target != L)
                    lua_xmove(target, L, 1);

                Closure* cl = clvalue(luaA_toobject(L, -1));
                if (Closures::is_hidden_cl(cl))
                {
                    lua_pop(L, 1);
                    continue;
                }

                lua_createtable(L, 0, 2);
                lua_insert(L, -2);
                lua_setfield(L, -2, "func");
                if (!cl->isC && ar.currentline >= 0)
                {
                    lua_pushinteger(L, ar.currentline);
                    lua_setfield(L, -2, "currentline");
                }
                lua_rawseti(L, out, ++count);
            }

            lua_State* orig = Oth::Original(target);
            if (orig == target)
                break;

            target = orig;
            level = 0;
        }

        lua_settop(L, out);
        return 1;
    }

    int debug_getregistry(lua_State* L)
    {
        lua_pushvalue(L, LUA_REGISTRYINDEX);
        return 1;
    }
}

void debug_library::initialize(lua_State* L)
{
    if (Debug::RedirectEngineBuiltin(L, nullptr, "error", Debug::ShieldedBuiltin<Debug::debug_error>))
        lua_setglobal(L, "error");
    if (Debug::RedirectEngineBuiltin(L, nullptr, "setfenv", Debug::ShieldedBuiltin<Debug::base_setfenv>))
        lua_setglobal(L, "setfenv");

    lua_getglobal(L, "debug");
    if (!lua_istable(L, -1))
    {
        lua_pop(L, 1);
        lua_newtable(L);
    }

    lua_newtable(L);
    lua_pushnil(L);
    while (lua_next(L, -3))
    {
        lua_pushvalue(L, -2);
        lua_insert(L, -2);
        lua_rawset(L, -4);
    }
    lua_remove(L, -2);

    if (Debug::RedirectEngineBuiltin(L, "debug", "info", Debug::ShieldedBuiltin<Debug::debug_info>))
        lua_setfield(L, -2, "info");
    if (Debug::RedirectEngineBuiltin(L, "debug", "traceback", Debug::ShieldedBuiltin<Debug::debug_traceback>))
        lua_setfield(L, -2, "traceback");

    if (Debug::RedirectEngineBuiltin(L, "coroutine", "running", Debug::ShieldedBuiltin<Debug::coroutine_running>))
        lua_pop(L, 1);

    Utils::AddTableFunction(L, "getproto", Debug::debug_getproto);
    Utils::AddTableFunction(L, "getprotos", Debug::debug_getprotos);
    Utils::AddTableFunction(L, "getstack", Debug::debug_getstack);
    Utils::AddTableFunction(L, "getinfo", Debug::debug_getinfo);
    Utils::AddTableFunction(L, "getupvalue", Debug::debug_getupvalue);
    Utils::AddTableFunction(L, "getupvalues", Debug::debug_getupvalues);
    Utils::AddTableFunction(L, "getconstant", Debug::debug_getconstant);
    Utils::AddTableFunction(L, "getconstants", Debug::debug_getconstants);
    Utils::AddTableFunction(L, "setconstant", Debug::debug_setconstant);
    Utils::AddTableFunction(L, "setstack", Debug::debug_setstack);
    Utils::AddTableFunction(L, "setupvalue", Debug::debug_setupvalue);
    Utils::AddTableFunction(L, "getregistry", Debug::debug_getregistry);
    Utils::AddTableFunction(L, "setinfo", Debug::debug_setinfo);
    Utils::AddTableFunction(L, "validlevel", Debug::debug_validlevel);
    Utils::AddTableFunction(L, "isvalidlevel", Debug::debug_validlevel);
    Utils::AddTableFunction(L, "getcallstack", Debug::debug_getcallstack);
    Utils::AddFunction(L, "getinfo", Debug::debug_getinfo);
    Utils::AddFunction(L, "setinfo", Debug::debug_setinfo);
    Utils::AddFunction(L, "setstack", Debug::debug_setstack);
    Utils::AddFunction(L, "getstack", Debug::debug_getstack);
    Utils::AddFunction(L, "getconstant", Debug::debug_getconstant);
    Utils::AddFunction(L, "setconstant", Debug::debug_setconstant);
    Utils::AddFunction(L, "getconstants", Debug::debug_getconstants);
    Utils::AddFunction(L, "getproto", Debug::debug_getproto);
    Utils::AddFunction(L, "getprotos", Debug::debug_getprotos);
    Utils::AddFunction(L, "getupvalue", Debug::debug_getupvalue);
    Utils::AddFunction(L, "setupvalue", Debug::debug_setupvalue);
    Utils::AddFunction(L, "getupvalues", Debug::debug_getupvalues);

    lua_setreadonly(L, -1, true);
    lua_setglobal(L, "debug");
}

namespace Debug
{
    void RegisterLibrary(lua_State* L)
    {
        ProtoProxy::RegisterLibrary(L);
        debug_library::initialize(L);
    }
}
