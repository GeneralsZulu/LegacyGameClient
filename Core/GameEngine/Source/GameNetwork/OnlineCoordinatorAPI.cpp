/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
*/

#include "PreRTS.h"
#include "GameNetwork/OnlineCoordinatorAPI.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#ifdef _WIN32
#include <winsock.h>
#define CLOSE_SOCKET(fd)    ::closesocket(fd)
#define SOCK_ERR_LAST       WSAGetLastError()
#define SOCK_ERR_WOULDBLOCK WSAEWOULDBLOCK
#define SOCK_ERR_INPROGRESS WSAEINPROGRESS
#define SOCK_ERR_ALREADY    WSAEALREADY
#define SOCK_ERR_ISCONN     WSAEISCONN
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#define CLOSE_SOCKET(fd)    ::close(fd)
#define SOCK_ERR_LAST       errno
#define SOCK_ERR_WOULDBLOCK EWOULDBLOCK
#define SOCK_ERR_INPROGRESS EINPROGRESS
#define SOCK_ERR_ALREADY    EALREADY
#define SOCK_ERR_ISCONN     EISCONN
#define SOCKET_ERROR        (-1)
#endif

#include "GameNetwork/networkutil.h"   // ResolveIP
#include "Common/GlobalData.h"         // m_coordPunchTTL override
#include "Common/ReleaseLog.h"

// timeGetTime is provided by the engine's PreRTS.h on both Windows and the
// stub used by docker/Linux builds.


// Wire constants must match tools/coordinator/protocol.go.
static const UnsignedInt  STUN_REQUEST_SIZE  = 4 + 16 + 1;
static const UnsignedInt  STUN_RESPONSE_SIZE = 4 + 4 + 2;
static const UnsignedInt  SESSION_TOKEN_BYTES = 16;
static const unsigned char STUN_PURPOSE_LOBBY = 0;
static const unsigned char STUN_PURPOSE_GAME  = 1;

static const UnsignedInt  STUN_PROBE_INTERVAL_MS = 500;
static const Int          STUN_PROBE_MAX_TRIES   = 6;
static const UnsignedInt  PUNCH_BLAST_INTERVAL_MS = 200;
static const UnsignedInt  PUNCH_TIMEOUT_MS        = 8000;
// The first punch volley goes out with a low IP TTL: enough hops to cross
// our own NAT(s) (home router + possible CGNAT) and create the outbound
// mapping, but expiring in transit before reaching the peer's NAT. Without
// this, whichever side's first packet lands early creates an unsolicited
// conntrack entry on Linux-style NATs that occupies the peer's advertised
// mapping and silently diverts their SNAT to a different port -- both sides
// then blast dead addresses forever (reproduced in the netns NAT lab; the
// low-TTL first volley fixes it there deterministically).
static const Int          PUNCH_LOW_TTL_DEFAULT   = 4;
static const Int          PUNCH_FULL_TTL          = 128;
static const UnsignedInt  PUNCH_LOW_TTL_MS        = 600;

// -coordpunchttl override (the netns lab needs 2; the internet default is 4).
static Int punchLowTTL(void)
{
	if (TheGlobalData && TheGlobalData->m_coordPunchTTL > 0)
		return TheGlobalData->m_coordPunchTTL;
	return PUNCH_LOW_TTL_DEFAULT;
}
static const UnsignedInt  TCP_RX_BUF_HIGH_WATER   = 64 * 1024;
// Keepalive interval for the stashed game socket. Well under the most
// aggressive NAT UDP idle TTLs (commonly 30s on home routers).
static const UnsignedInt  GAME_KEEPALIVE_INTERVAL_MS = 5000;

// --- Lobby-phase stash for the punched game UDP socket ---
// The OnlineCoordinatorAPI instance is destroyed during the LanLobbyMenu
// shutdown that follows PUNCH_OK, so the socket cannot live on the instance.
// We park it here, send periodic keepalives to maintain the NAT mapping, and
// ConnectionManager picks it up at game start.
static Int           s_stashGameFd            = -1;
static UnsignedShort s_stashGameLocalPort     = 0;
static UnsignedInt   s_stashGamePeerIPHost    = 0;  // host order (first joiner)
static UnsignedShort s_stashGamePeerPortHost  = 0;  // host order (first joiner)
static UnsignedInt   s_stashKeepaliveNextMs   = 0;
static AsciiString   s_stashKeepaliveNick;
// Additional in-game peer endpoints registered after the initial handoff
// (N-player: each subsequent joiner). The first peer above is the original
// punched peer and is always sent to first; entries here are sent in order
// alongside it on each keepalive tick.
static void setPunchTTL(Int fd, Int ttl);   // defined below with the punch pump

struct StashGamePeer { UnsignedInt ipHost; UnsignedShort portHost; Bool lowTtlProbeSent; };
static std::vector<StashGamePeer> s_stashGameExtraPeers;


// ---- platform helpers ------------------------------------------------------

static Bool setNonBlocking(Int fd)
{
#ifdef _WIN32
	unsigned long flag = 1;
	return (ioctlsocket(fd, FIONBIO, &flag) != SOCKET_ERROR);
#else
	Int flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0)
		return FALSE;
	return (fcntl(fd, F_SETFL, flags | O_NONBLOCK) >= 0);
#endif
}


// ---- minimal hex helpers ---------------------------------------------------

static Bool hexNibble(char c, unsigned char* out)
{
	if (c >= '0' && c <= '9') { *out = (unsigned char)(c - '0');      return TRUE; }
	if (c >= 'a' && c <= 'f') { *out = (unsigned char)(10 + c - 'a'); return TRUE; }
	if (c >= 'A' && c <= 'F') { *out = (unsigned char)(10 + c - 'A'); return TRUE; }
	return FALSE;
}

static Bool hexDecode(const char* hex, Int hexLen, unsigned char* out, Int outLen)
{
	if (hexLen != outLen * 2)
		return FALSE;
	Int i;
	for (i = 0; i < outLen; ++i)
	{
		unsigned char hi, lo;
		if (!hexNibble(hex[i*2], &hi) || !hexNibble(hex[i*2+1], &lo))
			return FALSE;
		out[i] = (unsigned char)((hi << 4) | lo);
	}
	return TRUE;
}


// ---- minimal JSON parser ---------------------------------------------------
// Just enough to extract well-known fields from flat JSON objects produced
// by the coordinator. Not general-purpose; no nested-escape support beyond
// simple \" within strings.

static const char* skipWs(const char* p)
{
	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
	return p;
}

// Find the position of "field": within a JSON object, returning the position
// of the value (after the colon, whitespace skipped). Returns NULL if not
// found. Only matches at object-property scope (i.e. preceded by ',' or '{').
static const char* findFieldValue(const char* obj, const char* fieldName)
{
	if (!obj || !fieldName) return NULL;
	Int nameLen = (Int)strlen(fieldName);
	const char* p = obj;
	const char* end = obj + strlen(obj);
	while (p < end)
	{
		// look for "fieldName"
		const char* q = strstr(p, fieldName);
		if (!q) return NULL;
		// must be enclosed in quotes
		if (q > obj && q[-1] == '"' && q[nameLen] == '"')
		{
			const char* after = q + nameLen + 1;
			after = skipWs(after);
			if (*after == ':')
			{
				++after;
				return skipWs(after);
			}
		}
		p = q + nameLen;
	}
	return NULL;
}

