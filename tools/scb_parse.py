#!/usr/bin/env python3
"""
Inspect and edit .scb (and the embedded script chunks of .map) files.

Reference: GeneralsMD/Code/GameEngine/Source/Common/System/DataChunk.cpp.

Read modes (default behavior, --symbols, --teams-only, --list-unit-types) are
non-destructive. The --apply-rules / --ban modes write a modified file with
in-place int32 patches on team templates, leaving every other byte untouched.

File layout
  4 bytes   ASCII 'CkMp'                magic
  4 bytes   int32 LE                    symbol count N
  N times:
    1 byte   uint8                       name length L
    L bytes  ASCII                       symbol name
    4 bytes  uint32 LE                   symbol id
  Until EOF:
    chunk:
      4 bytes   uint32 LE                chunk id (lookup in symbol table)
      2 bytes   uint16 LE                version
      4 bytes   int32 LE                 body size in bytes
      <body size> bytes                  nested chunks or primitives

Dict layout (used by ScriptTeams and the per-side ScriptsPlayers entries)
  2 bytes   uint16 LE                    pair count
  for each pair:
    4 bytes   uint32 LE                  packed: low 8 = type code,
                                                  high 24 = symbol id
    body for the type:
      0 BOOL           1 byte
      1 INT            4 bytes int32 LE
      2 REAL           4 bytes float32 LE
      3 ASCIISTRING    2 bytes uint16 len + len bytes
      4 UNICODESTRING  2 bytes uint16 len + len*2 bytes UTF-16LE

Script chunk body (per Scripts.cpp:1196 / 1232)
  ASCIISTRING  m_scriptName
  ASCIISTRING  m_comment
  ASCIISTRING  m_conditionComment
  ASCIISTRING  m_actionComment
  byte         m_isActive
  byte         m_isOneShot
  byte         m_easy
  byte         m_normal
  byte         m_hard
  byte         m_isSubroutine
  if version >= 2:
    int32      m_delayEvaluationSeconds
  nested OrCondition / ScriptAction / ScriptActionFalse chunks

Compression
  Real .map files inside .big archives are typically RefPack-compressed and
  must be decompressed before this tool can read them. The .scb files we
  have seen are stored uncompressed and start with the raw 'CkMp' magic. The
  parser refuses non-'CkMp' input and prints a hint.
"""

from __future__ import annotations

import argparse
import struct
import sys
from typing import Optional

DICT_BOOL, DICT_INT, DICT_REAL, DICT_ASCIISTRING, DICT_UNICODESTRING = 0, 1, 2, 3, 4
DICT_TYPE_NAMES = {0: "bool", 1: "int", 2: "real", 3: "string", 4: "wstring"}


# -----------------------------------------------------------------------------
# Low-level reader: walks a bytes buffer, tracking absolute file offsets so
# any value we find can be patched in place later.
# -----------------------------------------------------------------------------
class Reader:
    def __init__(self, data, base_offset=0):
        self.data = data
        self.pos = 0
        self.base_offset = base_offset

    def file_offset(self):
        return self.base_offset + self.pos

    def remaining(self):
        return len(self.data) - self.pos

    def read(self, n):
        if self.pos + n > len(self.data):
            raise EOFError(
                f"read past end at {self.file_offset():#x} "
                f"(need {n}, have {self.remaining()})"
            )
        out = self.data[self.pos : self.pos + n]
        self.pos += n
        return out

    def u8(self):
        return self.read(1)[0]

    def u16(self):
        return struct.unpack("<H", self.read(2))[0]

    def i32(self):
        return struct.unpack("<i", self.read(4))[0]

    def u32(self):
        return struct.unpack("<I", self.read(4))[0]

    def f32(self):
        return struct.unpack("<f", self.read(4))[0]

    def ascii_str(self):
        n = self.u16()
        return self.read(n).decode("latin-1", errors="replace")

    def unicode_str(self):
        n = self.u16()
        return self.read(n * 2).decode("utf-16-le", errors="replace")


# -----------------------------------------------------------------------------
# Symbol table (chunk id <-> name).
# -----------------------------------------------------------------------------
def read_symbol_table(r):
    magic = r.read(4)
    if magic != b"CkMp":
        raise ValueError(f"bad magic {magic!r}; expected b'CkMp' (was the file decompressed?)")
    count = r.i32()
    symbols = {}
    for _ in range(count):
        nlen = r.u8()
        name = r.read(nlen).decode("latin-1", errors="replace")
        sid = r.u32()
        symbols[sid] = name
    return symbols


