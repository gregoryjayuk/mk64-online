# Playing Mario Kart 64 online

Two people, two computers, one race. Each of you sees your own kart full screen, the way a one-player game looks, while the race itself is shared: same karts, same items, same everything, frame by frame. Races and battles both work.

## What each player needs

- A Mac with Apple Silicon (M1 or newer). Linux works too, built from the same source (see the end).
- This build of the game. You must both run the same zip, because the game refuses to connect two different versions.
- Your own Mario Kart 64 ROM, **US version**, as a `.z64` file. Its SHA-1 starts `579c48e2`. The game has no Nintendo data in it; it builds its data from your ROM the first time it starts.
- A controller is best: any normal USB or Bluetooth pad works. The keyboard works too.

## First start (each player, once)

1. Unzip `SpaghettiKart-Online-mac-….zip` and drag **SpaghettiKart.app** into **Applications**.
2. Open it. If macOS says it cannot check the app for malicious software, click Done. Then open **System Settings → Privacy & Security**, scroll down, click **Open Anyway** next to SpaghettiKart, and confirm. If there is no such button, run this in Terminal instead, then open the app again:
   ```bash
   xattr -dr com.apple.quarantine /Applications/SpaghettiKart.app
   ```
3. The first time, the game asks for your ROM. Pick your US Mario Kart 64 `.z64` file. It builds the game data, which takes about a minute, and then starts.
4. **Esc** opens the menu. **Settings → Controls** lets you check your controller. **Fn+F11**, or the green window button, toggles full screen.
5. If macOS asks whether SpaghettiKart may find devices on your local network, or may accept incoming network connections, click **Allow**. Online play needs both.

## Playing together

**Whoever hosts:**
1. Press **Esc**, then go to **Online → Play Online**. Type your name, then click **Host a game**.
2. The game restarts into the online lobby and shows a **room code**, for example `K7QM-2XRD`. Tell your friend the code.

**The friend:**
1. Press **Esc**, then go to **Online → Play Online**. Type your name, enter the room code under **Join**, and click **Join**.
2. The game restarts, finds the host and connects. This usually takes two or three seconds.

Both games then start together from the very beginning, like switching on one console with two controllers:
- The host is player 1 and drives the menus: on the main menu, choose **2P GAME**, then the mode (Grand Prix, VS or Battle).
- Each of you picks your character with your own controller.
- In the race, each of you sees your own player.
- After the finish line the normal split screen comes back for the results.

A small line in the top-left corner shows who you are playing, the round-trip time, and the input delay. Pressing Start pauses the game for both of you. In full screen, **Esc** opens the menu and brings back the mouse pointer.

## On the same network (the office, at home)

The room code works anywhere. On the same Wi-Fi, the friend can type the host's address instead of the code: the host's lobby lists it, for example `192.168.1.20`.

Many company Wi-Fi networks stop laptops from talking to each other. If the lobby stays on "Connecting…" in the office:
1. Turn on **Personal Hotspot** on a phone.
2. Join both Macs to that hotspot.
3. Host and join again.

A game uses very little data, only button presses.

## When something goes wrong

| What you see | What it means and what to do |
|---|---|
| "different versions of the game" | You are not running the same zip. Use exactly the same one. |
| "game data differs" | One of you has a non-US ROM, or has mods installed. You both need the US ROM and the same mods (or none). |
| "No game found for room …" | Check the code, and check the host is still in the lobby. Codes expire when the host closes the lobby. |
| Stuck on "Connecting…" | Your networks block direct connections. Use the same Wi-Fi or a phone hotspot (above). |
| "Waiting for …" | Your friend's game paused for a moment, for example while loading or because their connection dropped. It carries on when they are back. After 20 seconds without contact the session ends. |
| "Out of sync since frame …" | The two games have started to differ. This is a bug. Finish or quit, restart both games, and send the log file to whoever gave you the build, or attach it to an issue on GitHub: `~/Library/Application Support/SpaghettiKart/logs/Spaghetti Kart.log`. |
| "left the game" | Your friend quit. Click **Back to single player**. |

To leave a session, open **Online → Play Online** and click **Leave and play offline**, or just quit the game.

## Linux

Build from source with [docs/BUILDING.md](BUILDING.md), from the same commit as your friend's build. A Mac build and a Linux build of the same commit may play each other: both use strict IEEE floating point, so they compute the same physics. This is newer and less tested than Mac against Mac. If you see "Out of sync", that is why.