// Read a JSON string at *cursor (which should point at opening quote).
// Writes up to outCap-1 bytes plus NUL into out. Advances *cursor past the
// closing quote. Returns FALSE on malformed input.
static Bool readJsonString(const char** cursor, char* out, Int outCap)
{
	const char* p = *cursor;
	if (*p != '"') return FALSE;
	++p;
	Int w = 0;
	while (*p && *p != '"')
	{
		char c = *p++;
		if (c == '\\')
		{
			if (!*p) return FALSE;
			char esc = *p++;
			switch (esc)
			{
				case '"':  c = '"';  break;
				case '\\': c = '\\'; break;
				case '/':  c = '/';  break;
				case 'n':  c = '\n'; break;
				case 't':  c = '\t'; break;
				case 'r':  c = '\r'; break;
				default:   c = esc;  break;
			}
		}
		if (w < outCap - 1) out[w++] = c;
	}
	if (*p != '"') return FALSE;
	*cursor = p + 1;
	if (w >= outCap) w = outCap - 1;
	out[w] = '\0';
	return TRUE;
}

static Bool parseStringField(const char* obj, const char* fieldName, char* out, Int outCap)
{
	const char* v = findFieldValue(obj, fieldName);
	if (!v) return FALSE;
	return readJsonString(&v, out, outCap);
}

static Bool parseIntField(const char* obj, const char* fieldName, Int* out)
{
	const char* v = findFieldValue(obj, fieldName);
	if (!v) return FALSE;
	*out = (Int)strtol(v, NULL, 10);
	return TRUE;
}

static Bool parseUInt32Field(const char* obj, const char* fieldName, UnsignedInt* out)
{
	const char* v = findFieldValue(obj, fieldName);
	if (!v) return FALSE;
	*out = (UnsignedInt)strtoul(v, NULL, 10);
	return TRUE;
}

// Iterate top-level objects within an array value at *cursor (which should
// point at '['). For each {..} found, calls cb(objStart, objLen). Returns
// number of objects visited.
typedef void (*JsonArrayObjCB)(const char* objStart, Int objLen, void* user);

static Int forEachJsonArrayObj(const char* arrStart, JsonArrayObjCB cb, void* user)
{
	const char* p = arrStart;
	if (*p != '[') return 0;
	++p;
	Int count = 0;
	while (*p)
	{
		p = skipWs(p);
		if (*p == ']') break;
		if (*p != '{')
		{
			++p;
			continue;
		}
		const char* objStart = p;
		Int depth = 0;
		Bool inStr = FALSE;
		while (*p)
		{
			char c = *p;
			if (inStr)
			{
				if (c == '\\' && *(p+1)) { p += 2; continue; }
				if (c == '"') inStr = FALSE;
			}
			else
			{
				if      (c == '"') inStr = TRUE;
				else if (c == '{') ++depth;
				else if (c == '}') { --depth; if (depth == 0) { ++p; break; } }
			}
			++p;
		}
		Int objLen = (Int)(p - objStart);
		cb(objStart, objLen, user);
		++count;
		p = skipWs(p);
		if (*p == ',') ++p;
	}
	return count;
}


// ---- minimal JSON escape for outbound strings ------------------------------

static void appendEscaped(AsciiString& dst, const char* s)
{
	dst.concat('"');
	for (const char* p = s; *p; ++p)
	{
		char c = *p;
		switch (c)
		{
			case '"':  dst.concat("\\\""); break;
			case '\\': dst.concat("\\\\"); break;
			case '\n': dst.concat("\\n");  break;
			case '\r': dst.concat("\\r");  break;
			case '\t': dst.concat("\\t");  break;
			default:
				if ((unsigned char)c < 0x20)
				{
					char buf[8];
					snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
					dst.concat(buf);
				}
				else
				{
					dst.concat(c);
				}
				break;
		}
	}
	dst.concat('"');
}


// ---- static stash helpers --------------------------------------------------

Bool OnlineCoordinatorAPI::hasStashedGameSocket()
{
	return s_stashGameFd != -1;
}

Int OnlineCoordinatorAPI::takeStashedGameFd()
{
	Int fd = s_stashGameFd;
	s_stashGameFd = -1;
	DEBUG_LOG(("OnlineCoordinatorAPI: takeStashedGameFd -> %d (local port %u, peer %u.%u.%u.%u:%u)",
		fd, s_stashGameLocalPort,
		(s_stashGamePeerIPHost >> 24) & 0xff,
		(s_stashGamePeerIPHost >> 16) & 0xff,
		(s_stashGamePeerIPHost >>  8) & 0xff,
		(s_stashGamePeerIPHost      ) & 0xff,
		s_stashGamePeerPortHost));
	return fd;
}

UnsignedShort OnlineCoordinatorAPI::stashedGameLocalPort()
{
	return s_stashGameLocalPort;
}

UnsignedInt OnlineCoordinatorAPI::stashedGamePeerIPHost()
{
	return s_stashGamePeerIPHost;
}

UnsignedShort OnlineCoordinatorAPI::stashedGamePeerPortHost()
{
	return s_stashGamePeerPortHost;
}

void OnlineCoordinatorAPI::discardStashedGameSocket()
{
	if (s_stashGameFd != -1)
	{
		CLOSE_SOCKET(s_stashGameFd);
		s_stashGameFd = -1;
	}
	s_stashGameLocalPort    = 0;
	s_stashGamePeerIPHost   = 0;
	s_stashGamePeerPortHost = 0;
	s_stashKeepaliveNextMs  = 0;
	s_stashKeepaliveNick.clear();
	s_stashGameExtraPeers.clear();
}

void OnlineCoordinatorAPI::addStashedGamePeer(UnsignedInt ipHost, UnsignedShort portHost)
{
	if (s_stashGameFd == -1) return;
	if (ipHost == 0 || portHost == 0) return;
	// Skip duplicates (including the primary stash peer) so we don't double
	// up keepalives if the caller plumbs the same joiner twice.
	if (ipHost == s_stashGamePeerIPHost && portHost == s_stashGamePeerPortHost)
		return;
	for (size_t i = 0; i < s_stashGameExtraPeers.size(); ++i)
	{
		if (s_stashGameExtraPeers[i].ipHost == ipHost &&
		    s_stashGameExtraPeers[i].portHost == portHost)
			return;
	}
	StashGamePeer p;
	p.ipHost   = ipHost;
	p.portHost = portHost;
	p.lowTtlProbeSent = FALSE;
	s_stashGameExtraPeers.push_back(p);
	// Fire one immediately so the host's NAT installs an outbound mapping
	// before this joiner's first inbound packet arrives.
	s_stashKeepaliveNextMs = 0;
	DEBUG_LOG(("OnlineCoordinatorAPI::addStashedGamePeer - %u.%u.%u.%u:%u",
		(ipHost >> 24) & 0xff, (ipHost >> 16) & 0xff,
		(ipHost >> 8) & 0xff, ipHost & 0xff, portHost));
}

