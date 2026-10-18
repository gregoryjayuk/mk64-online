#include "NetplayGame.h"
#include "Netplay.h"
#include "NetplayUI.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unistd.h>

#include <fast/Fast3dWindow.h>
#include <fast/interpreter.h>
#include <libultraship/bridge/consolevariablebridge.h>
#include <SDL2/SDL.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <objc/message.h>
#include <objc/runtime.h>
#endif

extern "C" {
#include "main.h"
#include "menus.h"
#include "buffers/random.h"
#include "code_800029B0.h"
#include "defines.h"
#include "waypoints.h"
#include "racing/math_util.h"

extern u16 gPathIndexByPlayerId[];
extern u16 gPathCountByPathIndex[];
extern u16 gNearestPathPointByPlayerId[];
extern s16 gLapCountByPlayerId[];

struct Actor* CM_GetActor(size_t index);
size_t CM_GetActorSize();

// libultraship/src/libultraship/libultra/os_eeprom.cpp
extern uint8_t* gNetplayEepromOverride;
extern bool gNetplayEepromWriteThrough;
}

#if __has_include("netplay_build_id.h")
#include "netplay_build_id.h"
#endif
#ifndef NETPLAY_BUILD_ID
#define NETPLAY_BUILD_ID "dev"
#endif

using namespace netplay;

