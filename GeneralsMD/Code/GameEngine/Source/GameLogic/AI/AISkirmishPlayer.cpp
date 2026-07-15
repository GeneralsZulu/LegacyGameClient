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

// AISkirmishPlayer.cpp
// Computerized opponent
// Author: Michael S. Booth, January 2002

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine


#include "Common/ActionManager.h"
#include "Common/GameMemory.h"
#include "Common/GlobalData.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/PlayerTemplate.h"
#include "Common/Team.h"
#include "Common/ThingFactory.h"
#include "Common/BuildAssistant.h"
#include "Common/SpecialPower.h"
#include "Common/ThingTemplate.h"
#include "Common/Upgrade.h"
#include "Common/WellKnownKeys.h"
#include "Common/Xfer.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Object.h"
#include "GameLogic/AISkirmishPlayer.h"
#include "GameLogic/SidesList.h"
#include "Common/Recorder.h"
#include "GameLogic/AI.h"
#include "GameLogic/AIPathfind.h"
#include "GameLogic/TerrainLogic.h"
#include "GameLogic/Weapon.h"
#include "GameLogic/Module/AIUpdate.h"
#include "GameLogic/Module/DozerAIUpdate.h"
#include "GameLogic/Module/RebuildHoleBehavior.h"
#include "GameLogic/Module/UpdateModule.h"
#include "GameLogic/Locomotor.h"
#include "GameLogic/PartitionManager.h"
#include "GameLogic/ScriptEngine.h"
#include "GameLogic/Module/ProductionUpdate.h"
#include "GameLogic/Module/SpecialAbilityUpdate.h"
#include "GameClient/ControlBar.h"
#include "GameClient/TerrainVisual.h"
#include "GameClient/Drawable.h"
#include "GameClient/InGameUI.h"


#define USE_DOZER 1



///////////////////////////////////////////////////////////////////////////////////////////////////
// PRIVATE DATA ///////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////

AISkirmishPlayer::AISkirmishPlayer( Player *p ) :	AIPlayer(p),
m_curFlankBaseDefense(0),
m_curFrontBaseDefense(0),
m_curFrontLeftDefenseAngle(0),
m_curFrontRightDefenseAngle(0),
m_curLeftFlankLeftDefenseAngle(0),
m_curLeftFlankRightDefenseAngle(0),
m_curRightFlankLeftDefenseAngle(0),
m_curRightFlankRightDefenseAngle(0),
m_frameToCheckEnemy(0),
m_currentEnemy(nullptr),
m_nextIdleSweepFrame(0),
m_nextUpgradeSweepFrame(0),
m_nextLaserLockSweepFrame(0),
m_directiveBeaconID(INVALID_ID),
m_nextDirectiveScanFrame(0),
m_nextDirectiveAnnounceFrame(0),
m_nextSurrenderCheckFrame(0),
m_hasSurrendered(false),
m_surrenderConsentAnnounced(false),
m_nextMilestoneCheckFrame(0),
m_milestoneAnnouncedMask(0),
m_synAnnounced(false),
m_nextAttackAnnounceFrame(0),
m_nextDistressSampleFrame(0),
m_nextDistressAnnounceFrame(0),
m_distressRingHead(0),
m_distressRingFilled(0)

{
	Int i;
	for (i = 0; i < 5; ++i) { m_distressHPRing[i] = 0.0f; m_distressCountRing[i] = 0; }
	m_frameLastBuildingBuilt = TheGameLogic->getFrame();
	p->setCanBuildUnits(true); // turn on ai production by default.
}

AISkirmishPlayer::~AISkirmishPlayer()
{
	clearTeamsInQueue();
}


/**
 * Build our base.
 */
void AISkirmishPlayer::processBaseBuilding()
{
	//
	// Refresh base buildings. Scan through list, if a building is missing,
	// rebuild it, unless it's rebuild count is zero.
	//
	if (m_readyToBuildStructure)
	{
		const ThingTemplate *bldgPlan=nullptr;
		BuildListInfo	*bldgInfo = nullptr;
		Bool isPriority = false;
		Object *bldg = nullptr;
		const ThingTemplate *powerPlan=nullptr;
		BuildListInfo	*powerInfo = nullptr;
		Bool isUnderPowered = !m_player->getEnergy()->hasSufficientPower();
		Bool powerUnderConstruction = false;

		// Tactical AI USA: scan ahead of the main loop for owned power plants.
		// Two flags fall out of this:
		//   1. taiUsaWantsFirstPower — true when no power plant is yet owned
		//      (built or under construction). Used below to elevate the power
		//      plant as the first base building, regardless of whether the
		//      player currently reads as underpowered.
		//   2. taiUsaPowerUnderConstruction — true when a power plant is
		//      mid-build. Used in the main loop to skip later power plant
		//      entries entirely so the AI doesn't dispatch a second dozer to
		//      a parallel reactor (the existing post-loop powerUnderConstruction
		//      guard only blocks the override, not the bldgPlan-via-iteration
		//      path that fires when a later PP entry runs while bldgPlan is
		//      still null).
		Bool taiUsaWantsFirstPower = false;
		Bool taiUsaPowerUnderConstruction = false;
		if (isTacticalAI() && m_player->getBaseSide().compareNoCase("USA") == 0) {
			Bool ownsAnyPower = false;
			for (BuildListInfo *probe = m_player->getBuildList(); probe; probe = probe->getNext()) {
				Object *existing = TheGameLogic->findObjectByID(probe->getObjectID());
				if (!existing) continue;
				if (existing->getControllingPlayer() != m_player) continue;
				if (!existing->isKindOf(KINDOF_FS_POWER)) continue;
				if (existing->isKindOf(KINDOF_CASH_GENERATOR)) continue;
				ownsAnyPower = true;
				if (existing->getStatusBits().test(OBJECT_STATUS_UNDER_CONSTRUCTION)) {
					taiUsaPowerUnderConstruction = true;
				}
			}
			taiUsaWantsFirstPower = !ownsAnyPower;
		}

		for( BuildListInfo *info = m_player->getBuildList(); info; info = info->getNext() )
		{
			AsciiString name = info->getTemplateName();
			if (name.isEmpty()) continue;
			const ThingTemplate *curPlan = TheThingFactory->findTemplate( name );
			if (!curPlan) {
				DEBUG_LOG(("*** ERROR - Build list building '%s' doesn't exist.", name.str()));
				continue;
			}
			bldg = TheGameLogic->findObjectByID( info->getObjectID() );
			// check for hole.
			if (info->getObjectID() != INVALID_ID) {
				// used to have a building.
				Object *bldg = TheGameLogic->findObjectByID( info->getObjectID() );
				if (bldg==nullptr) {
					// got destroyed.
					ObjectID priorID;
					priorID = info->getObjectID();
					info->setObjectID(INVALID_ID);
					info->setObjectTimestamp(TheGameLogic->getFrame()+1);
					// Scan for a GLA hole.	KINDOF_REBUILD_HOLE
					Object *obj;
					for( obj = TheGameLogic->getFirstObject(); obj; obj = obj->getNextObject() ) {
						if (!obj->isKindOf(KINDOF_REBUILD_HOLE)) continue;
						RebuildHoleBehaviorInterface *rhbi = RebuildHoleBehavior::getRebuildHoleBehaviorInterfaceFromObject( obj );
						if( rhbi ) {
							ObjectID spawnerID = rhbi->getSpawnerID();
							if (priorID == spawnerID) {
								DEBUG_LOG(("AI Found hole to rebuild %s", curPlan->getName().str()));
								info->setObjectID(obj->getID());
							}
						}
 					}
				}	else {
					if (bldg->getControllingPlayer() == m_player) {
						// Check for built or dozer missing.
						if( bldg->getStatusBits().test( OBJECT_STATUS_UNDER_CONSTRUCTION ) )
						{
							if (bldg->isKindOf(KINDOF_FS_POWER) && !bldg->isKindOf(KINDOF_CASH_GENERATOR))
							{
								powerUnderConstruction = true;
							}
							// make sure dozer is working on him.
							ObjectID builder = bldg->getBuilderID();
							Object* myDozer = TheGameLogic->findObjectByID(builder);

              if (myDozer && ( myDozer->getControllingPlayer() != m_player || myDozer->isDisabledByType( DISABLED_UNMANNED ) ) )
              {//I don't expect this dozer to work well with me.
                myDozer = nullptr;
                bldg->setBuilder( nullptr );
              }

							if (myDozer==nullptr) {
								DEBUG_LOG(("AI's Dozer got killed (or captured).  Find another dozer."));
								queueDozer();
 								myDozer = findDozer(bldg->getPosition());
								if (myDozer==nullptr || myDozer->getAI()==nullptr) {
									continue;
								}
								myDozer->getAI()->aiResumeConstruction(bldg, CMD_FROM_AI);
							}	else {
								// make sure he is building.
								myDozer->getAI()->aiResumeConstruction(bldg, CMD_FROM_AI);
							}
						}
					} else {
						// oops, got captured.
						info->setObjectID(INVALID_ID);
						info->setObjectTimestamp(TheGameLogic->getFrame()+1);
					}
				}
			}
			if (info->getObjectID()==INVALID_ID && info->getObjectTimestamp()>0) {
				// this object was built at some time, and got destroyed at or near objectTimestamp.
				// Wait a few seconds before initiating a rebuild.
				if (info->getObjectTimestamp()+TheAI->getAiData()->m_rebuildDelaySeconds*LOGICFRAMES_PER_SECOND > TheGameLogic->getFrame()) {
					continue;
				}	else {
					DEBUG_LOG(("Enabling rebuild for %s", info->getTemplateName().str()));
					info->setObjectTimestamp(0); // ready to build.
				}
			}
			if (bldg) {
				continue; // already built.
			}
			// Make sure it is safe to build here.
			if (!isLocationSafe(info->getLocation(), curPlan)) {
				continue;
			}
			if (info->isPriorityBuild()) {
				// Always take priority build, unless we already have priority build.
				if (!isPriority) {
					bldgPlan = curPlan;
					bldgInfo = info;
					isPriority = true;
				}
			}
			if (curPlan->isKindOf(KINDOF_FS_POWER)) {
				if (powerPlan==nullptr && !curPlan->isKindOf(KINDOF_CASH_GENERATOR)) {
					if (isUnderPowered || info->isAutomaticBuild() || taiUsaWantsFirstPower) {
						powerPlan = curPlan;
						powerInfo = info;
					}
				}
				// TAI USA: don't let a second power plant entry slip into bldgPlan
				// via the bldgPlan==nullptr fallthrough below while one is already
				// under construction; the post-loop force gate only protects the
				// override path, not the line-257 first-buildable path.
				if (taiUsaPowerUnderConstruction && !curPlan->isKindOf(KINDOF_CASH_GENERATOR)) {
					continue;
				}
			}
			if (!info->isAutomaticBuild()) {
				continue; // marked to not build automatically.
			}
			Object *dozer = findDozer(info->getLocation());
			if (dozer==nullptr) {
				if (isUnderPowered) {
					queueDozer();
				}
				continue;
			}
			if (TheBuildAssistant->canMakeUnit(dozer, bldgPlan)!=CANMAKE_OK) {
				if (info->isBuildable()) {
					AsciiString bldgName = info->getTemplateName();
					bldgName.concat(" - Dozer unable to build - money or technology missing.");
					TheScriptEngine->AppendDebugMessage(bldgName, false);
				}
				continue;
			}
			// check if this building has any "rebuilds" left
			if (info->isBuildable())
			{
				if (bldgPlan == nullptr) {
					bldgPlan = curPlan;
					bldgInfo = info;
				}
			}
		}
		if (powerInfo && powerPlan && !powerPlan->isEquivalentTo(bldgPlan)) {
			if (!powerUnderConstruction) {
				bldgPlan = powerPlan;
				bldgInfo = powerInfo;
				DEBUG_LOG(("Forcing build of power plant."));
			}
		}
		if (bldgPlan && bldgInfo) {
#ifdef USE_DOZER
			// dozer-construct the building
			bldg = buildStructureWithDozer(bldgPlan, bldgInfo);
			// store the object with the build order
			if (bldg)
			{
				bldgInfo->setObjectID( bldg->getID() );
				bldgInfo->decrementNumRebuilds();

				m_readyToBuildStructure = false;
				m_structureTimer = TheAI->getAiData()->m_structureSeconds*LOGICFRAMES_PER_SECOND;
				if (m_player->getMoney()->countMoney() < TheAI->getAiData()->m_resourcesPoor) {
					m_structureTimer = m_structureTimer/TheAI->getAiData()->m_structuresPoorMod;
				}	else if (m_player->getMoney()->countMoney() > TheAI->getAiData()->m_resourcesWealthy) {
					m_structureTimer = m_structureTimer/TheAI->getAiData()->m_structuresWealthyMod;
				}
				m_frameLastBuildingBuilt = TheGameLogic->getFrame();
				// only build one building per delay loop
			}

#else
			// force delay between rebuilds
			Int framesToBuild = bldgPlan->calcTimeToBuild(m_player);
			if (TheGameLogic->getFrame() - m_frameLastBuildingBuilt < framesToBuild)
			{
				m_buildDelay = framesToBuild - (TheGameLogic->getFrame() - m_frameLastBuildingBuilt);
				return;
			}	else {
				// building is missing, (re)build it
				// deduct money to build, if we have it
				Int cost = bldgPlan->calcCostToBuild( m_player );
				if (m_player->getMoney()->countMoney() >= cost)
				{
					// we have the money, deduct it
					m_player->getMoney()->withdraw( cost );

					// inst-construct the building
					bldg = buildStructureNow(bldgPlan, bldgInfo);
					// store the object with the build order
					if (bldg)
					{
						bldgInfo->setObjectID( bldg->getID() );
						bldgInfo->decrementNumRebuilds();

						m_readyToBuildStructure = false;
						m_structureTimer = TheAI->getAiData()->m_structureSeconds*LOGICFRAMES_PER_SECOND;
						if (m_player->getMoney()->countMoney() < TheAI->getAiData()->m_resourcesPoor) {
							m_structureTimer = m_structureTimer/TheAI->getAiData()->m_structuresPoorMod;
						}	else if (m_player->getMoney()->countMoney() > TheAI->getAiData()->m_resourcesWealthy) {
							m_structureTimer = m_structureTimer/TheAI->getAiData()->m_structuresWealthyMod;
						}
						m_frameLastBuildingBuilt = TheGameLogic->getFrame();
					}
				}
			}
#endif
		}
	}
}

