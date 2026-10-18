# SpaghettiKart Online — Linux

1. Unpack this folder anywhere, for example `~/Games/SpaghettiKart-Online`.
2. Copy your **US** Mario Kart 64 ROM (`.z64`, SHA-1 starts `579c48e2`) into the folder.
3. Run `./run-spaghettikart.sh`. The first start builds the game data from your ROM, which takes about a minute. After that the ROM is no longer needed.
4. Online play works exactly as on the Mac: **Esc → Online → Play Online**, then **Host a game** or **Join**. See `HOW-TO-PLAY-ONLINE.md`.

This binary was built on Arch Linux (gcc 16, October 2026) and plays against the Mac build of the same commit. If it will not start because a `GLIBCXX`/`GLIBC` version is missing, update your system (`sudo pacman -Syu`) or build from source (`docs/BUILDING.md` in the repo, same commit).
