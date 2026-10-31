#pragma once

#include <ShlObj.h>
#include <new>

#include <lapi.h>
#include <lstate.h>
#include <lgc.h>
#include <ldo.h>
#include <ltable.h>
#include <lualib.h>
#include <Core/Utilities.hpp>
#include <Core/Context.hpp>
#include <Scripting/Modules/Closures.hpp>
#include <Scripting/Modules/CleanCall.hpp>

namespace Oth
{
    inline char g_Chains;
    inline char g_Contexts;
    inline char g_Defer;

    enum ChainSlot : int
    {
        Target = 1,
        OriginalFunction,
        Head,
        RootCallback
    };

    enum NodeSlot : int
    {
        HookFunction = 1,
        Previous,
        Enabled,
        CallbackFunction
    };

    enum ContextSlot : int
    {
        State = 1,
        Origin,
        Worker,
        Chain,
        Method,
        RequestNode,
        Values
    };

    enum class Phase
    {
        Hook,
        Original,
        Waiting,
        Finished
    };

    struct Invocation
    {
        lua_State* Origin = nullptr;
        lua_State* Worker = nullptr;
        ptrdiff_t Anchor = 0;
        int Count = 0;
        int Status = LUA_OK;
        Phase Phase = Phase::Hook;
        bool Busy = false;
        bool Request = false;
        bool Done = false;
        bool Queued = false;
    };

    inline int Dispatch(lua_State* L);
    inline int Invoke(lua_State* L);
    inline int Continue(lua_State* L, int Status);
    inline int WorkerEntry(lua_State* L);
    inline int WorkerContinue(lua_State* L, int Status);
    inline int Callback(lua_State* L);
    inline int CallbackContinue(lua_State* L, int Status);
    inline int CallbackReturn(lua_State* L, int Status);

    inline void PushValue(lua_State* L, const TValue* Value)
    {
        TValue Copy = *Value;
        lua_rawcheckstack(L, 1);
        luaC_threadbarrier(L);
        setobj2s(L, L->top, &Copy);
        ++L->top;
    }

    inline void PushTable(lua_State* L, LuaTable* Table)
    {
        lua_rawcheckstack(L, 1);
        luaC_threadbarrier(L);
        sethvalue(L, L->top, Table);
        ++L->top;
    }

    inline LuaTable* TableAt(lua_State* L, int Index)
    {
        return hvalue(luaA_toobject(L, Index));
    }

    inline void PushMap(lua_State* L, void* Key, bool Weak)
    {
        lua_rawgetp(L, LUA_REGISTRYINDEX, Key);
        if (lua_istable(L, -1))
            return;
        lua_pop(L, 1);
        lua_newtable(L);
        if (Weak)
        {
            lua_newtable(L);
            lua_pushliteral(L, "kv");
            lua_setfield(L, -2, "__mode");
            lua_setmetatable(L, -2);
        }
        lua_pushvalue(L, -1);
        lua_rawsetp(L, LUA_REGISTRYINDEX, Key);
    }

    inline LuaTable* Context(lua_State* L)
    {
        const TValue* Map = luaH_getp(hvalue(registry(L)), &g_Contexts, 0);
        if (!ttistable(Map))
            return nullptr;
        TValue Key;
        setthvalue(L, &Key, L);
        const TValue* Value = luaH_get(hvalue(Map), &Key);
        return ttistable(Value) ? hvalue(Value) : nullptr;
    }

    inline Invocation* StateOf(LuaTable* Record)
    {
        return static_cast<Invocation*>(static_cast<void*>(uvalue(luaH_getnum(Record, State))->data));
    }

    inline void Assign(lua_State* L, LuaTable* Record, int Slot, int Index)
    {
        TValue Copy = *luaA_toobject(L, Index);
        setobj2t(L, luaH_setnum(L, Record, Slot), &Copy);
        luaC_barriert(L, Record, &Copy);
    }

    inline void SaveValues(lua_State* L, LuaTable* Record, int First, int Count)
    {
        lua_createtable(L, Count, 0);
        const int List = lua_gettop(L);
        for (int Index = 0; Index < Count; ++Index)
        {
            lua_pushvalue(L, First + Index);
            lua_rawseti(L, List, Index + 1);
        }
        Assign(L, Record, Values, List);
        lua_pop(L, 1);
        StateOf(Record)->Count = Count;
    }

