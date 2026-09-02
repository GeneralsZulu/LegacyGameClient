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

////////////////////////////////////////////////////////////////////////////////
//																																						//
//  (c) 2001-2003 Electronic Arts Inc.																				//
//																																						//
////////////////////////////////////////////////////////////////////////////////


#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "GameNetwork/networkutil.h"

// TheSuperHackers @tweak Mauller 26/08/2025 reduce the minimum runahead from 10
// This lets network games run at latencies down to 133ms when the network conditions allow
Int MIN_LOGIC_FRAMES = 5;
Int MAX_FRAMES_AHEAD = 128;
Int MIN_RUNAHEAD = 4;
Int FRAME_DATA_LENGTH = (MAX_FRAMES_AHEAD+1)*2;
Int FRAMES_TO_KEEP = (MAX_FRAMES_AHEAD/2) + 1;

// Rates pinned during resume-from-replay catchup, where throughput is
// (frames in flight) / (packet send interval) rather than anything to do with
// realtime. Run-ahead must stay <= MAX_FRAMES_AHEAD/2 (the negotiation clamp,
// and what FRAMES_TO_KEEP is sized for). The frame rate is not a rate anything
// actually runs at during catchup -- it only feeds the send-interval formula in
// Network::processRunAheadCommand, where these two give (1000*32)/(1000*2) = 16ms.
Int CATCHUP_RUNAHEAD = 32;
Int CATCHUP_FRAME_RATE = 1000;

// --- Lockstep resilience tuning ----------------------------------------------
// Every command that needs an ack used to be retried on a fixed 2s timer, so
// one lost packet carrying frame data froze the whole lockstep game for up to
// two seconds. Retries now follow a per-connection RTT estimate (TCP-style
// SRTT + 4*RTTVAR, clamped), and frame-info commands, which gate every frame
// and repeat-code down to a couple of bytes, ride in every packet until acked.
Bool NET_LEGACY_TIMING = FALSE;
Int NET_RETRY_MIN_MS = 60;
Int NET_RETRY_MAX_MS = 2000;      // the retail value; also the ceiling now
Int NET_RETRY_DEFAULT_MS = 300;   // until the first ack sample arrives

// The packet send interval is derived from run-ahead (Network::
// processRunAheadCommand: 1000*runAhead/frameRate/2, retail cap 500ms), a
// dial-up era bandwidth economy. It is also the floor on how fast a lost
// packet's frame info can be repeated, and every sample of measured latency
// carries up to one interval of batching wait at each end, so a large
// run-ahead makes a long interval makes a high latency makes a larger
// run-ahead (T1-LOSSY: 35 frames, 583ms interval, 928ms worst stall). Cap
// it. At ~100-300 bytes per packet 10 pps per link is nothing.
Int NET_SEND_INTERVAL_MAX_MS = 100;

// Run-ahead used to be sized from MEAN round-trip latency, which is blind to
// jitter: a link averaging 80ms with 300ms spikes got a 133ms window and
// stalled on every spike. Each client now reports a high percentile of its
// latency history (capped at mean + JITTER_CAP so one retry-inflated sample
// cannot buy a second of input lag), and the router only lowers run-ahead
// after the lower value has held for DECAY_TICKS metrics ticks (500ms each).
// Short on purpose: a map-load or disconnect stall inflates every latency
// sample taken during it, run-ahead jumps, and holding that for long is a
// second of input lag for the rest of the match (T1-HOUSE4 sat at 38
// frames for a whole soak with a slow 1-frame-per-10s decay).
Int NET_RUNAHEAD_LAT_PERCENTILE = 95;
Int NET_RUNAHEAD_JITTER_CAP_MS = 250;
Int NET_RUNAHEAD_DECAY_TICKS = 6;

// ReleaseLog telemetry: one NETSTAT line per interval while in-game, a
// NETSTALL line for any single wait past STALL_LOG_MS (rate limited), and a
// "hitch" counter for gaps between consecutive Network::update calls, which
// is the main thread not turning at all (blocked call, frozen machine) as
// opposed to a stall, which is the sim waiting on a peer's frame data.
Int NET_STATS_INTERVAL_MS = 10000;
Int NET_HITCH_MS = 200;
Int NET_STALL_LOG_MS = 500;

