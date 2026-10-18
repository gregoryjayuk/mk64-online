#pragma once

// Online play windows: the "Online Play" menu (host or join), the lobby shown while connecting, and the small
// in-game overlay (who you are playing, ping, waiting, out-of-sync warnings).

void NetplayUI_Init();
void NetplayUI_ShowLobby(bool show);
bool NetplayUI_LobbyCancelled();
void NetplayUI_ShowOverlay(bool show);
void NetplayUI_OpenMenu();