/**
 * Invoked when a unit I am training comes into existence
 */
void AISkirmishPlayer::onUnitProduced( Object *factory, Object *unit )
{
	AIPlayer::onUnitProduced(factory, unit);
}

/**
 * Search the computer player's buildings for one that can build the given request
 * and start training the unit.
 * If busyOK is true, it will queue a unit even if one is building.  This lets
 * script invoked teams "push" to the front of the queue.
 */
Bool AISkirmishPlayer::startTraining( WorkOrder *order, Bool busyOK, AsciiString teamName)
{
	Object *factory = findFactory(order->m_thing, busyOK);
	if( factory )
	{
		ProductionUpdateInterface *pu = factory->getProductionUpdateInterface();
		if (pu && pu->queueCreateUnit( order->m_thing, pu->requestUniqueUnitID() )) {
			order->m_factoryID = factory->getID();
			if (TheGlobalData->m_debugAI) {
				AsciiString teamStr = "Queuing ";
				teamStr.concat(order->m_thing->getName());
				teamStr.concat(" for ");
				teamStr.concat(teamName);
				TheScriptEngine->AppendDebugMessage(teamStr, false);
			}
			return true;
		}
	}

	return FALSE;

}


/**
 * Check if this team is buildable, doesn't exceed maximum limits, meets conditions, and isn't under construction.
 */
Bool AISkirmishPlayer::isAGoodIdeaToBuildTeam( TeamPrototype *proto )
{
	// Check condition.
	if (!proto->evaluateProductionCondition()) {
		return false;
	}
	// check build limit. Use the live-instance count so depleted teams whose
	// only survivor is a lone straggler don't lock the prototype out of being
	// rebuilt. Teams still under construction always count as occupying a slot.
	if (proto->countTeamInstancesAlive() >= proto->getTemplateInfo()->m_maxInstances){
		if (TheGlobalData->m_debugAI) {
			AsciiString str;
			str.format("Team %s not chosen - %d already exist.", proto->getName().str(), proto->countTeamInstancesAlive());
			TheScriptEngine->AppendDebugMessage(str, false);
		}
		return false;	// Max already built.
	}

	for ( DLINK_ITERATOR<TeamInQueue> iter = iterate_TeamBuildQueue(); !iter.done(); iter.advance())
	{
		TeamInQueue *team = iter.cur();
		if (team->m_team->getPrototype() == proto) {
			return false; // currently building one of these.
		}
	}
	Bool needMoney;
	if (!isPossibleToBuildTeam( proto, true, needMoney)) {
		if (TheGlobalData->m_debugAI) {
			AsciiString str;
			if (needMoney) {
				str.format("Team %s not chosen - Not enough money.", proto->getName().str());
			} else {
				str.format("Team %s not chosen - Factory/tech missing or busy.", proto->getName().str());
			}
			TheScriptEngine->AppendDebugMessage(str, false);
		}
		return false;
	}
	return true;
}

/**
 * See if any existing teams need reinforcements, and have higher priority.
 */
Bool AISkirmishPlayer::selectTeamToReinforce( Int minPriority )
{
	return AIPlayer::selectTeamToReinforce(minPriority);
}

/**
 * Determine the next team to build.  Return true if one was selected.
 */
Bool AISkirmishPlayer::selectTeamToBuild()
{
	return AIPlayer::selectTeamToBuild();
}

/**
	Build a specific building.
	*/
void AISkirmishPlayer::buildSpecificAIBuilding(const AsciiString &thingName)
{
	//
	Bool found = false;
	Bool foundUnbuilt = false;
	for( BuildListInfo *info = m_player->getBuildList(); info; info = info->getNext() )
	{
		if (info->getTemplateName()==thingName)
		{
			AsciiString name = info->getTemplateName();
			if (name.isEmpty()) continue;
			const ThingTemplate *bldgPlan = TheThingFactory->findTemplate( name );
			if (!bldgPlan) {
				DEBUG_LOG(("*** ERROR - Build list building '%s' doesn't exist.", name.str()));
				continue;
			}
			Object *bldg = TheGameLogic->findObjectByID( info->getObjectID() );
			found = true;
			if (bldg) {
				continue; // already built.
			}
			if (info->isPriorityBuild()) {
				continue; // already marked for priority build.
			}
			foundUnbuilt = true;
			info->markPriorityBuild();
			break;
		}
	}
	if (foundUnbuilt) {
		m_buildDelay = 0;
		AsciiString buildingStr = "Queueing building '";
		buildingStr.concat(thingName);
		buildingStr.concat("' for construction.");
		TheScriptEngine->AppendDebugMessage(buildingStr, false);
	}	else if (found) {
		AsciiString buildingStr = "Warning - all instances of building '";
		buildingStr.concat(thingName);
		buildingStr.concat("' are already built or queued for build, not queueing.");
		TheScriptEngine->AppendDebugMessage(buildingStr, false);
	}	else {
		AsciiString buildingStr = "Error - could not find building '";
		buildingStr.concat(thingName);
		buildingStr.concat("' in the building template list.");
		TheScriptEngine->AppendDebugMessage(buildingStr, false);
	}
}



/**
	Gets the player index of my enemy.
	*/
Int AISkirmishPlayer::getMyEnemyPlayerIndex() {
	Int playerNdx;
	if (m_currentEnemy) {
		return m_currentEnemy->getPlayerIndex();
	}
	// For now, return first human player, as there should only be one. jba
	for (playerNdx=0; playerNdx<ThePlayerList->getPlayerCount(); playerNdx++) {
		if (ThePlayerList->getNthPlayer(playerNdx)->getPlayerType() == PLAYER_HUMAN) {
			break;
		}
	}
	return playerNdx;
}

/**
	Get the AI's enemy.  Recalc if it has been a while (5 seconds.)
*/
void AISkirmishPlayer::acquireEnemy()
{
	Player *bestEnemy = nullptr;
	Real bestDistanceSqr = HUGE_DIST*HUGE_DIST;

	if (m_currentEnemy) {
		Bool inBadShape = !m_currentEnemy->hasAnyUnits() || !m_currentEnemy->hasAnyBuildFacility();
		if (!inBadShape) return;
	}

	// look for the closest enemy.
	Int i;
	for (i=0; i<ThePlayerList->getPlayerCount(); i++) {
		Player *curPlayer = ThePlayerList->getNthPlayer(i);
		if (m_player->getRelationship(curPlayer->getDefaultTeam()) == ENEMIES) {
			if (curPlayer->hasAnyObjects()==false) continue; // not much of an enemy.
			// ok, we got an enemy;
			// If a player is out of units, or out of build facilities, we can lower his priority.
			Bool inBadShape = !curPlayer->hasAnyUnits() || !curPlayer->hasAnyBuildFacility();

			Coord3D enemyPos = m_baseCenter;
			Region2D bounds;
			Coord2D meanPos;
			getPlayerStructureBounds(&bounds, i, FALSE, &meanPos);
			enemyPos.x = meanPos.x;
			enemyPos.y = meanPos.y;
			Real curDistSqr = sqr(enemyPos.x-m_baseCenter.x) + sqr(enemyPos.y-m_baseCenter.y);

			//Fudge for in bad shape.	 If an enemy is crippled, concentrate on the other ones.
			if (inBadShape) {
				curDistSqr = HUGE_DIST*HUGE_DIST*0.5f;
			}
			// See if other ai's are attacking this target.
			// We don't want the ai's to gang up on one enemy.
			Int k;
			for (k=0; k<ThePlayerList->getPlayerCount(); k++) {
				if (k==i) continue;  // don't count self.
				Player *somePlayer = ThePlayerList->getNthPlayer(k);
				if (somePlayer->isSkirmishAIPlayer() && (somePlayer->getCurrentEnemy()==curPlayer)) {
					// Some ai is already targeting this guy.  Add a distance penalty.
					curDistSqr += (500*500);
				}
				if (somePlayer->isSkirmishAIPlayer() && (somePlayer->getCurrentEnemy()==m_player)) {
					// he is attacking me.  So I will (gently) prefer to attack him.
					curDistSqr -= (25*25);
					if (curDistSqr<0) curDistSqr = 0;
				}
			}

			// Ai enemy - will take if we don't get a better offer.
			if (curDistSqr<bestDistanceSqr) {
				bestEnemy = curPlayer;
				bestDistanceSqr = curDistSqr;
			}
		}
	}
	if (bestEnemy!=nullptr && (bestEnemy!=m_currentEnemy)) {
		m_currentEnemy = bestEnemy;
		AsciiString msg = TheNameKeyGenerator->keyToName(m_player->getPlayerNameKey());
		msg.concat(" acquiring target enemy player: ");
		msg.concat(TheNameKeyGenerator->keyToName(m_currentEnemy->getPlayerNameKey()));
		TheScriptEngine->AppendDebugMessage( msg, false);
	}
}



/**
	Get the AI's enemy.  Recalc if it has been a while (20 seconds.)
*/
Player *AISkirmishPlayer::getAiEnemy()
{
	if (TheGameLogic->getFrame()>=m_frameToCheckEnemy) {
		m_frameToCheckEnemy = TheGameLogic->getFrame() + 5*LOGICFRAMES_PER_SECOND;
		acquireEnemy();
	}
	return m_currentEnemy;
}

/**
	Build base defense structures on the front or flank of the base.
*/
void AISkirmishPlayer::buildAIBaseDefense(Bool flank)
{
	const AISideInfo *resInfo = TheAI->getAiData()->m_sideInfo;
	AsciiString defenseTemplateName;
	while (resInfo) {
		if (resInfo->m_side == m_player->getSide()) {
			defenseTemplateName = resInfo->m_baseDefenseStructure1;
			break;
		}
		resInfo = resInfo->m_next;
	}
	if (resInfo) {
		buildAIBaseDefenseStructure(resInfo->m_baseDefenseStructure1, flank);
	}
}

/**
	Build base defense structures on the front or flank of the base.
	Base defenses are placed as follows:
	m_baseCenter and m_baseRadius are calculated on map load.
	Defenses are placed along the this circle.
	Front defenses (!flank) are placed starting at the "Center" approach path.
	The first front defense is placed towards th Center path.  Number 2 is placed
	to the left of #1, #3 is placed to the right of #1, #4 is placed to the left of
	#2 and so on.  So it looks like:

												#1
									 #2 			#3
					#6  #4								 #5	  #7
		  #8																	#9

	The flank base defenses cover the "Flank" approach, and the "Backdoor" approach.
	They alternate between these two, so the first flank defense covers flank, and the second
	covers backdoor, and continue to alternate.  They cover the approach using the same
	pattern as front above.
	John A.

	*/
