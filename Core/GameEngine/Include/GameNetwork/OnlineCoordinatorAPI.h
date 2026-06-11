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
		AsciiString   publicAddr;       // lobby (8086) "ip:port" as told by the coordinator
		AsciiString   gamePublicAddr;   // in-game (8088) "ip:port" as told by the coordinator
		AsciiString   localAddr;        // "ip:port" of peer's LAN interface, if any (lobby side)
		Int           punchInMS;
		AsciiString   role;             // "host" or "guest" relative to the peer (i.e. our role)

		// Filled in once a UDP packet is received from the peer on the lobby
		// socket. Stored in HOST byte order to match the LAN networking layer
		// (UDP::Write applies htonl/htons internally; LANAPI's m_localIP and
		// slot getIP/getPort are also host-order).
		UnsignedInt   punchedIP;
		UnsignedShort punchedPort;

		// Same, but for the in-game data socket. ConnectionManager talks to
		// the peer here once the game starts.
		UnsignedInt   gamePunchedIP;
		UnsignedShort gamePunchedPort;
	};

	OnlineCoordinatorAPI();
	~OnlineCoordinatorAPI();

	// Resolve and TCP-connect to the coordinator, open BOTH UDP sockets, and
	// send the HELLO. coordHost is "name-or-ip", tcpPort is the coordinator's
	// signaling port.
	//   lobbyBindPort: UDP port that will be handed off to TheLAN's transport
	//     after punching (typically 8086).
	//   gameBindPort: UDP port that will be handed off to ConnectionManager
	//     after punching (typically NETWORK_BASE_PORT_NUMBER = 8088).
	// Both must be bindable on this host; a per-port punch is performed so
	// each socket has its own valid NAT mapping.
	Bool connect(const AsciiString& coordHost,
		UnsignedShort tcpPort,
		const AsciiString& nick,
		const AsciiString& version,
		UnsignedShort lobbyBindPort,
		UnsignedShort gameBindPort);

	void disconnect();

	// Non-blocking poll. Call every frame while a session is active.
	void update();

	// Once STATE_READY:
	void requestList();
	void requestHost(const UnicodeString& gameName, const AsciiString& mapName, Int maxPlayers);
	void requestUnhost();
	void requestJoin(const AsciiString& gameID);

	// Hand the game UDP socket off to the global stash so it survives the
	// destruction of this OnlineCoordinatorAPI instance at handoff time
	// (LanLobbyMenu is destroyed shortly after PUNCH_OK). After this call
	// the instance no longer owns the game socket; disconnect() will not
	// close it. Returns FALSE if there is no live game socket to stash.
	Bool stashGameSocketForGameStart();

	// --- Static lobby-phase stash (game socket survives lobby destruction) ---
	// During the LAN-lobby phase, between PUNCH_OK and the game actually
	// starting, the game UDP socket sits in this module-level stash. A
	// periodic keepalive sender is required because the lobby phase can
	// easily exceed typical NAT UDP idle timeouts. ConnectionManager picks
	// the FD up at game start via takeStashedGameFd().
	static Bool          hasStashedGameSocket();
	static Int           takeStashedGameFd();                   // -1 if none; transfers ownership
	static UnsignedShort stashedGameLocalPort();
	static UnsignedInt   stashedGamePeerIPHost();
	static UnsignedShort stashedGamePeerPortHost();
	static void          pumpStashedKeepalive();                // call from a per-frame update loop
	static void          discardStashedGameSocket();            // emergency cleanup (closes the FD)

	State                            state()           const { return m_state; }
	const AsciiString&               publicAddr()      const { return m_publicAddrLobby; }
	const AsciiString&               gamePublicAddr()  const { return m_publicAddrGame; }
	const AsciiString&               localAddr()       const { return m_localAddr; }
	const AsciiString&               lastError()       const { return m_lastError; }
	const std::vector<GameListEntry>& games()          const { return m_games; }
	const PeerInfo&                  peerInfo()        const { return m_peerInfo; }
	const AsciiString&               hostedGameID()    const { return m_hostedGameID; }
	UnsignedShort                    udpPort()         const { return m_udpBoundPortLobby; }
	UnsignedShort                    udpGamePort()     const { return m_udpBoundPortGame; }
	// TRUE between requestHost() and the next connect(). Authoritative for
	// "am I hosting this match?" so the caller does not have to interpret
	// the peer_info.role string (which has cost us a debugging session).
	Bool                             amIHost()         const { return m_amIHost; }

private:
	State         m_state;
	Int           m_tcpFd;
	// Two UDP sockets are kept in lockstep: one for the LAN lobby protocol
	// (8086) and one for the in-game data channel (8088). Each is STUNed and
	// punched separately so each has its own NAT mapping, which is required
	// because cone NATs translate per-socket and a mapping established on
	// one socket does not carry over to another.
	Int           m_udpFdLobby;
	Int           m_udpFdGame;
	UnsignedShort m_udpBoundPortLobby;
	UnsignedShort m_udpBoundPortGame;

	AsciiString   m_nick;
	AsciiString   m_version;

	AsciiString   m_sessionToken;     // hex, 32 chars
	UnsignedInt   m_stunMagic;
	UnsignedShort m_coordUdpPort;
	UnsignedInt   m_coordIPNet;       // network-order, for UDP sendto

	AsciiString   m_publicAddrLobby;
	AsciiString   m_publicAddrGame;
	AsciiString   m_localAddr;
	AsciiString   m_lastError;
	AsciiString   m_hostedGameID;

	std::vector<GameListEntry> m_games;
	PeerInfo      m_peerInfo;
	Bool          m_peerInfoArmed;    // peer_info received; punch in progress
	Bool          m_amIHost;          // TRUE if we called requestHost()

	// STUN discovery: track per-socket so the transition to STATE_READY
	// waits until BOTH external addresses have been observed.
	UnsignedInt   m_stunNextProbeMsLobby;
	UnsignedInt   m_stunNextProbeMsGame;
	Int           m_stunProbesSentLobby;
	Int           m_stunProbesSentGame;
	Bool          m_stunOkLobby;
	Bool          m_stunOkGame;

	// Hole punch: per-socket success flags. STATE_PUNCH_OK is set only after
	// inbound traffic has been observed on BOTH sockets.
	UnsignedInt   m_punchStartMs;
	UnsignedInt   m_punchNextBlastMs;
	UnsignedInt   m_punchDeadlineMs;
	Bool          m_punchOkLobby;
	Bool          m_punchOkGame;

	// TCP receive buffer (raw bytes; lines extracted in pumpTcpRecv)
	std::vector<char> m_rxBuf;

	void  setState(State s);
	void  setError(const AsciiString& msg);
	void  closeSockets();

	Bool  openUdpOnPort(UnsignedShort bindPort, Int& outFd, UnsignedShort& outBoundPort);
	Bool  beginTcpConnect(UnsignedInt ipHostOrder, UnsignedShort port);

	void  sendJsonLine(const AsciiString& line);
	void  pumpTcpRecv();
	void  onTcpMessage(const char* msgType, const char* dataJsonObj);

	// purpose: 0 = lobby, 1 = game (matches STUNPurpose* in protocol.go).
	void  sendStunProbe(Int fd, unsigned char purpose);
	void  pumpUdpRecv();
	void  pumpUdpRecvOne(Int fd, Bool isGame);
	void  pumpStunDiscovery(UnsignedInt nowMs);
	void  pumpPunch(UnsignedInt nowMs);
	void  blastPunchPacketsOn(Int fd, const AsciiString& publicAddr, const AsciiString& localAddr);
};
