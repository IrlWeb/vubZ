#pragma once

#define NOMINMAX
#include <Windows.h>
#include <bcrypt.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include <Security/VMProtectShim.h>
#include <Config/Secrets.hpp>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "psapi.lib")

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((NTSTATUS)0)
#endif

namespace PipeCrypto
{
    constexpr size_t kTokenLen = 32;
    constexpr size_t kAesKeyLen = 16;
    constexpr size_t kNonceLen = 16;
    constexpr size_t kAesBlockLen = 16;

    inline void MaterialiseToken(uint8_t out[kTokenLen])
    {
        VMProtectBeginUltra("brand.pipe.token");
        const char* hex = SUPQ_PIPE_AES_TOKEN_HEX;
        auto nib = [](char c) -> uint8_t {
            if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
            if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
            if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
            return 0;
        };
        for (size_t i = 0; i < kTokenLen; ++i)
            out[i] = static_cast<uint8_t>((nib(hex[i * 2]) << 4) | nib(hex[i * 2 + 1]));
        VMProtectEnd();
    }

    inline bool Contains(const uint8_t* haystack, size_t hlen,
                         const uint8_t* needle, size_t nlen)
    {
        if (hlen < nlen) return false;
        const uint8_t first = needle[0];
        const size_t end = hlen - nlen;
        for (size_t i = 0; i <= end; ++i)
        {
            if (haystack[i] != first) continue;
            if (std::memcmp(haystack + i, needle, nlen) == 0)
                return true;
        }
        return false;
    }

    inline bool ClientHasToken(DWORD pid, const uint8_t* token)
    {
        HANDLE proc = OpenProcess(
            PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
        if (!proc)
        {
            proc = OpenProcess(
                PROCESS_VM_READ | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        }
        if (!proc) return false;

        bool found = false;
        MEMORY_BASIC_INFORMATION mbi{};
        uint8_t* addr = nullptr;

        constexpr size_t kMaxRegionToScan = 64 * 1024 * 1024;
        constexpr size_t kChunk = 0x10000;
        std::vector<uint8_t> window(kChunk + kTokenLen - 1);

        while (VirtualQueryEx(proc, addr, &mbi, sizeof(mbi)) == sizeof(mbi) && !found)
        {
            const bool committed = (mbi.State == MEM_COMMIT);
            const bool readable = (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) != 0;
            const bool noGuard = (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0;

            if (committed && readable && noGuard && mbi.RegionSize <= kMaxRegionToScan)
            {
                const auto regionBase = static_cast<const uint8_t*>(mbi.BaseAddress);
                const size_t regionSize = mbi.RegionSize;

                for (size_t off = 0; off + kTokenLen <= regionSize && !found; off += kChunk)
                {
                    const size_t want = (off + kChunk + kTokenLen - 1 <= regionSize)
                                            ? (kChunk + kTokenLen - 1)
                                            : (regionSize - off);
                    SIZE_T got = 0;
                    const bool ok = ReadProcessMemory(proc, regionBase + off, window.data(), want, &got);
                    if ((ok || GetLastError() == ERROR_PARTIAL_COPY) && got >= kTokenLen)
                    {
                        if (Contains(window.data(), got, token, kTokenLen))
                            found = true;
                    }
                }
            }

            addr = static_cast<uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
        }

        CloseHandle(proc);
        return found;
    }

    inline bool DecryptCtr(const uint8_t key[kAesKeyLen],
                           const uint8_t nonce[kNonceLen],
                           uint8_t* data, size_t len)
    {
        BCRYPT_ALG_HANDLE hAlg = nullptr;
        BCRYPT_KEY_HANDLE hKey = nullptr;
        NTSTATUS s = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, nullptr, 0);
        if (s != STATUS_SUCCESS) return false;

        s = BCryptSetProperty(hAlg, BCRYPT_CHAINING_MODE,
                              (PUCHAR)BCRYPT_CHAIN_MODE_ECB,
                              sizeof(BCRYPT_CHAIN_MODE_ECB), 0);
        if (s != STATUS_SUCCESS) { BCryptCloseAlgorithmProvider(hAlg, 0); return false; }

        s = BCryptGenerateSymmetricKey(hAlg, &hKey, nullptr, 0,
                                       const_cast<PUCHAR>(key), (ULONG)kAesKeyLen, 0);
        if (s != STATUS_SUCCESS) { BCryptCloseAlgorithmProvider(hAlg, 0); return false; }

        uint8_t counterBlock[kAesBlockLen];
        std::memcpy(counterBlock, nonce, kAesBlockLen);

        uint8_t keystream[kAesBlockLen];
        uint32_t counter = 0;
        size_t off = 0;
        bool ok = true;

        while (off < len)
        {
            counterBlock[12] = static_cast<uint8_t>((counter >> 24) & 0xFF);
            counterBlock[13] = static_cast<uint8_t>((counter >> 16) & 0xFF);
            counterBlock[14] = static_cast<uint8_t>((counter >>  8) & 0xFF);
            counterBlock[15] = static_cast<uint8_t>((counter >>  0) & 0xFF);

            ULONG produced = 0;
            s = BCryptEncrypt(hKey, counterBlock, (ULONG)kAesBlockLen, nullptr,
                              nullptr, 0, keystream, (ULONG)kAesBlockLen,
                              &produced, 0);
            if (s != STATUS_SUCCESS || produced != kAesBlockLen) { ok = false; break; }

            const size_t chunk = (len - off) < kAesBlockLen ? (len - off) : kAesBlockLen;
            for (size_t i = 0; i < chunk; ++i)
                data[off + i] ^= keystream[i];

            off += chunk;
            ++counter;
        }

        BCryptDestroyKey(hKey);
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return ok;
    }
}