    inline void PushValues(lua_State* L, LuaTable* Record)
    {
        const int Count = StateOf(Record)->Count;
        LuaTable* List = hvalue(luaH_getnum(Record, Values));
        lua_rawcheckstack(L, Count + 4);
        for (int Index = 0; Index < Count; ++Index)
            PushValue(L, luaH_getnum(List, Index + 1));
    }

    inline void Detach(lua_State* L, LuaTable* Record)
    {
        Invocation* Call = StateOf(Record);
        Call->Phase = Phase::Finished;
        PushMap(L, &g_Contexts, true);
        PushValue(L, luaH_getnum(Record, Worker));
        lua_pushnil(L);
        lua_rawset(L, -3);
        lua_pop(L, 1);
    }

    inline int Wake(lua_State* L)
    {
        LuaTable* Record = TableAt(L, lua_upvalueindex(1));
        Invocation* Call = StateOf(Record);
        Call->Queued = false;
        if (Call->Phase != Phase::Waiting || lua_status(Call->Origin) != LUA_YIELD)
            return 0;
        lua_rawgetp(L, LUA_REGISTRYINDEX, &g_Defer);
        PushValue(L, luaH_getnum(Record, Origin));
        if (lua_pcall(L, 1, 0, 0) != LUA_OK)
            lua_error(L);
        return 0;
    }

    inline void Notify(lua_State* L, LuaTable* Record)
    {
        Invocation* Call = StateOf(Record);
        if (Call->Busy || Call->Queued || Call->Phase == Phase::Finished)
            return;
        Call->Queued = true;
        lua_rawgetp(L, LUA_REGISTRYINDEX, &g_Defer);
        PushTable(L, Record);
        lua_pushcclosure(L, Wake, nullptr, 1);
        if (lua_pcall(L, 1, 0, 0) != LUA_OK)
        {
            Call->Queued = false;
            lua_error(L);
        }
    }

    inline int ProtectedCall(lua_State* L, int First)
    {
        L->ci->errfunc = 0;
        L->ci->flags |= LUA_CALLINFO_HANDLE;
        StkId Function = L->base + First - 1;
        const int Status = luaD_pcall(L, [](lua_State* Thread, void* Data)
        {
            luaD_callint(Thread, static_cast<StkId>(Data), LUA_MULTRET, lua_isyieldable(Thread) != 0);
        }, Function, savestack(L, Function), 0);
        expandstacklimit(L, L->top);
        if (Status == LUA_OK && isyielded(L))
            return -1;
        L->ci->flags &= ~LUA_CALLINFO_HANDLE;
        return Status;
    }

    inline int Passthrough(lua_State* L, int Status)
    {
        if (Status != LUA_OK)
            lua_error(L);
        return lua_gettop(L);
    }

    inline int WorkerContinue(lua_State* L, int Status)
    {
        LuaTable* Record = TableAt(L, lua_upvalueindex(1));
        Invocation* Call = StateOf(Record);
        Call->Status = Status;
        Call->Done = true;
        SaveValues(L, Record, Status == LUA_OK ? 1 : lua_gettop(L), Status == LUA_OK ? lua_gettop(L) : 1);
        Notify(L, Record);
        return 0;
    }

    inline int WorkerEntry(lua_State* L)
    {
        lua_pushvalue(L, lua_upvalueindex(2));
        lua_insert(L, 1);
        const int Status = ProtectedCall(L, 1);
        return Status < 0 ? C_CALL_YIELD : WorkerContinue(L, Status);
    }

    inline int Namecall(lua_State* L)
    {
        LuaTable* Record = Context(L);
        if (Record)
            PushValue(L, luaH_getnum(Record, Method));
        else if (L->namecall)
            lua_pushlstring(L, L->namecall->data, L->namecall->len);
        else
            lua_pushnil(L);
        return 1;
    }

    inline int SetNamecall(lua_State* L)
    {
        luaL_checktype(L, 1, LUA_TSTRING);
        LuaTable* Record = Context(L);
        if (Record)
            Assign(L, Record, Method, 1);
        luaC_threadbarrier(L);
        L->namecall = tsvalue(luaA_toobject(L, 1));
        return 0;
    }

