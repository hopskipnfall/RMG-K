/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef PRACTICE_SOCKET_HPP
#define PRACTICE_SOCKET_HPP

// Thin cross-platform (Winsock / BSD) loopback-TCP shim used by the practice
// transport and its tests. Header-only on purpose: tests share it.

#include <cstddef>
#include <cstdint>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#ifndef _WIN32
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0 // macOS: SO_NOSIGPIPE is set per socket instead
#endif
#endif

namespace PracticeSocket
{
#ifdef _WIN32
using Handle = SOCKET;
constexpr Handle kInvalid = INVALID_SOCKET;
#else
using Handle = int;
constexpr Handle kInvalid = -1;
#endif

inline bool Startup()
{
#ifdef _WIN32
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
#else
    return true;
#endif
}

inline void Shutdown()
{
#ifdef _WIN32
    WSACleanup();
#endif
}

inline void Close(Handle socketHandle)
{
    if (socketHandle == kInvalid)
    {
        return;
    }
#ifdef _WIN32
    closesocket(socketHandle);
#else
    close(socketHandle);
#endif
}

inline void Configure(Handle socketHandle)
{
    int one = 1;
    setsockopt(socketHandle, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
#ifdef SO_NOSIGPIPE
    setsockopt(socketHandle, SOL_SOCKET, SO_NOSIGPIPE, reinterpret_cast<const char*>(&one), sizeof(one));
#endif
}

inline sockaddr_in LoopbackAddress(uint16_t port)
{
    sockaddr_in address{};
    address.sin_family      = AF_INET;
    address.sin_port        = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return address;
}

// Connects to 127.0.0.1:port. kInvalid on failure.
inline Handle ConnectLoopback(uint16_t port)
{
    Handle socketHandle = socket(AF_INET, SOCK_STREAM, 0);
    if (socketHandle == kInvalid)
    {
        return kInvalid;
    }
    sockaddr_in address = LoopbackAddress(port);
    if (connect(socketHandle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
    {
        Close(socketHandle);
        return kInvalid;
    }
    Configure(socketHandle);
    return socketHandle;
}

// Listens on 127.0.0.1:port (0 = ephemeral); the bound port is stored in
// *boundPort. Used by tests' fake server.
inline Handle ListenLoopback(uint16_t port, uint16_t* boundPort)
{
    Handle listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == kInvalid)
    {
        return kInvalid;
    }
    sockaddr_in address = LoopbackAddress(port);
    if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(listener, 1) != 0)
    {
        Close(listener);
        return kInvalid;
    }
    socklen_t length = sizeof(address);
    getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length);
    *boundPort = ntohs(address.sin_port);
    return listener;
}

// 1 = readable (or closed/error - recv will tell), 0 = timeout, -1 = failure.
inline int WaitReadable(Handle socketHandle, int timeoutMs)
{
#ifdef _WIN32
    WSAPOLLFD entry{};
    entry.fd     = socketHandle;
    entry.events = POLLRDNORM;
    const int ready = WSAPoll(&entry, 1, timeoutMs);
#else
    pollfd entry{};
    entry.fd     = socketHandle;
    entry.events = POLLIN;
    const int ready = poll(&entry, 1, timeoutMs);
#endif
    return ready > 0 ? 1 : (ready == 0 ? 0 : -1);
}

inline Handle Accept(Handle listener, int timeoutMs)
{
    if (WaitReadable(listener, timeoutMs) != 1)
    {
        return kInvalid;
    }
    Handle client = accept(listener, nullptr, nullptr);
    if (client != kInvalid)
    {
        Configure(client);
    }
    return client;
}

// Returns bytes read; 0 = peer closed; <0 = error.
inline long Recv(Handle socketHandle, uint8_t* buffer, size_t size)
{
    return static_cast<long>(recv(socketHandle, reinterpret_cast<char*>(buffer), static_cast<int>(size), 0));
}

inline bool SendAll(Handle socketHandle, const uint8_t* data, size_t size)
{
    while (size > 0)
    {
        const long sent = static_cast<long>(send(socketHandle, reinterpret_cast<const char*>(data),
                                                 static_cast<int>(size), MSG_NOSIGNAL));
        if (sent <= 0)
        {
            return false;
        }
        data += sent;
        size -= static_cast<size_t>(sent);
    }
    return true;
}
} // namespace PracticeSocket

#endif // PRACTICE_SOCKET_HPP
