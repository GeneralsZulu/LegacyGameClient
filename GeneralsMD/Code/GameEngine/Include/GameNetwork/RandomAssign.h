#pragma once

class GameInfo;

#include "Common/AsciiString.h"

#include <vector>

void performRandomAssign(GameInfo *game, const std::vector<Int> &lockedTemplates);
std::vector<Int> buildLockedTemplates();

// Source of randomness for assignStartPositionsGlobal. Returns a value in
// [lo, hi] inclusive. The lobby passes a rand()-based wrapper (host-only
// path); game start passes GameLogicRandomValue so every peer computes the
// same assignment.
typedef Int (*StartSpotRandomFunc)(Int lo, Int hi);

// Assign start positions to every occupied non-observer slot that doesn't
// have one, by enumerating all team-to-spot assignments and scoring the
// arrangement as a whole: maximize the smallest opposing-player distance,
// then minimize teammate spread, then pick randomly among near-ties.
// Distances are ground-path based when the map cache has them (rivers and
// cliffs separate spots that look adjacent), straight-line otherwise.
// Deliberately chosen spots are honored and score as fixed points.
void assignStartPositionsGlobal(GameInfo *game, StartSpotRandomFunc randFunc);

// If every occupied non-observer slot has team == -1, query the
// balance_teams API (TheGlobalData->m_balanceTeamsUrl) with the lobby's
// player names and assign team 0 / team 1 based on the first response.
// Returns true on success or no-op (teams already set, or fewer than two
// candidates). Returns false on API/network failure; *outError is then
// populated with a player-facing message suitable for chat. Slots are
// left untouched on failure.
bool tryBalanceTeamsViaApi(GameInfo *game, AsciiString *outError);
