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

// FILE: ControlBarObserver.cpp /////////////////////////////////////////////////
//-----------------------------------------------------------------------------
//
//                       Electronic Arts Pacific.
//
//                       Confidential Information
//                Copyright (C) 2002 - All Rights Reserved
//
//-----------------------------------------------------------------------------
//
//	created:	Aug 2002
//
//	Filename: 	ControlBarObserver.cpp
//
//	author:		Chris Huybregts
//
//	purpose:	All things related to the Observer Control bar, are in here.
//
//-----------------------------------------------------------------------------
///////////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------------
// SYSTEM INCLUDES ////////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// USER INCLUDES //////////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/GameUtility.h"
#include "Common/NameKeyGenerator.h"
#include "Common/PlayerList.h"
#include "Common/Player.h"
#include "Common/PlayerTemplate.h"
#include "Common/KindOf.h"
#include "Common/Recorder.h"
#include "Common/ScoreKeeper.h"
#include "GameClient/ControlBar.h"
#include "GameClient/Display.h"
#include "GameClient/GameFont.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/GadgetPushButton.h"
#include "GameClient/GadgetStaticText.h"
#include "GameClient/GameText.h"
#include "GameClient/GlobalLanguage.h"
#include "GameNetwork/NetworkDefs.h"
//-----------------------------------------------------------------------------
// DEFINES ////////////////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------
enum { MAX_BUTTONS = 8};
static NameKeyType buttonPlayerID[MAX_BUTTONS] = { NAMEKEY_INVALID,NAMEKEY_INVALID,
																										NAMEKEY_INVALID,NAMEKEY_INVALID,
																										NAMEKEY_INVALID,NAMEKEY_INVALID,
																										NAMEKEY_INVALID,NAMEKEY_INVALID };
static NameKeyType staticTextPlayerID[MAX_BUTTONS] = { NAMEKEY_INVALID,NAMEKEY_INVALID,
																										NAMEKEY_INVALID,NAMEKEY_INVALID,
																										NAMEKEY_INVALID,NAMEKEY_INVALID,
																										NAMEKEY_INVALID,NAMEKEY_INVALID };
static GameWindow *ObserverPlayerInfoWindow = nullptr;
static GameWindow *ObserverPlayerListWindow = nullptr;

static GameWindow *buttonPlayer[MAX_BUTTONS] = {0};
static GameWindow *staticTextPlayer[MAX_BUTTONS] = {0};


static NameKeyType buttonCancelID = NAMEKEY_INVALID;

static GameWindow *winFlag = nullptr;
static GameWindow *winGeneralPortrait = nullptr;
// TheSuperHackers @tweak Allow idle worker selection for observers.
static GameWindow *buttonIdleWorker = nullptr;
static GameWindow *staticTextNumberOfUnits = nullptr;
static GameWindow *staticTextNumberOfBuildings = nullptr;
static GameWindow *staticTextNumberOfUnitsKilled = nullptr;
static GameWindow *staticTextNumberOfUnitsLost = nullptr;
static GameWindow *staticTextPlayerName = nullptr;
// TRUE when Window/ZuluObserverBar.wnd loaded and supplies the observer
// windows (set in initObserverControls)
static Bool s_useZuluObserverBar = FALSE;

// saved window rects for restoring the installed HUD layout when leaving
// observer mode (radar / right HUD subtrees)
struct ZuluSavedWindowRect
{
	GameWindow *win;
	ICoord2D pos;
	ICoord2D size;
};
enum { MAX_ZULU_SAVED_RECTS = 64 };
static ZuluSavedWindowRect s_zuluHudSavedRects[MAX_ZULU_SAVED_RECTS];
static Int s_zuluHudSavedRectCount = 0;
static Bool s_zuluHudLayoutApplied = FALSE;

static NameKeyType s_replayObserverNameKey = NAMEKEY_INVALID;

//-----------------------------------------------------------------------------
// PUBLIC FUNCTIONS ///////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------

