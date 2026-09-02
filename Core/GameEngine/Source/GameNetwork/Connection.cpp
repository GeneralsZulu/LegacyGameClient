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

#include "GameNetwork/Connection.h"
#include "GameNetwork/networkutil.h"
#include "GameLogic/GameLogic.h"

enum { MaxQuitFlushTime = 30000 }; // wait this many milliseconds at most to retry things before quitting

/**
 * The constructor.
 */
Connection::Connection() {
	m_transport = nullptr;
	m_user = nullptr;
	m_netCommandList = nullptr;
	m_retryTime = 2000; // set retry time to 2 seconds.
	m_lastTimeSent = 0;
	m_frameGrouping = 1;
	m_isQuitting = false;
	m_quitTime = 0;
	m_averageLatency = 0.0f;
	m_srtt = 0.0f;
	m_rttvar = 0.0f;
	m_rttSeeded = FALSE;
	m_adaptiveRetryMs = NET_RETRY_DEFAULT_MS;
	m_resendCount = 0;
	m_redundantCount = 0;
	Int i;
	for(i = 0; i < CONNECTION_LATENCY_HISTORY_LENGTH; i++)
	{
		m_latencies[i] = 0.0f;
	}
}

/**
 * The destructor.
 */
Connection::~Connection() {
	deleteInstance(m_user);
	m_user = nullptr;

	deleteInstance(m_netCommandList);
	m_netCommandList = nullptr;
}

/**
 * Initialize the connection and any subsystems.
 */
void Connection::init() {
	m_transport = nullptr;

	deleteInstance(m_user);
	m_user = nullptr;

	if (m_netCommandList == nullptr) {
		m_netCommandList = newInstance(NetCommandList);
		m_netCommandList->init();
	}
	m_netCommandList->reset();

	m_lastTimeSent = 0;
	m_frameGrouping = 1;
	m_numRetries = 0;
	m_retryMetricsTime = 0;

	for (Int i = 0; i < CONNECTION_LATENCY_HISTORY_LENGTH; ++i) {
		m_latencies[i] = 0;
	}
	m_averageLatency = 0;
	m_srtt = 0.0f;
	m_rttvar = 0.0f;
	m_rttSeeded = FALSE;
	m_adaptiveRetryMs = NET_RETRY_DEFAULT_MS;
	m_resendCount = 0;
	m_redundantCount = 0;
	m_isQuitting = FALSE;
	m_quitTime = 0;
}

/**
 * Take the connection back to the initial state.
 */
void Connection::reset() {
	init();
}

/**
 * Doesn't really do anything.
 */
void Connection::update() {
}

/**
 * Attach the transport object that this connection should use.
 */
void Connection::attachTransport(Transport *transport) {
	m_transport = transport;
}

/**
 * Assign this connection a user.  This is the user to whome we send all our packetized goodies.
 */
void Connection::setUser(User *user) {
	deleteInstance(m_user);
	m_user = user;
}

/**
 * Return the user object.
 */
User * Connection::getUser() {
	return m_user;
}

/**
 * Add this network command to the send queue for this connection.
 * The relay is the mask specifying the people the person we are sending to should send to.
 * The relay mostly has to do with the packet router.
 */
