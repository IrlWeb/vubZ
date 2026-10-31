#pragma once

#define NOMINMAX

#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <Core/DebugLog.hpp>
#include <Security/BrandGuard.hpp>
#include <Security/VMProtectShim.h>

#include <algorithm>
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>
#include <backends/imgui_impl_dx11.h>
#include <Dependencies/imgui/addons/imgui_notify.h>
#include <Ui/Overlay/ToastStyle.hpp>
#include <Ui/Overlay/GuiWindows.hpp>
#include <Scripting/Obfuscation.hpp>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace Renderer
{
    enum class Kind
    {
        Line,
        Text,
        Circle,
        Square,
        Quad,
        Triangle,
        Image,
        Font,
        Shader,
    };

    enum class Font : int
    {
        UI = 0,
        System = 1,
        Plex = 2,
        Monospace = 3,
    };

    using Handle = std::uint32_t;
    constexpr Handle InvalidHandle = 0;

    struct Object
    {
        Kind kind = Kind::Line;

        int zindex = 1;
        bool visible = false;
        float transparency = 1.0f;
        ImColor color = ImColor(1.0f, 1.0f, 1.0f, 1.0f);

        float thickness = 1.0f;
        bool filled = false;
        ImVec2 position{ 0.0f, 0.0f };
        ImVec2 size{ 100.0f, 100.0f };

        ImVec2 from{ 0.0f, 0.0f };
        ImVec2 to{ 0.0f, 0.0f };

        std::string text;
        float textsize = 13.0f;
        bool center = false;
        bool outline = false;
        float outline_opacity = 1.0f;
        ImColor outline_color = ImColor(0.0f, 0.0f, 0.0f, 1.0f);
        Font font = Font::System;
        int customFont = 0;
        ImVec2 textbounds{ 0.0f, 0.0f };

        float radius = 50.0f;
        int numsides = 64;

        ImVec2 pointA{ 0.0f, 0.0f };
        ImVec2 pointB{ 0.0f, 0.0f };
        ImVec2 pointC{ 0.0f, 0.0f };
        ImVec2 pointD{ 0.0f, 0.0f };

        std::string data;
        std::uint64_t dataHash = 0;
        float rounding = 0.0f;

        int shaderId = 0;
        std::string vertex;
        std::string pixel;
    };

    namespace detail
    {
        inline std::mutex g_lock;
        inline std::vector<std::pair<Handle, Object>> g_objects;
        inline std::atomic<Handle> g_next{ 1 };

        inline std::atomic<bool> g_installed{ false };

        inline std::vector<std::pair<Handle, Object>> g_frame;

        inline Object* find(Handle handle)
        {
            for (auto& entry : g_objects)
            {
                if (entry.first == handle)
                    return &entry.second;
            }
            return nullptr;
        }

        inline ImU32 resolve(const ImColor& colour, float transparency)
        {
            return ImColor(
                colour.Value.x,
                colour.Value.y,
                colour.Value.z,
                std::clamp(transparency, 0.0f, 1.0f));
        }

        inline ImFont* (*g_pickFont)(Font font, float size) = nullptr;
        inline ImTextureID (*g_imageFor)(const Object& object) = nullptr;
        inline ImFont* (*g_customFont)(int id) = nullptr;
        inline void (*g_drawShader)(ImDrawList* list, const Object& object) = nullptr;

        inline std::mutex g_immediateLock;
        inline std::vector<std::pair<const void*, std::vector<Object>>> g_immediate;

        inline ImFont* face_for(const Object& o, float size)
        {
            if (o.customFont && g_customFont)
            {
                if (ImFont* face = g_customFont(o.customFont))
                    return face;
            }

            if (g_pickFont)
            {
                if (ImFont* face = g_pickFont(o.font, size))
                    return face;
            }

            return ImGui::GetFont();
        }

        inline ImVec2 measure(ImFont* face, const char* text, float size)
        {
            if (!face || !text || !*text)
                return ImVec2(0.0f, size);

            ImVec2 bounds = face->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
            bounds.x = IM_FLOOR(bounds.x + 0.99999f);
            return bounds;
        }

        inline void paint(ImDrawList* list, Object& o)
        {
            const ImU32 colour = resolve(o.color, o.transparency);
            const float thickness = o.thickness <= 0.0f ? 1.0f : o.thickness;

            switch (o.kind)
            {
            case Kind::Line:
                list->AddLine(o.from, o.to, colour, thickness);
                break;

            case Kind::Square:
            {
                const ImVec2 bottomRight(o.position.x + o.size.x, o.position.y + o.size.y);
                if (o.filled)
                    list->AddRectFilled(o.position, bottomRight, colour, o.rounding);
                else
                    list->AddRect(o.position, bottomRight, colour, o.rounding, ImDrawFlags_None, thickness);
                break;
            }

            case Kind::Circle:
                if (o.filled)
                    list->AddCircleFilled(o.position, o.radius, colour, o.numsides);
                else
                    list->AddCircle(o.position, o.radius, colour, o.numsides, thickness);
                break;

            case Kind::Triangle:
                if (o.filled)
                    list->AddTriangleFilled(o.pointA, o.pointB, o.pointC, colour);
                else
                    list->AddTriangle(o.pointA, o.pointB, o.pointC, colour, thickness);
                break;

            case Kind::Quad:
                if (o.filled)
                    list->AddQuadFilled(o.pointA, o.pointB, o.pointC, o.pointD, colour);
                else
                    list->AddQuad(o.pointA, o.pointB, o.pointC, o.pointD, colour, thickness);
                break;

            case Kind::Text:
            {
                if (o.text.empty())
                    break;

                const float size = o.textsize <= 0.0f ? 13.0f : o.textsize;
                ImFont* face = face_for(o, size);
                if (!face)
                    break;

                const ImVec2 bounds = measure(face, o.text.c_str(), size);
                o.textbounds = bounds;

                ImVec2 at = o.position;
                if (o.center)
                    at.x -= bounds.x * 0.5f;

                if (o.outline)
                {
                    const ImU32 edge = resolve(o.outline_color, o.outline_opacity);
                    list->AddText(face, size, ImVec2(at.x - 1, at.y - 1), edge, o.text.c_str());
                    list->AddText(face, size, ImVec2(at.x + 1, at.y - 1), edge, o.text.c_str());
                    list->AddText(face, size, ImVec2(at.x - 1, at.y + 1), edge, o.text.c_str());
                    list->AddText(face, size, ImVec2(at.x + 1, at.y + 1), edge, o.text.c_str());
                }

                list->AddText(face, size, at, colour, o.text.c_str());
                break;
            }

            case Kind::Image:
            {
                const ImVec2 bottomRight(o.position.x + o.size.x, o.position.y + o.size.y);
                const ImTextureID texture = g_imageFor ? g_imageFor(o) : nullptr;
                if (texture)
                {
                    const ImU32 tint = resolve(ImColor(1.0f, 1.0f, 1.0f, 1.0f), o.transparency);
                    list->AddImageRounded(texture, o.position, bottomRight, ImVec2(0, 0), ImVec2(1, 1),
                        tint, o.rounding);
                }
                else if (o.data.empty())
                {
                    list->AddRectFilled(o.position, bottomRight, colour, o.rounding);
                }
                break;
            }

            case Kind::Shader:
                if (g_drawShader && o.shaderId)
                    g_drawShader(list, o);
                break;

            case Kind::Font:
                break;
            }
        }

        inline void render()
        {
            {
                std::lock_guard<std::mutex> guard(g_lock);

                g_frame.clear();
                g_frame.reserve(g_objects.size());
                for (auto& entry : g_objects)
                {
                    if (entry.second.visible)
                        g_frame.push_back(entry);
                }
            }
            {
                std::lock_guard<std::mutex> guard(g_immediateLock);
                for (const auto& vm : g_immediate)
                {
                    for (const Object& object : vm.second)
                        g_frame.emplace_back(InvalidHandle, object);
                }
            }

            if (g_frame.empty())
                return;

            std::stable_sort(g_frame.begin(), g_frame.end(),
                [](const std::pair<Handle, Object>& a, const std::pair<Handle, Object>& b)
                {
                    return a.second.zindex < b.second.zindex;
                });

            ImDrawList* list = ImGui::GetBackgroundDrawList();
            if (!list)
                return;

            for (auto& entry : g_frame)
                paint(list, entry.second);

            std::lock_guard<std::mutex> guard(g_lock);
            for (const auto& painted : g_frame)
            {
                if (painted.second.kind != Kind::Text)
                    continue;

                if (Object* live = find(painted.first))
                    live->textbounds = painted.second.textbounds;
            }
        }
    }

namespace ToastOverlay
{
    struct ToastItem
    {
        uint64_t id = 0;
        std::wstring title;
        std::wstring content;
        ImGuiToastType type = ImGuiToastType::Success;
        int durationMs = 5000;
    };

    struct ActiveToast
    {
        uint64_t id = 0;
        std::wstring title;
        std::wstring content;
        ImGuiToastType type = ImGuiToastType::Success;
        int durationMs = 5000;
        std::chrono::steady_clock::time_point startTime;

        float currentY = 0.0f;
        float height = ToastStyle::kMinHeight;
        float slideX = 0.0f;
        float alpha = 0.0f;
        bool exiting = false;
        std::chrono::steady_clock::time_point exitStartTime;
    };

    inline std::mutex g_lock;
    inline std::vector<ToastItem> g_queue;
    inline HANDLE g_wake = nullptr;
    inline std::atomic<bool> g_running{ false };
    inline std::atomic<bool> g_started{ false };
    inline std::atomic<uint64_t> g_nextId{ 1 };

    inline std::wstring ToWString(const char* str)
    {
        if (!str || !*str) return L"";
        int len = MultiByteToWideChar(CP_UTF8, 0, str, -1, nullptr, 0);
        if (len <= 0) return L"";
        std::wstring result(len - 1, 0);
        MultiByteToWideChar(CP_UTF8, 0, str, -1, &result[0], len);
        return result;
    }

    inline void PumpMessages()
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    inline void WaitAndPump(DWORD timeoutMs)
    {
        MsgWaitForMultipleObjectsEx(g_wake ? 1 : 0, g_wake ? &g_wake : nullptr,
            timeoutMs, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        PumpMessages();
    }

    inline void WorkerThread()
    {
        SUPQ_LOG("toast: worker entry");

        ULONG_PTR gdiplusToken = 0;
        Gdiplus::GdiplusStartupInput gdiplusStartupInput;
        Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, nullptr);

        SUPQ_LOG("toast: gdiplus started");

        VMProtectBeginUltra("brand.toast.classname");
        auto makeRandName = []() -> std::wstring {
            LARGE_INTEGER pc{}; QueryPerformanceCounter(&pc);
            uint64_t seed = static_cast<uint64_t>(pc.QuadPart)
                ^ (static_cast<uint64_t>(GetCurrentProcessId()) * 0x9E3779B97F4A7C15ULL)
                ^ (static_cast<uint64_t>(GetTickCount64())      * 0xD2B74407B1CE6E93ULL)
                ^ (reinterpret_cast<uintptr_t>(&pc)              * 0xBF58476D1CE4E5B9ULL);
            seed ^= seed >> 33; seed *= 0xff51afd7ed558ccdULL;
            seed ^= seed >> 33; seed *= 0xc4ceb9fe1a85ec53ULL;
            seed ^= seed >> 33;
            static const wchar_t hex[] = L"0123456789abcdef";
            wchar_t buf[18];
            buf[0] = L'W';
            for (int i = 0; i < 16; ++i)
                buf[1 + i] = hex[(seed >> (i * 4)) & 0xF];
            buf[17] = L'\0';
            return std::wstring(buf);
        };
        std::wstring className  = makeRandName();
        std::wstring windowName = makeRandName();
        VMProtectEnd();

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = className.c_str();
        RegisterClassExW(&wc);

        const int windowWidth = ToastStyle::kWindowWidth;
        const int windowHeight = ToastStyle::kWindowHeight;

        HWND hwnd = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            wc.lpszClassName,
            windowName.c_str(),
            WS_POPUP,
            0, 0, windowWidth, windowHeight,
            nullptr, nullptr, wc.hInstance, nullptr);

        SUPQ_LOG("toast: window created 0x%llX", (unsigned long long)(uintptr_t)hwnd);

        ShowWindow(hwnd, SW_SHOWNOACTIVATE);

        HDC screenDc = GetDC(nullptr);
        HDC memDc = CreateCompatibleDC(screenDc);

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = windowWidth;
        bmi.bmiHeader.biHeight = -windowHeight;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        HBITMAP hBmp = CreateDIBSection(memDc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        HGDIOBJ oldBmp = SelectObject(memDc, hBmp);

        std::vector<ActiveToast> activeToasts;

        SUPQ_LOG("toast: dib ok, building fonts");

        auto surface = std::make_unique<Gdiplus::Bitmap>(windowWidth, windowHeight,
            windowWidth * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(bits));
        auto graphics = std::make_unique<Gdiplus::Graphics>(surface.get());
        graphics->SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics->SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
        graphics->SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
        graphics->SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);

        auto fonts = std::make_unique<ToastStyle::Fonts>();
        if (fonts->Load())
            SUPQ_LOG("toast: poppins loaded");
        else
            SUPQ_LOG_ERR("toast: poppins unavailable - falling back to Segoe UI");

        auto logo = std::make_unique<ToastStyle::Logo>();
        if (logo->Load())
            SUPQ_LOG("toast: supQ logo loaded");
        else
            SUPQ_LOG_ERR("toast: supQ logo unavailable");

        SUPQ_LOG("toast: entering message loop");

        while (g_running.load())
        {
            PumpMessages();

            bool idle = false;
            {
                std::lock_guard<std::mutex> lock(g_lock);
                idle = activeToasts.empty() && g_queue.empty();
            }

            if (idle)
            {
                POINT ptSrc{ 0, 0 };
                SIZE sizeWnd{ windowWidth, windowHeight };
                BLENDFUNCTION blend{};
                blend.BlendOp = AC_SRC_OVER;
                blend.BlendFlags = 0;
                blend.SourceConstantAlpha = 0;
                blend.AlphaFormat = AC_SRC_ALPHA;
                UpdateLayeredWindow(hwnd, screenDc, nullptr, &sizeWnd, memDc, &ptSrc, 0, &blend, ULW_ALPHA);

                for (;;)
                {
                    {
                        std::lock_guard<std::mutex> lock(g_lock);
                        if (!g_queue.empty() || !g_running.load())
                            break;
                    }
                    WaitAndPump(INFINITE);
                }
            }

            if (!g_running.load())
                break;

            {
                std::lock_guard<std::mutex> lock(g_lock);
                while (!g_queue.empty())
                {
                    ToastItem item = g_queue.front();
                    g_queue.erase(g_queue.begin());

                    if (activeToasts.size() >= 5)
                    {
                        activeToasts[0].exiting = true;
                        activeToasts[0].exitStartTime = std::chrono::steady_clock::now();
                    }

                    ActiveToast at{};
                    at.id = item.id;
                    at.title = item.title;
                    at.content = item.content;
                    at.type = item.type;
                    at.durationMs = item.durationMs > 0 ? item.durationMs : 5000;
                    at.startTime = std::chrono::steady_clock::now();
                    at.height = ToastStyle::MeasureCardHeight(*graphics, *fonts, at.content);
                    at.currentY = static_cast<float>(windowHeight - ToastStyle::kShadowPad) - at.height;
                    at.slideX = ToastStyle::kSlide;
                    at.alpha = 0.0f;
                    at.exiting = false;

                    activeToasts.push_back(std::move(at));
                }
            }

            auto now = std::chrono::steady_clock::now();

            float stackBottom = static_cast<float>(windowHeight - ToastStyle::kShadowPad);
            for (size_t n = activeToasts.size(); n-- > 0; )
            {
                auto& toast = activeToasts[n];
                const float desiredY = stackBottom - toast.height;
                stackBottom = desiredY - ToastStyle::kGap;
                toast.currentY += (desiredY - toast.currentY) * 0.22f;

                const int elapsed = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(now - toast.startTime).count());

                if (!toast.exiting)
                {
                    if (elapsed < ToastStyle::kEnterMs)
                    {
                        const float ease = ToastStyle::EaseOutQuart(static_cast<float>(elapsed) / ToastStyle::kEnterMs);
                        toast.slideX = ToastStyle::kSlide * (1.0f - ease);
                        toast.alpha = 255.0f * ease;
                    }
                    else
                    {
                        toast.slideX = 0.0f;
                        toast.alpha = 255.0f;

                        if (elapsed >= toast.durationMs)
                        {
                            toast.exiting = true;
                            toast.exitStartTime = now;
                        }
                    }
                }
                else
                {
                    const int exitElapsed = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(now - toast.exitStartTime).count());
                    const float ease = ToastStyle::EaseInCubic(static_cast<float>(exitElapsed) / ToastStyle::kExitMs);
                    toast.slideX = ToastStyle::kSlide * ease;
                    toast.alpha = 255.0f * (1.0f - ease);
                }
            }

            for (auto it = activeToasts.begin(); it != activeToasts.end(); )
            {
                if (it->exiting)
                {
                    int exitElapsed = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(now - it->exitStartTime).count());
                    if (exitElapsed >= ToastStyle::kExitMs)
                    {
                        it = activeToasts.erase(it);
                        continue;
                    }
                }
                ++it;
            }

            int targetX = 100, targetY = 100;
            HWND rbxWnd = FindWindowW(L"WINDOWSCLIENT", nullptr);
            RECT rc{};
            if (rbxWnd && IsWindow(rbxWnd) && !IsIconic(rbxWnd) && GetWindowRect(rbxWnd, &rc) && (rc.right - rc.left > 200))
            {
                targetX = rc.right - windowWidth + ToastStyle::kShadowPad - ToastStyle::kScreenMargin;
                targetY = rc.bottom - windowHeight + ToastStyle::kShadowPad - ToastStyle::kScreenMargin;
            }
            else
            {
                RECT work{};
                SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
                targetX = work.right - windowWidth + ToastStyle::kShadowPad - ToastStyle::kScreenMargin;
                targetY = work.bottom - windowHeight + ToastStyle::kShadowPad - ToastStyle::kScreenMargin;
            }

            {
                graphics->Clear(Gdiplus::Color(0, 0, 0, 0));

                for (const auto& toast : activeToasts)
                {
                    if (toast.alpha <= 1.0f)
                        continue;

                    const int elapsed = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(now - toast.startTime).count());
                    const float remaining = 1.0f - static_cast<float>(elapsed) / static_cast<float>(toast.durationMs);

                    ToastStyle::PaintCard(*graphics, *fonts, logo.get(), toast.title, toast.content, toast.type,
                        static_cast<float>(ToastStyle::kShadowPad) + toast.slideX, toast.currentY, toast.height,
                        (std::clamp)(toast.alpha / 255.0f, 0.0f, 1.0f),
                        (std::clamp)(remaining, 0.0f, 1.0f));
                }

                graphics->Flush(Gdiplus::FlushIntentionSync);
            }

            VMProtectBeginMutation("brand.toast.antitamper");
            {
                BYTE tamperAlpha = 0;
                DWORD tamperFlags = 0;
                if (GetLayeredWindowAttributes(hwnd, nullptr, &tamperAlpha, &tamperFlags))
                {
                    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
                    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex & ~static_cast<LONG_PTR>(WS_EX_LAYERED));
                    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex | WS_EX_LAYERED);
                }

                LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
                if ((ex & WS_EX_LAYERED) == 0)
                    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex | WS_EX_LAYERED);

                if ((ex & WS_EX_TOPMOST) == 0)
                    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex | WS_EX_TOPMOST);

                SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOSENDCHANGING);

                if (!IsWindowVisible(hwnd))
                    ShowWindow(hwnd, SW_SHOWNOACTIVATE);

                SetWindowRgn(hwnd, nullptr, FALSE);
            }
            VMProtectEnd();

            POINT ptSrc{ 0, 0 };
            POINT ptDst{ targetX, targetY };
            SIZE sizeWnd{ windowWidth, windowHeight };
            BLENDFUNCTION blend{};
            blend.BlendOp = AC_SRC_OVER;
            blend.BlendFlags = 0;
            blend.SourceConstantAlpha = 255;
            blend.AlphaFormat = AC_SRC_ALPHA;

            UpdateLayeredWindow(hwnd, screenDc, &ptDst, &sizeWnd, memDc, &ptSrc, 0, &blend, ULW_ALPHA);

            WaitAndPump(16);
        }

        logo.reset();
        fonts.reset();
        graphics.reset();
        surface.reset();

        SelectObject(memDc, oldBmp);
        DeleteObject(hBmp);
        DeleteDC(memDc);
        ReleaseDC(nullptr, screenDc);

        DestroyWindow(hwnd);
        UnregisterClassW(className.c_str(), wc.hInstance);
        Gdiplus::GdiplusShutdown(gdiplusToken);
    }

    inline void Start()
    {
        bool expected = false;
        if (g_started.compare_exchange_strong(expected, true))
        {
            SUPQ_LOG("toast: Start spawning worker");
            g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            g_running.store(true);
            std::thread(WorkerThread).detach();
            SUPQ_LOG("toast: Start returned");
        }
    }

    inline void Push(const ToastItem& item)
    {
        Start();
        {
            std::lock_guard<std::mutex> lock(g_lock);
            g_queue.push_back(item);
        }
        if (g_wake)
            SetEvent(g_wake);
    }
}

    inline bool Initialize()
    {
        bool expected = false;
        if (detail::g_installed.compare_exchange_strong(expected, true))
        {
            ToastOverlay::Start();
        }
        return true;
    }

    inline void PushToast(const ImGuiToast& toast)
    {
        Initialize();

        const char* rawTitle = toast.getTitle() ? toast.getTitle() : BrandGuard::ToastTitle().c_str();
        const char* rawContent = toast.getContent() ? toast.getContent() : BrandGuard::ToastUrl().c_str();

        if (strcmp(rawTitle, BrandGuard::Name().c_str()) == 0)
            rawTitle = BrandGuard::ToastTitle().c_str();
        if (strcmp(rawContent, SUPQ_OBF("Loaded successfully").c_str()) == 0)
            rawContent = BrandGuard::ToastUrl().c_str();

        ToastOverlay::ToastItem item;
        item.id = ToastOverlay::g_nextId.fetch_add(1);
        item.title = ToastOverlay::ToWString(rawTitle);
        item.content = ToastOverlay::ToWString(rawContent);
        item.type = toast.getType();
        item.durationMs = toast.getDuration() > 0 ? toast.getDuration() : 5000;

        ToastOverlay::Push(item);
    }

    inline void PushErrorToast(const char* title, const char* content)
    {
        Initialize();

        ToastOverlay::ToastItem item;
        item.id = ToastOverlay::g_nextId.fetch_add(1);
        item.title = ToastOverlay::ToWString(title ? title : "");
        item.content = ToastOverlay::ToWString(content ? content : "");
        item.type = ImGuiToastType::Error;
        item.durationMs = 7000;

        ToastOverlay::Push(item);
    }

    inline Handle Create(Kind kind)
    {
        const Handle handle = detail::g_next.fetch_add(1);

        Object object;
        object.kind = kind;
        if (kind == Kind::Text)
            object.color = ImColor(1.0f, 1.0f, 1.0f, 1.0f);

        std::lock_guard<std::mutex> guard(detail::g_lock);
        detail::g_objects.emplace_back(handle, std::move(object));
        return handle;
    }

    inline bool Alive(Handle handle)
    {
        std::lock_guard<std::mutex> guard(detail::g_lock);
        return detail::find(handle) != nullptr;
    }

    inline bool Read(Handle handle, Object& out)
    {
        std::lock_guard<std::mutex> guard(detail::g_lock);
        Object* object = detail::find(handle);
        if (!object)
            return false;

        out = *object;
        return true;
    }

    inline bool Write(Handle handle, const Object& value)
    {
        std::lock_guard<std::mutex> guard(detail::g_lock);
        Object* object = detail::find(handle);
        if (!object)
            return false;

        *object = value;
        return true;
    }

    inline void Destroy(Handle handle)
    {
        std::lock_guard<std::mutex> guard(detail::g_lock);
        for (auto it = detail::g_objects.begin(); it != detail::g_objects.end(); ++it)
        {
            if (it->first == handle)
            {
                detail::g_objects.erase(it);
                return;
            }
        }
    }

    inline void Reset()
    {
        std::lock_guard<std::mutex> guard(detail::g_lock);
        detail::g_objects.clear();
    }

    inline bool AnyVisible()
    {
        if (Gui::Any())
            return true;

        {
            std::lock_guard<std::mutex> guard(detail::g_immediateLock);
            for (const auto& vm : detail::g_immediate)
            {
                if (!vm.second.empty())
                    return true;
            }
        }

        std::lock_guard<std::mutex> guard(detail::g_lock);
        for (const auto& entry : detail::g_objects)
        {
            if (entry.second.visible)
                return true;
        }
        return false;
    }

    inline void SetImmediate(const void* vm, std::vector<Object> objects)
    {
        std::lock_guard<std::mutex> guard(detail::g_immediateLock);
        for (auto& entry : detail::g_immediate)
        {
            if (entry.first == vm)
            {
                entry.second = std::move(objects);
                return;
            }
        }
        detail::g_immediate.emplace_back(vm, std::move(objects));
    }

    inline void ClearImmediate()
    {
        std::lock_guard<std::mutex> guard(detail::g_immediateLock);
        detail::g_immediate.clear();
    }
}
