#pragma once

// Glue between the netplay session and the game. The C half is called from the decomp code (main.c),
// the C++ half from the port's startup (Game.cpp) and the UI.

#include <libultraship.h>

#ifdef __cplusplus
extern "C" {
#endif

// True for the whole life of a process launched for online play, from before the first frame.
bool Netplay_IsSessionMode(void);
// The player slot this machine controls (0 = host, 1 = joiner), or -1 outside online play.
s32 Netplay_LocalPlayer(void);
// read_controllers(): swaps the freshly read local pads for the inputs every machine agreed on for this frame.
void Netplay_ExchangeControllerPads(OSContPad* pads);
// Top of thread5_iteration(): the previous frame is complete; hash it for desync detection.
void Netplay_OnFrameStart(void);
// Rendering. During an online race each machine shows only its own player's split-screen region, stretched over
// the whole window by the renderer. Every machine still renders every screen: rendering feeds the simulation.
bool Netplay_FocusActive(void);
bool Netplay_IsFocusScreen(s32 screen);
// Just before the frame's display list runs: hands the renderer this frame's region (or switches it off).
void Netplay_UpdateRenderFocus(void);
// Audio: the camera (listener) whose sounds this machine plays during a two-player online race, else -1.
s32 Netplay_AudioListener(void);

#ifdef __cplusplus
}

#include <string>
#include <vector>

// Reads --host / --join and friends. Returns true when this launch is an online session.
bool NetplayGame_ParseArgs(int argc, char** argv);
// Before the game's first frame: shows the lobby and blocks until connected. False if the player gave up.
bool NetplayGame_RunLobby();
// Restores the player's own settings before they are saved on exit.
void NetplayGame_Shutdown();
// Starts a fresh copy of the game with these arguments and ends this one (online sessions always begin from a
// freshly started game so both machines share one identical starting state).
[[noreturn]] void NetplayGame_Relaunch(const std::vector<std::string>& args);
void NetplayGame_TestRelaunchHook();
#endif
