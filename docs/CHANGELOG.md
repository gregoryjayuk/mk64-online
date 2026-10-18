# CHANGELOG — mk64-online (SpaghettiKart with online play)

Newest first. One entry per version handed to players. Append; never rewrite.

## 2026-10-13 · first playable online build (netplay branch)

- **Online play for two players: races (Grand Prix, VS) and Battle.** Each player sees their own kart full screen, with stereo sound from their own camera.
- **Connecting:** room codes (exchanged through ntfy.sh), punching through home routers via STUN, a direct address on the same network, and a copy-paste connection code as fallback.
- **Online Play page** in the Esc menu (Host / Join), a lobby while connecting, and an overlay showing who you are playing, ping, input delay, pauses, waits and any out-of-sync warning.
- **Relay fallback** (`tools/netplay/relay`) for networks that block direct connections.
- **Gameplay settings are locked during a session**, and macOS is kept from throttling a background game (App Nap).
- **Hosting falls back to any free port** when 46464 is taken.
- **Linux:**
  - a package with a launcher;
  - the first run works without a dialog tool (Wayland without zenity);
  - window snapshots for tests (`MK64_SNAP_WINDOW`).
- **The window stays responsive while waiting**, and stall statistics are logged.
- **Lockstep netcode:** input delay chosen from the measured round trip, inputs sent redundantly so lost packets do not stall, a per-frame state hash to detect desyncs, and a version and game-data check at handshake.
- **Determinism fixes:**
  - strict IEEE floating point and signed char;
  - zeroed uninitialised actor, object, train, boat and crossing fields;
  - a fixed-pattern heap in online sessions;
  - music cues no longer cut the cameras;
  - the host's gameplay settings and save shared before the first frame.
- **macOS:**
  - fixed the Metal shader compile failure on macOS 27;
  - Retina-native rendering by default (2x internal resolution);
  - local-network permission text;
  - a packaging script for a zip another Mac can open.
- **Tests:**
  - an unattended two-copy runner (`tools/netplay/localtest.py`) with a driving bot, mode selection, lap skip, frame snapshots and a simulated lossy link;
  - all runs in sync: GP, VS, Battle, race finishes and results, 5,400 frames, and 5% loss at 70 ms.
  - Mac against Linux (x86-64, gcc 16) in sync: a GP race (2,401 frames) and a VS race through the finish (3,601 frames).
