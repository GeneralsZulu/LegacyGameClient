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

///////////////////////////////////////////////////////////////////////////////////////
// FILE: LANAPICallbacks.cpp
// Author: Chris Huybregts, October 2001
// Description: LAN API Callbacks
///////////////////////////////////////////////////////////////////////////////////////
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "strtok_r.h"
#include "Common/GameEngine.h"
#include "Common/GlobalData.h"
#include "Common/MessageStream.h"
#include "Common/MultiplayerSettings.h"
#include "Common/PlayerTemplate.h"
#include "Common/QuotedPrintable.h"
#include "Common/RandomValue.h"
#include "Common/ReleaseLog.h"
#include "Common/UserPreferences.h"
#include "GameClient/Color.h"
#include "GameClient/GameText.h"
#include "GameClient/LanguageFilter.h"
#include "GameClient/MapUtil.h"
#include "GameClient/MessageBox.h"
#include "GameLogic/GameLogic.h"
#include "GameNetwork/FileTransfer.h"
#include "GameNetwork/LANAPICallbacks.h"
#include "GameNetwork/MapDownloadHook.h"
#include "GameNetwork/networkutil.h"

// Zero Hour only: the multiplayer loading-screen "battlefield intel" panel.
// StatsUploader (and the radarvan endpoints it talks to) live in the Zero Hour
// tree, so gate the include and the trigger on the ZH build.
#if RTS_ZEROHOUR
#include "Common/StatsUploader.h"
#endif

LANAPI *TheLAN = nullptr;
extern Bool LANbuttonPushed;


//Colors used for the chat dialogs
const Color playerColor =  GameMakeColor(255,255,255,255);
const Color gameColor =  GameMakeColor(255,255,255,255);
const Color gameInProgressColor =  GameMakeColor(128,128,128,255);
const Color chatNormalColor =  GameMakeColor(50,215,230,255);
const Color chatActionColor =  GameMakeColor(255,0,255,255);
const Color chatLocalNormalColor =  GameMakeColor(255,128,0,255);
const Color chatLocalActionColor =  GameMakeColor(128,255,255,255);
const Color chatSystemColor =  GameMakeColor(255,255,255,255);
const Color acceptTrueColor =  GameMakeColor(0,255,0,255);
const Color acceptFalseColor =  GameMakeColor(255,0,0,255);


UnicodeString LANAPIInterface::getErrorStringFromReturnType( ReturnType ret )
{
	switch (ret)
	{
		case RET_OK:
			return TheGameText->fetch("LAN:OK");
		case RET_TIMEOUT:
			return TheGameText->fetch("LAN:ErrorTimeout");
		case RET_GAME_FULL:
			return TheGameText->fetch("LAN:ErrorGameFull");
		case RET_DUPLICATE_NAME:
			return TheGameText->fetch("LAN:ErrorDuplicateName");
		case RET_CRC_MISMATCH:
			return TheGameText->fetch("LAN:ErrorCRCMismatch");
		case RET_GAME_STARTED:
			return TheGameText->fetch("LAN:ErrorGameStarted");
		case RET_GAME_EXISTS:
			return TheGameText->fetch("LAN:ErrorGameExists");
		case RET_GAME_GONE:
			return TheGameText->fetch("LAN:ErrorGameGone");
		case RET_BUSY:
			return TheGameText->fetch("LAN:ErrorBusy");
		case RET_SERIAL_DUPE:
			return TheGameText->fetch("WOL:ChatErrorSerialDup");
		default:
			return TheGameText->fetch("LAN:ErrorUnknown");
	}
}

// On functions are (generally) the result of network traffic

void LANAPI::OnAccept( UnsignedInt playerIP, Bool status )
{
	if( AmIHost() )
	{
		// Resolve by (IP, source port), not IP alone: two players behind one
		// NAT share a public IP, and an IP-only match would apply the second
		// player's accept to the first -- leaving the second stuck
		// un-accepted forever (the start button never unlocks).
		Int i = findSlotForSender(playerIP);
		if (i < 0)
		{
			i = MAX_SLOTS;
		}
		else
		{
			if(status)
				m_currentGame->getLANSlot(i)->setAccept();
			else
				m_currentGame->getLANSlot(i)->unAccept();
		}
		if (i != MAX_SLOTS )
		{
			RequestGameOptions( GenerateGameOptionsString(), false );
			lanUpdateSlotList();
		}
	}
	else
	{
		//i'm not the host but if the accept came from the host...
		if( m_currentGame->getIP(0) == playerIP )
		{
			UnicodeString text;
			text = TheGameText->fetch("GUI:HostWantsToStart");
			OnChat(L"SYSTEM", m_localIP, text, LANCHAT_SYSTEM);
		}
	}
}