#ifdef DEBUG_LOGGING

void dumpBufferToLog(const void *vBuf, Int len, const char *fname, Int line)
{
	DEBUG_LOG(("======= dumpBufferToLog() %d bytes =======", len));
	DEBUG_LOG(("Source: %s:%d", fname, line));
	const char *buf = (const char *)vBuf;
	Int numLines = len / 8;
	if ((len % 8) != 0)
	{
		++numLines;
	}
	for (Int dumpindex = 0; dumpindex < numLines; ++dumpindex)
	{
		Int offset = dumpindex*8;
		DEBUG_LOG_RAW(("\t%5.5d\t", offset));
		Int dumpindex2;
		Int numBytesThisLine = min(8, len - offset);
		for (dumpindex2 = 0; dumpindex2 < numBytesThisLine; ++dumpindex2)
		{
			Int c = (buf[offset + dumpindex2] & 0xff);
			DEBUG_LOG_RAW(("%02X ", c));
		}
		for (; dumpindex2 < 8; ++dumpindex2)
		{
			DEBUG_LOG_RAW(("   "));
		}
		DEBUG_LOG_RAW((" | "));
		for (dumpindex2 = 0; dumpindex2 < numBytesThisLine; ++dumpindex2)
		{
			char c = buf[offset + dumpindex2];
			DEBUG_LOG_RAW(("%c", (isprint(c)?c:'.')));
		}
		DEBUG_LOG_RAW(("\n"));
	}
	DEBUG_LOG(("End of packet dump"));
}

#endif // DEBUG_LOGGING

/**
 * ResolveIP turns a string ("games2.westwood.com", or "192.168.0.1") into
 * a 32-bit unsigned integer.
 */
UnsignedInt ResolveIP(AsciiString host)
{
  struct hostent *hostStruct;
  struct in_addr *hostNode;

  if (host.isEmpty())
  {
	  DEBUG_LOG(("ResolveIP(): Can't resolve null"));
	  return 0;
  }

  // String such as "127.0.0.1"
  if (isdigit(host.getCharAt(0)))
  {
    return ( ntohl(inet_addr(host.str())) );
  }

  // String such as "localhost"
  hostStruct = gethostbyname(host.str());
  if (hostStruct == nullptr)
  {
	  DEBUG_LOG(("ResolveIP(): Can't resolve %s", host.str()));
	  return 0;
  }
  hostNode = (struct in_addr *) hostStruct->h_addr;
  return ( ntohl(hostNode->s_addr) );
}

/**
 * Returns the next network command ID.
 */
UnsignedShort GenerateNextCommandID() {
	static UnsignedShort commandID = 64000;
	++commandID;
	return commandID;
}

/**
 * Returns true if this type of command requires a unique command ID.
 */
Bool DoesCommandRequireACommandID(NetCommandType type) {
	if ((type == NETCOMMANDTYPE_GAMECOMMAND) ||
			(type == NETCOMMANDTYPE_FRAMEINFO) ||
			(type == NETCOMMANDTYPE_PLAYERLEAVE) ||
			(type == NETCOMMANDTYPE_DESTROYPLAYER) ||
			(type == NETCOMMANDTYPE_RUNAHEADMETRICS) ||
			(type == NETCOMMANDTYPE_RUNAHEAD) ||
			(type == NETCOMMANDTYPE_CHAT) ||
			(type == NETCOMMANDTYPE_DISCONNECTVOTE) ||
			(type == NETCOMMANDTYPE_LOADCOMPLETE) ||
			(type == NETCOMMANDTYPE_TIMEOUTSTART) ||
			(type == NETCOMMANDTYPE_WRAPPER) ||
			(type == NETCOMMANDTYPE_FILE) ||
			(type == NETCOMMANDTYPE_FILEANNOUNCE) ||
			(type == NETCOMMANDTYPE_FILEPROGRESS) ||
			(type == NETCOMMANDTYPE_DISCONNECTPLAYER) ||
			(type == NETCOMMANDTYPE_DISCONNECTFRAME) ||
			(type == NETCOMMANDTYPE_DISCONNECTSCREENOFF) ||
			(type == NETCOMMANDTYPE_FRAMERESENDREQUEST))
	{
		return TRUE;
	}
	return FALSE;
}