namespace {

struct Options {
    bool session = false;
    Config cfg;
    bool bot = false;
    uint32_t botSeed = 1;
    uint32_t quitAfter = 0;
    std::string hashLog;
    std::vector<uint32_t> snaps; // test: frames whose picture is saved as BMP
    std::string snapPrefix;
    int botSteerSign = 1;
    int botMode = 0; // 0 Grand Prix, 1 VS, 2 Battle (the 2P mode menu's rows)
    bool botSkipLaps = false; // test: debug-mode lap skip (needs the host's --test-debug) to reach the finish fast
    bool testDebug = false;
    std::string testBuildId; // test aid: claim another build's id (e.g. a Linux build one commit behind)
};

Options sOpt;
uint32_t sFrame = 0;
bool sStarted = false;
std::FILE* sHashLog = nullptr;
uint8_t sEeprom[512];
bool sTestDebugRaces = false;

// Settings that change how the game plays. Every machine must use the host's values or the simulations drift
// apart. Both builds are identical (the build id is checked), so the list order doubles as the wire format.
enum class Kind : uint8_t { Int, Float };
struct SharedCVar {
    const char* name;
    Kind kind;
};
const SharedCVar kSharedCVars[] = {
    { "gHarderCPU", Kind::Int },
    { "gDisableRubberbanding", Kind::Int },
    { "gEnableCustomCC", Kind::Int },
    { "gCustomCC", Kind::Float },
    { "gNoWallColision", Kind::Int },
    { "gMinHeight", Kind::Float },
    { "gEnableMoonJump", Kind::Int },
    { "gShellsShootStraight", Kind::Int },
    { "gMultiplayerNoFeatureCuts", Kind::Int },
    { "gDisableItemboxes", Kind::Int },
    { "gAllThwompsAreMarty", Kind::Int },
    { "gAllBombKartsChase", Kind::Int },
    { "gGoFish", Kind::Int },
    { "gNumTrains", Kind::Int },
    { "gNumCarriages", Kind::Int },
    { "gHasTender", Kind::Int },
    { "gNumTrucks", Kind::Int },
    { "gNumBuses", Kind::Int },
    { "gNumTankerTrucks", Kind::Int },
    { "gNumCars", Kind::Int },
    { "gNoCulling", Kind::Int },
    { "gDisableLod", Kind::Int },
    { "gControllerPakScreen", Kind::Int },
    { "gUniqueCharacterSelections", Kind::Int },
    { "gSkipIntro", Kind::Int },
};
// Local tools that would desynchronise a session; forced off on every machine.
const char* const kForcedOff[] = { "gFreecam", "gEnableDebugMode", "gEditorEnabled", "gLookBehind",
                                   "gEnhancements.Mods.AlternateAssets" };

struct SavedCVar {
    std::string name;
    Kind kind;
    bool wasSet;
    int32_t i;
    float f;
};
std::vector<SavedCVar> sSaved;

// The values every machine agreed on at the handshake; re-imposed each frame so a settings change made mid-session
// from the Esc menu cannot split the simulations.
struct LockedCVar {
    const char* name;
    Kind kind;
    bool set;
    int32_t i;
    float f;
};
std::vector<LockedCVar> sLocked;

void LockSessionSettings() {
    sLocked.clear();
    for (const SharedCVar& c : kSharedCVars) {
        LockedCVar l{ c.name, c.kind, false, 0, 0 };
        if (c.kind == Kind::Int) {
            l.i = CVarGetInteger(c.name, INT_MIN);
            l.set = l.i != INT_MIN;
        } else {
            l.f = CVarGetFloat(c.name, -12345.678f);
            l.set = l.f != -12345.678f;
        }
        sLocked.push_back(l);
    }
    for (const char* name : kForcedOff) {
        sLocked.push_back({ name, Kind::Int, true, 0, 0 });
    }
}

void EnforceSessionSettings() {
    for (const LockedCVar& l : sLocked) {
        if (l.kind == Kind::Int) {
            int32_t now = CVarGetInteger(l.name, INT_MIN);
            int32_t want = l.set ? l.i : INT_MIN;
            if (now != want) {
                if (l.set) {
                    CVarSetInteger(l.name, l.i);
                } else {
                    CVarClear(l.name);
                }
            }
        } else {
            float now = CVarGetFloat(l.name, -12345.678f);
            float want = l.set ? l.f : -12345.678f;
            if (now != want) {
                if (l.set) {
                    CVarSetFloat(l.name, l.f);
                } else {
                    CVarClear(l.name);
                }
            }
        }
    }
    gTickLogic = 2;
}

void Remember(const char* name, Kind kind) {
    for (const SavedCVar& s : sSaved) {
        if (s.name == name) {
            return;
        }
    }
    SavedCVar s{ name, kind, false, 0, 0 };
    if (kind == Kind::Int) {
        s.i = CVarGetInteger(name, INT_MIN);
        s.wasSet = s.i != INT_MIN;
    } else {
        s.f = CVarGetFloat(name, -12345.678f);
        s.wasSet = s.f != -12345.678f;
    }
    sSaved.push_back(s);
}

uint32_t Le32(const uint8_t* p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}
uint16_t Le16(const uint8_t* p) {
    return (uint16_t) (p[0] | (p[1] << 8));
}
uint64_t Le64(const uint8_t* p) {
    return (uint64_t) Le32(p) | ((uint64_t) Le32(p + 4) << 32);
}

// The identity of an .o2r (zip) archive: every file's name, size and CRC from its central directory. Two extractions
// of the same ROM differ only in timestamps, so the raw bytes cannot be compared.
uint64_t HashArchiveContents(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        return 0;
    }
    uint64_t size = (uint64_t) in.tellg();
    uint64_t tailLen = std::min<uint64_t>(size, 65536 + 22 + 20);
    std::vector<uint8_t> tail(tailLen);
    in.seekg((std::streamoff) (size - tailLen));
    in.read((char*) tail.data(), (std::streamsize) tailLen);
    int64_t eocd = -1;
    for (int64_t i = (int64_t) tailLen - 22; i >= 0; i--) {
        if (Le32(&tail[i]) == 0x06054b50) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0) {
        return 0;
    }
    uint64_t entries = Le16(&tail[eocd + 10]);
    uint64_t cdSize = Le32(&tail[eocd + 12]);
    uint64_t cdOffset = Le32(&tail[eocd + 16]);
    if ((entries == 0xFFFF || cdOffset == 0xFFFFFFFFu) && eocd >= 20 && Le32(&tail[eocd - 20]) == 0x07064b50) {
        uint64_t z64 = Le64(&tail[eocd - 20 + 8]);
        uint8_t rec[56];
        in.seekg((std::streamoff) z64);
        in.read((char*) rec, sizeof(rec));
        if (in && Le32(rec) == 0x06064b50) {
            entries = Le64(rec + 32);
            cdSize = Le64(rec + 40);
            cdOffset = Le64(rec + 48);
        }
    }
    if (cdOffset + cdSize > size) {
        return 0;
    }
    std::vector<uint8_t> cd(cdSize);
    in.seekg((std::streamoff) cdOffset);
    in.read((char*) cd.data(), (std::streamsize) cdSize);
    std::vector<std::string> items;
    size_t pos = 0;
    for (uint64_t e = 0; e < entries && pos + 46 <= cd.size(); e++) {
        if (Le32(&cd[pos]) != 0x02014b50) {
            break;
        }
        uint32_t crc = Le32(&cd[pos + 16]);
        uint32_t usize = Le32(&cd[pos + 24]);
        uint16_t nameLen = Le16(&cd[pos + 28]);
        uint16_t extraLen = Le16(&cd[pos + 30]);
        uint16_t commentLen = Le16(&cd[pos + 32]);
        if (pos + 46 + nameLen > cd.size()) {
            break;
        }
        std::string name((const char*) &cd[pos + 46], nameLen);
        char meta[32];
        snprintf(meta, sizeof(meta), "|%08x|%u", crc, usize);
        items.push_back(name + meta);
        pos += 46 + nameLen + extraLen + commentLen;
    }
    std::sort(items.begin(), items.end());
    uint64_t h = 0xcbf29ce484222325ull;
    for (const std::string& it : items) {
        for (unsigned char c : it) {
            h ^= c;
            h *= 0x100000001b3ull;
        }
        h ^= '\n';
        h *= 0x100000001b3ull;
    }
    return h;
}

