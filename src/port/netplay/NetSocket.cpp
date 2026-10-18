#include "NetSocket.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <random>
#include <thread>

#ifdef _WIN32
#include <iphlpapi.h>
#include <mstcpip.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <poll.h>
#include <unistd.h>
#endif

namespace netplay {

namespace {

#ifdef _WIN32
using SockFd = SOCKET;
#else
using SockFd = int;
#endif
SockFd S(intptr_t fd) {
    return (SockFd) fd;
}

#ifdef _WIN32
struct WsaInit {
    WsaInit() {
        WSADATA d;
        WSAStartup(MAKEWORD(2, 2), &d);
    }
};
void EnsureSockets() {
    static WsaInit init;
}
int LastError() {
    return WSAGetLastError();
}
bool WouldBlock(int err) {
    return err == WSAEWOULDBLOCK || err == WSAECONNRESET;
}
void CloseFd(intptr_t fd) {
    closesocket((SOCKET) fd);
}
#else
void EnsureSockets() {
}
int LastError() {
    return errno;
}
bool WouldBlock(int err) {
    // ICMP errors surface as ECONNREFUSED on some stacks; they are not fatal for an unconnected socket.
    return err == EAGAIN || err == EWOULDBLOCK || err == EINTR || err == ECONNREFUSED || err == ECONNRESET;
}
void CloseFd(intptr_t fd) {
    close((int) fd);
}
#endif

const uint8_t kV4Mapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };

NetAddr FromSockaddr(const sockaddr* sa) {
    NetAddr out;
    if (sa->sa_family == AF_INET6) {
        memcpy(&out.sa, sa, sizeof(sockaddr_in6));
        out.sa.sin6_scope_id = 0;
        out.sa.sin6_flowinfo = 0;
    } else if (sa->sa_family == AF_INET) {
        auto* v4 = (const sockaddr_in*) sa;
        out = NetAddr::FromV4((const uint8_t*) &v4->sin_addr, ntohs(v4->sin_port));
    }
    return out;
}

} // namespace

bool NetAddr::IsValid() const {
    return sa.sin6_family == AF_INET6 && sa.sin6_port != 0;
}

bool NetAddr::IsV4() const {
    return memcmp(sa.sin6_addr.s6_addr, kV4Mapped, 12) == 0;
}

