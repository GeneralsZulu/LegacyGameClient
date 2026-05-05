/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	Released under GPLv3+. See top-level LICENSE.
*/

// TacticalStrategies.cpp
// See TacticalStrategies.h for the rationale. Phase 1: registry + deterministic
// weighted picker + a single trivial logging strategy that proves the framework
// fires and survives save/load + replay round-trips.

#include "PreRTS.h"

#include <map>
#include <string.h>

#include "Common/DataChunk.h"
#include "Common/GameMemory.h"
#include "Common/GlobalData.h"
#include "Common/MapReaderWriterInfo.h"
#include "Common/NameKeyGenerator.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/Team.h"
#include "Common/ThingFactory.h"
#include "Common/ThingTemplate.h"
#include "GameClient/InGameUI.h"
#include "GameLogic/AISkirmishPlayer.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/LogicRandomValue.h"
#include "GameLogic/Module/AIUpdate.h"
#include "GameLogic/Module/DozerAIUpdate.h"
#include "GameLogic/Module/ProductionUpdate.h"
#include "GameLogic/Object.h"
#include "GameLogic/Scripts.h"
#include "GameLogic/ScriptEngine.h"
#include "GameLogic/SidesList.h"
#include "GameLogic/TacticalStrategies.h"

//-------------------------------------------------------------------------------------------------
// Loads a tactical strategy's .scb file and attaches its scripts (qualified
// to the AI player) to that player's SidesList ScriptList. Gracefully
// no-ops if the file is missing or unparseable so a strategy with a typoed
// path doesn't kill the AI — its m_apply (if any) still runs.
//
// Mirrors the pattern in GameLogic.cpp (around the MultiplayerScripts.scb
// load) — same DataChunk/CachedFileInputStream plumbing.
//-------------------------------------------------------------------------------------------------
static void attachStrategyScripts(AISkirmishPlayer *ai,
								  const AsciiString &path,
								  const AsciiString &templateName)
{
	if (!ai || !ai->getPlayer()) return;
	if (path.isEmpty()) return;

	CachedFileInputStream stream;
	if (!stream.open(path))
	{
		DEBUG_LOG(("**Tactical** Could not open strategy script: %s", path.str()));
		return;
	}

	ChunkInputStream *strm = &stream;
	DataChunkInput file(strm);
	file.registerParser("PlayerScriptsList", AsciiString::TheEmptyString,
		ScriptList::ParseScriptsDataChunk);
	if (!file.parse(nullptr))
	{
		DEBUG_LOG(("**Tactical** Could not parse strategy script: %s", path.str()));
		return;
	}

	ScriptList *scripts[MAX_PLAYER_COUNT];
	Int count = ScriptList::getReadScripts(scripts);

	Player *player = ai->getPlayer();
	SidesInfo *sideInfo = TheSidesList ? TheSidesList->getSideInfo(player->getPlayerIndex()) : nullptr;
	ScriptList *playerSL = sideInfo ? sideInfo->getScriptList() : nullptr;
	if (sideInfo && !playerSL)
	{
		// Player had no script list yet (rare — most skirmish players get one
		// from the side template). Make one so the appended scripts have a home.
		playerSL = newInstance(ScriptList);
		sideInfo->setScriptList(playerSL);
	}

	AsciiString playerName = TheNameKeyGenerator
		? TheNameKeyGenerator->keyToName(player->getPlayerNameKey())
		: AsciiString();
	AsciiString qualifier; // empty: name suffix not currently used by tactical scripts

	Int loadedScripts = 0;
	Int loadedGroups = 0;
	if (count > 0 && scripts[0] && playerSL)
	{
		Script *src = scripts[0]->getScript();
		while (src)
		{
			Script *dupe = src->duplicateAndQualify(qualifier, templateName, playerName);
			playerSL->addScript(dupe, 0);
			++loadedScripts;
			src = src->getNext();
		}
		ScriptGroup *grp = scripts[0]->getScriptGroup();
		while (grp)
		{
			ScriptGroup *dupe = grp->duplicateAndQualify(qualifier, templateName, playerName);
			playerSL->addGroup(dupe, 0);
			++loadedGroups;
			grp = grp->getNext();
		}
	}

	for (Int i = 0; i < count; ++i)
	{
		if (scripts[i]) deleteInstance(scripts[i]);
	}

	DEBUG_LOG(("**Tactical** Attached %d scripts + %d groups from %s to player %s",
		loadedScripts, loadedGroups, path.str(), playerName.str()));

	if (TheGlobalData && TheGlobalData->m_debugAITactical && TheInGameUI)
	{
		UnicodeString msg;
		msg.format(L"[Tactical %hs] Loaded %d scripts from %hs",
			player->getSide().str(), loadedScripts + loadedGroups, path.str());
		TheInGameUI->messageNoFormat(msg);
	}
}

