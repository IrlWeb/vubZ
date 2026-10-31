#pragma once

#include <string>
#include <vector>
#include <lstate.h>
#include <lualib.h>
#include <luacode.h>

namespace Environment
{
    void SetupEnvironment(lua_State* L);
    void TeardownEnvironment();
    extern std::vector<Closure*> function_array;
}
