#!/usr/bin/env python3
"""LLM (or heuristic) agent that plays Generals through the AIBridge.

Loop: read an observation -> summarize state -> ask a Policy for orders ->
encode them as action batches -> send. Repeat at a fixed game-frame cadence.

Two policies:
  * heuristic  -- rules only, no API key. Proves the observe->decide->act pipe
                  and gives a baseline "gather army, attack the enemy" bot.
  * claude     -- Claude Opus 4.8 with tool-use. Needs ANTHROPIC_API_KEY (or an
                  `ant auth login` profile). The model is shown grouped state and
                  calls action tools; we translate group indices back to unit ids.

Run against a live game started with `-aibridgeport <port> -aibridgeslot <n>`:
    python agent.py --port 8765 --policy heuristic
    ANTHROPIC_API_KEY=... python agent.py --port 8765 --policy claude
"""
from __future__ import annotations

import argparse
import math
import os
import sys
import time
from collections import defaultdict

from bridge_client import (
    BridgeClient, Observation, Obj,
    a_move, a_attackmove, a_attack, a_stop, a_queue_unit,
)

# Templates we treat as economy/production, not combat army, for grouping.
NON_ARMY_HINTS = ("Dozer", "Worker", "Ambulance", "SupplyTruck", "SupplyCenter",
                  "ConstructionCrane")
FACTORY_HINTS = ("Barracks", "WarFactory", "AirfieldUSA", "Airfield", "PalaceGLA",
                 "ArmsDealer", "WarFactory", "Cmd", "CommandCenter")


def _is_army(o: Obj) -> bool:
    if o.is_building:
        return False
    return not any(h in o.tmpl for h in NON_ARMY_HINTS)


def _centroid(objs):
    if not objs:
        return (0.0, 0.0)
    return (sum(o.x for o in objs) / len(objs), sum(o.y for o in objs) / len(objs))


def _short(tmpl: str) -> str:
    # "AmericaVehicleHumvee" -> "Humvee"; strip faction/kind prefixes for readability
    t = tmpl
    for pre in ("AmericaVehicle", "AmericaInfantry", "AmericaAircraft", "America",
                "ChinaVehicle", "ChinaInfantry", "ChinaAircraft", "China",
                "GLAVehicle", "GLAInfantry", "GLA", "Faction"):
        if t.startswith(pre):
            t = t[len(pre):]
            break
    return t or tmpl


class Groups:
    """Cluster the bot's units and the visible enemies into labeled groups."""

    def __init__(self, obs: Observation):
        self.obs = obs
        self.army_groups: list[dict] = []   # {label, tmpl, count, cx, cy, ids}
        self.factory_groups: list[dict] = []
        self.enemy_groups: list[dict] = []
        self._build()

    def _cluster(self, units, cell=250.0):
        # group by (template, coarse grid cell)
        buckets = defaultdict(list)
        for u in units:
            key = (u.tmpl, round(u.x / cell), round(u.y / cell))
            buckets[key].append(u)
        out = []
        for (tmpl, _, _), us in buckets.items():
            cx, cy = _centroid(us)
            out.append({"tmpl": tmpl, "count": len(us), "cx": cx, "cy": cy,
                        "ids": [u.id for u in us]})
        out.sort(key=lambda g: -g["count"])
        return out

    def _build(self):
        mine = self.obs.my_units()
        army = [o for o in mine if _is_army(o)]
        factories = [o for o in mine if o.is_building and any(h in o.tmpl for h in FACTORY_HINTS)]
        self.army_groups = self._cluster(army)
        for i, g in enumerate(self.army_groups):
            g["label"] = i
        # factories: one group per building (keep ids individual)
        fbuckets = defaultdict(list)
        for f in factories:
            fbuckets[f.tmpl].append(f)
        self.factory_groups = []
        for i, (tmpl, fs) in enumerate(fbuckets.items()):
            cx, cy = _centroid(fs)
            self.factory_groups.append({"label": i, "tmpl": tmpl, "count": len(fs),
                                        "cx": cx, "cy": cy, "ids": [f.id for f in fs]})
        # enemies clustered too, buildings and units separately labeled
        self.enemy_groups = self._cluster(self.obs.enemies())
        for i, g in enumerate(self.enemy_groups):
            g["label"] = i

    def army_ids(self, label: int):
        for g in self.army_groups:
            if g["label"] == label:
                return g["ids"]
        return []

    def factory_ids(self, label: int):
        for g in self.factory_groups:
            if g["label"] == label:
                return g["ids"]
        return []

    def enemy_target(self, label: int):
        for g in self.enemy_groups:
            if g["label"] == label:
                return g["ids"][0] if g["ids"] else None
        return None