void LANAPI::OnHasMap( UnsignedInt playerIP, Bool status )
{
	if( AmIHost() )
	{
		// Same-NAT disambiguation as OnAccept above.
		Int i = findSlotForSender(playerIP);
		if (i < 0)
		{
			i = MAX_SLOTS;
		}
		else if (m_currentGame->getLANSlot(i)->hasMap() == status)
		{
			// Joiners repeat their availability on the HELLO cadence in
			// direct-connect games; an unchanged report needs no UI refresh
			// and, more importantly, no repeated "player has no map" chat.
			return;
		}
		else
		{
			m_currentGame->getLANSlot(i)->setMapAvailability( status );
		}
		if (i != MAX_SLOTS )
		{
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
				mapDisplayName.format(L"%hs", m_currentGame->getMap().str());
				willTransfer = WouldMapTransfer(m_currentGame->getMap());
			}
			if (!status)
			{
				UnicodeString text;
				if (willTransfer)
					text.format(TheGameText->fetch("GUI:PlayerNoMapWillTransfer"), m_currentGame->getLANSlot(i)->getName().str(), mapDisplayName.str());
				else
					text.format(TheGameText->fetch("GUI:PlayerNoMap"), m_currentGame->getLANSlot(i)->getName().str(), mapDisplayName.str());
				OnChat(L"SYSTEM", m_localIP, text, LANCHAT_SYSTEM);
			}
			lanUpdateSlotList();
		}
	}
}

#if RTS_ZEROHOUR
// Have we already kicked off the intel fetch for the current start attempt?
// Set when the countdown timer starts, cleared when the game actually starts,
// so the timer + start pair only fires one worker.
static Bool s_lanIntelFired = FALSE;

// Build the roster from the current game and fire the (non-blocking) radarvan
// intel worker for the multiplayer load screen. Requires a clean two-team
// game; anything else (free-for-all, unteamed slots) just resets the intel
// state so the load screen shows the themed fallback instead.
static void startLanBattlefieldIntel(GameInfo *game)
{
	if (game == NULL)
	{
		RadarvanIntelReset();
		return;
	}

	std::vector<MapSummaryPlayer> roster;
	Int localTeam = 0;
	Int localRosterIdx = -1;
	Int localSlot = game->getLocalSlotNum();
	Int i;
	for (i = 0; i < MAX_SLOTS; ++i)
	{
		const GameSlot *slot = game->getConstSlot(i);
		if (!slot || !slot->isOccupied())
			continue;
		if (slot->getPlayerTemplate() == PLAYERTEMPLATE_OBSERVER)
			continue;
		MapSummaryPlayer entry;
		entry.name.translate(slot->getName());
		if (slot->isAI())
		{
			// AI names carry spaces ("Easy AI"); strip them so the server sees
			// a single token, matching how map_summary sends AI slots.
			AsciiString joined;
			for (const char *p = entry.name.str(); *p != '\0'; ++p)
			{
				if (*p != ' ')
					joined.concat(*p);
			}
			entry.name = joined;
		}
		if (entry.name.isEmpty())
			continue;
		entry.general = slot->getPlayerTemplate();
		// predict wants 1-based teams (0 = no team). getTeamNumber() is 0-based
		// with -1 = "no team", so a clean NvN maps to 1..N here.
		entry.team = slot->getTeamNumber() + 1;
		if (i == localSlot)
			localRosterIdx = (Int)roster.size();
		roster.push_back(entry);
	}

	// Require exactly two distinct teams, all teamed (no team-0 entries):
	// predict only handles two-team matches, and synergy/team_stats are team
	// concepts. Free-for-alls just get the fallback note.
	Int teamValues[MAX_SLOTS];
	Int distinct = 0;
	Bool hasUnteamed = FALSE;
	size_t r;
	for (r = 0; r < roster.size(); ++r)
	{
		Int t = roster[r].team;
		if (t <= 0) { hasUnteamed = TRUE; break; }
		Bool seen = FALSE;
		Int d;
		for (d = 0; d < distinct; ++d)
			if (teamValues[d] == t) { seen = TRUE; break; }
		if (!seen && distinct < MAX_SLOTS)
			teamValues[distinct++] = t;
	}
	// A 2-player game with no real teams is just a 1v1: synthesize opposing
	// teams so predict/synergy have two sides to work with. (3+ unteamed
	// players is a genuine free-for-all we can't forecast.)
	if (roster.size() == 2 && (hasUnteamed || distinct != 2))
	{
		roster[0].team = 1;
		roster[1].team = 2;
		hasUnteamed = FALSE;
		distinct = 2;
	}

	if (hasUnteamed || distinct != 2 || roster.size() < 2)
	{
		RadarvanIntelReset();
		return;
	}

	localTeam = (localRosterIdx >= 0 && localRosterIdx < (Int)roster.size())
		? roster[localRosterIdx].team : 0;

	// Map name: prefer the .map's display name (what players see), stripped of
	// the trailing " (N)" player-count suffix and lowercased, matching what
	// map_summary sends so the server keys on the same identifier.
	AsciiString mapName;
	if (TheMapCache)
	{
		const MapMetaData *md = TheMapCache->findMap(game->getMap());
		if (md && !md->m_displayName.isEmpty())
			mapName.translate(md->m_displayName);
	}
	const char *ms = mapName.str();
	Int mlen = mapName.getLength();
	if (mlen >= 4 && ms[mlen - 1] == ')')
	{
		Int mi = mlen - 2;
		while (mi > 0 && ms[mi] >= '0' && ms[mi] <= '9')
			--mi;
		if (mi >= 1 && ms[mi] == '(' && ms[mi - 1] == ' ' && mi != mlen - 2)
		{
			AsciiString trimmed;
			Int j;
			for (j = 0; j < mi - 1; ++j)
				trimmed.concat(ms[j]);
			mapName = trimmed;
		}
	}
	mapName.toLower();

	RadarvanIntelStart(TheGlobalData->m_predictUrl,
	                   TheGlobalData->m_teamStatsUrl,
	                   TheGlobalData->m_synergyUrl,
	                   mapName, localTeam, roster);
}
#endif // RTS_ZEROHOUR