void AISkirmishPlayer::buildAIBaseDefenseStructure(const AsciiString &thingName, Bool flank)
{
	const ThingTemplate *tTemplate = TheThingFactory->findTemplate(thingName);
	if (tTemplate==nullptr) {
		DEBUG_CRASH(("Couldn't find base defense structure '%s' for side %s", thingName.str(), m_player->getSide().str()));
		return;
	}
	do {
		AsciiString pathLabel;
		if (flank) {
			if (m_curFlankBaseDefense&1) {
				pathLabel.format("%s%d", SKIRMISH_FLANK, m_player->getMpStartIndex()+1);
			}	else {
				pathLabel.format("%s%d", SKIRMISH_BACKDOOR, m_player->getMpStartIndex()+1);
			}
		}	else {
			pathLabel.format("%s%d", SKIRMISH_CENTER, m_player->getMpStartIndex()+1);
		}

		Coord3D goalPos = m_baseCenter;
		Waypoint *way = TheTerrainLogic->getClosestWaypointOnPath( &goalPos, pathLabel );
		if (way) {
			goalPos = *way->getLocation();
		} else {
			if (flank) return;
			Region2D bounds;
			getPlayerStructureBounds(&bounds, getMyEnemyPlayerIndex());
			goalPos.x = bounds.lo.x + bounds.width()/2;
			goalPos.y = bounds.lo.y + bounds.height()/2;
		}
		Coord2D offset;
		offset.x = goalPos.x-m_baseCenter.x;
		offset.y = goalPos.y-m_baseCenter.y;
		offset.normalize();
		Real defenseDistance = m_baseRadius;
		defenseDistance += TheAI->getAiData()->m_skirmishBaseDefenseExtraDistance;
		offset.x *= defenseDistance;
		offset.y *= defenseDistance;

		Real structureRadius = tTemplate->getTemplateGeometryInfo().getBoundingCircleRadius();
		Real baseCircumference = 2*PI*defenseDistance;
		Real angleOffset = 2*PI*(structureRadius*4/baseCircumference);

		Int selector;
		Real angle;
		if (flank) {
			selector = m_curFlankBaseDefense>>1;
			if (m_curFlankBaseDefense&1) {
				if (selector&1) {
					m_curLeftFlankRightDefenseAngle -= angleOffset;
					angle = m_curLeftFlankRightDefenseAngle;
				}	else {
					angle = m_curLeftFlankLeftDefenseAngle;
					m_curLeftFlankLeftDefenseAngle += angleOffset;
				}
			}	else {
				if (selector&1) {
					m_curRightFlankRightDefenseAngle -= angleOffset;
					angle = m_curRightFlankRightDefenseAngle;
				}	else {
					angle = m_curRightFlankLeftDefenseAngle;
					m_curRightFlankLeftDefenseAngle += angleOffset;
				}
			}

		} else {
			selector = m_curFrontBaseDefense;
			if (selector&1) {
				m_curFrontRightDefenseAngle -= angleOffset;
				angle = m_curFrontRightDefenseAngle;
			}	else {
				angle = m_curFrontLeftDefenseAngle;
				m_curFrontLeftDefenseAngle += angleOffset;
			}
		}

		if (angle > PI/3) break;
		Real s = sin(angle);
		Real c = cos(angle);

// TheSuperHackers @info helmutbuhler 21/04/2025 This debug mutates the code to become CRC incompatible
#if defined(RTS_DEBUG) || !RETAIL_COMPATIBLE_CRC
		DEBUG_LOG(("buildAIBaseDefenseStructure -- Angle is %f sin %f, cos %f", 180*angle/PI, s, c));
		DEBUG_LOG(("buildAIBaseDefenseStructure -- Offset is %f  %f, Final Position is %f, %f",
			offset.x, offset.y,
			offset.x*c - offset.y*s,
			offset.y*c + offset.x*s
			));
#endif
		Coord3D buildPos = m_baseCenter;
		buildPos.x += offset.x*c - offset.y*s;
		buildPos.y += offset.y*c + offset.x*s;

		/* See if we can build there. */
		Bool canBuild;
		Real placeAngle = tTemplate->getPlacementViewAngle();
		canBuild = LBC_OK == TheBuildAssistant->isLocationLegalToBuild(&buildPos, tTemplate, placeAngle,
			BuildAssistant::TERRAIN_RESTRICTIONS|BuildAssistant::NO_OBJECT_OVERLAP, nullptr, m_player);
		TheTerrainVisual->removeAllBibs();	// isLocationLegalToBuild adds bib feedback, turn it off.  jba.
		if (flank) {
			m_curFlankBaseDefense++;
		} else {
			m_curFrontBaseDefense++;
		}
		if (canBuild) {
			m_player->addToPriorityBuildList(thingName, &buildPos, placeAngle);
			break;
		}
	}	while (true);

}


/**
	Checks bridges along a waypoint path.  If any are destroyed, sends a dozer to fix, and returns true.
	If there is no bridge problem, returns false.
	*/
Bool AISkirmishPlayer::checkBridges(Object *unit, Waypoint *way)
{
	Coord3D unitPos = *unit->getPosition();
	AIUpdateInterface *ai = unit->getAI();
	if (!ai) return false; // no ai
	const LocomotorSet& locoSet = ai->getLocomotorSet();
	Waypoint *curWay;
	for (curWay = way; curWay; curWay = curWay->getNext()) {
		if (TheAI->pathfinder()->clientSafeQuickDoesPathExist(locoSet, &unitPos, curWay->getLocation())) {
			continue;
		}
		ObjectID brokenBridge = INVALID_ID;
		if (TheAI->pathfinder()->findBrokenBridge(locoSet, &unitPos, curWay->getLocation(), &brokenBridge)) {
			repairStructure(brokenBridge);
			return true;
		}
	}
	return false;

}


/**
	Build a specific team.  If priorityBuild, put at front of queue with priority set.
	*/
void AISkirmishPlayer::buildSpecificAITeam( TeamPrototype *teamProto, Bool priorityBuild)
{
	AIPlayer::buildSpecificAITeam(teamProto, priorityBuild);
}


/**
	Recruit a specific team, within the specific radius of the home position.
	*/
void AISkirmishPlayer::recruitSpecificAITeam(TeamPrototype *teamProto, Real recruitRadius)
{
	if (recruitRadius < 1) recruitRadius = 99999.0f;
	//
	// Create "Team in queue" based on team population
	//
	if (teamProto)
	{
		if (teamProto->getIsSingleton()) {
			Team *singletonTeam = TheTeamFactory->findTeam( teamProto->getName() );
			if (singletonTeam && singletonTeam->hasAnyObjects()) {
				AsciiString teamStr = "Unable to recruit singleton team '";
				teamStr.concat("' because team already exists.");
				TheScriptEngine->AppendDebugMessage(teamStr, false);
				return;
			}
		}
		if (!teamProto->getTemplateInfo()->m_hasHomeLocation)
		{
			AsciiString teamStr = "Error : team '";
			teamStr.concat(teamProto->getName());
			teamStr.concat("' has no Home Position (or Origin).");
			TheScriptEngine->AppendDebugMessage(teamStr, false);
		}
		// create inactive team to place members into as they are built
		// when team is complete, the team is activated
		Team *theTeam = TheTeamFactory->createInactiveTeam( teamProto->getName() );
		AsciiString teamName = teamProto->getName();
		teamName.concat(" - Recruiting.");
		TheScriptEngine->AppendDebugMessage(teamName, false);
		const TCreateUnitsInfo *unitInfo = &teamProto->getTemplateInfo()->m_unitsInfo[0];
//		WorkOrder *orders = nullptr;
		Int i;
		Int unitsRecruited = 0;
		// Recruit.
		for( i=0; i<teamProto->getTemplateInfo()->m_numUnitsInfo; i++ )
		{
			const ThingTemplate *thing = TheThingFactory->findTemplate( unitInfo[i].unitThingName );
			if (thing)
			{
				int count = unitInfo[i].maxUnits;
				while (count>0) {
					Object *unit = theTeam->tryToRecruit(thing, &teamProto->getTemplateInfo()->m_homeLocation, recruitRadius);
					if (unit)
					{
						unitsRecruited++;

						AsciiString teamStr = "Team '";
						teamStr.concat(theTeam->getPrototype()->getName());
						teamStr.concat("' recruits ");
						teamStr.concat(thing->getName());
						teamStr.concat(" from team '");
						teamStr.concat(unit->getTeam()->getPrototype()->getName());
						teamStr.concat("'");
						TheScriptEngine->AppendDebugMessage(teamStr, false);

						unit->setTeam(theTeam);

						AIUpdateInterface *ai = unit->getAIUpdateInterface();
						if (ai)
						{
#ifdef DEBUG_LOGGING
							Coord3D pos = *unit->getPosition();
							Coord3D to = teamProto->getTemplateInfo()->m_homeLocation;
							DEBUG_LOG(("Moving unit from %f,%f to %f,%f", pos.x, pos.y , to.x, to.y ));
#endif
							ai->aiMoveToPosition( &teamProto->getTemplateInfo()->m_homeLocation, CMD_FROM_AI);
						}
					} else {
						break;
					}
					count--;
				}
			}
		}
		if (unitsRecruited>0)
		{
			/* We have something to build. */
			TeamInQueue *team = newInstance(TeamInQueue);
			// Put in front of queue.
			prependTo_TeamReadyQueue(team);
			team->m_priorityBuild = false;
			team->m_workOrders = nullptr;
			team->m_frameStarted = TheGameLogic->getFrame();
			team->m_team = theTeam;
			AsciiString teamName = teamProto->getName();
			teamName.concat(" - Finished recruiting.");
			TheScriptEngine->AppendDebugMessage(teamName, false);
		}	else {
			//disband.
			if (!theTeam->getPrototype()->getIsSingleton()) {
				deleteInstance(theTeam);
				theTeam = nullptr;
			}
			AsciiString teamName = teamProto->getName();
			teamName.concat(" - Recruited 0 units, disbanding.");
			TheScriptEngine->AppendDebugMessage(teamName, false);
		}
	}
}




/**
 * Train our teams.
 */
void AISkirmishPlayer::processTeamBuilding()
{
	// select a new team
	if (selectTeamToBuild()) {
		queueUnits();
	}
}

//----------------------------------------------------------------------------------------------------------
/**
 * See if it's time to build another base building.
 */
void AISkirmishPlayer::doBaseBuilding()
{
	if (m_player->getCanBuildBase()) {
		// See if we are ready to start trying a structure.
		if (!m_readyToBuildStructure) {
			m_structureTimer--;
			if (m_structureTimer<=0) {
				m_readyToBuildStructure = true;
				m_buildDelay = 0;
			}
			if (m_structureTimer > 3*LOGICFRAMES_PER_SECOND) {
				m_structureTimer = 3*LOGICFRAMES_PER_SECOND;
			}
		}
		// This timer is to keep from banging on the logic each frame.  If something interesting
		// happens, like a building is added or a unit finished, the timers are shortcut.
		m_buildDelay--;
		if (m_buildDelay<1) {
			if (m_readyToBuildStructure) {
				processBaseBuilding();
			}
			if (m_buildDelay<1) {	// processBaseBuilding may reset m_buildDelay.
				m_buildDelay = 2*LOGICFRAMES_PER_SECOND; // check again in 2 seconds.
			}
			// Note that this timer gets shortcut when a building is completed.
		}
	}
}

//----------------------------------------------------------------------------------------------------------
/**
 * See if any ready teams have finished moving to the rally point.
 */
void AISkirmishPlayer::checkReadyTeams()
{
	AIPlayer::checkReadyTeams();
}

//----------------------------------------------------------------------------------------------------------
/**
 * See if any queued teams have finished building, or have run out of time.
 */
void AISkirmishPlayer::checkQueuedTeams()
{
	AIPlayer::checkQueuedTeams();
}

//----------------------------------------------------------------------------------------------------------
/**
 * See if it is time to start another ai team building.
 */
void AISkirmishPlayer::doTeamBuilding()
{
	// See if any teams are expired.
	if (m_player->getCanBuildUnits()) {
		// See if we are ready to start trying a team.
		if (!m_readyToBuildTeam) {
			m_teamTimer--;
			if (m_teamTimer<=0) {
				m_readyToBuildTeam = true;
				m_teamDelay = 0;
			}
			if (m_teamTimer > 3*LOGICFRAMES_PER_SECOND) {
				m_teamTimer = 3*LOGICFRAMES_PER_SECOND;
			}
		}

		// This timer is to keep from banging on the logic each frame.  If something interesting
		// happens, like a building is added or a unit finished, the timers are shortcut.
		m_teamDelay--;
		if (m_teamDelay<1) {
			queueUnits(); // update the queues.
			if (m_readyToBuildTeam) {
				processTeamBuilding();
			}
			m_teamDelay = 2*LOGICFRAMES_PER_SECOND; // check again in 5 seconds.
			// Note that this timer gets shortcut when a unit or building is completed.
		}
	}
}

//----------------------------------------------------------------------------------------------------------
/**
 * Perform computer-controlled player AI
 */
void AISkirmishPlayer::update()
{
	AIPlayer::update();
	processBeaconDirective();
	// Surrender directive introduced in 1.3.0; skip for older replays so they
	// stay deterministic (see getReplayEpoch()).
	if (!TheRecorder || TheRecorder->isReplayEpochAtLeast(RecorderClass::REPLAY_EPOCH_V130))
		processSurrenderDirective();
	announceMilestones();
	processDistressSignal();
	commitIdleArmy();
	buyObjectUpgrades();
	laserLockMissileDefenders();
}


