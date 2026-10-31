#pragma once

#include <lstate.h>
#include <lualib.h>

#include <atomic>
#include <cstring>
#include <string>

#include <Core/Context.hpp>
#include <Core/Utilities.hpp>
#include <Ui/Overlay/Renderer.hpp>
#include <Ui/Overlay/WindowOverlay.hpp>

namespace ImGuiLib
{
    using Renderer::Gui::Element;
    using Renderer::Gui::ElementKind;

    inline const char* const MetatableName = "ImGuiWindow";
    inline char CallbacksKey;
    inline char MethodsKey;

    inline std::atomic<bool> g_drainQueued{ false };
    inline std::atomic<uint64_t> g_generation{ 0 };

    struct Ref
    {
        int id;
    };

    inline Ref* to_ref(lua_State* L, int index)
    {
        if (lua_type(L, index) == LUA_TUSERDATA && lua_getmetatable(L, index))
        {
            luaL_getmetatable(L, MetatableName);
            const bool ours = lua_rawequal(L, -1, -2) != 0;
            lua_pop(L, 2);
            if (ours)
                return static_cast<Ref*>(lua_touserdata(L, index));
        }
        luaL_typeerrorL(L, index, MetatableName);
        return nullptr;
    }

    inline int check_window(lua_State* L)
    {
        Ref* ref = to_ref(L, 1);
        if (!ref->id)
            luaL_error(L, "ImGuiWindow has been destroyed");
        return ref->id;
    }

    inline void push_callbacks(lua_State* L, int windowId, bool create)
    {
        lua_pushlightuserdata(L, &CallbacksKey);
        lua_rawget(L, LUA_REGISTRYINDEX);
        lua_rawgeti(L, -1, windowId);
        if (lua_isnil(L, -1) && create)
        {
            lua_pop(L, 1);
            lua_newtable(L);
            lua_pushvalue(L, -1);
            lua_rawseti(L, -3, windowId);
        }
        lua_remove(L, -2);
    }

    inline void store_callback(lua_State* L, int windowId, int elementId, int callback)
    {
        if (!elementId || !lua_isfunction(L, callback))
            return;
        callback = lua_absindex(L, callback);
        push_callbacks(L, windowId, true);
        lua_pushvalue(L, callback);
        lua_rawseti(L, -2, elementId);
        lua_pop(L, 1);
    }

    inline void append(lua_State* L, int windowId, Element element, int callback = 0)
    {
        const int elementId = Renderer::Gui::Append(windowId, std::move(element));
        if (!elementId)
            luaL_error(L, "ImGuiWindow has been destroyed");
        if (callback)
            store_callback(L, windowId, elementId, callback);
        lua_settop(L, 1);
    }

    inline void copy_text(char (&target)[256], const char* text, size_t length)
    {
        const size_t count = length < sizeof(target) - 1 ? length : sizeof(target) - 1;
        std::memcpy(target, text, count);
        target[count] = '\0';
    }

    inline void deliver()
    {
        g_drainQueued.store(false);
        lua_State* L = SharedVariables::ExploitThread;
        if (!L || g_generation.load() != SharedVariables::StateGeneration.load())
            return;

        const std::vector<Renderer::Gui::Event> events = Renderer::Gui::TakeEvents();
        if (events.empty() || !lua_checkstack(L, 4))
            return;

        const int top = lua_gettop(L);
        for (const Renderer::Gui::Event& event : events)
        {
            push_callbacks(L, event.window, false);
            if (!lua_istable(L, -1))
            {
                lua_settop(L, top);
                continue;
            }
            lua_rawgeti(L, -1, event.element);
            if (!lua_isfunction(L, -1))
            {
                lua_settop(L, top);
                continue;
            }
            const int callback = lua_gettop(L);

            lua_State* T = lua_newthread(L);
            if (!T || !lua_checkstack(T, 8))
            {
                lua_settop(L, top);
                continue;
            }
            lua_getglobal(T, "task");
            lua_getfield(T, -1, "defer");
            lua_remove(T, -2);
            if (!lua_isfunction(T, -1))
            {
                lua_settop(L, top);
                continue;
            }

            lua_pushvalue(L, callback);
            lua_xmove(L, T, 1);

            int argc = 0;
            switch (event.kind)
            {
            case ElementKind::Checkbox:
                lua_pushboolean(T, event.checked);
                argc = 1;
                break;
            case ElementKind::SliderFloat:
            case ElementKind::SliderInt:
                lua_pushnumber(T, event.number);
                argc = 1;
                break;
            case ElementKind::InputText:
                lua_pushlstring(T, event.text.data(), event.text.size());
                argc = 1;
                break;
            case ElementKind::ColorPicker:
                for (float component : event.color)
                    lua_pushnumber(T, component);
                argc = 4;
                break;
            default:
                break;
            }

            if (lua_pcall(T, 1 + argc, 0, 0) != LUA_OK)
                lua_pop(T, 1);
            lua_settop(L, top);
        }
    }

