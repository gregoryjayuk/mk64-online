#include "Signal.h"

#include <cctype>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#endif

namespace netplay {

namespace {

constexpr uint8_t kOfferVersion = 1;
constexpr const char* kCrockford = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
constexpr const char* kRendezvousBase = "https://ntfy.sh/";

uint64_t SplitMix(uint64_t& state) {
    uint64_t z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

void Scramble(std::vector<uint8_t>& data, const std::string& room) {
    uint64_t state = Fnv1a64(room + "|mk64-online|key");
    for (size_t i = 0; i < data.size(); i += 8) {
        uint64_t k = SplitMix(state);
        for (size_t j = 0; j < 8 && i + j < data.size(); j++) {
            data[i + j] ^= (uint8_t) (k >> (8 * j));
        }
    }
}

std::string TopicFor(const std::string& room) {
    char buf[32];
    snprintf(buf, sizeof(buf), "mk64o%016llx", (unsigned long long) Fnv1a64(room + "|mk64-online|topic"));
    return buf;
}

int RunCommand(const std::string& cmd, std::string& output) {
    output.clear();
    FILE* p = popen(cmd.c_str(), "r");
    if (p == nullptr) {
        return -1;
    }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), p)) > 0) {
        output.append(buf, n);
    }
    return pclose(p);
}

#ifdef _WIN32
constexpr const char* kCurl = "curl.exe";
constexpr const char* kQuiet = " 2>NUL";
#else
constexpr const char* kCurl = "curl";
constexpr const char* kQuiet = " 2>/dev/null";
#endif

// Pulls the string value of `"key":"..."` out of one JSON line. ntfy messages here are base64url, so no escapes.
std::string JsonString(const std::string& line, const char* key) {
    std::string needle = std::string("\"") + key + "\":\"";
    size_t at = line.find(needle);
    if (at == std::string::npos) {
        return "";
    }
    at += needle.size();
    size_t end = line.find('"', at);
    return end == std::string::npos ? "" : line.substr(at, end - at);
}

long long JsonNumber(const std::string& line, const char* key) {
    std::string needle = std::string("\"") + key + "\":";
    size_t at = line.find(needle);
    return at == std::string::npos ? 0 : std::atoll(line.c_str() + at + needle.size());
}

} // namespace

uint64_t Fnv1a64(const std::string& s) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 0x100000001b3ull;
    }
    return h;
}

std::string Base64UrlEncode(const uint8_t* data, size_t len) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < len; i++) {
        acc = (acc << 8) | data[i];
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            out += tbl[(acc >> bits) & 63];
        }
    }
    if (bits > 0) {
        out += tbl[(acc << (6 - bits)) & 63];
    }
    return out;
}

bool Base64UrlDecode(const std::string& text, std::vector<uint8_t>& out) {
    out.clear();
    uint32_t acc = 0;
    int bits = 0;
    for (char c : text) {
        int v;
        if (c >= 'A' && c <= 'Z') {
            v = c - 'A';
        } else if (c >= 'a' && c <= 'z') {
            v = c - 'a' + 26;
        } else if (c >= '0' && c <= '9') {
            v = c - '0' + 52;
        } else if (c == '-' || c == '+') {
            v = 62;
        } else if (c == '_' || c == '/') {
            v = 63;
        } else if (c == '=' || isspace((unsigned char) c)) {
            continue;
        } else {
            return false;
        }
        acc = (acc << 6) | (uint32_t) v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((uint8_t) (acc >> bits));
        }
    }
    return true;
}

std::vector<uint8_t> PackOffer(const Offer& offer) {
    std::vector<uint8_t> b;
    b.push_back(kOfferVersion);
    for (int i = 0; i < 4; i++) {
        b.push_back((uint8_t) (offer.token >> (8 * i)));
    }
    for (int i = 0; i < 4; i++) {
        b.push_back((uint8_t) (offer.peerToken >> (8 * i)));
    }
    b.push_back(offer.fromHost ? 1 : 0);
    size_t n = offer.cands.size() > 12 ? 12 : offer.cands.size();
    b.push_back((uint8_t) n);
    for (size_t i = 0; i < n; i++) {
        const Candidate& c = offer.cands[i];
        uint8_t raw[16];
        size_t alen = c.addr.RawAddress(raw);
        b.push_back((uint8_t) (((uint8_t) c.type << 4) | (alen == 4 ? 4 : 6)));
        b.insert(b.end(), raw, raw + alen);
        uint16_t port = c.addr.Port();
        b.push_back((uint8_t) (port >> 8));
        b.push_back((uint8_t) port);
    }
    return b;
}

