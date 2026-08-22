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

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#define WIN32_LEAN_AND_MEAN  // only bare bones windows stuff wanted

#include "Common/crc.h"
#include "Common/GameState.h"
#include "Common/Registry.h"
#include "Common/ReleaseLog.h"
#include "GameNetwork/LANAPI.h"
#include "GameNetwork/OnlineCoordinatorAPI.h"
#include "GameNetwork/networkutil.h"
#include "Common/GlobalData.h"
#include "Common/RandomValue.h"
#include "GameClient/GameText.h"
#include "GameClient/MapUtil.h"
#include "GameClient/MessageBox.h"
#include "Common/UserPreferences.h"
#include "GameLogic/GameLogic.h"


static const UnsignedShort lobbyPort = 8086; ///< This is the UDP port used by all LANAPI communication

AsciiString GetMessageTypeString(UnsignedInt type);

const UnsignedInt LANAPI::s_resendDelta = 10 * 1000;	///< This is how often we announce ourselves to the world
/*
LANGame::LANGame()
{
	m_gameName = L"";

	int player;
	for (player = 0; player < MAX_SLOTS; ++player)
	{
		m_playerName[player] = L"";
		m_playerIP[player]= 0;
		m_playerAccepted[player] = false;
	}
	m_lastHeard = 0;
	m_inProgress = false;
	m_next = nullptr;
}
*/




LANAPI::LANAPI() : m_transport(nullptr)
{
	DEBUG_LOG(("LANAPI::LANAPI() - max game option size is %d, sizeof(LANMessage)=%d, MAX_LANAPI_PACKET_SIZE=%d",
		m_lanMaxOptionsLength, sizeof(LANMessage), MAX_LANAPI_PACKET_SIZE));

	m_lastResendTime = 0;
	//
	m_lobbyPlayers = nullptr;
	m_games = nullptr;
	m_name = L""; // safe default?
	m_pendingAction = ACT_NONE;
	m_expiration = 0;
	m_localIP = 0;
	m_inLobby = true;
	m_isInLANMenu = TRUE;
	m_currentGame = nullptr;
	m_broadcastAddr = INADDR_BROADCAST;
	m_directConnectRemoteIP = 0;
	m_directConnectRemoteGamePort = 0;
	m_actionTimeout = 5000; // ms
	m_pendingResendIP = 0;
	m_nextResendMs = 0;
	m_lastUpdate = 0;
	m_transport = new Transport;
	m_isActive = TRUE;
	m_observerHost = nullptr;
	m_observerClient = nullptr;
	m_inGameCoord = nullptr;
	m_observerClientPlaybackKicked = FALSE;
	m_observerProgressLastMs = 0;
	m_observerProgressLastBytes = 0;
	m_observeUnreachableReported = FALSE;
	m_mapDownloadPending = FALSE;
	m_mapDownloadCRC = 0;
	m_mapDownloadFailedCRC = 0;
}

LANAPI::~LANAPI()
{
	reset();
	stopObserverHost();
	stopObserverClient();
	delete m_transport;
}

void LANAPI::init()
{
	m_gameStartTime = 0;
	m_gameStartSeconds = 0;
	m_transport->reset();
	m_transport->init(m_localIP, lobbyPort);
	m_transport->allowBroadcasts(true);

	m_pendingAction = ACT_NONE;
	m_expiration = 0;
	m_inLobby = true;
	m_isInLANMenu = TRUE;
	m_currentGame = nullptr;
	m_directConnectRemoteIP = 0;
	m_directConnectRemotePort = 0;
	m_directConnectRemoteGamePort = 0;
	m_dispatchSenderIP = 0;
	m_dispatchSenderPort = 0;

	// A download from a previous session is no longer ours to finish, and a CRC
	// that failed there deserves a fresh try in this one.
	m_mapDownloadPending = FALSE;
	m_mapDownloadCRC = 0;
	m_mapDownloadFailedCRC = 0;

	m_lastGameopt = "";

#if TELL_COMPUTER_IDENTITY_IN_LAN_LOBBY
	char userName[UNLEN + 1];
	DWORD bufSize = ARRAY_SIZE(userName);
	if (GetUserNameA(userName, &bufSize))
	{
		m_userName.set(userName, bufSize - 1);
	}
	else
	{
		m_userName = "unknown";
	}

	char computerName[MAX_COMPUTERNAME_LENGTH + 1];
	bufSize = ARRAY_SIZE(computerName);
	if (GetComputerNameA(computerName, &bufSize))
	{
		m_hostName.set(computerName, bufSize - 1);
	}
	else
	{
		m_hostName = "unknown";
	}
#endif
}

void LANAPI::reset()
{
	if (m_inLobby)
	{
		LANMessage msg;
		fillInLANMessage( &msg );
		msg.messageType = LANMessage::MSG_REQUEST_LOBBY_LEAVE;
		sendMessage(&msg);
	}
	m_transport->update();

	LANGameInfo *theGame = m_games;
	LANGameInfo *deletableGame = nullptr;

	while (theGame)
	{
		deletableGame = theGame;
		theGame = theGame->getNext();
		delete deletableGame;
	}

	LANPlayer *thePlayer = m_lobbyPlayers;
	LANPlayer *deletablePlayer = nullptr;

	while (thePlayer)
	{
		deletablePlayer = thePlayer;
		thePlayer = thePlayer->getNext();
		delete deletablePlayer;
	}

	m_games = nullptr;
	m_lobbyPlayers = nullptr;
	m_directConnectRemoteIP = 0;
	m_directConnectRemotePort = 0;
	m_directConnectRemoteGamePort = 0;
	m_dispatchSenderIP = 0;
	m_dispatchSenderPort = 0;
	m_pendingAction = ACT_NONE;
	m_expiration = 0;
	m_inLobby = true;
	m_isInLANMenu = TRUE;
	m_currentGame = nullptr;

	// Tear down any active observer session so a fresh LAN cycle doesn't
	// inherit a stale listen socket or live client connection.
	stopObserverHost();
	stopObserverClient();

	// Drop the in-game coordinator session (observer relay) carried over
	// from the previous match, if any.
	if (m_inGameCoord)
	{
		ReleaseLog("Coordinator teardown: LANAPI::reset (in-game session)");
		m_inGameCoord->disconnect();
		delete m_inGameCoord;
		m_inGameCoord = nullptr;
	}
}

void LANAPI::adoptCoordinator(OnlineCoordinatorAPI* coord)
{
	if (m_inGameCoord && m_inGameCoord != coord)
	{
		m_inGameCoord->disconnect();
		delete m_inGameCoord;
	}
	m_inGameCoord = coord;
	if (coord)
	{
		// Tell the coordinator our listed game is now in progress so the
		// games list can advertise "observe" instead of "join".
		coord->sendGameStarted();
		ReleaseLog("Coordinator adopted for in-game observer relay");
	}
}

Int LANAPI::findSlotForSender(UnsignedInt senderIP) const
{
	if (m_currentGame == nullptr)
		return -1;

	// Players behind one NAT share a public IP and differ only by port, so
	// prefer an exact (IP, source port) match against the per-slot lobby
	// port the host recorded when that player first spoke. Falls through to
	// the historical IP-only match for LAN games (where lobby ports are all
	// the same) and for peers whose port we have not learned yet.
	if (m_dispatchSenderPort != 0)
	{
		for (Int i = 0; i < MAX_SLOTS; ++i)
		{
			const LANGameSlot *slot = m_currentGame->getConstLANSlot(i);
			if (slot != nullptr && slot->isHuman() && slot->getIP() == senderIP
				&& slot->getLobbyPort() == m_dispatchSenderPort)
			{
				return i;
			}
		}
	}
	for (Int i = 0; i < MAX_SLOTS; ++i)
	{
		const LANGameSlot *slot = m_currentGame->getConstLANSlot(i);
		if (slot != nullptr && slot->isHuman() && slot->getIP() == senderIP)
			return i;
	}
	return -1;
}

