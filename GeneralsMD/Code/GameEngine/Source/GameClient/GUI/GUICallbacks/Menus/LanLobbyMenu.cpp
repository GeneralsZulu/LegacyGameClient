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
#include "Common/GlobalData.h"
#include "Common/MultiplayerSettings.h"
#include "Common/NameKeyGenerator.h"
#include "Common/Player.h"
#include "Common/PlayerTemplate.h"
#include "Common/QuotedPrintable.h"
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

static const char* COORD_HOST_DEFAULT = "cncstats.computersrfun.org";
static const UnsignedShort COORD_TCP_PORT_DEFAULT = 27500;
static const UnsignedInt   COORD_LIST_REFRESH_MS  = 5000;

void LanLobbyMenuSetUseCoordinator( Bool enable )
{
	s_useCoordinator = enable;
}

// Forward declarations for the coordinator helpers; the definitions live
// just above LanLobbyMenuUpdate further down in this file.
static void connectCoordinatorIfNeeded();
static void pumpCoordinator();



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

AsciiString LANPreferences::getPreferredMap()
{
	AsciiString ret;
	LANPreferences::const_iterator it = find("Map");
	if (it == end())
	{
		ret = getDefaultMap(TRUE);
		return ret;
	}

	ret = QuotedPrintableToAsciiString(it->second);
	ret.trim();
	if (ret.isEmpty() || !isValidMap(ret, TRUE))
	{
		ret = getDefaultMap(TRUE);
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

// hack to disable framerate limiter in LAN games
//static Bool shellmapOn;
static Bool useFpsLimit;
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
		useFpsLimit = TheGlobalData->m_useFpsLimit;
	}
	else if (!TheLAN)
	{
		TheLAN = NEW LANAPI();	/// @todo clh delete TheLAN and
		useFpsLimit = TheGlobalData->m_useFpsLimit;
	}
	else
	{
		TheWritableGlobalData->m_useFpsLimit = useFpsLimit;
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
		s_coord->disconnect();
		delete s_coord;
		s_coord = nullptr;
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
	TheWritableGlobalData->m_useFpsLimit = useFpsLimit;

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

static void connectCoordinatorIfNeeded()
{
	if (!s_coord) return;
	if (s_coord->state() != OnlineCoordinatorAPI::STATE_IDLE &&
	    s_coord->state() != OnlineCoordinatorAPI::STATE_ERROR)
	{
		return;
	}
	s_coordCurrentNick = readPlayerNickAscii();
	AsciiString host = COORD_HOST_DEFAULT;
	// Bind UDP/8086 (lobby) so the punched NAT mapping is on the port the LAN
	// code will rebind after PUNCH_OK, AND UDP/8088 (NETWORK_BASE_PORT_NUMBER,
	// in-game data) so ConnectionManager's later socket inherits an already-
	// punched mapping. The TCP signaling port is the listed coordinator port;
	// UDP STUN is on the port reported in hello_ok.
	if (!s_coord->connect(host, COORD_TCP_PORT_DEFAULT,
		s_coordCurrentNick, AsciiString("zulu/1"),
		/*lobbyBindPort=*/8086, /*gameBindPort=*/NETWORK_BASE_PORT_NUMBER))
	{
		DEBUG_LOG(("connectCoordinatorIfNeeded: connect failed: %s",
			s_coord->lastError().str()));
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
		row.format("%s   [%s]   %d/%d   map:%s   id:%s",
			g.name.str(), g.hostNick.str(), g.players, g.maxPlayers,
			g.map.str(), g.id.str());
		UnicodeString u;
		u.translate(row);
		GadgetListBoxAddEntryText(listboxGames, u, textColor, -1, 0);
		s_coordListedIDs.push_back(g.id);
	}
}

static void coordinatorPostStatus(const char* msg)
{
	UnicodeString u;
	u.translate(AsciiString(msg));
	GadgetListBoxAddEntryText(listboxChatWindow, u, GameMakeColor(180, 180, 255, 255), -1, 0);
}

static void doCoordinatorHandoffToLAN();

static void pumpCoordinator()
{
	OnlineCoordinatorAPI::State prevState = s_coord->state();
	s_coord->update();
	OnlineCoordinatorAPI::State state = s_coord->state();

	if (state != prevState)
	{
		AsciiString msg;
		msg.format("coordinator state -> %d", (Int)state);
		coordinatorPostStatus(msg.str());
		if (state == OnlineCoordinatorAPI::STATE_READY)
		{
			AsciiString s;
			s.format("public addr: %s", s_coord->publicAddr().str());
			coordinatorPostStatus(s.str());
			// Immediately fetch games on first READY transition.
			s_coord->requestList();
			s_coordLastListMs = timeGetTime();
		}
		if (state == OnlineCoordinatorAPI::STATE_ERROR)
		{
			AsciiString s;
			s.format("error: %s", s_coord->lastError().str());
			coordinatorPostStatus(s.str());
		}
		if (state == OnlineCoordinatorAPI::STATE_PUNCH_OK && !s_coordHandoffDone)
		{
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

	// When READY, periodically refresh the games list and dispatch any
	// pending host/join action that was queued before the connection
	// finished handshaking.
	if (state == OnlineCoordinatorAPI::STATE_READY)
	{
		UnsignedInt nowMs = timeGetTime();
		if (nowMs - s_coordLastListMs > COORD_LIST_REFRESH_MS)
		{
			s_coord->requestList();
			s_coordLastListMs = nowMs;
		}
		if (!s_coordPendingHostName.isEmpty())
		{
			UnicodeString u; u.translate(s_coordPendingHostName);
			s_coord->requestHost(u, AsciiString("unknown"), 2);
			s_coordPendingHostName.clear();
		}
		else if (!s_coordPendingJoinID.isEmpty())
		{
			s_coord->requestJoin(s_coordPendingJoinID);
			s_coordPendingJoinID.clear();
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
	DEBUG_LOG(("HANDOFF: start weAreHost=%d peerIP=0x%08x peerPort=%u gamePeerIP=0x%08x gamePeerPort=%u",
		(int)weAreHost, peerIP, peerPort, peer.gamePunchedIP, peer.gamePunchedPort));

	// Move the punched game UDP socket into the module-level stash so it
	// outlives the coordinator (and this menu); a keepalive sender keeps
	// the NAT mapping alive through the LAN lobby phase. ConnectionManager
	// adopts the FD at game start.
	if (!s_coord->stashGameSocketForGameStart())
	{
		DEBUG_LOG(("HANDOFF: WARNING: failed to stash game socket; in-game NAT traversal will fail"));
	}

	// Release the coordinator's lobby UDP socket so TheLAN can rebind 8086.
	// The NAT mapping established during punch persists for ~30s+, well
	// within the few ms it takes us to rebind.
	s_coord->disconnect();
	DEBUG_LOG(("HANDOFF: coord disconnected"));

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
	DEBUG_LOG(("HANDOFF: localIP=0x%08x", localIP));
	if (!TheLAN->SetLocalIP(localIP))
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
		coordinatorPostStatus("punch ok; hosting via direct-connect LAN");
	}
	else
	{
		// We are the joiner. Tell LAN to direct-connect to the punched
		// mapping.
		TheLAN->setDirectConnectRemotePort(peerPort);
		TheLAN->RequestGameJoinDirectConnect(peerIP);
		DEBUG_LOG(("HANDOFF: RequestGameJoinDirectConnect returned"));
		AsciiString s;
		s.format("punch ok; joining %u.%u.%u.%u:%u via direct connect",
			(peerIP >> 24) & 0xff, (peerIP >> 16) & 0xff,
			(peerIP >> 8) & 0xff, (peerIP) & 0xff, peerPort);
		coordinatorPostStatus(s.str());
	}
	s_coordHandoffDone = TRUE;
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
						if (rowSelected >= 0 && rowSelected < (Int)s_coordListedIDs.size())
						{
							connectCoordinatorIfNeeded();
							s_coordPendingJoinID = s_coordListedIDs[rowSelected];
							s_coordPendingHostName.clear();
						}
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
						coordinatorPostStatus("queued host request; awaiting STUN");
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
							connectCoordinatorIfNeeded();
							s_coordPendingJoinID = s_coordListedIDs[rowSelected];
							s_coordPendingHostName.clear();
							AsciiString status;
							status.format("queued join %s; awaiting STUN", s_coordPendingJoinID.str());
							coordinatorPostStatus(status.str());
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