//----------------------------------------------------------------------------------------------------------
// TacticalAI per-object upgrade purchasing.
//
// Some of the strongest upgrades in the game are Type = OBJECT upgrades bought on an
// individual vehicle rather than researched once at a structure. The stock AI reaches them
// badly, and it is worth being precise about why, because there are two paths and they do
// not behave the same:
//   - AI_PLAYER_BUILD_UPGRADE takes an Upgrade, and AIPlayer::buildUpgrade explicitly
//     REJECTS object upgrades ("is an object, not a player upgrade. Ignoring request").
//     EA scripted Upgrade_AmericaAdvancedControlRods through this path; it is silently
//     dropped. So this path buys no object upgrade, ever.
//   - TEAM_USE_COMMANDBUTTON_ABILITY takes a CommandButton, not an Upgrade, and reaches
//     AIGroup::groupDoCommandButton -> Object::doCommandButton -> queueUpgrade(). It
//     bypasses the type check entirely, and SkirmishScripts.scb does use it.
// So the stock AI DOES buy some object upgrades. It just buys them badly: the Overlord
// scripts start disabled and are enable-gated on enemy composition (Gattling wants 5+
// enemy aircraft, Propaganda 15+ enemy tanks), fire once, and only target the
// "China CT - Tank *" combat teams -- not the wave teams that actually field most
// Overlords. Measured: stock upgrades ~68% of Tank General Emperors but only ~7% of
// base-China Overlords (n=101), leaving a 2000-credit tank with no anti-air. USA vehicle
// drones it never buys at all. This sweep buys them the way a human does.
//
// How an object upgrade actually attaches (all verified against the shipped INI):
//   - The carrier vehicle has one ObjectCreationUpgrade module per option, each TriggeredBy
//     its upgrade and ConflictsWith the others, so the options are mutually exclusive.
//   - The carrier has its OWN ProductionUpdate, which is what "researches" the upgrade.
//     A human clicking the OBJECT_UPGRADE command button lands in ProductionUpdate::
//     queueUpgrade() on the vehicle itself; that is exactly the call we make.
//   - Cost is withdrawn immediately by queueUpgrade(), so affordability MUST be checked
//     first -- see the money note in the sweep below.
//
// Because the modules mutually conflict, affectedByUpgrade() returns FALSE once the vehicle
// carries any one of them, and a drone's death removes its upgrade from the host, so a
// vehicle that loses its drone becomes eligible for a replacement here, exactly like a
// human would re-buy it.
//
// THE RULE THIS TABLE ENCODES: the AI only ever buys what a human could actually click,
// i.e. what is present in the object's CommandSet -- not merely what the upgrade modules
// technically allow. Object::canProduceUpgrade() is precisely that CommandSet test, and it
// is what makes the Emperor Overlord resolve to Gattling-only with no special case here.
//----------------------------------------------------------------------------------------------------------

// One object-upgrade the TacticalAI is allowed to buy, for one faction.
// To teach the AI a new object upgrade: add a row here plus its INI weight on TAiData.
// Rows are matched against Player::getBaseSide(), so a faction's row covers that faction
// and all of its sub-generals (BaseSide is "China" for base China, Tank, Infantry, Nuke
// and Boss; "USA" for base USA, Air Force, Laser and SuperWeapon).
struct AIObjectUpgradeRow
{
	const char *baseSide;			// Player base side this row applies to.
	const char *upgradeName;		// The Type = OBJECT upgrade to buy.
	size_t      weightOffset;		// offsetof(TAiData, ...) of this row's relative weight.
	size_t      cashReserveOffset;	// offsetof(TAiData, ...) of the cash floor for this row.
	UnsignedInt replayFeature;		// RecorderClass::ZULU_AI_FEATURE_* gate, so older replays stay bit-exact.
};

static const AIObjectUpgradeRow TheAIObjectUpgradeTable[] =
{
	// USA: vehicle drones (Humvee, Ambulance, Tomahawk, Crusader, Paladin, Avenger, Microwave).
	{ "USA",   "Upgrade_AmericaBattleDrone",           offsetof(TAiData, m_taiUsaBattleDroneWeight),         offsetof(TAiData, m_taiUsaDroneCashReserve),        RecorderClass::ZULU_AI_FEATURE_USA_DRONES },
	{ "USA",   "Upgrade_AmericaScoutDrone",            offsetof(TAiData, m_taiUsaScoutDroneWeight),          offsetof(TAiData, m_taiUsaDroneCashReserve),        RecorderClass::ZULU_AI_FEATURE_USA_DRONES },
	{ "USA",   "Upgrade_AmericaHellfireDrone",         offsetof(TAiData, m_taiUsaHellfireDroneWeight),       offsetof(TAiData, m_taiUsaDroneCashReserve),        RecorderClass::ZULU_AI_FEATURE_USA_DRONES },

	// China: Overlord turret upgrades. The Overlord is a 2000cr tank the AI otherwise fields
	// completely bare, with no anti-air at all, so the Gattling Cannon (1200) dominates the
	// mix over the Propaganda Tower (500).
	//
	// Upgrade_ChinaOverlordBattleBunker (400) is DELIBERATELY ABSENT and must stay absent.
	// It conflicts with the other two and only pays off with infantry garrisoned inside,
	// which the AI has no logic to do, so buying it is a straight downgrade. Note that the
	// CommandSet test alone would NOT exclude it -- a standard Overlord does have a Battle
	// Bunker button -- so leaving it out of this table is the thing that excludes it.
	{ "China", "Upgrade_ChinaOverlordGattlingCannon",  offsetof(TAiData, m_taiChinaOverlordGattlingWeight),   offsetof(TAiData, m_taiChinaOverlordCashReserve),   RecorderClass::ZULU_AI_FEATURE_CHINA_OVERLORD },
	{ "China", "Upgrade_ChinaOverlordPropagandaTower", offsetof(TAiData, m_taiChinaOverlordPropagandaWeight), offsetof(TAiData, m_taiChinaOverlordCashReserve),   RecorderClass::ZULU_AI_FEATURE_CHINA_OVERLORD },
};

// enum (not static const Int): VC6 will not accept every const object as an array bound.
enum
{
	TheAIObjectUpgradeTableSize =
		sizeof(TheAIObjectUpgradeTable) / sizeof(TheAIObjectUpgradeTable[0])
};

// Read an Int field of TAiData given its offsetof(). Same idiom the engine's own INI
// FieldParse tables use on this very struct.
static Int readTAiDataInt( const TAiData *aiData, size_t offset )
{
	return *(const Int *)((const char *)aiData + offset);
}

// A row that survived the faction / replay-feature / weight filters, resolved to a template.
struct AIObjectUpgradeCandidate
{
	const UpgradeTemplate *upgrade;
	Int                    weight;
	Int                    cashReserve;
};

void AISkirmishPlayer::buyObjectUpgrades()
{
	// Gated behind the SLOT_TACTICAL_AI lobby option; vanilla Easy/Medium/Hard/Brutal
	// AIs keep their classic behavior unchanged and buy nothing here.
	if (!isTacticalAI()) return;

	const TAiData *aiData = TheAI ? TheAI->getAiData() : nullptr;
	if (!aiData) return;

	// Collect the rows that apply to this player, BEFORE touching the sweep clock.
	// A player with no applicable rows (wrong faction, feature disabled for this replay,
	// or all weights zeroed in INI) must fall out here having changed no state at all,
	// so that its m_nextUpgradeSweepFrame stays put and old replays stay bit-exact.
	AIObjectUpgradeCandidate candidates[TheAIObjectUpgradeTableSize];
	Int numCandidates = 0;

	const AsciiString &baseSide = m_player->getBaseSide();

	Int i;
	for (i = 0; i < TheAIObjectUpgradeTableSize; ++i)
	{
		const AIObjectUpgradeRow *row = &TheAIObjectUpgradeTable[i];

		if (baseSide.compareNoCase( row->baseSide ) != 0) continue;

		// During replay / resume-catchup of a recording made before this feature existed,
		// the original AI did not buy this upgrade -- spending its money here would diverge
		// from the recorded simulation.
		if (TheRecorder && !TheRecorder->isAIFeatureEnabled( row->replayFeature )) continue;

		Int weight = readTAiDataInt( aiData, row->weightOffset );
		if (weight <= 0) continue; // 0 = never pick this one; all-zero disables the faction.

		const UpgradeTemplate *upgrade = TheUpgradeCenter->findUpgrade( AsciiString( row->upgradeName ) );
		if (!upgrade) continue; // not in this INI set; ignore rather than assert.

		Int reserve = readTAiDataInt( aiData, row->cashReserveOffset );
		if (reserve < 0) reserve = 0;

		candidates[numCandidates].upgrade     = upgrade;
		candidates[numCandidates].weight      = weight;
		candidates[numCandidates].cashReserve = reserve;
		++numCandidates;
	}

	if (numCandidates == 0) return;

	const Int UPGRADE_SWEEP_INTERVAL_SECONDS = 2;
	const Int MAX_PURCHASES_PER_SWEEP = 2;

	UnsignedInt curFrame = TheGameLogic->getFrame();
	if (curFrame < m_nextUpgradeSweepFrame) return;
	m_nextUpgradeSweepFrame = curFrame + UPGRADE_SWEEP_INTERVAL_SECONDS * LOGICFRAMES_PER_SECOND;

	Money *money = m_player->getMoney();
	if (!money) return;

	Int purchases = 0;

	// Walk every team this player owns. Iteration order is deterministic across
	// clients (linked lists, stable insertion order), same as commitIdleArmy().
	Player::PlayerTeamList::const_iterator pti;
	for (pti = m_player->getPlayerTeams()->begin(); pti != m_player->getPlayerTeams()->end(); ++pti)
	{
		DLINK_ITERATOR<Team> ti = (*pti)->iterate_TeamInstanceList();
		for (; !ti.done(); ti.advance())
		{
			Team *team = ti.cur();
			if (!team) continue;

			DLINK_ITERATOR<Object> oi = team->iterate_TeamMemberList();
			for (; !oi.done(); oi.advance())
			{
				if (purchases >= MAX_PURCHASES_PER_SWEEP) return;

				Object *obj = oi.cur();
				if (!obj) continue;
				if (!obj->isKindOf(KINDOF_VEHICLE)) continue;
				if (obj->isKindOf(KINDOF_DRONE)) continue;
				if (obj->isEffectivelyDead()) continue;
				if (obj->getStatusBits().test(OBJECT_STATUS_UNDER_CONSTRUCTION)) continue;
				if (obj->getStatusBits().test(OBJECT_STATUS_SOLD)) continue;

				ProductionUpdateInterface *pu = obj->getProductionUpdateInterface();
				if (!pu) continue; // no ProductionUpdate => not an upgrade carrier at all.

				// Narrow the faction's rows to the ones THIS vehicle can actually buy right
				// now. Doing this BEFORE the weighted pick (rather than picking first and
				// discarding) is what makes the mix behave: an Emperor Overlord, which can
				// only ever take the Gattling, always gets a Gattling instead of rolling
				// "Propaganda" and being skipped forever.
				Int eligible[TheAIObjectUpgradeTableSize];
				Int numEligible = 0;
				Int totalWeight = 0;

				Int c;
				for (c = 0; c < numCandidates; ++c)
				{
					const UpgradeTemplate *upgrade = candidates[c].upgrade;

					// Already has it. (Because the modules ConflictsWith each other, this and
					// affectedByUpgrade() below together give us mutual exclusivity for free:
					// once an Overlord has a Gattling, nothing else here is eligible.)
					if (obj->hasUpgrade( upgrade )) continue;

					// Does this object have an upgrade module that responds to it at all?
					// This is what excludes the Emperor from Propaganda: its propaganda tower
					// is intrinsic, so it has no propaganda ObjectCreationUpgrade module.
					if (!obj->affectedByUpgrade( upgrade )) continue;

					// Could a human actually click this? Object::canProduceUpgrade() walks the
					// object's CommandSet looking for the OBJECT_UPGRADE button bound to this
					// upgrade. This is the "only buy what a human could buy" rule, and it is a
					// second, independent reason the Emperor never buys Propaganda (its
					// CommandSet has the propaganda button commented out in the stock INI).
					if (!obj->canProduceUpgrade( upgrade )) continue;

					if (pu->isUpgradeInQueue( upgrade )) continue;
					// The carrier's ProductionUpdate has a small MaxQueueEntries (1 on a standard
					// Overlord); queueing into a full queue asserts in ProductionUpdate::queueUpgrade.
					if (pu->canQueueUpgrade( upgrade ) == CANMAKE_QUEUE_FULL) continue;

					eligible[numEligible++] = c;
					totalWeight += candidates[c].weight;
				}

				if (numEligible == 0 || totalWeight <= 0) continue;

				// Weighted pick among the eligible options. The roll is a hash of the object's
				// ID rather than a draw from the logic RNG: it is identical on every client and
				// stable for the life of the unit, and it does NOT perturb the shared random
				// stream (which would desync every other consumer of it).
				UnsignedInt h = (UnsignedInt)obj->getID();
				h ^= h >> 15;
				h *= 2246822519U;
				h ^= h >> 13;
				h *= 3266489917U;
				h ^= h >> 16;

				Int roll = (Int)(h % (UnsignedInt)totalWeight);

				const AIObjectUpgradeCandidate *pick = nullptr;
				Int e;
				for (e = 0; e < numEligible; ++e)
				{
					const AIObjectUpgradeCandidate *cand = &candidates[ eligible[e] ];
					if (roll < cand->weight)
					{
						pick = cand;
						break;
					}
					roll -= cand->weight;
				}
				if (!pick) continue;

				// Money: queueUpgrade() withdraws the cost immediately, and Money::withdraw()
				// silently clamps at zero, so NOTHING below us stops the AI from getting the
				// upgrade free and/or draining its army budget if we don't check here. Keep
				// cashReserve untouched so upgrades are bought out of genuine surplus only.
				// The reserve is per-row, and the cost is the real cost, so a 1200 Gattling
				// naturally demands far more cash on hand than a 300 Battle Drone.
				// Skip this vehicle, don't abandon the sweep: the pick is a stable hash of the
				// object's ID, so one vehicle that rolled an upgrade it can never afford would
				// otherwise starve every vehicle behind it on every sweep, forever. A cheaper
				// upgrade on another vehicle may still be affordable right now.
				Int cost = pick->upgrade->calcCostToBuild( m_player );
				if ((Int)money->countMoney() < cost + pick->cashReserve)
					continue;

				if (pu->queueUpgrade( pick->upgrade ))
					++purchases;
			}
		}
	}
}