void LANAPI::sendMessage(LANMessage *msg, UnsignedInt ip /* = 0 */)
{
	if (ip != 0)
	{
		// Joiner side of a direct-connect game: rewrite the host's LAN-local
		// IP (as recorded in slot 0 from the options string) to the punched
		// coordinator mapping; the local IP is unroutable across NATs.
		if (m_currentGame != nullptr && m_currentGame->getIsDirectConnect() &&
			!AmIHost() && m_directConnectRemoteIP != 0 && ip != m_directConnectRemoteIP)
		{
			LANGameSlot *hostSlot = m_currentGame->getLANSlot(0);
			if (hostSlot != nullptr && ip == hostSlot->getIP())
			{
				ip = m_directConnectRemoteIP;
			}
		}
		// Choose the destination port in priority order:
		//   1) source port of the LAN message currently being dispatched. This
		//      is the exact endpoint the request came from, so it is right even
		//      when several players share one public IP (two people behind the
		//      same router/NAT). It MUST outrank the slot scan below, which
		//      matches on IP alone and would otherwise address the reply to
		//      whichever of them occupies the earlier slot -- the second
		//      joiner then never sees the answer and times out.
		//   2) per-peer lobby port stored on the matching game slot (used for
		//      sends we originate rather than reply to; the host learns this
		//      from each joiner's source port when running behind NAT).
		//   3) m_directConnectRemotePort, set by the coordinator on the joiner
		//      side before RequestGameJoinDirectConnect so the joiner's first
		//      sends to the host go to the punched mapping, not lobbyPort.
		//   4) lobbyPort (LAN default, unchanged behavior).
		UnsignedShort port = lobbyPort;
		if (m_dispatchSenderPort != 0 && ip == m_dispatchSenderIP)
		{
			port = m_dispatchSenderPort;
		}
		if (port == lobbyPort && m_currentGame != nullptr)
		{
			for (Int i = 0; i < MAX_SLOTS; ++i)
			{
				LANGameSlot *slot = m_currentGame->getLANSlot(i);
				if (slot != nullptr && slot->isHuman() && slot->getIP() == ip && slot->getLobbyPort() != 0)
				{
					port = slot->getLobbyPort();
					break;
				}
			}
		}
		if (port == lobbyPort && ip == m_directConnectRemoteIP && m_directConnectRemotePort != 0)
		{
			port = m_directConnectRemotePort;
		}
		m_transport->queueSend(ip, port, (unsigned char *)msg, sizeof(LANMessage) /*, 0, 0 */);
	}
	else if ((m_currentGame != nullptr) && (m_currentGame->getIsDirectConnect()))
	{
		Int localSlot = m_currentGame->getLocalSlotNum();
		for (Int i = 0; i < MAX_SLOTS; ++i)
		{
			if (i != localSlot) {
				LANGameSlot *slot = m_currentGame->getLANSlot(i);
				if ((slot != nullptr) && (slot->isHuman())) {
					UnsignedInt   destIP = slot->getIP();
					UnsignedShort port = slot->getLobbyPort() != 0 ? slot->getLobbyPort() : lobbyPort;
					// Joiner side: the host's slot (0) carries the host's
					// LAN-local IP from the options string, which is
					// unroutable from behind another NAT. Send host-bound
					// traffic to the punched coordinator mapping instead --
					// without this the joiner's periodic HELLO never reaches
					// the host and it drops us ~20s after joining.
					if (i == 0 && !AmIHost() && m_directConnectRemoteIP != 0)
					{
						destIP = m_directConnectRemoteIP;
						if (m_directConnectRemotePort != 0)
							port = m_directConnectRemotePort;
					}
					// Diagnostic breadcrumb for the NAT keepalive path: game
					// options (HELLO et al) are the host's liveness signal, so
					// record exactly where each one is aimed. Throttled.
					if (msg->messageType == LANMessage::MSG_GAME_OPTIONS)
					{
						static UnsignedInt s_lastHelloLogMs = 0;
						UnsignedInt nowLog = timeGetTime();
						if (nowLog - s_lastHelloLogMs > 5000)
						{
							s_lastHelloLogMs = nowLog;
							ReleaseLog("LAN dc-send GAMEOPT slot=%d dest=%d.%d.%d.%d:%u (slotIP=%d.%d.%d.%d lobbyPort=%u) remote=%d.%d.%d.%d:%u localSlot=%d",
								i, PRINTF_IP_AS_4_INTS(destIP), port,
								PRINTF_IP_AS_4_INTS(slot->getIP()), slot->getLobbyPort(),
								PRINTF_IP_AS_4_INTS(m_directConnectRemoteIP), m_directConnectRemotePort,
								localSlot);
						}
					}
					m_transport->queueSend(destIP, port, (unsigned char *)msg, sizeof(LANMessage) /*, 0, 0 */);
				}
			}
		}
	}
	else
	{
		// A coordinator (direct-connect) session should never fall through to
		// LAN broadcast: it means the game object lost its direct-connect
		// flag and unicast keepalives are now going nowhere routable.
		if (m_directConnectRemoteIP != 0)
		{
			static UnsignedInt s_lastBcastLogMs = 0;
			UnsignedInt nowLog = timeGetTime();
			if (nowLog - s_lastBcastLogMs > 5000)
			{
				s_lastBcastLogMs = nowLog;
				ReleaseLog("LAN dc BROADCAST-FALLBACK type=%d game=%p directConnect=%d remote=%d.%d.%d.%d:%u",
					msg->messageType, (void*)m_currentGame,
					m_currentGame ? (Int)m_currentGame->getIsDirectConnect() : -1,
					PRINTF_IP_AS_4_INTS(m_directConnectRemoteIP), m_directConnectRemotePort);
			}
		}
		m_transport->queueSend(m_broadcastAddr, lobbyPort, (unsigned char *)msg, sizeof(LANMessage) /*, 0, 0 */);
	}
}


AsciiString GetMessageTypeString(UnsignedInt type)
{
	AsciiString returnString;

	switch (type)
	{
		case LANMessage::MSG_REQUEST_LOCATIONS:
			returnString.format( "Request Locations (%d)",type);
			break;
		case LANMessage::MSG_GAME_ANNOUNCE:
			returnString.format("Game Announce (%d)",type);
			break;
		case LANMessage::MSG_LOBBY_ANNOUNCE:
			returnString.format("Lobby Announce (%d)",type);
			break;
		case LANMessage::MSG_REQUEST_JOIN:
			returnString.format("Request Join (%d)",type);
			break;
		case LANMessage::MSG_JOIN_ACCEPT:
			returnString.format("Join Accept (%d)",type);
			break;
		case LANMessage::MSG_JOIN_DENY:
			returnString.format("Join Deny (%d)",type);
			break;
		case LANMessage::MSG_REQUEST_GAME_LEAVE:
			returnString.format("Request Game Leave (%d)",type);
			break;
		case LANMessage::MSG_REQUEST_LOBBY_LEAVE:
			returnString.format("Request Lobby Leave (%d)",type);
			break;
		case LANMessage::MSG_SET_ACCEPT:
			returnString.format("Set Accept(%d)",type);
			break;
		case LANMessage::MSG_CHAT:
			returnString.format("Chat (%d)",type);
			break;
		case LANMessage::MSG_GAME_START:
			returnString.format("Game Start (%d)",type);
			break;
		case LANMessage::MSG_GAME_START_TIMER:
			returnString.format("Game Start Timer (%d)",type);
			break;
		case LANMessage::MSG_GAME_OPTIONS:
			returnString.format("Game Options (%d)",type);
			break;
		case LANMessage::MSG_REQUEST_GAME_INFO:
			returnString.format("Request GameInfo (%d)", type);
			break;
		case LANMessage::MSG_INACTIVE:
			returnString.format("Inactive (%d)", type);
			break;
		case LANMessage::MSG_OBSERVE_UNREACHABLE:
			returnString.format("Observe Unreachable (%d)", type);
			break;
		default:
			returnString.format("Unknown Message (%d)",type);
	}
	return returnString;
}


void LANAPI::checkMOTD()
{
#if defined(RTS_DEBUG)
	if (TheGlobalData->m_useLocalMOTD)
	{
		// for a playtest, let's log some play statistics, eh?
		if (TheGlobalData->m_playStats <= 0)
			TheWritableGlobalData->m_playStats = 30;

		static UnsignedInt oldMOTDCRC = 0;
		UnsignedInt newMOTDCRC = 0;
		AsciiString asciiMOTD;
		char buf[4096];
		FILE *fp = fopen(TheGlobalData->m_MOTDPath.str(), "r");
		Int len;
		if (fp)
		{
			while( (len = fread(buf, 1, 4096, fp)) > 0 )
			{
				buf[len] = 0;
				asciiMOTD.concat(buf);
			}
			fclose(fp);
			CRC crcObj;
			crcObj.computeCRC(asciiMOTD.str(), asciiMOTD.getLength());
			newMOTDCRC = crcObj.get();
		}

		if (oldMOTDCRC != newMOTDCRC)
		{
			// different MOTD... display it
			oldMOTDCRC = newMOTDCRC;
			AsciiString line;
			while (asciiMOTD.nextToken(&line, "\n"))
			{
				if (line.getCharAt(line.getLength()-1) == '\r')
					line.removeLastChar();	// there is a trailing '\r'

				if (line.isEmpty())
				{
					line = " ";
				}

				UnicodeString uniLine;
				uniLine.translate(line);
				OnChat( L"MOTD", 0, uniLine, LANCHAT_SYSTEM );
			}
		}
	}
#endif
}

extern Bool LANbuttonPushed;
extern Bool LANSocketErrorDetected;

// Defined in LANAPICallbacks.cpp; the observe prompt in OnGameJoin reads these
// to know which host/port to connect to. RequestGameJoin sets them directly for
// the robust (no-UDP-deny-needed) observe path below.
extern UnsignedInt   s_pendingObserveHostIP;
extern UnsignedShort s_pendingObservePort;

