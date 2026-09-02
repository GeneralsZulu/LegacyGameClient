/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
*/

// RelayRegistry.cpp -- see RelayRegistry.h for the design notes.

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "GameNetwork/RelayRegistry.h"
#include "Common/ReleaseLog.h"

#include <string.h>
#include <vector>

#ifdef _WIN32
#include <winsock.h>
#else
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

// How long we keep sending direct into silence before flipping the peer to
// relay. Must be comfortably shorter than the in-game disconnect timeout
// (60s) and the lobby join retry budget; long enough that a couple of lost
// packets on a healthy path never trigger it while we are receiving.
static const UnsignedInt RELAY_SILENCE_FLIP_MS = 4000;
// ... and how many packets we must have sent into that silence. In-game
// traffic is ~30/s so the time window dominates there (flip at ~4s), but
// lobby keepalives are one per 2s: a healthy mesh pair whose peers joined
// a few seconds apart would flip on pure timing skew without this (seen
// live in T1-BASELINE: the peer's first keepalive lands ~5-8s after ours
// because handoff times differ). Eight sparse sends means ~16s of genuine
// one-way silence before a lobby flip, which a healthy pair never shows.
static const Int RELAY_SILENCE_MIN_SENDS = 8;
// Long-stop tier: once a peer has been silent this long, flip after just a
// couple of sends regardless of RELAY_SILENCE_MIN_SENDS. When lockstep
// stalls on a dead pair the engine throttles its sends to that peer, so
// the 8-send gate alone healed a killed path in ~44s (measured), eating
// most of the 60s disconnect budget. 12s of true silence with sends
// outstanding is unambiguous; a peer that quiet on a working path is
// already on the disconnect screen.
//
// The long-stop applies ONLY to peers we have already heard from on the
// direct path (lastRecvMs != 0): it exists for a path that DIED. A peer
// never heard from is usually just still loading the map: T2-FULL8 on real
// cloud VMs auto-started seconds after the last join, everyone went quiet
// loading an 8-player map, and fast loaders long-stop-flipped half their
// healthy peers. Never-heard peers keep the strict 8-send tier, which
// load-progress packets reset long before it can fire.
static const UnsignedInt RELAY_SILENCE_LONGSTOP_MS = 12000;
static const Int RELAY_SILENCE_LONGSTOP_MIN_SENDS = 2;
// Map-load arming rule, learned across three cloud runs: while a match is
// loading, silence is normal (cloud VMs skew 10-40s), pre-game stash
// keepalives make every peer look "heard" already, and in-game send rates
// satisfy any send-count gate within seconds. So NEITHER tier may flip a
// peer that has not yet spoken in-game (received since the game transport
// was adopted), until this grace expires. After the grace, a still-silent
// peer is the genuinely-dead-from-start pair (the frame=8 class) and both
// tiers arm so it heals shortly after; a peer that spoke and then went
// quiet is a died path and heals at the 12s longstop regardless of grace.
static const UnsignedInt RELAY_SILENCE_GRACE_MS = 45000;
// How many post-adoption receipts make a peer count as "streaming in-game"
// (arming the silence tiers before the grace expires). Frame traffic hits
// this within a second; adoption-time keepalive races never do (T2-FULL8
// round 5: the host heard 1-2 stragglers from loading peers at adoption,
// armed the longstop, and flipped them at +28s, inside the grace).
static const Int RELAY_INGAME_RECV_ARM = 5;
// Direct-upgrade probing cadence: every PROBE_EVERY_MS an AUTO-flipped pair
// duplicates its sends onto the direct path for PROBE_WINDOW_MS. Receiving
// UNFLIP_RECVS direct packets while relayed proves the direct path works
// and unflips the receiver; at most UNFLIP_MAX upgrades per entry so a
// genuinely one-way path settles on the relay instead of flapping.
// Probe cadence and upgrade evidence are sized for REAL per-peer send
// rates: lockstep routes most traffic through the packet router, so a pair
// exchanges only ~1-2 packets/second directly. A 3s window per 10s yields
// 3-6 direct probes; with a 15s accumulation tolerance (longer than the
// probe period, so consecutive windows chain), 4 receipts arrive within
// one or two windows. The first cut (8 receipts, 1.5s windows, 5s
// tolerance) could mathematically never trigger at those rates.
static const UnsignedInt RELAY_PROBE_EVERY_MS  = 10000;
static const UnsignedInt RELAY_PROBE_WINDOW_MS = 3000;
static const UnsignedInt RELAY_BURST_GAP_MS    = 15000;
static const Int         RELAY_UNFLIP_RECVS    = 4;
static const Int         RELAY_UPGRADE_MAX     = 3;
// Upgrade dup window length, and how fresh direct inbound must be at its
// end to commit. During the window every packet rides BOTH paths, so a
// failed attempt costs nothing but duplicate datagrams.
static const UnsignedInt RELAY_UPGRADE_DUP_MS    = 5000;
static const UnsignedInt RELAY_UPGRADE_FRESH_MS  = 3000;