    inline void wake()
    {
        if (g_drainQueued.exchange(true))
            return;
        std::lock_guard<std::mutex> lock(SharedVariables::YieldQueueMutex);
        SharedVariables::YieldQueue.emplace(deliver);
    }

    inline int Text(lua_State* L)
    {
        const int id = check_window(L);
        Element element;
        element.kind = ElementKind::Text;
        element.label = luaL_checkstring(L, 2);
        append(L, id, std::move(element));
        return 1;
    }

    inline int Button(lua_State* L)
    {
        const int id = check_window(L);
        Element element;
        element.kind = ElementKind::Button;
        element.label = luaL_checkstring(L, 2);
        luaL_checktype(L, 3, LUA_TFUNCTION);
        append(L, id, std::move(element), 3);
        return 1;
    }

    inline int Checkbox(lua_State* L)
    {
        const int id = check_window(L);
        Element element;
        element.kind = ElementKind::Checkbox;
        element.label = luaL_checkstring(L, 2);
        element.checked = lua_toboolean(L, 3) != 0;
        if (!lua_isnoneornil(L, 4))
            luaL_checktype(L, 4, LUA_TFUNCTION);
        append(L, id, std::move(element), 4);
        return 1;
    }

    inline int SliderFloat(lua_State* L)
    {
        const int id = check_window(L);
        Element element;
        element.kind = ElementKind::SliderFloat;
        element.label = luaL_checkstring(L, 2);
        element.number = static_cast<float>(luaL_checknumber(L, 3));
        element.minimum = static_cast<float>(luaL_checknumber(L, 4));
        element.maximum = static_cast<float>(luaL_checknumber(L, 5));
        if (!lua_isnoneornil(L, 6))
            luaL_checktype(L, 6, LUA_TFUNCTION);
        append(L, id, std::move(element), 6);
        return 1;
    }

    inline int SliderInt(lua_State* L)
    {
        const int id = check_window(L);
        Element element;
        element.kind = ElementKind::SliderInt;
        element.label = luaL_checkstring(L, 2);
        element.integer = static_cast<int>(luaL_checkinteger(L, 3));
        element.intMinimum = static_cast<int>(luaL_checkinteger(L, 4));
        element.intMaximum = static_cast<int>(luaL_checkinteger(L, 5));
        if (!lua_isnoneornil(L, 6))
            luaL_checktype(L, 6, LUA_TFUNCTION);
        append(L, id, std::move(element), 6);
        return 1;
    }

    inline int InputText(lua_State* L)
    {
        const int id = check_window(L);
        Element element;
        element.kind = ElementKind::InputText;
        element.label = luaL_checkstring(L, 2);
        size_t length = 0;
        const char* text = luaL_optlstring(L, 3, "", &length);
        copy_text(element.text, text, length);
        if (!lua_isnoneornil(L, 4))
            luaL_checktype(L, 4, LUA_TFUNCTION);
        append(L, id, std::move(element), 4);
        return 1;
    }

    inline int ColorPicker(lua_State* L)
    {
        const int id = check_window(L);
        Element element;
        element.kind = ElementKind::ColorPicker;
        element.label = luaL_checkstring(L, 2);
        for (int i = 0; i < 4; ++i)
            element.color[i] = static_cast<float>(luaL_optnumber(L, 3 + i, 1.0));
        if (!lua_isnoneornil(L, 7))
            luaL_checktype(L, 7, LUA_TFUNCTION);
        append(L, id, std::move(element), 7);
        return 1;
    }

    inline int Separator(lua_State* L)
    {
        const int id = check_window(L);
        Element element;
        element.kind = ElementKind::Separator;
        append(L, id, std::move(element));
        return 1;
    }

    inline int SameLine(lua_State* L)
    {
        const int id = check_window(L);
        Element element;
        element.kind = ElementKind::SameLine;
        append(L, id, std::move(element));
        return 1;
    }

