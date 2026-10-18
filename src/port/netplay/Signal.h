#pragma once

// How two players find each other before any game packet flows.
//  - Room codes: the host gets a short code ("K7QM-2XRD") and reads it out; the joiner types it in. Both sides then
//    swap their candidate addresses through a public ntfy.sh topic derived from the code (HTTPS, via the curl CLI,
//    lightly obfuscated so the topic does not show raw IPs), and punch UDP holes towards each other.
//  - Connection strings: the same candidate list as copy-paste text, for when ntfy.sh is unreachable.
//  - Direct: "192.168.1.20" or "host:port" for a LAN or a port-forwarded host.

#include "NetSocket.h"

#include <cstdint>
#include <string>
#include <vector>

namespace netplay {

enum class CandType : uint8_t { Host = 0, Reflexive = 1, Relay = 2 };

struct Candidate {
    CandType type = CandType::Host;
    NetAddr addr;
};

struct Offer {
    uint32_t token = 0;     // random per session; every probe carries it
    uint32_t peerToken = 0; // joiner only: the host token it answers
    bool fromHost = false;
    std::vector<Candidate> cands;
};

std::vector<uint8_t> PackOffer(const Offer& offer);
bool UnpackOffer(const uint8_t* data, size_t len, Offer& out);

std::string EncodeConnectionString(const Offer& offer); // "MK64-...."
bool DecodeConnectionString(const std::string& text, Offer& out);

std::string NewRoomCode();                                // "K7QM2XRD"
std::string NormalizeRoomCode(const std::string& text);   // "" if not a valid code
std::string PrettyRoomCode(const std::string& normalized); // "K7QM-2XRD"
bool LooksLikeRoomCode(const std::string& text);

// Blocking calls (seconds); run them off the game thread.
namespace rendezvous {
bool Publish(const std::string& room, const Offer& offer, std::string* error);
// Newest offer on the room's topic sent by the other role, optionally only one answering `forToken`.
bool Fetch(const std::string& room, bool wantHost, uint32_t forToken, Offer& out, std::string* error);
} // namespace rendezvous

std::string Base64UrlEncode(const uint8_t* data, size_t len);
bool Base64UrlDecode(const std::string& text, std::vector<uint8_t>& out);
uint64_t Fnv1a64(const std::string& s);

} // namespace netplay