void LANAPI::update()
{
	if(LANbuttonPushed)
		return;
	static const UnsignedInt LANAPIUpdateDelay = 200;
	UnsignedInt now = timeGetTime();

	if( now > m_lastUpdate + LANAPIUpdateDelay)
	{
		m_lastUpdate = now;
	}
	else
	{
		return;
	}

	// Tick the coordinator's lobby-phase keepalive sender so the punched
	// NAT mapping on the game UDP port stays alive through the lobby. No-op
	// when no socket is stashed (most LAN sessions).
	OnlineCoordinatorAPI::pumpStashedKeepalive();

	// Let the UDP socket breathe
	if ((m_transport->update() == FALSE) && (LANSocketErrorDetected == FALSE)) {
		if (m_isInLANMenu == TRUE) {
			LANSocketErrorDetected = TRUE;
		}
	}

	// Handle any new messages
	int i;
	for (i=0; i<MAX_MESSAGES && !LANbuttonPushed; ++i)
	{
		if (m_transport->m_inBuffer[i].length > 0)
		{
			// Process the new message
			UnsignedInt senderIP = m_transport->m_inBuffer[i].addr;
			if (senderIP == m_localIP)
			{
				m_transport->m_inBuffer[i].length = 0;
				continue;
			}

			// Track the source port so reply-path sendMessage calls can
			// route back through the same NAT mapping (matters for the
			// online-coordinator flow; LAN packets always arrive on
			// lobbyPort and behave identically).
			m_dispatchSenderIP   = senderIP;
			m_dispatchSenderPort = m_transport->m_inBuffer[i].port;

			LANMessage *msg = (LANMessage *)(m_transport->m_inBuffer[i].data);
			// Direct-connect diagnostic: log the join-flow control messages
			// with their true source endpoint. GAME_OPTIONS is excluded (it
			// is the 10s keepalive and would drown the log).
			if (m_directConnectRemoteIP != 0 && msg->messageType != LANMessage::MSG_GAME_OPTIONS)
			{
				ReleaseLog("LAN dc RECV type=%d from %d.%d.%d.%d:%u",
					(Int)msg->messageType, PRINTF_IP_AS_4_INTS(senderIP), m_dispatchSenderPort);
			}
			//DEBUG_LOG(("LAN message type %s from %ls (%s@%s)", GetMessageTypeString(msg->messageType).str(),
			//	msg->name, msg->userName, msg->hostName));
			switch (msg->messageType)
			{
				// Location specification
			case LANMessage::MSG_REQUEST_LOCATIONS:		// Hey, where is everybody?
				DEBUG_LOG(("LANAPI::update - got a MSG_REQUEST_LOCATIONS from %d.%d.%d.%d", PRINTF_IP_AS_4_INTS(senderIP)));
				handleRequestLocations( msg, senderIP );
				break;
			case LANMessage::MSG_GAME_ANNOUNCE:				// Here someone is, and here's his game info!
				DEBUG_LOG(("LANAPI::update - got a MSG_GAME_ANNOUNCE from %d.%d.%d.%d", PRINTF_IP_AS_4_INTS(senderIP)));
				handleGameAnnounce( msg, senderIP );
				break;
			case LANMessage::MSG_LOBBY_ANNOUNCE:			// Hey, I'm in the lobby!
				DEBUG_LOG(("LANAPI::update - got a MSG_LOBBY_ANNOUNCE from %d.%d.%d.%d", PRINTF_IP_AS_4_INTS(senderIP)));
				handleLobbyAnnounce( msg, senderIP );
				break;
			case LANMessage::MSG_REQUEST_GAME_INFO:
				DEBUG_LOG(("LANAPI::update - got a MSG_REQUEST_GAME_INFO from %d.%d.%d.%d", PRINTF_IP_AS_4_INTS(senderIP)));
				handleRequestGameInfo( msg, senderIP );
				break;

				// Joining games
			case LANMessage::MSG_REQUEST_JOIN:				// Let me in!  Let me in!
				DEBUG_LOG(("LANAPI::update - got a MSG_REQUEST_JOIN from %d.%d.%d.%d", PRINTF_IP_AS_4_INTS(senderIP)));
				handleRequestJoin( msg, senderIP );
				break;
			case LANMessage::MSG_JOIN_ACCEPT:					// Okay, you can join.
				DEBUG_LOG(("LANAPI::update - got a MSG_JOIN_ACCEPT from %d.%d.%d.%d", PRINTF_IP_AS_4_INTS(senderIP)));
				handleJoinAccept( msg, senderIP );
				break;
			case LANMessage::MSG_JOIN_DENY:						// Go away!  We don't want any!
				DEBUG_LOG(("LANAPI::update - got a MSG_JOIN_DENY from %d.%d.%d.%d", PRINTF_IP_AS_4_INTS(senderIP)));
				handleJoinDeny( msg, senderIP );
				break;

				// Leaving games, lobby
			case LANMessage::MSG_REQUEST_GAME_LEAVE:				// I'm outa here!
				DEBUG_LOG(("LANAPI::update - got a MSG_REQUEST_GAME_LEAVE from %d.%d.%d.%d", PRINTF_IP_AS_4_INTS(senderIP)));
				handleRequestGameLeave( msg, senderIP );
				break;
			case LANMessage::MSG_REQUEST_LOBBY_LEAVE:				// I'm outa here!
				DEBUG_LOG(("LANAPI::update - got a MSG_REQUEST_LOBBY_LEAVE from %d.%d.%d.%d", PRINTF_IP_AS_4_INTS(senderIP)));
				handleRequestLobbyLeave( msg, senderIP );
				break;

				// Game options, chat, etc
			case LANMessage::MSG_SET_ACCEPT:					// I'm cool with everything as is.
				handleSetAccept( msg, senderIP );
				break;
			case LANMessage::MSG_MAP_AVAILABILITY:		// Map status
				handleHasMap( msg, senderIP );
				break;
			case LANMessage::MSG_CHAT:								// Just spouting my mouth off.
				handleChat( msg, senderIP );
				break;
			case LANMessage::MSG_GAME_START:					// Hold on; we're starting!
				handleGameStart( msg, senderIP );
				break;
			case LANMessage::MSG_GAME_START_TIMER:
				handleGameStartTimer( msg, senderIP );
				break;
			case LANMessage::MSG_GAME_OPTIONS:				// Here's some info about the game.
				DEBUG_LOG(("LANAPI::update - got a MSG_GAME_OPTIONS from %d.%d.%d.%d", PRINTF_IP_AS_4_INTS(senderIP)));
				handleGameOptions( msg, senderIP );
				break;
			case LANMessage::MSG_INACTIVE:		// someone is telling us that we're inactive.
				handleInActive( msg, senderIP );
				break;
			case LANMessage::MSG_OBSERVE_UNREACHABLE:	// somebody couldn't reach our observer port.
				DEBUG_LOG(("LANAPI::update - got a MSG_OBSERVE_UNREACHABLE from %d.%d.%d.%d", PRINTF_IP_AS_4_INTS(senderIP)));
				handleObserveUnreachable( msg, senderIP );
				break;

			default:
				DEBUG_LOG(("Unknown LAN message type %d", msg->messageType));
			}

			// Mark it as read
			m_transport->m_inBuffer[i].length = 0;

			// Clear the transient dispatch context so any sends queued from
			// outside the dispatch loop (timers, user input) don't pick up
			// a stale per-message port.
			m_dispatchSenderIP   = 0;
			m_dispatchSenderPort = 0;
		}
	}
	if(LANbuttonPushed)
		return;

	// Pump observer host accepts/streams and observer client recv/state.
	updateObserver();
	// Finish any in-flight background map download (see OnGameOptions).
	updateMapDownload();
	// Send out periodic I'm Here messages
	if (now > s_resendDelta + m_lastResendTime)
	{
		m_lastResendTime = now;

		if (m_inLobby)
		{
			RequestSetName(m_name);
		}
		else if (m_currentGame && !m_currentGame->isGameInProgress())
		{
			if (AmIHost())
			{
				RequestGameOptions( GenerateGameOptionsString(), true );
				RequestGameAnnounce();
			}
			else
			{
#if TELL_COMPUTER_IDENTITY_IN_LAN_LOBBY
				AsciiString text;
				text.format("User=%s", m_userName.str());
				RequestGameOptions( text, true );
				text.format("Host=%s", m_hostName.str());
				RequestGameOptions( text, true );
#endif
				RequestGameOptions( "HELLO", false );

				// Direct-connect lobbies ride single UDP datagrams with no
				// acks: if our one MSG_MAP_AVAILABILITY was lost, the host
				// keeps its default hasMap=TRUE for us and will start a game
				// we cannot load. Repeat the report on the HELLO cadence; the
				// host ignores repeats that don't change anything.
				if (m_currentGame->getIsDirectConnect())
					RequestHasMap();
			}
		}
		else if (m_currentGame)
		{
			// game is in progress - RequestGameAnnounce will check if we should send it
			RequestGameAnnounce();
		}
	}

	Bool playerListChanged = false;
	Bool gameListChanged = false;

	// Weed out people we haven't heard from in a while
	LANPlayer *player = m_lobbyPlayers;
	while (player)
	{
		if (player->getLastHeard() + s_resendDelta*2 < now)
		{
			// He's gone!
			removePlayer(player);
			LANPlayer *nextPlayer = player->getNext();
			delete player;
			player = nextPlayer;
			playerListChanged = true;
		}
		else
		{
			player = player->getNext();
		}
	}

	// Weed out people we haven't heard from in a while
	LANGameInfo *game = m_games;
	while (game)
	{
		if (game != m_currentGame && game->getLastHeard() + s_resendDelta*2 < now)
		{
			// He's gone!
			removeGame(game);
			LANGameInfo *nextGame = game->getNext();
			delete game;
			game = nextGame;
			gameListChanged = true;
		}
		else
		{
			game = game->getNext();
		}
	}
	if ( m_currentGame && !m_currentGame->isGameInProgress() )
	{
		if ( !AmIHost() && (m_currentGame->getLastHeard() + s_resendDelta*16 < now) )
		{
			// We haven't heard from the host in a while.  Bail.
			// Actually, fake a host leaving message. :)
			LANMessage msg;
			fillInLANMessage( &msg );
			msg.messageType = LANMessage::MSG_REQUEST_GAME_LEAVE;
			wcslcpy(msg.name, m_currentGame->getPlayerName(0).str(), ARRAY_SIZE(msg.name));
			handleRequestGameLeave(&msg, m_currentGame->getIP(0));
			UnicodeString text;
			text = TheGameText->fetch("LAN:HostNotResponding");
			OnChat(UnicodeString::TheEmptyString, m_localIP, text, LANCHAT_SYSTEM);
		}
		else if ( AmIHost() )
		{
			// Check each player for timeouts
			for (int p=1; p<MAX_SLOTS; ++p)
			{
				if (m_currentGame->getIP(p) && m_currentGame->getPlayerLastHeard(p) + s_resendDelta*8 < now)
				{
					ReleaseLog("LAN host dropping slot %d ip=%d.%d.%d.%d lastHeard=%u now=%u delta=%u",
						p, PRINTF_IP_AS_4_INTS(m_currentGame->getIP(p)),
						m_currentGame->getPlayerLastHeard(p), now,
						now - m_currentGame->getPlayerLastHeard(p));
					LANMessage msg;
					fillInLANMessage( &msg );
					UnicodeString theStr;
					theStr.format(TheGameText->fetch("LAN:PlayerDropped"), m_currentGame->getPlayerName(p).str());
					msg.messageType = LANMessage::MSG_REQUEST_GAME_LEAVE;
					wcslcpy(msg.name, m_currentGame->getPlayerName(p).str(), ARRAY_SIZE(msg.name));
					handleRequestGameLeave(&msg, m_currentGame->getIP(p));
					OnChat(UnicodeString::TheEmptyString, m_localIP, theStr, LANCHAT_SYSTEM);
				}
			}
		}
	}

	if (playerListChanged)
	{
		OnPlayerList(m_lobbyPlayers);
	}

	if (gameListChanged)
	{
		OnGameList(m_games);
	}

	// Retransmit an unanswered join-flow request. A single UDP packet over a
	// punched NAT path is easily lost (worst case: the host is mid-handoff,
	// tearing down its coordinator lobby, when our MSG_REQUEST_JOIN lands);
	// without a retry that one loss shows up as "Connection timed out".
	if ((m_pendingAction == ACT_JOIN || m_pendingAction == ACT_JOINDIRECTCONNECT)
		&& m_nextResendMs != 0 && now >= m_nextResendMs)
	{
		DEBUG_LOG(("LANAPI::update - retrying join request (action %d)", (Int)m_pendingAction));
		sendMessage(&m_pendingResendMsg, m_pendingResendIP);
		m_nextResendMs = now + 1000;
	}

	// Time out old actions
	if (m_pendingAction != ACT_NONE && now > m_expiration)
	{
		switch (m_pendingAction)
		{
		case ACT_JOIN:
			OnGameJoin(RET_TIMEOUT, nullptr);
			m_pendingAction = ACT_NONE;
			m_currentGame = nullptr;
			m_inLobby = true;
			break;
		case ACT_LEAVE:
			OnPlayerLeave(m_name);
			m_pendingAction = ACT_NONE;
			m_currentGame = nullptr;
			m_inLobby = true;
			break;
		case ACT_JOINDIRECTCONNECT:
			OnGameJoin(RET_TIMEOUT, nullptr);
			m_pendingAction = ACT_NONE;
			m_currentGame = nullptr;
			m_inLobby = true;
			break;
		default:
			m_pendingAction = ACT_NONE;
		}
	}

	// send out "game starting" messages
	if ( m_gameStartTime && m_gameStartSeconds && m_gameStartTime <= now )
	{
		// m_gameStartTime is when the next message goes out
		// m_gameStartSeconds is how many seconds remain in the message

		RequestGameStartTimer( m_gameStartSeconds );
	}
	else if (m_gameStartTime && m_gameStartTime <= now)
	{
//		DEBUG_LOG(("m_gameStartTime=%d, now=%d, m_gameStartSeconds=%d", m_gameStartTime, now, m_gameStartSeconds));
		ResetGameStartTimer();
		RequestGameStart();
	}

	// Check for an MOTD every few seconds
	static UnsignedInt lastMOTDCheck = 0;
	static const UnsignedInt motdInterval = 30000;
	if (now > lastMOTDCheck + motdInterval)
	{
		checkMOTD();
		lastMOTDCheck = now;
	}
}

