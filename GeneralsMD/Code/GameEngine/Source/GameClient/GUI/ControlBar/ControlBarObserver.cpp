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
#include "GameClient/GameWindowManager.h"
#include "GameClient/GadgetPushButton.h"
#include "GameClient/GadgetStaticText.h"
#include "GameClient/GameText.h"
#include "GameClient/HeaderTemplate.h"
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
// income-by-source readout; not part of any shipped ControlBar.wnd, created
// at runtime by createObserverIncomeText(), so all uses are null-checked
static GameWindow *staticTextObserverIncome = nullptr;

static NameKeyType s_replayObserverNameKey = NAMEKEY_INVALID;

//-----------------------------------------------------------------------------
// PUBLIC FUNCTIONS ///////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------

//-------------------------------------------------------------------------------------------------
/** The income readout is not part of any shipped ControlBar.wnd, so it is
	* created at runtime inside the observer info window and works with any
	* control bar layout (stock, ControlBarPro, ...). If the layout has free
	* space under its lowest static text the field goes there (ControlBarPro
	* leaves a strip), otherwise it floats just above the info window (the
	* stock layout is fully packed). */
//-------------------------------------------------------------------------------------------------
static GameWindow *createObserverIncomeText( NameKeyType id )
{
	if (ObserverPlayerInfoWindow == nullptr)
		return nullptr;

	Int parentWidth, parentHeight;
	ObserverPlayerInfoWindow->winGetSize(&parentWidth, &parentHeight);

	// find the bottom edge of the lowest static text; image/button children
	// (portrait backdrops and the like) can span the whole window, so only
	// text fields tell us where the layout's content actually ends
	Int lowestTextBottom = 0;
	for (GameWindow *child = ObserverPlayerInfoWindow->winGetChild(); child; child = child->winGetNext())
	{
		if (!BitIsSet(child->winGetStyle(), GWS_STATIC_TEXT))
			continue;
		Int childX, childY, childWidth, childHeight;
		child->winGetPosition(&childX, &childY);
		child->winGetSize(&childWidth, &childHeight);
		if (childY + childHeight > lowestTextBottom)
			lowestTextBottom = childY + childHeight;
	}

	Int fieldHeight = parentHeight / 10;
	if (fieldHeight < 20)
		fieldHeight = 20;

	Int fieldY;
	if (parentHeight - lowestTextBottom >= fieldHeight)
		fieldY = lowestTextBottom + (parentHeight - lowestTextBottom - fieldHeight) / 2;
	else
		fieldY = -fieldHeight;

	const Color textColor = GameMakeColor(254, 254, 254, 255);
	const Color dropColor = GameMakeColor(0, 0, 0, 255);

	WinInstanceData instData;
	instData.init();
	instData.m_style = GWS_STATIC_TEXT;
	instData.m_status = WIN_STATUS_ENABLED | WIN_STATUS_NO_INPUT;
	instData.m_id = (Int)id;
	instData.m_owner = ObserverPlayerInfoWindow;
	instData.m_enabledText.color = textColor;
	instData.m_enabledText.borderColor = dropColor;
	instData.m_disabledText.color = textColor;
	instData.m_disabledText.borderColor = dropColor;
	instData.m_hiliteText.color = textColor;
	instData.m_hiliteText.borderColor = dropColor;

	TextData textData;
	textData.text = nullptr;
	textData.centered = TRUE;
	textData.centeredVertically = TRUE;
	textData.leftMargin = 0;
	textData.topMargin = 0;

	// same font the observer info stat fields use in the shipped layouts
	GameFont *font = TheHeaderTemplateManager ? TheHeaderTemplateManager->getFontFromTemplate("LabelSmall") : nullptr;
	if (font == nullptr && TheGlobalLanguageData)
		font = TheWindowManager->winFindFont("Arial", TheGlobalLanguageData->adjustFontSize(10), FALSE);
	if (font == nullptr)
		font = ObserverPlayerInfoWindow->winGetFont();

	const Int marginX = parentWidth / 32;
	GameWindow *win = TheWindowManager->gogoGadgetStaticText(
			ObserverPlayerInfoWindow,
			WIN_STATUS_ENABLED | WIN_STATUS_NO_INPUT,
			marginX, fieldY, parentWidth - 2 * marginX, fieldHeight,
			&instData, &textData, font, FALSE );

	if (win)
		GadgetStaticTextSetFont(win, font);

	return win;
}


