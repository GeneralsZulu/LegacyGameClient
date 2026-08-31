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

#include "Common/crc.h"
#include "Common/ReleaseLog.h"
#include "GameNetwork/Transport.h"
#include "GameNetwork/NetworkInterface.h"
#include "GameNetwork/RelayRegistry.h"

// Keep the coordinator's relay return-address for this socket fresh (and the
// NAT mapping toward the coordinator open) while any relay-capable peer is
// registered on our channel. Comfortably under common 30s UDP idle TTLs.
static const UnsignedInt RELAY_KEEPALIVE_MS = 20000;


//--------------------------------------------------------------------------
// Packet-level encryption is an XOR operation, for speed reasons.  To get
// the max throughput, we only XOR whole 4-byte words, so the last bytes
// can be non-XOR'd.

// This assumes the buf is a multiple of 4 bytes.  Extra is not encrypted.
static inline void encryptBuf( unsigned char *buf, Int len )
{
	UnsignedInt mask = 0x0000Fade;

	UnsignedInt *uintPtr = (UnsignedInt *) (buf);

	for (int i=0 ; i<len/4 ; i++) {
		*uintPtr = (*uintPtr) ^ mask;
		*uintPtr = htonl(*uintPtr);
		uintPtr++;
		mask += 0x00000321; // just for fun
	}
}

// This assumes the buf is a multiple of 4 bytes.  Extra is not encrypted.
static inline void decryptBuf( unsigned char *buf, Int len )
{
	UnsignedInt mask = 0x0000Fade;

	UnsignedInt *uintPtr = (UnsignedInt *) (buf);

	for (int i=0 ; i<len/4 ; i++) {
		*uintPtr = htonl(*uintPtr);
		*uintPtr = (*uintPtr) ^ mask;
		uintPtr++;
		mask += 0x00000321; // just for fun
	}
}

//--------------------------------------------------------------------------

Transport::Transport()
{
	m_winsockInit = false;
	m_udpsock = nullptr;
	m_relayChannel = -1;
	m_relayKeepaliveNextMs = 0;
}

Transport::~Transport()
{
	reset();
}

Bool Transport::init( AsciiString ip, UnsignedShort port )
{
	return init(ResolveIP(ip), port);
}

Bool Transport::init( UnsignedInt ip, UnsignedShort port, Bool logFailure )
{
	// ----- Initialize Winsock -----
	if (!m_winsockInit)
	{
		WORD verReq = MAKEWORD(2, 2);
		WSADATA wsadata;

		int err = WSAStartup(verReq, &wsadata);
		if (err != 0) {
			return false;
		}

		if ((LOBYTE(wsadata.wVersion) != 2) || (HIBYTE(wsadata.wVersion) !=2)) {
			WSACleanup();
			return false;
		}
		m_winsockInit = true;
	}

	// ------- Bind our port --------
	delete m_udpsock;
	m_udpsock = NEW UDP();

	if (!m_udpsock)
		return false;

	int retval = -1;
	time_t now = timeGetTime();
	while ((retval != 0) && ((timeGetTime() - now) < 1000)) {
		retval = m_udpsock->Bind(ip, port);
	}

	if (retval != 0) {
		DEBUG_CRASH(("Could not bind to 0x%8.8X:%d", ip, port));
		DEBUG_LOG(("Transport::init - Failure to bind socket with error code %x", retval));
		// Bind failures are invisible in release builds but leave this
		// transport permanently dead (the caller may ignore our return
		// value), so put the evidence in the uploadable log. status comes
		// from UDP::GetStatus(), which passes the raw winsock error through
		// when it has no sockStat for it -- 10048 is WSAEADDRINUSE (another
		// process owns the port, typically a zombie game instance).
		//
		// Unless the caller told us this bind was speculative: see the
		// logFailure note on the declaration. LANAPI::init()'s bind races the
		// coordinator socket by design and is superseded a moment later, so
		// logging it here just cries wolf in every online player's log.
		if (logFailure)
		{
			ReleaseLog("Transport bind FAILED on %d.%d.%d.%d:%d status=%d",
				(ip>>24)&0xff, (ip>>16)&0xff, (ip>>8)&0xff, ip&0xff, port, retval);
		}
		delete m_udpsock;
		m_udpsock = nullptr;
		return false;
	}

	return finishInit(port);
}