# -----------------------------------------------------------------------------
# Dict reader. Each pair is returned with the file offset of its value, so
# fixed-size value types (bool, int, real) can be patched in place later.
# -----------------------------------------------------------------------------
class DictPair:
    __slots__ = ("key", "type_code", "value", "value_offset")

    def __init__(self, key, type_code, value, value_offset):
        self.key = key
        self.type_code = type_code
        self.value = value
        self.value_offset = value_offset


def read_dict(r, symbols):
    pair_count = r.u16()
    pairs = []
    for _ in range(pair_count):
        key_and_type = r.u32()
        type_code = key_and_type & 0xFF
        sid = key_and_type >> 8
        key = symbols.get(sid, f"<unknown_sym:{sid}>")
        value_offset = r.file_offset()
        if type_code == DICT_BOOL:
            v = bool(r.u8())
        elif type_code == DICT_INT:
            v = r.i32()
        elif type_code == DICT_REAL:
            v = r.f32()
        elif type_code == DICT_ASCIISTRING:
            v = r.ascii_str()
        elif type_code == DICT_UNICODESTRING:
            v = r.unicode_str()
        else:
            raise ValueError(
                f"unknown dict type {type_code} for key {key!r} at {r.file_offset():#x}"
            )
        pairs.append(DictPair(key, type_code, v, value_offset))
    return pairs


# -----------------------------------------------------------------------------
# Chunk walking. Each chunk records its body's absolute file offset so we
# can re-enter it for editing.
# -----------------------------------------------------------------------------
class Chunk:
    __slots__ = ("label", "version", "size", "offset", "body", "children", "decoded")

    def __init__(self, label, version, size, offset, body):
        self.label = label
        self.version = version
        self.size = size
        self.offset = offset
        self.body = body
        self.children = None
        self.decoded = None


def looks_like_chunk_header(body, symbols, expected_max_size):
    if len(body) < 10:
        return False
    chunk_id = struct.unpack("<I", body[0:4])[0]
    if chunk_id not in symbols:
        return False
    body_size = struct.unpack("<i", body[6:10])[0]
    if body_size < 0 or body_size + 10 > expected_max_size:
        return False
    return True


def parse_chunks(r, symbols):
    chunks = []
    while r.remaining() >= 10:
        chunk_id = r.u32()
        version = r.u16()
        size = r.i32()
        if chunk_id not in symbols:
            raise ValueError(
                f"unknown chunk id {chunk_id} at {r.file_offset()-10:#x}; "
                "file may be malformed"
            )
        if size < 0 or size > r.remaining():
            raise ValueError(
                f"chunk {symbols[chunk_id]!r} claims {size} bytes; "
                f"only {r.remaining()} available at {r.file_offset():#x}"
            )
        body_offset = r.file_offset()
        body = r.read(size)
        chunks.append(Chunk(symbols[chunk_id], version, size, body_offset, body))
    return chunks


def try_parse_nested(chunk, symbols):
    body = chunk.body
    if len(body) == 0:
        return []
    if not looks_like_chunk_header(body, symbols, len(body)):
        return None
    sub = Reader(body, base_offset=chunk.offset)
    try:
        children = parse_chunks(sub, symbols)
    except (EOFError, ValueError):
        return None
    if sub.remaining() != 0:
        return None
    return children


# -----------------------------------------------------------------------------
# Per-chunk decoders. ScriptTeams and ScriptsPlayers have primitive bodies;
# Script and friends have mixed primitive + nested chunk bodies, so the
# decoders read primitives and then recurse into the rest of the body as
# nested chunks.
# -----------------------------------------------------------------------------
def decode_script_teams(chunk, symbols):
    """Body = sequence of dicts (one team template each)."""
    sub = Reader(chunk.body, base_offset=chunk.offset)
    teams = []
    while sub.remaining() > 0:
        teams.append(read_dict(sub, symbols))
    return teams


def decode_scripts_players(chunk, symbols):
    """Body = optional v>=2 dict-flag + count + (name + optional dict)."""
    sub = Reader(chunk.body, base_offset=chunk.offset)
    read_dicts = sub.i32() if chunk.version >= 2 else 0
    num = sub.i32()
    out = []
    for _ in range(num):
        name = sub.ascii_str()
        d = read_dict(sub, symbols) if read_dicts else None
        out.append({"name": name, "dict": d})
    return out


