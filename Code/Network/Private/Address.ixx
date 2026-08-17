export module Network:Address;

import Core;

// IPv4 endpoint in host byte order. IPv6 is intentionally unsupported.
export struct NetAddress final
{
    uint32 ip = 0;   // host byte order: 127.0.0.1 == 0x7f000001
    uint16 port = 0;

    bool isValid() const { return ip != 0 || port != 0; }
    uint64 key() const { return (uint64(ip) << 16) | port; } // unique map key
    bool operator==(const NetAddress&) const = default;

    oc::string toString() const
    {
        return oc::to_string((ip >> 24) & 0xff) + "." + oc::to_string((ip >> 16) & 0xff) + "." +
               oc::to_string((ip >> 8) & 0xff) + "." + oc::to_string(ip & 0xff) + ":" + oc::to_string(port);
    }

    // "127.0.0.1:1234" or "127.0.0.1" (port stays 0). No DNS — use netResolveHost for names.
    static NetAddress fromString(oc::string_view text)
    {
        NetAddress out;
        const char* ptr = text.data();
        const char* end = ptr + text.size();
        uint32 ip = 0;
        for (int i = 0; i < 4; ++i)
        {
            uint32 octet = 0;
            const std::from_chars_result result = std::from_chars(ptr, end, octet);
            if (result.ec != std::errc() || octet > 255)
                return {};
            ip = (ip << 8) | octet;
            ptr = result.ptr;
            if (i < 3)
            {
                if (ptr >= end || *ptr != '.')
                    return {};
                ++ptr;
            }
        }
        out.ip = ip;
        if (ptr < end && *ptr == ':')
        {
            ++ptr;
            if (std::from_chars(ptr, end, out.port).ec != std::errc())
                return {};
        }
        return out;
    }

    static NetAddress loopback(uint16 port) { return { 0x7f000001, port }; }
    static NetAddress any(uint16 port) { return { 0, port }; }
    static NetAddress broadcastAll(uint16 port) { return { 0xffffffff, port }; } // requires UdpSocket allowBroadcast
};
