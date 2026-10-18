#include "Netplay.h"
#include "Stun.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace netplay {

namespace {

constexpr uint8_t kMagic[4] = { 'M', 'K', 'N', 'P' };

enum PacketType : uint8_t {
    PKT_PROBE = 1,
    PKT_PROBE_ACK = 2,
    PKT_HELLO = 3,
    PKT_WELCOME = 4,
    PKT_READY = 5,
    PKT_INPUT = 6,
    PKT_BYE = 7,
};

enum WelcomeResult : uint8_t {
    WELCOME_OK = 0,
    WELCOME_BAD_PROTOCOL = 1,
    WELCOME_BAD_BUILD = 2,
    WELCOME_BAD_CONTENT = 3,
    WELCOME_FULL = 4,
};

const char* WelcomeText(uint8_t r) {
    switch (r) {
        case WELCOME_OK:
            return "ok";
        case WELCOME_BAD_PROTOCOL:
        case WELCOME_BAD_BUILD:
            return "You and the host are running different versions of the game. Use the exact same build.";
        case WELCOME_BAD_CONTENT:
            return "Your game data (mk64.o2r) differs from the host's. Both players need the US ROM.";
        case WELCOME_FULL:
            return "That game already has two players.";
        default:
            return "The host refused the connection.";
    }
}

struct Writer {
    uint8_t buf[1400];
    size_t len = 0;

    void U8(uint8_t v) {
        if (len < sizeof(buf)) {
            buf[len++] = v;
        }
    }
    void U16(uint16_t v) {
        U8((uint8_t) v);
        U8((uint8_t) (v >> 8));
    }
    void U32(uint32_t v) {
        for (int i = 0; i < 4; i++) {
            U8((uint8_t) (v >> (8 * i)));
        }
    }
    void U64(uint64_t v) {
        for (int i = 0; i < 8; i++) {
            U8((uint8_t) (v >> (8 * i)));
        }
    }
    void Bytes(const void* p, size_t n) {
        n = std::min(n, sizeof(buf) - len);
        memcpy(buf + len, p, n);
        len += n;
    }
    void Str(const std::string& s) {
        size_t n = std::min<size_t>(s.size(), 32);
        U8((uint8_t) n);
        Bytes(s.data(), n);
    }
};

struct Reader {
    const uint8_t* p;
    size_t len;
    size_t pos = 0;
    bool ok = true;

    Reader(const uint8_t* data, size_t n) : p(data), len(n) {
    }
    uint8_t U8() {
        if (pos + 1 > len) {
            ok = false;
            return 0;
        }
        return p[pos++];
    }
    uint16_t U16() {
        uint16_t lo = U8();
        return (uint16_t) (lo | (U8() << 8));
    }
    uint32_t U32() {
        uint32_t v = 0;
        for (int i = 0; i < 4; i++) {
            v |= (uint32_t) U8() << (8 * i);
        }
        return v;
    }
    uint64_t U64() {
        uint64_t v = 0;
        for (int i = 0; i < 8; i++) {
            v |= (uint64_t) U8() << (8 * i);
        }
        return v;
    }
    std::string Str() {
        size_t n = U8();
        if (pos + n > len) {
            ok = false;
            return "";
        }
        std::string s((const char*) p + pos, n);
        pos += n;
        return s;
    }
    std::vector<uint8_t> Blob(size_t n) {
        if (pos + n > len) {
            ok = false;
            return {};
        }
        std::vector<uint8_t> v(p + pos, p + pos + n);
        pos += n;
        return v;
    }
};

void Log(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fprintf(stderr, "[netplay] %s\n", buf);
    fflush(stderr);
}

uint32_t Now32() {
    return (uint32_t) NowMs();
}

} // namespace

const char* PhaseName(Phase p) {
    switch (p) {
        case Phase::Idle:
            return "idle";
        case Phase::Gathering:
            return "gathering";
        case Phase::Waiting:
            return "waiting";
        case Phase::Punching:
            return "connecting";
        case Phase::Handshake:
            return "handshake";
        case Phase::Ready:
            return "ready";
        case Phase::Running:
            return "running";
        case Phase::Failed:
            return "failed";
        case Phase::Ended:
            return "ended";
    }
    return "?";
}

Session& GetSession() {
    static Session session;
    return session;
}

Session::~Session() {
    mCancel = true;
    if (mThread.joinable()) {
        mThread.join();
    }
}