def decode_script(chunk, symbols):
    """Body = 4 strings, 6 bytes, optional int32, then nested chunks."""
    sub = Reader(chunk.body, base_offset=chunk.offset)
    out = {
        "scriptName": sub.ascii_str(),
        "comment": sub.ascii_str(),
        "conditionComment": sub.ascii_str(),
        "actionComment": sub.ascii_str(),
        "isActive": bool(sub.u8()),
        "isOneShot": bool(sub.u8()),
        "easy": bool(sub.u8()),
        "normal": bool(sub.u8()),
        "hard": bool(sub.u8()),
        "isSubroutine": bool(sub.u8()),
    }
    if chunk.version >= 2:
        out["delayEvaluationSeconds"] = sub.i32()
    out["children"] = decode_remaining_as_chunks(sub, symbols)
    return out


def decode_script_group(chunk, symbols):
    """Body = string (groupName), byte (isGroupActive), optional byte (v2:
    isGroupSubroutine), then nested chunks."""
    sub = Reader(chunk.body, base_offset=chunk.offset)
    out = {
        "groupName": sub.ascii_str(),
        "isGroupActive": bool(sub.u8()),
    }
    if chunk.version >= 2:
        out["isGroupSubroutine"] = bool(sub.u8())
    out["children"] = decode_remaining_as_chunks(sub, symbols)
    return out


def decode_or_condition(chunk, symbols):
    """Body = nested Condition chunks."""
    sub = Reader(chunk.body, base_offset=chunk.offset)
    return {"children": decode_remaining_as_chunks(sub, symbols)}


def decode_condition(chunk, symbols):
    """Body = int32 type + NameKey int32 + int32 numParms + numParms params."""
    sub = Reader(chunk.body, base_offset=chunk.offset)
    cond_type = sub.i32()
    name_key_packed = sub.u32()
    name_sid = name_key_packed >> 8
    num_parms = sub.i32()
    parms = [decode_parameter(sub) for _ in range(num_parms)]
    return {
        "conditionType": cond_type,
        "name": symbols.get(name_sid, f"<sym:{name_sid}>"),
        "params": parms,
    }


def decode_script_action(chunk, symbols):
    """Body matches Condition: int32 type + NameKey + int32 numParms +
    numParms params."""
    sub = Reader(chunk.body, base_offset=chunk.offset)
    action_type = sub.i32()
    name_key_packed = sub.u32()
    name_sid = name_key_packed >> 8
    num_parms = sub.i32()
    parms = [decode_parameter(sub) for _ in range(num_parms)]
    return {
        "actionType": action_type,
        "name": symbols.get(name_sid, f"<sym:{name_sid}>"),
        "params": parms,
    }


# Parameter type code COORD3D is 13 in the engine enum (Scripts.h ParameterType).
# That value can shift if the enum changes, so this parser falls back to the
# generic int+real+string layout if it doesn't see exactly 12 bytes left in
# the body for the COORD3D variant.
COORD3D_PARAM_TYPE = 13


def decode_parameter(sub):
    """Parameter body: int32 paramType + (3*float32 if COORD3D else
    int32 + float32 + ASCIISTRING)."""
    param_type = sub.i32()
    if param_type == COORD3D_PARAM_TYPE:
        return {
            "paramType": param_type,
            "x": sub.f32(),
            "y": sub.f32(),
            "z": sub.f32(),
        }
    return {
        "paramType": param_type,
        "int": sub.i32(),
        "real": sub.f32(),
        "string": sub.ascii_str(),
    }


def decode_remaining_as_chunks(sub, symbols):
    """Helper: parse the remainder of `sub`'s buffer as a chunk sequence."""
    if sub.remaining() == 0:
        return []
    rest = sub.data[sub.pos :]
    rest_offset = sub.file_offset()
    rest_reader = Reader(rest, base_offset=rest_offset)
    try:
        return parse_chunks(rest_reader, symbols)
    except (EOFError, ValueError):
        return None