//-------------------------------------------------------------------------------------------------
// In-game narration of strategy decisions. Gated on m_debugAITactical so the
// chatter only appears when an operator turns it on. Display-only; runs on
// every client identically because all clients share the INI flag.
//-------------------------------------------------------------------------------------------------
static void announceTactical(AISkirmishPlayer *ai, const char *text)
{
	if (!TheGlobalData || !TheGlobalData->m_debugAITactical) return;
	if (!TheInGameUI) return;
	if (!ai || !ai->getPlayer()) return;

	AsciiString side = ai->getPlayer()->getSide();
	UnicodeString msg;
	msg.format(L"[Tactical %hs] %hs", side.str(), text);
	TheInGameUI->messageNoFormat(msg);
}

//-------------------------------------------------------------------------------------------------
void TacticalStrategy::applyTo(AISkirmishPlayer *ai) const
{
	if (!ai) return;
	if (!m_scriptFile.isEmpty())
		attachStrategyScripts(ai, m_scriptFile, m_scriptTemplateName);
	if (m_apply)
		m_apply(ai);
}

//-------------------------------------------------------------------------------------------------
Bool TacticalStrategy::matches(const AsciiString &ownSide,
							   const AsciiString &enemySide,
							   const AsciiString &enemyTemplateName,
							   Int numPlayers) const
{
	if (!m_ownSide.isEmpty() && m_ownSide.compareNoCase(ownSide) != 0)
		return false;
	if (!m_enemySide.isEmpty() && m_enemySide.compareNoCase(enemySide) != 0)
		return false;
	if (!m_enemyTemplateExcludeContains.isEmpty() && !enemyTemplateName.isEmpty())
	{
		// Substring match against the PlayerTemplate name (e.g. "AirForce"
		// matches "FactionAmericaAirForceGeneral"). AsciiString lacks a
		// contains() helper so we walk the underlying char* with strstr.
		if (strstr(enemyTemplateName.str(), m_enemyTemplateExcludeContains.str()) != nullptr)
			return false;
	}
	if (numPlayers < m_minNumPlayers) return false;
	if (numPlayers > m_maxNumPlayers) return false;
	return true;
}

//-------------------------------------------------------------------------------------------------
TacticalStrategyStore *TacticalStrategyStore::getInstance()
{
	// Function-local static avoids the static-init-order pitfalls VC6 is
	// notoriously prone to. Population happens lazily on first use.
	static TacticalStrategyStore s;
	return &s;
}

//-------------------------------------------------------------------------------------------------
void TacticalStrategyStore::registerStrategy(const TacticalStrategy &s)
{
	m_strategies.push_back(s);
}

//-------------------------------------------------------------------------------------------------
const TacticalStrategy *TacticalStrategyStore::findByName(const AsciiString &name) const
{
	size_t i;
	for (i = 0; i < m_strategies.size(); ++i)
	{
		if (m_strategies[i].m_name == name)
			return &m_strategies[i];
	}
	return nullptr;
}

