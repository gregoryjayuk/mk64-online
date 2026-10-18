#!/usr/bin/env python3
"""Runs two copies of the game against each other on this machine, both driven by the test bot, and compares
their per-frame state hashes. Any mismatch means the two simulations diverged (a desync).

Usage: python3 tools/netplay/localtest.py [--frames 1800] [--seed-a 1] [--seed-b 2] [--delay N]
Profiles: ~/Library/Application Support/SpaghettiKart-NetA and -NetB (Linux: ~/.local/share/...), each holding
mk64.o2r and a spaghettify.cfg.json (keep the volume at 0 for unattended runs).
"""
import argparse
import os
import platform
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def binary():
    if platform.system() == "Darwin":
        return os.path.join(ROOT, "build-cmake", "SpaghettiKart.app", "Contents", "MacOS", "SpaghettiKart")
    return os.path.join(ROOT, "build-cmake", "Spaghettify")


def profile(name):
    if platform.system() == "Darwin":
        return os.path.expanduser("~/Library/Application Support/SpaghettiKart-" + name)
    return os.path.expanduser("~/.local/share/SpaghettiKart-" + name)


def read_hashes(path):
    frames = {}
    end = None
    if not os.path.exists(path):
        return frames, end
    with open(path) as f:
        for line in f:
            parts = line.split()
            if not parts:
                continue
            if parts[0] == "END":
                end = line.strip()
                continue
            frames[int(parts[0])] = (parts[1], line.strip())
    return frames, end


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--frames", type=int, default=1800)
    ap.add_argument("--seed-a", type=int, default=1)
    ap.add_argument("--seed-b", type=int, default=2)
    ap.add_argument("--delay", type=int, default=0)
    ap.add_argument("--timeout", type=int, default=0, help="seconds; default frames/20 + 60")
    ap.add_argument("--tag", default="local")
    ap.add_argument("--snap", default="", help="comma-separated frames to save pictures of")
    ap.add_argument("--extra", default="", help="extra arguments for both games, space separated")
    ap.add_argument("--sim", default="", help="simulated internet on both ends: LOSS_PCT,LATENCY_MS,JITTER_MS")
    ap.add_argument("--join-mode", default="direct", choices=["direct", "room"],
                    help="direct: joiner types 127.0.0.1; room: joiner types the host's room code (via ntfy.sh)")
    args = ap.parse_args()

    runs = os.path.join(ROOT, "runs")
    os.makedirs(runs, exist_ok=True)
    ha = os.path.join(runs, args.tag + "-A.hash")
    hb = os.path.join(runs, args.tag + "-B.hash")
    for p in (ha, hb):
        if os.path.exists(p):
            os.remove(p)

    common = ["--bot", "--quit-after", str(args.frames)]
    if args.delay:
        common += ["--delay", str(args.delay)]
    if args.extra:
        common += args.extra.split()
    if args.sim:
        loss, lat, jit = args.sim.split(",")
        common += ["--sim-loss", loss, "--sim-latency", lat, "--sim-jitter", jit]
    snap_a = ["--snap", args.snap, "--snap-prefix", os.path.join(runs, args.tag + "-A")] if args.snap else []
    snap_b = ["--snap", args.snap, "--snap-prefix", os.path.join(runs, args.tag + "-B")] if args.snap else []
    env_a = dict(os.environ, SHIP_HOME=profile("NetA"))
    env_b = dict(os.environ, SHIP_HOME=profile("NetB"))
    log_a = open(os.path.join(runs, args.tag + "-A.log"), "w")
    log_b = open(os.path.join(runs, args.tag + "-B.log"), "w")
    host = subprocess.Popen([binary(), "--host", "--name", "Alice", "--bot-seed", str(args.seed_a), "--hash-log", ha]
                            + common + snap_a, env=env_a, stdout=log_a, stderr=subprocess.STDOUT)
    target = "127.0.0.1"
    if args.join_mode == "room":
        target = None
        deadline = time.time() + 30
        while time.time() < deadline and target is None:
            time.sleep(0.5)
            log_a.flush()
            with open(os.path.join(runs, args.tag + "-A.log")) as f:
                for line in f:
                    if "Room code " in line:
                        target = line.split("Room code ")[1].split()[0]
                        break
        if target is None:
            print("host never showed a room code")
            host.kill()
            return 3
        print("room code", target)
    else:
        time.sleep(2.0)
    join = subprocess.Popen([binary(), "--join", target, "--name", "Bob", "--bot-seed", str(args.seed_b),
                             "--hash-log", hb] + common + snap_b, env=env_b, stdout=log_b, stderr=subprocess.STDOUT)
    timeout = args.timeout or (args.frames // 20 + 60)
    start = time.time()
    while time.time() - start < timeout:
        if host.poll() is not None and join.poll() is not None:
            break
        time.sleep(1)
    for p in (host, join):
        if p.poll() is None:
            p.kill()
    elapsed = time.time() - start

    a, end_a = read_hashes(ha)
    b, end_b = read_hashes(hb)
    common_frames = sorted(set(a) & set(b))
    first_bad = None
    for f in common_frames:
        if a[f][0] != b[f][0]:
            first_bad = f
            break
    print(f"elapsed {elapsed:.0f}s, host frames {len(a)}, joiner frames {len(b)}, compared {len(common_frames)}")
    print(f"host exit {host.returncode} {end_a}; joiner exit {join.returncode} {end_b}")
    if first_bad is None:
        if common_frames:
            print(f"IN SYNC through frame {common_frames[-1]}  (last: {a[common_frames[-1]][1]})")
        else:
            print("NO FRAMES COMPARED")
        return 0 if common_frames else 2
    print(f"DESYNC at frame {first_bad}")
    for f in range(max(0, first_bad - 3), first_bad + 2):
        if f in a and f in b:
            print(f"  A {a[f][1]}\n  B {b[f][1]}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
