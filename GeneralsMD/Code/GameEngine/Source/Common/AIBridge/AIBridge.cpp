/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
*/

#include "PreRTS.h"

#include "Common/AIBridge/AIBridge.h"

#ifdef _WIN32
#include <winsock.h>
#ifdef _MSC_VER
#pragma comment(lib, "wsock32.lib")
#endif
#endif
#include <string.h>

#include "Common/MessageStream.h"
#include "Common/NameKeyGenerator.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/ThingFactory.h"
#include "Common/ThingTemplate.h"
#include "Common/Money.h"
#include "Common/Energy.h"
#include "Common/SpecialPower.h"
#include "Common/Upgrade.h"
#include "Common/Science.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Object.h"
#include "GameLogic/ObjectIter.h"
#include "GameLogic/PartitionManager.h"
#include "GameLogic/Module/BodyModule.h"
#include "GameLogic/Module/BehaviorModule.h"
#include "GameLogic/Module/SpecialPowerModule.h"
#include "GameLogic/Module/ProductionUpdate.h"

// =====================================================================
// Wire protocol
// =====================================================================
//
// Every frame on the wire is:
//
//   u32  magic           = 0x414E4547 ('GENA' read as little-endian)
//   u32  payload_length  bytes that follow this 12-byte header
//   u16  message_type
//   u16  schema_version  bumped on incompatible payload changes
//   u8[payload_length]   payload
//
// All multi-byte integers are little-endian. Floats are native
// IEEE-754 little-endian (same as x86 wire layout). Strings are
// length-prefixed: u16 length, then `length` bytes UTF-8.
//
// message_type:
//   1  HELLO        engine -> bot (sent on accept)
//   2  OBSERVATION  engine -> bot (per logic frame, when bot connected)
//   3  ACTION_BATCH bot -> engine
//   4  GAME_END     engine -> bot
//   5  HELLO_ACK    bot -> engine (optional; reserved)
//
// HELLO payload:
//   u32 frame
//   i8  bot_slot         // -1 if observation-only
//   u8  num_players
//   str map_name         // empty string if not yet known
//
// OBSERVATION payload:
//   u32 frame
//   i8  bot_slot
//   u8  num_players
//   for each player:
//     u8  player_index
//     i8  relation_to_bot  // 0=self, 1=ally, 2=enemy, 3=neutral, -1=na
//     u32 money
//     i32 power_produced
//     i32 power_consumed
//   u32 num_objects
//   for each visible object:
//     u32 object_id
//     u8  owner_player_index
//     u8  flags            // bit0 is_building, bit1 is_self, bit2 is_dead
//     str template_name
//     f32 pos_x, pos_y, pos_z
//     f32 angle
//     f32 hp, max_hp
//   // ---- schema v2+ trailing section: bot's own special-power readiness ----
//   u16 num_special_powers
//   for each ready-capable special power module the bot owns:
//     u32 source_object_id   // pass this back as sourceID when firing
//     str power_template_name
//     u8  is_ready           // 1/0
//     u8  percent_ready      // 0..100
//   // ---- schema v3+ trailing section: bot's completed upgrades ----
//   u16 num_completed_upgrades
//   for each completed upgrade the bot owns:
//     str upgrade_name
//
// ACTION_BATCH payload:
//   u32 frame_observed
//   u16 num_actions
//   for each action:
//     u16 kind
//     u16 num_unit_ids
//     u32[num_unit_ids] unit_ids
//     args based on kind:
//       1 SELECT_AND_MOVETO:        f32 x, y, z
//       2 SELECT_AND_ATTACKMOVE:    f32 x, y, z
//       3 SELECT_AND_ATTACK_TARGET: u32 target_object_id
//       4 SELECT_AND_STOP:          (no args)
//       5 DOZER_CONSTRUCT:          str template_name; f32 x, y, z; f32 angle
//                                   (selection must contain a dozer)
//       6 QUEUE_UNIT_CREATE:        str template_name
//                                   (selection must contain a factory)
//       7 SPECIAL_POWER_AT_LOCATION: str power_name; u32 source_object_id;
//                                    f32 x, y, z (no unit selection)
//       8 SPECIAL_POWER_AT_OBJECT:  str power_name; u32 source_object_id;
//                                    u32 target_object_id (no unit selection)
//       9 QUEUE_UPGRADE:            str upgrade_name; u32 source_object_id
//                                    (selection = the producing building)
//      10 PURCHASE_SCIENCE:         str science_name (player-level; no selection)
// =====================================================================

namespace
{
	const UnsignedInt   AIBRIDGE_MAGIC          = 0x414E4547u; // 'GENA' LE
	// v2 appends a trailing special-power readiness section to OBSERVATION;
	// v3 appends a second trailing section listing the bot's completed upgrades.
	const UnsignedShort AIBRIDGE_SCHEMA_VERSION = 3;

	enum
	{
		MSG_HELLO        = 1,
		MSG_OBSERVATION  = 2,
		MSG_ACTION_BATCH = 3,
		MSG_GAME_END     = 4,
		MSG_HELLO_ACK    = 5
	};

	enum
	{
		ACT_SELECT_AND_MOVETO        = 1,
		ACT_SELECT_AND_ATTACKMOVE    = 2,
		ACT_SELECT_AND_ATTACK_TARGET = 3,
		ACT_SELECT_AND_STOP          = 4,
		ACT_DOZER_CONSTRUCT          = 5,
		ACT_QUEUE_UNIT_CREATE        = 6,
		ACT_SPECIAL_POWER_AT_LOCATION = 7,
		ACT_SPECIAL_POWER_AT_OBJECT   = 8,
		ACT_QUEUE_UPGRADE             = 9,
		ACT_PURCHASE_SCIENCE          = 10
	};

	const UnsignedInt OBJ_FLAG_BUILDING = 0x01;
	const UnsignedInt OBJ_FLAG_SELF     = 0x02;
	const UnsignedInt OBJ_FLAG_DEAD     = 0x04;

	// Cap on outbound buffer to keep memory bounded if a slow bot stalls.
	const Int MAX_OUTBOUND_BYTES = 4 * 1024 * 1024;
	// Cap on inbound buffer; protects against hostile/garbled clients.
	const Int MAX_INBOUND_BYTES  = 1 * 1024 * 1024;
}

// =====================================================================
// Byte buffer helpers (little-endian, hand-rolled to avoid pulling in
// modern serialization libs the VC6 build can't tolerate).
// =====================================================================

class ByteWriter
{
public:
	ByteWriter() : m_data(nullptr), m_size(0), m_capacity(0) {}
	~ByteWriter() { delete[] m_data; }

	void clear() { m_size = 0; }
	const UnsignedByte* data() const { return m_data; }
	Int size() const { return m_size; }