// Request functions generate network traffic
void LANAPI::RequestLocations()
{
	LANMessage msg;
	msg.messageType = LANMessage::MSG_REQUEST_LOCATIONS;
	fillInLANMessage( &msg );
	sendMessage(&msg);
}

void LANAPI::sendNATKeepalive( UnsignedInt destIPHost, UnsignedShort destPortHost )
{
	if (destIPHost == 0 || destPortHost == 0) return;
	if (!m_transport) return;
	// MSG_REQUEST_LOCATIONS is a tiny, side-effect-free packet that any LAN
	// peer accepts. We use it as the NAT-opening probe: the host fires this
	// at a newly-arrived joiner's external lobby addr (which the coordinator
	// just told us about) so the host's NAT installs an outbound mapping
	// before the joiner's MSG_REQUEST_GAME_INFO arrives. For full-cone hosts
	// this is a no-op; for port-restricted hosts it's required.
	LANMessage msg;
	msg.messageType = LANMessage::MSG_REQUEST_LOCATIONS;
	fillInLANMessage( &msg );
	m_transport->queueSend(destIPHost, destPortHost, (unsigned char*)&msg, sizeof(LANMessage));
	DEBUG_LOG(("LANAPI::sendNATKeepalive - sent to %u.%u.%u.%u:%u",
		(destIPHost >> 24) & 0xff, (destIPHost >> 16) & 0xff,
		(destIPHost >> 8) & 0xff, destIPHost & 0xff, destPortHost));
}

void LANAPI::sendNATProbeLowTTL( UnsignedInt destIPHost, UnsignedShort destPortHost )
{
	if (destIPHost == 0 || destPortHost == 0) return;
	if (!m_transport) return;
	Int ttl = 4;
	if (TheGlobalData && TheGlobalData->m_coordPunchTTL > 0)
		ttl = TheGlobalData->m_coordPunchTTL;
	m_transport->sendNATProbe(destIPHost, destPortHost, ttl);
	DEBUG_LOG(("LANAPI::sendNATProbeLowTTL - probed %u.%u.%u.%u:%u",
		(destIPHost >> 24) & 0xff, (destIPHost >> 16) & 0xff,
		(destIPHost >> 8) & 0xff, destIPHost & 0xff, destPortHost));
}

void LANAPI::RequestGameJoin( LANGameInfo *game, UnsignedInt ip /* = 0 */ )
{
	if ((m_pendingAction != ACT_NONE) && (m_pendingAction != ACT_JOINDIRECTCONNECT))
	{
		OnGameJoin( RET_BUSY, nullptr );
		return;
	}

	if (!game)
	{
		OnGameJoin( RET_GAME_GONE, nullptr );
		return;
	}

	// Robust observe path: if we already know the game is in progress, don't
	// depend on the host's UDP MSG_JOIN_DENY round-trip to learn the observer
	// port. That handshake rides the lobby UDP socket, which the host services
	// only on a throttled in-game pump and which competes with the in-game
	// network flood, so it is lossy and gets lossier as a match goes on. The
	// observer TCP port is deterministic (same formula the host uses in
	// startObserverHost), so derive it locally, surface the observe prompt
	// immediately, and let TCP handle connection reliability. The UDP join/deny
	// flow below remains as a fallback for when our local in-progress flag is
	// stale (e.g. the announce that would have set it was dropped).
	if (game->isGameInProgress())
	{
		UnsignedInt   hostIP  = game->getSlot(0)->getIP();
		UnsignedShort obsPort = (UnsignedShort)(NETWORK_BASE_PORT_NUMBER + LAN_OBSERVER_PORT_OFFSET);
		LANObsLog("RequestGameJoin: game flagged in-progress; going direct to observer host=%08X port=%u (no UDP deny needed)",
			hostIP, obsPort);
		s_pendingObserveHostIP = hostIP;
		s_pendingObservePort   = obsPort;
		OnGameJoin( RET_GAME_STARTED, game );
		return;
	}

	LANMessage msg;
	msg.messageType = LANMessage::MSG_REQUEST_JOIN;
	fillInLANMessage( &msg );
	msg.GameToJoin.gameIP = game->getSlot(0)->getIP();
	msg.GameToJoin.exeCRC = TheGlobalData->m_exeCRC;
	msg.GameToJoin.iniCRC = TheGlobalData->m_iniCRC;

	AsciiString s;
	GetStringFromRegistry("\\ergc", "", s);
	strlcpy(msg.GameToJoin.serial, s.str(), ARRAY_SIZE(msg.GameToJoin.serial));

	LANObsLog("RequestGameJoin: game not flagged in-progress; sending UDP MSG_REQUEST_JOIN to host=%08X",
		msg.GameToJoin.gameIP);
	sendMessage(&msg, ip);

	m_pendingAction = ACT_JOIN;
	m_expiration = timeGetTime() + m_actionTimeout;
	// Arm the once-a-second retransmit (see update()).
	m_pendingResendMsg = msg;
	m_pendingResendIP = ip;
	m_nextResendMs = timeGetTime() + 1000;
}

void LANAPI::RequestGameJoinDirectConnect(UnsignedInt ipaddress)
{
	if (m_pendingAction != ACT_NONE)
	{
		OnGameJoin( RET_BUSY, nullptr );
		return;
	}

	if (ipaddress == 0)
	{
		OnGameJoin( RET_GAME_GONE, nullptr );
		return;
	}

	m_directConnectRemoteIP = ipaddress;

	LANMessage msg;
	msg.messageType = LANMessage::MSG_REQUEST_GAME_INFO;
	fillInLANMessage(&msg);
	msg.PlayerInfo.ip = GetLocalIP();
	wcslcpy(msg.PlayerInfo.playerName, m_name.str(), ARRAY_SIZE(msg.PlayerInfo.playerName));

	sendMessage(&msg, ipaddress);

	m_pendingAction = ACT_JOINDIRECTCONNECT;
	m_expiration = timeGetTime() + m_actionTimeout;
	// Arm the once-a-second retransmit (see update()).
	m_pendingResendMsg = msg;
	m_pendingResendIP = ipaddress;
	m_nextResendMs = timeGetTime() + 1000;
}

void LANAPI::RequestGameLeave()
{
	LANMessage msg;
	msg.messageType = LANMessage::MSG_REQUEST_GAME_LEAVE;
	fillInLANMessage( &msg );
	wcslcpy(msg.PlayerInfo.playerName, m_name.str(), ARRAY_SIZE(msg.PlayerInfo.playerName));
	sendMessage(&msg);
	m_transport->update();  // Send immediately, before OnPlayerLeave below resets everything.

	if (m_currentGame && m_currentGame->getIP(0) == m_localIP)
	{
		// Exit out immediately if we're hosting
		OnPlayerLeave(m_name);
		removeGame(m_currentGame);
		m_currentGame = nullptr;
		m_inLobby = true;
	}
	else
	{
		m_pendingAction = ACT_LEAVE;
		m_expiration = timeGetTime() + m_actionTimeout;
	}
}

void LANAPI::RequestGameAnnounce()
{
	// In game - are we a game host?
	if (m_currentGame && !(m_currentGame->getIsDirectConnect()))
	{
		if (m_currentGame->getIP(0) == m_localIP || (m_currentGame->isGameInProgress() && TheNetwork && TheNetwork->isPacketRouter())) // if we're in game we should reply if we're the packet router
		{
			LANMessage reply;
			fillInLANMessage( &reply );
			reply.messageType = LANMessage::MSG_GAME_ANNOUNCE;

			AsciiString gameOpts = GameInfoToAsciiString(m_currentGame);
			strlcpy(reply.GameInfo.options,gameOpts.str(), ARRAY_SIZE(reply.GameInfo.options));
			wcslcpy(reply.GameInfo.gameName, m_currentGame->getName().str(), ARRAY_SIZE(reply.GameInfo.gameName));
			reply.GameInfo.inProgress = m_currentGame->isGameInProgress();
			reply.GameInfo.isDirectConnect = m_currentGame->getIsDirectConnect();

			sendMessage(&reply);
		}
	}
}

void LANAPI::RequestAccept()
{
	if (m_inLobby || !m_currentGame)
		return;

	LANMessage msg;
	fillInLANMessage( &msg );
	msg.messageType = LANMessage::MSG_SET_ACCEPT;
	msg.Accept.isAccepted = true;
	wcslcpy(msg.Accept.gameName, m_currentGame->getName().str(), ARRAY_SIZE(msg.Accept.gameName));
	sendMessage(&msg);
}

