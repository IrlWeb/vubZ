#pragma once

#include <lstate.h>
#include <string>
#include <atomic>
#include <mutex>
#include <thread>
#include <Windows.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

#include <Scripting/Environment.hpp>
#include <Core/Context.hpp>
#include <Core/Utilities.hpp>
#include <Scripting/Modules/Signal.hpp>

namespace Websocket
{
    struct SocketState
    {
        lua_State* LuaState = nullptr;
        HINTERNET Session = nullptr;
        HINTERNET Connection = nullptr;
        HINTERNET WebSocket = nullptr;
        int MessageEventRef = LUA_NOREF;
        int CloseEventRef = LUA_NOREF;
        uint64_t Generation = 0;
        std::atomic<bool> Closing{ false };
        std::atomic<bool> Closed{ false };
        std::atomic<bool> CloseEventQueued{ false };
        std::mutex SendMutex;
    };

    inline void QueueEvent(SocketState* socket, int eventRef,
        std::string payload, bool hasPayload, bool binary = false)
    {
        if (!socket || eventRef == LUA_NOREF)
            return;

        std::lock_guard<std::mutex> lock(SharedVariables::YieldQueueMutex);
        SharedVariables::YieldQueue.emplace(
            [socket, eventRef, payload = std::move(payload), hasPayload, binary]()
            {
                if (!socket->LuaState ||
                    socket->Generation != SharedVariables::StateGeneration.load())
                    return;

                lua_State* L = socket->LuaState;
                const int top = lua_gettop(L);
                lua_getref(L, eventRef);
                if (lua_isuserdata(L, -1))
                {
                    const int signal = lua_gettop(L);
                    if (hasPayload)
                    {
                        lua_pushlstring(L, payload.data(), payload.size());
                        lua_pushboolean(L, binary);
                        LuaSignal::FireFromC(L, signal, 2);
                    }
                    else
                    {
                        LuaSignal::FireFromC(L, signal, 0);
                    }
                }
                lua_settop(L, top);
            });
    }

    inline void QueueCloseEvent(SocketState* socket)
    {
        if (socket &&
            !socket->CloseEventQueued.exchange(true, std::memory_order_acq_rel))
            QueueEvent(socket, socket->CloseEventRef, {}, false);
    }

    inline bool SendFrame(SocketState* socket, const std::string& payload,
        bool binary)
    {
        if (!socket->WebSocket)
            return false;

        const WINHTTP_WEB_SOCKET_BUFFER_TYPE type = binary
            ? WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE
            : WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE;
        return WinHttpWebSocketSend(
            socket->WebSocket,
            type,
            const_cast<char*>(payload.data()),
            static_cast<DWORD>(payload.size())) == NO_ERROR;
    }

