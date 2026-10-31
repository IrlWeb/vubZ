#pragma once
#include <string>
#include <cstdint>
#include <cstring>

#include <lstate.h>
#include <ldo.h>
#include <lfunc.h>
#include <lstring.h>
#include <lgc.h>
#include <ldebug.h>
#include <lmem.h>
#include <lualib.h>
#include <unordered_map>
#include <unordered_set>
#include <Core/Utilities.hpp>
#include <Core/Context.hpp>
#include <Runtime/Execution/ScriptExecution.hpp>
#include <Runtime/Scheduler/TaskScheduler.hpp>
#include <Scripting/Bridge/Manager.hpp>
#include <Scripting/Modules/ProtoProxy.hpp>
#include <Scripting/Modules/CleanCall.hpp>

namespace Closures
{
#define lua_check(L, i) if (lua_gettop(L) > i) { lua_settop(L, i); }

    struct HookRecord
    {
        int OriginalRef = LUA_NOREF;
        int HookRef = LUA_NOREF;
        Closure* HookClosure = nullptr;
    };

    struct NewCClosureRecord
    {
        int FunctionRef = LUA_NOREF;
        bool Reusable = false;
    };

    static constexpr const char* LuaHookDispatcherName = "__supq_hookfunction_dispatch";
    static std::unordered_map<Closure*, HookRecord> HookRecords;
    static std::unordered_map<Closure*, NewCClosureRecord> NewCClosureRefs;
    static std::unordered_set<Closure*> WrappedClosures;

    static std::unordered_set<Closure*> HookPlumbingClosures;
    using ExternalHookFn = int(*)(lua_State* L);
    using ExternalUnhookFn = int(*)(lua_State* L);
    using ExternalIsHookedFn = bool(*)(lua_State* L, Closure* cl);
    inline ExternalHookFn g_OthHook = nullptr;
    inline ExternalUnhookFn g_OthUnhook = nullptr;
    inline ExternalIsHookedFn g_OthIsHooked = nullptr;

    struct ProtoArrays
    {
        unsigned int* Code = nullptr;
        unsigned int* CodeEntry = nullptr;
        int SizeCode = 0;
        TValue* K = nullptr;
        int SizeK = 0;
        unsigned char* LineInfo = nullptr;
        int SizeLineInfo = 0;
        int LineGapLog2 = 0;
        int* AbsLineInfo = nullptr;
        unsigned char* DebugInsn = nullptr;
        FeedbackVectorSlot* FeedbackVec = nullptr;
        int FeedbackVecSize = 0;
    };

    static ProtoArrays read_proto_arrays(Proto* p)
    {
        ProtoArrays a;
        a.Code = p->code;
        a.CodeEntry = p->codeentry;
        a.SizeCode = p->sizecode;
        a.K = p->k;
        a.SizeK = p->sizek;
        a.LineInfo = p->lineinfo.Get();
        a.SizeLineInfo = p->sizelineinfo;
        a.LineGapLog2 = p->linegaplog2;
        a.AbsLineInfo = p->abslineinfo.Get();
        a.DebugInsn = p->debuginsn.Get();
        a.FeedbackVec = p->feedbackvec;
        a.FeedbackVecSize = p->feedbackvecsize;
        return a;
    }

    static void write_proto_arrays(Proto* p, const ProtoArrays& a)
    {
        p->code = a.Code;
        p->codeentry = a.CodeEntry;
        p->sizecode = a.SizeCode;
        p->k = a.K;
        p->sizek = a.SizeK;
        p->lineinfo.Set(a.LineInfo);
        p->sizelineinfo = a.SizeLineInfo;
        p->linegaplog2 = a.LineGapLog2;
        p->abslineinfo.Set(a.AbsLineInfo);
        p->debuginsn.Set(a.DebugInsn);
        p->feedbackvec = a.FeedbackVec;
        p->feedbackvecsize = a.FeedbackVecSize;
    }

    // hookproto swaps the array sets of the live Proto and a wrapper Proto, so
    // each array always has exactly one owner and a VM close frees it once.
    struct ProtoHookRecord
    {
        global_State* Global = nullptr;
        Proto* Wrapper = nullptr;

        unsigned char NumParams = 0;
        unsigned char IsVararg = 0;
        unsigned char MaxStackSize = 0;
        int FunId = 0;
        void* ExecData = nullptr;
        uintptr_t ExecTarget = 0;

        int HookRef = LUA_NOREF;      // the hook (also held by the wrapper's k)
        int WrapperRef = LUA_NOREF;   // keeps the wrapper proto (and the live proto's original arrays) alive
        int PinRef = LUA_NOREF;       // a closure of the live proto, so it can't be freed while grafted
    };

    static std::unordered_map<Proto*, ProtoHookRecord> ProtoHooks;
    inline std::unordered_set<Closure*> HardenedErrorFrames;

    static inline void set_bare_env(lua_State* L, int absIdx)
    {
        lua_newtable(L);
        lua_setfenv(L, absIdx);
    }

    inline void HardenErrorFrameLibs(lua_State* L)
    {
        static const char* const names[] = { "xpcall", "pcall", "assert" };

        const int top = lua_gettop(L);

        for (const char* name : names)
        {
            lua_getglobal(L, name);

            if (lua_isfunction(L, -1))
            {
                Closure* cl = clvalue(index2addr(L, -1));
                if (cl && HardenedErrorFrames.insert(cl).second)
                    set_bare_env(L, lua_gettop(L));
            }

            lua_settop(L, top);
        }

        lua_settop(L, top);
    }

    inline void ResetLibraryState()
    {
        HookRecords.clear();
        NewCClosureRefs.clear();
        WrappedClosures.clear();
        SharedVariables::ClearHiddenClosures();
        HookPlumbingClosures.clear();
        ProtoHooks.clear();
        HardenedErrorFrames.clear();
        CleanCall::Reset();
    }

    static inline void mark_hook_plumbing(Closure* closure)
    {
        if (closure)
            HookPlumbingClosures.insert(closure);
    }

    static inline bool is_hook_plumbing_cl(Closure* closure)
    {
        return closure && HookPlumbingClosures.count(closure) > 0;
    }

    static inline bool is_newcclosure_wrapper(Closure* closure)
    {
        return closure && WrappedClosures.count(closure) > 0;
    }

    static inline void set_hidden_cl(lua_State* L, Closure* closure, bool hidden)
    {
        SharedVariables::SetHiddenClosure(closure, hidden);
    }

    static inline bool is_hidden_cl(Closure* closure)
    {
        return SharedVariables::IsHiddenClosure(closure);
    }

    static inline void unhide_original(lua_State* L, int originalRef)
    {
        if (originalRef == LUA_NOREF)
            return;

        lua_getref(L, originalRef);
        if (lua_isfunction(L, -1))
            set_hidden_cl(L, clvalue(index2addr(L, -1)), false);
        lua_pop(L, 1);
    }

