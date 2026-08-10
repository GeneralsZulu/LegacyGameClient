# Replay reparse (income-by-source stats) — Windows setup

Reprocess the replay corpus through the headless engine to compute income-by-source
stats and upload CRC-clean results to cncstats.

## Why Windows (not Wine)

The sim forces the x87 FPU to 24-bit precision (`setFPMode`) for determinism, but
**Wine's math (esp. transcendentals via glibc) is not bit-identical to the Windows
MSVCRT/x87 the replays were recorded on.** Under Wine every replay desyncs ~1–2 min
in (verified: even the exact shipped v1.2.9 client desyncs identically). Run the
reparse on **native Windows** — same FP env as the recordings → CRC-clean.
Wine is fine for developing/testing the harness, just not the final pass.

## What you need

1. **`generalszh.exe`** — the reprocessing exe (headless build; crashing/logging OFF).
   Build on Windows with `PRESET=vc6` (or copy the cross-built one).
2. **A clean game data dir** — the VANILLA retail ZH 1.04 `.big` set:
   `INIZH MapsZH TerrainZH TexturesZH W3DZH W3DEnglishZH WindowZH EnglishZH AudioZH
   AudioEnglishZH SpeechZH SpeechEnglishZH MusicZH ShadersZH PatchZH` (+ `Data\`).
   **Do NOT include** `340_ControlBarPro*`, `400_ControlBarHD*`, `CustomContentMaps`,
   `!Hotkeys*`, `Gensec*` — those cause a duplicate-CommandButton crash.
   (Your existing install's base data already matches the recordings — verified:
   loaded iniCRC == replay iniCRC. Just exclude the mod bigs.)
3. **`bigs_by_version\Zulu_v*.big`** — the per-version data archives (ship as-is).
4. **`VERSION_MAP.csv`** — version → big mapping (in this folder).
5. **`replays_by_version\`** — the version-split corpus, copied to
   `%USERPROFILE%\Documents\Command and Conquer Generals Zero Hour Data\Replays\by_version\`
   (the engine resolves `-replay` relative to that Replays folder).

## Run

Dry run first (no upload) to check CRC-clean rates:
```powershell
.\reparse.ps1 -GameDir C:\zh-reparse\game -BigsDir C:\zh-reparse\bigs_by_version `
  -VersionMap .\VERSION_MAP.csv -Jobs 8
```
Then with upload:
```powershell
.\reparse.ps1 -GameDir C:\zh-reparse\game -BigsDir C:\zh-reparse\bigs_by_version `
  -VersionMap .\VERSION_MAP.csv -Jobs 8 -StatsUrl https://<cncstats-endpoint>
```

- Epoch is auto-detected from each replay's header (v121 / v128 / v130 / retail).
- Stats are exported/uploaded **only for CRC-clean (OK)** replays.
- Per-bucket `reparse-results\<bucket>.jsonl` + `summary.csv`.
- `verdict` per replay: `OK` / `DESYNC` / `INCOMPLETE` / `CANT_OPEN`; a file whose
  last line is `STARTED` = the process crashed on it.

## Determinism epochs (auto-detected; milestone-2, validated)

| Replay version | epoch | reverted sim behaviors |
|---|---|---|
| retail / ≤1.2.0 | retail | all below + pre-1.2.1 team timing (see note) |
| 1.2.1–1.2.7 | v121 | poison/flame/crate/scaffold (pre-1.2.8), surrender off |
| 1.2.8–1.2.9 | v128 | surrender off |
| 1.3.0–1.5.4 | v130 | legacy upgrade namekeys, community patch sim fixes off |
| 1.5.5+ | v155 | none (= current) |

Note on 1.5.4: the community patch and the stable-upgrade-id change landed
after 1.5.4 was cut and first shipped in **1.5.5**, but the commit that added
them named the epoch `V154` and gated it at 1.5.4+, so 1.5.4 replays were
re-simulated with the new behavior and desynced. Corrected in 1.5.6; the
epoch is now `v155`. There was never a 1.5.3. See
`docs/replay-data-versions.md`.

**Data matters as much as the epoch.** From 1.5.5 the shipped `Zulu.big`
carries the community balance patch, which also restructures the whole object
INI tree, so a pre-1.5.5 replay cannot re-simulate against it. Keep using the
per-version bigs below; the shipped client now does the same thing through
`ReplayData\` + Replay Theater.

Note: pre-1.2.1 team-create timing is not reverted (retail predates the change;
≤1.2.0 zulu builds with it aren't cleanly reproducible anyway — not in the corpus).

## Dev / prerelease / devmisc buckets (now handled)

These are dev builds; their data was identified by iniCRC and mapped in VERSION_MAP.csv:

| bucket | data (big) | why |
|---|---|---|
| `zulu_1.1_dev_20260520`, `zulu_1.2_dev_20260527`, `zulu_1.4_dev_20260510` | `Zulu_v1.2.0.big` | iniCRC C65B21AB = v1.2.0 assets (verified) |
| `zulu_1.4_dev_20260508` | `Zulu_v1.1.0.big` | iniCRC D78B2801 = v1.1.0 assets (verified) |
| `prerelease_1.05_feb2025`, `devmisc_*` | (no `-mod`) | iniCRC FEAAE3F3 = retail base data |

Epoch auto-detects to RETAIL for these (non-semantic version strings), which is correct — all are pre-1.2.8, and V121≡RETAIL in the gates. Under Wine they hit the FP wall (~1-2 min) like every other bucket; on Windows they should be CRC-clean where the original game was.

## Notes / caveats

- Re-running a bucket re-processes all its replays (appends to the JSONL; last line
  per file wins) and would re-upload OK ones. Do the dry run first; only add
  `-StatsUrl` once you're happy with the clean rate.
- Large buckets use the wildcard `-replay by_version\<bucket>\*.rep`; the `-jobs`
  worker pool caps concurrency. The 1.5 GB data set is shared via the OS page
  cache across workers, so `-jobs` is bounded by RAM, not just cores.
- The reprocessing exe reports version 1.4.601 (default) — harmless; the version
  check is non-blocking and the epoch comes from each replay, not the exe.