//-------------------------------------------------------------------------------------------------
AsciiString TacticalStrategyStore::pickForContext(TacticalPhase phase,
												  const AsciiString &ownSide,
												  const AsciiString &enemySide,
												  const AsciiString &enemyTemplateName,
												  Int numPlayers) const
{
	// Lazy init via const accessor — cast away const because population is
	// idempotent and only happens once across the program's lifetime.
	const_cast<TacticalStrategyStore *>(this)->ensureInitialized();

	Real totalWeight = 0.0f;
	size_t i;
	for (i = 0; i < m_strategies.size(); ++i)
	{
		const TacticalStrategy &s = m_strategies[i];
		if (s.m_phase != phase) continue;
		if (!s.matches(ownSide, enemySide, enemyTemplateName, numPlayers)) continue;
		if (s.m_weight <= 0.0f) continue;
		totalWeight += s.m_weight;
	}
	if (totalWeight <= 0.0f)
		return AsciiString::TheEmptyString;

	// GameLogicRandomValueReal is the deterministic lockstep RNG. Same call
	// sequence on every client => same pick.
	Real roll = GameLogicRandomValueReal(0.0f, totalWeight);
	Real running = 0.0f;
	for (i = 0; i < m_strategies.size(); ++i)
	{
		const TacticalStrategy &s = m_strategies[i];
		if (s.m_phase != phase) continue;
		if (!s.matches(ownSide, enemySide, enemyTemplateName, numPlayers)) continue;
		if (s.m_weight <= 0.0f) continue;
		running += s.m_weight;
		if (roll <= running)
			return s.m_name;
	}
	// Fall through (FP rounding); return the last eligible.
	for (i = m_strategies.size(); i > 0; --i)
	{
		const TacticalStrategy &s = m_strategies[i - 1];
		if (s.m_phase != phase) continue;
		if (!s.matches(ownSide, enemySide, enemyTemplateName, numPlayers)) continue;
		if (s.m_weight <= 0.0f) continue;
		return s.m_name;
	}
	return AsciiString::TheEmptyString;
}

//-------------------------------------------------------------------------------------------------
void TacticalStrategyStore::ensureInitialized()
{
	if (m_initialized) return;
	m_initialized = true;
	registerBuiltinStrategies();
}

//-------------------------------------------------------------------------------------------------
// USA Patriot Drop strategy — first real Tactical AI behavior.
//
// Implemented as a C++ state machine rather than scripts because (a) the
// chinook->dozer->patriot sequence references specific unit object IDs that
// scripts can't easily plumb, and (b) "build at arbitrary position" isn't
// in the script vocabulary. Per-player state lives in a static map keyed
// by player index — saves do not preserve mid-strategy state for now.
//
// Phases (named, not strictly sequential — failure aborts to DONE):
//   INIT           : announce, capture target position.
//   WAIT_PREREQS   : poll until power + barracks + supply exist.
//   BUILD_SQUAD    : (stub — relies on AI's normal team-build pipeline to
//                    train MDs and dozer; scans for them).
//   WAIT_SQUAD     : poll until 2 MDs + dozer + chinook all present.
//   LOAD_CHINOOK   : (stub) issue load orders.
//   MOVE_TO_DROP   : (stub) fly chinook to enemy supply centroid.
//   UNLOAD         : (stub) evacuate cargo.
//   BUILD_PATRIOT  : (stub) dozer constructs PatriotBattery at drop point.
//   DISBAND        : team disbands so commitIdleArmy() picks units up.
//   DONE           : strategy retires.
//
// "Stub" phases currently advance after a fixed timer + announcement so the
// state machine progression is observable end-to-end. Replacing each stub
// with real unit-command code is the follow-up.
//-------------------------------------------------------------------------------------------------
namespace
{
	enum PatriotDropPhase
	{
		PD_PHASE_INIT = 0,
		PD_PHASE_WAIT_PREREQS,
		PD_PHASE_BUILD_SQUAD,
		PD_PHASE_WAIT_SQUAD,
		PD_PHASE_LOAD_CHINOOK,
		PD_PHASE_MOVE_TO_DROP,
		PD_PHASE_UNLOAD,
		PD_PHASE_BUILD_PATRIOT,
		PD_PHASE_DISBAND,
		PD_PHASE_DONE,
		PD_PHASE_ABORTED
	};

	struct PatriotDropState
	{
		Int phase;
		Bool phaseInitialized;	///< false on phase entry, set true after issuing one-shot orders
		UnsignedInt phaseStartFrame;
		Coord3D dropTarget;
		ObjectID chinookID;
		ObjectID dozerID;
		ObjectID md1ID;
		ObjectID md2ID;
		PatriotDropState() : phase(PD_PHASE_INIT), phaseInitialized(false), phaseStartFrame(0),
			chinookID(INVALID_ID), dozerID(INVALID_ID), md1ID(INVALID_ID), md2ID(INVALID_ID)
		{
			dropTarget.zero();
		}
	};

