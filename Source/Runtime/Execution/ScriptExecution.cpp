#include <Runtime/Execution/ExecutionGuard.hpp>
#include <Core/Context.hpp>

struct lua_State;
namespace Miscellaneous { void EnforceIdentityGlobals(lua_State* L); }
#include <Runtime/Execution/ScriptExecution.hpp>
#include <Runtime/Scheduler/TaskScheduler.hpp>
#include <Scripting/Obfuscation.hpp>

#include <cstring>
#include <cstdio>
#include <stdexcept>

namespace
{
	void ReportExecutionError(const char* Stage, const char* Message)
	{
		const char* Text = Message && Message[0] ? Message : SUPQ2_OBF("unknown error");
		// Roblox::Print(3, SUPQ2_OBF("[supQ] %s: %s"), Stage, Text);

		char Buffer[1024]{};
		std::snprintf(Buffer, sizeof(Buffer), SUPQ2_OBF("[supQ] %s: %s\n"), Stage, Text);
		OutputDebugStringA(Buffer);
	}
}

class BytecodeEncoder : public Luau::BytecodeEncoder
{
	inline void encode(uint32_t* data, size_t count) override
	{
		const auto LookupTable = reinterpret_cast<BYTE*>(Offsets::OpcodeLookupTable);
		if (!data || !LookupTable)
			return;

		for (size_t i = 0; i < count;)
		{
			const uint8_t Opcode = LUAU_INSN_OP(data[i]);

			const int Length = Luau::getOpLength(static_cast<LuauOpcode>(Opcode));
			if (Length <= 0 || i + static_cast<size_t>(Length) > count)
				return;

			const uint8_t FinalOpcode = LookupTable[static_cast<uint8_t>(Opcode * 227)];
			data[i] = FinalOpcode | (data[i] & ~0xFF);

			i += static_cast<size_t>(Length);
		}
	}
};

namespace
{
	bool IsIdentifierChar(char c)
	{
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
	}

	bool IsSpaceChar(char c)
	{
		return c == ' ' || c == '\t' || c == '\r' || c == '\n';
	}

	size_t SkipSpaces(const std::string& source, size_t cursor)
	{
		while (cursor < source.size() && IsSpaceChar(source[cursor]))
			++cursor;
		return cursor;
	}

	void AppendGameArgument(const std::string& source, size_t cursor, std::string& output)
	{
		const size_t firstArg = SkipSpaces(source, cursor + 1);
		if (firstArg < source.size() && source[firstArg] == ')')
			output.append("(game");
		else
			output.append("(game, ");
	}

	bool MatchLongBracketOpen(const std::string& source, size_t position, size_t& equalsCount)
	{
		if (position >= source.size() || source[position] != '[')
			return false;

		size_t cursor = position + 1;
		while (cursor < source.size() && source[cursor] == '=')
			++cursor;

		if (cursor < source.size() && source[cursor] == '[')
		{
			equalsCount = cursor - position - 1;
			return true;
		}

		return false;
	}

	size_t FindLongBracketClose(const std::string& source, size_t position, size_t equalsCount)
	{
		std::string close = "]";
		close.append(equalsCount, '=');
		close.push_back(']');

		const size_t found = source.find(close, position);
		return found == std::string::npos ? source.size() : found + close.size();
	}

	bool TryRewriteGameMethod(const std::string& source, size_t& position, std::string& output, const char* method, const char* replacement, bool passGame)
	{
		const std::string pattern = std::string("game:") + method;
		const size_t length = pattern.size();

		if (source.compare(position, length, pattern) != 0)
			return false;

		if (position > 0 && IsIdentifierChar(source[position - 1]))
			return false;

		size_t cursor = position + length;
		if (cursor < source.size() && IsIdentifierChar(source[cursor]))
			return false;

		cursor = SkipSpaces(source, cursor);

		if (cursor >= source.size() || source[cursor] != '(')
			return false;

		output.append(replacement);

		if (passGame)
		{
			output.append(source, position + length, cursor - (position + length));
			AppendGameArgument(source, cursor, output);
			position = cursor + 1;
		}
		else
		{
			position += length;
		}

		return true;
	}

	bool TryRewriteGameDotCall(const std::string& source, size_t& position, std::string& output, const char* method, const char* replacement, bool passGame)
	{
		const std::string pattern = std::string("game.") + method;
		const size_t length = pattern.size();

		if (source.compare(position, length, pattern) != 0)
			return false;

		if (position > 0 && IsIdentifierChar(source[position - 1]))
			return false;

		size_t cursor = position + length;
		if (cursor < source.size() && IsIdentifierChar(source[cursor]))
			return false;

		cursor = SkipSpaces(source, cursor);

		if (cursor >= source.size() || source[cursor] != '(')
			return false;

		output.append(replacement);

		if (passGame)
		{
			output.append(source, position + length, cursor - (position + length));
			AppendGameArgument(source, cursor, output);
			position = cursor + 1;
		}
		else
		{
			position += length;
		}

		return true;
	}