    static inline bool cl_isC(Closure* c) {
        return c->isC;
    }

    static inline uint8_t cl_nups(Closure* c) {
        return c->nupvalues;
    }

    static inline void gc_barrier(lua_State* L, global_State* g) {
        L->marked &= ~4u;
        L->gclist = g->gray;
        g->gray = obj2gco(L);
    }

    static inline Closure* to_closure(lua_State* L, int index)
    {
        luaL_checktype(L, index, LUA_TFUNCTION);
        return clvalue(index2addr(L, index));
    }

    static inline void push_closure(lua_State* L, Closure* closure)
    {
        luaC_threadbarrier(L);
        setclvalue(L, L->top, closure);
        incr_top(L);
    }

    static inline int finish_yieldable_call(lua_State* L)
    {
        if (L->status == SCHEDULED_REENTRY || L->status == LUA_YIELD || L->status == LUA_BREAK)
            return C_CALL_YIELD;

        return lua_gettop(L);
    }

    static int DispatchContinuation(lua_State* L, int status)
    {
        return lua_gettop(L);
    }

    static inline int call_function_on_top(lua_State* L)
    {
        lua_insert(L, 1);
        luaD_callint(L, L->base, LUA_MULTRET, lua_isyieldable(L) != 0);
        return finish_yieldable_call(L);
    }

    static inline int call_referenced_function(lua_State* L, int ref)
    {
        lua_getref(L, ref);
        if (!lua_isfunction(L, -1))
        {
            luaL_error(L, "hookfunction: referenced value is not a function");
        }

        return call_function_on_top(L);
    }

    static inline int call_hook_closure(lua_State* L, Closure* hook)
    {
        if (!hook)
        {
            luaL_error(L, "hookfunction: missing hook closure");
        }

        push_closure(L, hook);
        return call_function_on_top(L);
    }

    static Closure* clone_closure(lua_State* L, Closure* source)
    {
        Closure* clone = nullptr;

        if (source->isC)
        {
            clone = luaF_newCclosure(L, source->nupvalues, source->env);
            clone->stacksize = source->stacksize;
            clone->preload = source->preload;
            clone->c.f = source->c.f;
            clone->c.cont = static_cast<lua_Continuation>(source->c.cont);
            clone->c.debugname = source->c.debugname;

            for (int i = 0; i < source->nupvalues; ++i)
                setobj2n(L, &clone->c.upvals[i], &source->c.upvals[i]);

            auto wrapped = NewCClosureRefs.find(source);
            if (wrapped != NewCClosureRefs.end())
            {
                NewCClosureRefs[clone] = { wrapped->second.FunctionRef, false };
            }

            CleanCall::OnCloned(source, clone);
        }
        else
        {
            clone = luaF_newLclosure(L, source->nupvalues, source->env, source->l.p);
            clone->stacksize = source->stacksize;
            clone->preload = source->preload;

            for (int i = 0; i < source->nupvalues; ++i)
                setobj2n(L, &clone->l.uprefs[i], &source->l.uprefs[i]);
        }

        return clone;
    }

    static inline void push_clone(lua_State* L, Closure* source)
    {
        push_closure(L, clone_closure(L, source));
    }

    static void copy_closure_body(lua_State* L, Closure* target, Closure* source)
    {
        if (target->isC != source->isC)
            luaL_error(L, "hookfunction: incompatible closure layout");

        target->stacksize = source->stacksize;
        target->preload = source->preload;
        target->env = source->env;
        if (source->env)
            luaC_objbarrier(L, target, source->env);

        const int upvalues = source->nupvalues < target->nupvalues
            ? source->nupvalues
            : target->nupvalues;

        if (source->isC)
        {
            lua_CFunction func = source->c.f;
            lua_Continuation cont = static_cast<lua_Continuation>(source->c.cont);
            CleanCall::RealEntry(source, func, cont);

            target->c.f = func;
            target->c.cont = cont;
            target->c.debugname = source->c.debugname;

            for (int i = 0; i < upvalues; ++i)
            {
                setobj2n(L, &target->c.upvals[i], &source->c.upvals[i]);
                luaC_barrier(L, target, &target->c.upvals[i]);
            }
        }
        else
        {
            target->l.p = source->l.p;
            if (source->l.p)
                luaC_objbarrier(L, target, source->l.p);

            for (int i = 0; i < upvalues; ++i)
            {
                setobj2n(L, &target->l.uprefs[i], &source->l.uprefs[i]);
                luaC_barrier(L, target, &target->l.uprefs[i]);
            }
        }
    }

    static void preserve_frames(lua_State* L, Closure* target, Closure* snapshot)
    {
        if (!L || !L->global || !target || !snapshot)
            return;

        struct FrameContext
        {
            lua_State* State;
            Closure* Target;
            Closure* Snapshot;
        } frameContext{ L, target, snapshot };

        // Only thread-sized blocks can hold a lua_State. Walking every page just to
        // reject non-threads made hookfunction O(heap); this narrows it to the pages
        // that can actually contain a stack.
        luaM_visitgcoblock(L, &frameContext, static_cast<int>(sizeof(lua_State)),
            [](void* ud, lua_Page*, GCObject* obj) -> bool {
                auto* ctx = static_cast<FrameContext*>(ud);

                if (isdead(ctx->State->global, obj))
                    return false;

                if (obj->gch.tt != LUA_TTHREAD)
                    return false;

                lua_State* thread = gco2th(obj);
                if (!thread->base_ci || !thread->ci)
                    return false;

                for (CallInfo* frame = thread->base_ci; frame <= thread->ci; ++frame)
                {
                    if (!frame->func || !ttisfunction(frame->func))
                        continue;
                    if (clvalue(frame->func) != ctx->Target)
                        continue;

                    luaC_threadbarrier(thread);
                    setclvalue(thread, frame->func, ctx->Snapshot);
                }

                return false;
            });

        if (!target->isC)
            return;

        // C hooks additionally drop the safeenv cache on every table. Tables are all
        // allocated at sizeof(LuaTable), so only those pages need visiting.
        struct TableContext
        {
            lua_State* State;
        } tableContext{ L };

        luaM_visitgcoblock(L, &tableContext, static_cast<int>(sizeof(LuaTable)),
            [](void* ud, lua_Page*, GCObject* obj) -> bool {
                auto* ctx = static_cast<TableContext*>(ud);

                if (isdead(ctx->State->global, obj))
                    return false;

                if (obj->gch.tt == LUA_TTABLE)
                    gco2h(obj)->safeenv = false;

                return false;
            });
    }