void ControlBar::initObserverControls()
{
	ObserverPlayerInfoWindow = TheWindowManager->winGetWindowFromId(nullptr, TheNameKeyGenerator->nameToKey("ControlBar.wnd:ObserverPlayerInfoWindow"));
	ObserverPlayerListWindow = TheWindowManager->winGetWindowFromId(nullptr, TheNameKeyGenerator->nameToKey("ControlBar.wnd:ObserverPlayerListWindow"));

	for (Int i = 0; i < MAX_BUTTONS; i++)
	{
		AsciiString tmpString;
		tmpString.format("ControlBar.wnd:ButtonPlayer%d", i);
		buttonPlayerID[i] = TheNameKeyGenerator->nameToKey( tmpString );
		buttonPlayer[i] = TheWindowManager->winGetWindowFromId( ObserverPlayerListWindow, buttonPlayerID[i] );
		tmpString.format("ControlBar.wnd:StaticTextPlayer%d", i);
		staticTextPlayerID[i] = TheNameKeyGenerator->nameToKey( tmpString );
		staticTextPlayer[i] = TheWindowManager->winGetWindowFromId( ObserverPlayerListWindow, staticTextPlayerID[i] );
	}

	staticTextNumberOfUnits = TheWindowManager->winGetWindowFromId(nullptr, TheNameKeyGenerator->nameToKey("ControlBar.wnd:StaticTextNumberOfUnits"));
	staticTextNumberOfBuildings = TheWindowManager->winGetWindowFromId(nullptr, TheNameKeyGenerator->nameToKey("ControlBar.wnd:StaticTextNumberOfBuildings"));
	staticTextNumberOfUnitsKilled = TheWindowManager->winGetWindowFromId(nullptr, TheNameKeyGenerator->nameToKey("ControlBar.wnd:StaticTextNumberOfUnitsKilled"));
	staticTextNumberOfUnitsLost = TheWindowManager->winGetWindowFromId(nullptr, TheNameKeyGenerator->nameToKey("ControlBar.wnd:StaticTextNumberOfUnitsLost"));
	staticTextPlayerName = TheWindowManager->winGetWindowFromId(nullptr, TheNameKeyGenerator->nameToKey("ControlBar.wnd:StaticTextPlayerName"));
	const NameKeyType incomeKey = TheNameKeyGenerator->nameToKey("ControlBar.wnd:StaticTextObserverIncome");
	staticTextObserverIncome = TheWindowManager->winGetWindowFromId(nullptr, incomeKey);
	if (staticTextObserverIncome == nullptr)
		staticTextObserverIncome = createObserverIncomeText(incomeKey);
	winFlag = TheWindowManager->winGetWindowFromId(nullptr, TheNameKeyGenerator->nameToKey("ControlBar.wnd:WinFlag"));
	winGeneralPortrait = TheWindowManager->winGetWindowFromId(nullptr, TheNameKeyGenerator->nameToKey("ControlBar.wnd:WinGeneralPortrait"));
	buttonIdleWorker = TheWindowManager->winGetWindowFromId(nullptr, TheNameKeyGenerator->nameToKey("ControlBar.wnd:ButtonIdleWorker"));

	buttonCancelID = TheNameKeyGenerator->nameToKey("ControlBar.wnd:ButtonCancel");

	s_replayObserverNameKey = TheNameKeyGenerator->nameToKey("ReplayObserver");
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

	// cumulative income by source, nonzero sources only. Deterministic game
	// state accumulated by ScoreKeeper on every client, so the observer's own
	// simulation already has every player's numbers.
	if (staticTextObserverIncome)
	{
		static const char *const incomeTypeNames[INCOME_COUNT] =
		{
			"other",		// INCOME_OTHER
			"supply",		// INCOME_SUPPLY
			"hacker",		// INCOME_HACKER
			"market",		// INCOME_BLACK_MARKET
			"drop",			// INCOME_SUPPLY_DROP
			"derrick",	// INCOME_OIL_DERRICK
			"bounty",		// INCOME_BOUNTY
			"salvage",	// INCOME_SALVAGE
			"crate",		// INCOME_CRATE
			"theft",		// INCOME_THEFT
		};

		const ScoreKeeper *scoreKeeper = m_observerLookAtPlayer->getScoreKeeper();
		UnicodeString breakdown;
		UnicodeString part;
		for (Int incomeType = 0; incomeType < INCOME_COUNT; ++incomeType)
		{
			const Int amount = scoreKeeper->getIncomeByType(static_cast<IncomeType>(incomeType));
			if (amount == 0)
				continue;
			if (!breakdown.isEmpty())
				breakdown.concat(L"   ");
			part.format(L"%hs %d", incomeTypeNames[incomeType], amount);
			breakdown.concat(part);
		}
		GadgetStaticTextSetText(staticTextObserverIncome, breakdown);
	}
	GadgetStaticTextSetText(staticTextPlayerName, m_observerLookAtPlayer->getPlayerDisplayName());
	Color color = m_observerLookAtPlayer->getPlayerColor();
	staticTextPlayerName->winSetEnabledTextColors(color, GameMakeColor(0,0,0,255));
	winFlag->winSetEnabledImage(0, m_observerLookAtPlayer->getPlayerTemplate()->getFlagWaterMarkImage());
	winGeneralPortrait->winHide(FALSE);
	buttonIdleWorker->winHide(FALSE);
}
