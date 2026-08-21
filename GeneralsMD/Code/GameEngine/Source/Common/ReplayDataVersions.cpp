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

// FILE: ReplayDataVersions.cpp /////////////////////////////////////////////
//
// Reader for ReplayData\versions.csv. See ReplayDataVersions.h for why the
// data a replay was recorded against has to be mounted at process start.
//
///////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"

#include "Common/ReplayDataVersions.h"
#include "Common/ArchiveFileSystem.h"
#include "Common/file.h"
#include "Common/FileSystem.h"
#include "Common/GlobalData.h"

// One parsed row. Only the three fields the runtime cares about; git_ref and
// note are build-time documentation.
struct ReplayDataRow
{
	AsciiString kind;
	AsciiString value;
	AsciiString bigFile;
};

// Copy [start, end) into an AsciiString and strip surrounding whitespace.
static AsciiString makeTrimmed(const char *start, const char *end)
{
	AsciiString out;
	if (end > start)
		out.set(start, (int)(end - start));
	out.trim();
	return out;
}

// Pull the first three comma-separated fields out of one line. Returns FALSE
// for blank lines, comments and the header row, so callers can just skip.
static Bool parseRow(const char *line, const char *lineEnd, ReplayDataRow& row)
{
	AsciiString field[3];
	Int fieldIndex = 0;
	const char *fieldStart = line;
	const char *p;

	for (p = line; p <= lineEnd; ++p)
	{
		if (p == lineEnd || *p == ',')
		{
			if (fieldIndex < 3)
				field[fieldIndex] = makeTrimmed(fieldStart, p);
			++fieldIndex;
			fieldStart = p + 1;
			if (fieldIndex >= 3 && p != lineEnd)
				break;   // the rest of the row is git_ref and note
		}
	}

	if (field[0].isEmpty() || field[0].getCharAt(0) == '#')
		return FALSE;
	if (field[0].compareNoCase("match_kind") == 0)
		return FALSE;
	if (field[2].isEmpty())
		return FALSE;

	row.kind = field[0];
	row.value = field[1];
	row.bigFile = field[2];
	return TRUE;
}

static UnsignedInt parseHex(const AsciiString& text)
{
	UnsignedInt value = 0;
	const char *p;
	for (p = text.str(); *p != '\0'; ++p)
	{
		const char c = *p;
		UnsignedInt digit;
		if (c >= '0' && c <= '9')      digit = (UnsignedInt)(c - '0');
		else if (c >= 'a' && c <= 'f') digit = (UnsignedInt)(c - 'a') + 10;
		else if (c >= 'A' && c <= 'F') digit = (UnsignedInt)(c - 'A') + 10;
		else return 0;
		value = (value << 4) | digit;
	}
	return value;
}

// The map is a few dozen short rows, so it is re-read whenever we need it
// rather than cached. Editing it then takes effect without a restart, which
// is convenient when adding a release.
//
// It lives in the user data directory, not next to the executable: we share
// the install directory with other clients (Generals Online mounts every .big
// it finds under there, recursively), so nothing of ours that looks like game
// data may sit in it. getPath_UserData() is absolute and ends in a backslash,
// so this does not depend on the working directory, which the save-game code
// moves around at runtime (GameState.cpp).
static AsciiString getVersionsCsvPath()
{
	AsciiString path = TheGlobalData->getPath_UserData();
	path.concat(ARCHIVE_REPLAY_DATA_FOLDER);
	path.concat("\\versions.csv");
	return path;
}

ReplayDataVersions::Resolution ReplayDataVersions::resolve(
	const AsciiString& versionString, UnsignedInt iniCRC, AsciiString& archiveOut)
{
	archiveOut.clear();

	File *file = TheFileSystem->openFile(getVersionsCsvPath().str(), File::READ | File::BINARY);
	if (file == nullptr)
	{
		DEBUG_LOG(("ReplayDataVersions - no %s; cannot map replay data versions",
			getVersionsCsvPath().str()));
		return RESOLVED_UNKNOWN;
	}

	const Int size = file->size();
	char *buffer = file->readEntireAndClose();   // exactly 'size' bytes, not terminated
	if (buffer == nullptr || size <= 0)
	{
		delete[] buffer;
		return RESOLVED_UNKNOWN;
	}

	// A version-string match always wins, but the rows are not ordered to
	// guarantee it is seen first, so remember any iniCRC hit as we go.
	AsciiString match;
	Bool haveVersionMatch = FALSE;
	AsciiString crcMatch;
	Bool haveCrcMatch = FALSE;

	const char *const end = buffer + size;
	const char *lineStart = buffer;
	const char *p;
	for (p = buffer; p <= end && !haveVersionMatch; ++p)
	{
		if (p != end && *p != '\n')
			continue;

		// Trim a trailing CR so CRLF files parse the same as LF ones.
		const char *lineEnd = p;
		if (lineEnd > lineStart && *(lineEnd - 1) == '\r')
			--lineEnd;

		ReplayDataRow row;
		if (parseRow(lineStart, lineEnd, row))
		{
			if (row.kind.compareNoCase("version") == 0
				&& row.value.compareNoCase(versionString) == 0)
			{
				match = row.bigFile;
				haveVersionMatch = TRUE;
			}
			else if (row.kind.compareNoCase("inicrc") == 0
				&& !haveCrcMatch && parseHex(row.value) == iniCRC)
			{
				crcMatch = row.bigFile;
				haveCrcMatch = TRUE;
			}
		}

		lineStart = p + 1;
		if (p == end)
			break;
	}
	delete[] buffer;

	if (!haveVersionMatch)
	{
		if (!haveCrcMatch)
			return RESOLVED_UNKNOWN;
		match = crcMatch;
	}

	if (match.compareNoCase("@default") == 0)
		return RESOLVED_DEFAULT;
	if (match.compareNoCase("@none") == 0)
		return RESOLVED_NONE;

	archiveOut = match;
	return RESOLVED_ARCHIVE;
}

Bool ReplayDataVersions::isCurrentlyMounted(Resolution resolution, const AsciiString& archive)
{
	// Note m_modBIG is the *resolved* path, not what was typed after -mod:
	// parseMod() prefixes a relative argument with the user data directory. So
	// test for the folder as a path component rather than as a prefix.
	const AsciiString& mounted = TheGlobalData->m_modBIG;
	const Bool inReplayData = isInReplayDataFolder(mounted);

	switch (resolution)
	{
		case RESOLVED_NONE:
			// Retail data means no override at all. Note that a normally
			// launched client auto-loads Zulu.big even without -mod, so this
			// is only ever true for a deliberately bare process.
			return mounted.isEmpty() && TheGlobalData->m_modDir.isEmpty();

		case RESOLVED_DEFAULT:
			// Whatever this build ships. True for any client that was not
			// pointed at one of the historical archives.
			return !inReplayData;

		case RESOLVED_ARCHIVE:
			return inReplayData && mounted.endsWithNoCase(archive);

		case RESOLVED_UNKNOWN:
		default:
			// Nothing to compare against; the caller decides what to say.
			return FALSE;
	}
}
