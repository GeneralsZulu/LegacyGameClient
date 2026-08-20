# Telemetry and Replay Uploads

## What it is

The Zulu client uploads replays, map files, and end-of-game stats
to a backend ("radarvan") so post-game analysis, leaderboards, and
map history blurbs work. The pipeline:

- **Replay upload** (commit `371d89b5d`): on game-over, the host
  POSTs the just-recorded `.rep` to `m_replayUrl` (default
  `https://www.radarvan.com/api/upload_replay`) as
  `multipart/form-data`.
- **Map upload** (commit `d90bbedb8`): on game-over, the host
  POSTs the `.map` file (and its sibling assets) so the backend
  can render previews / parse start positions without scraping
  every player's install.
- **Stats exporter** (commit `5ba918ba2`): in-game and end-of-game
  events get serialized and posted as cncstats-format payloads for
  leaderboards.
- **`ZUTG` trailer** (PR #14 — `076cfc2da`): the radarvan replay
  upload buffer gets an 8-byte trailer appended in memory before
  POST. Layout (little-endian): `"ZUTG"` magic (4 B) + version
  `0x0001` (2 B) + payload-len `0x0000` (2 B). The on-disk `.rep`
  is untouched — the trailer exists only in the upload buffer.

## Why

- **Replay upload** lets the radarvan site show recent games per
  map, per player, per general matchup without players having to
  hand-share `.rep` files.
- **Map upload** is what enables the lobby `map_summary` blurb,
  the labeled Discord preview, and the in-progress observer
  flow (LAN observer mode reads the host's `mapPreviews/<map>.tga`
  rather than the `.map`, but the backend's preview is what the
  Discord image starts from).
- **Stats exporter** is the leaderboard / ELO data feed.
- **`ZUTG` trailer** lets the radarvan server distinguish replays
  uploaded directly by the Zulu client from re-uploads of the
  same `.rep` file by third-party tools (notably gentool's
  parser, which whitelists `CE`/`CM`/`CH` slot types and rewrites
  the new `CT` Tactical AI slot to `X`, producing a "missing
  computer player" replay that the backend would otherwise have
  no way to tell apart from the Zulu-client copy).

## Code surface

- `GeneralsMD/Code/GameEngine/Include/Common/StatsUploader.h` and
  `Source/Common/StatsUploader.cpp` — the WinINet `multipart/form-data`
  POST helpers, plus the `AppendZuluUploadTag` helper that builds
  the `ZUTG` trailer. About 700 lines total.
- `GeneralsMD/Code/GameEngine/Include/Common/StatsExporter.h` plus
  the cncstats serialization in `StatsExporter.cpp`.
- `GeneralsMD/Code/GameEngine/Source/Common/GlobalData.cpp` — the
  default URLs (`m_replayUrl`, `m_balanceTeamsUrl`,
  `m_mapSummaryUrl`).
- `GeneralsMD/Code/GameEngine/Source/Common/CommandLine.cpp` —
  matching `-replayUrl`, `-balanceTeamsUrl`, `-mapSummaryUrl`
  overrides so QA / staging can redirect away from production.
- `cmake/zuluclientkey.cmake` — bakes the build-time radarvan auth
  key into `ZuluClientKey.h`, which `StatsUploader.cpp` injects as
  an `X-API-Key` header.
- `GeneralsMD/Code/GameEngine/Source/Common/Recorder.cpp` — invokes
  the upload helpers on game-over, gated on `>= 2 human players`
  (commit `9bc0a2a8d`).

## Connection-failure logs (online lobby)

Everything above fires at game-over. The one failure that never
reaches it is the one players report most: "I couldn't host" /
"I couldn't join". No match starts, so nothing is ever uploaded,
and the client's `ReleaseLog.txt` is rewritten on the next launch
— by the time anyone asks, the evidence is gone on both ends.

The online lobby therefore ships its own log the moment an attempt
fails (`coordinatorReportFailure` in `LanLobbyMenu.cpp`):

- **Triggers** — `connect` (socket/DNS failure before the session
  starts), `host` (the coordinator refused or errored the host
  request), `host-ack` (the listing was never confirmed within
  15 s), `join` (after the two automatic retries are exhausted),
  `version` (version-mismatch refusal), `closed` (the coordinator
  dropped a live session), `coordinator` (anything else).
- **What is sent** — a `Coordinator FAILURE (<phase>)` summary line
  (endpoint, nick, state, last error, discovered public addresses,
  hosted/join ids, retry count) is written to `ReleaseLog.txt`, then
  that log — plus the debug log in logging builds — is POSTed to the
  same cncstats `/logs` endpoint the match telemetry uses, on the
  same background worker. Nothing blocks the UI.
- **How it is keyed** — `X-Game-Seed` is not a seed here (there is
  no match); it is a per-UTC-day bucket, `connfail-YYYYMMDD`, and
  `X-Player` is `<nick>-<HHMMSS>-<phase>`. One evening's failures
  across all players come back in a single request:

      curl -H "Authorization: Bearer $KEY" \
        "https://cncstats.computersrfun.org/get_logs?seed=connfail-20260820" -o logs.zip

- **Bounded** — at most four uploads per visit to the lobby, so a
  player stuck in a retry loop can't spam the server.
- **Testing** — `-logsUrl http://127.0.0.1:9099/logs` redirects the
  upload at a local listener, and `-coordhost <host:port>` points the
  lobby at a dead or local coordinator to provoke each phase.

The server half is `pkg/serverlog` in cncstats: the coordinator's
log lines (tagged `component=coordinator`, including every rejection
it sends a client) are teed from container stderr into a per-day file
on the logs volume, so a redeploy no longer erases the record of a
player's failed session.

## Concerns

- **Privacy.** The replay and stats uploads include player names,
  game outcome, and the chosen factions / colors. Map files
  uploaded include the host's local copy verbatim. This is the
  same surface as gentool / GameSpy stats; players who don't want
  it can run an unsigned build with the URLs blanked, or use the
  command-line overrides.
- **Gating on 2+ humans.** Solo / AI-only games don't upload
  (commit `9bc0a2a8d`). This is policy, not a network concern;
  match results for AI-only games aren't leaderboard-meaningful
  and would just pollute the data.
- **Mismatch risk.** None. Uploads run after game-over from the
  host only. The `ZUTG` trailer is appended to a malloc'd upload
  buffer; the on-disk `.rep` file is never modified, so a replay
  played back locally is byte-identical to the upload-minus-trailer.
- **Backwards compatibility.**
  - Replay format on disk is unchanged.
  - `ZUTG` is a forward-compatible block: the server treats
    files lacking the trailer as third-party re-uploads.
  - The `ZULU` magic + feature-version block in the replay header
    (separate from `ZUTG` — that one is in the header, this one
    is appended to the buffer) is what gates AI-feature
    compatibility across Zulu binary versions.
- **Auth failure mode.** WinINet errors and HTTP non-2xx
  responses are logged to debug chat in `-zulu_debug` mode and
  otherwise swallowed. Uploads are not retried. A flaky network
  means that match's data is lost; the game itself is unaffected.
