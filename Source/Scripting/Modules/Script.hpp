#pragma once

#include <Windows.h>
#include <lstate.h>
#include <lgc.h>
#include <lualib.h>
#include <string>
#include <cstring>
#include <sstream>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <bitset>
#include <vector>
#include <cmath>
#include <wininet.h>
#include <urlmon.h>
#include <cpr/cpr.h>
#include <luacode.h>
#include <winhttp.h>
#include <zstd/xxhash.h>
#include <zstd/zstd.h>
#include "Scripting/Modules/Closures.hpp"
#include "Scripting/Modules/Crypt.hpp"
#include "Scripting/Modules/FunctionBytecode.hpp"
#include <Luau/BytecodeUtils.h>
#include <Luau/Compiler.h>

#pragma comment(lib, "zstd_static.lib")

#include <Core/DebugLog.hpp>
#include <Core/Utilities.hpp>
#include <Core/CallTrace.hpp>
#include <Core/Context.hpp> 
#include <Scripting/Coroutines/Yielding.hpp> 
#include "Update/Offsets.hpp"
#include <Runtime/Scheduler/TaskScheduler.hpp>
#include <Dependencies/Luau/VM/src/lmem.h>
#include "Scripting/Modules/Closures.hpp"
#include "Scripting/Modules/Debug.hpp"
#include <ltable.h>

#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "urlmon.lib")
#pragma comment(lib, "winhttp.lib")

static std::unordered_map<global_State*, LuaTable*> RenvCache;
static std::uint64_t RenvCacheGeneration = ~0ull;

namespace Console
{
    int RedirectedOutput(lua_State* L);
}

typedef void(__fastcall* PushInstanceWeakPtrT)(lua_State* L, std::weak_ptr<uintptr_t>);

namespace HelperFuncs
{
    __forceinline bool CheckMemory(uintptr_t address) {
        if (address < 0x10000 || address > 0x7FFFFFFFFFFF) {
            return false;
        }

        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)) == 0) {
            return false;
        }

        if (mbi.Protect & PAGE_NOACCESS || mbi.State != MEM_COMMIT) {
            return false;
        }

        return true;
    }
}

template<typename T>
inline bool read_value(std::uintptr_t address, T& output)
{
    output = T{};

    if (!address || address > (std::numeric_limits<std::uintptr_t>::max)() - (sizeof(T) - 1))
        return false;

    if (!HelperFuncs::CheckMemory(address) || !HelperFuncs::CheckMemory(address + sizeof(T) - 1))
        return false;

    __try
    {
        output = *reinterpret_cast<const T*>(address);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        output = T{};
        return false;
    }
}

namespace Script
{
    inline int ScriptError(lua_State* L, const char* message)
    {
        const char* error = message ? message : "script error";
        int top = lua_gettop(L);
        lua_getglobal(L, "error");
        if (lua_isfunction(L, -1))
        {
            lua_pushstring(L, error);
            lua_call(L, 1, 0);
            return 0;
        }

        lua_settop(L, top);
        lua_pushnil(L);
        lua_pushstring(L, error);
        return 2;
    }

    inline uint32_t Rotl32(uint32_t value, int shift)
    {
        return (value << shift) | (value >> (32 - shift));
    }

    inline uint32_t XXHash32(const uint8_t* data, size_t len, uint32_t seed)
    {
        constexpr uint32_t Prime1 = 2654435761U;
        constexpr uint32_t Prime2 = 2246822519U;
        constexpr uint32_t Prime3 = 3266489917U;
        constexpr uint32_t Prime4 = 668265263U;
        constexpr uint32_t Prime5 = 374761393U;

        const uint8_t* ptr = data;
        const uint8_t* end = data + len;
        uint32_t h32 = 0;

        auto read32 = [](const uint8_t* p) -> uint32_t {
            return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
            };

        if (len >= 16)
        {
            const uint8_t* limit = end - 16;
            uint32_t v1 = seed + Prime1 + Prime2;
            uint32_t v2 = seed + Prime2;
            uint32_t v3 = seed;
            uint32_t v4 = seed - Prime1;

            do
            {
                v1 = Rotl32(v1 + read32(ptr) * Prime2, 13) * Prime1; ptr += 4;
                v2 = Rotl32(v2 + read32(ptr) * Prime2, 13) * Prime1; ptr += 4;
                v3 = Rotl32(v3 + read32(ptr) * Prime2, 13) * Prime1; ptr += 4;
                v4 = Rotl32(v4 + read32(ptr) * Prime2, 13) * Prime1; ptr += 4;
            } while (ptr <= limit);

            h32 = Rotl32(v1, 1) + Rotl32(v2, 7) + Rotl32(v3, 12) + Rotl32(v4, 18);
        }
        else
        {
            h32 = seed + Prime5;
        }

        h32 += static_cast<uint32_t>(len);

        while (ptr + 4 <= end)
        {
            h32 = Rotl32(h32 + read32(ptr) * Prime3, 17) * Prime4;
            ptr += 4;
        }

        while (ptr < end)
        {
            h32 = Rotl32(h32 + (*ptr) * Prime5, 11) * Prime1;
            ++ptr;
        }

        h32 ^= h32 >> 15;
        h32 *= Prime2;
        h32 ^= h32 >> 13;
        h32 *= Prime3;
        h32 ^= h32 >> 16;
        return h32;
    }