// Game data every player must share: the ROM-derived archive and any mods (by name and size).
uint64_t ContentId() {
    std::string combined = std::to_string(HashArchiveContents(Ship::Context::LocateFileAcrossAppDirs("mk64.o2r")));
    std::vector<std::string> mods;
    std::error_code ec;
    std::filesystem::path modDir = Ship::Context::GetPathRelativeToAppDirectory("mods");
    for (const auto& entry : std::filesystem::recursive_directory_iterator(modDir, ec)) {
        if (entry.is_regular_file(ec)) {
            mods.push_back(entry.path().lexically_relative(modDir).generic_string() + "|" +
                           std::to_string(entry.file_size(ec)));
        }
    }
    std::sort(mods.begin(), mods.end());
    for (const std::string& m : mods) {
        combined += "\n" + m;
    }
    return Fnv1a64(combined);
}

std::vector<uint8_t> BuildHostBlob() {
    std::vector<uint8_t> b;
    b.push_back(1); // blob version
    memset(sEeprom, 0, sizeof(sEeprom));
    std::string savePath = Ship::Context::GetPathRelativeToAppDirectory("default.sav");
    if (std::FILE* fp = std::fopen(savePath.c_str(), "rb")) {
        size_t got = std::fread(sEeprom, 1, sizeof(sEeprom), fp);
        (void) got;
        std::fclose(fp);
    }
    b.insert(b.end(), sEeprom, sEeprom + sizeof(sEeprom));
    size_t countAt = b.size();
    b.push_back(0);
    uint8_t count = 0;
    for (size_t i = 0; i < std::size(kSharedCVars); i++) {
        const SharedCVar& c = kSharedCVars[i];
        uint32_t bits;
        if (c.kind == Kind::Int) {
            int32_t v = CVarGetInteger(c.name, INT_MIN);
            if (v == INT_MIN) {
                continue;
            }
            bits = (uint32_t) v;
        } else {
            float v = CVarGetFloat(c.name, -12345.678f);
            if (v == -12345.678f) {
                continue;
            }
            memcpy(&bits, &v, 4);
        }
        b.push_back((uint8_t) i);
        for (int k = 0; k < 4; k++) {
            b.push_back((uint8_t) (bits >> (8 * k)));
        }
        count++;
    }
    b[countAt] = count;
    if (sOpt.testDebug) {
        b.push_back(0xDB);
    }
    return b;
}

// Puts every machine on the host's settings and save data. Runs on every machine, host included, before frame 0.
void ApplySharedBlob(const std::vector<uint8_t>& b) {
    for (const SharedCVar& c : kSharedCVars) {
        Remember(c.name, c.kind);
        CVarClear(c.name);
    }
    for (const char* name : kForcedOff) {
        Remember(name, Kind::Int);
        CVarSetInteger(name, 0);
    }
    // Test sessions only (the host passed --test-debug): the game's debug cheats during races, e.g. lap skip.
    sTestDebugRaces = b.size() >= 1 + 512 + 1 && b.back() == 0xDB;
    if (b.size() >= 1 + 512 + 1 && b[0] == 1) {
        memcpy(sEeprom, b.data() + 1, 512);
        size_t pos = 1 + 512;
        uint8_t count = b[pos++];
        for (uint8_t n = 0; n < count && pos + 5 <= b.size(); n++) {
            uint8_t idx = b[pos];
            uint32_t bits = (uint32_t) b[pos + 1] | ((uint32_t) b[pos + 2] << 8) | ((uint32_t) b[pos + 3] << 16) |
                            ((uint32_t) b[pos + 4] << 24);
            pos += 5;
            if (idx >= std::size(kSharedCVars)) {
                continue;
            }
            const SharedCVar& c = kSharedCVars[idx];
            if (c.kind == Kind::Int) {
                CVarSetInteger(c.name, (int32_t) bits);
            } else {
                float f;
                memcpy(&f, &bits, 4);
                CVarSetFloat(c.name, f);
            }
        }
    }
    // The save lives in memory for the session. Only the host writes results back to its own save file.
    gNetplayEepromOverride = sEeprom;
    gNetplayEepromWriteThrough = sOpt.cfg.role == Role::Host;
    gTickLogic = 2;
    LockSessionSettings();
}