bool Session::Start(const Config& cfg) {
    if (IsLive()) {
        return false;
    }
    if (mThread.joinable()) {
        mThread.join();
    }
    mCfg = cfg;
    mCancel = false;
    mToken = 0;
    while (mToken == 0) {
        mToken = (uint32_t) RandomU64();
    }
    mPeerToken = 0;
    mLocalCands.clear();
    mRemoteCands.clear();
    mHaveRemoteOffer = false;
    mRoom.clear();
    mPeer = NetAddr();
    mPeerConfirmed = false;
    mBestRtt = -1;
    mInboundSeen.clear();
    mGotHello = mGotWelcome = mGotReady = false;
    mDelayFinal = false;
    mWelcomeResult = 0xff;
    mPeerName.clear();
    mSharedBlob = cfg.role == Role::Host ? cfg.hostBlob : std::vector<uint8_t>();
    mLocalSlot = cfg.role == Role::Host ? 0 : 1;
    mInputDelay = 2;
    mRttSamples = 0;
    mRttAvg = 0;
    for (auto& slots : mInputs) {
        for (InputSlot& s : slots) {
            s = InputSlot();
        }
    }
    for (int i = 0; i < kHistory; i++) {
        mLocalHashes[i] = HashSlot();
        mPeerHashes[i] = HashSlot();
    }
    mPeerContiguous = 0;
    mPeerAckOfUs = 0;
    mHighestScheduled = 0;
    mLastHashFrame = UINT32_MAX;
    mLastSend = mLastRecv = 0;
    mDead = false;
    mStallCount = mStallMs = mLongestStallMs = 0;
    mRelay = NetAddr();
    mRelayOn = false;
    mRelayLastReg = 0;
    mDelayed.clear();
    {
        std::lock_guard<std::mutex> lock(mStatusMutex);
        mStatus = Status();
        mStatus.localSlot = mLocalSlot;
    }
    mPhase = Phase::Gathering;
    mThread = std::thread(&Session::ConnectionThread, this);
    return true;
}

void Session::Cancel() {
    mCancel = true;
    if (mThread.joinable()) {
        mThread.join();
    }
    if (IsActive()) {
        SendBye();
    }
    mSock.Close();
    mPhase = Phase::Ended;
}

void Session::Shutdown() {
    Cancel();
}

bool Session::SupplyPeerConnectionString(const std::string& text) {
    Offer o;
    if (!DecodeConnectionString(text, o)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mPeerStringMutex);
    mPendingPeerString = text;
    return true;
}

Status Session::GetStatus() {
    std::lock_guard<std::mutex> lock(mStatusMutex);
    Status s = mStatus;
    s.phase = mPhase.load();
    return s;
}

void Session::SetPhase(Phase p, const std::string& msg) {
    mPhase = p;
    std::lock_guard<std::mutex> lock(mStatusMutex);
    mStatus.message = msg;
    Log("%s: %s", PhaseName(p), msg.c_str());
}

void Session::Fail(const std::string& err) {
    {
        std::lock_guard<std::mutex> lock(mStatusMutex);
        mStatus.error = err;
    }
    SetPhase(Phase::Failed, err);
}

// ---------------------------------------------------------------------------------------------------------------
// Connection

void Session::ConnectionThread() {
    SetPhase(Phase::Gathering, "Finding your network addresses...");
    uint16_t port = mCfg.port;
    if (port == 0 && mCfg.role == Role::Host && (mCfg.method == JoinMethod::Direct || mCfg.directHost)) {
        port = kDefaultPort;
    }
    bool opened;
    {
        std::lock_guard<std::mutex> lock(mSockMutex);
        opened = mSock.Open(port);
        if (!opened && port != 0) {
            // Usually another copy of the game still holds the port. Any port works for room codes; the lobby
            // shows the address with its port for direct joins.
            Log("port %u is busy, using another", (unsigned) port);
            opened = mSock.Open(0);
        }
    }
    if (!opened) {
        Fail("Could not open a network port. Check that nothing blocks the game from using the network.");
        return;
    }
    if (!GatherCandidates() || mCancel) {
        return;
    }
    if (!RunRendezvous() || mCancel) {
        return;
    }
    if (!Punch() || mCancel) {
        return;
    }
    if (!Handshake() || mCancel) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mStatusMutex);
        mStatus.inputDelay = mInputDelay;
        mStatus.peerName = mPeerName;
        mStatus.localSlot = mLocalSlot;
    }
    // Frames below the input delay have no scheduled input anywhere: they are neutral for everyone.
    for (int s = 0; s < kMaxPlayers; s++) {
        for (int f = 0; f < mInputDelay; f++) {
            mInputs[s][f].frame = (uint32_t) f;
            mInputs[s][f].pad = NetPad();
        }
    }
    mPeerContiguous = (uint32_t) mInputDelay;
    mLastRecv = NowMs();
    SetPhase(Phase::Ready, "Connected to " + (mPeerName.empty() ? std::string("your friend") : mPeerName) + " via " +
                               mPeer.ToString());
}