	// Keyed by Player::getPlayerIndex(). Map rather than array so we don't
	// need to size for MAX_PLAYER_COUNT and can iterate sparsely.
	static std::map<Int, PatriotDropState> g_patriotDropState;
}

static PatriotDropState *patriotDropStateFor(AISkirmishPlayer *ai, Bool createIfMissing)
{
	if (!ai || !ai->getPlayer()) return nullptr;
	Int idx = ai->getPlayer()->getPlayerIndex();
	std::map<Int, PatriotDropState>::iterator it = g_patriotDropState.find(idx);
	if (it != g_patriotDropState.end()) return &it->second;
	if (!createIfMissing) return nullptr;
	g_patriotDropState[idx] = PatriotDropState();
	return &g_patriotDropState[idx];
}

static void patriotDropAdvance(PatriotDropState *st, Int newPhase)
{
	st->phase = newPhase;
	st->phaseInitialized = false;
	st->phaseStartFrame = TheGameLogic ? TheGameLogic->getFrame() : 0;
}

// Walk player teams looking for an alive (non-construction) object whose
// template matches the given name. Optional excludes for already-claimed IDs.
static Object *patriotDropFindOwned(Player *p, const char *templateName,
	ObjectID excludeA = INVALID_ID, ObjectID excludeB = INVALID_ID)
{
	if (!p || !templateName) return nullptr;
	const ThingTemplate *want = TheThingFactory->findTemplate(templateName);
	if (!want) return nullptr;
	Player::PlayerTeamList::const_iterator pti;
	for (pti = p->getPlayerTeams()->begin(); pti != p->getPlayerTeams()->end(); ++pti)
	{
		DLINK_ITERATOR<Team> ti = (*pti)->iterate_TeamInstanceList();
		for (; !ti.done(); ti.advance())
		{
			Team *t = ti.cur();
			if (!t) continue;
			DLINK_ITERATOR<Object> oi = t->iterate_TeamMemberList();
			for (; !oi.done(); oi.advance())
			{
				Object *o = oi.cur();
				if (!o) continue;
				if (o->isEffectivelyDead()) continue;
				if (o->getStatusBits().test(OBJECT_STATUS_UNDER_CONSTRUCTION)) continue;
				if (o->getID() == excludeA || o->getID() == excludeB) continue;
				if (o->getTemplate() && o->getTemplate()->isEquivalentTo(want))
					return o;
			}
		}
	}
	return nullptr;
}

static Object *lookupObjectByID(ObjectID id)
{
	if (id == INVALID_ID || !TheGameLogic) return nullptr;
	return TheGameLogic->findObjectByID(id);
}

// True if the unit is currently inside the given transport.
static Bool patriotDropIsContainedBy(ObjectID unitID, ObjectID transportID)
{
	Object *u = lookupObjectByID(unitID);
	Object *t = lookupObjectByID(transportID);
	if (!u || !t) return false;
	return u->getContainedBy() == t;
}

// True if the player owns at least one of the given templates that's not
// under construction. Walks team membership; cheap because there are
// typically <100 units on a player.
static Bool patriotDropPlayerHasBuilding(Player *p, const char *templateName)
{
	if (!p || !templateName) return false;
	const ThingTemplate *want = TheThingFactory->findTemplate(templateName);
	if (!want) return false;
	Player::PlayerTeamList::const_iterator pti;
	for (pti = p->getPlayerTeams()->begin(); pti != p->getPlayerTeams()->end(); ++pti)
	{
		DLINK_ITERATOR<Team> ti = (*pti)->iterate_TeamInstanceList();
		for (; !ti.done(); ti.advance())
		{
			Team *t = ti.cur();
			if (!t) continue;
			DLINK_ITERATOR<Object> oi = t->iterate_TeamMemberList();
			for (; !oi.done(); oi.advance())
			{
				Object *o = oi.cur();
				if (!o) continue;
				if (o->isEffectivelyDead()) continue;
				if (o->getStatusBits().test(OBJECT_STATUS_UNDER_CONSTRUCTION)) continue;
				if (o->getTemplate() && o->getTemplate()->isEquivalentTo(want))
					return true;
			}
		}
	}
	return false;
}