CHUNK_DECODERS = {
    "ScriptTeams": ("teams", decode_script_teams),
    "ScriptsPlayers": ("players", decode_scripts_players),
    "Script": ("script", decode_script),
    "ScriptGroup": ("script_group", decode_script_group),
    "OrCondition": ("or_condition", decode_or_condition),
    "Condition": ("condition", decode_condition),
    "ScriptAction": ("script_action", decode_script_action),
    "ScriptActionFalse": ("script_action", decode_script_action),
}


def decode_chunk(chunk, symbols):
    """Populate chunk.children and chunk.decoded based on the chunk label."""
    if chunk.label in CHUNK_DECODERS:
        kind, decoder = CHUNK_DECODERS[chunk.label]
        try:
            chunk.decoded = (kind, decoder(chunk, symbols))
        except (EOFError, ValueError) as e:
            chunk.decoded = ("error", str(e))
            return
        # Recurse into any sub-chunks the decoder collected.
        decoded = chunk.decoded[1]
        if isinstance(decoded, dict):
            kids = decoded.get("children")
            if isinstance(kids, list):
                for c in kids:
                    decode_chunk(c, symbols)
        return
    # Unknown chunk: try nested-chunk auto-detect.
    children = try_parse_nested(chunk, symbols)
    chunk.children = children
    if children:
        for c in children:
            decode_chunk(c, symbols)


# -----------------------------------------------------------------------------
# Pretty-printer.
# -----------------------------------------------------------------------------
def fmt_value(v):
    if isinstance(v, float):
        return f"{v:g}"
    return repr(v)


def show_chunk(chunk, depth=0, out=sys.stdout):
    indent = "  " * depth
    print(
        f"{indent}{chunk.label} v{chunk.version} ({chunk.size} bytes @ {chunk.offset:#x})",
        file=out,
    )

    if chunk.decoded is None:
        if chunk.children is None:
            head = chunk.body[:48].hex(" ")
            suffix = " ..." if len(chunk.body) > 48 else ""
            print(f"{indent}  <opaque {len(chunk.body)} bytes> {head}{suffix}", file=out)
        else:
            for c in chunk.children:
                show_chunk(c, depth + 1, out=out)
        return

    kind, data = chunk.decoded

    if kind == "teams":
        for i, pairs in enumerate(data):
            name = next((p.value for p in pairs if p.key == "teamName"), f"<team_{i}>")
            print(f"{indent}  team[{i}] {name}", file=out)
            for p in pairs:
                if p.key == "teamName":
                    continue
                print(
                    f"{indent}    {p.key} = {fmt_value(p.value)} ({DICT_TYPE_NAMES[p.type_code]})",
                    file=out,
                )
        return

    if kind == "players":
        for i, p in enumerate(data):
            print(f"{indent}  player[{i}] = {p['name']!r}", file=out)
            if p["dict"]:
                for pair in p["dict"]:
                    print(
                        f"{indent}    {pair.key} = {fmt_value(pair.value)} ({DICT_TYPE_NAMES[pair.type_code]})",
                        file=out,
                    )
        return

    if kind == "script":
        flags = []
        if data["isActive"]:
            flags.append("active")
        if data["isOneShot"]:
            flags.append("one-shot")
        diff = "".join(c for c, on in zip("ENH", (data["easy"], data["normal"], data["hard"])) if on)
        if diff:
            flags.append(f"diff={diff}")
        if data["isSubroutine"]:
            flags.append("subroutine")
        delay = data.get("delayEvaluationSeconds")
        if delay is not None:
            flags.append(f"delay={delay}s")
        print(f"{indent}  name={data['scriptName']!r} [{', '.join(flags)}]", file=out)
        for label, key in (("comment", "comment"), ("cond-comment", "conditionComment"), ("act-comment", "actionComment")):
            v = data[key]
            if v:
                print(f"{indent}  {label}={v!r}", file=out)
        for c in data.get("children") or []:
            show_chunk(c, depth + 1, out=out)
        return

    if kind == "script_group":
        flags = ["active" if data["isGroupActive"] else "inactive"]
        if data.get("isGroupSubroutine"):
            flags.append("subroutine")
        print(f"{indent}  group={data['groupName']!r} [{', '.join(flags)}]", file=out)
        for c in data.get("children") or []:
            show_chunk(c, depth + 1, out=out)
        return

    if kind == "or_condition":
        for c in data.get("children") or []:
            show_chunk(c, depth + 1, out=out)
        return

    if kind == "condition":
        ps = ", ".join(_short_param(p) for p in data["params"])
        print(f"{indent}  cond {data['name']} (type={data['conditionType']}) [{ps}]", file=out)
        return

    if kind == "script_action":
        ps = ", ".join(_short_param(p) for p in data["params"])
        kind_label = chunk.label  # ScriptAction or ScriptActionFalse
        print(
            f"{indent}  {kind_label.lower()} {data['name']} (type={data['actionType']}) [{ps}]",
            file=out,
        )
        return

    if kind == "error":
        print(f"{indent}  <decode error: {data}>", file=out)
        return