bool UnpackOffer(const uint8_t* d, size_t len, Offer& out) {
    if (len < 11 || d[0] != kOfferVersion) {
        return false;
    }
    out = Offer();
    for (int i = 0; i < 4; i++) {
        out.token |= (uint32_t) d[1 + i] << (8 * i);
        out.peerToken |= (uint32_t) d[5 + i] << (8 * i);
    }
    out.fromHost = d[9] != 0;
    size_t n = d[10];
    size_t pos = 11;
    for (size_t i = 0; i < n; i++) {
        if (pos >= len) {
            return false;
        }
        uint8_t tag = d[pos++];
        size_t alen = (tag & 0x0f) == 4 ? 4 : 16;
        if (pos + alen + 2 > len) {
            return false;
        }
        Candidate c;
        c.type = (CandType) (tag >> 4);
        uint16_t port = (uint16_t) ((d[pos + alen] << 8) | d[pos + alen + 1]);
        c.addr = alen == 4 ? NetAddr::FromV4(d + pos, port) : NetAddr::FromV6(d + pos, port);
        pos += alen + 2;
        out.cands.push_back(c);
    }
    return out.token != 0 && !out.cands.empty();
}

std::string EncodeConnectionString(const Offer& offer) {
    std::vector<uint8_t> b = PackOffer(offer);
    return "MK64-" + Base64UrlEncode(b.data(), b.size());
}

bool DecodeConnectionString(const std::string& textIn, Offer& out) {
    std::string text;
    for (char c : textIn) {
        if (!isspace((unsigned char) c)) {
            text += c;
        }
    }
    if (text.rfind("MK64-", 0) != 0) {
        return false;
    }
    std::vector<uint8_t> b;
    return Base64UrlDecode(text.substr(5), b) && UnpackOffer(b.data(), b.size(), out);
}

std::string NewRoomCode() {
    uint64_t r = RandomU64();
    std::string code;
    for (int i = 0; i < 8; i++) {
        code += kCrockford[r & 31];
        r >>= 5;
    }
    return code;
}

std::string NormalizeRoomCode(const std::string& text) {
    std::string out;
    for (char c : text) {
        if (c == '-' || isspace((unsigned char) c)) {
            continue;
        }
        c = (char) toupper((unsigned char) c);
        if (c == 'O') {
            c = '0';
        } else if (c == 'I' || c == 'L') {
            c = '1';
        }
        if (strchr(kCrockford, c) == nullptr || c == '\0') {
            return "";
        }
        out += c;
    }
    return out.size() == 8 ? out : "";
}

std::string PrettyRoomCode(const std::string& n) {
    return n.size() == 8 ? n.substr(0, 4) + "-" + n.substr(4) : n;
}

bool LooksLikeRoomCode(const std::string& text) {
    return !NormalizeRoomCode(text).empty();
}

namespace rendezvous {

bool Publish(const std::string& room, const Offer& offer, std::string* error) {
    std::vector<uint8_t> blob = PackOffer(offer);
    Scramble(blob, room);
    std::string msg = std::string(offer.fromHost ? "H" : "J") + Base64UrlEncode(blob.data(), blob.size());
    std::string cmd = std::string(kCurl) + " -fsS -m 8 -H \"Cache: yes\" -d \"" + msg + "\" " + kRendezvousBase +
                      TopicFor(room) + kQuiet;
    std::string out;
    int rc = RunCommand(cmd, out);
    if (rc != 0) {
        if (error) {
            *error = "Could not reach ntfy.sh (is curl installed and the internet reachable?)";
        }
        return false;
    }
    return true;
}

bool Fetch(const std::string& room, bool wantHost, uint32_t forToken, Offer& out, std::string* error) {
    std::string cmd =
        std::string(kCurl) + " -fsS -m 8 \"" + kRendezvousBase + TopicFor(room) + "/json?poll=1&since=all\"" + kQuiet;
    std::string body;
    if (RunCommand(cmd, body) != 0) {
        if (error) {
            *error = "Could not reach ntfy.sh (is curl installed and the internet reachable?)";
        }
        return false;
    }
    bool found = false;
    long long bestTime = -1;
    size_t start = 0;
    while (start < body.size()) {
        size_t end = body.find('\n', start);
        if (end == std::string::npos) {
            end = body.size();
        }
        std::string line = body.substr(start, end - start);
        start = end + 1;
        if (JsonString(line, "event") != "message") {
            continue;
        }
        std::string msg = JsonString(line, "message");
        if (msg.size() < 2 || msg[0] != (wantHost ? 'H' : 'J')) {
            continue;
        }
        std::vector<uint8_t> blob;
        if (!Base64UrlDecode(msg.substr(1), blob)) {
            continue;
        }
        Scramble(blob, room);
        Offer o;
        if (!UnpackOffer(blob.data(), blob.size(), o) || o.fromHost != wantHost) {
            continue;
        }
        if (forToken != 0 && o.peerToken != forToken) {
            continue;
        }
        long long t = JsonNumber(line, "time");
        if (t >= bestTime) {
            bestTime = t;
            out = o;
            found = true;
        }
    }
    return found;
}

} // namespace rendezvous

} // namespace netplay
