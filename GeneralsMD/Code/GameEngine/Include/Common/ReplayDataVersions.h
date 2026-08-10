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

// FILE: ReplayDataVersions.h ///////////////////////////////////////////////
//
// TheSuperHackers @feature Which game data does a given replay need?
//
// A replay is a command log: playing it back re-runs the simulation, so the
// sim has to see the rules the recording client saw. Engine-side differences
// are handled by the replay determinism epochs (Recorder.h), but data-side
// ones cannot be: Zulu's own data lives in Zulu.big and has changed across
// releases, most sharply in 1.5.5, which folded in the community balance
// patch and restructured the whole object INI tree with it. Re-simulating a
// pre-1.5.5 replay against 1.5.5 data desyncs no matter what the engine does.
//
// So we ship every historical Zulu.big and mount the one that matches. The
// choice has to be made before the process starts -- ArchiveFileSystem::
// loadMods runs before any INI is parsed, and template pointers are handed
// out all over the engine afterwards, so there is no swapping it later. That
// is what ZuluLauncher's Replay Theater mode is for; this header just answers
// "which archive does this replay want", for the launcher to act on and for
// the in-game replay menu to warn about.
//
// The mapping itself ships as data (ReplayData\versions.csv in the user data
// dir, from installer/replay_data_versions.csv) so that adding a release does
// not need an engine change. ZuluLauncher parses the same file with its own
// tiny reader, since it deliberately links none of the engine.
//
///////////////////////////////////////////////////////////////////////////////

#pragma once

#ifndef __REPLAYDATAVERSIONS_H_
#define __REPLAYDATAVERSIONS_H_

#include "Common/AsciiString.h"

// The folder these archives live in is ARCHIVE_REPLAY_DATA_FOLDER, declared in
// Common/ArchiveFileSystem.h -- it has to be known down there too, so that the
// automatic *.big sweeps skip it.

class ReplayDataVersions
{
public:
	enum Resolution
	{
		RESOLVED_DEFAULT,   ///< this release's own Zulu.big; nothing to do
		RESOLVED_ARCHIVE,   ///< mount getArchive() instead
		RESOLVED_NONE,      ///< retail base data; run with no -mod at all
		RESOLVED_UNKNOWN,   ///< no row matched; we cannot promise fidelity
	};

	// Answer for one replay. 'versionString' and 'iniCRC' come straight from
	// its header. The version string is matched first and exactly; iniCRC is
	// the fallback that identifies dev and prerelease builds, which all report
	// a non-semantic version like "Version 1.04".
	static Resolution resolve(const AsciiString& versionString, UnsignedInt iniCRC,
		AsciiString& archiveOut);

	// True when this process is already running the data 'resolution' asks
	// for, i.e. the replay can be played right here.
	static Bool isCurrentlyMounted(Resolution resolution, const AsciiString& archive);
};

#endif // __REPLAYDATAVERSIONS_H_
