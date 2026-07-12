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

// FILE: LoadScreen.cpp /////////////////////////////////////////////////
//-----------------------------------------------------------------------------
//
//                       Electronic Arts Pacific.
//
//                       Confidential Information
//                Copyright (C) 2002 - All Rights Reserved
//
//-----------------------------------------------------------------------------
//
//	created:	Mar 2002
//
//	Filename: 	LoadScreen.cpp
//
//	author:		Chris Huybregts
//
//	purpose:	Contains each of the different derived LoadClasses for each of the
//						Different kind of games we can have.
//
//-----------------------------------------------------------------------------
///////////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------------
// SYSTEM INCLUDES ////////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

//-----------------------------------------------------------------------------
// USER INCLUDES //////////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------
#include "Common/NameKeyGenerator.h"
#include "Common/AudioAffect.h"
#include "Common/AudioEventRTS.h"
#include "Common/AudioHandleSpecialValues.h"
#include "Common/GameAudio.h"
#include "Common/GameEngine.h"
#include "Common/GameLOD.h"
#include "Common/GameState.h"
#include "Common/MultiplayerSettings.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/PlayerTemplate.h"
#include "GameClient/CampaignManager.h"
#include "GameClient/Color.h"
#include "GameClient/Display.h"
#include "GameClient/DisplayString.h"
#include "GameClient/DisplayStringManager.h"
#include "GameClient/GameFont.h"
#include "GameClient/Image.h"
#include <math.h>
#include "GameClient/GadgetProgressBar.h"
#include "GameClient/GadgetStaticText.h"
#include "GameClient/GameText.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/GameWindowTransitions.h"
#include "GameClient/Keyboard.h"
#include "GameClient/LoadScreen.h"
#include "GameClient/MapUtil.h"
// Zero Hour only: multiplayer loading-screen "battlefield intel" (radarvan).
#if RTS_ZEROHOUR
#include "Common/StatsUploader.h"
#endif
#include "GameClient/Mouse.h"
#include "GameClient/Shell.h"
#include "GameClient/VideoPlayer.h"
#include "GameClient/WindowLayout.h"
#include "GameClient/WindowVideoManager.h"
#include "GameClient/ChallengeGenerals.h"
#include "GameLogic/FPUControl.h"
#include "GameLogic/GameLogic.h"
#include "GameNetwork/GameSpy/PeerDefs.h"
#include "GameNetwork/GameSpy/PersistentStorageThread.h"
#include "GameNetwork/NetworkInterface.h"
#include "GameNetwork/RankPointValue.h"

//-----------------------------------------------------------------------------
// DEFINES ////////////////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------


//-----------------------------------------------------------------------------
// PRIVATE TYPES //////////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// PRIVATE DATA ///////////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// PUBLIC DATA ////////////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// PRIVATE PROTOTYPES /////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// PRIVATE FUNCTIONS //////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// PUBLIC FUNCTIONS ///////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------
void positionStartSpots( GameInfo *myGame, GameWindow *buttonMapStartPositions[], GameWindow *mapWindow);
void updateMapStartSpots( GameInfo *myGame, GameWindow *buttonMapStartPositions[], Bool onLoadScreen = FALSE );
void positionAdditionalImages( MapMetaData *mmd, GameWindow *mapWindow, Bool force);

enum{
FRAME_TITLES_START = 20,
FRAME_TELETYPE_START = 24,
FRAME_FUDGE_ADD = 30,
FRAME_PORTRAITS_START = 35,
FRAME_OUTER_CIRCLE_LINE_SHOW = 50,
FRAME_INNER_CIRCLE_LINE_SHOW = 52,
FRAME_OUTER_CIRCLE_ALPHA_SHOW = 63,
FRAME_INNER_CIRCLE_ALPHA_SHOW = 74,
FRAME_OUTER_CIRCLE_LINE_HIDE = 75,
FRAME_INNER_BACKDROP_ALPHA_SHOW = 80,
FRAME_INNER_CIRCLE_LINE_HIDE = 81,
FRAME_VS_ANIM_START = 98,
FRAME_RIGHT_VOICE = 140,
};

static const Int TELETYPE_UPDATE_FREQ = 2; // how many frames between teletype updates



//-----------------------------------------------------------------------------
// LoadScreen Class
//-----------------------------------------------------------------------------

Bool LoadScreen::s_quitRequested = FALSE;

LoadScreen::LoadScreen()
{
	m_loadScreen = nullptr;
}

LoadScreen::~LoadScreen()
{
	if(m_loadScreen)
		TheWindowManager->winDestroy( m_loadScreen );
}

void LoadScreen::update( Int percent )
{
	TheGameEngine->serviceWindowsOS();
	if (TheGameEngine->getQuitting())
		return;	//don't bother with any of this if the player is exiting game.

	TheWindowManager->update();
	TheDisplay->update();
	// redraw all views, update the GUI
	TheDisplay->draw();

	setFPMode();
}

//-----------------------------------------------------------------------------
/** Let the player work the gadgets on this load screen.
	*
	* A load screen is driven from inside GameLogic::startNewGame(), so TheGameClient is not
	* running and nothing puts raw input on the message stream, let alone propagates it. Feed the
	* mouse to the window system by hand. Nothing else about the client is pumped: no keyboard, no
	* game messages, no logic, so this cannot disturb the game that is being built underneath. */
//-----------------------------------------------------------------------------
void LoadScreen::serviceInput()
{
	if( TheMouse == nullptr )
		return;

	TheMouse->update();
	TheMouse->sendEventsToWindowSystem();
}

//-----------------------------------------------------------------------------
/** Throw away input that piled up while the game was busy loading, so that clicks aimed at the
	* screen we came from cannot land on a load screen gadget the moment we start listening. */
//-----------------------------------------------------------------------------
void LoadScreen::flushInput()
{
	if( TheMouse == nullptr )
		return;

	TheMouse->flushEvents();
}

//-----------------------------------------------------------------------------
/** System callback for the multiplayer load screen. The only thing on that screen the player can
	* work is the quit button, which lets them leave a game that a peer is never going to finish
	* loading instead of sitting out the whole load timeout. */
//-----------------------------------------------------------------------------
WindowMsgHandledType MultiplayerLoadScreenSystem( GameWindow *window, UnsignedInt msg,
																								 WindowMsgData mData1, WindowMsgData mData2 )
{
	static NameKeyType buttonQuit = NAMEKEY_INVALID;

	switch( msg )
	{

		//---------------------------------------------------------------------------------------------
		case GWM_CREATE:
		{
			buttonQuit = TheNameKeyGenerator->nameToKey( AsciiString( "MultiplayerLoadScreen.wnd:ButtonQuit" ) );
			break;
		}

		//---------------------------------------------------------------------------------------------
		case GWM_DESTROY:
		{
			break;
		}

		//---------------------------------------------------------------------------------------------
		case GBM_SELECTED:
		{
			GameWindow *control = (GameWindow *)mData1;
			Int controlID = control->winGetWindowId();

			if( controlID == buttonQuit )
				LoadScreen::setQuitRequested( TRUE );

			break;
		}

		//---------------------------------------------------------------------------------------------
		default:
			return MSG_IGNORED;

	}

	return MSG_HANDLED;
}

// SinglePlayerLoadScreen Class ///////////////////////////////////////////////
//-----------------------------------------------------------------------------
SinglePlayerLoadScreen::SinglePlayerLoadScreen()
{
	m_currentObjectiveLine = 0;
	m_currentObjectiveLineCharacter = 0;
	m_finishedObjectiveText = FALSE;
	m_currentObjectiveWidthOffset = 0;
	m_progressBar = nullptr;
	m_percent = nullptr;
	m_videoStream = nullptr;
	m_videoBuffer = nullptr;
	m_objectiveWin = nullptr;
	for(Int i = 0; i < MAX_OBJECTIVE_LINES; ++i)
		m_objectiveLines[i] = nullptr;

}

SinglePlayerLoadScreen::~SinglePlayerLoadScreen()
{
	delete m_videoBuffer;

	if ( m_videoStream )
	{
		m_videoStream->close();
	}

	TheAudio->removeAudioEvent( m_ambientLoopHandle );
}

void SinglePlayerLoadScreen::moveWindows( Int frame )
{
	enum{
		STATE_BEGIN = 250,
		STATE_SHOW_LOCATION = 251,
		STATE_BEGIN_BRIEFING = 255,
//		STATE_BEGIN_ANIMATING_TEXT = 250,
		STATE_SHOW_CAMEO_1 = 434,
		STATE_BEGIN_ANIMATING_TEXT = 356,
		STATE_HIDE_CAMEO_1 = 459,
		STATE_SHOW_CAMEO_2 = 464,
		STATE_HIDE_CAMEO_2 = 492,
		STATE_SHOW_CAMEO_3 = 497,
		STATE_HIDE_CAMEO_3 = 524,
//		STATE_END_ANIM_HEAD = 450,
		STATE_END_ANIMATING_TEXT = 730,
		STATE_END = 730
	};
	if(frame < STATE_BEGIN || frame > STATE_END)
		return;

	if( frame == STATE_BEGIN_BRIEFING)
	{
		// add sound support here
		TheAudio->friend_forcePlayAudioEventRTS(&TheCampaignManager->getCurrentMission()->m_briefingVoice);
	}

	if( frame == STATE_BEGIN_ANIMATING_TEXT)
	{
		m_objectiveWin->winHide(FALSE);
		// animate the text and stuff
	}

	if( frame > STATE_BEGIN_ANIMATING_TEXT && frame <= STATE_END_ANIMATING_TEXT && !m_finishedObjectiveText)
	{
		if(m_currentObjectiveLineCharacter >= m_unicodeObjectiveLines[m_currentObjectiveLine].getLength() )
		{
			m_currentObjectiveLine++;
			m_currentObjectiveLineCharacter =0;
		}
		if(m_currentObjectiveLine >= MAX_OBJECTIVE_LINES || m_unicodeObjectiveLines[m_currentObjectiveLine].isEmpty())
		{
			m_finishedObjectiveText = TRUE;
		}
		else
		{
			WideChar wChar = m_unicodeObjectiveLines[m_currentObjectiveLine].getCharAt(m_currentObjectiveLineCharacter);
			UnicodeString text = GadgetStaticTextGetText(m_objectiveLines[m_currentObjectiveLine]);
			text.concat(wChar);
			GadgetStaticTextSetText(m_objectiveLines[m_currentObjectiveLine], text);

		}
		m_currentObjectiveLineCharacter++;
	}
	switch (frame) {

	case STATE_SHOW_LOCATION:
		m_location->winHide(FALSE);
		break;
	case STATE_SHOW_CAMEO_1:
		m_unitDesc[0]->winHide(FALSE);
		break;
	case STATE_HIDE_CAMEO_1:
		m_unitDesc[0]->winHide(TRUE);
		break;
	case STATE_SHOW_CAMEO_2:
		m_unitDesc[1]->winHide(FALSE);
		break;
	case STATE_HIDE_CAMEO_2:
		m_unitDesc[1]->winHide(TRUE);
		break;
	case STATE_SHOW_CAMEO_3:
		m_unitDesc[2]->winHide(FALSE);
		break;
	case STATE_HIDE_CAMEO_3:
		m_unitDesc[2]->winHide(TRUE);
		break;
	}

}
/*
	static Bool on = FALSE;
	static ICoord2D startPos, endPos;
	enum{
		STATE_BEGIN = 275,
		STATE_BEGIN_ANIM = 290,
		STATE_ANIM_CAMEO1 = 300,
		STATE_ANIM_CAMEO1_TRANSITION_CAMEO2 = 350,
		STATE_ANIM_CAMEO2 = 400,
		STATE_ANIM_CAMEO2_TRANSITION_CAMEO3 = 450,
		STATE_ANIM_CAMEO3 = 500,
		STATED_END_ANIM = 550,
		STATE_END = 800
	};
	if(frame < STATE_BEGIN)
		return;
	else if(frame == STATE_BEGIN )
	{
		m_cameoWindow1->winHide(FALSE);
		m_cameoWindow2->winHide(FALSE);
		m_cameoWindow3->winHide(FALSE);
		m_cameoFrame->winHide(FALSE);
	}
	else if( frame == STATE_ANIM_CAMEO1)
	{
		m_cameoWindow1->winEnable(TRUE);
		GadgetStaticTextSetText(m_cameoText, TheGameText->fetch(TheCampaignManager->getCurrentMission()->m_cameoImageName[0]));
		//save of positions
	}
	else if( frame == STATE_ANIM_CAMEO1_TRANSITION_CAMEO2)
	{
		m_cameoWindow1->winEnable(FALSE);
		GadgetStaticTextSetText(m_cameoText, UnicodeString::TheEmptyString);
		ICoord2D tempPos;
		Int xOffset;
		m_cameoFrame->winGetPosition(&startPos.x, &startPos.y);
		m_cameoWindow1->winGetPosition(&tempPos.x, &tempPos.y);
		xOffset = tempPos.x - startPos.x;
		m_cameoWindow2->winGetPosition(&endPos.x, &endPos.y);
		endPos.x = endPos.x - xOffset;
		endPos.y = startPos.y;

	}
	else if( frame > STATE_ANIM_CAMEO1_TRANSITION_CAMEO2 && frame < STATE_ANIM_CAMEO2)
	{

		//extrapolate between start and end pos
		Real percent = INT_TO_REAL((frame - STATE_ANIM_CAMEO1_TRANSITION_CAMEO2)) / (STATE_ANIM_CAMEO2 - STATE_ANIM_CAMEO1_TRANSITION_CAMEO2);
		m_cameoFrame->winSetPosition(startPos.x + (endPos.x - startPos.x) * percent, endPos.y);
	}
	else if( frame == STATE_ANIM_CAMEO2 )
	{
		m_cameoWindow2->winEnable(TRUE);
		m_cameoFrame->winSetPosition(endPos.x, endPos.y);
		GadgetStaticTextSetText(m_cameoText, TheGameText->fetch(TheCampaignManager->getCurrentMission()->m_cameoImageName[1]));
	}
	else if( frame == STATE_ANIM_CAMEO2_TRANSITION_CAMEO3)
	{
		m_cameoWindow2->winEnable(FALSE);
		GadgetStaticTextSetText(m_cameoText, UnicodeString::TheEmptyString);
		ICoord2D tempPos;
		Int xOffset;
		m_cameoFrame->winGetPosition(&startPos.x, &startPos.y);
		m_cameoWindow2->winGetPosition(&tempPos.x, &tempPos.y);
		xOffset = tempPos.x - startPos.x;
		m_cameoWindow3->winGetPosition(&endPos.x, &endPos.y);
		endPos.x = endPos.x - xOffset;
		endPos.y = startPos.y;

	}
	else if( frame > STATE_ANIM_CAMEO2_TRANSITION_CAMEO3 && frame < STATE_ANIM_CAMEO3)
	{

		//extrapolate between start and end pos
		Real percent = INT_TO_REAL((frame - STATE_ANIM_CAMEO2_TRANSITION_CAMEO3)) / (STATE_ANIM_CAMEO3 - STATE_ANIM_CAMEO2_TRANSITION_CAMEO3);
		m_cameoFrame->winSetPosition(startPos.x + (endPos.x - startPos.x) * percent, endPos.y);
	}
	else if( frame == STATE_ANIM_CAMEO3 )
	{
		m_cameoFrame->winSetPosition(endPos.x, endPos.y);
		m_cameoWindow3->winEnable(TRUE);
		GadgetStaticTextSetText(m_cameoText, TheGameText->fetch(TheCampaignManager->getCurrentMission()->m_cameoImageName[2]));
	}
	else if( frame ==STATED_END_ANIM)
	{
		m_cameoWindow3->winEnable(FALSE);
		GadgetStaticTextSetText(m_cameoText, UnicodeString::TheEmptyString);
		m_cameoFrame->winHide(TRUE);

	}
}*/

