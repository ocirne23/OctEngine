module;

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <WinSock2.h>
#include <WS2tcpip.h>

module Network;

import Core;
import Core.Log;
import :Address;
import :Socket;

// stray ICMP "port unreachable" responses make recvfrom fail with WSAECONNRESET unless disabled
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

static_assert(sizeof(SOCKET) == sizeof(uint64));

static bool ensureWinsock()
{
    static const bool ok = []
    {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return ok;
}

static SOCKET toSocket(uint64 handle) { return (SOCKET)handle; }

static sockaddr_in toSockAddr(const NetAddress& address)
{
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(address.port);
    addr.sin_addr.s_addr = htonl(address.ip);
    return addr;
}

static NetAddress fromSockAddr(const sockaddr_in& addr)
{
    return { ntohl(addr.sin_addr.s_addr), ntohs(addr.sin_port) };
}

static void setNonBlocking(SOCKET s)
{
    u_long nonBlocking = 1;
    ioctlsocket(s, FIONBIO, &nonBlocking);
}

static void setNoDelay(SOCKET s)
{
    int on = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&on, sizeof(on));
}

NetAddress netResolveHost(oc::string_view hostName, uint16 port)
{
    if (!ensureWinsock())
        return {};
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* result = nullptr;
    const oc::string host(hostName);
    if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0 || !result)
        return {};
    NetAddress out = fromSockAddr(*reinterpret_cast<const sockaddr_in*>(result->ai_addr));
    out.port = port;
    freeaddrinfo(result);
    return out;
}

NetAddress netGetLocalAddress()
{
    if (!ensureWinsock())
        return NetAddress::loopback(0);
    const SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET)
        return NetAddress::loopback(0);
    // connect() on UDP only selects a route + source address; nothing goes on the wire
    sockaddr_in remote{};
    remote.sin_family = AF_INET;
    remote.sin_port = htons(53);
    remote.sin_addr.s_addr = htonl(0x08080808); // 8.8.8.8 - any public address works for routing
    NetAddress out = NetAddress::loopback(0);
    if (::connect(s, reinterpret_cast<const sockaddr*>(&remote), sizeof(remote)) == 0)
    {
        sockaddr_in local{};
        int len = sizeof(local);
        if (::getsockname(s, reinterpret_cast<sockaddr*>(&local), &len) == 0)
            out = fromSockAddr(local);
    }
    ::closesocket(s);
    out.port = 0;
    return out;
}

NetAddress netGetExternalAddress(uint32 timeoutMs)
{
    // Plain-HTTP IP echo services (no TLS in the engine). HTTP/1.0 keeps the reply un-chunked and
    // the server closes the connection after the body, so "read until closed" is the framing.
    constexpr const char* c_hosts[] = { "checkip.amazonaws.com", "api.ipify.org", "icanhazip.com" };
    const uint64 deadline = GetTickCount64() + timeoutMs;
    for (const char* host : c_hosts)
    {
        if (GetTickCount64() >= deadline)
            break;
        const NetAddress server = netResolveHost(host, 80);
        if (!server.isValid())
            continue;
        TcpSocket socket;
        if (!socket.connect(server))
            continue;
        while (socket.poll() == ETcpState::Connecting && GetTickCount64() < deadline)
            Sleep(5);
        if (socket.getState() != ETcpState::Connected)
            continue;

        char request[128];
        const int requestLen = snprintf(request, sizeof(request), "GET / HTTP/1.0\r\nHost: %s\r\n\r\n", host);
        oc::span<const uint8> pending(reinterpret_cast<const uint8*>(request), (size_t)requestLen);
        bool sendFailed = false;
        while (!pending.empty() && GetTickCount64() < deadline)
        {
            const int sent = socket.send(pending);
            if (sent < 0) { sendFailed = true; break; }
            if (sent == 0) { Sleep(5); socket.poll(); continue; }
            pending = pending.subspan((size_t)sent);
        }
        if (sendFailed || !pending.empty())
            continue;

        oc::string response;
        uint8 chunk[2048];
        while (GetTickCount64() < deadline && response.size() < 8192)
        {
            const int received = socket.receive(chunk);
            if (received < 0)
                break; // closed - the whole reply is in
            if (received == 0) { Sleep(5); socket.poll(); continue; }
            response.append(reinterpret_cast<const char*>(chunk), (size_t)received);
        }

        // "HTTP/1.x 200 ..." + blank line + the bare IP as the body
        if (response.find(" 200 ") == oc::string::npos)
            continue;
        const size_t bodyStart = response.find("\r\n\r\n");
        if (bodyStart == oc::string::npos)
            continue;
        oc::string_view body(response.c_str() + bodyStart + 4, response.size() - bodyStart - 4);
        while (!body.empty() && (body.back() == '\r' || body.back() == '\n' || body.back() == ' '))
            body.remove_suffix(1);
        while (!body.empty() && (body.front() == '\r' || body.front() == '\n' || body.front() == ' '))
            body.remove_prefix(1);
        const NetAddress external = NetAddress::fromString(body);
        if (external.ip != 0)
            return { external.ip, 0 };
    }
    return {};
}

