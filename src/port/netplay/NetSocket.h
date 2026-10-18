#pragma once

// Dual-stack UDP socket and endpoint helpers for netplay.
// One AF_INET6 socket with IPV6_V6ONLY off carries both IPv4 (as IPv4-mapped addresses) and IPv6 traffic,
// so the port the NAT maps for STUN is the same port the game talks on.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace netplay {

struct NetAddr {
    sockaddr_in6 sa{};

    bool IsValid() const;
    bool IsV4() const;
    bool IsLoopback() const;
    bool IsPrivate() const;   // 10/8, 172.16/12, 192.168/16, 100.64/10 (CGNAT), fc00::/7
    bool IsLinkLocal() const; // 169.254/16, fe80::/10
    uint16_t Port() const;
    void SetPort(uint16_t port);
    std::string HostString() const;
    std::string ToString() const; // "1.2.3.4:5" or "[2001:db8::1]:5"

    // 4 bytes for IPv4, 16 for IPv6. Returns the number of bytes written.
    size_t RawAddress(uint8_t out[16]) const;

    bool operator==(const NetAddr& o) const;
    bool operator!=(const NetAddr& o) const {
        return !(*this == o);
    }

    static NetAddr FromV4(const uint8_t ip[4], uint16_t port);
    static NetAddr FromV6(const uint8_t ip[16], uint16_t port);
    // Accepts "1.2.3.4", "1.2.3.4:5", "[v6]:5", "v6", "host", "host:5". Names are resolved (blocking).
    static bool Parse(const std::string& text, uint16_t defaultPort, NetAddr& out);
    static std::vector<NetAddr> Resolve(const std::string& host, uint16_t port);
};

class UdpSocket {
  public:
    UdpSocket() = default;
    ~UdpSocket();
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    // port 0 picks an ephemeral port. The socket is non-blocking.
    bool Open(uint16_t port);
    void Close();
    bool IsOpen() const;
    uint16_t LocalPort() const;
    bool SendTo(const NetAddr& to, const void* data, size_t len);
    // Bytes received, 0 when nothing is pending, -1 on a hard error.
    int RecvFrom(NetAddr& from, void* buf, size_t cap);
    bool WaitReadable(int timeoutMs);
    bool HasV6() const {
        return mV6;
    }

  private:
    intptr_t mFd = -1;
    bool mV6 = true;
};

// This machine's interface addresses (no loopback), all carrying `port`.
std::vector<NetAddr> LocalAddresses(uint16_t port);

uint64_t NowMs();
void SleepMs(int ms);
uint64_t RandomU64();

} // namespace netplay