void SinglePlayerLoadScreen::init( GameInfo *game )
{
	//No music in SinglePlayerLoadScreen

	// create the layout of the load screen
	m_loadScreen = TheWindowManager->winCreateFromScript( "Menus/SinglePlayerLoadScreen.wnd" );
	DEBUG_ASSERTCRASH(m_loadScreen, ("Can't initialize the single player loadscreen"));
	m_loadScreen->winHide(FALSE);
	m_loadScreen->winBringToTop();
//	Mission *mission = TheCampaignManager->getCurrentMission();
	// Store the pointer to the progress bar on the loadscreen
	m_progressBar = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( "SinglePlayerLoadScreen.wnd:ProgressLoad" ));
	DEBUG_ASSERTCRASH(m_progressBar, ("Can't initialize the progressbar for the single player loadscreen"));
	GadgetProgressBarSetProgress(m_progressBar, 0 );

	m_percent = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( "SinglePlayerLoadScreen.wnd:Percent" ));
	DEBUG_ASSERTCRASH(m_percent, ("Can't initialize the m_percent for the single player loadscreen"));
	GadgetStaticTextSetText(m_percent,L"0%");
	m_percent->winHide(TRUE);

	m_objectiveWin = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( "SinglePlayerLoadScreen.wnd:ObjectivesWin" ));
	DEBUG_ASSERTCRASH(m_objectiveWin, ("Can't initialize the m_objectiveWin for the single player loadscreen"));
	m_objectiveWin->winHide(TRUE);


	Mission *mission = TheCampaignManager->getCurrentMission();
	AsciiString lineName;
	Int i = 0;
	for(; i < MAX_OBJECTIVE_LINES; ++i)
	{
		lineName.format("SinglePlayerLoadScreen.wnd:StaticTextLine%d",i);
		m_objectiveLines[i] = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( lineName ));
		DEBUG_ASSERTCRASH(m_objectiveLines[i], ("Can't initialize the m_objectiveLines[%d] for the single player loadscreen", i));
		GadgetStaticTextSetText(m_objectiveLines[i],UnicodeString::TheEmptyString);

		// translate the objective lines
		if(mission->m_missionObjectivesLabel[i].isNotEmpty())
			m_unicodeObjectiveLines[i] = TheGameText->fetch(mission->m_missionObjectivesLabel[i]);
	}

	for(i = 0; i < MAX_DISPLAYED_UNITS; ++i)
	{
		lineName.format("SinglePlayerLoadScreen.wnd:StaticTextCameoText%d",i);
		m_unitDesc[i] = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( lineName ));
		DEBUG_ASSERTCRASH(m_unitDesc[i], ("Can't initialize the m_objectiveLines[%d] for the single player loadscreen", i));
		GadgetStaticTextSetText(m_unitDesc[i],TheGameText->fetch(mission->m_unitNames[i]));
		m_unitDesc[i]->winHide(TRUE);
	}
	m_location = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( "SinglePlayerLoadScreen.wnd:StaticTextCameoText3" ));
	DEBUG_ASSERTCRASH(m_location, ("Can't initialize the m_objectiveWin for the single player loadscreen"));
	m_location->winHide(TRUE);
	GadgetStaticTextSetText(m_location, TheGameText->fetch(mission->m_locationNameLabel));



	m_currentObjectiveLine = 0;
	m_currentObjectiveWidthOffset = 0;
	m_currentObjectiveLineCharacter = 0;
	m_finishedObjectiveText = FALSE;
/*
	m_cameoWindow1 = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( "SinglePlayerLoadScreen.wnd:WindowCameo1" ));
	DEBUG_ASSERTCRASH(m_cameoWindow1, ("Can't initialize the m_cameoWindow1 for the single player loadscreen"));
	m_cameoWindow1->winHide(TRUE);
	m_cameoWindow1->winEnable(FALSE);
	m_cameoWindow1->winSetEnabledImage(0, mission->m_cameoImage[0]);
	m_cameoWindow1->winSetDisabledImage(0, mission->m_cameoDisabledImage[0]);

	m_cameoWindow2 = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( "SinglePlayerLoadScreen.wnd:WindowCameo2" ));
	DEBUG_ASSERTCRASH(m_cameoWindow2, ("Can't initialize the m_cameoWindow2 for the single player loadscreen"));
	m_cameoWindow2->winHide(TRUE);
	m_cameoWindow2->winEnable(FALSE);
	m_cameoWindow2->winSetEnabledImage(0, mission->m_cameoImage[1]);
	m_cameoWindow2->winSetDisabledImage(0, mission->m_cameoDisabledImage[1]);

	m_cameoWindow3 = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( "SinglePlayerLoadScreen.wnd:WindowCameo3" ));
	DEBUG_ASSERTCRASH(m_cameoWindow3, ("Can't initialize the m_cameoWindow3 for the single player loadscreen"));
	m_cameoWindow3->winHide(TRUE);
	m_cameoWindow3->winEnable(FALSE);
	m_cameoWindow3->winSetEnabledImage(0, mission->m_cameoImage[2]);
	m_cameoWindow3->winSetDisabledImage(0, mission->m_cameoDisabledImage[2]);

	m_headMovie = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( "SinglePlayerLoadScreen.wnd:WindowHead" ));
	DEBUG_ASSERTCRASH(m_headMovie, ("Can't initialize the m_headMovie for the single player loadscreen"));
	m_headMovie->winHide(TRUE);
	m_cameoFrame = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( "SinglePlayerLoadScreen.wnd:WindowHiliteCameo" ));
	DEBUG_ASSERTCRASH(m_cameoFrame, ("Can't initialize the m_cameoFrame for the single player loadscreen"));
	m_cameoFrame->winHide(TRUE);
	m_cameoText = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( "SinglePlayerLoadScreen.wnd:StaticTextCameoText" ));
	DEBUG_ASSERTCRASH(m_cameoText, ("Can't initialize the m_cameoText for the single player loadscreen"));

*/
	m_ambientLoop.setEventName("LoadScreenAmbient");
	// create the new stream
	m_videoStream = TheVideoPlayer->open( TheCampaignManager->getCurrentMission()->m_movieLabel );
	if ( m_videoStream == nullptr )
	{
		m_percent->winHide(TRUE);
		return;
	}

	// Create the new buffer
	m_videoBuffer = TheDisplay->createVideoBuffer();
	if (	m_videoBuffer == nullptr ||
				!m_videoBuffer->allocate(	m_videoStream->width(),
													m_videoStream->height())
		)
	{
		delete m_videoBuffer;
		m_videoBuffer = nullptr;

		if ( m_videoStream )
		{
			m_videoStream->close();
			m_videoStream = nullptr;
		}

		return;
	}

	// format the progress bar: USA to blue, GLA to green, China to red
	// and set the background image
	AsciiString campaignName = TheCampaignManager->getCurrentCampaign()->m_name;
	GameWindow *backgroundWin = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( "SinglePlayerLoadScreen.wnd:ParentSinglePlayerLoadScreen" ));
	if (campaignName.compareNoCase("USA") == 0)
	{
		if (const Image *image = TheMappedImageCollection->findImageByName("MissionLoad_USA"))
		{
			backgroundWin->winSetEnabledImage( 0, image);
		}
		if (const Image *image = TheMappedImageCollection->findImageByName("LoadingBar_ProgressCenter2"))
		{
			m_progressBar->winSetEnabledImage( 6, image );
		}
	}
	else if (campaignName.compareNoCase("GLA") == 0)
	{
		if (const Image *image = TheMappedImageCollection->findImageByName("MissionLoad_GLA"))
		{
			backgroundWin->winSetEnabledImage( 0, image );
		}
		if (const Image *image = TheMappedImageCollection->findImageByName("LoadingBar_ProgressCenter3"))
		{
			m_progressBar->winSetEnabledImage( 6, image );
		}
	}
	else if (campaignName.compareNoCase("China") == 0)
	{
		if (const Image *image = TheMappedImageCollection->findImageByName("MissionLoad_China"))
		{
			backgroundWin->winSetEnabledImage( 0, image );
		}
		if (const Image *image = TheMappedImageCollection->findImageByName("LoadingBar_ProgressCenter1"))
		{
			m_progressBar->winSetEnabledImage( 6, image );
		}
	}
	// else leave the default background screen


	if(TheGameLODManager && TheGameLODManager->didMemPass())
	{
		// TheSuperHackers @bugfix Originally this movie render loop stopped rendering when the game window was inactive.
		// This either skipped the movie or caused decompression artifacts. Now the video just keeps playing until it done.

		Int progressUpdateCount = m_videoStream->frameCount() / FRAME_FUDGE_ADD;
		Int shiftedPercent = -FRAME_FUDGE_ADD + 1;
		while (m_videoStream->frameIndex() < m_videoStream->frameCount() - 1 )
		{
			// TheSuperHackers @feature User can now skip video by pressing ESC
			if (TheKeyboard)
			{
				TheKeyboard->UPDATE();
				KeyboardIO *io = TheKeyboard->findKey(KEY_ESC, KeyboardIO::STATUS_UNUSED);
				if (io && BitIsSet(io->state, KEY_STATE_DOWN))
				{
					io->setUsed();
					break;
				}
			}

			TheGameEngine->serviceWindowsOS();

			if(!m_videoStream->isFrameReady())
			{
				Sleep(1);
				continue;
			}

			m_videoStream->frameDecompress();
			m_videoStream->frameRender(m_videoBuffer);

#if RTS_GENERALS
			moveWindows( m_videoStream->frameIndex());
#endif

			m_videoStream->frameNext();

			if(m_videoBuffer)
				m_loadScreen->winGetInstanceData()->setVideoBuffer(m_videoBuffer);
			if(m_videoStream->frameIndex() % progressUpdateCount == 0)
			{
				shiftedPercent++;
				if(shiftedPercent >0)
					shiftedPercent = 0;
				Int percent = (shiftedPercent + FRAME_FUDGE_ADD)/1.3;
				UnicodeString per;
				per.format(L"%d%%",percent);
				TheMouse->setCursorTooltip(UnicodeString::TheEmptyString);
				GadgetProgressBarSetProgress(m_progressBar, percent);
				GadgetStaticTextSetText(m_percent, per);

			}
			TheWindowManager->update();

			// redraw all views, update the GUI
			TheDisplay->draw();
		}

#if !RTS_GENERALS
		// let the background image show through
		m_videoStream->close();
		m_videoStream = nullptr;
		m_loadScreen->winGetInstanceData()->setVideoBuffer( nullptr );
		TheDisplay->draw();
#endif
	}
	else
	{
#if RTS_GENERALS
		// if we're min speced
		m_videoStream->frameGoto(m_videoStream->frameCount()); // zero based
		while(!m_videoStream->isFrameReady())
			Sleep(1);
		m_videoStream->frameDecompress();
		m_videoStream->frameRender(m_videoBuffer);
		if(m_videoBuffer)
				m_loadScreen->winGetInstanceData()->setVideoBuffer(m_videoBuffer);

		m_objectiveWin->winHide(FALSE);
		for(i = 0; i < MAX_DISPLAYED_UNITS; ++i)
			m_unitDesc[i]->winHide(FALSE);
		m_location->winHide(FALSE);

		// Audio was choppy so, I chopped it out!
		TheAudio->friend_forcePlayAudioEventRTS(&TheCampaignManager->getCurrentMission()->m_briefingVoice);

		for(Int i = 0; i < MAX_OBJECTIVE_LINES; ++i)
		{
			GadgetStaticTextSetText(m_objectiveLines[i], m_unicodeObjectiveLines[i]);
		}
#else
		// if we're min spec'ed don't play a movie
#endif

		Int delay = mission->m_voiceLength * 1000;
		Int begin = timeGetTime();
		Int currTime = begin;
		Int fudgeFactor = 0;
		while(begin + delay > currTime )
		{
			fudgeFactor = 30 * ((currTime - begin)/ INT_TO_REAL(delay ));
			GadgetProgressBarSetProgress(m_progressBar, fudgeFactor);

			TheWindowManager->update();
			TheDisplay->draw();
			Sleep(100);
			currTime = timeGetTime();
		}


		TheWindowManager->update();
		TheDisplay->draw();

	}
	setFPMode();
	m_percent->winHide(TRUE);
	m_ambientLoopHandle = TheAudio->addAudioEvent(&m_ambientLoop);

}

void SinglePlayerLoadScreen::reset()
{
 setLoadScreen(nullptr);
 m_progressBar = nullptr;
}

void SinglePlayerLoadScreen::update( Int percent )
{
	percent = (percent + FRAME_FUDGE_ADD)/1.3;
	UnicodeString per;
	per.format(L"%d%%",percent);
	TheMouse->setCursorTooltip(UnicodeString::TheEmptyString);
	GadgetProgressBarSetProgress(m_progressBar, percent);
	GadgetStaticTextSetText(m_percent, per);

	// Do this last!
	LoadScreen::update( percent );
}

void SinglePlayerLoadScreen::setProgressRange( Int min, Int max )
{

}

