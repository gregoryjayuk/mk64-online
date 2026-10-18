#include "NetplayUI.h"
#include "Netplay.h"
#include "NetplayGame.h"

#include <imgui.h>
#include <libultraship/libultraship.h>
#include <libultraship/bridge/consolevariablebridge.h>
#include "port/ui/PortMenu.h"
#include "port/ui/MenuTypes.h"

extern "C" {
#include "code_800029B0.h"
}

namespace GameUI {
extern std::shared_ptr<PortMenu> mPortMenu;
}

#include <cstring>
#include <memory>
#include <string>

using namespace netplay;

namespace {

bool sLobbyVisible = false;
bool sLobbyCancelled = false;
bool sOverlayVisible = false;
char sPeerCode[512] = { 0 };
char sJoinTarget[256] = { 0 };
char sName[64] = { 0 };
char sRelay[128] = { 0 };
std::string sCodeFeedback;

const ImVec4 kRed(1.0f, 0.45f, 0.4f, 1.0f);
const ImVec4 kAmber(1.0f, 0.8f, 0.3f, 1.0f);
const ImVec4 kGreen(0.5f, 0.95f, 0.5f, 1.0f);
const ImVec4 kDim(0.75f, 0.75f, 0.75f, 1.0f);

std::string HostOnly(const std::string& addr) {
    std::string suffix = ":" + std::to_string(kDefaultPort);
    if (addr.size() > suffix.size() && addr.compare(addr.size() - suffix.size(), suffix.size(), suffix) == 0) {
        return addr.substr(0, addr.size() - suffix.size());
    }
    return addr;
}

// Full screen can capture and hide the pointer; the lobby and the error buttons need it.
void ReleaseMouse() {
    auto wnd = Ship::Context::GetRawInstance()->GetWindow();
    if (wnd != nullptr && !wnd->ShouldForceCursorVisibility()) {
        wnd->SetForceCursorVisibility(true);
        wnd->SetMouseCapture(false);
        wnd->SetCursorVisibility(true);
    }
}

void CopyButton(const char* id, const std::string& text) {
    ImGui::PushID(id);
    if (ImGui::SmallButton("Copy")) {
        ImGui::SetClipboardText(text.c_str());
    }
    ImGui::PopID();
}

void DrawTroubleshooting(const Status& st, bool host) {
    if (!ImGui::CollapsingHeader("Trouble connecting?")) {
        return;
    }
    ImGui::TextWrapped("%s", host ? "If the room code does not connect, send your friend this connection code "
                                    "(Teams, WhatsApp...) and paste theirs below."
                                  : "If the host asks for it, send them this reply code:");
    ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
    ImGui::TextColored(kDim, "%s", st.connString.empty() ? "(gathering...)" : st.connString.c_str());
    ImGui::PopTextWrapPos();
    if (!st.connString.empty()) {
        CopyButton("conn", st.connString);
    }
    if (host) {
        ImGui::Spacing();
        ImGui::TextUnformatted("Friend's code:");
        ImGui::SetNextItemWidth(-90);
        ImGui::InputText("##peercode", sPeerCode, sizeof(sPeerCode));
        ImGui::SameLine();
        if (ImGui::Button("Use it")) {
            sCodeFeedback = GetSession().SupplyPeerConnectionString(sPeerCode) ? "Connecting..." : "That is not a valid code.";
        }
        if (!sCodeFeedback.empty()) {
            ImGui::TextColored(kDim, "%s", sCodeFeedback.c_str());
        }
    }
    if (st.strictNat) {
        ImGui::Spacing();
        ImGui::TextColored(kAmber, "Your router hands out a new port for every destination (strict NAT).");
        ImGui::TextWrapped("Internet play may fail unless your friend's network is lenient. Same Wi-Fi, a phone "
                           "hotspot, or a forwarded UDP port %u on the host's router always work.",
                           (unsigned) kDefaultPort);
    }
}

class LobbyWindow : public Ship::GuiWindow {
  public:
    using Ship::GuiWindow::GuiWindow;