//----------------------------------------------------------------------------------------------------------
// TacticalAI USA Missile Defender laser lock.
//
// SPECIAL_MISSILE_DEFENDER_LASER_GUIDED_MISSILES ("Laser Lock") is a free SpecialAbility: no
// upgrade, no science, no cost, no cooldown, available from frame 0. It locks the Missile
// Defender to its SECONDARY weapon, which does the same 40 damage but fires every 500ms instead
// of every 1000ms and deals ARMOR_PIERCING instead of INFANTRY_MISSILE damage. That is 2x DPS
// against tanks and 4x against light/wheeled vehicles (which resist INFANTRY_MISSILE at 50%),
// for a one-time ~1000ms preparation. A human USA player clicks it constantly; the stock AI
// never clicks it at all, because nothing in the AI or in SkirmishScripts.scb issues it.
//
// This sweep issues exactly the command a human would click, and nothing more:
//   - it never picks a target of its own. The target is whatever vehicle the Missile Defender
//     is ALREADY shooting (AIUpdateInterface::getCurrentVictim), so no unit is pulled off its
//     team's orders to go hunt something, and there is no randomness to draw from the logic RNG.
//   - it goes through the object's own CommandSet to find the button (see below), so it can
//     only ever fire an ability the object genuinely has on its command bar.
//
// KNOWN AND ACCEPTED: a Missile Defender riding inside a Humvee cannot laser lock. TransportContain
// ::onContaining sets DISABLED_HELD on its riders, and Object::doSpecialPowerAtObject early-returns
// on isDisabled(), so the command is silently dropped -- for a human exactly as for the AI. Since
// the TacticalAI's Hard USA waves are Humvee + Missile Defenders, many Missile Defenders are
// passengers much of the time and will simply never lock. That is expected: no dismount logic here.
// The isDisabled() skip below is what keeps us from spamming commands the engine would drop.
//----------------------------------------------------------------------------------------------------------

// Find the CommandButton on this object's own CommandSet that fires the given special power.
//
// THE RULE: the AI only does what a human could actually click, i.e. what is present in the
// object's CommandSet. Matching on the special power TYPE rather than on a button NAME is
// deliberate and load-bearing: CommandButton.ini defines
// AirF_Command_AmericaMissileDefenderLaserGuidedMissiles, but that button is in NO CommandSet --
// the Air Force and SuperWeapon Missile Defenders both reference the base
// Command_AmericaMissileDefenderLaserGuidedMissiles. Name-matching would happen to work today and
// would silently rot the first time a sub-general got its own button. Type-matching is
// sub-general-proof, and it also guarantees the button is a GUI_COMMAND_SPECIAL_POWER, which is
// the branch of Object::doCommandButtonAtObject we intend to hit.
static const CommandButton *findSpecialPowerCommandButton( const Object *obj, SpecialPowerType type )
{
	if (!TheControlBar) return nullptr;

	const CommandSet *commandSet = TheControlBar->findCommandSet( obj->getCommandSetString() );
	if (!commandSet) return nullptr;

	Int i;
	for (i = 0; i < MAX_COMMANDS_PER_SET; ++i)
	{
		const CommandButton *button = commandSet->getCommandButton( i );
		if (!button) continue;
		if (button->getCommandType() != GUI_COMMAND_SPECIAL_POWER) continue;

		const SpecialPowerTemplate *spTemplate = button->getSpecialPowerTemplate();
		if (!spTemplate) continue;
		if (spTemplate->getSpecialPowerType() != type) continue;

		return button;
	}

	return nullptr;
}

// Should the sweep DECLINE to laser lock a Missile Defender that is shooting this victim because
// the victim is a fast fixed-wing jet (Raptor, MiG, Aurora, Stealth)?
//
// We lock ground vehicles AND hovering/slow aircraft (Comanche, Chinook, Helix), but NOT jets.
// In-game measurement showed locking jets is a NET LOSS -- ~3.6% WORSE anti-air -- for two
// independent reasons, both of which a hovering aircraft escapes:
//   1) ABORT. The lock has a ~1000ms preparation, and a jet routinely flies out of the ability's
//      AbilityAbortRange (250) before it completes -- roughly 60% of jet locks never finish. This
//      is not incidental: EVERY jet locomotor in Locomotor.ini carries MinSpeed=60, i.e. a jet is
//      physically unable to loiter, while HOVER locomotors (Comanche/Chinook/Helix) have no
//      MinSpeed and sit still. The very property we key on is the property that causes the abort.
//   2) LOST AIR BONUS. Both aircraft armors give the primary INFANTRY_MISSILE weapon a +20% air
//      bonus that the locked ARMOR_PIERCING secondary does not get (40 vs 48 per shot). So even a
//      jet lock that DOES complete throws that bonus away. Against a Comanche the trade is still a
//      win -- it loiters, the lock completes, and 48 DPS becomes 80 DPS, a real 1.67x -- so those
//      stay eligible.
//
// DISCRIMINANT: the locomotor APPEARANCE, an INI-derived, deterministic property of the unit's
// TYPE (Locomotor.ini). Jets/fixed-wing are WINGS, thrust flyers are THRUST, and both must keep
// moving forward; Comanche/Chinook/Helix are HOVER; ground vehicles are TREADS/WHEELS/HOVER. This
// is intentionally NOT the unit's momentary speed (getCurLocomotorSpeed), which is jittery and
// would misfire on a briefly-slow jet, and NOT KINDOF_PRODUCED_AT_HELIPAD (Comanche-only, too
// narrow). Default is FALSE (lock): if a victim's locomotor cannot be read we err toward locking,
// which never wrongly skips a ground vehicle. Do NOT "simplify" this back to locking all aircraft.
static Bool isFastJetVictim( const Object *victim )
{
	const AIUpdateInterface *ai = victim->getAIUpdateInterface();
	if (!ai) return FALSE;

	const Locomotor *loco = ai->getCurLocomotor();
	if (!loco) return FALSE;

	LocomotorAppearance app = loco->getAppearance();
	return (app == LOCO_WINGS || app == LOCO_THRUST);
}

void AISkirmishPlayer::laserLockMissileDefenders()
{
	// Gated behind the SLOT_TACTICAL_AI lobby option; vanilla Easy/Medium/Hard/Brutal AIs keep
	// their classic behavior and never laser lock.
	if (!isTacticalAI()) return;

	// Everything that could disqualify this player is checked BEFORE the sweep clock is touched.
	// m_nextLaserLockSweepFrame is xfer'd and CRC'd, so a player that can never laser lock (wrong
	// faction, feature disabled for this replay, INI kill switch) must fall out of here having
	// changed no state at all -- otherwise replays recorded before this feature existed would
	// diverge. Same reason buyObjectUpgrades() does its filtering first.

	// During replay / resume-catchup of a recording made before this feature existed, the original
	// AI did not laser lock -- issuing the command here would diverge from the recorded simulation.
	if (TheRecorder && !TheRecorder->isAIFeatureEnabled( RecorderClass::ZULU_AI_FEATURE_MD_LASER_LOCK ))
		return;

	// BaseSide is "USA" for base USA, Air Force, Laser and SuperWeapon, so this one test covers
	// every sub-general that fields a Missile Defender, and excludes China/GLA for free.
	if (m_player->getBaseSide().compareNoCase( "USA" ) != 0) return;

	const TAiData *aiData = TheAI ? TheAI->getAiData() : nullptr;
	if (!aiData) return;

	Int maxLocks = aiData->m_taiUsaLaserLockMaxPerSweep;
	if (maxLocks <= 0) return; // INI kill switch: 0 disables the feature.

	// Interval is in logic frames (30 = 1s), so a sub-second reactive rate is expressible;
	// default is 15 (0.5s). At least 1 frame, since 0 would sweep every frame with no spacing.
	Int intervalFrames = aiData->m_taiUsaLaserLockIntervalFrames;
	if (intervalFrames < 1) intervalFrames = 1;

	if (!TheActionManager) return;

	UnsignedInt curFrame = TheGameLogic->getFrame();
	// Phase the first sweep by player index so the AI players don't all sweep on the same
	// logic frame (a synchronized spike). getPlayerIndex() is identical on every client, so
	// this stays deterministic. Only matters until the first sweep; after that the interval
	// carries the phase forward.
	if (m_nextLaserLockSweepFrame == 0 && curFrame == 0)
		m_nextLaserLockSweepFrame = (UnsignedInt)(m_player->getPlayerIndex() % intervalFrames);
	if (curFrame < m_nextLaserLockSweepFrame) return;
	m_nextLaserLockSweepFrame = curFrame + intervalFrames;

	Int locks = 0;

	// Walk every team this player owns. Iteration order is deterministic across clients
	// (linked lists, stable insertion order), same as commitIdleArmy() and buyObjectUpgrades().
	Player::PlayerTeamList::const_iterator pti;
	for (pti = m_player->getPlayerTeams()->begin(); pti != m_player->getPlayerTeams()->end(); ++pti)
	{
		DLINK_ITERATOR<Team> ti = (*pti)->iterate_TeamInstanceList();
		for (; !ti.done(); ti.advance())
		{
			Team *team = ti.cur();
			if (!team) continue;

			DLINK_ITERATOR<Object> oi = team->iterate_TeamMemberList();
			for (; !oi.done(); oi.advance())
			{
				if (locks >= maxLocks) return;

				Object *obj = oi.cur();
				if (!obj) continue;
				if (obj->isEffectivelyDead()) continue;

				// LOAD-BEARING, not defensive boilerplate: riders in a transport carry DISABLED_HELD,
				// and doSpecialPowerAtObject() early-returns on isDisabled(). See the note above.
				if (obj->isDisabled()) continue;

				// Does this object own the ability at all? Asking the object rather than matching a
				// template name keeps this working for every sub-general's Missile Defender.
				if (!obj->hasSpecialPower( SPECIAL_MISSILE_DEFENDER_LASER_GUIDED_MISSILES )) continue;

				SpecialAbilityUpdate *ability =
					obj->findSpecialAbilityUpdate( SPECIAL_MISSILE_DEFENDER_LASER_GUIDED_MISSILES );
				if (!ability) continue;

				// Already preparing or already locked. This is the anti-thrash check: the ability stays
				// active from the moment it is initiated, through its ~1000ms preparation, and for as
				// long as the weapon stays locked, so a Missile Defender pays that preparation once per
				// target rather than once per sweep.
				if (ability->isActive()) continue;

				AIUpdateInterface *ai = obj->getAIUpdateInterface();
				if (!ai) continue;

				// The target is the vehicle this Missile Defender is already shooting -- we never choose
				// one, so there is nothing to randomize and no unit gets redirected.
				Object *victim = ai->getCurrentVictim();
				if (!victim) continue;
				if (victim->isEffectivelyDead()) continue;
				if (!victim->isKindOf( KINDOF_VEHICLE )) continue;

				// Lock ground vehicles AND hovering/slow aircraft (Comanche, Chinook, Helix), but skip
				// fast fixed-wing jets: measured net-negative anti-air because the lock aborts mid-windup
				// as the jet leaves AbilityAbortRange, and even a completed jet lock loses the primary
				// weapon's +20% air-armor bonus. See isFastJetVictim() for the full reasoning.
				if (isFastJetVictim( victim )) continue;

				// Only lock a target that is already inside the ability's own StartAbilityRange. Outside
				// it, SpecialAbilityUpdate::update() calls approachTarget() -> aiMoveToObject(), which
				// would walk the Missile Defender out of its team's formation to close the distance.
				// Mirrors SpecialAbilityUpdate::isWithinStartAbilityRange().
				Real startRange = ability->getStartAbilityRange();
				if (startRange < SPECIAL_ABILITY_HUGE_DISTANCE)
				{
					const Real UNDERSIZE = PATHFIND_CELL_SIZE_F * 0.25f;
					Real range = startRange - UNDERSIZE;
					if (range < 0.0f) range = 0.0f;

					Real distSq = ThePartitionManager->getDistanceSquared( obj, victim, FROM_BOUNDINGSPHERE_2D );
					if (distSq > range * range) continue;
				}

				// Could a human actually click this, on this unit, right now?
				const CommandButton *button =
					findSpecialPowerCommandButton( obj, SPECIAL_MISSILE_DEFENDER_LASER_GUIDED_MISSILES );
				if (!button) continue;

				const SpecialPowerTemplate *spTemplate = button->getSpecialPowerTemplate();

				// The same validation the UI runs before it lets the cursor commit: it re-checks that the
				// power is ready, that the target is not shrouded, and (for this power specifically) that
				// the target is a KINDOF_VEHICLE the player is at ENEMIES with.
				if (!TheActionManager->canDoSpecialPowerAtObject( obj, victim, CMD_FROM_AI, spTemplate, button->getOptions() ))
					continue;

				obj->doCommandButtonAtObject( button, victim, CMD_FROM_AI );
				++locks;
			}
		}
	}
}

