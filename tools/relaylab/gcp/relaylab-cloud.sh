#!/bin/bash
# relaylab-cloud.sh all: the whole tier 2 sequence: provision, T2-FULL8,
# T2-HOST-BAD, teardown. Any scenario failing still tears down; the exit
# code reflects the worst scenario result.
set -uo pipefail
cd "$(dirname "$0")"

[ "${1:-}" = all ] || { echo "usage: $0 all"; exit 1; }

./up.sh || exit 1
RC=0
./run.sh T2-FULL8 600      || RC=1
./run.sh T2-HOST-BAD 600   || RC=1
./down.sh
exit $RC