//-------------------------------------------------------------------------------------------------
/** The .wnd loader lets a HEADERTEMPLATE win over the FONT entry of a window, and the header
	* templates come from Data\<language>\HeaderTemplate.ini, which ControlBarPro-style addon bigs
	* replace with inflated point sizes (the 1440 pack: LabelSmall 8 -> 11, ButtonSmall 10 -> 14).
	* On top of that, header template fonts are resolution-scaled, so the observer info panel ended
	* up with text far too big for its rows on any install carrying such a big. Pin the panel's
	* fonts here, keyed by window name, so the panel reads the same everywhere. The sizes still go
	* through adjustFontSize, which is what the header templates do, so the text keeps scaling with
	* the resolution exactly like the panel rects do. */
//-------------------------------------------------------------------------------------------------
static GameFont *getObserverBarFont( const AsciiString &decoratedName )
{
	if( TheFontLibrary == nullptr || TheGlobalLanguageData == nullptr || decoratedName.isEmpty() )
		return nullptr;

	const char *shortName = strchr( decoratedName.str(), ':' );
	if( shortName == nullptr || *(++shortName) == '\0' )
		return nullptr;

	// the player name banner across the top of the info panel
	if( strcmp( shortName, "StaticTextPlayerName" ) == 0 )
		return TheFontLibrary->getFont( AsciiString("Arial"), TheGlobalLanguageData->adjustFontSize( 10 ), TRUE );

	// the stacked [number][label] readout rows, and the button under them
	if( strncmp( shortName, "StaticText", 10 ) == 0
			|| strcmp( shortName, "ButtonCancel" ) == 0 )
		return TheFontLibrary->getFont( AsciiString("Arial"), TheGlobalLanguageData->adjustFontSize( 8 ), FALSE );

	return nullptr;
}

//-------------------------------------------------------------------------------------------------
static void enforceObserverBarFonts( GameWindow *window )
{
	if( window == nullptr )
		return;

	GameFont *font = getObserverBarFont( window->winGetInstanceData()->m_decoratedNameString );
	if( font )
		window->winSetFont( font );

	GameWindow *child;
	for( child = window->winGetChild(); child; child = child->winGetNext() )
		enforceObserverBarFonts( child );
}