// ChallengeLoadScreen Class ///////////////////////////////////////////////
//-----------------------------------------------------------------------------
ChallengeLoadScreen::ChallengeLoadScreen()
{
	m_progressBar = nullptr;
	m_videoStream = nullptr;
	m_videoBuffer = nullptr;

	m_bioNameLeft = nullptr;
	m_bioAgeLeft = nullptr;
	m_bioBirthplaceLeft = nullptr;
	m_bioStrategyLeft = nullptr;
	m_bioBigNameEntryLeft = nullptr;
	m_bioNameEntryLeft = nullptr;
	m_bioAgeEntryLeft = nullptr;
	m_bioBirthplaceEntryLeft = nullptr;
	m_bioStrategyEntryLeft = nullptr;
	m_bioBigNameEntryRight = nullptr;
	m_bioNameRight = nullptr;
	m_bioAgeRight = nullptr;
	m_bioBirthplaceRight = nullptr;
	m_bioStrategyRight = nullptr;
	m_bioNameEntryRight = nullptr;
	m_bioAgeEntryRight = nullptr;
	m_bioBirthplaceEntryRight = nullptr;
	m_bioStrategyEntryRight = nullptr;

	m_portraitLeft = nullptr;
	m_portraitRight = nullptr;
	m_portraitMovieLeft = nullptr;
	m_portraitMovieRight = nullptr;

//	m_overlayReticleCrosshairs = nullptr;
//	m_overlayReticleCircleLineOuter = nullptr;
//	m_overlayReticleCircleLineInner = nullptr;
	m_overlayReticleCircleAlphaOuter = nullptr;
	m_overlayReticleCircleAlphaInner = nullptr;
	m_overlayVsBackdrop = nullptr;
	m_overlayVs = nullptr;
	m_wndVideoManager = nullptr;
}

ChallengeLoadScreen::~ChallengeLoadScreen()
{
	delete m_videoBuffer;

	if ( m_videoStream )
	{
		m_videoStream->close();
	}

	delete m_wndVideoManager;

	TheAudio->removeAudioEvent( m_ambientLoopHandle );
}

// accepts the number of chars to advance, the window we're concerned with, the total text for final display, and the current position of the readout
// returns the updated position of the readout
Int updateTeletypeText( Int num_chars, GameWindow* window, UnicodeString full_text, Int current_text_pos )
{
	DEBUG_ASSERTCRASH(window, ("No window for teletype text update"));
	UnicodeString currentText = GadgetStaticTextGetText(window);
	WideChar wChar;
	for (Int i = 0; i < num_chars; i++)
	{
		if (current_text_pos < full_text.getLength())
		{
			wChar = full_text.getCharAt(current_text_pos);
			currentText.concat(wChar);
			current_text_pos++;
		}
	}
	GadgetStaticTextSetText(window, currentText);
	return current_text_pos;
}

void ChallengeLoadScreen::activatePieces( Int frame, const GeneralPersona *generalPlayer, const GeneralPersona *generalOpponent )
{
	static Int textPosBigNameRight = 0;
	static Int textPosNameRight = 0;
	static Int textPosAgeRight = 0;
	static Int textPosBirthplaceRight = 0;
	static Int textPosStrategyRight = 0;
	static Int textPosBigNameLeft = 0;
	static Int textPosNameLeft = 0;
	static Int textPosAgeLeft = 0;
	static Int textPosBirthplaceLeft = 0;
	static Int textPosStrategyLeft = 0;

	AudioEventRTS eventLeftGeneral( generalPlayer->getNameSound() );
	AudioEventRTS eventVS("Taunts_GCAnnouncer12");
	AudioEventRTS eventRightGeneral( generalOpponent->getNameSound() );

	switch (frame)
	{
		case FRAME_TITLES_START:
			m_bioNameLeft->winHide(FALSE);
//			m_bioAgeLeft->winHide(FALSE);
			m_bioBirthplaceLeft->winHide(FALSE);
			m_bioStrategyLeft->winHide(FALSE);
			m_bioNameRight->winHide(FALSE);
//			m_bioAgeRight->winHide(FALSE);
			m_bioBirthplaceRight->winHide(FALSE);
			m_bioStrategyRight->winHide(FALSE);

			break;
		case FRAME_TELETYPE_START:
			// reinit the statics for each new load screen
			textPosBigNameRight = 0;
			textPosNameRight = 0;
			textPosAgeRight = 0;
			textPosBirthplaceRight = 0;
			textPosStrategyRight = 0;
			textPosBigNameLeft = 0;
			textPosNameLeft = 0;
			textPosAgeLeft = 0;
			textPosBirthplaceLeft = 0;
			textPosStrategyLeft = 0;

			m_bioBigNameEntryLeft->winHide(FALSE);
			m_bioNameEntryLeft->winHide(FALSE);
//			m_bioAgeEntryLeft->winHide(FALSE);
			m_bioBirthplaceEntryLeft->winHide(FALSE);
			m_bioStrategyEntryLeft->winHide(FALSE);
			GadgetStaticTextSetText( m_bioBigNameEntryLeft, UnicodeString::TheEmptyString );
			GadgetStaticTextSetText( m_bioNameEntryLeft, UnicodeString::TheEmptyString );
//			GadgetStaticTextSetText( m_bioAgeEntryLeft, UnicodeString::TheEmptyString );
			GadgetStaticTextSetText( m_bioBirthplaceEntryLeft, UnicodeString::TheEmptyString );
			GadgetStaticTextSetText( m_bioStrategyEntryLeft, UnicodeString::TheEmptyString );

			m_bioBigNameEntryRight->winHide(FALSE);
			m_bioNameEntryRight->winHide(FALSE);
//			m_bioAgeEntryRight->winHide(FALSE);
			m_bioBirthplaceEntryRight->winHide(FALSE);
			m_bioStrategyEntryRight->winHide(FALSE);
			GadgetStaticTextSetText( m_bioBigNameEntryRight, UnicodeString::TheEmptyString );
			GadgetStaticTextSetText( m_bioNameEntryRight, UnicodeString::TheEmptyString );
//			GadgetStaticTextSetText( m_bioAgeEntryRight, UnicodeString::TheEmptyString );
			GadgetStaticTextSetText( m_bioBirthplaceEntryRight, UnicodeString::TheEmptyString );
			GadgetStaticTextSetText( m_bioStrategyEntryRight, UnicodeString::TheEmptyString );
			break;
		case FRAME_PORTRAITS_START:

			m_wndVideoManager->playMovie( m_portraitMovieLeft, generalPlayer->getPortraitMovieLeftName(), WINDOW_PLAY_MOVIE_SHOW_LAST_FRAME);
			m_wndVideoManager->playMovie( m_portraitMovieRight, generalOpponent->getPortraitMovieRightName(), WINDOW_PLAY_MOVIE_SHOW_LAST_FRAME);
			m_portraitMovieLeft->winHide(FALSE);
			m_portraitMovieRight->winHide(FALSE);

			TheAudio->addAudioEvent( &eventLeftGeneral );

			break;
		case FRAME_OUTER_CIRCLE_LINE_SHOW:
//			m_overlayReticleCircleLineOuter->winHide(FALSE);
			break;
		case FRAME_INNER_CIRCLE_LINE_SHOW:
//			m_overlayReticleCircleLineInner->winHide(FALSE);
			break;
		case FRAME_OUTER_CIRCLE_ALPHA_SHOW:
			m_overlayReticleCircleAlphaOuter->winHide(FALSE);
			break;
		case FRAME_INNER_CIRCLE_ALPHA_SHOW:
			m_overlayReticleCircleAlphaInner->winHide(FALSE);
			break;
		case FRAME_OUTER_CIRCLE_LINE_HIDE:
//			m_overlayReticleCircleLineOuter->winHide(TRUE);
			break;
		case FRAME_INNER_BACKDROP_ALPHA_SHOW:
			m_overlayVsBackdrop->winHide(FALSE);
			break;
		case FRAME_INNER_CIRCLE_LINE_HIDE:
//			m_overlayReticleCircleLineInner->winHide(TRUE);
			break;
		case FRAME_VS_ANIM_START:
			// it's time to start the overlay movie
//					m_overlayVsBackdrop->winSetEnabledImage( 0, TheMappedImageCollection->findImageByFilename("))
			m_overlayVsBackdrop->winHide(FALSE);
			m_overlayVs->winHide(FALSE);
			m_wndVideoManager->playMovie( m_overlayVs, "VSSmall", WINDOW_PLAY_MOVIE_SHOW_LAST_FRAME);

			// "Verses"
			TheAudio->addAudioEvent( &eventVS );

			break;
		case FRAME_RIGHT_VOICE:
			TheAudio->addAudioEvent( &eventRightGeneral );

			break;
	}

	// update the teletype readout
	if (frame > FRAME_TELETYPE_START && (frame % TELETYPE_UPDATE_FREQ) == 0)
	{
		textPosNameLeft = updateTeletypeText( 1, m_bioNameEntryLeft, TheGameText->fetch(generalPlayer->getBioName()), textPosNameLeft);
		textPosBigNameLeft = updateTeletypeText( 1, m_bioBigNameEntryLeft, TheGameText->fetch(generalPlayer->getBioName()), textPosBigNameLeft);
//		textPosAgeLeft = updateTeletypeText( 1, m_bioAgeEntryLeft, TheGameText->fetch(generalPlayer->getBioDOB()), textPosAgeLeft);
		textPosBirthplaceLeft = updateTeletypeText( 1, m_bioBirthplaceEntryLeft, TheGameText->fetch(generalPlayer->getBioRank()), textPosBirthplaceLeft);
		textPosStrategyLeft = updateTeletypeText( 1, m_bioStrategyEntryLeft, TheGameText->fetch(generalPlayer->getBioStrategy()), textPosStrategyLeft);

		textPosNameRight = updateTeletypeText( 1, m_bioNameEntryRight, TheGameText->fetch(generalOpponent->getBioName()), textPosNameRight);
		textPosBigNameRight = updateTeletypeText( 1, m_bioBigNameEntryRight, TheGameText->fetch(generalOpponent->getBioName()), textPosBigNameRight);
//		textPosAgeRight = updateTeletypeText( 1, m_bioAgeEntryRight, TheGameText->fetch(generalOpponent->getBioDOB()), textPosAgeRight);
		textPosBirthplaceRight = updateTeletypeText( 1, m_bioBirthplaceEntryRight, TheGameText->fetch(generalOpponent->getBioRank()), textPosBirthplaceRight);
		textPosStrategyRight = updateTeletypeText( 1, m_bioStrategyEntryRight, TheGameText->fetch(generalOpponent->getBioStrategy()), textPosStrategyRight);
	}
}

void ChallengeLoadScreen::activatePiecesMinSpec(const GeneralPersona *generalPlayer, const GeneralPersona *generalOpponent)
{
	m_bioNameLeft->winHide(FALSE);
//	m_bioAgeLeft->winHide(FALSE);
	m_bioBirthplaceLeft->winHide(FALSE);
	m_bioStrategyLeft->winHide(FALSE);
	m_bioNameRight->winHide(FALSE);
//	m_bioAgeRight->winHide(FALSE);
	m_bioBirthplaceRight->winHide(FALSE);
	m_bioStrategyRight->winHide(FALSE);
	m_bioBigNameEntryLeft->winHide(FALSE);
	m_bioNameEntryLeft->winHide(FALSE);
//	m_bioAgeEntryLeft->winHide(FALSE);
	m_bioBirthplaceEntryLeft->winHide(FALSE);
	m_bioStrategyEntryLeft->winHide(FALSE);
	GadgetStaticTextSetText( m_bioBigNameEntryLeft, TheGameText->fetch(generalPlayer->getBioName()) );
	GadgetStaticTextSetText( m_bioNameEntryLeft, TheGameText->fetch(generalPlayer->getBioName()) );
//	GadgetStaticTextSetText( m_bioAgeEntryLeft, TheGameText->fetch(generalPlayer->getBioDOB()) );
	GadgetStaticTextSetText( m_bioBirthplaceEntryLeft, TheGameText->fetch(generalPlayer->getBioRank()) );
	GadgetStaticTextSetText( m_bioStrategyEntryLeft, TheGameText->fetch(generalPlayer->getBioStrategy()) );
	m_bioBigNameEntryRight->winHide(FALSE);
	m_bioNameEntryRight->winHide(FALSE);
//	m_bioAgeEntryRight->winHide(FALSE);
	m_bioBirthplaceEntryRight->winHide(FALSE);
	m_bioStrategyEntryRight->winHide(FALSE);
	GadgetStaticTextSetText( m_bioBigNameEntryRight, TheGameText->fetch(generalOpponent->getBioName()) );
	GadgetStaticTextSetText( m_bioNameEntryRight, TheGameText->fetch(generalOpponent->getBioName()) );
//	GadgetStaticTextSetText( m_bioAgeEntryRight, TheGameText->fetch(generalOpponent->getBioDOB()) );
	GadgetStaticTextSetText( m_bioBirthplaceEntryRight, TheGameText->fetch(generalOpponent->getBioRank()) );
	GadgetStaticTextSetText( m_bioStrategyEntryRight, TheGameText->fetch(generalOpponent->getBioStrategy()) );
	m_portraitLeft->winSetEnabledImage(0, generalPlayer->getBioPortraitLarge() );
	m_portraitRight->winSetEnabledImage(0, generalOpponent->getBioPortraitLarge() );
	m_portraitLeft->winHide(FALSE);
	m_portraitRight->winHide(FALSE);
	m_overlayReticleCircleAlphaOuter->winHide(FALSE);
	m_overlayReticleCircleAlphaInner->winHide(FALSE);
	m_overlayVsBackdrop->winHide(FALSE);
	m_overlayVsBackdrop->winHide(FALSE);
	m_overlayVs->winHide(FALSE);
	m_wndVideoManager->playMovie( m_overlayVs, "VSSmall", WINDOW_PLAY_MOVIE_SHOW_LAST_FRAME);
}