void LANAPI::OnGameStartTimer( Int seconds )
{
	UnicodeString text;
	if (seconds == 1)
		text.format(TheGameText->fetch("LAN:GameStartTimerSingular"), seconds);
	else
		text.format(TheGameText->fetch("LAN:GameStartTimerPlural"), seconds);
	OnChat(L"SYSTEM", m_localIP, text, LANCHAT_SYSTEM);

#if RTS_ZEROHOUR
	// Countdown has begun on this client: fire the intel fetch now so it has
	// the whole countdown + load time to come back. Guard so the follow-up
	// OnGameStart() doesn't start a second worker.
	if (!s_lanIntelFired)
	{
		startLanBattlefieldIntel(m_currentGame);
		s_lanIntelFired = TRUE;
	}
#endif
}

void LANAPI::OnGameStart()
{
	//DEBUG_LOG(("Map is '%s', preview is '%s'", m_currentGame->getMap().str(), GetPreviewFromMap(m_currentGame->getMap()).str()));
	//DEBUG_LOG(("Map is '%s', INI is '%s'", m_currentGame->getMap().str(), GetINIFromMap(m_currentGame->getMap()).str()));

#if RTS_ZEROHOUR
	// Immediate start (countdown of 0) never fires OnGameStartTimer, so kick
	// the intel fetch here if it hasn't run yet. Either way, clear the guard
	// so the next game starts fresh.
	if (!s_lanIntelFired)
		startLanBattlefieldIntel(m_currentGame);
	s_lanIntelFired = FALSE;
#endif

	if (m_currentGame)
	{
		LANPreferences pref;
		AsciiString option;
		option.format("%d", m_currentGame->getLANSlot( m_currentGame->getLocalSlotNum() )->getPlayerTemplate());
		pref["PlayerTemplate"] = option;
		option.format("%d", m_currentGame->getLANSlot( m_currentGame->getLocalSlotNum() )->getColor());
		pref["Color"] = option;
		if (m_currentGame->amIHost())
    {
    	pref["Map"] = AsciiStringToQuotedPrintable(m_currentGame->getMap());
      pref.setSuperweaponRestricted( m_currentGame->getSuperweaponRestriction() > 0 );
      pref.setStartingCash( m_currentGame->getStartingCash() );
    }
		pref.write();

		m_isInLANMenu = FALSE;

		//m_currentGame->startGame(0);

		// Set up the game network
		DEBUG_ASSERTCRASH(TheNetwork == nullptr, ("For some reason TheNetwork isn't null at the start of this game.  Better look into that."));

		delete TheNetwork;
		TheNetwork = nullptr;

		// Time to initialize TheNetwork for this game.
		TheNetwork = NetworkInterface::createNetwork();
		TheNetwork->init();
		TheNetwork->setLocalAddress(m_localIP, 8088);
		TheNetwork->initTransport();

		TheNetwork->parseUserList(m_currentGame);

		if (TheGameLogic->isInGame())
			TheGameLogic->clearGameData();

		Bool filesOk = DoAnyMapTransfers(m_currentGame);

		// see if we really have the map.  if not, back out.
		TheMapCache->updateCache();
		if (!filesOk || TheMapCache->findMap(m_currentGame->getMap()) == nullptr)
		{
			DEBUG_LOG(("After transfer, we didn't really have the map.  Bailing..."));
			OnPlayerLeave(m_name);
			removeGame(m_currentGame);
			m_currentGame = nullptr;
			m_inLobby = TRUE;

			delete TheNetwork;
			TheNetwork = nullptr;

			OnChat(UnicodeString::TheEmptyString, 0, TheGameText->fetch("GUI:CouldNotTransferMap"), LANCHAT_SYSTEM);
			return;
		}

		m_currentGame->startGame(0);

		// shutdown the top, but do not pop it off the stack
		//TheShell->hideShell();
		// setup the Global Data with the Map and Seed
		TheWritableGlobalData->m_pendingFile = m_currentGame->getMap();

		// send a message to the logic for a new game
		GameMessage *msg = TheMessageStream->appendMessage( GameMessage::MSG_NEW_GAME );
		msg->appendIntegerArgument(GAME_LAN);

		TheWritableGlobalData->m_useFpsLimit = false;

		// Set the seeds
		InitRandom( m_currentGame->getSeed() );
		DEBUG_LOG(("InitRandom( %d )", m_currentGame->getSeed()));

		// Open the observer-streaming TCP listen socket if we're the host.
		// Joiners who try to enter after this point are offered a "Watch as
		// observer" prompt instead of a hard rejection.
		if (m_currentGame->amIHost())
		{
			startObserverHost();
		}
	}
}

