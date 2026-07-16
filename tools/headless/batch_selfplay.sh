#!/usr/bin/env bash
#
# batch_selfplay.sh - generate self-play AI-vs-AI games with the headless
# skirmish runner, in parallel, collecting winners to a JSONL file and keeping
# the recorded .rep of each game.
#
# This is the "run games as fast as possible, batchable" half of the LLM-plays-
# Generals project: each game is a deterministic (map, factions, seed) triple,
# runs headless and uncapped, and drops a [HEADLESS RESULT] line + a .rep.
#
# STATUS: the runner it drives is built but not yet validated end-to-end. Run a
# single game by hand first (see docs/headless-skirmish.md) before batching.
#
# Requirements: the exe must sit in a working dir with the full retail ZH data
# + Zulu.big (a real install / wine prefix / VM guest share).
#
# Usage:
#   RUN="wine /path/to/install/generalszh.exe" \
#   OUT=./selfplay_out \
#   ./tools/headless/batch_selfplay.sh 32          # 32 games
#
# Env:
#   RUN   command prefix to launch the exe (default: wine ./generalszh.exe)
#   OUT   output dir for results.jsonl + copied .reps (default: ./selfplay_out)
#   PAR   max concurrent games (default: nproc/2, leaves cores for the desktop)
#   MAPS  space-separated map names (default: a small tournament set)
#   REPLAY_DIR  where the game writes .reps (to harvest them); optional

set -u

# RUN must launch the exe from an install dir; MOD is the absolute Z:\ path to
# Zulu.big (wine maps Z:\ to /). Example:
#   RUN="wine ./generalszh_test.exe" MOD='Z:\home\hrich\zrun\game\Zulu.big'
RUN="${RUN:-wine ./generalszh.exe}"
MOD="${MOD:-Zulu.big}"
OUT="${OUT:-./selfplay_out}"
PAR="${PAR:-$(( $(nproc) / 2 ))}"
NGAMES="${1:-16}"
# Short map-cache forms (maps/<name>.map); the engine expands to maps\<name>\<name>.map.
MAPS="${MAPS:-maps/alpine assault.map|maps/tournament desert.map|maps/tournament island.map|maps/final crusade.map}"

# Faction pairings to sample from. Extend freely.
PAIRS=(
  "USA:China" "China:GLA" "GLA:USA"
  "USA:USA" "China:China" "GLA:GLA"
)
DIFFS=("hard" "tactical")

mkdir -p "$OUT/reps"
RESULTS="$OUT/results.jsonl"
: > "$RESULTS"

# Split MAPS on '|' into an array (map names contain spaces).
IFS='|' read -r -a MAP_ARR <<< "$MAPS"

echo "batch_selfplay: $NGAMES games, up to $PAR concurrent"
echo "  RUN=$RUN"
echo "  OUT=$OUT"

run_one() {
  local idx="$1"
  local map="${MAP_ARR[$(( idx % ${#MAP_ARR[@]} ))]}"
  local pair="${PAIRS[$(( idx % ${#PAIRS[@]} ))]}"
  local fa="${pair%%:*}"
  local fb="${pair##*:}"
  local diff="${DIFFS[$(( idx % ${#DIFFS[@]} ))]}"
  local seed="$(( 1000 + idx ))"
  local log="$OUT/game_${idx}.log"

  # shellcheck disable=SC2086
  WINEDEBUG=-all $RUN -win -headless -mod "$MOD" -skirmish \
    -map "$map" -seed ${seed} \
    -slot faction=${fa},difficulty=${diff} \
    -slot faction=${fb},difficulty=${diff} > "$log" 2>&1

  # Parse the winner line into one JSON record.
  local res
  res="$(grep -m1 '^\[HEADLESS RESULT\] outcome=' "$log" 2>/dev/null)"
  local outcome frame winner
  outcome="$(sed -n 's/.*outcome=\([a-z]*\).*/\1/p' <<< "$res")"
  frame="$(sed -n 's/.*end_frame=\([0-9]*\).*/\1/p' <<< "$res")"
  winner="$(sed -n 's/.*winner_side=\([A-Za-z]*\).*/\1/p' <<< "$res")"
  printf '{"idx":%d,"map":"%s","a":"%s","b":"%s","diff":"%s","seed":%d,"outcome":"%s","end_frame":%s,"winner_side":"%s"}\n' \
    "$idx" "$map" "$fa" "$fb" "$diff" "$seed" "${outcome:-unknown}" "${frame:-0}" "${winner:-}" >> "$RESULTS"

  # Harvest the most recent .rep if a replay dir was given.
  if [ -n "${REPLAY_DIR:-}" ] && [ -d "$REPLAY_DIR" ]; then
    local newest
    newest="$(ls -t "$REPLAY_DIR"/*.rep 2>/dev/null | head -1)"
    [ -n "$newest" ] && cp "$newest" "$OUT/reps/game_${idx}.rep" 2>/dev/null
  fi
  echo "  game $idx done: ${outcome:-?} winner=${winner:-?} frame=${frame:-?} ($fa vs $fb, $map, $diff)"
}
export -f run_one
export RUN MOD OUT RESULTS REPLAY_DIR
export MAP_ARR_STR="$MAPS"

# xargs fan-out. Re-derive MAP_ARR/PAIRS/DIFFS inside the subshell via env.
seq 0 $(( NGAMES - 1 )) | xargs -P "$PAR" -I{} bash -c '
  IFS="|" read -r -a MAP_ARR <<< "$MAP_ARR_STR"
  PAIRS=(USA:China China:GLA GLA:USA USA:USA China:China GLA:GLA)
  DIFFS=(hard tactical)
  '"$(declare -f run_one)"'
  run_one {}
'

echo "done. results: $RESULTS  ($(wc -l < "$RESULTS") games)"