void Connection::sendNetCommandMsg(NetCommandMsg *msg, UnsignedByte relay) {
	static NetPacket *packet = nullptr;

	// this is done so we don't have to allocate and delete a packet every time we send a message.
	if (packet == nullptr) {
		packet = newInstance(NetPacket);
	}


	if (m_isQuitting)
		return;

	if (m_netCommandList != nullptr) {
		// check to see if this command will fit in a packet.  If not, we need to split it up.
		// we are splitting up the command here so that the retry logic will not try to
		// resend the ENTIRE command (i.e. multiple packets work of data) and only do the retry
		// one wrapper command at a time.
		packet->reset();

		NetCommandRef *tempref = NEW_NETCOMMANDREF(msg);

		Bool msgFits = packet->addCommand(tempref);
		deleteInstance(tempref); // delete the temporary reference.
		tempref = nullptr;

		if (!msgFits) {
			NetCommandRef *origref = NEW_NETCOMMANDREF(msg);
			origref->setRelay(relay);
			// the message doesn't fit in a single packet, need to split it up.
			NetPacketList packetList = NetPacket::ConstructBigCommandPacketList(origref);
			NetPacketListIter tempPacketPtr = packetList.begin();

			while (tempPacketPtr != packetList.end()) {
				NetPacket *tempPacket = (*tempPacketPtr);

				NetCommandList *list = tempPacket->getCommandList();
				NetCommandRef *ref1 = list->getFirstMessage();
				while (ref1 != nullptr) {
					NetCommandRef *ref2 = m_netCommandList->addMessage(ref1->getCommand());
					ref2->setRelay(relay);

					ref1 = ref1->getNext();
				}

				deleteInstance(tempPacket);
				tempPacket = nullptr;
				++tempPacketPtr;

				deleteInstance(list);
				list = nullptr;
			}

			deleteInstance(origref);
			origref = nullptr;

			return;
		}

		// the message fits in a packet, add to the command list normally.
		NetCommandRef *ref = m_netCommandList->addMessage(msg);

		if (ref != nullptr) {

/*
#if defined(RTS_DEBUG)
			if (msg->getNetCommandType() == NETCOMMANDTYPE_GAMECOMMAND) {
				DEBUG_LOG(("Connection::sendNetCommandMsg - added game command %d to net command list for frame %d.",
					msg->getID(), msg->getExecutionFrame()));
			} else if (msg->getNetCommandType() == NETCOMMANDTYPE_FRAMEINFO) {
				DEBUG_LOG(("Connection::sendNetCommandMsg - added frame info for frame %d", msg->getExecutionFrame()));
			}
#endif // RTS_DEBUG
*/

			ref->setRelay(relay);
		}
	}
}

void Connection::clearCommandsExceptFrom( Int playerIndex )
{
	NetCommandRef *tmp = m_netCommandList->getFirstMessage();
	while (tmp)
	{
		NetCommandRef *next = tmp->getNext();
		NetCommandMsg *msg = tmp->getCommand();

		if (msg->getPlayerID() != playerIndex)
		{
			DEBUG_LOG(("Connection::clearCommandsExceptFrom(%d) - clearing a command from player %d for frame %d",
				playerIndex, tmp->getCommand()->getPlayerID(), tmp->getCommand()->getExecutionFrame()));

			m_netCommandList->removeMessage(tmp);
			deleteInstance(tmp);
		}

		tmp = next;
	}
}

Bool Connection::isQueueEmpty() {
	if (m_netCommandList->getFirstMessage() == nullptr) {
		return TRUE;
	}
	return FALSE;
}

void Connection::setQuitting()
{
	m_isQuitting = TRUE;
	m_quitTime = timeGetTime();
	DEBUG_LOG(("Connection::setQuitting() at time %d", m_quitTime));
}

/**
 * This is the good part. We take all the network commands queued up for this connection,
 * packetize them and put them on the transport's send queue for actual sending.
 */