// Takes ownership of fd: on failure the fd is closed here so the caller can
// immediately re-bind the same port with init(). Failures go to ReleaseLog
// for the same reason as the bind failure in init() above -- a dead in-game
// transport is otherwise invisible in release builds.
Bool Transport::initFromFD( Int fd, UnsignedInt ip, UnsignedShort port )
{
	if (!m_winsockInit)
	{
		WORD verReq = MAKEWORD(2, 2);
		WSADATA wsadata;
		Bool wsOk = (WSAStartup(verReq, &wsadata) == 0);
		if (wsOk && ((LOBYTE(wsadata.wVersion) != 2) || (HIBYTE(wsadata.wVersion) != 2))) {
			WSACleanup();
			wsOk = false;
		}
		if (!wsOk)
		{
			ReleaseLog("Transport adopt FAILED on port %d: WSAStartup", port);
			if (fd >= 0)
			{
#ifdef _WIN32
				closesocket(fd);
#else
				close(fd);
#endif
			}
			return false;
		}
		m_winsockInit = true;
	}

	delete m_udpsock;
	m_udpsock = NEW UDP();
	if (!m_udpsock || m_udpsock->AdoptFD(fd, ip, port) != 0)
	{
		// AdoptFD only rejects fd < 0, so there is nothing to close here.
		ReleaseLog("Transport adopt FAILED on port %d: bad fd=%d", port, fd);
		delete m_udpsock;
		m_udpsock = nullptr;
		return false;
	}
	DEBUG_LOG(("Transport::initFromFD - adopted fd=%d as 0x%08X:%u", fd, ip, port));
	return finishInit(port);
}

// See setPunchTTL in OnlineCoordinatorAPI.cpp for why both winsock TTL
// option values are set: which one the stack honors depends on which
// winsock import library won at link time; the other is a harmless no-op.
static void setSockTTL(Int fd, Int ttl)
{
	if (fd < 0) return;
#ifdef _WIN32
	setsockopt(fd, IPPROTO_IP, 4, (const char*)&ttl, sizeof(ttl));
	setsockopt(fd, IPPROTO_IP, 7, (const char*)&ttl, sizeof(ttl));
#else
	setsockopt(fd, IPPROTO_IP, IP_TTL, (const char*)&ttl, sizeof(ttl));
#endif
}

Bool Transport::sendNATProbe( UnsignedInt ip, UnsignedShort port, Int ttl )
{
	if (!m_udpsock)
		return false;
	static const unsigned char probe[8] = { 'Z','P','R','O','B','E',0,0 };
	setSockTTL(m_udpsock->GetFD(), ttl);
	m_udpsock->Write(probe, sizeof(probe), ip, port);
	setSockTTL(m_udpsock->GetFD(), 128);
	return true;
}

Int Transport::getRawFD() const
{
	if (!m_udpsock)
		return -1;
	return m_udpsock->GetFD();
}

Bool Transport::finishInit( UnsignedShort port )
{
	// ------- Clear buffers --------
	int i=0;
	for (; i<MAX_MESSAGES; ++i)
	{
		m_outBuffer[i].length = 0;
		m_inBuffer[i].length = 0;
#if defined(RTS_DEBUG)
		m_delayedInBuffer[i].message.length = 0;
#endif
	}
	for (i=0; i<MAX_TRANSPORT_STATISTICS_SECONDS; ++i)
	{
		m_incomingBytes[i] = 0;
		m_outgoingBytes[i] = 0;
		m_unknownBytes[i] = 0;
		m_incomingPackets[i] = 0;
		m_outgoingPackets[i] = 0;
		m_unknownPackets[i] = 0;
	}
	m_statisticsSlot = 0;
	m_lastSecond = timeGetTime();

	m_port = port;

#if defined(RTS_DEBUG)
	if (TheGlobalData->m_latencyAverage > 0 || TheGlobalData->m_latencyNoise)
		m_useLatency = true;

	if (TheGlobalData->m_packetLoss)
		m_usePacketLoss = true;
#endif

	return true;
}

void Transport::reset()
{
	delete m_udpsock;
	m_udpsock = nullptr;
	m_relayChannel = -1;
	m_relayKeepaliveNextMs = 0;

	if (m_winsockInit)
	{
		WSACleanup();
		m_winsockInit = false;
	}
}