def _short_param(p):
    if "x" in p:
        return f"({p['x']:g}, {p['y']:g}, {p['z']:g})"
    parts = []
    if p["string"]:
        parts.append(repr(p["string"]))
    if p["int"]:
        parts.append(f"int={p['int']}")
    if p["real"]:
        parts.append(f"real={p['real']:g}")
    return f"t{p['paramType']}:{' '.join(parts) if parts else '0'}"


# -----------------------------------------------------------------------------
# Editing: parse the file once, locate every team's primitive fields, then
# patch teamMaxInstances=0 for any team that matches a ban rule. Editing is
# byte-in-place; the file size and structure don't change.
# -----------------------------------------------------------------------------
def collect_team_templates(chunks):
    """Walk top-level chunks, return the list of teams (DictPair lists) from
    every ScriptTeams chunk found."""
    teams = []
    for c in chunks:
        if c.label == "ScriptTeams" and c.decoded and c.decoded[0] == "teams":
            teams.extend(c.decoded[1])
    return teams


def team_lookup(pairs, key):
    """Return the DictPair for `key`, or None."""
    for p in pairs:
        if p.key == key:
            return p
    return None


def team_unit_types(pairs):
    """Return (slot_index, unit_name) pairs for non-empty teamUnitType slots."""
    out = []
    for p in pairs:
        if p.key.startswith("teamUnitType") and p.type_code == DICT_ASCIISTRING:
            try:
                slot = int(p.key[len("teamUnitType") :])
            except ValueError:
                continue
            if p.value:
                out.append((slot, p.value))
    return out


def parse_rule(s):
    """OWNER:UNIT, OWNER:UNIT:COUNT, or '*:UNIT' for any owner."""
    if ":" not in s:
        raise argparse.ArgumentTypeError(
            f"rule {s!r} must look like 'FACTION:UNIT' (use '*' for any faction)"
        )
    owner, unit = s.split(":", 1)
    if not unit:
        raise argparse.ArgumentTypeError(f"rule {s!r}: empty unit name")
    return (owner, unit)


def find_matching_teams(teams, owner_pat, unit_pat):
    for idx, pairs in enumerate(teams):
        owner_p = team_lookup(pairs, "teamOwner")
        owner = owner_p.value if owner_p else ""
        if owner_pat != "*" and owner != owner_pat:
            continue
        if any(u == unit_pat for _, u in team_unit_types(pairs)):
            yield idx, pairs


def apply_rules(buffer, teams, rules, dry_run, log=sys.stdout):
    """Patch teamMaxInstances=0 on every team matching any rule. Returns the
    number of teams modified."""
    modified = set()
    print(f"Applying {len(rules)} rule(s):", file=log)
    for owner_pat, unit_pat in rules:
        hits = list(find_matching_teams(teams, owner_pat, unit_pat))
        owner_label = owner_pat if owner_pat != "*" else "<any>"
        print(f"  {owner_label}:{unit_pat} -> {len(hits)} team(s)", file=log)
        for idx, pairs in hits:
            name_p = team_lookup(pairs, "teamName")
            mi_p = team_lookup(pairs, "teamMaxInstances")
            name = name_p.value if name_p else f"<team_{idx}>"
            if mi_p is None:
                print(f"    skip {name!r}: no teamMaxInstances field", file=log)
                continue
            if mi_p.type_code != DICT_INT:
                print(
                    f"    skip {name!r}: teamMaxInstances is type {DICT_TYPE_NAMES[mi_p.type_code]}",
                    file=log,
                )
                continue
            if mi_p.value == 0:
                if idx not in modified:
                    print(f"    keep {name!r}: already disabled (maxInstances=0)", file=log)
                continue
            print(
                f"    patch {name!r}: maxInstances {mi_p.value} -> 0 @ {mi_p.value_offset:#x}",
                file=log,
            )
            if not dry_run:
                struct.pack_into("<i", buffer, mi_p.value_offset, 0)
                mi_p.value = 0
            modified.add(idx)
    return len(modified)