    inline void RunSocket(SocketState* socket)
    {
        std::string incoming;
        while (!socket->Closing.load(std::memory_order_acquire) &&
            socket->Generation == SharedVariables::StateGeneration.load())
        {
            char buffer[8192];
            DWORD received = 0;
            WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
            const DWORD result = WinHttpWebSocketReceive(
                socket->WebSocket, buffer, sizeof(buffer), &received, &type);

            if (result == ERROR_WINHTTP_TIMEOUT)
                continue;
            if (result != NO_ERROR)
                break;
            if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE)
                break;

            incoming.append(buffer, received);
            if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE ||
                type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE)
            {
                QueueEvent(socket, socket->MessageEventRef,
                    std::move(incoming), true,
                    type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE);
                incoming.clear();
            }
        }

        HINTERNET webSocket = nullptr;
        HINTERNET connection = nullptr;
        HINTERNET session = nullptr;
        {
            std::lock_guard<std::mutex> lock(socket->SendMutex);
            webSocket = socket->WebSocket;
            connection = socket->Connection;
            session = socket->Session;
            socket->WebSocket = nullptr;
            socket->Connection = nullptr;
            socket->Session = nullptr;
            socket->Closed.store(true, std::memory_order_release);
        }

        if (webSocket)
        {
            WinHttpWebSocketClose(
                webSocket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
            WinHttpCloseHandle(webSocket);
        }
        if (connection)
            WinHttpCloseHandle(connection);
        if (session)
            WinHttpCloseHandle(session);

        QueueCloseEvent(socket);
    }

    inline int Send(lua_State* L)
    {
        auto* socket = static_cast<SocketState*>(
            lua_touserdata(L, lua_upvalueindex(1)));
        const int msgIdx = lua_istable(L, 1) ? 2 : 1;
        size_t length = 0;
        const char* message = luaL_checklstring(L, msgIdx, &length);
        const bool binary = lua_toboolean(L, msgIdx + 1) != 0;
        if (!socket || socket->Closing.load() || socket->Closed.load())
        {
            luaL_error(L, "WebSocket is closed");
            return 0;
        }

        std::lock_guard<std::mutex> lock(socket->SendMutex);
        if (!SendFrame(socket, std::string(message, length), binary))
        {
            luaL_error(L, "WebSocket failed to send message");
            return 0;
        }
        return 0;
    }

    inline int Close(lua_State* L)
    {
        auto* socket = static_cast<SocketState*>(
            lua_touserdata(L, lua_upvalueindex(1)));
        if (socket)
        {
            const bool wasClosing = socket->Closing.exchange(
                true, std::memory_order_acq_rel);
            if (!wasClosing)
            {
                {
                    std::lock_guard<std::mutex> lock(socket->SendMutex);
                    if (socket->WebSocket)
                        WinHttpWebSocketShutdown(
                            socket->WebSocket,
                            WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS,
                            nullptr,
                            0);
                }
                QueueCloseEvent(socket);
            }
        }
        return 0;
    }

    inline int CreateSignal(lua_State* L)
    {
        LuaSignal::PushNew(L);
        return lua_ref(L, -1);
    }

    inline int SocketIndex(lua_State* L)
    {
        auto* socket = static_cast<SocketState*>(
            lua_touserdata(L, lua_upvalueindex(1)));
        const char* key = lua_tostring(L, 2);
        if (key && strcmp(key, "IsClosed") == 0)
        {
            lua_pushboolean(L, !socket || socket->Closing.load());
            return 1;
        }

        lua_pushnil(L);
        return 1;
    }

    inline std::wstring ToWide(const std::string& value)
    {
        if (value.empty())
            return {};
        const int length = MultiByteToWideChar(
            CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
            nullptr, 0);
        std::wstring result(length, L'\0');
        MultiByteToWideChar(
            CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
            result.data(), length);
        return result;
    }

    inline int Connect(lua_State* L)
    {
        size_t urlLength = 0;
        const char* url = luaL_checklstring(L, 1, &urlLength);
        const std::string address(url, urlLength);
        if (address.rfind("ws://", 0) != 0 &&
            address.rfind("wss://", 0) != 0)
        {
            luaL_error(L, "Invalid WebSocket URL");
            return 0;
        }
        if (address == "ws://" || address == "wss://")
        {
            luaL_error(L, "Invalid WebSocket URL: missing host");
            return 0;
        }

        std::string httpAddress = address;
        const size_t legacyEcho = httpAddress.find("echo.websocket.events");
        if (legacyEcho != std::string::npos)
        {
            httpAddress.replace(legacyEcho, sizeof("echo.websocket.events") - 1, "echo.websocket.org");
        }
        const bool secure = address.rfind("wss://", 0) == 0;
        httpAddress.replace(0, secure ? 6 : 5, secure ? "https://" : "http://");
        std::wstring wideAddress = ToWide(httpAddress);

        URL_COMPONENTS components{};
        components.dwStructSize = sizeof(components);
        components.dwSchemeLength = static_cast<DWORD>(-1);
        components.dwHostNameLength = static_cast<DWORD>(-1);
        components.dwUrlPathLength = static_cast<DWORD>(-1);
        components.dwExtraInfoLength = static_cast<DWORD>(-1);
        if (!WinHttpCrackUrl(
            wideAddress.c_str(), static_cast<DWORD>(wideAddress.size()),
            0, &components))
        {
            luaL_error(L, "Failed to parse WebSocket URL");
            return 0;
        }

        const std::wstring host(
            components.lpszHostName, components.dwHostNameLength);
        std::wstring path(
            components.lpszUrlPath, components.dwUrlPathLength);
        if (components.dwExtraInfoLength)
            path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
        if (path.empty())
            path = L"/";

        HINTERNET session = WinHttpOpen(
            L"Void External/1.0",
            WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0);
        if (!session)
        {
            luaL_error(L, "Failed to initialize WebSocket");
            return 0;
        }

        WinHttpSetTimeouts(session, 15000, 15000, 15000, 15000);
        HINTERNET connection = WinHttpConnect(
            session, host.c_str(), components.nPort, 0);
        HINTERNET request = connection
            ? WinHttpOpenRequest(
                connection, L"GET", path.c_str(), nullptr,
                WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                secure ? WINHTTP_FLAG_SECURE : 0)
            : nullptr;

        bool upgraded = request &&
            WinHttpSetOption(
                request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) &&
            WinHttpSendRequest(
                request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(request, nullptr);
        HINTERNET webSocket = upgraded
            ? WinHttpWebSocketCompleteUpgrade(request, 0)
            : nullptr;

        if (request)
            WinHttpCloseHandle(request);
        if (!webSocket)
        {
            if (connection)
                WinHttpCloseHandle(connection);
            WinHttpCloseHandle(session);
            luaL_error(L, "WebSocket connection failed");
            return 0;
        }

        auto* socket = new SocketState();
        socket->LuaState = SharedVariables::ExploitThread
            ? SharedVariables::ExploitThread
            : L;
        socket->Session = session;
        socket->Connection = connection;
        socket->WebSocket = webSocket;
        socket->Generation = SharedVariables::StateGeneration.load();

        socket->MessageEventRef = CreateSignal(L);
        lua_pop(L, 1);
        socket->CloseEventRef = CreateSignal(L);
        lua_pop(L, 1);

        lua_newtable(L);
        const int output = lua_gettop(L);

        lua_getref(L, socket->MessageEventRef);
        lua_setfield(L, output, "OnMessage");

        lua_getref(L, socket->CloseEventRef);
        lua_setfield(L, output, "OnClose");

        lua_createtable(L, 0, 1);
        lua_pushlightuserdata(L, socket);
        lua_pushcclosure(L, SocketIndex, "WebSocket.__index", 1);
        lua_setfield(L, -2, "__index");
        lua_setmetatable(L, output);

        lua_pushlightuserdata(L, socket);
        lua_pushcclosure(L, Send, "WebSocket.Send", 1);
        lua_setfield(L, output, "Send");
        lua_pushlightuserdata(L, socket);
        lua_pushcclosure(L, Close, "WebSocket.Close", 1);
        lua_setfield(L, output, "Close");

        std::thread(RunSocket, socket).detach();
        return 1;
    }

    inline void RegisterLibrary(lua_State* L)
    {
        lua_newtable(L);
        Utils::AddTableFunction(L, "connect", Connect);
        Utils::AddTableFunction(L, "Connect", Connect);
        lua_pushvalue(L, -1);
        lua_setglobal(L, "WebSocket");
        lua_setglobal(L, "websocket");
    }

    inline void Start(lua_State* L)
    {
        RegisterLibrary(L);
    }
}

inline void WebsocketLib(lua_State* L)
{
    Websocket::RegisterLibrary(L);
}
