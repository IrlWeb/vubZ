#pragma once

#include <Core/CallTrace.hpp>

#include <Windows.h>
#include <atomic>
#include <cstdio>
#include <cwchar>

namespace CrashLog
{
    namespace Detail
    {
        inline wchar_t Path[MAX_PATH]{};

        constexpr size_t MaxSites = 256;
        inline std::atomic<uintptr_t> Sites[MaxSites]{};
        inline std::atomic<unsigned> Hits[MaxSites]{};

        inline void Append(const char* Text, int Length)
        {
            if (!Path[0] || Length <= 0)
                return;

            HANDLE File = CreateFileW(Path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (File == INVALID_HANDLE_VALUE)
                return;

            DWORD Ignored = 0;
            WriteFile(File, Text, static_cast<DWORD>(Length), &Ignored, nullptr);
            CloseHandle(File);
        }

        inline bool Describe(uintptr_t Address, char* Out, size_t Size)
        {
            HMODULE Module = nullptr;
            wchar_t Name[MAX_PATH]{};
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(Address), &Module) &&
                GetModuleFileNameW(Module, Name, MAX_PATH))
            {
                const wchar_t* Base = wcsrchr(Name, L'\\');
                std::snprintf(Out, Size, "%ls+0x%llX", Base ? Base + 1 : Name,
                    static_cast<unsigned long long>(Address - reinterpret_cast<uintptr_t>(Module)));
                return true;
            }

            std::snprintf(Out, Size, "0x%llX", static_cast<unsigned long long>(Address));
            return false;
        }

        inline bool IsFault(DWORD Code)
        {
            switch (Code)
            {
            case EXCEPTION_ACCESS_VIOLATION:
            case EXCEPTION_IN_PAGE_ERROR:
            case EXCEPTION_GUARD_PAGE:
            case EXCEPTION_ILLEGAL_INSTRUCTION:
            case EXCEPTION_PRIV_INSTRUCTION:
            case EXCEPTION_INT_DIVIDE_BY_ZERO:
            case EXCEPTION_STACK_OVERFLOW:
            case 0xC0000374:
            case 0xC0000409:
                return true;
            default:
                return false;
            }
        }

        inline int AppendUnwind(const CONTEXT* Source, char* Line, int Length, int Size)
        {
            CONTEXT Context = *Source;
            DWORD64 ImageBase = 0;

            if (!RtlLookupFunctionEntry(Context.Rip, &ImageBase, nullptr))
            {
                const NT_TIB* Tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
                const uintptr_t High = reinterpret_cast<uintptr_t>(Tib->StackBase);

                bool Seeded = false;
                for (uintptr_t Slot = Context.Rsp; Slot + 8 <= High && Slot < Context.Rsp + 0x200; Slot += 8)
                {
                    __try
                    {
                        const DWORD64 Value = *reinterpret_cast<const DWORD64*>(Slot);
                        if (Value > 0x10000 && RtlLookupFunctionEntry(Value, &ImageBase, nullptr))
                        {
                            Context.Rip = Value;
                            Context.Rsp = Slot + 8;
                            Seeded = true;
                        }
                    }
                    __except (EXCEPTION_EXECUTE_HANDLER)
                    {
                    }

                    if (Seeded)
                        break;
                }

                if (!Seeded)
                    return Length;
            }

            for (int Frame = 0; Frame < 12 && Context.Rip; ++Frame)
            {
                char Name[320];
                Describe(static_cast<uintptr_t>(Context.Rip), Name, sizeof(Name));

                const int Added = std::snprintf(Line + Length, Size - Length,
                    "    #%d %s\r\n", Frame, Name);
                if (Added <= 0 || Length + Added >= Size)
                    break;
                Length += Added;

                DWORD64 FrameBase = 0;
                PRUNTIME_FUNCTION Function = RtlLookupFunctionEntry(Context.Rip, &FrameBase, nullptr);
                if (!Function)
                    break;

                __try
                {
                    PVOID HandlerData = nullptr;
                    DWORD64 Establisher = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, FrameBase, Context.Rip, Function,
                        &Context, &HandlerData, &Establisher, nullptr);
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    break;
                }
            }