# --------------------------------------------------------------------------
# Policies
# --------------------------------------------------------------------------

class HeuristicPolicy:
    """No API key. Rally all army units and attack-move toward the enemy."""

    name = "heuristic"

    def decide(self, obs: Observation, groups: Groups) -> list:
        actions = []
        all_army = [uid for g in groups.army_groups for uid in g["ids"]]
        if not all_army:
            return actions
        enemies = obs.enemies()
        if enemies:
            # target the enemy centroid (favor buildings if any are visible)
            bldgs = [e for e in enemies if e.is_building]
            tx, ty = _centroid(bldgs or enemies)
        else:
            # no enemy in view: push toward map center as a scout/advance
            xs = [o.x for o in obs.objects] or [0]
            ys = [o.y for o in obs.objects] or [0]
            tx, ty = (sum(xs) / len(xs), sum(ys) / len(ys))
        actions.append(a_attackmove(all_army, tx, ty))
        return actions


class ClaudePolicy:
    """Claude Opus 4.8 with tool-use. Shown grouped state; calls action tools."""

    name = "claude"

    SYSTEM = (
        "You are commanding one army in a live match of Command & Conquer Generals: "
        "Zero Hour. You control ONE player's units. Your goal is to defeat the enemy: "
        "keep your army together, focus fire, destroy enemy production and command "
        "buildings, and don't feed units piecemeal. You are given the current game "
        "state each turn (your unit groups, your factories, visible enemies, resources). "
        "Issue orders by calling the action tools. Reference unit groups and enemy "
        "groups by their integer label. Positions are map coordinates (x,y). It's fine "
        "to issue several orders per turn, or none if the current plan is working. Be "
        "decisive and aggressive but not reckless."
    )

    TOOLS = [
        {"name": "attack_move_group",
         "description": "Move an army group toward (x,y), attacking any enemies encountered en route. Best for advancing on the enemy.",
         "input_schema": {"type": "object", "properties": {
             "group": {"type": "integer", "description": "army group label"},
             "x": {"type": "number"}, "y": {"type": "number"}},
             "required": ["group", "x", "y"]}},
        {"name": "attack_target",
         "description": "Order an army group to attack a specific enemy group (focus fire).",
         "input_schema": {"type": "object", "properties": {
             "group": {"type": "integer", "description": "army group label"},
             "enemy": {"type": "integer", "description": "enemy group label"}},
             "required": ["group", "enemy"]}},
        {"name": "move_group",
         "description": "Reposition an army group to (x,y) without seeking combat (retreat/regroup).",
         "input_schema": {"type": "object", "properties": {
             "group": {"type": "integer"}, "x": {"type": "number"}, "y": {"type": "number"}},
             "required": ["group", "x", "y"]}},
        {"name": "stop_group",
         "description": "Halt an army group where it is (hold position).",
         "input_schema": {"type": "object", "properties": {
             "group": {"type": "integer"}}, "required": ["group"]}},
        {"name": "train_unit",
         "description": "Queue a unit for production at a factory group. template is an exact unit template name.",
         "input_schema": {"type": "object", "properties": {
             "factory": {"type": "integer", "description": "factory group label"},
             "template": {"type": "string"}}, "required": ["factory", "template"]}},
    ]

    def __init__(self, model="claude-opus-4-8", effort="low"):
        import anthropic  # deferred so heuristic mode needs no SDK
        self.client = anthropic.Anthropic()
        self.model = model
        self.effort = effort

    def _state_text(self, obs: Observation, groups: Groups) -> str:
        me = obs.my_player()
        lines = [f"Frame {obs.frame}. You are player index {obs.bot_index}."]
        if me:
            lines.append(f"Resources: money={me.money}, power={me.power_produced}/{me.power_consumed}.")
        lines.append("Your army groups:")
        if groups.army_groups:
            for g in groups.army_groups:
                lines.append(f"  [{g['label']}] {g['count']}x {_short(g['tmpl'])} at ({g['cx']:.0f},{g['cy']:.0f})")
        else:
            lines.append("  (none)")
        lines.append("Your factories:")
        for g in groups.factory_groups:
            lines.append(f"  [{g['label']}] {g['count']}x {_short(g['tmpl'])} at ({g['cx']:.0f},{g['cy']:.0f})")
        lines.append("Visible enemies:")
        if groups.enemy_groups:
            for g in groups.enemy_groups:
                kind = "building" if any(h in g["tmpl"] for h in FACTORY_HINTS) else "unit"
                lines.append(f"  [{g['label']}] {g['count']}x {_short(g['tmpl'])} ({kind}) at ({g['cx']:.0f},{g['cy']:.0f})")
        else:
            lines.append("  (none in view)")
        return "\n".join(lines)

    def decide(self, obs: Observation, groups: Groups) -> list:
        text = self._state_text(obs, groups)
        resp = self.client.messages.create(
            model=self.model,
            max_tokens=2048,
            system=self.SYSTEM,
            output_config={"effort": self.effort},
            tools=self.TOOLS,
            messages=[{"role": "user", "content": text + "\n\nIssue your orders for this turn."}],
        )
        actions = []
        for block in resp.content:
            if block.type != "tool_use":
                continue
            name, a = block.name, block.input
            try:
                if name == "attack_move_group":
                    ids = groups.army_ids(int(a["group"]))
                    if ids:
                        actions.append(a_attackmove(ids, float(a["x"]), float(a["y"])))
                elif name == "attack_target":
                    ids = groups.army_ids(int(a["group"]))
                    tgt = groups.enemy_target(int(a["enemy"]))
                    if ids and tgt is not None:
                        actions.append(a_attack(ids, tgt))
                elif name == "move_group":
                    ids = groups.army_ids(int(a["group"]))
                    if ids:
                        actions.append(a_move(ids, float(a["x"]), float(a["y"])))
                elif name == "stop_group":
                    ids = groups.army_ids(int(a["group"]))
                    if ids:
                        actions.append(a_stop(ids))
                elif name == "train_unit":
                    ids = groups.factory_ids(int(a["factory"]))
                    if ids:
                        actions.append(a_queue_unit(ids, str(a["template"])))
            except (KeyError, ValueError, TypeError):
                continue
        return actions