	void writeU8(UnsignedByte v)
	{
		ensure(1);
		m_data[m_size++] = v;
	}
	void writeI8(Int v) { writeU8((UnsignedByte)(v & 0xFF)); }
	void writeU16(UnsignedShort v)
	{
		ensure(2);
		m_data[m_size + 0] = (UnsignedByte)(v & 0xFF);
		m_data[m_size + 1] = (UnsignedByte)((v >> 8) & 0xFF);
		m_size += 2;
	}
	void writeU32(UnsignedInt v)
	{
		ensure(4);
		m_data[m_size + 0] = (UnsignedByte)(v & 0xFF);
		m_data[m_size + 1] = (UnsignedByte)((v >> 8) & 0xFF);
		m_data[m_size + 2] = (UnsignedByte)((v >> 16) & 0xFF);
		m_data[m_size + 3] = (UnsignedByte)((v >> 24) & 0xFF);
		m_size += 4;
	}
	void writeI32(Int v) { writeU32((UnsignedInt)v); }
	void writeF32(Real v)
	{
		// IEEE-754 little-endian on x86; bit-pattern copy is fine.
		UnsignedInt bits;
		memcpy(&bits, &v, 4);
		writeU32(bits);
	}
	void writeStr(const char* s)
	{
		Int len = (s != nullptr) ? (Int)strlen(s) : 0;
		if (len > 0xFFFF) len = 0xFFFF;
		writeU16((UnsignedShort)len);
		ensure(len);
		if (len > 0) memcpy(m_data + m_size, s, len);
		m_size += len;
	}
	void writeRaw(const UnsignedByte* p, Int n)
	{
		ensure(n);
		memcpy(m_data + m_size, p, n);
		m_size += n;
	}

private:
	void ensure(Int more)
	{
		if (m_size + more <= m_capacity) return;
		Int newCap = (m_capacity > 0) ? m_capacity * 2 : 256;
		while (newCap < m_size + more) newCap *= 2;
		UnsignedByte* p = new UnsignedByte[newCap];
		if (m_size > 0) memcpy(p, m_data, m_size);
		delete[] m_data;
		m_data = p;
		m_capacity = newCap;
	}

	UnsignedByte* m_data;
	Int m_size;
	Int m_capacity;
};

class ByteReader
{
public:
	ByteReader(const UnsignedByte* data, Int size) : m_data(data), m_size(size), m_pos(0), m_ok(TRUE) {}

	Bool ok() const { return m_ok; }
	Int  remaining() const { return m_size - m_pos; }

	UnsignedByte readU8()
	{
		if (m_pos + 1 > m_size) { m_ok = FALSE; return 0; }
		return m_data[m_pos++];
	}
	Int readI8() { return (Int)(signed char)readU8(); }
	UnsignedShort readU16()
	{
		if (m_pos + 2 > m_size) { m_ok = FALSE; return 0; }
		UnsignedShort v = (UnsignedShort)m_data[m_pos]
			| ((UnsignedShort)m_data[m_pos + 1] << 8);
		m_pos += 2;
		return v;
	}
	UnsignedInt readU32()
	{
		if (m_pos + 4 > m_size) { m_ok = FALSE; return 0; }
		UnsignedInt v = (UnsignedInt)m_data[m_pos]
			| ((UnsignedInt)m_data[m_pos + 1] << 8)
			| ((UnsignedInt)m_data[m_pos + 2] << 16)
			| ((UnsignedInt)m_data[m_pos + 3] << 24);
		m_pos += 4;
		return v;
	}
	Int readI32() { return (Int)readU32(); }
	Real readF32()
	{
		UnsignedInt bits = readU32();
		Real v;
		memcpy(&v, &bits, 4);
		return v;
	}
	// Returns a length-prefixed UTF-8 string into an AsciiString. Empty/
	// truncated strings are returned as-is and m_ok is set FALSE on overrun.
	AsciiString readStr()
	{
		UnsignedShort len = readU16();
		if (m_pos + (Int)len > m_size)
		{
			m_ok = FALSE;
			return AsciiString::TheEmptyString;
		}
		AsciiString out;
		// AsciiString from a non-null-terminated buffer: copy through a temp.
		// Cap at a reasonable limit so a malicious bot can't OOM us.
		const Int CAP = 256;
		char tmp[CAP + 1];
		Int n = (Int)len;
		if (n > CAP) n = CAP;
		memcpy(tmp, m_data + m_pos, n);
		tmp[n] = '\0';
		out = tmp;
		m_pos += (Int)len;
		return out;
	}

private:
	const UnsignedByte* m_data;
	Int m_size;
	Int m_pos;
	Bool m_ok;
};

// =====================================================================
// AIBridgeServer: Winsock TCP listener and a single-client connection.
// Non-blocking I/O, polled once per game tick from AIBridge::tick().
// =====================================================================

class AIBridgeServer
{
public:
	AIBridgeServer()
		: m_listenSock(INVALID_SOCKET)
		, m_clientSock(INVALID_SOCKET)
		, m_wsaInited(FALSE)
		, m_inbound(nullptr)
		, m_inboundSize(0)
		, m_inboundCapacity(0)
		, m_outboundHead(nullptr)
	{
	}

	~AIBridgeServer()
	{
		shutdownAll();
		delete[] m_inbound;
	}

	Bool isClientConnected() const { return m_clientSock != INVALID_SOCKET; }

	Bool startListening(Int port)
	{
		if (m_listenSock != INVALID_SOCKET) return TRUE;

		WSADATA wsa;
		if (!m_wsaInited)
		{
			// WSAStartup is reference-counted, safe to call even if other
			// subsystems already inited it.
			if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
			{
				DEBUG_LOG(("AIBridge: WSAStartup failed"));
				return FALSE;
			}
			m_wsaInited = TRUE;
		}

		m_listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (m_listenSock == INVALID_SOCKET)
		{
			DEBUG_LOG(("AIBridge: socket() failed"));
			return FALSE;
		}

		BOOL one = TRUE;
		setsockopt(m_listenSock, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));

		struct sockaddr_in addr;
		memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 127.0.0.1 only
		addr.sin_port = htons((u_short)port);

		if (bind(m_listenSock, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR)
		{
			DEBUG_LOG(("AIBridge: bind on 127.0.0.1:%d failed (err %d)", port, WSAGetLastError()));
			closesocket(m_listenSock);
			m_listenSock = INVALID_SOCKET;
			return FALSE;
		}
		if (listen(m_listenSock, 1) == SOCKET_ERROR)
		{
			DEBUG_LOG(("AIBridge: listen failed"));
			closesocket(m_listenSock);
			m_listenSock = INVALID_SOCKET;
			return FALSE;
		}
		setNonBlocking(m_listenSock);
		DEBUG_LOG(("AIBridge: listening on 127.0.0.1:%d", port));
		return TRUE;
	}