void ControlBar::initObserverControls()
{
	//
	// The Zulu observer bar is a standalone window file layered over whatever
	// ControlBar.wnd the player has installed (stock, ControlBarPro, ...), so
	// every viewer gets the same observer windows without touching the
	// in-game bar. When the file is missing we fall back to the observer
	// windows inside ControlBar.wnd.
	//
	const char *prefix = "ControlBar.wnd";
	const NameKeyType zuluInfoKey = TheNameKeyGenerator->nameToKey("ZuluObserverBar.wnd:ObserverPlayerInfoWindow");
	if (TheWindowManager->winGetWindowFromId(nullptr, zuluInfoKey) == nullptr)
		TheWindowManager->winCreateFromScript(AsciiString("ZuluObserverBar.wnd"));
	s_useZuluObserverBar = TheWindowManager->winGetWindowFromId(nullptr, zuluInfoKey) != nullptr;
	const Bool useZuluObserverBar = s_useZuluObserverBar;
	if (useZuluObserverBar)
		prefix = "ZuluObserverBar.wnd";

	AsciiString tmpString;
	tmpString.format("%s:ObserverPlayerInfoWindow", prefix);
	ObserverPlayerInfoWindow = TheWindowManager->winGetWindowFromId(nullptr, TheNameKeyGenerator->nameToKey(tmpString));
	tmpString.format("%s:ObserverPlayerListWindow", prefix);
	ObserverPlayerListWindow = TheWindowManager->winGetWindowFromId(nullptr, TheNameKeyGenerator->nameToKey(tmpString));

	if (useZuluObserverBar)
	{
		// the ControlBar.wnd observer windows are superseded: hide them for
		// good and route the context switching at our windows instead
		GameWindow *original = TheWindowManager->winGetWindowFromId(nullptr, TheNameKeyGenerator->nameToKey("ControlBar.wnd:ObserverPlayerInfoWindow"));
		if (original)
			original->winHide(TRUE);
		original = TheWindowManager->winGetWindowFromId(nullptr, TheNameKeyGenerator->nameToKey("ControlBar.wnd:ObserverPlayerListWindow"));
		if (original)
			original->winHide(TRUE);
		m_contextParent[ CP_OBSERVER_INFO ] = ObserverPlayerInfoWindow;
		m_contextParent[ CP_OBSERVER_LIST ] = ObserverPlayerListWindow;
	}

	for (Int i = 0; i < MAX_BUTTONS; i++)
	{
		tmpString.format("%s:ButtonPlayer%d", prefix, i);
		buttonPlayerID[i] = TheNameKeyGenerator->nameToKey( tmpString );
		buttonPlayer[i] = TheWindowManager->winGetWindowFromId( ObserverPlayerListWindow, buttonPlayerID[i] );
		tmpString.format("%s:StaticTextPlayer%d", prefix, i);
		staticTextPlayerID[i] = TheNameKeyGenerator->nameToKey( tmpString );
		staticTextPlayer[i] = TheWindowManager->winGetWindowFromId( ObserverPlayerListWindow, staticTextPlayerID[i] );
	}

	tmpString.format("%s:StaticTextNumberOfUnits", prefix);
	staticTextNumberOfUnits = TheWindowManager->winGetWindowFromId(nullptr, TheNameKeyGenerator->nameToKey(tmpString));
	tmpString.format("%s:StaticTextNumberOfBuildings", prefix);
	staticTextNumberOfBuildings = TheWindowManager->winGetWindowFromId(nullptr, TheNameKeyGenerator->nameToKey(tmpString));
	tmpString.format("%s:StaticTextNumberOfUnitsKilled", prefix);
	staticTextNumberOfUnitsKilled = TheWindowManager->winGetWindowFromId(nullptr, TheNameKeyGenerator->nameToKey(tmpString));
	tmpString.format("%s:StaticTextNumberOfUnitsLost", prefix);
	staticTextNumberOfUnitsLost = TheWindowManager->winGetWindowFromId(nullptr, TheNameKeyGenerator->nameToKey(tmpString));
	tmpString.format("%s:StaticTextPlayerName", prefix);
	staticTextPlayerName = TheWindowManager->winGetWindowFromId(nullptr, TheNameKeyGenerator->nameToKey(tmpString));
	tmpString.format("%s:WinFlag", prefix);
	winFlag = TheWindowManager->winGetWindowFromId(nullptr, TheNameKeyGenerator->nameToKey(tmpString));
	tmpString.format("%s:WinGeneralPortrait", prefix);
	winGeneralPortrait = TheWindowManager->winGetWindowFromId(nullptr, TheNameKeyGenerator->nameToKey(tmpString));
	// the idle worker button stays with the installed control bar layout
	buttonIdleWorker = TheWindowManager->winGetWindowFromId(nullptr, TheNameKeyGenerator->nameToKey("ControlBar.wnd:ButtonIdleWorker"));

	tmpString.format("%s:ButtonCancel", prefix);
	buttonCancelID = TheNameKeyGenerator->nameToKey(tmpString);

	// whatever HeaderTemplate.ini the installed bigs provide, the observer panel keeps our fonts
	enforceObserverBarFonts( ObserverPlayerInfoWindow );
	enforceObserverBarFonts( ObserverPlayerListWindow );

	//
	// The stock labels ("Units Destroyed", "Units Lost") wrap onto a second line inside the
	// panel's one-line rows and collide with the row below. Give the rows short single-word
	// labels instead; the numbers next to them carry the meaning.
	//
	static const struct
	{
		const char *window;
		const char *label;
		const WideChar *fallback;
	}
	rowLabels[] =
	{
		{ "StaticTextObsUnits",				"CONTROLBAR:ObsLabelUnits",			L"Units"			},
		{ "StaticTextObsBuildings",		"CONTROLBAR:ObsLabelBuildings",	L"Buildings"	},
		{ "StaticTextObsUnitsKilled",	"CONTROLBAR:ObsLabelKills",			L"Kills"			},
		{ "StaticTextObsUnitsLost",		"CONTROLBAR:ObsLabelLosses",		L"Losses"			},
	};

	for (Int row = 0; row < ARRAY_SIZE(rowLabels); ++row)
	{
		tmpString.format("%s:%s", prefix, rowLabels[row].window);
		GameWindow *labelWindow = TheWindowManager->winGetWindowFromId(nullptr, TheNameKeyGenerator->nameToKey(tmpString));
		if (labelWindow == nullptr)
			continue;

		// the control bar comes up before the string table is guaranteed to be there, so keep
		// the shipped label as a fallback rather than blanking the row
		UnicodeString label;
		label.set(rowLabels[row].fallback);
		if (TheGameText)
		{
			Bool exists = FALSE;
			const UnicodeString translated = TheGameText->fetch(rowLabels[row].label, &exists);
			if (exists && !translated.isEmpty())
				label = translated;
		}
		GadgetStaticTextSetText(labelWindow, label);
	}

	s_replayObserverNameKey = TheNameKeyGenerator->nameToKey("ReplayObserver");
}

