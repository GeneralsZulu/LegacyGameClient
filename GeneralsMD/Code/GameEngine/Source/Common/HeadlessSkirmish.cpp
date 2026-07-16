/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
*/

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/HeadlessSkirmish.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#include "Common/GameEngine.h"
#include "Common/GlobalData.h"
#include "Common/MessageStream.h"
#include "Common/PlayerTemplate.h"
#include "Common/RandomValue.h"
#include "GameClient/MapUtil.h"
#include "GameLogic/GameLogic.h"
#include "GameNetwork/GameInfo.h"

// =====================================================================
// Configuration stashed by the command-line parsers. Parsing of the slot
// specs is deferred to startIfRequested() so that ThePlayerTemplateStore
// (faction resolution) and TheMapCache (map validation) are initialized.
// =====================================================================

namespace
{
	Bool                     s_requested = FALSE;
	Bool                     s_active    = FALSE;
	Int                      s_simFPS    = DEFAULT_HEADLESS_SIM_FPS;
	std::vector<AsciiString> s_slotSpecs;

	// A parsed + resolved slot ready to be written into the GameInfo.
	struct HeadlessSlot
	{
		SlotState state;       // difficulty -> slot state
		Int       factionIdx;  // playerTemplate index, or PLAYERTEMPLATE_RANDOM
		Int       team;        // 0-based team number, or -1 for none
		Int       color;       // color index, or -1 for auto
	};

	// Sentinel returned by resolveFaction() for an unrecognized faction.
	const Int FACTION_ERROR = -1000;

	void fail(const char *msg)
	{
		printf("[HEADLESS SKIRMISH] ERROR: %s\n", msg);
		fflush(stdout);
		exit(1);
	}

	Bool parseDifficulty(const char *v, SlotState &out)
	{
		if (stricmp(v, "easy") == 0)                             { out = SLOT_EASY_AI;     return TRUE; }
		if (stricmp(v, "medium") == 0 || stricmp(v, "med") == 0) { out = SLOT_MED_AI;      return TRUE; }
		if (stricmp(v, "hard") == 0 || stricmp(v, "brutal") == 0){ out = SLOT_BRUTAL_AI;   return TRUE; }
		if (stricmp(v, "tactical") == 0 || stricmp(v, "tai") == 0){ out = SLOT_TACTICAL_AI; return TRUE; }
		return FALSE;
	}

	// Map a faction argument to a playerTemplate index. Accepts "random", a
	// BaseSide name (USA/China/GLA), or an exact template name
	// (e.g. FactionAmericaChooseAGeneral). Returns FACTION_ERROR if unknown.
	Int resolveFaction(const AsciiString &arg)
	{
		if (arg.isEmpty() || arg.compareNoCase("random") == 0)
			return PLAYERTEMPLATE_RANDOM;

		if (ThePlayerTemplateStore == nullptr)
			return FACTION_ERROR;

		const Int count = ThePlayerTemplateStore->getPlayerTemplateCount();
		Int i;

		// Prefer a match on BaseSide ("USA"/"China"/"GLA") or Side among the
		// playable factions (a playable faction has a non-empty starting
		// building; this excludes civilian / observer templates). Matching
		// BaseSide picks the generic faction (e.g. "USA" -> FactionAmerica);
		// matching Side lets a caller name a specific sub-general.
		for (i = 0; i < count; ++i)
		{
			const PlayerTemplate *pt = ThePlayerTemplateStore->getNthPlayerTemplate(i);
			if (pt == nullptr || pt->getStartingBuilding().isEmpty())
				continue;
			if (arg.compareNoCase(pt->getBaseSide()) == 0 || arg.compareNoCase(pt->getSide()) == 0)
				return i;
		}

		// Fall back to an exact template-name match (lets a caller pick a
		// specific sub-general).
		for (i = 0; i < count; ++i)
		{
			const PlayerTemplate *pt = ThePlayerTemplateStore->getNthPlayerTemplate(i);
			if (pt == nullptr)
				continue;
			if (arg.compareNoCase(pt->getName()) == 0)
				return i;
		}

		return FACTION_ERROR;
	}