bool Session::GatherCandidates() {
    uint16_t port = mSock.LocalPort();
    for (const NetAddr& a : LocalAddresses(port)) {
        mLocalCands.push_back({ CandType::Host, a });
    }
    StunProbe probe;
    bool directJoin = mCfg.role == Role::Join && mCfg.method == JoinMethod::Direct;
    if (!directJoin) {
        std::lock_guard<std::mutex> lock(mSockMutex);
        probe.Start(mSock, DefaultStunServers());
    }
    while (!probe.Done() && !mCancel) {
        uint8_t buf[1500];
        NetAddr from;
        mSock.WaitReadable(20);
        for (;;) {
            int n;
            {
                std::lock_guard<std::mutex> lock(mSockMutex);
                n = mSock.RecvFrom(from, buf, sizeof(buf));
            }
            if (n <= 0) {
                break;
            }
            if (stun::IsStunPacket(buf, (size_t) n)) {
                probe.OnPacket(buf, (size_t) n);
            } else {
                HandlePacket(from, buf, (size_t) n); // an eager peer; keep its probe
            }
        }
        std::lock_guard<std::mutex> lock(mSockMutex);
        probe.Tick(mSock);
    }
    std::vector<NetAddr> v4Mapped;
    for (const NetAddr& a : probe.Results()) {
        bool dup = false;
        for (const Candidate& c : mLocalCands) {
            dup |= c.addr == a;
        }
        if (!dup) {
            mLocalCands.push_back({ CandType::Reflexive, a });
        }
        if (a.IsV4()) {
            v4Mapped.push_back(a);
        }
    }
    // Two STUN servers seeing two different ports means the router allocates a port per destination ("symmetric"
    // NAT): punching then only works if the other side's router is lenient.
    bool strict = false;
    for (size_t i = 1; i < v4Mapped.size(); i++) {
        strict |= v4Mapped[i].Port() != v4Mapped[0].Port();
    }
    if (!mCfg.relay.empty()) {
        NetAddr r;
        if (NetAddr::Parse(mCfg.relay, 46470, r)) {
            mLocalCands.push_back({ CandType::Relay, r });
            mRelay = r;
            mRelayOn = true;
        } else {
            Log("relay %s does not resolve", mCfg.relay.c_str());
        }
    }
    Offer mine;
    mine.token = mToken;
    mine.fromHost = mCfg.role == Role::Host;
    mine.cands = mLocalCands;
    {
        std::lock_guard<std::mutex> lock(mStatusMutex);
        mStatus.strictNat = strict;
        mStatus.connString = EncodeConnectionString(mine);
        mStatus.lanAddresses.clear();
        for (const Candidate& c : mLocalCands) {
            if (c.type == CandType::Host && c.addr.IsV4()) {
                mStatus.lanAddresses.push_back(c.addr.ToString());
            }
        }
        mStatus.publicAddress = v4Mapped.empty() ? "" : v4Mapped[0].ToString();
    }
    Log("candidates: %zu local+reflexive, strict NAT: %s", mLocalCands.size(), strict ? "yes" : "no");
    for (const Candidate& c : mLocalCands) {
        Log("  %s %s", c.type == CandType::Host ? "host" : "srflx", c.addr.ToString().c_str());
    }
    return true;
}

