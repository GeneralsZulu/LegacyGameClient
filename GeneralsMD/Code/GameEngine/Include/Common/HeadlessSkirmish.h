/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
*/

#pragma once

// HeadlessSkirmish: start a fresh AI-vs-AI skirmish match directly from the
// command line, no GUI and no human slot, for headless batch/CI runs and for
// generating self-play data.
//
// Activation: command-line flags (parsed in CommandLine.cpp)
//   -skirmish                 enable the headless skirmish runner
//   -map <name>               map to play (reuses the existing -map flag)
//   -slot <spec>              add one AI slot; repeat once per player, in order.
//                             <spec> is comma-separated key=value pairs:
//                               faction=USA|China|GLA|random|<TemplateName>
//                               difficulty=easy|medium|hard|tactical  (default hard)
//                               team=<0-based number, or -1 for none>  (default -1)
//                               color=<index, or -1 for auto>          (default -1)
//   -seed <n>                 fixed logic seed (reuses -seed; default 0 => reproducible)
//   -simfps <n>               logic-frame pacing target; higher = faster sim
//                             (default DEFAULT_HEADLESS_SIM_FPS)
//
// Example:
//   generalszh.exe -headless -skirmish -map "Tournament Desert" \
//     -slot faction=USA,difficulty=hard -slot faction=China,difficulty=hard
//
// The match records automatically (a .rep drops in the replay dir) and
// VictoryConditions emits a [HEADLESS RESULT] line on game-over, after which
// the runner quits the process. A bad map / faction / too-many-slots config
// prints a diagnostic and exits nonzero (fail-fast, for batch use).

#include "Lib/BaseType.h"

// Default render-fps limit for the headless sim, which (with logic time-scale
// disabled) also paces how fast logic ticks. The engine's MSG_NEW_GAME handler
// clamps this to [1,1000], so 1000 is the practical maximum and yields roughly
// 30x real-time (CPU-bound) headless simulation.
enum { DEFAULT_HEADLESS_SIM_FPS = 1000 };

// --- Configuration, stashed by CommandLine.cpp before the game starts. ---
// Parsing/validation is deferred to startIfRequested() so that TheMapCache and
// ThePlayerTemplateStore are initialized when factions/map are resolved.
void  HeadlessSkirmish_enable();                       ///< -skirmish
void  HeadlessSkirmish_addSlotSpec(const char *spec);  ///< -slot <spec>
void  HeadlessSkirmish_setSimFPS(Int fps);             ///< -simfps <n>

Bool  HeadlessSkirmish_isRequested();  ///< TRUE if -skirmish was passed
Bool  HeadlessSkirmish_isActive();     ///< TRUE once a match has actually been started

// Build TheSkirmishGameInfo from the stashed config and append MSG_NEW_GAME.
// Called once from GameEngine::init(). Returns TRUE if a headless skirmish was
// started; FALSE (no-op) if -skirmish was not requested. On any config error,
// prints a diagnostic and calls exit(1).
Bool  HeadlessSkirmish_startIfRequested();