/**
 * Returns true if this type of network command requires an ack.
 */
Bool CommandRequiresAck(NetCommandMsg *msg) {
	if ((msg->getNetCommandType() == NETCOMMANDTYPE_GAMECOMMAND) ||
			(msg->getNetCommandType() == NETCOMMANDTYPE_FRAMEINFO) ||
			(msg->getNetCommandType() == NETCOMMANDTYPE_PLAYERLEAVE) ||
			(msg->getNetCommandType() == NETCOMMANDTYPE_DESTROYPLAYER) ||
			(msg->getNetCommandType() == NETCOMMANDTYPE_RUNAHEADMETRICS) ||
			(msg->getNetCommandType() == NETCOMMANDTYPE_RUNAHEAD) ||
			(msg->getNetCommandType() == NETCOMMANDTYPE_CHAT) ||
			(msg->getNetCommandType() == NETCOMMANDTYPE_DISCONNECTVOTE) ||
			(msg->getNetCommandType() == NETCOMMANDTYPE_DISCONNECTPLAYER) ||
			(msg->getNetCommandType() == NETCOMMANDTYPE_LOADCOMPLETE) ||
			(msg->getNetCommandType() == NETCOMMANDTYPE_TIMEOUTSTART) ||
			(msg->getNetCommandType() == NETCOMMANDTYPE_WRAPPER) ||
			(msg->getNetCommandType() == NETCOMMANDTYPE_FILE) ||
			(msg->getNetCommandType() == NETCOMMANDTYPE_FILEANNOUNCE) ||
			(msg->getNetCommandType() == NETCOMMANDTYPE_FILEPROGRESS) ||
			(msg->getNetCommandType() == NETCOMMANDTYPE_DISCONNECTPLAYER) ||
			(msg->getNetCommandType() == NETCOMMANDTYPE_DISCONNECTFRAME) ||
			(msg->getNetCommandType() == NETCOMMANDTYPE_DISCONNECTSCREENOFF) ||
			(msg->getNetCommandType() == NETCOMMANDTYPE_FRAMERESENDREQUEST))
	{
		return TRUE;
	}
	return FALSE;
}

Bool IsCommandSynchronized(NetCommandType type) {
	if ((type == NETCOMMANDTYPE_GAMECOMMAND) ||
			(type == NETCOMMANDTYPE_FRAMEINFO) ||
			(type == NETCOMMANDTYPE_PLAYERLEAVE) ||
			(type == NETCOMMANDTYPE_DESTROYPLAYER) ||
			(type == NETCOMMANDTYPE_RUNAHEAD))
	{
		return TRUE;
	}
	return FALSE;
}

/**
 * Returns true if this type of network command requires the ack to be sent directly to the player
 * rather than going through the packet router.  This should really only be used by commands
 * used on the disconnect screen.
 */
Bool CommandRequiresDirectSend(NetCommandMsg *msg) {
	if ((msg->getNetCommandType() == NETCOMMANDTYPE_DISCONNECTVOTE) ||
			(msg->getNetCommandType() == NETCOMMANDTYPE_DISCONNECTPLAYER) ||
			(msg->getNetCommandType() == NETCOMMANDTYPE_LOADCOMPLETE) ||
			(msg->getNetCommandType() == NETCOMMANDTYPE_TIMEOUTSTART) ||
			(msg->getNetCommandType() == NETCOMMANDTYPE_FILE) ||
			(msg->getNetCommandType() == NETCOMMANDTYPE_FILEANNOUNCE) ||
			(msg->getNetCommandType() == NETCOMMANDTYPE_FILEPROGRESS) ||
			(msg->getNetCommandType() == NETCOMMANDTYPE_DISCONNECTFRAME) ||
			(msg->getNetCommandType() == NETCOMMANDTYPE_DISCONNECTSCREENOFF) ||
			(msg->getNetCommandType() == NETCOMMANDTYPE_FRAMERESENDREQUEST)) {
		return TRUE;
	}
	return FALSE;
}

