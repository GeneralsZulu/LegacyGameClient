<#
.SYNOPSIS
  Batch-reparse Generals Zero Hour replays for income-by-source stats, one
  version bucket at a time, on native Windows (required for CRC-clean
  determinism -- Wine's floating-point math diverges from the recording env).

.DESCRIPTION
  For each version-bucket folder under -ReplaysSubdir (which must live UNDER the
  game's user-data Replays folder, since the engine resolves -replay relative to
  it), this runs the headless reprocessing exe with the matching per-version
  Zulu.big (via -mod) and lets the engine auto-detect the determinism epoch from
  each replay's header. Each replay's verdict is appended to a per-bucket JSON-
  lines log; stats are only exported/uploaded for CRC-clean (OK) replays.

.NOTES
  - Run from any dir; pass -GameDir explicitly. The exe is launched with its CWD
    set to -GameDir so it finds the base .big files.
  - -GameDir must contain the VANILLA retail ZH 1.04 .big set + the reprocessing
    generalszh.exe. Do NOT include ControlBarPro/HD/Gensec/Hotkeys mod .big files
    -- they cause a duplicate-CommandButton crash.
  - Dry run first (omit -StatsUrl) to check CRC-clean rates before uploading.
#>
param(
  [Parameter(Mandatory=$true)][string]$GameDir,        # dir with vanilla ZH bigs + generalszh.exe
  [Parameter(Mandatory=$true)][string]$BigsDir,        # dir with Zulu_vX.Y.Z.big files
  [Parameter(Mandatory=$true)][string]$VersionMap,     # path to VERSION_MAP.csv
  [string]$ReplaysSubdir = "by_version",               # subpath under <userdata>\Replays\ holding the version buckets
  [string]$UserDataReplays = "$env:USERPROFILE\Documents\Command and Conquer Generals Zero Hour Data\Replays",
  [string]$StatsUrl = "",                              # cncstats URL; empty = no export/upload (dry run)
  [int]$Jobs = [Environment]::ProcessorCount,
  [string]$OutDir = "reparse-results"
)

$ErrorActionPreference = "Stop"
$exe = Join-Path $GameDir "generalszh.exe"
if (!(Test-Path $exe)) { throw "generalszh.exe not found in $GameDir" }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# version -> big filename map (from VERSION_MAP.csv). Blank big => run with no -mod (retail base data).
$map = @{}
Import-Csv $VersionMap | ForEach-Object { $map[$_.version] = $_.big_file }

$bucketRoot = Join-Path $UserDataReplays $ReplaysSubdir
if (!(Test-Path $bucketRoot)) { throw "Replay buckets not found: $bucketRoot (copy replays_by_version there)" }

# Map a bucket folder name -> key in the CSV. Dev/prerelease/devmisc buckets have
# explicit rows keyed by the full folder name (empty big_file => no -mod / retail base).
function Resolve-Version([string]$bucket) {
  if ($map.ContainsKey($bucket))        { return $bucket }         # dev / prerelease / devmisc rows
  if ($bucket -match '^\d+\.\d+\.\d+$') { return $bucket }         # 1.2.9
  if ($bucket -like 'retail*')          { return 'retail_1.04' }   # retail bucket -> no-mod row
  return $null                                                     # truly unknown: skip
}

$summary = @()
Get-ChildItem -Directory $bucketRoot | Sort-Object Name | ForEach-Object {
  $bucket = $_.Name
  $reps = Get-ChildItem -File -Filter *.rep $_.FullName
  if ($reps.Count -eq 0) { return }

  $ver = Resolve-Version $bucket
  if ($null -eq $ver -or -not $map.ContainsKey($ver)) {
    Write-Warning "SKIP bucket '$bucket' ($($reps.Count) reps): no version->big mapping. Handle manually."
    $summary += [pscustomobject]@{ bucket=$bucket; reps=$reps.Count; status="SKIPPED (no mapping)" }
    return
  }

  $big = $map[$ver]
  $modArgs = @()
  if ($big) {
    $bigPath = Join-Path $BigsDir $big
    if (!(Test-Path $bigPath)) { Write-Warning "SKIP '$bucket': big not found: $bigPath"; return }
    $modArgs = @("-mod", $bigPath)
  }

  $resultLog = Join-Path (Resolve-Path $OutDir) "$bucket.jsonl"
  $wildcard  = Join-Path $ReplaysSubdir "$bucket\*.rep"   # relative to <userdata>\Replays\

  $args = @("-headless", "-jobs", $Jobs, "-resultLog", $resultLog) + $modArgs
  if ($StatsUrl) { $args += @("-exportStats", "-statsUrl", $StatsUrl) }
  # epoch is auto-detected from each replay's header; pass -replayEpoch here only to override.
  $args += @("-replay", $wildcard)

  Write-Host "=== bucket $bucket : $($reps.Count) replays, big=$(if($big){$big}else{'(retail base)'}), jobs=$Jobs ===" -ForegroundColor Cyan
  Push-Location $GameDir
  try { & $exe @args | Write-Host } finally { Pop-Location }

  # tally verdicts (last line per file wins)
  $verdicts = @{}
  if (Test-Path $resultLog) {
    Get-Content $resultLog | ForEach-Object {
      try { $o = $_ | ConvertFrom-Json; $verdicts[$o.file] = $o.verdict } catch {}
    }
  }
  $ok = ($verdicts.Values | Where-Object { $_ -eq 'OK' }).Count
  $de = ($verdicts.Values | Where-Object { $_ -eq 'DESYNC' }).Count
  $other = $verdicts.Count - $ok - $de
  Write-Host ("    OK=$ok DESYNC=$de OTHER=$other  -> $resultLog") -ForegroundColor Green
  $summary += [pscustomobject]@{ bucket=$bucket; reps=$reps.Count; OK=$ok; DESYNC=$de; OTHER=$other }
}

Write-Host "`n===== SUMMARY =====" -ForegroundColor Yellow
$summary | Format-Table -AutoSize
$summary | Export-Csv -NoTypeInformation (Join-Path $OutDir "summary.csv")
Write-Host "Per-bucket JSONL + summary.csv in $OutDir"