void OnlineCoordinatorAPI::pumpStashedKeepalive()
{
	if (s_stashGameFd == -1) return;
	const Bool hasPrimary = (s_stashGamePeerIPHost != 0 && s_stashGamePeerPortHost != 0);
	if (!hasPrimary && s_stashGameExtraPeers.empty()) return;

	UnsignedInt nowMs = timeGetTime();
	if (nowMs < s_stashKeepaliveNextMs) return;
	s_stashKeepaliveNextMs = nowMs + GAME_KEEPALIVE_INTERVAL_MS;

	// Tiny payload; content doesn't matter to either NAT or the peer (it'll
	// arrive on a socket that nothing is reading until ConnectionManager
	// adopts the FD, but discarded unread packets still keep NAT mappings
	// alive on both sides).
	char msg[48];
	Int msgLen = snprintf(msg, sizeof(msg), "KEEPALIVE from %s",
		s_stashKeepaliveNick.isEmpty() ? "coord" : s_stashKeepaliveNick.str());
	if (msgLen <= 0) msgLen = 9;

	struct sockaddr_in dst;
	memset(&dst, 0, sizeof(dst));
	dst.sin_family = AF_INET;
	if (hasPrimary)
	{
		dst.sin_addr.s_addr = htonl(s_stashGamePeerIPHost);
		dst.sin_port        = htons(s_stashGamePeerPortHost);
		sendto(s_stashGameFd, msg, msgLen, 0, (struct sockaddr*)&dst, sizeof(dst));
	}
	Bool sentLowTtlProbe = FALSE;
	for (size_t i = 0; i < s_stashGameExtraPeers.size(); ++i)
	{
		dst.sin_addr.s_addr = htonl(s_stashGameExtraPeers[i].ipHost);
		dst.sin_port        = htons(s_stashGameExtraPeers[i].portHost);
		if (!s_stashGameExtraPeers[i].lowTtlProbeSent)
		{
			// First packet toward a brand-new joiner: TTL-limited so it
			// opens OUR mapping but expires before it can poison the
			// joiner's NAT (the joiner hasn't started punching yet).
			setPunchTTL(s_stashGameFd, punchLowTTL());
			sendto(s_stashGameFd, msg, msgLen, 0, (struct sockaddr*)&dst, sizeof(dst));
			setPunchTTL(s_stashGameFd, PUNCH_FULL_TTL);
			s_stashGameExtraPeers[i].lowTtlProbeSent = TRUE;
			sentLowTtlProbe = TRUE;
		}
		else
		{
			sendto(s_stashGameFd, msg, msgLen, 0, (struct sockaddr*)&dst, sizeof(dst));
		}
	}
	// A new joiner is mid-punch right now (8s deadline); make sure it sees
	// full-TTL inbound game traffic well before that instead of waiting a
	// whole keepalive interval.
	if (sentLowTtlProbe)
		s_stashKeepaliveNextMs = nowMs + 2000;
}

// Parse an "ip:port" string into host-order UnsignedInt + UnsignedShort.
// Both *ip and *port are set to 0 on failure (and may be NULL to skip).
static void parseHostOrderIpPort(const AsciiString& s, UnsignedInt* ip, UnsignedShort* port)
{
	if (ip)   *ip   = 0;
	if (port) *port = 0;
	const char* str = s.str();
	const char* colon = strchr(str, ':');
	if (!colon || colon == str) return;
	char ipbuf[64];
	Int ipLen = (Int)(colon - str);
	if (ipLen >= (Int)sizeof(ipbuf)) ipLen = (Int)sizeof(ipbuf) - 1;
	memcpy(ipbuf, str, ipLen);
	ipbuf[ipLen] = '\0';
	UnsignedInt nbo = inet_addr(ipbuf);
	if (nbo == INADDR_NONE) return;
	if (ip)   *ip   = ntohl(nbo);
	if (port) *port = (UnsignedShort)strtoul(colon + 1, NULL, 10);
}

// ---- class implementation --------------------------------------------------

OnlineCoordinatorAPI::OnlineCoordinatorAPI()
	: m_state(STATE_IDLE)
	, m_tcpFd(-1)
	, m_udpFdLobby(-1)
	, m_udpFdGame(-1)
	, m_udpBoundPortLobby(0)
	, m_udpBoundPortGame(0)
	, m_stunMagic(0)
	, m_coordUdpPort(0)
	, m_coordIPNet(0)
	, m_peerInfoArmed(FALSE)
	, m_amIHost(FALSE)
	, m_postHandoff(FALSE)
	, m_stunNextProbeMsLobby(0)
	, m_stunNextProbeMsGame(0)
	, m_stunProbesSentLobby(0)
	, m_stunProbesSentGame(0)
	, m_stunOkLobby(FALSE)
	, m_stunOkGame(FALSE)
	, m_punchStartMs(0)
	, m_punchNextBlastMs(0)
	, m_punchDeadlineMs(0)
	, m_punchOkLobby(FALSE)
	, m_punchOkGame(FALSE)
	, m_punchTtl(0)
	, m_lastHeartbeatMs(0)
	, m_coordTcpPort(0)
{
	memset(&m_peerInfo, 0, sizeof(m_peerInfo));
}

OnlineCoordinatorAPI::~OnlineCoordinatorAPI()
{
	closeSockets();
}

void OnlineCoordinatorAPI::setState(State s)
{
	if (m_state == s) return;
	DEBUG_LOG(("OnlineCoordinatorAPI: state %d -> %d", (Int)m_state, (Int)s));
	m_state = s;
}

void OnlineCoordinatorAPI::setError(const AsciiString& msg)
{
	m_lastError = msg;
	DEBUG_LOG(("OnlineCoordinatorAPI: error: %s", msg.str()));
	setState(STATE_ERROR);
}

void OnlineCoordinatorAPI::closeSockets()
{
	if (m_tcpFd      != -1) { CLOSE_SOCKET(m_tcpFd);      m_tcpFd      = -1; }
	if (m_udpFdLobby != -1) { CLOSE_SOCKET(m_udpFdLobby); m_udpFdLobby = -1; }
	// m_udpFdGame may have been transferred to the lobby-phase stash by
	// stashGameSocketForGameStart(); in that case it's no longer ours to
	// close (the static stash now owns it).
	if (m_udpFdGame  != -1) { CLOSE_SOCKET(m_udpFdGame);  m_udpFdGame  = -1; }
	m_rxBuf.clear();
}

Bool OnlineCoordinatorAPI::stashGameSocketForGameStart()
{
	if (m_udpFdGame == -1)
	{
		DEBUG_LOG(("OnlineCoordinatorAPI::stashGameSocketForGameStart: no game socket to stash"));
		return FALSE;
	}
	// A punched peer is NOT required. The host now hands off as soon as its
	// game is listed, before anyone has joined: there is no peer yet, and
	// the primary-peer fields stay zero until joiners register themselves
	// via addStashedGamePeer (pumpStashedKeepalive handles a stash whose
	// only peers are the "extra" ones). Refusing to stash here left the
	// socket owned by a coordinator that is about to stop punching, so the
	// first joiner's game-port punch had nothing to open the host's mapping
	// and failed with lobby=true game=false.
	const Bool havePunchedPeer =
		(m_punchOkGame && m_peerInfo.gamePunchedIP != 0 && m_peerInfo.gamePunchedPort != 0);
	// Replace whatever was previously stashed (shouldn't happen in a clean
	// flow, but a stale FD must not leak across rematches).
	if (s_stashGameFd != -1)
	{
		DEBUG_LOG(("OnlineCoordinatorAPI::stashGameSocketForGameStart: replacing stale stash"));
		CLOSE_SOCKET(s_stashGameFd);
	}
	s_stashGameFd            = m_udpFdGame;
	s_stashGameLocalPort     = m_udpBoundPortGame;
	s_stashGamePeerIPHost    = havePunchedPeer ? m_peerInfo.gamePunchedIP   : 0;
	s_stashGamePeerPortHost  = havePunchedPeer ? m_peerInfo.gamePunchedPort : 0;
	s_stashKeepaliveNextMs   = timeGetTime();
	s_stashKeepaliveNick     = m_nick;
	m_udpFdGame              = -1;
	DEBUG_LOG(("OnlineCoordinatorAPI::stashGameSocketForGameStart: fd=%d local=%u peer=%u.%u.%u.%u:%u",
		s_stashGameFd, s_stashGameLocalPort,
		(s_stashGamePeerIPHost >> 24) & 0xff, (s_stashGamePeerIPHost >> 16) & 0xff,
		(s_stashGamePeerIPHost >>  8) & 0xff, (s_stashGamePeerIPHost      ) & 0xff,
		s_stashGamePeerPortHost));
	return TRUE;
}

void OnlineCoordinatorAPI::disconnect()
{
	if (m_tcpFd != -1)
	{
		AsciiString line = "{\"type\":\"bye\"}\n";
		send(m_tcpFd, line.str(), (Int)strlen(line.str()), 0);
	}
	closeSockets();
	m_state = STATE_IDLE;
	m_peerInfoArmed = FALSE;
	m_postHandoff = FALSE;
	m_newPeers.clear();
}