    static void invalidate_direct_calls(lua_State* L, Closure* target)
    {
        if (!L || !L->global || !target || !target->isC)
            return;

        for (int tag = 0; tag < UTAG_INTERNAL_LIMIT; ++tag)
        {
            lua_UdataDirectAccessData& direct = L->global->udatadirect[tag];

            if (ttisfunction(&direct.indextm) && clvalue(&direct.indextm) == target)
            {
                direct.index = nullptr;
                L->global->udatadirectfields[tag] = nullptr;
            }

            if (ttisfunction(&direct.newindextm) && clvalue(&direct.newindextm) == target)
                direct.newindex = nullptr;

            if (ttisfunction(&direct.namecalltm) && clvalue(&direct.namecalltm) == target)
                direct.namecall = nullptr;
        }
    }

    static int NewCClosureDispatcher_Impl(lua_State* L)
    {
        if (SharedVariables::LastDataModel == 0)
            return 0;

        lua_pushvalue(L, lua_upvalueindex(1));
        if (!lua_isfunction(L, -1))
        {
            lua_pop(L, 1);
            if (SharedVariables::LastDataModel == 0)
                return 0;
            luaL_error(L, "newcclosure: missing wrapped function");
        }

        return call_function_on_top(L);
    }

    static int NewCClosureDispatcher(lua_State* L)
    {
        if (SharedVariables::LastDataModel == 0)
            return 0;

        unsigned long Fault = 0;
        int Results = SafeCall::Invoke(NewCClosureDispatcher_Impl, L, &Fault);
        if (Fault != 0)
        {
            if (Fault == EXCEPTION_STACK_OVERFLOW)
                _resetstkoflw();
            return 0;
        }
        return Results;
    }

    static int HookCContinuation(lua_State* L, int status)
    {
        if (SharedVariables::LastDataModel == 0)
            return 0;

        if (CleanCall::IsBaseFrame(L, curr_func(L)))
            return CleanCall::ContinueOriginal(L, status);

        if (status != LUA_OK)
        {
            lua_error(L);
            return 0;
        }

        return lua_gettop(L);
    }

    static int HookCDispatcher_Impl(lua_State* L)
    {
        if (SharedVariables::LastDataModel == 0)
        {
            auto entry = CleanCall::Targets.find(curr_func(L));
            if (entry != CleanCall::Targets.end() && entry->second.Func)
                return entry->second.Func(L);
            return 0;
        }

        Closure* current = curr_func(L);
        auto it = HookRecords.find(current);
        if (it == HookRecords.end() || it->second.HookRef == LUA_NOREF || !it->second.HookClosure)
        {
            auto entry = CleanCall::Targets.find(current);
            if (entry != CleanCall::Targets.end() && entry->second.Func)
                return entry->second.Func(L);
            return 0;
        }

        push_closure(L, it->second.HookClosure);
        lua_insert(L, 1);
        return lua_pcallyieldable(L, lua_gettop(L) - 1, LUA_MULTRET, 0);
    }

    static int HookCDispatcher(lua_State* L)
    {
        if (SharedVariables::LastDataModel == 0)
        {
            auto entry = CleanCall::Targets.find(curr_func(L));
            if (entry != CleanCall::Targets.end() && entry->second.Func)
                return entry->second.Func(L);
            return 0;
        }

        if (CleanCall::IsBaseFrame(L, curr_func(L)))
            return CleanCall::RunOriginal(L);

        unsigned long Fault = 0;
        int Results = SafeCall::Invoke(HookCDispatcher_Impl, L, &Fault);
        if (Fault != 0)
        {
            if (Fault == EXCEPTION_STACK_OVERFLOW)
                _resetstkoflw();
            return 0;
        }
        return Results;
    }

    static int HookLuaDispatcher_Impl(lua_State* L)
    {
        Closure* target = nullptr;
        for (CallInfo* ci = L->ci - 1; ci >= L->base_ci; --ci)
        {
            if (ci->func && ttisfunction(ci->func))
            {
                Closure* cl = clvalue(ci->func);
                if (HookRecords.find(cl) != HookRecords.end())
                {
                    target = cl;
                    break;
                }
            }
        }

        if (!target)
        {
            CallInfo* caller = L->ci - 1;
            if (caller >= L->base_ci && caller->func && ttisfunction(caller->func))
                target = clvalue(caller->func);
        }

        if (!target)
        {
            return 0;
        }

        auto it = HookRecords.find(target);
        if (it == HookRecords.end() || it->second.HookRef == LUA_NOREF || !it->second.HookClosure)
        {
            return 0;
        }

        return call_hook_closure(L, it->second.HookClosure);
    }

    static int HookLuaDispatcher(lua_State* L)
    {
        if (SharedVariables::LastDataModel == 0)
            return 0;

        unsigned long Fault = 0;
        int Results = SafeCall::Invoke(HookLuaDispatcher_Impl, L, &Fault);
        if (Fault != 0)
        {
            if (Fault == EXCEPTION_STACK_OVERFLOW)
                _resetstkoflw();
            return 0;
        }
        return Results;
    }

    static const std::string& lua_hook_dispatcher_bytecode()
    {
        static std::string bytecode;
        if (bytecode.empty())
            bytecode = Execution::CompileScript("return __supq_hookfunction_dispatch(...)");

        return bytecode;
    }

    static Proto* push_lua_hook_dispatcher(lua_State* L, Proto* replaced)
    {
        const std::string& bytecode = lua_hook_dispatcher_bytecode();
        if (luau_load(L, "=closure", bytecode.data(), bytecode.size(), NULL) != LUA_OK)
        {
            lua_error(L);
        }

        Closure* closure = clvalue(index2addr(L, -1));
        Proto* proto = closure->l.p;
        TaskScheduler::SetProtoCapabilities(proto, &MaxCapabilities);

        if (replaced)
        {
            proto->debugname.Set(replaced->debugname.Get());
            proto->source.Set(replaced->source.Get());
            proto->linedefined = replaced->linedefined;
        }

        return proto;
    }

    static int clonefunction(lua_State* L)
    {
        Closure* source = to_closure(L, 1);
        push_clone(L, source);
        return 1;
    }

