#pragma once

#include <Windows.h>
#include <tlhelp32.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>

#ifndef SUPQ_CALL_TRACE
#define SUPQ_CALL_TRACE 0
#endif

namespace CallTrace
{
    namespace Detail
    {
        constexpr unsigned RingSize = 16384;
        constexpr unsigned RingMask = RingSize - 1;
        constexpr unsigned DetailMax = 56;
        constexpr unsigned MaxThreads = 64;
        constexpr unsigned long long StuckMs = 3000;
        constexpr unsigned long long MaxFileBytes = 32ull * 1024 * 1024;

        struct Entry
        {
            std::atomic<unsigned long long> Stamp{ 0 };
            unsigned long long Ticks = 0;
            unsigned ThreadId = 0;
            int Depth = 0;
            char Kind = 0;
            const char* Name = nullptr;
            char Detail[DetailMax] = {};
        };

        inline Entry Ring[RingSize];
        inline std::atomic<unsigned long long> Sequence{ 0 };

        struct Active
        {
            std::atomic<unsigned> ThreadId{ 0 };
            std::atomic<unsigned long long> LastTicks{ 0 };
            std::atomic<char> LastKind{ 0 };
            std::atomic<int> Depth{ 0 };
            std::atomic<bool> Reported{ false };
            const char* Name = nullptr;
            char Detail[DetailMax] = {};
        };

        inline Active Threads[MaxThreads];
        inline std::atomic<unsigned> ThreadCount{ 0 };

        inline std::atomic<bool> Enabled{ false };
        inline std::atomic<bool> Running{ false };
        inline wchar_t Path[MAX_PATH]{};
        inline unsigned long long Written = 0;

        inline thread_local int SlotIndex = -1;

        inline Active* Slot()
        {
            if (SlotIndex >= 0)
                return &Threads[SlotIndex];

            const unsigned index = ThreadCount.fetch_add(1, std::memory_order_relaxed);
            if (index >= MaxThreads)
                return nullptr;

            SlotIndex = static_cast<int>(index);
            Threads[index].ThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);
            return &Threads[index];
        }

        inline void CopyDetail(char* out, const char* detail)
        {
            if (!detail)
            {
                out[0] = '\0';
                return;
            }

            unsigned i = 0;
            for (; i + 1 < DetailMax && detail[i]; ++i)
            {
                const char c = detail[i];
                out[i] = (c >= 0x20 && c < 0x7F) ? c : '.';
            }
            out[i] = '\0';
        }

        inline void Push(char kind, const char* name, const char* detail, int depth)
        {
            const unsigned long long seq = Sequence.fetch_add(1, std::memory_order_relaxed);
            Entry& entry = Ring[seq & RingMask];

            entry.Ticks = GetTickCount64();
            entry.ThreadId = GetCurrentThreadId();
            entry.Depth = depth;
            entry.Kind = kind;
            entry.Name = name;
            CopyDetail(entry.Detail, detail);

            entry.Stamp.store(seq + 1, std::memory_order_release);
        }

