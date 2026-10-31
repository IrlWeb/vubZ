#include <Core/CallTrace.hpp>
#include <Core/CrashLog.hpp>
#include <Core/DebugLog.hpp>
#include <Core/Utilities.hpp>
#include <Security/BrandGuard.hpp>
#include <Security/CloneGuard.hpp>
#include <Security/Pairing.hpp>
#include <Core/Context.hpp>
#include <Ui/Overlay/WindowOverlay.hpp>
#include <Runtime/Scheduler/TaskScheduler.hpp>
#include <Runtime/Execution/TeleportHandler.hpp>
#include <Network/Raknet/Raknet.hpp>
#include <Scripting/Obfuscation.hpp>
#include <Network/Ipc/IpcChannel.hpp>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <windows.h>
#include <tlhelp32.h>

namespace
{
    constexpr unsigned short kDecompilerPort = 4128;

    bool IsProcessRunning(const wchar_t* name)
    {
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
                if (_wcsicmp(entry.szExeFile, name) == 0)
                {
                    found = true;
                    break;
                }
            } while (Process32NextW(snapshot, &entry));
        }

        CloseHandle(snapshot);
        return found;
    }

    bool LaunchDecompilerServer(const std::filesystem::path& moduleDir)
    {
        try
        {
            if (IsProcessRunning(L"supQDecompiler.exe"))
                return true;

            std::filesystem::path exe = moduleDir / L"supQDecompiler.exe";

            std::error_code ec;
            if (!std::filesystem::exists(exe, ec))
                return false;

            std::wstring cmd = L"\"" + exe.wstring() + L"\" serve --port " +
                std::to_wstring(kDecompilerPort);

            std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
            mutableCmd.push_back(L'\0');

            STARTUPINFOW si{};
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;
            PROCESS_INFORMATION pi{};

            const std::wstring workDir = moduleDir.wstring();

            if (CreateProcessW(exe.wstring().c_str(), mutableCmd.data(),
                nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                nullptr, workDir.c_str(), &si, &pi))
            {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
                return true;
            }
        }
        catch (...)
        {
        }

        return false;
    }

    void WatchDecompilerServer(std::filesystem::path moduleDir)
    {
        bool reportedMissing = false;

        for (;;)
        {
            if (!IsProcessRunning(L"supQDecompiler.exe"))
            {
                if (LaunchDecompilerServer(moduleDir))
                {
                    SUPQ_LOG_OK(SUPQ2_OBF("decompiler was not running - restarted it"));
                    reportedMissing = false;
                }
                else if (!reportedMissing)
                {
                    SUPQ_LOG_ERR(SUPQ2_OBF("decompiler is not running and could not be started from %ls"),
                        moduleDir.c_str());
                    reportedMissing = true;
                }
            }

            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
    }
}

// Whitelist export the emulator resolves by name from this image only
// (supQ-Module.dll). The host pulls its decoded token through here and
// compares against its own copy; the plaintext lives in neither binary.
extern "C" __declspec(dllexport) int __cdecl EmuWL(unsigned char *out, unsigned cap)
{
    return Pairing::EmuWL_Write(out, cap);
}

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <string.h>
#include <thread>

static void tp_cb(PTP_CALLBACK_INSTANCE inst, PVOID pv, PTP_WORK work)
{
    (void)inst; (void)pv; (void)work;

    CrashLog::Install();
    CallTrace::Install();

    std::filesystem::path moduleDir;
    {
        wchar_t modulePath[MAX_PATH] = {};
        if (GetModuleFileNameW(NULL ,modulePath, MAX_PATH) && modulePath[0] != L'\0')
            moduleDir = std::filesystem::path(modulePath).parent_path();

        if (moduleDir.empty())
        {
            std::error_code ec;
            moduleDir = std::filesystem::current_path(ec);
        }
    }

    SharedVariables::workspace_folder = moduleDir / SUPQ2_OBF("workspace");
    SharedVariables::autoexecute_folder = moduleDir / SUPQ2_OBF("autoexecute");

    {
        std::error_code ec;
        if (!std::filesystem::exists(SharedVariables::workspace_folder, ec))
            std::filesystem::create_directories(SharedVariables::workspace_folder, ec);

        if (!std::filesystem::exists(SharedVariables::autoexecute_folder, ec))
            std::filesystem::create_directories(SharedVariables::autoexecute_folder, ec);
    }
    SUPQ_LOG_OK(SUPQ2_OBF("loaded - MainThread running. module dir: %ls"), moduleDir.c_str());

    if (!BrandGuard::Verify())
    {
        SUPQ_LOG_ERR(SUPQ2_OBF("brand integrity check failed - module image has been modified, terminating"));
        CloneGuard::CrashClient();
        return;
    }

    if (CloneGuard::RivalProcessRunning() || CloneGuard::RivalPortsListening())
    {
        SUPQ_LOG_ERR(SUPQ2_OBF("blacklisted rival repack detected at inject - terminating"));
        CloneGuard::CrashClient();
        return;
    }

    CloneGuard::StartWatcher();

    LaunchDecompilerServer(moduleDir);
    std::thread(WatchDecompilerServer, moduleDir).detach();

    WindowOverlay::Start();
    Communication::Initialize();
    SUPQ_LOG_OK(SUPQ2_OBF("Communication::Initialize returned"));

    Raknet::StartWatcher();

    TpHandler::Initialize();
    SUPQ_LOG_OK(SUPQ2_OBF("TpHandler::Initialize returned - now polling for DataModel"));
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r)
{
    (void)r;
    if (reason != DLL_PROCESS_ATTACH) return TRUE;

    DisableThreadLibraryCalls((HMODULE)h);

    PTP_WORK w = CreateThreadpoolWork(tp_cb, NULL, NULL);
    if (w)
    {
        SubmitThreadpoolWork(w);
        CloseThreadpoolWork(w);
    }

    return TRUE;
}