//-------------------------------------------------------------------------------------------------
Bool ControlBar::isUsingZuluObserverBar() const
{
	return s_useZuluObserverBar;
}

//-------------------------------------------------------------------------------------------------
static void saveWindowRectRecursive(GameWindow *win)
{
	if (win == nullptr || s_zuluHudSavedRectCount >= MAX_ZULU_SAVED_RECTS)
		return;
	ZuluSavedWindowRect &saved = s_zuluHudSavedRects[s_zuluHudSavedRectCount++];
	saved.win = win;
	win->winGetPosition(&saved.pos.x, &saved.pos.y);
	win->winGetSize(&saved.size.x, &saved.size.y);
	for (GameWindow *child = win->winGetChild(); child; child = child->winGetNext())
		saveWindowRectRecursive(child);
}

//-------------------------------------------------------------------------------------------------
static void scaleChildrenRecursive(GameWindow *win, Real scaleX, Real scaleY)
{
	for (GameWindow *child = win->winGetChild(); child; child = child->winGetNext())
	{
		Int x, y, width, height;
		child->winGetPosition(&x, &y);
		child->winGetSize(&width, &height);
		child->winSetPosition(REAL_TO_INT(x * scaleX), REAL_TO_INT(y * scaleY));
		child->winSetSize(REAL_TO_INT(width * scaleX), REAL_TO_INT(height * scaleY));
		scaleChildrenRecursive(child, scaleX, scaleY);
	}
}

//-------------------------------------------------------------------------------------------------
/** Move win (and proportionally its children) so it covers the given rect,
	* authored against a 1920x1080 screen, scaled to the actual resolution.
	* The previous rects are saved for restoring later. */
//-------------------------------------------------------------------------------------------------
static void moveWindowTo1080Rect(GameWindow *win, Int x0, Int y0, Int x1, Int y1)
{
	if (win == nullptr)
		return;

	saveWindowRectRecursive(win);

	const Real scaleToScreenX = TheDisplay->getWidth() / 1920.0f;
	const Real scaleToScreenY = TheDisplay->getHeight() / 1080.0f;
	const Int targetX = REAL_TO_INT(x0 * scaleToScreenX);
	const Int targetY = REAL_TO_INT(y0 * scaleToScreenY);
	const Int targetWidth = REAL_TO_INT((x1 - x0) * scaleToScreenX);
	const Int targetHeight = REAL_TO_INT((y1 - y0) * scaleToScreenY);

	Int oldWidth, oldHeight;
	win->winGetSize(&oldWidth, &oldHeight);

	Int parentX = 0, parentY = 0;
	GameWindow *parent = win->winGetParent();
	if (parent)
		parent->winGetScreenPosition(&parentX, &parentY);

	win->winSetPosition(targetX - parentX, targetY - parentY);
	win->winSetSize(targetWidth, targetHeight);
	if (oldWidth > 0 && oldHeight > 0)
		scaleChildrenRecursive(win, targetWidth / (Real)oldWidth, targetHeight / (Real)oldHeight);
}