void LANAPI::RequestHasMap()
{
	if (m_inLobby || !m_currentGame)
		return;

	// Direct-connect parses can leave us temporarily unmatched in the slot
	// list (our slot carries the NAT-external IP until restored); reporting
	// from getSlot(-1) would read garbage.
	if (m_currentGame->getLocalSlotNum() < 0)
		return;

	LANMessage msg;
	fillInLANMessage( &msg );
	msg.messageType = LANMessage::MSG_MAP_AVAILABILITY;
	msg.MapStatus.hasMap = m_currentGame->getSlot(m_currentGame->getLocalSlotNum())->hasMap();
	wcslcpy(msg.MapStatus.gameName, m_currentGame->getName().str(), ARRAY_SIZE(msg.MapStatus.gameName));
	CRC mapNameCRC;
//mapNameCRC.computeCRC(m_currentGame->getMap().str(), m_currentGame->getMap().getLength());
	AsciiString portableMapName = TheGameState->realMapPathToPortableMapPath(m_currentGame->getMap());
	mapNameCRC.computeCRC(portableMapName.str(), portableMapName.getLength());
	msg.MapStatus.mapCRC = mapNameCRC.get();
	sendMessage(&msg);

	if (!msg.MapStatus.hasMap)
	{
		UnicodeString text;
		UnicodeString mapDisplayName;
		const MapMetaData *mapData = TheMapCache->findMap( m_currentGame->getMap() );
		Bool willTransfer = TRUE;
		if (mapData)
		{
			mapDisplayName.format(L"%ls", mapData->m_displayName.str());
			if (mapData->m_isOfficial)
				willTransfer = FALSE;
		}
		else
		{
			mapDisplayName.format(L"%hs", TheGameState->getMapLeafName(m_currentGame->getMap()).str());
			willTransfer = WouldMapTransfer(m_currentGame->getMap());
		}
		if (willTransfer)
			text.format(TheGameText->fetch("GUI:LocalPlayerNoMapWillTransfer"), mapDisplayName.str());
		else
			text.format(TheGameText->fetch("GUI:LocalPlayerNoMap"), mapDisplayName.str());
		OnChat(L"SYSTEM", m_localIP, text, LANCHAT_SYSTEM);
	}
}

void LANAPI::RequestChat( UnicodeString message, ChatType format )
{
	LANMessage msg;
	fillInLANMessage( &msg );
	wcslcpy(msg.Chat.gameName, (m_currentGame) ? m_currentGame->getName().str() : L"", ARRAY_SIZE(msg.Chat.gameName));
	msg.messageType = LANMessage::MSG_CHAT;
	msg.Chat.chatType = format;
	wcslcpy(msg.Chat.message, message.str(), ARRAY_SIZE(msg.Chat.message));
	sendMessage(&msg);

	OnChat(m_name, m_localIP, message, format);
}

void LANAPI::RequestGameStart()
{
	if (m_inLobby || !m_currentGame || m_currentGame->getIP(0) != m_localIP)
		return;

	LANMessage msg;
	msg.messageType = LANMessage::MSG_GAME_START;
	fillInLANMessage( &msg );
	// Ship the final options with the start order so every joiner loads from
	// this exact slot state (see LANMessage::StartGame).
	AsciiString finalOptions = GenerateGameOptionsString();
	strlcpy(msg.StartGame.options, finalOptions.str(), ARRAY_SIZE(msg.StartGame.options));
	// The start order is a single UDP datagram per peer on the direct-connect
	// path; a peer that misses it is left behind in the lobby while everyone
	// else loads. Receivers ignore repeats (isGameInProgress guard), so send
	// it a few times.
	sendMessage(&msg);
	m_transport->update(); // force a send
	sendMessage(&msg);
	m_transport->update();
	sendMessage(&msg);
	m_transport->update();

	OnGameStart();
}

void LANAPI::ResetGameStartTimer()
{
	m_gameStartTime = 0;
	m_gameStartSeconds = 0;
}

void LANAPI::RequestGameStartTimer( Int seconds )
{
	if (m_inLobby || !m_currentGame || m_currentGame->getIP(0) != m_localIP)
		return;

	UnsignedInt now = timeGetTime();
	m_gameStartTime = now + 1000;
	m_gameStartSeconds = (seconds) ? seconds - 1 : 0;

	LANMessage msg;
	msg.messageType = LANMessage::MSG_GAME_START_TIMER;
	msg.StartTimer.seconds = seconds;
	fillInLANMessage( &msg );
	sendMessage(&msg);
	m_transport->update(); // force a send

	OnGameStartTimer(seconds);
}

void LANAPI::RequestGameOptions( AsciiString gameOptions, Bool isPublic, UnsignedInt ip /* = 0 */ )
{
	DEBUG_ASSERTCRASH(gameOptions.getLength() < m_lanMaxOptionsLength, ("Game options string is too long!"));

	if (!m_currentGame)
		return;

	LANMessage msg;
	fillInLANMessage( &msg );
	msg.messageType = LANMessage::MSG_GAME_OPTIONS;
	strlcpy(msg.GameOptions.options, gameOptions.str(), ARRAY_SIZE(msg.GameOptions.options));
	sendMessage(&msg, ip);

	m_lastGameopt = gameOptions;

	int player;
	for (player = 0; player<MAX_SLOTS; ++player)
	{
		if (m_currentGame->getIP(player) == m_localIP)
		{
			OnGameOptions(m_localIP, player, AsciiString(msg.GameOptions.options));
			break;
		}
	}

	// We can request game options (side, color, etc) while we don't have a slot yet.  Of course, we don't need to
	// call OnGameOptions for those, so it's okay to silently fail.
	//DEBUG_ASSERTCRASH(player != MAX_SLOTS, ("Requested game options, but we're not in slot list!");
}

void LANAPI::RequestGameCreate( UnicodeString gameName, Bool isDirectConnect )
{
	// No games of the same name should exist...  Ignore that for now.
	/// @todo: make sure LAN games with identical names don't crash things like in RA2.

	if ((!m_inLobby || m_currentGame) && !isDirectConnect)
	{
		DEBUG_ASSERTCRASH(m_inLobby && m_currentGame, ("Can't create a game while in one!"));
		OnGameCreate(LANAPIInterface::RET_BUSY);
		return;
	}

	if (m_pendingAction != ACT_NONE)
	{
		OnGameCreate(LANAPIInterface::RET_BUSY);
		return;
	}

	// Create the local game object
	m_inLobby = false;
	LANGameInfo *myGame = NEW LANGameInfo;

	myGame->setSeed(GetTickCount());

//	myGame->setInProgress(false);
	myGame->enterGame();
	UnicodeString s;
	s.format(L"%8.8X%8.8X", m_localIP, myGame->getSeed());
	if (gameName.isEmpty())
		s.concat(m_name);
	else
		s.concat(gameName);

	s.truncateTo(g_lanGameNameLength);

	DEBUG_LOG(("Setting local game name to '%ls'", s.str()));

	myGame->setName(s);

	LANGameSlot newSlot;
	newSlot.setState(SLOT_PLAYER, m_name);
	newSlot.setIP(m_localIP);
	newSlot.setPort(NETWORK_BASE_PORT_NUMBER); // LAN game, everyone has a unique IP, so it's ok to use the same port.
	newSlot.setLastHeard(0);
	newSlot.setLogin(m_userName);
	newSlot.setHost(m_hostName);

	myGame->setSlot(0,newSlot);
	myGame->setNext(nullptr);
	LANPreferences pref;

	AsciiString mapName = pref.getPreferredMap();

	myGame->setMap(mapName);
	myGame->setIsDirectConnect(isDirectConnect);

	myGame->setLastHeard(timeGetTime());
	m_currentGame = myGame;

/// @todo: Need to initialize the players elsewere.
/*	for (int player = 1; player < MAX_SLOTS; ++player)
	{
		myGame->setPlayerName(player, L"");
		myGame->setIP(player, 0);
		myGame->setAccepted(player, false);
	}*/

	// Add the game to the local game list
	addGame(myGame);

	// Send an announcement
	//RequestSlotList();
/*
	LANMessage msg;
	wcslcpy(msg.name, m_name.str(), ARRAY_SIZE(msg.name));
	wcscpy(msg.GameInfo.gameName, myGame->getName().str());
	for (player=0; player<MAX_SLOTS; ++player)
	{
		wcscpy(msg.GameInfo.name[player], myGame->getPlayerName(player).str());
		msg.GameInfo.ip[player] = myGame->getIP(player);
		msg.GameInfo.playerAccepted[player] = myGame->getAccepted(player);
	}
	msg.messageType = LANMessage::MSG_GAME_ANNOUNCE;
*/
	OnGameCreate(LANAPIInterface::RET_OK);
}


/*static const char slotListID		= 'S';
static const char gameOptionsID	= 'G';
static const char acceptID			= 'A';
static const char wannaStartID	= 'W';

AsciiString LANAPI::createSlotString()
{
	AsciiString slotList;
	slotList.concat(slotListID);
	for (int i=0; i<MAX_SLOTS; ++i)
	{
		LANGameSlot *slot = GetMyGame()->getLANSlot(i);
		AsciiString str;
		if (slot->isHuman())
		{
			str = "H";
			LANPlayer *user = slot->getUser();
			DEBUG_ASSERTCRASH(user, ("Human player has no User*!"));
			AsciiString name;
			name.translate(user->getName());
			str.concat(name);
			str.concat(',');
		}
		else if (slot->isAI())
		{
			if (slot->getState() == SLOT_EASY_AI)
				str = "CE,";
			if (slot->getState() == SLOT_MED_AI)
				str = "CM,";
			else if (slot->getState() == SLOT_TACTICAL_AI)
				str = "CT,";
			else
				str = "CB,";
		}
		else if (slot->getState() == SLOT_OPEN)
		{
			str = "O,";
		}
		else if (slot->getState() == SLOT_CLOSED)
		{
			str = "X,";
		}
		else
		{
			DEBUG_CRASH(("Bad slot type"));
			str = "X,";
		}

		slotList.concat(str);
	}
	return slotList;
}
*/
/*
void LANAPI::RequestSlotList()
{

	LANMessage reply;
	reply.messageType = LANMessage::MSG_GAME_ANNOUNCE;
	wcslcpy(reply.name, m_name.str(), ARRAY_SIZE(reply.name));
	int player;
	for (player = 0; player < MAX_SLOTS; ++player)
	{
		wcslcpy(reply.GameInfo.name[player], m_currentGame->getPlayerName(player).str(), ARRAY_SIZE(reply.GameInfo.name[player]));
		reply.GameInfo.ip[player] = m_currentGame->getIP(player);
		reply.GameInfo.playerAccepted[player] = m_currentGame->getSlot(player)->isAccepted();
	}
	wcslcpy(reply.GameInfo.gameName, m_currentGame->getName().str(), ARRAY_SIZE(reply.GameInfo.gameName));
	reply.GameInfo.inProgress = m_currentGame->isGameInProgress();

	sendMessage(&reply);

	OnSlotList(LANAPIInterface::RET_OK, m_currentGame);
}
*/
void LANAPI::RequestSetName( UnicodeString newName )
{
	newName.trim();
	if (m_pendingAction != ACT_NONE)
	{
		// Can't change name while joining games
		OnNameChange(m_localIP, newName);
		return;
	}

	// Set up timer
	m_lastResendTime = timeGetTime();

	if (m_inLobby && m_pendingAction == ACT_NONE)
	{
		m_name = newName;
		LANMessage msg;
		fillInLANMessage( &msg );
		msg.messageType = LANMessage::MSG_LOBBY_ANNOUNCE;
		sendMessage(&msg);

		// Update the interface
		LANPlayer *player = LookupPlayer(m_localIP);
		if (!player)
		{
			player = NEW LANPlayer;
			player->setIP(m_localIP);
		}
		else
		{
			removePlayer(player);
		}
		player->setName(m_name);
		player->setHost(m_hostName);
		player->setLogin(m_userName);
		player->setLastHeard(timeGetTime());

		addPlayer(player);

		OnNameChange(player->getIP(), player->getName());
	}
}

