#!/usr/bin/env python3
"""Build the per-release data archives used to play back old replays.

Reads installer/replay_data_versions.csv and, for every row that names a
git_ref, packs that ref's assets/ tree into <out-dir>/<big_file> using the same
packer that builds the current Zulu.big. Rows sharing a big_file are built
once.

The archives are reconstructed from git rather than checked in: they are ~95%
identical to each other, so storing 18 of them would add ~100 MB of near-
duplicate binaries to the repo for something git can regenerate exactly.

Usage: build_replay_bigs.py <csv> <out-dir>
"""

import os
import subprocess
import sys
import tarfile
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pack_big


def read_rows(csv_path):
    rows = []
    with open(csv_path, encoding='utf-8') as f:
        for line in f:
            line = line.rstrip('\n')
            if not line.strip() or line.lstrip().startswith('#'):
                continue
            parts = line.split(',')
            if parts[0] == 'match_kind':
                continue
            if len(parts) < 4:
                sys.exit("malformed row: %s" % line)
            rows.append({'big_file': parts[2].strip(), 'git_ref': parts[3].strip()})
    return rows


def resolve_assets_treeish(git_ref):
    """A git_ref is either a commit (take its assets/ subtree) or, for the rows
    carried over from VERSION_MAP.csv, the assets tree oid itself."""
    kind = subprocess.run(['git', 'cat-file', '-t', git_ref],
                          capture_output=True, text=True)
    if kind.returncode != 0:
        sys.exit("git_ref '%s' does not resolve in this repository" % git_ref)
    return git_ref if kind.stdout.strip() == 'tree' else '%s:assets' % git_ref


def extract_assets(git_ref, dest):
    """Unpack the ref's assets tree into dest. git archive streams a tar, which
    keeps this independent of the working tree's state."""
    proc = subprocess.Popen(['git', 'archive', '--format=tar', resolve_assets_treeish(git_ref)],
                            stdout=subprocess.PIPE)
    with tarfile.open(fileobj=proc.stdout, mode='r|') as tar:
        tar.extractall(dest)
    if proc.wait() != 0:
        sys.exit("git archive failed for ref '%s'" % git_ref)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    csv_path, out_dir = sys.argv[1], sys.argv[2]

    if not os.path.isdir(out_dir):
        os.makedirs(out_dir)

    built = {}
    for row in read_rows(csv_path):
        big_file, git_ref = row['big_file'], row['git_ref']
        if not git_ref or big_file.startswith('@'):
            continue
        if big_file in built:
            if built[big_file] != git_ref:
                sys.exit("%s is mapped to two different refs (%s, %s)"
                         % (big_file, built[big_file], git_ref))
            continue
        built[big_file] = git_ref

        out_path = os.path.join(out_dir, big_file)
        work = tempfile.mkdtemp(prefix='zulu-replay-big-')
        try:
            extract_assets(git_ref, work)
            pack_big.pack(work, out_path)
        finally:
            subprocess.call(['rm', '-rf', work])

    print("  built %d replay data archives into %s" % (len(built), out_dir))


if __name__ == '__main__':
    main()