bool NetAddr::IsLoopback() const {
    const uint8_t* a = sa.sin6_addr.s6_addr;
    if (IsV4()) {
        return a[12] == 127;
    }
    static const uint8_t kLoop[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
    return memcmp(a, kLoop, 16) == 0;
}

bool NetAddr::IsPrivate() const {
    const uint8_t* a = sa.sin6_addr.s6_addr;
    if (IsV4()) {
        const uint8_t* v = a + 12;
        return v[0] == 10 || (v[0] == 172 && (v[1] & 0xf0) == 16) || (v[0] == 192 && v[1] == 168) ||
               (v[0] == 100 && (v[1] & 0xc0) == 64);
    }
    return (a[0] & 0xfe) == 0xfc;
}

bool NetAddr::IsLinkLocal() const {
    const uint8_t* a = sa.sin6_addr.s6_addr;
    if (IsV4()) {
        return a[12] == 169 && a[13] == 254;
    }
    return a[0] == 0xfe && (a[1] & 0xc0) == 0x80;
}

uint16_t NetAddr::Port() const {
    return ntohs(sa.sin6_port);
}

void NetAddr::SetPort(uint16_t port) {
    sa.sin6_port = htons(port);
}

std::string NetAddr::HostString() const {
    char buf[INET6_ADDRSTRLEN] = { 0 };
    if (IsV4()) {
        inet_ntop(AF_INET, (const void*) (sa.sin6_addr.s6_addr + 12), buf, sizeof(buf));
    } else {
        inet_ntop(AF_INET6, (const void*) &sa.sin6_addr, buf, sizeof(buf));
    }
    return buf;
}

std::string NetAddr::ToString() const {
    if (IsV4()) {
        return HostString() + ":" + std::to_string(Port());
    }
    return "[" + HostString() + "]:" + std::to_string(Port());
}

size_t NetAddr::RawAddress(uint8_t out[16]) const {
    if (IsV4()) {
        memcpy(out, sa.sin6_addr.s6_addr + 12, 4);
        return 4;
    }
    memcpy(out, sa.sin6_addr.s6_addr, 16);
    return 16;
}

bool NetAddr::operator==(const NetAddr& o) const {
    return sa.sin6_port == o.sa.sin6_port && memcmp(sa.sin6_addr.s6_addr, o.sa.sin6_addr.s6_addr, 16) == 0;
}

NetAddr NetAddr::FromV4(const uint8_t ip[4], uint16_t port) {
    NetAddr out;
    out.sa.sin6_family = AF_INET6;
#ifdef __APPLE__
    out.sa.sin6_len = sizeof(sockaddr_in6);
#endif
    memcpy(out.sa.sin6_addr.s6_addr, kV4Mapped, 12);
    memcpy(out.sa.sin6_addr.s6_addr + 12, ip, 4);
    out.sa.sin6_port = htons(port);
    return out;
}

NetAddr NetAddr::FromV6(const uint8_t ip[16], uint16_t port) {
    NetAddr out;
    out.sa.sin6_family = AF_INET6;
#ifdef __APPLE__
    out.sa.sin6_len = sizeof(sockaddr_in6);
#endif
    memcpy(out.sa.sin6_addr.s6_addr, ip, 16);
    out.sa.sin6_port = htons(port);
    return out;
}

std::vector<NetAddr> NetAddr::Resolve(const std::string& host, uint16_t port) {
    EnsureSockets();
    std::vector<NetAddr> out;
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    std::string portStr = std::to_string(port);
    if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0 || res == nullptr) {
        return out;
    }
    for (addrinfo* it = res; it != nullptr; it = it->ai_next) {
        if (it->ai_family != AF_INET && it->ai_family != AF_INET6) {
            continue;
        }
        NetAddr a = FromSockaddr(it->ai_addr);
        if (a.IsValid() && std::find(out.begin(), out.end(), a) == out.end()) {
            out.push_back(a);
        }
    }
    freeaddrinfo(res);
    return out;
}

bool NetAddr::Parse(const std::string& textIn, uint16_t defaultPort, NetAddr& out) {
    std::string text = textIn;
    text.erase(0, text.find_first_not_of(" \t\r\n"));
    text.erase(text.find_last_not_of(" \t\r\n") + 1);
    if (text.empty()) {
        return false;
    }
    std::string host = text;
    uint16_t port = defaultPort;
    if (text[0] == '[') {
        size_t close = text.find(']');
        if (close == std::string::npos) {
            return false;
        }
        host = text.substr(1, close - 1);
        if (close + 1 < text.size() && text[close + 1] == ':') {
            port = (uint16_t) std::atoi(text.c_str() + close + 2);
        }
    } else if (std::count(text.begin(), text.end(), ':') == 1) {
        size_t colon = text.find(':');
        host = text.substr(0, colon);
        port = (uint16_t) std::atoi(text.c_str() + colon + 1);
    }
    if (port == 0) {
        return false;
    }
    std::vector<NetAddr> found = Resolve(host, port);
    if (found.empty()) {
        return false;
    }
    // Prefer IPv4: it is what LAN and port-forwarded hosts nearly always use.
    for (const NetAddr& a : found) {
        if (a.IsV4()) {
            out = a;
            return true;
        }
    }
    out = found[0];
    return true;
}

UdpSocket::~UdpSocket() {
    Close();
}