void LANAPI::fillInLANMessage( LANMessage *msg )
{
	if (!msg)
		return;

	wcslcpy(msg->name, m_name.str(), ARRAY_SIZE(msg->name));
	strlcpy(msg->userName, m_userName.str(), ARRAY_SIZE(msg->userName));
	strlcpy(msg->hostName, m_hostName.str(), ARRAY_SIZE(msg->hostName));
}

void LANAPI::RequestLobbyLeave( Bool forced )
{
	LANMessage msg;
	msg.messageType = LANMessage::MSG_REQUEST_LOBBY_LEAVE;
	fillInLANMessage( &msg );
	sendMessage(&msg);

	if (forced)
		m_transport->update();
}

// Misc utility functions
LANGameInfo * LANAPI::LookupGame( UnicodeString gameName )
{
	LANGameInfo *theGame = m_games;

	while (theGame && theGame->getName() != gameName)
	{
		theGame = theGame->getNext();
	}

	return theGame; // null means we didn't find anything.
}

LANGameInfo * LANAPI::LookupGameByListOffset( Int offset )
{
	LANGameInfo *theGame = m_games;

	if (offset < 0)
		return nullptr;

	while (offset-- && theGame)
	{
		theGame = theGame->getNext();
	}

	return theGame; // null means we didn't find anything.
}

LANGameInfo* LANAPI::LookupGameByHost(UnsignedInt hostIP)
{
	LANGameInfo* lastGame = nullptr;
	UnsignedInt lastHeard = 0;

	for (LANGameInfo* game = m_games; game; game = game->getNext())
	{
		if (game->getHostIP() == hostIP && game->getLastHeard() >= lastHeard)
		{
			lastGame = game;
			lastHeard = game->getLastHeard();
		}
	}

	return lastGame;
}

void LANAPI::removeGame( LANGameInfo *game )
{
	LANGameInfo *g = m_games;
	if (!game)
	{
		return;
	}
	else if (m_games == game)
	{
		m_games = m_games->getNext();
	}
	else
	{
		while (g->getNext() && g->getNext() != game)
		{
			g = g->getNext();
		}
		if (g->getNext() == game)
		{
			g->setNext(game->getNext());
		}
		else
		{
			// Odd.  We went the whole way without finding it in the list.
			DEBUG_CRASH(("LANGameInfo wasn't in the list"));
		}
	}
}

LANPlayer * LANAPI::LookupPlayer( UnsignedInt playerIP )
{
	LANPlayer *thePlayer = m_lobbyPlayers;

	while (thePlayer && thePlayer->getIP() != playerIP)
	{
		thePlayer = thePlayer->getNext();
	}

	return thePlayer; // null means we didn't find anything.
}

void LANAPI::removePlayer( LANPlayer *player )
{
	LANPlayer *p = m_lobbyPlayers;
	if (!player)
	{
		return;
	}
	else if (m_lobbyPlayers == player)
	{
		m_lobbyPlayers = m_lobbyPlayers->getNext();
	}
	else
	{
		while (p->getNext() && p->getNext() != player)
		{
			p = p->getNext();
		}
		if (p->getNext() == player)
		{
			p->setNext(player->getNext());
		}
		else
		{
			// Odd.  We went the whole way without finding it in the list.
			DEBUG_CRASH(("LANPlayer wasn't in the list"));
		}
	}
}

void LANAPI::addGame( LANGameInfo *game )
{
	if (!m_games)
	{
		m_games = game;
		game->setNext(nullptr);
		return;
	}
	else
	{
		if (game->getName().compareNoCase(m_games->getName()) < 0)
		{
			game->setNext(m_games);
			m_games = game;
			return;
		}
		else
		{
			LANGameInfo *g = m_games;
			while (g->getNext() && g->getNext()->getName().compareNoCase(game->getName()) > 0)
			{
				g = g->getNext();
			}
			game->setNext(g->getNext());
			g->setNext(game);
			return;
		}
	}
}

void LANAPI::addPlayer( LANPlayer *player )
{
	if (!m_lobbyPlayers)
	{
		m_lobbyPlayers = player;
		player->setNext(nullptr);
		return;
	}
	else
	{
		if (player->getName().compareNoCase(m_lobbyPlayers->getName()) < 0)
		{
			player->setNext(m_lobbyPlayers);
			m_lobbyPlayers = player;
			return;
		}
		else
		{
			LANPlayer *p = m_lobbyPlayers;
			while (p->getNext() && p->getNext()->getName().compareNoCase(player->getName()) > 0)
			{
				p = p->getNext();
			}
			player->setNext(p->getNext());
			p->setNext(player);
			return;
		}
	}
}

Bool LANAPI::SetLocalIP( UnsignedInt localIP )
{
	Bool retval = TRUE;
	m_localIP = localIP;

	m_transport->reset();
	retval = m_transport->init(m_localIP, lobbyPort);
	m_transport->allowBroadcasts(true);

	return retval;
}

Bool LANAPI::SetLocalIPAdoptingSocket( UnsignedInt localIP, Int fd )
{
	// Online (coordinator) handoff: take over the very socket that did the
	// STUN discovery and the hole punch, instead of closing it and binding
	// a fresh one on the same port.
	//
	// Rebinding looks equivalent but is not: the NAT mapping belongs to the
	// SOCKET, not the port. Carrier-grade NATs (Starlink was the case that
	// exposed this) allocate a NEW external port to the replacement socket,
	// so the peer -- which the coordinator told to talk to the ORIGINAL
	// external port -- ends up punching an address nothing listens on, and
	// our replies arrive from a port its NAT never expected and drops.
	// Symptom: punch outcome lobby=false while game=true, because the
	// in-game socket is handed over by fd and keeps its mapping.
	m_localIP = localIP;
	m_transport->reset();
	Bool retval = m_transport->initFromFD(fd, localIP, lobbyPort);
	if (!retval)
	{
		// initFromFD owns (and has closed) the fd on failure; fall back to a
		// fresh bind so the lobby still works on LAN-ish networks.
		ReleaseLog("LAN: adopting coordinator lobby socket FAILED; falling back to a fresh bind");
		retval = m_transport->init(m_localIP, lobbyPort);
	}
	else
	{
		ReleaseLog("LAN: adopted the punched coordinator lobby socket (port %u)", (unsigned)lobbyPort);
	}
	m_transport->allowBroadcasts(true);
	return retval;
}

void LANAPI::SetLocalIP( AsciiString localIP )
{
	UnsignedInt resolvedIP = ResolveIP(localIP);
	SetLocalIP(resolvedIP);
}

Bool LANAPI::AmIHost()
{
	return m_currentGame && m_currentGame->getIP(0) == m_localIP;
}

void LANAPI::setIsActive(Bool isActive) {
	DEBUG_LOG(("LANAPI::setIsActive - entering"));
	if (isActive != m_isActive) {
		DEBUG_LOG(("LANAPI::setIsActive - m_isActive changed to %s", isActive ? "TRUE" : "FALSE"));
		if (isActive == FALSE) {
			if ((m_inLobby == FALSE) && (m_currentGame != nullptr)) {
				LANMessage msg;
				fillInLANMessage( &msg );
				msg.messageType = LANMessage::MSG_INACTIVE;
				sendMessage(&msg);
				DEBUG_LOG(("LANAPI::setIsActive - sent an IsActive message"));
			}
		}
	}
	m_isActive = isActive;
}

// =====================================================================
// Observer mode
// =====================================================================

#include "Common/Recorder.h"
#include "Common/FileSystem.h"
#include "GameNetwork/MapDownloadHook.h"

// Socket close for the relay fd path; LANAPI.cpp otherwise only touches
// sockets through Transport/UDP wrappers.
#ifdef _WIN32
#define LANAPI_CLOSE_SOCKET(fd) ::closesocket(fd)
#else
#define LANAPI_CLOSE_SOCKET(fd) ::close(fd)
#endif

// Find a cached map by CRC. The MapCache is keyed by lowercased filename, but
// the live-observer flow only knows the map by the CRC carried in the replay
// header, so we scan. Returns the matching metadata, or NULL when no cached
// map has that CRC (or the cache / crc is unusable).
static const MapMetaData *findObserverMapByCRC(UnsignedInt crc)
{
	if (!TheMapCache || crc == 0)
		return NULL;

	for (MapCache::iterator it = TheMapCache->begin(); it != TheMapCache->end(); ++it)
	{
		if (it->second.m_CRC == crc)
			return &it->second;
	}
	return NULL;
}

