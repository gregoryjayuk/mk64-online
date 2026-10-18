#!/bin/sh
# SpaghettiKart Online (Linux). The first run needs your US Mario Kart 64 ROM (.z64) in this folder; the game finds
# it, builds its data (about a minute) and keeps everything in ~/.local/share/SpaghettiKart.
DIR=$(cd "$(dirname "$0")" && pwd)
export SHIP_HOME="${SHIP_HOME:-$HOME/.local/share/SpaghettiKart}"
mkdir -p "$SHIP_HOME"
for f in spaghetti.o2r gamecontrollerdb.txt; do
    [ -f "$SHIP_HOME/$f" ] && cmp -s "$DIR/$f" "$SHIP_HOME/$f" || cp "$DIR/$f" "$SHIP_HOME/$f"
done
cd "$DIR" && exec ./Spaghettify "$@"