struct Fnv32 {
    uint32_t h = 2166136261u;
    void Bytes(const void* p, size_t n) {
        const uint8_t* b = (const uint8_t*) p;
        for (size_t i = 0; i < n; i++) {
            h ^= b[i];
            h *= 16777619u;
        }
    }
    template <typename T> void Add(const T& v) {
        Bytes(&v, sizeof(T));
    }
};

uint32_t HashGameState() {
    Fnv32 h;
    h.Add(gRandomSeed16);
    h.Add(gGamestate);
    h.Add(gRaceState);
    h.Add(gIsGamePaused);
    h.Add((uint32_t) gTickCounter);
    h.Add(gCourseTimer);
    h.Add(gMenuSelection);
    h.Add(gPlayerCount);
    h.Add(gScreenModeSelection);
    h.Add(gModeSelection);
    for (int i = 0; i < 8; i++) {
        const Player& p = gPlayers[i];
        h.Add(p.type);
        h.Add(p.currentRank);
        h.Add(p.lapCount);
        h.Add(p.triggers);
        h.Add(p.currentItemCopy);
        h.Add(p.pos);
        h.Add(p.rotation);
        h.Add(p.velocity);
        h.Add(p.kartProps);
        h.Add(p.speed);
        h.Add(p.currentSpeed);
        h.Add(p.effects);
        h.Add(p.lakituProps);
        h.Add(p.boostTimer);
        h.Add(p.oobProps);
        h.Add(p.topSpeed);
        h.Add(p.driftState);
        h.Add(p.characterId);
    }
    if (gGamestate == RACING) {
        size_t n = CM_GetActorSize();
        h.Add((uint32_t) n);
        for (size_t i = 0; i < n; i++) {
            const Actor* a = CM_GetActor(i);
            h.Add(a->type);
            h.Add(a->flags);
            h.Add(a->state);
            h.Add(a->rot);
            h.Add(a->pos);
            h.Add(a->velocity);
        }
    }
    for (int i = 0; i < 4; i++) {
        h.Add(gControllers[i].button);
        h.Add(gControllers[i].rawStickX);
        h.Add(gControllers[i].rawStickY);
    }
    return h.h;
}

uint32_t Mix(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352d;
    x ^= x >> 15;
    x *= 0x846ca68b;
    x ^= x >> 16;
    return x;
}

// A test driver: steers through the menus into a two-player race, then follows the track's racing line, fires
// items and now and then hops. It reads only state both machines share, and sends ordinary pad input.
NetPad BotPad(uint32_t frame) {
    NetPad p;
    uint32_t r = Mix(frame * 2654435761u ^ sOpt.botSeed);
    int me = Netplay_LocalPlayer();
    if (gGamestate == RACING) {
        p.buttons |= A_BUTTON;
        if (me >= 0 && me < 8) {
            const Player& pl = gPlayers[me];
            u16 pathIndex = gPathIndexByPlayerId[me];
            u16 count = pathIndex < 4 ? gPathCountByPathIndex[pathIndex] : 0;
            if (count > 0 && gTrackPaths[pathIndex] != nullptr) {
                // Stuck (no progress along the path for two seconds): back off with the wheel turned the other way.
                static uint32_t sLastProgressFrame = 0, sUnstickUntil = 0;
                static u16 sBestPoint = 0;
                u16 nearest = gNearestPathPointByPlayerId[me];
                int ahead = ((int) nearest - (int) sBestPoint + count) % count;
                if (ahead > 0 && ahead < count / 2) {
                    sBestPoint = nearest;
                    sLastProgressFrame = frame;
                } else if (frame - sLastProgressFrame > 60 && frame >= sUnstickUntil && gRaceState == RACE_IN_PROGRESS) {
                    sUnstickUntil = frame + 40;
                    sLastProgressFrame = frame + 40;
                }
                const TrackPathPoint& t = gTrackPaths[pathIndex][(nearest + 8) % count];
                u16 want = atan2s((f32) t.x - pl.pos[0], (f32) t.z - pl.pos[2]);
                int diff = (int16_t) (uint16_t) (want - (u16) pl.rotation[1]);
                int stick = sOpt.botSteerSign * diff / 40;
                p.stickX = (int8_t) std::clamp(stick, -80, 80);
                if (frame < sUnstickUntil) {
                    p.buttons = B_BUTTON;
                    p.stickX = (int8_t) -p.stickX;
                    return p;
                }
            }
        }
        if (r % 97 == 0) {
            p.buttons |= Z_TRIG;
        }
        if ((frame + sOpt.botSeed * 37) % 400 < 12) {
            p.buttons |= R_TRIG;
        }
        if (sOpt.botSkipLaps && me == 0 && gRaceState == RACE_IN_PROGRESS && gLapCountByPlayerId[0] < 2 &&
            gLapCountByPlayerId[1] < 2 && frame % 60 == 7) {
            p.buttons |= R_JPAD; // debug mode: both players straight onto the final lap
        }
        return p;
    }
    if (me == 0 && gMenuSelection == MAIN_MENU && gPlayerCount < 2 && frame % 30 == 15) {
        p.stickX = 80;
        return p;
    }
    if (me == 0 && gMenuSelection == MAIN_MENU && gMainMenuSelection == MAIN_MENU_MODE_SELECT && gPlayerCount == 2 &&
        gGameModeMenuColumn[1] != sOpt.botMode) {
        if (frame % 30 == 15) {
            p.stickY = gGameModeMenuColumn[1] < sOpt.botMode ? -80 : 80;
        }
        return p;
    }
    if (me != 0 && gMenuSelection == MAIN_MENU) {
        return p; // every controller works in the main menu; leave the choices to player 1
    }
    if (frame % 30 < 2) {
        p.buttons |= A_BUTTON;
    }
    return p;
}