void LANAPI::startObserverHost()
{
	if (!AmIHost())
	{
		LANObsLog("startObserverHost: skipped, AmIHost()=0 (currentGame=%d)", m_currentGame ? 1 : 0);
		return;
	}
	if (!m_observerHost)
		m_observerHost = new LANObserverHost();
	if (!m_observerHost->isRunning())
	{
		// New listen socket, so a new game: let it warn once again if
		// spectators still cannot get in.
		m_observeUnreachableReported = FALSE;
		UnsignedShort port = (UnsignedShort)(NETWORK_BASE_PORT_NUMBER + LAN_OBSERVER_PORT_OFFSET);
		if (!m_observerHost->start(port))
		{
			// Leave the failure visible; updateObserver re-attempts on a
			// throttle while the game is in progress.
			LANObsLog("startObserverHost: start(%u) FAILED; will retry from updateObserver", port);
			DEBUG_LOG(("LANAPI::startObserverHost - failed to start on port %u", port));
			delete m_observerHost;
			m_observerHost = nullptr;
			return;
		}
	}
	// Wire the host's currently-recording .rep file path. If recording hasn't
	// started yet (we may be a frame early), updateObserver refreshes the
	// path on every tick via the same lookup.
	if (TheRecorder)
	{
		AsciiString dir = RecorderClass::getReplayDir();
		AsciiString file = RecorderClass::getLastReplayFileName();
		file.concat(RecorderClass::getReplayExtention());
		AsciiString full = dir;
		full.concat(file);
		m_observerHost->setReplayFile(full);
		LANObsLog("startObserverHost: streaming replay '%s'", full.str());
	}
}

void LANAPI::stopObserverHost()
{
	if (m_observerHost)
	{
		m_observerHost->stop();
		delete m_observerHost;
		m_observerHost = nullptr;
	}
}

void LANAPI::stopObserverClient()
{
	if (m_observerClient)
	{
		// Tell the recorder so it stops waiting at EOF.
		if (TheRecorder)
			TheRecorder->setLiveObserverStreamOpen(FALSE);
		m_observerClient->close();
		delete m_observerClient;
		m_observerClient = nullptr;
	}
	m_observerClientPlaybackKicked = FALSE;
	m_observerProgressLastMs = 0;
	m_observerProgressLastBytes = 0;
}

// Observer-client reconnect state. Written by RequestObserve, consumed by
// updateObserver: if the TCP connect fails or the stream dies before
// playback starts, we retry the connect a couple of times before surfacing
// an error dialog. File statics (like the s_pendingObserve pair) so no
// header change is needed.
static UnsignedInt   s_observeHostIPHostOrder = 0;
static UnsignedShort s_observeConnectPort     = 0;
static Int           s_observeAttemptsLeft    = 0;
static UnsignedInt   s_observeLastBytes       = 0;
static UnsignedInt   s_observeLastProgressMs  = 0;
// Give up on a connected-but-silent host after this long without a single
// new byte. Generous because the host may still be inside its blocking map
// load right after game start, during which nothing is pumped.
static const UnsignedInt OBS_CLIENT_STALL_TIMEOUT_MS = 60000;

void LANAPI::RequestObserve(UnsignedInt hostIP, UnsignedShort observerPort)
{
	LANObsLog("RequestObserve: hostIP=%08X port=%u", hostIP, observerPort);
	if (observerPort == 0)
	{
		LANObsLog("RequestObserve: aborting, no observer port");
		DEBUG_LOG(("LANAPI::RequestObserve - no observer port advertised by host"));
		return;
	}

	s_observeHostIPHostOrder = hostIP;
	s_observeConnectPort     = observerPort;
	s_observeAttemptsLeft    = 2; // retries after the initial attempt
	s_observeLastBytes       = 0;
	s_observeLastProgressMs  = timeGetTime();

	stopObserverClient();
	m_observerClient = new LANObserverClient();

	// Scratch file in the replay dir. Reuse a single name so we don't pile
	// up junk; the file is rewritten each session. A fresh install that has
	// never recorded a replay has no Replays\ directory yet, so create it
	// or the fopen below fails with ENOENT.
	if (TheFileSystem)
		TheFileSystem->createDirectory(RecorderClass::getReplayDir());
	AsciiString path = RecorderClass::getReplayDir();
	path.concat("_live_observer");
	path.concat(RecorderClass::getReplayExtention());

	// hostIP arrives in HOST byte order from LANMessage payloads (consistent
	// with how the rest of LANAPI deals with IPs). Sockets want network order.
	UnsignedInt nbo = htonl(hostIP);

	if (!m_observerClient->connect(nbo, observerPort, path))
	{
		DEBUG_LOG(("LANAPI::RequestObserve - connect kickoff failed"));
		stopObserverClient();
		return;
	}
	if (TheRecorder)
		TheRecorder->setLiveObserverStreamOpen(TRUE);
	m_observerClientPlaybackKicked = FALSE;
	DEBUG_LOG(("LANAPI::RequestObserve - observing host %08X port %u", hostIP, observerPort));
}

void LANAPI::RequestObserveAdoptedFd(Int fd)
{
	LANObsLog("RequestObserveAdoptedFd: fd=%d", fd);
	if (fd < 0)
		return;

	// No reconnect target exists for a relayed stream: a retry would need a
	// whole new observe request through the coordinator. Disable the
	// connect-retry machinery; failures surface the normal error dialog.
	s_observeHostIPHostOrder = 0;
	s_observeConnectPort     = 0;
	s_observeAttemptsLeft    = 0;
	s_observeLastBytes       = 0;
	s_observeLastProgressMs  = timeGetTime();

	stopObserverClient();
	m_observerClient = new LANObserverClient();

	// Same fresh-install consideration as RequestObserve above.
	if (TheFileSystem)
		TheFileSystem->createDirectory(RecorderClass::getReplayDir());
	AsciiString path = RecorderClass::getReplayDir();
	path.concat("_live_observer");
	path.concat(RecorderClass::getReplayExtention());

	if (!m_observerClient->adoptFd(fd, path))
	{
		LANObsLog("RequestObserveAdoptedFd: adoptFd failed");
		stopObserverClient();
		return;
	}
	if (TheRecorder)
		TheRecorder->setLiveObserverStreamOpen(TRUE);
	m_observerClientPlaybackKicked = FALSE;
}

// Make sure the map referenced by the just-buffered live-observer snapshot
// exists locally before we start playback. The observer stream never carries
// the .map itself; we only learn which map is being played from the replay
// header's GameInfo string. If the map is missing, loadMap() later fails
// silently into black/corrupt terrain, so we fetch it from the cncstats CDN
// here (the same TheMapDownloadHook used by the LAN/WOL join path).
//
// Returns TRUE when the map is available (already present, freshly installed,
// or we couldn't even determine the map and should let playback proceed and
// report its own failure). Returns FALSE only when the map is genuinely
// missing and could not be obtained, so the caller should abort the join.
Bool LANAPI::ensureObserverMapAvailable(AsciiString relReplayPath)
{
	if (!TheRecorder || !TheMapCache)
		return TRUE; // can't check; behave as before and let playback proceed

	// Peek the replay header to learn the map + CRC. forPlayback=FALSE reads
	// the header and closes the file again without arming playback, so the
	// playbackFileLiveObserver() call that follows is unaffected.
	RecorderClass::ReplayHeader header;
	header.filename = relReplayPath;
	header.forPlayback = FALSE;
	if (!TheRecorder->readReplayHeader(header))
	{
		LANObsLog("ensureObserverMapAvailable: readReplayHeader failed for '%s'", relReplayPath.str());
		return TRUE; // let playbackFileLiveObserver surface the real failure
	}

	// readReplayHeader resets its own GameInfo before returning, so re-parse
	// the raw options string into a local GameInfo to read the map identity.
	ReplayGameInfo info;
	info.reset();
	info.enterGame();
	if (!ParseAsciiStringToGameInfo(&info, header.gameOptions))
	{
		LANObsLog("ensureObserverMapAvailable: GameInfo parse failed; proceeding");
		return TRUE;
	}

	AsciiString mapName = info.getMap();
	UnsignedInt mapCRC  = info.getMapCRC();
	Int         mapMask = info.getMapContentsMask();

	// Already have it? CRC is the exact key; fall back to filename lookup.
	if (findObserverMapByCRC(mapCRC) != NULL)
		return TRUE;
	if (!mapName.isEmpty() && TheMapCache->findMap(mapName) != NULL)
		return TRUE;

	// Missing locally. We need a CRC and the download hook to fetch it; the
	// hook is null in classic Generals builds, in which case we can't recover.
	if (mapCRC == 0 || mapName.isEmpty() || TheMapDownloadHook == NULL)
	{
		LANObsLog("ensureObserverMapAvailable: map '%s' missing and not fetchable (crc=%u hook=%d)",
			mapName.str(), mapCRC, TheMapDownloadHook != NULL);
		return FALSE;
	}

	// Let the user know the join is fetching the map (the download is a
	// blocking call, same as the lobby map-vote / join paths).
	UnicodeString msg;
	msg.format(L"Downloading map for observer playback...");
	OnChat(L"", 0, msg, LANCHAT_SYSTEM);

	// The host's map name is a full relative .map path, so it doubles as the
	// install path (mirrors the LAN join precedent in LANAPICallbacks.cpp).
	// 0x7E = all sidecars (preview|ini|str|solo|assets|readme); use the
	// header's mask when present, otherwise ask for everything. The hook
	// CRC-verifies the bytes and refreshes the cache on success.
	UnsignedInt mask = (mapMask != 0) ? (UnsignedInt)mapMask : 0x7E;
	if (!TheMapDownloadHook(mapName, mapCRC, mask))
	{
		LANObsLog("ensureObserverMapAvailable: CDN download failed (crc=%u path='%s')",
			mapCRC, mapName.str());
		return FALSE;
	}

	// Confirm the refreshed cache now sees the installed map.
	if (findObserverMapByCRC(mapCRC) != NULL)
		return TRUE;
	if (TheMapCache->findMap(mapName) != NULL)
		return TRUE;

	LANObsLog("ensureObserverMapAvailable: map still not in cache after download (crc=%u)", mapCRC);
	return FALSE;
}