	void shutdownAll()
	{
		dropClient();
		if (m_listenSock != INVALID_SOCKET)
		{
			closesocket(m_listenSock);
			m_listenSock = INVALID_SOCKET;
		}
		if (m_wsaInited)
		{
			WSACleanup();
			m_wsaInited = FALSE;
		}
		freeOutbound();
	}

	// Try to accept a pending client. Non-blocking; returns silently if none.
	void poll()
	{
		acceptIfPending();
		recvIfReady();
		flushOutbound();
	}

	// Drain one complete inbound frame, if available. Returns TRUE and fills
	// (msgType, payload, payloadSize) on success. Caller must NOT free payload;
	// it's a pointer into the server's internal buffer and is invalidated by
	// the next call to popInboundFrame.
	Bool popInboundFrame(Int& msgType, const UnsignedByte*& payload, Int& payloadSize)
	{
		// Header is 12 bytes.
		if (m_inboundSize < 12) return FALSE;

		UnsignedInt magic = readU32LE(m_inbound + 0);
		UnsignedInt plen  = readU32LE(m_inbound + 4);

		if (magic != AIBRIDGE_MAGIC)
		{
			DEBUG_LOG(("AIBridge: bad inbound magic 0x%08x; dropping client", magic));
			dropClient();
			return FALSE;
		}
		if (plen > (UnsignedInt)MAX_INBOUND_BYTES)
		{
			DEBUG_LOG(("AIBridge: oversized inbound frame %u; dropping client", plen));
			dropClient();
			return FALSE;
		}

		Int totalLen = 12 + (Int)plen;
		if (m_inboundSize < totalLen) return FALSE;

		UnsignedShort mtype = readU16LE(m_inbound + 8);
		// schema_version at +10 ignored for now; v1 only.

		msgType = (Int)mtype;
		payload = m_inbound + 12;
		payloadSize = (Int)plen;

		// Consume the frame from the buffer.
		Int leftover = m_inboundSize - totalLen;
		if (leftover > 0)
			memmove(m_inbound, m_inbound + totalLen, leftover);
		m_inboundSize = leftover;
		return TRUE;
	}

	// Queue an outbound frame. Caller fills `payload` via its own ByteWriter.
	void sendFrame(Int msgType, const UnsignedByte* payload, Int payloadSize)
	{
		if (m_clientSock == INVALID_SOCKET) return;
		if (payloadSize < 0) return;

		Int total = 12 + payloadSize;
		OutboundChunk* chunk = new OutboundChunk;
		chunk->data = new UnsignedByte[total];
		chunk->size = total;
		chunk->offset = 0;
		chunk->next = nullptr;

		writeU32LE(chunk->data + 0, AIBRIDGE_MAGIC);
		writeU32LE(chunk->data + 4, (UnsignedInt)payloadSize);
		writeU16LE(chunk->data + 8, (UnsignedShort)msgType);
		writeU16LE(chunk->data + 10, AIBRIDGE_SCHEMA_VERSION);
		if (payloadSize > 0)
			memcpy(chunk->data + 12, payload, payloadSize);

		// Append to outbound queue.
		if (m_outboundHead == nullptr)
		{
			m_outboundHead = chunk;
		}
		else
		{
			OutboundChunk* tail = m_outboundHead;
			while (tail->next != nullptr) tail = tail->next;
			tail->next = chunk;
		}

		// Cap memory.
		Int total_pending = 0;
		OutboundChunk* c;
		for (c = m_outboundHead; c != nullptr; c = c->next)
			total_pending += (c->size - c->offset);
		if (total_pending > MAX_OUTBOUND_BYTES)
		{
			DEBUG_LOG(("AIBridge: outbound buffer exceeded %d bytes; dropping slow client",
				MAX_OUTBOUND_BYTES));
			dropClient();
		}
	}

private:
	struct OutboundChunk
	{
		UnsignedByte*  data;
		Int            size;
		Int            offset;
		OutboundChunk* next;
	};

	void setNonBlocking(SOCKET s)
	{
		u_long mode = 1;
		ioctlsocket(s, FIONBIO, &mode);
	}

	void acceptIfPending()
	{
		if (m_listenSock == INVALID_SOCKET) return;
		if (m_clientSock != INVALID_SOCKET) return;

		struct sockaddr_in peer;
		int peerLen = sizeof(peer);
		SOCKET s = accept(m_listenSock, (struct sockaddr*)&peer, &peerLen);
		if (s == INVALID_SOCKET) return;

		setNonBlocking(s);

		// Disable Nagle: we want observation frames to flush immediately.
		BOOL one = TRUE;
		setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));

		m_clientSock = s;
		m_inboundSize = 0;
		freeOutbound();
		DEBUG_LOG(("AIBridge: accepted client connection"));
	}

	void recvIfReady()
	{
		if (m_clientSock == INVALID_SOCKET) return;

		// Drain everything pending. recv() returns -1/EWOULDBLOCK when empty.
		const Int CHUNK = 4096;
		while (TRUE)
		{
			ensureInbound(CHUNK);
			int n = recv(m_clientSock, (char*)(m_inbound + m_inboundSize), CHUNK, 0);
			if (n > 0)
			{
				m_inboundSize += n;
				if (m_inboundSize > MAX_INBOUND_BYTES)
				{
					DEBUG_LOG(("AIBridge: inbound buffer exceeded; dropping client"));
					dropClient();
					return;
				}
				continue;
			}
			if (n == 0)
			{
				// Orderly close.
				DEBUG_LOG(("AIBridge: client closed connection"));
				dropClient();
				return;
			}
			int err = WSAGetLastError();
			if (err == WSAEWOULDBLOCK) return;
			DEBUG_LOG(("AIBridge: recv error %d; dropping client", err));
			dropClient();
			return;
		}
	}

	void flushOutbound()
	{
		if (m_clientSock == INVALID_SOCKET) return;

		while (m_outboundHead != nullptr)
		{
			OutboundChunk* c = m_outboundHead;
			Int toSend = c->size - c->offset;
			int n = send(m_clientSock, (const char*)(c->data + c->offset), toSend, 0);
			if (n > 0)
			{
				c->offset += n;
				if (c->offset >= c->size)
				{
					m_outboundHead = c->next;
					delete[] c->data;
					delete c;
				}
				continue;
			}
			int err = WSAGetLastError();
			if (err == WSAEWOULDBLOCK) return;
			DEBUG_LOG(("AIBridge: send error %d; dropping client", err));
			dropClient();
			return;
		}
	}

	void dropClient()
	{
		if (m_clientSock != INVALID_SOCKET)
		{
			closesocket(m_clientSock);
			m_clientSock = INVALID_SOCKET;
		}
		m_inboundSize = 0;
		freeOutbound();
	}

	void freeOutbound()
	{
		while (m_outboundHead != nullptr)
		{
			OutboundChunk* c = m_outboundHead;
			m_outboundHead = c->next;
			delete[] c->data;
			delete c;
		}
	}

	void ensureInbound(Int more)
	{
		if (m_inboundSize + more <= m_inboundCapacity) return;
		Int newCap = (m_inboundCapacity > 0) ? m_inboundCapacity * 2 : 4096;
		while (newCap < m_inboundSize + more) newCap *= 2;
		UnsignedByte* p = new UnsignedByte[newCap];
		if (m_inboundSize > 0) memcpy(p, m_inbound, m_inboundSize);
		delete[] m_inbound;
		m_inbound = p;
		m_inboundCapacity = newCap;
	}

	static UnsignedInt readU32LE(const UnsignedByte* p)
	{
		return (UnsignedInt)p[0] | ((UnsignedInt)p[1] << 8)
		     | ((UnsignedInt)p[2] << 16) | ((UnsignedInt)p[3] << 24);
	}
	static UnsignedShort readU16LE(const UnsignedByte* p)
	{
		return (UnsignedShort)p[0] | ((UnsignedShort)p[1] << 8);
	}
	static void writeU32LE(UnsignedByte* p, UnsignedInt v)
	{
		p[0] = (UnsignedByte)(v & 0xFF);
		p[1] = (UnsignedByte)((v >> 8) & 0xFF);
		p[2] = (UnsignedByte)((v >> 16) & 0xFF);
		p[3] = (UnsignedByte)((v >> 24) & 0xFF);
	}
	static void writeU16LE(UnsignedByte* p, UnsignedShort v)
	{
		p[0] = (UnsignedByte)(v & 0xFF);
		p[1] = (UnsignedByte)((v >> 8) & 0xFF);
	}

	SOCKET m_listenSock;
	SOCKET m_clientSock;
	Bool   m_wsaInited;

	UnsignedByte* m_inbound;
	Int           m_inboundSize;
	Int           m_inboundCapacity;

	OutboundChunk* m_outboundHead;
};

