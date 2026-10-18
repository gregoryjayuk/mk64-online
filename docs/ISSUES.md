# ISSUES — what is wrong or risky

Open bugs and risks, numbered. Append during work; mark fixed items with the date and keep the line.

## Open

- **N1 (2026-10-13) Office Wi-Fi may block laptop-to-laptop traffic.** Many company networks isolate clients, and some block outbound UDP too. Room codes then fail in the office, and so do direct addresses. Workaround: a phone hotspot (HOW-TO-PLAY-ONLINE.md). Proper fix: a relay (IMPROVEMENTS I1).
- **N2 (2026-10-13) The lobby is ImGui and has never been seen by a human.** The connection logic is tested end to end, but the lobby's and overlay's layout on a real screen is untested: screen recording was unavailable overnight. **2026-10-14: seen in whole-window captures on Linux (`MK64_SNAP_WINDOW`).** The hosting lobby, the room code with Copy, the LAN address, the error states and the in-race overlay all render correctly. The ImGui code is the same on the Mac.
- **N3 (2026-10-13) Audio change untested by ear.** Muting the other camera and the one-player pan should work by reading the code, but nobody has listened yet.
- **N4 (2026-10-13) Mac against Linux.** Strict IEEE FP is on, but clang and gcc can still differ: libm calls, compiler-specific undefined behaviour. **2026-10-14: tested in sync**, a Mac against an x86-64 Arch Linux PC (gcc 16), in GP and VS through the finish. Still open for other distributions and compilers.
- **N5 (2026-10-13) A desync cannot be recovered.** On mismatch the overlay warns and the game goes on, so the two screens may disagree from then on. Recovery would need a full state transfer.
- **N6 (2026-10-13) Uninitialised stack locals in decomp code are not audited.** Heap garbage is made identical; stack garbage depends on each machine's render path, because ImGui and interpolation run between game frames. So far no desync points at it.
- **N7 (2026-10-13) Focus remap heuristics.** (2026-10-14: in 2P horizontal, the racer-position icons sit half off the bottom edge.) Rectangles at least 90% of the region's width or height stretch, and smaller ones keep their size. Wide HUD bars, the minimap or the progress column might sit oddly in some modes: 2P vertical, and Battle (which shows "LAP 1/3").
- **N8 (2026-10-13) The ntfy.sh rate limit.** The host polls every 4 s. A lobby left open for many minutes may hit the public instance's per-IP limits.
- **N9 (2026-10-13) macOS 27 Metal shader compile failure** in upstream SpaghettiKart: `thread const texture2d`/`sampler` parameters. **Fixed here 2026-10-13**, in both shader copies; worth upstreaming.
- **N10 (2026-10-14) A Linux build crashed at the lobby: stale objects.** `rsync -a` from the Mac preserves modification times, so a file edited on the Mac before the previous Linux compile looked older than its object file and was not recompiled. Old and new objects then disagreed on the `Session` layout, and the game jumped into the heap. **Fixed 2026-10-14:** the changed files were touched and rebuilt. Sync with `rsync -rlpgoD --checksum` (no `-t`), so changed files arrive with a fresh time.