//-------------------------------------------------------------------------------------------------
/** The Zulu observer bar art (drawn by the ZuluObserver control bar scheme)
	* frames the radar at the lower left and the unit portrait at the lower
	* right, but those windows sit wherever the installed ControlBar.wnd puts
	* them. Move them into the art's frames while the observer bar is up and
	* restore the installed layout when leaving it. Rects are authored
	* against the 1920x1080 ControlBarPro original. */
//-------------------------------------------------------------------------------------------------
void ControlBar::applyZuluObserverHudLayout(Bool enable)
{
	if (!s_useZuluObserverBar)
		return;
	if (enable == s_zuluHudLayoutApplied)
		return;
	s_zuluHudLayoutApplied = enable;

	// the generals experience bar sits at a layout-specific spot and means
	// nothing to a viewer; hide it while the observer bar is up
	GameWindow *expBar = TheWindowManager->winGetWindowFromId(nullptr, TheNameKeyGenerator->nameToKey("ControlBar.wnd:GeneralsExp"));
	if (expBar)
		expBar->winHide(enable);
	expBar = TheWindowManager->winGetWindowFromId(nullptr, TheNameKeyGenerator->nameToKey("ControlBar.wnd:ExpBarForeground"));
	if (expBar)
		expBar->winHide(enable);

	if (enable)
	{
		s_zuluHudSavedRectCount = 0;
		moveWindowTo1080Rect(TheWindowManager->winGetWindowFromId(nullptr, TheNameKeyGenerator->nameToKey("ControlBar.wnd:LeftHUD")), 5, 753, 327, 1075);
		moveWindowTo1080Rect(TheWindowManager->winGetWindowFromId(nullptr, TheNameKeyGenerator->nameToKey("ControlBar.wnd:RightHUD")), 1642, 887, 1871, 1071);
	}
	else
	{
		for (Int i = 0; i < s_zuluHudSavedRectCount; ++i)
		{
			const ZuluSavedWindowRect &saved = s_zuluHudSavedRects[i];
			saved.win->winSetPosition(saved.pos.x, saved.pos.y);
			saved.win->winSetSize(saved.size.x, saved.size.y);
		}
		s_zuluHudSavedRectCount = 0;
	}
}

//-------------------------------------------------------------------------------------------------
/** The Zulu observer bar art leaves the middle of the screen open. When the
	* viewer selects something, the context windows appear over the bare
	* world, so draw a fitted panel behind each visible context window to
	* keep them readable. Runs from the W3DCommandBarBackgroundDraw hook
	* after the scheme background, i.e. underneath the windows. */
//-------------------------------------------------------------------------------------------------
void ControlBar::drawObserverContextBackdrop()
{
	// the observer player list / info windows carry their own art
	if( m_currContext == CB_CONTEXT_NONE ||
			m_currContext == CB_CONTEXT_OBSERVER_LIST ||
			m_currContext == CB_CONTEXT_OBSERVER_INFO )
		return;

	static const Color fillColor = GameMakeColor( 0, 0, 0, 170 );
	static const Color borderColor = GameMakeColor( 110, 110, 110, 200 );
	const Int pad = 4;

	GameWindow *panels[] =
	{
		m_contextParent[ CP_COMMAND ],
		m_contextParent[ CP_BUILD_QUEUE ],
		m_contextParent[ CP_BEACON ],
		m_contextParent[ CP_UNDER_CONSTRUCTION ],
		m_contextParent[ CP_OCL_TIMER ],
		m_rightHUDUnitSelectParent,
	};

	for( Int i = 0; i < ARRAY_SIZE( panels ); ++i )
	{
		GameWindow *win = panels[ i ];
		if( win == nullptr || win->winIsHidden() )
			continue;

		ICoord2D pos, size;
		win->winGetScreenPosition( &pos.x, &pos.y );
		win->winGetSize( &size.x, &size.y );
		TheDisplay->drawFillRect( pos.x - pad, pos.y - pad, size.x + 2 * pad, size.y + 2 * pad, fillColor );
		TheDisplay->drawOpenRect( pos.x - pad, pos.y - pad, size.x + 2 * pad, size.y + 2 * pad, 1.0f, borderColor );
	}
}

