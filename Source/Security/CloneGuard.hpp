#pragma once
#include <string>

namespace CloneGuard
{
    enum class Signal
    {
        LauncherMissing,
        RivalProcess,
        RivalPorts,
        RivalGlobals,
    };

    [[noreturn]] void CrashClient();

    bool LauncherRunning();
    bool RivalProcessRunning();
    bool RivalPortsListening();

    void Flag(Signal signal, const std::string& detail = {});

    bool Detected();

    using Notifier = void (*)(const char* title, const char* body);
    void SetNotifier(Notifier notifier);

    void StartWatcher();
}
