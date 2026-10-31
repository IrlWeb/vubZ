#pragma once

#include <Windows.h>
#include <malloc.h>

#include <lstate.h>
#include <lualib.h>
#include <lapi.h>

#include <Core/CallTrace.hpp>

namespace SafeCall
{
    inline int FaultFilter(unsigned long Code) noexcept
    {
        if (Code == 0xE06D7363)
            return EXCEPTION_CONTINUE_SEARCH;

        if (Code == 0x40010006 || Code == 0x406D1388)
            return EXCEPTION_CONTINUE_SEARCH;

        if ((Code & 0xC0000000) == 0xC0000000)
            return EXCEPTION_EXECUTE_HANDLER;

        if (Code == 0x80000001 || Code == 0x80000002)
            return EXCEPTION_EXECUTE_HANDLER;

        return EXCEPTION_CONTINUE_SEARCH;
    }

    inline const char* FaultName(unsigned long Code) noexcept
    {
        switch (Code)
        {
        case EXCEPTION_ACCESS_VIOLATION:     return "access violation";
        case EXCEPTION_IN_PAGE_ERROR:        return "in-page error";
        case EXCEPTION_DATATYPE_MISALIGNMENT:return "misaligned access";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:return "bounds check";
        case EXCEPTION_ILLEGAL_INSTRUCTION:  return "illegal instruction";
        case EXCEPTION_PRIV_INSTRUCTION:     return "privileged instruction";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:   return "integer divide by zero";
        case EXCEPTION_INT_OVERFLOW:         return "integer overflow";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:   return "float divide by zero";
        case EXCEPTION_STACK_OVERFLOW:       return "stack overflow";
        case 0x80000001:                     return "guard page violation";
        case EXCEPTION_INVALID_HANDLE:       return "invalid handle";
        default:                             return "hardware fault";
        }
    }

    inline int Invoke(lua_CFunction Function, lua_State* L, unsigned long* FaultCode = nullptr)
    {
        __try
        {
            return Function(L);
        }
        __except (FaultFilter(GetExceptionCode()))
        {
            if (FaultCode)
                *FaultCode = GetExceptionCode();
            return 0;
        }
    }

    typedef void (*VoidAction)(void* context);
    inline bool InvokeVoid(VoidAction Action, void* Context, unsigned long* FaultCode = nullptr)
    {
        __try
        {
            Action(Context);
            return true;
        }
        __except (FaultFilter(GetExceptionCode()))
        {
            if (FaultCode)
                *FaultCode = GetExceptionCode();
            return false;
        }
    }

    inline int Guard(lua_State* L)
    {
        void* Target = lua_tolightuserdata(L, lua_upvalueindex(1));
        if (!Target)
            luaL_error(L, "environment function is unavailable");

        const char* Name = static_cast<const char*>(
            lua_tolightuserdata(L, lua_upvalueindex(2)));

        const int initialTop = lua_gettop(L);
        unsigned long Fault = 0;

        const int depth = static_cast<int>(L->nCcalls);
        CallTrace::Enter(Name ? Name : "environment function", nullptr, depth);

        const int Results = Invoke(reinterpret_cast<lua_CFunction>(Target), L, &Fault);

        CallTrace::Leave(Name ? Name : "environment function", depth);

        if (Fault != 0)
        {
            if (Fault == EXCEPTION_STACK_OVERFLOW)
                _resetstkoflw();

            lua_settop(L, initialTop);

            luaL_error(L, "%s: %s (0x%08X)",
                Name ? Name : "environment function",
                FaultName(Fault),
                static_cast<unsigned int>(Fault));
        }

        return Results;
    }
}
