#!/usr/bin/env python3
"""Order-persistence test for the scripted-AI suppression gate.

Issue exactly ONE attack-move to the bot's army toward a far target, then STOP
sending orders and watch for ~N frames. If the scripted AI is suppressed for the
bridged slot, the one-shot order persists and the army keeps closing on the
target. If the AI is still micro-managing the slot, it countermands the order and
the army stalls or turns back.

Run against a game started with a TACTICAL bridged slot, e.g.:
    -aibridgeport 8765 -aibridgeslot 0 -slot faction=USA,difficulty=tactical ...
then:
    python persistence_test.py --port 8765
"""
from __future__ import annotations

import argparse
import math

from bridge_client import BridgeClient, a_attackmove


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--watch-frames", type=int, default=150)
    ap.add_argument("--offset", type=float, default=700.0,
                    help="target = army centroid + this offset on x and y")
    args = ap.parse_args()

    cli = BridgeClient(args.host, args.port)
    print(f"connected to {args.host}:{args.port}", flush=True)

    # 1. Wait for army units, pick the whole army group, issue ONE attack-move.
    army_ids = None
    target = None
    start_cx = start_cy = None
    while army_ids is None:
        obs = cli.next_observation()
        if obs is None:
            print("GAME_END before any army seen"); return
        army = [o for o in obs.my_units() if not o.is_building and o.hp > 0
                and "Dozer" not in o.tmpl]
        if army:
            army_ids = [o.id for o in army]
            start_cx = sum(o.x for o in army) / len(army)
            start_cy = sum(o.y for o in army) / len(army)
            target = (start_cx + args.offset, start_cy + args.offset)
            d0 = math.hypot(start_cx - target[0], start_cy - target[1])
            cli.send_actions(obs.frame, [a_attackmove(army_ids, target[0], target[1])])
            print(f"frame {obs.frame}: ONE attack-move issued for {len(army_ids)} units "
                  f"from ({start_cx:.0f},{start_cy:.0f}) -> ({target[0]:.0f},{target[1]:.0f}) "
                  f"dist0={d0:.0f}. NOT re-issuing.", flush=True)

    # 2. Watch without re-issuing. Track army-centroid distance to target.
    d0 = math.hypot(start_cx - target[0], start_cy - target[1])
    track = []
    watched = 0
    while watched < args.watch_frames:
        obs = cli.next_observation()
        if obs is None:
            print("GAME_END during watch"); break
        army = [o for o in obs.my_units() if o.id in set(army_ids)]
        if not army:
            print("army all gone during watch"); break
        cx = sum(o.x for o in army) / len(army)
        cy = sum(o.y for o in army) / len(army)
        d = math.hypot(cx - target[0], cy - target[1])
        track.append((obs.frame, cx, cy, d))
        watched += 1

    if not track:
        print("RESULT: INCONCLUSIVE — no frames watched"); return
    dlast = track[-1][3]
    closed = d0 - dlast
    disp = math.hypot(track[-1][1] - start_cx, track[-1][2] - start_cy)
    # sample the trajectory
    print(f"watched {len(track)} frames. dist_to_target: start={d0:.0f} end={dlast:.0f} "
          f"(closed {closed:.0f}); centroid displacement={disp:.0f}", flush=True)
    for i in (0, len(track)//4, len(track)//2, 3*len(track)//4, len(track)-1):
        f, x, y, d = track[i]
        print(f"  frame {f}: centroid=({x:.0f},{y:.0f}) dist={d:.0f}")
    if closed > 150:
        print("RESULT: PASS — one-shot order persisted; army kept closing on target "
              "(scripted AI suppressed)")
    elif disp > 60:
        print("RESULT: PARTIAL — army moved but did not clearly close on target "
              "(AI may still be interfering, or army engaged en route)")
    else:
        print("RESULT: FAIL — army did not advance on a single order "
              "(order countermanded / suppression not active)")
    cli.close()


if __name__ == "__main__":
    main()