    static int hookfunction(lua_State* L)
    {
        Closure* target = to_closure(L, 1);
        Closure* hook = to_closure(L, 2);

        if (target->isC && g_OthHook)
        {
            return g_OthHook(L);
        }

        HookRecord& record = HookRecords[target];
        if (record.OriginalRef == LUA_NOREF)
        {
            push_clone(L, target);

            Closure* clone = clvalue(index2addr(L, -1));
            set_hidden_cl(L, clone, true);

            if (target->isC && SharedVariables::ExecutorClosures.find(target) == SharedVariables::ExecutorClosures.end())
            {
                mark_hook_plumbing(target);
                mark_hook_plumbing(clone);

                if (!CleanCall::IsStackSensitive(L, target))
                    CleanCall::MakeOld(L, clone, target, target->c.f, static_cast<lua_Continuation>(target->c.cont));
            }

            record.OriginalRef = lua_ref(L, -1);
            lua_pop(L, 1);

            preserve_frames(L, target, clone);
        }
        else
        {
            lua_getref(L, record.OriginalRef);
            Closure* original = to_closure(L, -1);

            if (hook == original)
            {
                if (target->isC != original->isC)
                {
                    auto targetWrapped = NewCClosureRefs.find(target);
                    if (target->isC && !original->isC && targetWrapped != NewCClosureRefs.end())
                    {
                        if (target->nupvalues < 1)
                        {
                            lua_pop(L, 1);
                            luaL_error(L, "hookfunction: target has no upvalue slot");
                        }

                        target->stacksize = LUA_MINSTACK;
                        target->preload = 0;
                        target->env = original->env;
                        target->c.f = NewCClosureDispatcher;
                        target->c.cont = DispatchContinuation;
                        {
                            TString* dbgname = luaS_new(L, "newcclosure");
                            target->c.debugname = dbgname;
                            luaC_objbarrier(L, target, dbgname);
                        }
                        setclvalue(L, &target->c.upvals[0], original);
                        luaC_barrier(L, target, &target->c.upvals[0]);
                        if (original->env)
                            luaC_objbarrier(L, target, original->env);
                    }
                    else
                    {
                        lua_pop(L, 1);
                        luaL_error(L, "hookfunction: incompatible closure layout");
                    }
                }
                else
                {
                    copy_closure_body(L, target, original);

                    if (target->isC)
                    {
                        auto originalWrapped = NewCClosureRefs.find(original);
                        auto targetWrapped = NewCClosureRefs.find(target);

                        if (originalWrapped != NewCClosureRefs.end())
                        {
                            NewCClosureRefs[target] = { originalWrapped->second.FunctionRef, true };
                            WrappedClosures.insert(target);
                        }
                        else if (targetWrapped != NewCClosureRefs.end() && targetWrapped->second.Reusable)
                        {
                            NewCClosureRefs.erase(targetWrapped);
                            WrappedClosures.erase(target);
                        }
                    }
                }

                CleanCall::Forget(target);
                invalidate_direct_calls(L, target);

                if (record.HookRef != LUA_NOREF)
                    lua_unref(L, record.HookRef);

                set_hidden_cl(L, record.HookClosure, false);
                unhide_original(L, record.OriginalRef);
                lua_unref(L, record.OriginalRef);
                HookRecords.erase(target);

                if (NewCClosureRefs.find(target) == NewCClosureRefs.end())
                    SharedVariables::ExecutorClosures.erase(target);

                return 1;
            }

            lua_pop(L, 1);

            if (record.HookRef != LUA_NOREF)
            {
                lua_unref(L, record.HookRef);
                set_hidden_cl(L, record.HookClosure, false);
                record.HookRef = LUA_NOREF;
                record.HookClosure = nullptr;
            }
        }

        lua_pushvalue(L, 2);
        record.HookRef = lua_ref(L, -1);
        record.HookClosure = hook;
        lua_pop(L, 1);

        set_hidden_cl(L, hook, true);

        if (target->isC)
        {
            target->c.f = HookCDispatcher;
            target->c.cont = HookCContinuation;
            invalidate_direct_calls(L, target);
        }
        else
        {
            Proto* dispatcher = push_lua_hook_dispatcher(L, target->l.p);

            target->stacksize = dispatcher->maxstacksize;
            target->preload = 0;
            target->env = L->gt;
            target->l.p = dispatcher;
            luaC_objbarrier(L, target, L->gt);
            luaC_objbarrier(L, target, dispatcher);
            lua_pop(L, 1);
        }

        SharedVariables::ExecutorClosures.insert(target);
        lua_getref(L, record.OriginalRef);
        return 1;
    }

    static int restorefunction(lua_State* L)
    {
        Closure* target = to_closure(L, 1);
        auto it = HookRecords.find(target);
        if (it == HookRecords.end() || it->second.OriginalRef == LUA_NOREF)
        {
            if (g_OthIsHooked && g_OthUnhook && g_OthIsHooked(L, target))
            {
                return g_OthUnhook(L);
            }
            lua_pushboolean(L, false);
            return 1;
        }

        lua_getref(L, it->second.OriginalRef);
        Closure* original = to_closure(L, -1);

        if (target->isC != original->isC)
        {
            auto targetWrapped = NewCClosureRefs.find(target);
            if (target->isC && !original->isC && targetWrapped != NewCClosureRefs.end())
            {
                if (target->nupvalues < 1)
                {
                    lua_pop(L, 1);
                    luaL_error(L, "restorefunction: target has no upvalue slot");
                }

                target->stacksize = LUA_MINSTACK;
                target->preload = 0;
                target->env = original->env;
                target->c.f = NewCClosureDispatcher;
                target->c.cont = DispatchContinuation;
                {
                    TString* dbgname = luaS_new(L, "newcclosure");
                    target->c.debugname = dbgname;
                    luaC_objbarrier(L, target, dbgname);
                }
                setclvalue(L, &target->c.upvals[0], original);
                luaC_barrier(L, target, &target->c.upvals[0]);
                if (original->env)
                    luaC_objbarrier(L, target, original->env);

                lua_pop(L, 1);
                invalidate_direct_calls(L, target);

                if (it->second.HookRef != LUA_NOREF)
                    lua_unref(L, it->second.HookRef);

                set_hidden_cl(L, it->second.HookClosure, false);
                lua_unref(L, it->second.OriginalRef);
                HookRecords.erase(it);

                lua_pushboolean(L, true);
                return 1;
            }

            lua_pop(L, 1);
            luaL_error(L, "Closure's layout is incompatible [restorefunction]");
        }

        copy_closure_body(L, target, original);
        if (target->isC)
        {
            auto originalWrapped = NewCClosureRefs.find(original);
            auto targetWrapped = NewCClosureRefs.find(target);

            if (originalWrapped != NewCClosureRefs.end())
            {
                NewCClosureRefs[target] = { originalWrapped->second.FunctionRef, true };
                WrappedClosures.insert(target);
            }
            else if (targetWrapped != NewCClosureRefs.end() && targetWrapped->second.Reusable)
            {
                NewCClosureRefs.erase(targetWrapped);
                WrappedClosures.erase(target);
            }
        }
        lua_pop(L, 1);
        CleanCall::Forget(target);
        invalidate_direct_calls(L, target);

        if (it->second.HookRef != LUA_NOREF)
            lua_unref(L, it->second.HookRef);

        set_hidden_cl(L, it->second.HookClosure, false);
        unhide_original(L, it->second.OriginalRef);
        lua_unref(L, it->second.OriginalRef);
        HookRecords.erase(it);

        lua_pushboolean(L, true);
        return 1;
    }