static const unsigned char RELAY_PURPOSE_DATA    = 2;
static const unsigned char RELAY_PURPOSE_DELIVER = 3;
static const Int           RELAY_TOKEN_BYTES     = 16;

struct RelayPeerEntry
{
	UnsignedInt   relayID;
	Int           channel;        // CHANNEL_LOBBY or CHANNEL_GAME
	UnsignedInt   ipHost;         // logical addr, host byte order
	UnsignedShort portHost;
	AsciiString   nick;
	Bool          sendViaRelay;
	// Silence trigger bookkeeping. firstSendMs is (re)set on the first send
	// after each received packet, so the measured window is always "how long
	// have we been sending without hearing anything"; sendsSinceRecv gates
	// the flip on actual send volume too (see RELAY_SILENCE_MIN_SENDS).
	UnsignedInt   lastRecvMs;     // 0 = never
	UnsignedInt   firstSendMs;    // 0 = not currently sending into silence
	Int           sendsSinceRecv;
	// Packets received from this peer since the game transport was adopted.
	// "Heard in-game" for silence-trigger arming requires several: exactly
	// one or two race packets (the tail of the peer's stash keepalives)
	// land right at adoption and must not count as an established stream.
	Int           recvsSinceAdopt;
	// Direct-path upgrade probing (only for AUTO flips, never grants): a
	// relayed pair periodically duplicates its sends onto the direct path;
	// sustained direct receipts from the peer unflip us, our direct stream
	// unflips them, and a spurious flip (load skew, transient stall) heals
	// back to direct no matter whose timing model was wrong.
	Bool          grantFlip;            // TRUE = flipped by relay_grant: never probe back
	UnsignedInt   probeWindowStartMs;   // current/last probe window start (0 = none yet)
	// Direct receipts counted in a rolling burst window (NOT contiguously:
	// probe packets interleave with the peer's ongoing relayed stream by
	// design, so a contiguity requirement can never be met).
	Int           directRecvBurst;
	UnsignedInt   lastDirectRecvMs;
	// Upgrade handshake: burst evidence opens a dup window (send BOTH paths,
	// sticky suppressed); at its end we commit to direct only if direct
	// inbound is still fresh, else revert to the relay. An instant unflip
	// flapped in the cloud lab: the peer's not-yet-upgraded relayed stream
	// sticky-flipped us back within the same second.
	UnsignedInt   dupUntilMs;           // 0 = no upgrade attempt in flight
	// Post-commit sticky holdoff: relay-path stragglers stay in flight for
	// an RTT after both sides go direct and must not re-flip us.
	UnsignedInt   stickyHoldoffUntilMs;
	Int           upgradeAttempts;      // capped; a one-way path settles on relay
};

static Bool          s_active = FALSE;
static UnsignedInt   s_coordIPHost = 0;
static UnsignedShort s_coordUdpPort = 0;
static UnsignedInt   s_magic = 0;
static unsigned char s_token[RELAY_TOKEN_BYTES];
static UnsignedInt   s_myRelayID = 0;
static UnsignedInt   s_gameAdoptedMs = 0;   // 0 = game transport not adopted yet
static std::vector<RelayPeerEntry> s_peers;