    void Draw() override {
        if (!sLobbyVisible) {
            return;
        }
        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(600, 0), ImGuiCond_Always);
        ImGui::Begin("Online Play##lobby", nullptr,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings);
        Status st = GetSession().GetStatus();
        bool host = Netplay_LocalPlayer() == 0;
        if (host) {
            ImGui::TextUnformatted("You are hosting. Give your friend this room code:");
            ImGui::Spacing();
            ImGui::SetWindowFontScale(2.4f);
            ImGui::TextColored(kGreen, "%s", st.roomCode.empty() ? "...." : st.roomCode.c_str());
            ImGui::SetWindowFontScale(1.0f);
            if (!st.roomCode.empty()) {
                ImGui::SameLine();
                CopyButton("room", st.roomCode);
            }
            ImGui::TextWrapped("In their game: Online Play, then Join, then type the code.");
            if (!st.lanAddresses.empty()) {
                ImGui::Spacing();
                ImGui::TextUnformatted("On the same network they can type your address instead:");
                for (const std::string& a : st.lanAddresses) {
                    ImGui::BulletText("%s", HostOnly(a).c_str());
                }
            }
        } else {
            ImGui::TextUnformatted("Joining your friend's game...");
        }
        ImGui::Separator();
        if (st.message != st.error) {
            ImGui::TextWrapped("%s", st.message.c_str());
        }
        if (!st.error.empty()) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextColored(st.phase == Phase::Failed ? kRed : kAmber, "%s", st.error.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::Spacing();
        DrawTroubleshooting(st, host);
        ImGui::Spacing();
        if (ImGui::Button(st.phase == Phase::Failed ? "Play offline instead" : "Cancel")) {
            sLobbyCancelled = true;
        }
        ImGui::End();
    }

  protected:
    void InitElement() override {
    }
    void DrawElement() override {
    }
    void UpdateElement() override {
    }
};

class OverlayWindow : public Ship::GuiWindow {
  public:
    using Ship::GuiWindow::GuiWindow;

    void Draw() override {
        if (!sOverlayVisible) {
            return;
        }
        Status st = GetSession().GetStatus();
        bool problem = st.waiting || st.desync || !st.error.empty() ||
                       (gIsGamePaused != 0 && (int) gIsGamePaused - 1 != st.localSlot);
        ImGui::SetNextWindowPos(ImVec2(ImGui::GetMainViewport()->Pos.x + 12, ImGui::GetMainViewport()->Pos.y + 12),
                                ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(problem ? 0.75f : 0.3f);
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                 ImGuiWindowFlags_NoNav;
        if (st.error.empty()) {
            flags |= ImGuiWindowFlags_NoInputs;
        }
        ImGui::Begin("##netplay-overlay", nullptr, flags);
        ImGui::SetWindowFontScale(0.9f);
        std::string who = st.peerName.empty() ? "your friend" : st.peerName;
        if (st.rttMs >= 0) {
            ImGui::TextColored(kDim, "Online with %s  |  %d ms  |  delay %d", who.c_str(), st.rttMs, st.inputDelay);
        } else {
            ImGui::TextColored(kDim, "Online with %s  |  delay %d", who.c_str(), st.inputDelay);
        }
        if (st.waiting) {
            ImGui::TextColored(kAmber, "Waiting for %s... %.1fs", who.c_str(), st.waitingMs / 1000.0);
        }
        if (gIsGamePaused != 0 && (int) gIsGamePaused - 1 != st.localSlot) {
            ImGui::TextColored(kAmber, "%s paused the game.", who.c_str());
        }
        if (st.desync) {
            ImGui::TextColored(kRed, "Out of sync since frame %u: your screens may now disagree.", st.desyncFrame);
        }
        if (!st.error.empty()) {
            ImGui::TextColored(kRed, "%s", st.error.c_str());
            ReleaseMouse(); // full screen may have captured the pointer; the button needs it
            if (ImGui::Button("Back to single player")) {
                NetplayGame_Relaunch({});
            }
        }
        ImGui::End();
    }

  protected:
    void InitElement() override {
    }
    void DrawElement() override {
    }
    void UpdateElement() override {
    }
};

std::vector<std::string> WithRelay(std::vector<std::string> args) {
    if (sRelay[0] != '\0') {
        args.push_back("--relay");
        args.push_back(sRelay);
    }
    return args;
}

void LoadName() {
    if (sRelay[0] == '\0') {
        const char* relay = CVarGetString("gNetplayRelay", "");
        if (relay != nullptr) {
            strncpy(sRelay, relay, sizeof(sRelay) - 1);
        }
    }
    if (sName[0] != '\0') {
        return;
    }
    const char* saved = CVarGetString("gNetplayName", "");
    if (saved != nullptr && saved[0] != '\0') {
        strncpy(sName, saved, sizeof(sName) - 1);
    } else if (const char* user = std::getenv("USER")) {
        strncpy(sName, user, sizeof(sName) - 1);
    }
}

// The "Online" page of the F1 menu (and the standalone window): host or join, or the state of the session.
void DrawOnlinePanel() {
    LoadName();
    if (Netplay_IsSessionMode()) {
        Status st = GetSession().GetStatus();
        std::string who = st.peerName.empty() ? "your friend" : st.peerName;
        ImGui::TextWrapped("You are playing online with %s.", who.c_str());
        if (st.rttMs >= 0) {
            ImGui::TextColored(kDim, "Round trip %d ms, input delay %d frames.", st.rttMs, st.inputDelay);
        }
        ImGui::Spacing();
        if (ImGui::Button("Leave and play offline", ImVec2(240, 0))) {
            NetplayGame_Relaunch({});
        }
        ImGui::TextColored(kDim, "The game restarts; %s's game will say you left.", who.c_str());
        return;
    }
    ImGui::TextWrapped("Race or battle a friend on another computer. Each of you sees your own kart full screen. "
                       "You both need this same build of the game and your own copy of the US ROM.");
    ImGui::Spacing();
    ImGui::TextUnformatted("Your name");
    ImGui::SetNextItemWidth(220);
    if (ImGui::InputText("##name", sName, sizeof(sName))) {
        CVarSetString("gNetplayName", sName);
        CVarSave();
    }
    ImGui::Separator();
    ImGui::TextUnformatted("Host");
    ImGui::TextColored(kDim, "You get a room code to give your friend.");
    if (ImGui::Button("Host a game", ImVec2(200, 0))) {
        NetplayGame_Relaunch(WithRelay({ "--host", "--name", sName }));
    }
    ImGui::Separator();
    ImGui::TextUnformatted("Join");
    ImGui::TextColored(kDim, "Type the host's room code, or their address on the same network.");
    ImGui::SetNextItemWidth(220);
    ImGui::InputTextWithHint("##join", "K7QM-2XRD or 192.168.1.20", sJoinTarget, sizeof(sJoinTarget));
    ImGui::SameLine();
    bool canJoin = sJoinTarget[0] != '\0';
    if (!canJoin) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Join", ImVec2(100, 0))) {
        NetplayGame_Relaunch(WithRelay({ "--join", sJoinTarget, "--name", sName }));
    }
    if (!canJoin) {
        ImGui::EndDisabled();
    }
    ImGui::Spacing();
    ImGui::TextColored(kDim, "The game restarts into the online lobby; your settings are kept.");
    if (ImGui::CollapsingHeader("Advanced: relay server")) {
        ImGui::TextWrapped("For networks that block direct connections. The host's relay is used by both players "
                           "automatically; leave this empty unless you run mk64-relay somewhere reachable.");
        ImGui::SetNextItemWidth(260);
        if (ImGui::InputTextWithHint("##relay", "host:46470", sRelay, sizeof(sRelay))) {
            CVarSetString("gNetplayRelay", sRelay);
            CVarSave();
        }
    }
}

void DrawOnlineWidget(WidgetInfo&) {
    DrawOnlinePanel();
}

void RegisterOnlineMenu() {
    GameUI::mPortMenu->AddMenuEntry("Online", "gSettings.Menu.OnlineSidebarSection");
    GameUI::mPortMenu->AddSidebarEntry("Online", "Play Online", 1);
    WidgetPath path = { "Online", "Play Online", SECTION_COLUMN_1 };
    GameUI::mPortMenu->AddWidget(path, "Play Online", WIDGET_CUSTOM).CustomFunction(DrawOnlineWidget);
}

RegisterMenuInitFunc sOnlineMenuInit(RegisterOnlineMenu);

class MenuWindow : public Ship::GuiWindow {
  public:
    using Ship::GuiWindow::GuiWindow;

