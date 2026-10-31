#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <tlhelp32.h>

#include <Security/CloneGuard.hpp>
#include <Security/BrandGuard.hpp>
#include <Core/DebugLog.hpp>
#include <Scripting/Obfuscation.hpp>

#include <atomic>
#include <chrono>
#include <iterator>
#include <mutex>
#include <thread>
#include <vector>

#pragma comment(lib, "iphlpapi.lib")

namespace CloneGuard
{
    namespace
    {
        constexpr unsigned short kRivalPorts[] = { 47899, 47900, 47901, 59888 };

        constexpr auto kWatchInterval = std::chrono::seconds(5);

        constexpr auto kLauncherGrace = std::chrono::seconds(20);

        std::atomic<bool> g_watching{ false };
        std::atomic<bool> g_detected{ false };
        std::atomic<bool> g_notified{ false };
        std::atomic<bool> g_toastPending{ false };

        std::mutex g_notifierLock;
        Notifier g_notifier = nullptr;

        std::atomic<bool> g_raised[4]{};

        std::wstring Widen(const std::string& narrow)
        {
            if (narrow.empty())
                return {};

            const int len = MultiByteToWideChar(CP_UTF8, 0, narrow.c_str(),
                static_cast<int>(narrow.size()), nullptr, 0);
            if (len <= 0)
                return {};

            std::wstring wide(static_cast<size_t>(len), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, narrow.c_str(),
                static_cast<int>(narrow.size()), &wide[0], len);
            return wide;
        }

        bool ProcessRunning(const std::wstring& imageName)
        {
            if (imageName.empty())
                return false;

            HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (snapshot == INVALID_HANDLE_VALUE)
                return false;

            PROCESSENTRY32W entry{};
            entry.dwSize = sizeof(entry);
            bool found = false;

            if (Process32FirstW(snapshot, &entry))
            {
                do
                {
                    if (_wcsicmp(entry.szExeFile, imageName.c_str()) == 0)
                    {
                        found = true;
                        break;
                    }
                } while (Process32NextW(snapshot, &entry));
            }

            CloseHandle(snapshot);
            return found;
        }

        bool IsWatchedPort(unsigned short port)
        {
            for (unsigned short watched : kRivalPorts)
            {
                if (watched == port)
                    return true;
            }
            return false;
        }

        std::vector<char> QueryTcpTable(ULONG family, ULONG tableClass)
        {
            std::vector<char> buffer;
            DWORD size = 0;

            for (int attempt = 0; attempt < 4; ++attempt)
            {
                DWORD result = GetExtendedTcpTable(buffer.empty() ? nullptr : buffer.data(),
                    &size, FALSE, family,
                    static_cast<TCP_TABLE_CLASS>(tableClass), 0);

                if (result == NO_ERROR)
                    return buffer;

                if (result != ERROR_INSUFFICIENT_BUFFER)
                    break;

                buffer.assign(size, 0);
            }

            return {};
        }

        bool AnyRivalPortV4()
        {
            std::vector<char> buffer = QueryTcpTable(AF_INET, TCP_TABLE_OWNER_PID_LISTENER);
            if (buffer.empty())
                return false;

            const auto* table = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(buffer.data());
            for (DWORD i = 0; i < table->dwNumEntries; ++i)
            {
                const MIB_TCPROW_OWNER_PID& row = table->table[i];
                if (row.dwState != MIB_TCP_STATE_LISTEN)
                    continue;

                const unsigned short port =
                    ntohs(static_cast<unsigned short>(row.dwLocalPort & 0xFFFF));
                if (IsWatchedPort(port))
                    return true;
            }

            return false;
        }

        bool AnyRivalPortV6()
        {
            std::vector<char> buffer = QueryTcpTable(AF_INET6, TCP_TABLE_OWNER_PID_LISTENER);
            if (buffer.empty())
                return false;

            const auto* table = reinterpret_cast<const MIB_TCP6TABLE_OWNER_PID*>(buffer.data());
            for (DWORD i = 0; i < table->dwNumEntries; ++i)
            {
                const MIB_TCP6ROW_OWNER_PID& row = table->table[i];
                if (row.dwState != MIB_TCP_STATE_LISTEN)
                    continue;

                const unsigned short port =
                    ntohs(static_cast<unsigned short>(row.dwLocalPort & 0xFFFF));
                if (IsWatchedPort(port))
                    return true;
            }

            return false;
        }