Bool Transport::update()
{
	Bool retval = TRUE;
	if (doRecv() == FALSE && m_udpsock && m_udpsock->GetStatus() == UDP::ADDRNOTAVAIL)
	{
		retval = FALSE;
	}
	DEBUG_ASSERTLOG(retval, ("WSA error is %s", GetWSAErrorString(WSAGetLastError()).str()));
	if (doSend() == FALSE && m_udpsock && m_udpsock->GetStatus() == UDP::ADDRNOTAVAIL)
	{
		retval = FALSE;
	}
	DEBUG_ASSERTLOG(retval, ("WSA error is %s", GetWSAErrorString(WSAGetLastError()).str()));
	return retval;
}

Bool Transport::doSend() {
	if (!m_udpsock)
	{
		DEBUG_LOG(("Transport::doSend() - m_udpSock is null!"));
		return FALSE;
	}

	Bool retval = TRUE;

	// Statistics gathering
	UnsignedInt now = timeGetTime();
	if (m_lastSecond + 1000 < now)
	{
		m_lastSecond = now;
		m_statisticsSlot = (m_statisticsSlot + 1) % MAX_TRANSPORT_STATISTICS_SECONDS;
		m_outgoingPackets[m_statisticsSlot] = 0;
		m_outgoingBytes[m_statisticsSlot] = 0;
		m_incomingPackets[m_statisticsSlot] = 0;
		m_incomingBytes[m_statisticsSlot] = 0;
		m_unknownPackets[m_statisticsSlot] = 0;
		m_unknownBytes[m_statisticsSlot] = 0;
	}

	// Relay keepalive: while this socket has relay-capable peers, the
	// coordinator must always hold a fresh return address for it, or the
	// first relayed frame of a mid-game failover has nowhere to land.
	if (m_relayChannel >= 0 && RelayRegistry::isActive() &&
	    RelayRegistry::anyPeerOnChannel(m_relayChannel))
	{
		if (m_relayKeepaliveNextMs == 0 || now >= m_relayKeepaliveNextMs)
		{
			unsigned char kaBuf[RelayRegistry::RELAY_DATA_HEADER_SIZE];
			Int kaLen = 0;
			UnsignedInt coordIP = 0;
			UnsignedShort coordPort = 0;
			if (RelayRegistry::buildKeepalive(m_relayChannel, kaBuf, sizeof(kaBuf), &kaLen, &coordIP, &coordPort))
				m_udpsock->Write(kaBuf, kaLen, coordIP, coordPort);
			m_relayKeepaliveNextMs = now + RELAY_KEEPALIVE_MS;
			// NETPATH heartbeat: one greppable line per keepalive tick tells
			// the test harness (and a post-mortem) which path each channel is
			// on without decoding packets.
			Int totalPeers = 0;
			Int relayedPeers = 0;
			RelayRegistry::countPeers(m_relayChannel, &totalPeers, &relayedPeers);
			ReleaseLog("NETPATH ch=%s peers=%d relayed=%d out_pps=%d in_pps=%d",
				m_relayChannel == RelayRegistry::CHANNEL_GAME ? "game" : "lobby",
				totalPeers, relayedPeers,
				(Int)getOutgoingPacketsPerSecond(), (Int)getIncomingPacketsPerSecond());
		}
	}

	// Scratch space for relay-wrapped sends (header + encrypted packet).
	unsigned char relayBuf[MAX_NETWORK_MESSAGE_LEN + RelayRegistry::RELAY_DATA_HEADER_SIZE];

	// Send all messages
	int i;
	for (i=0; i<MAX_MESSAGES; ++i)
	{
		if (m_outBuffer[i].length != 0)
		{
			int bytesSent = 0;
			// TheSuperHackers @info The handling of data sizing of the payload within a UDP packet is confusing due to the current networking implementation
			// The max game packet size needs to be smaller than max udp payload by sizeof(TransportMessageHeader)
			// But the max network message size needs to include the bytes of the transport message header and equal the max udp payload
			// Therefore, transmitted data needs to add the extra bytes of the network header to the payloads length
			int bytesToSend = m_outBuffer[i].length + sizeof(TransportMessageHeader);
			// A peer flipped to relay gets the identical (encrypted) packet
			// wrapped in a relay header and sent to the coordinator instead;
			// the peer's shim unwraps it and sees the same bytes from our
			// logical address. wrapForSend also owns the silence-trigger
			// flip decision for direct peers.
			Int relayLen = 0;
			UnsignedInt relayIP = 0;
			UnsignedShort relayPort = 0;
			Bool probeDirect = FALSE;
			if (RelayRegistry::isActive() &&
			    RelayRegistry::wrapForSend(m_outBuffer[i].addr, m_outBuffer[i].port,
			        (unsigned char *)(&m_outBuffer[i]), bytesToSend,
			        relayBuf, sizeof(relayBuf), &relayLen, &relayIP, &relayPort, &probeDirect))
			{
				bytesSent = m_udpsock->Write(relayBuf, relayLen, relayIP, relayPort);
				// Direct-upgrade probe: also send the plain packet to the
				// peer. A duplicate datagram is harmless to lockstep; a
				// delivered one is the evidence that unflips the pair.
				if (probeDirect)
					m_udpsock->Write((unsigned char *)(&m_outBuffer[i]), bytesToSend,
						m_outBuffer[i].addr, m_outBuffer[i].port);
				// Normalize for the queue/statistics logic below, which
				// compares against the unwrapped size.
				if (bytesSent == relayLen)
					bytesSent = bytesToSend;
			}
			else
			{
				// Send this message
				bytesSent = m_udpsock->Write((unsigned char *)(&m_outBuffer[i]), bytesToSend, m_outBuffer[i].addr, m_outBuffer[i].port);
			}
			if (bytesSent > 0)
			{
				m_outgoingPackets[m_statisticsSlot]++;
				m_outgoingBytes[m_statisticsSlot] += m_outBuffer[i].length + sizeof(TransportMessageHeader);
				m_outBuffer[i].length = 0;  // Remove from queue
				if (bytesSent != bytesToSend)
				{
					DEBUG_LOG(("Transport::doSend - wanted to send %d bytes, only sent %d bytes to %d.%d.%d.%d:%d",
						bytesToSend, bytesSent,
						PRINTF_IP_AS_4_INTS(m_outBuffer[i].addr), m_outBuffer[i].port));
				}
			}
			else
			{
				//DEBUG_LOG(("Could not write to socket!!!  Not discarding message!"));
				retval = FALSE;
				//DEBUG_LOG(("Transport::doSend returning FALSE"));
			}
		}
	}

#if defined(RTS_DEBUG)
	// Latency simulation - deliver anything we're holding on to that is ready
	if (m_useLatency)
	{
		for (i=0; i<MAX_MESSAGES; ++i)
		{
			if (m_delayedInBuffer[i].message.length != 0 && m_delayedInBuffer[i].deliveryTime <= now)
			{
				for (int j=0; j<MAX_MESSAGES; ++j)
				{
					if (m_inBuffer[j].length == 0)
					{
						// Empty slot; use it
						memcpy(&m_inBuffer[j], &m_delayedInBuffer[i].message, sizeof(TransportMessage));
						m_delayedInBuffer[i].message.length = 0;
						break;
					}
				}
			}
		}
	}
#endif
	return retval;
}