//-------------------------------------------------------------------------------------------------
void ControlBar::setObserverLookAtPlayer(Player *player)
{
	if (player != nullptr && player == ThePlayerList->findPlayerWithNameKey(s_replayObserverNameKey))
	{
		// Looking at the observer. Treat as not looking at player.
		m_observerLookAtPlayer = nullptr;
	}
	else
	{
		m_observerLookAtPlayer = player;
	}
}

//-------------------------------------------------------------------------------------------------
void ControlBar::setObservedPlayer(Player *player)
{
	if (player != nullptr && player == ThePlayerList->findPlayerWithNameKey(s_replayObserverNameKey))
	{
		// Looking at the observer. Treat as not observing player.
		m_observedPlayer = nullptr;
	}
	else
	{
		m_observedPlayer = player;
	}
}

//-------------------------------------------------------------------------------------------------
/** System callback for the ControlBarObserverSystem */
//-------------------------------------------------------------------------------------------------
WindowMsgHandledType ControlBarObserverSystem( GameWindow *window, UnsignedInt msg,
																			 WindowMsgData mData1, WindowMsgData mData2 )
{
	static NameKeyType buttonCommunicator = NAMEKEY_INVALID;

	switch( msg )
	{
		// --------------------------------------------------------------------------------------------
		case GWM_CREATE:
		{
				break;

		}

		//---------------------------------------------------------------------------------------------
		case GBM_MOUSE_ENTERING:
		case GBM_MOUSE_LEAVING:
		{
			break;
		}

		//---------------------------------------------------------------------------------------------
		case GBM_SELECTED:
		case GBM_SELECTED_RIGHT:
		{
			GameWindow *control = (GameWindow *)mData1;

			Int controlID = control->winGetWindowId();
			if( controlID == buttonCancelID)
			{
				rts::changeObservedPlayer(nullptr);

				ObserverPlayerInfoWindow->winHide(TRUE);
				ObserverPlayerListWindow->winHide(FALSE);
				buttonIdleWorker->winHide(TRUE);
				TheControlBar->populateObserverList();
			}

			for(Int i = 0; i <MAX_BUTTONS; ++i)
			{
				if( controlID == buttonPlayerID[i])
				{
					Player* player = static_cast<Player*>(GadgetButtonGetData(buttonPlayer[i]));
					rts::changeObservedPlayer(player);

					ObserverPlayerInfoWindow->winHide(FALSE);
					ObserverPlayerListWindow->winHide(TRUE);

					if(TheControlBar->getObserverLookAtPlayer())
						TheControlBar->populateObserverInfoWindow();

					return MSG_HANDLED;
				}
			}

		//	if( controlID == buttonCommunicator && TheGameLogic->getGameMode() == GAME_INTERNET )
	/*
		{
				popupCommunicatorLayout = TheWindowManager->winCreateLayout( "Menus/PopupCommunicator.wnd" );
				popupCommunicatorLayout->runInit();
				popupCommunicatorLayout->hide( FALSE );
				popupCommunicatorLayout->bringForward();
			}
*/

			break;

		}

		//---------------------------------------------------------------------------------------------
		default:
			return MSG_IGNORED;

	}

	return MSG_HANDLED;

}

//-----------------------------------------------------------------------------
// PRIVATE FUNCTIONS //////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------

