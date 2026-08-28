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

// LANAPI.h ///////////////////////////////////////////////////////////////
// LANAPI singleton class - defines interface to LAN broadcast communications
// Author: Matthew D. Campbell, October 2001

#pragma once

#include <map>

#include "GameNetwork/Transport.h"
#include "GameNetwork/NetworkInterface.h"
#include "GameNetwork/NetworkDefs.h"
#include "GameNetwork/LANPlayer.h"
#include "GameNetwork/LANGameInfo.h"
#include "GameNetwork/LANObserverStream.h"

class OnlineCoordinatorAPI;

//static const Int g_lanPlayerNameLength = 20;
static const Int g_lanPlayerNameLength = 12; // reduced length because of game option length
//static const Int g_lanLoginNameLength = 16;
//static const Int g_lanHostNameLength = 16;
static const Int g_lanLoginNameLength = 1;
static const Int g_lanHostNameLength = 1;
//static const Int g_lanGameNameLength = 32;
static const Int g_lanGameNameLength = 16; // reduced length because of game option length
static const Int g_lanGameNameReservedLength = 16; // save N wchars for ID info
static const Int g_lanMaxChatLength = 200; // max ~201 before LANMessage exceeds MAX_LANAPI_PACKET_SIZE (see static_assert below)
static const Int m_lanMaxOptionsLength = MAX_LANAPI_PACKET_SIZE - ( 8 + (g_lanGameNameLength+1)*2 + 4 + (g_lanPlayerNameLength+1)*2
																														+ (g_lanLoginNameLength+1) + (g_lanHostNameLength+1) );
static const Int g_maxSerialLength = 23; // including the trailing '\0'

struct LANMessage;

/**
 * The LANAPI class is used to instantiate a singleton which
 * implements the interface to all LAN broadcast communications.
 */
class LANAPIInterface : public SubsystemInterface
{
public:

	virtual ~LANAPIInterface() override { };

	virtual void init() = 0;															///< Initialize or re-initialize the instance
	virtual void reset() = 0;															///< reset the logic system
	virtual void update() = 0;														///< update the world

	virtual void setIsActive(Bool isActive ) = 0;								///< Tell TheLAN whether or not the app is active.

	// Possible types of chat messages
	enum ChatType
	{
		LANCHAT_NORMAL = 0,
		LANCHAT_EMOTE,
		LANCHAT_SYSTEM,
	};

	// Request functions generate network traffic
	virtual void RequestLocations() = 0;																				///< Request everybody to respond with where they are
	virtual void RequestGameJoin( LANGameInfo *game, UnsignedInt ip = 0 ) = 0;				///< Request to join a game
	virtual void RequestGameJoinDirectConnect( UnsignedInt ipaddress ) = 0;						///< Request to join a game at an IP address
	virtual void RequestGameLeave() = 0;																				///< Tell everyone we're leaving
	virtual void RequestAccept() = 0;																						///< Indicate we're OK with the game options
	virtual void RequestHasMap() = 0;																						///< Send our map status
	virtual void RequestChat( UnicodeString message, ChatType format ) = 0;						///< Send a chat message
	virtual void RequestGameStart() = 0;																				///< Tell everyone the game is starting
	virtual void RequestGameStartTimer( Int seconds ) = 0;
	virtual void RequestGameOptions( AsciiString gameOptions, Bool isPublic, UnsignedInt ip = 0 ) = 0;		///< Change the game options
	virtual void RequestGameCreate( UnicodeString gameName, Bool isDirectConnect ) = 0;	///< Try to host a game
	virtual void RequestGameAnnounce() = 0;																			///< Sound out current game info if host
//	virtual void RequestSlotList() = 0;																					///< Pump out the Slot info.
	virtual void RequestSetName( UnicodeString newName ) = 0;													///< Pick a new name
	virtual void RequestLobbyLeave( Bool forced ) = 0;																///< Announce that we're leaving the lobby
	virtual void ResetGameStartTimer() = 0;

