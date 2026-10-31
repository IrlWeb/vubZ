#pragma once

#include <ldo.h>
#include <string>
#include <lualib.h>
#include <lstate.h>
#include <Luau/Compiler.h>
#include <Luau/BytecodeUtils.h>
#include <Luau/BytecodeBuilder.h>

void ElevateThread(lua_State* L, uint64_t identity, bool isInstant);

namespace Execution
{
	std::string CompileScript(std::string Source);
	std::string PrepareScript(std::string Script);
	std::string DefaultChunkName(const char* Source, size_t Length);
	void ExecuteScript(lua_State* L, std::string Script);
	void ExecuteBytecode(lua_State* L, const std::string& Bytecode);
}

