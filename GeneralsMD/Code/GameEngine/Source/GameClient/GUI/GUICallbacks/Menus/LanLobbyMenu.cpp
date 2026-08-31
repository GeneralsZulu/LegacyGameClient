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
// FILE: LanLobbyMenu.cpp
// Author: Chris Huybregts, October 2001
// Description: Lan Lobby Menu
///////////////////////////////////////////////////////////////////////////////////////

// INCLUDES ///////////////////////////////////////////////////////////////////////////////////////
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Lib/BaseType.h"
#include "Common/crc.h"
#include "Common/GameEngine.h"
#include "Common/version.h"
#include "Common/GlobalData.h"
#include "Common/MultiplayerSettings.h"
#include "Common/NameKeyGenerator.h"
#include "Common/Player.h"
#include "Common/PlayerTemplate.h"
#include "Common/QuotedPrintable.h"
#include "Common/ReleaseLog.h"
#include "Common/StatsUploader.h"
#include "Common/OptionPreferences.h"
#include "GameClient/AnimateWindowManager.h"
#include "GameClient/ClientInstance.h"
#include "GameClient/GameText.h"
#include "GameClient/MapUtil.h"
#include "GameClient/Mouse.h"
#include "GameClient/WindowLayout.h"
#include "GameClient/Gadget.h"
#include "GameClient/Shell.h"
#include "GameClient/ShellHooks.h"
#include "GameClient/KeyDefs.h"
#include "GameClient/GameInfoWindow.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/GadgetListBox.h"
#include "GameClient/GadgetTextEntry.h"
#include "GameClient/MessageBox.h"
#include "GameClient/GameWindowTransitions.h"
#include "GameLogic/GameLogic.h"
#include "GameNetwork/IPEnumeration.h"
#include "GameNetwork/LANAPICallbacks.h"
#include "GameNetwork/LANGameInfo.h"
#include "GameNetwork/NetworkDefs.h"
#include "GameNetwork/OnlineCoordinatorAPI.h"

Bool LANisShuttingDown = false;
Bool LANbuttonPushed = false;
Bool LANSocketErrorDetected = FALSE;
char *LANnextScreen = nullptr;

static Int	initialGadgetDelay = 2;
static Bool justEntered = FALSE;

// -- online-coordinator mode -------------------------------------------------
// When TRUE, the LAN lobby is reused as a UI for the cncstats coordinator
// instead of broadcasting on the LAN. The mode is set by MainMenu (the
// "Online" button) before the menu is pushed, and cleared on shutdown.
static Bool                     s_useCoordinator    = FALSE;
static OnlineCoordinatorAPI*    s_coord             = nullptr;
static UnsignedInt              s_coordLastListMs   = 0;
static AsciiString              s_coordPendingHostName;  // game name to host once READY
static AsciiString              s_coordPendingJoinID;    // game id to join once READY
static AsciiString              s_coordCurrentNick;      // sent in HELLO at connect time
static std::vector<AsciiString> s_coordListedIDs;        // parallel to listbox rows
static Bool                     s_coordHandoffDone  = FALSE;
// LAN-like join resilience: remember the last join target so a failed
// attempt (usually a punch timeout) can transparently reconnect and retry
// instead of parking the lobby on an error.
static AsciiString              s_coordLastJoinID;
static Int                      s_coordJoinRetries  = 0;
static const Int                COORD_JOIN_MAX_RETRIES = 2;
// Observe-in-progress: game id whose observe request should be sent once
// the coordinator connection is READY (mirrors s_coordPendingJoinID).
static AsciiString              s_coordPendingObserveID;
// Failure diagnostics: a host/join that never becomes a game produces no
// end-of-match telemetry, so the ReleaseLog is shipped from here instead.
// Bounded per lobby visit so a player stuck in a retry loop can't spam the
// server (and so the log we do get is the first, most informative one).
static Int                      s_coordFailureUploads = 0;
static const Int                COORD_MAX_FAILURE_UPLOADS = 4;
// Time requestHost() was sent, for the listing-ack watchdog below. 0 = no
// host request outstanding.
static UnsignedInt              s_coordHostRequestMs = 0;

static const char* COORD_HOST_DEFAULT = "cncstats.computersrfun.org";
static const UnsignedShort COORD_TCP_PORT_DEFAULT = 27500;
static const UnsignedInt   COORD_LIST_REFRESH_MS  = 5000;
// How long to wait for the coordinator to ack a host request before calling
// the attempt failed. The ack is one TCP round trip; anything approaching
// this means the signaling channel is not working, and without the watchdog
// the lobby just sits on "Waiting for players to join..." forever.
static const UnsignedInt   COORD_HOST_ACK_TIMEOUT_MS = 15000;

void LanLobbyMenuSetUseCoordinator( Bool enable )
{
	s_useCoordinator = enable;
}

// Cross-menu accessor: LanGameOptionsMenu calls this each frame so the
// coordinator TCP signaling stays pumped after the handoff. The host uses it
// to receive additional joiners; joiners use it to receive "peer" role mesh
// notifications about the game's other guests. Returns NULL when no coord
// session is alive (regular LAN games, or after teardown).
OnlineCoordinatorAPI* LanLobbyMenuGetCoordinator()
{
	return s_coord;
}

// Cross-menu teardown: LanGameOptionsMenu calls this when a player backs out
// of the lobby (a host's game start instead RELEASES the session into
// LANAPI, see LanLobbyMenuReleaseCoordinator). Idempotent.
void LanLobbyMenuShutdownCoordinator()
{
	if (s_coord)
	{
		ReleaseLog("Coordinator teardown: LanLobbyMenuShutdownCoordinator");
		s_coord->disconnect();
		delete s_coord;
		s_coord = nullptr;
	}
	// Backing out abandons the game, so let go of the punched in-game
	// socket too. Left behind it would keep holding NETWORK_BASE_PORT_NUMBER
	// and, worse, ConnectionManager would happily adopt this dead session's
	// socket for the NEXT game the player starts -- including a plain LAN
	// or skirmish game that has nothing to do with the coordinator.
	if (OnlineCoordinatorAPI::hasStashedGameSocket())
	{
		ReleaseLog("Coordinator: releasing stashed game socket (abandoned the lobby)");
		OnlineCoordinatorAPI::discardStashedGameSocket();
	}
}

// Joiner-side teardown at game start: the TCP signaling session is done (the
// match is starting, no more mesh notifications matter), but the stashed
// game socket is about to be adopted by ConnectionManager, so it must NOT be
// discarded here. Hosts never hit this: their session was already released
// into LANAPI by the Start-press path.
void LanLobbyMenuShutdownCoordinatorKeepStash()
{
	if (s_coord)
	{
		ReleaseLog("Coordinator teardown: LanLobbyMenuShutdownCoordinatorKeepStash (game starting)");
		s_coord->disconnect();
		delete s_coord;
		s_coord = nullptr;
	}
}

// Ownership transfer at game start: the host keeps its coordinator TCP
// session alive through the match so viewers can request to observe the
// in-progress game (relayed through the coordinator). The caller (game
// start path) hands the returned instance to TheLAN->adoptCoordinator();
// after this, no lobby teardown path touches it.
OnlineCoordinatorAPI* LanLobbyMenuReleaseCoordinator()
{
	OnlineCoordinatorAPI* c = s_coord;
	s_coord = nullptr;
	return c;
}

// Forward declarations for the coordinator helpers; the definitions live
// just above LanLobbyMenuUpdate further down in this file.
static void connectCoordinatorIfNeeded();
static void pumpCoordinator();
static void coordinatorPostStatus(const char* msg);



LANPreferences::LANPreferences()
{
	loadFromIniFile();
}

LANPreferences::~LANPreferences()
{
}

Bool LANPreferences::loadFromIniFile()
{
	if (rts::ClientInstance::getInstanceId() > 1u)
	{
		AsciiString fname;
		fname.format("Network_Instance%.2u.ini", rts::ClientInstance::getInstanceId());
		return load(fname);
	}

	return load("Network.ini");
}

UnicodeString LANPreferences::getUserName()
{
	UnicodeString ret;

	LANPreferences::const_iterator it = find("UserName");
	if (it != end())
	{
		// Found an user name. Use it if valid.
		ret = QuotedPrintableToUnicodeString(it->second);
		ret.trim();
		if (!ret.isEmpty())
		{
			return ret;
		}
	}

	if (rts::ClientInstance::getInstanceId() > 1u)
	{
		// TheSuperHackers @feature Use the instance id as default user name
		// to avoid duplicate names for multiple client instances.
		ret.format(L"Instance%.2d", rts::ClientInstance::getInstanceId());
		return ret;
	}

	// Use machine name as default user name.
	IPEnumeration IPs;
	ret.translate(IPs.getMachineName());
	return ret;
}

Int LANPreferences::getPreferredColor()
{
	Int ret;
	LANPreferences::const_iterator it = find("Color");
	if (it == end())
	{
		return -1;
	}

	ret = atoi(it->second.str());
	if (ret < -1 || ret >= TheMultiplayerSettings->getNumColors())
		ret = -1;

	return ret;
}

