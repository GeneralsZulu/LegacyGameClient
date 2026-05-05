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

#include "Common/DataChunk.h"
#include "Common/GameMemory.h"
#include "Common/GlobalData.h"
#include "Common/MapReaderWriterInfo.h"
#include "Common/NameKeyGenerator.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/Team.h"
#include "GameClient/InGameUI.h"
#include "GameLogic/AISkirmishPlayer.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/LogicRandomValue.h"
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
							   Int numPlayers) const
{
	if (!m_ownSide.isEmpty() && m_ownSide.compareNoCase(ownSide) != 0)
		return false;
	if (!m_enemySide.isEmpty() && m_enemySide.compareNoCase(enemySide) != 0)
		return false;
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
		if (!s.matches(ownSide, enemySide, numPlayers)) continue;
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
		if (!s.matches(ownSide, enemySide, numPlayers)) continue;
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
		if (!s.matches(ownSide, enemySide, numPlayers)) continue;
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