    int newcclosure(lua_State* L)
    {
        luaL_checktype(L, 1, LUA_TFUNCTION);
        Closure* closure = clvalue(index2addr(L, 1));
        if (closure->isC) {
            lua_pushvalue(L, 1);
            return 1;
        }
        for (auto& pair : NewCClosureRefs) {
            if (!pair.second.Reusable)
                continue;

            lua_getref(L, pair.second.FunctionRef);
            bool same = lua_isfunction(L, -1) && clvalue(index2addr(L, -1)) == closure;
            lua_pop(L, 1);

            if (same) {
                push_closure(L, pair.first);
                return 1;
            }
        }
        const char* debugname = lua_isstring(L, 2) ? lua_tostring(L, 2) : "newcclosure";
        lua_pushvalue(L, 1);
        lua_pushcclosurek(L, NewCClosureDispatcher, debugname, 1, DispatchContinuation);
        Closure* wrapper = clvalue(index2addr(L, -1));

        lua_pushvalue(L, 1);
        NewCClosureRefs[wrapper] = { lua_ref(L, -1), true };
        lua_pop(L, 1);

        WrappedClosures.insert(wrapper);
        wrapper->env = closure->env;
        SharedVariables::ExecutorClosures.insert(wrapper);
        Environment::function_array.push_back(wrapper);
        return 1;
    }

    int isnewcclosure(lua_State* L)
    {
        luaL_checktype(L, 1, LUA_TFUNCTION);
        Closure* closure = clvalue(index2addr(L, 1));
        lua_pushboolean(L, WrappedClosures.count(closure) > 0);
        return 1;
    }

    static int PushError(lua_State* L, const char* message)
    {
        lua_pushnil(L);
        lua_pushstring(L, message);
        return 2;
    }

    static bool IsLuauBytecode(const char* data, size_t size)
    {
        if (!data || size < 2)
            return false;

        const uint8_t bytecodeVersion = static_cast<uint8_t>(data[0]);

        if (bytecodeVersion < LBC_VERSION_MIN || bytecodeVersion > LBC_VERSION_MAX)
            return false;

        if (bytecodeVersion >= 4)
        {
            const uint8_t typeVersion = static_cast<uint8_t>(data[1]);

            if (typeVersion < LBC_TYPE_VERSION_MIN ||
                typeVersion > LBC_TYPE_VERSION_MAX)
                return false;
        }

        return std::memchr(data, '\0', size) != nullptr;
    }

    int loadstring(lua_State* L)
    {
        if (!IsElevatedExecutorThread(L))
            return PushError(L, CodeLoadDeniedReason(L));

        size_t sourceLength = 0;
        const char* source = luaL_checklstring(L, 1, &sourceLength);
        if (!source)
            return PushError(L, "invalid source");

        const std::string defaultChunkName = lua_isnoneornil(L, 2)
            ? Execution::DefaultChunkName(source, sourceLength)
            : std::string();
        const char* chunkName = lua_isnoneornil(L, 2) ? defaultChunkName.c_str() : luaL_checkstring(L, 2);

        if (IsLuauBytecode(source, sourceLength))
            return PushError(L, "attempt to load Luau bytecode as source");

        std::string bytecode;

        try
        {
            bytecode = Execution::CompileScript(std::string(source, sourceLength));
        }
        catch (const std::exception& exception)
        {
            return PushError(L, exception.what());
        }
        catch (...)
        {
            return PushError(L, "unknown compiler error");
        }

        if (bytecode.empty())
            return PushError(L, "compiler returned empty bytecode");

        if (luau_load(L, chunkName, bytecode.data(), bytecode.size(), 0) != LUA_OK)
        {
            lua_pushnil(L);
            lua_insert(L, -2);
            return 2;
        }

        if (!lua_isfunction(L, -1) || lua_iscfunction(L, -1))
        {
            lua_pop(L, 1);
            return PushError(L, "compiler did not produce a Lua closure");
        }

        Closure* closure = clvalue(luaA_toobject(L, -1));
        if (!closure || closure->isC || !closure->l.p)
        {
            lua_pop(L, 1);
            return PushError(L, "invalid compiled closure");
        }

        TaskScheduler::SetProtoCapabilities(closure->l.p, &MaxCapabilities);

        lua_setsafeenv(L, LUA_ENVIRONINDEX, false);
        return 1;
    }

    int dumpbytecode(lua_State* L)
    {
        if (!IsExecutorThread(L))
            return PushError(L, "The current thread cannot dump bytecode");

        size_t sourceLength = 0;
        const char* source = luaL_checklstring(L, 1, &sourceLength);

        if (!source)
            return PushError(L, "invalid source");

        if (IsLuauBytecode(source, sourceLength))
            return PushError(L, "attempt to dump already-compiled bytecode");

        std::string bytecode;
        try
        {
            bytecode = Execution::CompileScript(std::string(source, sourceLength));
        }
        catch (const std::exception& exception)
        {
            return PushError(L, exception.what());
        }
        catch (...)
        {
            return PushError(L, "unknown compiler error");
        }

        if (bytecode.empty())
            return PushError(L, "compiler returned empty bytecode");

        lua_pushlstring(L, bytecode.data(), bytecode.size());
        return 1;
    }

    int runbytecode(lua_State* L)
    {
        if (!IsElevatedExecutorThread(L))
            return PushError(L, CodeLoadDeniedReason(L));

        size_t bytecodeLength = 0;
        const char* bytecode = luaL_checklstring(L, 1, &bytecodeLength);

        if (!bytecode)
            return PushError(L, "invalid bytecode");

        if (!IsLuauBytecode(bytecode, bytecodeLength))
            return PushError(L, "attempt to run source as bytecode, use loadstring instead");

        if (luau_load(L, "runbytecode", bytecode, bytecodeLength, 0) != LUA_OK)
        {
            lua_pushnil(L);
            lua_insert(L, -2);
            return 2;
        }

        if (!lua_isfunction(L, -1) || lua_iscfunction(L, -1))
        {
            lua_pop(L, 1);
            return PushError(L, "bytecode did not produce a Lua closure");
        }

        Closure* closure = clvalue(luaA_toobject(L, -1));
        if (!closure || closure->isC || !closure->l.p)
        {
            lua_pop(L, 1);
            return PushError(L, "invalid bytecode closure");
        }

        TaskScheduler::SetProtoCapabilities(closure->l.p, &MaxCapabilities);
        lua_setsafeenv(L, LUA_ENVIRONINDEX, false);

        int nargs = 0;
        int stackBefore = lua_gettop(L) - 1;
        if (lua_pcall(L, nargs, LUA_MULTRET, 0) != LUA_OK)
        {
            lua_pushnil(L);
            lua_insert(L, -2);
            return 2;
        }

        return lua_gettop(L) - stackBefore;
    }

    int iscclosure(lua_State* L)
    {
        if (!lua_isfunction(L, 1))
        {
            lua_pushboolean(L, false);
            return 1;
        }
        lua_pushboolean(L, lua_iscfunction(L, 1));
        return 1;
    }

    int islclosure(lua_State* L)
    {
        if (!lua_isfunction(L, 1))
        {
            lua_pushboolean(L, false);
            return 1;
        }
        lua_pushboolean(L, !lua_iscfunction(L, 1));
        return 1;
    }