void LANAPI::OnGameOptions( UnsignedInt playerIP, Int playerSlot, AsciiString options )
{
	if (!m_currentGame)
		return;

	if (m_currentGame->getIP(playerSlot) != playerIP)
		return; // He's not in our game?!?


	if (m_currentGame->isGameInProgress())
		return; // we don't want to process any game options while in game.

	if (playerSlot == 0 && !m_currentGame->amIHost())
	{
		m_currentGame->setLastHeard(timeGetTime());
		AsciiString oldOptions = GameInfoToAsciiString(m_currentGame); // save these off for if we get booted

		// Remember our local slot index BEFORE the parse, because the parse
		// overwrites slot IPs with the host's LAN view -- after that,
		// getLocalSlotNum() (which matches by IP) would return -1.
		Int preParseLocalSlotNum = m_currentGame->getLocalSlotNum();

		if(ParseGameOptionsString(m_currentGame,options))
		{
			// Restore NAT-aware slot IPs and ports that the parse just
			// clobbered with the host's LAN-view addresses. For direct-
			// connect games the host's own slot needs to be its externally-
			// routable IP (where this packet came from) so subsequent
			// matching/routing works, and our own slot needs to be
			// m_localIP so getLocalSlotNum() and isLocalPlayer() keep
			// identifying us correctly. The game-data port on slot[0] must
			// be the host's punched external port (ConnectionManager reads
			// slot.getPort() when sending in-game), not the announced
			// NETWORK_BASE_PORT_NUMBER which is the host's local 8088.
			if (m_currentGame->getIsDirectConnect())
			{
				m_currentGame->getLANSlot(0)->setIP(playerIP);
				if (m_directConnectRemoteGamePort != 0)
					m_currentGame->getLANSlot(0)->setPort(m_directConnectRemoteGamePort);
				if (preParseLocalSlotNum > 0)
				{
					m_currentGame->getLANSlot(preParseLocalSlotNum)->setIP(m_localIP);
				}
				else
				{
					// We didn't know our own slot before the parse (a prior
					// parse already clobbered it, or this is the first options
					// string after joining). IP matching can't recover -- the
					// host's string carries our NAT-external address -- so fall
					// back to our unique lobby name to reclaim the slot.
					for (Int slotIdx = 1; slotIdx < MAX_SLOTS; ++slotIdx)
					{
						LANGameSlot *namedSlot = m_currentGame->getLANSlot(slotIdx);
						if (namedSlot && namedSlot->isHuman() && namedSlot->getName().compare(m_name) == 0)
						{
							namedSlot->setIP(m_localIP);
							break;
						}
					}
				}

				// The parse ran while every slot still held the host's view of
				// the addresses, so getLocalSlotNum() was -1 inside it and both
				// the local map-availability recheck (GameInfo::setMapCRC) and
				// the changed-map report (ParseGameOptionsString ->
				// RequestHasMap) silently skipped. Redo them against the
				// restored slot; otherwise the host keeps its default
				// hasMap=TRUE for us, never runs the launch-time transfer, and
				// starts a game we cannot load (2026-08-05 Whispering Woods).
				Int restoredSlot = m_currentGame->getLocalSlotNum();
				if (restoredSlot >= 0)
				{
					Bool reportedHasMap = m_currentGame->getConstSlot(restoredSlot)->hasMap();
					m_currentGame->setMapCRC(m_currentGame->getMapCRC()); // recompute availability
					if (m_currentGame->getConstSlot(restoredSlot)->hasMap() != reportedHasMap)
					{
						RequestHasMap();
					}
				}
			}

			// Peer-side cncstats download: if the host just advertised a
			// map CRC we don't have locally, fetch it from the cncstats
			// server now so the lobby preview can show immediately
			// (instead of waiting for the launch-time P2P transfer and
			// requiring a game restart for the preview to load).
			//
			// The fetch runs on a worker thread: we are inside the network
			// callback here, on the same thread that services the LAN
			// heartbeat, so a slow CDN would freeze the lobby and get us
			// dropped from the game. updateMapDownload() installs the map
			// and flips the availability bit once the bytes are in.
			//
			// Hooks are set by GeneralsMD; null in classic Generals builds.
			// On failure we fall through to the legacy at-launch P2P
			// transfer in FileTransfer::DoAnyMapTransfers.
			Int localSlot = m_currentGame->getLocalSlotNum();
			if (TheMapDownloadStartHook != nullptr
				&& !m_mapDownloadPending
				&& localSlot >= 0
				&& !m_currentGame->getConstSlot(localSlot)->hasMap()
				&& m_currentGame->getMapCRC() != 0
				&& m_currentGame->getMapCRC() != m_mapDownloadFailedCRC)
			{
				UnsignedInt crc = m_currentGame->getMapCRC();
				if (TheMapDownloadStartHook(m_currentGame->getMap(), crc,
					m_currentGame->getMapContentsMask()))
				{
					m_mapDownloadPending = TRUE;
					m_mapDownloadCRC = crc;
					UnicodeString msg;
					msg.format(L"Downloading map from server...");
					OnChat(L"SYSTEM", m_localIP, msg, LANCHAT_SYSTEM);
				}
				else
				{
					// Nothing started (no download URL, or one already in
					// flight). Don't retry this CRC on every options packet.
					m_mapDownloadFailedCRC = crc;
				}
			}

			lanUpdateSlotList();
			updateGameOptions();
		}
		Bool booted = true;
		for(Int player = 1; player< MAX_SLOTS; player++)
		{
			if(m_currentGame->getIP(player) == m_localIP)
			{
				booted = false;
				break;
			}
		}
		if(booted)
		{
			// restore the options with us in so we can save prefs
			ParseGameOptionsString(m_currentGame, oldOptions);
			OnPlayerLeave(m_name);
		}

	}
	else
	{
		// Check for user/host updates
		{
			AsciiString key;
			AsciiString munkee = options;
			munkee.nextToken(&key, "=");
			//DEBUG_LOG(("GameOpt request: key=%s, val=%s from player %d", key.str(), munkee.str(), playerSlot));

			LANGameSlot *slot = m_currentGame->getLANSlot(playerSlot);
			if (!slot)
				return;

			if (key == "User")
			{
				slot->setLogin(munkee.str()+1);
				return;
			}
			else if (key == "Host")
			{
				slot->setHost(munkee.str()+1);
				return;
			}
		}

		// Parse player requests (side, color, etc)
		if( AmIHost() && m_localIP != playerIP)
		{
			if (options.compare("HELLO") == 0)
			{
				m_currentGame->setPlayerLastHeard(playerSlot, timeGetTime());
				// Direct-connect diagnostic: positive confirmation that the
				// joiner's keepalive arrived and was credited. Throttled.
				if (m_currentGame->getIsDirectConnect())
				{
					static UnsignedInt s_lastHelloCreditLogMs = 0;
					UnsignedInt nowLog = timeGetTime();
					if (nowLog - s_lastHelloCreditLogMs > 10000)
					{
						s_lastHelloCreditLogMs = nowLog;
						ReleaseLog("LAN dc HELLO credited slot=%d ip=%d.%d.%d.%d",
							playerSlot, PRINTF_IP_AS_4_INTS(playerIP));
					}
				}
			}
			else
			{
				m_currentGame->setPlayerLastHeard(playerSlot, timeGetTime());
				Bool change = false;
				Bool shouldUnaccept = false;
				AsciiString key;
				options.nextToken(&key, "=");
				Int val = atoi(options.str()+1);
				DEBUG_LOG(("GameOpt request: key=%s, val=%s from player %d", key.str(), options.str(), playerSlot));

				LANGameSlot *slot = m_currentGame->getLANSlot(playerSlot);
				if (!slot)
					return;

				if (key == "Color")
				{
					if (val >= -1 && val < TheMultiplayerSettings->getNumColors() && val != slot->getColor() && slot->getPlayerTemplate() != PLAYERTEMPLATE_OBSERVER)
					{
						Bool colorAvailable = TRUE;
						if(val != -1 )
						{
							for(Int i=0; i <MAX_SLOTS; i++)
							{
								LANGameSlot *checkSlot = m_currentGame->getLANSlot(i);
								if(val == checkSlot->getColor() && slot != checkSlot)
								{
									colorAvailable = FALSE;
									break;
								}
							}
						}
						if(colorAvailable)
							slot->setColor(val);
						change = true;
					}
					else
					{
						DEBUG_LOG(("Rejecting invalid color %d", val));
					}
				}
				else if (key == "PlayerTemplate")
				{
					// While enforce-random is set, only Random (and Observer) are
					// allowed; concrete faction requests (including the preferred
					// faction a joiner sends right after joining) are rejected.
					if (m_currentGame->getEnforceRandom() && val >= 0)
					{
						DEBUG_LOG(("Rejecting PlayerTemplate %d because enforce random is set", val));
					}
					else if (val >= PLAYERTEMPLATE_MIN && val < ThePlayerTemplateStore->getPlayerTemplateCount() && val != slot->getPlayerTemplate())
					{
						slot->setPlayerTemplate(val);
						if (val == PLAYERTEMPLATE_OBSERVER)
						{
							slot->setColor(-1);
							slot->setStartPos(-1);
							slot->setTeamNumber(-1);
						}
						change = true;
						shouldUnaccept = true;
					}
					else
					{
						DEBUG_LOG(("Rejecting invalid PlayerTemplate %d", val));
					}
				}
				else if (key == "StartPos" && slot->getPlayerTemplate() != PLAYERTEMPLATE_OBSERVER)
				{

					if (val >= -1 && val < MAX_SLOTS && val != slot->getStartPos())
					{
						Bool startPosAvailable = TRUE;
						if(val != -1)
							for(Int i=0; i <MAX_SLOTS; i++)
							{
								LANGameSlot *checkSlot = m_currentGame->getLANSlot(i);
								if(val == checkSlot->getStartPos() && slot != checkSlot)
								{
									startPosAvailable = FALSE;
									break;
								}
							}
						if(startPosAvailable)
							slot->setStartPos(val);
						change = true;
						shouldUnaccept = true;
					}
					else
					{
						DEBUG_LOG(("Rejecting invalid startPos %d", val));
					}
				}
				else if (key == "Team")
				{
					// While enforce-random is set, only a random team (-1) is allowed.
					if (m_currentGame->getEnforceRandom() && val != -1)
					{
						DEBUG_LOG(("Rejecting team %d because enforce random is set", val));
					}
					else if (val >= -1 && val < MAX_SLOTS/2 && val != slot->getTeamNumber() && slot->getPlayerTemplate() != PLAYERTEMPLATE_OBSERVER)
					{
						slot->setTeamNumber(val);
						change = true;
						shouldUnaccept = true;
					}
					else
					{
						DEBUG_LOG(("Rejecting invalid team %d", val));
					}
				}
				else if (key == "NAT")
				{
					if ((val >= FirewallHelperClass::FIREWALL_TYPE_SIMPLE) &&
							(val <= FirewallHelperClass::FIREWALL_TYPE_DESTINATION_PORT_DELTA))
					{
						slot->setNATBehavior((FirewallHelperClass::FirewallBehaviorType)val);
						DEBUG_LOG(("NAT behavior set to %d for player %d", val, playerSlot));
						change = true;
					}
					else
					{
						DEBUG_LOG(("Rejecting invalid NAT behavior %d", (Int)val));
					}
				}

				if (change)
				{
					if (shouldUnaccept)
						m_currentGame->resetAccepted();
					RequestGameOptions(GenerateGameOptionsString(), true);
					lanUpdateSlotList();
					DEBUG_LOG(("Slot value is color=%d, PlayerTemplate=%d, startPos=%d, team=%d",
						slot->getColor(), slot->getPlayerTemplate(), slot->getStartPos(), slot->getTeamNumber()));
					DEBUG_LOG(("Slot list updated to %s", GenerateGameOptionsString().str()));
				}
			}
		}
	}
}