const char* GetNetCommandTypeAsString(NetCommandType type) {

	switch (type) {
	case NETCOMMANDTYPE_ACKBOTH:
		return "NETCOMMANDTYPE_ACKBOTH";
	case NETCOMMANDTYPE_ACKSTAGE1:
		return "NETCOMMANDTYPE_ACKSTAGE1";
	case NETCOMMANDTYPE_ACKSTAGE2:
		return "NETCOMMANDTYPE_ACKSTAGE2";
	case NETCOMMANDTYPE_FRAMEINFO:
		return "NETCOMMANDTYPE_FRAMEINFO";
	case NETCOMMANDTYPE_GAMECOMMAND:
		return "NETCOMMANDTYPE_GAMECOMMAND";
	case NETCOMMANDTYPE_PLAYERLEAVE:
		return "NETCOMMANDTYPE_PLAYERLEAVE";
	case NETCOMMANDTYPE_RUNAHEADMETRICS:
		return "NETCOMMANDTYPE_RUNAHEADMETRICS";
	case NETCOMMANDTYPE_RUNAHEAD:
		return "NETCOMMANDTYPE_RUNAHEAD";
	case NETCOMMANDTYPE_DESTROYPLAYER:
		return "NETCOMMANDTYPE_DESTROYPLAYER";
	case NETCOMMANDTYPE_KEEPALIVE:
		return "NETCOMMANDTYPE_KEEPALIVE";
	case NETCOMMANDTYPE_DISCONNECTCHAT:
		return "NETCOMMANDTYPE_DISCONNECTCHAT";
	case NETCOMMANDTYPE_CHAT:
		return "NETCOMMANDTYPE_CHAT";
	case NETCOMMANDTYPE_MANGLERQUERY:
		return "NETCOMMANDTYPE_MANGLERQUERY";
	case NETCOMMANDTYPE_MANGLERRESPONSE:
		return "NETCOMMANDTYPE_MANGLERRESPONSE";
	case NETCOMMANDTYPE_PROGRESS:
		return "NETCOMMANDTYPE_PROGRESS";
	case NETCOMMANDTYPE_LOADCOMPLETE:
		return "NETCOMMANDTYPE_LOADCOMPLETE";
	case NETCOMMANDTYPE_TIMEOUTSTART:
		return "NETCOMMANDTYPE_TIMEOUTSTART";
	case NETCOMMANDTYPE_WRAPPER:
		return "NETCOMMANDTYPE_WRAPPER";
	case NETCOMMANDTYPE_FILE:
		return "NETCOMMANDTYPE_FILE";
	case NETCOMMANDTYPE_FILEANNOUNCE:
		return "NETCOMMANDTYPE_FILEANNOUNCE";
	case NETCOMMANDTYPE_FILEPROGRESS:
		return "NETCOMMANDTYPE_FILEPROGRESS";
	case NETCOMMANDTYPE_DISCONNECTKEEPALIVE:
		return "NETCOMMANDTYPE_DISCONNECTKEEPALIVE";
	case NETCOMMANDTYPE_DISCONNECTPLAYER:
		return "NETCOMMANDTYPE_DISCONNECTPLAYER";
	case NETCOMMANDTYPE_PACKETROUTERQUERY:
		return "NETCOMMANDTYPE_PACKETROUTERQUERY";
	case NETCOMMANDTYPE_PACKETROUTERACK:
		return "NETCOMMANDTYPE_PACKETROUTERACK";
	case NETCOMMANDTYPE_DISCONNECTVOTE:
		return "NETCOMMANDTYPE_DISCONNECTVOTE";
	case NETCOMMANDTYPE_DISCONNECTFRAME:
		return "NETCOMMANDTYPE_DISCONNECTFRAME";
	case NETCOMMANDTYPE_DISCONNECTSCREENOFF:
		return "NETCOMMANDTYPE_DISCONNECTSCREENOFF";
	case NETCOMMANDTYPE_FRAMERESENDREQUEST:
		return "NETCOMMANDTYPE_FRAMERESENDREQUEST";
	default:
		DEBUG_CRASH(("Unknown NetCommandType in GetNetCommandTypeAsString"));
		return "UNKNOWN";
	}

}