    inline void SetMethod(lua_State* L, LuaTable* Record)
    {
        const TValue* Value = luaH_getnum(Record, Method);
        luaC_threadbarrier(L);
        L->namecall = ttisstring(Value) ? tsvalue(Value) : nullptr;
    }

    inline int ResumeWorker(lua_State* L, LuaTable* Record, int Count)
    {
        Invocation* Call = StateOf(Record);
        Call->Busy = true;
        Call->Phase = Phase::Hook;
        SetMethod(Call->Worker, Record);
        const int Status = lua_resume(Call->Worker, L, Count);
        Call->Busy = false;
        if (Status != LUA_OK && Status != LUA_YIELD && Status != LUA_BREAK)
        {
            Call->Done = true;
            Call->Status = Status;
            lua_xmove(Call->Worker, L, 1);
            SaveValues(L, Record, lua_gettop(L), 1);
            lua_pop(L, 1);
        }
        return Status;
    }

    inline void PushInvocation(lua_State* L, LuaTable* Chain, const TValue* Node)
    {
        TValue Copy = *Node;
        PushTable(L, Chain);
        PushValue(L, &Copy);
        lua_pushcclosurek(L, Invoke, nullptr, 2, Continue);
    }

    inline int Pump(lua_State* L, LuaTable* Record)
    {
        Invocation* Call = StateOf(Record);
        while (true)
        {
            if (Call->Done)
            {
                lua_settop(L, 1);
                PushValues(L, Record);
                const int Count = Call->Count;
                const int Status = Call->Status;
                Detach(L, Record);
                if (Status != LUA_OK)
                    lua_error(L);
                return Count;
            }
            if (Call->Request)
            {
                Call->Request = false;
                Call->Phase = Phase::Original;
                lua_settop(L, 1);
                SetMethod(L, Record);
                PushInvocation(L, hvalue(luaH_getnum(Record, ContextSlot::Chain)), luaH_getnum(Record, RequestNode));
                PushValues(L, Record);
                const int Status = ProtectedCall(L, 2);
                if (Status < 0)
                    return C_CALL_YIELD;
                const int Count = Status == LUA_OK ? lua_gettop(L) - 1 : 1;
                lua_rawcheckstack(Call->Worker, Count + 4);
                lua_pushboolean(Call->Worker, Status == LUA_OK);
                lua_xmove(L, Call->Worker, Count);
                lua_settop(L, 1);
                ResumeWorker(L, Record, Count + 1);
                continue;
            }
            Call->Phase = Phase::Waiting;
            Closure* OriginalClosure = clvalue(luaH_getnum(hvalue(luaH_getnum(Record, ContextSlot::Chain)), ChainSlot::OriginalFunction));
            bool isOrigYieldable = (OriginalClosure && OriginalClosure->isC && OriginalClosure->c.cont != nullptr);
            if (!lua_isyieldable(L) || (!IsExecutorThread(L) && !isOrigYieldable))
            {
                Detach(L, Record);
                luaL_error(L, "attempt to yield across metamethod/C-call boundary");
            }
            const int Count = lua_status(Call->Worker) == LUA_BREAK ? 0 : lua_gettop(Call->Worker);
            lua_rawcheckstack(L, Count + 2);
            lua_xmove(Call->Worker, L, Count);
            return lua_yield(L, Count);
        }
    }

    inline int Continue(lua_State* L, int Status)
    {
        if (lua_isnil(L, 1))
        {
            if (Status != LUA_OK)
                lua_error(L);
            return lua_gettop(L) - 1;
        }
        LuaTable* Record = TableAt(L, 1);
        Invocation* Call = StateOf(Record);
        if (Call->Phase == Phase::Original)
        {
            const int Count = Status == LUA_OK ? lua_gettop(L) - 1 : 1;
            lua_rawcheckstack(Call->Worker, Count + 4);
            lua_pushboolean(Call->Worker, Status == LUA_OK);
            lua_xmove(L, Call->Worker, Count);
            lua_settop(L, 1);
            ResumeWorker(L, Record, Count + 1);
        }
        else if (!Call->Done && !Call->Request)
        {
            const int Count = lua_gettop(L) - 1;
            lua_rawcheckstack(Call->Worker, Count + 2);
            lua_xmove(L, Call->Worker, Count);
            ResumeWorker(L, Record, Count);
        }
        lua_settop(L, 1);
        return Pump(L, Record);
    }

