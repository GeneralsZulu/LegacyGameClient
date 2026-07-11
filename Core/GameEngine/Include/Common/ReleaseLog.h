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

// ReleaseLog.h
// -----------------------------------------------------------------------------
// A lightweight, always-compiled diagnostic log channel for release builds.
//
// Unlike DEBUG_LOG (which compiles out entirely in a release build), this
// channel is always present. It is deliberately quiet: only a curated set of
// match-lifecycle and desync-diagnostic lines are written, so a shipping build
// produces a small, uploadable log rather than the full debug firehose.
//
// Output goes to "ReleaseLog.txt" in the per-user data dir (same root as
// Replays\), fflush'd after every line so the file survives a crash. It is
// picked up by the end-of-match telemetry upload alongside ObserverLog.txt.
//
// Modelled on LANObsLog (GameNetwork/LANObserverStream.h).
// -----------------------------------------------------------------------------

#pragma once

#include "Lib/BaseType.h"

// Emit one timestamped line (printf-style, trailing newline added). Safe to
// call at any time; the log file is opened lazily on first use.
void ReleaseLog(const char *fmt, ...);

// Path/name of the release log file, for the telemetry uploader. Never null.
const char *ReleaseGetLogFileName();

// --- Tier 2: overall game-CRC ring buffer ----------------------------------
// Record the overall logic CRC for a frame. Kept in a fixed-size in-memory
// ring; nothing is written to disk until ReleaseLogDumpCRCHistory() is called.
// Cheap enough to call every CRC interval in a shipping build.
void ReleaseLogRecordFrameCRC(UnsignedInt frame, UnsignedInt crc);

// Clear the CRC history (call at match start).
void ReleaseLogResetCRCHistory();

// Flush the recorded CRC history to the release log. Call when a CRC mismatch /
// desync is detected so the last N frames of agreed-upon CRCs are captured. If
// per-subsystem capture is enabled (see below), those are dumped too.
void ReleaseLogDumpCRCHistory();

// --- Tier 3: per-subsystem CRC capture (enabled by default) -----------------
// When TheReleaseLogSubsystemCRCs is TRUE, per-subsystem CRC components are
// captured into a separate ring and dumped alongside the overall CRC history on
// a mismatch, pinpointing which subsystem diverged. Enabled by default: the
// capture is effectively free in release (getCRC runs only every ~100 frames,
// the reads are O(1), and the ring is in-memory - nothing hits disk until a
// desync dumps it). Set to FALSE to disable.
// `tag` must be a string literal or otherwise have static lifetime (only the
// pointer is stored, not a copy).
extern Bool TheReleaseLogSubsystemCRCs;
void ReleaseLogRecordSubsystemCRC(UnsignedInt frame, const char *tag, UnsignedInt crc);