void ChallengeLoadScreen::init( GameInfo *game )
{
	const Campaign *campaign = TheCampaignManager->getCurrentCampaign();
	const Mission *mission = TheCampaignManager->getCurrentMission();

	// the player general is tied to the campaign
	const GeneralPersona* generalPlayer = TheChallengeGenerals->getPlayerGeneralByCampaignName( campaign->m_name );

	// the opponent general is tied to the mission
	DEBUG_ASSERTCRASH(mission->m_generalName.isNotEmpty(), ("No GeneralName associated with this mission, check Campaign.ini"));
	const GeneralPersona* generalOpponent = TheChallengeGenerals->getGeneralByGeneralName( mission->m_generalName );

	// create the layout of the load screen
	m_loadScreen = TheWindowManager->winCreateFromScript( "Menus/ChallengeLoadScreen.wnd" );
	DEBUG_ASSERTCRASH(m_loadScreen, ("Can't initialize the single player loadscreen"));
	m_loadScreen->winHide(FALSE);
	m_loadScreen->winBringToTop();

	// Store the pointer to the progress bar on the loadscreen
	m_progressBar = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( "ChallengeLoadScreen.wnd:ProgressLoad" ));
	DEBUG_ASSERTCRASH(m_progressBar, ("Can't initialize the progressbar for the single player loadscreen"));
	GadgetProgressBarSetProgress(m_progressBar, 0 );

	m_ambientLoop.setEventName("LoadScreenAmbient");

	// create the new background video stream
	m_videoStream = TheVideoPlayer->open( TheCampaignManager->getCurrentMission()->m_movieLabel );

	// Create the new buffer
	m_videoBuffer = TheDisplay->createVideoBuffer();
	if (m_videoBuffer == nullptr || !m_videoBuffer->allocate(	m_videoStream->width(), m_videoStream->height() ))
	{
		delete m_videoBuffer;
		m_videoBuffer = nullptr;

		if ( m_videoStream )
		{
			m_videoStream->close();
			m_videoStream = nullptr;
		}

		return;
	}

	// init overlays
	NameKeyType namekey = TheNameKeyGenerator->nameToKey( "ChallengeLoadScreen.wnd:PortraitLeft");
	m_portraitLeft = TheWindowManager->winGetWindowFromId( m_loadScreen, namekey );
	namekey = TheNameKeyGenerator->nameToKey( "ChallengeLoadScreen.wnd:PortraitRight");
	m_portraitRight = TheWindowManager->winGetWindowFromId( m_loadScreen, namekey );

	namekey = TheNameKeyGenerator->nameToKey( "ChallengeLoadScreen.wnd:PortraitMovieLeft");
	m_portraitMovieLeft = TheWindowManager->winGetWindowFromId( m_loadScreen, namekey );
	namekey = TheNameKeyGenerator->nameToKey( "ChallengeLoadScreen.wnd:PortraitMovieRight");
	m_portraitMovieRight = TheWindowManager->winGetWindowFromId( m_loadScreen, namekey );

//	namekey = TheNameKeyGenerator->nameToKey( "ChallengeLoadScreen.wnd:ReticleCrosshairs");
//	m_overlayReticleCrosshairs = TheWindowManager->winGetWindowFromId( m_loadScreen, namekey );
/*
	namekey = TheNameKeyGenerator->nameToKey( "ChallengeLoadScreen.wnd:OuterCircleLine");
	m_overlayReticleCircleLineOuter = TheWindowManager->winGetWindowFromId( m_loadScreen, namekey );
	namekey = TheNameKeyGenerator->nameToKey( "ChallengeLoadScreen.wnd:InnerCircleLine");
	m_overlayReticleCircleLineInner = TheWindowManager->winGetWindowFromId( m_loadScreen, namekey );
*/
	namekey = TheNameKeyGenerator->nameToKey( "ChallengeLoadScreen.wnd:CircleAlphaOuter");
	m_overlayReticleCircleAlphaOuter = TheWindowManager->winGetWindowFromId( m_loadScreen, namekey );
	namekey = TheNameKeyGenerator->nameToKey( "ChallengeLoadScreen.wnd:CircleAlphaInner");
	m_overlayReticleCircleAlphaInner = TheWindowManager->winGetWindowFromId( m_loadScreen, namekey );
	namekey = TheNameKeyGenerator->nameToKey( "ChallengeLoadScreen.wnd:VersusBackdrop");
	m_overlayVsBackdrop = TheWindowManager->winGetWindowFromId( m_loadScreen, namekey );
	namekey = TheNameKeyGenerator->nameToKey( "ChallengeLoadScreen.wnd:OverlayVs");
	m_overlayVs = TheWindowManager->winGetWindowFromId( m_loadScreen, namekey );

	namekey = TheNameKeyGenerator->nameToKey( "ChallengeLoadScreen.wnd:BioNameLeft");
	m_bioNameLeft = TheWindowManager->winGetWindowFromId( m_loadScreen, namekey );
//	namekey = TheNameKeyGenerator->nameToKey( "ChallengeLoadScreen.wnd:BioDOBLeft");
//	m_bioAgeLeft = TheWindowManager->winGetWindowFromId( m_loadScreen, namekey );
	namekey = TheNameKeyGenerator->nameToKey( "ChallengeLoadScreen.wnd:BioBirthplaceLeft");
	m_bioBirthplaceLeft = TheWindowManager->winGetWindowFromId( m_loadScreen, namekey );
	namekey = TheNameKeyGenerator->nameToKey( "ChallengeLoadScreen.wnd:BioStrategyLeft");
	m_bioStrategyLeft = TheWindowManager->winGetWindowFromId( m_loadScreen, namekey );
	namekey = TheNameKeyGenerator->nameToKey( "ChallengeLoadScreen.wnd:BigNameEntryLeft");
	m_bioBigNameEntryLeft = TheWindowManager->winGetWindowFromId( m_loadScreen, namekey );
	namekey = TheNameKeyGenerator->nameToKey( "ChallengeLoadScreen.wnd:BioNameEntryLeft");
	m_bioNameEntryLeft = TheWindowManager->winGetWindowFromId( m_loadScreen, namekey );
//	namekey = TheNameKeyGenerator->nameToKey( "ChallengeLoadScreen.wnd:BioDOBEntryLeft");
//	m_bioAgeEntryLeft = TheWindowManager->winGetWindowFromId( m_loadScreen, namekey );
	namekey = TheNameKeyGenerator->nameToKey( "ChallengeLoadScreen.wnd:BioBirthplaceEntryLeft");
	m_bioBirthplaceEntryLeft = TheWindowManager->winGetWindowFromId( m_loadScreen, namekey );
	namekey = TheNameKeyGenerator->nameToKey( "ChallengeLoadScreen.wnd:BioStrategyEntryLeft");
	m_bioStrategyEntryLeft = TheWindowManager->winGetWindowFromId( m_loadScreen, namekey );
	namekey = TheNameKeyGenerator->nameToKey( "ChallengeLoadScreen.wnd:BioNameRight");
	m_bioNameRight = TheWindowManager->winGetWindowFromId( m_loadScreen, namekey );
//	namekey = TheNameKeyGenerator->nameToKey( "ChallengeLoadScreen.wnd:BioDOBRight");
//	m_bioAgeRight = TheWindowManager->winGetWindowFromId( m_loadScreen, namekey );
	namekey = TheNameKeyGenerator->nameToKey( "ChallengeLoadScreen.wnd:BioBirthplaceRight");
	m_bioBirthplaceRight = TheWindowManager->winGetWindowFromId( m_loadScreen, namekey );
	namekey = TheNameKeyGenerator->nameToKey( "ChallengeLoadScreen.wnd:BioStrategyRight");
	m_bioStrategyRight = TheWindowManager->winGetWindowFromId( m_loadScreen, namekey );
	namekey = TheNameKeyGenerator->nameToKey( "ChallengeLoadScreen.wnd:BigNameEntryRight");
	m_bioBigNameEntryRight = TheWindowManager->winGetWindowFromId( m_loadScreen, namekey );
	namekey = TheNameKeyGenerator->nameToKey( "ChallengeLoadScreen.wnd:BioNameEntryRight");
	m_bioNameEntryRight = TheWindowManager->winGetWindowFromId( m_loadScreen, namekey );
//	namekey = TheNameKeyGenerator->nameToKey( "ChallengeLoadScreen.wnd:BioDOBEntryRight");
//	m_bioAgeEntryRight = TheWindowManager->winGetWindowFromId( m_loadScreen, namekey );
	namekey = TheNameKeyGenerator->nameToKey( "ChallengeLoadScreen.wnd:BioBirthplaceEntryRight");
	m_bioBirthplaceEntryRight = TheWindowManager->winGetWindowFromId( m_loadScreen, namekey );
	namekey = TheNameKeyGenerator->nameToKey( "ChallengeLoadScreen.wnd:BioStrategyEntryRight");
	m_bioStrategyEntryRight = TheWindowManager->winGetWindowFromId( m_loadScreen, namekey );


	// make sure reticle stuff starts out hidden
//	m_overlayReticleCircleLineOuter->winHide(TRUE);
//	m_overlayReticleCircleLineInner->winHide(TRUE);
	m_overlayReticleCircleAlphaOuter->winHide(TRUE);
	m_overlayReticleCircleAlphaInner->winHide(TRUE);
	m_overlayVsBackdrop->winHide(TRUE);
	m_overlayVs->winHide(TRUE);

	m_wndVideoManager = NEW WindowVideoManager;
	m_wndVideoManager->init();

	if(TheGameLODManager && TheGameLODManager->didMemPass())
	{
		// TheSuperHackers @bugfix Originally this movie render loop stopped rendering when the game window was inactive.
		// This either skipped the movie or caused decompression artifacts. Now the video just keeps playing until it done.

		Int progressUpdateCount = m_videoStream->frameCount() / FRAME_FUDGE_ADD;
		Int shiftedPercent = -FRAME_FUDGE_ADD + 1;
		while (m_videoStream->frameIndex() < m_videoStream->frameCount() - 1 )
		{
			// TheSuperHackers @feature User can now skip video by pressing ESC
			if (TheKeyboard)
			{
				TheKeyboard->UPDATE();
				KeyboardIO *io = TheKeyboard->findKey(KEY_ESC, KeyboardIO::STATUS_UNUSED);
				if (io && BitIsSet(io->state, KEY_STATE_DOWN))
				{
					io->setUsed();
					break;
				}
			}

			TheGameEngine->serviceWindowsOS();

			if(!m_videoStream->isFrameReady())
			{
				Sleep(1);
				continue;
			}

			m_videoStream->frameDecompress();
			m_videoStream->frameRender(m_videoBuffer);
			m_videoStream->frameNext();

			if(m_videoBuffer)
				m_loadScreen->winGetInstanceData()->setVideoBuffer(m_videoBuffer);

			Int frame = m_videoStream->frameIndex();
			if(frame % progressUpdateCount == 0)
			{
				shiftedPercent++;
				if(shiftedPercent >0)
					shiftedPercent = 0;
				Int percent = (shiftedPercent + FRAME_FUDGE_ADD)/1.3;
				UnicodeString per;
				per.format(L"%d%%",percent);
				TheMouse->setCursorTooltip(UnicodeString::TheEmptyString);
				GadgetProgressBarSetProgress(m_progressBar, percent);
			}
			TheWindowManager->update();

			activatePieces(frame, generalPlayer, generalOpponent);
			m_wndVideoManager->update();

			// redraw all views, update the GUI
			TheDisplay->draw();

			TheAudio->update();
		}
	}
	else
	{
		// if we're min speced
		m_videoStream->frameGoto(m_videoStream->frameCount()); // zero based
		while(!m_videoStream->isFrameReady())
			Sleep(1);
		m_videoStream->frameDecompress();
		m_videoStream->frameRender(m_videoBuffer);
		if(m_videoBuffer)
			m_loadScreen->winGetInstanceData()->setVideoBuffer(m_videoBuffer);

		activatePiecesMinSpec(generalPlayer, generalOpponent);

		Int delay = mission->m_voiceLength * 1000;
		Int begin = timeGetTime();
		Int currTime = begin;
		Int fudgeFactor = 0;
		while(begin + delay > currTime )
		{
			fudgeFactor = 30 * ((currTime - begin)/ INT_TO_REAL(delay ));
			GadgetProgressBarSetProgress(m_progressBar, fudgeFactor);

			TheWindowManager->update();
			TheDisplay->draw();
			Sleep(100);
			currTime = timeGetTime();
		}

		m_wndVideoManager->update();
		TheWindowManager->update();
		TheDisplay->draw();
	}
	setFPMode();


	AudioEventRTS event( generalOpponent->getRandomTauntSound() );
	TheAudio->addAudioEvent( &event );

	m_ambientLoopHandle = TheAudio->addAudioEvent(&m_ambientLoop);
	TheAudio->update();
}

void ChallengeLoadScreen::reset()
{
 setLoadScreen(nullptr);
 m_progressBar = nullptr;
}

void ChallengeLoadScreen::update( Int percent )
{
	percent = (percent + FRAME_FUDGE_ADD)/1.3;
	UnicodeString per;
	per.format(L"%d%%",percent);
	TheMouse->setCursorTooltip(UnicodeString::TheEmptyString);
	GadgetProgressBarSetProgress(m_progressBar, percent);

	// Do this last!
	LoadScreen::update( percent );
}

void ChallengeLoadScreen::setProgressRange( Int min, Int max )
{

}

// ShellGameLoadScreen Class //////////////////////////////////////////////////
//-----------------------------------------------------------------------------
ShellGameLoadScreen::ShellGameLoadScreen()
{
	m_progressBar = nullptr;
}

ShellGameLoadScreen::~ShellGameLoadScreen()
{
}

void ShellGameLoadScreen::init( GameInfo *game )
{
	static BOOL firstLoad = TRUE;


	// create the layout of the load screen
	m_loadScreen = TheWindowManager->winCreateFromScript( "Menus/ShellGameLoadScreen.wnd" );
	DEBUG_ASSERTCRASH(m_loadScreen, ("Can't initialize the ShellGame loadscreen"));
	m_loadScreen->winHide(FALSE);
	m_loadScreen->winBringToTop();

	// Store the pointer to the progress bar on the loadscreen
	m_progressBar = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( "ShellGameLoadScreen.wnd:ProgressLoad" ));
	DEBUG_ASSERTCRASH(m_progressBar, ("Can't initialize the progressbar for the single player loadscreen"));
	GadgetProgressBarSetProgress(m_progressBar, 0 );
	m_progressBar->winHide(TRUE);

	if(m_loadScreen && firstLoad && TheGameLODManager && TheGameLODManager->didMemPass())
	{
		m_loadScreen->winSetEnabledImage(0, TheMappedImageCollection->findImageByName("TitleScreen"));
		TheWritableGlobalData->m_breakTheMovie = FALSE;

		GameWindow *win = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( "ShellGameLoadScreen.wnd:StaticTextLegal" ));
		if(win)
			win->winHide(FALSE);
		firstLoad = FALSE;
	}
	m_progressBar->winHide(FALSE);
}

void ShellGameLoadScreen::reset()
{
 setLoadScreen(nullptr);
 m_progressBar = nullptr;
}