std::string ExecutablePath() {
#if defined(__APPLE__)
    char buf[4096];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) {
        return buf;
    }
#elif defined(__linux__)
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = 0;
        return buf;
    }
#endif
    return "";
}

} // namespace

// Test aid: the jumbotron copy of each frame (the centre 4:3 of the window at 320x240) saved as a BMP, so an
// unattended run can show what each player saw.
static void WriteSnapshot(uint32_t frame) {
    for (int fb = 0; fb < 3; fb++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s-%u-fb%d.bmp", sOpt.snapPrefix.c_str(), frame, fb);
        std::FILE* f = std::fopen(path, "wb");
        if (f == nullptr) {
            continue;
        }
        const int w = SCREEN_WIDTH, h = SCREEN_HEIGHT;
        uint32_t dataSize = (uint32_t) (w * h * 3);
        uint8_t hdr[54] = { 'B', 'M' };
        auto put32 = [&](int at, uint32_t v) {
            for (int i = 0; i < 4; i++) {
                hdr[at + i] = (uint8_t) (v >> (8 * i));
            }
        };
        put32(2, 54 + dataSize);
        put32(10, 54);
        put32(14, 40);
        put32(18, (uint32_t) w);
        put32(22, (uint32_t) h);
        hdr[26] = 1;
        hdr[28] = 24;
        put32(34, dataSize);
        std::fwrite(hdr, 1, sizeof(hdr), f);
        std::vector<uint8_t> row((size_t) w * 3);
        for (int y = h - 1; y >= 0; y--) {
            for (int x = 0; x < w; x++) {
                const uint8_t* px = (const uint8_t*) &gPortFramebuffers[fb][y * w + x];
                uint16_t v = (uint16_t) ((px[0] << 8) | px[1]); // stored in N64 (big-endian) order
                row[x * 3 + 2] = (uint8_t) (((v >> 11) & 31) << 3);
                row[x * 3 + 1] = (uint8_t) (((v >> 6) & 31) << 3);
                row[x * 3 + 0] = (uint8_t) (((v >> 1) & 31) << 3);
            }
            std::fwrite(row.data(), 1, row.size(), f);
        }
        std::fclose(f);
    }
}

extern "C" bool Netplay_IsSessionMode(void) {
    return sOpt.session;
}

extern "C" s32 Netplay_LocalPlayer(void) {
    return sOpt.session ? GetSession().LocalSlot() : -1;
}

extern "C" bool Netplay_FocusActive(void) {
    if (!sOpt.session || !sStarted || gGamestate != RACING || gActiveScreenMode == SCREEN_MODE_1P) {
        return false;
    }
    if (gRaceState < RACE_SETUP || gRaceState > RACE_CALCULATE_RANKS) {
        return false; // results, menus and the winner's victory lap use the normal split screen
    }
    int slot = GetSession().LocalSlot();
    const ScreenContext* sc = &gScreenContexts[slot];
    return sc->camera != nullptr && sc->screenWidth >= 8 && sc->screenHeight >= 8;
}

extern "C" bool Netplay_IsFocusScreen(s32 screen) {
    return screen == GetSession().LocalSlot() && Netplay_FocusActive();
}

extern "C" s32 Netplay_AudioListener(void) {
    if (!sOpt.session || !sStarted) {
        return -1;
    }
    if (gActiveScreenMode != SCREEN_MODE_2P_SPLITSCREEN_HORIZONTAL &&
        gActiveScreenMode != SCREEN_MODE_2P_SPLITSCREEN_VERTICAL) {
        return -1;
    }
    return GetSession().LocalSlot();
}