	// Possible result codes passed to On functions
	enum ReturnType
	{
		RET_OK,							// Any function
		RET_TIMEOUT,				// OnGameJoin/Leave/Start, etc
		RET_GAME_FULL,			// OnGameJoin
		RET_DUPLICATE_NAME,	// OnGameJoin
		RET_CRC_MISMATCH,		// OnGameJoin
		RET_SERIAL_DUPE,		// OnGameJoin
		RET_GAME_STARTED,		// OnGameJoin
		RET_GAME_EXISTS,		// OnGameCreate
		RET_GAME_GONE,			// OnGameJoin
		RET_BUSY,						// OnGameCreate/Join/etc if another action is in progress
		RET_UNKNOWN,				// Default message for oddity
	};
	UnicodeString getErrorStringFromReturnType( ReturnType ret );

	// On functions are (generally) the result of network traffic
	virtual void OnGameList( LANGameInfo *gameList ) = 0;																							///< List of games
	virtual void OnPlayerList( LANPlayer *playerList ) = 0;																				///< List of players in the Lobby
	virtual void OnGameJoin( ReturnType ret, LANGameInfo *theGame ) = 0;															///< Did we get in the game?
	virtual void OnPlayerJoin( Int slot, UnicodeString playerName ) = 0;													///< Someone else joined our game (host only; joiners get a slotlist)
	virtual void OnHostLeave() = 0;																													///< Host left the game
	virtual void OnPlayerLeave( UnicodeString player ) = 0;																				///< Someone left the game
	virtual void OnAccept( UnsignedInt playerIP, Bool status ) = 0;																///< Someone's accept status changed
	virtual void OnHasMap( UnsignedInt playerIP, Bool status ) = 0;																///< Someone's map status changed
	virtual void OnChat( UnicodeString player, UnsignedInt ip,
											 UnicodeString message, ChatType format ) = 0;														///< Chat message from someone
	virtual void OnGameStart() = 0;																													///< The game is starting
	virtual void OnGameStartTimer( Int seconds ) = 0;
	virtual void OnGameOptions( UnsignedInt playerIP, Int playerSlot, AsciiString options ) = 0;	///< Someone sent game options
	virtual void OnGameCreate( ReturnType ret ) = 0;																							///< Your game is created
//	virtual void OnSlotList( ReturnType ret, LANGameInfo *theGame ) = 0;															///< Slotlist for a game in setup
	virtual void OnNameChange( UnsignedInt IP, UnicodeString newName ) = 0;												///< Someone has morphed

	// Misc utility functions
	virtual LANGameInfo * LookupGame( UnicodeString gameName ) = 0;														///< return a pointer to a game we know about
	virtual LANGameInfo * LookupGameByListOffset( Int offset ) = 0;														///< return a pointer to a game we know about
	virtual LANGameInfo * LookupGameByHost( UnsignedInt hostIP ) = 0;													///< return a pointer to the most recent game associated to the host IP address
	virtual Bool SetLocalIP( UnsignedInt localIP ) = 0;																		///< For multiple NIC machines
	virtual void SetLocalIP( AsciiString localIP ) = 0;																		///< For multiple NIC machines
	virtual Bool AmIHost() = 0;																											///< Am I hosting a game?
	virtual inline UnicodeString GetMyName() = 0;																		///< What's my name?
	virtual inline LANGameInfo *GetMyGame() = 0;															          ///< What's my Game?
	virtual void fillInLANMessage( LANMessage *msg ) = 0;																	///< Fill in default params
	virtual void checkMOTD() = 0;
};


/**
 * LAN message class
 */
#pragma pack(push, 1)
struct LANMessage
{
	enum Type				          ///< What kind of message are we?
	{
		// Locating everybody
		MSG_REQUEST_LOCATIONS,	///< Hey, where is everybody?
		MSG_GAME_ANNOUNCE,			///< Here I am, and here's my game info!
		MSG_LOBBY_ANNOUNCE,			///< Hey, I'm in the lobby!

		// Joining games
		MSG_REQUEST_JOIN,				///< Let me in!  Let me in!
		MSG_JOIN_ACCEPT,				///< Okay, you can join.
		MSG_JOIN_DENY,					///< Go away!  We don't want any!