// =====================================================================
// Observation builder.
// =====================================================================

namespace
{
	// Map a Generals Relationship enum value to the wire's relation_to_bot
	// byte. We translate at the boundary so wire format doesn't depend on
	// internal enum values.
	Int relationToBotByte(const Player* bot, const Player* p)
	{
		if (bot == nullptr || p == nullptr) return -1;
		if (bot->getPlayerIndex() == p->getPlayerIndex()) return 0;
		Relationship r = bot->getRelationship(p->getDefaultTeam());
		if (r == ALLIES)   return 1;
		if (r == ENEMIES)  return 2;
		return 3; // NEUTRAL
	}

	// Iterator callback passed to PartitionManager::iterateAllObjects.
	struct VisibleObjectCollector
	{
		ByteWriter*  writer;
		Int          botPlayerIndex;
		UnsignedInt  count;
	};

	// Note: we use iterateAllObjects + manual filter rather than a
	// PartitionFilter chain because the filter chain would still need to
	// touch each object to check shroud, so this is no slower and clearer.
	void writeOneVisibleObject(VisibleObjectCollector* coll, const Object* obj)
	{
		if (obj == nullptr) return;
		if (obj->isEffectivelyDead()) return;

		// Shroud check from bot's POV. OBJECTSHROUD_PARTIAL_CLEAR is the
		// "still visible" threshold; anything beyond it is fogged.
		if (coll->botPlayerIndex >= 0)
		{
			ObjectShroudStatus shroud = obj->getShroudedStatus(coll->botPlayerIndex);
			if (shroud > OBJECTSHROUD_PARTIAL_CLEAR)
				return;
		}

		const ThingTemplate* tmpl = obj->getTemplate();
		if (tmpl == nullptr) return;

		const Player* owner = obj->getControllingPlayer();
		Int ownerIdx = (owner != nullptr) ? (Int)owner->getPlayerIndex() : -1;

		UnsignedByte flags = 0;
		if (tmpl->isKindOf(KINDOF_STRUCTURE))     flags |= (UnsignedByte)OBJ_FLAG_BUILDING;
		if (ownerIdx == coll->botPlayerIndex)     flags |= (UnsignedByte)OBJ_FLAG_SELF;

		const Coord3D* pos = obj->getPosition();
		Real angle = obj->getOrientation();

		Real hp    = obj->getBodyModule() != nullptr ? obj->getBodyModule()->getHealth()    : 0.0f;
		Real maxhp = obj->getBodyModule() != nullptr ? obj->getBodyModule()->getMaxHealth() : 0.0f;

		coll->writer->writeU32((UnsignedInt)obj->getID());
		coll->writer->writeU8 ((UnsignedByte)(ownerIdx & 0xFF));
		coll->writer->writeU8 (flags);
		coll->writer->writeStr(tmpl->getName().str());
		coll->writer->writeF32(pos != nullptr ? pos->x : 0.0f);
		coll->writer->writeF32(pos != nullptr ? pos->y : 0.0f);
		coll->writer->writeF32(pos != nullptr ? pos->z : 0.0f);
		coll->writer->writeF32(angle);
		coll->writer->writeF32(hp);
		coll->writer->writeF32(maxhp);
		coll->count++;
	}

	void buildHelloPayload(ByteWriter& w, Int botSlot)
	{
		UnsignedInt frame = (TheGameLogic != nullptr) ? TheGameLogic->getFrame() : 0;
		Int numPlayers = (ThePlayerList != nullptr) ? ThePlayerList->getPlayerCount() : 0;

		w.writeU32(frame);
		w.writeI8(botSlot);
		w.writeU8((UnsignedByte)(numPlayers & 0xFF));
		const char* mapName = (TheGlobalData != nullptr) ? TheGlobalData->m_mapName.str() : "";
		w.writeStr(mapName);
	}