void ShellGameLoadScreen::update( Int percent )
{
	TheMouse->setCursorTooltip(UnicodeString::TheEmptyString);
	GadgetProgressBarSetProgress(m_progressBar, percent);

	// Do this last!
	LoadScreen::update( percent );
}

// MultiPlayerLoadScreen Class //////////////////////////////////////////////////
//-----------------------------------------------------------------------------
#if RTS_ZEROHOUR
// ---------------------------------------------------------------------------
// Battlefield-intel "win probability" card. Replaces the local general's
// portrait with a procedurally-drawn meter fed by radarvan (see
// RadarvanIntelReadyData). Rendered each frame through a GameWindow draw
// callback - no runtime textures and no new art. Only one multiplayer load
// screen is up at a time, so a single file-static card is sufficient.
// ---------------------------------------------------------------------------
enum { INTEL_CARD_AWAITING = 0, INTEL_CARD_READY = 1, INTEL_CARD_NODATA = 2 };

struct IntelCard
{
	Int state;
	RadarvanIntelData d;
	Color yourColor;
	Color enemyColor;
	Int localTeam;       // 1 or 2; 0 = observer/none
	DisplayString *pct;  // cached "NN%" number (owned by the load screen)
	const Image *gauge;  // ZuluGaugeDial art, or NULL -> primitive-bar fallback
};
static IntelCard s_intelCard = { INTEL_CARD_AWAITING };

// Gauge dial calibration, measured from the source art (fractions of the
// image rect): the needle pivot (brass hub) and the scale's end angles
// (degrees, 0 = right, 90 = up). Win% 0 -> 0deg-end, 100 -> 100deg-end.
static const Real INTEL_GAUGE_HUB_FX = 0.485f;
static const Real INTEL_GAUGE_HUB_FY = 0.614f;
static const Real INTEL_GAUGE_ANG0   = 163.3f; // needle angle at 0%
static const Real INTEL_GAUGE_ANG100 = 19.4f;  // needle angle at 100%
static const Real INTEL_GAUGE_ASPECT = 341.0f / 512.0f; // dial art h/w

// Small filled triangle (chevron) built from horizontal scanlines, since the
// display has no polygon primitive. `up` points the apex upward.
static void intelDrawChevron(Int cx, Int topY, Int halfW, Int h, Bool up, Color color)
{
	Int i;
	for (i = 0; i < h; ++i)
	{
		Int frac = up ? i : (h - 1 - i);
		Int denom = (h > 1) ? (h - 1) : 1;
		Int ww = (halfW * 2 * frac) / denom;
		TheDisplay->drawFillRect(cx - ww / 2, topY + i, ww, 1, color);
	}
}

// One team row: house-color chip on the left, a tall green/red win-loss bar
// filling the middle (hollow if the squad has no tracked record), and a
// hot/cold synergy chevron on the right. Sized to fill the given row height.
static void intelDrawTeamRow(Int x, Int top, Int w, Int rowH, Color chip,
                             Bool hasRec, Int wins, Int losses, Bool pair, Real delta)
{
	Int chipSz = rowH;
	Int gap = chipSz / 3;
	if (gap < 3) gap = 3;

	// house-color chip
	TheDisplay->drawFillRect(x, top, chipSz, chipSz, chip);
	TheDisplay->drawOpenRect(x, top, chipSz, chipSz, 1.0f, GameMakeColor(230, 230, 230, 200));

	// synergy chevron on the right (green up = hot duo, red down = cold duo)
	Int chevW = chipSz;
	Int chevX = x + w - chevW;
	if (pair)
	{
		Color c = (delta >= 0.0f) ? GameMakeColor(60, 200, 80, 255) : GameMakeColor(210, 70, 70, 255);
		intelDrawChevron(chevX + chevW / 2, top, chevW / 2, rowH, (delta >= 0.0f), c);
	}

	// win/loss ratio bar between chip and chevron
	Int wlX = x + chipSz + gap;
	Int wlRight = x + w - chevW - gap;
	Int wlW = wlRight - wlX;
	if (wlW < 4)
		return;
	Int wlH = (rowH * 6) / 10;
	Int wlY = top + (rowH - wlH) / 2;
	if (hasRec && (wins + losses) > 0)
	{
		Int gw = (wlW * wins) / (wins + losses);
		TheDisplay->drawFillRect(wlX, wlY, gw, wlH, GameMakeColor(60, 180, 70, 255));
		TheDisplay->drawFillRect(wlX + gw, wlY, wlW - gw, wlH, GameMakeColor(170, 60, 60, 255));
		TheDisplay->drawOpenRect(wlX, wlY, wlW, wlH, 1.0f, GameMakeColor(200, 210, 220, 180));
	}
	else
	{
		// no tracked record ("untested lineup") - hollow bar so the row reads
		TheDisplay->drawOpenRect(wlX, wlY, wlW, wlH, 1.0f, GameMakeColor(110, 120, 130, 160));
	}
}

// Primitive fallback layout (used when the gauge art isn't available):
// dark plate + odds split bar + two team rows.
static void drawIntelBar(Int x, Int y, Int w, Int h)
{
	// backing plate + border
	TheDisplay->drawFillRect(x, y, w, h, GameMakeColor(6, 8, 12, 220));
	TheDisplay->drawOpenRect(x, y, w, h, 1.0f, GameMakeColor(120, 140, 160, 200));

	Int pad = w / 12;
	if (pad < 8) pad = 8;
	Int innerX = x + pad;
	Int innerW = w - 2 * pad;

	Color yourC = (s_intelCard.localTeam != 0) ? s_intelCard.yourColor : GameMakeColor(60, 120, 220, 255);
	Color enemC = (s_intelCard.localTeam != 0) ? s_intelCard.enemyColor : GameMakeColor(200, 60, 60, 255);

	// odds bar sits in the upper-middle; team rows fill the space below it.
	Int barY = y + (h * 30) / 100;
	Int barH = (h * 17) / 100;

	if (s_intelCard.state == INTEL_CARD_NODATA)
	{
		Color red = GameMakeColor(200, 40, 40, 220);
		TheDisplay->drawLine(innerX, y + pad, innerX + innerW, y + h - pad, 2.0f, red);
		TheDisplay->drawLine(innerX + innerW, y + pad, innerX, y + h - pad, 2.0f, red);
		return;
	}

	if (s_intelCard.state != INTEL_CARD_READY)
	{
		// awaiting: empty gauge outline so the area reads as "gathering intel"
		TheDisplay->drawOpenRect(innerX, barY, innerW, barH, 1.0f, GameMakeColor(120, 140, 160, 160));
		return;
	}

	// your win probability (favoredWinProb is the FAVORED team's probability)
	Real yourProb;
	if (s_intelCard.localTeam == 0)
		yourProb = s_intelCard.d.favoredWinProb;
	else
		yourProb = (s_intelCard.d.favoredTeam == s_intelCard.localTeam)
			? s_intelCard.d.favoredWinProb
			: (1.0f - s_intelCard.d.favoredWinProb);
	if (yourProb < 0.0f) yourProb = 0.0f;
	if (yourProb > 1.0f) yourProb = 1.0f;

	// big percentage number, centered in the band above the bar
	if (s_intelCard.pct != NULL)
	{
		Int tw = 0, th = 0;
		s_intelCard.pct->getSize(&tw, &th);
		Int numY = y + ((barY - y) - th) / 2;
		if (numY < y + 2) numY = y + 2;
		s_intelCard.pct->draw(x + (w - tw) / 2, numY,
			GameMakeColor(255, 255, 255, 255), GameMakeColor(0, 0, 0, 255));
	}

	// odds split bar (hero)
	Int yourW = (Int)(innerW * yourProb + 0.5f);
	TheDisplay->drawFillRect(innerX, barY, yourW, barH, yourC);
	TheDisplay->drawFillRect(innerX + yourW, barY, innerW - yourW, barH, enemC);
	TheDisplay->drawOpenRect(innerX, barY, innerW, barH, 1.0f, GameMakeColor(230, 230, 230, 230));
	TheDisplay->drawLine(innerX + yourW, barY - 3, innerX + yourW, barY + barH + 3, 1.0f, GameMakeColor(255, 255, 255, 255));

	// map A/B (lobby team 1/2) onto left(=yours)/right(=enemy)
	Bool aIsYours = (s_intelCard.localTeam != 2);
	Bool  lRec = aIsYours ? s_intelCard.d.aHasRecord : s_intelCard.d.bHasRecord;
	Int   lW   = aIsYours ? s_intelCard.d.aWins      : s_intelCard.d.bWins;
	Int   lL   = aIsYours ? s_intelCard.d.aLosses    : s_intelCard.d.bLosses;
	Bool  rRec = aIsYours ? s_intelCard.d.bHasRecord : s_intelCard.d.aHasRecord;
	Int   rW   = aIsYours ? s_intelCard.d.bWins      : s_intelCard.d.aWins;
	Int   rL   = aIsYours ? s_intelCard.d.bLosses    : s_intelCard.d.aLosses;
	Bool  lPair = aIsYours ? s_intelCard.d.aHasPair : s_intelCard.d.bHasPair;
	Real  lDelta = aIsYours ? s_intelCard.d.aDelta  : s_intelCard.d.bDelta;
	Bool  rPair = aIsYours ? s_intelCard.d.bHasPair : s_intelCard.d.aHasPair;
	Real  rDelta = aIsYours ? s_intelCard.d.bDelta  : s_intelCard.d.aDelta;

	// two team rows, evenly filling the space from below the bar to the bottom
	Int rowsTop = barY + barH + (h * 8) / 100;
	Int rowGap = (h * 5) / 100;
	Int rowH = ((y + h - pad) - rowsTop - rowGap) / 2;
	if (rowH < 10) rowH = 10;
	intelDrawTeamRow(innerX, rowsTop, innerW, rowH, yourC, lRec, lW, lL, lPair, lDelta);
	intelDrawTeamRow(innerX, rowsTop + rowH + rowGap, innerW, rowH, enemC, rRec, rW, rL, rPair, rDelta);
}

// Gauge layout: the ZuluGaugeDial art fills the top (aspect-preserved) with a
// house-colored win-probability needle drawn over it and the win% as a digital
// readout in the dial's lower face; two team rows fill the strip below.
static void drawIntelGauge(Int x, Int y, Int w, Int h)
{
	Int imgH = (Int)(w * INTEL_GAUGE_ASPECT + 0.5f);
	if (imgH > h)
		imgH = h;
	TheDisplay->drawImage(s_intelCard.gauge, x, y, x + w, y + imgH, GameMakeColor(255, 255, 255, 255));

	Color yourC = (s_intelCard.localTeam != 0) ? s_intelCard.yourColor : GameMakeColor(60, 120, 220, 255);
	Color enemC = (s_intelCard.localTeam != 0) ? s_intelCard.enemyColor : GameMakeColor(200, 60, 60, 255);

	Int hubX = x + (Int)(w * INTEL_GAUGE_HUB_FX + 0.5f);
	Int hubY = y + (Int)(imgH * INTEL_GAUGE_HUB_FY + 0.5f);

	if (s_intelCard.state == INTEL_CARD_READY)
	{
		Real yourProb;
		if (s_intelCard.localTeam == 0)
			yourProb = s_intelCard.d.favoredWinProb;
		else
			yourProb = (s_intelCard.d.favoredTeam == s_intelCard.localTeam)
				? s_intelCard.d.favoredWinProb
				: (1.0f - s_intelCard.d.favoredWinProb);
		if (yourProb < 0.0f) yourProb = 0.0f;
		if (yourProb > 1.0f) yourProb = 1.0f;

		// needle angle (deg, 0=right/90=up), interpolated across the scale
		Real deg = INTEL_GAUGE_ANG0 + (INTEL_GAUGE_ANG100 - INTEL_GAUGE_ANG0) * yourProb;
		Real rad = deg * 3.14159265f / 180.0f;
		Int len = (Int)(w * 0.25f + 0.5f);
		Int tipX = hubX + (Int)(len * (Real)cos(rad));
		Int tipY = hubY - (Int)(len * (Real)sin(rad)); // screen y is down

		// needle: fixed high-visibility red (readable over any team colors),
		// with a drop shadow and a bright highlight core.
		Color needleC = GameMakeColor(220, 40, 40, 255);
		TheDisplay->drawLine(hubX + 1, hubY + 1, tipX + 1, tipY + 1, 3.0f, GameMakeColor(0, 0, 0, 160));
		TheDisplay->drawLine(hubX, hubY, tipX, tipY, 3.0f, needleC);
		TheDisplay->drawLine(hubX, hubY, tipX, tipY, 1.0f, GameMakeColor(255, 235, 235, 220));
		// hub cap
		TheDisplay->drawFillRect(hubX - 4, hubY - 4, 8, 8, GameMakeColor(20, 20, 20, 255));
		TheDisplay->drawOpenRect(hubX - 4, hubY - 4, 8, 8, 1.0f, needleC);

		// win% readout on the dark lower face of the dial
		if (s_intelCard.pct != NULL)
		{
			Int tw = 0, th = 0;
			s_intelCard.pct->getSize(&tw, &th);
			Int numY = hubY + (Int)(imgH * 0.06f);
			if (numY + th > y + imgH)
				numY = y + imgH - th;
			s_intelCard.pct->draw(x + (w - tw) / 2, numY,
				GameMakeColor(255, 240, 200, 255), GameMakeColor(0, 0, 0, 255));
		}

		// team rows in the strip below the dial
		Int stripTop = y + imgH + 4;
		Int avail = (y + h - 4) - stripTop;
		if (avail >= 20)
		{
			Int pad = w / 12;
			if (pad < 8) pad = 8;
			Int innerX = x + pad;
			Int innerW = w - 2 * pad;
			Int gap = avail / 12;
			if (gap < 2) gap = 2;
			Int rowH = (avail - gap) / 2;

			Bool aIsYours = (s_intelCard.localTeam != 2);
			Bool  lRec = aIsYours ? s_intelCard.d.aHasRecord : s_intelCard.d.bHasRecord;
			Int   lW   = aIsYours ? s_intelCard.d.aWins      : s_intelCard.d.bWins;
			Int   lL   = aIsYours ? s_intelCard.d.aLosses    : s_intelCard.d.bLosses;
			Bool  rRec = aIsYours ? s_intelCard.d.bHasRecord : s_intelCard.d.aHasRecord;
			Int   rW   = aIsYours ? s_intelCard.d.bWins      : s_intelCard.d.aWins;
			Int   rL   = aIsYours ? s_intelCard.d.bLosses    : s_intelCard.d.aLosses;
			Bool  lPair = aIsYours ? s_intelCard.d.aHasPair : s_intelCard.d.bHasPair;
			Real  lDelta = aIsYours ? s_intelCard.d.aDelta  : s_intelCard.d.bDelta;
			Bool  rPair = aIsYours ? s_intelCard.d.bHasPair : s_intelCard.d.aHasPair;
			Real  rDelta = aIsYours ? s_intelCard.d.bDelta  : s_intelCard.d.aDelta;

			intelDrawTeamRow(innerX, stripTop, innerW, rowH, yourC, lRec, lW, lL, lPair, lDelta);
			intelDrawTeamRow(innerX, stripTop + rowH + gap, innerW, rowH, enemC, rRec, rW, rL, rPair, rDelta);
		}
	}
	// AWAITING / NODATA: leave the dial without a needle (the features text
	// panel shows the "gathering intel" / "recon down" note).
}