            return Length;
        }

        inline LONG CALLBACK Handler(EXCEPTION_POINTERS* Info)
        {
            const EXCEPTION_RECORD* Record = Info->ExceptionRecord;
            if (!Record || !IsFault(Record->ExceptionCode))
                return EXCEPTION_CONTINUE_SEARCH;

            CallTrace::Flush();

            const uintptr_t Rip = reinterpret_cast<uintptr_t>(Record->ExceptionAddress);

            size_t Slot = MaxSites;
            for (size_t i = 0; i < MaxSites; ++i)
            {
                uintptr_t Current = Sites[i].load(std::memory_order_relaxed);
                if (Current == 0 && Sites[i].compare_exchange_strong(Current, Rip))
                    Current = Rip;
                if (Current == Rip)
                {
                    Slot = i;
                    break;
                }
            }
            if (Slot == MaxSites)
                return EXCEPTION_CONTINUE_SEARCH;

            const unsigned Hit = Hits[Slot].fetch_add(1, std::memory_order_relaxed) + 1;
            if (Hit & (Hit - 1))
                return EXCEPTION_CONTINUE_SEARCH;

            char Where[320];
            Describe(Rip, Where, sizeof(Where));

            char Target[64] = "";
            if (Record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && Record->NumberParameters >= 2)
            {
                const ULONG_PTR Kind = Record->ExceptionInformation[0];
                std::snprintf(Target, sizeof(Target), " %s 0x%llX",
                    Kind == 0 ? "read" : Kind == 1 ? "write" : "exec",
                    static_cast<unsigned long long>(Record->ExceptionInformation[1]));
            }

            char Line[4096];
            int Length = std::snprintf(Line, sizeof(Line), "tid %lu code 0x%08lX at %s%s (hit %u)\r\n",
                GetCurrentThreadId(), Record->ExceptionCode, Where, Target, Hit);

            const CONTEXT* Context = Info->ContextRecord;
            if (Context && Length > 0 && Length < static_cast<int>(sizeof(Line)))
                Length = AppendUnwind(Context, Line, Length, static_cast<int>(sizeof(Line)));

            if (Hit == 1 && Context && Length > 0 && Length < static_cast<int>(sizeof(Line)))
            {
                struct Reg { const char* Name; DWORD64 Value; };
                const Reg Regs[] = {
                    { "rax", Context->Rax }, { "rcx", Context->Rcx }, { "rdx", Context->Rdx },
                    { "r8", Context->R8 }, { "r9", Context->R9 }, { "rbx", Context->Rbx },
                    { "rsi", Context->Rsi }, { "rdi", Context->Rdi }, { "r14", Context->R14 },
                };
                for (const Reg& R : Regs)
                {
                    char Self[320] = "";
                    char Vt[320] = "";
                    Describe(static_cast<uintptr_t>(R.Value), Self, sizeof(Self));
                    MEMORY_BASIC_INFORMATION Mbi{};
                    if (R.Value > 0x10000 && !(R.Value & 7) &&
                        VirtualQuery(reinterpret_cast<LPCVOID>(R.Value), &Mbi, sizeof(Mbi)) &&
                        Mbi.State == MEM_COMMIT && !(Mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
                    {
                        const uintptr_t First = *reinterpret_cast<const uintptr_t*>(R.Value);
                        if (First > 0x10000)
                            Describe(First, Vt, sizeof(Vt));
                    }
                    const int Added = std::snprintf(Line + Length, sizeof(Line) - Length,
                        "    %s=%s  [0]=%s\r\n", R.Name, Self, Vt[0] ? Vt : "-");
                    if (Added <= 0 || Length + Added >= static_cast<int>(sizeof(Line)))
                        break;
                    Length += Added;
                }

                PWSTR ThreadName = nullptr;
                if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &ThreadName)) && ThreadName)
                {
                    const int Added = std::snprintf(Line + Length, sizeof(Line) - Length,
                        "    thread \"%ls\"\r\n", ThreadName);
                    if (Added > 0 && Length + Added < static_cast<int>(sizeof(Line)))
                        Length += Added;
                    LocalFree(ThreadName);
                }

                const NT_TIB* Tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
                const uintptr_t Low = Context->Rsp;
                const uintptr_t High = reinterpret_cast<uintptr_t>(Tib->StackBase);

                int Shown = 0;
                for (uintptr_t Slot2 = Low; Slot2 + 8 <= High && Slot2 < Low + 0x1000 && Shown < 16; Slot2 += 8)
                {
                    const uintptr_t Value = *reinterpret_cast<const uintptr_t*>(Slot2);
                    char Name[320];
                    if (Value < 0x10000 || !Describe(Value, Name, sizeof(Name)))
                        continue;

                    const int Added = std::snprintf(Line + Length, sizeof(Line) - Length,
                        "    [rsp+0x%llX] %s\r\n", static_cast<unsigned long long>(Slot2 - Low), Name);
                    if (Added <= 0 || Length + Added >= static_cast<int>(sizeof(Line)))
                        break;
                    Length += Added;
                    ++Shown;
                }
            }

            Append(Line, Length);
            return EXCEPTION_CONTINUE_SEARCH;
        }
    }

    inline void Install()
    {
        wchar_t AppData[MAX_PATH]{};
        if (GetEnvironmentVariableW(L"APPDATA", AppData, MAX_PATH) == 0 || AppData[0] == L'\0')
        {
            GetTempPathW(MAX_PATH, AppData);
        }

        wchar_t Dir[MAX_PATH]{};
        _snwprintf_s(Dir, MAX_PATH, _TRUNCATE, L"%s\\supQ", AppData);
        CreateDirectoryW(Dir, nullptr);

        _snwprintf_s(Detail::Path, MAX_PATH, _TRUNCATE, L"%s\\supQ_crash.txt", Dir);
        DeleteFileW(Detail::Path);

        WIN32_FIND_DATAW fd{};
        wchar_t search[MAX_PATH]{};
        _snwprintf_s(search, MAX_PATH, _TRUNCATE, L"%s\\supQ_crash_*.txt", Dir);
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

        HMODULE Self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&Detail::Handler), &Self);

        char Header[128];
        const int Length = std::snprintf(Header, sizeof(Header), "fault log for pid %lu, Module.dll @ 0x%llX\r\n",
            GetCurrentProcessId(), static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(Self)));
        Detail::Append(Header, Length);

        AddVectoredExceptionHandler(1, &Detail::Handler);
    }
}