  protected:
    void InitElement() override {
    }
    void DrawElement() override {
        DrawOnlinePanel();
    }
    void UpdateElement() override {
    }
};

std::shared_ptr<MenuWindow> sMenu;

} // namespace

void NetplayUI_Init() {
    auto gui = Ship::Context::GetRawInstance()->GetWindow()->GetGui();
    gui->AddGuiWindow(std::make_shared<LobbyWindow>("gNetplayLobbyWindow", false, "Online Play##lobbyhost",
                                                    ImVec2(600, 400)));
    gui->AddGuiWindow(std::make_shared<OverlayWindow>("gNetplayOverlayWindow", false, "##netplayoverlayhost",
                                                      ImVec2(200, 50)));
    sMenu = std::make_shared<MenuWindow>("gNetplayMenuOpen", "Online Play", ImVec2(420, 360));
    gui->AddGuiWindow(sMenu);
}

void NetplayUI_ShowLobby(bool show) {
    sLobbyVisible = show;
    auto wnd = Ship::Context::GetRawInstance()->GetWindow();
    if (show) {
        sLobbyCancelled = false;
        ReleaseMouse();
    } else if (wnd != nullptr && wnd->ShouldForceCursorVisibility()) {
        wnd->SetForceCursorVisibility(false); // back to the game's own pointer rules
    }
}

bool NetplayUI_LobbyCancelled() {
    return sLobbyCancelled;
}

void NetplayUI_ShowOverlay(bool show) {
    sOverlayVisible = show;
}

void NetplayUI_OpenMenu() {
    if (sMenu != nullptr) {
        sMenu->Show();
    }
}