static void drawIntelPortrait(GameWindow *win, WinInstanceData * /*inst*/)
{
	if (win == NULL || TheDisplay == NULL)
		return;

	Int x, y, w, h;
	win->winGetScreenPosition(&x, &y);
	win->winGetSize(&w, &h);

	if (s_intelCard.gauge != NULL)
		drawIntelGauge(x, y, w, h);
	else
		drawIntelBar(x, y, w, h);
}
#endif // RTS_ZEROHOUR

MultiPlayerLoadScreen::MultiPlayerLoadScreen()
{
	m_mapPreview = nullptr;
	m_portraitLocalGeneral = nullptr;
	m_featuresLocalGeneral = nullptr;
	m_nameLocalGeneral = nullptr;
	m_intelResolved = FALSE;
	m_intelWaitCalls = 0;

	for(Int i = 0; i < MAX_SLOTS; ++i)
	{
		m_buttonMapStartPosition[i] = nullptr;
		m_progressBars[i] = nullptr;
		m_playerNames[i] = nullptr;
		m_playerSide[i]= nullptr;
		m_playerLookup[i] = -1;
	}
}

MultiPlayerLoadScreen::~MultiPlayerLoadScreen()
{
	if(m_mapPreview)
	{
		m_mapPreview->winSetUserData(nullptr);
	}

#if RTS_ZEROHOUR
	// The intel card draw func and its cached % string are torn down with the
	// window; drop our reference so a fresh load screen starts clean.
	if (s_intelCard.pct != NULL)
	{
		TheDisplayStringManager->freeDisplayString(s_intelCard.pct);
		s_intelCard.pct = nullptr;
	}
	s_intelCard.state = INTEL_CARD_AWAITING;
#endif

	TheAudio->removeAudioEvent( AHSV_StopTheMusicFade );
//	TheAudio->stopAudio( AudioAffect_Music );
}

void MultiPlayerLoadScreen::init( GameInfo *game )
{
	// create the layout of the load screen
	m_loadScreen = TheWindowManager->winCreateFromScript( "Menus/MultiplayerLoadScreen.wnd" );
	DEBUG_ASSERTCRASH(m_loadScreen, ("Can't initialize the Multiplayer loadscreen"));
	m_loadScreen->winHide(FALSE);
	m_loadScreen->winBringToTop();

	setQuitRequested( FALSE );

	//
	// The quit button is only good for a game that waits on other players to finish loading.
	// Skirmish shares this screen and never waits, so there is nothing there to quit out of.
	//
	const Bool offerQuit = TheGameLogic->isInMultiplayerGame();
	GameWindow *buttonQuit = TheWindowManager->winGetWindowFromId( m_loadScreen,
		TheNameKeyGenerator->nameToKey( "MultiplayerLoadScreen.wnd:ButtonQuit" ) );
	if(buttonQuit)
		buttonQuit->winHide( !offerQuit );
	if(offerQuit)
	{
		// the player needs a cursor to hit the button with, and startNewGame() hides the cursor for
		// every load screen just before it gets here
		TheMouse->setVisibility( TRUE );
	}

	m_mapPreview = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( "MultiplayerLoadScreen.wnd:WinMapPreview"));
	GameSlot *lSlot = game->getSlot(game->getLocalSlotNum());
	const PlayerTemplate* pt;
	if (lSlot->getPlayerTemplate() >= 0)
		pt = ThePlayerTemplateStore->getNthPlayerTemplate(lSlot->getPlayerTemplate());
	else
		pt = ThePlayerTemplateStore->findPlayerTemplate( TheNameKeyGenerator->nameToKey("FactionObserver") );

#if RTS_GENERALS
	const Image *loadScreenImage = TheMappedImageCollection->findImageByName(pt->getLoadScreen());
	if(loadScreenImage)
		m_loadScreen->winSetEnabledImage(0, loadScreenImage);
#else
	// add portrait, features, and name for the local player's general
	const GeneralPersona *localGeneral = TheChallengeGenerals->getGeneralByTemplateName( pt->getName() );
	const Image *portrait = nullptr;
	UnicodeString localName;
	if (localGeneral)
	{
		portrait = localGeneral->getBioPortraitLarge();
		localName = TheGameText->fetch( localGeneral->getBioName() );
	}
	else
	{
		// the main original factions don't have associated generals
		AsciiString imageName;
		if (pt->getName() == "FactionAmerica")
			portrait = TheMappedImageCollection->findImageByName("SAFactionLogoLg_US");
		else if (pt->getName() == "FactionGLA")
			portrait = TheMappedImageCollection->findImageByName("SUFactionLogoLg_GLA");
		else if (pt->getName() == "FactionChina")
			portrait = TheMappedImageCollection->findImageByName("SNFactionLogoLg_China");
		else
			DEBUG_CRASH(("Unexpected player template"));

		localName = pt->getDisplayName();
	}
	m_portraitLocalGeneral = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( "MultiplayerLoadScreen.wnd:LocalGeneralPortrait"));
	m_portraitLocalGeneral->winSetEnabledImage( 0, portrait);
	m_featuresLocalGeneral = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( "MultiplayerLoadScreen.wnd:LocalGeneralFeatures"));
	AsciiString features = pt->getGeneralFeatures();
	GadgetStaticTextSetText( m_featuresLocalGeneral, TheGameText->fetch( features.isEmpty() ? "GUI:PlayerObserver" : pt->getGeneralFeatures() ) );
	// The general-features panel is repurposed as the radarvan "battlefield
	// intel" area. Show a themed placeholder until the (non-blocking) worker
	// started at countdown returns; update() swaps in the real intel or a
	// fallback note. Reset the poll state for this game.
	m_intelResolved = FALSE;
	m_intelWaitCalls = 0;
	if (m_featuresLocalGeneral && !TheGlobalData->m_predictUrl.isEmpty())
	{
		UnicodeString awaiting;
		awaiting.translate(AsciiString("BATTLEFIELD INTEL\nRecon inbound, General..."));
		GadgetStaticTextSetText( m_featuresLocalGeneral, awaiting );
	}
	m_nameLocalGeneral = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( "MultiplayerLoadScreen.wnd:LocalGeneralName"));
	// Show the faction (side) name rather than the specific general's name.
	{
		AsciiString sideKey;
		sideKey.format("SIDE:%s", pt->getSide().str());
		GadgetStaticTextSetText( m_nameLocalGeneral, TheGameText->fetch(sideKey) );
	}

	// Replace the general portrait with the battlefield-intel meter card.
	{
		s_intelCard.state = INTEL_CARD_AWAITING;
		s_intelCard.localTeam = 0;
		s_intelCard.yourColor = GameMakeColor(60, 120, 220, 255);
		s_intelCard.enemyColor = GameMakeColor(200, 60, 60, 255);
		// gauge dial art (shipped in Zulu.big); NULL -> primitive-bar fallback
		s_intelCard.gauge = TheMappedImageCollection
			? TheMappedImageCollection->findImageByName("ZuluGaugeDial") : NULL;
		const GameSlot *localSlot = game->getConstSlot(game->getLocalSlotNum());
		if (localSlot)
		{
			Int myTeam = localSlot->getTeamNumber(); // 0-based, -1 = none
			s_intelCard.localTeam = (myTeam >= 0) ? (myTeam + 1) : 0;
			s_intelCard.yourColor = TheMultiplayerSettings->getColor(localSlot->getApparentColor())->getColor();
			Int si;
			for (si = 0; si < MAX_SLOTS; ++si)
			{
				const GameSlot *es = game->getConstSlot(si);
				if (!es || !es->isOccupied())
					continue;
				if (es->getPlayerTemplate() == PLAYERTEMPLATE_OBSERVER)
					continue;
				if (es->getTeamNumber() == myTeam)
					continue;
				s_intelCard.enemyColor = TheMultiplayerSettings->getColor(es->getApparentColor())->getColor();
				break;
			}
		}
		if (s_intelCard.pct == NULL)
			s_intelCard.pct = TheDisplayStringManager->newDisplayString();
		if (s_intelCard.pct != NULL)
		{
			// Give the win% its own large bold font (the portrait's default font
			// is tiny) so it anchors the top of the card. Size to the box.
			GameFont *bigFont = NULL;
			if (m_portraitLocalGeneral != NULL)
			{
				Int pw = 0, ph = 0;
				m_portraitLocalGeneral->winGetSize(&pw, &ph);
				Int ptSize = ph / 6;
				if (ptSize < 18) ptSize = 18;
				if (ptSize > 40) ptSize = 40;
				bigFont = TheFontLibrary->getFont(AsciiString("Arial"), ptSize, TRUE);
			}
			if (bigFont != NULL)
				s_intelCard.pct->setFont(bigFont);
			else if (m_portraitLocalGeneral != NULL)
				s_intelCard.pct->setFont(m_portraitLocalGeneral->winGetFont());
			s_intelCard.pct->setText(UnicodeString::TheEmptyString);
		}
		if (m_portraitLocalGeneral != NULL)
			m_portraitLocalGeneral->winSetDrawFunc(drawIntelPortrait);
	}
#endif

	AsciiString musicName = pt->getLoadScreenMusic();
	if ( ! musicName.isEmpty() )
	{
		TheAudio->removeAudioEvent( AHSV_StopTheMusicFade );
		AudioEventRTS event( musicName );
		event.setShouldFade( TRUE );

		TheAudio->addAudioEvent( &event );
		TheAudio->update();//Since GameEngine::update() is suspended until after I am gone...

	}

	//DEBUG_ASSERTCRASH(TheNetwork, ("Where the Heck is the Network!!!!"));
	//DEBUG_LOG(("NumPlayers %d", TheNetwork->getNumPlayers()));

	GameWindow *teamWin[MAX_SLOTS];
	Int i = 0;
	for (; i < MAX_SLOTS; ++i)
	{
		teamWin[i] = nullptr;
	}

	Int netSlot = 0;
	// Loop through and make the loadscreen look all good.
	for (i = 0; i < MAX_SLOTS; ++i)
	{
		// Load the Progress Bar
		AsciiString winName;
		winName.format( "MultiplayerLoadScreen.wnd:ProgressLoad%d",i);
		m_progressBars[i] = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( winName ));
		DEBUG_ASSERTCRASH(m_progressBars[i], ("Can't initialize the progressbars for the Multiplayer loadscreen"));
		// set the progressbar to zero
		GadgetProgressBarSetProgress(m_progressBars[i], 0 );

		// Load MapStart Positions
		winName.format( "MultiplayerLoadScreen.wnd:ButtonMapStartPosition%d",i);
		m_buttonMapStartPosition[i] = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( winName ));
		DEBUG_ASSERTCRASH(m_buttonMapStartPosition[i], ("Can't initialize the MapStart Positions for the MultiplayerLoadScreen loadscreen"));


		// Load the Player's name
		winName.format( "MultiplayerLoadScreen.wnd:StaticTextPlayer%d",i);
		m_playerNames[i] = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( winName ));
		DEBUG_ASSERTCRASH(m_playerNames[i], ("Can't initialize the Names for the Multiplayer loadscreen"));

		// Load the Player's Side
		winName.format( "MultiplayerLoadScreen.wnd:StaticTextSide%d",i);
		m_playerSide[i] = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( winName ));
		DEBUG_ASSERTCRASH(m_playerSide[i], ("Can't initialize the Sides for the Multiplayer loadscreen"));

		winName.format( "MultiplayerLoadScreen.wnd:StaticTextTeam%d",i);
		teamWin[i] = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( winName ));

		// get the slot man!
		GameSlot *slot = game->getSlot(i);
		if (!slot || !slot->isOccupied())
			continue;

		Color houseColor = TheMultiplayerSettings->getColor(slot->getApparentColor())->getColor();
#if RTS_GENERALS
		GadgetProgressBarSetEnabledBarColor(m_progressBars[netSlot],houseColor );
#else
		// format the progress bar to house colors
		AsciiString imageName;
		imageName.format("LoadingBar_ProgressCenter%d", slot->getApparentColor());
		const Image *houseImage = TheMappedImageCollection->findImageByName(imageName);
		if (! houseImage)
			houseImage = TheMappedImageCollection->findImageByName("LoadingBar_Progress");
		m_progressBars[netSlot]->winSetEnabledImage( 6, houseImage );
#endif

		UnicodeString name = slot->getName();
		GadgetStaticTextSetText(m_playerNames[netSlot], name );
		m_playerNames[netSlot]->winSetEnabledTextColors(houseColor, m_playerNames[netSlot]->winGetEnabledTextBorderColor());

		GadgetStaticTextSetText(m_playerSide[netSlot], slot->getApparentPlayerTemplateDisplayName() );
		m_playerSide[netSlot]->winSetEnabledTextColors(houseColor, m_playerSide[netSlot]->winGetEnabledTextBorderColor());

		if (slot->isAI() && m_progressBars[netSlot])
			m_progressBars[netSlot]->winHide(TRUE);

		if (teamWin[netSlot])
		{
			AsciiString teamStr;
			teamStr.format("Team:%d", slot->getTeamNumber() + 1);
			GadgetStaticTextSetText(teamWin[netSlot], TheGameText->fetch(teamStr));
			teamWin[netSlot]->winSetEnabledTextColors(houseColor, m_playerNames[netSlot]->winGetEnabledTextBorderColor());
		}

		m_playerLookup[i] = netSlot; // save our mapping so we can update progress correctly

		netSlot++;
	}

	for(i = netSlot; i < MAX_SLOTS; ++i)
	{
		m_progressBars[i]->winHide(TRUE);
		m_playerNames[i]->winHide(TRUE);
		m_playerSide[i]->winHide(TRUE);
		teamWin[i]->winHide(TRUE);
	}

	if(m_mapPreview)
	{
		const MapMetaData *mmd = TheMapCache->findMap(game->getMap());
		Image *image = getMapPreviewImage(game->getMap());
		m_mapPreview->winSetUserData((void *)mmd);

		positionStartSpots( game, m_buttonMapStartPosition, m_mapPreview);
		updateMapStartSpots( game, m_buttonMapStartPosition, TRUE );
		//positionAdditionalImages((MapMetaData *)mmd, m_mapPreview, TRUE);
		if(image)
		{
			m_mapPreview->winSetStatus(WIN_STATUS_IMAGE);
			m_mapPreview->winSetEnabledImage(0, image);
		}
		else
		{
			m_mapPreview->winClearStatus(WIN_STATUS_IMAGE);
		}
	}


	TheGameLogic->initTimeOutValues();
}