Int OnlineCoordinatorAPI::takeLobbyUdpFdForHandoff()
{
	Int fd = m_udpFdLobby;
	m_udpFdLobby = -1;   // caller owns it now; disconnect() must not close it
	m_postHandoff = TRUE;
	DEBUG_LOG(("OnlineCoordinatorAPI: lobby socket fd=%d handed to TheLAN; post-handoff mode", fd));
	return fd;
}

void OnlineCoordinatorAPI::closeLobbyUdpForHostHandoff()
{
	if (m_udpFdLobby != -1)
	{
		CLOSE_SOCKET(m_udpFdLobby);
		m_udpFdLobby = -1;
	}
	// Don't touch TCP -- we still want peer_info for subsequent joiners.
	// Don't touch the game UDP socket -- it's already stashed elsewhere.
	// We leave m_state at STATE_HOSTING so the coord doesn't think anything
	// went wrong; we just stop being able to STUN/punch.
	m_postHandoff = TRUE;
	DEBUG_LOG(("OnlineCoordinatorAPI: entered post-handoff mode; TCP signaling stays open for additional joiners"));
}

Bool OnlineCoordinatorAPI::consumeNewPeer(PeerInfo* out)
{
	if (m_newPeers.empty()) return FALSE;
	if (out) *out = m_newPeers.front();
	m_newPeers.erase(m_newPeers.begin());
	return TRUE;
}

Bool OnlineCoordinatorAPI::openUdpOnPort(UnsignedShort bindPort, Int& outFd, UnsignedShort& outBoundPort)
{
	Int fd = (Int)socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
	{
		setError("udp socket failed");
		return FALSE;
	}
	if (!setNonBlocking(fd))
	{
		CLOSE_SOCKET(fd);
		setError("udp setNonBlocking failed");
		return FALSE;
	}
	struct sockaddr_in a;
	memset(&a, 0, sizeof(a));
	a.sin_family      = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_ANY);
	a.sin_port        = htons(bindPort);
	if (bind(fd, (struct sockaddr*)&a, sizeof(a)) == SOCKET_ERROR)
	{
		// Preferred port taken (typically a second client behind the same
		// NAT / on the same machine, e.g. someone watching while another
		// instance plays). Fall back to an ephemeral port: peers always
		// send to the coordinator-observed EXTERNAL address, and the local
		// socket is handed off by fd (initFromFD / AdoptFD), so nothing
		// downstream depends on the local port number.
		if (bindPort != 0)
		{
			a.sin_port = 0;
			if (bind(fd, (struct sockaddr*)&a, sizeof(a)) != SOCKET_ERROR)
			{
				ReleaseLog("Coordinator: UDP port %u taken; using an ephemeral port instead",
					(unsigned)bindPort);
				goto bound;
			}
		}
		CLOSE_SOCKET(fd);
		AsciiString msg;
		msg.format("udp bind failed on port %u", (unsigned)bindPort);
		setError(msg);
		return FALSE;
	}
bound:
	// Read back what we actually bound to (matters when bindPort=0).
	struct sockaddr_in b;
	// VC6 winsock has no socklen_t; int* works on both stacks.
	int bLen = sizeof(b);
	memset(&b, 0, sizeof(b));
	if (getsockname(fd, (struct sockaddr*)&b, &bLen) == 0)
	{
		outBoundPort = ntohs(b.sin_port);
	}
	else
	{
		outBoundPort = bindPort;
	}
	outFd = fd;
	DEBUG_LOG(("OnlineCoordinatorAPI: udp bound on port %u", outBoundPort));
	return TRUE;
}

Bool OnlineCoordinatorAPI::beginTcpConnect(UnsignedInt ipHostOrder, UnsignedShort port)
{
	Int fd = (Int)socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
	{
		setError("tcp socket failed");
		return FALSE;
	}
	if (!setNonBlocking(fd))
	{
		CLOSE_SOCKET(fd);
		setError("tcp setNonBlocking failed");
		return FALSE;
	}
	struct sockaddr_in a;
	memset(&a, 0, sizeof(a));
	a.sin_family      = AF_INET;
	a.sin_addr.s_addr = htonl(ipHostOrder);
	a.sin_port        = htons(port);

	Int rc = ::connect(fd, (struct sockaddr*)&a, sizeof(a));
	if (rc == SOCKET_ERROR)
	{
		Int err = SOCK_ERR_LAST;
		if (err != SOCK_ERR_WOULDBLOCK && err != SOCK_ERR_INPROGRESS && err != SOCK_ERR_ALREADY)
		{
			CLOSE_SOCKET(fd);
			AsciiString msg;
			msg.format("tcp connect failed (%d)", err);
			setError(msg);
			return FALSE;
		}
	}
	m_tcpFd = fd;
	return TRUE;
}

Bool OnlineCoordinatorAPI::connect(const AsciiString& coordHost,
	UnsignedShort tcpPort,
	const AsciiString& nick,
	const AsciiString& version,
	UnsignedShort lobbyBindPort,
	UnsignedShort gameBindPort)
{
	disconnect();
	m_lastError.clear();

	UnsignedInt ipHostOrder = ResolveIP(coordHost);
	if (ipHostOrder == 0)
	{
		setError(AsciiString("cannot resolve coordinator host"));
		return FALSE;
	}
	m_coordIPNet = htonl(ipHostOrder);
	m_coordTcpPort = tcpPort;
	m_nick    = nick;
	m_version = version;
	m_games.clear();
	m_observerReqTokens.clear();
	m_observeOkToken.clear();
	memset(&m_peerInfo, 0, sizeof(m_peerInfo));
	m_peerInfoArmed = FALSE;
	m_amIHost = FALSE;
	m_publicAddrLobby.clear();
	m_publicAddrGame.clear();
	m_localAddr.clear();
	m_hostedGameID.clear();
	m_sessionToken.clear();
	m_stunProbesSentLobby = 0;
	m_stunProbesSentGame  = 0;
	m_stunNextProbeMsLobby = 0;
	m_stunNextProbeMsGame  = 0;
	m_stunOkLobby = FALSE;
	m_stunOkGame  = FALSE;
	m_punchOkLobby = FALSE;
	m_punchOkGame  = FALSE;
	m_postHandoff  = FALSE;
	m_newPeers.clear();

	if (!openUdpOnPort(lobbyBindPort, m_udpFdLobby, m_udpBoundPortLobby))
		return FALSE;
	if (!openUdpOnPort(gameBindPort, m_udpFdGame, m_udpBoundPortGame))
	{
		closeSockets();
		return FALSE;
	}

	if (!beginTcpConnect(ipHostOrder, tcpPort))
	{
		closeSockets();
		return FALSE;
	}
	setState(STATE_CONNECTING);
	return TRUE;
}

void OnlineCoordinatorAPI::sendJsonLine(const AsciiString& line)
{
	if (m_tcpFd == -1) return;
	AsciiString withNL = line;
	withNL.concat('\n');
	::send(m_tcpFd, withNL.str(), (Int)strlen(withNL.str()), 0);
}

void OnlineCoordinatorAPI::requestList()
{
	if (m_state < STATE_READY) return;
	sendJsonLine(AsciiString("{\"type\":\"list\",\"data\":{}}"));
}

