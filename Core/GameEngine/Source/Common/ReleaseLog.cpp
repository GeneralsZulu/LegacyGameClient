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

// ReleaseLog.cpp - see ReleaseLog.h for rationale.

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/ReleaseLog.h"

#include <stdio.h>
#include <stdarg.h>
#include <time.h>

// Log file name. Relative (next to the working dir / exe), matching the proven
// ObserverLog.txt approach so the telemetry uploader can pick it up by name.
static const char *const RELEASE_LOG_FILE_NAME = "ReleaseLog.txt";

// Opened lazily on first write, truncating once per app session so a single
// file accumulates every match played in that session. fflush after each line
// so it survives a crash.
static FILE *s_releaseLogFile = NULL;
static Bool  s_releaseLogOpenFailed = FALSE;

void ReleaseLog(const char *fmt, ...)
{
	if (s_releaseLogFile == NULL)
	{
		if (s_releaseLogOpenFailed)
			return;
		s_releaseLogFile = fopen(RELEASE_LOG_FILE_NAME, "w");
		if (s_releaseLogFile == NULL)
		{
			// Don't retry every call if the file can't be opened.
			s_releaseLogOpenFailed = TRUE;
			return;
		}
	}

	time_t now = time(NULL);
	struct tm *lt = localtime(&now);
	if (lt != NULL)
		fprintf(s_releaseLogFile, "[%02d:%02d:%02d] ", lt->tm_hour, lt->tm_min, lt->tm_sec);

	va_list ap;
	va_start(ap, fmt);
	vfprintf(s_releaseLogFile, fmt, ap);
	va_end(ap);

	fputc('\n', s_releaseLogFile);
	fflush(s_releaseLogFile);
}

const char *ReleaseGetLogFileName()
{
	return RELEASE_LOG_FILE_NAME;
}

// ---------------------------------------------------------------------------
// Tier 2: overall game-CRC ring buffer
// ---------------------------------------------------------------------------

// ~1024 samples. At a per-frame CRC interval and 30 fps that is ~34 seconds of
// history, which is plenty to see the run-up to a divergence. 8 bytes/entry.
static const Int RELEASE_CRC_RING_SIZE = 1024;

struct ReleaseFrameCRC
{
	UnsignedInt frame;
	UnsignedInt crc;
};

static ReleaseFrameCRC s_crcRing[RELEASE_CRC_RING_SIZE];
static Int s_crcRingNext  = 0;	// index of next slot to write
static Int s_crcRingCount = 0;	// total records written (may exceed ring size)

void ReleaseLogResetCRCHistory()
{
	s_crcRingNext  = 0;
	s_crcRingCount = 0;
}

void ReleaseLogRecordFrameCRC(UnsignedInt frame, UnsignedInt crc)
{
	s_crcRing[s_crcRingNext].frame = frame;
	s_crcRing[s_crcRingNext].crc   = crc;
	++s_crcRingNext;
	if (s_crcRingNext == RELEASE_CRC_RING_SIZE)
		s_crcRingNext = 0;
	++s_crcRingCount;
}

// ---------------------------------------------------------------------------
// Tier 3 hook (dormant by default): per-subsystem CRC components
// ---------------------------------------------------------------------------

// Enabled by default: per-subsystem CRC capture is effectively free in release
// (getCRC runs only every ~100 frames and the reads are O(1); the ring is
// in-memory and only spills to disk on a desync). See ReleaseLog.h.
Bool TheReleaseLogSubsystemCRCs = TRUE;

// Larger ring: ~6 components per frame, so this holds ~1300 frames when enabled.
// Only touched when the flag is on, so it costs nothing in a normal release run.
static const Int RELEASE_SUB_RING_SIZE = 8192;

struct ReleaseSubCRC
{
	UnsignedInt frame;
	const char *tag;	// static-lifetime string literal supplied by caller
	UnsignedInt crc;
};

static ReleaseSubCRC s_subRing[RELEASE_SUB_RING_SIZE];
static Int s_subRingNext  = 0;
static Int s_subRingCount = 0;

void ReleaseLogRecordSubsystemCRC(UnsignedInt frame, const char *tag, UnsignedInt crc)
{
	if (!TheReleaseLogSubsystemCRCs)
		return;
	s_subRing[s_subRingNext].frame = frame;
	s_subRing[s_subRingNext].tag   = tag;
	s_subRing[s_subRingNext].crc   = crc;
	++s_subRingNext;
	if (s_subRingNext == RELEASE_SUB_RING_SIZE)
		s_subRingNext = 0;
	++s_subRingCount;
}

void ReleaseLogDumpCRCHistory()
{
	// Overall CRC history, oldest first.
	Int have  = (s_crcRingCount < RELEASE_CRC_RING_SIZE) ? s_crcRingCount : RELEASE_CRC_RING_SIZE;
	Int start = (s_crcRingCount < RELEASE_CRC_RING_SIZE) ? 0 : s_crcRingNext;
	ReleaseLog("--- CRC history (last %d recorded CRCs) ---", have);
	Int k;
	for (k = 0; k < have; ++k)
	{
		const ReleaseFrameCRC &e = s_crcRing[(start + k) % RELEASE_CRC_RING_SIZE];
		ReleaseLog("  frame %d CRC=%8.8X", e.frame, e.crc);
	}

	// Per-subsystem history, only if the Tier-3 capture was enabled.
	if (TheReleaseLogSubsystemCRCs && s_subRingCount > 0)
	{
		Int subHave  = (s_subRingCount < RELEASE_SUB_RING_SIZE) ? s_subRingCount : RELEASE_SUB_RING_SIZE;
		Int subStart = (s_subRingCount < RELEASE_SUB_RING_SIZE) ? 0 : s_subRingNext;
		ReleaseLog("--- per-subsystem CRC history (last %d records) ---", subHave);
		for (k = 0; k < subHave; ++k)
		{
			const ReleaseSubCRC &e = s_subRing[(subStart + k) % RELEASE_SUB_RING_SIZE];
			ReleaseLog("  frame %d %s CRC=%8.8X", e.frame, e.tag ? e.tag : "(null)", e.crc);
		}
	}
}