static RelayPeerEntry* findByAddr(UnsignedInt ipHost, UnsignedShort portHost)
{
	for (size_t i = 0; i < s_peers.size(); ++i)
	{
		if (s_peers[i].ipHost == ipHost && s_peers[i].portHost == portHost)
			return &s_peers[i];
	}
	return NULL;
}

static RelayPeerEntry* findByID(UnsignedInt relayID, Int channel)
{
	for (size_t i = 0; i < s_peers.size(); ++i)
	{
		if (s_peers[i].relayID == relayID && s_peers[i].channel == channel)
			return &s_peers[i];
	}
	return NULL;
}

static void logFlip(RelayPeerEntry* e, const char* why)
{
	ReleaseLog("Relay: %s traffic to %s (%u.%u.%u.%u:%u, id %u) now via coordinator (%s)",
		e->channel == RelayRegistry::CHANNEL_GAME ? "game" : "lobby",
		e->nick.isEmpty() ? "peer" : e->nick.str(),
		(e->ipHost >> 24) & 0xff, (e->ipHost >> 16) & 0xff,
		(e->ipHost >> 8) & 0xff, e->ipHost & 0xff,
		e->portHost, e->relayID, why);
}

void RelayRegistry::configure(UnsignedInt coordIPHost, UnsignedShort coordUdpPort,
	UnsignedInt magic, const unsigned char token[16], UnsignedInt myRelayID)
{
	s_peers.clear();
	s_coordIPHost  = coordIPHost;
	s_coordUdpPort = coordUdpPort;
	s_magic        = magic;
	memcpy(s_token, token, RELAY_TOKEN_BYTES);
	s_myRelayID    = myRelayID;
	s_active       = (myRelayID != 0 && coordIPHost != 0 && coordUdpPort != 0);
	DEBUG_LOG(("RelayRegistry: configured, myRelayID=%u active=%d", myRelayID, (Int)s_active));
}

void RelayRegistry::clear()
{
	s_peers.clear();
	s_active = FALSE;
	s_gameAdoptedMs = 0;
	s_myRelayID = 0;
	s_coordIPHost = 0;
	s_coordUdpPort = 0;
	s_magic = 0;
	memset(s_token, 0, sizeof(s_token));
}

Bool RelayRegistry::isActive()
{
	return s_active;
}

UnsignedInt RelayRegistry::myRelayID()
{
	return s_myRelayID;
}

void RelayRegistry::addPeer(UnsignedInt relayID, Int channel,
	UnsignedInt ipHost, UnsignedShort portHost, const AsciiString& nick)
{
	if (!s_active || relayID == 0 || ipHost == 0 || portHost == 0)
		return;
	RelayPeerEntry* e = findByID(relayID, channel);
	if (e != NULL)
	{
		// Join retry / re-delivered peer_info: refresh the address, keep the
		// flip state (a pair already proven relay-only must stay relayed).
		e->ipHost   = ipHost;
		e->portHost = portHost;
		e->nick     = nick;
		return;
	}
	// If another peer entry already claims this exact logical addr (which
	// should not happen outside address-collision oddities on symmetric
	// NATs), keep both entries but log it: lookups by addr return the first.
	if (findByAddr(ipHost, portHost) != NULL)
	{
		ReleaseLog("Relay: address collision registering id %u on %u.%u.%u.%u:%u",
			relayID, (ipHost >> 24) & 0xff, (ipHost >> 16) & 0xff,
			(ipHost >> 8) & 0xff, ipHost & 0xff, portHost);
	}
	RelayPeerEntry ne;
	ne.relayID      = relayID;
	ne.channel      = channel;
	ne.ipHost       = ipHost;
	ne.portHost     = portHost;
	ne.nick         = nick;
	ne.sendViaRelay = FALSE;
	ne.lastRecvMs   = 0;
	ne.firstSendMs  = 0;
	ne.sendsSinceRecv = 0;
	ne.recvsSinceAdopt = 0;
	ne.grantFlip = FALSE;
	ne.probeWindowStartMs = 0;
	ne.directRecvBurst = 0;
	ne.lastDirectRecvMs = 0;
	ne.dupUntilMs = 0;
	ne.stickyHoldoffUntilMs = 0;
	ne.upgradeAttempts = 0;
	s_peers.push_back(ne);
	DEBUG_LOG(("RelayRegistry: peer id=%u ch=%d %u.%u.%u.%u:%u (%s)",
		relayID, channel,
		(ipHost >> 24) & 0xff, (ipHost >> 16) & 0xff,
		(ipHost >> 8) & 0xff, ipHost & 0xff, portHost, nick.str()));
}

