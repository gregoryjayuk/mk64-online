#pragma once

// Lockstep online play. Both machines run the whole game; only controller inputs travel, delayed by a few frames so
// they arrive before they are needed. A per-frame hash of the gameplay state is exchanged to catch any divergence.
//
// Lifecycle: Start() (connection thread: candidate gathering, rendezvous, hole punching, handshake) -> Ready ->
// the game thread calls GetFrameInputs() once per logic frame and ReportHash() after it -> Shutdown().

#include "NetSocket.h"
#include "Signal.h"

#include <atomic>
#include <functional>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace netplay {

constexpr uint8_t kProtocolVersion = 1;
constexpr uint16_t kDefaultPort = 46464;
constexpr int kMaxPlayers = 2;

struct NetPad {
    uint16_t buttons = 0;
    int8_t stickX = 0;
    int8_t stickY = 0;

    bool operator==(const NetPad& o) const {
        return buttons == o.buttons && stickX == o.stickX && stickY == o.stickY;
    }
};

enum class Role { None, Host, Join };
enum class JoinMethod { RoomCode, ConnectionString, Direct };

enum class Phase { Idle, Gathering, Waiting, Punching, Handshake, Ready, Running, Failed, Ended };

struct Config {
    Role role = Role::None;
    JoinMethod method = JoinMethod::RoomCode;
    std::string target;   // joiner: room code, "MK64-..." string or host[:port]
    uint16_t port = 0;    // 0: ephemeral for room codes, kDefaultPort for direct hosting
    std::string name;
    int inputDelay = -1;  // frames; -1 picks from the measured round trip
    uint64_t buildId = 0;
    uint64_t contentId = 0;
    std::vector<uint8_t> hostBlob; // host only: settings and save data every machine must share
    bool directHost = false;       // host also accepts direct IP joins (LAN / port forward)
    double frameMs = 1000.0 / 30.0;
    std::string relay; // optional "host:port" of an mk64-relay, used when no direct path answers
    bool testNoDirect = false; // test aid: pretend every direct path is blocked
    // Test aids: simulate an internet path on outgoing packets.
    int simLossPct = 0;
    int simLatencyMs = 0;
    int simJitterMs = 0;
};

struct Status {
    Phase phase = Phase::Idle;
    std::string message;
    std::string error;
    std::string roomCode;       // pretty, host only
    std::string connString;     // our candidate list as copy-paste text
    std::vector<std::string> lanAddresses;
    std::string publicAddress;
    bool strictNat = false;
    std::string peerName;
    std::string path;           // the address pair actually in use
    int rttMs = -1;
    int inputDelay = 0;
    int localSlot = 0;
    bool waiting = false;       // game thread is stalled on the other player
    uint64_t waitingMs = 0;
    bool desync = false;
    uint32_t desyncFrame = 0;
    uint32_t frame = 0;
    uint64_t stallCount = 0;   // frames that had to wait for the other player
    uint64_t stallMs = 0;      // total time spent waiting
    uint64_t longestStallMs = 0;
};

class Session {
  public:
    ~Session();

    bool Start(const Config& cfg);
    void Cancel();
    // Joiner using connection strings, or host answering one: hand over the other side's text.
    bool SupplyPeerConnectionString(const std::string& text);

    Status GetStatus();
    bool IsReady() const {
        return mPhase.load() == Phase::Ready || mPhase.load() == Phase::Running;
    }
    bool IsActive() const {
        Phase p = mPhase.load();
        return p == Phase::Ready || p == Phase::Running;
    }
    bool IsLive() const {
        Phase p = mPhase.load();
        return p != Phase::Idle && p != Phase::Failed && p != Phase::Ended;
    }

    int LocalSlot() const {
        return mLocalSlot;
    }
    int NumPlayers() const {
        return 2;
    }
    const std::vector<uint8_t>& SharedBlob() const {
        return mSharedBlob;
    }