    inline bool IsExecutorProto(Proto* proto)
    {
        if (!proto)
            return false;

        if (proto->userdata)
        {
            void* raw = proto->userdata.Get();
            if (raw && *static_cast<uintptr_t*>(raw) == MaxCapabilities)
                return true;
        }

        for (int i = 0; proto->p && i < proto->sizep; ++i)
        {
            if (IsExecutorProto(proto->p[i]))
                return true;
        }

        return false;
    }

    inline bool IsExecutorClosure(Closure* closure)
    {
        if (!closure)
            return false;

        if (SharedVariables::ExecutorClosures.contains(closure) ||
            WrappedClosures.contains(closure) ||
            NewCClosureRefs.contains(closure) ||
            HookRecords.contains(closure))
        {
            return true;
        }

        if (!closure->isC)
            return IsExecutorProto(closure->l.p);

        // function_array entries can be collected (signal Disconnect, dropped wrappers), so never
        // dereference them; every entry's c.f is SafeCall::Guard or NewCClosureDispatcher.
        return closure->c.f == NewCClosureDispatcher ||
            closure->c.f == HookCDispatcher ||
            closure->c.f == HookLuaDispatcher ||
            closure->c.f == SafeCall::Guard;
    }

    inline bool IsExecutorOwnedClosure(Closure* closure)
    {
        if (!closure)
            return false;

        if (!closure->isC)
            return IsExecutorProto(closure->l.p);

        for (Closure* registered : Environment::function_array)
        {
            if (registered == closure)
                return true;
        }

        if (WrappedClosures.contains(closure) || NewCClosureRefs.contains(closure))
            return true;

        if (closure->c.f == NewCClosureDispatcher || closure->c.f == HookLuaDispatcher)
            return true;

        if (HookRecords.contains(closure) || closure->c.f == HookCDispatcher)
            return false;

        return closure->c.f == SafeCall::Guard;
    }

    auto is_executor_closure(lua_State* rl) -> int
    {
        if (lua_type(rl, 1) != LUA_TFUNCTION) {
            lua_pushboolean(rl, false);
            return 1;
        }

        Closure* closure = clvalue(index2addr(rl, 1));
        lua_pushboolean(rl, IsExecutorClosure(closure));
        return 1;
    }

    inline int cloneref(lua_State* L) {
        lua_check(L, 1);
        luaL_checktype(L, 1, LUA_TUSERDATA);
        std::string ud_t = luaL_typename(L, 1);
        if (ud_t != "Instance")
            luaL_typeerrorL(L, 1, "Instance");

        const auto instance = *static_cast<void**>(lua_touserdata(L, 1));

        lua_pushlightuserdata(L, Roblox::PushInstance);
        lua_rawget(L, LUA_REGISTRYINDEX);
        const int cacheIdx = lua_gettop(L);

        lua_pushlightuserdata(L, instance);
        lua_rawget(L, cacheIdx);
        const int savedIdx = lua_gettop(L);

        lua_pushlightuserdata(L, instance);
        lua_pushnil(L);
        lua_rawset(L, cacheIdx);

        lua_getmetatable(L, 1);
        const int mtIdx = lua_gettop(L);

        Roblox::PushInstance(L, lua_touserdata(L, 1));
        const int newIdx = lua_gettop(L);

        if (!lua_isnil(L, mtIdx)) {
            lua_pushvalue(L, mtIdx);
            lua_setmetatable(L, newIdx);
        }
        lua_pushlightuserdata(L, instance);
        lua_pushvalue(L, savedIdx);
        lua_rawset(L, cacheIdx);

        // This frame sits inside the caller's register window: after we return, slot 1 (the
        // argument) and savedIdx are stale registers the GC still marks, which keeps the original
        // userdata alive for as long as the clone lives. Nil every slot but the result.
        lua_pushvalue(L, newIdx);
        const int top = lua_gettop(L);
        for (int i = 1; i < top; ++i) {
            lua_pushnil(L);
            lua_replace(L, i);
        }
        return 1;
    }

    inline int gettenv(lua_State* L)
    {
        lua_State* thread = L;
        if (!lua_isnoneornil(L, 1)) {
            luaL_checktype(L, 1, LUA_TTHREAD);
            thread = lua_tothread(L, 1);
        }
        if (!thread || !thread->gt) {
            lua_pushnil(L);
            return 1;
        }
        luaC_threadbarrier(L);
        sethvalue(L, L->top, thread->gt);
        incr_top(L);
        return 1;
    }

    static int isfunctionhooked(lua_State* L)
    {
        luaL_checktype(L, 1, LUA_TFUNCTION);
        Closure* closure = clvalue(index2addr(L, 1));
        bool hooked = (HookRecords.count(closure) > 0 && HookRecords[closure].OriginalRef != LUA_NOREF);
        if (!hooked && g_OthIsHooked)
        {
            hooked = g_OthIsHooked(L, closure);
        }
        lua_pushboolean(L, hooked);
        return 1;
    }

    static Closure* getstackhiddentarget(lua_State* L)
    {
        if (lua_isfunction(L, 1))
            return clvalue(index2addr(L, 1));

        if (lua_isnumber(L, 1))
        {
            const int level = lua_tointeger(L, 1);
            if (level < 0 || level >= lua_stackdepth(L))
            {
                luaL_argerror(L, 1, "level out of range");
                return nullptr;
            }

            lua_Debug information{};
            if (!lua_getinfo(L, level, "f", &information) || !lua_isfunction(L, -1))
            {
                luaL_argerror(L, 1, "failed to get function from level");
                return nullptr;
            }

            Closure* closure = clvalue(index2addr(L, -1));
            lua_pop(L, 1);
            return closure;
        }

        luaL_typeerror(L, 1, "function or number");
        return nullptr;
    }

    static int setstackhidden(lua_State* L)
    {
        if (!lua_isnone(L, 2))
            luaL_checktype(L, 2, LUA_TBOOLEAN);

        const bool hidden = lua_isnone(L, 2) || lua_toboolean(L, 2) != 0;
        const int type = lua_type(L, 1);

        if (type == LUA_TFUNCTION)
        {
            set_hidden_cl(L, clvalue(index2addr(L, 1)), hidden);
            return 0;
        }

        if (type == LUA_TNUMBER)
        {
            const int level = static_cast<int>(lua_tointeger(L, 1));
            if (level < 1)
                luaL_argerror(L, 1, "level must be positive");

            CallInfo* ci = L->ci;
            for (int i = 0; i < level; ++i)
            {
                if (ci <= L->base_ci)
                    luaL_argerror(L, 1, "level out of range");

                --ci;
            }

            if (!ci->func || !ttisfunction(ci->func))
                luaL_argerror(L, 1, "level does not point to a function");

            set_hidden_cl(L, clvalue(ci->func), hidden);
            return 0;
        }

        luaL_typeerror(L, 1, "function or number");
        return 0;
    }

