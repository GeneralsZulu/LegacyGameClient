/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

////////////////////////////////////////////////////////////////////////////////
//																																						//
//  (c) 2001-2003 Electronic Arts Inc.																				//
//																																						//
////////////////////////////////////////////////////////////////////////////////

// Transport.h ///////////////////////////////////////////////////////////////
// Transport layer - a thin layer around a UDP socket, with queues.
// Author: Matthew D. Campbell, July 2001

#pragma once

#include "GameNetwork/udp.h"
#include "GameNetwork/NetworkDefs.h"

/**
 * The transport layer handles the UDP socket for the game, and will packetize and
 * de-packetize multiple ACK/CommandPacket/etc packets into larger aggregates.
 */
// we only ever allocate one of there, and it is quite large, so we really DON'T want
// it to be a MemoryPoolObject (srj)
class Transport //: public MemoryPoolObject
{
	//MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE(Transport, "Transport")
public:

	Transport();
	~Transport();

	Bool init( AsciiString ip, UnsignedShort port );
	/// @param logFailure FALSE for a speculative bind whose caller re-binds or
	///        adopts a socket on the same port immediately afterwards. Such a
	///        bind is expected to fail whenever something else legitimately
	///        still holds the port, and the follow-up call reports any failure
	///        that actually matters. Defaults to TRUE.
	Bool init( UnsignedInt ip, UnsignedShort port, Bool logFailure = TRUE );
	/// Initialize from an already-bound UDP socket FD instead of binding a new
	/// one. Used by the online-coordinator handoff so ConnectionManager keeps
	/// the same socket (and therefore the same NAT mapping) that was punched
	/// during the coordinator session.
	Bool initFromFD( Int fd, UnsignedInt ip, UnsignedShort port );
	// Immediately send a tiny raw probe with a temporary IPv4 TTL. Used as a
	// NAT-opening packet that creates our outbound mapping but expires in
	// transit before it can reach (and poison) the peer's NAT. Bypasses the
	// normal queue/CRC framing; the payload is meant to be dropped.
	Bool sendNATProbe( UnsignedInt ip, UnsignedShort port, Int ttl );
	/// The raw socket, so code that owned this port before the coordinator
	/// handoff can keep sending on it (the online coordinator's STUN
	/// keepalive). Returns -1 when there is no socket. Anything written here
	/// bypasses the queue and the CRC framing, and any reply lands in
	/// doRecv()'s unknown-packet bucket, so only send traffic the peer is
	/// meant to ignore.
	Int getRawFD() const;
	/// Which relay channel this socket carries (RelayRegistry::CHANNEL_*).
	/// Set by the coordinator handoff paths (LANAPI adopts the lobby socket,
	/// ConnectionManager adopts the game socket); -1 (default) for pure-LAN
	/// transports. With a channel set and relay-capable peers registered,
	/// doSend also emits a periodic relay keepalive to the coordinator so
	/// the server always has a fresh return address for this socket even
	/// when all links are currently direct (a mid-game silence-trigger flip
	/// needs the very first relayed frame to be deliverable).
	void setRelayChannel(Int channel) { m_relayChannel = channel; }
	void reset();
	Bool update();									///< Call this once a GameEngine tick, regardless of whether the frame advances.

	Bool doRecv();		///< call this to service the receive packets
	Bool doSend();		///< call this to service the send queue.

	Bool queueSend(UnsignedInt addr, UnsignedShort port, const UnsignedByte *buf, Int len /*,
		NetMessageFlags flags, Int id */);				///< Queue a packet for sending to the specified address and port.  This will be sent on the next update() call.

	Bool allowBroadcasts(Bool val) { if (!m_udpsock) return false; return (m_udpsock->AllowBroadcasts(val))?true:false; }

	// Latency insertion and packet loss
	void setLatency( Bool val ) { m_useLatency = val; }
	void setPacketLoss( Bool val ) { m_usePacketLoss = val; }

	// Bandwidth metrics
	Real getIncomingBytesPerSecond();
	Real getIncomingPacketsPerSecond();
	Real getOutgoingBytesPerSecond();
	Real getOutgoingPacketsPerSecond();
	Real getUnknownBytesPerSecond();
	Real getUnknownPacketsPerSecond();

	TransportMessage m_outBuffer[MAX_MESSAGES];
	TransportMessage m_inBuffer[MAX_MESSAGES];

#if defined(RTS_DEBUG)
	DelayedTransportMessage m_delayedInBuffer[MAX_MESSAGES];
#endif

	UnsignedShort m_port;
private:
	Bool m_winsockInit;
	UDP *m_udpsock;

	// Relay fallback plumbing (see RelayRegistry.h). -1 = not a coordinator
	// socket, no relay work at all.
	Int m_relayChannel;
	UnsignedInt m_relayKeepaliveNextMs;

	Bool finishInit( UnsignedShort port );  // shared tail of init() / initFromFD()

	// Latency insertion and packet loss
	Bool m_useLatency;
	Bool m_usePacketLoss;

	// Bandwidth metrics
	UnsignedInt m_incomingBytes[MAX_TRANSPORT_STATISTICS_SECONDS];
	UnsignedInt m_unknownBytes[MAX_TRANSPORT_STATISTICS_SECONDS];
	UnsignedInt m_outgoingBytes[MAX_TRANSPORT_STATISTICS_SECONDS];
	UnsignedInt m_incomingPackets[MAX_TRANSPORT_STATISTICS_SECONDS];
	UnsignedInt m_unknownPackets[MAX_TRANSPORT_STATISTICS_SECONDS];
	UnsignedInt m_outgoingPackets[MAX_TRANSPORT_STATISTICS_SECONDS];
	Int m_statisticsSlot;
	UnsignedInt m_lastSecond;

	Bool isGeneralsPacket( TransportMessage *msg );
};
