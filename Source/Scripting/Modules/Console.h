#pragma once

#include <lstate.h>
#include <lualib.h>

#include <Core/Context.hpp>
#include <Runtime/Execution/ScriptExecution.hpp>
#include <Scripting/Environment.hpp>
#include <Runtime/Scheduler/TaskScheduler.hpp>
#include <Scripting/Coroutines/Yielding.hpp>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>
#include <thread>
#include <Windows.h>
#include <wincrypt.h>
#include <intrin.h>

#include <Core/Utilities.hpp>
#include <ltable.h>

#define RESET    "\033[0m"
#define BRACKET  "\033[38;5;239m"
#define TIME     "\033[38;5;242m"
#define GRAY     "\033[38;5;242m"
#define CON_TEXT "\033[37m"
#define BLUE     "\033[94m"
#define ORANGE   "\033[38;5;214m"
#define RED      "\033[91m"

namespace Console
{
    inline bool ConsoleExists = false;
    inline HANDLE OriginalConsoleHandle = INVALID_HANDLE_VALUE;

    void EnableAnsi()
    {
        HANDLE OutputHandle = GetStdHandle(STD_OUTPUT_HANDLE);
        if (OutputHandle == INVALID_HANDLE_VALUE)
            return;
        DWORD Mode = 0;
        if (GetConsoleMode(OutputHandle, &Mode))
            SetConsoleMode(OutputHandle, Mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        SetConsoleCP(65001);
        SetConsoleOutputCP(65001);
    }

    void BypassConsole(bool Visible)
    {
        __try
        {
            PVOID Peb = (PVOID)__readgsqword(0x60);
            if (!Peb)
                return;

            PVOID ProcessParams = *(PVOID*)((BYTE*)Peb + 0x20);
            if (!ProcessParams)
                return;

            HANDLE* ConsoleHandle = (HANDLE*)((BYTE*)ProcessParams + 0x10);

            if (!Visible)
            {
                if (OriginalConsoleHandle == INVALID_HANDLE_VALUE)
                    OriginalConsoleHandle = *ConsoleHandle;
                *ConsoleHandle = NULL;
            }
            else
            {
                if (OriginalConsoleHandle != INVALID_HANDLE_VALUE)
                    *ConsoleHandle = OriginalConsoleHandle;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    std::string ReadConsoleInput()
    {
        HANDLE InputHandle = GetStdHandle(STD_INPUT_HANDLE);
        if (InputHandle == INVALID_HANDLE_VALUE)
            return "";

        char Buffer[256] = {};
        DWORD BytesRead = 0;

        BypassConsole(true);
        std::thread([] { Sleep(100); BypassConsole(false); }).detach();

        if (!ReadConsoleA(InputHandle, Buffer, sizeof(Buffer) - 1, &BytesRead, NULL))
            return "";

        while (BytesRead > 0 && (Buffer[BytesRead - 1] == '\r' || Buffer[BytesRead - 1] == '\n'))
            Buffer[--BytesRead] = '\0';

        return std::string(Buffer, BytesRead);
    }

    void ClearConsoleScreen()
    {
        BypassConsole(true);

        HANDLE OutputHandle = GetStdHandle(STD_OUTPUT_HANDLE);
        if (OutputHandle == INVALID_HANDLE_VALUE)
        {
            BypassConsole(false);
            return;
        }

        CONSOLE_SCREEN_BUFFER_INFO ScreenInfo;
        DWORD Written;
        COORD Home = { 0, 0 };

        if (GetConsoleScreenBufferInfo(OutputHandle, &ScreenInfo))
        {
            DWORD Size = ScreenInfo.dwSize.X * ScreenInfo.dwSize.Y;
            FillConsoleOutputCharacterA(OutputHandle, ' ', Size, Home, &Written);
            FillConsoleOutputAttribute(OutputHandle, ScreenInfo.wAttributes, Size, Home, &Written);
            SetConsoleCursorPosition(OutputHandle, Home);
        }

        BypassConsole(false);
    }

    std::string GetTimestamp()
    {
        SYSTEMTIME Time;
        GetLocalTime(&Time);

        char Buffer[16];
        sprintf_s(Buffer, "%02d:%02d:%02d", Time.wHour, Time.wMinute, Time.wSecond);

        return Buffer;
    }

    void PrintFormatted(const char* TypeColor, const char* Type, const char* Text)
    {
        if (!ConsoleExists)
            return;

        SYSTEMTIME Time;
        GetLocalTime(&Time);
        char Timestamp[16];
        sprintf_s(Timestamp, "%02d:%02d:%02d", Time.wHour, Time.wMinute, Time.wSecond);

        __try
        {
            printf(
                BRACKET "[" TIME "%s" BRACKET "] "
                BRACKET "[" "%s%s" BRACKET "] "
                CON_TEXT "%s" RESET "\n",
                Timestamp,
                TypeColor, Type,
                Text
            );
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    int RConsoleCreate(lua_State* L)
    {
        if (ConsoleExists)
        {
            BypassConsole(true);
            ShowWindow(GetConsoleWindow(), SW_SHOW);
            BypassConsole(false);
            return 0;
        }

        if (!AllocConsole())
        {
            Roblox::Print(3, ("Error Creating Console"));
            return 0;
        }

        FILE* File = nullptr;
        freopen_s(&File, "CONOUT$", "w", stdout);
        freopen_s(&File, "CONOUT$", "w", stderr);
        freopen_s(&File, "CONIN$", "r", stdin);

        SetConsoleTitleW((L"Roblox"));
        EnableAnsi();
        ConsoleExists = true;
        BypassConsole(false);
        return 0;
    }

    int RConsoleClear(lua_State* L)
    {
        if (!ConsoleExists)
            return 0;
        ClearConsoleScreen();
        return 0;
    }

    int RConsoleInput(lua_State* L)
    {
        if (!ConsoleExists)
        {
            lua_pushliteral(L, "");
            return 1;
        }
        std::string Result = ReadConsoleInput();
        lua_pushlstring(L, Result.data(), Result.size());
        return 1;
    }

    int RConsoleDestroy(lua_State* L)
    {
        if (ConsoleExists)
        {
            ClearConsoleScreen();
            FreeConsole();
            OriginalConsoleHandle = INVALID_HANDLE_VALUE;
            ConsoleExists = false;
        }
        return 0;
    }

    int RConsoleSetTitle(lua_State* L)
    {
        if (lua_type(L, 1) != LUA_TSTRING)
            return 0;

        if (!ConsoleExists)
            return 0;

        std::string Title = lua_tostring(L, 1);
        std::wstring WideTitle(Title.begin(), Title.end());

        BypassConsole(true);
        SetConsoleTitleW(WideTitle.c_str());
        BypassConsole(false);

        return 0;
    }

    int RConsoleTopmost(lua_State* L)
    {
        if (ConsoleExists)
        {
            BypassConsole(true);
            SetWindowPos(GetConsoleWindow(), HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            BypassConsole(false);
        }
        return 0;
    }

    int RConsolePrint(lua_State* L)
    {
        if (lua_type(L, 1) != LUA_TSTRING)
            return 0;

        if (!ConsoleExists)
            return 0;

        const char* Text = luaL_checklstring(L, 1, nullptr);
        if (!Text) Text = "";

        BypassConsole(true);
        PrintFormatted(GRAY, ("Print"), Text);
        BypassConsole(false);

        return 0;
    }

    int RConsoleInfo(lua_State* L)
    {
        if (lua_type(L, 1) != LUA_TSTRING)
            return 0;

        if (!ConsoleExists)
            return 0;

        const char* Text = luaL_checklstring(L, 1, nullptr);
        if (!Text) Text = "";

        BypassConsole(true);
        PrintFormatted(BLUE, ("Info"), Text);
        BypassConsole(false);

        return 0;
    }

    int RConsoleWarn(lua_State* L)
    {
        if (lua_type(L, 1) != LUA_TSTRING)
            return 0;

        if (!ConsoleExists)
            return 0;

        const char* Text = luaL_checklstring(L, 1, nullptr);
        if (!Text) Text = "";

        BypassConsole(true);
        PrintFormatted(ORANGE, ("Warning"), Text);
        BypassConsole(false);

        return 0;
    }

    int RConsoleErr(lua_State* L)
    {
        if (lua_type(L, 1) != LUA_TSTRING)
            return 0;

        if (!ConsoleExists)
            return 0;

        const char* Text = luaL_checklstring(L, 1, nullptr);
        if (!Text) Text = "";

        BypassConsole(true);
        PrintFormatted(RED, ("Error"), Text);
        BypassConsole(false);
        return 0;
    }

    int RConsoleHide(lua_State* L)
    {
        BypassConsole(true);
        ShowWindow(GetConsoleWindow(), SW_HIDE);
        BypassConsole(false);
        return 0;
    }

    int RConsoleShow(lua_State* L)
    {
        BypassConsole(true);
        ShowWindow(GetConsoleWindow(), SW_SHOW);
        BypassConsole(false);
        return 0;
    }

    int RConsoleToggle(lua_State* L)
    {
        BypassConsole(true);

        HWND Window = GetConsoleWindow();
        if (Window)
            ShowWindow(Window, IsWindowVisible(Window) ? SW_HIDE : SW_SHOW);

        BypassConsole(false);
        return 0;
    }

    int RConsoleHidden(lua_State* L)
    {
        BypassConsole(true);

        HWND Window = GetConsoleWindow();
        lua_pushboolean(L, Window && !IsWindowVisible(Window));

        BypassConsole(false);
        return 1;
    }

    int RedirectedOutput(lua_State* L)
    {
        const int Count = lua_gettop(L);
        std::string Message;
        for (int Index = 1; Index <= Count; ++Index)
        {
            size_t Length = 0;
            const char* Text = luaL_tolstring(L, Index, &Length);
            if (Index > 1)
                Message.push_back(' ');
            Message.append(Text ? Text : "", Length);
            lua_pop(L, 1);
        }

        lua_pushvalue(L, lua_upvalueindex(1));
        lua_pushvalue(L, lua_upvalueindex(2));
        lua_pushlstring(L, Message.data(), Message.size());
        lua_call(L, 2, 0);
        return 0;
    }

    int MakeOutputRedirect(lua_State* L)
    {
        luaL_checktype(L, 1, LUA_TFUNCTION);
        const int Kind = luaL_checkinteger(L, 2);
        lua_settop(L, 2);
        lua_pushcclosurek(L, RedirectedOutput, Kind == 2 ? "warn" : "print", 2, nullptr);
        return 1;
    }

    inline void RegisterLibrary(lua_State* L)
    {
        Utils::AddFunction(L, ("__supq_output_redirect"), MakeOutputRedirect);
        Utils::AddFunction(L, ("rconsolecreate"), RConsoleCreate);
        Utils::AddFunction(L, ("consolecreate"), RConsoleCreate);
        Utils::AddFunction(L, ("rconsoleclear"), RConsoleClear);
        Utils::AddFunction(L, ("consoleclear"), RConsoleClear);
        Utils::AddFunction(L, ("rconsoleinput"), RConsoleInput);
        Utils::AddFunction(L, ("consoleinput"), RConsoleInput);
        Utils::AddFunction(L, ("rconsoleprint"), RConsolePrint);
        Utils::AddFunction(L, ("consoleprint"), RConsolePrint);
        Utils::AddFunction(L, ("rconsoledestroy"), RConsoleDestroy);
        Utils::AddFunction(L, ("consoledestroy"), RConsoleDestroy);
        Utils::AddFunction(L, ("rconsolesettitle"), RConsoleSetTitle);
        Utils::AddFunction(L, ("consolesettitle"), RConsoleSetTitle);
        Utils::AddFunction(L, ("rconsolename"), RConsoleSetTitle);
        Utils::AddFunction(L, ("consolename"), RConsoleSetTitle);
        Utils::AddFunction(L, ("rconsoletopmost"), RConsoleTopmost);
        Utils::AddFunction(L, ("consoletopmost"), RConsoleTopmost);
        Utils::AddFunction(L, ("rconsoletop"), RConsoleTopmost);
        Utils::AddFunction(L, ("consoletop"), RConsoleTopmost);
        Utils::AddFunction(L, ("rconsoleinfo"), RConsoleInfo);
        Utils::AddFunction(L, ("consoleinfo"), RConsoleInfo);
        Utils::AddFunction(L, ("rconsolewarn"), RConsoleWarn);
        Utils::AddFunction(L, ("consolewarn"), RConsoleWarn);
        Utils::AddFunction(L, ("rconsoleerr"), RConsoleErr);
        Utils::AddFunction(L, ("rconsoleerror"), RConsoleErr);
        Utils::AddFunction(L, ("consoleerr"), RConsoleErr);
        Utils::AddFunction(L, ("consoleerror"), RConsoleErr);
        Utils::AddFunction(L, ("rconsolehide"), RConsoleHide);
        Utils::AddFunction(L, ("consolehide"), RConsoleHide);
        Utils::AddFunction(L, ("rconsoleshow"), RConsoleShow);
        Utils::AddFunction(L, ("consoleshow"), RConsoleShow);
        Utils::AddFunction(L, ("rconsoletoggle"), RConsoleToggle);
        Utils::AddFunction(L, ("consoletoggle"), RConsoleToggle);
        Utils::AddFunction(L, ("rconsolehidden"), RConsoleHidden);
        Utils::AddFunction(L, ("consolehidden"), RConsoleHidden);
    }
}