void MultiPlayerLoadScreen::reset()
{
	setLoadScreen(nullptr);
	for(Int i = 0; i < MAX_SLOTS; ++i)
	{
		m_progressBars[i] = nullptr;
		m_playerNames[i] = nullptr;
		m_playerSide[i]= nullptr;
	}
}

void MultiPlayerLoadScreen::update( Int percent )
{
	if (TheNetwork)
	{
		if(percent <= 100)
			TheNetwork->updateLoadProgress( percent );
		TheNetwork->liteupdate();
	}
	else
	{
		if (percent <= 100)
			TheGameLogic->processProgress( TheGameInfo->getLocalSlotNum(), percent );
	}

	//GadgetProgressBarSetProgress(m_progressBars[TheNetwork->getLocalPlayerID()], percent );

	TheMouse->setCursorTooltip(UnicodeString::TheEmptyString);

#if RTS_ZEROHOUR
	// Poll the radarvan "battlefield intel" worker (started at countdown) and
	// drop the result into the general-features panel. Purely display; this
	// never blocks loading. Once resolved (real intel or fallback) we stop.
	if (!m_intelResolved && m_featuresLocalGeneral)
	{
		++m_intelWaitCalls;
		AsciiString intelText;
		Bool ready = RadarvanIntelReady(intelText);
		Bool pending = RadarvanIntelPending();
		// Close the ready/pending race: the worker sets "ready" and clears
		// "pending" together under one lock, so if we saw "not ready" but also
		// "not pending", it just finished - re-read readiness before giving up.
		if (!ready && !pending)
			ready = RadarvanIntelReady(intelText);
		if (ready)
		{
			UnicodeString u;
			u.translate(intelText);
			GadgetStaticTextSetText(m_featuresLocalGeneral, u);
			// Drive the portrait meter card from the same result.
			RadarvanIntelReadyData(s_intelCard.d);
			// Use the team the worker actually predicted against (data.localTeam
			// == job.localTeam) for the win% math, not the raw slot team derived
			// at setup. They differ in a 1v1 with no teams assigned: the worker
			// synthesizes team 1 vs 2, but the slot's getTeamNumber() is -1 there,
			// which would leave localTeam == 0 and make the needle/readout show
			// the FAVORED team's odds instead of the local player's own.
			s_intelCard.localTeam = s_intelCard.d.localTeam;
			s_intelCard.state = INTEL_CARD_READY;
			if (s_intelCard.pct != NULL)
			{
				Real yp;
				if (s_intelCard.localTeam == 0)
					yp = s_intelCard.d.favoredWinProb;
				else
					yp = (s_intelCard.d.favoredTeam == s_intelCard.localTeam)
						? s_intelCard.d.favoredWinProb
						: (1.0f - s_intelCard.d.favoredWinProb);
				Int pctI = (Int)(yp * 100.0f + 0.5f);
				AsciiString ps;
				ps.format("%d%%", pctI);
				UnicodeString pu;
				pu.translate(ps);
				s_intelCard.pct->setText(pu);
			}
			m_intelResolved = TRUE;
		}
		else if (!pending || m_intelWaitCalls > 3000)
		{
			// Worker finished without a usable result (or the long backstop
			// tripped): show the themed fallback and give up. NOTE: we must NOT
			// fall back merely because loading hit 100% - after local load the
			// game spins in a wait-for-players loop calling update(101) ~10x/s,
			// which is precisely the window a late result arrives in. We keep
			// polling there and only give up once the worker itself is done.
			UnicodeString u;
			u.translate(AsciiString("BATTLEFIELD INTEL\nRecon uplink is down, General.\nNo intel on this engagement.\nTrust your instincts."));
			GadgetStaticTextSetText(m_featuresLocalGeneral, u);
			s_intelCard.state = INTEL_CARD_NODATA;
			m_intelResolved = TRUE;
		}
	}
#endif

	// Do this last!
	LoadScreen::update( percent );
}

void MultiPlayerLoadScreen::processProgress(Int playerId, Int percentage)
{

	if( percentage < 0 || percentage > 100 || playerId >= MAX_SLOTS || playerId < 0 || m_playerLookup[playerId] == -1)
	{
		DEBUG_CRASH(("Percentage %d was passed in for Player %d", percentage, playerId));
		return;
	}
	//DEBUG_LOG(("Percentage %d was passed in for Player %d (in loadscreen position %d)", percentage, playerId, m_playerLookup[playerId]));
	if(m_progressBars[m_playerLookup[playerId]])
		GadgetProgressBarSetProgress(m_progressBars[m_playerLookup[playerId]], percentage );
}

// GameSpyLoadScreen Class //////////////////////////////////////////////////
//-----------------------------------------------------------------------------
GameSpyLoadScreen::GameSpyLoadScreen()
{

	m_mapPreview = nullptr;
	m_portraitLocalGeneral = nullptr;
	m_featuresLocalGeneral = nullptr;
	m_nameLocalGeneral = nullptr;

	for(Int i = 0; i < MAX_SLOTS; ++i)
	{

		m_buttonMapStartPosition[i] = nullptr;
		m_playerRank[i] = nullptr;

		m_playerOfficerMedal[i] = nullptr;
		m_progressBars[i] = nullptr;
		m_playerNames[i] = nullptr;
		m_playerSide[i]= nullptr;
		m_playerLookup[i] = -1;
		m_playerFavoriteFactions[i]= nullptr;
		m_playerTotalDisconnects[i]= nullptr;
		m_playerWin[i]= nullptr;
		m_playerWinLosses[i]= nullptr;
	}
}

GameSpyLoadScreen::~GameSpyLoadScreen()
{
	if(m_mapPreview)
	{
		m_mapPreview->winSetUserData(nullptr);
	}
}

extern Int GetAdditionalDisconnectsFromUserFile(Int playerID);

void GameSpyLoadScreen::init( GameInfo *game )
{
	// create the layout of the load screen
	m_loadScreen = TheWindowManager->winCreateFromScript( "Menus/GameSpyLoadScreen.wnd" );
	DEBUG_ASSERTCRASH(m_loadScreen, ("Can't initialize the Multiplayer loadscreen"));
	m_loadScreen->winHide(FALSE);
	m_loadScreen->winBringToTop();
	m_mapPreview = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( "GameSpyLoadScreen.wnd:WinMapPreview"));
	DEBUG_ASSERTCRASH(TheNetwork, ("Where the Heck is the Network!!!!"));
	DEBUG_LOG(("NumPlayers %d", TheNetwork->getNumPlayers()));
GameSlot *lSlot = game->getSlot(game->getLocalSlotNum());
	const PlayerTemplate* pt;
	if (lSlot->getPlayerTemplate() >= 0)
		pt = ThePlayerTemplateStore->getNthPlayerTemplate(lSlot->getPlayerTemplate());
	else
		pt = ThePlayerTemplateStore->findPlayerTemplate( TheNameKeyGenerator->nameToKey("FactionObserver") );

#if RTS_GENERALS
	const Image *loadScreenImage = TheMappedImageCollection->findImageByName(pt->getLoadScreen());
	if(loadScreenImage)
		m_loadScreen->winSetEnabledImage(0, loadScreenImage);
#else
	// add portrait, features, and name for the local player's general
	const GeneralPersona *localGeneral = TheChallengeGenerals->getGeneralByTemplateName( pt->getName() );
	const Image *portrait = nullptr;
	UnicodeString localName;
	if (localGeneral)
	{
		portrait = localGeneral->getBioPortraitLarge();
		localName = TheGameText->fetch( localGeneral->getBioName() );
	}
	else
	{
		// the main original factions don't have associated generals
		AsciiString imageName;
		if (pt->getName() == "FactionAmerica")
			portrait = TheMappedImageCollection->findImageByName("SAFactionLogo144_US");
		else if (pt->getName() == "FactionGLA")
			portrait = TheMappedImageCollection->findImageByName("SUFactionLogo144_GLA");
		else if (pt->getName() == "FactionChina")
			portrait = TheMappedImageCollection->findImageByName("SNFactionLogo144_China");
		else
			DEBUG_CRASH(("Unexpected player template"));

		localName = pt->getDisplayName();
	}
	m_portraitLocalGeneral = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( "GameSpyLoadScreen.wnd:LocalGeneralPortrait"));
	m_portraitLocalGeneral->winSetEnabledImage( 0, portrait);
	m_featuresLocalGeneral = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( "GameSpyLoadScreen.wnd:LocalGeneralFeatures"));
	AsciiString features = pt->getGeneralFeatures();
	GadgetStaticTextSetText( m_featuresLocalGeneral, TheGameText->fetch( features.isEmpty() ? "GUI:PlayerObserver" : pt->getGeneralFeatures() ) );
	m_nameLocalGeneral = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( "GameSpyLoadScreen.wnd:LocalGeneralName"));
	GadgetStaticTextSetText( m_nameLocalGeneral, localName );
#endif

	GameWindow *teamWin[MAX_SLOTS];
	Int i = 0;
	for (; i < MAX_SLOTS; ++i)
	{
		teamWin[i] = nullptr;
	}

	Int netSlot = 0;
	// Loop through and make the loadscreen look all good.
	for (i = 0; i < MAX_SLOTS; ++i)
	{
		// Load the Progress Bar
		AsciiString winName;
		winName.format( "GameSpyLoadScreen.wnd:ProgressLoad%d",i);
		m_progressBars[i] = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( winName ));
		DEBUG_ASSERTCRASH(m_progressBars[i], ("Can't initialize the progressbars for the GameSpyLoadScreen loadscreen"));
		// set the progressbar to zero
		GadgetProgressBarSetProgress(m_progressBars[i], 0 );

		// Load the Player's name
		winName.format( "GameSpyLoadScreen.wnd:StaticTextPlayer%d",i);
		m_playerNames[i] = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( winName ));
		DEBUG_ASSERTCRASH(m_playerNames[i], ("Can't initialize the Names for the GameSpyLoadScreen loadscreen"));

		// Load MapStart Positions
		winName.format( "GameSpyLoadScreen.wnd:ButtonMapStartPosition%d",i);
		m_buttonMapStartPosition[i] = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( winName ));
		DEBUG_ASSERTCRASH(m_buttonMapStartPosition[i], ("Can't initialize the MapStart Positions for the GameSpyLoadScreen loadscreen"));


		// Load the Player's Side
		winName.format( "GameSpyLoadScreen.wnd:StaticTextSide%d",i);
		m_playerSide[i] = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( winName ));
		DEBUG_ASSERTCRASH(m_playerSide[i], ("Can't initialize the Sides for the GameSpyLoadScreen loadscreen"));

		// Load the Player's window
		winName.format( "GameSpyLoadScreen.wnd:WinPlayer%d",i);
		m_playerWin[i] = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( winName ));
		DEBUG_ASSERTCRASH(m_playerWin[i], ("Can't initialize the WinPlayer for the GameSpyLoadScreen loadscreen"));

		// Load the Player's m_playerTotalDisconnects
		winName.format( "GameSpyLoadScreen.wnd:StaticTextTotalDisconnects%d",i);
		m_playerTotalDisconnects[i] = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( winName ));
		DEBUG_ASSERTCRASH(m_playerTotalDisconnects[i], ("Can't initialize the m_playerTotalDisconnects for the GameSpyLoadScreen loadscreen"));

//		// Load the Player's m_playerFavoriteFactions
//		winName.format( "GameSpyLoadScreen.wnd:StaticTextFavoriteFaction%d",i);
//		m_playerFavoriteFactions[i] = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( winName ));
//		DEBUG_ASSERTCRASH(m_playerFavoriteFactions[i], ("Can't initialize the StaticTextFavoriteFaction for the GameSpyLoadScreen loadscreen"));

		// Load the Player's m_playerWinLosses
		winName.format( "GameSpyLoadScreen.wnd:StaticTextWinLoss%d",i);
		m_playerWinLosses[i] = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( winName ));
		DEBUG_ASSERTCRASH(m_playerWinLosses[i], ("Can't initialize the m_playerWinLosses for the GameSpyLoadScreen loadscreen"));

		// Load the Player's m_playerWinLosses
		winName.format( "GameSpyLoadScreen.wnd:WinRank%d",i);
		m_playerRank[i] = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( winName ));
		DEBUG_ASSERTCRASH(m_playerRank[i], ("Can't initialize the m_playerRank for the GameSpyLoadScreen loadscreen"));

		// Load the Player's m_playerOfficerMedal
		winName.format( "GameSpyLoadScreen.wnd:WinOfficer%d",i);
		m_playerOfficerMedal[i] = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( winName ));
		DEBUG_ASSERTCRASH(m_playerOfficerMedal[i], ("Can't initialize the m_playerOfficerMedal for the GameSpyLoadScreen loadscreen"));

		winName.format( "MultiplayerLoadScreen.wnd:StaticTextTeam%d",i);
		teamWin[i] = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( winName ));

		// get the slot man!
		GameSpyGameSlot *slot = (GameSpyGameSlot *)game->getSlot(i);
		if (!slot || !slot->isOccupied())
			continue;

		Color houseColor = TheMultiplayerSettings->getColor(slot->getApparentColor())->getColor();
#if RTS_GENERALS
		GadgetProgressBarSetEnabledBarColor(m_progressBars[netSlot],houseColor );
#else
		// format the progress bar to house colors
		AsciiString imageName;
		imageName.format("LoadingBar_ProgressCenter%d", slot->getApparentColor());
		const Image *houseImage = TheMappedImageCollection->findImageByName(imageName);
		if (! houseImage)
			houseImage = TheMappedImageCollection->findImageByName("LoadingBar_Progress");
		m_progressBars[netSlot]->winSetEnabledImage( 6, houseImage );