// Completion half of the peer-side background map download kicked off in
// OnGameOptions. The poll hook does the disk install and the MapCache refresh,
// so it has to run here on the main thread rather than on the worker.
void LANAPI::updateMapDownload()
{
	if (!m_mapDownloadPending || TheMapDownloadPollHook == nullptr)
		return;

	MapDownloadStatus status = TheMapDownloadPollHook();
	if (status == MAPDOWNLOAD_PENDING)
		return;

	m_mapDownloadPending = FALSE;

	// We left the game while the download was running: the result is moot.
	if (m_currentGame == nullptr)
		return;

	UnicodeString msg;
	if (status == MAPDOWNLOAD_INSTALLED)
	{
		// Re-apply the host's CRC so GameInfo's local mapAvailability bit flips
		// from false to true now that the file exists on disk and MapCache
		// knows about it. setMapCRC re-runs the cache lookup.
		m_currentGame->setMapCRC(m_currentGame->getMapCRC());
		RequestHasMap();
		lanUpdateSlotList();
		updateGameOptions();
		msg.format(L"Map downloaded.");
	}
	else
	{
		// Don't hammer the server for this CRC again on every options packet.
		// The launch-time P2P transfer from the host is still available as a
		// fallback, which is what the message is telling the user.
		m_mapDownloadFailedCRC = m_mapDownloadCRC;
		msg.format(L"Map download failed; the host will send it when the game starts.");
	}
	OnChat(L"SYSTEM", m_localIP, msg, LANCHAT_SYSTEM);
}