	// Parse one "-slot" spec of comma-separated key=value pairs. Fills `out`
	// with defaults (random faction, hard difficulty, no team, auto color)
	// then applies overrides. Fails-fast (exit 1) on malformed input.
	void parseSlotSpec(const AsciiString &spec, HeadlessSlot &out)
	{
		AsciiString factionArg = "random";
		out.state      = SLOT_BRUTAL_AI; // "hard" default
		out.team       = -1;
		out.color      = -1;

		char buf[512];
		strncpy(buf, spec.str(), sizeof(buf) - 1);
		buf[sizeof(buf) - 1] = '\0';

		char *tok = strtok(buf, ",");
		while (tok != nullptr)
		{
			char *eq = strchr(tok, '=');
			if (eq == nullptr)
			{
				printf("[HEADLESS SKIRMISH] ERROR: slot token '%s' is not key=value (spec '%s')\n",
					tok, spec.str());
				fflush(stdout);
				exit(1);
			}
			*eq = '\0';
			const char *key = tok;
			const char *val = eq + 1;

			if (stricmp(key, "faction") == 0)
			{
				factionArg = val;
			}
			else if (stricmp(key, "difficulty") == 0 || stricmp(key, "diff") == 0)
			{
				if (!parseDifficulty(val, out.state))
				{
					printf("[HEADLESS SKIRMISH] ERROR: unknown difficulty '%s' (use easy|medium|hard|tactical)\n", val);
					fflush(stdout);
					exit(1);
				}
			}
			else if (stricmp(key, "team") == 0)
			{
				out.team = atoi(val);
			}
			else if (stricmp(key, "color") == 0)
			{
				out.color = atoi(val);
			}
			else
			{
				printf("[HEADLESS SKIRMISH] ERROR: unknown slot key '%s' (use faction|difficulty|team|color)\n", key);
				fflush(stdout);
				exit(1);
			}

			tok = strtok(nullptr, ",");
		}

		out.factionIdx = resolveFaction(factionArg);
		if (out.factionIdx == FACTION_ERROR)
		{
			printf("[HEADLESS SKIRMISH] ERROR: unknown faction '%s' (use USA|China|GLA|random or a template name)\n",
				factionArg.str());
			fflush(stdout);
			exit(1);
		}
	}
}

// =====================================================================
// Command-line entry points.
// =====================================================================

void HeadlessSkirmish_enable()                       { s_requested = TRUE; }
void HeadlessSkirmish_addSlotSpec(const char *spec)  { if (spec != nullptr) s_slotSpecs.push_back(AsciiString(spec)); }
void HeadlessSkirmish_setSimFPS(Int fps)             { if (fps > 0) s_simFPS = fps; }
Bool HeadlessSkirmish_isRequested()                  { return s_requested; }
Bool HeadlessSkirmish_isActive()                     { return s_active; }

// =====================================================================
// The launcher. Mirrors SkirmishGameOptionsMenu's init + reallyDoStart, but
// populates the slot list programmatically and designates the first slot as
// the (synthetic) local player. PlayerList::newGame promotes the first
// non-neutral player to a local human when no human slot exists, so an all-AI
// slot list runs headless with no human in any slot.
// =====================================================================