UnsignedInt Connection::doSend() {
	Int numpackets = 0;
	// timeGetTime() returns a DWORD (UnsignedInt); on this VC6 build time_t is
	// signed 32-bit, so once the OS uptime exceeds ~24.85 days timeGetTime()
	// passes 2^31 ms and "time_t curtime = timeGetTime()" silently wraps to a
	// negative value. The downstream gate "(curtime - m_lastTimeSent) <
	// m_frameGrouping" then compares as signed (negative < small positive =
	// TRUE) and this function returns 0 forever -- no game packets ever go
	// out. Keep curtime unsigned so subtractions promote everything to
	// UnsignedInt and modular arithmetic gives the correct delta.
	UnsignedInt curtime = timeGetTime();
	Bool couldQueue = TRUE;

	// Do this check first, since it's an important fail-safe
	if (m_isQuitting && curtime > m_quitTime + MaxQuitFlushTime)
	{
		DEBUG_LOG(("Timed out a quitting connection.  Deleting all %d messages", m_netCommandList->length()));
		m_netCommandList->reset();
		return 0;
	}

	if ((curtime - m_lastTimeSent) < m_frameGrouping) {
//		DEBUG_LOG(("not sending packet, time = %d, m_lastFrameSent = %d, m_frameGrouping = %d", curtime, m_lastTimeSent, m_frameGrouping));
		return 0;
	}

	// iterate through all the messages and put them into a packet(s).
	NetCommandRef *msg = m_netCommandList->getFirstMessage();

	while ((msg != nullptr) && couldQueue) {
		NetPacket *packet = newInstance(NetPacket);
		packet->init();
		packet->setAddress(m_user->GetIPAddr(), m_user->GetPort());

		Bool notDone = TRUE;

		// add the command messages until either we run out of messages or the packet is full.
		while ((msg != nullptr) && notDone) {
			NetCommandRef *next = msg->getNext(); // Need this since msg could be deleted

			// Same signed-time_t-overflow concern as the outer curtime: keep
			// the value unsigned so the retry-delta comparison is correct on
			// long-uptime systems.
			UnsignedInt timeLastSent = (UnsignedInt)msg->getTimeLastSent();
			const Bool neverSent = (timeLastSent == (UnsignedInt)-1);

			// Retry interval is per command now: adaptive RTT-based for most,
			// zero (every packet) for frame info that is still ahead of the sim.
			const time_t interval = retryIntervalFor(msg);
			if (neverSent || ((curtime - timeLastSent) >= (UnsignedInt)interval)) {
				notDone = packet->addCommand(msg);
				if (notDone) {
					// the msg command was added to the packet.
					if (CommandRequiresAck(msg->getCommand())) {
						if (!neverSent) {
							++m_numRetries;
							if (interval == 0) {
								++m_redundantCount;
							} else {
								++m_resendCount;
							}
						} else {
							msg->setTimeFirstSent(curtime);
						}
						doRetryMetrics();
						msg->setTimeLastSent(curtime);
					} else {
						m_netCommandList->removeMessage(msg);
						deleteInstance(msg);
					}
				}
			}
			msg = next;
		}

		if (msg != nullptr) {
			DEBUG_LOG(("didn't finish sending all commands in connection"));
		}

		++numpackets;

		/// @todo Make the act of giving the transport object a packet to send more efficient.  Make the transport take a NetPacket object rather than the raw data, thus avoiding an extra memcpy.
		if (packet->getNumCommands() > 0) {
			// If the packet actually has any information to give, give it to the transport object
			// for transmission.
			couldQueue = m_transport->queueSend(packet->getAddr(), packet->getPort(), packet->getData(), packet->getLength());
			m_lastTimeSent = curtime;
		}

		deleteInstance(packet); // delete the packet now that we're done with it.
	}

	return numpackets;
}

NetCommandRef * Connection::processAck(NetAckStage1CommandMsg *msg) {
	return processAck(msg->getCommandID(), msg->getOriginalPlayerID());
}

NetCommandRef * Connection::processAck(NetAckBothCommandMsg *msg) {
	return processAck(msg->getCommandID(), msg->getOriginalPlayerID());
}

NetCommandRef * Connection::processAck(NetCommandMsg *msg) {
	if (msg->getNetCommandType() == NETCOMMANDTYPE_ACKSTAGE1) {
		NetAckStage1CommandMsg *ackmsg = (NetAckStage1CommandMsg *)msg;
		return processAck(ackmsg);
	}

	if (msg->getNetCommandType() == NETCOMMANDTYPE_ACKBOTH) {
		NetAckBothCommandMsg *ackmsg = (NetAckBothCommandMsg *)msg;
		return processAck(ackmsg);
	}

	return nullptr;
}

/**
 * The person we are sending to has ack'd one of the messages we sent him.
 * Take that message off the list of commands to send.
 */
NetCommandRef * Connection::processAck(UnsignedShort commandID, UnsignedByte originalPlayerID) {
	NetCommandRef *temp = m_netCommandList->getFirstMessage();
	while ((temp != nullptr) && ((temp->getCommand()->getID() != commandID) || (temp->getCommand()->getPlayerID() != originalPlayerID))) {

		// cycle through the commands till we find the one we need to remove.
		// Need to check for both the command ID and the player ID.
		temp = temp->getNext();
	}
	if (temp == nullptr) {
		return nullptr;
	}

#if defined(RTS_DEBUG)
	Bool doDebug = FALSE;
	if (temp->getCommand()->getNetCommandType() == NETCOMMANDTYPE_DISCONNECTFRAME) {
		doDebug = TRUE;
	}
#endif

	Int index = temp->getCommand()->getID() % CONNECTION_LATENCY_HISTORY_LENGTH;
	m_averageLatency -= ((Real)(m_latencies[index])) / CONNECTION_LATENCY_HISTORY_LENGTH;
	// Measure from the FIRST send. With frame info repeated in every packet
	// most commands are sent more than once; timing from the last copy would
	// produce tiny samples whenever the original's ack was simply in flight,
	// and the estimate would spiral down into ever more resends.
	const UnsignedInt nowMs = timeGetTime();
	time_t sentAt = temp->getTimeFirstSent();
	if (sentAt == -1) {
		sentAt = temp->getTimeLastSent();
	}
	Real lat = (Real)(nowMs - (UnsignedInt)sentAt);
	m_averageLatency += lat / CONNECTION_LATENCY_HISTORY_LENGTH;
	m_latencies[index] = lat;
	if (temp->getTimeLastSent() != -1) {
		addRttSample(lat);
	}

#if defined(RTS_DEBUG)
	if (doDebug == TRUE) {
		DEBUG_LOG(("Connection::processAck - disconnect frame command %d found, removing from command list.", commandID));
	}
#endif
	m_netCommandList->removeMessage(temp);
	return temp;
}