    static int isstackhidden(lua_State* L)
    {
        luaL_checktype(L, 1, LUA_TFUNCTION);
        lua_pushboolean(L, is_hidden_cl(clvalue(index2addr(L, 1))));
        return 1;
    }

    static int gethook(lua_State* L)
    {
        Closure* target = to_closure(L, 1);

        auto it = HookRecords.find(target);
        if (it == HookRecords.end() ||
            it->second.HookRef == LUA_NOREF ||
            !it->second.HookClosure)
        {
            lua_pushnil(L);
            return 1;
        }

        lua_getref(L, it->second.HookRef);

        if (!lua_isfunction(L, -1))
        {
            lua_pop(L, 1);
            lua_pushnil(L);
        }

        return 1;
    }

    static const std::string& lclosure_wrapper_bytecode()
    {
        static std::string bytecode;
        if (bytecode.empty())
            bytecode = Execution::CompileScript("local f = ...\nreturn function(...) return f(...) end");

        return bytecode;
    }

    static int newlclosure(lua_State* L)
    {
        luaL_checktype(L, 1, LUA_TFUNCTION);
        const char* debugname = luaL_optstring(L, 2, nullptr);
        lua_settop(L, 2);

        const std::string& bytecode = lclosure_wrapper_bytecode();
        if (bytecode.empty() || luau_load(L, "=newlclosure", bytecode.data(), bytecode.size(), 0) != LUA_OK)
            luaL_error(L, "newlclosure: failed to build wrapper");

        TaskScheduler::SetProtoCapabilities(clvalue(index2addr(L, -1))->l.p, &MaxCapabilities);

        lua_pushvalue(L, 1);
        lua_call(L, 1, 1);

        if (!lua_isfunction(L, -1) || lua_iscfunction(L, -1))
            luaL_error(L, "newlclosure: failed to build wrapper");

        Closure* wrapper = clvalue(index2addr(L, -1));
        if (debugname && *debugname && wrapper->l.p)
        {
            TString* name = luaS_new(L, debugname);
            wrapper->l.p->debugname.Set(name);
            luaC_objbarrier(L, wrapper->l.p, name);
        }

        return 1;
    }

    static Proto* resolve_proto(lua_State* L, int index)
    {
        if (Proto* proxied = ProtoProxy::To(L, index))
            return proxied;

        luaL_checktype(L, index, LUA_TFUNCTION);
        Closure* closure = clvalue(index2addr(L, index));
        if (closure->isC || !closure->l.p)
            luaL_argerror(L, index, "Luau function or ProtoProxy expected");

        return closure->l.p;
    }