void ControlBar::populateObserverList()
{
	Int currentButton = 0, i;
	if(TheRecorder->isMultiplayer())
	{

		for (i = 0; i < MAX_SLOTS; ++i)
		{
			AsciiString name;
			name.format("player%d", i);
			Player *p = ThePlayerList->findPlayerWithNameKey(TheNameKeyGenerator->nameToKey(name));
			if(p)
			{
				if(p->isPlayerObserver())
					continue;
				DEBUG_ASSERTCRASH(currentButton < MAX_BUTTONS, ("ControlBar::populateObserverList trying to populate more buttons then we have"));
				GadgetButtonSetData(buttonPlayer[currentButton], (void *)p);
				GadgetButtonSetEnabledImage( buttonPlayer[currentButton], p->getPlayerTemplate()->getEnabledImage() );
				//GadgetButtonSetHiliteImage( buttonPlayer[currentButton], p->getPlayerTemplate()->getHiliteImage() );
				//GadgetButtonSetHiliteSelectedImage( buttonPlayer[currentButton], p->getPlayerTemplate()->getPushedImage() );
				//GadgetButtonSetDisabledImage( buttonPlayer[currentButton], p->getPlayerTemplate()->getDisabledImage() );
				buttonPlayer[currentButton]->winSetTooltip(p->getPlayerDisplayName());
				buttonPlayer[currentButton]->winHide(FALSE);
				buttonPlayer[currentButton]->winSetStatus( WIN_STATUS_USE_OVERLAY_STATES );

				const GameSlot *slot = TheGameInfo->getConstSlot(i);
				Color playerColor = p->getPlayerColor();
				Color backColor = GameMakeColor(0, 0, 0, 255);
				staticTextPlayer[currentButton]->winSetEnabledTextColors( playerColor, backColor );
				staticTextPlayer[currentButton]->winHide(FALSE);
				AsciiString teamStr;
				teamStr.format("Team:%d", slot->getTeamNumber() + 1);
				if (slot->isAI() && slot->getTeamNumber() == -1)
					teamStr = "Team:AI";

				UnicodeString text;
				text.format(TheGameText->fetch("CONTROLBAR:ObsPlayerLabel"), p->getPlayerDisplayName().str(),
					TheGameText->fetch(teamStr).str());

				GadgetStaticTextSetText(staticTextPlayer[currentButton], text );

				++currentButton;
			}
		}
		for(currentButton; currentButton<MAX_BUTTONS; ++currentButton)
		{
			buttonPlayer[currentButton]->winHide(TRUE);
			staticTextPlayer[currentButton]->winHide(TRUE);
		}
	}
	else
	{
		for(i =0; i < MAX_PLAYER_COUNT; ++i)
		{
			Player *p = ThePlayerList->getNthPlayer(i);
			if(p && !p->isPlayerObserver() && p->getPlayerType() == PLAYER_HUMAN)
			{
				DEBUG_ASSERTCRASH(currentButton < MAX_BUTTONS, ("ControlBar::populateObserverList trying to populate more buttons then we have"));
				GadgetButtonSetData(buttonPlayer[currentButton], (void *)p);
				GadgetButtonSetEnabledImage( buttonPlayer[currentButton], p->getPlayerTemplate()->getEnabledImage() );
				//GadgetButtonSetHiliteImage( buttonPlayer[currentButton], p->getPlayerTemplate()->getHiliteImage() );
				//GadgetButtonSetHiliteSelectedImage( buttonPlayer[currentButton], p->getPlayerTemplate()->getPushedImage() );
				//GadgetButtonSetDisabledImage( buttonPlayer[currentButton], p->getPlayerTemplate()->getDisabledImage() );
				buttonPlayer[currentButton]->winSetTooltip(p->getPlayerDisplayName());
				buttonPlayer[currentButton]->winHide(FALSE);
				buttonPlayer[currentButton]->winSetStatus( WIN_STATUS_USE_OVERLAY_STATES );

				Color playerColor = p->getPlayerColor();
				Color backColor = GameMakeColor(0, 0, 0, 255);
				staticTextPlayer[currentButton]->winSetEnabledTextColors( playerColor, backColor );
				staticTextPlayer[currentButton]->winHide(FALSE);
				GadgetStaticTextSetText(staticTextPlayer[currentButton], p->getPlayerDisplayName());

				++currentButton;
				break;
			}
		}
		for(currentButton; currentButton<MAX_BUTTONS; ++currentButton)
		{
			buttonPlayer[currentButton]->winHide(TRUE);
			staticTextPlayer[currentButton]->winHide(TRUE);
		}
	}
}