		// Leaving games
		MSG_REQUEST_GAME_LEAVE,	///< I want to leave the game
		MSG_REQUEST_LOBBY_LEAVE,///< I'm leaving the lobby

		// Game options, chat, etc
		MSG_SET_ACCEPT,					///< I'm cool with everything as is.
		MSG_MAP_AVAILABILITY,		///< I do (not) have the map.
		MSG_CHAT,								///< Just spouting my mouth off.
		MSG_GAME_START,					///< Hold on; we're starting!
		MSG_GAME_START_TIMER,		///< The game will start in N seconds
		MSG_GAME_OPTIONS,				///< Here's some info about the game.
		MSG_INACTIVE,						///< I've alt-tabbed out.  Unaccept me cause I'm a poo-flinging monkey.

		MSG_REQUEST_GAME_INFO,	///< For direct connect, get the game info from a specific IP Address

		///< I gave up trying to reach your observer stream port. Appended last
		///< on purpose: the wire values of everything above stay put, and an
		///< older client that receives this falls into update()'s default case
		///< and ignores it.
		MSG_OBSERVE_UNREACHABLE,
	} messageType;

	WideChar name[g_lanPlayerNameLength+1]; ///< My name, for convenience
	char userName[g_lanLoginNameLength+1];	///< login name, for convenience
	char hostName[g_lanHostNameLength+1];		///< machine name, for convenience

	// No additional data is required for REQUEST_LOCATIONS, LOBBY_ANNOUNCE,
	// REQUEST_LOBBY_LEAVE.
	union
	{
		// StartTimer is sent with GAME_START_TIMER
		struct
		{
			Int seconds;
		} StartTimer;

		// StartGame is sent with GAME_START. The host embeds its final options
		// string so joiners load from the authoritative slot state even if the
		// last MSG_GAME_OPTIONS never arrived or was clobbered locally; a game
		// that starts from divergent options desyncs on the first CRC check.
		struct
		{
			char options[m_lanMaxOptionsLength+1];
		} StartGame;

		// GameJoined is sent with REQUEST_GAME_LEAVE
		struct
		{
			WideChar gameName[g_lanGameNameLength+1];
		} GameToLeave;

		// GameInfo if sent with GAME_ANNOUNCE
		struct
		{
			WideChar gameName[g_lanGameNameLength+1];
			Bool inProgress;
			char options[m_lanMaxOptionsLength+1];
			Bool isDirectConnect;
		} GameInfo;

		// PlayerInfo is sent with REQUEST_GAME_INFO for direct connect games.
		struct
		{
			UnsignedInt ip;
			WideChar playerName[g_lanPlayerNameLength+1];
		} PlayerInfo;

		// GameToJoin is sent with REQUEST_JOIN
		struct
		{
			UnsignedInt gameIP;
			UnsignedInt exeCRC;
			UnsignedInt iniCRC;
			char serial[g_maxSerialLength];
		} GameToJoin;

		// GameJoined is sent with JOIN_ACCEPT
		struct
		{
			WideChar gameName[g_lanGameNameLength+1];
			UnsignedInt gameIP;
			UnsignedInt playerIP;
			Int slotPosition;
		} GameJoined;

		// GameNotJoined is sent with JOIN_DENY
		struct
		{
			WideChar gameName[g_lanGameNameLength+1];
			UnsignedInt gameIP;
			UnsignedInt playerIP;
			LANAPIInterface::ReturnType reason;
			// When reason == RET_GAME_STARTED, the host fills in the TCP
			// port where it is streaming the live replay for observers.
			// 0 means "no observer mode available" (legacy host).
			UnsignedShort observerPort;
		} GameNotJoined;

		// Accept is sent with SET_ACCEPT
		struct
		{
			WideChar gameName[g_lanGameNameLength+1];
			Bool isAccepted;
		} Accept;

		// Accept is sent with MAP_AVAILABILITY
		struct
		{
			WideChar gameName[g_lanGameNameLength+1];
			UnsignedInt mapCRC;	// to make sure we're talking about the same map
			Bool hasMap;
		} MapStatus;