    inline bool SafeCopyMemory(void* dst, const void* src, size_t size)
    {
        __try
        {
            std::memcpy(dst, src, size);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    inline bool ReadProtectedString(std::uintptr_t stringAddress, std::string& out)
    {
        out.clear();

        if (!stringAddress ||
            !HelperFuncs::CheckMemory(stringAddress) ||
            !HelperFuncs::CheckMemory(stringAddress + 0x18))
        {
            return false;
        }

        std::size_t length = 0;
        std::size_t capacity = 0;
        if (!SafeCopyMemory(&length, reinterpret_cast<const void*>(stringAddress + 0x10), sizeof(length)) ||
            !SafeCopyMemory(&capacity, reinterpret_cast<const void*>(stringAddress + 0x18), sizeof(capacity)))
        {
            return false;
        }

        constexpr std::size_t MaxLength = 64ull * 1024ull * 1024ull;

        if (length > MaxLength || capacity < length)
            return false;

        if (length == 0)
            return true;

        std::uintptr_t dataAddress = stringAddress;

        if (capacity > 0xF)
        {
            if (!SafeCopyMemory(&dataAddress, reinterpret_cast<const void*>(stringAddress), sizeof(dataAddress)) || !dataAddress)
                return false;
        }

        if (dataAddress > (std::numeric_limits<std::uintptr_t>::max)() - (length - 1))
        {
            return false;
        }

        if (!HelperFuncs::CheckMemory(dataAddress) || !HelperFuncs::CheckMemory(dataAddress + length - 1))
        {
            return false;
        }

        out.resize(length);
        if (!SafeCopyMemory(&out[0], reinterpret_cast<const void*>(dataAddress), length))
        {
            out.clear();
            return false;
        }

        return true;
    }
    // Mirrors the client's RSB1 decoder (0.744: sub_144255D30). Three payload shapes:
    //   1. 8-byte plain marker  -> raw bytecode follows, no XOR / hash
    //   2. RSB1, size field == 0 -> XOR + xxh32 check, raw (uncompressed) bytecode follows
    //   3. RSB1, size field != 0 -> XOR + xxh32 check, zstd frame follows
    // The old implementation only handled (3), so (1)/(2) scripts read as "no bytecode".
    inline bool DecodeCompressedBytecode(const std::string& compressed, std::string& output, std::string& error)
    {
        output.clear();
        error.clear();

        if (compressed.size() <= 8)
        {
            error = "Compressed data too short";
            return false;
        }

        constexpr std::uint64_t PlainMarker = 0xE009325B4A107A52ull; // client: head + 0x1FF6CDA4B5EF85AE == 0
        std::uint64_t head = 0;
        std::memcpy(&head, compressed.data(), sizeof(head));
        if (head == PlainMarker)
        {
            output.assign(compressed.data() + 8, compressed.size() - 8);
            return true;
        }

        std::vector<uint8_t> data(compressed.begin(), compressed.end());
        uint8_t key[4] = {
            static_cast<uint8_t>(data[0] ^ 0x52),
            static_cast<uint8_t>((data[1] ^ 0x53) - 41),
            static_cast<uint8_t>((data[2] ^ 0x42) - 82),
            static_cast<uint8_t>((data[3] ^ 0x31) - 123),
        };

        for (size_t i = 0; i < data.size(); ++i)
            data[i] ^= static_cast<uint8_t>(key[i & 3] + 41 * i);

        uint32_t expectedHash = uint32_t(key[0]) | (uint32_t(key[1]) << 8) | (uint32_t(key[2]) << 16) | (uint32_t(key[3]) << 24);
        uint32_t actualHash = XXH32(data.data(), data.size(), 42);
        if (actualHash != expectedHash)
        {
            error = "Hash mismatch during decompression";
            return false;
        }

        uint32_t decompressedSize =
            uint32_t(data[4]) | (uint32_t(data[5]) << 8) | (uint32_t(data[6]) << 16) | (uint32_t(data[7]) << 24);

        if (decompressedSize == 0)
        {
            // Stored uncompressed.
            output.assign(reinterpret_cast<const char*>(data.data()) + 8, data.size() - 8);
            return true;
        }

        if (decompressedSize > 64 * 1024 * 1024)
        {
            error = "Invalid decompressed size";
            return false;
        }

        constexpr uint32_t ZstdMagic = 0xFD2FB528u;
        uint32_t frameMagic = 0;
        if (data.size() - 8 <= 4 || (std::memcpy(&frameMagic, data.data() + 8, 4), frameMagic != ZstdMagic))
        {
            error = "Missing zstd frame";
            return false;
        }

        output.resize(decompressedSize);
        size_t result = ZSTD_decompress(output.data(), output.size(), data.data() + 8, data.size() - 8);
        if (ZSTD_isError(result) || result != decompressedSize)
        {
            error = std::string("ZSTD decompression error: ") +
                (ZSTD_isError(result) ? ZSTD_getErrorName(result) : "size mismatch");
            output.clear();
            return false;
        }

        return true;
    }

    inline bool DecodeProtectedStringAt(std::uintptr_t stringAddress, std::string& bytecode)
    {
        std::string compressed;
        if (!ReadProtectedString(stringAddress, compressed) || compressed.size() < 8)
            return false;

        std::string decoded;
        std::string decodeError;
        if (!DecodeCompressedBytecode(compressed, decoded, decodeError) || decoded.empty())
            return false;

        bytecode = std::move(decoded);
        return true;
    }

    // Where the bytecode ProtectedString lives: instance+outer -> holder, holder+inner -> std::string.
    // inner == SIZE_MAX means the std::string is inline at instance+outer (no holder indirection).
    struct BytecodeLocation
    {
        std::uintptr_t outer = 0;
        std::uintptr_t inner = 0x10;
    };

    // Last failure diagnostics, surfaced in the getscriptbytecode error so an offset/encoding
    // change can be told apart from a script that simply has no bytecode (runtime-created).
    inline thread_local std::string LastBytecodeDiag;

    inline bool TryFetchBytecodeAt(std::uintptr_t instance, BytecodeLocation loc, std::string& bytecode)
    {
        const std::uintptr_t holderAddress = instance + loc.outer;
        if (!HelperFuncs::CheckMemory(holderAddress) || !HelperFuncs::CheckMemory(holderAddress + 7))
            return false;

        if (loc.inner == SIZE_MAX)
            return DecodeProtectedStringAt(holderAddress, bytecode);

        std::uintptr_t holder = 0;
        if (!read_value(holderAddress, holder) || !holder || !HelperFuncs::CheckMemory(holder))
            return false;

        return DecodeProtectedStringAt(holder + loc.inner, bytecode);
    }

    // Records the first non-empty, binary-looking string that failed to decode. That points at
    // an encoding change (hash mismatch) rather than a wrong offset.
    inline void ProbeUndecodable(std::uintptr_t instance, BytecodeLocation loc, std::string& diag)
    {
        if (!diag.empty())
            return;

        std::uintptr_t strAddr = instance + loc.outer;
        if (loc.inner != SIZE_MAX)
        {
            std::uintptr_t holder = 0;
            if (!read_value(strAddr, holder) || !holder || !HelperFuncs::CheckMemory(holder))
                return;
            strAddr = holder + loc.inner;
        }

        std::string raw;
        if (!ReadProtectedString(strAddr, raw) || raw.size() < 16 || raw.size() > 16 * 1024 * 1024)
            return;

        std::string out, err;
        if (DecodeCompressedBytecode(raw, out, err))
            return;

        // An RSB1 blob is high-entropy; skip names/paths/plain text.
        size_t printable = 0;
        const size_t sample = (std::min)(raw.size(), size_t(64));
        for (size_t i = 0; i < sample; ++i)
            printable += (raw[i] >= 0x20 && raw[i] < 0x7F);
        if (printable * 10 > sample * 8)
            return;

        char buf[192];
        std::snprintf(buf, sizeof(buf), "; undecodable %zu-byte blob at +0x%llX/+0x%llX (%s)",
            raw.size(), (unsigned long long)loc.outer,
            (unsigned long long)(loc.inner == SIZE_MAX ? 0 : loc.inner), err.c_str());
        diag = buf;
    }

    inline bool FetchScriptBytecode(std::uintptr_t instance, const std::string& className, std::string& bytecode)
    {
        LastBytecodeDiag.clear();
        const bool isModuleScript = className == "ModuleScript";

        const std::uintptr_t preferred[] = {
            isModuleScript ? Offsets::Scripts::ModuleScriptByteCode : Offsets::Scripts::LocalScriptByteCode,
            isModuleScript ? (Offsets::Scripts::ModuleScriptByteCode - 0x8) : (Offsets::Scripts::LocalScriptByteCode - 0x8),
            isModuleScript ? Offsets::Scripts::LocalScriptByteCode : Offsets::Scripts::ModuleScriptByteCode,
            isModuleScript ? (Offsets::Scripts::LocalScriptByteCode - 0x8) : (Offsets::Scripts::ModuleScriptByteCode - 0x8),
        };

        for (const std::uintptr_t inner : { 0x10ull, 0x18ull })
        {
            for (const std::uintptr_t offset : preferred)
            {
                if (TryFetchBytecodeAt(instance, { offset, inner }, bytecode))
                    return true;
            }
        }

        // LocalScript and Script share a layout; ModuleScript has its own.
        const char* cacheKey = isModuleScript ? "ModuleScript" : "LocalScript";
        static std::unordered_map<std::string, BytecodeLocation> discovered;
        const auto cached = discovered.find(cacheKey);
        if (cached != discovered.end() && TryFetchBytecodeAt(instance, cached->second, bytecode))
            return true;

        // Full rescan of both the outer offset on the instance and the inner offset of the
        // string inside the holder - Roblox has moved both before. Decoding verifies an xxhash,
        // so misreading random memory as a std::string can't produce a false positive.
        constexpr std::uintptr_t ScanBegin = 0x60;
        constexpr std::uintptr_t ScanEnd = 0x600;
        constexpr std::uintptr_t Inners[] = { 0x10, 0x18, 0x20, 0x8, 0x28, 0x0, 0x30, 0x38, 0x40, SIZE_MAX };

        std::string diag;
        for (const std::uintptr_t inner : Inners)
        {
            for (std::uintptr_t offset = ScanBegin; offset <= ScanEnd; offset += sizeof(std::uintptr_t))
            {
                const BytecodeLocation loc{ offset, inner };
                if (!TryFetchBytecodeAt(instance, loc, bytecode))
                {
                    ProbeUndecodable(instance, loc, diag);
                    continue;
                }

                discovered[cacheKey] = loc;
                SUPQ_LOG_OK("getscriptbytecode: %s bytecode found at instance+0x%llX -> +0x%llX (configured 0x%llX -> +0x18) - update Offsets::Scripts",
                    className.c_str(), (unsigned long long)offset,
                    (unsigned long long)(inner == SIZE_MAX ? 0 : inner), (unsigned long long)preferred[0]);
                return true;
            }
        }

        LastBytecodeDiag = diag;
        return false;
    }

    // Reads Script.Source. Only populated for scripts created/edited at runtime
    // (Instance.new("LocalScript") + .Source = ...); replicated scripts ship bytecode only.
    inline int ReadSourceProp(lua_State* L)
    {
        lua_getfield(L, 1, "Source");
        return 1;
    }

    inline bool ReadScriptSource(lua_State* L, int index, std::string& out)
    {
        out.clear();
        index = lua_absindex(L, index);
        const int top = lua_gettop(L);

        // pcall'd so a property security check can't throw out of the caller.
        lua_pushcfunction(L, ReadSourceProp, "ReadSourceProp");
        lua_pushvalue(L, index);
        if (lua_pcall(L, 1, 1, 0) == LUA_OK && lua_isstring(L, -1))
        {
            size_t len = 0;
            const char* s = lua_tolstring(L, -1, &len);
            if (s && len)
                out.assign(s, len);
        }

        lua_settop(L, top);
        return !out.empty();
    }

    // Shipped bytecode first; falls back to compiling Source for runtime-created scripts.
    inline bool FetchOrCompileBytecode(lua_State* L, int index, std::uintptr_t instance,
        const std::string& className, std::string& bytecode, bool* fromSource = nullptr)
    {
        if (fromSource) *fromSource = false;
        if (FetchScriptBytecode(instance, className, bytecode) && !bytecode.empty())
            return true;

        const std::string diag = LastBytecodeDiag;
        std::string source;
        if (!ReadScriptSource(L, index, source))
        {
            LastBytecodeDiag = diag;
            return false;
        }

        // Encode opcodes the way Roblox's server compiler does (op * 227), so this blob is
        // interchangeable with shipped bytecode for decompile/getscriptclosure.
        struct ShippedEncoder : Luau::BytecodeEncoder
        {
            void encode(uint32_t* data, size_t count) override
            {
                for (size_t i = 0; i < count;)
                {
                    const uint8_t op = LUAU_INSN_OP(data[i]);
                    const int length = Luau::getOpLength(static_cast<LuauOpcode>(op));
                    if (length <= 0 || i + static_cast<size_t>(length) > count)
                        return;
                    data[i] = (data[i] & ~0xFFu) | static_cast<uint8_t>(op * 227);
                    i += static_cast<size_t>(length);
                }
            }
        } encoder;

        Luau::CompileOptions options;
        options.optimizationLevel = 1;
        options.debugLevel = 1;

        std::string compiled;
        try
        {
            compiled = Luau::compile(source, options, {}, &encoder);
        }
        catch (...)
        {
            compiled.clear();
        }

        if (compiled.empty() || compiled[0] == 0) // version byte 0 = compile error message
        {
            LastBytecodeDiag = diag + "; Source failed to compile";
            return false;
        }

        bytecode = std::move(compiled);
        if (fromSource) *fromSource = true;
        return true;
    }

    inline bool GetInstanceClassName(lua_State* L, int index, std::string& className)
    {
        int top = lua_gettop(L);
        lua_getfield(L, index, "ClassName");
        if (!lua_isstring(L, -1))
        {
            lua_settop(L, top);
            return false;
        }

        const char* value = lua_tostring(L, -1);
        className = value ? value : "";
        lua_settop(L, top);
        return true;
    }

    inline std::string read_bytecode(uintptr_t addr) {
        uintptr_t str = addr + 0x10;
        size_t len = *(size_t*)(str + 0x10);
        size_t cap = *(size_t*)(str + 0x18);
        uintptr_t data_ptr = (cap > 0x0f) ? *(uintptr_t*)(str + 0x00) : str;
        return std::string(reinterpret_cast<const char*>(data_ptr), len);
    }

    inline std::map<void*, std::string> ScriptHashes;

    bool IsValidScriptClass(const char* className)
    {
        if (!className) return false;
        return strcmp(className, "LocalScript") == 0 ||
            strcmp(className, "ModuleScript") == 0 ||
            strcmp(className, "Script") == 0;
    }

    inline std::string Blake2Hash(const char* data, size_t len)
    {
        if (!data || len == 0) return std::string(96, '0');

        uint64_t h[8] = {
            0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL,
            0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
            0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
            0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL
        };

        for (size_t i = 0; i < len; i++) {
            uint64_t val = static_cast<unsigned char>(data[i]);
            h[i % 8] ^= val;
            h[i % 8] = (h[i % 8] << 7) | (h[i % 8] >> 57);
            h[(i + 1) % 8] += val;
        }

        for (int round = 0; round < 12; round++) {
            for (int i = 0; i < 8; i++) {
                h[i] ^= h[(i + 1) % 8];
                h[i] = (h[i] << 13) | (h[i] >> 51);
            }
        }

        char output[97];
        for (int i = 0; i < 6; i++) {
            sprintf_s(output + (i * 16), 17, "%016llx", h[i]);
        }
        output[96] = '\0';

        return std::string(output);
    }

    inline int getscriptbytecode(lua_State* L)
    {
        luaL_checktype(L, 1, LUA_TUSERDATA);

        std::string className;
        if (!GetInstanceClassName(L, 1, className))
            return ScriptError(L, "Invalid Instance");

        if (className != "ModuleScript" &&
            className != "LocalScript" &&
            className != "Script" &&
            className != "CoreScript")
        {
            return ScriptError(L, "Script expected");
        }

        void* userdata = lua_touserdata(L, 1);
        if (!userdata)
            return ScriptError(L, "Invalid userdata");

        const std::uintptr_t instance =*reinterpret_cast<const std::uintptr_t*>(userdata);

        if (!instance || !HelperFuncs::CheckMemory(instance))
            return ScriptError(L, "Invalid Instance");

        std::string bytecode;

        if (!FetchOrCompileBytecode(L, 1, instance, className, bytecode))
        {
            const std::string message =
                "no readable bytecode on this " + className +
                " (empty ProtectedString at every candidate offset" + LastBytecodeDiag + ")";

            return ScriptError(L, message.c_str());
        }

        static const std::uint8_t targetVersion = []() -> std::uint8_t {
            size_t size = 0;
            char* dummy = luau_compile("return 0", 8, nullptr, &size);
            std::uint8_t v = (dummy && size) ? static_cast<std::uint8_t>(dummy[0]) : 0;
            free(dummy);
            return v;
        }();

        const bool raw = lua_isboolean(L, 2) && lua_toboolean(L, 2);
        if (!raw && targetVersion != 0 && bytecode[0] != 0)
            bytecode[0] = static_cast<char>(targetVersion);

        lua_pushlstring(L, bytecode.data(), bytecode.size());

        return 1;
    }

    static int getscripthash(lua_State* L)
    {
        luaL_checktype(L, 1, LUA_TUSERDATA);

        std::string className;
        if (!GetInstanceClassName(L, 1, className))
            return ScriptError(L, "Invalid Instance");

        const bool isModuleScript = className == "ModuleScript";
        const bool isLocalScript = className == "LocalScript";
        const bool isScript = className == "Script";
        const bool isCoreScript = className == "CoreScript";

        if (!isModuleScript && !isLocalScript && !isScript && !isCoreScript)
            return ScriptError(L, "Script expected");

        void* userdata = lua_touserdata(L, 1);
        if (!userdata || !HelperFuncs::CheckMemory(reinterpret_cast<std::uintptr_t>(userdata)))
            return ScriptError(L, "Invalid Instance");

        const std::uintptr_t instance = *reinterpret_cast<const std::uintptr_t*>(userdata);
        if (!instance || !HelperFuncs::CheckMemory(instance))
        {
            lua_pushnil(L);
            return 1;
        }
        if (isScript)
        {
            int runContext = -1;

            lua_getfield(L, 1, "RunContext");
            if (!lua_isnil(L, -1))
            {
                lua_getfield(L, -1, "Value");
                if (lua_isnumber(L, -1))
                    runContext = static_cast<int>(lua_tointeger(L, -1));
                lua_pop(L, 1);
            }
            lua_pop(L, 1);

            if (runContext != 2)
                return ScriptError(L, "Expected a Script with RunContext set to Client");
        }

        // Source wins when present: it's only populated for runtime-created/edited scripts, and
        // editing Source doesn't touch the shipped bytecode, so hashing bytecode alone would
        // return the same hash for a modified script.
        std::string payload;
        if (!ReadScriptSource(L, 1, payload) && !FetchScriptBytecode(instance, className, payload))
        {
            lua_pushnil(L);
            return 1;
        }

        const std::string hash = sha384(payload);
        if (hash.empty())
        {
            lua_pushnil(L);
            return 1;
        }

        lua_pushlstring(L, hash.data(), hash.size());
        return 1;
    }

    inline bool IsModuleScriptValue(lua_State* L, int index)
    {
        if (!lua_isuserdata(L, index))
            return false;

        index = lua_absindex(L, index);
        const int top = lua_gettop(L);

        lua_getfield(L, index, "ClassName");
        const bool isModule = lua_isstring(L, -1) &&
            strcmp(lua_tostring(L, -1), "ModuleScript") == 0;

        lua_settop(L, top);
        return isModule;
    }

    int getloadedmodules(lua_State* L) {
        lua_normalisestack(L, 0);
        lua_newtable(L);
        const int candidateTable = lua_gettop(L);

        struct GCOContext {
            lua_State* pLua;
            int itemsFound;
            int tableIndex;
            std::unordered_map<uintptr_t, bool> map;
        } gcCtx{ L, 0, candidateTable };

        const auto oldThreshold = L->global->GCthreshold;
        L->global->GCthreshold = SIZE_MAX;

        luaM_visitgco(L, &gcCtx, [](void* ctx, lua_Page*, GCObject* pGcObj) -> bool {
            auto* c = static_cast<GCOContext*>(ctx);
            auto* cL = c->pLua;

            if (isdead(cL->global, pGcObj)) return false;
            if (pGcObj->gch.tt != LUA_TFUNCTION) return false;

            Closure* closure = gco2cl(pGcObj);
            if (Closures::IsExecutorClosure(closure))
                return false;

            cL->top->value.gc = pGcObj;
            cL->top->tt = LUA_TFUNCTION;
            ++cL->top;

            lua_getfenv(cL, -1);
            if (!lua_isnil(cL, -1)) {
                lua_getfield(cL, -1, ("script"));
                if (lua_isuserdata(cL, -1)) {
                    void* script_userdata = lua_touserdata(cL, -1);
                    uintptr_t script_addr = script_userdata ?
                        *reinterpret_cast<uintptr_t*>(script_userdata) : 0;

                    if (script_addr && IsModuleScriptValue(cL, -1) &&
                        c->map.emplace(script_addr, true).second) {
                        lua_rawseti(cL, c->tableIndex, ++c->itemsFound);
                    }
                    else {
                        lua_pop(cL, 1);
                    }
                }
                else lua_pop(cL, 1);
            }
            lua_pop(cL, 2);
            return false;
            });

        L->global->GCthreshold = oldThreshold;
        return 1;
    }

    // Walks the engine's instance bridge cache (every Instance userdata pushed to Lua, nil-parented
    // ones included) - the same source getnilinstances uses. The old raw GC page walk missed
    // freshly created, unparented scripts.
    __forceinline int getscripts(lua_State* L) {
        lua_settop(L, 0);
        lua_newtable(L);
        const int result = 1;
        int n = 0;

        lua_pushlightuserdata(L, reinterpret_cast<void*>(Roblox::InstanceBridge::UintPtr));
        lua_rawget(L, LUA_REGISTRYINDEX);
        if (!lua_istable(L, -1)) {
            lua_settop(L, result);
            return 1;
        }
        const int cache = lua_gettop(L);

        const auto oldThreshold = L->global->GCthreshold;
        L->global->GCthreshold = SIZE_MAX;

        lua_pushnil(L);
        while (lua_next(L, cache)) {
            if (lua_isuserdata(L, -1) && !strcmp(luaL_typename(L, -1), "Instance")) {
                bool take = false;

                lua_getfield(L, -1, "ClassName");
                const char* cls = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : nullptr;
                if (cls && (!strcmp(cls, "LocalScript") || !strcmp(cls, "ModuleScript") || !strcmp(cls, "CoreScript"))) {
                    take = true;
                }
                else if (cls && !strcmp(cls, "Script")) {
                    // Only client-run Scripts; Legacy/Server ones never execute here.
                    lua_getfield(L, -2, "RunContext");
                    if (lua_isuserdata(L, -1)) {
                        lua_getfield(L, -1, "Name");
                        const char* runContext = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : nullptr;
                        take = runContext && !strcmp(runContext, "Client");
                        lua_pop(L, 1);
                    }
                    lua_pop(L, 1);
                }
                lua_pop(L, 1);

                if (take) {
                    lua_pushvalue(L, -1);
                    lua_rawseti(L, result, ++n);
                }
            }
            lua_pop(L, 1);
        }

        L->global->GCthreshold = oldThreshold;
        lua_settop(L, result);
        return 1;
    }

    uintptr_t GetInstancePropertyByName(const uintptr_t instance, const std::string& targetPropertyName)
    {
        const uintptr_t classDescriptor =
            *reinterpret_cast<uintptr_t*>(instance + 0x18);

        const uintptr_t propertiesStart =
            *reinterpret_cast<uintptr_t*>(classDescriptor + 0x28);

        for (uint32_t i = 0; i < 80; ++i)
        {
            const uintptr_t entry =
                propertiesStart + (i * 0x10);

            const uintptr_t prop =
                *reinterpret_cast<uintptr_t*>(entry);

            if (!prop)
                continue;

            const uintptr_t namePtr =
                *reinterpret_cast<uintptr_t*>(prop + 0x8);

            if (!namePtr)
                continue;

            const char* name =
                reinterpret_cast<const char*>(namePtr);

            if (name && targetPropertyName == name)
                return prop;
        }

        return 0;
    }

    inline void PushGCObjects(lua_State* L, bool includeTables)
    {
        lua_newtable(L);
        lua_newtable(L);

        lua_pushstring(L, "kvs");
        lua_setfield(L, -2, "__mode");
        lua_setmetatable(L, -2);

        typedef struct {
            lua_State* luaThread;
            bool includeTables;
            int itemsFound;
        } GCOContext;

        auto GCContext = GCOContext{ L, includeTables, 0 };

        const auto oldGCThreshold = L->global->GCthreshold;
        L->global->GCthreshold = SIZE_MAX;

        __try
        {
            luaM_visitgco(L, &GCContext, [](void* ctx, lua_Page* page, GCObject* gcObj) -> bool {
                const auto context = static_cast<GCOContext*>(ctx);
                const auto luaThread = context->luaThread;

                if (isdead(luaThread->global, gcObj))
                    return false;

                const auto gcObjectType = gcObj->gch.tt;
                if (gcObjectType == LUA_TFUNCTION || gcObjectType == LUA_TTHREAD || gcObjectType == LUA_TUSERDATA ||
                    gcObjectType == LUA_TLIGHTUSERDATA || gcObjectType == LUA_TBUFFER ||
                    (gcObjectType == LUA_TTABLE && context->includeTables)) {
                    lua_checkstack(luaThread, 3);
                    luaThread->top->value.gc = gcObj;
                    luaThread->top->tt = gcObjectType;
                    incr_top(luaThread);

                    const auto newTableIndex = ++context->itemsFound;
                    lua_rawseti(luaThread, -2, newTableIndex);
                }

                return false;
                });
        }
        __except (SafeCall::FaultFilter(GetExceptionCode()))
        {
        }

        L->global->GCthreshold = oldGCThreshold;
    }

    inline int getgc(lua_State* L) {
        PushGCObjects(L, luaL_optboolean(L, 1, false));
        return 1;
    };

    inline int getallthreads(lua_State* L)
    {
        lua_newtable(L);
        lua_createtable(L, 0, 1);
        lua_pushstring(L, "v");
        lua_setfield(L, -2, "__mode");
        lua_setmetatable(L, -2);

        struct Context
        {
            lua_State* L;
            int count;
        } context{ L, 0 };

        const auto oldThreshold = L->global->GCthreshold;
        L->global->GCthreshold = SIZE_MAX;

        __try
        {
            luaM_visitgco(L, &context, [](void* raw, lua_Page*, GCObject* object) -> bool {
                auto* ctx = static_cast<Context*>(raw);
                lua_State* L = ctx->L;

                if (object->gch.tt != LUA_TTHREAD || isdead(L->global, object))
                    return false;

                lua_State* thread = gco2th(object);
                if (thread != L && thread->userdata &&
                    thread->userdata->capabilities == MaxCapabilities)
                    return false;

                lua_checkstack(L, 2);
                L->top->value.gc = object;
                L->top->tt = LUA_TTHREAD;
                incr_top(L);
                lua_rawseti(L, -2, ++ctx->count);
                return false;
            });
        }
        __except (SafeCall::FaultFilter(GetExceptionCode()))
        {
        }

        L->global->GCthreshold = oldThreshold;
        return 1;
    }

    inline int isexecutorthread(lua_State* L)
    {
        lua_State* target = L;
        if (!lua_isnoneornil(L, 1))
        {
            luaL_checktype(L, 1, LUA_TTHREAD);
            target = lua_tothread(L, 1);
        }

        const bool ours = target &&
            ((SharedVariables::ExploitThread && target == SharedVariables::ExploitThread) ||
                (target->userdata && target->userdata->capabilities == MaxCapabilities));
        lua_pushboolean(L, ours);
        return 1;
    }

    namespace FilterGC
    {
        inline bool SameValue(lua_State* L, int a, int b)
        {
            if (lua_type(L, a) == LUA_TNUMBER && lua_type(L, b) == LUA_TNUMBER)
            {
                const double x = lua_tonumber(L, a);
                const double y = lua_tonumber(L, b);
                return x == y || (std::isnan(x) && std::isnan(y));
            }

            return lua_rawequal(L, a, b) != 0;
        }

        inline bool HasConstants(lua_State* L, Closure* cl, int wanted)
        {
            if (cl->isC || !cl->l.p)
                return false;

            const Proto* p = cl->l.p;
            const std::vector<int> imports = Debug::collect_import_slots(p);

            lua_pushnil(L);
            while (lua_next(L, wanted) != 0)
            {
                const int want = lua_gettop(L);
                bool found = false;

                for (int i = 0; i < p->sizek && !found; ++i)
                {
                    if (i < static_cast<int>(imports.size()) && imports[i] != -1)
                        continue;

                    const TValue* k = &p->k[i];
                    if (ttisnil(k) || ttistable(k) || ttisfunction(k))
                        continue;

                    luaC_threadbarrier(L);
                    luaA_pushvalue(L, k);
                    found = SameValue(L, -1, want);
                    lua_pop(L, 1);
                }

                lua_pop(L, 1);
                if (!found)
                {
                    lua_pop(L, 1);
                    return false;
                }
            }

            return true;
        }

        inline bool HasUpvalues(lua_State* L, int fn, int wanted)
        {
            lua_pushnil(L);
            while (lua_next(L, wanted) != 0)
            {
                const int want = lua_gettop(L);
                bool found = false;

                for (int n = 1; !found && lua_getupvalue(L, fn, n); ++n)
                {
                    found = SameValue(L, -1, want);
                    lua_pop(L, 1);
                }

                lua_pop(L, 1);
                if (!found)
                {
                    lua_pop(L, 1);
                    return false;
                }
            }

            return true;
        }

        inline bool HasKeys(lua_State* L, int tbl, int wanted)
        {
            lua_pushnil(L);
            while (lua_next(L, wanted) != 0)
            {
                lua_rawget(L, tbl);
                const bool present = !lua_isnil(L, -1);
                lua_pop(L, 1);

                if (!present)
                {
                    lua_pop(L, 1);
                    return false;
                }
            }

            return true;
        }

        inline bool HasValues(lua_State* L, int tbl, int wanted)
        {
            lua_pushnil(L);
            while (lua_next(L, wanted) != 0)
            {
                const int want = lua_gettop(L);
                bool found = false;

                lua_pushnil(L);
                while (lua_next(L, tbl) != 0)
                {
                    found = SameValue(L, -1, want);
                    lua_pop(L, 1);
                    if (found)
                    {
                        lua_pop(L, 1);
                        break;
                    }
                }

                lua_pop(L, 1);
                if (!found)
                {
                    lua_pop(L, 1);
                    return false;
                }
            }

            return true;
        }

        inline bool HasKeyValuePairs(lua_State* L, int tbl, int wanted)
        {
            lua_pushnil(L);
            while (lua_next(L, wanted) != 0)
            {
                lua_pushvalue(L, -2);
                lua_rawget(L, tbl);
                const bool equal = SameValue(L, -1, -2);
                lua_pop(L, 2);

                if (!equal)
                {
                    lua_pop(L, 1);
                    return false;
                }
            }

            return true;
        }

        struct FunctionOptions
        {
            int Name = 0;
            int Hash = 0;
            int Constants = 0;
            int Upvalues = 0;
            int Environment = 0;
            int StartLine = 0;
            int Source = 0;
            int UpvalueCount = 0;
            int ConstantCount = 0;
            bool IgnoreExecutor = true;
            mutable std::unordered_map<const Proto*, std::string> HashCache;
        };

        struct TableOptions
        {
            int Keys = 0;
            int Values = 0;
            int KeyValuePairs = 0;
            int Metatable = 0;
        };

        inline int OptionField(lua_State* L, int options, const char* field, int expectedType)
        {
            lua_getfield(L, options, field);
            if (lua_isnil(L, -1))
            {
                lua_pop(L, 1);
                return 0;
            }

            if (expectedType != LUA_TNONE && lua_type(L, -1) != expectedType)
                luaL_error(L, "filtergc: option '%s' expected %s, got %s", field,
                    lua_typename(L, expectedType), luaL_typename(L, -1));

            return lua_gettop(L);
        }

        inline bool MatchFunction(lua_State* L, int fn, const FunctionOptions& o)
        {
            Closure* cl = clvalue(luaA_toobject(L, fn));
            if (!cl)
                return false;

            if (o.IgnoreExecutor && Closures::IsExecutorClosure(cl))
                return false;

            if (cl->isC && (o.Constants || o.Upvalues || o.Hash || o.ConstantCount))
                return false;

            if (o.UpvalueCount && cl->nupvalues != static_cast<int>(lua_tointeger(L, o.UpvalueCount)))
                return false;

            if (o.ConstantCount && (!cl->l.p || cl->l.p->sizek != static_cast<int>(lua_tointeger(L, o.ConstantCount))))
                return false;

            if (o.Name || o.StartLine || o.Source)
            {
                lua_Debug ar{};
                lua_pushvalue(L, fn);
                const int ok = lua_getinfo(L, -1, "sn", &ar);
                lua_pop(L, 1);
                if (!ok)
                    return false;

                if (o.Name)
                {
                    size_t len = 0;
                    const char* want = lua_tolstring(L, o.Name, &len);
                    if (!ar.name || strlen(ar.name) != len || memcmp(ar.name, want, len) != 0)
                        return false;
                }

                if (o.StartLine && (cl->isC || ar.linedefined != static_cast<int>(lua_tointeger(L, o.StartLine))))
                    return false;

                if (o.Source && (!ar.short_src || strcmp(ar.short_src, lua_tostring(L, o.Source)) != 0))
                    return false;
            }

            if (o.Environment)
            {
                lua_getfenv(L, fn);
                const bool same = SameValue(L, -1, o.Environment);
                lua_pop(L, 1);
                if (!same)
                    return false;
            }

            if (o.Constants && !HasConstants(L, cl, o.Constants))
                return false;

            if (o.Upvalues && !HasUpvalues(L, fn, o.Upvalues))
                return false;

            if (o.Hash)
            {
                if (cl->isC || !cl->l.p)
                    return false;

                auto cached = o.HashCache.find(cl->l.p);
                if (cached == o.HashCache.end())
                {
                    const std::vector<unsigned char> bytes = Crypt::getProtoBytes(cl->l.p);
                    cached = o.HashCache.emplace(cl->l.p, sha384(std::string(bytes.begin(), bytes.end()))).first;
                }

                size_t len = 0;
                const char* want = lua_tolstring(L, o.Hash, &len);
                if (cached->second.size() != len || _strnicmp(cached->second.data(), want, len) != 0)
                    return false;
            }

            return true;
        }

        inline bool MatchTable(lua_State* L, int tbl, const TableOptions& o)
        {
            if (o.Metatable)
            {
                if (!lua_getmetatable(L, tbl))
                    return false;

                const bool same = SameValue(L, -1, o.Metatable);
                lua_pop(L, 1);
                if (!same)
                    return false;
            }

            if (o.Keys && !HasKeys(L, tbl, o.Keys))
                return false;

            if (o.KeyValuePairs && !HasKeyValuePairs(L, tbl, o.KeyValuePairs))
                return false;

            if (o.Values && !HasValues(L, tbl, o.Values))
                return false;

            return true;
        }

        inline bool IsFilterInput(lua_State* L, int candidate, int options)
        {
            if (lua_rawequal(L, candidate, options))
                return true;

            bool hit = false;
            lua_pushnil(L);
            while (lua_next(L, options) != 0)
            {
                hit = lua_rawequal(L, -1, candidate) != 0;
                lua_pop(L, 1);
                if (hit)
                {
                    lua_pop(L, 1);
                    break;
                }
            }

            return hit;
        }
    }

    inline int filtergc(lua_State* L)
    {
        const char* kind = luaL_checkstring(L, 1);
        luaL_checktype(L, 2, LUA_TTABLE);
        const bool returnOne = luaL_optboolean(L, 3, false);
        lua_settop(L, 3);

        const bool wantFunctions = strcmp(kind, "function") == 0;
        const bool wantTables = strcmp(kind, "table") == 0;
        if (!wantFunctions && !wantTables)
            luaL_argerror(L, 1, "expected 'function' or 'table'");

        FilterGC::FunctionOptions fo;
        FilterGC::TableOptions to;

        if (wantFunctions)
        {
            fo.Name = FilterGC::OptionField(L, 2, "Name", LUA_TSTRING);
            fo.Hash = FilterGC::OptionField(L, 2, "Hash", LUA_TSTRING);
            fo.Constants = FilterGC::OptionField(L, 2, "Constants", LUA_TTABLE);
            fo.Upvalues = FilterGC::OptionField(L, 2, "Upvalues", LUA_TTABLE);
            fo.Environment = FilterGC::OptionField(L, 2, "Environment", LUA_TTABLE);
            fo.StartLine = FilterGC::OptionField(L, 2, "StartLine", LUA_TNUMBER);
            fo.Source = FilterGC::OptionField(L, 2, "Source", LUA_TSTRING);
            fo.UpvalueCount = FilterGC::OptionField(L, 2, "UpvalueCount", LUA_TNUMBER);
            fo.ConstantCount = FilterGC::OptionField(L, 2, "ConstantCount", LUA_TNUMBER);

            if (const int ignore = FilterGC::OptionField(L, 2, "IgnoreExecutor", LUA_TBOOLEAN))
                fo.IgnoreExecutor = lua_toboolean(L, ignore) != 0;
        }
        else
        {
            to.Keys = FilterGC::OptionField(L, 2, "Keys", LUA_TTABLE);
            to.Values = FilterGC::OptionField(L, 2, "Values", LUA_TTABLE);
            to.KeyValuePairs = FilterGC::OptionField(L, 2, "KeyValuePairs", LUA_TTABLE);
            to.Metatable = FilterGC::OptionField(L, 2, "Metatable", LUA_TTABLE);
        }

        PushGCObjects(L, wantTables);
        const int objects = lua_gettop(L);
        const int count = lua_objlen(L, objects);

        int results = 0;
        if (!returnOne)
        {
            lua_createtable(L, 0, 0);
            results = lua_gettop(L);
        }

        int matched = 0;
        for (int i = 1; i <= count; ++i)
        {
            lua_checkstack(L, 8);
            lua_rawgeti(L, objects, i);
            const int candidate = lua_gettop(L);

            bool match = false;
            if (wantFunctions && lua_isfunction(L, candidate))
                match = FilterGC::MatchFunction(L, candidate, fo);
            else if (wantTables && lua_istable(L, candidate) && !FilterGC::IsFilterInput(L, candidate, 2))
                match = FilterGC::MatchTable(L, candidate, to);

            if (!match)
            {
                lua_settop(L, candidate - 1);
                continue;
            }

            if (returnOne)
                return 1;

            lua_rawseti(L, results, ++matched);
            lua_settop(L, results);
        }

        if (returnOne)
            lua_pushnil(L);
        else
            lua_settop(L, results);

        return 1;
    }

    inline void MirrorOutputRedirect(lua_State* L, lua_State* RobloxState, const char* name)
    {
        lua_rawcheckstack(L, 3);
        luaC_threadbarrier(L);
        L->top->value.p = RobloxState->gt;
        L->top->tt = LUA_TTABLE;
        L->top++;
        lua_rawgetfield(L, -1, name);
        lua_remove(L, -2);

        lua_State* exploit = SharedVariables::ExploitThread;
        if (exploit && exploit->global == L->global && exploit->gt)
        {
            L->top->value.p = exploit->gt;
            L->top->tt = LUA_TTABLE;
            L->top++;
            lua_rawgetfield(L, -1, name);
            lua_remove(L, -2);

            if (lua_tocfunction(L, -1) == Console::RedirectedOutput)
                lua_remove(L, -2);
            else
                lua_pop(L, 1);
        }

        lua_rawsetfield(L, -2, name);
    }

    int getrenv(lua_State* L)
    {
        lua_check(L, 0);
        lua_State* RobloxState = L->global->mainthread;

        const std::uint64_t generation = SharedVariables::StateGeneration.load(std::memory_order_acquire);
        if (generation != RenvCacheGeneration)
        {
            RenvCache.clear();
            RenvCacheGeneration = generation;
        }

        LuaTable*& cached = RenvCache[L->global];
        if (!cached)
        {
            LuaTable* clone = luaH_clone(L, RobloxState->gt);

            lua_rawcheckstack(L, 2);
            luaC_threadbarrier(L);
            luaC_threadbarrier(RobloxState);
            L->top->value.p = clone;
            L->top->tt = LUA_TTABLE;
            L->top++;

            lua_pushvalue(L, -1);
            lua_ref(L, -1);
            lua_pop(L, 1);

            lua_rawgeti(L, LUA_REGISTRYINDEX, 2);
            lua_setfield(L, -2, "_G");
            lua_rawgeti(L, LUA_REGISTRYINDEX, 4);
            lua_setfield(L, -2, "shared");

            cached = clone;
            lua_pop(L, 1);
        }

        lua_rawcheckstack(L, 1);
        luaC_threadbarrier(L);
        L->top->value.p = cached;
        L->top->tt = LUA_TTABLE;
        L->top++;

        MirrorOutputRedirect(L, RobloxState, "print");
        MirrorOutputRedirect(L, RobloxState, "warn");

        return 1;
    }

    inline void TraceGetfenvArgument(lua_State* L)
    {
        if (!CallTrace::IsEnabled())
            return;

        char detail[56];

        if (lua_isnumber(L, 1))
        {
            std::snprintf(detail, sizeof(detail), "level %d", static_cast<int>(lua_tointeger(L, 1)));
        }
        else if (lua_isfunction(L, 1))
        {
            Closure* closure = clvalue(luaA_toobject(L, 1));
            const bool ours = Closures::IsExecutorClosure(closure);

            if (closure->isC)
            {
                const char* name = closure->c.debugname ? getstr(closure->c.debugname) : "?";
                std::snprintf(detail, sizeof(detail), "C %s%s", name, ours ? " [ours]" : "");
            }
            else
            {
                const char* source = (closure->l.p && closure->l.p->source)
                    ? getstr(closure->l.p->source) : "?";
                std::snprintf(detail, sizeof(detail), "L %s:%d%s", source,
                    closure->l.p ? closure->l.p->linedefined : -1, ours ? " [ours]" : "");
            }
        }
        else
        {
            std::snprintf(detail, sizeof(detail), "%s", luaL_typename(L, 1));
        }

        CallTrace::Note("getfenv.arg", detail, static_cast<int>(L->nCcalls));
    }

    int getfenv(lua_State* L)
    {
        TraceGetfenvArgument(L);

        if (lua_isfunction(L, 1))
        {
            lua_pushvalue(L, 1);
        }
        else
        {
            const int level = static_cast<int>(luaL_optinteger(L, 1, 1));
            luaL_argcheck(L, level >= 0, 1, "level must be non-negative");

            if (level == 0)
            {
                lua_pushvalue(L, LUA_GLOBALSINDEX);
                lua_setsafeenv(L, -1, false);
                return 1;
            }

            lua_State* state = L;
            int resolved = -1;
            if (Oth::OnHookThread(state))
            {
                CallInfo* frame = Oth::Frame(state, level);
                if (frame)
                    resolved = static_cast<int>(state->ci - frame);
            }
            else
            {
                resolved = Debug::resolve_hidden_aware_level(L, level);
            }

            lua_Debug ar;
            if (resolved < 0 || lua_getinfo(state, resolved, "f", &ar) == 0)
                luaL_argerror(L, 1, "invalid level");

            if (state != L)
                lua_xmove(state, L, 1);

            if (lua_isnil(L, -1))
                luaL_error(L, "no function environment for tail call at level %d", level);
        }

        if (lua_iscfunction(L, -1))
        {
            Closure* CFunction = clvalue(index2addr(L, -1));

            if (Closures::is_hook_plumbing_cl(CFunction) && CFunction->env && CFunction->env != L->gt)
            {
                lua_rawcheckstack(L, 1);
                luaC_threadbarrier(L);
                sethvalue(L, L->top, CFunction->env);
                incr_top(L);
                return 1;
            }

            if (!lua_isfunction(L, 1) || !Closures::is_newcclosure_wrapper(CFunction))
            {
                lua_pushvalue(L, LUA_GLOBALSINDEX);
                lua_setsafeenv(L, -1, false);
                return 1;
            }
        }

        lua_getfenv(L, -1);
        lua_setsafeenv(L, -1, false);
        return 1;
    }

    inline void PushTable(lua_State* L, LuaTable* Table)
    {
        luaC_threadbarrier(L);
        sethvalue(L, L->top, Table);
        incr_top(L);
    }

    int getcallingscript(lua_State* L)
    {
        lua_State* target = Oth::Original(L);
        if (target->userdata)
        {
            if (std::shared_ptr<uintptr_t> source = target->userdata->source.lock())
            {
                Roblox::PushInstance(L, &source);
                return 1;
            }
        }

        for (CallInfo* Frame = target->ci; Frame >= target->base_ci; --Frame)
        {
            if (!ttisfunction(Frame->func))
                continue;

            Closure* Function = clvalue(Frame->func);
            if (!Function->env)
                continue;

            PushTable(L, Function->env);
            lua_getfield(L, -1, "script");
            lua_remove(L, -2);

            if (!lua_isnil(L, -1))
                return 1;

            lua_pop(L, 1);
        }

        lua_pushnil(L);
        return 1;
    }

    int getsenv(lua_State* L) {
        luaL_checktype(L, 1, LUA_TUSERDATA);

        auto script = *reinterpret_cast<std::uintptr_t*>(lua_touserdata(L, 1));
        if (!script) {
            lua_pushnil(L);
            return 1;
        }

        lua_getfield(L, 1, "ClassName");
        const char* rawClassName = lua_tostring(L, -1);
        std::string className = rawClassName ? rawClassName : "";
        lua_pop(L, 1);

        if (className != "LocalScript" && className != "Script" && className != "ModuleScript") {
            lua_pushnil(L);
            return 1;
        }

        lua_pushliteral(L, "script");
        TString* scriptKey = tsvalue(index2addr(L, -1));

        LuaTable* found = nullptr;
        std::unordered_set<LuaTable*> seenEnvs;

        for (int pass = 0; pass < 2 && !found; ++pass) {
            for (lua_Page* cur = L->global->allgcopages; cur && !found;) {
                lua_Page* next = luaM_getnextpage(cur);

                char* start;
                char* end;
                int busy;
                int block;
                luaM_getpagewalkinfo(cur, &start, &end, &busy, &block);

                for (char* pos = start; pos != end; pos += block) {
                    auto gco = reinterpret_cast<GCObject*>(pos);
                    if (!gco || isdead(L->global, gco))
                        continue;

                    if (pass == 0) {
                        if (gco->gch.tt != LUA_TTHREAD)
                            continue;

                        auto thread = reinterpret_cast<lua_State*>(pos);
                        if (!thread->userdata || thread->userdata->source.expired() || !thread->gt)
                            continue;
                        if ((uintptr_t)thread->userdata->source.lock().get() != script)
                            continue;

                        found = thread->gt;
                        break;
                    }

                    if (gco->gch.tt != LUA_TFUNCTION)
                        continue;

                    auto closure = reinterpret_cast<Closure*>(pos);
                    LuaTable* env = closure->env;
                    if (closure->isC || !env || !seenEnvs.insert(env).second)
                        continue;

                    const TValue* value = luaH_getstr(env, scriptKey);
                    if (!ttisuserdata(value))
                        continue;

                    if (*reinterpret_cast<std::uintptr_t*>(uvalue(value)->data) == script) {
                        found = env;
                        break;
                    }
                }

                cur = next;
            }
        }

        lua_pop(L, 1);
        if (!found) {
            lua_pushnil(L);
            return 1;
        }

        luaC_threadbarrier(L);
        sethvalue(L, L->top, found);
        incr_top(L);
        return 1;
    }

    int getmenv(lua_State* L)
    {

        if (lua_isuserdata(L, 1)) {
            lua_getfield(L, 1, "ClassName");
            const char* cn = lua_isstring(L, -1) ? lua_tostring(L, -1) : nullptr;
            lua_pop(L, 1);

            if (cn && strcmp(cn, "ModuleScript") == 0) {
                lua_getglobal(L, "require");
                if (lua_isfunction(L, -1)) {
                    lua_pushvalue(L, 1);
                    if (lua_pcall(L, 1, 1, 0) == 0) {
                        if (lua_istable(L, -1))
                            return 1;

                        if (lua_isfunction(L, -1)) {
                            lua_getfenv(L, -1);
                            if (lua_istable(L, -1)) {
                                lua_remove(L, -2);
                                return 1;
                            }
                            lua_pop(L, 1);
                        }
                    }
                    lua_pop(L, 1);
                }
                else {
                    lua_pop(L, 1);
                }
            }
        }

        lua_pushvalue(L, LUA_GLOBALSINDEX);
        return 1;
    }

    inline int isexecutorclosure(lua_State* L)
    {
        luaL_checktype(L, 1, LUA_TFUNCTION);

        Closure* closure = clvalue(luaA_toobject(L, 1));
        if (!closure)
        {
            lua_pushboolean(L, 0);
            return 1;
        }

        if (closure->isC)
        {
            lua_pushboolean(L, closure->c.f == nullptr ? 0 : 1);
            return 1;
        }

        Proto* proto = closure->l.p;
        if (!proto)
        {
            lua_pushboolean(L, 0);
            return 1;
        }

        lua_Debug ar;
        lua_pushvalue(L, 1);
        lua_getinfo(L, -1, "S", &ar);
        lua_pop(L, 1);

        if (ar.source && (
            strstr(ar.source, "@") == ar.source ||
            strstr(ar.source, "=[C]") == ar.source
            ))
        {
            lua_pushboolean(L, 0);
            return 1;
        }

        lua_pushboolean(L, 1);
        return 1;
    }

    struct HookRecord
    {
        int OriginalRef = LUA_NOREF;
        int HookRef = LUA_NOREF;
    };

    inline std::unordered_map<Closure*, HookRecord> HookRecords;
    inline std::unordered_map<Closure*, int> NewCClosureRefs;
    inline std::unordered_set<Closure*> ExecutorClosures;

    inline int FinishYieldableCall(lua_State* L)
    {
        if (L->status == SCHEDULED_REENTRY || L->status == LUA_YIELD || L->status == LUA_BREAK)
            return C_CALL_YIELD;

        return lua_gettop(L);
    }

    inline int CallReferencedFunction(lua_State* L, int Ref)
    {
        lua_getref(L, Ref);
        lua_insert(L, 1);

        luaD_callint(L, L->base, LUA_MULTRET, lua_isyieldable(L) != 0);
        return FinishYieldableCall(L);
    }

    int DispatchContinuation(lua_State* L, int Status)
    {
        return lua_gettop(L);
    }

    int NewCClosureDispatcher(lua_State* L)
    {
        Closure* Current = curr_func(L);
        auto It = NewCClosureRefs.find(Current);
        if (It == NewCClosureRefs.end())
            luaL_error(L, "newcclosure: missing wrapped function");

        return CallReferencedFunction(L, It->second);
    }

    inline Closure* ToClosure(lua_State* L, int Index)
    {
        luaL_checktype(L, Index, LUA_TFUNCTION);
        return clvalue(luaA_toobject(L, Index));
    }

    inline void PushClosure(lua_State* L, Closure* ClosureObject)
    {
        luaC_threadbarrier(L);
        setclvalue(L, L->top, ClosureObject);
        incr_top(L);
    }

    inline Closure* CloneClosure(lua_State* L, Closure* Source)
    {
        Closure* Clone = nullptr;

        if (Source->isC)
        {
            Clone = luaF_newCclosure(L, Source->nupvalues, Source->env);
            Clone->stacksize = Source->stacksize;
            Clone->preload = Source->preload;
            Clone->c.f = Source->c.f;
            Clone->c.cont = static_cast<lua_Continuation>(Source->c.cont);
            Clone->c.debugname = Source->c.debugname;

            for (int i = 0; i < Source->nupvalues; ++i)
                setobj2n(L, &Clone->c.upvals[i], &Source->c.upvals[i]);
        }
        else
        {
            Clone = luaF_newLclosure(L, Source->nupvalues, Source->env, Source->l.p);
            Clone->stacksize = Source->stacksize;
            Clone->preload = Source->preload;

            for (int i = 0; i < Source->nupvalues; ++i)
                setobj2n(L, &Clone->l.uprefs[i], &Source->l.uprefs[i]);
        }

        return Clone;
    }

    inline void PushClone(lua_State* L, Closure* Source)
    {
        PushClosure(L, CloneClosure(L, Source));
    }

    inline bool CopyClosureBody(lua_State* L, Closure* Target, Closure* Source)
    {
        if (Target->isC != Source->isC || Source->nupvalues > Target->nupvalues)
            return false;

        const int allocated = Target->nupvalues;

        Target->stacksize = Source->stacksize;
        Target->preload = Source->preload;
        Target->env = Source->env;

        if (Source->isC)
        {
            Target->c.f = Source->c.f;
            Target->c.cont = static_cast<lua_Continuation>(Source->c.cont);
            Target->c.debugname = Source->c.debugname;

            for (int i = 0; i < Source->nupvalues; ++i)
                setobj2n(L, &Target->c.upvals[i], &Source->c.upvals[i]);
            for (int i = Source->nupvalues; i < allocated; ++i)
                setnilvalue(&Target->c.upvals[i]);
        }
        else
        {
            Target->l.p = Source->l.p;

            for (int i = 0; i < Source->nupvalues; ++i)
                setobj2n(L, &Target->l.uprefs[i], &Source->l.uprefs[i]);
            for (int i = Source->nupvalues; i < allocated; ++i)
                setnilvalue(&Target->l.uprefs[i]);
        }

        return true;
    }

    int HookCDispatcher(lua_State* L)
    {
        Closure* Current = curr_func(L);
        auto It = HookRecords.find(Current);
        if (It == HookRecords.end())
            luaL_error(L, "hookfunction: missing hook");

        return CallReferencedFunction(L, It->second.HookRef);
    }

    inline int HookFunctionAt(lua_State* L, int TargetIndex, int HookIndex)
    {
        TargetIndex = lua_absindex(L, TargetIndex);
        HookIndex = lua_absindex(L, HookIndex);

        Closure* Target = ToClosure(L, TargetIndex);
        Closure* Hook = ToClosure(L, HookIndex);

        if (!Target->isC && (Hook->isC || Hook->nupvalues > Target->nupvalues))
            luaL_error(L, "hookfunction: incompatible Luau closure layout");

        HookRecord& Record = HookRecords[Target];
        if (Record.OriginalRef == LUA_NOREF)
        {
            PushClone(L, Target);
            Record.OriginalRef = lua_ref(L, -1);
            lua_pop(L, 1);
        }
        else
        {
            lua_getref(L, Record.OriginalRef);
            Closure* Original = ToClosure(L, -1);

            if (Hook == Original)
            {
                if (!CopyClosureBody(L, Target, Original))
                {
                    lua_pop(L, 1);
                    luaL_error(L, "hookfunction: incompatible Luau closure layout");
                }

                if (Record.HookRef != LUA_NOREF)
                    lua_unref(L, Record.HookRef);

                lua_unref(L, Record.OriginalRef);
                HookRecords.erase(Target);
                ExecutorClosures.erase(Target);
                return 1;
            }

            lua_pop(L, 1);

            if (Record.HookRef != LUA_NOREF)
            {
                lua_unref(L, Record.HookRef);
                Record.HookRef = LUA_NOREF;
            }
        }

        lua_pushvalue(L, HookIndex);
        Record.HookRef = lua_ref(L, -1);
        lua_pop(L, 1);

        if (Target->isC)
        {
            Target->c.f = HookCDispatcher;
            Target->c.cont = DispatchContinuation;
            Target->c.debugname = luaS_new(L, "hookfunction");
        }
        else
        {
            if (!CopyClosureBody(L, Target, Hook))
                luaL_error(L, "hookfunction: failed to apply Lua hook wrapper");
        }

        SharedVariables::ExecutorClosures.insert(Target);
        lua_getref(L, Record.OriginalRef);
        return 1;
    }

    int checkcaller(lua_State* L) {
        lua_pushboolean(L, IsExecutorThread(L));
        return 1;
    }

    inline bool EncodeShippedOpcodes(Proto* root)
    {
        const auto table = reinterpret_cast<const uint8_t*>(Offsets::OpcodeLookupTable);
        if (!root || !table)
            return false;

        std::vector<Proto*> stack{ root };
        std::unordered_set<Proto*> seen;

        while (!stack.empty())
        {
            Proto* p = stack.back();
            stack.pop_back();

            if (!p || !seen.insert(p).second)
                continue;

            for (int i = 0; i < p->sizecode;)
            {
                const uint8_t shipped = static_cast<uint8_t>(LUAU_INSN_OP(p->code[i]));
                const uint8_t op = static_cast<uint8_t>(shipped * 203);
                if (op >= LOP__COUNT)
                    return false;

                const int length = Luau::getOpLength(static_cast<LuauOpcode>(op));
                if (length <= 0 || i + length > p->sizecode)
                    return false;

                p->code[i] = (p->code[i] & ~0xFFu) | table[shipped];
                i += length;
            }

            for (int i = 0; i < p->sizep; ++i)
                stack.push_back(p->p[i]);
        }

        return true;
    }

    int getscriptclosure(lua_State* L)
    {
        luaL_checktype(L, 1, LUA_TUSERDATA);

        std::string className;
        if (!GetInstanceClassName(L, 1, className))
            return ScriptError(L, "Invalid Instance");

        if (className != "ModuleScript" && className != "LocalScript" &&
            className != "Script" && className != "CoreScript")
            return ScriptError(L, "Script expected");

        void* userdata = lua_touserdata(L, 1);
        if (!userdata)
            return ScriptError(L, "Invalid Instance");

        const std::uintptr_t script = *reinterpret_cast<const uintptr_t*>(userdata);

        if (!script || !HelperFuncs::CheckMemory(script))
            return ScriptError(L, "Invalid Instance");

        std::string bytecode;
        if (!FetchOrCompileBytecode(L, 1, script, className, bytecode) || bytecode.empty())
        {
            lua_pushnil(L);
            return 1;
        }

        lua_State* thread = lua_newthread(L);
        if (!thread)
            return ScriptError(L, "Failed to create thread");

        const int thread_ref = lua_ref(L, -1);
        lua_pop(L, 1);

        luaL_sandboxthread(thread);
        TaskScheduler::SetIdentity(thread, 8, false);

        lua_pushvalue(L, 1);
        lua_xmove(L, thread, 1);
        lua_setglobal(thread, "script");

        const int loadResult = luau_load(thread, "getscriptclosure", bytecode.data(), bytecode.size(), 0);

        if (loadResult != LUA_OK)
        {
            lua_settop(thread, 0);
            lua_unref(L, thread_ref);
            lua_pushnil(L);
            return 1;
        }

        if (!lua_isLfunction(thread, -1))
        {

            lua_settop(thread, 0);
            lua_unref(L, thread_ref);
            lua_pushnil(L);
            return 1;
        }

        Closure* closure = clvalue(luaA_toobject(thread, -1));

        if (!closure || closure->isC || !closure->l.p)
        {
            lua_settop(thread, 0);
            lua_unref(L, thread_ref);
            lua_pushnil(L);
            return 1;
        }

        if (!EncodeShippedOpcodes(closure->l.p))
        {
            lua_settop(thread, 0);
            lua_unref(L, thread_ref);
            lua_pushnil(L);
            return 1;
        }

        TaskScheduler::SetProtoCapabilities(closure->l.p, &MaxCapabilities);

        lua_xmove(thread, L, 1);
        lua_settop(thread, 0);
        lua_unref(L, thread_ref);

        return 1;
    }

    typedef void(__fastcall* PushInstanceWeakPtrT)(lua_State* L, std::weak_ptr<uintptr_t>);
    static PushInstanceWeakPtrT PushInstanceWeakPtr = (PushInstanceWeakPtrT)Roblox::PushInstance;
    inline int getscriptthread(lua_State* L)
    {
        luaL_checktype(L, 1, LUA_TUSERDATA);

        const auto script = *reinterpret_cast<std::uintptr_t*>(lua_touserdata(L, 1));
        lua_State* found = nullptr;

        for (lua_Page* cur = script ? L->global->allgcopages : nullptr; cur && !found;) {
            lua_Page* next = luaM_getnextpage(cur);

            char* start;
            char* end;
            int busy;
            int block;
            luaM_getpagewalkinfo(cur, &start, &end, &busy, &block);

            for (char* pos = start; pos != end; pos += block) {
                auto gco = reinterpret_cast<GCObject*>(pos);
                if (!gco || isdead(L->global, gco) || gco->gch.tt != LUA_TTHREAD)
                    continue;

                auto thread = reinterpret_cast<lua_State*>(pos);
                if (thread == L || !thread->userdata || thread->userdata->source.expired())
                    continue;
                if (thread->userdata->capabilities == MaxCapabilities)
                    continue;
                if ((uintptr_t)thread->userdata->source.lock().get() != script)
                    continue;

                found = thread;
                break;
            }

            cur = next;
        }

        if (!found) {
            lua_pushnil(L);
            return 1;
        }

        luaC_threadbarrier(L);
        setthvalue(L, L->top, found);
        incr_top(L);
        return 1;
    }

    inline int getscriptfromthread(lua_State* L)
    {
        luaL_checktype(L, 1, LUA_TTHREAD);

        lua_State* thread = lua_tothread(L, 1);
        if (thread)
            thread = Oth::Original(thread);

        if (!thread || !thread->userdata || thread->userdata->source.expired()) {
            lua_pushnil(L);
            return 1;
        }

        auto script = thread->userdata->source.lock();
        PushInstanceWeakPtr(L, script);

        return 1;
    }

    inline int getrunningscriptsv2(lua_State* L)
    {
        lua_newtable(L);
        int tableIndex = lua_gettop(L);
        int i = 0;
        lua_newtable(L);
        int deferredIndex = lua_gettop(L);
        int deferred = 0;
        std::unordered_set<std::uintptr_t**> running_scripts = { };
        lua_rawcheckstack(L, 5);
        luaC_threadbarrier(L);
        lua_pushvalue(L, LUA_REGISTRYINDEX);
        lua_pushnil(L);
        while (lua_next(L, -2)) {
            if (lua_isthread(L, -1)) {
                lua_State* thread = lua_tothread(L, -1);
                if (thread && thread->userdata != nullptr && !thread->userdata->source.expired()) {
                    if (thread->global->mainthread == L->global->mainthread) {
                        const auto script = *reinterpret_cast<std::shared_ptr<std::uintptr_t*>*>(&thread->userdata->source);
                        if (!running_scripts.contains(script.get())) {
                            Roblox::InstanceBridge::Shared(L, script);
                            lua_getfield(L, -1, "ClassName");
                            const char* className = lua_tostring(L, -1);
                            lua_pop(L, 1);

                            bool isValidScript = false;

                            if (className) {
                                if (strcmp(className, "LocalScript") == 0 || strcmp(className, "ModuleScript") == 0) {
                                    isValidScript = true;
                                }
                                else if (strcmp(className, "Script") == 0) {
                                    lua_getfield(L, -1, "RunContext");
                                    if (lua_type(L, -1) != LUA_TNIL) {
                                        if (lua_isnumber(L, -1)) {
                                            int runContext = lua_tointeger(L, -1);
                                            isValidScript = (runContext == 2);
                                        }
                                        else {
                                            lua_getfield(L, -1, "Value");
                                            if (lua_isnumber(L, -1)) {
                                                int runContext = lua_tointeger(L, -1);
                                                isValidScript = (runContext == 2);
                                            }
                                            lua_pop(L, 1);
                                        }
                                    }
                                    lua_pop(L, 1);
                                }
                            }

                            if (isValidScript) {
                                running_scripts.insert(script.get());
                                if (strcmp(className, "Script") == 0)
                                    lua_rawseti(L, deferredIndex, ++deferred);
                                else
                                    lua_rawseti(L, tableIndex, ++i);
                            }
                            else {
                                lua_pop(L, 1);
                            }
                        }
                    }
                }
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);

        for (int j = 1; j <= deferred; ++j)
        {
            lua_rawgeti(L, deferredIndex, j);
            lua_rawseti(L, tableIndex, ++i);
        }
        lua_pop(L, 1);
        return 1;
    }

    int getloadedmodulesv3(lua_State* L)
    {
        lua_newtable(L);

        typedef struct {
            lua_State* pLua;
            int itemsFound;
            std::map< uintptr_t, bool > map;
        } GCOContext;

        auto gcCtx = GCOContext{ L, 0 };

        const auto ullOldThreshold = L->global->GCthreshold;
        L->global->GCthreshold = SIZE_MAX;

        luaM_visitgco(L, &gcCtx, [](void* ctx, lua_Page* pPage, GCObject* pGcObj) -> bool {
            const auto pCtx = static_cast<GCOContext*>(ctx);
            const auto ctxL = pCtx->pLua;

            if (isdead(ctxL->global, pGcObj))
                return false;

            if (const auto gcObjType = pGcObj->gch.tt;
                gcObjType == LUA_TFUNCTION) {
                ctxL->top->value.gc = pGcObj;
                ctxL->top->tt = gcObjType;
                ctxL->top++;

                lua_getfenv(ctxL, -1);

                if (!lua_isnil(ctxL, -1)) {
                    lua_getfield(ctxL, -1, "script");

                    if (lua_isuserdata(ctxL, -1)) {
                        uintptr_t script_addr = *static_cast<uintptr_t*>(lua_touserdata(ctxL, -1));

                        lua_getfield(ctxL, -1, "ClassName");
                        const char* class_name = lua_isstring(ctxL, -1) ? lua_tostring(ctxL, -1) : nullptr;
                        const bool is_module = class_name && strcmp(class_name, "ModuleScript") == 0;
                        lua_pop(ctxL, 1);

                        if (script_addr && pCtx->map.find(script_addr) == pCtx->map.end() && is_module) {
                            pCtx->map.insert({ script_addr, true });
                            lua_rawseti(ctxL, -4, ++pCtx->itemsFound);
                        }
                        else {
                            lua_pop(ctxL, 1);
                        }
                    }
                    else {
                        lua_pop(ctxL, 1);
                    }
                }

                lua_pop(ctxL, 2);
            }
            return false;
            });

        L->global->GCthreshold = ullOldThreshold;

        return 1;
    }

    inline void RegisterLibrary(lua_State* L)
    {
        if (!L) return;

        Utils::AddFunction(L, "getloadedmodules", getloadedmodules);
        Utils::AddFunction(L, "getrunningscripts", getrunningscriptsv2);
        Utils::AddFunction(L, "checkcaller", checkcaller);
        Utils::AddFunction(L, "getscriptfromthread", getscriptfromthread);
        Utils::AddFunction(L, "getscripts", getscripts);
        Utils::AddFunction(L, "filtergc", filtergc);
        Utils::AddFunction(L, "getrenv", getrenv);
        if (Debug::RedirectEngineBuiltin(L, nullptr, "getfenv", Debug::ShieldedBuiltin<getfenv>))
            lua_setglobal(L, "getfenv");
        else
            Utils::AddFunction(L, "getfenv", getfenv);
        Utils::AddFunction(L, "getscripthash", getscripthash);
        Utils::AddFunction(L, "getcallingscript", getcallingscript);
        Utils::AddFunction(L, "getgc", getgc);
        Utils::AddFunction(L, "getallthreads", getallthreads);
        Utils::AddFunction(L, "isexecutorthread", isexecutorthread);
        Utils::AddFunction(L, "getscriptthread", getscriptthread);
        Utils::AddFunction(L, "getfunctionbytecode", FunctionBytecode::getfunctionbytecode);
        Utils::AddFunction(L, "getsenv", getsenv);
        Utils::AddFunction(L, "getmenv", getmenv);
        Utils::AddFunction(L, "getscriptclosure", getscriptclosure);
        Utils::AddFunction(L, "getscriptfunction", getscriptclosure);
        Utils::AddFunction(L, "getscriptbytecode", getscriptbytecode);
        Utils::AddFunction(L, "dumpstring", getscriptbytecode);
    }
}