    static std::vector<Closure*> closures_of_proto(lua_State* L, Proto* proto)
    {
        std::vector<Closure*> found;
        global_State* g = L->global;

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
                if (!closure->isC && closure->l.p == proto)
                    found.push_back(closure);
            }
        }
        lua_gc(L, LUA_GCRESTART, 0);

        return found;
    }

    // Compiled once; the tag constant is replaced by the hook after loading, so
    // the grafted code is just `return hook(...)`.
    static constexpr char ProtoHookTag[] = "\0HOOKPROTO_TARGET";

    static const std::string& proto_hook_wrapper_bytecode()
    {
        static std::string bytecode;
        if (bytecode.empty())
            bytecode = Execution::CompileScript("return (\"\\0HOOKPROTO_TARGET\")(...)");

        return bytecode;
    }

    // True when some thread has a frame of `proto` whose pc lies in `code`.
    // Such a frame would resume against a swapped k/lineinfo and crash.
    static bool proto_code_running(lua_State* L, Proto* proto, const unsigned int* code, int sizecode)
    {
        struct Context
        {
            global_State* g;
            Proto* proto;
            const Instruction* begin;
            const Instruction* end;
            bool found;
        } context{ L->global, proto, code, code + sizecode, false };

        luaM_visitgco(L, &context, [](void* ud, lua_Page*, GCObject* object) -> bool {
            auto* ctx = static_cast<Context*>(ud);
            if (ctx->found || object->gch.tt != LUA_TTHREAD || isdead(ctx->g, object))
                return false;

            lua_State* thread = gco2th(object);
            for (CallInfo* ci = thread->base_ci + 1; ci <= thread->ci; ++ci)
            {
                const bool ours = ci->p == ctx->proto ||
                    (ttisfunction(ci->func) && !clvalue(ci->func)->isC && clvalue(ci->func)->l.p == ctx->proto);
                if (ours && ci->savedpc >= ctx->begin && ci->savedpc <= ctx->end)
                {
                    ctx->found = true;
                    break;
                }
            }
            return false;
        });

        return context.found;
    }

    static void raise_closure_stacksize(lua_State* L, Proto* proto, unsigned char size)
    {
        for (Closure* closure : closures_of_proto(L, proto))
        {
            if (closure->stacksize < size)
                closure->stacksize = size;
        }
    }

    static void restore_proto_record(lua_State* L, Proto* p, ProtoHookRecord& record)
    {
        // restoreproto from inside the hook leaves a frame on the wrapper's
        // RETURN; keep the wrapper (and its code array) alive for it.
        const bool wrapperRunning = proto_code_running(L, p, p->code, p->sizecode);

        Proto* w = record.Wrapper;
        const ProtoArrays grafted = read_proto_arrays(p);
        write_proto_arrays(p, read_proto_arrays(w));
        write_proto_arrays(w, grafted);

        p->numparams = record.NumParams;
        p->is_vararg = record.IsVararg;
        p->maxstacksize = record.MaxStackSize;
        p->funid = record.FunId;
        p->execdata = record.ExecData;
        p->exectarget = record.ExecTarget;

        for (int i = 0; i < p->sizek; ++i)
            luaC_barrier(L, p, &p->k[i]);
        for (int i = 0; i < w->sizek; ++i)
            luaC_barrier(L, w, &w->k[i]);

        raise_closure_stacksize(L, p, p->maxstacksize);

        if (record.Global == L->global)
        {
            lua_unref(L, record.HookRef);
            lua_unref(L, record.PinRef);
            if (!wrapperRunning)
                lua_unref(L, record.WrapperRef);
        }
    }

    static int hookproto(lua_State* L)
    {
        Proto* live = resolve_proto(L, 1);
        luaL_checktype(L, 2, LUA_TFUNCTION);
        Closure* hook = clvalue(index2addr(L, 2));
        if (hook->isC)
            luaL_argerror(L, 2, "Luau function expected");
        if (hook->nupvalues != 0)
            luaL_argerror(L, 2, "hook must not have upvalues");
        if (hook->l.p == live)
            luaL_argerror(L, 2, "hook cannot use the hooked proto");

        auto existing = ProtoHooks.find(live);
        const unsigned int* originalCode = existing != ProtoHooks.end() ? existing->second.Wrapper->code : live->code;
        const int originalSize = existing != ProtoHooks.end() ? existing->second.Wrapper->sizecode : live->sizecode;
        if (proto_code_running(L, live, originalCode, originalSize))
            luaL_error(L, "hookproto: proto is currently running on a thread");

        const std::string& bytecode = proto_hook_wrapper_bytecode();
        if (bytecode.empty() || luau_load(L, "=hookproto", bytecode.data(), bytecode.size(), 0) != LUA_OK)
            luaL_error(L, "hookproto: failed to build wrapper");
        const int wrapperIndex = lua_gettop(L);
        Proto* w = clvalue(index2addr(L, wrapperIndex))->l.p;

        bool patched = false;
        for (int i = 0; i < w->sizek && !patched; ++i)
        {
            if (!ttisstring(&w->k[i]))
                continue;
            const TString* ts = tsvalue(&w->k[i]);
            if (ts->len == sizeof(ProtoHookTag) - 1 && memcmp(getstr(ts), ProtoHookTag, ts->len) == 0)
            {
                setclvalue(L, &w->k[i], hook);
                luaC_objbarrier(L, w, hook);
                patched = true;
            }
        }
        if (!patched)
            luaL_error(L, "hookproto: failed to build wrapper");

        // Every wrapper instruction reports the hooked function's own line.
        if (w->lineinfo.Get() && w->sizecode > 0)
        {
            int* abs = w->abslineinfo.Get();
            unsigned char* lines = w->lineinfo.Get();
            for (int i = 0, n = ((w->sizecode - 1) >> w->linegaplog2) + 1; i < n; ++i)
                abs[i] = live->linedefined;
            for (int pc = 0; pc < w->sizecode; ++pc)
                lines[pc] = 0;
        }

        if (existing != ProtoHooks.end())
        {
            restore_proto_record(L, live, existing->second);
            ProtoHooks.erase(existing);
        }

        ProtoHookRecord record;
        record.Global = L->global;
        record.Wrapper = w;
        record.NumParams = live->numparams;
        record.IsVararg = live->is_vararg;
        record.MaxStackSize = live->maxstacksize;
        record.FunId = live->funid;
        record.ExecData = live->execdata;
        record.ExecTarget = live->exectarget;

        const std::vector<Closure*> owners = closures_of_proto(L, live);
        push_closure(L, owners.empty() ? luaF_newLclosure(L, live->nups, L->gt, live) : owners.front());
        record.PinRef = lua_ref(L, -1);
        lua_pop(L, 1);

        record.HookRef = lua_ref(L, 2);
        record.WrapperRef = lua_ref(L, wrapperIndex);

        const ProtoArrays original = read_proto_arrays(live);
        write_proto_arrays(live, read_proto_arrays(w));
        write_proto_arrays(w, original);
        for (int i = 0; i < w->sizek; ++i)
            luaC_barrier(L, w, &w->k[i]);

        live->numparams = w->numparams;
        live->is_vararg = w->is_vararg;
        live->maxstacksize = w->maxstacksize;
        // A fresh id fails the CMPPROTO guard of call sites that inlined the
        // original body, so they fall back to a real call into the hook.
        live->funid = L->global->lastprotoid++;
        // Interpret the grafted code; native code would still be the original.
        live->execdata = nullptr;
        live->exectarget = 0;
        luaC_objbarrier(L, live, hook);

        raise_closure_stacksize(L, live, live->maxstacksize);

        ProtoHooks[live] = record;
        lua_settop(L, 2);
        return 0;
    }

    static int restoreproto(lua_State* L)
    {
        Proto* proto = resolve_proto(L, 1);
        auto it = ProtoHooks.find(proto);
        if (it == ProtoHooks.end())
            luaL_error(L, "restoreproto: proto is not hooked");

        restore_proto_record(L, proto, it->second);
        ProtoHooks.erase(it);
        return 0;
    }

    static int isprotohooked(lua_State* L)
    {
        Proto* proto = resolve_proto(L, 1);
        lua_pushboolean(L, ProtoHooks.find(proto) != ProtoHooks.end());
        return 1;
    }

    void RegisterLibrary(lua_State* L)
    {
        lua_pushcclosurek(L, Closures::HookLuaDispatcher, LuaHookDispatcherName, 0, DispatchContinuation);
        SharedVariables::SetHiddenClosure(clvalue(index2addr(L, -1)), true);
        lua_setglobal(L, LuaHookDispatcherName);

        Utils::AddFunction(L, "loadstring", Closures::loadstring);
        Utils::AddFunction(L, "dumpbytecode", Closures::dumpbytecode);
        Utils::AddFunction(L, "runbytecode", Closures::runbytecode);
        Utils::AddFunction(L, "hookfunction", Closures::hookfunction);
        Utils::AddFunction(L, "hookfunc", Closures::hookfunction);
        Utils::AddFunction(L, "replaceclosure", Closures::hookfunction);
        Utils::AddFunction(L, "newcclosure", Closures::newcclosure);
        Utils::AddFunction(L, "isnewcclosure", Closures::isnewcclosure);
        Utils::AddFunction(L, "iscustomcclosure", Closures::isnewcclosure);
        Utils::AddFunction(L, "newlclosure", Closures::newlclosure);
        Utils::AddFunction(L, "hookproto", Closures::hookproto);
        Utils::AddFunction(L, "restoreproto", Closures::restoreproto);
        Utils::AddFunction(L, "isprotohooked", Closures::isprotohooked);
        Utils::AddFunction(L, "restorefunction", Closures::restorefunction);
        Utils::AddFunction(L, "restorefunc", Closures::restorefunction);
        Utils::AddFunction(L, "clonefunction", Closures::clonefunction);
        Utils::AddFunction(L, "iscclosure", Closures::iscclosure);
        Utils::AddFunction(L, "islclosure", Closures::islclosure);

        Utils::AddFunction(L, "isourclosure", Closures::is_executor_closure);
        Utils::AddFunction(L, "checkclosure", Closures::is_executor_closure);
        Utils::AddFunction(L, "isexecutorclosure", Closures::is_executor_closure);
        Utils::AddFunction(L, "isexecutorfunction", Closures::is_executor_closure);
        Utils::AddFunction(L, "cloneref", Closures::cloneref);
        Utils::AddFunction(L, "clonereference", Closures::cloneref);
        Utils::AddFunction(L, "gettenv", gettenv);
        Utils::AddFunction(L, "isfunctionhooked", isfunctionhooked);
        Utils::AddFunction(L, "gethook", gethook);
        Utils::AddFunction(L, "setstackhidden", setstackhidden);
        Utils::AddFunction(L, "set_stack_hidden", setstackhidden);
        Utils::AddFunction(L, "sethiddenstack", setstackhidden);
        Utils::AddFunction(L, "isstackhidden", isstackhidden);
        Utils::AddFunction(L, "is_stack_hidden", isstackhidden);
        Utils::AddFunction(L, "ishiddenstack", isstackhidden);
    }
}