void RelayRegistry::rekeyPeer(UnsignedInt relayID, Int channel,
	UnsignedInt ipHost, UnsignedShort portHost)
{
	if (!s_active || ipHost == 0 || portHost == 0)
		return;
	RelayPeerEntry* e = findByID(relayID, channel);
	if (e == NULL)
		return;
	if (e->ipHost == ipHost && e->portHost == portHost)
		return;
	DEBUG_LOG(("RelayRegistry: rekey id=%u ch=%d -> %u.%u.%u.%u:%u",
		relayID, channel,
		(ipHost >> 24) & 0xff, (ipHost >> 16) & 0xff,
		(ipHost >> 8) & 0xff, ipHost & 0xff, portHost));
	e->ipHost   = ipHost;
	e->portHost = portHost;
	// The punch just delivered a packet from this addr.
	e->lastRecvMs  = timeGetTime();
	e->firstSendMs = 0;
}

void RelayRegistry::forceRelayChannel(UnsignedInt relayID, Int channel)
{
	RelayPeerEntry* e = findByID(relayID, channel);
	if (e == NULL)
		return;
	// A grant is authoritative (the punch failed): even if the entry was
	// already AUTO-flipped, mark it granted so probing never tries to
	// upgrade a pair that provably cannot punch.
	e->grantFlip = TRUE;
	if (e->sendViaRelay)
		return;
	e->sendViaRelay = TRUE;
	logFlip(e, "granted");
}

void RelayRegistry::forgetPeerByAddr(UnsignedInt ipHost, UnsignedShort portHost)
{
	if (!s_active)
		return;
	RelayPeerEntry* e = findByAddr(ipHost, portHost);
	if (e == NULL)
		return;
	const UnsignedInt relayID = e->relayID;
	AsciiString nick = e->nick;
	size_t i = 0;
	while (i < s_peers.size())
	{
		if (s_peers[i].relayID == relayID)
			s_peers.erase(s_peers.begin() + i);
		else
			++i;
	}
	ReleaseLog("Relay: forgot departed peer %s (id %u)",
		nick.isEmpty() ? "peer" : nick.str(), relayID);
}

void RelayRegistry::forceRelay(UnsignedInt relayID)
{
	forceRelayChannel(relayID, CHANNEL_LOBBY);
	forceRelayChannel(relayID, CHANNEL_GAME);
}

Bool RelayRegistry::hasPeer(UnsignedInt relayID)
{
	return (findByID(relayID, CHANNEL_LOBBY) != NULL ||
	        findByID(relayID, CHANNEL_GAME)  != NULL);
}

Bool RelayRegistry::anyPeerOnChannel(Int channel)
{
	if (!s_active)
		return FALSE;
	for (size_t i = 0; i < s_peers.size(); ++i)
	{
		if (s_peers[i].channel == channel)
			return TRUE;
	}
	return FALSE;
}

void RelayRegistry::countPeers(Int channel, Int* total, Int* relayed)
{
	Int t = 0;
	Int r = 0;
	for (size_t i = 0; i < s_peers.size(); ++i)
	{
		if (s_peers[i].channel != channel)
			continue;
		++t;
		if (s_peers[i].sendViaRelay)
			++r;
	}
	if (total)   *total   = t;
	if (relayed) *relayed = r;
}

Bool RelayRegistry::isOtherPeerAddr(UnsignedInt relayID, UnsignedInt ipHost, UnsignedShort portHost)
{
	for (size_t i = 0; i < s_peers.size(); ++i)
	{
		if (s_peers[i].relayID != relayID &&
		    s_peers[i].ipHost == ipHost && s_peers[i].portHost == portHost)
			return TRUE;
	}
	return FALSE;
}