void OnlineCoordinatorAPI::requestHost(const UnicodeString& gameName, const AsciiString& mapName, Int maxPlayers)
{
	if (m_state != STATE_READY) return;
	AsciiString nameAscii;
	nameAscii.translate(gameName);

	AsciiString line = "{\"type\":\"host\",\"data\":{\"name\":";
	appendEscaped(line, nameAscii.str());
	line.concat(",\"map\":");
	appendEscaped(line, mapName.str());
	AsciiString tail;
	tail.format(",\"max_players\":%d,\"local_addr\":\"%s\",\"public_addr\":\"%s\",\"game_public_addr\":\"%s\"}}",
		maxPlayers, m_localAddr.str(), m_publicAddrLobby.str(), m_publicAddrGame.str());
	line.concat(tail);

	sendJsonLine(line);
	m_amIHost = TRUE;
	setState(STATE_HOSTING);
}

void OnlineCoordinatorAPI::requestUnhost()
{
	if (m_state != STATE_HOSTING) return;
	sendJsonLine(AsciiString("{\"type\":\"unhost\",\"data\":{}}"));
	m_hostedGameID.clear();
	setState(STATE_READY);
}

void OnlineCoordinatorAPI::sendGameStarted()
{
	if (m_tcpFd == -1) return;
	sendJsonLine(AsciiString("{\"type\":\"game_started\",\"data\":{}}"));
}

void OnlineCoordinatorAPI::requestObserve(const AsciiString& gameID)
{
	if (m_state != STATE_READY) return;
	AsciiString line = "{\"type\":\"observe\",\"data\":{\"game_id\":";
	appendEscaped(line, gameID.str());
	line.concat("}}");
	sendJsonLine(line);
}

Bool OnlineCoordinatorAPI::consumeObserverRequestToken(AsciiString* outToken)
{
	if (m_observerReqTokens.empty())
		return FALSE;
	*outToken = m_observerReqTokens.front();
	m_observerReqTokens.erase(m_observerReqTokens.begin());
	return TRUE;
}

Bool OnlineCoordinatorAPI::consumeObserveOkToken(AsciiString* outToken)
{
	if (m_observeOkToken.isEmpty())
		return FALSE;
	*outToken = m_observeOkToken;
	m_observeOkToken.clear();
	return TRUE;
}