    inline int Begin(lua_State* L, LuaTable* Chain, const TValue* Entry)
    {
        LuaTable* Node = ttistable(Entry) ? hvalue(Entry) : nullptr;
        while (Node && !bvalue(luaH_getnum(Node, Enabled)))
        {
            const TValue* Parent = luaH_getnum(Node, Previous);
            Node = ttistable(Parent) ? hvalue(Parent) : nullptr;
        }
        if (!Node)
        {
            lua_pushnil(L);
            lua_insert(L, 1);
            PushValue(L, luaH_getnum(Chain, OriginalFunction));
            lua_insert(L, 2);
            const int Status = ProtectedCall(L, 2);
            return Status < 0 ? C_CALL_YIELD : Continue(L, Status);
        }
        const int Arguments = lua_gettop(L);
        lua_rawcheckstack(L, 12);
        lua_createtable(L, Values, 0);
        LuaTable* Record = TableAt(L, -1);
        Invocation* Call = static_cast<Invocation*>(lua_newuserdata(L, sizeof(Invocation)));
        new (Call) Invocation{};
        Call->Origin = L;
        Call->Anchor = L->ci - L->base_ci;
        Closure* Function = clvalue(luaH_getnum(Chain, Target));
        for (CallInfo* Active = L->ci; Active > L->base_ci; --Active)
        {
            if (ttisfunction(Active->func) && ci_func(Active) == Function)
            {
                Call->Anchor = Active - L->base_ci;
                break;
            }
        }
        Assign(L, Record, State, -1);
        lua_pop(L, 1);
        lua_pushthread(L);
        Assign(L, Record, Origin, -1);
        lua_pop(L, 1);
        PushTable(L, Chain);
        Assign(L, Record, ContextSlot::Chain, -1);
        lua_pop(L, 1);
        Namecall(L);
        Assign(L, Record, Method, -1);
        lua_pop(L, 1);
        lua_State* Thread = lua_newthread(L);
        Call->Worker = Thread;
        Assign(L, Record, Worker, -1);
        lua_pop(L, 1);
        PushMap(L, &g_Contexts, true);
        PushValue(L, luaH_getnum(Record, Worker));
        PushTable(L, Record);
        lua_rawset(L, -3);
        lua_pop(L, 1);
        lua_rawcheckstack(Thread, Arguments + 6);
        PushTable(Thread, Record);
        PushValue(Thread, luaH_getnum(Node, HookFunction));
        lua_pushcclosurek(Thread, WorkerEntry, nullptr, 2, WorkerContinue);
        for (int Index = 1; Index <= Arguments; ++Index)
            lua_xpush(L, Thread, Index);
        lua_insert(L, 1);
        lua_settop(L, 1);
        ResumeWorker(L, Record, Arguments);
        return Pump(L, Record);
    }

    inline LuaTable* ChainOf(lua_State* L, Closure* Target)
    {
        const TValue* Map = luaH_getp(hvalue(registry(L)), &g_Chains, 0);
        if (!ttistable(Map))
            return nullptr;
        TValue Key;
        setclvalue(L, &Key, Target);
        const TValue* Value = luaH_get(hvalue(Map), &Key);
        return ttistable(Value) ? hvalue(Value) : nullptr;
    }

    inline int Dispatch(lua_State* L)
    {
        LuaTable* Chain = nullptr;
        if (lua_istable(L, lua_upvalueindex(1)))
            Chain = TableAt(L, lua_upvalueindex(1));
        else
            Chain = ChainOf(L, curr_func(L));

        if (!Chain)
            return 0;

        return Begin(L, Chain, luaH_getnum(Chain, Head));
    }

    inline int Invoke(lua_State* L)
    {
        return Begin(L, TableAt(L, lua_upvalueindex(1)), luaA_toobject(L, lua_upvalueindex(2)));
    }