Bool Transport::doRecv()
{
	if (!m_udpsock)
	{
		DEBUG_LOG(("Transport::doRecv() - m_udpSock is null!"));
		return FALSE;
	}

	Bool retval = TRUE;

	// Read in anything on our socket
	sockaddr_in from;
#if defined(RTS_DEBUG)
	UnsignedInt now = timeGetTime();
#endif
	// TheSuperHackers @info The handling of data sizing of the payload within a UDP packet is confusing due to the current networking implementation
	// The max game packet size needs to be smaller than max udp payload by sizeof(TransportMessageHeader)
	// But the max network message size needs to include the bytes of the transport message header and equal the max udp payload
	// Therefore, when receiving data we use the max udp payload size to receive the game packet payload and network header
	TransportMessage incomingMessage;
	unsigned char *buf = (unsigned char *)&incomingMessage;
	int len = MAX_NETWORK_MESSAGE_LEN;
//	DEBUG_LOG(("Transport::doRecv - checking"));
	while ( (len=m_udpsock->Read(buf, MAX_NETWORK_MESSAGE_LEN, &from)) > 0 )
	{
#if defined(RTS_DEBUG)
		// Packet loss simulation
		if (m_usePacketLoss)
		{
			if ( TheGlobalData->m_packetLoss >= GameClientRandomValue(0, 100) )
			{
				continue;
			}
		}
#endif

		// Relay shim. Frames from the coordinator's UDP port are either
		// relayed peer packets (unwrap in place and present them as coming
		// from the peer's logical address, so everything downstream matches
		// the direct path) or STUN keepalive replies (drop). Any other
		// source is a normal packet; note it so the silence trigger knows
		// the direct path from that peer is alive.
		if (RelayRegistry::isActive())
		{
			UnsignedInt   srcIPHost   = ntohl(from.sin_addr.S_un.S_addr);
			UnsignedShort srcPortHost = ntohs(from.sin_port);
			Int newLen = 0;
			UnsignedInt logicalIP = 0;
			UnsignedShort logicalPort = 0;
			Bool dropIt = FALSE;
			if (RelayRegistry::handleIncoming(srcIPHost, srcPortHost, buf, len,
					&newLen, &logicalIP, &logicalPort, &dropIt))
			{
				if (dropIt)
				{
					m_unknownPackets[m_statisticsSlot]++;
					m_unknownBytes[m_statisticsSlot] += len;
					continue;
				}
				len = newLen;
				from.sin_addr.S_un.S_addr = htonl(logicalIP);
				from.sin_port = htons(logicalPort);
			}
			else
			{
				RelayRegistry::noteDirectRecv(srcIPHost, srcPortHost);
			}
		}

//		DEBUG_LOG(("Transport::doRecv - Got something! len = %d", len));
		// Decrypt the packet
//		DEBUG_LOG_RAW(("buffer = "));
//		for (Int munkee = 0; munkee < len; ++munkee) {
//			DEBUG_LOG_RAW(("%02x", *(buf + munkee)));
//		}
//		DEBUG_LOG_RAW(("\n"));
		decryptBuf(buf, len);

		incomingMessage.length = len - sizeof(TransportMessageHeader);

		if (len <= sizeof(TransportMessageHeader) || !isGeneralsPacket( &incomingMessage ))
		{
			DEBUG_LOG(("Transport::doRecv - unknownPacket! len = %d", len));
			m_unknownPackets[m_statisticsSlot]++;
			m_unknownBytes[m_statisticsSlot] += len;
			continue;
		}

		// Something there; stick it somewhere
//		DEBUG_LOG(("Saw %d bytes from %d:%d", len, ntohl(from.sin_addr.S_un.S_addr), ntohs(from.sin_port)));
		m_incomingPackets[m_statisticsSlot]++;
		m_incomingBytes[m_statisticsSlot] += len;

		for (int i=0; i<MAX_MESSAGES; ++i)
		{
#if defined(RTS_DEBUG)
			// Latency simulation
			if (m_useLatency)
			{
				if (m_delayedInBuffer[i].message.length == 0)
				{
					// Empty slot; use it
					m_delayedInBuffer[i].deliveryTime =
						now + TheGlobalData->m_latencyAverage +
						(Int)(TheGlobalData->m_latencyAmplitude * sin(now * TheGlobalData->m_latencyPeriod)) +
						GameClientRandomValue(-TheGlobalData->m_latencyNoise, TheGlobalData->m_latencyNoise);
					m_delayedInBuffer[i].message.length = incomingMessage.length;
					m_delayedInBuffer[i].message.addr = ntohl(from.sin_addr.S_un.S_addr);
					m_delayedInBuffer[i].message.port = ntohs(from.sin_port);
					memcpy(&m_delayedInBuffer[i].message, buf, len);
					break;
				}
			}
			else
			{
#endif
				if (m_inBuffer[i].length == 0)
				{
					// Empty slot; use it
					m_inBuffer[i].length = incomingMessage.length;
					m_inBuffer[i].addr = ntohl(from.sin_addr.S_un.S_addr);
					m_inBuffer[i].port = ntohs(from.sin_port);
					memcpy(&m_inBuffer[i], buf, len);
					break;
				}
#if defined(RTS_DEBUG)
			}
#endif
		}
		//DEBUG_ASSERTCRASH(i<MAX_MESSAGES, ("Message lost!"));
	}

	if (len == -1) {
		// there was a socket error trying to perform a read.
		//DEBUG_LOG(("Transport::doRecv returning FALSE"));
		retval = FALSE;
	}

	return retval;
}