Int OnlineCoordinatorAPI::openObserverRelayFd(const AsciiString& token, Bool asHost)
{
	if (m_coordIPNet == 0 || m_coordTcpPort == 0)
		return -1;

	Int fd = (Int)socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;

	// Blocking connect: the coordinator just spoke to us over TCP, so it is
	// reachable; a stuck connect fails fast with a RST or the OS timeout.
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family      = AF_INET;
	addr.sin_addr.s_addr = m_coordIPNet;
	addr.sin_port        = htons(m_coordTcpPort);
	if (::connect(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0)
	{
		DEBUG_LOG(("OnlineCoordinatorAPI::openObserverRelayFd - connect failed (%d)", SOCK_ERR_LAST));
		CLOSE_SOCKET(fd);
		return -1;
	}

	AsciiString line = "{\"type\":\"relay_attach\",\"data\":{\"token\":";
	appendEscaped(line, token.str());
	line.concat(",\"role\":");
	appendEscaped(line, asHost ? "host" : "viewer");
	line.concat("}}\n");
	Int len = (Int)strlen(line.str());
	if (send(fd, line.str(), len, 0) != len)
	{
		DEBUG_LOG(("OnlineCoordinatorAPI::openObserverRelayFd - send failed (%d)", SOCK_ERR_LAST));
		CLOSE_SOCKET(fd);
		return -1;
	}
	DEBUG_LOG(("OnlineCoordinatorAPI::openObserverRelayFd - relay fd=%d role=%s", fd, asHost ? "host" : "viewer"));
	return fd;
}

void OnlineCoordinatorAPI::requestJoin(const AsciiString& gameID)
{
	if (m_state != STATE_READY) return;
	AsciiString line = "{\"type\":\"join\",\"data\":{\"game_id\":";
	appendEscaped(line, gameID.str());
	AsciiString tail;
	tail.format(",\"local_addr\":\"%s\",\"public_addr\":\"%s\",\"game_public_addr\":\"%s\"}}",
		m_localAddr.str(), m_publicAddrLobby.str(), m_publicAddrGame.str());
	line.concat(tail);
	sendJsonLine(line);
	m_amIHost = FALSE;
	setState(STATE_JOINING);
}

void OnlineCoordinatorAPI::sendStunProbe(Int fd, unsigned char purpose)
{
	if (fd == -1 || m_sessionToken.isEmpty()) return;
	unsigned char buf[STUN_REQUEST_SIZE];
	UnsignedInt magicBE = htonl(m_stunMagic);
	memcpy(buf, &magicBE, 4);
	if (!hexDecode(m_sessionToken.str(), (Int)strlen(m_sessionToken.str()), buf + 4, (Int)SESSION_TOKEN_BYTES))
	{
		setError(AsciiString("bad session token hex"));
		return;
	}
	buf[4 + SESSION_TOKEN_BYTES] = purpose;
	struct sockaddr_in dst;
	memset(&dst, 0, sizeof(dst));
	dst.sin_family      = AF_INET;
	dst.sin_addr.s_addr = m_coordIPNet;
	dst.sin_port        = htons(m_coordUdpPort);
	sendto(fd, (const char*)buf, (Int)sizeof(buf), 0, (struct sockaddr*)&dst, sizeof(dst));
}

void OnlineCoordinatorAPI::pumpStunDiscovery(UnsignedInt nowMs)
{
	if (m_state != STATE_DISCOVERING) return;

	// Lobby socket
	if (!m_stunOkLobby && nowMs >= m_stunNextProbeMsLobby)
	{
		if (m_stunProbesSentLobby >= STUN_PROBE_MAX_TRIES)
		{
			setError(AsciiString("STUN(lobby): no response after retries"));
			return;
		}
		sendStunProbe(m_udpFdLobby, STUN_PURPOSE_LOBBY);
		++m_stunProbesSentLobby;
		m_stunNextProbeMsLobby = nowMs + STUN_PROBE_INTERVAL_MS;
	}

	// Game socket
	if (!m_stunOkGame && nowMs >= m_stunNextProbeMsGame)
	{
		if (m_stunProbesSentGame >= STUN_PROBE_MAX_TRIES)
		{
			setError(AsciiString("STUN(game): no response after retries"));
			return;
		}
		sendStunProbe(m_udpFdGame, STUN_PURPOSE_GAME);
		++m_stunProbesSentGame;
		m_stunNextProbeMsGame = nowMs + STUN_PROBE_INTERVAL_MS;
	}

	if (m_stunOkLobby && m_stunOkGame)
	{
		DEBUG_LOG(("OnlineCoordinatorAPI: STUN complete: lobby=%s game=%s",
			m_publicAddrLobby.str(), m_publicAddrGame.str()));
		setState(STATE_READY);
	}
}

void OnlineCoordinatorAPI::blastPunchPacketsOn(Int fd, const AsciiString& publicAddr, const AsciiString& localAddr)
{
	if (fd == -1) return;

	char msg[64];
	Int msgLen = snprintf(msg, sizeof(msg), "PUNCH from %s", m_nick.str());
	if (msgLen <= 0) msgLen = 5;

	// Public address candidate.
	{
		const char* s = publicAddr.str();
		const char* colon = strchr(s, ':');
		if (colon && colon != s)
		{
			char ipbuf[64];
			Int ipLen = (Int)(colon - s);
			if (ipLen >= (Int)sizeof(ipbuf)) ipLen = (Int)sizeof(ipbuf) - 1;
			memcpy(ipbuf, s, ipLen);
			ipbuf[ipLen] = '\0';
			UnsignedShort port = (UnsignedShort)strtoul(colon + 1, NULL, 10);
			struct sockaddr_in dst;
			memset(&dst, 0, sizeof(dst));
			dst.sin_family      = AF_INET;
			dst.sin_addr.s_addr = inet_addr(ipbuf);
			dst.sin_port        = htons(port);
			if (dst.sin_addr.s_addr != INADDR_NONE)
				sendto(fd, msg, msgLen, 0, (struct sockaddr*)&dst, sizeof(dst));
		}
	}

	// Local address candidate (for same-LAN play). Only the lobby side has
	// a local_addr exchanged through the coordinator; the game side reuses
	// the same LAN IP at the local game port if both peers share a LAN.
	if (!localAddr.isEmpty())
	{
		const char* s = localAddr.str();
		const char* colon = strchr(s, ':');
		if (colon && colon != s)
		{
			char ipbuf[64];
			Int ipLen = (Int)(colon - s);
			if (ipLen >= (Int)sizeof(ipbuf)) ipLen = (Int)sizeof(ipbuf) - 1;
			memcpy(ipbuf, s, ipLen);
			ipbuf[ipLen] = '\0';
			UnsignedShort port = (UnsignedShort)strtoul(colon + 1, NULL, 10);
			struct sockaddr_in dst;
			memset(&dst, 0, sizeof(dst));
			dst.sin_family      = AF_INET;
			dst.sin_addr.s_addr = inet_addr(ipbuf);
			dst.sin_port        = htons(port);
			if (dst.sin_addr.s_addr != INADDR_NONE)
				sendto(fd, msg, msgLen, 0, (struct sockaddr*)&dst, sizeof(dst));
		}
	}
}

// Set the IPv4 unicast TTL on a punch socket. On Windows the numeric value
// of IP_TTL depends on the winsock generation: winsock.h (WS1) says 7,
// ws2_32 uses 4, and which one the stack honors depends on which import
// library won at link time. Set both; the wrong one is a harmless
// no-op/ENOPROTOOPT on the other stack.
static void setPunchTTL(Int fd, Int ttl)
{
	if (fd == -1) return;
#ifdef _WIN32
	setsockopt(fd, IPPROTO_IP, 4, (const char*)&ttl, sizeof(ttl));
	setsockopt(fd, IPPROTO_IP, 7, (const char*)&ttl, sizeof(ttl));
#else
	setsockopt(fd, IPPROTO_IP, IP_TTL, (const char*)&ttl, sizeof(ttl));
#endif
}

void OnlineCoordinatorAPI::sendPunchOutcome(Bool ok)
{
	if (m_tcpFd == -1) return;
	UnsignedInt nowMs = timeGetTime();
	UnsignedInt tookMs = (nowMs > m_punchStartMs) ? (nowMs - m_punchStartMs) : 0;
	AsciiString line;
	line.format("{\"type\":\"punch_outcome\",\"data\":{\"ok\":%s,\"lobby_ok\":%s,\"game_ok\":%s,\"ms\":%u,\"role\":\"%s\"}}",
		ok ? "true" : "false",
		m_punchOkLobby ? "true" : "false",
		m_punchOkGame  ? "true" : "false",
		tookMs,
		m_amIHost ? "host" : "guest");
	sendJsonLine(line);
}

void OnlineCoordinatorAPI::pumpPunch(UnsignedInt nowMs)
{
	if (m_state != STATE_PUNCHING) return;
	if (!m_peerInfoArmed) return;
	if (nowMs >= m_punchDeadlineMs)
	{
		sendPunchOutcome(FALSE);
		if (m_amIHost && !m_postHandoff && !m_newPeers.empty())
		{
			// The current joiner never punched through, but another joiner is
			// queued. Promote it instead of killing the whole lobby. No punch
			// delay: the queued peer started blasting when its peer_info was
			// delivered, so it's us who is late.
			DEBUG_LOG(("OnlineCoordinatorAPI: punch timed out for %s; promoting next queued joiner %s",
				m_peerInfo.nick.str(), m_newPeers.front().nick.str()));
			m_peerInfo = m_newPeers.front();
			m_newPeers.erase(m_newPeers.begin());
			m_punchOkLobby     = FALSE;
			m_punchOkGame      = FALSE;
			m_punchTtl         = 0;   // restart the low-TTL phase for the new peer
			m_punchStartMs     = nowMs;
			m_punchNextBlastMs = nowMs;
			m_punchDeadlineMs  = nowMs + PUNCH_TIMEOUT_MS;
			return;
		}
		setError(AsciiString("punch: no inbound packet within timeout"));
		return;
	}
	if (nowMs >= m_punchNextBlastMs)
	{
		Int wantTtl = (nowMs < m_punchStartMs + PUNCH_LOW_TTL_MS) ? punchLowTTL() : PUNCH_FULL_TTL;
		if (wantTtl != m_punchTtl)
		{
			setPunchTTL(m_udpFdLobby, wantTtl);
			setPunchTTL(m_udpFdGame,  wantTtl);
			m_punchTtl = wantTtl;
		}
		if (!m_punchOkLobby)
			blastPunchPacketsOn(m_udpFdLobby, m_peerInfo.publicAddr, m_peerInfo.localAddr);
		if (!m_punchOkGame)
			// Game-side has no exchanged local_addr today; pass empty so the
			// helper only fires the public candidate.
			blastPunchPacketsOn(m_udpFdGame, m_peerInfo.gamePublicAddr, AsciiString::TheEmptyString);
		m_punchNextBlastMs = nowMs + PUNCH_BLAST_INTERVAL_MS;
	}
}

void OnlineCoordinatorAPI::pumpUdpRecv()
{
	pumpUdpRecvOne(m_udpFdLobby, FALSE);
	pumpUdpRecvOne(m_udpFdGame,  TRUE);

	if (m_state == STATE_PUNCHING && m_punchOkLobby && m_punchOkGame)
	{
		DEBUG_LOG(("OnlineCoordinatorAPI: PUNCH OK on both sockets, lobby=%u.%u.%u.%u:%u game=%u.%u.%u.%u:%u",
			(m_peerInfo.punchedIP >> 24) & 0xff,
			(m_peerInfo.punchedIP >> 16) & 0xff,
			(m_peerInfo.punchedIP >> 8) & 0xff,
			(m_peerInfo.punchedIP) & 0xff,
			m_peerInfo.punchedPort,
			(m_peerInfo.gamePunchedIP >> 24) & 0xff,
			(m_peerInfo.gamePunchedIP >> 16) & 0xff,
			(m_peerInfo.gamePunchedIP >> 8) & 0xff,
			(m_peerInfo.gamePunchedIP) & 0xff,
			m_peerInfo.gamePunchedPort));
		setState(STATE_PUNCH_OK);
		sendPunchOutcome(TRUE);
	}
}

void OnlineCoordinatorAPI::pumpUdpRecvOne(Int fd, Bool isGame)
{
	if (fd == -1) return;
	unsigned char buf[1500];
	for (;;)
	{
		struct sockaddr_in src;
		int srcLen = sizeof(src);
		memset(&src, 0, sizeof(src));
		Int n = (Int)recvfrom(fd, (char*)buf, (Int)sizeof(buf), 0,
			(struct sockaddr*)&src, &srcLen);
		if (n <= 0)
		{
			// EWOULDBLOCK or socket closed; bail.
			break;
		}

		// STUN response: 10 bytes starting with our magic. Only consumed
		// during discovery (otherwise we ignore stray late replies).
		if (n == (Int)STUN_RESPONSE_SIZE && m_state == STATE_DISCOVERING)
		{
			UnsignedInt magicBE;
			memcpy(&magicBE, buf, 4);
			if (ntohl(magicBE) == m_stunMagic)
			{
				unsigned char ip[4];
				UnsignedShort portBE;
				memcpy(ip, buf + 4, 4);
				memcpy(&portBE, buf + 8, 2);
				UnsignedShort port = ntohs(portBE);
				AsciiString addr;
				addr.format("%u.%u.%u.%u:%u", ip[0], ip[1], ip[2], ip[3], port);
				if (isGame)
				{
					m_publicAddrGame = addr;
					m_stunOkGame = TRUE;
					DEBUG_LOG(("OnlineCoordinatorAPI: STUN(game) public=%s", addr.str()));
				}
				else
				{
					m_publicAddrLobby = addr;
					m_stunOkLobby = TRUE;
					DEBUG_LOG(("OnlineCoordinatorAPI: STUN(lobby) public=%s", addr.str()));
				}
				continue;
			}
		}

		// In PUNCHING state, any packet on a socket from the peer counts as a
		// successful punch on that socket. We don't validate contents.
		if (m_state == STATE_PUNCHING && m_peerInfoArmed)
		{
			UnsignedInt   srcIPHost   = ntohl(src.sin_addr.s_addr);
			UnsignedShort srcPortHost = ntohs(src.sin_port);
			if (isGame && !m_punchOkGame)
			{
				m_peerInfo.gamePunchedIP   = srcIPHost;
				m_peerInfo.gamePunchedPort = srcPortHost;
				m_punchOkGame = TRUE;
				DEBUG_LOG(("OnlineCoordinatorAPI: PUNCH(game) OK from %u.%u.%u.%u:%u",
					(srcIPHost >> 24) & 0xff, (srcIPHost >> 16) & 0xff,
					(srcIPHost >> 8) & 0xff, (srcIPHost) & 0xff, srcPortHost));
			}
			else if (!isGame && !m_punchOkLobby)
			{
				m_peerInfo.punchedIP   = srcIPHost;
				m_peerInfo.punchedPort = srcPortHost;
				m_punchOkLobby = TRUE;
				DEBUG_LOG(("OnlineCoordinatorAPI: PUNCH(lobby) OK from %u.%u.%u.%u:%u",
					(srcIPHost >> 24) & 0xff, (srcIPHost >> 16) & 0xff,
					(srcIPHost >> 8) & 0xff, (srcIPHost) & 0xff, srcPortHost));
			}
			// Send one acknowledgement so the peer also sees inbound traffic
			// quickly even if its first blast was lost. Inbound traffic means
			// both NAT mappings are settled, so the low-TTL phase (if still
			// active) is over: the ACK must survive the full path.
			if (m_punchTtl != PUNCH_FULL_TTL)
			{
				setPunchTTL(m_udpFdLobby, PUNCH_FULL_TTL);
				setPunchTTL(m_udpFdGame,  PUNCH_FULL_TTL);
				m_punchTtl = PUNCH_FULL_TTL;
			}
			char ack[32];
			Int ackLen = snprintf(ack, sizeof(ack), "ACK from %s", m_nick.str());
			if (ackLen > 0)
				sendto(fd, ack, ackLen, 0, (struct sockaddr*)&src, sizeof(src));
		}
	}
}

void OnlineCoordinatorAPI::update()
{
	if (m_state == STATE_IDLE || m_state == STATE_ERROR)
		return;

	UnsignedInt nowMs = timeGetTime();

	// CONNECTING: detect non-blocking connect completion via select(write).
	// recv-with-len-0 was unreliable on winsock (returns WSAENOTCONN while
	// the connect is still pending instead of WSAEWOULDBLOCK).
	if (m_state == STATE_CONNECTING && m_tcpFd != -1)
	{
		fd_set wfds;
		FD_ZERO(&wfds);
		FD_SET(m_tcpFd, &wfds);
		struct timeval tv;
		tv.tv_sec  = 0;
		tv.tv_usec = 0;
		Int rc = select((int)m_tcpFd + 1, NULL, &wfds, NULL, &tv);
		if (rc < 0)
		{
			AsciiString msg;
			msg.format("tcp connect select failed (%d)", SOCK_ERR_LAST);
			setError(msg);
			return;
		}
		if (rc == 0 || !FD_ISSET(m_tcpFd, &wfds))
		{
			// connect still in progress; try again next tick
			return;
		}
		// Socket is writable: either connected or failed. SO_ERROR tells us.
		int soerr = 0;
		// VC6 winsock has no socklen_t; int* works on both stacks.
		int errlen = sizeof(soerr);
		if (getsockopt(m_tcpFd, SOL_SOCKET, SO_ERROR, (char*)&soerr, &errlen) != 0)
		{
			AsciiString msg;
			msg.format("tcp getsockopt(SO_ERROR) failed (%d)", SOCK_ERR_LAST);
			setError(msg);
			return;
		}
		if (soerr != 0)
		{
			AsciiString msg;
			msg.format("tcp connect failed (%d)", soerr);
			setError(msg);
			return;
		}
		// Connected. Send HELLO and transition.
		AsciiString hello = "{\"type\":\"hello\",\"data\":{\"nick\":";
		appendEscaped(hello, m_nick.str());
		hello.concat(",\"version\":");
		appendEscaped(hello, m_version.str());
		hello.concat("}}");
		sendJsonLine(hello);
		setState(STATE_HANDSHAKING);
	}

	// Keepalive: the coordinator reaps sessions idle for 5 minutes, and a
	// host can easily sit in the game-options screen longer than that
	// without generating any TCP traffic (its game silently vanishes from
	// the list). One heartbeat a minute keeps LastSeen fresh, pre- and
	// post-handoff alike.
	if (m_tcpFd != -1 && m_state != STATE_CONNECTING)
	{
		const UnsignedInt HEARTBEAT_INTERVAL_MS = 60 * 1000;
		if (m_lastHeartbeatMs == 0)
			m_lastHeartbeatMs = nowMs;
		else if (nowMs - m_lastHeartbeatMs >= HEARTBEAT_INTERVAL_MS)
		{
			sendJsonLine(AsciiString("{\"type\":\"heartbeat\"}"));
			m_lastHeartbeatMs = nowMs;
		}
	}

	pumpTcpRecv();
	pumpUdpRecv();
	pumpStunDiscovery(nowMs);
	pumpPunch(nowMs);
}

void OnlineCoordinatorAPI::pumpTcpRecv()
{
	if (m_tcpFd == -1) return;
	char buf[4096];
	for (;;)
	{
		Int n = (Int)recv(m_tcpFd, buf, (Int)sizeof(buf), 0);
		if (n > 0)
		{
			m_rxBuf.insert(m_rxBuf.end(), buf, buf + n);
			if (m_rxBuf.size() > TCP_RX_BUF_HIGH_WATER)
			{
				setError(AsciiString("tcp rx buffer overflow"));
				return;
			}
			continue;
		}
		if (n == 0)
		{
			// peer closed
			DEBUG_LOG(("OnlineCoordinatorAPI: tcp closed by coordinator"));
			closeSockets();
			setState(STATE_IDLE);
			return;
		}
		Int err = SOCK_ERR_LAST;
		if (err == SOCK_ERR_WOULDBLOCK)
			break;
		// other error
		AsciiString msg;
		msg.format("tcp recv error (%d)", err);
		setError(msg);
		return;
	}

	// Extract newline-terminated messages.
	while (!m_rxBuf.empty())
	{
		size_t nl = 0;
		Bool found = FALSE;
		for (size_t i = 0; i < m_rxBuf.size(); ++i)
		{
			if (m_rxBuf[i] == '\n') { nl = i; found = TRUE; break; }
		}
		if (!found) break;

		// extract one line
		AsciiString line;
		if (nl > 0) line.set(AsciiString(""));
		std::vector<char> tmp(m_rxBuf.begin(), m_rxBuf.begin() + nl);
		tmp.push_back('\0');
		line.set(&tmp[0]);
		m_rxBuf.erase(m_rxBuf.begin(), m_rxBuf.begin() + nl + 1);

		// Parse envelope: {"type":"...","data":{...}}
		char typeBuf[64];
		if (!parseStringField(line.str(), "type", typeBuf, sizeof(typeBuf)))
		{
			DEBUG_LOG(("OnlineCoordinatorAPI: malformed line (no type): %s", line.str()));
			continue;
		}
		// Find the "data" object start.
		const char* dataV = findFieldValue(line.str(), "data");
		// We pass the entire line as the JSON-object context; subsequent
		// parseStringField/parseIntField calls look anywhere in the line,
		// which is fine because field names within the small protocol
		// don't collide with envelope field names.
		const char* obj = line.str();
		(void)dataV;
		onTcpMessage(typeBuf, obj);
	}
}

namespace {
struct GamesParseCtx
{
	std::vector<OnlineCoordinatorAPI::GameListEntry>* out;
};
static void gamesParseCb(const char* objStart, Int objLen, void* user)
{
	GamesParseCtx* ctx = (GamesParseCtx*)user;
	// Copy to NUL-terminated buffer so the string-based parser works.
	std::vector<char> buf(objStart, objStart + objLen);
	buf.push_back('\0');
	const char* obj = &buf[0];

	OnlineCoordinatorAPI::GameListEntry e;
	char tmp[256];
	if (parseStringField(obj, "id",        tmp, sizeof(tmp))) e.id       = tmp;
	if (parseStringField(obj, "name",      tmp, sizeof(tmp))) e.name     = tmp;
	if (parseStringField(obj, "host_nick", tmp, sizeof(tmp))) e.hostNick = tmp;
	if (parseStringField(obj, "map",       tmp, sizeof(tmp))) e.map      = tmp;
	parseIntField(obj, "players",     &e.players);
	parseIntField(obj, "max_players", &e.maxPlayers);
	e.inProgress = 0;
	parseIntField(obj, "in_progress", &e.inProgress);
	ctx->out->push_back(e);
}
}

void OnlineCoordinatorAPI::onTcpMessage(const char* msgType, const char* obj)
{
	if (strcmp(msgType, "hello_ok") == 0)
	{
		char tok[64];
		UnsignedInt magic = 0;
		Int udpPort = 0;
		if (!parseStringField(obj, "session_token", tok, sizeof(tok)))
		{
			setError(AsciiString("hello_ok: missing session_token"));
			return;
		}
		parseUInt32Field(obj, "stun_magic", &magic);
		parseIntField(obj, "udp_port", &udpPort);
		m_sessionToken = tok;
		m_stunMagic    = magic;
		m_coordUdpPort = (UnsignedShort)udpPort;
		// Capture our local IP for the local_addr hint. The advertised port
		// is the lobby socket (LAN games look it up here for same-LAN play).
		struct sockaddr_in self;
		int selfLen = sizeof(self);
		if (m_tcpFd != -1 && getsockname(m_tcpFd, (struct sockaddr*)&self, &selfLen) == 0)
		{
			UnsignedInt nbo = self.sin_addr.s_addr;
			AsciiString s;
			s.format("%u.%u.%u.%u:%u",
				(nbo) & 0xff, (nbo>>8) & 0xff, (nbo>>16) & 0xff, (nbo>>24) & 0xff,
				m_udpBoundPortLobby);
			m_localAddr = s;
		}
		setState(STATE_DISCOVERING);
		UnsignedInt nowMs = timeGetTime();
		m_stunNextProbeMsLobby = nowMs; // probe immediately
		m_stunNextProbeMsGame  = nowMs;
		return;
	}

	if (strcmp(msgType, "hosted") == 0)
	{
		char gid[64];
		if (parseStringField(obj, "game_id", gid, sizeof(gid)))
			m_hostedGameID = gid;
		return;
	}

	if (strcmp(msgType, "games") == 0)
	{
		m_games.clear();
		const char* arr = findFieldValue(obj, "games");
		if (arr && *arr == '[')
		{
			GamesParseCtx ctx; ctx.out = &m_games;
			forEachJsonArrayObj(arr, gamesParseCb, &ctx);
		}
		return;
	}

	if (strcmp(msgType, "peer_info") == 0)
	{
		PeerInfo p;
		memset(&p, 0, sizeof(p));
		char tmp[128];
		if (parseStringField(obj, "nick",             tmp, sizeof(tmp))) p.nick           = tmp;
		if (parseStringField(obj, "public_addr",      tmp, sizeof(tmp))) p.publicAddr     = tmp;
		if (parseStringField(obj, "game_public_addr", tmp, sizeof(tmp))) p.gamePublicAddr = tmp;
		if (parseStringField(obj, "local_addr",       tmp, sizeof(tmp))) p.localAddr      = tmp;
		if (parseStringField(obj, "role",             tmp, sizeof(tmp))) p.role           = tmp;
		parseIntField(obj, "punch_in_ms", &p.punchInMS);

		// Resolve the peer's punched-port pair from the addr strings (the
		// coordinator-observed external addrs). After handoff we can't punch
		// any more so these are the values the lobby UI plumbs into TheLAN.
		parseHostOrderIpPort(p.publicAddr,     &p.punchedIP,     &p.punchedPort);
		parseHostOrderIpPort(p.gamePublicAddr, &p.gamePunchedIP, &p.gamePunchedPort);

		if (m_postHandoff || m_peerInfoArmed)
		{
			// Two reasons we can't punch this peer right now:
			//  - post-handoff: we no longer own the UDP sockets. The lobby UI
			//    drains the queue and plumbs the game-port into TheLAN.
			//  - a punch is already armed for another peer (or has completed
			//    and is awaiting the UI handoff). Overwriting m_peerInfo here
			//    would redirect the in-flight punch mid-blast and strand the
			//    first joiner. Queue instead; the queue is drained by the UI
			//    once we enter post-handoff, or promoted by pumpPunch if the
			//    current punch times out.
			DEBUG_LOG(("OnlineCoordinatorAPI: peer_info from %s (game %s) while %s; queueing",
				p.publicAddr.str(), p.gamePublicAddr.str(),
				m_postHandoff ? "post-handoff" : "punch already in flight"));
			m_newPeers.push_back(p);
			return;
		}

		m_peerInfo = p;
		m_peerInfoArmed = TRUE;
		m_punchOkLobby = FALSE;
		m_punchOkGame  = FALSE;
		m_punchTtl     = 0;   // force TTL (re)apply on the first blast
		UnsignedInt nowMs = timeGetTime();
		m_punchStartMs     = nowMs + (UnsignedInt)m_peerInfo.punchInMS;
		m_punchNextBlastMs = m_punchStartMs;
		m_punchDeadlineMs  = m_punchStartMs + PUNCH_TIMEOUT_MS;
		setState(STATE_PUNCHING);
		return;
	}

	if (strcmp(msgType, "observer_request") == 0)
	{
		// Host side: a viewer wants to observe our in-progress game. Queue
		// the relay token; the in-game pump opens the relay connection and
		// hands it to LANObserverHost.
		char tok[128];
		if (parseStringField(obj, "token", tok, sizeof(tok)))
		{
			m_observerReqTokens.push_back(AsciiString(tok));
			DEBUG_LOG(("OnlineCoordinatorAPI: observer_request token=%s", tok));
		}
		return;
	}

	if (strcmp(msgType, "observe_ok") == 0)
	{
		// Viewer side: the coordinator accepted our observe request and
		// told the host. The relay is ready for both attach connections.
		char tok[128];
		if (parseStringField(obj, "token", tok, sizeof(tok)))
		{
			m_observeOkToken = tok;
			DEBUG_LOG(("OnlineCoordinatorAPI: observe_ok token=%s", tok));
		}
		return;
	}

	if (strcmp(msgType, "error") == 0)
	{
		char msg[256] = "unknown";
		parseStringField(obj, "message", msg, sizeof(msg));
		AsciiString s; s.format("coordinator: %s", msg);
		setError(s);
		return;
	}

	// Unknown message type: ignore for forward compatibility.
}
