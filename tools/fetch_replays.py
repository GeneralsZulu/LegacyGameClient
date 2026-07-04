#!/usr/bin/env python3
"""Fetch all replays from radarvan.

Walks the radarvan API and downloads every replay (.rep) and, optionally, the
parsed-JSON sidecar for each match:

  1. GET /api/matches/<count>?exclude_dev=true   -> {"matches": [MatchInfo, ...]}
  2. GET /api/replays/?match_id=<id>             -> [GameRecord, ...]
  3. download GameRecord.replay_presigned_url (and json_presigned_url with --json)

Auth mirrors the game client: the radarvan host expects

  X-API-Key: <ZULU_CLIENT_KEY>

The key is the GCP Secret Manager secret "zuluclientkey" (same one
cmake/zuluclientkey.cmake bakes into the build). This script reads it from the
ZULU_CLIENT_KEY env var, or fetches it via gcloud when the var is unset.

Examples:
  # last 500 non-dev matches, replays into ./replays
  ./fetch_replays.py --count 500

  # everything radarvan will give us, replays + parsed JSON, into /tmp/rv
  ./fetch_replays.py --count 100000 --json --out /tmp/rv
"""

import argparse
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

BASE_URL = "https://www.radarvan.com"
DEFAULT_TIMEOUT = 60


def get_client_key():
    """Return the radarvan API key (ZULU_CLIENT_KEY), preferring the env var."""
    key = os.environ.get("ZULU_CLIENT_KEY", "").strip()
    if key:
        return key
    try:
        key = subprocess.check_output(
            ["gcloud", "secrets", "versions", "access", "latest",
             "--secret=zuluclientkey"],
            stderr=subprocess.PIPE,
        ).decode().strip()
    except FileNotFoundError:
        sys.exit("ZULU_CLIENT_KEY is not set and gcloud is not installed.")
    except subprocess.CalledProcessError as e:
        sys.exit("Failed to read secret 'zuluclientkey' from GCP Secret "
                 "Manager:\n" + e.stderr.decode().strip())
    if not key:
        sys.exit("Secret 'zuluclientkey' is empty.")
    return key


def api_get(path, key, params=None, timeout=DEFAULT_TIMEOUT):
    """GET a radarvan API path with the X-API-Key header and parse JSON."""
    url = BASE_URL + path
    if params:
        url += "?" + urllib.parse.urlencode(params)
    req = urllib.request.Request(url, headers={"X-API-Key": key})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode())


def download(url, dest, timeout=DEFAULT_TIMEOUT):
    """Download a (presigned) URL to dest. Presigned URLs carry their own auth,
    so no API key header is sent. Returns the number of bytes written."""
    tmp = dest + ".part"
    # Presigned S3 URLs reject extra/unsigned headers, so use a bare request.
    req = urllib.request.Request(url)
    with urllib.request.urlopen(req, timeout=timeout) as resp, open(tmp, "wb") as f:
        total = 0
        while True:
            chunk = resp.read(1 << 16)
            if not chunk:
                break
            f.write(chunk)
            total += len(chunk)
    os.replace(tmp, dest)
    return total


def safe_name(s):
    """Make a filesystem-safe component out of an arbitrary string."""
    return "".join(c if c.isalnum() or c in "-_." else "_" for c in str(s))


def main():
    ap = argparse.ArgumentParser(description="Fetch all replays from radarvan.")
    ap.add_argument("--count", type=int, default=100000,
                    help="max matches to request (GET /api/matches/<count>); "
                         "default 100000 (effectively all).")
    ap.add_argument("--out", default="replays",
                    help="output directory (default: ./replays).")
    ap.add_argument("--include-dev", action="store_true",
                    help="include dev-build matches (default excludes them).")
    ap.add_argument("--json", action="store_true",
                    help="also download the parsed-JSON sidecar per replay.")
    ap.add_argument("--overwrite", action="store_true",
                    help="re-download files that already exist on disk.")
    ap.add_argument("--all-records", action="store_true",
                    help="download every replay record per match (default: "
                         "only the first).")
    ap.add_argument("--sleep", type=float, default=0.0,
                    help="seconds to sleep between downloads (be polite).")
    args = ap.parse_args()

    key = get_client_key()
    os.makedirs(args.out, exist_ok=True)

    exclude_dev = not args.include_dev
    print(f"Fetching up to {args.count} matches (exclude_dev={exclude_dev}) ...")
    data = api_get(f"/api/matches/{args.count}", key,
                   params={"exclude_dev": str(exclude_dev).lower()})
    matches = data.get("matches", []) if isinstance(data, dict) else data
    print(f"Got {len(matches)} matches.")

    n_rep = n_json = n_skip = n_err = 0
    for i, m in enumerate(matches, 1):
        match_id = m.get("id")
        if match_id is None:
            continue
        label = f"[{i}/{len(matches)}] match {match_id}"
        try:
            records = api_get("/api/replays/", key, params={"match_id": match_id})
        except urllib.error.HTTPError as e:
            print(f"{label}: replays lookup failed ({e.code}); skipping.")
            n_err += 1
            continue

        if not records:
            print(f"{label}: no replay records.")
            continue

        # One match can have multiple GameRecords; by default grab just the first.
        if not args.all_records:
            records = records[:1]

        for j, rec in enumerate(records):
            # One match can have multiple GameRecords; suffix when so.
            suffix = "" if len(records) == 1 else f"_{j}"
            stem = f"{match_id}{suffix}_{safe_name(m.get('map', 'map'))}"

            jobs = [(".rep", rec.get("replay_presigned_url"))]
            if args.json:
                jobs.append((".json", rec.get("json_presigned_url")))

            for ext, url in jobs:
                if not url:
                    continue
                dest = os.path.join(args.out, stem + ext)
                if os.path.exists(dest) and not args.overwrite:
                    n_skip += 1
                    continue
                try:
                    nbytes = download(url, dest)
                    if ext == ".rep":
                        n_rep += 1
                    else:
                        n_json += 1
                    print(f"{label}: {os.path.basename(dest)} ({nbytes} bytes)")
                except (urllib.error.URLError, OSError) as e:
                    print(f"{label}: failed {os.path.basename(dest)}: {e}")
                    n_err += 1
                if args.sleep:
                    time.sleep(args.sleep)

    print(f"\nDone. replays={n_rep} json={n_json} skipped={n_skip} errors={n_err}")
    print(f"Output: {os.path.abspath(args.out)}")


if __name__ == "__main__":
    main()
