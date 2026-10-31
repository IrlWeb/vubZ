#pragma once
#include <string>
#include <atomic>

namespace BrandGuard
{
    bool Tampered();

    const std::string& Name();
    const std::string& Version();
    const std::string& UserAgent();
    const std::string& ToastTitle();
    const std::string& ToastUrl();

    const std::string& LauncherProcess();
    const std::string& RivalProcess();
    const std::string& RivalConsoleGlobal();
    const std::string& RivalProbeGlobal();
    const std::string& CloneToastTitle();
    const std::string& CloneToastBody();

    bool Verify();
}
