/*
**	Command & Conquer Generals Zero Hour(tm)
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

#pragma once

#include "Common/AsciiString.h"

class Object;
class Player;
class DamageInfo;

/// Export game statistics as a gzipped JSON file alongside the replay file.
/// @param replayDir Directory containing replays (e.g. "[UserDataPath]/Replays/")
/// @param replayFileName Replay filename with extension (e.g. "LastReplay.rep")
/// @param uploadInline When TRUE (the default) the written file is also POSTed
///        to the cncstats stats endpoint synchronously, as before. Recorder
///        passes FALSE so the upload can be handed to the background telemetry
///        worker instead of blocking the main thread at end-of-match.
/// @return The path of the .gamestats.json.gz that was written, or an empty
///         AsciiString if nothing was written (collection inactive, missing
///         globals, or the file couldn't be opened).
AsciiString ExportGameStatsJSON(const AsciiString& replayDir, const AsciiString& replayFileName,
                                Bool uploadInline = TRUE);

/// Collect a time-series snapshot of all players' stats (called every game logic frame).
/// Snapshots are taken every 30 frames (~1 second) and stored in memory.
void StatsExporterCollectSnapshot();

/// Begin recording stats for a new replay. Activates recording and resets all stored data.
void StatsExporterBeginRecording();

/// Returns true between StatsExporterBeginRecording() and ExportGameStatsJSON().
/// Used by Recorder::stopRecording to decide whether to do the replay upload
/// (we only upload if we were collecting for this game — i.e., host of a
/// multiplayer/skirmish match).
bool StatsExporterIsActive();

/// Returns true if the current game has at least two human game-players.
/// Used by Recorder::stopRecording and ExportGameStatsJSON to gate the
/// end-of-game telemetry uploads (cncstats stats, radarvan replay, radarvan
/// map) so we don't ship data for solo skirmishes, campaign missions, or
/// replay-watch sessions where humans-vs-humans isn't actually happening.
bool StatsExporterHasMinHumansForUpload();

/// TRUE when the player has lost the ability to rebuild: no living dozer/worker
/// and no finished, unsold structure whose command set can produce one. Shared
/// with the observer UI so what viewers see and what the stats export records
/// can never disagree. Walks every object the player owns, so callers that run
/// per frame should cache the result rather than call it each time. Callers
/// must skip dead players themselves; elimination destroys their objects and
/// would otherwise read as "hunted".
Bool StatsExporterComputePlayerIsHunted(const Player *player);

/// Record a kill event with full context (called from Object::scoreTheKill).
void StatsExporterRecordKill(const Object *killer, const Object *victim, const DamageInfo *damageInfo);

/// Record a build event (called from Player::onUnitCreated / onStructureConstructionComplete).
void StatsExporterRecordBuild(const Object *producer, const Object *built);

/// Record a capture event (called from Object::onCapture).
void StatsExporterRecordCapture(const Object *captured, const Player *oldOwner, const Player *newOwner);