extern "C" void Netplay_UpdateRenderFocus(void) {
    if (!sOpt.session) {
        return;
    }
    auto wnd = std::dynamic_pointer_cast<Fast::Fast3dWindow>(Ship::Context::GetRawInstance()->GetWindow());
    if (wnd == nullptr) {
        return;
    }
    auto interp = wnd->GetInterpreterWeak().lock();
    if (interp == nullptr) {
        return;
    }
    if (!Netplay_FocusActive()) {
        interp->SetFocusRegion(false, 0, 0, 0, 0);
        return;
    }
    const ScreenContext* sc = &gScreenContexts[GetSession().LocalSlot()];
    float w = sc->screenWidth;
    float h = sc->screenHeight;
    interp->SetFocusRegion(true, sc->screenStartX - w / 2.0f, sc->screenStartY - h / 2.0f, w, h);
}

extern "C" void Netplay_OnFrameStart(void) {
    if (!sOpt.session || !sStarted) {
        return;
    }
    EnforceSessionSettings(); // the very top of the frame: nothing has read a setting yet
    // sFrame inputs were consumed by the frame that just finished.
    uint32_t done = sFrame - 1;
    uint32_t hash = HashGameState();
    GetSession().ReportHash(done, hash);
    if (sHashLog != nullptr) {
        int me = GetSession().LocalSlot();
        std::fprintf(sHashLog, "%u %08x gs=%d menu=%d race=%d seed=%04x pt=%u lap=%d\n", done, hash, gGamestate,
                     gMenuSelection, gRaceState, gRandomSeed16, gNearestPathPointByPlayerId[me],
                     gLapCountByPlayerId[me]);
        if (done % 60 == 0) {
            std::fflush(sHashLog);
        }
    }
    if (!sOpt.snaps.empty() && std::find(sOpt.snaps.begin(), sOpt.snaps.end(), done) != sOpt.snaps.end()) {
        WriteSnapshot(done);
    }
    if (sOpt.quitAfter != 0 && done >= sOpt.quitAfter) {
        Status st = GetSession().GetStatus();
        std::fprintf(stderr, "[netplay] quit-after reached at frame %u, desync=%d (frame %u), stalls %llu (%llu ms, longest %llu ms)\n",
                     done, st.desync ? 1 : 0, st.desyncFrame, (unsigned long long) st.stallCount,
                     (unsigned long long) st.stallMs, (unsigned long long) st.longestStallMs);
        if (sHashLog != nullptr) {
            std::fprintf(sHashLog, "END desync=%d desyncFrame=%u stalls=%llu stallMs=%llu longest=%llu\n",
                         st.desync ? 1 : 0, st.desyncFrame, (unsigned long long) st.stallCount,
                         (unsigned long long) st.stallMs, (unsigned long long) st.longestStallMs);
            std::fclose(sHashLog);
        }
        // Let the other side reach the same frame before the goodbye cuts it short.
        GetSession().Shutdown();
        NetplayGame_Shutdown();
        _Exit(0);
    }
}

extern "C" void Netplay_ExchangeControllerPads(OSContPad* pads) {
    if (!sOpt.session) {
        return;
    }
    sStarted = true;
    EnforceSessionSettings(); // before the frame's simulation, at the same point on every machine
    NetPad local;
    if (sOpt.bot) {
        local = BotPad(sFrame);
    } else {
        local.buttons = pads[0].button;
        local.stickX = pads[0].stick_x;
        local.stickY = pads[0].stick_y;
    }
    NetPad all[kMaxPlayers];
    GetSession().GetFrameInputs(sFrame, local, all);
    if (sTestDebugRaces) {
        gEnableDebugMode = gGamestate == RACING; // same frame on every machine: inputs are exchanged here
    }
    for (int i = 0; i < MAXCONTROLLERS; i++) {
        OSContPad blank{};
        pads[i] = blank;
    }
    for (int i = 0; i < kMaxPlayers; i++) {
        pads[i].button = all[i].buttons;
        pads[i].stick_x = all[i].stickX;
        pads[i].stick_y = all[i].stickY;
    }
    sFrame++;
}

