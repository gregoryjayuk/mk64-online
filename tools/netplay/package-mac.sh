#!/bin/sh
# Packages the current Release build as a zip to hand to another Mac player: the app bundle with the engine data
# (spaghetti.o2r), SDL's controller database for gamepads, and an ad-hoc signature. No Nintendo data goes in: each
# player picks their own US ROM the first time the game starts.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
APP="$ROOT/build-cmake/SpaghettiKart.app"
OUT="$ROOT/dist"
STAGE="$OUT/stage"
# Reconfigure so the online build id names this exact commit (players can only connect identical builds).
if [ -n "$(git -C "$ROOT" status --porcelain --untracked-files=no)" ]; then
    echo "Commit first: the build id would say -dirty and no other build could ever match it."
    exit 1
fi
cmake --build "$ROOT/build-cmake" -j7 > /dev/null
[ -d "$APP" ] || { echo "Build failed"; exit 1; }
rm -rf "$STAGE"
mkdir -p "$STAGE"
ditto "$APP" "$STAGE/SpaghettiKart.app"
cp "$ROOT/build-cmake/spaghetti.o2r" "$STAGE/SpaghettiKart.app/Contents/Resources/spaghetti.o2r"
DB="$OUT/gamecontrollerdb.txt"
if [ ! -s "$DB" ]; then
    curl -fsSL -o "$DB" https://raw.githubusercontent.com/mdqinc/SDL_GameControllerDB/master/gamecontrollerdb.txt
fi
cp "$DB" "$STAGE/SpaghettiKart.app/Contents/Resources/gamecontrollerdb.txt"
codesign --force --deep --sign - --options runtime --entitlements "$ROOT/cmake/macos/entitlements.plist" "$STAGE/SpaghettiKart.app"
cp "$ROOT/docs/HOW-TO-PLAY-ONLINE.md" "$STAGE/HOW-TO-PLAY-ONLINE.md" 2>/dev/null || true
# Named after the online build id: players can connect exactly when their packages carry the same name.
ID=$(sed -n 's/.*"\(.*\)-strictfp".*/\1/p' "$ROOT/build-cmake/netplay_build_id.h")
ZIP="$OUT/SpaghettiKart-Online-mac-$ID.zip"
rm -f "$ZIP"
(cd "$STAGE" && ditto -c -k --sequesterRsrc . "$ZIP")
rm -rf "$STAGE"
echo "$ZIP"