		// Chat is sent with CHAT
		struct
		{
			WideChar gameName[g_lanGameNameLength+1];
			LANAPIInterface::ChatType chatType;
			WideChar message[g_lanMaxChatLength+1];
		} Chat;

		// GameOptions is sent with GAME_OPTIONS
		struct
		{
			char options[m_lanMaxOptionsLength+1];
		} GameOptions;

		// ObserveFailure is sent with MSG_OBSERVE_UNREACHABLE. The would-be
		// observer tells the host that its stream port never answered, which
		// is a fact only the observer holds: the host's listen socket bound
		// fine and simply never saw a connection. Carries the port so the
		// host can name it, and how long we spent trying so the host's log
		// shows this was a real timeout rather than one dropped packet.
		struct
		{
			UnsignedInt   gameIP;
			UnsignedShort observerPort;
			UnsignedInt   attemptMs;
		} ObserveFailure;

	};
};
#pragma pack(pop)

static_assert(sizeof(LANMessage) <= MAX_LANAPI_PACKET_SIZE, "LANMessage struct cannot be larger than the max packet size");


/**
 * The LANAPI class is used to instantiate a singleton which
 * implements the interface to all LAN broadcast communications.
 */
class LANAPI : public LANAPIInterface
{
public:

	LANAPI();
	virtual ~LANAPI() override;

	virtual void init() override;															///< Initialize or re-initialize the instance
	virtual void reset() override;															///< reset the logic system
	virtual void update() override;														///< update the world

	virtual void setIsActive(Bool isActive) override;								///< tell TheLAN whether or not

	// Request functions generate network traffic
	virtual void RequestLocations() override;																				///< Request everybody to respond with where they are
	virtual void RequestGameJoin( LANGameInfo *game, UnsignedInt ip = 0 ) override;				///< Request to join a game
	virtual void RequestGameJoinDirectConnect( UnsignedInt ipaddress ) override;						///< Request to join a game at an IP address

	/// Set the remote UDP port for the next RequestGameJoinDirectConnect target.
	/// 0 (default) means use lobbyPort, which is the only behavior LAN cares
	/// about. The online coordinator sets this to the peer's NAT-translated
	/// port discovered during hole punch, since the peer's LAN code is bound
	/// internally to lobbyPort but visible externally on a different port.
	void setDirectConnectRemotePort( UnsignedShort port ) { m_directConnectRemotePort = port; }
	UnsignedShort getDirectConnectRemotePort() const { return m_directConnectRemotePort; }

	/// Set the peer's NAT-translated game-data port (NETWORK_BASE_PORT_NUMBER
	/// equivalent visible externally). When non-zero and the current game is
	/// direct-connect, slot setup uses this instead of the hardcoded
	/// NETWORK_BASE_PORT_NUMBER so ConnectionManager sends to the punched
	/// mapping rather than to an unrouted port that nobody is listening on.
	/// Used for the single-joiner case (2-player coord). For N-player, see
	/// setDirectConnectGamePortForPeer below which keeps a per-peer map.
	void setDirectConnectRemoteGamePort( UnsignedShort port ) { m_directConnectRemoteGamePort = port; }
	UnsignedShort getDirectConnectRemoteGamePort() const { return m_directConnectRemoteGamePort; }