	void buildObservationPayload(ByteWriter& w, Int botSlot)
	{
		UnsignedInt frame = (TheGameLogic != nullptr) ? TheGameLogic->getFrame() : 0;
		w.writeU32(frame);
		w.writeI8(botSlot);

		Int numPlayers = (ThePlayerList != nullptr) ? ThePlayerList->getPlayerCount() : 0;
		w.writeU8((UnsignedByte)(numPlayers & 0xFF));

		Player* bot = (botSlot >= 0 && ThePlayerList != nullptr)
			? ThePlayerList->getNthPlayer(botSlot) : nullptr;

		Int p;
		for (p = 0; p < numPlayers; ++p)
		{
			Player* pl = ThePlayerList->getNthPlayer(p);
			w.writeU8((UnsignedByte)(p & 0xFF));
			w.writeI8(relationToBotByte(bot, pl));
			UnsignedInt money = (pl != nullptr && pl->getMoney() != nullptr)
				? pl->getMoney()->countMoney() : 0;
			Int prod = (pl != nullptr && pl->getEnergy() != nullptr)
				? pl->getEnergy()->getProduction() : 0;
			Int cons = (pl != nullptr && pl->getEnergy() != nullptr)
				? pl->getEnergy()->getConsumption() : 0;
			w.writeU32(money);
			w.writeI32(prod);
			w.writeI32(cons);
		}

		// Reserve a u32 for object count and remember its offset; we'll
		// patch in the actual count after iteration.
		Int countPos = w.size();
		w.writeU32(0);

		VisibleObjectCollector coll;
		coll.writer = &w;
		coll.botPlayerIndex = botSlot;
		coll.count = 0;

		if (ThePartitionManager != nullptr)
		{
			SimpleObjectIterator* iter = ThePartitionManager->iterateAllObjects(nullptr);
			if (iter != nullptr)
			{
				Object* o;
				for (o = iter->first(); o != nullptr; o = iter->next())
				{
					writeOneVisibleObject(&coll, o);
				}
				deleteInstance(iter);
			}
		}

		// Patch the object count.
		UnsignedByte* p_u8 = const_cast<UnsignedByte*>(w.data()) + countPos;
		p_u8[0] = (UnsignedByte)(coll.count & 0xFF);
		p_u8[1] = (UnsignedByte)((coll.count >> 8) & 0xFF);
		p_u8[2] = (UnsignedByte)((coll.count >> 16) & 0xFF);
		p_u8[3] = (UnsignedByte)((coll.count >> 24) & 0xFF);

		// ---- schema v2 trailing section: bot's own special-power readiness ----
		// Walk the bot player's own objects and enumerate every special power
		// module they carry, reporting owner id + power name + readiness so the
		// bot knows WHEN (not just how) to fire. Strictly at the end of the
		// payload so pre-v2 decoders that stop after the object list are safe.
		Int spCountPos = w.size();
		w.writeU16(0);
		UnsignedShort spCount = 0;
		if (bot != nullptr && ThePartitionManager != nullptr)
		{
			SimpleObjectIterator* spIter = ThePartitionManager->iterateAllObjects(nullptr);
			if (spIter != nullptr)
			{
				Object* o;
				for (o = spIter->first(); o != nullptr; o = spIter->next())
				{
					if (o == nullptr || o->isEffectivelyDead()) continue;
					if (o->getControllingPlayer() != bot) continue;

					BehaviorModule** b;
					for (b = o->getBehaviorModules(); b && *b; ++b)
					{
						SpecialPowerModuleInterface* sp = (*b)->getSpecialPower();
						if (sp == nullptr) continue;
						const SpecialPowerTemplate* spt = sp->getSpecialPowerTemplate();
						if (spt == nullptr) continue;

						Real pct = sp->getPercentReady();       // 1.0 = ready
						Int pct100 = (Int)(pct * 100.0f + 0.5f);
						if (pct100 < 0) pct100 = 0;
						if (pct100 > 100) pct100 = 100;

						w.writeU32((UnsignedInt)o->getID());
						w.writeStr(spt->getName().str());
						w.writeU8((UnsignedByte)(sp->isReady() ? 1 : 0));
						w.writeU8((UnsignedByte)pct100);
						++spCount;
					}
				}
				deleteInstance(spIter);
			}
		}

		// Patch the special-power count.
		UnsignedByte* sp_u8 = const_cast<UnsignedByte*>(w.data()) + spCountPos;
		sp_u8[0] = (UnsignedByte)(spCount & 0xFF);
		sp_u8[1] = (UnsignedByte)((spCount >> 8) & 0xFF);

		// ---- schema v3 trailing section: bot's completed upgrades ----
		// Lets the bot know which upgrades it already owns so it doesn't waste
		// money re-researching them. Walk the global upgrade template list and
		// report the names the bot player has fully completed. Strictly after the
		// readiness section so older decoders that stop earlier stay valid.
		Int upCountPos = w.size();
		w.writeU16(0);
		UnsignedShort upCount = 0;
		if (bot != nullptr && TheUpgradeCenter != nullptr)
		{
			const UpgradeTemplate* upg;
			for (upg = TheUpgradeCenter->firstUpgradeTemplate();
				 upg != nullptr;
				 upg = upg->friend_getNext())
			{
				if (bot->hasUpgradeComplete(upg))
				{
					w.writeStr(upg->getUpgradeName().str());
					++upCount;
				}
			}
		}

		// Patch the completed-upgrade count.
		UnsignedByte* up_u8 = const_cast<UnsignedByte*>(w.data()) + upCountPos;
		up_u8[0] = (UnsignedByte)(upCount & 0xFF);
		up_u8[1] = (UnsignedByte)((upCount >> 8) & 0xFF);
	}
}

// =====================================================================
// Action parser. Mints GameMessages with the bot's playerIndex and
// appends to TheCommandList. Critical: GameLogicDispatch derives the
// "currently selected group" from the message-issuing player's selection
// at execution time (GameLogicDispatch.cpp:367-389), so each unit-targeting
// action emits TWO messages: a MSG_CREATE_SELECTED_GROUP_NO_SOUND to set
// the bot player's selection, then the actual command which operates on
// that selection.
// =====================================================================

namespace
{
	GameMessage* newBotMessage(GameMessage::Type type, Int botSlot)
	{
		GameMessage* m = newInstance(GameMessage)(type);
		m->friend_setPlayerIndex(botSlot);
		return m;
	}

	void emitSelectionMessage(Int botSlot, ByteReader& r, UnsignedShort numUnits)
	{
		GameMessage* sel = newBotMessage(GameMessage::MSG_CREATE_SELECTED_GROUP_NO_SOUND, botSlot);
		// First arg per CommandXlat: boolean = "create new group" (vs append).
		sel->appendBooleanArgument(TRUE);
		UnsignedShort i;
		for (i = 0; i < numUnits; ++i)
		{
			UnsignedInt id = r.readU32();
			sel->appendObjectIDArgument((ObjectID)id);
		}
		TheCommandList->appendMessage(sel);
	}

	void emitMoveOrAttackMove(Int botSlot, ByteReader& r, UnsignedShort numUnits, Bool attackMove)
	{
		emitSelectionMessage(botSlot, r, numUnits);

		Coord3D loc;
		loc.x = r.readF32();
		loc.y = r.readF32();
		loc.z = r.readF32();

		GameMessage::Type t = attackMove
			? GameMessage::MSG_DO_ATTACKMOVETO
			: GameMessage::MSG_DO_MOVETO;
		GameMessage* m = newBotMessage(t, botSlot);
		m->appendLocationArgument(loc);
		TheCommandList->appendMessage(m);
	}

	void emitAttackTarget(Int botSlot, ByteReader& r, UnsignedShort numUnits)
	{
		emitSelectionMessage(botSlot, r, numUnits);

		UnsignedInt targetID = r.readU32();
		GameMessage* m = newBotMessage(GameMessage::MSG_DO_ATTACK_OBJECT, botSlot);
		m->appendObjectIDArgument((ObjectID)targetID);
		TheCommandList->appendMessage(m);
	}