Bool RelayRegistry::isRelayedAddr(UnsignedInt ipHost, UnsignedShort portHost)
{
	if (!s_active)
		return FALSE;
	RelayPeerEntry* e = findByAddr(ipHost, portHost);
	return (e != NULL && e->sendViaRelay);
}

// Shared tail: write the client relay header for (channel, destRelayID) and
// append payload (payloadLen may be 0 for keepalives).
static Bool buildDataFrame(Int channel, UnsignedInt destRelayID,
	const unsigned char* payload, Int payloadLen,
	unsigned char* outBuf, Int outCap, Int* outLen)
{
	Int need = RelayRegistry::RELAY_DATA_HEADER_SIZE + payloadLen;
	if (need > outCap)
		return FALSE;
	UnsignedInt magicBE = htonl(s_magic);
	memcpy(outBuf, &magicBE, 4);
	memcpy(outBuf + 4, s_token, RELAY_TOKEN_BYTES);
	outBuf[20] = RELAY_PURPOSE_DATA;
	outBuf[21] = (unsigned char)channel;
	UnsignedInt destBE = htonl(destRelayID);
	memcpy(outBuf + 22, &destBE, 4);
	if (payloadLen > 0)
		memcpy(outBuf + RelayRegistry::RELAY_DATA_HEADER_SIZE, payload, payloadLen);
	*outLen = need;
	return TRUE;
}