bool Session::RunRendezvous() {
    // Direct join: the target is an address; the host reveals its token in the probe answer.
    if (mCfg.role == Role::Join && mCfg.method == JoinMethod::Direct) {
        NetAddr target;
        if (!NetAddr::Parse(mCfg.target, kDefaultPort, target)) {
            Fail("Could not find the address \"" + mCfg.target + "\".");
            return false;
        }
        mRemoteCands.push_back({ CandType::Host, target });
        mHaveRemoteOffer = true;
        return true;
    }
    if (mCfg.role == Role::Join && mCfg.method == JoinMethod::ConnectionString) {
        Offer o;
        if (!DecodeConnectionString(mCfg.target, o) || !o.fromHost) {
            Fail("That connection text is not a valid host code.");
            return false;
        }
        mPeerToken = o.token;
        mRemoteCands = o.cands;
        mHaveRemoteOffer = true;
        UseRelayFrom(o.cands);
        return true;
    }
    if (mCfg.role == Role::Join) {
        mRoom = NormalizeRoomCode(mCfg.target);
        if (mRoom.empty()) {
            Fail("\"" + mCfg.target + "\" is not a room code (8 letters and digits, like K7QM-2XRD).");
            return false;
        }
        SetPhase(Phase::Waiting, "Looking up room " + PrettyRoomCode(mRoom) + "...");
        Offer host;
        std::string err;
        uint64_t start = NowMs();
        bool found = false;
        while (!mCancel && NowMs() - start < 45000) {
            if (rendezvous::Fetch(mRoom, true, 0, host, &err)) {
                found = true;
                break;
            }
            if (!err.empty()) {
                Fail(err + " Ask your friend for a connection code instead (Online Play > Advanced).");
                return false;
            }
            SleepMs(1500);
        }
        if (!found) {
            if (!mCancel) {
                Fail("No game found for room " + PrettyRoomCode(mRoom) +
                     ". Check the code, and that your friend is hosting right now.");
            }
            return false;
        }
        mPeerToken = host.token;
        mRemoteCands = host.cands;
        mHaveRemoteOffer = true;
        UseRelayFrom(host.cands);
        Offer mine;
        mine.token = mToken;
        mine.peerToken = host.token;
        mine.fromHost = false;
        mine.cands = mLocalCands;
        // Publish in the background so punching starts at once.
        std::thread([room = mRoom, mine] {
            for (int i = 0; i < 3; i++) {
                if (rendezvous::Publish(room, mine, nullptr)) {
                    return;
                }
                SleepMs(1000);
            }
        }).detach();
        return true;
    }

    // Host: publish the room, then wait for the joiner's answer, a pasted answer string, or a direct probe.
    mRoom = NewRoomCode();
    {
        std::lock_guard<std::mutex> lock(mStatusMutex);
        mStatus.roomCode = PrettyRoomCode(mRoom);
    }
    Offer mine;
    mine.token = mToken;
    mine.fromHost = true;
    mine.cands = mLocalCands;
    std::atomic<bool> published{ false };
    std::atomic<bool> stopWorker{ false };
    std::mutex answerMutex;
    Offer answer;
    bool haveAnswer = false;
    std::string workerError;
    std::thread worker([&] {
        std::string err;
        for (int i = 0; i < 3 && !stopWorker; i++) {
            if (rendezvous::Publish(mRoom, mine, &err)) {
                published = true;
                break;
            }
            SleepMs(1500);
        }
        if (!published) {
            std::lock_guard<std::mutex> lock(answerMutex);
            workerError = err;
            return;
        }
        while (!stopWorker) {
            Offer o;
            if (rendezvous::Fetch(mRoom, false, mToken, o, nullptr)) {
                std::lock_guard<std::mutex> lock(answerMutex);
                answer = o;
                haveAnswer = true;
                return;
            }
            for (int i = 0; i < 40 && !stopWorker; i++) {
                SleepMs(100);
            }
        }
    });
    SetPhase(Phase::Waiting, "Hosting. Room code " + PrettyRoomCode(mRoom) + " - waiting for your friend to join.");
    bool ok = false;
    bool warned = false;
    while (!mCancel) {
        PumpSocket(20);
        {
            std::lock_guard<std::mutex> lock(answerMutex);
            if (haveAnswer && !mHaveRemoteOffer) {
                mPeerToken = answer.token;
                mRemoteCands = answer.cands;
                mHaveRemoteOffer = true;
                UseRelayFrom(answer.cands);
            }
            if (!workerError.empty() && !warned) {
                warned = true;
                std::lock_guard<std::mutex> slock(mStatusMutex);
                mStatus.error = "Room codes need ntfy.sh, which is unreachable (" + workerError +
                                "). Your friend can still join with your connection code or LAN address.";
            }
        }
        {
            std::lock_guard<std::mutex> lock(mPeerStringMutex);
            if (!mPendingPeerString.empty() && !mHaveRemoteOffer) {
                Offer o;
                if (DecodeConnectionString(mPendingPeerString, o) && !o.fromHost) {
                    mPeerToken = o.token;
                    mRemoteCands = o.cands;
                    mHaveRemoteOffer = true;
                    UseRelayFrom(o.cands);
                }
                mPendingPeerString.clear();
            }
        }
        // A direct or LAN join announces itself with a probe; HandlePacket records it as a remote candidate.
        if (mHaveRemoteOffer || mPeerConfirmed || !mRemoteCands.empty()) {
            ok = true;
            break;
        }
    }
    stopWorker = true;
    worker.join();
    return ok && !mCancel;
}

bool Session::Punch() {
    SetPhase(Phase::Punching, "Connecting...");
    uint64_t start = NowMs();
    mPunchStart = start;
    uint64_t lastRound = 0;
    int round = 0;
    while (!mCancel) {
        uint64_t now = NowMs();
        if (mPeerConfirmed && now - mConfirmedAt > 300) {
            break;
        }
        uint64_t interval = now - start < 3000 ? 40 : 150;
        if (now - lastRound >= interval) {
            lastRound = now;
            round++;
            std::vector<NetAddr> targets;
            for (const Candidate& c : mRemoteCands) {
                if (c.type == CandType::Relay || mCfg.testNoDirect) {
                    continue; // the relay is tried below, once direct paths have had their chance
                }
                targets.push_back(c.addr);
            }
            if (mRelayOn && now - start > 4000) {
                targets.push_back(mRelay);
                if (now - mRelayLastReg > 2000) {
                    RelayRegister();
                }
            }
            for (const NetAddr& a : mInboundSeen) {
                if (mCfg.testNoDirect && !(mRelayOn && a == mRelay)) {
                    continue;
                }
                if (std::find(targets.begin(), targets.end(), a) == targets.end()) {
                    targets.push_back(a);
                }
            }
            for (const NetAddr& t : targets) {
                if (!t.IsV4() && !mSock.HasV6()) {
                    continue;
                }
                SendProbe(t, false, (uint32_t) RandomU64(), Now32());
            }
        }
        if (now - start > 45000) {
            Fail("Could not reach your friend's game. Both of your networks may block direct connections: try "
                 "the same Wi-Fi, a phone hotspot, or a relay (see HOW-TO-PLAY).");
            return false;
        }
        PumpSocket(5);
    }
    if (mCancel) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mStatusMutex);
        mStatus.path = mPeer.ToString();
        mStatus.rttMs = mBestRtt;
    }
    Log("path confirmed: %s rtt %d ms after %d probe rounds", mPeer.ToString().c_str(), mBestRtt, round);
    return true;
}

int Session::AutoDelay() const {
    double rtt = mBestRtt >= 0 ? mBestRtt : 40;
    double oneWay = rtt / 2.0;
    int d = (int) std::ceil((oneWay + 8.0) / mCfg.frameMs);
    if (rtt > 12) {
        d += 1; // headroom for jitter once the traffic leaves the building
    }
    return std::clamp(d, 1, 8);
}

