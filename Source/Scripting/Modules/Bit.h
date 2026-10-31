#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>

#include <lapi.h>
#include <lualib.h>

#include <Core/Utilities.hpp>

namespace Bit
{
    inline bool touint(lua_State* L, int index, uint32_t& out)
    {
        if (lua_type(L, index) != LUA_TNUMBER && !lua_isnumber(L, index))
            return false;

        const double value = lua_tonumber(L, index);
        if (!std::isfinite(value) || std::fabs(value) >= 9.2e18)
            return false;

        out = static_cast<uint32_t>(static_cast<int64_t>(std::trunc(value)));
        return true;
    }

    inline uint32_t checkuint(lua_State* L, int index)
    {
        uint32_t value = 0;
        if (!touint(L, index, value))
            luaL_typeerrorL(L, index, "number");
        return value;
    }

    inline int pushuint(lua_State* L, uint32_t value)
    {
        lua_pushnumber(L, static_cast<double>(value));
        return 1;
    }

    template <typename Op>
    inline int fold(lua_State* L, Op op)
    {
        const int count = lua_gettop(L);
        bool first = true;
        uint32_t result = 0;

        for (int i = 1; i <= count; ++i)
        {
            uint32_t value = 0;
            if (!touint(L, i, value))
                continue;

            result = first ? value : op(L, result, value);
            first = false;
        }

        return pushuint(L, result);
    }

    inline int badd(lua_State* L)
    {
        return fold(L, [](lua_State*, uint32_t a, uint32_t b) { return a + b; });
    }

    inline int bsub(lua_State* L)
    {
        return fold(L, [](lua_State*, uint32_t a, uint32_t b) { return a - b; });
    }

    inline int bmul(lua_State* L)
    {
        return fold(L, [](lua_State*, uint32_t a, uint32_t b) { return a * b; });
    }

    inline int bdiv(lua_State* L)
    {
        return fold(L, [](lua_State* L, uint32_t a, uint32_t b) {
            if (b == 0)
                luaL_error(L, "division by zero");
            return a / b;
        });
    }

    inline int band(lua_State* L)
    {
        return fold(L, [](lua_State*, uint32_t a, uint32_t b) { return a & b; });
    }

    inline int bor(lua_State* L)
    {
        return fold(L, [](lua_State*, uint32_t a, uint32_t b) { return a | b; });
    }

    inline int bxor(lua_State* L)
    {
        return fold(L, [](lua_State*, uint32_t a, uint32_t b) { return a ^ b; });
    }

    inline int bnot(lua_State* L)
    {
        return pushuint(L, ~checkuint(L, 1));
    }

    inline int bswap(lua_State* L)
    {
        const uint32_t value = checkuint(L, 1);

        return pushuint(L,
            ((value & 0x000000FFu) << 24) |
            ((value & 0x0000FF00u) << 8) |
            ((value & 0x00FF0000u) >> 8) |
            ((value & 0xFF000000u) >> 24));
    }

    inline int ror(lua_State* L)
    {
        const uint32_t value = checkuint(L, 1);
        const uint32_t shift = checkuint(L, 2) & 31u;

        return pushuint(L, shift == 0 ? value : (value >> shift) | (value << (32u - shift)));
    }

    inline int rol(lua_State* L)
    {
        const uint32_t value = checkuint(L, 1);
        const uint32_t shift = checkuint(L, 2) & 31u;

        return pushuint(L, shift == 0 ? value : (value << shift) | (value >> (32u - shift)));
    }

    inline int lshift(lua_State* L)
    {
        const uint32_t value = checkuint(L, 1);
        const uint32_t shift = checkuint(L, 2);

        return pushuint(L, shift >= 32 ? 0 : value << shift);
    }

    inline int rshift(lua_State* L)
    {
        const uint32_t value = checkuint(L, 1);
        const uint32_t shift = checkuint(L, 2);

        return pushuint(L, shift >= 32 ? 0 : value >> shift);
    }

    inline int arshift(lua_State* L)
    {
        const int32_t value = static_cast<int32_t>(checkuint(L, 1));
        const uint32_t shift = checkuint(L, 2);

        const int32_t result = shift >= 32 ? (value < 0 ? -1 : 0) : value >> shift;
        return pushuint(L, static_cast<uint32_t>(result));
    }

    inline int tohex(lua_State* L)
    {
        char buffer[9];
        std::snprintf(buffer, sizeof(buffer), "%x", static_cast<unsigned int>(checkuint(L, 1)));

        lua_pushstring(L, buffer);
        return 1;
    }

    inline int tobit(lua_State* L)
    {
        return pushuint(L, checkuint(L, 1));
    }

    inline void RegisterLibrary(lua_State* L)
    {
        lua_newtable(L);
        lua_setreadonly(L, -1, false);

        Utils::AddTableFunction(L, "bdiv", bdiv);
        Utils::AddTableFunction(L, "badd", badd);
        Utils::AddTableFunction(L, "bsub", bsub);
        Utils::AddTableFunction(L, "band", band);
        Utils::AddTableFunction(L, "bor", bor);
        Utils::AddTableFunction(L, "bxor", bxor);
        Utils::AddTableFunction(L, "bnot", bnot);
        Utils::AddTableFunction(L, "bmul", bmul);
        Utils::AddTableFunction(L, "bswap", bswap);
        Utils::AddTableFunction(L, "ror", ror);
        Utils::AddTableFunction(L, "rol", rol);
        Utils::AddTableFunction(L, "tohex", tohex);
        Utils::AddTableFunction(L, "tobit", tobit);
        Utils::AddTableFunction(L, "lshift", lshift);
        Utils::AddTableFunction(L, "rshift", rshift);
        Utils::AddTableFunction(L, "arshift", arshift);

        lua_setreadonly(L, -1, true);
        lua_setglobal(L, "bit");
    }
}