	/// Record a joiner's punched external game-data port. Keyed by BOTH the
	/// external IP and the peer's external LOBBY port, because two players
	/// behind one NAT share an IP: keying on IP alone let the second joiner
	/// overwrite the first's game port, and the host would then send in-game
	/// traffic for one of them to the other's port. Populated by the lobby UI
	/// as the coordinator delivers peer_info for each new joiner;
	/// handleRequestJoin looks the joiner up here (by the source port of the
	/// join request) before falling back to the single-value setter above.
	void setDirectConnectGamePortForPeer( UnsignedInt ip, UnsignedShort lobbyPort, UnsignedShort gamePort )
	{
		for (size_t i = 0; i < m_directConnectGamePorts.size(); ++i)
		{
			if (m_directConnectGamePorts[i].ip == ip && m_directConnectGamePorts[i].lobbyPort == lobbyPort)
			{
				m_directConnectGamePorts[i].gamePort = gamePort;
				return;
			}
		}
		DirectConnectPeerPorts entry;
		entry.ip        = ip;
		entry.lobbyPort = lobbyPort;
		entry.gamePort  = gamePort;
		m_directConnectGamePorts.push_back(entry);
	}
	UnsignedShort lookupDirectConnectGamePort( UnsignedInt ip, UnsignedShort lobbyPort ) const
	{
		size_t i;
		// Exact (ip, lobby port) match first.
		for (i = 0; i < m_directConnectGamePorts.size(); ++i)
		{
			if (m_directConnectGamePorts[i].ip == ip && m_directConnectGamePorts[i].lobbyPort == lobbyPort)
				return m_directConnectGamePorts[i].gamePort;
		}
		// Fall back to IP-only (peer whose lobby port we never learned).
		for (i = 0; i < m_directConnectGamePorts.size(); ++i)
		{
			if (m_directConnectGamePorts[i].ip == ip)
				return m_directConnectGamePorts[i].gamePort;
		}
		return (UnsignedShort)0;
	}

	/// Send a tiny fill-in-style LOBBY_ANNOUNCE packet directly to (ip:port)
	/// to open the host's NAT mapping for that external addr. Used for
	/// N-player coord: when the coordinator tells the host about a new
	/// joiner, the host fires this so its NAT lets the joiner's subsequent
	/// MSG_REQUEST_GAME_INFO through (for non-cone NATs).
	void sendNATKeepalive( UnsignedInt destIPHost, UnsignedShort destPortHost );
	// TTL-limited NAT-opening probe: creates our outbound lobby-socket
	// mapping without reaching (and poisoning) the peer's NAT. Fire this
	// the moment the coordinator reports a new joiner, then follow up with
	// a full-TTL sendNATKeepalive once the joiner has started punching.
	void sendNATProbeLowTTL( UnsignedInt destIPHost, UnsignedShort destPortHost );

	/// Raw lobby socket, or -1. After the coordinator handoff this socket is
	/// ours but its NAT mapping is still the address the coordinator hands to
	/// joiners, so the coordinator keeps STUNning from it to hold that mapping
	/// open. See OnlineCoordinatorAPI::pumpStunKeepalive.
	Int getLobbyRawFD() const;
	virtual void RequestGameLeave() override;																				///< Tell everyone we're leaving
	virtual void RequestAccept() override;																						///< Indicate we're OK with the game options
	virtual void RequestHasMap() override;																						///< Send our map status
	virtual void RequestChat( UnicodeString message, ChatType format ) override;						///< Send a chat message
	virtual void RequestGameStart() override;																				///< Tell everyone the game is starting
	virtual void RequestGameStartTimer( Int seconds ) override;
	virtual void RequestGameOptions( AsciiString gameOptions, Bool isPublic, UnsignedInt ip = 0 ) override;		///< Change the game options
	virtual void RequestGameCreate( UnicodeString gameName, Bool isDirectConnect ) override;	///< Try to host a game
	virtual void RequestGameAnnounce() override;																			///< Send out game info if host
	virtual void RequestSetName( UnicodeString newName ) override;													///< Pick a new name
//	virtual void RequestSlotList();																					///< Pump out the Slot info.
	virtual void RequestLobbyLeave( Bool forced ) override;																///< Announce that we're leaving the lobby
	virtual void ResetGameStartTimer() override;

