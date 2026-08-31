/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
*/

// RelayRegistry.h
// Module-level table of coordinator-brokered peers and whether traffic to
// each one goes direct (hole-punched) or wrapped through the coordinator's
// UDP relay. Static (like the game-socket stash in OnlineCoordinatorAPI)
// because the punched sockets outlive every object that owns them: the fd
// travels OnlineCoordinatorAPI -> TheLAN's Transport -> the stash ->
// ConnectionManager's Transport, and the relay decision has to travel with
// it.
//
// The LAN layer never sees the relay. Transport::doSend consults the table
// and wraps the (already CRC'd and XOR-encrypted) packet in a small relay
// header addressed to the coordinator; Transport::doRecv unwraps inbound
// relay frames and presents them with the peer's LOGICAL address (the
// public addr the coordinator advertised), so slot lookup, reply-port
// capture and the per-peer game-port map all behave exactly as on a direct
// path.
//
// Three rules decide the direct-vs-relay flip, all of which only ever move
// a pair TOWARD a path that is delivering packets:
//  1. relay_grant from the coordinator (both sides of a failed punch pair).
//  2. Sticky rule: receiving a relayed frame from a peer flips our sends to
//     that peer (handled in handleIncoming).
//  3. Silence trigger: actively sending to a peer for RELAY_SILENCE_FLIP_MS
//     without hearing anything back flips our sends (handled in
//     wrapForSend). Safe to fire spuriously; relay is just another path for
//     the same bytes.
//
// Empty in pure-LAN games, so every check short-circuits on isActive().
//
// Wire format (must match tools/coordinator/protocol.go):
//  client->server RelayData:    magic(4 BE) token(16) purpose=2 channel(1) destRelayID(4 BE) payload
//  server->client RelayDeliver: magic(4 BE) purpose=3 channel(1) srcRelayID(4 BE) payload

#pragma once

#include "Lib/BaseType.h"
#include "Common/AsciiString.h"

class RelayRegistry
{
public:
	enum
	{
		CHANNEL_LOBBY = 0,
		CHANNEL_GAME  = 1,
	};

	// Wire sizes. Client header = 4 magic + 16 token + 1 purpose + 1 channel
	// + 4 dest relay id; server header drops the token.
	enum
	{
		RELAY_DATA_HEADER_SIZE    = 26,
		RELAY_DELIVER_HEADER_SIZE = 10,
	};

	// Called from OnlineCoordinatorAPI when hello_ok carries a relay_id
	// (i.e. server supports relaying). Clears any previous session's peers.
	// token is the 16-byte decoded session token.
	static void configure(UnsignedInt coordIPHost, UnsignedShort coordUdpPort,
		UnsignedInt magic, const unsigned char token[16], UnsignedInt myRelayID);

	// Full teardown: called on a fresh coordinator connect and at game
	// teardown (ConnectionManager::reset) so stale entries can never
	// misroute a later match's traffic.
	static void clear();

	static Bool        isActive();
	static UnsignedInt myRelayID();

	// Register one channel of a coordinator peer, keyed by the logical
	// address we will send to. viaRelay starts FALSE (direct until proven
	// otherwise) unless forced later. Re-registering the same relayID and
	// channel updates the address (join retries).
	static void addPeer(UnsignedInt relayID, Int channel,
		UnsignedInt ipHost, UnsignedShort portHost, const AsciiString& nick);

	// Punch succeeded from a possibly different source addr than advertised
	// (port-drifting NATs): re-key the entry so it matches the addr the LAN
	// layer will actually be sending to.
	static void rekeyPeer(UnsignedInt relayID, Int channel,
		UnsignedInt ipHost, UnsignedShort portHost);

	static void forceRelay(UnsignedInt relayID);                 // both channels
	static void forceRelayChannel(UnsignedInt relayID, Int channel);
	static Bool hasPeer(UnsignedInt relayID);

