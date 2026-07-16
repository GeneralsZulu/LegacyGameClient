#!/usr/bin/env python3
"""Reusable client for the Generals AIBridge wire protocol.

Decodes the binary observation stream and encodes action batches, matching the
format documented at the top of
GeneralsMD/Code/GameEngine/Source/Common/AIBridge/AIBridge.cpp.

All frames are little-endian, 12-byte header: u32 magic, u32 payload_len,
u16 msg_type, u16 schema.
"""
from __future__ import annotations

import socket
import struct
from dataclasses import dataclass, field

MAGIC = 0x414E4547  # 'GENA' LE
SCHEMA = 1

MSG_HELLO, MSG_OBSERVATION, MSG_ACTION_BATCH, MSG_GAME_END, MSG_HELLO_ACK = 1, 2, 3, 4, 5

ACT_MOVE, ACT_ATTACKMOVE, ACT_ATTACK, ACT_STOP, ACT_CONSTRUCT, ACT_QUEUE_UNIT = 1, 2, 3, 4, 5, 6

# object flag bits
FLAG_BUILDING, FLAG_SELF, FLAG_DEAD = 0x01, 0x02, 0x04
# relation bytes
REL_SELF, REL_ALLY, REL_ENEMY, REL_NEUTRAL, REL_NA = 0, 1, 2, 3, -1


@dataclass
class Obj:
    id: int
    owner: int
    flags: int
    tmpl: str
    x: float
    y: float
    z: float
    angle: float
    hp: float
    max_hp: float

    @property
    def is_building(self) -> bool:
        return bool(self.flags & FLAG_BUILDING)

    @property
    def is_self(self) -> bool:
        return bool(self.flags & FLAG_SELF)

    @property
    def is_dead(self) -> bool:
        return bool(self.flags & FLAG_DEAD)


@dataclass
class Player:
    index: int
    relation: int  # relation to bot: 0 self,1 ally,2 enemy,3 neutral,-1 n/a
    money: int
    power_produced: int
    power_consumed: int


@dataclass
class Observation:
    frame: int
    bot_index: int
    players: list[Player] = field(default_factory=list)
    objects: list[Obj] = field(default_factory=list)

    def my_units(self) -> list[Obj]:
        return [o for o in self.objects if o.is_self and not o.is_dead]

    def my_movable(self) -> list[Obj]:
        return [o for o in self.my_units() if not o.is_building and o.hp > 0]

    def enemies(self) -> list[Obj]:
        me = self.bot_index
        enemy_idxs = {p.index for p in self.players if p.relation == REL_ENEMY}
        return [o for o in self.objects
                if not o.is_dead and o.owner in enemy_idxs and o.owner != me]

    def my_player(self) -> Player | None:
        for p in self.players:
            if p.index == self.bot_index:
                return p
        return None


class _Cursor:
    def __init__(self, payload: bytes):
        self.p, self.o = payload, 0

    def u8(self): v = self.p[self.o]; self.o += 1; return v
    def i8(self): v = struct.unpack_from("<b", self.p, self.o)[0]; self.o += 1; return v
    def u16(self): v = struct.unpack_from("<H", self.p, self.o)[0]; self.o += 2; return v
    def u32(self): v = struct.unpack_from("<I", self.p, self.o)[0]; self.o += 4; return v
    def i32(self): v = struct.unpack_from("<i", self.p, self.o)[0]; self.o += 4; return v
    def f32(self): v = struct.unpack_from("<f", self.p, self.o)[0]; self.o += 4; return v

    def s(self) -> str:
        n = self.u16()
        v = self.p[self.o:self.o + n]
        self.o += n
        return v.decode("utf-8", "replace")


def _encode_str(sval: str) -> bytes:
    b = sval.encode("utf-8")
    return struct.pack("<H", len(b)) + b


def encode_frame(msg_type: int, payload: bytes) -> bytes:
    return struct.pack("<IIHH", MAGIC, len(payload), msg_type, SCHEMA) + payload


# --- Action builders. Each returns (kind, unit_ids, extra_bytes). ---
def a_move(unit_ids, x, y, z=0.0):
    return (ACT_MOVE, list(unit_ids), struct.pack("<fff", x, y, z))

def a_attackmove(unit_ids, x, y, z=0.0):
    return (ACT_ATTACKMOVE, list(unit_ids), struct.pack("<fff", x, y, z))

def a_attack(unit_ids, target_id):
    return (ACT_ATTACK, list(unit_ids), struct.pack("<I", target_id))

def a_stop(unit_ids):
    return (ACT_STOP, list(unit_ids), b"")

def a_construct(dozer_ids, template, x, y, z=0.0, angle=0.0):
    return (ACT_CONSTRUCT, list(dozer_ids), _encode_str(template) + struct.pack("<ffff", x, y, z, angle))

def a_queue_unit(factory_ids, template):
    return (ACT_QUEUE_UNIT, list(factory_ids), _encode_str(template))


def encode_action_batch(frame_observed: int, actions) -> bytes:
    body = struct.pack("<IH", frame_observed, len(actions))
    for kind, unit_ids, extra in actions:
        body += struct.pack("<HH", kind, len(unit_ids))
        for uid in unit_ids:
            body += struct.pack("<I", uid)
        body += extra
    return encode_frame(MSG_ACTION_BATCH, body)


class BridgeClient:
    """Blocking TCP client with frame-buffered reads."""

    def __init__(self, host="127.0.0.1", port=8765, timeout=30.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.buf = bytearray()
        self.bot_index = -1

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

    def _recv_frame(self):
        while True:
            if len(self.buf) >= 12:
                magic, plen, mt, _sch = struct.unpack_from("<IIHH", self.buf, 0)
                if magic != MAGIC:
                    raise ValueError(f"bad magic 0x{magic:08x}")
                if len(self.buf) >= 12 + plen:
                    payload = bytes(self.buf[12:12 + plen])
                    del self.buf[:12 + plen]
                    return mt, payload
            chunk = self.sock.recv(65536)
            if not chunk:
                raise EOFError("bridge closed connection")
            self.buf += chunk

    def next_observation(self) -> Observation | None:
        """Read frames until an OBSERVATION arrives. Returns None on GAME_END."""
        while True:
            mt, payload = self._recv_frame()
            if mt == MSG_HELLO:
                c = _Cursor(payload)
                c.u32()  # frame
                self.bot_index = c.i8()
            elif mt == MSG_OBSERVATION:
                return self._decode_obs(payload)
            elif mt == MSG_GAME_END:
                return None
            # ignore others

    def _decode_obs(self, payload: bytes) -> Observation:
        c = _Cursor(payload)
        frame = c.u32()
        bot = c.i8()
        if bot >= 0:
            self.bot_index = bot
        npl = c.u8()
        players = []
        for _ in range(npl):
            idx = c.u8(); rel = c.i8(); money = c.u32(); prod = c.i32(); cons = c.i32()
            players.append(Player(idx, rel, money, prod, cons))
        nobj = c.u32()
        objs = []
        for _ in range(nobj):
            oid = c.u32(); owner = c.u8(); flags = c.u8(); tmpl = c.s()
            x = c.f32(); y = c.f32(); z = c.f32(); ang = c.f32(); hp = c.f32(); mhp = c.f32()
            objs.append(Obj(oid, owner, flags, tmpl, x, y, z, ang, hp, mhp))
        return Observation(frame, self.bot_index, players, objs)

    def send_actions(self, frame_observed: int, actions) -> None:
        if not actions:
            return
        self.sock.sendall(encode_action_batch(frame_observed, actions))