	void emitStop(Int botSlot, ByteReader& r, UnsignedShort numUnits)
	{
		emitSelectionMessage(botSlot, r, numUnits);

		GameMessage* m = newBotMessage(GameMessage::MSG_DO_STOP, botSlot);
		TheCommandList->appendMessage(m);
	}

	void emitDozerConstruct(Int botSlot, ByteReader& r, UnsignedShort numUnits)
	{
		emitSelectionMessage(botSlot, r, numUnits);

		AsciiString name = r.readStr();
		Coord3D loc;
		loc.x = r.readF32();
		loc.y = r.readF32();
		loc.z = r.readF32();
		Real angle = r.readF32();

		const ThingTemplate* tmpl = (TheThingFactory != nullptr)
			? TheThingFactory->findTemplate(name) : nullptr;
		if (tmpl == nullptr)
		{
			DEBUG_LOG(("AIBridge: DOZER_CONSTRUCT unknown template '%s'", name.str()));
			return;
		}

		GameMessage* m = newBotMessage(GameMessage::MSG_DOZER_CONSTRUCT, botSlot);
		m->appendIntegerArgument((Int)tmpl->getTemplateID());
		m->appendLocationArgument(loc);
		m->appendRealArgument(angle);
		TheCommandList->appendMessage(m);
	}

	void emitQueueUnitCreate(Int botSlot, ByteReader& r, UnsignedShort numUnits)
	{
		emitSelectionMessage(botSlot, r, numUnits);

		AsciiString name = r.readStr();

		const ThingTemplate* tmpl = (TheThingFactory != nullptr)
			? TheThingFactory->findTemplate(name) : nullptr;
		if (tmpl == nullptr)
		{
			DEBUG_LOG(("AIBridge: QUEUE_UNIT_CREATE unknown template '%s'", name.str()));
			return;
		}

		GameMessage* m = newBotMessage(GameMessage::MSG_QUEUE_UNIT_CREATE, botSlot);
		m->appendIntegerArgument((Int)tmpl->getTemplateID());
		// PRODUCTIONID_INVALID(=0) lets the producer pick an ID; matches what
		// the GUI does when the player clicks a unit-build icon.
		m->appendIntegerArgument((Int)PRODUCTIONID_INVALID);
		TheCommandList->appendMessage(m);
	}

	// Fire a special power aimed at a ground location. No unit selection is
	// needed: the source object is explicit (arg5), and GameLogicDispatch builds
	// a one-object AIGroup from it. Args must match GameLogicDispatch.cpp:757.
	void emitSpecialPowerAtLocation(Int botSlot, ByteReader& r, UnsignedShort /*numUnits*/)
	{
		AsciiString name = r.readStr();
		UnsignedInt sourceID = r.readU32();
		Coord3D loc;
		loc.x = r.readF32();
		loc.y = r.readF32();
		loc.z = r.readF32();

		const SpecialPowerTemplate* spt = (TheSpecialPowerStore != nullptr)
			? TheSpecialPowerStore->findSpecialPowerTemplate(name) : nullptr;
		if (spt == nullptr)
		{
			DEBUG_LOG(("AIBridge: SPECIAL_POWER_AT_LOCATION unknown power '%s'", name.str()));
			return;
		}

		GameMessage* m = newBotMessage(GameMessage::MSG_DO_SPECIAL_POWER_AT_LOCATION, botSlot);
		m->appendIntegerArgument((Int)spt->getID());   // arg0 specialPowerID
		m->appendLocationArgument(loc);                 // arg1 targetCoord
		m->appendRealArgument(0.0f);                    // arg2 angle
		m->appendObjectIDArgument(INVALID_ID);          // arg3 objectInWay
		m->appendIntegerArgument(0);                    // arg4 options
		m->appendObjectIDArgument((ObjectID)sourceID);  // arg5 sourceID
		TheCommandList->appendMessage(m);
	}

	// Fire a special power aimed at a target object. Args must match
	// GameLogicDispatch.cpp:814.
	void emitSpecialPowerAtObject(Int botSlot, ByteReader& r, UnsignedShort /*numUnits*/)
	{
		AsciiString name = r.readStr();
		UnsignedInt sourceID = r.readU32();
		UnsignedInt targetID = r.readU32();

		const SpecialPowerTemplate* spt = (TheSpecialPowerStore != nullptr)
			? TheSpecialPowerStore->findSpecialPowerTemplate(name) : nullptr;
		if (spt == nullptr)
		{
			DEBUG_LOG(("AIBridge: SPECIAL_POWER_AT_OBJECT unknown power '%s'", name.str()));
			return;
		}

		GameMessage* m = newBotMessage(GameMessage::MSG_DO_SPECIAL_POWER_AT_OBJECT, botSlot);
		m->appendIntegerArgument((Int)spt->getID());    // arg0 specialPowerID
		m->appendObjectIDArgument((ObjectID)targetID);  // arg1 targetID
		m->appendIntegerArgument(0);                    // arg2 options
		m->appendObjectIDArgument((ObjectID)sourceID);  // arg3 sourceID
		TheCommandList->appendMessage(m);
	}

	// Queue an upgrade on the selected producing building. The dispatcher reads
	// the upgrade STABLE id from arg1 (arg0 is the source, for logging only) and
	// applies it to the currently-selected group, so we select first. Args must
	// match GameLogicDispatch.cpp:1405.
	void emitQueueUpgrade(Int botSlot, ByteReader& r, UnsignedShort numUnits)
	{
		emitSelectionMessage(botSlot, r, numUnits);

		AsciiString name = r.readStr();
		UnsignedInt sourceID = r.readU32();

		const UpgradeTemplate* upg = (TheUpgradeCenter != nullptr)
			? TheUpgradeCenter->findUpgrade(name) : nullptr;
		if (upg == nullptr)
		{
			DEBUG_LOG(("AIBridge: QUEUE_UPGRADE unknown upgrade '%s'", name.str()));
			return;
		}
		Int stableId = TheUpgradeCenter->getStableUpgradeId(upg);

		GameMessage* m = newBotMessage(GameMessage::MSG_QUEUE_UPGRADE, botSlot);
		m->appendObjectIDArgument((ObjectID)sourceID);  // arg0 source (logging)
		m->appendIntegerArgument(stableId);             // arg1 stable upgrade id
		TheCommandList->appendMessage(m);
	}