/*
void LANAPI::OnSlotList( ReturnType ret, LANGameInfo *theGame )
{
	if (!theGame || theGame != m_currentGame)
		return;

	Bool foundMe = false;
	for (int player = 0; player < MAX_SLOTS; ++player)
	{
		if (m_currentGame->getIP(player) == m_localIP)
		{
			foundMe = true;
			break;
		}
	}
	if (!foundMe)
	{
		// I've been kicked - back to the lobby for me!
		// We're counting on the fact that OnPlayerLeave winds up calling reset on TheLAN.
		OnPlayerLeave(m_name);
		return;
	}

	lanUpdateSlotList();
}
*/
void LANAPI::OnPlayerJoin( Int slot, UnicodeString playerName )
{
	if (m_currentGame && m_currentGame->getIP(0) == m_localIP)
	{
		// Someone New Joined.. lets reset the accepts
		m_currentGame->resetAccepted();

		// Send out the game options
		RequestGameOptions(GenerateGameOptionsString(), true);

		// Direct-connect (online) games: joiners arrive through the
		// coordinator with no LAN-lobby presence beforehand, so give the
		// host an explicit heads-up in chat (on LAN you watch people walk
		// in from the lobby; online they just materialize in a slot).
		if (m_currentGame->getIsDirectConnect())
		{
			UnicodeString msg;
			msg.format(L"%s has joined the game.", playerName.str());
			OnChat(L"", 0, msg, LANCHAT_SYSTEM);
		}
	}

	lanUpdateSlotList();
}