static void apply_USAPatriotDrop(AISkirmishPlayer *ai)
{
	announceTactical(ai, "Patriot drop: strategy active");
	PatriotDropState *st = patriotDropStateFor(ai, true);
	if (!st) return;
	patriotDropAdvance(st, PD_PHASE_INIT);
	// Capture drop target now (enemy structure centroid) so the goal is
	// stable even as the enemy's bounds shift mid-game.
	Player *enemy = ai->getAiEnemy();
	if (enemy)
	{
		Region2D bounds;
		Coord2D mean;
		AIPlayer::getPlayerStructureBounds(&bounds, enemy->getPlayerIndex(), FALSE, &mean);
		st->dropTarget.x = mean.x;
		st->dropTarget.y = mean.y;
		st->dropTarget.z = 0;
	}
}

static void tick_USAPatriotDrop(AISkirmishPlayer *ai)
{
	PatriotDropState *st = patriotDropStateFor(ai, false);
	if (!st) return;
	if (st->phase == PD_PHASE_DONE) return;

	UnsignedInt curFrame = TheGameLogic ? TheGameLogic->getFrame() : 0;
	UnsignedInt phaseAge = curFrame - st->phaseStartFrame;

	switch (st->phase)
	{
		case PD_PHASE_INIT:
			announceTactical(ai, "Patriot drop: waiting for prereqs (power+barracks+supply)");
			patriotDropAdvance(st, PD_PHASE_WAIT_PREREQS);
			break;

		case PD_PHASE_WAIT_PREREQS:
		{
			Player *p = ai->getPlayer();
			Bool havePower    = patriotDropPlayerHasBuilding(p, "AmericaPowerPlant");
			Bool haveBarracks = patriotDropPlayerHasBuilding(p, "AmericaBarracks");
			Bool haveSupply   = patriotDropPlayerHasBuilding(p, "AmericaSupplyCenter");
			if (havePower && haveBarracks && haveSupply)
			{
				announceTactical(ai, "Patriot drop: prereqs met, queueing squad");
				patriotDropAdvance(st, PD_PHASE_BUILD_SQUAD);
			}
			break;
		}

		case PD_PHASE_BUILD_SQUAD:
		{
			// Queue 2 Missile Defenders at the barracks. Dozer and Chinook
			// are expected to already exist (every faction starts with a
			// dozer; Chinooks are auto-spawned with the Supply Center).
			if (!st->phaseInitialized)
			{
				Player *p = ai->getPlayer();
				Object *barracks = patriotDropFindOwned(p, "AmericaBarracks");
				const ThingTemplate *mdT = TheThingFactory->findTemplate("AmericaInfantryMissileDefender");
				if (barracks && mdT)
				{
					ProductionUpdateInterface *pu = barracks->getProductionUpdateInterface();
					if (pu)
					{
						// queueCreateUnit takes a unique ProductionID — request one per unit.
						pu->queueCreateUnit(mdT, pu->requestUniqueUnitID());
						pu->queueCreateUnit(mdT, pu->requestUniqueUnitID());
						announceTactical(ai, "Patriot drop: queued 2 Missile Defenders");
					}
					else
					{
						announceTactical(ai, "Patriot drop: barracks has no production interface — abort");
						patriotDropAdvance(st, PD_PHASE_ABORTED);
						break;
					}
				}
				else
				{
					announceTactical(ai, "Patriot drop: missing barracks or MD template — abort");
					patriotDropAdvance(st, PD_PHASE_ABORTED);
					break;
				}
				st->phaseInitialized = true;
			}
			patriotDropAdvance(st, PD_PHASE_WAIT_SQUAD);
			break;
		}

		case PD_PHASE_WAIT_SQUAD:
		{
			Player *p = ai->getPlayer();
			// Look for: 2 distinct Missile Defenders, 1 Dozer, 1 Chinook, all
			// owned, alive, fully built, not yet claimed by us.
			Object *md1 = patriotDropFindOwned(p, "AmericaInfantryMissileDefender");
			Object *md2 = md1
				? patriotDropFindOwned(p, "AmericaInfantryMissileDefender", md1->getID())
				: nullptr;
			Object *dozer = patriotDropFindOwned(p, "AmericaVehicleDozer");
			Object *chinook = patriotDropFindOwned(p, "AmericaVehicleChinook");
			if (md1 && md2 && dozer && chinook)
			{
				st->md1ID = md1->getID();
				st->md2ID = md2->getID();
				st->dozerID = dozer->getID();
				st->chinookID = chinook->getID();
				announceTactical(ai, "Patriot drop: squad ready, loading chinook");
				patriotDropAdvance(st, PD_PHASE_LOAD_CHINOOK);
			}
			else if (phaseAge > 90 * LOGICFRAMES_PER_SECOND)
			{
				// 90s without a full squad — give up.
				announceTactical(ai, "Patriot drop: squad never assembled — abort");
				patriotDropAdvance(st, PD_PHASE_ABORTED);
			}
			break;
		}

		case PD_PHASE_LOAD_CHINOOK:
		{
			if (!st->phaseInitialized)
			{
				Object *chinook = lookupObjectByID(st->chinookID);
				Object *md1 = lookupObjectByID(st->md1ID);
				Object *md2 = lookupObjectByID(st->md2ID);
				Object *dozer = lookupObjectByID(st->dozerID);
				if (!chinook || !md1 || !md2 || !dozer)
				{
					announceTactical(ai, "Patriot drop: lost a unit before load — abort");
					patriotDropAdvance(st, PD_PHASE_ABORTED);
					break;
				}
				if (md1->getAI()) md1->getAI()->aiEnter(chinook, CMD_FROM_AI);
				if (md2->getAI()) md2->getAI()->aiEnter(chinook, CMD_FROM_AI);
				if (dozer->getAI()) dozer->getAI()->aiEnter(chinook, CMD_FROM_AI);
				announceTactical(ai, "Patriot drop: load orders issued");
				st->phaseInitialized = true;
			}
			// Poll: all three must report they're inside the chinook.
			if (patriotDropIsContainedBy(st->md1ID, st->chinookID)
				&& patriotDropIsContainedBy(st->md2ID, st->chinookID)
				&& patriotDropIsContainedBy(st->dozerID, st->chinookID))
			{
				announceTactical(ai, "Patriot drop: all loaded, en route to drop");
				patriotDropAdvance(st, PD_PHASE_MOVE_TO_DROP);
			}
			else if (phaseAge > 60 * LOGICFRAMES_PER_SECOND)
			{
				announceTactical(ai, "Patriot drop: load timed out — abort");
				patriotDropAdvance(st, PD_PHASE_ABORTED);
			}
			break;
		}

		case PD_PHASE_MOVE_TO_DROP:
		{
			Object *chinook = lookupObjectByID(st->chinookID);
			if (!chinook)
			{
				announceTactical(ai, "Patriot drop: chinook lost in transit — abort");
				patriotDropAdvance(st, PD_PHASE_ABORTED);
				break;
			}
			if (!st->phaseInitialized)
			{
				if (chinook->getAI())
					chinook->getAI()->aiMoveToPosition(&st->dropTarget, CMD_FROM_AI);
				AsciiString msg;
				msg.format("Patriot drop: moving to (%.0f, %.0f)",
					st->dropTarget.x, st->dropTarget.y);
				announceTactical(ai, msg.str());
				st->phaseInitialized = true;
			}
			// Poll arrival within ~150 game units of the drop target.
			Coord3D pos = *chinook->getPosition();
			Real dx = pos.x - st->dropTarget.x;
			Real dy = pos.y - st->dropTarget.y;
			Real distSq = dx * dx + dy * dy;
			const Real ARRIVAL_RADIUS_SQ = 150.0f * 150.0f;
			if (distSq <= ARRIVAL_RADIUS_SQ)
			{
				announceTactical(ai, "Patriot drop: arrived at drop, evacuating");
				patriotDropAdvance(st, PD_PHASE_UNLOAD);
			}
			else if (phaseAge > 90 * LOGICFRAMES_PER_SECOND)
			{
				announceTactical(ai, "Patriot drop: travel timeout — abort");
				patriotDropAdvance(st, PD_PHASE_ABORTED);
			}
			break;
		}

		case PD_PHASE_UNLOAD:
		{
			Object *chinook = lookupObjectByID(st->chinookID);
			if (!chinook)
			{
				announceTactical(ai, "Patriot drop: chinook lost during unload — abort");
				patriotDropAdvance(st, PD_PHASE_ABORTED);
				break;
			}
			if (!st->phaseInitialized)
			{
				if (chinook->getAI())
					chinook->getAI()->aiEvacuate(false, CMD_FROM_AI);
				announceTactical(ai, "Patriot drop: evacuate issued");
				st->phaseInitialized = true;
			}
			// Wait until at least the dozer is out — patriot construct needs it on the ground.
			Object *dozer = lookupObjectByID(st->dozerID);
			if (dozer && dozer->getContainedBy() != chinook)
			{
				announceTactical(ai, "Patriot drop: dozer on ground, building patriot");
				patriotDropAdvance(st, PD_PHASE_BUILD_PATRIOT);
			}
			else if (phaseAge > 30 * LOGICFRAMES_PER_SECOND)
			{
				announceTactical(ai, "Patriot drop: unload timeout — abort");
				patriotDropAdvance(st, PD_PHASE_ABORTED);
			}
			break;
		}

		case PD_PHASE_BUILD_PATRIOT:
		{
			Object *dozer = lookupObjectByID(st->dozerID);
			if (!dozer)
			{
				announceTactical(ai, "Patriot drop: dozer lost — abort");
				patriotDropAdvance(st, PD_PHASE_ABORTED);
				break;
			}
			if (!st->phaseInitialized)
			{
				const ThingTemplate *patriotT = TheThingFactory->findTemplate("AmericaPatriotBattery");
				DozerAIInterface *dozerAI = dozer->getAI() ? dozer->getAI()->getDozerAIInterface() : nullptr;
				if (patriotT && dozerAI)
				{
					Coord3D buildPos = *dozer->getPosition();
					dozerAI->construct(patriotT, &buildPos, 0.0f, ai->getPlayer(), false);
					announceTactical(ai, "Patriot drop: dozer constructing PatriotBattery");
				}
				else
				{
					announceTactical(ai, "Patriot drop: missing patriot template or dozer interface — abort");
					patriotDropAdvance(st, PD_PHASE_ABORTED);
					break;
				}
				st->phaseInitialized = true;
			}
			// 30s after issuing, declare done. The actual building progress is owned
			// by the dozer; commitIdleArmy() picks the surviving units up.
			if (phaseAge > 30 * LOGICFRAMES_PER_SECOND)
			{
				announceTactical(ai, "Patriot drop: build window closed");
				patriotDropAdvance(st, PD_PHASE_DISBAND);
			}
			break;
		}

		case PD_PHASE_DISBAND:
			// No explicit team to disband (we never formed a TeamPrototype-based
			// team — units stayed on the player's default team throughout).
			// Idle units fall through to commitIdleArmy() naturally.
			announceTactical(ai, "Patriot drop: complete");
			patriotDropAdvance(st, PD_PHASE_DONE);
			break;

		case PD_PHASE_ABORTED:
			// Settle into PD_PHASE_DONE so the tick early-returns going forward.
			// All units (or what's left of them) stay on the default team and
			// commitIdleArmy() will collect them on its next sweep.
			patriotDropAdvance(st, PD_PHASE_DONE);
			break;

		case PD_PHASE_DONE:
		default:
			break;
	}
}

