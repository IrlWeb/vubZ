#pragma once

#include <lstate.h>
#include <lualib.h>
#include <vector>
#include <string>
#include <Scripting/Environment.hpp>
#include <Scripting/Bridge/SafeCall.hpp>
#include <Core/Context.hpp>
#include <zstd/xxhash.h>
#include <zstd/zstd.h>

namespace Utils
{
	inline void TrackExecutorClosure(Closure* ClosureObject)
	{
		if (!ClosureObject)
			return;

		if (SharedVariables::ExecutorClosures.insert(ClosureObject).second)
			Environment::function_array.push_back(ClosureObject);
	}

	inline void PushGuardedFunction(lua_State* L, const char* Name, lua_CFunction Function)
	{
		lua_pushlightuserdata(L, reinterpret_cast<void*>(Function));
		lua_pushlightuserdata(L, const_cast<char*>(Name));
		lua_pushcclosure(L, SafeCall::Guard, Name, 2);
		TrackExecutorClosure(clvalue(index2addr(L, -1)));
	}

	inline void AddFunction(lua_State* L, const char* Name, lua_CFunction Function)
	{
		PushGuardedFunction(L, Name, Function);
		lua_setglobal(L, Name);
	}

	inline void AddTableFunction(lua_State* L, const char* Name, lua_CFunction Function)
	{
		PushGuardedFunction(L, Name, Function);
		lua_setfield(L, -2, Name);
	}

	inline void RegisterAliases(lua_State* L, lua_CFunction Function, const std::vector<const char*>& Aliases)
	{
		for (const char* alias : Aliases)
		{
			PushGuardedFunction(L, alias, Function);
			lua_setglobal(L, alias);
		}
	}

	inline bool IsInGame(uintptr_t DataModel)
	{
		if (!DataModel)
			return false;

		__try
		{
			if (*reinterpret_cast<const bool*>(DataModel + Offsets::DataModel::GameLoaded))
				return true;

			const int64_t placeId = *reinterpret_cast<const int64_t*>(DataModel + Offsets::DataModel::PlaceId);
			if (placeId > 0)
			{
				uintptr_t children = *reinterpret_cast<const uintptr_t*>(DataModel + Offsets::Instance::Children);
				if (children != 0 && *reinterpret_cast<const uintptr_t*>(children) != 0)
					return true;
			}

			if (*reinterpret_cast<const bool*>(DataModel + 0x630) ||
			    *reinterpret_cast<const bool*>(DataModel + 0x5D8) ||
			    *reinterpret_cast<const bool*>(DataModel + 0x5C0))
				return true;

			return false;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	inline std::string decompress_bytecode(const std::string& source)
	{
		constexpr const char bytecode_magic[] = "RSB1";

		std::string input = source;

		std::uint8_t hash_bytes[4];
		std::memcpy(hash_bytes, &input[0], 4);

		for (auto i = 0u; i < 4; ++i) {
			hash_bytes[i] ^= bytecode_magic[i];
			hash_bytes[i] -= i * 41;
		}

		for (size_t i = 0; i < input.size(); ++i)
			input[i] ^= hash_bytes[i % 4] + i * 41;

		XXH32(&input[0], input.size(), 42);

		std::uint32_t data_size;
		std::memcpy(&data_size, &input[4], 4);

		std::vector<std::uint8_t> data(data_size);
		const size_t result = ZSTD_decompress(&data[0], data_size, &input[8], input.size() - 8);
		if (ZSTD_isError(result))
			return {};

		return std::string(reinterpret_cast<char*>(&data[0]), result);
	}
}