Bool RelayRegistry::wrapForSend(UnsignedInt ipHost, UnsignedShort portHost,
	const unsigned char* payload, Int payloadLen,
	unsigned char* outBuf, Int outCap, Int* outLen,
	UnsignedInt* outCoordIPHost, UnsignedShort* outCoordPort,
	Bool* alsoSendDirect)
{
	if (alsoSendDirect)
		*alsoSendDirect = FALSE;
	if (!s_active)
		return FALSE;
	RelayPeerEntry* e = findByAddr(ipHost, portHost);
	if (e == NULL)
		return FALSE;

	UnsignedInt nowMs = timeGetTime();
	if (!e->sendViaRelay)
	{
		// The silence trigger only applies to the GAME channel, where every
		// peer sends every frame so silence really means a dead path. Lobby
		// traffic is one-way by design (the host broadcasts, joiners mostly
		// listen), so a quiet lobby peer is normal; lobby channels flip only
		// via relay_grant or the sticky rule. Seen live in T1-BASELINE: the
		// host silence-flipped a perfectly healthy joiner's lobby channel.
		if (e->channel != CHANNEL_GAME)
			return FALSE;
		// Silence trigger: enough time AND enough sends with nothing back.
		if (e->firstSendMs == 0 || (e->lastRecvMs != 0 && e->lastRecvMs > e->firstSendMs))
			e->firstSendMs = nowMs;
		e->sendsSinceRecv++;
		UnsignedInt sinceMs = nowMs - ((e->lastRecvMs > e->firstSendMs) ? e->lastRecvMs : e->firstSendMs);
		Bool heardInGame = (s_gameAdoptedMs != 0 && e->recvsSinceAdopt >= RELAY_INGAME_RECV_ARM);
		Bool graceOver = (s_gameAdoptedMs != 0 && nowMs - s_gameAdoptedMs >= RELAY_SILENCE_GRACE_MS);
		if (!heardInGame && !graceOver)
			return FALSE;   // map-load window; see RELAY_SILENCE_GRACE_MS
		Bool flip = (sinceMs >= RELAY_SILENCE_FLIP_MS && e->sendsSinceRecv >= RELAY_SILENCE_MIN_SENDS) ||
		            (sinceMs >= RELAY_SILENCE_LONGSTOP_MS && e->sendsSinceRecv >= RELAY_SILENCE_LONGSTOP_MIN_SENDS);
		if (!flip)
			return FALSE;   // direct path still on probation; keep sending direct
		e->sendViaRelay = TRUE;
		logFlip(e, "silence trigger");
	}

	if (!buildDataFrame(e->channel, e->relayID, payload, payloadLen, outBuf, outCap, outLen))
		return FALSE;      // oversized; let it go direct rather than vanish
	*outCoordIPHost = s_coordIPHost;
	*outCoordPort   = s_coordUdpPort;

	// Upgrade handshake in flight: dup every packet onto the direct path,
	// and at window end commit (direct inbound still fresh) or revert.
	if (e->dupUntilMs != 0)
	{
		if (nowMs < e->dupUntilMs)
		{
			if (alsoSendDirect)
				*alsoSendDirect = TRUE;
			return TRUE;
		}
		e->dupUntilMs = 0;
		if (e->lastDirectRecvMs != 0 && nowMs - e->lastDirectRecvMs <= RELAY_UPGRADE_FRESH_MS)
		{
			e->sendViaRelay = FALSE;
			e->directRecvBurst = 0;
			e->probeWindowStartMs = 0;
			// Fresh silence bookkeeping (stale counters would re-flip on the
			// very next send) and a straggler holdoff for the sticky rule.
			e->firstSendMs = 0;
			e->sendsSinceRecv = 0;
			e->lastRecvMs = nowMs;
			e->stickyHoldoffUntilMs = nowMs + 2000;
			ReleaseLog("Relay: %s traffic to %s (id %u) back to DIRECT (upgrade %d/%d committed)",
				e->channel == CHANNEL_GAME ? "game" : "lobby",
				e->nick.isEmpty() ? "peer" : e->nick.str(),
				e->relayID, e->upgradeAttempts, RELAY_UPGRADE_MAX);
			// Fall through: this packet still goes via relay one last time
			// (harmless dup); the next send is direct.
		}
		else
		{
			ReleaseLog("Relay: upgrade attempt %d/%d to %s reverted (direct went quiet)",
				e->upgradeAttempts, RELAY_UPGRADE_MAX,
				e->nick.isEmpty() ? "peer" : e->nick.str());
		}
	}

	// Direct-upgrade probing (AUTO flips only, game channel only, capped):
	// inside a probe window the caller also sends the plain packet direct,
	// giving the peer the sustained direct receipts that open ITS window.
	if (alsoSendDirect && !*alsoSendDirect && !e->grantFlip &&
	    e->channel == CHANNEL_GAME && e->upgradeAttempts < RELAY_UPGRADE_MAX)
	{
		if (e->probeWindowStartMs == 0 ||
		    nowMs - e->probeWindowStartMs >= RELAY_PROBE_EVERY_MS)
			e->probeWindowStartMs = nowMs;
		if (nowMs - e->probeWindowStartMs < RELAY_PROBE_WINDOW_MS)
			*alsoSendDirect = TRUE;
	}
	return TRUE;
}

Bool RelayRegistry::wrapIfRelayed(UnsignedInt ipHost, UnsignedShort portHost,
	const unsigned char* payload, Int payloadLen,
	unsigned char* outBuf, Int outCap, Int* outLen,
	UnsignedInt* outCoordIPHost, UnsignedShort* outCoordPort)
{
	if (!s_active)
		return FALSE;
	RelayPeerEntry* e = findByAddr(ipHost, portHost);
	if (e == NULL || !e->sendViaRelay)
		return FALSE;
	if (!buildDataFrame(e->channel, e->relayID, payload, payloadLen, outBuf, outCap, outLen))
		return FALSE;
	*outCoordIPHost = s_coordIPHost;
	*outCoordPort   = s_coordUdpPort;
	return TRUE;
}

