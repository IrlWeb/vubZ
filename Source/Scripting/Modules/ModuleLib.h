#pragma once

#include <Core/Utilities.hpp>
#include <Core/Context.hpp>
#include <Ui/Overlay/Renderer.hpp>

namespace supQLib
{
    int getinstancesbyclass(lua_State* L)
    {
        const char* className = luaL_checkstring(L, 1);

        lua_newtable(L);
        int result = lua_gettop(L);
        int index = 1;

        lua_getglobal(L, "game");

        if (!lua_isuserdata(L, -1))
        {
            lua_pop(L, 1);
            return 1;
        }

        int game = lua_gettop(L);

        lua_getfield(L, game, "GetDescendants");

        if (!lua_isfunction(L, -1))
        {
            lua_pop(L, 2);
            return 1;
        }

        lua_pushvalue(L, game);

        if (lua_pcall(L, 1, 1, 0) != LUA_OK)
        {
            lua_pop(L, 1);
            return 1;
        }

        if (!lua_istable(L, -1))
        {
            lua_pop(L, 1);
            return 1;
        }

        int descendants = lua_gettop(L);
        int count = lua_objlen(L, descendants);

        for (int i = 1; i <= count; i++)
        {
            lua_rawgeti(L, descendants, i);

            if (lua_isuserdata(L, -1))
            {
                lua_getfield(L, -1, "ClassName");

                if (lua_isstring(L, -1))
                {
                    const char* current = lua_tostring(L, -1);

                    if (strcmp(current, className) == 0)
                    {
                        lua_pushvalue(L, -2);
                        lua_rawseti(L, result, index++);
                    }
                }

                lua_pop(L, 1);
            }

            lua_pop(L, 1);
        }

        lua_pop(L, 2);

        return 1;
    }

    int gethwid(lua_State* L)
    {
        std::string hwid = Http::gethwid();
        lua_pushlstring(L, hwid.data(), hwid.size());
        return 1;
    }

    int createmodule(lua_State* L) {
        lua_getglobal(L, "game");

        lua_getfield(L, -1, "CoreGui");
        lua_getfield(L, -1, "RobloxGui");
        lua_getfield(L, -1, "Modules");
        lua_getfield(L, -1, "Common");
        lua_getfield(L, -1, "CommonUtil");

        lua_getfield(L, -1, "Clone");
        lua_pushvalue(L, -2);
        if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
            const char* err = lua_tostring(L, -1);
            luaL_error(L, "Failed To Clone");
            lua_pop(L, 1);
            lua_pushnil(L);
            return 1;
        }

        lua_pushstring(L, "Name");
        lua_pushstring(L, "Constants");
        lua_settable(L, -3);

        lua_pushstring(L, "Parent");
        lua_getfield(L, -5, "Common");
        lua_settable(L, -3);