bool Session::Handshake() {
    SetPhase(Phase::Handshake, "Checking game versions...");
    uint64_t start = NowMs();
    uint64_t lastSent = 0;
    if (mCfg.role == Role::Join) {
        while (!mCancel && !mGotWelcome) {
            if (NowMs() - lastSent > 250) {
                SendHello();
                lastSent = NowMs();
            }
            PumpSocket(10);
            if (NowMs() - start > 15000) {
                Fail("The host stopped answering during the handshake.");
                return false;
            }
        }
        if (mCancel) {
            return false;
        }
        if (mWelcomeResult != WELCOME_OK) {
            Fail(WelcomeText(mWelcomeResult));
            return false;
        }
        for (int i = 0; i < 3; i++) {
            SendReady();
            PumpSocket(30);
        }
        return true;
    }
    // Host.
    mInputDelay = mCfg.inputDelay > 0 ? mCfg.inputDelay : AutoDelay();
    mDelayFinal = true;
    if (mGotHello && mWelcomeResult == WELCOME_OK) {
        SendWelcome(WELCOME_OK);
    }
    while (!mCancel && !mGotReady) {
        PumpSocket(10);
        if (mGotHello && mWelcomeResult != WELCOME_OK && mWelcomeResult != 0xff) {
            // Tell them why a few times, then give up.
            for (int i = 0; i < 3; i++) {
                SendWelcome(mWelcomeResult);
                SleepMs(50);
            }
            Fail(std::string("Your friend could not join: ") + WelcomeText(mWelcomeResult));
            return false;
        }
        if (NowMs() - start > 20000) {
            Fail("Your friend's game stopped answering during the handshake.");
            return false;
        }
    }
    return !mCancel;
}

// ---------------------------------------------------------------------------------------------------------------
// Packets

void Session::PumpSocket(int waitMs) {
    FlushDelayed();
    if (waitMs > 0) {
        mSock.WaitReadable(waitMs);
    }
    FlushDelayed();
    uint8_t buf[1500];
    for (int i = 0; i < 64; i++) {
        NetAddr from;
        int n;
        {
            std::lock_guard<std::mutex> lock(mSockMutex);
            n = mSock.RecvFrom(from, buf, sizeof(buf));
        }
        if (n <= 0) {
            break;
        }
        if (mRelayOn && from == mRelay && n >= 5 && memcmp(buf, "RLY1", 4) == 0) {
            if (buf[4] == 'd') {
                HandlePacket(mRelay, buf + 5, (size_t) n - 5); // the relay stands in for the peer's address
            }
            continue;
        }
        HandlePacket(from, buf, (size_t) n);
    }
    if (mRelayOn && mPeerConfirmed && mPeer == mRelay && NowMs() - mRelayLastReg > 10000) {
        RelayRegister(); // keepalive
    }
}

void Session::UseRelayFrom(const std::vector<Candidate>& cands) {
    if (mRelayOn) {
        return;
    }
    for (const Candidate& c : cands) {
        if (c.type == CandType::Relay) {
            mRelay = c.addr;
            mRelayOn = true;
            Log("relay offered by the other player: %s", c.addr.ToString().c_str());
            return;
        }
    }
}

// Both players register under the host's token (a room id both know) and their own token.
void Session::RelayRegister() {
    uint32_t hostToken = mCfg.role == Role::Host ? mToken : mPeerToken;
    if (!mRelayOn || hostToken == 0) {
        return;
    }
    uint8_t pkt[17];
    memcpy(pkt, "RLY1R", 5);
    for (int i = 0; i < 4; i++) {
        pkt[5 + i] = (uint8_t) (hostToken >> (8 * i));
    }
    memcpy(pkt + 9, "MK64", 4);
    for (int i = 0; i < 4; i++) {
        pkt[13 + i] = (uint8_t) (mToken >> (8 * i));
    }
    std::lock_guard<std::mutex> lock(mSockMutex);
    mSock.SendTo(mRelay, pkt, sizeof(pkt));
    mRelayLastReg = NowMs();
}

void Session::SendProbe(const NetAddr& to, bool ack, uint32_t nonce, uint32_t echoTime) {
    Writer w;
    w.Bytes(kMagic, 4);
    w.U8(kProtocolVersion);
    w.U8(ack ? PKT_PROBE_ACK : PKT_PROBE);
    w.U32(mToken);
    w.U32(mPeerToken);
    w.U32(nonce);
    w.U32(echoTime);
    Send(to, w.buf, w.len);
}

void Session::SendHello() {
    Writer w;
    w.Bytes(kMagic, 4);
    w.U8(kProtocolVersion);
    w.U8(PKT_HELLO);
    w.U32(mToken);
    w.U64(mCfg.buildId);
    w.U64(mCfg.contentId);
    w.Str(mCfg.name);
    Send(mPeer, w.buf, w.len);
}