bool UdpSocket::open(uint16 port, bool allowBroadcast)
{
    close();
    if (!ensureWinsock())
        return false;
    const SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET)
        return false;
    setNonBlocking(s);
    int bufferSize = 512 * 1024;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char*)&bufferSize, sizeof(bufferSize));
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char*)&bufferSize, sizeof(bufferSize));
    if (allowBroadcast)
    {
        int on = 1;
        setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char*)&on, sizeof(on));
    }
    BOOL behavior = FALSE;
    DWORD bytesReturned = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &behavior, sizeof(behavior), nullptr, 0, &bytesReturned, nullptr, nullptr);
    const sockaddr_in addr = toSockAddr(NetAddress::any(port));
    if (::bind(s, (const sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR)
    {
        Log::error("UdpSocket: bind failed on port " + oc::to_string(port) + " (error " + oc::to_string(WSAGetLastError()) + ")");
        closesocket(s);
        return false;
    }
    m_handle = (uint64)s;
    return true;
}

void UdpSocket::close()
{
    if (!isOpen())
        return;
    closesocket(toSocket(m_handle));
    m_handle = InvalidSocketHandle;
}

bool UdpSocket::sendTo(const NetAddress& to, oc::span<const uint8> data)
{
    if (!isOpen())
        return false;
    const sockaddr_in addr = toSockAddr(to);
    return ::sendto(toSocket(m_handle), (const char*)data.data(), (int)data.size(), 0, (const sockaddr*)&addr, sizeof(addr)) == (int)data.size();
}

int UdpSocket::receiveFrom(oc::span<uint8> buffer, NetAddress& outFrom)
{
    if (!isOpen())
        return -1;
    sockaddr_in from{};
    int fromLength = sizeof(from);
    const int result = ::recvfrom(toSocket(m_handle), (char*)buffer.data(), (int)buffer.size(), 0, (sockaddr*)&from, &fromLength);
    if (result == SOCKET_ERROR)
        return -1; // WSAEWOULDBLOCK or a stray network error: nothing to read
    outFrom = fromSockAddr(from);
    return result;
}

uint16 UdpSocket::getLocalPort() const
{
    if (!isOpen())
        return 0;
    sockaddr_in addr{};
    int length = sizeof(addr);
    if (getsockname(toSocket(m_handle), (sockaddr*)&addr, &length) == SOCKET_ERROR)
        return 0;
    return ntohs(addr.sin_port);
}

bool TcpSocket::connect(const NetAddress& to)
{
    close();
    if (!ensureWinsock())
        return false;
    const SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET)
        return false;
    setNonBlocking(s);
    setNoDelay(s);
    const sockaddr_in addr = toSockAddr(to);
    if (::connect(s, (const sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK)
    {
        closesocket(s);
        m_state = ETcpState::Failed;
        return false;
    }
    m_handle = (uint64)s;
    m_state = ETcpState::Connecting;
    return true;
}

ETcpState TcpSocket::poll()
{
    if (m_state != ETcpState::Connecting || !isOpen())
        return m_state;
    const SOCKET s = toSocket(m_handle);
    fd_set writeSet, exceptSet;
    FD_ZERO(&writeSet);
    FD_ZERO(&exceptSet);
    FD_SET(s, &writeSet);
    FD_SET(s, &exceptSet);
    timeval timeout{ 0, 0 };
    if (select(0, nullptr, &writeSet, &exceptSet, &timeout) > 0)
    {
        if (FD_ISSET(s, &exceptSet))
            m_state = ETcpState::Failed;
        else if (FD_ISSET(s, &writeSet))
            m_state = ETcpState::Connected;
    }
    return m_state;
}

int TcpSocket::send(oc::span<const uint8> data)
{
    if (m_state != ETcpState::Connected || !isOpen() || data.empty())
        return m_state == ETcpState::Connected ? 0 : -1;
    const int result = ::send(toSocket(m_handle), (const char*)data.data(), (int)data.size(), 0);
    if (result == SOCKET_ERROR)
    {
        if (WSAGetLastError() == WSAEWOULDBLOCK)
            return 0;
        m_state = ETcpState::Failed;
        return -1;
    }
    return result;
}

int TcpSocket::receive(oc::span<uint8> buffer)
{
    if (m_state != ETcpState::Connected || !isOpen())
        return -1;
    const int result = ::recv(toSocket(m_handle), (char*)buffer.data(), (int)buffer.size(), 0);
    if (result == 0) // graceful close
    {
        m_state = ETcpState::Closed;
        return -1;
    }
    if (result == SOCKET_ERROR)
    {
        if (WSAGetLastError() == WSAEWOULDBLOCK)
            return 0;
        m_state = ETcpState::Failed;
        return -1;
    }
    return result;
}

void TcpSocket::close()
{
    if (isOpen())
        closesocket(toSocket(m_handle));
    m_handle = InvalidSocketHandle;
    m_state = ETcpState::Closed;
}

NetAddress TcpSocket::getRemoteAddress() const
{
    if (!isOpen())
        return {};
    sockaddr_in addr{};
    int length = sizeof(addr);
    if (getpeername(toSocket(m_handle), (sockaddr*)&addr, &length) == SOCKET_ERROR)
        return {};
    return fromSockAddr(addr);
}

bool TcpListener::listen(uint16 port, int backlog)
{
    close();
    if (!ensureWinsock())
        return false;
    const SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET)
        return false;
    setNonBlocking(s);
    const sockaddr_in addr = toSockAddr(NetAddress::any(port));
    if (::bind(s, (const sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR || ::listen(s, backlog) == SOCKET_ERROR)
    {
        Log::error("TcpListener: listen failed on port " + oc::to_string(port) + " (error " + oc::to_string(WSAGetLastError()) + ")");
        closesocket(s);
        return false;
    }
    m_handle = (uint64)s;
    return true;
}

bool TcpListener::accept(TcpSocket& outSocket, NetAddress& outFrom)
{
    if (!isOpen())
        return false;
    sockaddr_in from{};
    int fromLength = sizeof(from);
    const SOCKET s = ::accept(toSocket(m_handle), (sockaddr*)&from, &fromLength);
    if (s == INVALID_SOCKET)
        return false;
    setNonBlocking(s);
    setNoDelay(s);
    outSocket.close();
    outSocket.m_handle = (uint64)s;
    outSocket.m_state = ETcpState::Connected;
    outFrom = fromSockAddr(from);
    return true;
}

void TcpListener::close()
{
    if (!isOpen())
        return;
    closesocket(toSocket(m_handle));
    m_handle = InvalidSocketHandle;
}