        return 1;
    }

    int create_toast(lua_State* L)
    {
        ImGuiToastType type = ImGuiToastType::Info;
        std::string title = "supQ";
        std::string content = "";
        int duration = 3000;

        if (lua_istable(L, 1))
        {
            lua_getfield(L, 1, "title");
            if (!lua_isstring(L, -1)) { lua_pop(L, 1); lua_getfield(L, 1, "Title"); }
            if (lua_isstring(L, -1)) title = lua_tostring(L, -1);
            lua_pop(L, 1);

            lua_getfield(L, 1, "content");
            if (!lua_isstring(L, -1)) { lua_pop(L, 1); lua_getfield(L, 1, "Content"); }
            if (!lua_isstring(L, -1)) { lua_pop(L, 1); lua_getfield(L, 1, "text"); }
            if (!lua_isstring(L, -1)) { lua_pop(L, 1); lua_getfield(L, 1, "Text"); }
            if (!lua_isstring(L, -1)) { lua_pop(L, 1); lua_getfield(L, 1, "message"); }
            if (!lua_isstring(L, -1)) { lua_pop(L, 1); lua_getfield(L, 1, "Message"); }
            if (lua_isstring(L, -1)) content = lua_tostring(L, -1);
            lua_pop(L, 1);

            lua_getfield(L, 1, "duration");
            if (!lua_isnumber(L, -1)) { lua_pop(L, 1); lua_getfield(L, 1, "Duration"); }
            if (lua_isnumber(L, -1)) {
                double d = lua_tonumber(L, -1);
                duration = d < 50 ? static_cast<int>(d * 1000) : static_cast<int>(d);
            }
            lua_pop(L, 1);

            lua_getfield(L, 1, "type");
            if (!lua_isstring(L, -1)) { lua_pop(L, 1); lua_getfield(L, 1, "Type"); }
            if (lua_isstring(L, -1)) {
                const char* t = lua_tostring(L, -1);
                if (_stricmp(t, "success") == 0) type = ImGuiToastType::Success;
                else if (_stricmp(t, "warning") == 0) type = ImGuiToastType::Warning;
                else if (_stricmp(t, "error") == 0) type = ImGuiToastType::Error;
                else if (_stricmp(t, "info") == 0) type = ImGuiToastType::Info;
            }
            lua_pop(L, 1);
        }
        else
        {
            int top = lua_gettop(L);
            if (top >= 1 && lua_isstring(L, 1))
                title = lua_tostring(L, 1);
            if (top >= 2 && lua_isstring(L, 2))
                content = lua_tostring(L, 2);
            if (top >= 3)
            {
                if (lua_isnumber(L, 3))
                {
                    double d = lua_tonumber(L, 3);
                    duration = d < 50 ? static_cast<int>(d * 1000) : static_cast<int>(d);
                }
                else if (lua_isstring(L, 3))
                {
                    const char* t = lua_tostring(L, 3);
                    if (_stricmp(t, "success") == 0) type = ImGuiToastType::Success;
                    else if (_stricmp(t, "warning") == 0) type = ImGuiToastType::Warning;
                    else if (_stricmp(t, "error") == 0) type = ImGuiToastType::Error;
                    else if (_stricmp(t, "info") == 0) type = ImGuiToastType::Info;
                }
            }
            if (top >= 4)
            {
                if (lua_isnumber(L, 4))
                {
                    double d = lua_tonumber(L, 4);
                    duration = d < 50 ? static_cast<int>(d * 1000) : static_cast<int>(d);
                }
                else if (lua_isstring(L, 4))
                {
                    const char* t = lua_tostring(L, 4);
                    if (_stricmp(t, "success") == 0) type = ImGuiToastType::Success;
                    else if (_stricmp(t, "warning") == 0) type = ImGuiToastType::Warning;
                    else if (_stricmp(t, "error") == 0) type = ImGuiToastType::Error;
                    else if (_stricmp(t, "info") == 0) type = ImGuiToastType::Info;
                }
            }
        }

        ImGuiToast toast(type, duration);
        toast.setTitle("%s", title.c_str());
        toast.setContent("%s", content.c_str());
        Renderer::PushToast(toast);

        return 0;
    }

    void RegisterLibrary(lua_State* L)
    {
        Utils::AddFunction(L, "getinstancesbyclass", supQLib::getinstancesbyclass);
        Utils::AddFunction(L, "gethwid", supQLib::gethwid);
        Utils::AddFunction(L, "createmodule", supQLib::createmodule);
        Utils::AddFunction(L, "create_toast", supQLib::create_toast);
        Utils::AddFunction(L, "toast_notification", supQLib::create_toast);

        lua_createtable(L, 0, 5);
        Utils::AddTableFunction(L, "getinstancesbyclass", supQLib::getinstancesbyclass);
        Utils::AddTableFunction(L, "gethwid", supQLib::gethwid);
        Utils::AddTableFunction(L, "createmodule", supQLib::createmodule);
        Utils::AddTableFunction(L, "create_toast", supQLib::create_toast);
        Utils::AddTableFunction(L, "toast_notification", supQLib::create_toast);
        lua_pushvalue(L, -1);
        lua_setglobal(L, "supQ");
        lua_setglobal(L, "supq");
    }
}

