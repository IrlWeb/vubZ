#pragma once

#include <unordered_map>

#include <lapi.h>
#include <lstate.h>
#include <lgc.h>
#include <ldo.h>
#include <lualib.h>

namespace CleanCall
{
    struct Entry
    {
        lua_CFunction Func;
        lua_Continuation Cont;
    };

    enum class RunState
    {
        Running,
        CallerWaiting,
        Abandoned,
    };

    struct Run
    {
        Closure* Target;
        lua_State* Caller;
        int CallerRef;
        int SelfRef;
        int YieldTop;
        RunState State;
        bool Failed;
    };

    struct Pending
    {
        lua_State* Thread;
        int StackBase;
        bool Completed;
        bool Failed;
    };

    inline std::unordered_map<Closure*, Entry> Targets;
    inline std::unordered_map<Closure*, Closure*> Olds;
    inline std::unordered_map<lua_State*, Run> Runs;
    inline std::unordered_map<lua_State*, Pending> Pendings;

    inline void Reset()
    {
        Targets.clear();
        Olds.clear();
        Runs.clear();
        Pendings.clear();
    }

    inline void push_closure(lua_State* L, Closure* closure)
    {
        lua_rawcheckstack(L, 1);
        luaC_threadbarrier(L);
        setclvalue(L, L->top, closure);
        incr_top(L);
    }

    inline void push_thread(lua_State* L, lua_State* thread)
    {
        lua_rawcheckstack(L, 1);
        luaC_threadbarrier(L);
        setthvalue(L, L->top, thread);
        incr_top(L);
    }

    inline void release_refs(lua_State* L, Run& run)
    {
        if (run.CallerRef != LUA_NOREF)
            lua_unref(L, run.CallerRef);
        if (run.SelfRef != LUA_NOREF)
            lua_unref(L, run.SelfRef);

        run.CallerRef = LUA_NOREF;
        run.SelfRef = LUA_NOREF;
    }

    inline bool IsStackSensitive(lua_State* L, Closure* target)
    {
        static const char* const Globals[] = {
            "getfenv", "setfenv", "error", "pcall", "xpcall", "ypcall", "require",
            "wait", "Wait", "spawn", "Spawn", "delay", "Delay", "loadstring",
        };
        static const char* const Libraries[] = { "debug", "coroutine", "task" };

        auto matches = [&](int index) {
            const TValue* value = index2addr(L, index);
            if (!ttisfunction(value))
                return false;
            Closure* cl = clvalue(value);
            return cl == target || (cl->isC && cl->c.f == target->c.f);
        };

        const int top = lua_gettop(L);
        lua_rawcheckstack(L, 4);
        luaC_threadbarrier(L);
        sethvalue(L, L->top, L->global->mainthread->gt);
        incr_top(L);
        const int globals = lua_gettop(L);

        bool found = false;
        for (const char* name : Globals)
        {
            lua_rawgetfield(L, globals, name);
            found = matches(-1);
            lua_pop(L, 1);
            if (found)
                break;
        }

        for (int i = 0; !found && i < static_cast<int>(sizeof(Libraries) / sizeof(*Libraries)); ++i)
        {
            lua_rawgetfield(L, globals, Libraries[i]);
            if (lua_istable(L, -1))
            {
                lua_pushnil(L);
                while (lua_next(L, -2))
                {
                    found = matches(-1);
                    lua_pop(L, 1);
                    if (found)
                        break;
                }
            }
        }

        lua_settop(L, top);
        return found;
    }

    inline bool IsBaseFrame(lua_State* L, Closure* target)
    {
        if (L->ci != L->base_ci + 1)
            return false;

        auto it = Runs.find(L);
        return it != Runs.end() && it->second.Target == target;
    }

    inline int Finish(lua_State* L, int results, int status)
    {
        auto it = Runs.find(L);
        if (it == Runs.end())
        {
            if (status != LUA_OK)
                lua_error(L);
            return results;
        }

        if (it->second.State == RunState::Running)
        {
            it->second.Failed = status != LUA_OK;
            return results;
        }

        Run run = it->second;
        Runs.erase(it);

        if (run.State == RunState::CallerWaiting)
        {
            auto pending = Pendings.find(run.Caller);
            if (pending != Pendings.end() && pending->second.Thread == L)
            {
                pending->second.Completed = true;
                pending->second.Failed = status != LUA_OK;

                lua_rawcheckstack(L, 4);
                luaC_threadbarrier(L);
                sethvalue(L, L->top, L->global->mainthread->gt);
                incr_top(L);
                lua_rawgetfield(L, -1, "task");
                lua_remove(L, -2);
                if (lua_istable(L, -1))
                    lua_rawgetfield(L, -1, "spawn");
                else
                    lua_pushnil(L);
                lua_remove(L, -2);
                lua_insert(L, -(results + 1));
                push_thread(L, run.Caller);
                lua_insert(L, -(results + 1));

                release_refs(L, run);

                if (lua_pcall(L, results + 1, 0, 0) != LUA_OK)
                {
                    Pendings.erase(run.Caller);
                    lua_pop(L, 1);
                }

                return 0;
            }
        }

        release_refs(L, run);
        lua_settop(L, 0);
        return 0;
    }