void LANAPI::updateObserver()
{
	// Host self-heal: if we're hosting an in-progress LAN game, the observer
	// listen socket must be up. It can be missing because the bind failed at
	// game-start time (port still held by another process/instance) or
	// because startObserverHost never ran; either way, re-attempt on a
	// throttle so a transient failure doesn't permanently kill observing.
	// While it is up, refresh the streamed replay path every tick: at
	// OnGameStart time recording hasn't begun yet, so the path wired then
	// can predate the recorder's actual file.
	if (m_currentGame && m_currentGame->isGameInProgress() && AmIHost())
	{
		if (!m_observerHost || !m_observerHost->isRunning())
		{
			static UnsignedInt s_lastListenRetryMs = 0;
			UnsignedInt now = timeGetTime();
			if (s_lastListenRetryMs == 0 || now - s_lastListenRetryMs >= 5000u)
			{
				s_lastListenRetryMs = now;
				LANObsLog("updateObserver: observer listen socket not running; attempting start");
				startObserverHost();
			}
		}
		else if (TheRecorder)
		{
			AsciiString file = RecorderClass::getLastReplayFileName();
			file.concat(RecorderClass::getReplayExtention());
			AsciiString full = RecorderClass::getReplayDir();
			full.concat(file);
			m_observerHost->setReplayFile(full);
		}
	}

	// Online-coordinator observer relay (host side): keep the adopted
	// coordinator session alive during the match and service observe
	// requests by dialing a relay connection per token and attaching it to
	// the observer host exactly like an accepted LAN observer.
	if (m_inGameCoord)
	{
		m_inGameCoord->update();
		AsciiString token;
		while (m_inGameCoord->consumeObserverRequestToken(&token))
		{
			LANObsLog("updateObserver: observer_request token=%s; opening relay", token.str());
			if (!m_observerHost)
				startObserverHost();
			Int relayFd = m_inGameCoord->openObserverRelayFd(token, TRUE);
			if (relayFd >= 0 && m_observerHost)
			{
				UnicodeString name = L"Online observer";
				if (m_observerHost->adoptObserverFd(relayFd, name))
				{
					UnicodeString msg;
					msg.format(L"%s connected through the online relay.", name.str());
					OnChat(L"", 0, msg, LANCHAT_SYSTEM);
				}
			}
			else if (relayFd >= 0)
			{
				LANAPI_CLOSE_SOCKET(relayFd);
			}
		}
	}

	if (m_observerHost)
	{
		// Capture up to a handful of names per tick for chat notification.
		UnicodeString newNames[4];
		Int newCount = m_observerHost->update(newNames, 4);
		for (Int i = 0; i < newCount; ++i)
		{
			UnicodeString msg;
			msg.format(L"%s is now observing the game.", newNames[i].str());
			OnChat(L"", 0, msg, LANCHAT_SYSTEM);
		}
	}

	if (m_observerClient)
	{
		m_observerClient->update();

		// Track byte progress for the stall guard.
		if (m_observerClient->bytesReceived() != s_observeLastBytes)
		{
			s_observeLastBytes      = m_observerClient->bytesReceived();
			s_observeLastProgressMs = timeGetTime();
		}

		// A connected host that never sends anything (not even the 4-byte
		// snapshot header) is as dead as a failed connect; fold it into the
		// same retry path below by treating it as a failed attempt.
		Bool stalled = (m_observerClient->state() == LANObserverClient::STATE_BUFFERING
		    && timeGetTime() - s_observeLastProgressMs > OBS_CLIENT_STALL_TIMEOUT_MS);

		// Connect failed, stream died before playback started, or stalled:
		// retry the TCP connect (each attempt has its own handshake deadline),
		// then surface an error dialog instead of silently doing nothing.
		// STATE_IDLE here means a retry's connect() kickoff itself failed
		// (socket/file error); treat it like a failed attempt too.
		if (!m_observerClientPlaybackKicked
		    && (m_observerClient->state() == LANObserverClient::STATE_CLOSED
		        || m_observerClient->state() == LANObserverClient::STATE_IDLE
		        || stalled))
		{
			if (s_observeAttemptsLeft > 0 && s_observeConnectPort != 0)
			{
				--s_observeAttemptsLeft;
				LANObsLog("updateObserver: observer connect %s; retrying (%d attempt(s) left)",
					stalled ? "stalled" : "failed", s_observeAttemptsLeft);
				OnChat(L"", 0, TheGameText->FETCH_OR_SUBSTITUTE("LAN:ObserveRetrying",
					L"Observer connection failed; retrying..."), LANCHAT_SYSTEM);
				AsciiString path = RecorderClass::getReplayDir();
				path.concat("_live_observer");
				path.concat(RecorderClass::getReplayExtention());
				s_observeLastBytes      = 0;
				s_observeLastProgressMs = timeGetTime();
				m_observerClient->connect(htonl(s_observeHostIPHostOrder), s_observeConnectPort, path);
				if (TheRecorder)
					TheRecorder->setLiveObserverStreamOpen(TRUE);
				return;
			}
			LANObsLog("updateObserver: observer connect failed after all retries; giving up");

			// Tell the host, but only when the TCP handshake never completed
			// once. That case means our SYNs never got answered while the
			// host's listen socket was bound and healthy, which from the
			// host's side is invisible: it sees no connection attempt at all.
			// The lobby UDP path is a separate protocol and port and is
			// routinely fine when the stream port is blocked, which is exactly
			// how 2026-08-21 went. A stall or a mid-stream close means we did
			// connect, so inbound works and this would be a false alarm.
			if (!m_observerClient->everConnected() && s_observeHostIPHostOrder != 0)
			{
				// Host byte order throughout, like every other IP in a
				// LANMessage payload; only the socket calls use htonl.
				LANMessage failMsg;
				fillInLANMessage( &failMsg );
				failMsg.messageType                 = LANMessage::MSG_OBSERVE_UNREACHABLE;
				failMsg.ObserveFailure.gameIP       = s_observeHostIPHostOrder;
				failMsg.ObserveFailure.observerPort = s_observeConnectPort;
				failMsg.ObserveFailure.attemptMs    = timeGetTime() - s_observeLastProgressMs;
				LANObsLog("updateObserver: reporting unreachable observer port %u to host %08X",
					s_observeConnectPort, s_observeHostIPHostOrder);
				sendMessage(&failMsg, s_observeHostIPHostOrder);
			}

			stopObserverClient();
			UnicodeString title = TheGameText->fetch("LAN:JoinFailed");
			UnicodeString body  = TheGameText->FETCH_OR_SUBSTITUTE("LAN:ObserveConnectFailedBody",
				L"Could not connect to the host's observer stream. Check that the host machine's firewall allows the game and that the host is running the same game version.");
			MessageBoxOk(title, body, NULL);
			return;
		}

		// Periodic snapshot-download progress as a lobby chat line so the user
		// can see the join is doing something during the catch-up wait, which
		// scales with game length. Throttled to once per second and only while
		// the snapshot is still arriving (BUFFERING + non-zero declared size).
		if (m_observerClient->state() == LANObserverClient::STATE_BUFFERING
		    && m_observerClient->snapshotSize() > 0)
		{
			UnsignedInt now      = timeGetTime();
			UnsignedInt received = m_observerClient->bytesReceived();
			UnsignedInt total    = m_observerClient->snapshotSize();
			Bool timeElapsed = (now - m_observerProgressLastMs) >= 1000u;
			if (m_observerProgressLastMs == 0 || timeElapsed)
			{
				UnsignedInt pct  = total ? (UnsignedInt)((UnsignedInt64)received * 100u / total) : 0;
				Real receivedMB  = (Real)received / (1024.0f * 1024.0f);
				Real totalMB     = (Real)total    / (1024.0f * 1024.0f);
				UnicodeString msg;
				msg.format(L"Downloading replay: %.1f / %.1f MB (%u%%)",
					receivedMB, totalMB, pct);
				OnChat(L"", 0, msg, LANCHAT_SYSTEM);
				m_observerProgressLastMs    = now;
				m_observerProgressLastBytes = received;
			}
		}

		// Transition from snapshot-ready -> kick off playback (once).
		if (!m_observerClientPlaybackKicked
		    && m_observerClient->state() == LANObserverClient::STATE_READY
		    && TheRecorder)
		{
			AsciiString path = m_observerClient->localFilePath();
			// playbackFile expects a path relative to getReplayDir(); strip
			// the dir prefix if present.
			AsciiString dir = RecorderClass::getReplayDir();
			AsciiString rel = path;
			if (path.startsWith(dir.str()))
				rel = path.str() + dir.getLength();

			LANObsLog("kick playback: localPath='%s' rel='%s' snapshot=%u written=%u",
				path.str(), rel.str(),
				m_observerClient->snapshotSize(), m_observerClient->bytesReceived());

			// Make sure we actually have the map being played before starting
			// playback; pull it from the cncstats CDN if not. Failing here
			// (vs. blindly starting playback) avoids the silent black-terrain
			// loadMap() failure when the observer lacks the map.
			if (!ensureObserverMapAvailable(rel))
			{
				LANObsLog("observer map unavailable; aborting join");
				DEBUG_LOG(("LANAPI - observer map unavailable; aborting"));
				stopObserverClient();
				m_observerClientPlaybackKicked = TRUE; // don't re-run the gate
				UnicodeString title = TheGameText->fetch("LAN:JoinFailed");
				UnicodeString body  = TheGameText->fetch("LAN:ObserveMapMissingBody");
				MessageBoxOk(title, body, NULL);
				return;
			}

			DEBUG_LOG(("LANAPI - observer kicking playbackFileLiveObserver('%s')", rel.str()));
			if (TheRecorder->playbackFileLiveObserver(rel))
			{
				LANObsLog("playbackFileLiveObserver returned TRUE");
				m_observerClient->markPlaybackStarted();
				m_observerClientPlaybackKicked = TRUE;
			}
			else
			{
				LANObsLog("playbackFileLiveObserver returned FALSE; aborting");
				DEBUG_LOG(("LANAPI - observer playbackFileLiveObserver failed; aborting"));
				stopObserverClient();
				// Surface the failure so the user isn't left staring at the
				// lobby wondering what happened. Most common cause: the host's
				// snapshot was incomplete or the .rep prefix didn't parse.
				UnicodeString title = TheGameText->fetch("LAN:JoinFailed");
				UnicodeString body  = TheGameText->fetch("LAN:ObserveFailedBody");
				MessageBoxOk(title, body, NULL);
			}
		}

		// Stream closed: tell the recorder so it stops waiting at EOF.
		if (m_observerClient && m_observerClient->isStreamClosed())
		{
			if (TheRecorder)
				TheRecorder->setLiveObserverStreamOpen(FALSE);
			// Don't delete the client object yet; the recorder may still be
			// reading the buffered file. Cleanup happens at reset/dtor.
		}

		// User exited the live-observer playback (ESC, alt-F4, replay-end
		// after stream closure, etc.). Clean up the network client now so
		// the next observe attempt starts from a fresh state.
		if (m_observerClientPlaybackKicked
		    && TheRecorder
		    && !TheRecorder->isLiveObserverMode())
		{
			DEBUG_LOG(("LANAPI - recorder exited LIVE_OBSERVER, dropping observer client"));
			stopObserverClient();
		}
	}
}