    inline int CollapsingHeader(lua_State* L)
    {
        const int id = check_window(L);
        Element begin;
        begin.kind = ElementKind::HeaderBegin;
        begin.label = luaL_checkstring(L, 2);
        luaL_checktype(L, 3, LUA_TFUNCTION);
        if (!Renderer::Gui::Append(id, std::move(begin)))
            luaL_error(L, "ImGuiWindow has been destroyed");

        lua_pushvalue(L, 3);
        lua_pushvalue(L, 1);
        if (lua_pcall(L, 1, 0, 0) != LUA_OK)
            lua_pop(L, 1);

        Element end;
        end.kind = ElementKind::HeaderEnd;
        Renderer::Gui::Append(id, std::move(end));
        lua_settop(L, 1);
        return 1;
    }

    inline int destroy(lua_State* L)
    {
        Ref* ref = to_ref(L, 1);
        if (!ref->id)
            return 0;

        Renderer::Gui::Destroy(ref->id);
        lua_pushlightuserdata(L, &CallbacksKey);
        lua_rawget(L, LUA_REGISTRYINDEX);
        lua_pushnil(L);
        lua_rawseti(L, -2, ref->id);
        lua_pop(L, 1);
        ref->id = 0;
        return 0;
    }

    inline int index(lua_State* L)
    {
        to_ref(L, 1);
        const char* key = luaL_checkstring(L, 2);
        lua_pushlightuserdata(L, &MethodsKey);
        lua_rawget(L, LUA_REGISTRYINDEX);
        lua_getfield(L, -1, key);
        if (lua_isnil(L, -1))
            luaL_error(L, "%s is not a valid member of ImGuiWindow", key);
        return 1;
    }

    inline int tostring(lua_State* L)
    {
        Ref* ref = to_ref(L, 1);
        lua_pushfstring(L, "ImGuiWindow(%d)", ref->id);
        return 1;
    }

    inline int create(lua_State* L)
    {
        size_t length = 0;
        const char* title = luaL_optlstring(L, 1, "ImGui", &length);
        if (length == 0)
            luaL_argerror(L, 1, "title must not be empty");

        const int id = Renderer::Gui::Create(std::string(title, length));
        if (!id)
            luaL_error(L, "an ImGui window titled '%s' already exists", title);

        Renderer::Initialize();
        WindowOverlay::Start();

        auto* ref = static_cast<Ref*>(lua_newuserdata(L, sizeof(Ref)));
        ref->id = id;
        luaL_getmetatable(L, MetatableName);
        lua_setmetatable(L, -2);
        return 1;
    }

    inline int setvsync(lua_State* L)
    {
        luaL_checktype(L, 1, LUA_TBOOLEAN);
        Renderer::Gui::g_vsync.store(lua_toboolean(L, 1) != 0);
        return 0;
    }

    inline void RegisterLibrary(lua_State* L)
    {
        Renderer::Gui::Reset();
        Renderer::Gui::g_vsync.store(true);
        Renderer::Gui::g_wake = wake;
        g_drainQueued.store(false);
        g_generation.store(SharedVariables::StateGeneration.load());

        lua_pushlightuserdata(L, &CallbacksKey);
        lua_newtable(L);
        lua_rawset(L, LUA_REGISTRYINDEX);

        lua_pushlightuserdata(L, &MethodsKey);
        lua_newtable(L);
        Utils::AddTableFunction(L, "Text", Text);
        Utils::AddTableFunction(L, "Button", Button);
        Utils::AddTableFunction(L, "Checkbox", Checkbox);
        Utils::AddTableFunction(L, "SliderFloat", SliderFloat);
        Utils::AddTableFunction(L, "SliderInt", SliderInt);
        Utils::AddTableFunction(L, "InputText", InputText);
        Utils::AddTableFunction(L, "ColorPicker", ColorPicker);
        Utils::AddTableFunction(L, "Separator", Separator);
        Utils::AddTableFunction(L, "SameLine", SameLine);
        Utils::AddTableFunction(L, "CollapsingHeader", CollapsingHeader);
        Utils::AddTableFunction(L, "destroy", destroy);
        Utils::AddTableFunction(L, "Destroy", destroy);
        lua_rawset(L, LUA_REGISTRYINDEX);

        luaL_newmetatable(L, MetatableName);
        lua_pushcfunction(L, index, "__index");
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, tostring, "__tostring");
        lua_setfield(L, -2, "__tostring");
        lua_pushstring(L, MetatableName);
        lua_setfield(L, -2, "__type");
        lua_pushstring(L, "The metatable is locked");
        lua_setfield(L, -2, "__metatable");
        lua_pop(L, 1);

        lua_newtable(L);
        Utils::AddTableFunction(L, "create", create);
        Utils::AddTableFunction(L, "setvsync", setvsync);
        lua_setreadonly(L, -1, true);
        lua_setglobal(L, "imgui");
    }
}