//----------------------------------------------------------------------------------------------------------
// Idle Army Commit: backstops the script-driven team flow. Skirmish maps
// rely on .scb scripts to dispatch attack teams; once those scripts stop
// firing (or when surviving units of dead teams remain on a still-existing
// team blocking maxInstances), combat units pile up in the base. This sweep
// gathers the surplus and group-attacks the current enemy.
//----------------------------------------------------------------------------------------------------------
void AISkirmishPlayer::commitIdleArmy()
{
	// Gated behind the SLOT_TACTICAL_AI lobby option; vanilla Easy/Medium/Brutal
	// AIs keep their classic behavior unchanged.
	if (!isTacticalAI()) return;

	// During replay/resume-catchup of older recordings, the original AI did
	// not run this sweep — issuing new commands here would diverge from the
	// recorded simulation. The recorder gates each AI feature on the replay's
	// declared feature version.
	if (TheRecorder && !TheRecorder->isAIFeatureEnabled(RecorderClass::ZULU_AI_FEATURE_IDLE_COMMIT))
		return;

	const Int IDLE_SWEEP_INTERVAL_SECONDS = 10;
	const Int MIN_GARRISON_UNITS = 4;
	const Int MIN_ATTACK_FORCE = 5;

	UnsignedInt curFrame = TheGameLogic->getFrame();
	if (curFrame < m_nextIdleSweepFrame) return;
	m_nextIdleSweepFrame = curFrame + IDLE_SWEEP_INTERVAL_SECONDS * LOGICFRAMES_PER_SECOND;

	// When a directive beacon is active, the beacon location takes the place
	// of the regular enemy-structure target — units still attack-move, so they
	// engage anything on the way.
	Object *directiveBeacon = (m_directiveBeaconID != INVALID_ID)
		? TheGameLogic->findObjectByID(m_directiveBeaconID)
		: nullptr;

	Player *enemy = getAiEnemy();
	if (!directiveBeacon) {
		if (!enemy) return;
		if (!enemy->hasAnyBuildFacility() && !enemy->hasAnyUnits()) return;
	}

	// Teams that are mid-build or sitting at the rally point waiting for activation
	// must be left alone — their units are about to receive scripted orders.
	std::set<const Team*> reservedTeams;
	{
		DLINK_ITERATOR<TeamInQueue> bIter = iterate_TeamBuildQueue();
		for (; !bIter.done(); bIter.advance())
		{
			TeamInQueue *t = bIter.cur();
			if (t && t->m_team) reservedTeams.insert(t->m_team);
		}
		DLINK_ITERATOR<TeamInQueue> rIter = iterate_TeamReadyQueue();
		for (; !rIter.done(); rIter.advance())
		{
			TeamInQueue *t = rIter.cur();
			if (t && t->m_team) reservedTeams.insert(t->m_team);
		}
	}

	// Walk every team this player owns and collect idle combat-capable units.
	// Iteration order is deterministic across clients (linked lists, stable insertion order).
	std::vector<Object*> eligible;
	Player::PlayerTeamList::const_iterator pti;
	for (pti = m_player->getPlayerTeams()->begin(); pti != m_player->getPlayerTeams()->end(); ++pti)
	{
		DLINK_ITERATOR<Team> ti = (*pti)->iterate_TeamInstanceList();
		for (; !ti.done(); ti.advance())
		{
			Team *team = ti.cur();
			if (!team) continue;
			if (reservedTeams.find(team) != reservedTeams.end()) continue;

			DLINK_ITERATOR<Object> oi = team->iterate_TeamMemberList();
			for (; !oi.done(); oi.advance())
			{
				Object *obj = oi.cur();
				if (!obj) continue;
				if (obj->isKindOf(KINDOF_STRUCTURE)) continue;
				if (obj->isKindOf(KINDOF_DOZER)) continue;
				if (obj->isKindOf(KINDOF_HARVESTER)) continue;
				if (obj->isKindOf(KINDOF_AIRCRAFT)) continue;
				if (obj->isKindOf(KINDOF_DRONE)) continue;
				if (!obj->isKindOf(KINDOF_INFANTRY) && !obj->isKindOf(KINDOF_VEHICLE)) continue;
				if (!obj->isAbleToAttack()) continue;
				if (!obj->hasAnyWeapon()) continue;
				if (obj->getStatusBits().test(OBJECT_STATUS_UNDER_CONSTRUCTION)) continue;
				if (obj->getStatusBits().test(OBJECT_STATUS_SOLD)) continue;
				AIUpdateInterface *ai = obj->getAIUpdateInterface();
				if (!ai || !ai->isIdle()) continue;
				eligible.push_back(obj);
			}
		}
	}

	Int total = (Int)eligible.size();
	Int toSend = total - MIN_GARRISON_UNITS;
	if (toSend < MIN_ATTACK_FORCE) return;

	// Aim at the enemy structure closest to our base center. Real targets are
	// always reachable terrain and always meaningful (vs. the bbox midpoint
	// which can land in dead space between structure clusters). Per-unit
	// auto-targeting still engages threats encountered along the way.
	Coord3D target;
	Object *closestStructure = nullptr;
	if (directiveBeacon) {
		target = *directiveBeacon->getPosition();
	} else {
		Real closestDistSqr = 0.0f;
		Player::PlayerTeamList::const_iterator eti;
		for (eti = enemy->getPlayerTeams()->begin(); eti != enemy->getPlayerTeams()->end(); ++eti)
		{
			DLINK_ITERATOR<Team> tIter = (*eti)->iterate_TeamInstanceList();
			for (; !tIter.done(); tIter.advance())
			{
				Team *team = tIter.cur();
				if (!team) continue;
				DLINK_ITERATOR<Object> oIter = team->iterate_TeamMemberList();
				for (; !oIter.done(); oIter.advance())
				{
					Object *o = oIter.cur();
					if (!o) continue;
					if (!o->isKindOf(KINDOF_STRUCTURE)) continue;
					if (o->isEffectivelyDead()) continue;
					Coord3D p = *o->getPosition();
					Real d = sqr(p.x - m_baseCenter.x) + sqr(p.y - m_baseCenter.y);
					if (!closestStructure || d < closestDistSqr)
					{
						closestStructure = o;
						closestDistSqr = d;
					}
				}
			}
		}
		if (!closestStructure) return; // enemy has no structures; let scripts/auto-target handle.
		target = *closestStructure->getPosition();
	}

	AIGroupPtr theGroup = TheAI->createGroup();
	if (!theGroup) return;

	// First MIN_GARRISON_UNITS in iteration order stay home; the rest commit.
	Int i;
	for (i = MIN_GARRISON_UNITS; i < total; ++i)
	{
		theGroup->add(eligible[i]);
	}

	theGroup->groupAttackMoveToPosition(&target, NO_MAX_SHOTS_LIMIT, CMD_FROM_AI);

	// Ally chatter: announce attack-commit dispatches against an enemy structure.
	// Beacon-directive dispatches are already announced via processBeaconDirective().
	if (!directiveBeacon && enemy)
	{
		announceAttackCommit(&target, enemy);
	}

	if (TheGlobalData->m_debugAI)
	{
		AsciiString msg;
		if (directiveBeacon) {
			msg.format("**AI** Idle army commit: sending %d units to ally beacon at (%.0f, %.0f)",
				toSend, target.x, target.y);
		} else {
			msg.format("**AI** Idle army commit: sending %d units to attack %s structure '%s'",
				toSend,
				TheNameKeyGenerator->keyToName(enemy->getPlayerNameKey()).str(),
				closestStructure->getTemplate()->getName().str());
		}
		TheScriptEngine->AppendDebugMessage(msg, false);
	}
}

//----------------------------------------------------------------------------------------------------------
// Beacon Directive: a TacticalAI follows the most recently placed ally beacon
// whose caption begins with "!attack" (case-insensitive). The beacon is treated
// as an attack target by commitIdleArmy(); when the beacon is removed or its
// caption no longer carries the prefix, the AI reverts to its default target
// selection. Once per game-minute the lowest-index TacticalAI on the beacon
// owner's team prints a local on-screen message naming the beacon owner; the
// message is shown only on ally clients. Because the announcement is generated
// deterministically inside the simulation, every client (and replay viewer)
// produces the same message at the same frame without any network chat.
//----------------------------------------------------------------------------------------------------------
void AISkirmishPlayer::processBeaconDirective()
{
	if (!isTacticalAI()) return;

	const Int DIRECTIVE_SCAN_INTERVAL_SECONDS  = 1;
	const Int DIRECTIVE_ANNOUNCE_INTERVAL_SECS = 60;
	static const WideChar PREFIX[] = L"!attack";

	UnsignedInt curFrame = TheGameLogic->getFrame();
	if (curFrame < m_nextDirectiveScanFrame) return;
	m_nextDirectiveScanFrame = curFrame + DIRECTIVE_SCAN_INTERVAL_SECONDS * LOGICFRAMES_PER_SECOND;

	// Find the newest active directive beacon owned by an ally. Newest = highest
	// ObjectID, which is monotonic and identical across clients, keeping the
	// selection deterministic.
	Object *bestBeacon = nullptr;
	Player *beaconOwner = nullptr;
	Int playerCount = ThePlayerList->getPlayerCount();
	Int pi;
	for (pi = 0; pi < playerCount; ++pi)
	{
		Player *ally = ThePlayerList->getNthPlayer(pi);
		if (!ally || ally == m_player) continue;
		if (!ally->isPlayerActive()) continue;
		if (m_player->getRelationship(ally->getDefaultTeam()) != ALLIES) continue;
		const PlayerTemplate *pt = ally->getPlayerTemplate();
		if (!pt) continue;
		const ThingTemplate *beaconTmpl = TheThingFactory->findTemplate(pt->getBeaconTemplate());
		if (!beaconTmpl) continue;

		Player::PlayerTeamList::const_iterator pti;
		for (pti = ally->getPlayerTeams()->begin(); pti != ally->getPlayerTeams()->end(); ++pti)
		{
			DLINK_ITERATOR<Team> ti = (*pti)->iterate_TeamInstanceList();
			for (; !ti.done(); ti.advance())
			{
				Team *team = ti.cur();
				if (!team) continue;
				DLINK_ITERATOR<Object> oi = team->iterate_TeamMemberList();
				for (; !oi.done(); oi.advance())
				{
					Object *obj = oi.cur();
					if (!obj) continue;
					if (!obj->getTemplate()->isEquivalentTo(beaconTmpl)) continue;
					Drawable *d = obj->getDrawable();
					if (!d) continue;
					UnicodeString cap = d->getCaptionText();
					if (!cap.startsWithNoCase(PREFIX)) continue;
					if (!bestBeacon || obj->getID() > bestBeacon->getID())
					{
						bestBeacon = obj;
						beaconOwner = ally;
					}
				}
			}
		}
	}

	ObjectID newID = bestBeacon ? bestBeacon->getID() : INVALID_ID;
	Bool switched = (newID != m_directiveBeaconID);
	m_directiveBeaconID = newID;

	if (newID == INVALID_ID)
	{
		// No active directive: drop announcement state so a future beacon
		// re-announces immediately on switch.
		m_nextDirectiveAnnounceFrame = 0;
		return;
	}

	Bool dueAnnounce = switched || curFrame >= m_nextDirectiveAnnounceFrame;
	if (!dueAnnounce) return;
	m_nextDirectiveAnnounceFrame = curFrame + DIRECTIVE_ANNOUNCE_INTERVAL_SECS * LOGICFRAMES_PER_SECOND;

	// Only the lowest-index TacticalAI ally of the beacon owner announces, so
	// multiple TacticalAI players on the same team don't spam duplicates.
	Int lowestAllyTacticalAIIndex = -1;
	for (pi = 0; pi < playerCount; ++pi)
	{
		Player *p = ThePlayerList->getNthPlayer(pi);
		if (!p || !p->isPlayerActive()) continue;
		if (p != beaconOwner && beaconOwner->getRelationship(p->getDefaultTeam()) != ALLIES) continue;
		if (!p->isTacticalAIPlayer()) continue;
		lowestAllyTacticalAIIndex = pi;
		break;
	}
	if (lowestAllyTacticalAIIndex != m_player->getPlayerIndex()) return;

	// Display only on clients whose local player is allied to (or is) the beacon
	// owner. The branch is purely cosmetic — sim state is identical everywhere.
	Player *localPlayer = ThePlayerList->getLocalPlayer();
	if (!localPlayer) return;
	Bool ally = (localPlayer == beaconOwner)
		|| (localPlayer->getRelationship(beaconOwner->getDefaultTeam()) == ALLIES);
	if (!ally) return;

	RGBColor rgb;
	rgb.setFromInt(beaconOwner->getPlayerColor());
	UnicodeString announcement;
	announcement.format(L"%ls is following %ls's beacon",
		m_player->getPlayerDisplayName().str(),
		beaconOwner->getPlayerDisplayName().str());
	TheInGameUI->messageColor(&rgb, announcement);
}

