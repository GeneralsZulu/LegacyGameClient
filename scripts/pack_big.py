#!/usr/bin/env python3
"""Pack a directory tree into a BIGF archive in one pass.

Replaces the per-file `big add` loop in the Makefile's zulu-big rule: that
tool rewrites the whole archive per added file, which is O(n^2) and took ~12
minutes once the community patch INI tree (2000+ files) moved into assets/.

Header entries and data blobs are written in sorted-path order, data packed
contiguously after the header. Paths inside the archive use backslashes.

That entry order differs from what the retired `big add` loop produced, so an
archive rebuilt here is not byte-identical to one built by the old pipeline.
It is equivalent, though: the engine reads archive contents through sorted
std::map / std::set containers (ArchiveFileSystem.h, FilenameList), so nothing
observes the order in the file. Verified against the shipped 1.4.0 and 1.5.2
archives -- same file set, every file byte-identical.

Usage: pack_big.py <assets-dir> <out.big>
"""

import os
import struct
import sys


def pack(assets_dir, out_path):
    files = []
    for root, _dirs, names in os.walk(assets_dir):
        for name in names:
            files.append(os.path.join(root, name))
    # LC_ALL=C sort on full path, matching the Makefile's previous ordering.
    files.sort()

    entries = []
    for path in files:
        rel = os.path.relpath(path, assets_dir)
        archive_path = rel.replace('/', '\\')
        entries.append((archive_path, path, os.path.getsize(path)))

    # Header: "BIGF", u32le total size, u32be file count, u32be header size
    # (== offset of the first data byte). Each entry: u32be offset, u32be
    # size, NUL-terminated archive path.
    header_size = 16 + sum(8 + len(ap) + 1 for ap, _p, _s in entries)
    total_size = header_size + sum(s for _ap, _p, s in entries)

    with open(out_path, 'wb') as out:
        out.write(b'BIGF')
        out.write(struct.pack('<I', total_size))
        out.write(struct.pack('>I', len(entries)))
        out.write(struct.pack('>I', header_size))
        offset = header_size
        for archive_path, _path, size in entries:
            out.write(struct.pack('>II', offset, size))
            out.write(archive_path.encode('latin1') + b'\0')
            offset += size
        for _archive_path, path, _size in entries:
            with open(path, 'rb') as f:
                out.write(f.read())

    print("  packed %d files into %s (%d bytes)" % (len(entries), out_path, total_size))


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    pack(sys.argv[1], sys.argv[2])


if __name__ == '__main__':
    main()
