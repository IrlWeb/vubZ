#pragma once

#include <Windows.h>
#include <bcrypt.h>
#include <string>
#include <vector>
#include <cstdio>

#pragma comment(lib, "bcrypt.lib")

namespace ExecutionGuard
{
    inline constexpr const char* kTokenSalt = "supQ.v1.exec";

    inline constexpr const char* kTokenPrefix = "--@";

    namespace Detail
    {
        inline std::string Sha256Hex(const std::vector<unsigned char>& Input)
        {
            BCRYPT_ALG_HANDLE Algorithm = nullptr;
            BCRYPT_HASH_HANDLE Hash = nullptr;
            std::string Result;

            if (BCryptOpenAlgorithmProvider(&Algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
                return Result;

            DWORD HashLength = 0, Copied = 0;
            if (BCryptGetProperty(Algorithm, BCRYPT_HASH_LENGTH,
                    reinterpret_cast<PUCHAR>(&HashLength), sizeof(HashLength), &Copied, 0) == 0 &&
                BCryptCreateHash(Algorithm, &Hash, nullptr, 0, nullptr, 0, 0) == 0)
            {
                std::vector<unsigned char> Digest(HashLength);
                BCryptHashData(Hash, const_cast<PUCHAR>(Input.data()),
                    static_cast<ULONG>(Input.size()), 0);
                BCryptFinishHash(Hash, Digest.data(), HashLength, 0);

                Result.resize(static_cast<size_t>(HashLength) * 2);
                for (DWORD i = 0; i < HashLength; ++i)
                    std::snprintf(&Result[static_cast<size_t>(i) * 2], 3, "%02x", Digest[i]);

                BCryptDestroyHash(Hash);
            }

            BCryptCloseAlgorithmProvider(Algorithm, 0);
            return Result;
        }

        inline std::string ComputeToken()
        {
            std::vector<unsigned char> Material;
            auto Append = [&Material](const void* Data, size_t Size)
            {
                const auto* Bytes = static_cast<const unsigned char*>(Data);
                Material.insert(Material.end(), Bytes, Bytes + Size);
            };

            HW_PROFILE_INFOA Profile{};
            if (GetCurrentHwProfileA(&Profile))
            {
                for (char c : std::string(Profile.szHwProfileGuid))
                    if (c != '{' && c != '}' && c != '-')
                        Material.push_back(static_cast<unsigned char>(c));
            }
            Material.push_back('|');

            char Computer[MAX_COMPUTERNAME_LENGTH + 1]{};
            DWORD ComputerLength = MAX_COMPUTERNAME_LENGTH + 1;
            if (GetComputerNameA(Computer, &ComputerLength))
                Append(Computer, ComputerLength);
            Material.push_back('|');

            DWORD VolumeSerial = 0;
            if (GetVolumeInformationA("C:\\", nullptr, 0, &VolumeSerial, nullptr, nullptr, nullptr, 0))
                Append(&VolumeSerial, sizeof(VolumeSerial));
            Material.push_back('|');

            Append(kTokenSalt, std::char_traits<char>::length(kTokenSalt));

            return Sha256Hex(Material);
        }
    }

    inline const std::string& Token()
    {
        static const std::string Value = Detail::ComputeToken();
        return Value;
    }

    inline std::string Wrap(const std::string& Source)
    {
        return std::string(kTokenPrefix) + Token() + "\n" + Source;
    }

    inline bool Consume(std::string& Source)
    {
        const std::string Marker = kTokenPrefix;
        const std::string& Expected = Token();

        if (Expected.empty())
            return false;

        size_t Begin = 0;
        while (Begin < Source.size() && (Source[Begin] == ' ' || Source[Begin] == '\t' ||
                                         Source[Begin] == '\r' || Source[Begin] == '\n'))
            ++Begin;

        if (Source.compare(Begin, Marker.size(), Marker) != 0)
            return false;

        const size_t TokenStart = Begin + Marker.size();
        if (Source.size() < TokenStart + Expected.size())
            return false;

        unsigned Diff = 0;
        for (size_t i = 0; i < Expected.size(); ++i)
            Diff |= static_cast<unsigned char>(Source[TokenStart + i]) ^
                    static_cast<unsigned char>(Expected[i]);
        if (Diff != 0)
            return false;

        size_t LineEnd = Source.find('\n', TokenStart);
        LineEnd = (LineEnd == std::string::npos) ? Source.size() : LineEnd + 1;
        Source.erase(0, LineEnd);
        return true;
    }
}
