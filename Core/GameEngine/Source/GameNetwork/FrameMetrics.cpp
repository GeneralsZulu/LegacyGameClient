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

/** FrameMetrics.cpp */

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include <numeric>

#include "GameNetwork/FrameMetrics.h"
#include "Common/Recorder.h"
#include "GameClient/Display.h"
#include "GameNetwork/networkutil.h"
#include <algorithm>

FrameMetrics::FrameMetrics()
{
	m_averageFps = 0.0f;
	m_averageLatency = 0.0f;
	m_cushionIndex = 0;
	m_fpsListIndex = 0;
	m_lastFpsTimeThing = 0;
	m_minimumCushion = 0;

	m_pendingLatencies = NEW time_t[MAX_FRAMES_AHEAD];
	for(Int i = 0; i < MAX_FRAMES_AHEAD; i++)
		m_pendingLatencies[i] = 0;
	m_fpsList = NEW Real[TheGlobalData->m_networkFPSHistoryLength];
	m_latencyList = NEW Real[TheGlobalData->m_networkLatencyHistoryLength];
	m_latencyScratch = NEW Real[TheGlobalData->m_networkLatencyHistoryLength];
}

FrameMetrics::~FrameMetrics() {
	delete m_fpsList;
	m_fpsList = nullptr;

	delete m_latencyList;
	m_latencyList = nullptr;

	delete[] m_latencyScratch;
	m_latencyScratch = nullptr;

	delete[] m_pendingLatencies;
	m_pendingLatencies = nullptr;
}

void FrameMetrics::init() {
	m_averageFps = 30;
	m_averageLatency = (Real)0.2;
	m_minimumCushion = -1;

	UnsignedInt i = 0;
	for (; i < TheGlobalData->m_networkFPSHistoryLength; ++i) {
		m_fpsList[i] = 30.0;
	}
	m_fpsListIndex = 0;
	for (i = 0; i < TheGlobalData->m_networkLatencyHistoryLength; ++i) {
		m_latencyList[i] = (Real)0.2;
	}
	m_cushionIndex = 0;
}

void FrameMetrics::reset() {
	init();
}

void FrameMetrics::doPerFrameMetrics(UnsignedInt frame) {
	// Do the measurement of the fps.
	time_t curTime = timeGetTime();
	if ((curTime - m_lastFpsTimeThing) >= 1000) {
		// This history is the input to ConnectionManager::updateRunAhead, i.e. it is
		// how this machine tells the rest of the game how fast it can run. During
		// resume-from-replay catchup the renderer is deliberately decoupled from the
		// logic rate (GameEngine::update draws on a wall-clock cadence while logic
		// fast-forwards), so display FPS measures the throttle, not the machine, and
		// sampling it here would be a lie that the network then acts on: run-ahead
		// collapses to its floor and the derived packet send interval blows out,
		// which is precisely how "skip frames to catch up faster" ends up slower than
		// realtime. Hold the history steady through catchup -- it is seeded to 30 and
		// resumes sampling for real on handoff, so live play re-converges normally.
		const Bool inCatchup = (TheRecorder != nullptr) && TheRecorder->isResumeCatchupMode();
		if (!inCatchup) {
//		if ((m_fpsListIndex % 16) == 0) {
//			DEBUG_LOG(("FrameMetrics::doPerFrameMetrics - adding %f to fps history. average before: %f ", m_fpsList[m_fpsListIndex], m_averageFps));
//		}
			m_averageFps -= ((m_fpsList[m_fpsListIndex])) / TheGlobalData->m_networkFPSHistoryLength; // subtract out the old value from the average.
			m_fpsList[m_fpsListIndex] = TheDisplay->getAverageFPS();
//		m_fpsList[m_fpsListIndex] = TheGameClient->getFrame() - m_fpsStartingFrame;
			m_averageFps += ((Real)(m_fpsList[m_fpsListIndex])) / TheGlobalData->m_networkFPSHistoryLength; // add the new value to the average.
//		DEBUG_LOG(("average after: %f", m_averageFps));
			++m_fpsListIndex;
			m_fpsListIndex %= TheGlobalData->m_networkFPSHistoryLength;
		}
		m_lastFpsTimeThing = curTime;
	}

	Int pendingLatenciesIndex = frame % MAX_FRAMES_AHEAD;
	m_pendingLatencies[pendingLatenciesIndex] = curTime;

}