//----------------------------------------------------------------------------------------------------------
// Surrender Directive: when a human ally types "!surrender" in chat, the
// receive-side ConnectionManager::processChat() latches m_surrenderConsented
// on the sender (frame-synchronized, identical across clients). This method
// then kills this AI player once (a) at least one human ally has consented
// AND (b) every human on this team is no longer active. "!unsurrender"
// clears the flag on the sending player.
//----------------------------------------------------------------------------------------------------------
void AISkirmishPlayer::processSurrenderDirective()
{
	if (m_hasSurrendered) return;
	if (!m_player->isPlayerActive()) return;

	const Int SURRENDER_CHECK_INTERVAL_SECONDS = 1;
	UnsignedInt curFrame = TheGameLogic->getFrame();
	if (curFrame < m_nextSurrenderCheckFrame) return;
	m_nextSurrenderCheckFrame = curFrame + SURRENDER_CHECK_INTERVAL_SECONDS * LOGICFRAMES_PER_SECOND;

	Bool anyHumanConsented = false;
	Bool anyHumanActive = false;
	Player *consentingHuman = nullptr;
	Int lowestAIIndex = -1;

	Int playerCount = ThePlayerList->getPlayerCount();
	for (Int pi = 0; pi < playerCount; ++pi)
	{
		Player *ally = ThePlayerList->getNthPlayer(pi);
		if (!ally) continue;
		Bool onMyTeam = (ally == m_player)
			|| (m_player->getRelationship(ally->getDefaultTeam()) == ALLIES);
		if (!onMyTeam) continue;

		if (ally->getPlayerType() == PLAYER_COMPUTER && ally->isPlayerActive() && lowestAIIndex < 0)
		{
			lowestAIIndex = pi;
		}

		if (ally == m_player) continue;
		if (ally->getPlayerType() != PLAYER_HUMAN) continue;

		if (ally->getSurrenderConsented())
		{
			anyHumanConsented = true;
			if (!consentingHuman) consentingHuman = ally;
		}
		if (ally->isPlayerActive())
		{
			anyHumanActive = true;
		}
	}

	// Dedup consent/withdrawal chatter: only the lowest-index active AI on this
	// team prints the announcement. Every AI on the team still updates its own
	// latch so each one's state remains consistent.
	Bool iAmAnnouncer = (lowestAIIndex == m_player->getPlayerIndex());

	// Ally-only one-shot notice when the first consent latches (cosmetic; sim
	// state is identical across clients regardless of whether the message renders).
	if (anyHumanConsented && !m_surrenderConsentAnnounced)
	{
		m_surrenderConsentAnnounced = true;
		if (iAmAnnouncer && consentingHuman)
		{
			Player *localPlayer = ThePlayerList->getLocalPlayer();
			if (localPlayer)
			{
				Bool isAlly = (localPlayer == consentingHuman)
					|| (localPlayer->getRelationship(consentingHuman->getDefaultTeam()) == ALLIES);
				if (isAlly)
				{
					RGBColor rgb;
					rgb.setFromInt(consentingHuman->getPlayerColor());
					UnicodeString announcement;
					announcement.format(L"%ls has called for surrender. AI allies will give up once every human on the team is out.",
						consentingHuman->getPlayerDisplayName().str());
					TheInGameUI->messageColor(&rgb, announcement);
				}
			}
		}
	}
	// If consent is withdrawn before all humans are out, announce once and allow
	// the latch to fire again on a future "!surrender".
	if (!anyHumanConsented && m_surrenderConsentAnnounced)
	{
		m_surrenderConsentAnnounced = false;
		if (iAmAnnouncer)
		{
			Player *localPlayer = ThePlayerList->getLocalPlayer();
			if (localPlayer)
			{
				Bool isAlly = (localPlayer == m_player)
					|| (localPlayer->getRelationship(m_player->getDefaultTeam()) == ALLIES);
				if (isAlly)
				{
					TheInGameUI->message(L"Surrender call withdrawn.");
				}
			}
		}
	}

	if (!anyHumanConsented) return;
	if (anyHumanActive) return;

	// Trigger: kill this AI player. killPlayer() evacuates contains, kills units,
	// zeros money, and sets the dead flag, which the existing defeat detection
	// surfaces as a normal player elimination.
	m_hasSurrendered = true;

	Player *localPlayer = ThePlayerList->getLocalPlayer();
	if (localPlayer)
	{
		Bool isAlly = (localPlayer == m_player)
			|| (localPlayer->getRelationship(m_player->getDefaultTeam()) == ALLIES);
		if (isAlly)
		{
			RGBColor rgb;
			rgb.setFromInt(m_player->getPlayerColor());
			UnicodeString announcement;
			announcement.format(L"%ls has surrendered.", m_player->getPlayerDisplayName().str());
			TheInGameUI->messageColor(&rgb, announcement);
		}
	}

	m_player->killPlayer();
}

//----------------------------------------------------------------------------------------------------------
// Ally Communication helpers
//
// All TacticalAI announcements use TheInGameUI->messageColor() inside the
// deterministic simulation tick. Each client renders the same string at the
// same frame; replays show the same announcements; no chat/network traffic.
// Ally-only display is a cosmetic local-player relationship check on the
// rendering side. The Syn/Dan easter egg drops the ally check so every player
// sees it.
//----------------------------------------------------------------------------------------------------------

namespace {

// Map quadrant label for a world position. Used to give attack-commit and
// distress messages directional context ("base under attack — northeast!").
const WideChar *quadrantLabel(const Coord3D &pos)
{
	Region3D bounds;
	TheTerrainLogic->getMaximumPathfindExtent(&bounds);
	Real cx = bounds.lo.x + bounds.width()  * 0.5f;
	Real cy = bounds.lo.y + bounds.height() * 0.5f;
	Real dx = pos.x - cx;
	Real dy = pos.y - cy;
	Real edgeMargin = (bounds.width() < bounds.height() ? bounds.width() : bounds.height()) * 0.15f;
	if (fabsf(dx) < edgeMargin && fabsf(dy) < edgeMargin) return L"center";
	if (dy >= 0 && dx >= 0) return L"northeast";
	if (dy >= 0 && dx <  0) return L"northwest";
	if (dy <  0 && dx >= 0) return L"southeast";
	return L"southwest";
}

// True if `me` is the lowest-index TacticalAI player on `team` (the team being
// the relationship reference: typically the announcing AI's own default team).
// Used to dedup shared announcements when multiple TacticalAIs sit on one team.
Bool isLowestIndexTacticalAIOnTeam(const Player *me)
{
	Int playerCount = ThePlayerList->getPlayerCount();
	Int pi;
	for (pi = 0; pi < playerCount; ++pi)
	{
		Player *p = ThePlayerList->getNthPlayer(pi);
		if (!p || !p->isPlayerActive()) continue;
		if (p != me && me->getRelationship(p->getDefaultTeam()) != ALLIES) continue;
		if (!p->isTacticalAIPlayer()) continue;
		return p == me;
	}
	return false;
}

// Render an ally-only message: a local cosmetic gate on the rendering side.
void renderAllyMessage(const Player *speaker, const UnicodeString &text)
{
	Player *localPlayer = ThePlayerList->getLocalPlayer();
	if (!localPlayer) return;
	Bool ally = (localPlayer == speaker)
		|| (localPlayer->getRelationship(speaker->getDefaultTeam()) == ALLIES);
	if (!ally) return;
	RGBColor rgb;
	rgb.setFromInt(speaker->getPlayerColor());
	TheInGameUI->messageColor(&rgb, text);
}

// Render a message visible to every player.
void renderBroadcastMessage(const Player *speaker, const UnicodeString &text)
{
	if (!ThePlayerList->getLocalPlayer()) return;
	RGBColor rgb;
	rgb.setFromInt(speaker->getPlayerColor());
	TheInGameUI->messageColor(&rgb, text);
}

// Sum current HP and count of all of `p`'s living non-dozer non-harvester structures.
void sampleStructures(const Player *p, Real *outHP, Int *outCount)
{
	Real hp = 0.0f;
	Int count = 0;
	Player::PlayerTeamList::const_iterator pti;
	for (pti = p->getPlayerTeams()->begin(); pti != p->getPlayerTeams()->end(); ++pti)
	{
		DLINK_ITERATOR<Team> ti = (*pti)->iterate_TeamInstanceList();
		for (; !ti.done(); ti.advance())
		{
			Team *team = ti.cur();
			if (!team) continue;
			DLINK_ITERATOR<Object> oi = team->iterate_TeamMemberList();
			for (; !oi.done(); oi.advance())
			{
				Object *o = oi.cur();
				if (!o) continue;
				if (!o->isKindOf(KINDOF_STRUCTURE)) continue;
				if (o->isEffectivelyDead()) continue;
				BodyModuleInterface *body = o->getBodyModule();
				if (!body) continue;
				hp += body->getHealth();
				++count;
			}
		}
	}
	*outHP = hp;
	*outCount = count;
}

} // anonymous namespace

//----------------------------------------------------------------------------------------------------------
// Phase milestone announcements: at 5min ("MidGame") and 15min ("LateGame") of
// game time, the lowest-index TacticalAI on each team posts a one-shot ally-
// visible blurb. The 15min milestone also fires the Syn/Dan easter egg if this
// AI's current target's display name is "Syn" — that one is broadcast to all.
//----------------------------------------------------------------------------------------------------------
void AISkirmishPlayer::announceMilestones()
{
	if (!isTacticalAI()) return;

	const Int CHECK_INTERVAL_SECONDS = 1;
	const UnsignedInt MIDGAME_FRAME  = 5  * 60 * LOGICFRAMES_PER_SECOND;
	const UnsignedInt LATEGAME_FRAME = 15 * 60 * LOGICFRAMES_PER_SECOND;
	const Int FLAG_MIDGAME  = 1 << 0;
	const Int FLAG_LATEGAME = 1 << 1;

	UnsignedInt curFrame = TheGameLogic->getFrame();
	if (curFrame < m_nextMilestoneCheckFrame) return;
	m_nextMilestoneCheckFrame = curFrame + CHECK_INTERVAL_SECONDS * LOGICFRAMES_PER_SECOND;

	if (curFrame >= MIDGAME_FRAME && !(m_milestoneAnnouncedMask & FLAG_MIDGAME))
	{
		m_milestoneAnnouncedMask |= FLAG_MIDGAME;
		if (isLowestIndexTacticalAIOnTeam(m_player))
		{
			UnicodeString msg;
			msg.format(L"%ls: massing forces for the mid-game push.",
				m_player->getPlayerDisplayName().str());
			renderAllyMessage(m_player, msg);
		}
	}

	if (curFrame >= LATEGAME_FRAME && !(m_milestoneAnnouncedMask & FLAG_LATEGAME))
	{
		m_milestoneAnnouncedMask |= FLAG_LATEGAME;

		// Syn/Dan easter egg: each TacticalAI checks its own current target.
		// Per-player m_synAnnounced prevents repeat firing within one player.
		Player *enemy = getAiEnemy();
		if (enemy && !m_synAnnounced)
		{
			UnicodeString display = enemy->getPlayerDisplayName();
			if (display.compareNoCase(L"Syn") == 0)
			{
				m_synAnnounced = true;
				UnicodeString msg = UnicodeString(L"I'm coming for you Dan!");
				renderBroadcastMessage(m_player, msg);
			}
		}

		if (isLowestIndexTacticalAIOnTeam(m_player))
		{
			UnicodeString msg;
			msg.format(L"%ls: late-game offensive committed.",
				m_player->getPlayerDisplayName().str());
			renderAllyMessage(m_player, msg);
		}
	}
}