Int LANPreferences::getPreferredFaction()
{
	Int ret;
	LANPreferences::const_iterator it = find("PlayerTemplate");
	if (it == end())
	{
		return PLAYERTEMPLATE_RANDOM;
	}

	ret = atoi(it->second.str());
	if (ret == PLAYERTEMPLATE_OBSERVER || ret < PLAYERTEMPLATE_MIN || ret >= ThePlayerTemplateStore->getPlayerTemplateCount())
		ret = PLAYERTEMPLATE_RANDOM;

	if (ret >= 0)
	{
		const PlayerTemplate *fac = ThePlayerTemplateStore->getNthPlayerTemplate(ret);
		if (!fac)
			ret = PLAYERTEMPLATE_RANDOM;
		else if (fac->getStartingBuilding().isEmpty())
			ret = PLAYERTEMPLATE_RANDOM;
	}

	return ret;
}

Bool LANPreferences::usesSystemMapDir()
{
	OptionPreferences::const_iterator it = find("UseSystemMapDir");
	if (it == end())
		return TRUE;

	if (stricmp(it->second.str(), "yes") == 0) {
		return TRUE;
	}
	return FALSE;
}

// When no (valid) map preference exists, prefer the first OFFICIAL
// multiplayer map. getDefaultMap(TRUE) returns the alphabetically first
// multiplayer map of any kind, and with user maps installed that tends to be
// something like "(3 Letter names required).map" (punctuation sorts before
// letters), which is a terrible first impression for a fresh lobby.
static AsciiString getDefaultLanMap()
{
	AsciiString ret = getDefaultOfficialMap();
	if (ret.isEmpty())
		ret = getDefaultMap(TRUE);
	return ret;
}

AsciiString LANPreferences::getPreferredMap()
{
	AsciiString ret;
	LANPreferences::const_iterator it = find("Map");
	if (it == end())
	{
		ret = getDefaultLanMap();
		return ret;
	}

	ret = QuotedPrintableToAsciiString(it->second);
	ret.trim();
	if (ret.isEmpty() || !isValidMap(ret, TRUE))
	{
		ret = getDefaultLanMap();
		return ret;
	}

	return ret;
}

Int LANPreferences::getNumRemoteIPs()
{
	Int ret;
	LANPreferences::const_iterator it = find("NumRemoteIPs");
	if (it == end())
	{
		ret = 0;
		return ret;
	}

	ret = atoi(it->second.str());
	return ret;
}

UnicodeString LANPreferences::getRemoteIPEntry(Int i)
{
	UnicodeString ret;
	AsciiString key;
	key.format("RemoteIP%d", i);

	AsciiString ipstr;
	AsciiString asciientry;

	LANPreferences::const_iterator it = find(key.str());
	if (it == end())
	{
		asciientry = "";
		return ret;
	}

	asciientry = it->second;

	asciientry.nextToken(&ipstr, ":");
	asciientry.set(asciientry.str() + 1); // skip the ':'

	ret.translate(ipstr);
	if (!asciientry.isEmpty())
	{
		ret.concat(L"(");
		ret.concat(QuotedPrintableToUnicodeString(asciientry));
		ret.concat(L")");
	}

	return ret;
}

static const char superweaponRestrictionKey[] = "SuperweaponRestrict";

Bool LANPreferences::getSuperweaponRestricted() const
{
  LANPreferences::const_iterator it = find(superweaponRestrictionKey);
  if (it == end())
  {
    return false;
  }

  return ( it->second.compareNoCase( "yes" ) == 0 );
}

void LANPreferences::setSuperweaponRestricted( Bool superweaponRestricted )
{
  (*this)[superweaponRestrictionKey] = superweaponRestricted ? "Yes" : "No";
}

static const char startingCashKey[] = "StartingCash";
Money LANPreferences::getStartingCash() const
{
  LANPreferences::const_iterator it = find(startingCashKey);
  if (it == end())
  {
    return TheMultiplayerSettings->getDefaultStartingMoney();
  }

  Money money;
  money.deposit( strtoul( it->second.str(), nullptr, 10 ), FALSE, FALSE );

  return money;
}

void LANPreferences::setStartingCash( const Money & startingCash )
{
  AsciiString option;

  option.format( "%d", startingCash.countMoney() );

  (*this)[startingCashKey] = option;
}

// PRIVATE DATA ///////////////////////////////////////////////////////////////////////////////////


// window ids ------------------------------------------------------------------------------
static NameKeyType parentLanLobbyID = NAMEKEY_INVALID;
static NameKeyType buttonBackID = NAMEKEY_INVALID;
static NameKeyType buttonClearID = NAMEKEY_INVALID;
static NameKeyType buttonHostID = NAMEKEY_INVALID;
static NameKeyType buttonJoinID = NAMEKEY_INVALID;
static NameKeyType buttonDirectConnectID = NAMEKEY_INVALID;
static NameKeyType buttonEmoteID = NAMEKEY_INVALID;
static NameKeyType staticToolTipID = NAMEKEY_INVALID;
static NameKeyType textEntryPlayerNameID = NAMEKEY_INVALID;
static NameKeyType textEntryChatID = NAMEKEY_INVALID;
static NameKeyType listboxPlayersID = NAMEKEY_INVALID;
static NameKeyType staticTextGameInfoID = NAMEKEY_INVALID;


// Window Pointers ------------------------------------------------------------------------
static GameWindow *parentLanLobby = nullptr;
static GameWindow *buttonBack = nullptr;
static GameWindow *buttonClear = nullptr;
static GameWindow *buttonHost = nullptr;
static GameWindow *buttonJoin = nullptr;
static GameWindow *buttonDirectConnect = nullptr;
static GameWindow *buttonEmote = nullptr;
static GameWindow *staticToolTip = nullptr;
static GameWindow *textEntryPlayerName = nullptr;
static GameWindow *textEntryChat = nullptr;
static GameWindow *staticTextGameInfo = nullptr;

//external declarations of the Gadgets the callbacks can use
NameKeyType listboxChatWindowID = NAMEKEY_INVALID;
GameWindow *listboxChatWindow = nullptr;
GameWindow *listboxPlayers = nullptr;
NameKeyType listboxGamesID = NAMEKEY_INVALID;
GameWindow *listboxGames = nullptr;

//static Bool shellmapOn;
static UnicodeString defaultName;

static void playerTooltip(GameWindow *window,
													WinInstanceData *instData,
													UnsignedInt mouse)
{
	Int x, y, row, col;
	x = LOLONGTOSHORT(mouse);
	y = HILONGTOSHORT(mouse);

	GadgetListBoxGetEntryBasedOnXY(window, x, y, row, col);

	if (row == -1 || col == -1)
	{
		//TheMouse->setCursorTooltip( TheGameText->fetch("TOOLTIP:LobbyPlayers") );
		return;
	}

	UnsignedInt playerIP = (UnsignedInt)GadgetListBoxGetItemData( window, row, col );
	if (!TheLAN)
		return;
	LANPlayer *player = TheLAN->LookupPlayer(playerIP);
	if (!player)
	{
		DEBUG_CRASH(("No player info in listbox!"));
		//TheMouse->setCursorTooltip( TheGameText->fetch("TOOLTIP:LobbyPlayers") );
		return;
	}

	setLANPlayerTooltip(player);
}