bool UdpSocket::Open(uint16_t port) {
    EnsureSockets();
    Close();
    intptr_t fd = (intptr_t) socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    mV6 = true;
#ifdef _WIN32
    if ((SOCKET) fd == INVALID_SOCKET) {
#else
    if (fd < 0) {
#endif
        fd = (intptr_t) socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        mV6 = false;
#ifdef _WIN32
        if ((SOCKET) fd == INVALID_SOCKET) {
#else
        if (fd < 0) {
#endif
            return false;
        }
    }
    if (mV6) {
        int off = 0;
        setsockopt(S(fd), IPPROTO_IPV6, IPV6_V6ONLY, (const char*) &off, sizeof(off));
    }
    int bufSize = 1 << 18;
    setsockopt(S(fd), SOL_SOCKET, SO_RCVBUF, (const char*) &bufSize, sizeof(bufSize));
    setsockopt(S(fd), SOL_SOCKET, SO_SNDBUF, (const char*) &bufSize, sizeof(bufSize));

    int bound;
    if (mV6) {
        sockaddr_in6 any{};
        any.sin6_family = AF_INET6;
        any.sin6_addr = in6addr_any;
        any.sin6_port = htons(port);
        bound = bind(S(fd), (const sockaddr*) &any, sizeof(any));
    } else {
        sockaddr_in any{};
        any.sin_family = AF_INET;
        any.sin_addr.s_addr = htonl(INADDR_ANY);
        any.sin_port = htons(port);
        bound = bind(S(fd), (const sockaddr*) &any, sizeof(any));
    }
    if (bound != 0) {
        CloseFd(fd);
        return false;
    }
#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket((SOCKET) fd, FIONBIO, &nb);
    BOOL reset = FALSE;
    DWORD ret = 0;
    WSAIoctl((SOCKET) fd, SIO_UDP_CONNRESET, &reset, sizeof(reset), nullptr, 0, &ret, nullptr, nullptr);
#else
    fcntl((int) fd, F_SETFL, fcntl((int) fd, F_GETFL, 0) | O_NONBLOCK);
#endif
    mFd = fd;
    return true;
}

void UdpSocket::Close() {
    if (mFd != -1) {
        CloseFd(mFd);
        mFd = -1;
    }
}

bool UdpSocket::IsOpen() const {
    return mFd != -1;
}

uint16_t UdpSocket::LocalPort() const {
    if (mFd == -1) {
        return 0;
    }
    sockaddr_storage ss{};
    socklen_t len = sizeof(ss);
    if (getsockname(S(mFd), (sockaddr*) &ss, &len) != 0) {
        return 0;
    }
    if (ss.ss_family == AF_INET6) {
        return ntohs(((sockaddr_in6*) &ss)->sin6_port);
    }
    return ntohs(((sockaddr_in*) &ss)->sin_port);
}

bool UdpSocket::SendTo(const NetAddr& to, const void* data, size_t len) {
    if (mFd == -1 || !to.IsValid()) {
        return false;
    }
    long sent;
    if (mV6) {
        sent = sendto(S(mFd), (const char*) data, (int) len, 0, (const sockaddr*) &to.sa,
                      sizeof(sockaddr_in6));
    } else {
        if (!to.IsV4()) {
            return false;
        }
        sockaddr_in v4{};
        v4.sin_family = AF_INET;
        memcpy(&v4.sin_addr, to.sa.sin6_addr.s6_addr + 12, 4);
        v4.sin_port = to.sa.sin6_port;
        sent = sendto(S(mFd), (const char*) data, (int) len, 0, (const sockaddr*) &v4,
                      sizeof(v4));
    }
    return sent == (long) len;
}

int UdpSocket::RecvFrom(NetAddr& from, void* buf, size_t cap) {
    if (mFd == -1) {
        return -1;
    }
    sockaddr_storage ss{};
    socklen_t len = sizeof(ss);
    long got = recvfrom(S(mFd), (char*) buf, (int) cap, 0, (sockaddr*) &ss, &len);
    if (got < 0) {
        return WouldBlock(LastError()) ? 0 : -1;
    }
    from = FromSockaddr((const sockaddr*) &ss);
    return (int) got;
}

bool UdpSocket::WaitReadable(int timeoutMs) {
    if (mFd == -1) {
        return false;
    }
#ifdef _WIN32
    WSAPOLLFD p{};
    p.fd = (SOCKET) mFd;
    p.events = POLLRDNORM;
    return WSAPoll(&p, 1, timeoutMs) > 0;
#else
    pollfd p{};
    p.fd = (int) mFd;
    p.events = POLLIN;
    return poll(&p, 1, timeoutMs) > 0;
#endif
}

std::vector<NetAddr> LocalAddresses(uint16_t port) {
    EnsureSockets();
    std::vector<NetAddr> v4, v6;
#ifdef _WIN32
    // Ask the routing table which local address reaches the internet; enough for the common single-NIC case.
    const char* probes[] = { "8.8.8.8", "2001:4860:4860::8888" };
    for (const char* probe : probes) {
        std::vector<NetAddr> dst = NetAddr::Resolve(probe, 53);
        if (dst.empty()) {
            continue;
        }
        int fam = dst[0].IsV4() ? AF_INET : AF_INET6;
        SOCKET s = socket(fam, SOCK_DGRAM, IPPROTO_UDP);
        if (s == INVALID_SOCKET) {
            continue;
        }
        int ok;
        if (fam == AF_INET) {
            sockaddr_in a{};
            a.sin_family = AF_INET;
            memcpy(&a.sin_addr, dst[0].sa.sin6_addr.s6_addr + 12, 4);
            a.sin_port = htons(53);
            ok = connect(s, (sockaddr*) &a, sizeof(a));
        } else {
            ok = connect(s, (sockaddr*) &dst[0].sa, sizeof(sockaddr_in6));
        }
        sockaddr_storage ss{};
        int len = sizeof(ss);
        if (ok == 0 && getsockname(s, (sockaddr*) &ss, &len) == 0) {
            NetAddr a = FromSockaddr((sockaddr*) &ss);
            a.SetPort(port);
            (a.IsV4() ? v4 : v6).push_back(a);
        }
        closesocket(s);
    }
#else
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) == 0) {
        for (ifaddrs* it = list; it != nullptr; it = it->ifa_next) {
            if (it->ifa_addr == nullptr || !(it->ifa_flags & IFF_UP) || (it->ifa_flags & IFF_LOOPBACK)) {
                continue;
            }
            int fam = it->ifa_addr->sa_family;
            if (fam != AF_INET && fam != AF_INET6) {
                continue;
            }
            NetAddr a = FromSockaddr(it->ifa_addr);
            a.SetPort(port);
            if (a.IsLoopback()) {
                continue;
            }
            if (!a.IsV4() && a.IsLinkLocal()) {
                continue; // needs a scope id; AWDL and friends only have these
            }
            std::vector<NetAddr>& bucket = a.IsV4() ? v4 : v6;
            if (std::find(bucket.begin(), bucket.end(), a) == bucket.end()) {
                bucket.push_back(a);
            }
        }
        freeifaddrs(list);
    }
#endif
    // Routable private IPv4 first, link-local (e.g. a Thunderbolt cable between two Macs) last.
    std::stable_sort(v4.begin(), v4.end(),
                     [](const NetAddr& a, const NetAddr& b) { return !a.IsLinkLocal() && b.IsLinkLocal(); });
    // IPv6: global unicast only, and at most two (macOS lists several temporary addresses).
    std::vector<NetAddr> out;
    for (size_t i = 0; i < v4.size() && i < 4; i++) {
        out.push_back(v4[i]);
    }
    size_t taken = 0;
    for (const NetAddr& a : v6) {
        if (taken < 2 && !a.IsPrivate()) {
            out.push_back(a);
            taken++;
        }
    }
    return out;
}

uint64_t NowMs() {
    using namespace std::chrono;
    return (uint64_t) duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void SleepMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

uint64_t RandomU64() {
    static std::mutex m;
    static std::mt19937_64 rng([] {
        std::random_device rd;
        return ((uint64_t) rd() << 32) ^ rd() ^ (uint64_t) std::chrono::high_resolution_clock::now().time_since_epoch().count();
    }());
    std::lock_guard<std::mutex> lock(m);
    return rng();
}

} // namespace netplay