void Session::SendWelcome(uint8_t result) {
    Writer w;
    w.Bytes(kMagic, 4);
    w.U8(kProtocolVersion);
    w.U8(PKT_WELCOME);
    w.U32(mToken);
    w.U8(result);
    w.U8(1); // the joiner's slot
    w.U8((uint8_t) mInputDelay);
    w.Str(mCfg.name);
    w.U16((uint16_t) mSharedBlob.size());
    w.Bytes(mSharedBlob.data(), mSharedBlob.size());
    Send(mPeer, w.buf, w.len);
}

void Session::SendReady() {
    Writer w;
    w.Bytes(kMagic, 4);
    w.U8(kProtocolVersion);
    w.U8(PKT_READY);
    w.U32(mToken);
    Send(mPeer, w.buf, w.len);
}

void Session::SendBye() {
    Writer w;
    w.Bytes(kMagic, 4);
    w.U8(kProtocolVersion);
    w.U8(PKT_BYE);
    w.U32(mToken);
    for (int i = 0; i < 3; i++) {
        Send(mPeer, w.buf, w.len);
    }
    for (int i = 0; i < 20 && !mDelayed.empty(); i++) {
        SleepMs(10);
        FlushDelayed(); // goodbyes must get out before we exit
    }
}

void Session::SendInputs(uint32_t uptoFrame) {
    Writer w;
    w.Bytes(kMagic, 4);
    w.U8(kProtocolVersion);
    w.U8(PKT_INPUT);
    w.U32(mToken);
    w.U8((uint8_t) mLocalSlot);
    w.U32(mPeerContiguous);
    uint32_t first = std::max<uint32_t>(mPeerAckOfUs, (uint32_t) mInputDelay);
    if (uptoFrame + 1 > first + 48) {
        // Never skip ahead of what the peer still needs; the window slides as acknowledgements arrive.
        uptoFrame = first + 47;
    }
    uint8_t count = uptoFrame + 1 > first ? (uint8_t) (uptoFrame + 1 - first) : 0;
    w.U32(first);
    w.U8(count);
    for (uint32_t f = first; f < first + count; f++) {
        const InputSlot& s = mInputs[mLocalSlot][f % kHistory];
        NetPad p = s.frame == f ? s.pad : NetPad();
        w.U16(p.buttons);
        w.U8((uint8_t) p.stickX);
        w.U8((uint8_t) p.stickY);
    }
    // The newest few state hashes; the receiver compares whichever it also has.
    uint8_t nh = 0;
    uint32_t hashes[4][2];
    if (mLastHashFrame != UINT32_MAX) {
        for (uint32_t k = 0; k < 4 && k <= mLastHashFrame; k++) {
            uint32_t f = mLastHashFrame - k;
            const HashSlot& h = mLocalHashes[f % kHistory];
            if (h.frame == f) {
                hashes[nh][0] = f;
                hashes[nh][1] = h.hash;
                nh++;
            }
        }
    }
    w.U8(nh);
    for (int i = 0; i < nh; i++) {
        w.U32(hashes[i][0]);
        w.U32(hashes[i][1]);
    }
    Send(mPeer, w.buf, w.len);
    mLastSend = NowMs();
}

void Session::Send(const NetAddr& to, const void* dataIn, size_t lenIn) {
    const void* data = dataIn;
    size_t len = lenIn;
    uint8_t wrapped[1500];
    if (mRelayOn && to == mRelay && mPeerToken != 0 && lenIn + 21 <= sizeof(wrapped)) {
        uint32_t hostToken = mCfg.role == Role::Host ? mToken : mPeerToken;
        memcpy(wrapped, "RLY1D", 5);
        for (int i = 0; i < 4; i++) {
            wrapped[5 + i] = (uint8_t) (hostToken >> (8 * i));
            wrapped[13 + i] = (uint8_t) (mToken >> (8 * i));
            wrapped[17 + i] = (uint8_t) (mPeerToken >> (8 * i));
        }
        memcpy(wrapped + 9, "MK64", 4);
        memcpy(wrapped + 21, dataIn, lenIn);
        data = wrapped;
        len = lenIn + 21;
    }
    if (mCfg.simLossPct > 0 && (int) (RandomU64() % 100) < mCfg.simLossPct) {
        return;
    }
    if (mCfg.simLatencyMs > 0 || mCfg.simJitterMs > 0) {
        int jitter = mCfg.simJitterMs > 0 ? (int) (RandomU64() % (uint64_t) (2 * mCfg.simJitterMs + 1)) - mCfg.simJitterMs : 0;
        Delayed d;
        d.due = NowMs() + (uint64_t) std::max(0, mCfg.simLatencyMs + jitter);
        d.to = to;
        d.data.assign((const uint8_t*) data, (const uint8_t*) data + len);
        std::lock_guard<std::mutex> lock(mSockMutex);
        mDelayed.push_back(std::move(d));
        return;
    }
    std::lock_guard<std::mutex> lock(mSockMutex);
    mSock.SendTo(to, data, len);
}

void Session::FlushDelayed() {
    if (mDelayed.empty()) {
        return;
    }
    uint64_t now = NowMs();
    std::lock_guard<std::mutex> lock(mSockMutex);
    for (size_t i = 0; i < mDelayed.size();) {
        if (mDelayed[i].due <= now) {
            mSock.SendTo(mDelayed[i].to, mDelayed[i].data.data(), mDelayed[i].data.size());
            mDelayed.erase(mDelayed.begin() + (long) i);
        } else {
            i++;
        }
    }
}