bool NetplayGame_ParseArgs(int argc, char** argv) {
    Config& c = sOpt.cfg;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&](std::string& out) {
            if (i + 1 < argc) {
                out = argv[++i];
                return true;
            }
            return false;
        };
        std::string v;
        if (a == "--host") {
            sOpt.session = true;
            c.role = Role::Host;
            c.method = JoinMethod::RoomCode;
            c.directHost = true;
        } else if (a == "--join" && next(v)) {
            sOpt.session = true;
            c.role = Role::Join;
            c.target = v;
            if (v.rfind("MK64-", 0) == 0) {
                c.method = JoinMethod::ConnectionString;
            } else if (LooksLikeRoomCode(v)) {
                c.method = JoinMethod::RoomCode;
            } else {
                c.method = JoinMethod::Direct;
            }
        } else if (a == "--port" && next(v)) {
            c.port = (uint16_t) std::atoi(v.c_str());
        } else if (a == "--name" && next(v)) {
            c.name = v;
        } else if (a == "--delay" && next(v)) {
            c.inputDelay = std::atoi(v.c_str());
        } else if (a == "--bot") {
            sOpt.bot = true;
        } else if (a == "--bot-seed" && next(v)) {
            sOpt.botSeed = (uint32_t) std::strtoul(v.c_str(), nullptr, 10);
        } else if (a == "--quit-after" && next(v)) {
            sOpt.quitAfter = (uint32_t) std::strtoul(v.c_str(), nullptr, 10);
        } else if (a == "--hash-log" && next(v)) {
            sOpt.hashLog = v;
        } else if (a == "--snap" && next(v)) {
            size_t pos = 0;
            while (pos < v.size()) {
                size_t comma = v.find(',', pos);
                sOpt.snaps.push_back((uint32_t) std::strtoul(v.substr(pos, comma - pos).c_str(), nullptr, 10));
                pos = comma == std::string::npos ? v.size() : comma + 1;
            }
        } else if (a == "--snap-prefix" && next(v)) {
            sOpt.snapPrefix = v;
        } else if (a == "--bot-steer-sign" && next(v)) {
            sOpt.botSteerSign = std::atoi(v.c_str());
        } else if (a == "--bot-mode" && next(v)) {
            sOpt.botMode = v == "vs" ? 1 : (v == "battle" ? 2 : 0);
        } else if (a == "--bot-skip-laps") {
            sOpt.botSkipLaps = true;
        } else if (a == "--test-build-id" && next(v)) {
            sOpt.testBuildId = v;
        } else if (a == "--test-debug") {
            sOpt.testDebug = true;
        } else if (a == "--relay" && next(v)) {
            c.relay = v;
        } else if (a == "--test-no-direct") {
            c.testNoDirect = true;
        } else if (a == "--sim-loss" && next(v)) {
            c.simLossPct = std::atoi(v.c_str());
        } else if (a == "--sim-latency" && next(v)) {
            c.simLatencyMs = std::atoi(v.c_str());
        } else if (a == "--sim-jitter" && next(v)) {
            c.simJitterMs = std::atoi(v.c_str());
        }
    }
    if (!sOpt.session) {
        return false;
    }
    // Uninitialised heap memory has to read the same on every machine. Have the allocator fill new blocks with a
    // fixed pattern (0xAA on both systems); the setting only takes effect at process start, hence the re-exec.
#if defined(__APPLE__)
    if (std::getenv("MallocPreScribble") == nullptr) {
        setenv("MallocPreScribble", "1", 1);
        setenv("MallocScribble", "1", 1);
        setenv("MallocNanoZone", "0", 1);
        std::string exe = ExecutablePath();
        execv(exe.c_str(), argv);
    }
#elif defined(__linux__)
    if (std::getenv("MALLOC_PERTURB_") == nullptr) {
        setenv("MALLOC_PERTURB_", "85", 1);
        std::string exe = ExecutablePath();
        execv(exe.c_str(), argv);
    }
#endif
    if (!sOpt.hashLog.empty()) {
        sHashLog = std::fopen(sOpt.hashLog.c_str(), "w");
    }
    return true;
}

// macOS slows down apps that are not in front (App Nap, timer coalescing). In lockstep the slower machine sets the
// pace for both, so an online session asks for latency-critical, user-initiated activity for its whole life.
static void KeepFullSpeed() {
#if defined(__APPLE__)
    static id sActivity = nullptr;
    if (sActivity != nullptr) {
        return;
    }
    id info = ((id(*)(id, SEL)) objc_msgSend)((id) objc_getClass("NSProcessInfo"), sel_registerName("processInfo"));
    id reason = ((id(*)(id, SEL, const char*)) objc_msgSend)((id) objc_getClass("NSString"),
                                                             sel_registerName("stringWithUTF8String:"),
                                                             "Online play: the other player's game waits on this one");
    const uint64_t kUserInitiated = 0x00FFFFFFull, kLatencyCritical = 0xFF00000000ull;
    id activity = ((id(*)(id, SEL, uint64_t, id)) objc_msgSend)(
        info, sel_registerName("beginActivityWithOptions:reason:"), kUserInitiated | kLatencyCritical, reason);
    sActivity = ((id(*)(id, SEL)) objc_msgSend)(activity, sel_registerName("retain"));
#endif
}

