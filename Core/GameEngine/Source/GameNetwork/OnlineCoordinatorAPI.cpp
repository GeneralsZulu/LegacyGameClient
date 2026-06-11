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

// timeGetTime is provided by the engine's PreRTS.h on both Windows and the
// stub used by docker/Linux builds.


// Wire constants must match tools/coordinator/protocol.go.
static const UnsignedInt  STUN_REQUEST_SIZE  = 4 + 16;
static const UnsignedInt  STUN_RESPONSE_SIZE = 4 + 4 + 2;
static const UnsignedInt  SESSION_TOKEN_BYTES = 16;

static const UnsignedInt  STUN_PROBE_INTERVAL_MS = 500;
static const Int          STUN_PROBE_MAX_TRIES   = 6;
static const UnsignedInt  PUNCH_BLAST_INTERVAL_MS = 200;
static const UnsignedInt  PUNCH_TIMEOUT_MS        = 8000;
static const UnsignedInt  TCP_RX_BUF_HIGH_WATER   = 64 * 1024;


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


// ---- class implementation --------------------------------------------------

OnlineCoordinatorAPI::OnlineCoordinatorAPI()
	: m_state(STATE_IDLE)
	, m_tcpFd(-1)
	, m_udpFd(-1)
	, m_udpBoundPort(0)
	, m_stunMagic(0)
	, m_coordUdpPort(0)
	, m_coordIPNet(0)
	, m_peerInfoArmed(FALSE)
	, m_amIHost(FALSE)
	, m_stunNextProbeMs(0)
	, m_stunProbesSent(0)
	, m_punchStartMs(0)
	, m_punchNextBlastMs(0)
	, m_punchDeadlineMs(0)
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
	if (m_tcpFd != -1) { CLOSE_SOCKET(m_tcpFd); m_tcpFd = -1; }
	if (m_udpFd != -1) { CLOSE_SOCKET(m_udpFd); m_udpFd = -1; }
	m_rxBuf.clear();
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
}

Bool OnlineCoordinatorAPI::openUdp(UnsignedShort bindPort)
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
		CLOSE_SOCKET(fd);
		setError("udp bind failed");
		return FALSE;
	}
	// Read back what we actually bound to (matters when bindPort=0).
	struct sockaddr_in b;
	// VC6 winsock has no socklen_t; int* works on both stacks.
	int bLen = sizeof(b);
	memset(&b, 0, sizeof(b));
	if (getsockname(fd, (struct sockaddr*)&b, &bLen) == 0)
	{
		m_udpBoundPort = ntohs(b.sin_port);
	}
	else
	{
		m_udpBoundPort = bindPort;
	}
	m_udpFd = fd;
	DEBUG_LOG(("OnlineCoordinatorAPI: udp bound on port %u", m_udpBoundPort));
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
	UnsignedShort udpBindPort)
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
	m_nick    = nick;
	m_version = version;
	m_games.clear();
	memset(&m_peerInfo, 0, sizeof(m_peerInfo));
	m_peerInfoArmed = FALSE;
	m_amIHost = FALSE;
	m_publicAddr.clear();
	m_localAddr.clear();
	m_hostedGameID.clear();
	m_sessionToken.clear();
	m_stunProbesSent  = 0;
	m_stunNextProbeMs = 0;

	if (!openUdp(udpBindPort))
		return FALSE;

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
	tail.format(",\"max_players\":%d,\"local_addr\":\"%s\",\"public_addr\":\"%s\"}}",
		maxPlayers, m_localAddr.str(), m_publicAddr.str());
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

void OnlineCoordinatorAPI::requestJoin(const AsciiString& gameID)
{
	if (m_state != STATE_READY) return;
	AsciiString line = "{\"type\":\"join\",\"data\":{\"game_id\":";
	appendEscaped(line, gameID.str());
	AsciiString tail;
	tail.format(",\"local_addr\":\"%s\",\"public_addr\":\"%s\"}}",
		m_localAddr.str(), m_publicAddr.str());
	line.concat(tail);
	sendJsonLine(line);
	m_amIHost = FALSE;
	setState(STATE_JOINING);
}

void OnlineCoordinatorAPI::sendStunProbe()
{
	if (m_udpFd == -1 || m_sessionToken.isEmpty()) return;
	unsigned char buf[STUN_REQUEST_SIZE];
	UnsignedInt magicBE = htonl(m_stunMagic);
	memcpy(buf, &magicBE, 4);
	if (!hexDecode(m_sessionToken.str(), (Int)strlen(m_sessionToken.str()), buf + 4, (Int)SESSION_TOKEN_BYTES))
	{
		setError(AsciiString("bad session token hex"));
		return;
	}
	struct sockaddr_in dst;
	memset(&dst, 0, sizeof(dst));
	dst.sin_family      = AF_INET;
	dst.sin_addr.s_addr = m_coordIPNet;
	dst.sin_port        = htons(m_coordUdpPort);
	sendto(m_udpFd, (const char*)buf, (Int)sizeof(buf), 0, (struct sockaddr*)&dst, sizeof(dst));
	++m_stunProbesSent;
}