    inline int CallbackContinue(lua_State* L, int Status)
    {
        if (Status != LUA_OK || !lua_toboolean(L, 1))
            lua_error(L);
        return lua_gettop(L) - 1;
    }

    inline int Callback(lua_State* L)
    {
        LuaTable* Chain = TableAt(L, lua_upvalueindex(1));
        const TValue* Node = luaA_toobject(L, lua_upvalueindex(2));
        LuaTable* Record = Context(L);
        if (!Record || hvalue(luaH_getnum(Record, ContextSlot::Chain)) != Chain)
        {
            PushInvocation(L, Chain, Node);
            lua_insert(L, 1);
            const int Status = ProtectedCall(L, 1);
            return Status < 0 ? C_CALL_YIELD : Passthrough(L, Status);
        }
        const int Count = lua_gettop(L);
        Assign(L, Record, RequestNode, lua_upvalueindex(2));
        SaveValues(L, Record, 1, Count);
        StateOf(Record)->Request = true;
        Notify(L, Record);
        lua_settop(L, 0);
        return lua_yield(L, 0);
    }

    inline int CallbackReturn(lua_State* L, int Status)
    {
        LuaTable* Record = Context(L);
        if (Record && hvalue(luaH_getnum(Record, ContextSlot::Chain)) == TableAt(L, lua_upvalueindex(1)))
            return CallbackContinue(L, Status);
        return Passthrough(L, Status);
    }

    inline void PushCallback(lua_State* L, int Chain, int Node)
    {
        Chain = lua_absindex(L, Chain);
        Node = lua_absindex(L, Node);
        lua_pushvalue(L, Chain);
        lua_pushvalue(L, Node);
        lua_pushcclosurek(L, Callback, nullptr, 2, CallbackReturn);
    }

    inline int Hook(lua_State* L)
    {
        Closure* TargetFunction = Closures::to_closure(L, 1);
        Closure* hookClosure = Closures::to_closure(L, 2);
        if (!TargetFunction->isC)
            luaL_argerror(L, 1, "C function expected");

        if (hookClosure->isC)
        {
            auto it = Closures::NewCClosureRefs.find(hookClosure);
            if (it != Closures::NewCClosureRefs.end() && it->second.FunctionRef != LUA_NOREF)
            {
                lua_getref(L, it->second.FunctionRef);
                hookClosure = Closures::to_closure(L, -1);
                lua_replace(L, 2);
            }
            else
            {
                luaL_argerror(L, 2, "Luau function expected");
            }
        }
        if (lua_rawequal(L, 1, 2))
            luaL_argerror(L, 2, "target and hook must be different functions");

        lua_settop(L, 2);
        lua_rawcheckstack(L, 20);
        PushMap(L, &g_Chains, false);
        const int Map = lua_gettop(L);
        lua_pushvalue(L, 1);
        lua_rawget(L, Map);
        const bool New = !lua_istable(L, -1);
        if (New)
        {
            lua_pop(L, 1);
            lua_createtable(L, RootCallback, 0);
            lua_pushvalue(L, 1);
            lua_rawseti(L, -2, Target);
        }
        const int Chain = lua_gettop(L);
        lua_createtable(L, CallbackFunction, 0);
        const int Node = lua_gettop(L);
        lua_pushvalue(L, 2);
        lua_rawseti(L, Node, HookFunction);
        lua_rawgeti(L, Chain, Head);
        lua_pushvalue(L, -1);
        lua_rawseti(L, Node, Previous);
        PushCallback(L, Chain, -1);
        lua_pushvalue(L, -1);
        lua_rawseti(L, Node, CallbackFunction);
        const int Result = lua_gettop(L);
        lua_pushboolean(L, true);
        lua_rawseti(L, Node, Enabled);
        if (New)
        {
            lua_pushnil(L);
            PushCallback(L, Chain, -1);
            lua_rawseti(L, Chain, RootCallback);
            lua_pop(L, 1);

            Closures::push_clone(L, TargetFunction);
            Closure* OriginalClone = clvalue(index2addr(L, -1));
            Closures::set_hidden_cl(L, OriginalClone, true);
            Closures::preserve_frames(L, TargetFunction, OriginalClone);
            lua_rawseti(L, Chain, OriginalFunction);

            TargetFunction->c.f = Dispatch;
            TargetFunction->c.cont = Continue;
            Closures::invalidate_direct_calls(L, TargetFunction);
            if (TargetFunction->env)
                TargetFunction->env->safeenv = false;
            if (L->gt)
                L->gt->safeenv = false;

            lua_pushvalue(L, 1);
            lua_pushvalue(L, Chain);
            lua_rawset(L, Map);
        }
        lua_pushvalue(L, Node);
        lua_rawseti(L, Chain, Head);
        lua_pushvalue(L, Result);
        return 1;
    }

