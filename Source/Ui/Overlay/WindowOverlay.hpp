#pragma once

#include <Ui/Overlay/Renderer.hpp>

#include <d3dcompiler.h>
#include <dcomp.h>
#include <wincodec.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace WindowOverlay
{
    namespace detail
    {
        enum Stage : int
        {
            NotStarted = 0,
            Window,
            Device,
            Swapchain,
            Composition,
            Imgui,
            Running,
            Failed,
        };

        inline std::atomic<bool> g_started{ false };
        inline std::atomic<int> g_stage{ NotStarted };
        inline std::atomic<int> g_failedAt{ NotStarted };
        inline std::atomic<long> g_lastHr{ S_OK };
        inline std::atomic<unsigned long long> g_frames{ 0 };
        inline std::atomic<bool> g_shown{ false };
        inline std::atomic<int> g_width{ 0 };
        inline std::atomic<int> g_height{ 0 };

        inline HWND g_window = nullptr;
        inline ID3D11Device* g_device = nullptr;
        inline ID3D11DeviceContext* g_context = nullptr;
        inline IDXGISwapChain1* g_swapchain = nullptr;
        inline ID3D11RenderTargetView* g_target = nullptr;
        inline IDCompositionDevice* g_dcomp = nullptr;
        inline IDCompositionTarget* g_dtarget = nullptr;
        inline IDCompositionVisual* g_visual = nullptr;
        inline IWICImagingFactory* g_wic = nullptr;
        inline UINT g_bufferWidth = 0;
        inline UINT g_bufferHeight = 0;
        inline unsigned long long g_frameIndex = 0;
        inline std::atomic<bool> g_captureExcluded{ true };
        inline std::atomic<int> g_captureAffinityApplied{ -1 };

        inline void apply_capture_affinity()
        {
            if (!g_window)
                return;
            const int want = g_captureExcluded.load(std::memory_order_relaxed) ? 1 : 0;
            if (g_captureAffinityApplied.load(std::memory_order_relaxed) == want)
                return;
            if (SetWindowDisplayAffinity(g_window, want ? 0x11 : WDA_NONE))
                g_captureAffinityApplied.store(want, std::memory_order_relaxed);
        }

        template <typename T>
        inline void release(T*& object)
        {
            if (object)
            {
                object->Release();
                object = nullptr;
            }
        }

        inline bool fail(Stage stage, HRESULT hr)
        {
            g_failedAt.store(stage);
            g_lastHr.store(hr);
            g_stage.store(Failed);
            SUPQ_LOG("overlay: bring-up failed at stage %d hr=0x%08X", stage, static_cast<unsigned int>(hr));
            return false;
        }

        constexpr int SizeCount = 5;
        constexpr float Sizes[SizeCount] = { 14.0f, 20.0f, 28.0f, 40.0f, 64.0f };
        inline ImFont* g_faces[4][SizeCount] = {};

        inline ImFont* pick_font(Renderer::Font font, float size)
        {
            int face = static_cast<int>(font);
            if (face < 0 || face > 3)
                face = static_cast<int>(Renderer::Font::System);

            ImFont* largest = nullptr;
            for (int i = 0; i < SizeCount; ++i)
            {
                ImFont* candidate = g_faces[face][i];
                if (!candidate)
                    continue;

                if (Sizes[i] >= size)
                    return candidate;
                largest = candidate;
            }

            return largest;
        }

        inline void load_fonts()
        {
            ImGuiIO& io = ImGui::GetIO();
            io.Fonts->AddFontDefault();

            wchar_t windows[MAX_PATH] = {};
            GetWindowsDirectoryW(windows, MAX_PATH);

            const wchar_t* files[4] = { L"segoeui.ttf", L"arial.ttf", L"segoeui.ttf", L"consola.ttf" };

            ImFontConfig config;
            config.OversampleH = 2;
            config.OversampleV = 1;
            config.PixelSnapH = false;

            for (int face = 0; face < 4; ++face)
            {
                bool shared = false;
                for (int earlier = 0; earlier < face && !shared; ++earlier)
                {
                    if (wcscmp(files[earlier], files[face]) == 0)
                    {
                        for (int i = 0; i < SizeCount; ++i)
                            g_faces[face][i] = g_faces[earlier][i];
                        shared = true;
                    }
                }
                if (shared)
                    continue;

                wchar_t path[MAX_PATH] = {};
                _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\Fonts\\%s", windows, files[face]);
                if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
                    continue;

                char narrow[MAX_PATH * 3] = {};
                WideCharToMultiByte(CP_UTF8, 0, path, -1, narrow, sizeof(narrow), nullptr, nullptr);

                for (int i = 0; i < SizeCount; ++i)
                    g_faces[face][i] = io.Fonts->AddFontFromFileTTF(narrow, Sizes[i], &config, io.Fonts->GetGlyphRangesDefault());
            }
        }

        struct PendingFont
        {
            int id;
            std::vector<unsigned char> data;
            float pixelSize;
            std::vector<ImWchar> ranges;
        };

        inline std::mutex g_fontQueueLock;
        inline std::vector<PendingFont> g_fontQueue;
        inline std::unordered_map<int, ImFont*> g_customFonts;
        inline std::deque<std::vector<ImWchar>> g_rangeStorage;

        inline ImFont* custom_font(int id)
        {
            auto it = g_customFonts.find(id);
            return it == g_customFonts.end() ? nullptr : it->second;
        }

        inline void add_pending_fonts()
        {
            std::vector<PendingFont> pending;
            {
                std::lock_guard<std::mutex> guard(g_fontQueueLock);
                pending.swap(g_fontQueue);
            }
            if (pending.empty())
                return;

            ImGuiIO& io = ImGui::GetIO();
            for (PendingFont& font : pending)
            {
                const ImWchar* ranges = io.Fonts->GetGlyphRangesDefault();
                if (!font.ranges.empty())
                {
                    g_rangeStorage.push_back(std::move(font.ranges));
                    ranges = g_rangeStorage.back().data();
                }

                void* copy = IM_ALLOC(font.data.size());
                std::memcpy(copy, font.data.data(), font.data.size());

                ImFontConfig config;
                config.FontDataOwnedByAtlas = true;
                config.OversampleH = 2;
                config.OversampleV = 1;
                if (ImFont* face = io.Fonts->AddFontFromMemoryTTF(copy, static_cast<int>(font.data.size()),
                    font.pixelSize, &config, ranges))
                {
                    g_customFonts[font.id] = face;
                }
            }

            ImGui_ImplDX11_InvalidateDeviceObjects();
        }

        struct Texture
        {
            ID3D11ShaderResourceView* view = nullptr;
            unsigned long long lastUsed = 0;
            bool failed = false;
        };

        inline std::unordered_map<std::uint64_t, Texture> g_textures;

        inline ID3D11ShaderResourceView* decode_image(const std::string& data)
        {
            if (!g_wic || data.empty() || data.size() > 0x7FFFFFFF)
                return nullptr;

            IWICStream* stream = nullptr;
            IWICBitmapDecoder* decoder = nullptr;
            IWICBitmapFrameDecode* frame = nullptr;
            IWICFormatConverter* converter = nullptr;
            ID3D11Texture2D* texture = nullptr;
            ID3D11ShaderResourceView* view = nullptr;

            UINT width = 0;
            UINT height = 0;
            std::vector<BYTE> pixels;

            bool ok = SUCCEEDED(g_wic->CreateStream(&stream))
                && SUCCEEDED(stream->InitializeFromMemory(
                    reinterpret_cast<BYTE*>(const_cast<char*>(data.data())), static_cast<DWORD>(data.size())))
                && SUCCEEDED(g_wic->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder))
                && SUCCEEDED(decoder->GetFrame(0, &frame))
                && SUCCEEDED(g_wic->CreateFormatConverter(&converter))
                && SUCCEEDED(converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
                    nullptr, 0.0, WICBitmapPaletteTypeCustom))
                && SUCCEEDED(converter->GetSize(&width, &height))
                && width > 0 && height > 0 && width <= 8192 && height <= 8192;

            if (ok)
            {
                pixels.resize(static_cast<size_t>(width) * height * 4);
                ok = SUCCEEDED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data()));
            }

            if (ok)
            {
                D3D11_TEXTURE2D_DESC desc{};
                desc.Width = width;
                desc.Height = height;
                desc.MipLevels = 1;
                desc.ArraySize = 1;
                desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                desc.SampleDesc.Count = 1;
                desc.Usage = D3D11_USAGE_IMMUTABLE;
                desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

                D3D11_SUBRESOURCE_DATA initial{};
                initial.pSysMem = pixels.data();
                initial.SysMemPitch = width * 4;

                ok = SUCCEEDED(g_device->CreateTexture2D(&desc, &initial, &texture))
                    && SUCCEEDED(g_device->CreateShaderResourceView(texture, nullptr, &view));
            }

            release(texture);
            release(converter);
            release(frame);
            release(decoder);
            release(stream);

            if (!ok)
                release(view);
            return view;
        }

        inline ImTextureID image_for(const Renderer::Object& object)
        {
            if (object.data.empty() || object.dataHash == 0)
                return nullptr;

            Texture& entry = g_textures[object.dataHash];
            entry.lastUsed = g_frameIndex;
            if (!entry.view && !entry.failed)
            {
                entry.view = decode_image(object.data);
                entry.failed = entry.view == nullptr;
            }

            return entry.view;
        }

        inline void evict_textures()
        {
            if (g_frameIndex % 600 != 0)
                return;

            for (auto it = g_textures.begin(); it != g_textures.end();)
            {
                if (g_frameIndex - it->second.lastUsed > 600)
                {
                    release(it->second.view);
                    it = g_textures.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        struct GameSearch
        {
            DWORD pid;
            HWND best;
            long long bestArea;
        };

        struct ShaderProgram
        {
            std::string vertexBlob;
            std::string pixelBlob;
            std::uint64_t version = 0;
            std::uint64_t built = 0;
            ID3D11VertexShader* vertex = nullptr;
            ID3D11PixelShader* pixel = nullptr;
        };

        struct ShaderParams
        {
            float time;
            float resolution[2];
            float pad0;
            float mouse[2];
            float pad1[2];
        };

        struct ShaderDraw
        {
            int id;
            float x, y, width, height;
        };

        inline std::mutex g_shaderLock;
        inline std::unordered_map<int, ShaderProgram> g_shaders;
        inline std::vector<ShaderProgram> g_shaderGraveyard;
        inline int g_nextShader = 1;
        inline ID3D11Buffer* g_shaderParams = nullptr;
        inline std::deque<ShaderDraw> g_shaderDraws;
        inline LARGE_INTEGER g_shaderEpoch{};

        inline void release_program(ShaderProgram& program)
        {
            release(program.vertex);
            release(program.pixel);
        }

        inline void collect_shaders()
        {
            g_shaderDraws.clear();
            std::lock_guard<std::mutex> guard(g_shaderLock);
            for (ShaderProgram& program : g_shaderGraveyard)
                release_program(program);
            g_shaderGraveyard.clear();
        }

        inline void shader_callback(const ImDrawList*, const ImDrawCmd* command)
        {
            const auto* draw = static_cast<const ShaderDraw*>(command->UserCallbackData);
            if (!draw || draw->width <= 0.0f || draw->height <= 0.0f)
                return;

            std::lock_guard<std::mutex> guard(g_shaderLock);
            auto it = g_shaders.find(draw->id);
            if (it == g_shaders.end() || it->second.vertexBlob.empty() || it->second.pixelBlob.empty())
                return;

            ShaderProgram& program = it->second;
            if (program.built != program.version)
            {
                release_program(program);
                program.built = program.version;
                if (FAILED(g_device->CreateVertexShader(program.vertexBlob.data(), program.vertexBlob.size(), nullptr, &program.vertex)) ||
                    FAILED(g_device->CreatePixelShader(program.pixelBlob.data(), program.pixelBlob.size(), nullptr, &program.pixel)))
                {
                    release_program(program);
                }
            }
            if (!program.vertex || !program.pixel)
                return;

            if (!g_shaderParams)
            {
                D3D11_BUFFER_DESC desc{};
                desc.ByteWidth = sizeof(ShaderParams);
                desc.Usage = D3D11_USAGE_DYNAMIC;
                desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                if (FAILED(g_device->CreateBuffer(&desc, nullptr, &g_shaderParams)))
                    return;
                QueryPerformanceCounter(&g_shaderEpoch);
            }

            LARGE_INTEGER now{}, frequency{};
            QueryPerformanceCounter(&now);
            QueryPerformanceFrequency(&frequency);
            POINT cursor{};
            GetCursorPos(&cursor);
            ScreenToClient(g_window, &cursor);

            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(g_context->Map(g_shaderParams, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            {
                ShaderParams params{};
                params.time = static_cast<float>(static_cast<double>(now.QuadPart - g_shaderEpoch.QuadPart) / frequency.QuadPart);
                params.resolution[0] = draw->width;
                params.resolution[1] = draw->height;
                params.mouse[0] = static_cast<float>(cursor.x) - draw->x;
                params.mouse[1] = static_cast<float>(cursor.y) - draw->y;
                std::memcpy(mapped.pData, &params, sizeof(params));
                g_context->Unmap(g_shaderParams, 0);
            }

            const D3D11_VIEWPORT viewport{ draw->x, draw->y, draw->width, draw->height, 0.0f, 1.0f };
            const D3D11_RECT scissor{
                static_cast<LONG>(draw->x), static_cast<LONG>(draw->y),
                static_cast<LONG>(draw->x + draw->width), static_cast<LONG>(draw->y + draw->height) };
            g_context->RSSetViewports(1, &viewport);
            g_context->RSSetScissorRects(1, &scissor);
            g_context->IASetInputLayout(nullptr);
            g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            g_context->VSSetShader(program.vertex, nullptr, 0);
            g_context->VSSetConstantBuffers(0, 1, &g_shaderParams);
            g_context->PSSetShader(program.pixel, nullptr, 0);
            g_context->PSSetConstantBuffers(0, 1, &g_shaderParams);
            g_context->GSSetShader(nullptr, nullptr, 0);
            g_context->Draw(3, 0);
        }

        inline void draw_shader(ImDrawList* list, const Renderer::Object& object)
        {
            ShaderDraw draw{ object.shaderId, object.position.x, object.position.y, object.size.x, object.size.y };
            if (draw.width <= 0.0f || draw.height <= 0.0f)
            {
                draw.width = static_cast<float>(g_bufferWidth) - draw.x;
                draw.height = static_cast<float>(g_bufferHeight) - draw.y;
            }
            g_shaderDraws.push_back(draw);
            list->AddCallback(shader_callback, &g_shaderDraws.back());
            list->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
        }

        inline BOOL CALLBACK pick_game_window(HWND hwnd, LPARAM param)
        {
            auto* search = reinterpret_cast<GameSearch*>(param);

            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid != search->pid || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER))
                return TRUE;

            const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
            if (ex & (WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE))
                return TRUE;

            RECT rc{};
            if (!GetClientRect(hwnd, &rc))
                return TRUE;

            const long long area = static_cast<long long>(rc.right - rc.left) * (rc.bottom - rc.top);
            if (area > search->bestArea)
            {
                search->bestArea = area;
                search->best = hwnd;
            }

            return TRUE;
        }

        inline HWND find_game_window()
        {
            GameSearch search{ GetCurrentProcessId(), nullptr, 0 };
            EnumWindows(pick_game_window, reinterpret_cast<LPARAM>(&search));
            return search.best;
        }

        inline bool game_is_foreground()
        {
            HWND foreground = GetForegroundWindow();
            if (!foreground)
                return false;

            DWORD pid = 0;
            GetWindowThreadProcessId(foreground, &pid);
            return pid == GetCurrentProcessId();
        }

        inline std::atomic<bool> g_interactive{ false };
        inline bool g_keyboard = false;

        inline ImGuiKey map_key(WPARAM vk)
        {
            switch (vk)
            {
            case VK_TAB: return ImGuiKey_Tab;
            case VK_LEFT: return ImGuiKey_LeftArrow;
            case VK_RIGHT: return ImGuiKey_RightArrow;
            case VK_UP: return ImGuiKey_UpArrow;
            case VK_DOWN: return ImGuiKey_DownArrow;
            case VK_PRIOR: return ImGuiKey_PageUp;
            case VK_NEXT: return ImGuiKey_PageDown;
            case VK_HOME: return ImGuiKey_Home;
            case VK_END: return ImGuiKey_End;
            case VK_INSERT: return ImGuiKey_Insert;
            case VK_DELETE: return ImGuiKey_Delete;
            case VK_BACK: return ImGuiKey_Backspace;
            case VK_SPACE: return ImGuiKey_Space;
            case VK_RETURN: return ImGuiKey_Enter;
            case VK_ESCAPE: return ImGuiKey_Escape;
            case 'A': return ImGuiKey_A;
            case 'C': return ImGuiKey_C;
            case 'V': return ImGuiKey_V;
            case 'X': return ImGuiKey_X;
            case 'Y': return ImGuiKey_Y;
            case 'Z': return ImGuiKey_Z;
            default: return ImGuiKey_None;
            }
        }

        inline LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
        {
            const bool imgui = ImGui::GetCurrentContext() != nullptr;
            switch (message)
            {
            case WM_NCHITTEST:
                return g_interactive.load() ? HTCLIENT : HTTRANSPARENT;
            case WM_MOUSEACTIVATE:
                return MA_NOACTIVATE;
            case WM_SETCURSOR:
                SetCursor(LoadCursorW(nullptr, IDC_ARROW));
                return TRUE;
            case WM_MOUSEWHEEL:
                if (imgui)
                    ImGui::GetIO().AddMouseWheelEvent(0.0f, static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA);
                return 0;
            case WM_KEYDOWN:
            case WM_KEYUP:
            case WM_SYSKEYDOWN:
            case WM_SYSKEYUP:
                if (imgui)
                {
                    ImGuiIO& io = ImGui::GetIO();
                    const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
                    io.AddKeyEvent(ImGuiKey_ModCtrl, (GetKeyState(VK_CONTROL) & 0x8000) != 0);
                    io.AddKeyEvent(ImGuiKey_ModShift, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
                    io.AddKeyEvent(ImGuiKey_ModAlt, (GetKeyState(VK_MENU) & 0x8000) != 0);
                    if (const ImGuiKey key = map_key(wparam); key != ImGuiKey_None)
                        io.AddKeyEvent(key, down);
                }
                return 0;
            case WM_CHAR:
                if (imgui && wparam > 0 && wparam < 0x10000)
                    ImGui::GetIO().AddInputCharacterUTF16(static_cast<unsigned short>(wparam));
                return 0;
            default:
                return DefWindowProcW(hwnd, message, wparam, lparam);
            }
        }

        inline void set_interactive(bool interactive)
        {
            if (g_interactive.exchange(interactive) == interactive)
                return;
            const LONG_PTR ex = GetWindowLongPtrW(g_window, GWL_EXSTYLE);
            SetWindowLongPtrW(g_window, GWL_EXSTYLE,
                interactive ? (ex & ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT)) : (ex | WS_EX_TRANSPARENT));
        }

        inline void feed_mouse()
        {
            ImGuiIO& io = ImGui::GetIO();
            if (!Renderer::Gui::Any())
            {
                io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
                return;
            }

            io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
            POINT cursor{};
            if (GetCursorPos(&cursor) && ScreenToClient(g_window, &cursor))
                io.AddMousePosEvent(static_cast<float>(cursor.x), static_cast<float>(cursor.y));
            io.AddMouseButtonEvent(0, (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0);
            io.AddMouseButtonEvent(1, (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0);
            io.AddMouseButtonEvent(2, (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0);
        }

        inline void update_focus(HWND game)
        {
            ImGuiIO& io = ImGui::GetIO();
            const bool gui = Renderer::Gui::Any();
            set_interactive(gui && io.WantCaptureMouse);

            if (g_keyboard && GetForegroundWindow() != g_window)
            {
                g_keyboard = false;
                io.AddFocusEvent(false);
            }

            const bool wantKeyboard = gui && io.WantTextInput;
            if (wantKeyboard && !g_keyboard)
            {
                g_keyboard = SetForegroundWindow(g_window) != FALSE;
                if (g_keyboard)
                    io.AddFocusEvent(true);
            }
            else if (!wantKeyboard && g_keyboard)
            {
                g_keyboard = false;
                if (game && IsWindow(game))
                    SetForegroundWindow(game);
            }
        }

        inline std::wstring random_name()
        {
            LARGE_INTEGER counter{};
            QueryPerformanceCounter(&counter);
            std::uint64_t seed = static_cast<std::uint64_t>(counter.QuadPart)
                ^ (static_cast<std::uint64_t>(GetCurrentThreadId()) * 0x9E3779B97F4A7C15ULL);
            seed ^= seed >> 33; seed *= 0xff51afd7ed558ccdULL;
            seed ^= seed >> 33; seed *= 0xc4ceb9fe1a85ec53ULL;
            seed ^= seed >> 33;

            static const wchar_t hex[] = L"0123456789abcdef";
            std::wstring name(17, L'W');
            for (int i = 0; i < 16; ++i)
                name[1 + i] = hex[(seed >> (i * 4)) & 0xF];
            return name;
        }

        inline bool create_window()
        {
            static const std::wstring className = random_name();

            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = window_proc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = className.c_str();
            RegisterClassExW(&wc);

            g_window = CreateWindowExW(
                WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT |
                    WS_EX_NOREDIRECTIONBITMAP,
                className.c_str(), random_name().c_str(), WS_POPUP,
                0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);

            if (!g_window)
                return fail(Window, HRESULT_FROM_WIN32(GetLastError()));

            SetLayeredWindowAttributes(g_window, 0, 255, LWA_ALPHA);
            g_captureAffinityApplied.store(-1, std::memory_order_relaxed);
            apply_capture_affinity();
            return true;
        }

        inline bool create_target()
        {
            ID3D11Texture2D* backbuffer = nullptr;
            HRESULT hr = g_swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backbuffer));
            if (SUCCEEDED(hr))
                hr = g_device->CreateRenderTargetView(backbuffer, nullptr, &g_target);
            release(backbuffer);
            return SUCCEEDED(hr);
        }

        inline bool create_device(UINT width, UINT height)
        {
            const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
            HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &g_device, nullptr, &g_context);
            if (FAILED(hr))
                return fail(Device, hr);

            g_stage.store(Swapchain);

            IDXGIDevice* dxgiDevice = nullptr;
            IDXGIAdapter* adapter = nullptr;
            IDXGIFactory2* factory = nullptr;

            hr = g_device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice));
            if (SUCCEEDED(hr))
                hr = dxgiDevice->GetAdapter(&adapter);
            if (SUCCEEDED(hr))
                hr = adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory));

            if (SUCCEEDED(hr))
            {
                DXGI_SWAP_CHAIN_DESC1 desc{};
                desc.Width = width;
                desc.Height = height;
                desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                desc.SampleDesc.Count = 1;
                desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                desc.BufferCount = 2;
                desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
                desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
                desc.Scaling = DXGI_SCALING_STRETCH;
                hr = factory->CreateSwapChainForComposition(g_device, &desc, nullptr, &g_swapchain);
            }

            release(factory);
            release(adapter);

            if (FAILED(hr))
            {
                release(dxgiDevice);
                return fail(Swapchain, hr);
            }

            g_bufferWidth = width;
            g_bufferHeight = height;

            g_stage.store(Composition);

            hr = DCompositionCreateDevice(dxgiDevice, __uuidof(IDCompositionDevice), reinterpret_cast<void**>(&g_dcomp));
            release(dxgiDevice);
            if (SUCCEEDED(hr))
                hr = g_dcomp->CreateTargetForHwnd(g_window, TRUE, &g_dtarget);
            if (SUCCEEDED(hr))
                hr = g_dcomp->CreateVisual(&g_visual);
            if (SUCCEEDED(hr))
                hr = g_visual->SetContent(g_swapchain);
            if (SUCCEEDED(hr))
                hr = g_dtarget->SetRoot(g_visual);
            if (SUCCEEDED(hr))
                hr = g_dcomp->Commit();
            if (FAILED(hr))
                return fail(Composition, hr);

            if (!create_target())
                return fail(Composition, E_FAIL);

            return true;
        }

        inline bool create_imgui()
        {
            g_stage.store(Imgui);

            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                __uuidof(IWICImagingFactory), reinterpret_cast<void**>(&g_wic));

            IMGUI_CHECKVERSION();
            ImGui::CreateContext();

            ImGuiIO& io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.LogFilename = nullptr;
            io.ConfigFlags |= ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoMouseCursorChange;
            io.DisplaySize = ImVec2(static_cast<float>(g_bufferWidth), static_cast<float>(g_bufferHeight));

            load_fonts();

            if (!ImGui_ImplDX11_Init(g_device, g_context))
                return fail(Imgui, E_FAIL);

            Renderer::detail::g_pickFont = pick_font;
            Renderer::detail::g_imageFor = image_for;
            Renderer::detail::g_customFont = custom_font;
            Renderer::detail::g_drawShader = draw_shader;
            return true;
        }

        inline void resize(UINT width, UINT height)
        {
            if (width == g_bufferWidth && height == g_bufferHeight)
                return;

            release(g_target);
            if (FAILED(g_swapchain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0)))
                return;

            g_bufferWidth = width;
            g_bufferHeight = height;
            create_target();
        }

        inline void pump()
        {
            apply_capture_affinity();
            MSG message;
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }

        inline void wait(DWORD milliseconds)
        {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, milliseconds, QS_ALLINPUT);
            pump();
        }

        inline void hide()
        {
            set_interactive(false);
            g_keyboard = false;
            if (g_shown.exchange(false))
                ShowWindow(g_window, SW_HIDE);
        }

        inline void paint_frame(float deltaTime, HWND game)
        {
            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = ImVec2(static_cast<float>(g_bufferWidth), static_cast<float>(g_bufferHeight));
            io.DeltaTime = deltaTime > 0.0f ? deltaTime : 1.0f / 60.0f;

            add_pending_fonts();
            collect_shaders();
            feed_mouse();
            ImGui_ImplDX11_NewFrame();
            ImGui::NewFrame();
            update_focus(game);
            Renderer::detail::render();
            Renderer::Gui::Render();
            ImGui::Render();

            const float clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            g_context->OMSetRenderTargets(1, &g_target, nullptr);
            g_context->ClearRenderTargetView(g_target, clear);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

            const bool vsync = Renderer::Gui::g_vsync.load(std::memory_order_relaxed);
            g_swapchain->Present(vsync ? 1 : 0, 0);
            if (!vsync)
                wait(1);

            ++g_frameIndex;
            g_frames.fetch_add(1, std::memory_order_relaxed);
            evict_textures();
        }

        inline void thread_main()
        {
            SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

            g_stage.store(Window);
            if (!create_window())
                return;

            g_stage.store(Device);
            if (!create_device(64, 64) || !create_imgui())
                return;

            g_stage.store(Running);
            SUPQ_LOG("overlay: running");

            HWND game = nullptr;
            ULONGLONG lastSearch = 0;
            RECT placed{};

            LARGE_INTEGER frequency{}, last{};
            QueryPerformanceFrequency(&frequency);
            QueryPerformanceCounter(&last);

            for (;;)
            {
                pump();

                const ULONGLONG now = GetTickCount64();
                if (!game || !IsWindow(game) || now - lastSearch > 1000)
                {
                    game = find_game_window();
                    lastSearch = now;
                }

                if (!game || IsIconic(game) || !game_is_foreground() || !Renderer::AnyVisible())
                {
                    hide();
                    wait(50);
                    continue;
                }

                RECT client{};
                POINT origin{ 0, 0 };
                if (!GetClientRect(game, &client) || !ClientToScreen(game, &origin) ||
                    client.right <= 0 || client.bottom <= 0)
                {
                    hide();
                    wait(50);
                    continue;
                }

                const RECT target{ origin.x, origin.y, origin.x + client.right, origin.y + client.bottom };
                if (!EqualRect(&target, &placed) || !g_shown.load())
                {
                    resize(static_cast<UINT>(client.right), static_cast<UINT>(client.bottom));
                    SetWindowPos(g_window, HWND_TOPMOST, target.left, target.top, client.right, client.bottom,
                        SWP_NOACTIVATE | SWP_SHOWWINDOW);
                    placed = target;
                    g_shown.store(true);
                    g_width.store(client.right);
                    g_height.store(client.bottom);
                }

                if (!g_target)
                {
                    wait(50);
                    continue;
                }

                LARGE_INTEGER current{};
                QueryPerformanceCounter(&current);
                const float delta = static_cast<float>(current.QuadPart - last.QuadPart) /
                    static_cast<float>(frequency.QuadPart);
                last = current;

                paint_frame(delta, game);
            }
        }
    }

    inline void QueueFont(int id, const std::vector<unsigned char>& data, float pixelSize,
        std::vector<ImWchar> ranges)
    {
        std::lock_guard<std::mutex> guard(detail::g_fontQueueLock);
        detail::g_fontQueue.push_back({ id, data, pixelSize, std::move(ranges) });
    }

    inline void Start()
    {
        bool expected = false;
        if (detail::g_started.compare_exchange_strong(expected, true))
            std::thread(detail::thread_main).detach();
    }

    inline void SetCaptureExcluded(bool excluded)
    {
        detail::g_captureExcluded.store(excluded, std::memory_order_relaxed);
        detail::apply_capture_affinity();
        if (detail::g_window)
            PostMessageW(detail::g_window, WM_NULL, 0, 0);
    }

    inline bool IsCaptureExcluded()
    {
        return detail::g_captureExcluded.load(std::memory_order_relaxed);
    }

    inline int NewShader()
    {
        std::lock_guard<std::mutex> guard(detail::g_shaderLock);
        const int id = detail::g_nextShader++;
        detail::g_shaders.emplace(id, detail::ShaderProgram{});
        return id;
    }

    inline void ReleaseShader(int id)
    {
        std::lock_guard<std::mutex> guard(detail::g_shaderLock);
        auto it = detail::g_shaders.find(id);
        if (it == detail::g_shaders.end())
            return;
        detail::g_shaderGraveyard.push_back(it->second);
        detail::g_shaders.erase(it);
    }

    inline bool CompileShader(int id, const std::string& vertex, const std::string& pixel, std::string& error)
    {
        const bool sm5 = !detail::g_device || detail::g_device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_0;

        auto compile = [&](const std::string& source, const char* name, const char* target, std::string& blob) -> bool
        {
            ID3DBlob* code = nullptr;
            ID3DBlob* messages = nullptr;
            const HRESULT hr = D3DCompile(source.data(), source.size(), name, nullptr, nullptr, "main", target,
                D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &messages);
            if (messages)
            {
                if (FAILED(hr))
                    error.assign(static_cast<const char*>(messages->GetBufferPointer()), messages->GetBufferSize());
                messages->Release();
            }
            if (FAILED(hr) || !code)
            {
                if (error.empty())
                    error = std::string(name) + " shader failed to compile";
                detail::release(code);
                return false;
            }
            blob.assign(static_cast<const char*>(code->GetBufferPointer()), code->GetBufferSize());
            code->Release();
            return true;
        };

        if (vertex.empty() || pixel.empty())
        {
            error = "Vertex and Pixel must both be set before Create";
            return false;
        }

        std::string vertexBlob;
        std::string pixelBlob;
        if (!compile(vertex, "Vertex", sm5 ? "vs_5_0" : "vs_4_0", vertexBlob) ||
            !compile(pixel, "Pixel", sm5 ? "ps_5_0" : "ps_4_0", pixelBlob))
            return false;

        std::lock_guard<std::mutex> guard(detail::g_shaderLock);
        auto it = detail::g_shaders.find(id);
        if (it == detail::g_shaders.end())
        {
            error = "shader has been destroyed";
            return false;
        }
        it->second.vertexBlob = std::move(vertexBlob);
        it->second.pixelBlob = std::move(pixelBlob);
        ++it->second.version;
        return true;
    }

    inline std::string DebugState()
    {
        char buffer[160];
        _snprintf_s(buffer, sizeof(buffer), _TRUNCATE,
            "window-overlay stage=%d failedAt=%d hr=0x%08X frames=%llu shown=%d size=%dx%d",
            detail::g_stage.load(),
            detail::g_failedAt.load(),
            static_cast<unsigned int>(detail::g_lastHr.load()),
            detail::g_frames.load(),
            detail::g_shown.load() ? 1 : 0,
            detail::g_width.load(),
            detail::g_height.load());
        return std::string(buffer);
    }
}
