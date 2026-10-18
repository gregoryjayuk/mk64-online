# PUBLISHING — how the public fork is made

The public repository, github.com/gregoryjayuk/mk64-online, is a GitHub fork of HarbourMasters/SpaghettiKart. Its `netplay` branch is one squashed commit on top of the upstream commit this work started from, so the private history (commit messages, old file versions, author e-mail) never goes public. The `libultraship` submodule points at a matching single-commit branch, `mk64-online-netplay`, on a fork of Kenix3/libultraship.

## Excluded from the public tree

- `docs/STATUS.md`: live operational notes about the author's own machines, home network and test partners.
- `docs/LOCAL.md`: private how-to, if present.
- `tools/netplay/public-snapshot.sh`: the snapshot script names the private details it removes.
- Never committed, and refused by the script: ROMs (`*.z64`, `*.n64`, `*.v64`), ROM-derived archives (`*.o2r`, `*.otr`), `build-cmake/`, `vcpkg_installed/`, `runs/`, `dist/`.

## Scrubbed in the public tree

- `docs/ISSUES.md`, `docs/IMPROVEMENTS.md`, `docs/CHANGELOG.md`: machine and home-network names become generic descriptions.
- `docs/HOW-TO-PLAY-ONLINE.md`: "send the log to a named person" becomes "send it to whoever gave you the build, or attach it to an issue".
- `.gitmodules` and the `libultraship` gitlink: the fork and its squashed commit.
- `docs/screenshots/online-lobby.png`: the host's LAN address is pixelated (done once, in the committed image).
- Author and committer `Gregoryjay <gregoryjayuk@users.noreply.github.com>` with a fixed date, so rebuilding the same tree gives the same commits.

## Rebuild

```bash
tools/netplay/public-snapshot.sh /path/to/staging   # makes staging/libultraship and staging/mk64-online
git -C /path/to/staging/mk64-online log --oneline -3 netplay
```

Push `mk64-online-netplay` from `staging/libultraship` to the libultraship fork first, then `netplay` from `staging/mk64-online` to the mk64-online fork and make it the default branch. Both staging repos are shallow at their upstream commit; the push works because each fork already contains that commit.