void FrameMetrics::processLatencyResponse(UnsignedInt frame) {
	time_t curTime = timeGetTime();
	Int pendingIndex = frame % MAX_FRAMES_AHEAD;
	time_t timeDiff = curTime - m_pendingLatencies[pendingIndex];

	Int latencyListIndex = frame % TheGlobalData->m_networkLatencyHistoryLength;
	m_latencyList[latencyListIndex] = (Real)timeDiff / (Real)1000; // convert to seconds from milliseconds.
	const Real latencySum = std::accumulate(m_latencyList, m_latencyList + TheGlobalData->m_networkLatencyHistoryLength, 0.0f);
	m_averageLatency = latencySum / (Real)TheGlobalData->m_networkLatencyHistoryLength;

	if (frame % 16 == 0) {
//		DEBUG_LOG(("ConnectionManager::processFrameInfoAck - average latency = %f", m_averageLatency));
	}
}

void FrameMetrics::addCushion(Int cushion) {
	++m_cushionIndex;
	m_cushionIndex %= TheGlobalData->m_networkCushionHistoryLength;
	if (m_cushionIndex == 0) {
		m_minimumCushion = -1;
	}
	if ((cushion < m_minimumCushion) || (m_minimumCushion == -1)) {
		m_minimumCushion = cushion;
	}
}

Int FrameMetrics::getAverageFPS() {
	return (Int)m_averageFps;
}

Real FrameMetrics::getAverageLatency() {
	return m_averageLatency;
}

Real FrameMetrics::getLatencyPercentile(Int pct) {
	const Int n = TheGlobalData->m_networkLatencyHistoryLength;
	if (n <= 0) {
		return m_averageLatency;
	}
	memcpy(m_latencyScratch, m_latencyList, n * sizeof(Real));
	std::sort(m_latencyScratch, m_latencyScratch + n);
	Int idx = (n * pct) / 100;
	if (idx < 0) {
		idx = 0;
	}
	if (idx > n - 1) {
		idx = n - 1;
	}
	return m_latencyScratch[idx];
}

/**
 * The mean is blind to jitter: a link that averages 80ms but spikes to 300ms
 * every few seconds gets a window sized for 80ms and stalls on every spike.
 * So add the spread between the mean and a high percentile of the history.
 *
 * Two guards keep this from feeding on itself. Packets go out every
 * batchSec (the frame grouping, itself derived from run-ahead), so every
 * sample already carries up to a batch interval of random wait at each end;
 * that spread is subtracted before it counts as jitter, otherwise a bigger
 * window makes bigger "jitter" and run-ahead ratchets to its cap even on a
 * perfect LAN (T1-BASELINE went to 18 frames). And the allowance is capped
 * at NET_RUNAHEAD_JITTER_CAP_MS so a few retry-inflated samples cannot buy
 * the whole game a second of input lag.
 */
Real FrameMetrics::getRunAheadLatency(Real batchSec) {
	if (NET_LEGACY_TIMING) {
		return m_averageLatency;
	}
	Real spread = getLatencyPercentile(NET_RUNAHEAD_LAT_PERCENTILE) - m_averageLatency - batchSec;
	if (spread < 0.0f) {
		spread = 0.0f;
	}
	const Real cap = (Real)NET_RUNAHEAD_JITTER_CAP_MS / 1000.0f;
	if (spread > cap) {
		spread = cap;
	}
	return m_averageLatency + spread;
}

Int FrameMetrics::getMinimumCushion() {
	return m_minimumCushion;
}