	// On functions are (generally) the result of network traffic
	virtual void OnGameList( LANGameInfo *gameList ) override;																							///< List of games
	virtual void OnPlayerList( LANPlayer *playerList ) override;																				///< List of players in the Lobby
	virtual void OnGameJoin( ReturnType ret, LANGameInfo *theGame ) override;															///< Did we get in the game?
	virtual void OnPlayerJoin( Int slot, UnicodeString playerName ) override;													///< Someone else joined our game (host only; joiners get a slotlist)
	virtual void OnHostLeave() override;																													///< Host left the game
	virtual void OnPlayerLeave( UnicodeString player ) override;																				///< Someone left the game
	virtual void OnAccept( UnsignedInt playerIP, Bool status ) override;																///< Someone's accept status changed
	virtual void OnHasMap( UnsignedInt playerIP, Bool status ) override;																///< Someone's map status changed
	virtual void OnChat( UnicodeString player, UnsignedInt ip,
											 UnicodeString message, ChatType format ) override;														///< Chat message from someone
	virtual void OnGameStart() override;																													///< The game is starting
	virtual void OnGameStartTimer( Int seconds ) override;
	virtual void OnGameOptions( UnsignedInt playerIP, Int playerSlot, AsciiString options ) override;	///< Someone sent game options
	virtual void OnGameCreate( ReturnType ret ) override;																							///< Your game is created
	//virtual void OnSlotList( ReturnType ret, LANGameInfo *theGame );															///< Slotlist for a game in setup
	virtual void OnNameChange( UnsignedInt IP, UnicodeString newName ) override;												///< Someone has morphed
	virtual void OnInActive( UnsignedInt IP );																								///< Someone has alt-tabbed out.


	// Misc utility functions
	virtual LANGameInfo * LookupGame( UnicodeString gameName ) override;														///< return a pointer to a game we know about
	virtual LANGameInfo * LookupGameByListOffset( Int offset ) override;														///< return a pointer to a game we know about
	virtual LANGameInfo * LookupGameByHost( UnsignedInt hostIP ) override;													///< return a pointer to the most recent game associated to the host IP address
	virtual LANPlayer * LookupPlayer( UnsignedInt playerIP );													///< return a pointer to a player we know about
	virtual Bool SetLocalIP( UnsignedInt localIP ) override;																		///< For multiple NIC machines
	virtual void SetLocalIP( AsciiString localIP ) override;																		///< For multiple NIC machines
	/// Online handoff: adopt the coordinator's already-punched lobby socket
	/// by fd instead of rebinding the port. Preserves the NAT mapping the
	/// peer was told to talk to (CGNATs give a rebound socket a new one).
	/// Takes ownership of fd.
	Bool SetLocalIPAdoptingSocket( UnsignedInt localIP, Int fd );

	/// Slot index for the peer whose message is currently being dispatched.
	/// Matches (IP, source port) first so two players sharing one public IP
	/// (same household/NAT) are told apart; falls back to IP-only. -1 if none.
	Int findSlotForSender( UnsignedInt senderIP ) const;
	virtual Bool AmIHost() override;																											///< Am I hosting a game?
	virtual UnicodeString GetMyName() override { return m_name; }                 ///< What's my name?
	virtual LANGameInfo* GetMyGame() override { return m_currentGame; }					      ///< What's my Game?
	virtual UnsignedInt GetLocalIP() { return m_localIP; }								///< What's my IP?
	virtual void fillInLANMessage( LANMessage *msg ) override;																	///< Fill in default params
	virtual void checkMOTD() override;
protected:

	enum PendingActionType
	{
		ACT_NONE = 0,
		ACT_JOIN,
		ACT_JOINDIRECTCONNECT,
		ACT_LEAVE,
	};

	static const UnsignedInt s_resendDelta; // in ms

protected:
	LANPlayer *					m_lobbyPlayers;			///< List of players in the lobby
	LANGameInfo *				m_games;								///< List of games
	UnicodeString				m_name;							///< Who do we think we are?
	AsciiString					m_userName;						///< login name
	AsciiString					m_hostName;						///< machine name
	UnsignedInt					m_gameStartTime;
	Int									m_gameStartSeconds;