        std::string SignalName(Signal signal)
        {
            switch (signal)
            {
            case Signal::LauncherMissing: return SUPQ_OBF("launcher missing");
            case Signal::RivalProcess:    return SUPQ_OBF("rival process");
            case Signal::RivalPorts:      return SUPQ_OBF("rival ports");
            case Signal::RivalGlobals:    return SUPQ_OBF("rival globals");
            }
            return SUPQ_OBF("unknown");
        }

        void RaiseToast()
        {
        }

        void WatcherThread()
        {
            const std::wstring launcher = Widen(BrandGuard::LauncherProcess());
            const std::wstring rival = Widen(BrandGuard::RivalProcess());

            const auto deadline = std::chrono::steady_clock::now() + kLauncherGrace;
            bool launcherSeen = false;
            bool launcherSettled = false;

            for (;;)
            {
                if (!launcherSettled)
                {
                    if (!launcherSeen && ProcessRunning(launcher))
                    {
                        launcherSeen = true;
                        launcherSettled = true;
                    }
                    else if (std::chrono::steady_clock::now() >= deadline)
                    {
                        launcherSettled = true;
                        Flag(Signal::LauncherMissing, BrandGuard::LauncherProcess());
                    }
                }

                if (ProcessRunning(rival))
                    Flag(Signal::RivalProcess, BrandGuard::RivalProcess());

                if (AnyRivalPortV4() || AnyRivalPortV6())
                    Flag(Signal::RivalPorts);

                std::this_thread::sleep_for(kWatchInterval);
            }
        }
    }

    bool LauncherRunning()
    {
        return ProcessRunning(Widen(BrandGuard::LauncherProcess()));
    }

    bool RivalProcessRunning()
    {
        return ProcessRunning(Widen(BrandGuard::RivalProcess()));
    }

    bool RivalPortsListening()
    {
        return AnyRivalPortV4() || AnyRivalPortV6();
    }

    bool Detected()
    {
        return g_detected.load(std::memory_order_relaxed);
    }

    [[noreturn]] void CrashClient()
    {
        SUPQ_LOG_ERR(SUPQ2_OBF("cloneguard: blacklisted repack/clone detected - crashing client immediately"));
        __fastfail(FAST_FAIL_FATAL_APP_EXIT);
        TerminateProcess(GetCurrentProcess(), 0xDEADBEEF);
        *reinterpret_cast<volatile int*>(0) = 0xDEAD;
        std::abort();
    }

    void Flag(Signal signal, const std::string& detail)
    {
        const size_t slot = static_cast<size_t>(signal);
        if (slot < std::size(g_raised))
        {
            bool expected = false;
            if (!g_raised[slot].compare_exchange_strong(expected, true))
                return;
        }

        g_detected.store(true, std::memory_order_relaxed);

        if (detail.empty())
            SUPQ_LOG_ERR(SUPQ2_OBF("cloneguard: %s - terminating client"), SignalName(signal).c_str());
        else
            SUPQ_LOG_ERR(SUPQ2_OBF("cloneguard: %s (%s) - terminating client"), SignalName(signal).c_str(), detail.c_str());

        CrashClient();
    }

    void SetNotifier(Notifier notifier)
    {
        {
            std::lock_guard<std::mutex> lock(g_notifierLock);
            g_notifier = notifier;
        }

        if (!notifier)
            return;

        if (g_toastPending.exchange(false, std::memory_order_acq_rel))
        {
            notifier(BrandGuard::CloneToastTitle().c_str(),
                BrandGuard::CloneToastBody().c_str());
        }
    }

    void StartWatcher()
    {
        bool expected = false;
        if (!g_watching.compare_exchange_strong(expected, true))
            return;

        std::thread(WatcherThread).detach();
        SUPQ_LOG_OK(SUPQ2_OBF("cloneguard: watcher started"));
    }
}