        inline void Describe(uintptr_t address, char* out, size_t size)
        {
            HMODULE module = nullptr;
            wchar_t name[MAX_PATH]{};
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(address), &module) &&
                GetModuleFileNameW(module, name, MAX_PATH))
            {
                const wchar_t* base = wcsrchr(name, L'\\');
                std::snprintf(out, size, "%ls+0x%llX", base ? base + 1 : name,
                    static_cast<unsigned long long>(address - reinterpret_cast<uintptr_t>(module)));
                return;
            }

            std::snprintf(out, size, "0x%llX", static_cast<unsigned long long>(address));
        }

        inline void Append(const char* text, int length)
        {
            if (!Path[0] || length <= 0 || Written > MaxFileBytes)
                return;

            HANDLE file = CreateFileW(Path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
                return;

            DWORD written = 0;
            WriteFile(file, text, static_cast<DWORD>(length), &written, nullptr);
            CloseHandle(file);
            Written += written;
        }

        inline void Drain()
        {
            static unsigned long long Cursor = 0;
            static char LastLine[256] = {};
            static unsigned Repeats = 0;

            const unsigned long long end = Sequence.load(std::memory_order_acquire);

            if (end - Cursor > RingSize)
            {
                char note[96];
                const int n = std::snprintf(note, sizeof(note), "    ... %llu entries lost (ring wrapped)\r\n",
                    end - Cursor - RingSize);
                Append(note, n);
                Cursor = end - RingSize;
            }

            char line[256];
            for (; Cursor < end; ++Cursor)
            {
                Entry& entry = Ring[Cursor & RingMask];
                if (entry.Stamp.load(std::memory_order_acquire) != Cursor + 1)
                    continue;

                const int n = std::snprintf(line, sizeof(line), "%llu t%u %*s%c%s%s%s\r\n",
                    entry.Ticks,
                    entry.ThreadId,
                    entry.Depth > 16 ? 16 : entry.Depth, "",
                    entry.Kind,
                    entry.Name ? entry.Name : "?",
                    entry.Detail[0] ? " " : "",
                    entry.Detail);
                if (n <= 0)
                    continue;

                if (std::strcmp(line, LastLine) == 0)
                {
                    ++Repeats;
                    continue;
                }

                if (Repeats)
                {
                    char note[64];
                    const int m = std::snprintf(note, sizeof(note), "    ... repeated %u more times\r\n", Repeats);
                    Append(note, m);
                    Repeats = 0;
                }

                Append(line, n);
                std::memcpy(LastLine, line, sizeof(LastLine));
            }
        }

        inline void CheckStuck()
        {
            const unsigned long long now = GetTickCount64();
            const unsigned count = ThreadCount.load(std::memory_order_relaxed);

            for (unsigned i = 0; i < count && i < MaxThreads; ++i)
            {
                Active& slot = Threads[i];
                const unsigned long long entered = slot.LastTicks.load(std::memory_order_acquire);
                if (!entered || slot.LastKind.load(std::memory_order_relaxed) != '>')
                {
                    slot.Reported.store(false, std::memory_order_relaxed);
                    continue;
                }

                if (now - entered < StuckMs || slot.Reported.load(std::memory_order_relaxed))
                    continue;

                slot.Reported.store(true, std::memory_order_relaxed);

                char note[256];
                const int n = std::snprintf(note, sizeof(note),
                    "\r\n*** STUCK: thread %u has been inside %s%s%s for %llu ms (depth %d) ***\r\n\r\n",
                    slot.ThreadId.load(std::memory_order_relaxed),
                    slot.Name ? slot.Name : "?",
                    slot.Detail[0] ? " " : "",
                    slot.Detail,
                    now - entered,
                    slot.Depth.load(std::memory_order_relaxed));
                Append(note, n);
            }
        }

        constexpr unsigned long long HangMs = 6000;

        constexpr unsigned MaxFrames = 24;

        inline unsigned CaptureStack(HANDLE thread, uintptr_t* frames, unsigned max)
        {
            CONTEXT context{};
            context.ContextFlags = CONTEXT_FULL;
            if (!GetThreadContext(thread, &context))
                return 0;

            unsigned count = 0;
            __try
            {
                while (count < max && context.Rip)
                {
                    frames[count++] = static_cast<uintptr_t>(context.Rip);

                    DWORD64 imageBase = 0;
                    PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
                    if (!function)
                    {
                        const DWORD64 returnAddress = *reinterpret_cast<DWORD64*>(context.Rsp);
                        context.Rsp += 8;
                        context.Rip = returnAddress;
                        continue;
                    }

                    PVOID handlerData = nullptr;
                    DWORD64 establisher = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function,
                        &context, &handlerData, &establisher, nullptr);
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }

            return count;
        }

        inline void DumpHungThreads()
        {
            const DWORD self = GetCurrentThreadId();
            const DWORD pid = GetCurrentProcessId();

            HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snapshot == INVALID_HANDLE_VALUE)
                return;

            static int SampleIndex = 0;
            char header[160];
            int n = std::snprintf(header, sizeof(header),
                "\r\n*** HUNG sample %d (no traced call in %llu ms) ***\r\n",
                ++SampleIndex, HangMs);
            Append(header, n);

            THREADENTRY32 entry{};
            entry.dwSize = sizeof(entry);
            if (Thread32First(snapshot, &entry))
            {
                do
                {
                    if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == self)
                        continue;

                    HANDLE thread = OpenThread(
                        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                        FALSE, entry.th32ThreadID);
                    if (!thread)
                        continue;

                    uintptr_t frames[MaxFrames]{};
                    unsigned count = 0;
                    if (SuspendThread(thread) != static_cast<DWORD>(-1))
                    {
                        count = CaptureStack(thread, frames, MaxFrames);
                        ResumeThread(thread);
                    }
                    CloseHandle(thread);

                    if (!count)
                        continue;

                    if (SampleIndex > 1)
                    {
                        char top[160];
                        Describe(frames[0], top, sizeof(top));
                        if (std::strstr(top, "ntdll.dll") || std::strstr(top, "win32u.dll"))
                            continue;
                    }

                    const char* lastName = nullptr;
                    const char* lastDetail = "";
                    for (unsigned i = 0; i < ThreadCount.load(std::memory_order_relaxed) && i < MaxThreads; ++i)
                    {
                        if (Threads[i].ThreadId.load(std::memory_order_relaxed) == entry.th32ThreadID)
                        {
                            lastName = Threads[i].Name;
                            lastDetail = Threads[i].Detail;
                            break;
                        }
                    }

                    char line[320];
                    n = std::snprintf(line, sizeof(line), "\r\nthread %lu%s%s%s%s\r\n",
                        entry.th32ThreadID,
                        lastName ? " (last traced: " : "",
                        lastName ? lastName : "",
                        (lastName && lastDetail && lastDetail[0]) ? " " : "",
                        lastName ? (lastDetail ? lastDetail : "") : "");
                    Append(line, n);
                    if (lastName)
                        Append(")\r\n", 3);

                    for (unsigned i = 0; i < count; ++i)
                    {
                        char described[160];
                        Describe(frames[i], described, sizeof(described));
                        n = std::snprintf(line, sizeof(line), "    #%02u %s\r\n", i, described);
                        Append(line, n);
                    }
                } while (Thread32Next(snapshot, &entry));
            }

            CloseHandle(snapshot);
            Append("\r\n*** end of hang dump ***\r\n\r\n", 33);
        }

        inline void CheckHang()
        {
            static unsigned long long LastSequence = 0;
            static unsigned long long QuietSince = 0;
            static bool Dumped = false;

            const unsigned long long sequence = Sequence.load(std::memory_order_relaxed);
            const unsigned long long now = GetTickCount64();

            if (sequence != LastSequence)
            {
                LastSequence = sequence;
                QuietSince = now;
                Dumped = false;
                return;
            }

            if (!sequence || Dumped || !QuietSince)
            {
                if (!QuietSince)
                    QuietSince = now;
                return;
            }

            if (now - QuietSince < HangMs)
                return;

            Dumped = true;

            for (int sample = 0; sample < 3; ++sample)
            {
                if (sample)
                    Sleep(750);
                DumpHungThreads();
            }
        }

        inline void Worker()
        {
            while (Running.load(std::memory_order_relaxed))
            {
                Drain();
                CheckStuck();
                CheckHang();
                Sleep(250);
            }
            Drain();
        }
    }

    inline bool IsEnabled()
    {
        return Detail::Enabled.load(std::memory_order_relaxed);
    }

    inline void SetEnabled(bool on)
    {
        Detail::Enabled.store(on, std::memory_order_relaxed);
    }

    inline void Flush()
    {
        Detail::Drain();
    }

    namespace Detail
    {
        inline void Record(char kind, const char* name, const char* detail, int depth)
        {
            Push(kind, name, detail, depth);

            if (Active* slot = Slot())
            {
                slot->Name = name;
                CopyDetail(slot->Detail, detail);
                slot->Depth.store(depth, std::memory_order_relaxed);
                slot->Reported.store(false, std::memory_order_relaxed);
                slot->LastKind.store(kind, std::memory_order_relaxed);
                slot->LastTicks.store(GetTickCount64(), std::memory_order_release);
            }
        }
    }

    inline void Enter(const char* name, const char* detail, int depth)
    {
#if SUPQ_CALL_TRACE
        if (IsEnabled())
            Detail::Record('>', name, detail, depth);
#else
        (void)name; (void)detail; (void)depth;
#endif
    }

    inline void Leave(const char* name, int depth)
    {
#if SUPQ_CALL_TRACE
        if (IsEnabled())
            Detail::Record('<', name, nullptr, depth);
#else
        (void)name; (void)depth;
#endif
    }

    inline void Note(const char* name, const char* detail = nullptr, int depth = 0)
    {
#if SUPQ_CALL_TRACE
        if (IsEnabled())
            Detail::Record('!', name, detail, depth);
#else
        (void)name; (void)detail; (void)depth;
#endif
    }

    inline void Install()
    {
#if SUPQ_CALL_TRACE
        if (Detail::Running.exchange(true))
            return;

        wchar_t AppData[MAX_PATH]{};
        if (GetEnvironmentVariableW(L"APPDATA", AppData, MAX_PATH) == 0 || AppData[0] == L'\0')
        {
            GetTempPathW(MAX_PATH, AppData);
        }

        wchar_t Dir[MAX_PATH]{};
        _snwprintf_s(Dir, MAX_PATH, _TRUNCATE, L"%s\\supQ", AppData);
        CreateDirectoryW(Dir, nullptr);

        _snwprintf_s(Detail::Path, MAX_PATH, _TRUNCATE, L"%s\\supQ\\supQ_trace.txt", Dir);
        DeleteFileW(Detail::Path);

        WIN32_FIND_DATAW fd{};
        wchar_t search[MAX_PATH]{};
        _snwprintf_s(search, MAX_PATH, _TRUNCATE, L"%s\\supQ_trace_*.txt", Dir);
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

        char header[192];
        const int n = std::snprintf(header, sizeof(header),
            "call trace for pid %lu\r\n"
            "'>' enter, '<' leave, '!' note; columns: ticks thread indent kind name detail\r\n"
            "a '>' with no matching '<' at the end of the file is where it stopped\r\n\r\n",
            GetCurrentProcessId());
        Detail::Append(header, n);

        SetEnabled(true);
        std::thread(Detail::Worker).detach();
#endif
    }
}
