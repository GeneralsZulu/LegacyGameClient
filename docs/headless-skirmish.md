# Headless AI-vs-AI Skirmish Runner (phase 1)

Start a fresh skirmish match from the command line with **no GUI and no human
player**, run it headless and fast (~30x real-time), and read the winner off
stdout. This is the foundation for fast batch/CI testing and for generating
self-play data for an LLM/agent to learn from.

It reuses the engine's existing message-driven start path: the runner populates
`TheSkirmishGameInfo` programmatically and fires the same `MSG_NEW_GAME` a human
does from the skirmish menu, so the match records to a `.rep` and stays
deterministic and replay-compatible. Nothing here runs unless `-skirmish` is
passed; the shipping game is byte-identical without it.

**Status: validated end-to-end (2026-07-15).** Runs on Linux under wine (no VM):
e.g. `USA-Hard vs China-Hard` on Alpine Assault plays to victory at frame 50201
(~28 game-minutes) in under a minute of wall time and exits cleanly. Determinism
note: a `.rep` recorded under wine will not replay bit-exact on Windows (RNG
differs across platforms), which does not matter for playing fresh games.

## Flags

| flag | meaning |
|------|---------|
| `-skirmish` | enable the headless skirmish runner |
| `-map maps/<name>.map` | map to play. Use the short `maps/<name>.map` form; the engine expands it to `maps\<name>\<name>.map`. The `<name>` must exist in the map cache. |
| `-slot <spec>` | add one AI slot; repeat once per player, in order |
| `-seed <n>` | fixed logic seed (default `0` = reproducible); vary it for run diversity |
| `-simfps <n>` | render/sim pacing (default `1000` = ~30x real-time). The engine clamps to `[1,1000]`; values above 1000 fall back to real-time, so do not exceed 1000. |
| `-headless` | no window/render (required for batch) |
| `-mod Zulu.big` | required: the fork loads `SkirmishScripts.scb` from Zulu.big |

`<spec>` is comma-separated `key=value` pairs:

- `faction=` `USA` \| `China` \| `GLA` \| `random` \| `<TemplateName>`  (default `random`).
  Base sides match on the generic faction; a template name selects a sub-general.
- `difficulty=` `easy` \| `medium` \| `hard` \| `tactical`  (default `hard`).
  `tactical` selects the fork's Tactical AI (`SLOT_TACTICAL_AI`).
- `team=` 0-based team number, or `-1` for none  (default `-1`)
- `color=` color index, or `-1` for auto  (default `-1`)

A bad map / faction / too-many-slots config prints a `[HEADLESS SKIRMISH] ERROR`
line and exits nonzero (fail-fast, for batch use).

## Output

On game over, `VictoryConditions` emits:

```
[HEADLESS RESULT] outcome=victory end_frame=N winner_index=I winner_side=S winner_name=playerN
[HEADLESS RESULT] ally|defeated index=I side=S name=playerN
```

(or `outcome=draw`), then the runner quits the process (clean exit 0). The match
also auto-records a `.rep` in the replay dir.

## Running

The build produces `build/docker-vc6/GeneralsMD/generalszh.exe`. It needs the
full retail Zero Hour data set + `Zulu.big` on the working path.

**Under wine on Linux** (validated path; `/home/hrich/zrun/game` is a working
install). `-mod` resolves relative to the user-data dir, so pass an absolute
`Z:\...` path (wine maps `Z:\` to `/`):

```
cd /home/hrich/zrun/game
WINEDEBUG=-all wine ./generalszh.exe -win -headless \
  -mod "Z:\home\hrich\zrun\game\Zulu.big" -skirmish \
  -map "maps/alpine assault.map" -seed 1 \
  -slot faction=USA,difficulty=hard -slot faction=China,difficulty=hard
```

**In the VMware Windows guest** (from the install dir):

```
generalszh.exe -win -headless -mod Zulu.big -skirmish ^
  -map "maps/alpine assault.map" -seed 1 ^
  -slot faction=USA,difficulty=hard -slot faction=China,difficulty=hard
```

See `tools/headless/batch_selfplay.sh` for the parallel batch generator.