//-------------------------------------------------------------------------------------------------
/** Initialize the Lan Lobby Menu */
//-------------------------------------------------------------------------------------------------
void LanLobbyMenuInit( WindowLayout *layout, void *userData )
{
	LANnextScreen = nullptr;
	LANbuttonPushed = false;
	LANisShuttingDown = false;

	// get the ids for our controls
	parentLanLobbyID = TheNameKeyGenerator->nameToKey( "LanLobbyMenu.wnd:LanLobbyMenuParent" );
	buttonBackID = TheNameKeyGenerator->nameToKey( "LanLobbyMenu.wnd:ButtonBack" );
	buttonClearID = TheNameKeyGenerator->nameToKey( "LanLobbyMenu.wnd:ButtonClear" );
	buttonHostID = TheNameKeyGenerator->nameToKey( "LanLobbyMenu.wnd:ButtonHost" );
	buttonJoinID = TheNameKeyGenerator->nameToKey( "LanLobbyMenu.wnd:ButtonJoin" );
	buttonDirectConnectID = TheNameKeyGenerator->nameToKey( "LanLobbyMenu.wnd:ButtonDirectConnect" );
	buttonEmoteID = TheNameKeyGenerator->nameToKey( "LanLobbyMenu.wnd:ButtonEmote" );
	staticToolTipID = TheNameKeyGenerator->nameToKey( "LanLobbyMenu.wnd:StaticToolTip" );
	textEntryPlayerNameID = TheNameKeyGenerator->nameToKey( "LanLobbyMenu.wnd:TextEntryPlayerName" );
	textEntryChatID = TheNameKeyGenerator->nameToKey( "LanLobbyMenu.wnd:TextEntryChat" );
	listboxPlayersID = TheNameKeyGenerator->nameToKey( "LanLobbyMenu.wnd:ListboxPlayers" );
	listboxChatWindowID = TheNameKeyGenerator->nameToKey( "LanLobbyMenu.wnd:ListboxChatWindowLanLobby" );
	listboxGamesID = TheNameKeyGenerator->nameToKey( "LanLobbyMenu.wnd:ListboxGames" );
	staticTextGameInfoID = TheNameKeyGenerator->nameToKey( "LanLobbyMenu.wnd:StaticTextGameInfo" );


	// Get pointers to the window buttons
	parentLanLobby = TheWindowManager->winGetWindowFromId( nullptr, parentLanLobbyID );
	buttonBack = TheWindowManager->winGetWindowFromId( nullptr,  buttonBackID);
	buttonClear = TheWindowManager->winGetWindowFromId( nullptr,  buttonClearID);
	buttonHost = TheWindowManager->winGetWindowFromId( nullptr, buttonHostID );
	buttonJoin = TheWindowManager->winGetWindowFromId( nullptr, buttonJoinID );
	buttonDirectConnect = TheWindowManager->winGetWindowFromId( nullptr, buttonDirectConnectID );
	buttonEmote = TheWindowManager->winGetWindowFromId( nullptr,buttonEmoteID  );
	staticToolTip = TheWindowManager->winGetWindowFromId( nullptr, staticToolTipID );
	textEntryPlayerName = TheWindowManager->winGetWindowFromId( nullptr, textEntryPlayerNameID );
	textEntryChat = TheWindowManager->winGetWindowFromId( nullptr, textEntryChatID );
	listboxPlayers = TheWindowManager->winGetWindowFromId( nullptr, listboxPlayersID );
	listboxChatWindow = TheWindowManager->winGetWindowFromId( nullptr, listboxChatWindowID );
	listboxGames = TheWindowManager->winGetWindowFromId( nullptr, listboxGamesID );
	staticTextGameInfo = TheWindowManager->winGetWindowFromId( nullptr, staticTextGameInfoID );
	listboxPlayers->winSetTooltipFunc(playerTooltip);

	// Show Menu
	layout->hide( FALSE );

	// In coordinator mode we delay (and may skip) LANAPI bring-up: the
	// coordinator wants UDP/8086 first for the STUN punch, and the LAN
	// transport would steal it. TheLAN is created later, after PUNCH_OK,
	// when we hand the punched peer address into the LAN direct-connect
	// flow.
	s_coordHandoffDone = FALSE;
	s_coordPendingHostName.clear();
	s_coordPendingJoinID.clear();
	s_coordListedIDs.clear();
	s_coordLastListMs = 0;
	s_coordFailureUploads = 0;
	s_coordHostRequestMs = 0;
	if (s_useCoordinator)
	{
		if (TheLAN)
		{
			delete TheLAN;
			TheLAN = nullptr;
		}
		if (s_coord)
		{
			delete s_coord;
		}
		s_coord = new OnlineCoordinatorAPI();
	}
	else if (!TheLAN)
	{
		TheLAN = NEW LANAPI();	/// @todo clh delete TheLAN and
	}
	else
	{
		TheLAN->reset();
	}

	// Choose an IP address, then initialize the LAN singleton
	UnsignedInt IP = TheGlobalData->m_defaultIP;
	IPEnumeration IPs;
	const WideChar* IPSource;
	if (!IP)
	{
		EnumeratedIP *IPlist = IPs.getAddresses();
		/*
		while (IPlist && IPlist->getNext())
		{
			IPlist = IPlist->getNext();
		}
		*/
		DEBUG_ASSERTCRASH(IPlist, ("No IP addresses found!"));
		if (!IPlist)
		{
			/// @todo: display error and exit lan lobby if no IPs are found
		}

		IPSource = L"Local IP chosen";
		IP = IPlist->getIP();
	}
	else
	{
		IPSource = L"Default local IP";
	}
#if defined(RTS_DEBUG)
	UnicodeString str;
	str.format(L"%s: %d.%d.%d.%d", IPSource, PRINTF_IP_AS_4_INTS(IP));
	GadgetListBoxAddEntryText(listboxChatWindow, str, chatSystemColor, -1, 0);
#endif

	// TheLAN->init() sets us to be in a LAN menu screen automatically.
	if (!s_useCoordinator)
	{
		TheLAN->init();
		if (TheLAN->SetLocalIP(IP) == FALSE) {
			LANSocketErrorDetected = TRUE;
		}
	}

	//Initialize the gadgets on the window
	//UnicodeString	txtInput;
	//txtInput.translate(IPs.getMachineName());
	LANPreferences prefs;
	defaultName = prefs.getUserName();
	defaultName.truncateTo(g_lanPlayerNameLength);

	// Test automation: -coordnick overrides the persisted player name so
	// multiple lab clients sharing an install don't collide on the
	// duplicate-name check.
	if (!TheGlobalData->m_coordNick.isEmpty())
	{
		defaultName.translate(TheGlobalData->m_coordNick);
		defaultName.truncateTo(g_lanPlayerNameLength);
	}

	GadgetTextEntrySetText( textEntryPlayerName, defaultName);
	// Clear the text entry line
	GadgetTextEntrySetText(textEntryChat, UnicodeString::TheEmptyString);

	GadgetListBoxReset(listboxPlayers);
	GadgetListBoxReset(listboxGames);

	defaultName.truncateTo(g_lanPlayerNameLength);
	if (!s_useCoordinator)
	{
		TheLAN->RequestSetName(defaultName);
		TheLAN->RequestLocations();
	}

	/*
	UnicodeString unicodeChat;

	unicodeChat = L"Local IP list:";
	GadgetListBoxAddEntryText(listboxChatWindow, unicodeChat, chatSystemColor, -1, 0);

	IPlist = IPs.getAddresses();
	while (IPlist)
	{
		unicodeChat.translate(IPlist->getIPstring());
		GadgetListBoxAddEntryText(listboxChatWindow, unicodeChat, chatSystemColor, -1, 0);
		IPlist = IPlist->getNext();
	}
	*/

	// Set Keyboard to Main Parent
	//TheWindowManager->winSetFocus( parentLanLobby );
	TheWindowManager->winSetFocus( textEntryChat );
	CreateLANGameInfoWindow(staticTextGameInfo);

	//TheShell->showShellMap(FALSE);
	//shellmapOn = FALSE;
	// coming out of a game, re-load the shell map
	TheShell->showShellMap(TRUE);

	// check for MOTD
	if (!s_useCoordinator)
	{
		TheLAN->checkMOTD();
	}
	else
	{
		// Kick off the coordinator handshake immediately so the games list
		// populates without needing the user to click Host/Join first.
		connectCoordinatorIfNeeded();
	}
	layout->hide(FALSE);
	layout->bringForward();

	justEntered = TRUE;
	initialGadgetDelay = 2;
	GameWindow *win = TheWindowManager->winGetWindowFromId(nullptr, TheNameKeyGenerator->nameToKey("LanLobbyMenu.wnd:GadgetParent"));
	if(win)
		win->winHide(TRUE);


	// animate controls
	//TheShell->registerWithAnimateManager(parentLanLobby, WIN_ANIMATION_SLIDE_TOP, TRUE);
//	TheShell->registerWithAnimateManager(buttonHost, WIN_ANIMATION_SLIDE_LEFT, TRUE, 600);
//	TheShell->registerWithAnimateManager(buttonJoin, WIN_ANIMATION_SLIDE_LEFT, TRUE, 400);
//	TheShell->registerWithAnimateManager(buttonDirectConnect, WIN_ANIMATION_SLIDE_LEFT, TRUE, 200);
//	//TheShell->registerWithAnimateManager(buttonOptions, WIN_ANIMATION_SLIDE_LEFT, TRUE, 1);
//	TheShell->registerWithAnimateManager(buttonBack, WIN_ANIMATION_SLIDE_RIGHT, TRUE, 1);

}

//-------------------------------------------------------------------------------------------------
/** This is called when a shutdown is complete for this menu */
//-------------------------------------------------------------------------------------------------
static void shutdownComplete( WindowLayout *layout )
{

	LANisShuttingDown = false;

	// hide the layout
	layout->hide( TRUE );

	// our shutdown is complete
	TheShell->shutdownComplete( layout, (LANnextScreen != nullptr) );

	if (LANnextScreen != nullptr)
	{
		TheShell->push(LANnextScreen);
	}

	LANnextScreen = nullptr;

}