    inline int Unhook(lua_State* L)
    {
        luaL_checktype(L, 1, LUA_TFUNCTION);
        if (!lua_isnoneornil(L, 2))
            luaL_checktype(L, 2, LUA_TFUNCTION);
        lua_settop(L, 2);
        PushMap(L, &g_Chains, false);
        const int Map = lua_gettop(L);
        lua_pushvalue(L, 1);
        lua_rawget(L, Map);
        if (!lua_istable(L, -1))
        {
            lua_pushboolean(L, false);
            return 1;
        }
        LuaTable* Chain = TableAt(L, -1);
        LuaTable* Match = nullptr;
        const TValue* Value = luaH_getnum(Chain, Head);
        const bool Selected = !lua_isnil(L, 2);
        int Count = 0;
        for (; ttistable(Value); Value = luaH_getnum(hvalue(Value), Previous))
        {
            LuaTable* Node = hvalue(Value);
            if (!bvalue(luaH_getnum(Node, Enabled)))
                continue;
            ++Count;
            if (!Selected || clvalue(luaA_toobject(L, 2)) == clvalue(luaH_getnum(Node, HookFunction)) || clvalue(luaA_toobject(L, 2)) == clvalue(luaH_getnum(Node, CallbackFunction)))
                Match = Node;
        }
        if (!Match || (!Selected && Count != 1))
        {
            lua_pushboolean(L, false);
            return 1;
        }
        if (Count == 1)
        {
            Closure* targetClosure = clvalue(luaH_getnum(Chain, ChainSlot::Target));
            Closure* originalClosure = clvalue(luaH_getnum(Chain, ChainSlot::OriginalFunction));
            targetClosure->c.f = originalClosure->c.f;
            targetClosure->c.cont = static_cast<lua_Continuation>(originalClosure->c.cont);
            Closures::invalidate_direct_calls(L, targetClosure);
            lua_pushvalue(L, 1);
            lua_pushnil(L);
            lua_rawset(L, Map);
        }
        lua_pushboolean(L, false);
        Assign(L, Match, Enabled, -1);
        lua_pushboolean(L, true);
        return 1;
    }

    inline int GetRootCallback(lua_State* L)
    {
        LuaTable* Record = Context(L);
        if (!Record)
            luaL_error(L, "not running on a hook thread");
        PushValue(L, luaH_getnum(hvalue(luaH_getnum(Record, ContextSlot::Chain)), RootCallback));
        return 1;
    }

    inline int IsHookThread(lua_State* L)
    {
        lua_pushboolean(L, Context(L) != nullptr);
        return 1;
    }

    inline bool OnHookThread(lua_State* L)
    {
        return Context(L) != nullptr;
    }

    inline int GetOriginalThread(lua_State* L)
    {
        LuaTable* Record = Context(L);
        if (Record)
            PushValue(L, luaH_getnum(Record, Origin));
        else
            lua_pushnil(L);
        return 1;
    }

    inline lua_State* Original(lua_State* L)
    {
        LuaTable* Record = Context(L);
        return Record ? StateOf(Record)->Origin : L;
    }

    inline CallInfo* Frame(lua_State*& L, int Level)
    {
        if (Level < 0)
            return nullptr;
        CallInfo* Active = L->ci;
        LuaTable* Record = Context(L);
        if (Record && Level > 0)
        {
            Invocation* Call = StateOf(Record);
            L = Call->Origin;
            if (Call->Anchor < 1 || Call->Anchor > L->ci - L->base_ci)
                return nullptr;
            Active = L->base_ci + Call->Anchor;
            --Level;
        }
        for (; Active > L->base_ci; --Active)
        {
            if (!Active->func || !ttisfunction(Active->func) || Closures::is_hidden_cl(clvalue(Active->func)))
                continue;
            if (Level-- == 0)
                return Active;
        }
        return nullptr;
    }