void Connection::setFrameGrouping(time_t frameGrouping) {
	m_frameGrouping = frameGrouping;
//	m_retryTime = frameGrouping * 4;
}

/**
 * How long a command may sit unacked before it goes out again.
 */
time_t Connection::retryIntervalFor(const NetCommandRef *msg) const {
	if (NET_LEGACY_TIMING) {
		return m_retryTime;
	}
	const NetCommandMsg *cmd = msg->getCommand();
	if (cmd->getNetCommandType() == NETCOMMANDTYPE_FRAMEINFO
			&& (cmd->getExecutionFrame() + 1) >= TheGameLogic->getFrame()) {
		// Frame info gates every frame of the lockstep and repeat-codes down to
		// a couple of bytes, so repeat it in every packet while the frame it
		// describes is still ahead of the sim. One lost packet then costs one
		// send interval instead of a retry timeout. Once the sim has passed the
		// frame the info is only needed to clear the ack, so it falls back to
		// the adaptive interval and the pending set stays bounded.
		return 0;
	}
	return m_adaptiveRetryMs;
}

/**
 * TCP-style smoothed RTT with variance (RFC 6298 shape), clamped for the
 * retry interval. Called from processAck with a sample measured from the
 * command's first send.
 */
void Connection::addRttSample(Real ms) {
	// A sample longer than the retry ceiling is a stalled peer (map load, a
	// disconnect screen), not a round trip; feeding 45s into the estimator
	// pins the retry interval at its ceiling for dozens of samples.
	if (ms > (Real)NET_RETRY_MAX_MS) {
		ms = (Real)NET_RETRY_MAX_MS;
	}
	if (!m_rttSeeded) {
		m_srtt = ms;
		m_rttvar = ms / 2.0f;
		m_rttSeeded = TRUE;
	} else {
		Real err = m_srtt - ms;
		if (err < 0.0f) {
			err = -err;
		}
		m_rttvar = 0.75f * m_rttvar + 0.25f * err;
		m_srtt = 0.875f * m_srtt + 0.125f * ms;
	}
	// Variance floor: acks ride the peer's next send interval and the
	// receiver's next engine loop, so even a clean link jitters by tens of
	// ms; a spurious retry is only a duplicate packet, but keep them rare.
	Real var4 = 4.0f * m_rttvar;
	if (var4 < 50.0f) {
		var4 = 50.0f;
	}
	m_adaptiveRetryMs = clamp<Int>(NET_RETRY_MIN_MS, (Int)(m_srtt + var4), NET_RETRY_MAX_MS);
}

Int Connection::takeResendCount() {
	Int n = m_resendCount;
	m_resendCount = 0;
	return n;
}

Int Connection::takeRedundantCount() {
	Int n = m_redundantCount;
	m_redundantCount = 0;
	return n;
}

void Connection::doRetryMetrics() {
	static Int numSeconds = 0;
	time_t curTime = timeGetTime();

	if ((curTime - m_retryMetricsTime) > 10000) {
		m_retryMetricsTime = curTime;
		++numSeconds;
//		DEBUG_LOG(("Retries in the last 10 seconds = %d, average latency = %fms", m_numRetries, m_averageLatency));
		m_numRetries = 0;
//		m_retryTime = m_averageLatency * 1.5;
	}
}

#if defined(RTS_DEBUG)
void Connection::debugPrintCommands() {
	NetCommandRef *ref = m_netCommandList->getFirstMessage();
	while (ref != nullptr) {
		DEBUG_LOG(("Connection::debugPrintCommands - ID: %d\tType: %s\tRelay: 0x%X for frame %d",
			ref->getCommand()->getID(), GetNetCommandTypeAsString(ref->getCommand()->getNetCommandType()),
			ref->getRelay(), ref->getCommand()->getExecutionFrame()));
		ref = ref->getNext();
	}
}
#endif
