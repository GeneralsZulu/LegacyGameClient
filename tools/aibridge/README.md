# AIBridge agent — an LLM (or heuristic) plays Generals

This is the external side of the AIBridge: a Python process that connects to a
running game over TCP, reads the game state, decides on orders, and sends them
back as unit commands. It's the "Phase 2" half of the llm-plays-generals project
(the engine side is `GeneralsMD/.../Common/AIBridge/`).

## Pieces

- `bridge_client.py` — wire protocol only. Decodes the observation stream
  (players, objects, positions, HP, ownership) and encodes action batches
  (move / attack-move / attack / stop / construct / queue-unit). No game logic.
- `agent.py` — the loop and the policies:
  - **heuristic** (default): rules only, no API key. Rallies every army unit and
    attack-moves toward the enemy. Proves the whole observe→decide→act pipe and
    gives a baseline aggressive bot.
  - **claude**: Claude Opus 4.8 with tool-use. The model is shown the state as
    labeled unit/enemy groups and calls action tools; the agent maps group labels
    back to unit ids. Needs `ANTHROPIC_API_KEY` (or an `ant auth login` profile).

The two policies share the exact same observation → action path, so a game that
works under `heuristic` works under `claude` the moment a key is present.

## Running it

1. Start a headless game with the bridge listening on a slot. The engine flags:
   `-aibridgeport <port> -aibridgeslot <lobby-slot>` (added alongside the
   `-skirmish`/`-slot`/`-simfps` headless flags). The bridge listens on that TCP
   port and drives the player in that lobby slot.

2. Point the agent at it:
   ```
   # heuristic (no key needed) — proves the pipe, plays a baseline bot
   /home/hrich/llm-generals-agent/bin/python agent.py --port <port> --policy heuristic

   # Claude drives it (drop the key in and flip the flag)
   ANTHROPIC_API_KEY=sk-... /home/hrich/llm-generals-agent/bin/python agent.py \
       --port <port> --policy claude --effort low
   ```

Useful flags: `--interval-frames N` (decide every N logic frames; 30 ≈ 1s of game
time), `--max-turns N` (stop after N decisions), `--model`, `--effort`.

## Notes / gotchas

- The bot's player *index* is not the lobby *slot*: slot 0's player is usually
  index 2 (neutral/civilian take the low indices). The engine resolves this and
  reports the real `bot_index` in HELLO/OBSERVATION; the agent uses that.
- **Order persistence vs. the scripted AI.** The scripted AI co-drives the
  bridged slot every frame, so at a 1s decision cadence it could countermand a
  bridge order before the next tick. The engine now **suppresses the scripted
  AI's army micro for the bridged slot** (`AIBridge_isControllingPlayer()` gates
  the TacticalAI combat directives in `AISkirmishPlayer::update()`); economy,
  base building, and production keep running. This suppression only affects the
  fork's TacticalAI dispatch, so **run the bridged slot as
  `difficulty=tactical`** — e.g. `-aibridgeslot 0 -slot faction=USA,difficulty=tactical`.
  A non-tactical (`hard`) slot gets its army orders from the stock skirmish
  scripts instead, which this gate does not touch; there the heuristic's
  every-tick re-issue is what keeps the army moving.
- Wine RNG differs from Windows, so games recorded under wine won't replay
  bit-exact on Windows. That's fine for *playing* new games (which is all this
  does); it only matters for deterministic replay.

## Environment

Python venv with the Anthropic SDK: `/home/hrich/llm-generals-agent`
(`pip install anthropic`; currently 0.116.0). The heuristic policy imports no
third-party packages, so it runs under any Python 3.10+.