def make_policy(name: str, model: str, effort: str):
    if name == "heuristic":
        return HeuristicPolicy()
    if name == "claude":
        return ClaudePolicy(model=model, effort=effort)
    raise SystemExit(f"unknown policy '{name}'")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--policy", choices=("heuristic", "claude"), default="heuristic")
    ap.add_argument("--model", default="claude-opus-4-8")
    ap.add_argument("--effort", default="low", choices=("low", "medium", "high"))
    ap.add_argument("--interval-frames", type=int, default=30,
                    help="decide once every N logic frames (30 = ~1s of game time)")
    ap.add_argument("--max-turns", type=int, default=0, help="stop after N decisions (0=forever)")
    args = ap.parse_args()

    if args.policy == "claude" and not (os.getenv("ANTHROPIC_API_KEY") or os.getenv("ANTHROPIC_AUTH_TOKEN")):
        print("NOTE: claude policy needs ANTHROPIC_API_KEY (or an ant auth profile). "
              "Falling back to heuristic.", file=sys.stderr)
        args.policy = "heuristic"

    policy = make_policy(args.policy, args.model, args.effort)
    print(f"connecting to {args.host}:{args.port}, policy={policy.name}", flush=True)
    client = BridgeClient(args.host, args.port)
    print("connected; waiting for observations", flush=True)

    turns = 0
    last_decide_frame = -10 ** 9
    t0 = time.time()
    try:
        while True:
            obs = client.next_observation()
            if obs is None:
                print("GAME_END", flush=True)
                break
            if obs.frame - last_decide_frame < args.interval_frames:
                continue
            last_decide_frame = obs.frame
            groups = Groups(obs)
            actions = policy.decide(obs, groups)
            client.send_actions(obs.frame, actions)
            turns += 1
            me = obs.my_player()
            print(f"turn {turns} frame {obs.frame}: army_groups={len(groups.army_groups)} "
                  f"enemies={len(groups.enemy_groups)} money={me.money if me else '?'} "
                  f"-> {len(actions)} orders", flush=True)
            if args.max_turns and turns >= args.max_turns:
                print(f"reached max-turns={args.max_turns}", flush=True)
                break
    except (EOFError, KeyboardInterrupt) as e:
        print(f"stopped: {e}  (after {turns} turns, {time.time()-t0:.1f}s)", flush=True)
    finally:
        client.close()


if __name__ == "__main__":
    main()