	// Purchase a player-level science (general's promotion power). No unit
	// selection: the dispatcher acts on the message-issuing player. Args must
	// match GameLogicDispatch.cpp:2173.
	void emitPurchaseScience(Int botSlot, ByteReader& r, UnsignedShort /*numUnits*/)
	{
		AsciiString name = r.readStr();

		ScienceType science = (TheScienceStore != nullptr)
			? TheScienceStore->getScienceFromInternalName(name) : SCIENCE_INVALID;
		if (science == SCIENCE_INVALID)
		{
			DEBUG_LOG(("AIBridge: PURCHASE_SCIENCE unknown science '%s'", name.str()));
			return;
		}

		GameMessage* m = newBotMessage(GameMessage::MSG_PURCHASE_SCIENCE, botSlot);
		m->appendIntegerArgument((Int)science);         // arg0 ScienceType
		TheCommandList->appendMessage(m);
	}

	void parseActionBatch(Int botSlot, const UnsignedByte* payload, Int size)
	{
		ByteReader r(payload, size);
		(void)r.readU32(); // frame_observed; informational only

		UnsignedShort numActions = r.readU16();
		UnsignedShort i;
		for (i = 0; i < numActions; ++i)
		{
			if (!r.ok()) break;
			UnsignedShort kind     = r.readU16();
			UnsignedShort numUnits = r.readU16();

			// Enforce a hard cap so a malformed message can't fan out into
			// allocating millions of selection ids.
			if (numUnits > 256)
			{
				DEBUG_LOG(("AIBridge: action with %u units exceeds cap; aborting batch", numUnits));
				return;
			}

			switch (kind)
			{
				case ACT_SELECT_AND_MOVETO:
					emitMoveOrAttackMove(botSlot, r, numUnits, FALSE);
					break;
				case ACT_SELECT_AND_ATTACKMOVE:
					emitMoveOrAttackMove(botSlot, r, numUnits, TRUE);
					break;
				case ACT_SELECT_AND_ATTACK_TARGET:
					emitAttackTarget(botSlot, r, numUnits);
					break;
				case ACT_SELECT_AND_STOP:
					emitStop(botSlot, r, numUnits);
					break;
				case ACT_DOZER_CONSTRUCT:
					emitDozerConstruct(botSlot, r, numUnits);
					break;
				case ACT_QUEUE_UNIT_CREATE:
					emitQueueUnitCreate(botSlot, r, numUnits);
					break;
				case ACT_SPECIAL_POWER_AT_LOCATION:
					emitSpecialPowerAtLocation(botSlot, r, numUnits);
					break;
				case ACT_SPECIAL_POWER_AT_OBJECT:
					emitSpecialPowerAtObject(botSlot, r, numUnits);
					break;
				case ACT_QUEUE_UPGRADE:
					emitQueueUpgrade(botSlot, r, numUnits);
					break;
				case ACT_PURCHASE_SCIENCE:
					emitPurchaseScience(botSlot, r, numUnits);
					break;
				default:
					DEBUG_LOG(("AIBridge: unknown action kind %u; aborting batch", kind));
					return;
			}
		}
	}
}

// =====================================================================
// AIBridge singleton: lifecycle + per-tick driver.
// =====================================================================

AIBridge* TheAIBridge = nullptr;

namespace
{
	// Module-level configuration written by CommandLine parsers, read by
	// AIBridge::init(). Held outside the class because parsing happens
	// before TheAIBridge is allocated.
	Int s_configuredPort = 0;
	Int s_configuredSlot = -1;
}

void AIBridge_setListenPort(Int port) { s_configuredPort = port; }
void AIBridge_setBotSlot(Int slot)    { s_configuredSlot = slot; }
Int  AIBridge_getListenPort()         { return s_configuredPort; }
Int  AIBridge_getBotSlot()            { return s_configuredSlot; }

namespace
{
	// Observation log: when -aiobslog is set, per-frame observations are written
	// to this file as the SAME length-prefixed wire frames the socket bridge
	// sends, so the existing Python decoder reads them unchanged. This is the
	// robust path for offline replay -> dataset extraction: the flat-out headless
	// replay loop bypasses GameEngine::update() and starves wine's async socket
	// layer, so streaming over TCP during -replay is unreliable; a direct file
	// write is deterministic and needs no collector process.
	FILE*       s_obsLogFile     = nullptr;
	AsciiString s_obsLogPath;
	Int         s_obsLogDecimate = 30;   // write 1 of every N logic frames

	void writeObsLogFrame(Int msgType, const UnsignedByte* payload, Int payloadSize)
	{
		if (s_obsLogFile == nullptr || payloadSize < 0)
			return;
		const UnsignedInt   magic = AIBRIDGE_MAGIC;
		const UnsignedInt   plen  = (UnsignedInt)payloadSize;
		const UnsignedShort mt    = (UnsignedShort)msgType;
		const UnsignedShort sv    = AIBRIDGE_SCHEMA_VERSION;
		UnsignedByte hdr[12];
		hdr[0]=(UnsignedByte)(magic);      hdr[1]=(UnsignedByte)(magic>>8);  hdr[2]=(UnsignedByte)(magic>>16); hdr[3]=(UnsignedByte)(magic>>24);
		hdr[4]=(UnsignedByte)(plen);       hdr[5]=(UnsignedByte)(plen>>8);   hdr[6]=(UnsignedByte)(plen>>16);  hdr[7]=(UnsignedByte)(plen>>24);
		hdr[8]=(UnsignedByte)(mt);         hdr[9]=(UnsignedByte)(mt>>8);
		hdr[10]=(UnsignedByte)(sv);        hdr[11]=(UnsignedByte)(sv>>8);
		fwrite(hdr, 1, 12, s_obsLogFile);
		if (payloadSize > 0)
			fwrite(payload, 1, (size_t)payloadSize, s_obsLogFile);
	}
}

// Configured by CommandLine (-aiobslog / -aiobsdecimate) before subsystem init.
void AIBridge_setObsLogPath(const char* path) { if (path != nullptr) s_obsLogPath = path; }
void AIBridge_setObsLogDecimate(Int n)        { if (n >= 1) s_obsLogDecimate = n; }

// The headless replay loop (Core/ReplaySimulation.cpp) runs its own tight loop
// that bypasses GameEngine::update(), so the AIBridge is never pumped during
// -replay. Core exposes a null-defaulted per-frame hook pointer that the higher
// GeneralsMD layer registers here (Core must not include AIBridge.h). Once set,
// the bridge streams observations frame-by-frame during headless replay exactly
// as it does during a live skirmish.
extern void (*TheHeadlessReplayFrameHook)(void);
namespace
{
	void aibridgeReplayFrameTick(void)
	{
		if (TheAIBridge != nullptr)
			TheAIBridge->tick();
	}
}

AIBridge* createAIBridge()
{
	return new AIBridge;
}

AIBridge::AIBridge()
	: m_started(FALSE)
	, m_server(nullptr)
{
}

