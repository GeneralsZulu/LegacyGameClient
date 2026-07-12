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
*/

// In-lobby map download hook.
//
// Engine-side (Core) callsites invoke TheMapDownloadHook from the LAN /
// GameSpy lobby callbacks when a peer sees a new map CRC it doesn't have
// locally. GeneralsMD installs an implementation at startup that fetches
// the .map plus any sidecars from the cncstats server, writes them under
// the user maps dir, validates the .map's CRC, and refreshes MapCache so
// the lobby preview picks the new map up without restarting the game.
//
// Default (nullptr): no download is attempted; the peer falls back to
// the legacy at-launch P2P file transfer in FileTransfer::DoAnyMapTransfers.
//
// Lives in Core so LANAPICallbacks / GameSpy callbacks can call it
// without dragging in GeneralsMD-only headers (StatsUploader, etc.),
// which the classic Generals build does not ship.

#pragma once

#include "Common/AsciiString.h"

// Signature: given the peer's locally-resolved real path for the map
// (post portableMapPathToRealMapPath), the CRC the host advertised, and
// the host's MapContentsMask bitfield (bit 2 = preview, 4 = ini, 8 = str,
// 16 = solo, 32 = assets, 64 = readme), download the .map plus the
// requested sidecars from the configured cncstats endpoint. Return true
// only on a CRC-verified .map write; sidecar failures don't gate true.
typedef Bool (*MapDownloadHookFn)(const AsciiString& localMapPath,
                                  UnsignedInt mapCRC,
                                  UnsignedInt contentsMask);

extern MapDownloadHookFn TheMapDownloadHook;

// Asynchronous form of the same download, for callsites that run on the
// main/network thread and must not block on WinINet (the lobby: a stalled
// download there freezes the UI and stops the LAN heartbeat, which gets the
// peer dropped from the game). The start hook hands the HTTP off to a worker
// and returns immediately; the poll hook is called every frame from the main
// thread and does the disk install + MapCache refresh once the bytes are in.
//
// Only one download may be in flight at a time: a second start while one is
// pending returns false.
enum MapDownloadStatus
{
	MAPDOWNLOAD_IDLE = 0,   ///< nothing in flight (and no result waiting)
	MAPDOWNLOAD_PENDING,    ///< worker still fetching
	MAPDOWNLOAD_INSTALLED,  ///< CRC-verified and written to disk; MapCache refreshed
	MAPDOWNLOAD_FAILED      ///< fetch, CRC check or install failed
};

// Same arguments as MapDownloadHookFn. Returns true when a worker was started.
typedef Bool (*MapDownloadStartHookFn)(const AsciiString& localMapPath,
                                       UnsignedInt mapCRC,
                                       UnsignedInt contentsMask);

// Main-thread only. Returns MAPDOWNLOAD_PENDING while the worker runs, then
// exactly once returns INSTALLED or FAILED (the result is consumed by that
// call and the state drops back to IDLE).
typedef MapDownloadStatus (*MapDownloadPollHookFn)(void);

extern MapDownloadStartHookFn TheMapDownloadStartHook;
extern MapDownloadPollHookFn  TheMapDownloadPollHook;
