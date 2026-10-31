#include <Security/BrandGuard.hpp>
#include <Security/VMProtectShim.h>

namespace BrandGuard
{
    namespace
    {
        std::atomic<bool> g_tampered{ false };

        bool BytewiseEqual(const char* a, const char* b)
        {
            if (!a || !b) return a == b;
            volatile unsigned diff = 0;
            size_t i = 0;
            for (; a[i] && b[i]; ++i)
                diff = diff | static_cast<unsigned char>(a[i] ^ b[i]);
            diff = diff | static_cast<unsigned char>(a[i] ^ b[i]);
            return diff == 0;
        }

        const std::string& Cache(std::string& slot,
                                 const char* primary,
                                 const char* shadow)
        {
            if (!BytewiseEqual(primary, shadow))
            {
                g_tampered.store(true, std::memory_order_relaxed);
                slot.assign(shadow ? shadow : "");
            }
            else
            {
                slot.assign(primary ? primary : "");
            }
            return slot;
        }
    }

    bool Tampered()
    {
        return g_tampered.load(std::memory_order_relaxed);
    }

    const std::string& Name()
    {
        static std::string value = [] {
            VMProtectBeginUltra("brand.name");
            std::string s;
            const char* a = VMProtectDecryptStringA("supQ");
            const char* b = VMProtectDecryptStringA("supQ");
            Cache(s, a, b);
            VMProtectEnd();
            return s;
        }();
        return value;
    }

    const std::string& Version()
    {
        static std::string value = [] {
            VMProtectBeginUltra("brand.version");
            std::string s;
            const char* a = VMProtectDecryptStringA("1.0.0");
            const char* b = VMProtectDecryptStringA("1.0.0");
            Cache(s, a, b);
            VMProtectEnd();
            return s;
        }();
        return value;
    }

    const std::string& UserAgent()
    {
        static std::string value = [] {
            VMProtectBeginUltra("brand.useragent");
            std::string s;
            const char* a = VMProtectDecryptStringA("supQ");
            const char* b = VMProtectDecryptStringA("supQ");
            Cache(s, a, b);
            VMProtectEnd();
            return s;
        }();
        return value;
    }

    const std::string& ToastTitle()
    {
        static std::string value = [] {
            VMProtectBeginUltra("brand.toast.title");
            std::string s;
            const char* a = VMProtectDecryptStringA("supQ Loaded");
            const char* b = VMProtectDecryptStringA("supQ Loaded");
            Cache(s, a, b);
            VMProtectEnd();
            return s;
        }();
        return value;
    }

    const std::string& ToastUrl()
    {
        static std::string value = [] {
            VMProtectBeginUltra("brand.toast.url");
            std::string s;
            const char* a = VMProtectDecryptStringA("https://supq.lol");
            const char* b = VMProtectDecryptStringA("https://supq.lol");
            Cache(s, a, b);
            VMProtectEnd();
            return s;
        }();
        return value;
    }

    const std::string& LauncherProcess()
    {
        static std::string value = [] {
            VMProtectBeginUltra("brand.launcher.process");
            std::string s;
            const char* a = VMProtectDecryptStringA("supQ.exe");
            const char* b = VMProtectDecryptStringA("supQ.exe");
            Cache(s, a, b);
            VMProtectEnd();
            return s;
        }();
        return value;
    }

    const std::string& RivalProcess()
    {
        static std::string value = [] {
            VMProtectBeginUltra("brand.rival.process");
            std::string s;
            const char* a = VMProtectDecryptStringA("Crescent.exe");
            const char* b = VMProtectDecryptStringA("Crescent.exe");
            Cache(s, a, b);
            VMProtectEnd();
            return s;
        }();
        return value;
    }

    const std::string& RivalConsoleGlobal()
    {
        static std::string value = [] {
            VMProtectBeginUltra("brand.rival.global.console");
            std::string s;
            const char* a = VMProtectDecryptStringA("_CrescentConsoleHook");
            const char* b = VMProtectDecryptStringA("_CrescentConsoleHook");
            Cache(s, a, b);
            VMProtectEnd();
            return s;
        }();
        return value;
    }

    const std::string& RivalProbeGlobal()
    {
        static std::string value = [] {
            VMProtectBeginUltra("brand.rival.global.probe");
            std::string s;
            const char* a = VMProtectDecryptStringA("_CrescentGameHubProbe");
            const char* b = VMProtectDecryptStringA("_CrescentGameHubProbe");
            Cache(s, a, b);
            VMProtectEnd();
            return s;
        }();
        return value;
    }

    const std::string& CloneToastTitle()
    {
        static std::string value = [] {
            VMProtectBeginUltra("brand.clone.toast.title");
            std::string s;
            const char* a = VMProtectDecryptStringA("SKIDDED MODULE ALERT");
            const char* b = VMProtectDecryptStringA("SKIDDED MODULE ALERT");
            Cache(s, a, b);
            VMProtectEnd();
            return s;
        }();
        return value;
    }

    const std::string& CloneToastBody()
    {
        static std::string value = [] {
            VMProtectBeginUltra("brand.clone.toast.body");
            std::string s;
            const char* a = VMProtectDecryptStringA("This is a skidded module. Get the real one at https://supq.lol");
            const char* b = VMProtectDecryptStringA("This is a skidded module. Get the real one at https://supq.lol");
            Cache(s, a, b);
            VMProtectEnd();
            return s;
        }();
        return value;
    }

    bool Verify()
    {
        VMProtectBeginUltra("brand.verify");

        Name();
        Version();
        UserAgent();
        ToastTitle();
        ToastUrl();
        LauncherProcess();
        RivalProcess();
        RivalConsoleGlobal();
        RivalProbeGlobal();
        CloneToastTitle();
        CloneToastBody();

        const bool crcOk = VMProtectIsValidImageCRC();
        if (!crcOk)
            g_tampered.store(true, std::memory_order_relaxed);

        const bool ok = !Tampered();
        VMProtectEnd();
        return ok;
    }
}