Bool RelayRegistry::handleIncoming(UnsignedInt srcIPHost, UnsignedShort srcPortHost,
	unsigned char* buf, Int len, Int* newLen,
	UnsignedInt* logicalIPHost, UnsignedShort* logicalPortHost, Bool* drop)
{
	if (!s_active)
		return FALSE;
	if (srcIPHost != s_coordIPHost || srcPortHost != s_coordUdpPort)
		return FALSE;

	// Everything from the coordinator's UDP port is ours to consume: STUN
	// keepalive replies (exactly 10 bytes), relay delivers, or junk.
	*drop = TRUE;
	if (len <= RELAY_DELIVER_HEADER_SIZE)
		return TRUE;
	UnsignedInt magicBE;
	memcpy(&magicBE, buf, 4);
	if (ntohl(magicBE) != s_magic || buf[4] != RELAY_PURPOSE_DELIVER)
		return TRUE;
	Int channel = (Int)buf[5];
	UnsignedInt srcIDBE;
	memcpy(&srcIDBE, buf + 6, 4);
	UnsignedInt srcID = ntohl(srcIDBE);
	RelayPeerEntry* e = findByID(srcID, channel);
	if (e == NULL)
		return TRUE;

	if (!e->sendViaRelay && e->dupUntilMs == 0 &&
	    (e->stickyHoldoffUntilMs == 0 || timeGetTime() >= e->stickyHoldoffUntilMs))
	{
		e->sendViaRelay = TRUE;
		logFlip(e, "sticky: peer reached us via relay");
	}
	e->lastRecvMs  = timeGetTime();
	e->firstSendMs = 0;
	e->sendsSinceRecv = 0;
	if (s_gameAdoptedMs != 0 && e->channel == CHANNEL_GAME)
		e->recvsSinceAdopt++;
	memmove(buf, buf + RELAY_DELIVER_HEADER_SIZE, len - RELAY_DELIVER_HEADER_SIZE);
	*newLen          = len - RELAY_DELIVER_HEADER_SIZE;
	*logicalIPHost   = e->ipHost;
	*logicalPortHost = e->portHost;
	*drop            = FALSE;
	return TRUE;
}

void RelayRegistry::noteGameTransportAdopted()
{
	s_gameAdoptedMs = timeGetTime();
	for (size_t i = 0; i < s_peers.size(); ++i)
		s_peers[i].recvsSinceAdopt = 0;
}

void RelayRegistry::noteDirectRecv(UnsignedInt ipHost, UnsignedShort portHost)
{
	if (!s_active)
		return;
	RelayPeerEntry* e = findByAddr(ipHost, portHost);
	if (e == NULL)
		return;
	e->lastRecvMs  = timeGetTime();
	e->firstSendMs = 0;
	e->sendsSinceRecv = 0;
	if (s_gameAdoptedMs != 0 && e->channel == CHANNEL_GAME)
		e->recvsSinceAdopt++;
	// Sustained direct traffic while we are relaying an AUTO flip proves
	// the peer's direct path to us works (they are probing, or never lost
	// it): open the upgrade dup window. Burst-windowed: receipts count
	// together when less than 5s apart, regardless of interleaved relayed
	// packets.
	if (e->sendViaRelay && !e->grantFlip && e->dupUntilMs == 0 &&
	    e->upgradeAttempts < RELAY_UPGRADE_MAX)
	{
		UnsignedInt nowMs = timeGetTime();
		if (e->lastDirectRecvMs == 0 || nowMs - e->lastDirectRecvMs > RELAY_BURST_GAP_MS)
			e->directRecvBurst = 1;
		else
			e->directRecvBurst++;
		e->lastDirectRecvMs = nowMs;
		if (e->directRecvBurst >= RELAY_UNFLIP_RECVS)
		{
			e->upgradeAttempts++;
			e->dupUntilMs = nowMs + RELAY_UPGRADE_DUP_MS;
			e->directRecvBurst = 0;
			DEBUG_LOG(("RelayRegistry: upgrade attempt %d to id %u (dup window)",
				e->upgradeAttempts, e->relayID));
		}
	}
	else if (e->sendViaRelay)
	{
		e->lastDirectRecvMs = timeGetTime();
	}
}

Bool RelayRegistry::buildKeepalive(Int channel,
	unsigned char* outBuf, Int outCap, Int* outLen,
	UnsignedInt* outCoordIPHost, UnsignedShort* outCoordPort)
{
	if (!s_active)
		return FALSE;
	if (!buildDataFrame(channel, 0, NULL, 0, outBuf, outCap, outLen))
		return FALSE;
	*outCoordIPHost = s_coordIPHost;
	*outCoordPort   = s_coordUdpPort;
	return TRUE;
}