// Pending observer-prompt state stashed for the MessageBoxYesNo callbacks,
// which are plain function pointers and have no userData parameter. Set by
// handleJoinDeny in LANAPIhandlers.cpp (where the join-deny payload lives),
// consumed here in OnGameJoin to decide whether to show the observer prompt.
UnsignedInt   s_pendingObserveHostIP   = 0;
UnsignedShort s_pendingObservePort     = 0;

static void lanAcceptObserveCallback()
{
	if (TheLAN && s_pendingObservePort != 0)
	{
		((LANAPI *)TheLAN)->RequestObserve(s_pendingObserveHostIP, s_pendingObservePort);
	}
	s_pendingObserveHostIP = 0;
	s_pendingObservePort   = 0;
}

static void lanDeclineObserveCallback()
{
	s_pendingObserveHostIP = 0;
	s_pendingObservePort   = 0;
}

void LANAPI::OnGameJoin( ReturnType ret, LANGameInfo *theGame )
{
	if (ret == RET_OK)
	{
		LANbuttonPushed = true;
		TheShell->push( "Menus/LanGameOptionsMenu.wnd" );
		//lanUpdateSlotList();

		LANPreferences pref;
		AsciiString options;
		options.format("PlayerTemplate=%d", pref.getPreferredFaction());
		RequestGameOptions(options, true);
		options.format("Color=%d", pref.getPreferredColor());
		RequestGameOptions(options, true);
		options.format("User=%s", m_userName.str());
		RequestGameOptions( options, true );
		options.format("Host=%s", m_hostName.str());
		RequestGameOptions( options, true );
		options.format("NAT=%d", FirewallHelperClass::FIREWALL_TYPE_SIMPLE); // BGC: This is a LAN game, so there is no firewall.
		RequestGameOptions( options, true );
	}
	else if (ret == RET_GAME_STARTED && s_pendingObservePort != 0)
	{
		// theGame may be null here because handleRequestJoin's deny payload
		// for RET_GAME_STARTED doesn't set the game name; the joiner only
		// needs the host IP + observer port, both already stashed by
		// handleJoinDeny.
		(void)theGame;
		// Game's already started. Offer to spectate via the observer stream.
		// The host advertised the TCP observer port in the JOIN_DENY payload;
		// handleJoinDeny stashed it into s_pendingObservePort for us.
		UnicodeString title, body;
		title = TheGameText->fetch("LAN:JoinFailed");
		body  = TheGameText->fetch("LAN:OfferObserveBody");
		if (body.isEmpty())
		{
			// Fallback if the localization key hasn't been added yet so we
			// still show something useful on dev builds.
			body = L"This game is already in progress. Watch it as an observer?";
		}
		MessageBoxYesNo(title, body, lanAcceptObserveCallback, lanDeclineObserveCallback);
	}
	else if (ret != RET_BUSY)
	{
		/// @todo: re-enable lobby controls?  Error msgs?
		UnicodeString title, body;
		title = TheGameText->fetch("LAN:JoinFailed");
		body = getErrorStringFromReturnType(ret);
		MessageBoxOk(title, body, nullptr);
		s_pendingObserveHostIP = 0;
		s_pendingObservePort   = 0;
	}
}

void LANAPI::OnHostLeave()
{
	DEBUG_ASSERTCRASH(!m_inLobby && m_currentGame, ("Game info is gone!"));
	if (m_inLobby || !m_currentGame)
		return;
	LANbuttonPushed = true;
	DEBUG_LOG(("Host left - popping to lobby"));
	TheShell->pop();
}

void LANAPI::OnPlayerLeave( UnicodeString player )
{
	DEBUG_ASSERTCRASH(!m_inLobby && m_currentGame, ("Game info is gone!"));
	if (m_inLobby || !m_currentGame || m_currentGame->isGameInProgress())
		return;

	if (m_name.compare(player) == 0)
	{
		// We're leaving.  Save options and Pop the shell up a screen.
		//DEBUG_CRASH(("Slot is %d", m_currentGame->getLocalSlotNum()));
		if (m_currentGame && m_currentGame->isInGame() && m_currentGame->getLocalSlotNum() >= 0)
		{
			LANPreferences pref;
			AsciiString option;
			option.format("%d", m_currentGame->getLANSlot( m_currentGame->getLocalSlotNum() )->getPlayerTemplate());
			pref["PlayerTemplate"] = option;
			option.format("%d", m_currentGame->getLANSlot( m_currentGame->getLocalSlotNum() )->getColor());
			pref["Color"] = option;
			if (m_currentGame->amIHost())
				pref["Map"] = AsciiStringToQuotedPrintable(m_currentGame->getMap());
			pref.write();
		}
		LANbuttonPushed = true;
		DEBUG_LOG(("OnPlayerLeave says we're leaving!  pop away!"));
		TheShell->pop();
	}
	else
	{
		if (m_currentGame && m_currentGame->getIP(0) == m_localIP)
		{
			// Force a new slotlist send
			m_lastResendTime = 0;

			lanUpdateSlotList();
			RequestGameOptions( GenerateGameOptionsString(), true );

		}
	}
}