//----------------------------------------------------------------------------------------------------------
// Distress signal: poll structure HP every 2s into a 5-slot ring (10s window).
// Trigger when (a) total structure HP drops by at least DISTRESS_HP_DELTA over
// the window, or (b) the structure count drops by at least DISTRESS_COUNT_DROP
// inside the window. Earlier versions fired on any single structure loss, which
// produced false-positives for things like a Patriot Missile being picked off.
// Cooldown is 45s and bypasses other ally-message cooldowns.
//----------------------------------------------------------------------------------------------------------
void AISkirmishPlayer::processDistressSignal()
{
	if (!isTacticalAI()) return;

	const Int SAMPLE_INTERVAL_SECONDS    = 2;
	const Int DISTRESS_COOLDOWN_SECONDS  = 45;
	const Real DISTRESS_HP_DELTA         = 6000.0f;
	const Int DISTRESS_COUNT_DROP        = 2;

	UnsignedInt curFrame = TheGameLogic->getFrame();
	if (curFrame < m_nextDistressSampleFrame) return;
	m_nextDistressSampleFrame = curFrame + SAMPLE_INTERVAL_SECONDS * LOGICFRAMES_PER_SECOND;

	Real curHP = 0.0f;
	Int  curCount = 0;
	sampleStructures(m_player, &curHP, &curCount);

	// Compare against the oldest entry in the ring (5 samples * 2s = 10s window).
	Bool hasOldest = (m_distressRingFilled >= 5);
	Real oldestHP = hasOldest ? m_distressHPRing[m_distressRingHead] : 0.0f;
	Int oldestCount = hasOldest ? m_distressCountRing[m_distressRingHead] : 0;

	// Write current sample into the slot that just became the new "head + 5",
	// i.e. overwrite the oldest. Then advance head.
	m_distressHPRing[m_distressRingHead] = curHP;
	m_distressCountRing[m_distressRingHead] = curCount;
	m_distressRingHead = (m_distressRingHead + 1) % 5;
	if (m_distressRingFilled < 5) ++m_distressRingFilled;

	if (!hasOldest) return;
	if (curFrame < m_nextDistressAnnounceFrame) return;

	Bool structureLost = ((oldestCount - curCount) >= DISTRESS_COUNT_DROP);
	Bool heavyDamage   = ((oldestHP - curHP) >= DISTRESS_HP_DELTA);
	if (!structureLost && !heavyDamage) return;

	m_nextDistressAnnounceFrame = curFrame + DISTRESS_COOLDOWN_SECONDS * LOGICFRAMES_PER_SECOND;

	// Quadrant of the player's base center, as the rough hot zone.
	const WideChar *quad = quadrantLabel(m_baseCenter);
	UnicodeString msg;
	msg.format(L"%ls: under heavy attack at %ls!",
		m_player->getPlayerDisplayName().str(), quad);
	renderAllyMessage(m_player, msg);
}

//----------------------------------------------------------------------------------------------------------
// Attack-commit announcement: posted when commitIdleArmy() dispatches a force
// against an enemy structure. 60s per-AI cooldown so several short dispatches
// don't pile up. Beacon-directive dispatches are excluded by the caller; those
// are already announced via processBeaconDirective().
//----------------------------------------------------------------------------------------------------------
void AISkirmishPlayer::announceAttackCommit(const Coord3D *target, Player *enemyPlayer)
{
	if (!target || !enemyPlayer) return;

	const Int ATTACK_ANNOUNCE_COOLDOWN_SECONDS = 60;
	UnsignedInt curFrame = TheGameLogic->getFrame();
	if (curFrame < m_nextAttackAnnounceFrame) return;
	m_nextAttackAnnounceFrame = curFrame + ATTACK_ANNOUNCE_COOLDOWN_SECONDS * LOGICFRAMES_PER_SECOND;

	const WideChar *quad = quadrantLabel(*target);
	UnicodeString msg;
	msg.format(L"%ls: attacking %ls at %ls.",
		m_player->getPlayerDisplayName().str(),
		enemyPlayer->getPlayerDisplayName().str(),
		quad);
	renderAllyMessage(m_player, msg);
}

//----------------------------------------------------------------------------------------------------------
/**
 * Adjusts the build list to match the starting position.
 */
void AISkirmishPlayer::adjustBuildList(BuildListInfo *list)
{
	Bool foundStart = false;
	Coord3D startPos;

	// Find our command center location.
	Object *obj;
	for( obj = TheGameLogic->getFirstObject(); obj; obj = obj->getNextObject() )
	{

		Player *owner = obj->getControllingPlayer();
		if (owner==m_player) {
			// See if it's a command center.
			if (obj->isKindOf(KINDOF_COMMANDCENTER)) {
				foundStart = true;
				startPos = *obj->getPosition();
				m_player->onStructureUndone(obj);
				TheAI->pathfinder()->removeObjectFromPathfindMap(obj);
				TheGameLogic->destroyObject(obj);
				break;
			}
		}
	}
	if (!foundStart) {
		DEBUG_LOG(("Couldn't find starting command center for ai player."));
		return;
	}
	// Find the location of the command center in the build list.
	Bool foundInBuildList = false;
	Coord3D buildPos;
	BuildListInfo *cur = list;
	while (cur) {
		const ThingTemplate *tTemplate = TheThingFactory->findTemplate(cur->getTemplateName());
		if (tTemplate && tTemplate->isKindOf(KINDOF_COMMANDCENTER)) {
			foundInBuildList = true;
			buildPos = *cur->getLocation();
			cur->setInitiallyBuilt(true);
		}
		cur = cur->getNext();
	}
	Region3D bounds;
	TheTerrainLogic->getMaximumPathfindExtent(&bounds);
	/* calculate section of 3x3 grid:
		6 7 8
		3 4 5
		0 1 2 */

	Int gridIndex = 0;
	if (startPos.x > bounds.lo.x + bounds.width()/3) {
		gridIndex++;
	}
	if (startPos.x > bounds.lo.x + 2*bounds.width()/3) {
		gridIndex++;
	}

	if (startPos.y > bounds.lo.y + bounds.height()/3) {
		gridIndex+=3;
	}
	if (startPos.y > bounds.lo.y + 2*bounds.height()/3) {
		gridIndex+=3;
	}

	Real angle = 0;
	if (TheAI->getAiData()->m_rotateSkirmishBases) {
		switch (gridIndex) {
			case 0 : angle = 0; break;
			case 1 : angle = PI/4; break;// 45 degrees.
			case 2 : angle = PI/2; break; // 90 degrees;
			case 3 : angle = -PI/4; break; // -45 degrees.
			case 4 : angle = 0; break;
			case 5 : angle = 3*PI/4; break; // 135 degrees.
			case 6 : angle = -PI/2; break; // -90 degrees;
			case 7 : angle = -3*PI/4; break; // -135 degrees.
			case 8 : angle = PI; break; // 180 degrees.
		}
	}

	angle += 3*PI/4;

	Real s = sin(angle);
	Real c = cos(angle);

	cur = list;
	while (cur) {
		const ThingTemplate *tTemplate = TheThingFactory->findTemplate(list->getTemplateName());
		if (tTemplate && tTemplate->isKindOf(KINDOF_COMMANDCENTER)) {
			foundInBuildList = true;
			Coord3D curPos = *cur->getLocation();
			// Transform to new coords.
			curPos.x -= buildPos.x;
			curPos.y -= buildPos.y;
			Real newX = curPos.x*c - curPos.y*s;
			Real newY = curPos.y*c + curPos.x*s;
			curPos.x = newX + startPos.x;
			curPos.y = newY + startPos.y;
			cur->setLocation(curPos);
			cur->setAngle(cur->getAngle());
		}
		cur = cur->getNext();
	}

}



//----------------------------------------------------------------------------------------------------------
/**
 * Find any things that build stuff & add them to the build list.  Then build any initially built
 * buildings.
 */
void AISkirmishPlayer::newMap()
{

	/* Get our proper build list. */
	AsciiString mySide = m_player->getSide();
	DEBUG_LOG(("AI Player side is %s", mySide.str()));
	const AISideBuildList *build = TheAI->getAiData()->m_sideBuildLists;
	while (build) {
		if (build->m_side == mySide) {
			BuildListInfo *buildList = build->m_buildList->duplicate();
			adjustBuildList(buildList); // adjust to  our start position.
			m_player->setBuildList(buildList);
			computeCenterAndRadiusOfBase(&m_baseCenter, &m_baseRadius);
			break;
		}
		build = build->m_next;
	}
	DEBUG_ASSERTLOG(build!=nullptr, ("Couldn't find build list for skirmish player."));

	// Build any with the initially built flag.
	for( BuildListInfo *info = m_player->getBuildList(); info; info = info->getNext() )
	{
		AsciiString name = info->getTemplateName();
		if (name.isEmpty()) continue;
		const ThingTemplate *bldgPlan = TheThingFactory->findTemplate( name );
		if (!bldgPlan) {
			DEBUG_LOG(("*** ERROR - Build list building '%s' doesn't exist.", name.str()));
			continue;
		}
		if (info->isInitiallyBuilt()) {
			buildStructureNow(bldgPlan, info);
		} else {
			info->incrementNumRebuilds(); // the initial build in the normal build list consumes a rebuild, so add one.
		}
	}
}

//----------------------------------------------------------------------------------------------------------
/**
 * Queues up a dozer.
 */
void AISkirmishPlayer::queueDozer()
{
	AIPlayer::queueDozer();
}

//----------------------------------------------------------------------------------------------------------
/**
 * Finds a dozer that isn't building or collecting resources.
 */
Object * AISkirmishPlayer::findDozer( const Coord3D *pos )
{
	return AIPlayer::findDozer(pos);
}


//----------------------------------------------------------------------------------------------------------
/**
 * Find a good spot to fire a superweapon.
 */
Bool AISkirmishPlayer::computeSuperweaponTarget(const SpecialPowerTemplate *power, Coord3D *retPos, Int playerNdx, Real weaponRadius)
{

	Region2D bounds;
	getPlayerStructureBounds(&bounds, playerNdx);

	if( power->getSpecialPowerType() == SPECIAL_CLUSTER_MINES || power->getSpecialPowerType() == NUKE_SPECIAL_CLUSTER_MINES )
	{
		// hackus brutus - mine the entrances to our base.
		AsciiString pathLabel;
		Int mode = GameLogicRandomValue(0, 2);
		if (mode==1) {
				pathLabel.format("%s%d", SKIRMISH_FLANK, m_player->getMpStartIndex()+1);
		}	else if (mode==2) {
				pathLabel.format("%s%d", SKIRMISH_BACKDOOR, m_player->getMpStartIndex()+1);
		}	else {
			pathLabel.format("%s%d", SKIRMISH_CENTER, m_player->getMpStartIndex()+1);
		}

		Coord3D goalPos = m_baseCenter;
		Waypoint *way = TheTerrainLogic->getClosestWaypointOnPath( &goalPos, pathLabel );
		if (way) {
			goalPos = *way->getLocation();
		} else {
			Region2D bounds;
			getPlayerStructureBounds(&bounds, getMyEnemyPlayerIndex());
			goalPos.x = bounds.lo.x + bounds.width()/2;
			goalPos.y = bounds.lo.y + bounds.height()/2;
		}
		Coord2D offset;
		offset.x = goalPos.x-m_baseCenter.x;
		offset.y = goalPos.y-m_baseCenter.y;
		offset.normalize();
		offset.x *= m_baseRadius;
		offset.y *= m_baseRadius;
		*retPos = m_baseCenter;
		retPos->x += offset.x;
		retPos->y += offset.y;
		retPos->z = TheTerrainLogic->getGroundHeight(retPos->x, retPos->y);
		return TRUE;
	}

	return AIPlayer::computeSuperweaponTarget(power, retPos, playerNdx, weaponRadius);

}

// ------------------------------------------------------------------------------------------------
/** CRC */
// ------------------------------------------------------------------------------------------------
void AISkirmishPlayer::crc( Xfer *xfer )
{

}

// ------------------------------------------------------------------------------------------------
/** Xfer method
	* Version Info;
	* 1: Initial version
	* 2: Added m_nextIdleSweepFrame
	* 3: Added m_nextUpgradeSweepFrame
	* 4: Added m_nextLaserLockSweepFrame */
// ------------------------------------------------------------------------------------------------
void AISkirmishPlayer::xfer( Xfer *xfer )
{

	// version
	XferVersion currentVersion = 4;
	XferVersion version = currentVersion;
	xfer->xferVersion( &version, currentVersion );

	// xfer base class info
	AIPlayer::xfer( xfer );

	// front base defense
	xfer->xferInt( &m_curFrontBaseDefense );

	// flank base defense
	xfer->xferInt( &m_curFlankBaseDefense );

	// front left defense angle
	xfer->xferReal( &m_curFrontLeftDefenseAngle );

	// front right defense angle
	xfer->xferReal( &m_curFrontRightDefenseAngle );

	// left flank left defense angle
	xfer->xferReal( &m_curLeftFlankLeftDefenseAngle );

	// left flank right defense angle
	xfer->xferReal( &m_curLeftFlankRightDefenseAngle );

	// right flank left defense angle
	xfer->xferReal( &m_curRightFlankLeftDefenseAngle );

	// right flank right defense angle
	xfer->xferReal( &m_curRightFlankRightDefenseAngle );

	if (version >= 2)
	{
		xfer->xferUnsignedInt( &m_nextIdleSweepFrame );
	}

	if (version >= 3)
	{
		xfer->xferUnsignedInt( &m_nextUpgradeSweepFrame );
	}

	if (version >= 4)
	{
		xfer->xferUnsignedInt( &m_nextLaserLockSweepFrame );
	}

}

// ------------------------------------------------------------------------------------------------
/** Load post process */
// ------------------------------------------------------------------------------------------------
void AISkirmishPlayer::loadPostProcess()
{

}

