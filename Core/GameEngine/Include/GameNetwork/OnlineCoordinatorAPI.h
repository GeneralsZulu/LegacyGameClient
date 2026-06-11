/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
*/

// OnlineCoordinatorAPI.h
// Client for the external matchmaking coordinator that lets players find
// each other across the internet. Single-threaded, non-blocking sockets.
// Pumped by the caller via update().
//
// Wire protocol matches tools/coordinator: newline-delimited JSON over TCP
// for signaling, small binary STUN-style probe over UDP for public-address
// discovery and hole punch.
//
// Typical flow:
//   1) connect()  -> STATE_CONNECTING -> STATE_HANDSHAKING -> STATE_DISCOVERING
//   2) STUN probe loop completes      -> STATE_READY
//   3) requestList() / requestHost() / requestJoin()
//   4) peer_info arrives              -> STATE_PUNCHING (auto)
//   5) punch packets exchanged        -> STATE_PUNCH_OK
// The caller reads peerInfo() to discover the punched-through peer address
// and hands it off to the rest of the game-setup flow.

#pragma once

#include "Lib/BaseType.h"
#include "Common/AsciiString.h"
#include "Common/UnicodeString.h"

#include <vector>

class OnlineCoordinatorAPI
{
public:
	enum State
	{
		STATE_IDLE,
		STATE_CONNECTING,    // TCP connect() in flight
		STATE_HANDSHAKING,   // sent hello, awaiting hello_ok
		STATE_DISCOVERING,   // sending UDP STUN probes, awaiting response
		STATE_READY,         // public address known; can host/list/join
		STATE_HOSTING,       // hosted; waiting for a joiner
		STATE_JOINING,       // sent join; waiting for peer_info
		STATE_PUNCHING,      // peer_info received; firing UDP punch packets
		STATE_PUNCH_OK,      // received an inbound UDP packet from peer
		STATE_ERROR,
	};

	struct GameListEntry
	{
		AsciiString   id;
		AsciiString   name;
		AsciiString   hostNick;
		AsciiString   map;
		Int           players;
		Int           maxPlayers;
	};

	struct PeerInfo
	{
		AsciiString   nick;
		AsciiString   publicAddr;   // "ip:port" as told by the coordinator
		AsciiString   localAddr;    // "ip:port" of peer's LAN interface, if any
		Int           punchInMS;
		AsciiString   role;         // "host" or "guest" relative to the peer (i.e. our role)

		// Filled in once a UDP packet is received from the peer. Both values
		// are stored in HOST byte order to match the LAN networking layer
		// (UDP::Write applies htonl/htons internally; LANAPI's m_localIP
		// and slot getIP/getPort are also host-order).
		UnsignedInt   punchedIP;
		UnsignedShort punchedPort;
	};

	OnlineCoordinatorAPI();
	~OnlineCoordinatorAPI();

	// Resolve and TCP-connect to the coordinator, open a UDP socket, and
	// send the HELLO. coordHost is "name-or-ip", tcpPort is the coordinator's
	// signaling port. udpBindPort = 0 lets the OS pick; pass a specific port
	// only if you want to align with another component's binding.
	Bool connect(const AsciiString& coordHost,
		UnsignedShort tcpPort,
		const AsciiString& nick,
		const AsciiString& version,
		UnsignedShort udpBindPort = 0);

	void disconnect();

	// Non-blocking poll. Call every frame while a session is active.
	void update();

	// Once STATE_READY:
	void requestList();
	void requestHost(const UnicodeString& gameName, const AsciiString& mapName, Int maxPlayers);
	void requestUnhost();
	void requestJoin(const AsciiString& gameID);

	State                            state()       const { return m_state; }
	const AsciiString&               publicAddr()  const { return m_publicAddr; }
	const AsciiString&               localAddr()   const { return m_localAddr; }
	const AsciiString&               lastError()   const { return m_lastError; }
	const std::vector<GameListEntry>& games()      const { return m_games; }
	const PeerInfo&                  peerInfo()    const { return m_peerInfo; }
	const AsciiString&               hostedGameID() const { return m_hostedGameID; }
	UnsignedShort                    udpPort()     const { return m_udpBoundPort; }
	// TRUE between requestHost() and the next connect(). Authoritative for
	// "am I hosting this match?" so the caller does not have to interpret
	// the peer_info.role string (which has cost us a debugging session).
	Bool                             amIHost()     const { return m_amIHost; }

private:
	State         m_state;
	Int           m_tcpFd;
	Int           m_udpFd;
	UnsignedShort m_udpBoundPort;

	AsciiString   m_nick;
	AsciiString   m_version;

	AsciiString   m_sessionToken;     // hex, 32 chars
	UnsignedInt   m_stunMagic;
	UnsignedShort m_coordUdpPort;
	UnsignedInt   m_coordIPNet;       // network-order, for UDP sendto

	AsciiString   m_publicAddr;
	AsciiString   m_localAddr;
	AsciiString   m_lastError;
	AsciiString   m_hostedGameID;

	std::vector<GameListEntry> m_games;
	PeerInfo      m_peerInfo;
	Bool          m_peerInfoArmed;    // peer_info received; punch in progress
	Bool          m_amIHost;          // TRUE if we called requestHost()

	// STUN discovery
	UnsignedInt   m_stunNextProbeMs;
	Int           m_stunProbesSent;

	// Hole punch
	UnsignedInt   m_punchStartMs;
	UnsignedInt   m_punchNextBlastMs;
	UnsignedInt   m_punchDeadlineMs;

	// TCP receive buffer (raw bytes; lines extracted in pumpTcpRecv)
	std::vector<char> m_rxBuf;

	void  setState(State s);
	void  setError(const AsciiString& msg);
	void  closeSockets();

	Bool  openUdp(UnsignedShort bindPort);
	Bool  beginTcpConnect(UnsignedInt ipHostOrder, UnsignedShort port);

	void  sendJsonLine(const AsciiString& line);
	void  pumpTcpRecv();
	void  onTcpMessage(const char* msgType, const char* dataJsonObj);

	void  sendStunProbe();
	void  pumpUdpRecv();
	void  pumpStunDiscovery(UnsignedInt nowMs);
	void  pumpPunch(UnsignedInt nowMs);
	void  blastPunchPackets();
};