//-------------------------------------------------------------------------------------------------
// Built-in test strategies. Phase-1 placeholders that announce themselves
// via the new tactical-debug message system but don't change AI behavior
// yet — phase 3 will replace each m_apply body with a real ScriptList.
// Faction-flavored entries exist alongside wildcard fallbacks so we can
// see condition filtering and weighted random pick both working.
//-------------------------------------------------------------------------------------------------

// --- Opening ---
static void apply_OpeningGeneric(AISkirmishPlayer *ai)
{
	announceTactical(ai, "Opening: standard build order");
}
static void apply_OpeningAmericaPatriotRush(AISkirmishPlayer *ai)
{
	announceTactical(ai, "Opening: USA patriot rush");
}
static void apply_OpeningChinaTankSpam(AISkirmishPlayer *ai)
{
	announceTactical(ai, "Opening: China tank spam");
}
static void apply_OpeningGLAStealthHarass(AISkirmishPlayer *ai)
{
	announceTactical(ai, "Opening: GLA stealth harass");
}

// --- Mid-game ---
static void apply_MidPushArmor(AISkirmishPlayer *ai)
{
	announceTactical(ai, "Mid-game: armored push");
}
static void apply_MidTurtleAndTech(AISkirmishPlayer *ai)
{
	announceTactical(ai, "Mid-game: turtle and tech");
}