void Session::HandlePacket(const NetAddr& from, const uint8_t* data, size_t len) {
    if (len < 10 || memcmp(data, kMagic, 4) != 0 || data[4] != kProtocolVersion) {
        return;
    }
    Reader r(data, len);
    r.pos = 5;
    uint8_t type = r.U8();
    uint32_t sender = r.U32();
    if (!r.ok || sender == 0 || sender == mToken) {
        return;
    }
    bool knownPeer = mPeerToken != 0 && sender == mPeerToken;

    if (type == PKT_PROBE || type == PKT_PROBE_ACK) {
        if (mCfg.testNoDirect && !(mRelayOn && from == mRelay)) {
            return;
        }
        uint32_t receiver = r.U32();
        uint32_t nonce = r.U32();
        uint32_t t = r.U32();
        if (!r.ok) {
            return;
        }
        // Accept probes addressed to us, or token-less probes when hosting (direct / LAN joins).
        bool forUs = receiver == mToken || (receiver == 0 && mCfg.role == Role::Host);
        if (!forUs) {
            return;
        }
        if (mPeerToken == 0 && !IsActive()) {
            mPeerToken = sender;
            knownPeer = true;
        }
        if (!knownPeer) {
            return; // someone else; this game already has its two players
        }
        if (type == PKT_PROBE) {
            if (std::find(mInboundSeen.begin(), mInboundSeen.end(), from) == mInboundSeen.end()) {
                mInboundSeen.push_back(from);
                if (mRemoteCands.empty()) {
                    mRemoteCands.push_back({ CandType::Host, from });
                }
            }
            SendProbe(from, true, nonce, t);
        } else {
            int rtt = (int) (Now32() - t);
            if (rtt < 0 || rtt > 10000) {
                rtt = 0;
            }
            bool viaRelay = mRelayOn && from == mRelay;
            bool currentRelay = mRelayOn && mPeerConfirmed && mPeer == mRelay;
            bool better = !mPeerConfirmed || (currentRelay && !viaRelay) ||
                          (!viaRelay && (rtt + 5 < mBestRtt || (from.IsPrivate() && !mPeer.IsPrivate() && rtt <= mBestRtt + 20)));
            if (better && !IsActive()) {
                mPeer = from;
                mBestRtt = rtt;
                if (!mPeerConfirmed) {
                    mConfirmedAt = NowMs();
                }
                mPeerConfirmed = true;
            }
        }
        mLastRecv = NowMs();
        return;
    }

    if (!knownPeer) {
        return;
    }
    // Follow the peer if its address changes (NAT rebinding) once a game packet proves it is them.
    if (mPeerConfirmed && from != mPeer && (type == PKT_INPUT || type == PKT_HELLO) && !(mRelayOn && from == mRelay)) {
        Log("peer moved from %s to %s", mPeer.ToString().c_str(), from.ToString().c_str());
        mPeer = from;
    }
    mLastRecv = NowMs();

    switch (type) {
        case PKT_HELLO: {
            uint64_t build = r.U64();
            uint64_t content = r.U64();
            std::string name = r.Str();
            if (!r.ok || mCfg.role != Role::Host) {
                return;
            }
            mGotHello = true;
            mPeerName = name;
            if (build != mCfg.buildId) {
                mWelcomeResult = WELCOME_BAD_BUILD;
            } else if (content != mCfg.contentId) {
                mWelcomeResult = WELCOME_BAD_CONTENT;
            } else {
                mWelcomeResult = WELCOME_OK;
                if (mDelayFinal) {
                    SendWelcome(WELCOME_OK);
                }
            }
            break;
        }
        case PKT_WELCOME: {
            uint8_t result = r.U8();
            uint8_t slot = r.U8();
            uint8_t delay = r.U8();
            std::string name = r.Str();
            uint16_t blobLen = r.U16();
            std::vector<uint8_t> blob = r.Blob(blobLen);
            if (!r.ok || mCfg.role != Role::Join) {
                return;
            }
            if (mGotWelcome) {
                SendReady(); // our READY was lost
                return;
            }
            mGotWelcome = true;
            mWelcomeResult = result;
            mLocalSlot = slot < kMaxPlayers ? slot : 1;
            mInputDelay = std::clamp<int>(delay, 1, 8);
            mPeerName = name;
            mSharedBlob = blob;
            break;
        }
        case PKT_READY:
            mGotReady = true;
            break;
        case PKT_INPUT: {
            if (mCfg.role == Role::Host) {
                mGotReady = true;
            }
            uint8_t slot = r.U8();
            uint32_t ack = r.U32();
            uint32_t first = r.U32();
            uint8_t count = r.U8();
            if (!r.ok || slot >= kMaxPlayers || slot == mLocalSlot) {
                return;
            }
            if (ack > mPeerAckOfUs) {
                mPeerAckOfUs = ack;
            }
            for (uint32_t f = first; f < first + count; f++) {
                NetPad p;
                p.buttons = r.U16();
                p.stickX = (int8_t) r.U8();
                p.stickY = (int8_t) r.U8();
                if (!r.ok) {
                    return;
                }
                if (f + kHistory / 2 < mPeerContiguous || f >= mPeerContiguous + kHistory / 2) {
                    continue; // ancient retransmit, or nonsense
                }
                InputSlot& s = mInputs[slot][f % kHistory];
                if (s.frame != f) {
                    s.frame = f;
                    s.pad = p;
                }
            }
            while (mInputs[slot][mPeerContiguous % kHistory].frame == mPeerContiguous) {
                mPeerContiguous++;
            }
            uint8_t nh = r.U8();
            for (int i = 0; i < nh && r.ok; i++) {
                uint32_t f = r.U32();
                uint32_t h = r.U32();
                if (!r.ok) {
                    break;
                }
                mPeerHashes[f % kHistory] = { f, h };
                const HashSlot& mine = mLocalHashes[f % kHistory];
                if (mine.frame == f && mine.hash != h) {
                    std::lock_guard<std::mutex> lock(mStatusMutex);
                    if (!mStatus.desync) {
                        mStatus.desync = true;
                        mStatus.desyncFrame = f;
                        Log("DESYNC at frame %u: local %08x remote %08x", f, mine.hash, h);
                    }
                }
            }
            break;
        }
        case PKT_BYE:
            if (IsActive()) {
                mDead = true;
                std::lock_guard<std::mutex> lock(mStatusMutex);
                mStatus.error = (mPeerName.empty() ? std::string("Your friend") : mPeerName) + " left the game.";
            }
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Lockstep

bool Session::GetFrameInputs(uint32_t frame, const NetPad& local, NetPad out[kMaxPlayers]) {
    if (mThread.joinable()) {
        mThread.join();
    }
    int remote = 1 - mLocalSlot;
    if (!IsActive() || mDead) {
        for (int i = 0; i < kMaxPlayers; i++) {
            out[i] = NetPad();
        }
        out[mLocalSlot] = local;
        return false;
    }
    mPhase = Phase::Running;
    uint32_t target = frame + (uint32_t) mInputDelay;
    InputSlot& mine = mInputs[mLocalSlot][target % kHistory];
    if (mine.frame != target) {
        mine.frame = target;
        mine.pad = local;
    }
    if (target > mHighestScheduled) {
        mHighestScheduled = target;
    }
    SendInputs(mHighestScheduled);

    uint64_t waitStart = NowMs();
    uint64_t lastHook = waitStart;
    bool flagged = false;
    bool waited = false;
    while (mInputs[remote][frame % kHistory].frame != frame) {
        waited = true;
        PumpSocket(1);
        uint64_t now = NowMs();
        if (mWaitHook && now - lastHook >= 20) {
            lastHook = now;
            if (!mWaitHook()) {
                mDead = true;
                break;
            }
        }
        if (now - mLastSend >= 16) {
            SendInputs(mHighestScheduled);
        }
        if (now - waitStart > 200 && !flagged) {
            flagged = true;
            std::lock_guard<std::mutex> lock(mStatusMutex);
            mStatus.waiting = true;
        }
        if (flagged) {
            std::lock_guard<std::mutex> lock(mStatusMutex);
            mStatus.waitingMs = now - waitStart;
        }
        if (mDead || now - mLastRecv > 20000) {
            mDead = true;
            std::lock_guard<std::mutex> lock(mStatusMutex);
            if (mStatus.error.empty()) {
                mStatus.error = "Lost the connection to your friend.";
            }
            mStatus.waiting = false;
            break;
        }
    }
    if (waited) {
        uint64_t stalled = NowMs() - waitStart;
        mStallCount++;
        mStallMs += stalled;
        mLongestStallMs = std::max(mLongestStallMs, stalled);
        std::lock_guard<std::mutex> lock(mStatusMutex);
        mStatus.stallCount = mStallCount;
        mStatus.stallMs = mStallMs;
        mStatus.longestStallMs = mLongestStallMs;
    }
    if (flagged) {
        std::lock_guard<std::mutex> lock(mStatusMutex);
        mStatus.waiting = false;
        mStatus.waitingMs = 0;
    }
    for (int i = 0; i < kMaxPlayers; i++) {
        const InputSlot& s = mInputs[i][frame % kHistory];
        out[i] = s.frame == frame ? s.pad : NetPad();
    }
    {
        std::lock_guard<std::mutex> lock(mStatusMutex);
        mStatus.frame = frame;
    }
    return !mDead;
}

void Session::ReportHash(uint32_t frame, uint32_t hash) {
    if (!IsActive()) {
        return;
    }
    mLocalHashes[frame % kHistory] = { frame, hash };
    mLastHashFrame = frame;
    const HashSlot& theirs = mPeerHashes[frame % kHistory];
    if (theirs.frame == frame && theirs.hash != hash) {
        std::lock_guard<std::mutex> lock(mStatusMutex);
        if (!mStatus.desync) {
            mStatus.desync = true;
            mStatus.desyncFrame = frame;
            Log("DESYNC at frame %u: local %08x remote %08x", frame, hash, theirs.hash);
        }
    }
}

} // namespace netplay