	bool TryRewriteGameMemberCall(const std::string& source, size_t& position, std::string& output, const char* method, const char* replacement)
	{
		const bool lowerGame = source.compare(position, 4, "game") == 0;
		const bool upperGame = source.compare(position, 4, "Game") == 0;
		if (!lowerGame && !upperGame)
			return false;

		if (position > 0 && IsIdentifierChar(source[position - 1]))
			return false;

		size_t cursor = position + 4;
		if (cursor < source.size() && IsIdentifierChar(source[cursor]))
			return false;

		cursor = SkipSpaces(source, cursor);

		if (cursor >= source.size() || (source[cursor] != ':' && source[cursor] != '.'))
			return false;

		const char separator = source[cursor++];
		cursor = SkipSpaces(source, cursor);

		const size_t methodLength = strlen(method);
		if (source.compare(cursor, methodLength, method) != 0)
			return false;

		const size_t methodEnd = cursor + methodLength;
		if (methodEnd < source.size() && IsIdentifierChar(source[methodEnd]))
			return false;

		cursor = SkipSpaces(source, methodEnd);

		if (cursor >= source.size() || source[cursor] != '(')
			return false;

		output.append(replacement);

		if (strcmp(method, "GetObjects") == 0 && separator == ':')
		{
			output.append(source, methodEnd, cursor - methodEnd);
			AppendGameArgument(source, cursor, output);
			position = cursor + 1;
		}
		else
		{
			position = methodEnd;
		}

		return true;
	}

	std::string NormalizeExecutorSource(const std::string& source)
	{
		std::string output;
		output.reserve(source.size());

		for (size_t i = 0; i < source.size();)
		{
			if (TryRewriteGameMemberCall(source, i, output, "HttpGetAsync", "HttpGet") ||
				TryRewriteGameMemberCall(source, i, output, "HttpGet", "HttpGet") ||
				TryRewriteGameMemberCall(source, i, output, "GetObjects", "GetObjects") ||
				TryRewriteGameMethod(source, i, output, "HttpGetAsync", "HttpGet", false) ||
				TryRewriteGameMethod(source, i, output, "HttpGet", "HttpGet", false) ||
				TryRewriteGameMethod(source, i, output, "GetObjects", "GetObjects", true) ||
				TryRewriteGameDotCall(source, i, output, "HttpGetAsync", "HttpGet", false) ||
				TryRewriteGameDotCall(source, i, output, "HttpGet", "HttpGet", false) ||
				TryRewriteGameDotCall(source, i, output, "GetObjects", "GetObjects", true))
			{
				continue;
			}

			const char c = source[i];

			if (c == '-' && i + 1 < source.size() && source[i + 1] == '-')
			{
				size_t equalsCount = 0;
				if (i + 2 < source.size() && MatchLongBracketOpen(source, i + 2, equalsCount))
				{
					const size_t end = FindLongBracketClose(source, i + 2, equalsCount);
					output.append(source, i, end - i);
					i = end;
					continue;
				}

				const size_t end = source.find('\n', i + 2);
				const size_t count = (end == std::string::npos ? source.size() : end) - i;
				output.append(source, i, count);
				i += count;
				continue;
			}

			if (c == '\'' || c == '"' || c == '`')
			{
				const char quote = c;

				const bool multiline = quote == '`';

				const size_t start = i++;
				bool escaped = false;

				while (i < source.size())
				{
					const char current = source[i];
					if (!multiline && current == '\n' && !escaped)
						break;

					++i;

					if (escaped)
					{
						escaped = false;
						continue;
					}
					if (current == '\\')
					{
						escaped = true;
						continue;
					}
					if (current == quote)
						break;
				}

				output.append(source, start, i - start);
				continue;
			}

			if (c == '[')
			{
				size_t equalsCount = 0;
				if (MatchLongBracketOpen(source, i, equalsCount))
				{
					const size_t end = FindLongBracketClose(source, i, equalsCount);
					output.append(source, i, end - i);
					i = end;
					continue;
				}
			}

			output.push_back(c);
			++i;
		}

		return output;
	}
}