Bool Transport::queueSend(UnsignedInt addr, UnsignedShort port, const UnsignedByte *buf, Int len /*,
						  NetMessageFlags flags, Int id */)
{
	int i;

	if (len < 1 || len > MAX_PACKET_SIZE)
	{
		DEBUG_LOG(("Transport::queueSend - Invalid Packet size"));
		return false;
	}

	for (i=0; i<MAX_MESSAGES; ++i)
	{
		if (m_outBuffer[i].length == 0)
		{
			// Insert data here
			m_outBuffer[i].length = len;
			memcpy(m_outBuffer[i].data, buf, len);
			m_outBuffer[i].addr = addr;
			m_outBuffer[i].port = port;
//			m_outBuffer[i].header.flags = flags;
//			m_outBuffer[i].header.id = id;
			m_outBuffer[i].header.magic = GENERALS_MAGIC_NUMBER;

			CRC crc;
			crc.computeCRC( (unsigned char *)(&(m_outBuffer[i].header.magic)), m_outBuffer[i].length + sizeof(TransportMessageHeader) - sizeof(UnsignedInt) );
//			DEBUG_LOG(("About to assign the CRC for the packet"));
			m_outBuffer[i].header.crc = crc.get();

			// Encrypt packet
//			DEBUG_LOG(("buffer: "));
			encryptBuf((unsigned char *)&m_outBuffer[i], len + sizeof(TransportMessageHeader));
//			DEBUG_LOG((""));

			return true;
		}
	}
	DEBUG_LOG(("Send Queue is getting full, dropping packets"));
	return false;
}