    inline int checkcallstack(lua_State* L)
    {
        lua_State* state = Original(L);

        const char* filter = luaL_optstring(L, 1, "b");
        int level = static_cast<int>(luaL_optinteger(L, 2, 0));

        bool wantExecutor = false;
        bool wantGame = false;
        for (const char* p = filter; *p; ++p)
        {
            if (*p == 'e')
                wantExecutor = true;
            else if (*p == 'g')
                wantGame = true;
            else if (*p == 'b')
                wantExecutor = wantGame = true;
            else
                luaL_argerror(L, 1, "invalid filter");
        }

        if (!wantExecutor && !wantGame)
            luaL_argerror(L, 1, "invalid filter");

        if (level < 0)
        {
            lua_pushboolean(L, false);
            return 1;
        }

        if (!lua_isnoneornil(L, 2))
        {
            CallInfo* frame = state->ci - level;
            if (frame <= state->base_ci || !frame->func || !ttisfunction(frame->func))
            {
                lua_pushboolean(L, false);
                return 1;
            }

            Closure* cl = clvalue(frame->func);
            const bool ours = Closures::IsExecutorClosure(cl);
            lua_pushboolean(L, (ours && wantExecutor) || (!ours && wantGame));
            return 1;
        }

        for (CallInfo* frame = state->ci - 1; frame > state->base_ci; --frame)
        {
            if (!frame->func || !ttisfunction(frame->func))
                continue;

            Closure* cl = clvalue(frame->func);
            const bool ours = Closures::IsExecutorClosure(cl);
            if ((ours && wantExecutor) || (!ours && wantGame))
            {
                lua_pushboolean(L, true);
                return 1;
            }
        }

        lua_pushboolean(L, false);
        return 1;
    }

    inline bool Disabled()
    {
        bool off = false;
        wchar_t appData[MAX_PATH]{};
        if (GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH) != 0 && appData[0] != L'\0')
        {
            wchar_t path[MAX_PATH];
            off = swprintf_s(path, L"%s\\supQ\\oth.off", appData) > 0
                && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
        }
        return off;
    }

    inline void RegisterLibrary(lua_State* L)
    {
        SharedVariables::AddHiddenEntry(Oth::Invoke);
        SharedVariables::AddHiddenEntry(Oth::WorkerEntry);
        SharedVariables::AddHiddenEntry(Oth::Callback);

        Closures::g_OthHook = Oth::Hook;
        Closures::g_OthUnhook = Oth::Unhook;
        Closures::g_OthIsHooked = [](lua_State* state, Closure* cl) -> bool {
            return Oth::ChainOf(state, cl) != nullptr;
        };

        PushMap(L, &g_Chains, false);
        PushMap(L, &g_Contexts, true);

        lua_getglobal(L, "task");
        lua_getfield(L, -1, "defer");
        if (lua_isfunction(L, -1))
        {
            Closures::push_clone(L, clvalue(index2addr(L, -1)));
            lua_rawsetp(L, LUA_REGISTRYINDEX, &g_Defer);
            lua_pop(L, 1);
        }
        else
        {
            lua_pop(L, 1);
        }
        lua_pop(L, 1);

        Utils::AddFunction(L, "checkcallstack", Oth::checkcallstack);

        if (Disabled())
        {
            lua_pushnil(L);
            lua_setglobal(L, "oth");
            return;
        }

        lua_newtable(L);

        Utils::AddTableFunction(L, "hook", Oth::Hook);
        Utils::AddTableFunction(L, "unhook", Oth::Unhook);
        Utils::AddTableFunction(L, "get_original_thread", Oth::GetOriginalThread);
        Utils::AddTableFunction(L, "get_root_callback", Oth::GetRootCallback);
        Utils::AddTableFunction(L, "is_hook_thread", Oth::IsHookThread);

        lua_setglobal(L, "oth");
    }
}
