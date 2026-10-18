// mk64-relay: forwards online-play packets between two players who cannot reach each other directly (strict office
// firewalls, symmetric NATs). Run it on any machine with a public UDP port:
//     c++ -O2 -std=c++17 relay.cpp -o mk64-relay && ./mk64-relay 46470
// Wire format (all packets start with "RLY1"):
//   client -> relay  R room[8] token[4]                      register (repeat every few seconds as a keepalive)
//   relay  -> client A ip-family[1] port[2] addr[4|16]       registered; your address as the relay sees it
//   client -> relay  D room[8] fromToken[4] toToken[4] data  forward data to the other registered player
//   relay  -> client d data                                  forwarded data
// The relay never looks inside the game packets.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

namespace {

struct Endpoint {
    sockaddr_in6 addr;
    uint64_t lastSeen;
};

uint64_t NowMs() {
    using namespace std::chrono;
    return (uint64_t) duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::string Key(const uint8_t* room, const uint8_t* token) {
    return std::string((const char*) room, 8) + std::string((const char*) token, 4);
}

} // namespace

int main(int argc, char** argv) {
    int port = argc > 1 ? std::atoi(argv[1]) : 46470;
    int fd = socket(AF_INET6, SOCK_DGRAM, 0);
    int off = 0;
    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));
    sockaddr_in6 any{};
    any.sin6_family = AF_INET6;
    any.sin6_addr = in6addr_any;
    any.sin6_port = htons((uint16_t) port);
    if (bind(fd, (sockaddr*) &any, sizeof(any)) != 0) {
        std::perror("bind");
        return 1;
    }
    std::printf("mk64-relay listening on UDP %d\n", port);
    std::fflush(stdout);

    std::map<std::string, Endpoint> registered;
    uint64_t lastSweep = NowMs();
    uint8_t buf[1600];
    for (;;) {
        sockaddr_in6 from{};
        socklen_t len = sizeof(from);
        ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, (sockaddr*) &from, &len);
        uint64_t now = NowMs();
        if (now - lastSweep > 10000) {
            for (auto it = registered.begin(); it != registered.end();) {
                it = now - it->second.lastSeen > 60000 ? registered.erase(it) : std::next(it);
            }
            lastSweep = now;
        }
        if (n < 5 || std::memcmp(buf, "RLY1", 4) != 0) {
            continue;
        }
        char type = (char) buf[4];
        if (type == 'R' && n >= 5 + 8 + 4) {
            registered[Key(buf + 5, buf + 13)] = { from, now };
            uint8_t reply[5 + 1 + 2 + 16];
            std::memcpy(reply, "RLY1A", 5);
            const uint8_t* a = from.sin6_addr.s6_addr;
            static const uint8_t kMapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };
            bool v4 = std::memcmp(a, kMapped, 12) == 0;
            reply[5] = v4 ? 4 : 6;
            std::memcpy(reply + 6, &from.sin6_port, 2);
            std::memcpy(reply + 8, v4 ? a + 12 : a, v4 ? 4 : 16);
            sendto(fd, reply, v4 ? 12 : 24, 0, (sockaddr*) &from, sizeof(from));
        } else if (type == 'D' && n >= 5 + 8 + 4 + 4) {
            auto self = registered.find(Key(buf + 5, buf + 13));
            if (self == registered.end() || std::memcmp(&self->second.addr, &from, sizeof(from)) != 0) {
                continue; // only registered senders, from the address they registered with
            }
            self->second.lastSeen = now;
            auto peer = registered.find(Key(buf + 5, buf + 17));
            if (peer == registered.end()) {
                continue;
            }
            size_t payload = (size_t) n - 21;
            uint8_t out[1600];
            std::memcpy(out, "RLY1d", 5);
            std::memcpy(out + 5, buf + 21, payload);
            sendto(fd, out, 5 + payload, 0, (sockaddr*) &peer->second.addr, sizeof(peer->second.addr));
        }
    }
}