	// --- Transport send path ---
	// If (ipHost:portHost) is a registered peer whose sends go via relay
	// (including a silence-trigger flip decided right here), wrap payload
	// into outBuf and return TRUE with the coordinator's address; caller
	// sends outBuf there instead. Returns FALSE to send direct. When
	// *alsoSendDirect comes back TRUE, the caller additionally sends the
	// PLAIN packet to the peer's address: the direct-upgrade probe that
	// lets a spuriously flipped pair converge back to direct.
	static Bool wrapForSend(UnsignedInt ipHost, UnsignedShort portHost,
		const unsigned char* payload, Int payloadLen,
		unsigned char* outBuf, Int outCap, Int* outLen,
		UnsignedInt* outCoordIPHost, UnsignedShort* outCoordPort,
		Bool* alsoSendDirect);

	// Like wrapForSend but WITHOUT the silence-trigger decision: wraps only
	// when the peer is already flipped. For raw senders on sockets nothing
	// is currently reading (the lobby-phase game-socket stash keepalive),
	// where "no packets received" says nothing about the direct path.
	static Bool wrapIfRelayed(UnsignedInt ipHost, UnsignedShort portHost,
		const unsigned char* payload, Int payloadLen,
		unsigned char* outBuf, Int outCap, Int* outLen,
		UnsignedInt* outCoordIPHost, UnsignedShort* outCoordPort);

	// --- Transport receive path ---
	// Returns TRUE when the packet came from the coordinator (caller's
	// normal processing must not see it as-is). If *drop, discard it (STUN
	// reply or malformed). Otherwise the relay header has been stripped in
	// place (payload moved to buf[0]), *newLen set, and the peer's logical
	// address returned; the caller presents the packet as coming from that
	// address. Applies the sticky flip and the receive timestamp.
	static Bool handleIncoming(UnsignedInt srcIPHost, UnsignedShort srcPortHost,
		unsigned char* buf, Int len, Int* newLen,
		UnsignedInt* logicalIPHost, UnsignedShort* logicalPortHost, Bool* drop);

	// Direct packet seen from (ipHost:portHost): refresh the silence timer.
	static void noteDirectRecv(UnsignedInt ipHost, UnsignedShort portHost);

	// Called when ConnectionManager adopts the game socket at match start.
	// Starts the longstop grace period: while everyone is loading the map,
	// silence is normal (and pre-load stash keepalives mean every peer has
	// already been "heard"), so the 12s longstop must not fire during the
	// load window.
	static void noteGameTransportAdopted();

	// Empty RelayData frame (destRelayID 0): registers/refreshes our return
	// address for this channel at the server and keeps the NAT mapping to
	// the coordinator warm. Sent by Transport every RELAY_KEEPALIVE_MS while
	// it has relay-capable peers on its channel.
	static Bool buildKeepalive(Int channel,
		unsigned char* outBuf, Int outCap, Int* outLen,
		UnsignedInt* outCoordIPHost, UnsignedShort* outCoordPort);

	static Bool anyPeerOnChannel(Int channel);

	// For the NETPATH heartbeat log: how many peers this channel has and how
	// many of them are currently relayed.
	static void countPeers(Int channel, Int* total, Int* relayed);

	// Whether sends to this address are currently relayed (stash keepalive
	// uses this plus wrapForSend; also handy for logging).
	static Bool isRelayedAddr(UnsignedInt ipHost, UnsignedShort portHost);

	// TRUE when (ipHost:portHost) is the registered address of a peer OTHER
	// than relayID. Punch evidence validation: two players behind one NAT
	// share an IP, so a same-IP sibling's keepalive passes an IP-only check
	// and rekeys the punch peer's entry to the sibling's port (seen live in
	// T2-FULL8, c1/c2 behind one Cloud NAT IP).
	static Bool isOtherPeerAddr(UnsignedInt relayID, UnsignedInt ipHost, UnsignedShort portHost);
};
