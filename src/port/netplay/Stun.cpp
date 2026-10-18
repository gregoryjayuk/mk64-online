#include "Stun.h"

#include <algorithm>
#include <cstring>

namespace netplay {

namespace stun {

namespace {
uint16_t Get16(const uint8_t* p) {
    return (uint16_t) ((p[0] << 8) | p[1]);
}
uint32_t Get32(const uint8_t* p) {
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | p[3];
}
} // namespace

size_t BuildBindingRequest(uint8_t out[20], const uint8_t txid[12]) {
    out[0] = 0x00; // Binding Request
    out[1] = 0x01;
    out[2] = 0x00; // no attributes
    out[3] = 0x00;
    out[4] = (kMagicCookie >> 24) & 0xff;
    out[5] = (kMagicCookie >> 16) & 0xff;
    out[6] = (kMagicCookie >> 8) & 0xff;
    out[7] = kMagicCookie & 0xff;
    memcpy(out + 8, txid, 12);
    return 20;
}

bool IsStunPacket(const uint8_t* data, size_t len) {
    return len >= 20 && (data[0] & 0xc0) == 0 && Get32(data + 4) == kMagicCookie;
}

bool ParseBindingResponse(const uint8_t* data, size_t len, const uint8_t txid[12], NetAddr& mapped) {
    if (!IsStunPacket(data, len) || Get16(data) != 0x0101 || memcmp(data + 8, txid, 12) != 0) {
        return false;
    }
    size_t bodyLen = Get16(data + 2);
    if (20 + bodyLen > len) {
        return false;
    }
    bool found = false;
    size_t pos = 20;
    while (pos + 4 <= 20 + bodyLen) {
        uint16_t type = Get16(data + pos);
        uint16_t alen = Get16(data + pos + 2);
        const uint8_t* v = data + pos + 4;
        if (pos + 4 + alen > 20 + bodyLen) {
            break;
        }
        bool xored = type == 0x0020;
        if ((type == 0x0020 || type == 0x0001) && alen >= 8) {
            uint8_t family = v[1];
            uint16_t port = Get16(v + 2);
            if (xored) {
                port ^= (uint16_t) (kMagicCookie >> 16);
            }
            if (family == 0x01) {
                uint8_t ip[4];
                memcpy(ip, v + 4, 4);
                if (xored) {
                    for (int i = 0; i < 4; i++) {
                        ip[i] ^= (uint8_t) (kMagicCookie >> (24 - 8 * i));
                    }
                }
                mapped = NetAddr::FromV4(ip, port);
                found = true;
            } else if (family == 0x02 && alen >= 20) {
                uint8_t ip[16];
                memcpy(ip, v + 4, 16);
                if (xored) {
                    uint8_t key[16];
                    for (int i = 0; i < 4; i++) {
                        key[i] = (uint8_t) (kMagicCookie >> (24 - 8 * i));
                    }
                    memcpy(key + 4, txid, 12);
                    for (int i = 0; i < 16; i++) {
                        ip[i] ^= key[i];
                    }
                }
                mapped = NetAddr::FromV6(ip, port);
                found = true;
            }
            if (found && xored) {
                break; // XOR-MAPPED-ADDRESS wins over the legacy attribute
            }
        }
        pos += 4 + ((alen + 3) & ~3u);
    }
    return found;
}

} // namespace stun

const std::vector<std::string>& DefaultStunServers() {
    static const std::vector<std::string> servers = {
        "stun.l.google.com:19302",
        "stun.cloudflare.com:3478",
        "stun1.l.google.com:19302",
    };
    return servers;
}

void StunProbe::Start(UdpSocket& sock, const std::vector<std::string>& servers) {
    mQueries.clear();
    mResults.clear();
    mStarted = NowMs();
    for (const std::string& s : servers) {
        size_t colon = s.rfind(':');
        std::string host = s.substr(0, colon);
        uint16_t port = (uint16_t) std::atoi(s.c_str() + colon + 1);
        for (const NetAddr& a : NetAddr::Resolve(host, port)) {
            if (!a.IsV4() && !sock.HasV6()) {
                continue;
            }
            Query q;
            q.server = a;
            for (int i = 0; i < 12; i++) {
                q.txid[i] = (uint8_t) RandomU64();
            }
            mQueries.push_back(q);
        }
    }
    Tick(sock);
}

void StunProbe::Tick(UdpSocket& sock) {
    uint64_t now = NowMs();
    for (Query& q : mQueries) {
        if (q.answered || q.tries >= 4) {
            continue;
        }
        if (q.tries == 0 || now - q.lastSent >= (uint64_t) (250 << q.tries)) {
            uint8_t req[20];
            stun::BuildBindingRequest(req, q.txid);
            sock.SendTo(q.server, req, sizeof(req));
            q.lastSent = now;
            q.tries++;
        }
    }
}

void StunProbe::OnPacket(const uint8_t* data, size_t len) {
    for (Query& q : mQueries) {
        NetAddr mapped;
        if (!q.answered && stun::ParseBindingResponse(data, len, q.txid, mapped)) {
            q.answered = true;
            if (std::find(mResults.begin(), mResults.end(), mapped) == mResults.end()) {
                mResults.push_back(mapped);
            }
            return;
        }
    }
}

bool StunProbe::Done() const {
    uint64_t now = NowMs();
    bool haveV4 = false, haveV6 = false, waitingV4 = false, waitingV6 = false;
    for (const NetAddr& r : mResults) {
        (r.IsV4() ? haveV4 : haveV6) = true;
    }
    for (const Query& q : mQueries) {
        bool pending = !q.answered && (q.tries < 4 || now - q.lastSent < 1000);
        if (pending) {
            (q.server.IsV4() ? waitingV4 : waitingV6) = true;
        }
    }
    // One answer per family is enough; give up on a family once every server for it has timed out.
    if (now - mStarted > 4000) {
        return true;
    }
    return (haveV4 || !waitingV4) && (haveV6 || !waitingV6);
}

} // namespace netplay