	PendingActionType		m_pendingAction;	///< What action are we performing?
	UnsignedInt					m_expiration;						///< When should we give up on our action?
	UnsignedInt					m_actionTimeout;
	// Join-request retransmit: MSG_REQUEST_GAME_INFO / MSG_REQUEST_JOIN are
	// single UDP packets; over punched NAT paths either can be lost (e.g.
	// while the host is mid-handoff creating its game), which used to surface
	// as "Connection timed out" on the joiner. While ACT_JOIN or
	// ACT_JOINDIRECTCONNECT is pending, update() re-sends the stashed request
	// once a second until it is answered or m_expiration hits. The host
	// answers re-joins idempotently (see handleRequestJoin).
	LANMessage					m_pendingResendMsg;   ///< Verbatim copy of the last join-flow request
	UnsignedInt					m_pendingResendIP;    ///< Destination passed to sendMessage for it
	UnsignedInt					m_nextResendMs;       ///< When to fire the next retry (0 = disarmed)
	UnsignedInt					m_directConnectRemoteIP;///< The IP address of the game we are direct connecting to.
	UnsignedShort				m_directConnectRemotePort;///< Optional non-default UDP port for direct-connect target. 0 = use lobbyPort. Set by online coordinator before RequestGameJoinDirectConnect.
	UnsignedShort				m_directConnectRemoteGamePort;///< Peer's punched game-data port. Used to override slot.setPort in direct-connect mode so ConnectionManager talks to the NAT-translated port, not NETWORK_BASE_PORT_NUMBER. Single-value version for 2-player coord.
	struct DirectConnectPeerPorts
	{
		UnsignedInt   ip;         ///< peer's external IP
		UnsignedShort lobbyPort;  ///< peer's external lobby port (tells same-IP peers apart)
		UnsignedShort gamePort;   ///< peer's punched external game-data port
	};
	std::vector<DirectConnectPeerPorts> m_directConnectGamePorts; ///< Per-peer punched game-data ports from coordinator peer_info (N-player coord).
	UnsignedInt					m_dispatchSenderIP;  ///< Source IP of the LAN message currently being dispatched (transient).
	UnsignedShort				m_dispatchSenderPort;///< Source port of the LAN message currently being dispatched. Lets reply-style handlers send back through NAT-translated mappings instead of hardcoded lobbyPort.

	// Resend timer ---------------------------------------------------------------------------
	UnsignedInt					m_lastResendTime; // in ms

	Bool								m_isInLANMenu;		///< true while we are in a LAN menu (lobby, game options, direct connect)
	Bool								m_inLobby;											///< Are we in the lobby (not in a game)?
	LANGameInfo *				m_currentGame;							///< Pointer to game (setup screen) we are currently in (null for lobby)
	//LANGameInfo *m_currentGameInfo;			///< Pointer to game setup info we are currently in.

	UnsignedInt					m_localIP;
	Transport*					m_transport;

	UnsignedInt					m_broadcastAddr;

	UnsignedInt					m_lastUpdate;
	AsciiString					m_lastGameopt; /// @todo: hack for demo - remove this

	Bool								m_isActive;			///< is the game currently active?

	// Observer mode. m_observerHost is non-null on the host while a game is in
	// progress; it accepts incoming TCP observer connections and streams the
	// host's growing .rep file. m_observerClient is non-null on a joining
	// player who chose to spectate; it pulls bytes into a local .rep file
	// and signals the recorder when the snapshot is buffered and when the
	// stream eventually closes.
	LANObserverHost*			m_observerHost;
	LANObserverClient*		m_observerClient;
	OnlineCoordinatorAPI*	m_inGameCoord;                  // host: coordinator session adopted at game start (observer relay)
	Bool									m_observerClientPlaybackKicked; // we called playbackFileLiveObserver already
	UnsignedInt						m_observerProgressLastMs;       // last time we posted a download-progress chat line
	UnsignedInt						m_observerProgressLastBytes;    // bytes reported at the last progress post
	// Host: whether we have already told the player that someone could not
	// reach our observer port this game. Several blocked spectators, or one
	// retrying from the menu, would otherwise each post their own line.
	Bool									m_observeUnreachableReported;

	// Non-blocking map download (peer side). When the host advertises a map CRC
	// we don't have, OnGameOptions kicks off a background CDN fetch and
	// updateMapDownload() finishes the job once the bytes are in. Downloading
	// inline would block the same thread that services the LAN heartbeat.
	Bool									m_mapDownloadPending;    // a fetch is in flight
	UnsignedInt						m_mapDownloadCRC;        // CRC being fetched
	UnsignedInt						m_mapDownloadFailedCRC;  // CRC we already failed on; don't retry it every OnGameOptions

public:
	// Observer mode entry points (called from UI / callbacks).

