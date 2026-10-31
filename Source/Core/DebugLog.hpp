#pragma once

#include <Windows.h>
#include "Update/Offsets.hpp"
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <mutex>

#ifndef SUPQ_LOG_TO_CONSOLE
#define SUPQ_LOG_TO_CONSOLE 0
#endif

namespace DebugLog
{
    enum : int
    {
        LevelPrint = 0,
        LevelInfo = 1,
        LevelWarning = 2,
        LevelError = 3,
    };

    namespace Detail
    {
        inline std::atomic<unsigned> Sequence{ 0 };
        inline std::mutex FileMutex;

        inline bool SafeConsoleWrite(int Level, const char* Line)
        {
            __try
            {
                if (!Offsets::Print)
                    return false;

                Roblox::Print(Level, "%s", Line);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        inline void AppendToFile(const char* Line)
        {
            static wchar_t Path[MAX_PATH]{};
            static bool Resolved = false;

            std::lock_guard<std::mutex> Lock(FileMutex);

            if (!Resolved)
            {
                Resolved = true;

                wchar_t AppData[MAX_PATH]{};
                if (GetEnvironmentVariableW(L"APPDATA", AppData, MAX_PATH) == 0 || AppData[0] == L'\0')
                {
                    GetTempPathW(MAX_PATH, AppData);
                }

                wchar_t Dir[MAX_PATH]{};
                _snwprintf_s(Dir, MAX_PATH, _TRUNCATE, L"%s\\supQ", AppData);
                CreateDirectoryW(Dir, nullptr);

                _snwprintf_s(Path, MAX_PATH, _TRUNCATE, L"%s\\supQ_log.txt", Dir);
                DeleteFileW(Path);

                WIN32_FIND_DATAW fd{};
                wchar_t search[MAX_PATH]{};
                _snwprintf_s(search, MAX_PATH, _TRUNCATE, L"%s\\supQ_log_*.txt", Dir);
                HANDLE hFind = FindFirstFileW(search, &fd);
                if (hFind != INVALID_HANDLE_VALUE)
                {
                    do {
                        wchar_t oldFile[MAX_PATH]{};
                        _snwprintf_s(oldFile, MAX_PATH, _TRUNCATE, L"%s\\%s", Dir, fd.cFileName);
                        DeleteFileW(oldFile);
                    } while (FindNextFileW(hFind, &fd));
                    FindClose(hFind);
                }
            }

            char Buffer[1280]{};
            const int Length = std::snprintf(Buffer, sizeof(Buffer), "%s\r\n", Line);
            OutputDebugStringA(Buffer);

            if (!Path[0])
                return;

            HANDLE File = CreateFileW(Path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (File == INVALID_HANDLE_VALUE)
                return;

            if (Length > 0)
            {
                DWORD Ignored = 0;
                WriteFile(File, Buffer, static_cast<DWORD>(Length), &Ignored, nullptr);
            }

            CloseHandle(File);
        }
    }

    inline void WriteAt(int Level, const char* Format, ...)
    {
        char Body[1024]{};

        va_list Args;
        va_start(Args, Format);
        const int Written = std::vsnprintf(Body, sizeof(Body), Format, Args);
        va_end(Args);

        if (Written < 0)
            return;

        const unsigned Seq = Detail::Sequence.fetch_add(1) + 1;

        char Line[1152]{};
        std::snprintf(Line, sizeof(Line), "[supQ %03u] %s%s", Seq,
            Level >= LevelError ? "ERROR: " : "", Body);

        Detail::AppendToFile(Line);

#if SUPQ_LOG_TO_CONSOLE
        if (!Detail::SafeConsoleWrite(LevelWarning, Line))
        {
            char Fallback[1184]{};
            std::snprintf(Fallback, sizeof(Fallback), "%s\n", Line);
            OutputDebugStringA(Fallback);
        }
#endif
    }

    inline void Write(const char* Format, ...)
    {
        char Body[1024]{};

        va_list Args;
        va_start(Args, Format);
        const int Written = std::vsnprintf(Body, sizeof(Body), Format, Args);
        va_end(Args);

        if (Written < 0)
            return;

        WriteAt(LevelWarning, "%s", Body);
    }

    inline bool TryReadU32(uintptr_t Address, unsigned long& Out)
    {
        __try
        {
            Out = *reinterpret_cast<const unsigned long*>(Address);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    inline bool TryReadBytes(uintptr_t Address, unsigned char* Out, unsigned Count)
    {
        __try
        {
            for (unsigned i = 0; i < Count; ++i)
                Out[i] = *reinterpret_cast<const unsigned char*>(Address + i);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    inline void DumpObject(const char* Label, uintptr_t Address, unsigned Count)
    {
        if (!Address)
        {
            WriteAt(LevelError, "  dump %s: NULL", Label);
            return;
        }

        unsigned char Bytes[0x100]{};
        if (Count > sizeof(Bytes))
            Count = sizeof(Bytes);

        if (!TryReadBytes(Address, Bytes, Count))
        {
            WriteAt(LevelError, "  dump %s @ 0x%llX: UNREADABLE", Label,
                (unsigned long long)Address);
            return;
        }

        WriteAt(LevelError, "  dump %s @ 0x%llX:", Label, (unsigned long long)Address);

        for (unsigned Row = 0; Row < Count; Row += 8)
        {
            unsigned long long Qword = 0;
            for (int i = 7; i >= 0; --i)
                Qword = (Qword << 8) | Bytes[Row + i];

            WriteAt(LevelError,
                "    +0x%02X: %02X %02X %02X %02X %02X %02X %02X %02X   (qword 0x%llX)",
                Row, Bytes[Row + 0], Bytes[Row + 1], Bytes[Row + 2], Bytes[Row + 3],
                Bytes[Row + 4], Bytes[Row + 5], Bytes[Row + 6], Bytes[Row + 7], Qword);
        }
    }

    inline void DumpGameLoadedCandidates(uintptr_t DataModel)
    {
        static const uintptr_t Candidates[] = { 0x5D0, 0x5D8, 0x630 };

        for (uintptr_t Offset : Candidates)
        {
            unsigned long Value = 0;
            if (TryReadU32(DataModel + Offset, Value))
                WriteAt(LevelError, "  GameLoaded candidate 0x%03llX = 0x%08lX (low byte %u)",
                    (unsigned long long)Offset, Value, (unsigned)(Value & 0xFF));
            else
                WriteAt(LevelError, "  GameLoaded candidate 0x%03llX = <unreadable>",
                    (unsigned long long)Offset);
        }
    }
}

#ifndef SUPQ_VERBOSE_BRINGUP
#define SUPQ_VERBOSE_BRINGUP 0
#endif

#if SUPQ_VERBOSE_BRINGUP
#define SUPQ_LOG(...)        ::DebugLog::Write(__VA_ARGS__)
#define SUPQ_LOG_ERR(...)    ::DebugLog::WriteAt(::DebugLog::LevelError, __VA_ARGS__)
#define SUPQ_LOG_OK(...)     ::DebugLog::WriteAt(::DebugLog::LevelPrint, __VA_ARGS__)
#define SUPQ_DUMP(label, addr, count) ::DebugLog::DumpObject((label), (addr), (count))
#define SUPQ_DUMP_GAMELOADED(dm)      ::DebugLog::DumpGameLoadedCandidates((dm))
#else
#define SUPQ_LOG(...)        ((void)0)
#define SUPQ_LOG_ERR(...)    ((void)0)
#define SUPQ_LOG_OK(...)     ((void)0)
#define SUPQ_DUMP(label, addr, count) ((void)0)
#define SUPQ_DUMP_GAMELOADED(dm)      ((void)0)
#endif