std::string Execution::CompileScript(std::string Source)
{
	if (!Offsets::OpcodeLookupTable)
	{
		ReportExecutionError(SUPQ2_OBF("compile"), SUPQ2_OBF("opcode lookup table is unavailable"));
		return {};
	}

	if (Source.size() >= 3 &&
		static_cast<unsigned char>(Source[0]) == 0xEF &&
		static_cast<unsigned char>(Source[1]) == 0xBB &&
		static_cast<unsigned char>(Source[2]) == 0xBF)
	{
		Source.erase(0, 3);
	}

	Source = NormalizeExecutorSource(Source);

	auto BytecodeEncoding = BytecodeEncoder();
	static const char* CommonGlobals[] = { "Game", "Workspace", "game", "plugin", "script", "shared", "workspace", "_G", "_ENV", nullptr };

	Luau::CompileOptions Options;
	Options.debugLevel = 1;
	Options.optimizationLevel = 1;
	Options.mutableGlobals = CommonGlobals;
	Options.vectorLib = "Vector3";
	Options.vectorCtor = "new";
	Options.vectorType = "Vector3";

	return Luau::compile(Source, Options, {}, &BytecodeEncoding);
}

std::string Execution::DefaultChunkName(const char* Source, size_t Length)
{
	size_t Line = 0;
	while (Line < Length && Line < LUA_IDSIZE &&
		Source[Line] != '\n' && Source[Line] != '\r' && Source[Line] != '\0')
	{
		++Line;
	}

	std::string Name(Source, Line);
	if (Line < Length)
		Name.push_back('\n');
	return Name;
}

std::string Execution::PrepareScript(std::string Script)
{
	try
	{
		std::string Bytecode = Execution::CompileScript(std::move(Script));
		if (Bytecode.empty())
			ReportExecutionError(SUPQ2_OBF("compile"), SUPQ2_OBF("compiler produced no bytecode"));
		return Bytecode;
	}
	catch (const std::exception& Error)
	{
		ReportExecutionError(SUPQ2_OBF("compile"), Error.what());
	}
	catch (...)
	{
		ReportExecutionError(SUPQ2_OBF("compile"), SUPQ2_OBF("unrecognized compiler failure"));
	}

	return {};
}

void Execution::ExecuteScript(lua_State* L, std::string Script)
{
	Execution::ExecuteBytecode(L, Execution::PrepareScript(std::move(Script)));
}

void Execution::ExecuteBytecode(lua_State* L, const std::string& Bytecode)
{
	if (!L || Bytecode.empty())
		return;

	const int OriginalTop = lua_gettop(L);
	lua_State* ExecutionThread = lua_newthread(L);
	if (!ExecutionThread)
	{
		lua_settop(L, OriginalTop);
		ReportExecutionError(SUPQ2_OBF("execute"), SUPQ2_OBF("failed to create thread"));
		return;
	}

	const int ThreadRef = lua_ref(L, -1);
	lua_pop(L, 1);
	const auto Cleanup = [&]()
	{
		lua_unref(L, ThreadRef);
		lua_settop(L, OriginalTop);
	};

	luaL_sandboxthread(ExecutionThread);
	if (SharedVariables::ExecutorFastcallsDisabled)
		lua_setsafeenv(ExecutionThread, LUA_GLOBALSINDEX, false);
	TaskScheduler::SetThreadCapabilities(ExecutionThread, 8, MaxCapabilities);

	if (luau_load(ExecutionThread, "=main", Bytecode.c_str(), Bytecode.length(), 0) != LUA_OK)
	{
		const char* Error = lua_tostring(ExecutionThread, -1);
		ReportExecutionError(SUPQ2_OBF("load"), Error);
		lua_pop(ExecutionThread, 1);
		Cleanup();
		return;
	}

	Closure* Closure = clvalue(luaA_toobject(ExecutionThread, -1));
	if (!Closure || Closure->isC || !Closure->l.p)
	{
		ReportExecutionError(SUPQ2_OBF("load"), SUPQ2_OBF("loaded chunk is not a Luau closure"));
		Cleanup();
		return;
	}
	TaskScheduler::SetProtoCapabilities(Closure->l.p, &MaxCapabilities);

	Miscellaneous::EnforceIdentityGlobals(SharedVariables::ExploitThread);

	lua_getglobal(ExecutionThread, "task");
	if (!lua_istable(ExecutionThread, -1))
	{
		ReportExecutionError(SUPQ2_OBF("schedule"), SUPQ2_OBF("global 'task' is unavailable"));
		lua_pop(ExecutionThread, 1);
		Cleanup();
		return;
	}

	lua_getfield(ExecutionThread, -1, "defer");
	if (!lua_isfunction(ExecutionThread, -1))
	{
		ReportExecutionError(SUPQ2_OBF("schedule"), SUPQ2_OBF("task.defer is unavailable"));
		lua_pop(ExecutionThread, 2);
		Cleanup();
		return;
	}

	lua_remove(ExecutionThread, -2);
	lua_insert(ExecutionThread, -2);
	if (lua_pcall(ExecutionThread, 1, 0, 0) != LUA_OK)
	{
		const char* Error = lua_tostring(ExecutionThread, -1);
		ReportExecutionError(SUPQ2_OBF("task.defer"), Error);
		lua_pop(ExecutionThread, 1);
		Cleanup();
		return;
	}

	Cleanup();
}
