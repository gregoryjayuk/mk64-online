# IMPROVEMENTS — what could be

Ideas with their blueprint attached. Append; mark delivered items with the date.

- **I1 (2026-10-13) Relay fallback for hostile networks.** A tiny UDP relay (`tools/netplay/relay.cpp`, ~150 lines): each peer sends `REGISTER(room, token)`, then the relay forwards datagrams between the two registered endpoints. It runs on any machine with one forwarded UDP port, or on a small VPS. Add it to the offer as a `CandType::Relay` candidate, tried after direct paths fail for 5 s. A TCP/443 variant would get through office firewalls that drop UDP.
- **I2 (2026-10-13) Three and four players.** Host-relayed input fan-out: guests send to the host, which sends all-player frame sets, as MKart360 does. Also the 3P/4P focus layout (quarter regions), and a slot assignment in WELCOME.
- **I3 (2026-10-13) Desync forensics.** On mismatch, both sides dump the hashed fields per player and actor for that frame and exchange the first differing field, so the cause shows up in the log.
- **I4 (2026-10-13) Smarter input delay.** Measure RTT continuously from INPUT round trips and let the host step the delay up or down between races. It is fixed at handshake now.
- **I5 (2026-10-13) A one-player-style HUD for the local view.** Draw the 1P HUD (speedometer, lap-time after-images) from a private copy of `playerHUD`, instead of remapping the 2P HUD.
- **I6 (2026-10-13) Rejoin.** If a peer drops, keep its last inputs neutral and let it reconnect with a state snapshot. This needs a serialisable game state first.
- **I7 (2026-10-13) Upstream the macOS 27 shader fix and the uninitialised-member fixes** to HarbourMasters/SpaghettiKart.
- **I8 (2026-10-13) Windows build.** The sockets layer already has Winsock paths. Needs testing, plus `ws2_32`/`iphlpapi` linked in CMake.