Bool Transport::isGeneralsPacket( TransportMessage *msg )
{
	if (!msg)
		return false;

	if (msg->length < 0 || msg->length > MAX_NETWORK_MESSAGE_LEN)
		return false;

	CRC crc;
//	crc.computeCRC( (unsigned char *)msg->data, msg->length );
	crc.computeCRC( (unsigned char *)(&(msg->header.magic)), msg->length + sizeof(TransportMessageHeader) - sizeof(UnsignedInt) );

	if (crc.get() != msg->header.crc)
		return false;

	if (msg->header.magic != GENERALS_MAGIC_NUMBER)
		return false;

	return true;
}

// Statistics ---------------------------------------------------
Real Transport::getIncomingBytesPerSecond()
{
	Real val = 0.0;
	for (int i=0; i<MAX_TRANSPORT_STATISTICS_SECONDS; ++i)
	{
		if (i != m_statisticsSlot)
			val += m_incomingBytes[i];
	}
	return val / (MAX_TRANSPORT_STATISTICS_SECONDS-1);
}

Real Transport::getIncomingPacketsPerSecond()
{
	Real val = 0.0;
	for (int i=0; i<MAX_TRANSPORT_STATISTICS_SECONDS; ++i)
	{
		if (i != m_statisticsSlot)
			val += m_incomingPackets[i];
	}
	return val / (MAX_TRANSPORT_STATISTICS_SECONDS-1);
}

Real Transport::getOutgoingBytesPerSecond()
{
	Real val = 0.0;
	for (int i=0; i<MAX_TRANSPORT_STATISTICS_SECONDS; ++i)
	{
		if (i != m_statisticsSlot)
			val += m_outgoingBytes[i];
	}
	return val / (MAX_TRANSPORT_STATISTICS_SECONDS-1);
}

Real Transport::getOutgoingPacketsPerSecond()
{
	Real val = 0.0;
	for (int i=0; i<MAX_TRANSPORT_STATISTICS_SECONDS; ++i)
	{
		if (i != m_statisticsSlot)
			val += m_outgoingPackets[i];
	}
	return val / (MAX_TRANSPORT_STATISTICS_SECONDS-1);
}

Real Transport::getUnknownBytesPerSecond()
{
	Real val = 0.0;
	for (int i=0; i<MAX_TRANSPORT_STATISTICS_SECONDS; ++i)
	{
		if (i != m_statisticsSlot)
			val += m_unknownBytes[i];
	}
	return val / (MAX_TRANSPORT_STATISTICS_SECONDS-1);
}

Real Transport::getUnknownPacketsPerSecond()
{
	Real val = 0.0;
	for (int i=0; i<MAX_TRANSPORT_STATISTICS_SECONDS; ++i)
	{
		if (i != m_statisticsSlot)
			val += m_unknownPackets[i];
	}
	return val / (MAX_TRANSPORT_STATISTICS_SECONDS-1);
}