void OnlineCoordinatorAPI::pumpStunDiscovery(UnsignedInt nowMs)
{
	if (m_state != STATE_DISCOVERING) return;
	if (nowMs < m_stunNextProbeMs) return;
	if (m_stunProbesSent >= STUN_PROBE_MAX_TRIES)
	{
		setError(AsciiString("STUN: no response after retries"));
		return;
	}
	sendStunProbe();
	m_stunNextProbeMs = nowMs + STUN_PROBE_INTERVAL_MS;
}

void OnlineCoordinatorAPI::blastPunchPackets()
{
	if (m_udpFd == -1) return;
	if (!m_peerInfoArmed) return;

	char msg[64];
	Int msgLen = snprintf(msg, sizeof(msg), "PUNCH from %s", m_nick.str());
	if (msgLen <= 0) msgLen = 5;

	// Public address candidate.
	{
		const char* s = m_peerInfo.publicAddr.str();
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
				sendto(m_udpFd, msg, msgLen, 0, (struct sockaddr*)&dst, sizeof(dst));
		}
	}

	// Local address candidate (for same-LAN play).
	if (!m_peerInfo.localAddr.isEmpty())
	{
		const char* s = m_peerInfo.localAddr.str();
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
				sendto(m_udpFd, msg, msgLen, 0, (struct sockaddr*)&dst, sizeof(dst));
		}
	}
}

void OnlineCoordinatorAPI::pumpPunch(UnsignedInt nowMs)
{
	if (m_state != STATE_PUNCHING) return;
	if (nowMs >= m_punchDeadlineMs)
	{
		setError(AsciiString("punch: no inbound packet within timeout"));
		return;
	}
	if (nowMs >= m_punchNextBlastMs)
	{
		blastPunchPackets();
		m_punchNextBlastMs = nowMs + PUNCH_BLAST_INTERVAL_MS;
	}
}

void OnlineCoordinatorAPI::pumpUdpRecv()
{
	if (m_udpFd == -1) return;
	unsigned char buf[1500];
	for (;;)
	{
		struct sockaddr_in src;
		int srcLen = sizeof(src);
		memset(&src, 0, sizeof(src));
		Int n = (Int)recvfrom(m_udpFd, (char*)buf, (Int)sizeof(buf), 0,
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
				m_publicAddr = addr;
				DEBUG_LOG(("OnlineCoordinatorAPI: STUN discovered public addr %s", m_publicAddr.str()));
				setState(STATE_READY);
				continue;
			}
		}

		// In PUNCHING state, any packet from the peer's candidate addresses
		// counts as a successful punch. We don't validate contents.
		if (m_state == STATE_PUNCHING && m_peerInfoArmed)
		{
			// recvfrom returns network byte order; convert to host order
			// before storing so callers can pass straight through to the
			// LAN code (which always works in host order).
			m_peerInfo.punchedIP   = ntohl(src.sin_addr.s_addr);
			m_peerInfo.punchedPort = ntohs(src.sin_port);
			DEBUG_LOG(("OnlineCoordinatorAPI: PUNCH OK from %u.%u.%u.%u:%u",
				(src.sin_addr.s_addr) & 0xff,
				(src.sin_addr.s_addr >> 8) & 0xff,
				(src.sin_addr.s_addr >> 16) & 0xff,
				(src.sin_addr.s_addr >> 24) & 0xff,
				m_peerInfo.punchedPort));
			// Send one acknowledgement so the peer also sees inbound traffic
			// quickly even if its first blast was lost.
			char ack[32];
			Int ackLen = snprintf(ack, sizeof(ack), "ACK from %s", m_nick.str());
			if (ackLen > 0)
				sendto(m_udpFd, ack, ackLen, 0, (struct sockaddr*)&src, sizeof(src));
			setState(STATE_PUNCH_OK);
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
		// Capture our local IP for the local_addr hint.
		struct sockaddr_in self;
		int selfLen = sizeof(self);
		if (m_tcpFd != -1 && getsockname(m_tcpFd, (struct sockaddr*)&self, &selfLen) == 0)
		{
			UnsignedInt nbo = self.sin_addr.s_addr;
			AsciiString s;
			s.format("%u.%u.%u.%u:%u",
				(nbo) & 0xff, (nbo>>8) & 0xff, (nbo>>16) & 0xff, (nbo>>24) & 0xff,
				m_udpBoundPort);
			m_localAddr = s;
		}
		setState(STATE_DISCOVERING);
		m_stunNextProbeMs = timeGetTime(); // probe immediately
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
		char tmp[128];
		memset(&m_peerInfo, 0, sizeof(m_peerInfo));
		if (parseStringField(obj, "nick",        tmp, sizeof(tmp))) m_peerInfo.nick       = tmp;
		if (parseStringField(obj, "public_addr", tmp, sizeof(tmp))) m_peerInfo.publicAddr = tmp;
		if (parseStringField(obj, "local_addr",  tmp, sizeof(tmp))) m_peerInfo.localAddr  = tmp;
		if (parseStringField(obj, "role",        tmp, sizeof(tmp))) m_peerInfo.role       = tmp;
		parseIntField(obj, "punch_in_ms", &m_peerInfo.punchInMS);

		m_peerInfoArmed = TRUE;
		UnsignedInt nowMs = timeGetTime();
		m_punchStartMs     = nowMs + (UnsignedInt)m_peerInfo.punchInMS;
		m_punchNextBlastMs = m_punchStartMs;
		m_punchDeadlineMs  = m_punchStartMs + PUNCH_TIMEOUT_MS;
		setState(STATE_PUNCHING);
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