void LANAPI::OnGameList( LANGameInfo *gameList )
{

	if (m_inLobby)
	{
		LANDisplayGameList(listboxGames, gameList);
	}
}

void LANAPI::OnGameCreate( ReturnType ret )
{
	if (ret == RET_OK)
	{

		LANbuttonPushed = true;
		TheShell->push( "Menus/LanGameOptionsMenu.wnd" );

		RequestLobbyLeave( false );
		//RequestGameAnnounce(); // can't do this here, since we don't have a map set
	}
	else
	{
		if(m_inLobby)
		{
			switch( ret )
			{
			case RET_GAME_EXISTS:
				GadgetListBoxAddEntryText(listboxChatWindow, TheGameText->fetch("LAN:ErrorGameExists"), chatSystemColor, -1, -1);
				break;
			case RET_BUSY:
				GadgetListBoxAddEntryText(listboxChatWindow, TheGameText->fetch("LAN:ErrorBusy"), chatSystemColor, -1, -1);
				break;
			default:
				GadgetListBoxAddEntryText(listboxChatWindow, TheGameText->fetch("LAN:ErrorUnknown"), chatSystemColor, -1, -1);
			}
		}
	}

}

void LANAPI::OnPlayerList( LANPlayer *playerList )
{
	if (m_inLobby)
	{

		UnsignedInt selectedIP = 0;
		Int selectedIndex = -1;
		Int indexToSelect = -1;
		GadgetListBoxGetSelected(listboxPlayers, &selectedIndex);

		if (selectedIndex != -1 )
			selectedIP = (UnsignedInt) GadgetListBoxGetItemData(listboxPlayers, selectedIndex, 0);

		GadgetListBoxReset(listboxPlayers);

		LANPlayer *player = m_lobbyPlayers;
		while (player)
		{
			Int addedIndex = GadgetListBoxAddEntryText(listboxPlayers, player->getName(), playerColor, -1, -1);
			GadgetListBoxSetItemData(listboxPlayers, (void *)player->getIP(),addedIndex, 0 );

			if (selectedIP == player->getIP())
				indexToSelect = addedIndex;

			player = player->getNext();
		}

		if (indexToSelect >= 0)
			GadgetListBoxSetSelected(listboxPlayers, indexToSelect);
	}
}

void LANAPI::OnNameChange( UnsignedInt IP, UnicodeString newName )
{
	OnPlayerList(m_lobbyPlayers);
}

void LANAPI::OnInActive(UnsignedInt IP) {

}

void LANAPI::OnChat( UnicodeString player, UnsignedInt ip, UnicodeString message, ChatType format )
{
	GameWindow *chatWindow = nullptr;

	if (m_inLobby)
	{
		chatWindow = listboxChatWindow;
	}
	else if( m_currentGame && m_currentGame->isGameInProgress() && TheShell->isShellActive())
	{
		chatWindow = listboxChatWindowScoreScreen;
	}
	else if( m_currentGame && !m_currentGame->isGameInProgress())
	{
		chatWindow = listboxChatWindowLanGame;
	}
	if (chatWindow == nullptr)
		return;
	Int index = -1;
	UnicodeString unicodeChat;
	switch (format)
	{
		case LANAPIInterface::LANCHAT_SYSTEM:
			unicodeChat = message;
			index =GadgetListBoxAddEntryText(chatWindow, unicodeChat, chatSystemColor, -1, -1);
			break;
		case LANAPIInterface::LANCHAT_EMOTE:
			unicodeChat = player;
			unicodeChat.concat(L' ');
			unicodeChat.concat(message);
			if (ip == m_localIP)
				index =GadgetListBoxAddEntryText(chatWindow, unicodeChat, chatLocalActionColor, -1, -1);
			else
				index =GadgetListBoxAddEntryText(chatWindow, unicodeChat, chatActionColor, -1, -1);
			break;
		case LANAPIInterface::LANCHAT_NORMAL:
		default:
		{
			// Do the language filtering.
			TheLanguageFilter->filterLine(message);

			Color chatColor = GameMakeColor(255, 255, 255, 255);
			if (m_currentGame)
			{
				Int slotNum = m_currentGame->getSlotNum(player);
				// it'll be -1 if its invalid.
				if (slotNum >= 0) {
					GameSlot *gs = m_currentGame->getSlot(slotNum);
					if (gs) {
						Int colorIndex = gs->getColor();
						MultiplayerColorDefinition *def = TheMultiplayerSettings->getColor(colorIndex);
						if (def)
							chatColor = GameMakeColorReadable(def->getColor());
					}
				}
			}

			unicodeChat = L"[";
			unicodeChat.concat(player);
			unicodeChat.concat(L"] ");
			unicodeChat.concat(message);
			if (ip == m_localIP)
				index =GadgetListBoxAddEntryText(chatWindow, unicodeChat, chatColor, -1, -1);
			else
				index =GadgetListBoxAddEntryText(chatWindow, unicodeChat, chatColor, -1, -1);
			break;
		}
	}
	GadgetListBoxSetItemData(chatWindow, (void *)-1, index);
}