	// Joining player accepted the "Watch as observer" prompt. Opens TCP to
	// the host and starts buffering bytes into a scratch .rep file.
	void RequestObserve(UnsignedInt hostIP, UnsignedShort observerPort);

	// Online (coordinator) variant: the stream socket was spliced through
	// the coordinator relay and is already connected; adopt it instead of
	// dialing the host directly (which NAT would block).
	void RequestObserveAdoptedFd(Int fd);

	// Host accessors used by chat notifications etc.
	Int   getObserverCount() const { return m_observerHost ? m_observerHost->observerCount() : 0; }
	Bool  isObservingClient() const { return m_observerClient != nullptr; }

	// Online-coordinator session carried into the game. The host adopts the
	// lobby's OnlineCoordinatorAPI at game start (instead of tearing it
	// down) so viewers can request to observe the in-progress game; the
	// updateObserver pump keeps it alive and services observer_request
	// tokens by attaching relay connections to m_observerHost. Ownership
	// transfers here; reset() deletes it.
	void adoptCoordinator(OnlineCoordinatorAPI* coord);

public:
	// Observer state-machine pump. Normally invoked from update() at the LAN
	// lobby's tick cadence, but also callable from the game loop while a
	// LIVE_OBSERVER playback is running, since LANAPI::update() is only
	// driven by the LAN lobby menu and stops once playback starts.
	void updateObserver();

	// Finish a peer-side background map download started in OnGameOptions:
	// installs the map (on the main thread), flips our slot's map-availability
	// bit and refreshes the lobby. Reports both outcomes as SYSTEM chat lines.
	// No-op when nothing is in flight. Called from update().
	void updateMapDownload();

protected:
	// Host-side hook: open the observer listen socket and arm streaming
	// against the recorder's current file. Safe to call multiple times.
	void startObserverHost();
	void stopObserverHost();
	// Client-side teardown.
	void stopObserverClient();
	// Ensure the map referenced by the buffered live-observer snapshot is
	// available locally (downloading it from the cncstats CDN if missing)
	// before playback starts. Returns FALSE only when the map is genuinely
	// missing and could not be obtained.
	Bool ensureObserverMapAvailable(AsciiString relReplayPath);

protected:
	void sendMessage(LANMessage *msg, UnsignedInt ip = 0); // Convenience function
	void removePlayer(LANPlayer *player);
	void removeGame(LANGameInfo *game);
	void addPlayer(LANPlayer *player);
	void addGame(LANGameInfo *game);
	AsciiString createSlotString();

	// Functions to handle incoming messages -----------------------------------
	void handleRequestLocations( LANMessage *msg, UnsignedInt senderIP );
	void handleGameAnnounce( LANMessage *msg, UnsignedInt senderIP );
	void handleLobbyAnnounce( LANMessage *msg, UnsignedInt senderIP );
	void handleRequestGameInfo( LANMessage *msg, UnsignedInt senderIP );
	void handleRequestJoin( LANMessage *msg, UnsignedInt senderIP );
	void handleJoinAccept( LANMessage *msg, UnsignedInt senderIP );
	void handleJoinDeny( LANMessage *msg, UnsignedInt senderIP );
	void handleRequestGameLeave( LANMessage *msg, UnsignedInt senderIP );
	void handleRequestLobbyLeave( LANMessage *msg, UnsignedInt senderIP );
	void handleSetAccept( LANMessage *msg, UnsignedInt senderIP );
	void handleHasMap( LANMessage *msg, UnsignedInt senderIP );
	void handleChat( LANMessage *msg, UnsignedInt senderIP );
	void handleGameStart( LANMessage *msg, UnsignedInt senderIP );
	void handleGameStartTimer( LANMessage *msg, UnsignedInt senderIP );
	void handleGameOptions( LANMessage *msg, UnsignedInt senderIP );
	void handleInActive( LANMessage *msg, UnsignedInt senderIP );
	void handleObserveUnreachable( LANMessage *msg, UnsignedInt senderIP );

};