    // Game thread, once per logic frame, frames counting up from 0 with no gaps. Blocks until every player's input
    // for `frame` is known. Returns false once the session has died (out[] then holds neutral pads for remote slots).
    bool GetFrameInputs(uint32_t frame, const NetPad& local, NetPad out[kMaxPlayers]);
    // Called every few milliseconds while GetFrameInputs waits (keeps the window alive). Return false to give up.
    void SetWaitHook(std::function<bool()> hook) {
        mWaitHook = std::move(hook);
    }
    void ReportHash(uint32_t frame, uint32_t hash);
    void Shutdown();

  private:
    struct InputSlot {
        uint32_t frame = UINT32_MAX;
        NetPad pad;
    };
    struct HashSlot {
        uint32_t frame = UINT32_MAX;
        uint32_t hash = 0;
    };
    static constexpr int kHistory = 1024;

    void ConnectionThread();
    bool GatherCandidates();
    bool RunRendezvous();
    bool Punch();
    bool Handshake();
    void SetPhase(Phase p, const std::string& msg);
    void Fail(const std::string& err);

    // Packet handling shared by the connection thread and the game thread.
    void PumpSocket(int waitMs);
    void HandlePacket(const NetAddr& from, const uint8_t* data, size_t len);
    void SendProbe(const NetAddr& to, bool ack, uint32_t nonce, uint32_t echoTime);
    void SendHello();
    void SendWelcome(uint8_t result);
    void SendReady();
    void SendInputs(uint32_t uptoFrame);
    void SendBye();
    void Send(const NetAddr& to, const void* data, size_t len);
    void FlushDelayed();
    void RelayRegister();
    void UseRelayFrom(const std::vector<Candidate>& cands);
    int AutoDelay() const;

    Config mCfg;
    std::thread mThread;
    std::atomic<bool> mCancel{ false };
    std::atomic<Phase> mPhase{ Phase::Idle };
    std::mutex mStatusMutex;
    Status mStatus;

    UdpSocket mSock;
    std::mutex mSockMutex;
    struct Delayed {
        uint64_t due;
        NetAddr to;
        std::vector<uint8_t> data;
    };
    std::vector<Delayed> mDelayed;
    NetAddr mRelay;
    bool mRelayOn = false;
    uint64_t mRelayLastReg = 0;
    uint64_t mPunchStart = 0;
    uint32_t mToken = 0;
    uint32_t mPeerToken = 0;
    std::vector<Candidate> mLocalCands;
    std::vector<Candidate> mRemoteCands;
    bool mHaveRemoteOffer = false;
    std::string mRoom;
    std::string mPendingPeerString;
    std::mutex mPeerStringMutex;

    // Hole punching.
    NetAddr mPeer;
    bool mPeerConfirmed = false;
    uint64_t mConfirmedAt = 0;
    int mBestRtt = -1;
    std::vector<NetAddr> mInboundSeen;

    // Handshake.
    bool mGotHello = false;
    bool mGotWelcome = false;
    bool mGotReady = false;
    bool mDelayFinal = false;
    uint8_t mWelcomeResult = 0xff;
    std::string mPeerName;
    std::vector<uint8_t> mSharedBlob;
    int mInputDelay = 2;
    int mLocalSlot = 0;
    int mRttSamples = 0;
    double mRttAvg = 0;

    // Lockstep.
    InputSlot mInputs[kMaxPlayers][kHistory];
    HashSlot mLocalHashes[kHistory];
    HashSlot mPeerHashes[kHistory];
    uint32_t mPeerContiguous = 0;  // every remote input below this frame is known
    uint32_t mPeerAckOfUs = 0;     // the peer holds all our inputs below this frame
    uint32_t mHighestScheduled = 0;
    uint32_t mLastHashFrame = UINT32_MAX;
    uint64_t mLastSend = 0;
    uint64_t mLastRecv = 0;
    bool mDead = false;
    std::function<bool()> mWaitHook;
    uint64_t mStallCount = 0;
    uint64_t mStallMs = 0;
    uint64_t mLongestStallMs = 0;
};

Session& GetSession();

const char* PhaseName(Phase p);

} // namespace netplay
