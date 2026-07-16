# Kickoff: LLM plays Generals (fast sim + external control API)

Paste this into a fresh session to start the project. It is self-contained; the
project memory `project_llm_plays_generals.md` holds the same context.

---

## The goal

Two things, in service of one outcome — **an LLM playing skirmish Command & Conquer
Generals: Zero Hour**:

1. **Run games as fast as possible**, headless and batchable, for testing and for the
   LLM to learn from many games quickly.
2. **An API an external process (the LLM) uses to observe the game and control its
   units** — read the relevant state, issue orders, build, use abilities.

The brief is deliberately open ("go crazy with it"). Explore ambitiously. The one hard
constraint: **keep the stable base stable.** zulu/main ships to real players; nothing
here may destabilize it.

## Where we are (the stable base)

- Repo: `/home/hrich/gastown/GeneralsGameCode`. Remote `zulu` = GeneralsZulu/LegacyGameClient.
- zulu/main is at the 2026-07-14 state (MD laser lock PR #92 is the tip). All recent AI
  work — object-upgrade purchasing, laser lock — lives in `AISkirmishPlayer`.
- Build: `./scripts/docker-build.sh --target z_generals` (target is `z_generals`). Header
  edits need `--clean`. **GeneralsMD only** (never the `Generals/` dir). VC6 for-loop
  scoping rules apply (a for-init variable leaks into the enclosing scope).

## Prior art — read these BEFORE writing anything; do not reinvent

- **`feature/external-ai-bridge` branch (commit bc03e8c7d).** Already contains
  `Core/GameEngine/{Include,Source}/Common/AIBridge/AIBridge.{h,cpp}` — ~1200 lines of a
  TCP observation/action interface — plus `tools/aibridge/observe.py` (a 270-line Python
  client) and `docs/external-ai-bridge.md`. **It is 161 commits behind main and was never
  live-tested.** Treat it as a design to port/refresh onto current main, NOT something to
  `git merge`. Read `docs/external-ai-bridge.md` and the AIBridge source first.
- **Observer stream** (`Core/GameEngine/{Include,Source}/GameNetwork/LANObserverStream.{h,cpp}`):
  the game already streams live state over TCP to observers. This is the state-export
  precedent. Memory: `reference_lan_observer_arch.md`.
- **Headless** (memory `project_headless_replay.md`): `generalszh.exe -headless -replay X.rep
  -resultLog out.json` runs a replay headless and emits a verdict/winner. **Known gap: there
  is no clean way to START a fresh AI-vs-AI (or LLM-vs-AI) match headless** — only replay is
  wired. Closing that is the foundation for everything else.
- **The deterministic command path** (memory `project_ai_object_upgrades.md`, the two-path
  finding): the ONLY replay-safe, lockstep-safe way to issue an order is to produce the same
  GameMessage a human does — `Object::doCommandButton*`, `AIGroup::group*`,
  `GameLogicDispatch` handling `MSG_*`, `TEAM_USE_COMMANDBUTTON_ABILITY`. The action API must
  go through this, or it desyncs and can't be recorded.
- **Fast-sim lever:** `TheFramePacer->setFramesPerSecondLimit(...)` at
  `GameEngine.cpp:712`; `TheGlobalData->m_framesPerSecondLimit`. Logic is a fixed 30Hz
  (`LOGICFRAMES_PER_SECOND`); fast sim means uncapping and decoupling from render/headless
  while keeping the 30Hz logic tick's determinism.
- **VM validation:** memory `reference_vmware_guest_automation.md` — the many hard-won
  lessons (run the `vc6-releaselog` build for measurement, keep `Zulu.big`, never
  `revertToSnapshot`, `ReleaseLog` not `DEBUG_LOG`, save screenshots to the host, verify the
  match is real). There is also a KNOWN pre-existing crash in the optimized release build
  (`project_release_build_gameengine_crash.md`) — use the releaselog build.

## Non-negotiable guardrails

- **Work on a feature branch off zulu/main. Never commit to main.** (Bill commits/pushes
  only when he explicitly asks; author is always `Bill Rich <bill.rich@gmail.com>`, no Claude
  co-author.)
- **Gate all of this behind a mode/flag** so the shipping game is byte-identical when the
  feature is off.
- **Determinism is sacred.** Anything touching the simulation uses frame-count timing, a
  `ZULU_AI_FEATURE_*`-style version gate for replay compat, and never wall-clock or RNG off
  the shared logic stream. A fresh replay must be CRC-clean and an old replay must still play
  back bit-exact.
- **Validate each phase in-game**, don't trust "it compiles." Use instrumented counts, not
  eyeballing.

## Suggested phasing (scope each, build each, validate each, keep base green)

1. **Fast headless AI-only skirmish runner.** Start an AI-vs-AI match headless from the
   command line (map, factions, seed), run the logic uncapped with no render, emit a result
   JSON, exit. This is the foundation and it also replaces the throwaway `-zzzauto` harness
   agents kept rebuilding. Get determinism + batchability right here first.
2. **Port/refresh the AIBridge transport** onto current main (the TCP observe/action socket).
3. **Observation API:** serialize the relevant state for one player — own units (id, type,
   pos, health, state), resources, production queues, visible enemies, map/terrain summary —
   to JSON over the socket. Model it on what the observer stream and cncstats already extract.
4. **Action API:** move / attack / attack-move / build / train / use-ability / set-rally,
   issued through the deterministic command path so it records and replays cleanly.
5. **Wire an actual LLM** to drive one player: an external loop that reads observation, asks
   the model for actions, submits them. Start dumb (rules or a tiny loop), prove the pipe,
   then hand the wheel to the model.

## First move for the fresh session

Don't build yet. First: read the AIBridge branch source + `docs/external-ai-bridge.md`, read
the four memories named above, and come back with a short scoping note — what the AIBridge
already does vs. what's missing, whether to port it or rewrite, and a concrete plan for
phase 1 (the headless AI-only runner). Then confirm direction before writing engine code.