AIBridge::~AIBridge()
{
	if (m_server != nullptr)
	{
		m_server->shutdownAll();
		delete m_server;
		m_server = nullptr;
	}
	if (s_obsLogFile != nullptr)
	{
		fclose(s_obsLogFile);
		s_obsLogFile = nullptr;
	}
}

Bool AIBridge::isEnabled() const
{
	return s_configuredPort > 0;
}

void AIBridge::init()
{
	// Open the observation log if requested. Independent of the socket bridge, so
	// -aiobslog works during -replay with or without -aibridgeport.
	if (!s_obsLogPath.isEmpty() && s_obsLogFile == nullptr)
	{
		s_obsLogFile = fopen(s_obsLogPath.str(), "wb");
		if (s_obsLogFile == nullptr)
			DEBUG_LOG(("AIBridge: could not open obs log '%s'", s_obsLogPath.str()));
	}

	// Lazily start the listener only if -aibridgeport was passed.
	if (isEnabled() && !m_started)
	{
		m_server = new AIBridgeServer;
		if (m_server->startListening(s_configuredPort))
			m_started = TRUE;
		else
		{
			delete m_server;
			m_server = nullptr;
		}
	}

	// Register the per-frame pump so observations are also produced during the
	// headless -replay loop, which does not run GameEngine::update().
	if (m_server != nullptr || s_obsLogFile != nullptr)
		TheHeadlessReplayFrameHook = aibridgeReplayFrameTick;
}

void AIBridge::reset()
{
	// Per-game reset: drop any client and clear buffers, but keep the
	// listener up so the same bot process can reconnect for the next game.
	if (m_server != nullptr)
	{
		m_server->shutdownAll();
		delete m_server;
		m_server = nullptr;
		m_started = FALSE;
	}
	if (isEnabled())
	{
		m_server = new AIBridgeServer;
		if (m_server->startListening(s_configuredPort))
		{
			m_started = TRUE;
		}
		else
		{
			delete m_server;
			m_server = nullptr;
		}
	}
}

void AIBridge::update()
{
	// We deliberately do nothing here: the bridge drives off the explicit
	// tick() call from GameEngine::update() so we control exactly when it
	// runs relative to MessageStream propagation and Network update.
}

Bool AIBridge::isCommandSourceLive() const
{
	if (s_configuredSlot < 0) return FALSE;
	if (m_server == nullptr) return FALSE;
	return m_server->isClientConnected();
}

namespace
{
	// The -aibridgeslot value is a LOBBY SLOT (0..7), but observation self-flags,
	// shroud checks, and action stamping all need the engine PLAYER INDEX, which
	// is generally NOT equal to the slot (neutral/civilian take low indices).
	// Skirmish players are named "player<slot>", so resolve the slot to the real
	// player index via the player list. Returns -1 for observation-only mode and
	// falls back to the raw slot if the lookup fails (e.g. pre-game).
	Int resolveBotPlayerIndex(Int slot)
	{
		if (slot < 0) return -1;
		if (ThePlayerList == nullptr || TheNameKeyGenerator == nullptr) return slot;
		AsciiString name;
		name.format("player%d", slot);
		Player* p = ThePlayerList->findPlayerWithNameKey(TheNameKeyGenerator->nameToKey(name));
		return (p != nullptr) ? (Int)p->getPlayerIndex() : slot;
	}
}

// Suppression hook for the scripted AI. Returns TRUE when the AIBridge is
// configured to drive the given engine player index. The scripted AISkirmishPlayer
// consults this to stop issuing army/combat orders to a bridge-controlled slot (so
// external orders issued at ~1s cadence are not countermanded every frame), while
// leaving that player's economy, base building, and production running normally.
// Returns FALSE in observation-only mode (no -aibridgeslot) so ordinary AI slots
// are never affected.
Bool AIBridge_isControllingPlayer(Int playerIndex)
{
	if (playerIndex < 0) return FALSE;
	if (s_configuredSlot < 0) return FALSE;
	return resolveBotPlayerIndex(s_configuredSlot) == playerIndex;
}

void AIBridge::tick()
{
	const Bool haveServer = (m_started && m_server != nullptr);
	const Bool haveLog    = (s_obsLogFile != nullptr);
	// Nothing to do unless a socket bridge is live or an observation log is open.
	if (!haveServer && !haveLog)
		return;

	// Resolve the configured lobby slot to the engine player index once per tick.
	const Int botPlayerIndex = resolveBotPlayerIndex(s_configuredSlot);

	if (haveServer)
	{
		// Drive socket I/O.
		m_server->poll();

		// On a fresh connection, send Hello first. Detect the connection edge via
		// a sticky bit combined with the public isClientConnected().
		static Bool s_helloSent = FALSE;
		if (!m_server->isClientConnected())
		{
			s_helloSent = FALSE;
		}
		else if (!s_helloSent)
		{
			ByteWriter w;
			buildHelloPayload(w, botPlayerIndex);
			m_server->sendFrame(MSG_HELLO, w.data(), w.size());
			s_helloSent = TRUE;
		}

		// Drain inbound action batches.
		if (isCommandSourceLive())
		{
			Int msgType;
			const UnsignedByte* payload = nullptr;
			Int payloadSize = 0;
			while (m_server->popInboundFrame(msgType, payload, payloadSize))
			{
				if (msgType == MSG_ACTION_BATCH)
					parseActionBatch(botPlayerIndex, payload, payloadSize);
				else if (msgType == MSG_HELLO_ACK)
					; // optional; ignored
				else
					DEBUG_LOG(("AIBridge: unknown inbound msg type %d", msgType));
			}
		}
		else
		{
			// Even in observation-only mode, drain & discard inbound frames so
			// the buffer doesn't grow unbounded.
			Int msgType;
			const UnsignedByte* payload = nullptr;
			Int payloadSize = 0;
			while (m_server->popInboundFrame(msgType, payload, payloadSize))
				(void)msgType;
		}
	}

	// Emit an observation for this frame. The live socket bridge only sends when a
	// bot is connected; the observation log (used for offline replay -> dataset
	// extraction, where the flat-out headless replay loop starves wine's socket
	// layer) is written every Nth frame regardless. Build the payload at most once.
	const Bool clientConnected = haveServer && m_server->isClientConnected();
	Bool writeLog = FALSE;
	if (haveLog)
	{
		const UnsignedInt frame = (TheGameLogic != nullptr) ? TheGameLogic->getFrame() : 0;
		writeLog = (s_obsLogDecimate <= 1) || ((frame % (UnsignedInt)s_obsLogDecimate) == 0);
	}
	if (clientConnected || writeLog)
	{
		ByteWriter w;
		buildObservationPayload(w, botPlayerIndex);
		if (clientConnected)
			m_server->sendFrame(MSG_OBSERVATION, w.data(), w.size());
		if (writeLog)
			writeObsLogFrame(MSG_OBSERVATION, w.data(), w.size());
	}
}