bool NetplayGame_RunLobby() {
    Config& c = sOpt.cfg;
    if (c.name.empty()) {
        const char* user = std::getenv("USER");
        c.name = user != nullptr ? user : (c.role == Role::Host ? "Host" : "Guest");
    }
    std::string buildId = sOpt.testBuildId.empty() ? std::string(NETPLAY_BUILD_ID) : sOpt.testBuildId;
    c.buildId = Fnv1a64(buildId + "|proto" + std::to_string(kProtocolVersion));
    c.contentId = ContentId();
    c.frameMs = 1000.0 / 30.0;
    if (c.role == Role::Host) {
        c.hostBlob = BuildHostBlob();
    }
    SPDLOG_INFO("[netplay] build {:016x} content {:016x} role {}", c.buildId, c.contentId,
                c.role == Role::Host ? "host" : "join");

    KeepFullSpeed();
    Session& s = GetSession();
    // While a frame waits for the other player: keep the window answering the system, and let a close end it.
    s.SetWaitHook([] {
        SDL_PumpEvents();
        return !SDL_QuitRequested();
    });
    s.Start(c);
    NetplayUI_ShowLobby(true);

    auto wnd = std::dynamic_pointer_cast<Fast::Fast3dWindow>(Ship::Context::GetRawInstance()->GetWindow());
    static Gfx sEmptyList[] = { gsSPEndDisplayList() };
    bool ok = false;
    while (WindowIsRunning()) {
        if (s.IsReady()) {
            ok = true;
            break;
        }
        if (NetplayUI_LobbyCancelled()) {
            break;
        }
        if (wnd != nullptr) {
            wnd->HandleEvents();
            wnd->DrawAndRunGraphicsCommands(sEmptyList, {});
        } else {
            SDL_Delay(16);
        }
    }
    NetplayUI_ShowLobby(false);
    if (!ok) {
        s.Cancel();
        sOpt.session = false; // carry on as an ordinary offline game
        return false;
    }
    ApplySharedBlob(s.SharedBlob());
    NetplayUI_ShowOverlay(true);
    SPDLOG_INFO("[netplay] session ready: slot {} delay {}", s.LocalSlot(), s.GetStatus().inputDelay);
    return true;
}

void NetplayGame_Shutdown() {
    if (!sOpt.session) {
        return;
    }
    Status st = GetSession().GetStatus();
    SPDLOG_INFO("[netplay] session over: frame {}, desync {} (frame {}), stalls {} ({} ms, longest {} ms)", st.frame,
                st.desync, st.desyncFrame, st.stallCount, st.stallMs, st.longestStallMs);
    GetSession().Shutdown();
    gNetplayEepromOverride = nullptr;
    for (const SavedCVar& s : sSaved) {
        if (!s.wasSet) {
            CVarClear(s.name.c_str());
        } else if (s.kind == Kind::Int) {
            CVarSetInteger(s.name.c_str(), s.i);
        } else {
            CVarSetFloat(s.name.c_str(), s.f);
        }
    }
    sSaved.clear();
}

// Test aid: MK64_TEST_RELAUNCH="--host --name X ..." makes an ordinary launch restart itself the way the Online menu
// buttons do, so the restart path can be exercised without clicking.
void NetplayGame_TestRelaunchHook() {
    const char* spec = std::getenv("MK64_TEST_RELAUNCH");
    if (spec == nullptr || sOpt.session) {
        return;
    }
    std::vector<std::string> args;
    std::string cur;
    for (const char* c = spec; ; c++) {
        if (*c == ' ' || *c == '\0') {
            if (!cur.empty()) {
                args.push_back(cur);
            }
            cur.clear();
            if (*c == '\0') {
                break;
            }
        } else {
            cur += *c;
        }
    }
    unsetenv("MK64_TEST_RELAUNCH");
    NetplayGame_Relaunch(args);
}

void NetplayGame_Relaunch(const std::vector<std::string>& args) {
    NetplayGame_Shutdown();
    CVarSave();
    std::string exe = ExecutablePath();
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(exe.c_str()));
    for (const std::string& a : args) {
        argv.push_back(const_cast<char*>(a.c_str()));
    }
    argv.push_back(nullptr);
    execv(exe.c_str(), argv.data());
    std::fprintf(stderr, "[netplay] relaunch failed\n");
    _Exit(1);
}
