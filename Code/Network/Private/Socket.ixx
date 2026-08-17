export module Network:Socket;

import Core;
import :Address;

// Thin non-blocking Winsock wrappers. IPv4 only, Winsock is initialized lazily on first use.
// RAII movable handles, like PhysicsBody/RenderNode.

export constexpr uint64 InvalidSocketHandle = ~0ull;

// blocking DNS lookup (first IPv4 result), invalid address on failure
export NetAddress netResolveHost(oc::string_view hostName, uint16 port);

export class UdpSocket final
{
public:

    UdpSocket() = default;
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;
    UdpSocket(UdpSocket&& other) noexcept : m_handle(other.m_handle) { other.m_handle = InvalidSocketHandle; }
    UdpSocket& operator=(UdpSocket&& other) noexcept
    {
        if (this != &other)
        {
            close();
            m_handle = other.m_handle;
            other.m_handle = InvalidSocketHandle;
        }
        return *this;
    }
    ~UdpSocket() { close(); }

    bool open(uint16 port = 0, bool allowBroadcast = false); // port 0 = ephemeral
    void close();
    bool isOpen() const { return m_handle != InvalidSocketHandle; }

    bool sendTo(const NetAddress& to, oc::span<const uint8> data);
    int receiveFrom(oc::span<uint8> buffer, NetAddress& outFrom); // datagram size, -1 when none pending

    uint16 getLocalPort() const;

private:

    uint64 m_handle = InvalidSocketHandle;
};

export enum class ETcpState : uint8
{
    Closed,
    Connecting,
    Connected,
    Failed,
};

export class TcpSocket final
{
public:

    TcpSocket() = default;
    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;
    TcpSocket(TcpSocket&& other) noexcept : m_handle(other.m_handle), m_state(other.m_state)
    {
        other.m_handle = InvalidSocketHandle;
        other.m_state = ETcpState::Closed;
    }
    TcpSocket& operator=(TcpSocket&& other) noexcept
    {
        if (this != &other)
        {
            close();
            m_handle = other.m_handle;
            m_state = other.m_state;
            other.m_handle = InvalidSocketHandle;
            other.m_state = ETcpState::Closed;
        }
        return *this;
    }
    ~TcpSocket() { close(); }

    bool connect(const NetAddress& to); // begins a non-blocking connect, poll() resolves it
    ETcpState poll();                   // Connecting -> Connected/Failed
    ETcpState getState() const { return m_state; }
    bool isOpen() const { return m_handle != InvalidSocketHandle; }

    int send(oc::span<const uint8> data); // bytes accepted (may be < data.size()), -1 on error
    int receive(oc::span<uint8> buffer);  // bytes read, 0 = nothing pending, -1 = closed or error

    void close();
    NetAddress getRemoteAddress() const;

private:

    friend class TcpListener;
    uint64 m_handle = InvalidSocketHandle;
    ETcpState m_state = ETcpState::Closed;
};

export class TcpListener final
{
public:

    TcpListener() = default;
    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;
    TcpListener(TcpListener&& other) noexcept : m_handle(other.m_handle) { other.m_handle = InvalidSocketHandle; }
    TcpListener& operator=(TcpListener&& other) noexcept
    {
        if (this != &other)
        {
            close();
            m_handle = other.m_handle;
            other.m_handle = InvalidSocketHandle;
        }
        return *this;
    }
    ~TcpListener() { close(); }

    bool listen(uint16 port, int backlog = 16);
    bool accept(TcpSocket& outSocket, NetAddress& outFrom); // false when no pending connection
    void close();
    bool isOpen() const { return m_handle != InvalidSocketHandle; }

private:

    uint64 m_handle = InvalidSocketHandle;
};

// Length-prefixed message framing over a TcpSocket (uint32 size + payload) with buffering for
// partial non-blocking sends/receives. Call flushSend() every frame while data is pending.
export class TcpMessageStream final
{
public:

    static constexpr uint32 MaxMessageSize = 16 * 1024 * 1024;

    TcpMessageStream() = default;
    explicit TcpMessageStream(TcpSocket&& socket) : m_socket(oc::move(socket)) {}

    TcpSocket& socket() { return m_socket; }
    bool isConnected() { return m_socket.poll() == ETcpState::Connected; }

    bool sendMessage(oc::span<const uint8> data)
    {
        if (data.size() > MaxMessageSize)
            return false;
        const uint32 size = uint32(data.size());
        const uint8* sizeBytes = reinterpret_cast<const uint8*>(&size);
        m_sendBuffer.insert(m_sendBuffer.end(), sizeBytes, sizeBytes + sizeof(size));
        m_sendBuffer.insert(m_sendBuffer.end(), data.begin(), data.end());
        return flushSend();
    }

    // moves one complete message into out; false when none is available (yet)
    bool receiveMessage(oc::vector<uint8>& out)
    {
        pumpReceive();
        if (m_recvBuffer.size() - m_recvOffset < sizeof(uint32))
            return false;
        uint32 size = 0;
        memcpy(&size, m_recvBuffer.data() + m_recvOffset, sizeof(size));
        if (size > MaxMessageSize) // corrupt/hostile stream, no way to resync
        {
            m_socket.close();
            return false;
        }
        if (m_recvBuffer.size() - m_recvOffset - sizeof(uint32) < size)
            return false;
        const uint8* payload = m_recvBuffer.data() + m_recvOffset + sizeof(uint32);
        out.assign(payload, payload + size);
        m_recvOffset += sizeof(uint32) + size;
        if (m_recvOffset >= m_recvBuffer.size())
        {
            m_recvBuffer.clear();
            m_recvOffset = 0;
        }
        else if (m_recvOffset > 64 * 1024)
        {
            m_recvBuffer.erase(m_recvBuffer.begin(), m_recvBuffer.begin() + m_recvOffset);
            m_recvOffset = 0;
        }
        return true;
    }

    // false when the socket errored; pending data survives until the connection drops
    bool flushSend()
    {
        m_socket.poll();
        while (m_sendOffset < m_sendBuffer.size())
        {
            const int sent = m_socket.send(oc::span(m_sendBuffer).subspan(m_sendOffset));
            if (sent < 0)
                return false;
            if (sent == 0)
                return true; // would block, retry next frame
            m_sendOffset += sent;
        }
        m_sendBuffer.clear();
        m_sendOffset = 0;
        return true;
    }

    bool hasPendingSend() const { return m_sendOffset < m_sendBuffer.size(); }

private:

    void pumpReceive()
    {
        m_socket.poll();
        uint8 chunk[16 * 1024];
        while (true)
        {
            const int received = m_socket.receive(chunk);
            if (received <= 0)
                break;
            m_recvBuffer.insert(m_recvBuffer.end(), chunk, chunk + received);
        }
    }

    TcpSocket m_socket;
    oc::vector<uint8> m_sendBuffer;
    size_t m_sendOffset = 0;
    oc::vector<uint8> m_recvBuffer;
    size_t m_recvOffset = 0;
};
