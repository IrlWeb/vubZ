#pragma once

#include <lua.h>
#include <Dependencies\ixwebsocket\IXNetSystem.h>
#include <Dependencies\ixwebsocket\IXWebSocket.h>
#include <memory>
#include <unordered_map>

class SocketType
{
public:
    ix::WebSocket ix_socket = ix::WebSocket();

    std::string url;

    std::atomic_bool isUseable;
    std::atomic_bool isClosing;

    int onMessageRef = LUA_NOREF;
    int onCloseRef = LUA_NOREF;

    void setup(lua_State*);
};

inline std::unordered_map<void*, std::shared_ptr<SocketType>> sockets_map;