//-------------------------------------------------------------------------------------------------
/** Lan Lobby menu shutdown method */
//-------------------------------------------------------------------------------------------------
void LanLobbyMenuShutdown( WindowLayout *layout, void *userData )
{
	LANPreferences prefs;
	prefs["UserName"] = UnicodeStringToQuotedPrintable(GadgetTextEntryGetText( textEntryPlayerName ));
	prefs.write();

	DestroyGameInfoWindow();
	// hide menu
	//layout->hide( TRUE );

	if (TheLAN)
	{
		TheLAN->RequestLobbyLeave( true );
	}

	if (s_coord)
	{
		// After a handoff, leave s_coord alive so LanGameOptionsMenu can
		// pump it: the host accepts additional joiners through it, joiners
		// receive guest<->guest mesh notifications. Torn down later by
		// LanLobbyMenuShutdownCoordinator[KeepStash]().
		if (!s_coordHandoffDone)
		{
			ReleaseLog("Coordinator teardown: LanLobbyMenuShutdown (no handoff; amIHost=%d)",
				(int)s_coord->amIHost());
			s_coord->disconnect();
			delete s_coord;
			s_coord = nullptr;
		}
		else
		{
			ReleaseLog("Coordinator kept alive through lobby shutdown (amIHost=%d)",
				(int)s_coord->amIHost());
		}
	}
	// If the handoff to TheLAN already happened we keep s_useCoordinator
	// set so the post-handoff lobby push still uses the coordinator-aware
	// flow if it pushes us back here. The MainMenu callback resets the
	// flag the next time Online vs Network is chosen.
	if (!s_coordHandoffDone)
	{
		s_useCoordinator = FALSE;
	}

	// Reset the LAN singleton
	//TheLAN->reset();

	// our shutdown is complete
	//TheShell->shutdownComplete( layout );

	LANisShuttingDown = true;

	// if we are shutting down for an immediate pop, skip the animations
	Bool popImmediate = *(Bool *)userData;

	LANSocketErrorDetected = FALSE;

	if( popImmediate )
	{

		shutdownComplete( layout );
		return;

	}

	TheShell->reverseAnimatewindow();
	TheTransitionHandler->reverse("LanLobbyFade");
	//if(	shellmapOn)
//		TheShell->showShellMap(TRUE);
}


//-------------------------------------------------------------------------------------------------
// Helpers for coordinator-mode lobby.
//-------------------------------------------------------------------------------------------------

static AsciiString readPlayerNickAscii()
{
	UnicodeString u = GadgetTextEntryGetText( textEntryPlayerName );
	AsciiString a;
	a.translate(u);
	a.trim();
	if (a.isEmpty()) a = "anonymous";
	return a;
}

// Coordinator endpoint, honouring the -coordhost "host[:port]" override
// used by the lab/test harnesses.
static void coordinatorEndpoint(AsciiString& outHost, UnsignedShort& outPort)
{
	outHost = COORD_HOST_DEFAULT;
	outPort = COORD_TCP_PORT_DEFAULT;
	if (TheGlobalData->m_coordHost.isEmpty())
		return;

	AsciiString host = TheGlobalData->m_coordHost;
	const char* colon = strchr(host.str(), ':');
	if (colon)
	{
		outPort = (UnsignedShort)atoi(colon + 1);
		AsciiString hostOnly;
		for (const char* p = host.str(); p != colon; ++p)
			hostOnly.concat(*p);
		host = hostOnly;
	}
	outHost = host;
}