//-------------------------------------------------------------------------------------------------
/** While the observer player table is up it lists every player already, so the strip's name labels
	* are redundant - and worse, the table is drawn over the strip, so the labels it does not fully
	* cover get sliced through the middle and read as a rendering glitch. Hide just the labels while
	* the table is up. The flag buttons stay visible and clickable: with nothing selected they are
	* the only way for the observer to enter a player's view, so they must never go away.
	*
	* Restoring only unhides labels whose flag button is showing, so the empty slots that
	* populateObserverList() hid stay hidden. */
//-------------------------------------------------------------------------------------------------
void ControlBar::setObserverPlayerNamesHidden(Bool hide)
{
	Int i;
	for (i = 0; i < MAX_BUTTONS; ++i)
	{
		if (staticTextPlayer[i] == nullptr)
			continue;

		if (hide)
			staticTextPlayer[i]->winHide(TRUE);
		else if (buttonPlayer[i] && !buttonPlayer[i]->winIsHidden())
			staticTextPlayer[i]->winHide(FALSE);
	}
}

void ControlBar::populateObserverInfoWindow ()
{
	if(ObserverPlayerInfoWindow->winIsHidden())
		return;

	if( !m_observerLookAtPlayer )
	{
		ObserverPlayerInfoWindow->winHide(TRUE);
		ObserverPlayerListWindow->winHide(FALSE);
		buttonIdleWorker->winHide(TRUE);
		populateObserverList();
		return;
	}

	UnicodeString uString;
	KindOfMaskType mask,clearmask;
	mask.set(KINDOF_SCORE);
	clearmask.set(KINDOF_STRUCTURE);

	uString.format(L"%d",m_observerLookAtPlayer->countObjects(mask,clearmask));
	GadgetStaticTextSetText(staticTextNumberOfUnits, uString);

	Int numBuildings = 0;
	mask.clear();
	mask.set(KINDOF_SCORE);
	mask.set(KINDOF_STRUCTURE);
	clearmask.clear();
	numBuildings = m_observerLookAtPlayer->countObjects(mask,clearmask);
	mask.clear();
	mask.set(KINDOF_SCORE_CREATE);
	mask.set(KINDOF_STRUCTURE);
	numBuildings += m_observerLookAtPlayer->countObjects(mask,clearmask);
	mask.clear();
	mask.set(KINDOF_SCORE_DESTROY);
	mask.set(KINDOF_STRUCTURE);
	numBuildings += m_observerLookAtPlayer->countObjects(mask,clearmask);
	uString.format(L"%d",numBuildings);
	GadgetStaticTextSetText(staticTextNumberOfBuildings, uString);
	uString.format(L"%d",m_observerLookAtPlayer->getScoreKeeper()->getTotalUnitsDestroyed());
	GadgetStaticTextSetText(staticTextNumberOfUnitsKilled, uString);
	uString.format(L"%d",m_observerLookAtPlayer->getScoreKeeper()->getTotalUnitsLost());
	GadgetStaticTextSetText(staticTextNumberOfUnitsLost, uString);
	GadgetStaticTextSetText(staticTextPlayerName, m_observerLookAtPlayer->getPlayerDisplayName());
	Color color = m_observerLookAtPlayer->getPlayerColor();
	staticTextPlayerName->winSetEnabledTextColors(color, GameMakeColor(0,0,0,255));
	winFlag->winSetEnabledImage(0, m_observerLookAtPlayer->getPlayerTemplate()->getFlagWaterMarkImage());
	winGeneralPortrait->winHide(FALSE);
	buttonIdleWorker->winHide(FALSE);
}
