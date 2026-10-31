#pragma once

#include <imgui.h>

#include <Security/BrandGuard.hpp>

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cstdio>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Renderer::Gui
{
    enum class ElementKind
    {
        Text,
        Button,
        Checkbox,
        SliderFloat,
        SliderInt,
        InputText,
        ColorPicker,
        Separator,
        SameLine,
        HeaderBegin,
        HeaderEnd,
    };

    struct Element
    {
        ElementKind kind = ElementKind::Text;
        int id = 0;
        std::string label;
        bool checked = false;
        float number = 0.0f;
        float minimum = 0.0f;
        float maximum = 1.0f;
        int integer = 0;
        int intMinimum = 0;
        int intMaximum = 100;
        char text[256] = {};
        float color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    };

    struct Window
    {
        int id = 0;
        std::string title;
        std::vector<Element> elements;
        int nextElement = 1;
    };

    struct Event
    {
        int window = 0;
        int element = 0;
        ElementKind kind = ElementKind::Button;
        bool checked = false;
        double number = 0.0;
        std::string text;
        float color[4] = {};
    };

    inline std::mutex g_lock;
    inline std::vector<std::unique_ptr<Window>> g_windows;
    inline int g_nextWindow = 1;
    inline std::atomic<bool> g_any{ false };
    inline std::atomic<bool> g_vsync{ true };

    inline std::mutex g_eventLock;
    inline std::vector<Event> g_events;
    inline void (*g_wake)() = nullptr;

    struct PurchasePrompt
    {
        std::string method;
        std::string kind;
        std::string item;
        std::string id;
        std::string price;
        unsigned long long deadline = 0;
        std::atomic<int> decision{ 0 };
    };

    inline unsigned long long NowMs()
    {
        return static_cast<unsigned long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    inline std::mutex g_promptLock;
    inline std::vector<std::shared_ptr<PurchasePrompt>> g_prompts;
    inline std::atomic<int> g_promptCount{ 0 };

    inline void PushPrompt(const std::shared_ptr<PurchasePrompt>& prompt)
    {
        std::lock_guard<std::mutex> guard(g_promptLock);
        g_prompts.push_back(prompt);
        g_promptCount.store(static_cast<int>(g_prompts.size()));
    }

    inline void ClosePrompt(const std::shared_ptr<PurchasePrompt>& prompt, int fallback)
    {
        int expected = 0;
        prompt->decision.compare_exchange_strong(expected, fallback);
        std::lock_guard<std::mutex> guard(g_promptLock);
        g_prompts.erase(std::remove(g_prompts.begin(), g_prompts.end(), prompt), g_prompts.end());
        g_promptCount.store(static_cast<int>(g_prompts.size()));
    }

    inline Window* find(int id)
    {
        for (auto& window : g_windows)
        {
            if (window->id == id)
                return window.get();
        }
        return nullptr;
    }

    inline bool Any()
    {
        return g_any.load(std::memory_order_relaxed) || g_promptCount.load(std::memory_order_relaxed) > 0;
    }

    inline int Create(const std::string& title)
    {
        std::lock_guard<std::mutex> guard(g_lock);
        for (const auto& window : g_windows)
        {
            if (window->title == title)
                return 0;
        }
        auto window = std::make_unique<Window>();
        window->id = g_nextWindow++;
        window->title = title;
        const int id = window->id;
        g_windows.push_back(std::move(window));
        g_any.store(true);
        return id;
    }

    inline int Append(int windowId, Element element)
    {
        std::lock_guard<std::mutex> guard(g_lock);
        Window* window = find(windowId);
        if (!window)
            return 0;
        element.id = window->nextElement++;
        window->elements.push_back(std::move(element));
        return window->elements.back().id;
    }

    inline bool Destroy(int windowId)
    {
        {
            std::lock_guard<std::mutex> guard(g_lock);
            auto it = std::find_if(g_windows.begin(), g_windows.end(),
                [windowId](const std::unique_ptr<Window>& window) { return window->id == windowId; });
            if (it == g_windows.end())
                return false;
            g_windows.erase(it);
            g_any.store(!g_windows.empty());
        }
        std::lock_guard<std::mutex> guard(g_eventLock);
        g_events.erase(std::remove_if(g_events.begin(), g_events.end(),
            [windowId](const Event& event) { return event.window == windowId; }), g_events.end());
        return true;
    }

    inline void Reset()
    {
        {
            std::lock_guard<std::mutex> guard(g_lock);
            g_windows.clear();
            g_any.store(false);
        }
        {
            std::lock_guard<std::mutex> guard(g_promptLock);
            for (auto& prompt : g_prompts)
            {
                int expected = 0;
                prompt->decision.compare_exchange_strong(expected, 2);
            }
            g_prompts.clear();
            g_promptCount.store(0);
        }
        std::lock_guard<std::mutex> guard(g_eventLock);
        g_events.clear();
    }

    inline std::vector<Event> TakeEvents()
    {
        std::lock_guard<std::mutex> guard(g_eventLock);
        std::vector<Event> events;
        events.swap(g_events);
        return events;
    }

    namespace detail
    {
        inline void queue(std::vector<Event>& pending, Event event)
        {
            if (event.kind != ElementKind::Button)
            {
                for (Event& existing : pending)
                {
                    if (existing.window == event.window && existing.element == event.element)
                    {
                        existing = std::move(event);
                        return;
                    }
                }
            }
            pending.push_back(std::move(event));
        }

        inline void render_window(Window& window, int index, std::vector<Event>& pending)
        {
            ImGui::SetNextWindowPos(ImVec2(40.0f + 32.0f * (index % 8), 40.0f + 32.0f * (index % 8)), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(340.0f, 0.0f), ImGuiCond_FirstUseEver);

            ImGui::PushID(window.id);
            const bool open = ImGui::Begin(window.title.c_str(), nullptr, ImGuiWindowFlags_NoSavedSettings);
            if (open)
            {
                int skipDepth = 0;
                for (Element& element : window.elements)
                {
                    if (skipDepth > 0)
                    {
                        if (element.kind == ElementKind::HeaderBegin)
                            ++skipDepth;
                        else if (element.kind == ElementKind::HeaderEnd)
                            --skipDepth;
                        continue;
                    }

                    ImGui::PushID(element.id);
                    Event event;
                    event.window = window.id;
                    event.element = element.id;
                    event.kind = element.kind;
                    bool changed = false;

                    switch (element.kind)
                    {
                    case ElementKind::Text:
                        ImGui::TextWrapped("%s", element.label.c_str());
                        break;
                    case ElementKind::Button:
                        changed = ImGui::Button(element.label.c_str());
                        break;
                    case ElementKind::Checkbox:
                        changed = ImGui::Checkbox(element.label.c_str(), &element.checked);
                        event.checked = element.checked;
                        break;
                    case ElementKind::SliderFloat:
                        changed = ImGui::SliderFloat(element.label.c_str(), &element.number, element.minimum, element.maximum);
                        event.number = element.number;
                        break;
                    case ElementKind::SliderInt:
                        changed = ImGui::SliderInt(element.label.c_str(), &element.integer, element.intMinimum, element.intMaximum);
                        event.number = element.integer;
                        break;
                    case ElementKind::InputText:
                        changed = ImGui::InputText(element.label.c_str(), element.text, sizeof(element.text));
                        event.text = element.text;
                        break;
                    case ElementKind::ColorPicker:
                        changed = ImGui::ColorEdit4(element.label.c_str(), element.color);
                        std::memcpy(event.color, element.color, sizeof(event.color));
                        break;
                    case ElementKind::Separator:
                        ImGui::Separator();
                        break;
                    case ElementKind::SameLine:
                        ImGui::SameLine();
                        break;
                    case ElementKind::HeaderBegin:
                        if (!ImGui::CollapsingHeader(element.label.c_str()))
                            skipDepth = 1;
                        break;
                    case ElementKind::HeaderEnd:
                        break;
                    }

                    if (changed)
                        queue(pending, std::move(event));
                    ImGui::PopID();
                }
            }
            ImGui::End();
            ImGui::PopID();
        }
    }

    namespace detail
    {
        inline ImVec4 rgb(unsigned int hex, float alpha = 1.0f)
        {
            return ImVec4(((hex >> 16) & 0xFF) / 255.0f, ((hex >> 8) & 0xFF) / 255.0f, (hex & 0xFF) / 255.0f, alpha);
        }

        inline void prompt_row(const char* label, const std::string& value, const ImVec4* color = nullptr)
        {
            ImGui::TextDisabled("%s", label);
            ImGui::SameLine(84.0f);
            if (color)
                ImGui::TextColored(*color, "%s", value.c_str());
            else
                ImGui::TextWrapped("%s", value.c_str());
        }

        inline void render_prompt(PurchasePrompt& prompt, int index)
        {
            const unsigned long long now = NowMs();
            if (prompt.decision.load() != 0)
                return;
            if (now >= prompt.deadline)
            {
                int expected = 0;
                prompt.decision.compare_exchange_strong(expected, 2);
                return;
            }

            const ImGuiIO& io = ImGui::GetIO();
            ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f + 24.0f * index, io.DisplaySize.y * 0.5f + 24.0f * index),
                ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowSizeConstraints(ImVec2(380.0f, 0.0f), ImVec2(520.0f, FLT_MAX));

            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f, 16.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 7.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_WindowBg, rgb(0x0A070D, 0.97f));
            ImGui::PushStyleColor(ImGuiCol_TitleBg, rgb(0x140E1A));
            ImGui::PushStyleColor(ImGuiCol_TitleBgActive, rgb(0x140E1A));
            ImGui::PushStyleColor(ImGuiCol_Border, rgb(0x271D30));
            ImGui::PushStyleColor(ImGuiCol_Separator, rgb(0x271D30));
            ImGui::PushStyleColor(ImGuiCol_Text, rgb(0xF4EEFA));
            ImGui::PushStyleColor(ImGuiCol_TextDisabled, rgb(0x9D8FAD));

            char title[160];
            _snprintf_s(title, sizeof(title), _TRUNCATE, "%s  -  Purchase request###purchase_%p",
                BrandGuard::Name().c_str(), static_cast<void*>(&prompt));

            bool open = true;
            if (ImGui::Begin(title, &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::TextWrapped("A script wants to open a Roblox purchase prompt.");
                ImGui::TextDisabled("Allow only if you meant to buy this.");
                ImGui::Separator();

                const ImVec4 accent = rgb(0xC084FC);
                prompt_row("Type", prompt.kind);
                prompt_row("Item", prompt.item);
                if (!prompt.id.empty())
                    prompt_row("ID", prompt.id);
                prompt_row("Price", prompt.price, &accent);
                prompt_row("Call", prompt.method);

                ImGui::Separator();
                ImGui::TextDisabled("Denied automatically in %llus", (prompt.deadline - now + 999) / 1000);

                const float width = (ImGui::GetContentRegionAvail().x - 8.0f) * 0.5f;
                ImGui::PushStyleColor(ImGuiCol_Button, rgb(0xA855F7));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, rgb(0xB872F8));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, rgb(0x9333EA));
                ImGui::PushStyleColor(ImGuiCol_Text, rgb(0xFFFFFF));
                const bool allow = ImGui::Button("Allow", ImVec2(width, 30.0f));
                ImGui::PopStyleColor(4);
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Button, rgb(0x1C1424));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, rgb(0x2A1F35));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, rgb(0x342640));
                const bool deny = ImGui::Button("Deny", ImVec2(width, 30.0f));
                ImGui::PopStyleColor(3);

                int expected = 0;
                if (allow)
                    prompt.decision.compare_exchange_strong(expected, 1);
                else if (deny)
                    prompt.decision.compare_exchange_strong(expected, 2);
            }
            ImGui::End();
            ImGui::PopStyleColor(7);
            ImGui::PopStyleVar(5);

            if (!open)
            {
                int expected = 0;
                prompt.decision.compare_exchange_strong(expected, 2);
            }
        }
    }

    inline void Render()
    {
        if (!Any())
            return;

        if (g_promptCount.load(std::memory_order_relaxed) > 0)
        {
            std::vector<std::shared_ptr<PurchasePrompt>> prompts;
            {
                std::lock_guard<std::mutex> guard(g_promptLock);
                prompts = g_prompts;
            }
            int index = 0;
            for (auto& prompt : prompts)
                detail::render_prompt(*prompt, index++);
        }

        std::vector<Event> pending;
        {
            std::lock_guard<std::mutex> guard(g_lock);
            int index = 0;
            for (auto& window : g_windows)
                detail::render_window(*window, index++, pending);
        }

        if (pending.empty())
            return;

        {
            std::lock_guard<std::mutex> guard(g_eventLock);
            for (Event& event : pending)
                detail::queue(g_events, std::move(event));
        }
        if (g_wake)
            g_wake();
    }
}