// --- Late-game ---
static void apply_LateSuperweaponPush(AISkirmishPlayer *ai)
{
	announceTactical(ai, "Late-game: superweapon push");
}
static void apply_LateAttritionGrind(AISkirmishPlayer *ai)
{
	announceTactical(ai, "Late-game: attrition grind");
}

void TacticalStrategyStore::registerBuiltinStrategies()
{
	TacticalStrategy s;

	// Opening pool. Wildcard generic always matches; faction-specific entries
	// have higher weight so faction matchups bias toward themed picks.
	s = TacticalStrategy();
	s.m_name = "Opening_Generic";
	s.m_weight = 1.0f;
	s.m_phase = TACTICAL_PHASE_OPENING;
	s.m_apply = &apply_OpeningGeneric;
	registerStrategy(s);

	s = TacticalStrategy();
	s.m_name = "Opening_USA_PatriotRush";
	s.m_weight = 2.0f;
	s.m_phase = TACTICAL_PHASE_OPENING;
	s.m_ownSide = "America";
	s.m_apply = &apply_OpeningAmericaPatriotRush;
	registerStrategy(s);

	// Real Patriot Drop strategy: USA mirror (any general) but skip when
	// the enemy is Air Force, who wakes up Comanches early enough to
	// shred the chinook in transit. Heavy weight so it dominates other
	// USA opening picks during testing.
	s = TacticalStrategy();
	s.m_name = "Opening_USA_PatriotDrop";
	s.m_weight = 5.0f;
	s.m_phase = TACTICAL_PHASE_OPENING;
	s.m_ownSide = "America";
	s.m_enemySide = "America";
	s.m_enemyTemplateExcludeContains = "AirForce";
	s.m_apply = &apply_USAPatriotDrop;
	s.m_perFrameUpdate = &tick_USAPatriotDrop;
	registerStrategy(s);

	s = TacticalStrategy();
	s.m_name = "Opening_China_TankSpam";
	s.m_weight = 2.0f;
	s.m_phase = TACTICAL_PHASE_OPENING;
	s.m_ownSide = "China";
	s.m_apply = &apply_OpeningChinaTankSpam;
	registerStrategy(s);

	s = TacticalStrategy();
	s.m_name = "Opening_GLA_StealthHarass";
	s.m_weight = 2.0f;
	s.m_phase = TACTICAL_PHASE_OPENING;
	s.m_ownSide = "GLA";
	s.m_apply = &apply_OpeningGLAStealthHarass;
	registerStrategy(s);

	// Mid-game pool. Both wildcards; weighted so push is more common than turtle.
	s = TacticalStrategy();
	s.m_name = "Mid_PushArmor";
	s.m_weight = 1.0f;
	s.m_phase = TACTICAL_PHASE_MIDGAME;
	s.m_apply = &apply_MidPushArmor;
	registerStrategy(s);

	s = TacticalStrategy();
	s.m_name = "Mid_TurtleAndTech";
	s.m_weight = 0.5f;
	s.m_phase = TACTICAL_PHASE_MIDGAME;
	s.m_apply = &apply_MidTurtleAndTech;
	registerStrategy(s);

	// Late-game pool. Both wildcards.
	s = TacticalStrategy();
	s.m_name = "Late_SuperweaponPush";
	s.m_weight = 1.0f;
	s.m_phase = TACTICAL_PHASE_LATEGAME;
	s.m_apply = &apply_LateSuperweaponPush;
	registerStrategy(s);

	s = TacticalStrategy();
	s.m_name = "Late_AttritionGrind";
	s.m_weight = 1.0f;
	s.m_phase = TACTICAL_PHASE_LATEGAME;
	s.m_apply = &apply_LateAttritionGrind;
	registerStrategy(s);
}