#endif

		UnicodeString name = slot->getName();
		GadgetStaticTextSetText(m_playerNames[netSlot], name );
		m_playerNames[netSlot]->winSetEnabledTextColors(houseColor, m_playerNames[netSlot]->winGetEnabledTextBorderColor());

		// Get the stats for the player
		PSPlayerStats stats = TheGameSpyPSMessageQueue->findPlayerStatsByID(slot->getProfileID());
		DEBUG_LOG(("LoadScreen - populating info for %ls(%d) - stats returned id %d",
			slot->getName().str(), slot->getProfileID(), stats.id));

		Bool isPreorder = TheGameSpyInfo->didPlayerPreorder(stats.id);
		Int rankPoints = CalculateRank(stats);
		Int favSide = GetFavoriteSide(stats);
		const Image *preorderImg = TheMappedImageCollection->findImageByName("OfficersClubsmall");
		if (!isPreorder)
			preorderImg = nullptr;
		const Image *rankImg = LookupSmallRankImage(favSide, rankPoints);
		m_playerOfficerMedal[i]->winSetEnabledImage(0, preorderImg);
		m_playerRank[i]->winSetEnabledImage(0, rankImg);

		UnicodeString formatString;

		// pop wins and losses
		Int numLosses = 0;
		PerGeneralMap::iterator it;
		for(it = stats.losses.begin(); it != stats.losses.end(); ++it)
		{
			numLosses += it->second;
		}
		Int numWins = 0;
		for(it =stats.wins.begin(); it != stats.wins.end(); ++it)
		{
			numWins += it->second;
		}
		formatString.format(L"%d/%d", numWins, numLosses);
		GadgetStaticTextSetText(m_playerWinLosses[netSlot], formatString);
		m_playerWinLosses[netSlot]->winSetEnabledTextColors(houseColor, m_playerWinLosses[netSlot]->winGetEnabledTextBorderColor());
		// favoriteFaction
			Int numGames = 0;
		Int favorite = 0;
		for(it =stats.games.begin(); it != stats.games.end(); ++it)
		{
			if(it->second >= numGames)
			{
				numGames = it->second;
				favorite = it->first;
			}
		}
//		if(numGames == 0)
//			GadgetStaticTextSetText(m_playerFavoriteFactions[netSlot], TheGameText->fetch("GUI:None"));
//		else if( stats.gamesAsRandom > numGames )
//			GadgetStaticTextSetText(m_playerFavoriteFactions[netSlot], TheGameText->fetch("GUI:Random"));
//		else
//		{
//			const PlayerTemplate *fac = ThePlayerTemplateStore->getNthPlayerTemplate(favorite);
//			if (fac)
//			{
//				AsciiString side;
//				side.format("SIDE:%s", fac->getSide().str());
//
//				GadgetStaticTextSetText(m_playerFavoriteFactions[netSlot], TheGameText->fetch(side));
//			}
//		}
//		m_playerFavoriteFactions[netSlot]->winSetEnabledTextColors(houseColor, m_playerFavoriteFactions[netSlot]->winGetEnabledTextBorderColor());
		// disconnects
		numGames = 0;
		for(it =stats.discons.begin(); it != stats.discons.end(); ++it)
		{
			numGames += it->second;
		}
		for(it =stats.desyncs.begin(); it != stats.desyncs.end(); ++it)
		{
			numGames += it->second;
		}
		numGames += GetAdditionalDisconnectsFromUserFile(stats.id);

		formatString.format(L"%d", numGames);
		GadgetStaticTextSetText(m_playerTotalDisconnects[netSlot], formatString);
		m_playerTotalDisconnects[netSlot]->winSetEnabledTextColors(houseColor, m_playerTotalDisconnects[netSlot]->winGetEnabledTextBorderColor());
		GadgetStaticTextSetText(m_playerSide[netSlot], slot->getApparentPlayerTemplateDisplayName() );
		m_playerSide[netSlot]->winSetEnabledTextColors(houseColor, m_playerSide[netSlot]->winGetEnabledTextBorderColor());

		if (slot->isAI())
		{
			if (m_progressBars[netSlot])
				m_progressBars[netSlot]->winHide(TRUE);
			if (m_playerTotalDisconnects[netSlot])
				m_playerTotalDisconnects[netSlot]->winHide(TRUE);
//			if (m_playerFavoriteFactions[netSlot])
//				m_playerFavoriteFactions[netSlot]->winHide(TRUE);
			if (m_playerWinLosses[netSlot])
				m_playerWinLosses[netSlot]->winHide(TRUE);
			if (m_playerRank[netSlot])
				m_playerRank[netSlot]->winHide(TRUE);
			if (m_playerOfficerMedal[netSlot])
				m_playerOfficerMedal[netSlot]->winHide(TRUE);
		}

		if (teamWin[netSlot])
		{
			AsciiString teamStr;
			teamStr.format("Team:%d", slot->getTeamNumber() + 1);
			if (slot->isAI() && slot->getTeamNumber() == -1)
				teamStr = "Team:AI";
			GadgetStaticTextSetText(teamWin[netSlot], TheGameText->fetch(teamStr));
			teamWin[netSlot]->winSetEnabledTextColors(houseColor, m_playerNames[netSlot]->winGetEnabledTextBorderColor());
		}

		m_playerLookup[i] = netSlot; // save our mapping so we can update progress correctly

		netSlot++;
	}

	for(i = netSlot; i < MAX_SLOTS; ++i)
	{
		m_playerWin[i]->winHide(TRUE);
		//m_playerNames[i]->winHide(TRUE);
		//m_playerSide[i]->winHide(TRUE);
	}

	if(m_mapPreview)
	{
		const MapMetaData *mmd = TheMapCache->findMap(game->getMap());
		Image *image = getMapPreviewImage(game->getMap());
		m_mapPreview->winSetUserData((void *)mmd);

		positionStartSpots( game, m_buttonMapStartPosition, m_mapPreview);
		updateMapStartSpots( game, m_buttonMapStartPosition, TRUE );
		//positionAdditionalImages((MapMetaData *)mmd, m_mapPreview, TRUE);
		if(image)
		{
			m_mapPreview->winSetStatus(WIN_STATUS_IMAGE);
			m_mapPreview->winSetEnabledImage(0, image);
		}
		else
		{
			m_mapPreview->winClearStatus(WIN_STATUS_IMAGE);
		}
	}

	TheGameLogic->initTimeOutValues();
}

void GameSpyLoadScreen::reset()
{
	setLoadScreen(nullptr);
	for(Int i = 0; i < MAX_SLOTS; ++i)
	{
		m_progressBars[i] = nullptr;
		m_playerNames[i] = nullptr;
		m_playerSide[i]= nullptr;
	}
}

void GameSpyLoadScreen::update( Int percent )
{
	if(percent <= 100)
		TheNetwork->updateLoadProgress( percent );
	TheNetwork->liteupdate();

	//GadgetProgressBarSetProgress(m_progressBars[TheNetwork->getLocalPlayerID()], percent );

	TheMouse->setCursorTooltip(UnicodeString::TheEmptyString);

	// Do this last!
	LoadScreen::update( percent );
}

void GameSpyLoadScreen::processProgress(Int playerId, Int percentage)
{

	if( percentage < 0 || percentage > 100 || playerId >= MAX_SLOTS || playerId < 0 || m_playerLookup[playerId] == -1)
	{
		DEBUG_CRASH(("Percentage %d was passed in for Player %d", percentage, playerId));
		return;
	}
	//DEBUG_LOG(("Percentage %d was passed in for Player %d (in loadscreen position %d)", percentage, playerId, m_playerLookup[playerId]));
	if(m_progressBars[m_playerLookup[playerId]])
		GadgetProgressBarSetProgress(m_progressBars[m_playerLookup[playerId]], percentage );
}

// MapTransferLoadScreen Class //////////////////////////////////////////////////
//-----------------------------------------------------------------------------
MapTransferLoadScreen::MapTransferLoadScreen()
{
	m_oldTimeout = 0;
	for(Int i = 0; i < MAX_SLOTS; ++i)
	{
		m_progressBars[i] = nullptr;
		m_playerNames[i] = nullptr;
		m_progressText[i]= nullptr;
		m_playerLookup[i] = -1;
		m_oldProgress[i] = -1;
	}
	m_fileNameText = nullptr;
	m_timeoutText = nullptr;
}

MapTransferLoadScreen::~MapTransferLoadScreen()
{
}

void MapTransferLoadScreen::init( GameInfo *game )
{
	// create the layout of the load screen
	m_loadScreen = TheWindowManager->winCreateFromScript( "Menus/MapTransferScreen.wnd" );
	DEBUG_ASSERTCRASH(m_loadScreen, ("Can't initialize the map transfer loadscreen"));
	if (!m_loadScreen)
		return;

	m_loadScreen->winHide(FALSE);
	m_loadScreen->winBringToTop();

	DEBUG_ASSERTCRASH(TheNetwork, ("Where the Heck is the Network?!!!!"));
	DEBUG_LOG(("NumPlayers %d", TheNetwork->getNumPlayers()));

	AsciiString winName;
	Int i;

	// Load the Filename Text
	m_fileNameText = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( "MapTransferScreen.wnd:StaticTextCurrentFile" ));
	DEBUG_ASSERTCRASH(m_fileNameText, ("Can't initialize the filename for the map transfer loadscreen"));

	// Load the Timeout Text
	m_timeoutText = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( "MapTransferScreen.wnd:StaticTextTimeout" ));
	DEBUG_ASSERTCRASH(m_timeoutText, ("Can't initialize the timeout for the map transfer loadscreen"));

	Int netSlot = 0;
	// Loop through and make the loadscreen look all good.
	for (i = 0; i < MAX_SLOTS; ++i)
	{
		// Load the Progress Bar
		winName.format( "MapTransferScreen.wnd:ProgressLoad%d",i);
		m_progressBars[i] = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( winName ));
		DEBUG_ASSERTCRASH(m_progressBars[i], ("Can't initialize the progressbars for the map transfer loadscreen"));
		// set the progressbar to zero
		GadgetProgressBarSetProgress(m_progressBars[i], 0 );

		// Load the Player's name
		winName.format( "MapTransferScreen.wnd:StaticTextPlayer%d",i);
		m_playerNames[i] = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( winName ));
		DEBUG_ASSERTCRASH(m_playerNames[i], ("Can't initialize the Names for the map transfer loadscreen"));

		// Load the Progress Text
		winName.format( "MapTransferScreen.wnd:StaticTextProgress%d",i);
		m_progressText[i] = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( winName ));
		DEBUG_ASSERTCRASH(m_progressText[i], ("Can't initialize the progress text for the map transfer loadscreen"));

		// get the slot man!
		GameSlot *slot = game->getSlot(i);
		if (!slot || !slot->isHuman())
			continue;
		Color houseColor = TheMultiplayerSettings->getColor(slot->getApparentColor())->getColor();
		GadgetProgressBarSetEnabledBarColor(m_progressBars[netSlot], houseColor );

		UnicodeString name = slot->getName();
		GadgetStaticTextSetText(m_playerNames[netSlot], name );
		m_playerNames[netSlot]->winSetEnabledTextColors(houseColor, m_playerNames[netSlot]->winGetEnabledTextBorderColor());

		GadgetStaticTextSetText(m_progressText[netSlot], UnicodeString::TheEmptyString );
		m_progressText[netSlot]->winSetEnabledTextColors(houseColor, m_progressText[netSlot]->winGetEnabledTextBorderColor());

		if ((i == 0 || (TheGameInfo->getConstSlot(i)->isHuman() && TheGameInfo->getConstSlot(i)->hasMap())) && m_progressBars[netSlot])
			m_progressBars[netSlot]->winHide(TRUE);

		m_playerLookup[i] = netSlot; // save our mapping so we can update progress correctly

		netSlot++;
	}

	for(i = netSlot; i < MAX_SLOTS; ++i)
	{
		m_progressBars[i]->winHide(TRUE);
		m_playerNames[i]->winHide(TRUE);
		m_progressText[i]->winHide(TRUE);
	}
}

void MapTransferLoadScreen::reset()
{
	setLoadScreen(nullptr);
	for(Int i = 0; i < MAX_SLOTS; ++i)
	{
		m_progressBars[i] = nullptr;
		m_playerNames[i] = nullptr;
		m_progressText[i]= nullptr;
		m_playerLookup[i] = -1;
		m_oldProgress[i] = -1;
	}
	m_fileNameText = nullptr;
	m_timeoutText = nullptr;
}

void MapTransferLoadScreen::update( Int percent )
{
	if (TheNetwork)
	{
		TheNetwork->liteupdate();
	}

	TheMouse->setCursorTooltip(UnicodeString::TheEmptyString);

	// Do this last!
	LoadScreen::update( percent );
}

void MapTransferLoadScreen::processProgress(Int playerId, Int percentage, AsciiString stateStr)
{

	if( percentage < 0 || percentage > 100 || playerId >= MAX_SLOTS || playerId < 0 || m_playerLookup[playerId] == -1)
	{
		DEBUG_CRASH(("Percentage %d was passed in for Player %d", percentage, playerId));
		return;
	}

	if (m_oldProgress[playerId] == percentage)
		return;
	m_oldProgress[playerId] = percentage;

	Int translatedSlot = m_playerLookup[playerId];
	if(m_progressBars[translatedSlot])
		GadgetProgressBarSetProgress(m_progressBars[translatedSlot], percentage );
	if (m_progressText[translatedSlot])
		GadgetStaticTextSetText(m_progressText[translatedSlot], TheGameText->fetch(stateStr));
}

void MapTransferLoadScreen::processTimeout(Int secondsLeft)
{
	if (m_oldTimeout == secondsLeft)
		return;
	m_oldTimeout = secondsLeft;

	if (m_timeoutText)
	{
		UnicodeString txt;
		txt.format(TheGameText->fetch("MapTransfer:Timeout"), (secondsLeft/60), (secondsLeft%60));
		GadgetStaticTextSetText(m_timeoutText, txt);
	}
}

void MapTransferLoadScreen::setCurrentFilename(AsciiString filename)
{
	if (m_fileNameText)
	{
		UnicodeString txt;
		txt.translate(TheGameState->getMapLeafName(filename));
		txt.format(TheGameText->fetch("MapTransfer:CurrentFile"), txt.str());
		GadgetStaticTextSetText(m_fileNameText, txt);
	}
}