# -----------------------------------------------------------------------------
# CLI.
# -----------------------------------------------------------------------------
def load_scb(path):
    with open(path, "rb") as f:
        data = bytearray(f.read())
    if data[:4] != b"CkMp":
        raise SystemExit(
            f"file does not start with 'CkMp' (got {bytes(data[:4])!r}); "
            "if it is a .map straight from a .big it is probably RefPack-compressed "
            "and needs to be decompressed first"
        )
    r = Reader(bytes(data))
    symbols = read_symbol_table(r)
    chunks = parse_chunks(r, symbols)
    for c in chunks:
        decode_chunk(c, symbols)
    return data, symbols, chunks


def cmd_show(data, symbols, chunks, args):
    for c in chunks:
        if args.teams_only and c.label != "ScriptTeams":
            continue
        show_chunk(c)


def cmd_symbols(data, symbols, chunks, args):
    for sid in sorted(symbols):
        print(f"{sid:>5}  {symbols[sid]}")


def cmd_list_unit_types(data, symbols, chunks, args):
    teams = collect_team_templates(chunks)
    counts = {}
    for pairs in teams:
        owner_p = team_lookup(pairs, "teamOwner")
        owner = owner_p.value if owner_p else ""
        if args.owner and owner != args.owner:
            continue
        for _, unit in team_unit_types(pairs):
            counts.setdefault(owner, {}).setdefault(unit, 0)
            counts[owner][unit] += 1

    for owner in sorted(counts):
        print(f"{owner or '<unowned>'}:")
        for unit, n in sorted(counts[owner].items(), key=lambda kv: (-kv[1], kv[0])):
            print(f"  {n:>4}  {unit}")


def cmd_apply_rules(data, symbols, chunks, args):
    teams = collect_team_templates(chunks)
    if not teams:
        raise SystemExit("no team templates found; nothing to edit")

    rules = list(args.ban or [])
    if not rules:
        raise SystemExit("no ban rules supplied (use --ban OWNER:UNIT)")

    modified = apply_rules(data, teams, rules, args.dry_run)
    if args.dry_run:
        print(f"\nDRY RUN: would have modified {modified} team(s); no file written.")
        return
    if not args.output:
        raise SystemExit("--output PATH is required when applying rules without --dry-run")
    with open(args.output, "wb") as f:
        f.write(data)
    print(f"\nWrote {args.output} ({modified} team(s) modified)")


def main():
    ap = argparse.ArgumentParser(
        description="Inspect and edit .scb (and .map script chunks)."
    )
    ap.add_argument("path", help="input .scb file")

    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--symbols", action="store_true", help="dump the symbol table and exit")
    mode.add_argument("--teams-only", action="store_true", help="dump ScriptTeams chunks only")
    mode.add_argument(
        "--list-unit-types",
        action="store_true",
        help="summarise unit types referenced by team templates",
    )
    mode.add_argument(
        "--apply-rules",
        action="store_true",
        help="apply --ban rules and write a modified .scb to --output",
    )

    ap.add_argument("--owner", help="restrict --list-unit-types to a single owner faction")

    ap.add_argument(
        "--ban",
        action="append",
        type=parse_rule,
        metavar="OWNER:UNIT",
        help="(repeatable, with --apply-rules) disable any team whose owner is OWNER "
        "and that uses UNIT in any teamUnitTypeN slot. Use '*' as OWNER to "
        "match every faction.",
    )
    ap.add_argument("--output", help="path to write the modified .scb")
    ap.add_argument(
        "--dry-run",
        action="store_true",
        help="report what --apply-rules would change without writing a file",
    )

    args = ap.parse_args()

    data, symbols, chunks = load_scb(args.path)

    if args.symbols:
        cmd_symbols(data, symbols, chunks, args)
    elif args.list_unit_types:
        cmd_list_unit_types(data, symbols, chunks, args)
    elif args.apply_rules:
        cmd_apply_rules(data, symbols, chunks, args)
    else:
        cmd_show(data, symbols, chunks, args)


if __name__ == "__main__":
    main()