    inline int RunOriginal(lua_State* L)
    {
        auto entry = Targets.find(curr_func(L));
        if (entry == Targets.end())
            return 0;

        L->ci->flags |= LUA_CALLINFO_HANDLE;
        L->ci->errfunc = 0;

        const int results = entry->second.Func(L);
        if (results < 0)
        {
            auto run = Runs.find(L);
            if (run != Runs.end())
                run->second.YieldTop = lua_gettop(L);
            return results;
        }

        return Finish(L, results, LUA_OK);
    }

    inline int ContinueOriginal(lua_State* L, int status)
    {
        if (status != LUA_OK)
            return Finish(L, 1, status);

        auto entry = Targets.find(curr_func(L));
        if (entry != Targets.end() && entry->second.Cont)
        {
            const int results = entry->second.Cont(L, status);
            if (results < 0)
                return results;
            return Finish(L, results, LUA_OK);
        }

        auto run = Runs.find(L);
        const int base = run != Runs.end() ? run->second.YieldTop : 0;
        const int results = lua_gettop(L) - base;
        return Finish(L, results > 0 ? results : 0, LUA_OK);
    }

    inline int CallCont(lua_State* L, int status)
    {
        auto it = Pendings.find(L);

        if (it == Pendings.end())
        {
            if (status != LUA_OK)
                lua_error(L);
            return lua_gettop(L);
        }

        const Pending pending = it->second;
        Pendings.erase(it);

        if (!pending.Completed)
        {
            auto run = Runs.find(pending.Thread);
            if (run != Runs.end())
                run->second.State = RunState::Abandoned;
        }
        else if (pending.Failed)
        {
            lua_error(L);
        }

        return lua_gettop(L) - pending.StackBase;
    }

    inline int Call(lua_State* L)
    {
        auto old = Olds.find(curr_func(L));
        if (old == Olds.end())
            return 0;

        Closure* target = old->second;
        const int nargs = lua_gettop(L);

        if (Targets.find(target) == Targets.end())
        {
            push_closure(L, target);
            lua_insert(L, 1);
            return lua_callyieldable(L, nargs, LUA_MULTRET);
        }

        lua_State* thread = lua_newthread(L);
        thread->namecall = L->namecall;

        lua_rawcheckstack(thread, nargs + 1);
        push_closure(thread, target);
        for (int i = 1; i <= nargs; ++i)
            lua_xpush(L, thread, i);

        Runs[thread] = Run{ target, L, LUA_NOREF, LUA_NOREF, 0, RunState::Running, false };

        const int status = lua_resume(thread, L, nargs);

        if (status == LUA_OK)
        {
            auto run = Runs.find(thread);
            const bool failed = run != Runs.end() && run->second.Failed;
            if (run != Runs.end())
                Runs.erase(run);

            if (failed)
            {
                lua_xmove(thread, L, 1);
                lua_error(L);
            }

            const int results = lua_gettop(thread);
            lua_rawcheckstack(L, results);
            lua_xmove(thread, L, results);
            return results;
        }

        if (status == LUA_YIELD || status == LUA_BREAK)
        {
            Run& run = Runs[thread];

            lua_pushvalue(L, nargs + 1);
            run.SelfRef = lua_ref(L, -1);
            lua_pop(L, 1);

            lua_pushthread(L);
            run.CallerRef = lua_ref(L, -1);
            lua_pop(L, 1);

            if (!lua_isyieldable(L))
            {
                run.State = RunState::Abandoned;
                luaL_error(L, "attempt to yield across metamethod/C-call boundary");
            }

            run.State = RunState::CallerWaiting;
            Pendings[L] = Pending{ thread, nargs + 1, false, false };
            return lua_yield(L, 0);
        }

        Runs.erase(thread);
        lua_xmove(thread, L, 1);
        lua_error(L);
        return 0;
    }

    inline void MakeOld(lua_State* L, Closure* clone, Closure* target, lua_CFunction func, lua_Continuation cont)
    {
        Targets[target] = Entry{ func, cont };
        Olds[clone] = target;

        push_closure(L, target);
        lua_ref(L, -1);
        lua_pop(L, 1);

        clone->c.f = Call;
        clone->c.cont = CallCont;
    }

    inline void OnCloned(Closure* source, Closure* clone)
    {
        auto it = Olds.find(source);
        if (it != Olds.end())
            Olds[clone] = it->second;
    }

    inline bool RealEntry(Closure* old, lua_CFunction& func, lua_Continuation& cont)
    {
        auto it = Olds.find(old);
        if (it == Olds.end())
            return false;

        auto entry = Targets.find(it->second);
        if (entry == Targets.end())
            return false;

        func = entry->second.Func;
        cont = entry->second.Cont;
        return true;
    }

    inline void Forget(Closure* target)
    {
        Targets.erase(target);
    }
}
