#pragma once

// STUN (RFC 5389) binding requests: ask a public server which address and port our UDP socket appears as
// from the internet. That "server-reflexive" address is what the other player punches towards.

#include "NetSocket.h"

#include <string>
#include <vector>

namespace netplay {

namespace stun {

constexpr uint32_t kMagicCookie = 0x2112A442;

size_t BuildBindingRequest(uint8_t out[20], const uint8_t txid[12]);
bool IsStunPacket(const uint8_t* data, size_t len);
// True when `data` is a binding success response for `txid`; `mapped` receives the reflexive address.
bool ParseBindingResponse(const uint8_t* data, size_t len, const uint8_t txid[12], NetAddr& mapped);

} // namespace stun

// Queries a list of public STUN servers over the caller's socket. Not thread-safe; drive it from the thread that
// owns the socket: Start(), then feed every received STUN packet to OnPacket() and call Tick() regularly.
class StunProbe {
  public:
    void Start(UdpSocket& sock, const std::vector<std::string>& servers);
    void Tick(UdpSocket& sock);
    void OnPacket(const uint8_t* data, size_t len);
    bool Done() const;
    const std::vector<NetAddr>& Results() const {
        return mResults;
    }

  private:
    struct Query {
        NetAddr server;
        uint8_t txid[12];
        uint64_t lastSent = 0;
        int tries = 0;
        bool answered = false;
    };
    std::vector<Query> mQueries;
    std::vector<NetAddr> mResults;
    uint64_t mStarted = 0;
};

const std::vector<std::string>& DefaultStunServers();

} // namespace netplay