// An online attempt failed before any game existed. Write the full picture to
// the ReleaseLog and ship that log to cncstats right now.
//
// This is the only path that ever produces server-side evidence for "I
// couldn't host" / "I couldn't join": no match means no end-of-match
// telemetry, and the player's ReleaseLog is truncated by their next launch.
// The upload is keyed by a per-day bucket so a whole evening's failures come
// back from one /get_logs?seed=connfail-YYYYMMDD, and by a per-attempt player
// id so two failures never overwrite each other.
//
// `phase` is a short tag for what was being attempted; coordFailurePhase()
// derives it from the state the session failed out of.
static void coordinatorReportFailure(const char* phase)
{
	if (s_coord == nullptr)
		return;

	AsciiString host;
	UnsignedShort port = 0;
	coordinatorEndpoint(host, port);

	ReleaseLog("Coordinator FAILURE (%s): coord=%s:%u nick=%s state=%d amIHost=%d error=\"%s\" "
		"publicLobby=%s publicGame=%s local=%s hostedID=%s joinID=%s retries=%d",
		phase, host.str(), (unsigned)port, s_coordCurrentNick.str(),
		(Int)s_coord->state(), (Int)s_coord->amIHost(), s_coord->lastError().str(),
		s_coord->publicAddr().str(), s_coord->gamePublicAddr().str(), s_coord->localAddr().str(),
		s_coord->hostedGameID().str(), s_coordLastJoinID.str(), s_coordJoinRetries);

	if (TheGlobalData->m_logsUrl.isEmpty())
		return;
	if (s_coordFailureUploads >= COORD_MAX_FAILURE_UPLOADS)
		return;
	++s_coordFailureUploads;

	// UTC so every player's failures on one game night land in the same
	// bucket regardless of time zone.
	SYSTEMTIME st;
	GetSystemTime(&st);
	AsciiString seedLabel;
	seedLabel.format("connfail-%04u%02u%02u",
		(unsigned)st.wYear, (unsigned)st.wMonth, (unsigned)st.wDay);
	AsciiString playerId;
	playerId.format("%s-%02u%02u%02u-%s",
		s_coordCurrentNick.isEmpty() ? "anonymous" : s_coordCurrentNick.str(),
		(unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond, phase);

	std::vector<AsciiString> paths;
	paths.push_back(AsciiString(ReleaseGetLogFileName()));
#ifdef DEBUG_LOGGING
	paths.push_back(AsciiString(DebugGetLogFileName()));
#endif
	ReleaseLog("Coordinator FAILURE (%s): uploading logs as %s/%s",
		phase, seedLabel.str(), playerId.str());
	StartDiagnosticLogUpload(TheGlobalData->m_logsUrl, seedLabel, playerId, paths);
}

// What the session was doing when it failed. STATE_ERROR erases the previous
// state, so the caller passes the state from before update() ran; this is the
// difference between "we never reached the coordinator" and "the punch to the
// other player timed out", which is the first thing anyone triaging the
// uploaded log needs to know.
static const char* coordFailurePhase(OnlineCoordinatorAPI::State prev)
{
	switch (prev)
	{
	case OnlineCoordinatorAPI::STATE_CONNECTING:  return "connect";
	case OnlineCoordinatorAPI::STATE_HANDSHAKING: return "hello";
	case OnlineCoordinatorAPI::STATE_DISCOVERING: return "stun";
	case OnlineCoordinatorAPI::STATE_READY:       return "ready";
	case OnlineCoordinatorAPI::STATE_HOSTING:     return "host";
	case OnlineCoordinatorAPI::STATE_JOINING:     return "join";
	case OnlineCoordinatorAPI::STATE_PUNCHING:    return "punch";
	default:                                      return "coordinator";
	}
}

static void connectCoordinatorIfNeeded()
{
	if (!s_coord) return;
	if (s_coord->state() != OnlineCoordinatorAPI::STATE_IDLE &&
	    s_coord->state() != OnlineCoordinatorAPI::STATE_ERROR)
	{
		return;
	}
	s_coordCurrentNick = readPlayerNickAscii();
	AsciiString host;
	UnsignedShort tcpPort = COORD_TCP_PORT_DEFAULT;
	coordinatorEndpoint(host, tcpPort);
	s_coordHostRequestMs = 0;
	// Bind UDP/8086 (lobby) so the punched NAT mapping is on the port the LAN
	// code will rebind after PUNCH_OK, AND UDP/8088 (NETWORK_BASE_PORT_NUMBER,
	// in-game data) so ConnectionManager's later socket inherits an already-
	// punched mapping. The TCP signaling port is the listed coordinator port;
	// UDP STUN is on the port reported in hello_ok.
	// Send the real build version so the coordinator can refuse to match
	// clients running different game versions (which would only meet again
	// as an in-game desync). TheVersion is the same string the main menu
	// shows (APPVERSION via the installer chain).
	AsciiString buildVersion = "zulu/unknown";
	if (TheVersion)
		buildVersion = TheVersion->getAsciiVersion();
	ReleaseLog("Coordinator connect: %s:%u nick=%s version=%s", host.str(), (unsigned)tcpPort,
		s_coordCurrentNick.str(), buildVersion.str());
	if (!s_coord->connect(host, tcpPort,
		s_coordCurrentNick, buildVersion,
		/*lobbyBindPort=*/8086, /*gameBindPort=*/NETWORK_BASE_PORT_NUMBER))
	{
		// Synchronous failures (name resolution, or UDP 8086/8088 already
		// taken by another copy of the game) never reach the state-change
		// handler in pumpCoordinator, so report them here or they are
		// invisible to both the player and the server.
		ReleaseLog("Coordinator connect FAILED: %s", s_coord->lastError().str());
		AsciiString msg;
		msg.format("Could not start the online connection: %s", s_coord->lastError().str());
		coordinatorPostStatus(msg.str());
		coordinatorReportFailure("connect");
	}
}

static void rebuildGamesListbox()
{
	const std::vector<OnlineCoordinatorAPI::GameListEntry>& games = s_coord->games();
	GadgetListBoxReset(listboxGames);
	s_coordListedIDs.clear();
	Color textColor = GameMakeColor(255, 255, 255, 255);
	for (size_t i = 0; i < games.size(); ++i)
	{
		const OnlineCoordinatorAPI::GameListEntry& g = games[i];
		AsciiString row;
		if (g.inProgress)
		{
			row.format("%s   [%s]   IN PROGRESS - double-click to watch",
				g.name.str(), g.hostNick.str());
		}
		else
		{
			row.format("%s   [%s]   %d/%d   %s%s",
				g.name.str(), g.hostNick.str(), g.players, g.maxPlayers,
				g.map.str(),
				// Set by the coordinator once any joiner could not punch
				// this host: everyone connects through the relay, so the
				// game runs with extra latency. Players can prefer another.
				g.restrictedHost ? "   [relayed host]" : "");
		}
		UnicodeString u;
		u.translate(row);
		GadgetListBoxAddEntryText(listboxGames, u,
			g.inProgress ? GameMakeColor(200, 200, 160, 255) : textColor, -1, 0);
		s_coordListedIDs.push_back(g.id);
	}
}

static void coordinatorPostStatus(const char* msg)
{
	UnicodeString u;
	u.translate(AsciiString(msg));
	GadgetListBoxAddEntryText(listboxChatWindow, u, GameMakeColor(180, 180, 255, 255), -1, 0);
}

// Shared handler for "the user picked a game row": in-progress games get an
// observe request (watch via the coordinator relay), waiting games get the
// normal join flow. Both are queued and dispatched once the coordinator
// connection reports READY.
static void coordinatorJoinOrObserveRow(Int rowSelected)
{
	if (rowSelected < 0 || rowSelected >= (Int)s_coordListedIDs.size())
		return;
	connectCoordinatorIfNeeded();

	Bool observe = FALSE;
	const std::vector<OnlineCoordinatorAPI::GameListEntry>& games = s_coord->games();
	if (rowSelected < (Int)games.size() && games[rowSelected].inProgress)
		observe = TRUE;

	if (observe)
	{
		s_coordPendingObserveID = s_coordListedIDs[rowSelected];
		s_coordPendingJoinID.clear();
		s_coordPendingHostName.clear();
		coordinatorPostStatus("Requesting to watch the game...");
	}
	else
	{
		s_coordPendingJoinID = s_coordListedIDs[rowSelected];
		s_coordPendingObserveID.clear();
		s_coordPendingHostName.clear();
		coordinatorPostStatus("Joining game as soon as the connection is ready...");
	}
}

static void doCoordinatorHandoffToLAN();
static void doCoordinatorHostHandoffToLAN();

// User-facing text for each coordinator state transition. Returns null for
// states that are pure plumbing (the user doesn't need a line for them).
static const char* coordStateStatusText(OnlineCoordinatorAPI::State state)
{
	switch (state)
	{
	case OnlineCoordinatorAPI::STATE_CONNECTING:  return "Connecting to the online service...";
	case OnlineCoordinatorAPI::STATE_HANDSHAKING: return nullptr;
	case OnlineCoordinatorAPI::STATE_DISCOVERING: return "Detecting your internet address...";
	case OnlineCoordinatorAPI::STATE_READY:       return "Online. You can host or join a game.";
	case OnlineCoordinatorAPI::STATE_HOSTING:     return "Game registered. Waiting for players to join...";
	case OnlineCoordinatorAPI::STATE_JOINING:     return "Joining game...";
	case OnlineCoordinatorAPI::STATE_PUNCHING:    return "Connecting to the other player...";
	case OnlineCoordinatorAPI::STATE_PUNCH_OK:    return "Connected!";
	default: return nullptr;
	}
}

static void pumpCoordinator()
{
	OnlineCoordinatorAPI::State prevState = s_coord->state();
	s_coord->update();
	OnlineCoordinatorAPI::State state = s_coord->state();

	if (state != prevState)
	{
		const char* stateText = coordStateStatusText(state);
		if (stateText != nullptr)
			coordinatorPostStatus(stateText);
		ReleaseLog("Coordinator state %d -> %d%s%s", (Int)prevState, (Int)state,
			(state == OnlineCoordinatorAPI::STATE_ERROR) ? " error: " : "",
			(state == OnlineCoordinatorAPI::STATE_ERROR) ? s_coord->lastError().str() : "");
		if (state == OnlineCoordinatorAPI::STATE_READY)
		{
			// The public address is diagnostic gold when players report punch
			// failures, but it reads as noise to most users; keep it in the
			// ReleaseLog only.
			ReleaseLog("Coordinator public addr: %s", s_coord->publicAddr().str());
			// Immediately fetch games on first READY transition.
			s_coord->requestList();
			s_coordLastListMs = timeGetTime();
		}
		if (state == OnlineCoordinatorAPI::STATE_ERROR)
		{
			// Definitive rejections must not be retried or paraphrased: a
			// version mismatch reads the same on every attempt, and the
			// server's message names both versions, which is exactly what
			// the user needs to see.
			Bool definitiveError = (strstr(s_coord->lastError().str(), "version mismatch") != NULL);
			if (definitiveError)
			{
				coordinatorPostStatus(s_coord->lastError().str());
				coordinatorReportFailure("version");
				s_coordLastJoinID.clear();
				s_coordJoinRetries = 0;
				connectCoordinatorIfNeeded();
			}
			// A failed JOIN attempt (usually "punch: no inbound packet
			// within timeout") retries automatically, like LAN where a
			// dropped join request is invisible to the user. connect()
			// tears down the failed attempt's sockets, and re-queueing the
			// pending id re-dispatches the join once READY again.
			else if (!s_coord->amIHost() && !s_coordLastJoinID.isEmpty()
				&& s_coordJoinRetries < COORD_JOIN_MAX_RETRIES)
			{
				++s_coordJoinRetries;
				ReleaseLog("Coordinator join retry %d/%d after error: %s",
					s_coordJoinRetries, COORD_JOIN_MAX_RETRIES, s_coord->lastError().str());
				coordinatorPostStatus("Connection attempt failed. Retrying...");
				connectCoordinatorIfNeeded();
				s_coordPendingJoinID = s_coordLastJoinID;
			}
			else if (!s_coord->amIHost() && !s_coordLastJoinID.isEmpty())
			{
				// Retries exhausted: give a human answer and put the lobby
				// back into a usable browsing state.
				coordinatorPostStatus("Could not connect to that game's host. Their network may be blocking the connection.");
				coordinatorReportFailure("join");
				s_coordLastJoinID.clear();
				s_coordJoinRetries = 0;
				connectCoordinatorIfNeeded();
			}
			else
			{
				AsciiString s;
				s.format("Connection error: %s", s_coord->lastError().str());
				coordinatorPostStatus(s.str());
				// Hosting has no retry path (nobody is waiting on us), so
				// this is where a failed host lands, along with any error
				// raised before a host/join was ever dispatched.
				coordinatorReportFailure(coordFailurePhase(prevState));
			}
		}
		// The coordinator closing the TCP session drops us to IDLE without an
		// error. Before the handoff that silently kills hosting/browsing: the
		// lobby keeps showing a stale games list and the Host button does
		// nothing. Treat it as a failure, then reconnect so the lobby heals.
		if (state == OnlineCoordinatorAPI::STATE_IDLE && !s_coordHandoffDone
			&& prevState != OnlineCoordinatorAPI::STATE_IDLE
			&& prevState != OnlineCoordinatorAPI::STATE_ERROR)
		{
			coordinatorPostStatus("The online service closed the connection. Reconnecting...");
			coordinatorReportFailure("closed");
			s_coordHostRequestMs = 0;
			connectCoordinatorIfNeeded();
		}
		if (state == OnlineCoordinatorAPI::STATE_PUNCH_OK && !s_coordHandoffDone)
		{
			s_coordLastJoinID.clear();
			s_coordJoinRetries = 0;
			doCoordinatorHandoffToLAN();
			// Handoff triggers TheShell->push which synchronously runs
			// LanLobbyMenuShutdown, which deletes s_coord. The rest of this
			// function would null-deref on s_coord->games() below; bail.
			return;
		}
	}

	// Defensive: if anything else nulled s_coord between calls, do not touch
	// it further in this tick.
	if (!s_coord)
		return;

	// Host watchdog: the request went out but the coordinator never acked
	// the listing. Without this the lobby claims the game is registered and
	// waits forever for joiners who can't see it.
	if (!s_coordHandoffDone
	    && s_coord->amIHost()
	    && s_coord->state() == OnlineCoordinatorAPI::STATE_HOSTING
	    && s_coord->hostedGameID().isEmpty()
	    && s_coordHostRequestMs != 0
	    && timeGetTime() - s_coordHostRequestMs > COORD_HOST_ACK_TIMEOUT_MS)
	{
		coordinatorPostStatus("The online service never confirmed your game. Reconnecting - please try hosting again.");
		coordinatorReportFailure("host-ack");
		s_coordHostRequestMs = 0;
		// Force a clean session: connectCoordinatorIfNeeded only acts from
		// IDLE/ERROR, and this one is stuck in HOSTING.
		s_coord->disconnect();
		connectCoordinatorIfNeeded();
		return;
	}

	// Host: as soon as the coordinator ACKS our listing (game_id in hand,
	// so joiners can actually find us), hand off to the LAN lobby instead
	// of waiting for a first joiner to punch. This is what makes hosting
	// feel like LAN -- you sit in the real lobby, pick the map and chat --
	// and it removes the first-joiner special case entirely: every joiner
	// now arrives through the post-handoff path that joiners 2..N already
	// used. Checked per tick (not on state change) because STATE_HOSTING
	// is entered when the request is SENT; the ack lands a round trip later.
	if (!s_coordHandoffDone
	    && s_coord->amIHost()
	    && s_coord->state() == OnlineCoordinatorAPI::STATE_HOSTING
	    && !s_coord->hostedGameID().isEmpty())
	{
		doCoordinatorHostHandoffToLAN();
		// The handoff pushed the game-options screen, which tore down this
		// menu's gadgets; nothing below may touch them this tick.
		return;
	}

	// When READY, periodically refresh the games list and dispatch any
	// pending host/join action that was queued before the connection
	// finished handshaking.
	if (state == OnlineCoordinatorAPI::STATE_READY)
	{
		// Test automation: queue the host/join action once we're READY.
		// Join waits until the games list contains the wanted name.
		static Bool coordAutoDispatched = FALSE;
		if (!coordAutoDispatched)
		{
			if (!TheGlobalData->m_coordAutoHostName.isEmpty())
			{
				s_coordPendingHostName = TheGlobalData->m_coordAutoHostName;
				coordAutoDispatched = TRUE;
				coordinatorPostStatus("auto: hosting");
			}
			else if (!TheGlobalData->m_coordAutoJoinName.isEmpty())
			{
				const std::vector<OnlineCoordinatorAPI::GameListEntry>& games = s_coord->games();
				for (size_t gi = 0; gi < games.size(); ++gi)
				{
					if (games[gi].name == TheGlobalData->m_coordAutoJoinName)
					{
						// Same decision a human double-click makes: watch
						// in-progress games, join waiting ones.
						if (games[gi].inProgress)
						{
							s_coordPendingObserveID = games[gi].id;
							coordinatorPostStatus("auto: observing");
						}
						else
						{
							s_coordPendingJoinID = games[gi].id;
							coordinatorPostStatus("auto: joining");
						}
						coordAutoDispatched = TRUE;
						break;
					}
				}
			}
		}

		UnsignedInt nowMs = timeGetTime();
		if (nowMs - s_coordLastListMs > COORD_LIST_REFRESH_MS)
		{
			s_coord->requestList();
			s_coordLastListMs = nowMs;
		}
		if (!s_coordPendingHostName.isEmpty())
		{
			UnicodeString u; u.translate(s_coordPendingHostName);
			// Advertise the real map the lobby will open with (the same
			// preference LanGameOptionsMenuInit applies) so the games list
			// shows something meaningful instead of "unknown 0/2".
			LANPreferences pref;
			AsciiString mapPath = pref.getPreferredMap();
			Int maxPlayers = 2;
			AsciiString mapLeaf = mapPath;
			if (TheMapCache)
			{
				const MapMetaData* md = TheMapCache->findMap(mapPath);
				if (md && md->m_numPlayers > 0)
					maxPlayers = md->m_numPlayers;
			}
			// Leaf name reads better than the full maps\...\... path.
			{
				const char* leaf = mapPath.reverseFind('\\');
				if (leaf && leaf[1] != '\0')
					mapLeaf = leaf + 1;
			}
			s_coord->requestHost(u, mapLeaf, maxPlayers);
			// NAT self-check verdict: a symmetric/CGNAT connection means no
			// one can punch us and every joiner rides the relay. Warn once
			// per session; hosting still proceeds (the relay carries it,
			// just with more latency for everyone). Suppressed for the
			// unattended -coordautohost flows, which must never block on a
			// dialog.
			static Bool s_warnedRestrictiveNat = FALSE;
			if (!s_warnedRestrictiveNat && s_coord->natLooksSymmetric() &&
				TheGlobalData->m_coordAutoHostName.isEmpty())
			{
				s_warnedRestrictiveNat = TRUE;
				ReleaseLog("NATCHECK host warning shown (symmetric NAT)");
				UnicodeString title, body;
				title.translate(AsciiString("Restrictive Connection"));
				body.translate(AsciiString(
					"Your internet connection does not accept direct connections from other players, "
					"so everyone will connect through the relay server.\n\n"
					"You can host, but games may run smoother if another player hosts."));
				MessageBoxOk(title, body, nullptr);
			}
			else if (s_coord->natLooksSymmetric())
			{
				ReleaseLog("NATCHECK symmetric host (warning suppressed: auto flow or already shown)");
			}
			s_coordPendingHostName.clear();
			// Arm the listing-ack watchdog (never 0, which means "not armed").
			s_coordHostRequestMs = timeGetTime();
			if (s_coordHostRequestMs == 0)
				s_coordHostRequestMs = 1;
		}
		else if (!s_coordPendingJoinID.isEmpty())
		{
			s_coordLastJoinID = s_coordPendingJoinID;
			s_coord->requestJoin(s_coordPendingJoinID);
			s_coordPendingJoinID.clear();
		}
		else if (!s_coordPendingObserveID.isEmpty())
		{
			s_coord->requestObserve(s_coordPendingObserveID);
			s_coordPendingObserveID.clear();
		}
	}

	// Observe accepted: attach our end of the relay and hand the connected
	// socket to the LAN observer client. From here the flow is identical to
	// watching a LAN game (snapshot buffering, map fetch, live playback).
	{
		AsciiString observeToken;
		if (s_coord->consumeObserveOkToken(&observeToken))
		{
			Int relayFd = s_coord->openObserverRelayFd(observeToken, FALSE);
			if (relayFd >= 0)
			{
				// Coordinator mode enters the lobby WITHOUT a LANAPI (it is
				// normally created during the join handoff, which an
				// observer never runs). The observer machinery lives on
				// TheLAN, so bring one up now; the relay socket replaces
				// any direct host connection.
				if (!TheLAN)
				{
					// Our coordinator session still holds UDP/8086;
					// release it or TheLAN->init()'s bind fails and the
					// socket-error path boots us to the main menu.
					s_coord->closeLobbyUdpForHostHandoff();
					TheLAN = NEW LANAPI();
					TheLAN->init();
					UnsignedInt localIP = TheGlobalData->m_defaultIP;
					if (!localIP)
					{
						IPEnumeration IPs;
						EnumeratedIP* list = IPs.getAddresses();
						if (list) localIP = list->getIP();
					}
					TheLAN->SetLocalIP(localIP);
					TheLAN->RequestSetName(GadgetTextEntryGetText(textEntryPlayerName));
				}
				coordinatorPostStatus("Connected. Buffering the game for playback...");
				TheLAN->RequestObserveAdoptedFd(relayFd);
			}
			else
			{
				coordinatorPostStatus("Could not connect to the observer relay.");
			}
		}
	}

	// Listbox can lag the internal games vector by one tick; rebuild any
	// time the model size disagrees with what we last rendered. Cheap and
	// correct enough at lobby refresh rates.
	if ((Int)s_coord->games().size() != (Int)s_coordListedIDs.size())
	{
		rebuildGamesListbox();
	}
}

// Host-side handoff, run as soon as the coordinator confirms our game is
// listed -- BEFORE any joiner shows up. The host lands in the real game
// lobby immediately, exactly like LAN: pick the map, chat, wait.
//
// Every joiner (including the first) then arrives through the same
// post-handoff path joiners 2..N already used: the coordinator delivers
// peer_info to LanGameOptionsMenu, which plumbs the punched game port into
// TheLAN and fires NAT-opening probes/keepalives at the joiner's lobby
// address while the joiner does the active punching.
static void doCoordinatorHostHandoffToLAN()
{
	if (!s_coord || s_coordHandoffDone) return;
	DEBUG_LOG(("HOST HANDOFF: start (pre-joiner)"));
	// Listing acked: the host watchdog has nothing left to catch.
	s_coordHostRequestMs = 0;

	// Set BEFORE anything can push a screen: RequestGameCreate's
	// OnGameCreate callback runs TheShell->push synchronously, which runs
	// LanLobbyMenuShutdown mid-call; with the flag clear that would tear
	// down the coordinator session every joiner still needs.
	s_coordHandoffDone = TRUE;

	// Park the punched game socket in the module-level stash (keepalives
	// keep its NAT mapping alive through the lobby phase; ConnectionManager
	// adopts the FD at game start), then release the lobby socket so
	// TheLAN can rebind 8086. TCP signaling stays up for joiner delivery.
	if (!s_coord->stashGameSocketForGameStart())
	{
		DEBUG_LOG(("HOST HANDOFF: WARNING: failed to stash game socket; in-game NAT traversal will fail"));
	}
	// Hand the punched lobby socket to TheLAN by fd. Rebinding the port
	// instead would lose the NAT mapping peers are told to talk to.
	Int lobbyFd = s_coord->takeLobbyUdpFdForHandoff();

	if (!TheLAN)
	{
		TheLAN = NEW LANAPI();
	}
	TheLAN->init();
	UnsignedInt localIP = TheGlobalData->m_defaultIP;
	if (!localIP)
	{
		IPEnumeration IPs;
		EnumeratedIP* list = IPs.getAddresses();
		if (list) localIP = list->getIP();
	}
	Bool lanReady = (lobbyFd != -1)
		? TheLAN->SetLocalIPAdoptingSocket(localIP, lobbyFd)
		: TheLAN->SetLocalIP(localIP);
	if (!lanReady)
	{
		coordinatorPostStatus("LAN: SetLocalIP failed after coordinator handoff");
		DEBUG_LOG(("HOST HANDOFF: SetLocalIP returned FALSE"));
	}
	TheLAN->RequestSetName(GadgetTextEntryGetText(textEntryPlayerName));

	UnicodeString gameName = GadgetTextEntryGetText(textEntryPlayerName);
	gameName.concat(L"'s game");
	TheLAN->RequestGameCreate(gameName, /*isDirectConnect=*/TRUE);
	ReleaseLog("Coordinator host handoff to LAN done (pre-joiner)");
	DEBUG_LOG(("HOST HANDOFF: done"));
}

static void doCoordinatorHandoffToLAN()
{
	if (!s_coord) return;
	const OnlineCoordinatorAPI::PeerInfo& peer = s_coord->peerInfo();
	UnsignedInt   peerIP   = peer.punchedIP;
	UnsignedShort peerPort = peer.punchedPort;
	// Use OUR local intent, not peer_info.role, to decide which side we
	// are: m_amIHost is set inside requestHost/requestJoin so it cannot
	// drift out of sync with what we actually asked the coordinator for.
	Bool weAreHost = s_coord->amIHost();
	Int  lobbyFd   = -1;   // punched lobby socket handed to TheLAN below
	DEBUG_LOG(("HANDOFF: start weAreHost=%d peerIP=0x%08x peerPort=%u gamePeerIP=0x%08x gamePeerPort=%u",
		(int)weAreHost, peerIP, peerPort, peer.gamePunchedIP, peer.gamePunchedPort));

	// Mark the handoff BEFORE anything below can push a screen:
	// RequestGameCreate's OnGameCreate callback runs TheShell->push
	// SYNCHRONOUSLY, which runs LanLobbyMenuShutdown mid-call. If the flag
	// isn't set yet, the host-keepalive check there sees handoffDone=FALSE
	// and tears down the coordinator session that joiners 2..N still need.
	s_coordHandoffDone = TRUE;

	// Move the punched game UDP socket into the module-level stash so it
	// outlives the coordinator (and this menu); a keepalive sender keeps
	// the NAT mapping alive through the LAN lobby phase. ConnectionManager
	// adopts the FD at game start.
	if (!s_coord->stashGameSocketForGameStart())
	{
		DEBUG_LOG(("HANDOFF: WARNING: failed to stash game socket; in-game NAT traversal will fail"));
	}

	// Hand the punched lobby UDP socket to TheLAN by fd (NOT close+rebind:
	// the NAT mapping belongs to the socket, and CGNATs give a replacement
	// socket a different external port than the one peers were told).
	//
	// BOTH roles keep the TCP signaling session alive past the handoff. The
	// host needs it so the coordinator can deliver peer_info for additional
	// joiners; joiners need it for "peer" role mesh notifications about the
	// game's other guests, so every guest pair punches mutual NAT mappings
	// while still in the lobby (the coordinator only orchestrates the
	// host<->guest punch; without the mesh, guest<->guest keepalive and
	// file-transfer traffic stalls at game start behind restricted NATs).
	// The s_coord instance survives LanLobbyMenuShutdown (see guard there)
	// and is pumped by LanGameOptionsMenu.
	lobbyFd = s_coord->takeLobbyUdpFdForHandoff();
	DEBUG_LOG(("HANDOFF: coord lobby fd=%d handed over; TCP kept alive (host=%d)", lobbyFd, (int)weAreHost));

	if (!TheLAN)
	{
		TheLAN = NEW LANAPI();
	}
	TheLAN->init();
	DEBUG_LOG(("HANDOFF: TheLAN->init() done"));
	UnsignedInt localIP = TheGlobalData->m_defaultIP;
	if (!localIP)
	{
		IPEnumeration IPs;
		EnumeratedIP* list = IPs.getAddresses();
		if (list) localIP = list->getIP();
	}
	DEBUG_LOG(("HANDOFF: localIP=0x%08x lobbyFd=%d", localIP, lobbyFd));
	Bool lanReady = (lobbyFd != -1)
		? TheLAN->SetLocalIPAdoptingSocket(localIP, lobbyFd)
		: TheLAN->SetLocalIP(localIP);
	if (!lanReady)
	{
		coordinatorPostStatus("LAN: SetLocalIP failed after coordinator handoff");
		DEBUG_LOG(("HANDOFF: SetLocalIP returned FALSE"));
	}
	TheLAN->RequestSetName(GadgetTextEntryGetText(textEntryPlayerName));
	DEBUG_LOG(("HANDOFF: RequestSetName done"));

	// Plumb the peer's punched game-data port so direct-connect slot setup
	// (both host's handleRequestJoin and joiner's handleJoinAccept) records
	// it on the slot, which ConnectionManager then reads as the in-game
	// destination port. Without this the slots default to 8088 and packets
	// land on the peer's NAT with no mapping.
	TheLAN->setDirectConnectRemoteGamePort(peer.gamePunchedPort);

	if (weAreHost)
	{
		// Create a direct-connect LAN game and wait for the joiner's
		// MSG_REQUEST_GAME_INFO to arrive through the punched mapping.
		UnicodeString gameName = GadgetTextEntryGetText(textEntryPlayerName);
		gameName.concat(L"'s game");
		DEBUG_LOG(("HANDOFF: about to RequestGameCreate"));
		TheLAN->RequestGameCreate(gameName, /*isDirectConnect=*/TRUE);
		DEBUG_LOG(("HANDOFF: RequestGameCreate returned"));
		coordinatorPostStatus("Setting up your game lobby...");
	}
	else
	{
		// We are the joiner. Tell LAN to direct-connect to the punched
		// mapping.
		TheLAN->setDirectConnectRemotePort(peerPort);
		TheLAN->RequestGameJoinDirectConnect(peerIP);
		DEBUG_LOG(("HANDOFF: RequestGameJoinDirectConnect returned"));
		coordinatorPostStatus("Connected to host. Joining the game lobby...");
	}
	ReleaseLog("Coordinator handoff to LAN done (host=%d peer=%u.%u.%u.%u:%u)",
		(int)weAreHost,
		(peerIP >> 24) & 0xff, (peerIP >> 16) & 0xff,
		(peerIP >> 8) & 0xff, peerIP & 0xff, peerPort);
	DEBUG_LOG(("HANDOFF: done"));
}

//-------------------------------------------------------------------------------------------------
/** Lan Lobby menu update method */
//-------------------------------------------------------------------------------------------------
void LanLobbyMenuUpdate( WindowLayout * layout, void *userData)
{
	if (TheGameLogic->isInShellGame() && TheGameLogic->getFrame() == 1)
	{
		SignalUIInteraction(SHELL_SCRIPT_HOOK_LAN_ENTERED_FROM_GAME);
	}

	if(justEntered)
	{
		if(initialGadgetDelay == 1)
		{
			TheTransitionHandler->setGroup("LanLobbyFade");
			initialGadgetDelay = 2;
			justEntered = FALSE;
		}
		else
			initialGadgetDelay--;
	}

	if(LANisShuttingDown && TheShell->isAnimFinished() && TheTransitionHandler->isFinished())
		shutdownComplete(layout);

	if (TheShell->isAnimFinished() && !LANbuttonPushed && TheLAN)
		TheLAN->update();

	if (s_useCoordinator && s_coord && !LANbuttonPushed && !s_coordHandoffDone)
	{
		pumpCoordinator();
	}

	if (LANSocketErrorDetected == TRUE) {
		LANSocketErrorDetected = FALSE;
		DEBUG_LOG(("SOCKET ERROR!  BAILING!"));
		MessageBoxOk(TheGameText->fetch("GUI:NetworkError"), TheGameText->fetch("GUI:SocketError"), nullptr);

		// we have a socket problem, back out to the main menu.
		TheWindowManager->winSendSystemMsg(buttonBack->winGetParent(), GBM_SELECTED,
																			 (WindowMsgData)buttonBack, buttonBackID);
	}


}

//-------------------------------------------------------------------------------------------------
/** Lan Lobby menu input callback */
//-------------------------------------------------------------------------------------------------
WindowMsgHandledType LanLobbyMenuInput( GameWindow *window, UnsignedInt msg,
																			 WindowMsgData mData1, WindowMsgData mData2 )
{
	switch( msg )
	{

		// --------------------------------------------------------------------------------------------
		case GWM_CHAR:
		{
			UnsignedByte key = mData1;
			UnsignedByte state = mData2;
			if (LANbuttonPushed)
				break;

			switch( key )
			{

				// ----------------------------------------------------------------------------------------
				case KEY_ESC:
				{

					//
					// send a simulated selected event to the parent window of the
					// back/exit button
					//
					if( BitIsSet( state, KEY_STATE_UP ) )
					{
						TheWindowManager->winSendSystemMsg( window, GBM_SELECTED,
																							(WindowMsgData)buttonBack, buttonBackID );

					}

					// don't let key fall through anywhere else
					return MSG_HANDLED;

				}

			}

		}

	}

	return MSG_IGNORED;
}

//-------------------------------------------------------------------------------------------------
/** Lan Lobby menu window system callback */
//-------------------------------------------------------------------------------------------------
WindowMsgHandledType LanLobbyMenuSystem( GameWindow *window, UnsignedInt msg,
														 WindowMsgData mData1, WindowMsgData mData2 )
{
	UnicodeString txtInput;

	switch( msg )
	{


		case GWM_CREATE:
			{
				SignalUIInteraction(SHELL_SCRIPT_HOOK_LAN_OPENED);
				break;
			}

		case GWM_DESTROY:
			{
				SignalUIInteraction(SHELL_SCRIPT_HOOK_LAN_CLOSED);
				break;
			}

		case GWM_INPUT_FOCUS:
			{
				// if we're givin the opportunity to take the keyboard focus we must say we want it
				if( mData1 == TRUE )
					*(Bool *)mData2 = TRUE;

				return MSG_HANDLED;
			}
		case GLM_DOUBLE_CLICKED:
			{
				if (LANbuttonPushed)
					break;
				GameWindow *control = (GameWindow *)mData1;
				Int controlID = control->winGetWindowId();
				if( controlID == listboxGamesID )
				{
					int rowSelected = mData2;

					if (s_useCoordinator && s_coord)
					{
						coordinatorJoinOrObserveRow(rowSelected);
					}
					else if (rowSelected >= 0 && TheLAN)
					{
						LANGameInfo * theGame = TheLAN->LookupGameByListOffset(rowSelected);
						if (theGame)
						{
							TheLAN->RequestGameJoin(theGame);
						}
					}
				}
				break;
			}
		case GLM_SELECTED:
			{
				if (LANbuttonPushed)
					break;
				GameWindow *control = (GameWindow *)mData1;
				Int controlID = control->winGetWindowId();
				if( controlID == listboxGamesID )
				{
					int rowSelected = mData2;
					if( rowSelected < 0 )
					{
						HideGameInfoWindow(TRUE);
						break;
					}
					if (s_useCoordinator)
					{
						// Coordinator games don't have a populated LANGameInfo
						// to render in the right-hand details panel; leave it
						// hidden. The row text itself shows id/players/map.
						HideGameInfoWindow(TRUE);
						break;
					}
					LANGameInfo * theGame = TheLAN->LookupGameByListOffset(rowSelected);
					if (theGame)
						RefreshGameInfoWindow(theGame, theGame->getName());
					else
						HideGameInfoWindow(TRUE);

				}
				break;
			}
		case GBM_SELECTED:
			{
				if (LANbuttonPushed)
					break;
				GameWindow *control = (GameWindow *)mData1;
				Int controlID = control->winGetWindowId();

				if ( controlID == buttonBackID )
				{
					//shellmapOn = TRUE;
					LANbuttonPushed = true;
					DEBUG_LOG(("Back was hit - popping to main menu"));
					TheShell->pop();
					delete TheLAN;
					TheLAN = nullptr;
					//TheTransitionHandler->reverse("LanLobbyFade");

				}
				else if ( controlID == buttonHostID )
				{
					if (s_useCoordinator && s_coord)
					{
						connectCoordinatorIfNeeded();
						AsciiString nick = readPlayerNickAscii();
						AsciiString gameName;
						gameName.format("%s's game", nick.str());
						s_coordPendingHostName = gameName;
						s_coordPendingJoinID.clear();
						coordinatorPostStatus("Creating game as soon as the connection is ready...");
					}
					else
					{
						TheLAN->RequestGameCreate( L"", FALSE);
					}
				}
				else if ( controlID == buttonClearID )
				{
					GadgetTextEntrySetText(textEntryPlayerName, UnicodeString::TheEmptyString);
					TheWindowManager->winSendSystemMsg( window,
																						GEM_UPDATE_TEXT,
																						(WindowMsgData)textEntryPlayerName,
																						0 );

				}
				else if ( controlID == buttonJoinID )
				{

					//TheShell->push( "Menus/LanGameOptionsMenu.wnd" );

					int rowSelected = -1;
					GadgetListBoxGetSelected( listboxGames, &rowSelected );

					if (s_useCoordinator && s_coord)
					{
						if (rowSelected < 0 || rowSelected >= (Int)s_coordListedIDs.size())
						{
							GadgetListBoxAddEntryText(listboxChatWindow, TheGameText->fetch("LAN:ErrorNoGameSelected") , chatSystemColor, -1, 0);
						}
						else
						{
							coordinatorJoinOrObserveRow(rowSelected);
						}
					}
					else if (rowSelected >= 0)
					{
						LANGameInfo * theGame = TheLAN->LookupGameByListOffset(rowSelected);
						if (theGame)
						{
							TheLAN->RequestGameJoin(theGame);
						}
					}
					else
					{
						GadgetListBoxAddEntryText(listboxChatWindow, TheGameText->fetch("LAN:ErrorNoGameSelected") , chatSystemColor, -1, 0);
					}

				}
				else if ( controlID == buttonEmoteID )
				{
					// read the user's input
					txtInput.set(GadgetTextEntryGetText( textEntryChat ));
					// Clear the text entry line
					GadgetTextEntrySetText(textEntryChat, UnicodeString::TheEmptyString);
					// Clean up the text (remove leading/trailing chars, etc)
					txtInput.trim();
					// Echo the user's input to the chat window
					if (!txtInput.isEmpty() && TheLAN) {
//						TheLAN->RequestChat(txtInput, LANAPIInterface::LANCHAT_EMOTE);
						TheLAN->RequestChat(txtInput, LANAPIInterface::LANCHAT_NORMAL);
					}
				}
				else if (controlID == buttonDirectConnectID)
				{
					if (TheLAN)
					{
						TheLAN->RequestLobbyLeave( false );
					}
					TheShell->push("Menus/NetworkDirectConnect.wnd");
				}

				break;
			}

		case GEM_UPDATE_TEXT:
			{
				if (LANbuttonPushed)
					break;
				GameWindow *control = (GameWindow *)mData1;
				Int controlID = control->winGetWindowId();

				if ( controlID == textEntryPlayerNameID )
				{
					// grab the user's name
					txtInput.set(GadgetTextEntryGetText( textEntryPlayerName ));

					// Clean up the text (remove leading/trailing chars, etc)
					const WideChar *c = txtInput.str();
					while (c && (iswspace(*c)))
						c++;

					if (c)
						txtInput = UnicodeString(c);
					else
						txtInput = UnicodeString::TheEmptyString;

					txtInput.truncateTo(g_lanPlayerNameLength);

					if (!txtInput.isEmpty() && txtInput.getCharAt(txtInput.getLength()-1) == L',')
						txtInput.removeLastChar(); // we use , for strtok's so we can't allow them in names.  :(

					if (!txtInput.isEmpty() && txtInput.getCharAt(txtInput.getLength()-1) == L':')
						txtInput.removeLastChar(); // we use : for strtok's so we can't allow them in names.  :(

					if (!txtInput.isEmpty() && txtInput.getCharAt(txtInput.getLength()-1) == L';')
						txtInput.removeLastChar(); // we use ; for strtok's so we can't allow them in names.  :(

					// send it over the network (LAN only; coordinator picks up
					// the name from the text entry at connect time)
					if (TheLAN)
					{
						if (!txtInput.isEmpty())
							TheLAN->RequestSetName(txtInput);
						else
							TheLAN->RequestSetName(defaultName);
					}

					// Put the whitespace-free version in the box
					GadgetTextEntrySetText( textEntryPlayerName, txtInput );

				}
				break;
			}
		case GEM_EDIT_DONE:
			{
				if (LANbuttonPushed)
					break;
				GameWindow *control = (GameWindow *)mData1;
				Int controlID = control->winGetWindowId();

				// Take the user's input and echo it into the chat window as well as
				// send it to the other clients on the lan
				if ( controlID == textEntryChatID )
				{

					// read the user's input
					txtInput.set(GadgetTextEntryGetText( textEntryChat ));
					// Clear the text entry line
					GadgetTextEntrySetText(textEntryChat, UnicodeString::TheEmptyString);
					// Clean up the text (remove leading/trailing chars, etc)
					while (!txtInput.isEmpty() && iswspace(txtInput.getCharAt(0)))
						txtInput = UnicodeString(txtInput.str()+1);

					// Echo the user's input to the chat window
					if (!txtInput.isEmpty() && TheLAN)
						TheLAN->RequestChat(txtInput, LANAPIInterface::LANCHAT_NORMAL);

				}
				/*
				else if ( controlID == textEntryPlayerNameID )
				{
					// grab the user's name
					txtInput.set(GadgetTextEntryGetText( textEntryPlayerName ));

					// Clean up the text (remove leading/trailing chars, etc)
					txtInput.trim();

					// send it over the network
					if (!txtInput.isEmpty())
						TheLAN->RequestSetName(txtInput);

					// Put the whitespace-free version in the box
					GadgetTextEntrySetText( textEntryPlayerName, txtInput );

				}
				*/
				break;
			}
		default:
			return MSG_IGNORED;

	}

	return MSG_HANDLED;
}
