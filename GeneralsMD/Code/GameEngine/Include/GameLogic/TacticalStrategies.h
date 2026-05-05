/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	Released under GPLv3+. See top-level LICENSE.
*/

// TacticalStrategies.h
// Phase-1 framework: a registry of named tactical "openings" / behavior packs
// that Tactical AI players (SLOT_TACTICAL_AI) can roll on. Each strategy
// declares the matchup conditions under which it is a valid choice. The
// picker filters by conditions, then weighted-random-selects via the
// deterministic lockstep RNG so all clients agree.
//
// In phase 1 the strategy body is a C++ function pointer (lets us validate
// the framework + replay versioning without authoring .scb files yet).
// Phase 2 will swap the body for a parsed ScriptList loaded from disk.

#pragma once

#include "Common/AsciiString.h"
#include "Common/STLTypedefs.h"

class AISkirmishPlayer;

enum TacticalPhase CPP_11(: Int)
{
	TACTICAL_PHASE_OPENING = 0,
	TACTICAL_PHASE_MIDGAME,
	TACTICAL_PHASE_LATEGAME,
	TACTICAL_PHASE_REACTIVE,

	TACTICAL_PHASE_COUNT
};

// Phase-1 strategy body. Receives the AI it is being applied to. Free to
// poke at the player's build list, queue teams, etc.
typedef void (*TacticalStrategyApplyFunc)(AISkirmishPlayer *ai);

class TacticalStrategy
{
public:
	TacticalStrategy()
		: m_weight(1.0f)
		, m_phase(TACTICAL_PHASE_OPENING)
		, m_minNumPlayers(0)
		, m_maxNumPlayers(0x7fffffff)
		, m_apply(nullptr)
	{}

	AsciiString					m_name;					///< unique identifier; logged & serialized
	Real						m_weight;				///< relative pick weight among candidates (>0)
	TacticalPhase				m_phase;				///< which slot this strategy fills
	AsciiString					m_ownSide;				///< empty = wildcard
	AsciiString					m_enemySide;			///< empty = wildcard
	Int							m_minNumPlayers;
	Int							m_maxNumPlayers;
	TacticalStrategyApplyFunc	m_apply;				///< invoked once at strategy assignment

	// True if every condition is satisfied by the given context.
	Bool matches(const AsciiString &ownSide, const AsciiString &enemySide, Int numPlayers) const;
};

// Singleton registry. Strategies are registered on first access (lazy init).
class TacticalStrategyStore
{
public:
	static TacticalStrategyStore *getInstance();

	void registerStrategy(const TacticalStrategy &s);

	// Returns the picked strategy's name, or an empty AsciiString if no
	// strategy in the given phase matches the context. Uses GameLogicRandomValue
	// internally so the pick is identical across lockstep clients.
	AsciiString pickForContext(TacticalPhase phase,
							   const AsciiString &ownSide,
							   const AsciiString &enemySide,
							   Int numPlayers) const;

	const TacticalStrategy *findByName(const AsciiString &name) const;

private:
	TacticalStrategyStore() : m_initialized(false) {}
	void ensureInitialized();
	void registerBuiltinStrategies();

	std::vector<TacticalStrategy> m_strategies;
	Bool m_initialized;
};