Bool HeadlessSkirmish_startIfRequested()
{
	if (!s_requested)
		return FALSE;

	// --- Validate slot count. ---
	if (s_slotSpecs.size() < 2)
		fail("need at least two -slot entries for an AI-vs-AI match");
	if ((Int)s_slotSpecs.size() > MAX_SLOTS)
		fail("too many -slot entries (exceeds MAX_SLOTS)");

	// --- Validate the map. ---
	AsciiString mapName = TheGlobalData->m_mapName;
	if (mapName.isEmpty())
		fail("no map specified (pass -map <name>)");
	const MapMetaData *md = (TheMapCache != nullptr) ? TheMapCache->findMap(mapName) : nullptr;
	if (md == nullptr)
	{
		printf("[HEADLESS SKIRMISH] ERROR: map '%s' not found in the map cache\n", mapName.str());
		fflush(stdout);
		exit(1);
	}
	if ((Int)s_slotSpecs.size() > md->m_numPlayers)
	{
		printf("[HEADLESS SKIRMISH] ERROR: %d slots requested but map '%s' supports at most %d players\n",
			(Int)s_slotSpecs.size(), mapName.str(), md->m_numPlayers);
		fflush(stdout);
		exit(1);
	}

	// --- Parse + resolve every slot BEFORE mutating any game state, so a bad
	//     config fails fast without leaving a half-built GameInfo. ---
	std::vector<HeadlessSlot> slots;
	size_t i;
	for (i = 0; i < s_slotSpecs.size(); ++i)
	{
		HeadlessSlot s;
		parseSlotSpec(s_slotSpecs[i], s);
		slots.push_back(s);
	}

	// --- Build TheSkirmishGameInfo (mirrors SkirmishGameOptionsMenu init). ---
	if (TheSkirmishGameInfo == nullptr)
		TheSkirmishGameInfo = NEW SkirmishGameInfo;
	else if (TheSkirmishGameInfo->isInGame())
		TheSkirmishGameInfo->endGame();

	TheSkirmishGameInfo->init();
	TheSkirmishGameInfo->clearSlotList();
	TheSkirmishGameInfo->reset();
	TheSkirmishGameInfo->enterGame();

	// --- Populate slots. Slot 0 gets a sentinel IP and is designated local so
	//     getLocalSlotNum() resolves; it stays an AI (promoted to a synthetic
	//     human by PlayerList::newGame). ---
	for (i = 0; i < slots.size(); ++i)
	{
		GameSlot gs;
		UnicodeString name;
		name.format(L"CPU%d", (Int)i);
		gs.setState(slots[i].state, name);
		gs.setPlayerTemplate(slots[i].factionIdx);
		if (slots[i].color >= 0)
			gs.setColor(slots[i].color);
		gs.setTeamNumber(slots[i].team);
		gs.setStartPos(-1); // -1 => assigned by populateRandomStartPosition
		if (i == 0)
			gs.setIP(1); // sentinel: makes this the local slot
		TheSkirmishGameInfo->setSlot((Int)i, gs);
	}
	TheSkirmishGameInfo->setLocalIP(1);
	TheSkirmishGameInfo->setMap(mapName);

	// --- Seed. Reproducible by default; -seed varies it for run diversity. ---
	const Int seed = (TheGlobalData->m_fixedSeed >= 0) ? TheGlobalData->m_fixedSeed : 0;
	TheSkirmishGameInfo->setSeed(seed);

	// --- Fire the start (mirrors reallyDoStart). ---
	TheWritableGlobalData->m_mapName    = TheSkirmishGameInfo->getMap();
	TheWritableGlobalData->m_shellMapOn = FALSE;
	TheWritableGlobalData->m_playIntro  = FALSE;
	TheWritableGlobalData->m_afterIntro = TRUE;

	if (TheGameLogic->isInGame())
		TheGameLogic->clearGameData(FALSE);

	TheSkirmishGameInfo->startGame(0);
	InitRandom(TheSkirmishGameInfo->getSeed());

	GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_NEW_GAME);
	msg->appendIntegerArgument(GAME_SKIRMISH);
	msg->appendIntegerArgument(DIFFICULTY_NORMAL); // per-slot difficulty drives AI; this arg is unused
	msg->appendIntegerArgument(0);
	msg->appendIntegerArgument(s_simFPS);          // frame pacing target (fast sim)

	s_active = TRUE;
	printf("[HEADLESS SKIRMISH] started: map='%s' slots=%d seed=%d simfps=%d\n",
		mapName.str(), (Int)slots.size(), seed, s_simFPS);
	fflush(stdout);
	return TRUE;
}
