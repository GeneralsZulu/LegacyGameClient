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

#include "Common/Recorder.h"
#include "Common/file.h"
#include "Common/FileSystem.h"
#include "Common/FramePacer.h"
#include "Common/PlayerList.h"
#include "Common/Player.h"
#include "Common/GlobalData.h"
#include "Common/GameEngine.h"
#include "GameClient/ClientInstance.h"
#include "GameClient/GameWindow.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/InGameUI.h"
#include "GameClient/MapUtil.h"
#include "GameClient/Shell.h"
#include "GameClient/GameText.h"

#include "GameNetwork/FileTransfer.h"
#include "GameNetwork/GameInfo.h"
#include "GameNetwork/LANAPICallbacks.h"
#include "GameNetwork/MapDownloadHook.h"
#include "GameNetwork/GameMessageParser.h"
#include "GameNetwork/GameSpy/PeerDefs.h"
#include "GameNetwork/networkutil.h"
#include "GameLogic/GameLogic.h"
#include "Common/RandomValue.h"
#include "Common/CRCDebug.h"
#include "Common/ReleaseLog.h"
#include "Common/OptionPreferences.h"
#include "Common/StatsExporter.h"
#include "Common/StatsUploader.h"
#include "Common/version.h"

constexpr const char s_genrep[] = "GENREP";
constexpr const UnsignedInt replayBufferBytes = 8192;

Int REPLAY_CRC_INTERVAL = 100;

const char *replayExtention = ".rep";
const char *lastReplayFileName = "00000000";	// a name the user is unlikely to ever type, but won't cause panic & confusion

// TheSuperHackers @tweak helmutbuhler 25/04/2025
// The replay header contains two time fields; startTime and endTime of type time_t.
// time_t is 32 bit wide on VC6, but on newer compilers it is 64 bit wide.
// In order to remain compatible we need to load and save time values with 32 bits.
// Note that this will overflow on January 18, 2038. @todo Upgrade to 64 bits when we break compatibility.
typedef int32_t replay_time_t;

static time_t startTime;
static const UnsignedInt startTimeOffset = 6;
static const UnsignedInt endTimeOffset = startTimeOffset + sizeof(replay_time_t);
static const UnsignedInt frameCountOffset = endTimeOffset + sizeof(replay_time_t);
static const UnsignedInt desyncOffset = frameCountOffset + sizeof(UnsignedInt);
static const UnsignedInt quitEarlyOffset = desyncOffset + sizeof(Bool);
static const UnsignedInt disconOffset = quitEarlyOffset + sizeof(Bool);

static void writeAtOffset(File* file, Int offset, const void* data, Int dataSize)
{
	UnsignedInt fileSize = file->size();
	DEBUG_ASSERTCRASH((UnsignedInt)(offset + dataSize) <= fileSize, ("writeAtOffset would exceed file size!"));
	if (file->seek(offset, File::seekMode::START) == offset)
	{
		file->write(data, dataSize);
	}
	MAYBE_UNUSED Int res = file->seek(fileSize, File::seekMode::START);
	(void)res;
	DEBUG_ASSERTCRASH(res == fileSize, ("Could not seek to end of file!"));
}

#if defined(RTS_DEBUG)
static FILE* openStatsLogFile()
{
	unsigned long bufSize = MAX_COMPUTERNAME_LENGTH + 1;
	char computerName[MAX_COMPUTERNAME_LENGTH + 1];
	if (!GetComputerName(computerName, &bufSize))
	{
		strcpy(computerName, "unknown");
	}
	AsciiString statsFile = TheGlobalData->m_baseStatsDir;
	statsFile.concat(computerName);
	statsFile.concat(".txt");
	return fopen(statsFile.str(), "a+");
}
#endif

void RecorderClass::logGameStart(AsciiString options)
{
	if (!m_file)
		return;

	time(&startTime);
	replay_time_t tmp = (replay_time_t)startTime;
	writeAtOffset(m_file, startTimeOffset, &tmp, sizeof(tmp));

#if defined(RTS_DEBUG)
	if (TheNetwork && TheGlobalData->m_saveStats)
	{
		TheFileSystem->createDirectory(TheGlobalData->m_baseStatsDir);
		FILE *logFP = openStatsLogFile();
		if (!logFP)
		{
			TheWritableGlobalData->m_baseStatsDir = TheGlobalData->getPath_UserData();
			logFP = openStatsLogFile();
		}
		if (logFP)
		{
			struct tm *t2 = localtime(&startTime);
			fprintf(logFP, "\nGame start at %s\tOptions are %s\n", asctime(t2), options.str());
			fclose(logFP);
		}
	}
#endif
}

void RecorderClass::logPlayerDisconnect(UnicodeString player, Int slot)
{
	if (!m_file)
		return;

	DEBUG_ASSERTCRASH((slot >= 0) && (slot < MAX_SLOTS), ("Attempting to disconnect an invalid slot number"));
	if ((slot < 0) || (slot >= (MAX_SLOTS)))
	{
		return;
	}
	Bool flag = TRUE;
	Int playerSlotDisconOffset = disconOffset + slot * sizeof(Bool);
	writeAtOffset(m_file, playerSlotDisconOffset, &flag, sizeof(flag));

#if defined(RTS_DEBUG)
	if (TheGlobalData->m_saveStats)
	{
		FILE *logFP = openStatsLogFile();
		if (logFP)
		{
			time_t t;
			time(&t);
			struct tm *t2 = localtime(&t);
			fprintf(logFP, "\tPlayer %ls dropped at %s", player.str(), asctime(t2));
			fclose(logFP);
		}
	}
#endif
}

void RecorderClass::logCRCMismatch()
{
	if (!m_file)
		return;

	Bool flag = TRUE;
	writeAtOffset(m_file, desyncOffset, &flag, sizeof(flag));

#if defined(RTS_DEBUG)
	if (TheGlobalData->m_saveStats)
	{
		m_wasDesync = TRUE;
		FILE *logFP = openStatsLogFile();
		if (logFP)
		{
			time_t t;
			time(&t);
			struct tm *t2 = localtime(&t);
			fprintf(logFP, "\tCRC mismatch at %s", asctime(t2));
			fclose(logFP);
		}
	}
#endif
}

void RecorderClass::logGameEnd()
{
	if (!m_file)
		return;

	time_t t;
	time(&t);
	UnsignedInt frameCount = TheGameLogic->getFrame();
	replay_time_t tmp = (replay_time_t)t;
	writeAtOffset(m_file, endTimeOffset, &tmp, sizeof(tmp));
	writeAtOffset(m_file, frameCountOffset, &frameCount, sizeof(frameCount));

#if defined(RTS_DEBUG)
	if (TheNetwork && TheGlobalData->m_saveStats)
	{
		FILE *logFP = openStatsLogFile();
		if (logFP)
		{
			struct tm *t2 = localtime(&t);
			time_t duration = t - startTime;
			Int minutes = duration/60;
			Int seconds = duration%60;
			fprintf(logFP, "Game end at   %s(%d:%2.2d elapsed time)\n", asctime(t2), minutes, seconds);
			fclose(logFP);
		}
	}
#endif
}

void RecorderClass::cleanUpReplayFile()
{
#if defined(RTS_DEBUG)
	if (TheGlobalData->m_saveStats)
	{
		char fname[_MAX_PATH+1];
		strlcpy(fname, TheGlobalData->m_baseStatsDir.str(), ARRAY_SIZE(fname));
		strlcat(fname, m_fileName.str(), ARRAY_SIZE(fname));
		DEBUG_LOG(("Saving replay to %s", fname));
		AsciiString oldFname;
		oldFname.format("%s%s", getReplayDir().str(), m_fileName.str());
		CopyFile(oldFname.str(), fname, TRUE);

#ifdef DEBUG_LOGGING
		const char* logFileName = DebugGetLogFileName();
		if (logFileName[0] == '\0')
			return;

		AsciiString debugFname = fname;
		debugFname.truncateBy(3);
		debugFname.concat("txt");
		UnsignedInt fileSize = 0;
		FILE *fp = fopen(logFileName, "rb");
		if (fp)
		{
			fseek(fp, 0, SEEK_END);
			fileSize = ftell(fp);
			fclose(fp);
			fp = nullptr;
			DEBUG_LOG(("Log file size was %d", fileSize));
		}

		const int MAX_DEBUG_SIZE = 65536;
		if (fileSize <= MAX_DEBUG_SIZE || TheGlobalData->m_saveAllStats)
		{
			DEBUG_LOG(("Using CopyFile to copy %s", logFileName));
			CopyFile(logFileName, debugFname.str(), TRUE);
		}
		else
		{
			DEBUG_LOG(("manual copy of %s", logFileName));
			FILE *ifp = fopen(logFileName, "rb");
			FILE *ofp = fopen(debugFname.str(), "wb");
			if (ifp && ofp)
			{
				fseek(ifp, fileSize-MAX_DEBUG_SIZE, SEEK_SET);
				char buf[4096];
				Int len;
				while ( (len=fread(buf, 1, 4096, ifp)) > 0 )
				{
					fwrite(buf, 1, len, ofp);
				}
				fclose(ofp);
				fclose(ifp);
				ifp = nullptr;
				ofp = nullptr;
			}
			else
			{
				if (ifp) fclose(ifp);
				if (ofp) fclose(ofp);
				ifp = nullptr;
				ofp = nullptr;
			}
		}
#endif // DEBUG_LOGGING
	}
#endif
}

/**
 * The recorder object.
 */
RecorderClass *TheRecorder = nullptr;

/**
 * Constructor
 */
RecorderClass::RecorderClass()
{
	m_originalGameMode = GAME_NONE;
	m_mode = RECORDERMODETYPE_RECORD;
	m_file = nullptr;
	m_fileName.clear();
	m_currentFilePosition = 0;
	m_doingAnalysis = FALSE;
	m_archiveReplays = FALSE;
	m_nextFrame = 0;
	m_wasDesync = FALSE;
	m_replayAIFeatureVersion = ZULU_AI_FEATURE_CURRENT;
	m_replayEpoch = REPLAY_EPOCH_CURRENT;
	m_replayLegacyUpgradeKeyDelta = 0;
	m_playbackStaleObjectRefs = 0;
	m_liveObserverStreamOpen      = FALSE;
	m_liveObserverWaitingForBytes = FALSE;
	m_liveObserverArming          = FALSE;
	m_liveObserverRetryPos        = 0;
	m_liveObserverFpsBoosted      = FALSE;
	m_liveObserverSavedFpsLimit   = 0;
	m_replayShortRead             = FALSE;
	m_resumeRecordPos             = 0;
	m_liveObserverStarvedSinceMs  = 0;
	init(); // just for the heck of it.
}

/**
 * Destructor
 */
RecorderClass::~RecorderClass() {
}

//----------------------------------------------------------------------------------------------------------
// Zulu replay extension block helpers.
//----------------------------------------------------------------------------------------------------------
void RecorderClass::writeZuluReplayExtension()
{
	const char zuluMagic[4] = {'Z','U','L','U'};
	m_file->write(zuluMagic, sizeof(zuluMagic));
	UnsignedInt version = ZULU_AI_FEATURE_CURRENT;
	m_file->write(&version, sizeof(version));
	m_replayAIFeatureVersion = version;
}

void RecorderClass::readZuluReplayExtension()
{
	// Try to consume the extension block. Vanilla / pre-Zulu replays will
	// fail the magic check; rewind so the bytes we peeked at are interpreted
	// as the start of the command stream, as before.
	char magic[4] = {0,0,0,0};
	Int pos = m_file->position();
	Int got = m_file->read(magic, sizeof(magic));
	if (got == (Int)sizeof(magic)
		&& magic[0] == 'Z' && magic[1] == 'U' && magic[2] == 'L' && magic[3] == 'U')
	{
		UnsignedInt version = 0;
		m_file->read(&version, sizeof(version));
		m_replayAIFeatureVersion = version;
		DEBUG_LOG(("RecorderClass: Zulu replay extension version %u", version));
	}
	else
	{
		m_file->seek(pos, File::START);
		m_replayAIFeatureVersion = ZULU_AI_FEATURE_NONE;
		DEBUG_LOG(("RecorderClass: vanilla replay; Zulu AI features disabled for playback"));
	}
}

Bool RecorderClass::isAIFeatureEnabled(UnsignedInt featureVersion) const
{
	if (m_mode == RECORDERMODETYPE_PLAYBACK
		|| m_mode == RECORDERMODETYPE_SIMULATION_PLAYBACK
		|| m_mode == RECORDERMODETYPE_RESUME_CATCHUP)
	{
		return m_replayAIFeatureVersion >= featureVersion;
	}
	return TRUE;
}

//----------------------------------------------------------------------------------------------------------
// Replay determinism epoch.
//----------------------------------------------------------------------------------------------------------
Int RecorderClass::s_replayEpochOverride = -1;

// Map a semantic version to its determinism epoch. Only Zulu builds after the
// version-string switch render "major.minor.patch" (e.g. "1.2.8"); retail and
// pre-switch builds render "Version 1.04" and are handled by the caller as
// REPLAY_EPOCH_RETAIL.
static RecorderClass::ReplayEpoch epochFromSemanticVersion(Int major, Int minor, Int patch)
{
	if (major != 1)
		return RecorderClass::REPLAY_EPOCH_CURRENT;
	if (minor < 2)
		return RecorderClass::REPLAY_EPOCH_RETAIL;   // 1.0.x, 1.1.x
	if (minor == 2)
	{
		if (patch <= 0) return RecorderClass::REPLAY_EPOCH_RETAIL;  // 1.2.0
		if (patch <= 7) return RecorderClass::REPLAY_EPOCH_V121;    // 1.2.1 - 1.2.7
		return RecorderClass::REPLAY_EPOCH_V128;                    // 1.2.8, 1.2.9
	}
	// 1.5.4 was cut at 9e68b663e (Aug 4 2026) and carries none of the
	// stable-upgrade-id / community-patch sim changes -- those landed after it
	// and first shipped in 1.5.5. (The commit that introduced them bumped
	// APPVERSION 1.5.2 -> 1.5.4 and named the epoch V154, which is why 1.5.4
	// replays mismatched in 1.5.5; corrected in 1.5.6. There was never a 1.5.3.)
	if (minor < 5 || (minor == 5 && patch <= 4))
		return RecorderClass::REPLAY_EPOCH_V130;      // 1.3.0 - 1.5.4
	return RecorderClass::REPLAY_EPOCH_V155;          // 1.5.5+
}

// Pre-V155 replays carry upgrade purchases as raw NameKeyType values. The
// numbering belongs to the RECORDING environment (binary registrations + all
// names registered by data parsed before Upgrade.ini). Playback loads the
// recording's data package, so the data half cancels out; what remains is the
// binary difference: 92674b28d (first shipped in 1.5.2) added one early
// FunctionLexicon registration, shifting every later namekey by one -- the
// cause of the v1.5.2 replay-fidelity regression. This returns OUR binary's
// early-registration count minus the recording binary's: add it to a recorded
// upgrade key to get the same upgrade under our numbering. If a future change
// ever adds/removes another pre-Upgrade.ini registration, gate it here the
// same way (older recordings get a bigger delta).
static Int legacyUpgradeKeyDeltaFromHeader(const RecorderClass::ReplayHeader& header)
{
	AsciiString vs;
	vs.translate(header.versionString);
	const char *s = vs.str();
	Int a = 0, b = 0, c = 0;
	if (s[0] >= '0' && s[0] <= '9' && sscanf(s, "%d.%d.%d", &a, &b, &c) == 3)
	{
		// Zulu 1.5.2 and later dev builds share our early-registration set.
		if (a > 1 || (a == 1 && (b > 5 || (b == 5 && c >= 2))))
			return 0;
	}
	return 1;   // retail and Zulu <= 1.5.1: one fewer early registration than us
}

static RecorderClass::ReplayEpoch deriveReplayEpochFromHeader(const RecorderClass::ReplayHeader& header)
{
	AsciiString vs;
	vs.translate(header.versionString);
	const char *s = vs.str();
	Int a = 0, b = 0, c = 0;
	// Treat as a Zulu semantic version only if it starts with a digit and has
	// all three components; otherwise (retail "Version 1.04", dev builds) fall
	// back to the retail epoch. The -replayEpoch override is authoritative for
	// anything the header can't disambiguate.
	if (s[0] >= '0' && s[0] <= '9' && sscanf(s, "%d.%d.%d", &a, &b, &c) == 3)
		return epochFromSemanticVersion(a, b, c);
	return RecorderClass::REPLAY_EPOCH_RETAIL;
}

RecorderClass::ReplayEpoch RecorderClass::getReplayEpoch() const
{
	if (m_mode == RECORDERMODETYPE_PLAYBACK
		|| m_mode == RECORDERMODETYPE_SIMULATION_PLAYBACK
		|| m_mode == RECORDERMODETYPE_RESUME_CATCHUP)
	{
		return m_replayEpoch;
	}
	return REPLAY_EPOCH_CURRENT;
}

Int RecorderClass::getReplayLegacyUpgradeKeyDelta() const
{
	if (m_mode == RECORDERMODETYPE_PLAYBACK
		|| m_mode == RECORDERMODETYPE_SIMULATION_PLAYBACK
		|| m_mode == RECORDERMODETYPE_RESUME_CATCHUP)
	{
		return m_replayLegacyUpgradeKeyDelta;
	}
	return 0;
}

/**
 * Initialization
 * The recorder will record by default since every game will be recorded.
 * Obviously a game that is being played back will not be recorded.
 * Since the playback is done through a special interface, that interface
 * will set the recorder mode to RECORDERMODETYPE_PLAYBACK.
 */
void RecorderClass::init() {
	m_originalGameMode = GAME_NONE;
	m_mode = RECORDERMODETYPE_NONE;
	m_file = nullptr;
	m_fileName.clear();
	m_currentFilePosition = 0;
	m_gameInfo.clearSlotList();
	m_gameInfo.reset();
	if (TheGlobalData->m_pendingFile.isEmpty())
		m_gameInfo.setMap(TheGlobalData->m_mapName);
	else
		m_gameInfo.setMap(TheGlobalData->m_pendingFile);
	m_gameInfo.setSeed(GetGameLogicRandomSeed());
	m_wasDesync = FALSE;
	m_doingAnalysis = FALSE;
	m_playbackFrameCount = 0;
	m_replayAIFeatureVersion = ZULU_AI_FEATURE_CURRENT;
	m_replayEpoch = REPLAY_EPOCH_CURRENT;
	m_replayLegacyUpgradeKeyDelta = 0;
	m_playbackStaleObjectRefs = 0;

	// Live-observer state must not survive a teardown. Quitting while starved
	// leaves m_liveObserverWaitingForBytes set, and if it leaks into the next
	// game the starved early-return in GameLogic::update() freezes logic on
	// its first frame (LAN start countdown hangs, then crashes).
	m_liveObserverStreamOpen      = FALSE;
	m_liveObserverWaitingForBytes = FALSE;
	m_liveObserverRetryPos        = 0;
	m_liveObserverFpsBoosted      = FALSE;
	m_liveObserverSavedFpsLimit   = 0;
	m_replayShortRead             = FALSE;
	m_resumeRecordPos             = 0;
	m_liveObserverStarvedSinceMs  = 0;

	OptionPreferences optionPref;
	m_archiveReplays = optionPref.getArchiveReplaysEnabled();
}

/**
 * Reset the recorder to the "initialized state."
 */
void RecorderClass::reset() {
	if (m_file != nullptr) {
		m_file->close();
		m_file = nullptr;
	}
	m_fileName.clear();

	init();
}

/**
 * update
 * Do the update for this frame.
 */
void RecorderClass::update() {
	// LIVE_OBSERVER: keep TiVOFastMode forced off. The standard replay FF
	// hotkey races the observer past the live edge and desyncs us from the
	// host. Snapshot catch-up already runs at the high FPS limit set in
	// playbackFileLiveObserver, so the user never needs manual FF here.
	if (m_mode == RECORDERMODETYPE_LIVE_OBSERVER
	    && TheGlobalData && TheGlobalData->m_TiVOFastMode)
	{
		TheWritableGlobalData->m_TiVOFastMode = FALSE;
		if (TheInGameUI)
			TheInGameUI->messageNoFormat(
				TheGameText->FETCH_OR_SUBSTITUTE("GUI:LiveObserverNoFF",
					L"Fast Forward is unavailable while observing a live game."));
	}

	// Diagnostic: every recorder update fires a one-liner in LIVE_OBSERVER
	// mode so the trail in ObserverLog.txt shows ticks elapsing.
	if (m_mode == RECORDERMODETYPE_LIVE_OBSERVER)
		LANObsLog("Recorder::update tick mode=%d frame=%u", (int)m_mode,
			TheGameLogic ? TheGameLogic->getFrame() : 0xFFFFFFFFu);

	// LIVE_OBSERVER (joiner side) and RECORD (host side): TheLAN::update is
	// only driven by the LAN lobby menu, so once we're in-game the observer
	// host listen socket and client TCP recv stop being pumped. Drive them
	// here every game frame instead. Safe no-op when no observer host/client
	// is active.
	if (TheLAN
	    && (m_mode == RECORDERMODETYPE_LIVE_OBSERVER || m_mode == RECORDERMODETYPE_RECORD))
	{
		TheLAN->updateObserver();
	}

	if (m_mode == RECORDERMODETYPE_RESUME_CATCHUP) {
		updateResumeCatchup();
	} else if (m_mode == RECORDERMODETYPE_RECORD || m_mode == RECORDERMODETYPE_NONE) {
		updateRecord();
	} else if (isPlaybackMode()) {
		updatePlayback();
	}
}

/**
 * Do the update for the next frame of this playback.
 */
void RecorderClass::updatePlayback() {
	// LIVE_OBSERVER: per-tick checkpoint so the crash log can be correlated
	// with what the playback was doing at the moment of the fault.
	if (isLiveObserverMode())
	{
		LANObsLog("updatePlayback tick: gameFrame=%u m_nextFrame=%d filePos=%d waiting=%d streamOpen=%d",
			TheGameLogic ? TheGameLogic->getFrame() : 0xFFFFFFFFu,
			(Int)m_nextFrame,
			m_file ? m_file->position() : -1,
			m_liveObserverWaitingForBytes ? 1 : 0,
			m_liveObserverStreamOpen ? 1 : 0);
	}

	// Remove any bad commands that have been inserted by the local user that shouldn't be
	// executed during playback.
	CullBadCommandsResult result = cullBadCommands();

	if (result.hasClearGameDataMessage) {
		// TheSuperHackers @bugfix Stop appending more commands if the replay playback is about to end.
		// Previously this would be able to append more commands, which could have unintended consequences,
		// such as crashing the game when a MSG_PLACE_BEACON is appended after MSG_CLEAR_GAME_DATA.
		// MSG_CLEAR_GAME_DATA is supposed to be processed later this frame, which will then stop this playback.
		return;
	}

	// LIVE_OBSERVER: if we were waiting at EOF for more bytes from the host,
	// try to re-read now. Updates to the host's .rep file aren't visible to
	// our stdio read buffer until we close-and-reopen the File, so do that
	// before retrying.
	if (isLiveObserverMode() && m_liveObserverWaitingForBytes)
	{
		// If the stream has closed since we started waiting, finalize.
		if (!m_liveObserverStreamOpen)
		{
			DEBUG_LOG(("RecorderClass::updatePlayback - LIVE_OBSERVER stream closed; ending playback"));
			m_liveObserverWaitingForBytes = FALSE;
			m_nextFrame = -1;
			stopPlayback();
			return;
		}

		// A host that quits without closing the socket (back to the lobby, a
		// crash behind NAT, ...) leaves the stream open but silent forever.
		// A live match always produces bytes within a few seconds (logic CRC
		// messages are recorded on an interval even when nobody issues
		// orders), so a long silence means the match is over: finalize
		// instead of waiting forever. The window must comfortably cover the
		// longest legitimate silence: observing a game in its FIRST seconds
		// means waiting through the host's entire blocking map load before
		// the first command is ever flushed (30s+ on slow machines); 30s
		// here killed exactly that case.
		const UnsignedInt LIVE_OBSERVER_STARVATION_TIMEOUT_MS = 120000;
		const UnsignedInt nowMs = timeGetTime();
		if (m_liveObserverStarvedSinceMs == 0)
		{
			m_liveObserverStarvedSinceMs = nowMs;
		}
		else if (nowMs - m_liveObserverStarvedSinceMs > LIVE_OBSERVER_STARVATION_TIMEOUT_MS)
		{
			DEBUG_LOG(("RecorderClass::updatePlayback - LIVE_OBSERVER starved for %u ms; ending playback", nowMs - m_liveObserverStarvedSinceMs));
			LANObsLog("updatePlayback: starved %u ms with stream open; ending playback", nowMs - m_liveObserverStarvedSinceMs);
			m_liveObserverWaitingForBytes = FALSE;
			m_nextFrame = -1;
			stopPlayback();
			return;
		}

		if (m_file == nullptr)
		{
			// We already lost the file (a prior reopen failed); give up.
			m_liveObserverWaitingForBytes = FALSE;
			m_nextFrame = -1;
			return;
		}

		AsciiString fname = m_file->getName();
		m_file->close();
		m_file = TheFileSystem->openFile(fname.str(), File::READ | File::BINARY);
		if (m_file == nullptr)
		{
			DEBUG_LOG(("RecorderClass::updatePlayback - LIVE_OBSERVER reopen failed"));
			m_liveObserverWaitingForBytes = FALSE;
			m_nextFrame = -1;
			return;
		}
		m_file->seek(m_liveObserverRetryPos, File::START);

		// Retry the read. If it still fails, readNextFrame will re-arm
		// m_liveObserverWaitingForBytes and we'll try again next tick.
		m_liveObserverWaitingForBytes = FALSE;
		readNextFrame();
		if (m_liveObserverWaitingForBytes)
			return;

		// bytes arrived again: the starvation stretch is over
		m_liveObserverStarvedSinceMs = 0;
	}

	if (m_nextFrame == -1) {
		// This is reached if there are no more commands to be executed.
		return;
	}
	UnsignedInt curFrame = TheGameLogic->getFrame();
	if (m_doingAnalysis)
		curFrame = m_nextFrame;

	// While there are commands to be queued up for this frame, do it.
	while (m_nextFrame == curFrame) {
		Int posBeforeAppend = isLiveObserverMode() && m_file ? m_file->position() : 0;
		appendNextCommand();	// append the next command to TheCommandQueue
		Int posAfterAppend = isLiveObserverMode() && m_file ? m_file->position() : 0;
		if (isLiveObserverMode())
			LANObsLog("appendNextCommand frame=%d gameFrame=%u: %d -> %d (delta=%d)",
				(Int)m_nextFrame, curFrame, posBeforeAppend, posAfterAppend,
				posAfterAppend - posBeforeAppend);
		readNextFrame();	// Read the next command's frame number for playback.
		// LIVE_OBSERVER: if readNextFrame hit EOF, bail so the retry path
		// runs next tick rather than spinning forever here.
		if (isLiveObserverMode() && m_liveObserverWaitingForBytes)
			break;
	}

	// LIVE_OBSERVER: once we hit EOF for the first time, we've drained the
	// snapshot and are now at the live edge — drop the FPS limit back to
	// normal so we don't keep racing ahead of the host. While catching up
	// through the snapshot, FPS stays at 1000 (set in playbackFileLiveObserver).
	if (isLiveObserverMode()
	    && m_liveObserverWaitingForBytes
	    && m_liveObserverFpsBoosted
	    && TheFramePacer)
	{
		LANObsLog("caught up to live edge; restoring FPS limit %d", m_liveObserverSavedFpsLimit);
		TheFramePacer->setFramesPerSecondLimit(m_liveObserverSavedFpsLimit);
		m_liveObserverFpsBoosted = FALSE;
	}
}

/**
 * Stop the currently running playback. This is probably due either to the user exiting out of the playback or
 * reaching the end of the playback file.
 */
void RecorderClass::stopPlayback() {
	if (m_file != nullptr) {
		m_file->close();
		m_file = nullptr;
	}
	m_fileName.clear();

	if (!m_doingAnalysis)
	{
		TheGameLogic->exitGame();
	}

	// A Replay Theater process is booted with some older release's data
	// mounted purely to watch one replay, so there is nowhere sensible for it
	// to go afterwards: dropping to the shell would let the player start a
	// live game on rules nobody else has. Quit instead and let ZuluLauncher
	// bring the picker back up.
	//
	// This covers the ways playback ends inside the recorder: the last frame,
	// a short read, the upload trailer. Quitting out from the in-game menu
	// never reaches here -- it returns to the shell instead, which is why
	// Shell::showShell carries the same guard.
	if (TheGlobalData->m_replayTheater && TheGameEngine != nullptr)
	{
		TheGameEngine->setQuitting(TRUE);
	}
}

/**
 * Update function for recording a game. Basically all the pertinent logic commands for this frame are written out
 * to a file.
 */
void RecorderClass::updateRecord()
{
	Bool needFlush = FALSE;
	static Int lastFrame = -1;
	GameMessage *msg = TheCommandList->getFirstMessage();
	while (msg != nullptr) {
		if (msg->getType() == GameMessage::MSG_NEW_GAME &&
			 msg->getArgument(0)->integer != GAME_SHELL &&
			 msg->getArgument(0)->integer != GAME_SINGLE_PLAYER && // Due to the massive amount of scripts that use <local player> in GC and single player, replays have been cut for them.
			 msg->getArgument(0)->integer != GAME_NONE)
		{
			m_originalGameMode = msg->getArgument(0)->integer;
			DEBUG_LOG(("RecorderClass::updateRecord() - original game is mode %d", m_originalGameMode));
			lastFrame = 0;
			GameDifficulty diff = DIFFICULTY_NORMAL;
			if (msg->getArgumentCount() >= 2)
				diff = (GameDifficulty)msg->getArgument(1)->integer;
			Int rankPoints = 0;
			if (msg->getArgumentCount() >= 3)
				rankPoints = msg->getArgument(2)->integer;
			Int maxFPS = 0;
			if (msg->getArgumentCount() >= 4)
				maxFPS = msg->getArgument(3)->integer;

			// If the LAN lobby has a resume-replay armed, switch into catchup
			// mode instead of starting a fresh recording. Catchup opens the
			// replay file for READ; startRecording would open it for WRITE
			// and truncate the very file we need to read from.
			Bool armedResume = (TheLAN && TheLAN->GetMyGame()
				&& !TheLAN->GetMyGame()->getResumeReplayFile().isEmpty());
			if (armedResume)
			{
				AsciiString resumeFile = TheLAN->GetMyGame()->getResumeReplayFile();
				UnsignedInt handoff    = TheLAN->GetMyGame()->getResumeHandoffFrame();
				if (!startResumeCatchup(resumeFile, handoff))
				{
					// Fallback to a normal recording if catchup setup failed
					// so the game at least runs instead of getting stuck with
					// no replay source.
					DEBUG_LOG(("RecorderClass::updateRecord - resume catchup setup failed, falling back to normal record"));
					startRecording(diff, m_originalGameMode, rankPoints, maxFPS);
				}
			}
			else
			{
				startRecording(diff, m_originalGameMode, rankPoints, maxFPS);
			}
		} else if (msg->getType() == GameMessage::MSG_CLEAR_GAME_DATA) {
			if (m_file != nullptr) {
				lastFrame = -1;
				writeToFile(msg);
				stopRecording();
				needFlush = FALSE;
			}
			m_fileName.clear();
		} else {
			if (m_file != nullptr) {
				if ((msg->getType() > GameMessage::MSG_BEGIN_NETWORK_MESSAGES) &&
						(msg->getType() < GameMessage::MSG_END_NETWORK_MESSAGES)) {
					// Only write the important messages to the file.
					writeToFile(msg);
					needFlush = TRUE;
				}
			}
		}
		msg = msg->next();
	}

	if (needFlush) {
		DEBUG_ASSERTCRASH(m_file != nullptr, ("RecorderClass::updateRecord() - unexpected call to fflush(m_file)"));
		m_file->flush();
	}
}

/**
 * Start a new file for recording. This will always overwrite the "LastReplay.rep" file with the new one.
 * So don't call this unless you really mean it.
 */
void RecorderClass::startRecording(GameDifficulty diff, Int originalGameMode, Int rankPoints, Int maxFPS) {
	DEBUG_ASSERTCRASH(m_file == nullptr, ("Starting to record game while game is in progress."));

	reset();

	m_mode = RECORDERMODETYPE_RECORD;

	AsciiString filepath = getReplayDir();

	// We have to make sure the replay dir exists.
	TheFileSystem->createDirectory(filepath);

	m_fileName = getLastReplayFileName();
	m_fileName.concat(getReplayExtention());
	filepath.concat(m_fileName);
	m_file = TheFileSystem->openFile(filepath.str(), File::WRITE | File::BINARY);
	if (m_file == nullptr) {
		DEBUG_ASSERTCRASH(m_file != nullptr, ("Failed to create replay file"));
		return;
	}
	// TheSuperHackers @info the null terminator needs to be ignored to maintain retail replay file layout
	m_file->writeFormat("%s", s_genrep);

	//
	// save space for stats to be filled in.
	//
	// **** if this changes, change the LAN code above ****
	//
	replay_time_t time = 0;
	m_file->write(&time, sizeof(time));	// reserve space for start time
	m_file->write(&time, sizeof(time));	// reserve space for end time

	UnsignedInt frames = 0;
	m_file->write(&frames, sizeof(frames));	// reserve space for duration in frames

	Bool flag = FALSE;
	m_file->write(&flag, sizeof(flag));	// reserve space for flag (true if we desync)
	m_file->write(&flag, sizeof(flag));	// reserve space for flag (true if we quit early)
	for (Int i=0; i<MAX_SLOTS; ++i)
	{
		m_file->write(&flag, sizeof(flag));	// reserve space for flag (true if player i disconnects)
	}

	// Print out the name of the replay.
	UnicodeString replayName;
	replayName = TheGameText->fetch("GUI:LastReplay");
	m_file->writeFormat(L"%s", replayName.str());
	m_file->writeChar(L"\0");

	// Date and Time
	SYSTEMTIME systemTime;
	GetLocalTime( &systemTime );
	m_file->write(&systemTime, sizeof(systemTime));

	// write out version info
	UnicodeString versionString = TheVersion->getUnicodeVersion();
	UnicodeString versionTimeString = TheVersion->getUnicodeBuildTime();
	UnsignedInt versionNumber = TheVersion->getVersionNumber();
	m_file->writeFormat(L"%s", versionString.str());
	m_file->writeChar(L"\0");
	m_file->writeFormat(L"%s", versionTimeString.str());
	m_file->writeChar(L"\0");
	m_file->write(&versionNumber, sizeof(versionNumber));
	m_file->write(&(TheGlobalData->m_exeCRC), sizeof(TheGlobalData->m_exeCRC));
	m_file->write(&(TheGlobalData->m_iniCRC), sizeof(TheGlobalData->m_iniCRC));

	// Number of players
	/*
	Int numPlayers = ThePlayerList->getPlayerCount();
	fwrite(&numPlayers, sizeof(numPlayers), 1, m_file);
	*/

	// Write the slot list.
	AsciiString theSlotList;
	Int localIndex = -1;
	if (TheNetwork)
	{
		if (TheLAN)
		{
			GameInfo *game = TheLAN->GetMyGame();
			DEBUG_ASSERTCRASH(game, ("Starting a LAN game with no LANGameInfo object!"));
			theSlotList = GameInfoToAsciiString(game);

			for (Int i=0; i<MAX_SLOTS; ++i)
			{
				if (game->getLocalIP() == game->getSlot(i)->getIP())
				{
					localIndex = i;
					break;
				}
			}
		}
		else
		{
			theSlotList = GameInfoToAsciiString(TheGameSpyGame);
			localIndex = TheGameSpyGame->getLocalSlotNum();
		}
	}
	else
	{
    if(TheSkirmishGameInfo)
    {
			TheSkirmishGameInfo->setCRCInterval(REPLAY_CRC_INTERVAL);
      theSlotList = GameInfoToAsciiString(TheSkirmishGameInfo);
      DEBUG_LOG(("GameInfo String: %s",theSlotList.str()));
			localIndex = 0;
    }
    else
    {
		  // single player.  format the generic (empty) slotlist
			m_gameInfo.setCRCInterval(REPLAY_CRC_INTERVAL);
		  theSlotList = GameInfoToAsciiString(&m_gameInfo);
    }
	}
	logGameStart(theSlotList);
	DEBUG_LOG(("RecorderClass::startRecording - theSlotList = %s", theSlotList.str()));

	// write slot list (starting spots, color, alliances, etc
	m_file->writeFormat("%s", theSlotList.str());
	m_file->writeChar("\0");

	m_file->writeFormat("%d", localIndex);
	m_file->writeChar("\0");

	/*
	/// @todo fix this to use starting spots and player alliances when those are put in the game.
	for (Int i = 0; i < numPlayers; ++i) {
		Player *player = ThePlayerList->getNthPlayer(i);
		if (player == nullptr) {
			continue;
		}
		UnicodeString name = player->getPlayerDisplayName();
		fwprintf(m_file, L"%s", name.str());
		fputwc(0, m_file);
		UnicodeString faction = player->getFaction()->getFactionDisplayName();
		fwprintf(m_file, L"%s", faction.str());
		fputwc(0, m_file);
		Int color = player->getColor()->getAsInt();
		fwrite(&color, sizeof(color), 1, m_file);
		Int team = 0;
		Int startingSpot = 0;
		fwrite(&startingSpot, sizeof(Int), 1, m_file);
		fwrite(&team, sizeof(Int), 1, m_file);
	}
	*/

	// Write the game difficulty.
	m_file->write(&diff, sizeof(diff));

	// Write original game mode
	m_file->write(&originalGameMode, sizeof(originalGameMode));

	// Write rank points to add at game start
	m_file->write(&rankPoints, sizeof(rankPoints));

	// Write maxFPS chosen
	m_file->write(&maxFPS, sizeof(maxFPS));

	// Zulu replay extension. Tags this replay with the AI feature version
	// the recording binary supports, so older binaries reject the file via
	// exeCRC mismatch and newer binaries can identify what features to run.
	writeZuluReplayExtension();

	DEBUG_LOG(("RecorderClass::startRecording() - diff=%d, mode=%d, FPS=%d", diff, originalGameMode, maxFPS));

	/*
	// Write the map name.
	fprintf(m_file, "%s", (TheGlobalData->m_mapName).str());
	fputc(0, m_file);
	*/

	/// @todo Need to write game options when there are some to be written.

	// Flush the header to disk before any commands arrive. Without this, a LAN
	// observer that connects within the first frame or two of game start would
	// see an empty .rep on disk (the recorder's stdio buffer holds the header
	// until the first MSG_BEGIN_NETWORK_MESSAGES range write triggers
	// updateRecord's needFlush block). That empty-file snapshot makes the
	// joiner's playbackFile fail in readReplayHeader and silently abort.
	m_file->flush();
}

/**
 * This will stop the current recording session and close the file. This should always be called at the end of
 * every game.
 */
void RecorderClass::stopRecording() {
	logGameEnd();

	// Release diagnostics (always compiled): mark the end of the match in the
	// uploadable release log.
	ReleaseLog("=== Match end: frame=%d desync=%d ===",
		TheGameLogic ? TheGameLogic->getFrame() : -1, (int)m_wasDesync);
	if (TheNetwork)
	{
		//if (TheLAN)
		{
			if (m_wasDesync)
				cleanUpReplayFile();
			m_wasDesync = FALSE;
		}
	}
	if (m_file != nullptr) {
		m_file->close();
		m_file = nullptr;

		if (m_archiveReplays)
			archiveReplay(m_fileName);
	}

	// Stats + replay upload: fires for every client in a LAN/Internet
	// multiplayer game (the conditions under which
	// StatsExporterBeginRecording was called in GameLogic::startNewGame).
	// For replay viewers and single-player campaigns, exportingActive is
	// FALSE and both the stats export and the replay upload are skipped.
	//
	// Order: stats first, replay second. The replay upload runs even if the
	// stats upload failed — UploadStatsToServer is best-effort and logs
	// errors but does not propagate them, so control just falls through.
	if (!m_fileName.isEmpty())
	{
		const bool wasCollecting = StatsExporterIsActive();
		// Write the stats JSON on this (main) thread - it reads live game state -
		// but defer the actual upload to the background telemetry worker below.
		const AsciiString statsFilePath = ExportGameStatsJSON(getReplayDir(), m_fileName, FALSE);

		// Telemetry uploads (replay + map) only fire if the game had at
		// least two human players. Mirrors the gate inside
		// ExportGameStatsJSON for the cncstats stats upload, so all three
		// telemetry channels behave consistently. Logs once here so the
		// skip is visible in stdout when debugging upload behaviour.
		const bool hasMinHumans = StatsExporterHasMinHumansForUpload();
		if (wasCollecting && !hasMinHumans)
		{
			printf("[telemetry] Skipping replay/map upload: fewer than 2 human players\n");
			fflush(stdout);
		}

		// Local player identity for the telemetry uploads below (replay + logs).
		// The lobby display name is UTF-8 encoded for the replay upload's
		// optional form field; localPlayerSlot is kept as a stable fallback id
		// for the per-player log grouping. Computed once, under the shared gate.
		AsciiString playerNameUtf8;
		Int localPlayerSlot = -1;
		if (wasCollecting && hasMinHumans && TheGameInfo != nullptr)
		{
			localPlayerSlot = TheGameInfo->getLocalSlotNum();
			if (localPlayerSlot >= 0)
			{
				const GameSlot *s = TheGameInfo->getConstSlot(localPlayerSlot);
				if (s != nullptr)
				{
					UnicodeString w = s->getName();
					const WideChar *p = w.str();
					if (p != nullptr)
					{
						for (; *p != L'\0'; ++p)
						{
							unsigned int c = static_cast<unsigned int>(*p);
							if (c < 0x80)
							{
								playerNameUtf8.concat(static_cast<char>(c));
							}
							else if (c < 0x800)
							{
								playerNameUtf8.concat(static_cast<char>(0xC0 | (c >> 6)));
								playerNameUtf8.concat(static_cast<char>(0x80 | (c & 0x3F)));
							}
							else
							{
								playerNameUtf8.concat(static_cast<char>(0xE0 | (c >> 12)));
								playerNameUtf8.concat(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
								playerNameUtf8.concat(static_cast<char>(0x80 | (c & 0x3F)));
							}
						}
					}
				}
			}
		}

		// Gather every telemetry channel into one request and hand it to the
		// background worker. Everything below only reads main-thread-only state
		// (TheGameInfo / TheMapCache / TheGlobalData); StartMatchTelemetryUpload
		// snapshots the volatile files (replay, logs) before it returns, so the
		// next game is free to overwrite them while the upload is still in flight.
		// Nothing here blocks on HTTP.
		if (wasCollecting && hasMinHumans)
		{
			MatchTelemetryUpload up;
			up.seed = GetGameLogicRandomSeed();
			up.mapCRC = 0;
			up.mapContentsMask = 0;

			// Stats: the gzipped JSON we just wrote (upload deferred here).
			if (!TheGlobalData->m_statsUrl.isEmpty() && !statsFilePath.isEmpty())
			{
				up.statsUrl = TheGlobalData->m_statsUrl;
				up.statsFilePath = statsFilePath;
			}

			// Replay.
			if (!TheGlobalData->m_replayUrl.isEmpty())
			{
				AsciiString replayPath = getReplayDir();
				replayPath.concat(m_fileName);
				up.replayUrl = TheGlobalData->m_replayUrl;
				up.replayFilePath = replayPath;
				up.replayFileName = m_fileName;
				up.playerNameUtf8 = playerNameUtf8;
			}

			// Per-match debug/observer logs. The server groups them under
			// <seed>/<player>/, so every client's copy is retrievable together.
			// The debug log only exists in logging builds; the observer log only
			// when this client observed - absent files are skipped by the worker.
			if (!TheGlobalData->m_logsUrl.isEmpty())
			{
				AsciiString playerId = playerNameUtf8;
				if (playerId.isEmpty())
					playerId.format("slot%d", localPlayerSlot);
				up.logsUrl = TheGlobalData->m_logsUrl;
				up.playerId = playerId;
#ifdef DEBUG_LOGGING
				up.logFilePaths.push_back(AsciiString(DebugGetLogFileName())); // absolute path
#endif
				up.logFilePaths.push_back(AsciiString(LANObsGetLogFileName()));   // observer log, in the user data dir
				up.logFilePaths.push_back(AsciiString(ReleaseGetLogFileName())); // always-on release log, same dir
			}

			// Map check + conditional map upload. Look up the played map's CRC
			// from the cache; the worker asks the server whether it already has
			// it and, if not, uploads the .map plus every sidecar on disk.
			// 0xFE = all sidecar bits set (2|4|8|16|32|64).
			if (!TheGlobalData->m_mapCheckUrl.isEmpty() && TheMapCache != nullptr)
			{
				AsciiString mapName = TheGlobalData->m_mapName;
				const MapMetaData *md = TheMapCache->findMap(mapName);
				if (md == nullptr || md->m_CRC == 0)
				{
					printf("[map] No cached metadata for \"%s\", skipping map check\n", mapName.str());
					fflush(stdout);
				}
				else
				{
					up.mapCheckUrl = TheGlobalData->m_mapCheckUrl;
					up.mapUploadUrl = TheGlobalData->m_mapUploadUrl;
					up.mapCRC = md->m_CRC;
					up.mapFilePath = md->m_fileName;
					up.mapContentsMask = 0xFE;
				}
			}

			StartMatchTelemetryUpload(up);
		}
	}

	m_fileName.clear();
}

/**
 * TheSuperHackers @feature Stubbjax 17/10/2025 Copy the replay file to the archive directory and rename it using the current timestamp.
 */
void RecorderClass::archiveReplay(AsciiString fileName)
{
	SYSTEMTIME st;
	GetLocalTime(&st);

	AsciiString archiveFileName;
	// Use a standard YYYYMMDD_HHMMSS format for simplicity and to avoid conflicts.
	archiveFileName.format("%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

	AsciiString extension = getReplayExtention();
	AsciiString sourcePath = getReplayDir();
	sourcePath.concat(fileName);

	if (!sourcePath.endsWith(extension))
		sourcePath.concat(extension);

	AsciiString destPath = getReplayArchiveDir();
	TheFileSystem->createDirectory(destPath.str());

	destPath.concat(archiveFileName);
	destPath.concat(extension);

	if (!CopyFile(sourcePath.str(), destPath.str(), FALSE))
		DEBUG_LOG(("RecorderClass::archiveReplay: Failed to copy %s to %s", sourcePath.str(), destPath.str()));
}

/**
 * Write this game message to the record file. This also writes the game message's execution frame.
 */
void RecorderClass::writeToFile(GameMessage * msg) {
	// Write the frame number for this command.
	UnsignedInt frame = TheGameLogic->getFrame();
	m_file->write(&frame, sizeof(frame));

	// Write the command type
	GameMessage::Type type = msg->getType();
	m_file->write(&type, sizeof(type));

	// Write the player index
	Int playerIndex = msg->getPlayerIndex();
	m_file->write(&playerIndex, sizeof(playerIndex));

#ifdef DEBUG_LOGGING
	AsciiString commandName = msg->getCommandAsString();
	if (type < GameMessage::MSG_BEGIN_NETWORK_MESSAGES || type > GameMessage::MSG_END_NETWORK_MESSAGES)
	{
		commandName.concat(" (Non-Network message!)");
	}
	else if (type == GameMessage::MSG_BEGIN_NETWORK_MESSAGES)
	{
		AsciiString tmp;
		tmp.format(" (CRC 0x%8.8X)", msg->getArgument(0)->integer);
		commandName.concat(tmp);
	}

	//DEBUG_LOG(("RecorderClass::writeToFile - Adding %s command from player %d to TheCommandList on frame %d",
		//commandName.str(), msg->getPlayerIndex(), TheGameLogic->getFrame()));
#endif // DEBUG_LOGGING

	GameMessageParser *parser = newInstance(GameMessageParser)(msg);
	UnsignedByte numTypes = parser->getNumTypes();
	m_file->write(&numTypes, sizeof(numTypes));

	GameMessageParserArgumentType *argType = parser->getFirstArgumentType();
	while (argType != nullptr) {
		UnsignedByte type = (UnsignedByte)(argType->getType());
		m_file->write(&type, sizeof(type));

		UnsignedByte argTypeCount = (UnsignedByte)(argType->getArgCount());
		m_file->write(&argTypeCount, sizeof(argTypeCount));

		argType = argType->getNext();
	}

//	UnsignedByte lasttype = (UnsignedByte)ARGUMENTDATATYPE_UNKNOWN;
	Int numArgs = msg->getArgumentCount();
	for (Int i = 0; i < numArgs; ++i) {
//		UnsignedByte type = (UnsignedByte)(msg->getArgumentDataType(i));
//		if (lasttype != type) {
//			fwrite(&type, sizeof(type), 1, m_file);
//			lasttype = type;
//		}
		writeArgument(msg->getArgumentDataType(i), *(msg->getArgument(i)));
	}

	deleteInstance(parser);
	parser = nullptr;

}

void RecorderClass::writeArgument(GameMessageArgumentDataType type, const GameMessageArgumentType arg) {

	switch (type) {

		case ARGUMENTDATATYPE_INTEGER:
			m_file->write( &(arg.integer), sizeof(arg.integer) );
			break;
		case ARGUMENTDATATYPE_REAL:
			m_file->write( &(arg.real), sizeof(arg.real) );
			break;
		case ARGUMENTDATATYPE_BOOLEAN:
			m_file->write( &(arg.boolean), sizeof(arg.boolean) );
			break;
		case ARGUMENTDATATYPE_OBJECTID:
			m_file->write( &(arg.objectID), sizeof(arg.objectID) );
			break;
		case ARGUMENTDATATYPE_DRAWABLEID:
			m_file->write( &(arg.drawableID), sizeof(arg.drawableID) );
			break;
		case ARGUMENTDATATYPE_TEAMID:
			m_file->write( &(arg.teamID), sizeof(arg.teamID) );
			break;
		case ARGUMENTDATATYPE_LOCATION:
			m_file->write( &(arg.location), sizeof(arg.location) );
			break;
		case ARGUMENTDATATYPE_PIXEL:
			m_file->write( &(arg.pixel), sizeof(arg.pixel) );
			break;
		case ARGUMENTDATATYPE_PIXELREGION:
			m_file->write( &(arg.pixelRegion), sizeof(arg.pixelRegion) );
			break;
		case ARGUMENTDATATYPE_TIMESTAMP:
			m_file->write( &(arg.timestamp), sizeof(arg.timestamp) );
			break;
		case ARGUMENTDATATYPE_WIDECHAR:
			m_file->write( &(arg.wChar), sizeof(arg.wChar) );
			break;
		default:
			DEBUG_LOG(("Unknown GameMessageArgumentDataType in RecorderClass::writeArgument"));
			break;
	}
}

/**
 * Read in a replay header, for (1) populating a replay listbox or (2) starting playback.  In
 * case (2), set FILE *m_file.
 */
Bool RecorderClass::readReplayHeader(ReplayHeader& header)
{
	AsciiString filepath = getReplayDir();
	filepath.concat(header.filename.str());

	// TheSuperHackers @performance More buffered data reduces disk overhead and will improve fast forward playback
	const UnsignedInt buffersize = header.forPlayback ? replayBufferBytes : File::BUFFERSIZE;
	m_file = TheFileSystem->openFile(filepath.str(), File::READ | File::BINARY, buffersize);

	if (m_file == nullptr)
	{
		DEBUG_LOG(("Can't open %s (%s)", filepath.str(), header.filename.str()));
		return FALSE;
	}

	// Read the GENREP header.
	char genrep[sizeof(s_genrep) - 1] = {0};
	m_file->read( &genrep, sizeof(s_genrep) - 1 );
	if ( strncmp(genrep, s_genrep, sizeof(s_genrep) - 1 ) != 0 ) {
		DEBUG_LOG(("RecorderClass::readReplayHeader - replay file did not have GENREP at the start."));
		m_file->close();
		m_file = nullptr;
		return FALSE;
	}

	// read in some stats
	replay_time_t tmp;
	m_file->read(&tmp, sizeof(tmp));
	header.startTime = tmp;
	m_file->read(&tmp, sizeof(tmp));
	header.endTime = tmp;

	m_file->read(&header.frameCount, sizeof(header.frameCount));

	m_file->read(&header.desyncGame, sizeof(header.desyncGame));
	m_file->read(&header.quitEarly, sizeof(header.quitEarly));
	for (Int i=0; i<MAX_SLOTS; ++i)
	{
		m_file->read(&(header.playerDiscons[i]), sizeof(Bool));
	}

	// Read the Replay Name.  We don't actually do anything with it.  Oh well.
	header.replayName = readUnicodeString();

	// Read the date and time.  We don't really do anything with this either. Oh well.
	m_file->read(&header.timeVal, sizeof(header.timeVal));

	// Read in the Version info
	header.versionString = readUnicodeString();
	header.versionTimeString = readUnicodeString();
	m_file->read(&header.versionNumber, sizeof(header.versionNumber));
	m_file->read(&header.exeCRC, sizeof(header.exeCRC));
	m_file->read(&header.iniCRC, sizeof(header.iniCRC));

	// Read in the GameInfo
	header.gameOptions = readAsciiString();
	m_gameInfo.reset();
	m_gameInfo.enterGame();
	DEBUG_LOG(("RecorderClass::readReplayHeader - GameInfo = %s", header.gameOptions.str()));
	if (!ParseAsciiStringToGameInfo(&m_gameInfo, header.gameOptions))
	{
		DEBUG_LOG(("RecorderClass::readReplayHeader - replay file did not have a valid GameInfo string."));
		m_file->close();
		m_file = nullptr;
		return FALSE;
	}
	m_gameInfo.startGame(0);

	AsciiString playerIndex = readAsciiString();
	header.localPlayerIndex = atoi(playerIndex.str());
	if (header.localPlayerIndex < -1 || header.localPlayerIndex >= MAX_SLOTS)
	{
		DEBUG_LOG(("RecorderClass::readReplayHeader - invalid local slot number."));
		m_gameInfo.endGame();
		m_gameInfo.reset();
		m_file->close();
		m_file = nullptr;
		return FALSE;
	}
	if (header.localPlayerIndex >= 0)
	{
		Int localIP = m_gameInfo.getSlot(header.localPlayerIndex)->getIP();
		m_gameInfo.setLocalIP(localIP);
	}

	if (!header.forPlayback)
	{
		m_gameInfo.endGame();
		m_gameInfo.reset();
		m_file->close();
		m_file = nullptr;
	}

	return TRUE;
}

Bool RecorderClass::simulateReplay(AsciiString filename)
{
	Bool success = playbackFile(filename);
	if (success)
		m_mode = RECORDERMODETYPE_SIMULATION_PLAYBACK;
	return success;
}

// LAN observer entry point. Reuses the normal playback bootstrap (which reads
// the header and queues MSG_NEW_GAME) and then flips into LIVE_OBSERVER mode
// so readNextFrame waits at EOF instead of terminating playback. The caller
// (LANObserverClient) must call setLiveObserverStreamOpen(TRUE) before this,
// and setLiveObserverStreamOpen(FALSE) when the network stream closes so the
// recorder knows when to finalize.
Bool RecorderClass::playbackFileLiveObserver(AsciiString filename)
{
	LANObsLog("playbackFileLiveObserver entry: filename='%s'", filename.str());
	m_liveObserverArming = TRUE;
	Bool success = playbackFile(filename);
	m_liveObserverArming = FALSE;
	LANObsLog("playbackFile returned %s; m_nextFrame=%d waitingAtOpen=%d",
		success?"TRUE":"FALSE", (Int)m_nextFrame, m_liveObserverWaitingForBytes ? 1 : 0);
	if (success)
	{
		m_mode = RECORDERMODETYPE_LIVE_OBSERVER;
		// If the open-time first read already hit EOF (snapshot from a
		// just-started game: header flushed, first command not yet), KEEP
		// the armed wait state so updatePlayback's retry loop picks it up;
		// otherwise start from a clean slate.
		if (!m_liveObserverWaitingForBytes)
			m_liveObserverRetryPos = 0;

		// The stream owner set m_liveObserverStreamOpen before this call, but
		// when a shell map was loaded, playbackFile() above just tore it down
		// (clearGameData -> GameEngine::reset -> resetAll -> our init()),
		// wiping the flag. With it stuck FALSE the first EOF is treated as
		// end-of-replay and the observer exits the moment it catches up to
		// live. Re-assert it here: playback is only ever kicked from a
		// connected client (STATE_READY), and LANAPI::updateObserver turns it
		// back off within a tick if the socket has already died.
		m_liveObserverStreamOpen = TRUE;

		// Boost FPS while draining the initial snapshot so the observer
		// catches up to the live edge fast. updatePlayback drops it back to
		// the saved value on the first EOF. Same trick RESUME_CATCHUP uses.
		if (TheFramePacer)
		{
			m_liveObserverSavedFpsLimit = TheFramePacer->getFramesPerSecondLimit();
			if (m_liveObserverSavedFpsLimit <= 0)
				m_liveObserverSavedFpsLimit = 30;
			TheFramePacer->setFramesPerSecondLimit(1000);
			TheWritableGlobalData->m_useFpsLimit = TRUE;
			m_liveObserverFpsBoosted = TRUE;
			LANObsLog("FPS boost: saved=%d, boosted to 1000", m_liveObserverSavedFpsLimit);
		}
		m_liveObserverStarvedSinceMs = 0;

		LANObsLog("mode set to LIVE_OBSERVER; file pos=%d", m_file ? m_file->position() : -1);
	}
	return success;
}

#if defined(RTS_DEBUG)
Bool RecorderClass::analyzeReplay( AsciiString filename )
{
	m_doingAnalysis = TRUE;
	return playbackFile(filename);
}



#endif

Bool RecorderClass::isPlaybackInProgress() const
{
	return isPlaybackMode() && m_nextFrame != -1;
}

AsciiString RecorderClass::getCurrentReplayFilename()
{
	if (isPlaybackMode())
	{
		return m_currentReplayFilename;
	}
	return AsciiString::TheEmptyString;
}

// TheSuperHackers @info helmutbuhler 03/04/2025
// Some info about CRC:
// In each game, each peer periodically calculates a CRC from the local gamestate and sends that
// in a message to all peers (including itself) so that everyone can check that the crc is synchronous.
// In a network game, there is a delay between sending the CRC message and receiving it. This is
// necessary because if you were to wait each frame for all messages from all peers, things would go
// horribly slow.
// But this delay is not a problem for CRC checking because everyone receives the CRC in the same frame
// and every peer just makes sure all the received CRCs are equal.
// While playing replays, this is a problem however: The CRC messages in the replays appear on the frame
// they were received, which can be a few frames delayed if it was a network game. And if we were to
// compare those with the local gamestate, they wouldn't sync up.
// So, in order to fix this, we need to queue up our local CRCs,
// so that we can check it with the crc messages that come later.
// This class is basically that queue.
class CRCInfo
{
public:
	CRCInfo(UnsignedInt localPlayer, Bool isMultiplayer);
	void addCRC(UnsignedInt val);
	UnsignedInt readCRC();

	int GetQueueSize() const { return m_data.size(); }

	UnsignedInt getLocalPlayer() { return m_localPlayer; }

	void setSawCRCMismatch() { m_sawCRCMismatch = TRUE; }
	Bool sawCRCMismatch() const { return m_sawCRCMismatch; }

protected:

	Bool m_sawCRCMismatch;
	Bool m_skippedOne;
	std::list<UnsignedInt> m_data;
	UnsignedInt m_localPlayer;
};

CRCInfo::CRCInfo(UnsignedInt localPlayer, Bool isMultiplayer)
{
	m_localPlayer = localPlayer;
	m_skippedOne = !isMultiplayer;
	m_sawCRCMismatch = FALSE;
}

void CRCInfo::addCRC(UnsignedInt val)
{
	// TheSuperHackers @fix helmutbuhler 03/04/2025
	// In Multiplayer, the first MSG_LOGIC_CRC message somehow doesn't make it through the network.
	// Perhaps this happens because the network is not yet set up on frame 0.
	// So we also don't queue up the first local crc message, otherwise the crc
	// messages wouldn't match up anymore and we'd desync immediately during playback.
	if (!m_skippedOne)
	{
		m_skippedOne = TRUE;
		return;
	}

	m_data.push_back(val);
	//DEBUG_LOG(("CRCInfo::addCRC() - crc %8.8X pushes list to %d entries (full=%d)", val, m_data.size(), !m_data.empty()));
}

UnsignedInt CRCInfo::readCRC()
{
	if (m_data.empty())
	{
		DEBUG_LOG(("CRCInfo::readCRC() - bailing, full=0, size=%d", m_data.size()));
		return 0;
	}

	UnsignedInt val = m_data.front();
	m_data.pop_front();
	//DEBUG_LOG(("CRCInfo::readCRC() - returning %8.8X, full=%d, size=%d", val, !m_data.empty(), m_data.size()));
	return val;
}

Bool RecorderClass::sawCRCMismatch() const
{
	return m_crcInfo->sawCRCMismatch();
}

void RecorderClass::handleCRCMessage(UnsignedInt newCRC, Int playerIndex, Bool fromPlayback)
{
	if (fromPlayback)
	{
		//DEBUG_LOG(("RecorderClass::handleCRCMessage() - Adding CRC of %X from %d to m_crcInfo", newCRC, playerIndex));
		m_crcInfo->addCRC(newCRC);
		return;
	}

	Int localPlayerIndex = m_crcInfo->getLocalPlayer();
	Bool samePlayer = FALSE;
	AsciiString playerName;
	playerName.format("player%d", localPlayerIndex);
	const Player *p = ThePlayerList->getNthPlayer(playerIndex);
	if (!p || (p->getPlayerNameKey() == NAMEKEY(playerName)))
		samePlayer = TRUE;
	if (samePlayer || (localPlayerIndex < 0))
	{
		UnsignedInt playbackCRC = m_crcInfo->readCRC();
		//DEBUG_LOG(("RecorderClass::handleCRCMessage() - Comparing CRCs of InGame:%8.8X Replay:%8.8X Frame:%d from Player %d",
		//	playbackCRC, newCRC, TheGameLogic->getFrame()-m_crcInfo->GetQueueSize()-1, playerIndex));

		// Replays recorded by retail-lineage clients ("Version 1.04"/"1.06"
		// headers) carry Checksum values that never match a re-simulated CRC
		// at any frame offset -- retail's own playback had the same permanent
		// mismatch, which is why the warning below was disabled in 2003. A
		// faithfully re-simulated retail game (verified by outcome) still
		// mismatches 100% of its checkpoints, so for the retail epoch these
		// values carry no information: drain the queue and leave the verdict
		// to the completion and stale-reference checks in ReplaySimulation.
		// Fork-recorded replays keep the full strict check below.
		if (getReplayEpoch() == REPLAY_EPOCH_RETAIL)
			return;

		if (TheGameLogic->getFrame() > 0 && newCRC != playbackCRC && !m_crcInfo->sawCRCMismatch())
		{
			//Kris: Patch 1.01 November 10, 2003 (integrated changes from Matt Campbell)
			// Since we don't seem to have any *visible* desyncs when replaying games, but get this warning
			// virtually every replay, the assumption is our CRC checking is faulty.  Since we're at the
			// tail end of patch season, let's just disable the message, and hope the users believe the
			// problem is fixed. -MDC 3/20/2003
			//
			// TheSuperHackers @tweak helmutbuhler 03/04/2025
			// More than 20 years later, but finally fixed and re-enabled!
			TheInGameUI->message("GUI:CRCMismatch");

			// TheSuperHackers @info helmutbuhler 03/04/2025
			// Note: We subtract the queue size from the frame number. This way we calculate the correct frame
			// the mismatch first happened in case the NetCRCInterval is set to 1 during the game.
			const UnsignedInt mismatchFrame = TheGameLogic->getFrame() - m_crcInfo->GetQueueSize() - 1;

			// Now also prints a UI message for it.
			const UnicodeString mismatchDetailsStr = TheGameText->FETCH_OR_SUBSTITUTE("GUI:CRCMismatchDetails", L"InGame:%8.8X Replay:%8.8X Frame:%d");
			TheInGameUI->message(mismatchDetailsStr, playbackCRC, newCRC, mismatchFrame);

			DEBUG_LOG(("Replay has gone out of sync!\nInGame:%8.8X Replay:%8.8X\nFrame:%d",
				playbackCRC, newCRC, mismatchFrame));

			// Release diagnostics (always compiled): record the divergence and
			// dump the recent CRC history to the uploadable release log.
			ReleaseLog("Replay out of sync: InGame=%8.8X Replay=%8.8X frame=%d",
				playbackCRC, newCRC, mismatchFrame);
			ReleaseLogDumpCRCHistory();

			// Print Mismatch in case we are simulating replays from console.
			printf("CRC Mismatch in Frame %d\n", mismatchFrame);

			// TheSuperHackers @tweak Pause the game on mismatch.
			// But not when a window with focus is opened, because that can make resuming difficult.
			if (TheWindowManager->winGetFocus() == nullptr)
			{
				Bool pause = TRUE;
				Bool pauseMusic = FALSE;
				Bool pauseInput = FALSE;
				TheGameLogic->setGamePaused(pause, pauseMusic, pauseInput);

				// Mark this mismatch as seen when we had the chance to pause once.
				m_crcInfo->setSawCRCMismatch();
			}
		}
		return;
	}

	//DEBUG_LOG(("RecorderClass::handleCRCMessage() - Skipping CRC of %8.8X from %d (our index is %d)", newCRC, playerIndex, localPlayerIndex));
}

/**
 * Returns true if this version of the file is the same as our version of the game
 */
Bool RecorderClass::replayMatchesGameVersion(AsciiString filename)
{
	ReplayHeader header;
	header.forPlayback = TRUE;
	header.filename = filename;
	if ( readReplayHeader( header ) )
	{
		return replayMatchesGameVersion( header );
	}
	return FALSE;
}

Bool RecorderClass::replayMatchesGameVersion(const ReplayHeader& header)
{
	// TheSuperHackers @fix No longer checks the build time here to prevent incorrect Replay playback incompatibility messages when the Replay playback would actually be technically compatible.
	if (header.versionString != TheVersion->getUnicodeVersion())
		return false;
	if (header.versionNumber != TheVersion->getVersionNumber())
		return false;
	if (header.exeCRC != TheGlobalData->m_exeCRC)
		return false;
	if (header.iniCRC != TheGlobalData->m_iniCRC)
		return false;
	return true;
}

// TheSuperHackers @feature Ensure the replay's map is installed before headless
// playback. A .rep only carries the map identity (name + CRC) in its header, not
// the .map bytes, so a replay recorded on a map this machine doesn't have would
// otherwise loadMap() into empty/black terrain and produce a bogus simulation.
//
// findReplayMapByCRC mirrors the live-observer lookup: the MapCache is keyed by
// lowercased filename, but the replay header only knows the map by CRC, so scan.
static const MapMetaData *findReplayMapByCRC(UnsignedInt crc)
{
	if (!TheMapCache || crc == 0)
		return NULL;

	for (MapCache::iterator it = TheMapCache->begin(); it != TheMapCache->end(); ++it)
	{
		if (it->second.m_CRC == crc)
			return &it->second;
	}
	return NULL;
}

// When running headless, fetch the replay's map from the cncstats CDN if it's
// not already installed, using the same TheMapDownloadHook as the LAN/WOL lobby
// join path. Best-effort: on any failure we log and let playback proceed so it
// surfaces its own error rather than aborting the run here.
static void ensureReplayMapAvailable(const GameInfo *gameInfo)
{
	if (!gameInfo || !TheMapCache)
		return;

	AsciiString mapName = gameInfo->getMap();		// real (resolved) map path
	UnsignedInt mapCRC  = gameInfo->getMapCRC();
	Int         mapMask = gameInfo->getMapContentsMask();

	// Already installed? CRC is the exact key; fall back to filename lookup.
	if (findReplayMapByCRC(mapCRC) != NULL)
		return;
	if (!mapName.isEmpty() && TheMapCache->findMap(mapName) != NULL)
		return;

	// Missing locally. We need the CRC, a real install path, and the download
	// hook to fetch it; the hook is null in classic Generals builds.
	if (mapCRC == 0 || mapName.isEmpty() || TheMapDownloadHook == NULL)
	{
		printf("Map '%s' (crc %u) is missing and cannot be downloaded.\n", mapName.str(), mapCRC);
		fflush(stdout);
		DEBUG_LOG(("ensureReplayMapAvailable - map '%s' missing and not fetchable (crc=%u hook=%d)",
			mapName.str(), mapCRC, TheMapDownloadHook != NULL));
		return;
	}

	printf("Downloading map '%s' (crc %u) from CDN for replay playback...\n", mapName.str(), mapCRC);
	fflush(stdout);

	// 0x7E = all sidecars (preview|ini|str|solo|assets|readme); use the header's
	// mask when present, otherwise ask for everything. The hook CRC-verifies the
	// .map bytes and refreshes MapCache on success.
	UnsignedInt mask = (mapMask != 0) ? (UnsignedInt)mapMask : 0x7E;
	if (!TheMapDownloadHook(mapName, mapCRC, mask))
	{
		printf("Map download failed for '%s' (crc %u); playback may fail.\n", mapName.str(), mapCRC);
		fflush(stdout);
		DEBUG_LOG(("ensureReplayMapAvailable - CDN download failed (crc=%u path='%s')", mapCRC, mapName.str()));
		return;
	}

	printf("Map '%s' installed.\n", mapName.str());
	fflush(stdout);
}

/**
 * Start playback of the file. Return true or false depending on if the file is
 * a valid replay file or not.
 */
Bool RecorderClass::playbackFile(AsciiString filename)
{
	if (!m_doingAnalysis)
	{
		if (TheGameLogic->isInGame())
		{
			TheGameLogic->clearGameData();
		}
	}

	m_mode = RECORDERMODETYPE_PLAYBACK;

	ReplayHeader header;
	header.forPlayback = TRUE;
	header.filename = filename;
	Bool success = readReplayHeader( header );
	if (!success)
	{
		return FALSE;
	}

	// Select the determinism epoch to simulate: the -replayEpoch override wins,
	// otherwise auto-detect from the recorded version string. Gated sim paths
	// read this via getReplayEpoch() to reproduce older behavior bit-exactly.
	m_replayEpoch = (s_replayEpochOverride >= 0)
		? (ReplayEpoch)s_replayEpochOverride
		: deriveReplayEpochFromHeader(header);
	m_replayLegacyUpgradeKeyDelta = legacyUpgradeKeyDeltaFromHeader(header);
	DEBUG_LOG(("RecorderClass::playbackFile - replay determinism epoch = %d (override=%d)",
		(Int)m_replayEpoch, s_replayEpochOverride));

#ifdef DEBUG_CRASHING
	Bool versionStringDiff = header.versionString != TheVersion->getUnicodeVersion();
	Bool versionTimeStringDiff = header.versionTimeString != TheVersion->getUnicodeBuildTime();
	Bool versionNumberDiff = header.versionNumber != TheVersion->getVersionNumber();
	Bool exeCRCDiff = header.exeCRC != TheGlobalData->m_exeCRC;
	Bool exeDifferent = versionStringDiff || versionTimeStringDiff || versionNumberDiff || exeCRCDiff;
	Bool iniDifferent = header.iniCRC != TheGlobalData->m_iniCRC;

	AsciiString debugString;
	AsciiString tempStr;
	if (exeDifferent)
	{
		// TheSuperHackers @fix helmutbuhler 05/05/2025 No longer attempts to print unicode as ascii
		// via a call to AsciiString::format with %ls format. It does not work with non-ascii characters.
		UnicodeString tempStrWide;
		debugString = "EXE is different:\n";
		if (versionStringDiff)
		{
			tempStrWide.format(L"   Version [%s] vs [%s]\n", TheVersion->getUnicodeVersion().str(), header.versionString.str());
			tempStr.translate(tempStrWide);
			debugString.concat(tempStr);
		}
		if (versionTimeStringDiff)
		{
			tempStrWide.format(L"   Build Time [%s] vs [%s]\n", TheVersion->getUnicodeBuildTime().str(), header.versionTimeString.str());
			tempStr.translate(tempStrWide);
			debugString.concat(tempStr);
		}
		if (versionNumberDiff)
		{
			tempStr.format("   Version Number %8.8X vs %8.8X\n", TheVersion->getVersionNumber(), header.versionNumber);
			debugString.concat(tempStr);
		}
		if (exeCRCDiff)
		{
			tempStr.format("   CRC %8.8X vs %8.8X\n", TheGlobalData->m_exeCRC, header.exeCRC);
			debugString.concat(tempStr);
		}
	}
	if (iniDifferent)
	{
		debugString.concat("INIs are different:\n");
		tempStr.format("   CRC %8.8X vs %8.8X\n", TheGlobalData->m_iniCRC, header.iniCRC);
		debugString.concat(tempStr);
	}
	DEBUG_ASSERTCRASH(!exeDifferent && !iniDifferent, (debugString.str()));
#endif

	TheWritableGlobalData->m_pendingFile = m_gameInfo.getMap();

	// When playing back headless (e.g. -headless -replay against the stats
	// server), the referenced map may not be installed on this machine. Pull it
	// from the CDN now, before the engine tries to loadMap() it, so the
	// simulation runs on the real terrain instead of empty fallback terrain.
	if (TheGlobalData && TheGlobalData->m_headless)
		ensureReplayMapAvailable(&m_gameInfo);

#ifdef DEBUG_LOGGING
	if (header.localPlayerIndex >= 0)
	{
		DEBUG_LOG(("Local player is %ls (slot %d, IP %8.8X)",
			m_gameInfo.getSlot(header.localPlayerIndex)->getName().str(), header.localPlayerIndex, m_gameInfo.getSlot(header.localPlayerIndex)->getIP()));
	}
#endif

	Bool isMultiplayer = m_gameInfo.getSlot(header.localPlayerIndex)->getIP() != 0;
	m_crcInfo = NEW CRCInfo(header.localPlayerIndex, isMultiplayer);
	REPLAY_CRC_INTERVAL = m_gameInfo.getCRCInterval();
	DEBUG_LOG(("Player index is %d, replay CRC interval is %d", m_crcInfo->getLocalPlayer(), REPLAY_CRC_INTERVAL));

	Int difficulty = 0;
	m_file->read(&difficulty, sizeof(difficulty));

	m_file->read(&m_originalGameMode, sizeof(m_originalGameMode));

	Int rankPoints = 0;
	m_file->read(&rankPoints, sizeof(rankPoints));

	Int maxFPS = 0;
	m_file->read(&maxFPS, sizeof(maxFPS));

	// Zulu replay extension. Vanilla / pre-Zulu replays don't have this block;
	// readZuluReplayExtension peeks for the magic and rewinds if absent so the
	// next read still sees the start of the command stream.
	readZuluReplayExtension();

	DEBUG_LOG(("RecorderClass::playbackFile() - original game was mode %d", m_originalGameMode));

	// TheSuperHackers @fix helmutbuhler 03/04/2025
	// In case we restart a replay, we need to clear the command list.
	// Otherwise a crc message remains and messes up the crc calculation on the restarted replay.
	TheCommandList->reset();

	readNextFrame();

	// send a message to the logic for a new game
	if (!m_doingAnalysis)
	{
		// TheSuperHackers @info helmutbuhler 13/04/2025
		// We send the New Game message here directly to the command list and bypass the TheMessageStream.
		// That's ok because Multiplayer is disabled during replay playback and is actually required
		// during replay simulation because we don't update TheMessageStream during simulation.
		GameMessage *msg = newInstance(GameMessage)(GameMessage::MSG_NEW_GAME);
		msg->appendIntegerArgument(GAME_REPLAY);
		msg->appendIntegerArgument(difficulty);
		msg->appendIntegerArgument(rankPoints);
		if( maxFPS != 0 )
			msg->appendIntegerArgument(maxFPS);
		TheCommandList->appendMessage( msg );
		InitRandom( m_gameInfo.getSeed() );
	}

	m_currentReplayFilename = filename;
	m_playbackFrameCount = header.frameCount;
	return TRUE;
}

/**
 * Read a unicode string from the current file position. The string is assumed to be 0-terminated.
 */
UnicodeString RecorderClass::readUnicodeString() {
	WideChar str[1024] = L"";
	Int index = 0;

	Int c = m_file->readWideChar();
	if (c == EOF) {
		str[index] = 0;
	}
	str[index] = c;

	while (index < 1024 && str[index] != 0) {
		++index;
		Int c = m_file->readWideChar();
		if (c == EOF) {
			str[index] = 0;
			break;
		}
		str[index] = c;
	}
	str[1023] = L'\0';

	UnicodeString retval(str);
	return retval;
}

/**
 * Read an ascii string from the current file position. The string is assumed to be 0-terminated.
 */
AsciiString RecorderClass::readAsciiString() {
	char str[1024] = "";
	Int index = 0;

	Int c =	m_file->readChar();
	if (c == EOF) {
		str[index] = 0;
	}
	str[index] = c;

	while (index < 1024 && str[index] != 0) {
		++index;
		Int c = m_file->readChar();
		if (c == EOF) {
			str[index] = 0;
			break;
		}
		str[index] = c;
	}
	str[1023] = '\0';

	AsciiString retval(str);
	return retval;
}

/**
 * Read the frame number for the next command in the playback file. If the end of the file is reached, the playback
 * is stopped and the next frame is said to be -1.
 *
 * LIVE_OBSERVER mode: EOF means "host hasn't flushed the next frame yet" rather than
 * "replay is over." We rewind the file position so a future retry can re-read this
 * spot, set m_liveObserverWaitingForBytes, and leave m_nextFrame alone so
 * updatePlayback won't advance.
 */
void RecorderClass::readNextFrame() {
	Int posBefore = m_file->position();
	Int bytesRead = m_file->read(&m_nextFrame, sizeof(m_nextFrame));
	if (bytesRead != sizeof(m_nextFrame)) {
		// m_liveObserverArming covers the open-time read inside
		// playbackFile, which runs before the mode flips to LIVE_OBSERVER
		// (and after clearGameData has wiped m_liveObserverStreamOpen):
		// a snapshot from a just-started game legitimately contains no
		// commands yet, and stopping playback here queues the clear-game-
		// data that later dumps the observer at the score screen.
		if (m_liveObserverArming || (isLiveObserverMode() && m_liveObserverStreamOpen))
		{
			// Roll back any partial read and wait for more bytes to arrive.
			// The retry in updatePlayback closes-and-reopens the file so the
			// engine File's stdio buffer doesn't keep telling us EOF after
			// new bytes have actually been appended.
			m_liveObserverRetryPos        = posBefore;
			m_liveObserverWaitingForBytes = TRUE;
			DEBUG_LOG(("RecorderClass::readNextFrame - LIVE_OBSERVER EOF at pos %d, will retry", posBefore));
			return;
		}
		DEBUG_LOG(("RecorderClass::readNextFrame - read failed on frame %d", TheGameLogic->getFrame()));
		m_nextFrame = -1;
		stopPlayback();
		return;
	}

	// TheSuperHackers @bugfix Zulu replay uploads append an 8-byte "ZUTG"
	// identification trailer to the bytes stored on the stats server (see
	// AppendZuluUploadTag), so a replay downloaded from the server has those
	// bytes after the final command frame. Without this guard readNextFrame
	// reads the 'Z','U','T','G' magic as an enormous (~1.2 billion) frame
	// number that the game frame can never reach, so playback never hits a
	// clean EOF and the headless simulation loop spins forever. Treat the
	// trailer as end of replay. A real frame number can never collide with
	// this (~462 days at 30 logic frames/sec).
	const unsigned char *magic = (const unsigned char *)&m_nextFrame;
	if (magic[0] == 'Z' && magic[1] == 'U' && magic[2] == 'T' && magic[3] == 'G')
	{
		DEBUG_LOG(("RecorderClass::readNextFrame - reached ZUTG upload trailer at frame %d; ending playback", TheGameLogic->getFrame()));
		m_nextFrame = -1;
		stopPlayback();
	}
}

/**
 * Read exactly size bytes, flagging a short read rather than silently continuing.
 */
Bool RecorderClass::readReplayBytes(void *dst, Int size) {
	const Int bytesRead = m_file->read(dst, size);
	if (bytesRead != size) {
		m_replayShortRead = TRUE;
		return FALSE;
	}
	return TRUE;
}

/**
 * A record could not be read in full. Put the file position back to where the record
 * started so it can be re-read from the top once the rest of its bytes arrive.
 */
void RecorderClass::rollbackTornRecord(Int posBefore) {
	if (isLiveObserverMode() && m_liveObserverStreamOpen) {
		// The host simply has not flushed the rest of this record to us yet. The retry
		// in updatePlayback closes and reopens the file so stdio does not keep insisting
		// on EOF after new bytes have landed.
		m_liveObserverRetryPos        = posBefore;
		m_liveObserverWaitingForBytes = TRUE;
		DEBUG_LOG(("RecorderClass::appendNextCommand - LIVE_OBSERVER torn record at pos %d, will retry", posBefore));
		return;
	}
	// A file that is not being appended to does not grow, so a short read there means the
	// replay is genuinely truncated. readNextFrame hits the same EOF next and ends
	// playback, so just decline to append the partial command.
	DEBUG_LOG(("RecorderClass::appendNextCommand - short read on frame %d", m_nextFrame));
}

/**
 * This reads the next command from the replay file and appends it to TheCommandList.
 *
 * A replay record is [frame][type][playerIndex][numTypes][argType,argCount]*[args]*
 * with no length prefix, so a record can only be validated by reading it whole. For a
 * live observer the file underneath us is being appended by LANObserverClient as TCP
 * chunks arrive, and TCP is a byte stream, so the file routinely ends mid-record. Every
 * read here therefore has to be checked: previously only the first one was, and the rest
 * ignored their return values, so a torn record left the file offset stranded in the
 * middle of it. From there argument bytes get reinterpreted as frame numbers, m_nextFrame
 * goes to garbage and stops matching the logic frame, so commands quietly stop being
 * applied (the game appears to freeze and stutter), and eventually a garbage type is fed
 * to newInstance(GameMessage) and the observer crashes -- which the host sees as a send
 * error and drops them.
 *
 * On any short read, roll the file position back to the start of the record and let the
 * live-observer retry path re-read it once the rest of the bytes have landed. This mirrors
 * what readNextFrame() already does.
 */
void RecorderClass::appendNextCommand() {
	const Int posBefore = m_file->position();
	m_replayShortRead = FALSE;

	GameMessage::Type type;
	if (!readReplayBytes(&type, sizeof(type))) {
		rollbackTornRecord(posBefore);
		return;
	}

	GameMessage *msg = newInstance(GameMessage)(type);

#ifdef DEBUG_LOGGING
	AsciiString commandName = msg->getCommandAsString();
	if (type < GameMessage::MSG_BEGIN_NETWORK_MESSAGES || type > GameMessage::MSG_END_NETWORK_MESSAGES)
	{
		commandName.concat(" (Non-Network message!)");
	}
	else if (type == GameMessage::MSG_BEGIN_NETWORK_MESSAGES)
	{
		commandName.concat(" (CRC message!)");
	}
#endif // DEBUG_LOGGING

	Int playerIndex = -1;
	if (!readReplayBytes(&playerIndex, sizeof(playerIndex))) {
		deleteInstance(msg);
		rollbackTornRecord(posBefore);
		return;
	}
	msg->friend_setPlayerIndex(playerIndex);

	// don't debug log this if we're debugging sync errors, as it will cause diff problems between a game and it's replay...
#ifdef DEBUG_LOGGING
	Bool logCommand = true;
#ifdef DEBUG_CRC
	if (!m_doingAnalysis)
		logCommand = false;
#endif
	if (logCommand)
	{
		DEBUG_LOG(("RecorderClass::appendNextCommand - Adding %s command from player %d to TheCommandList on frame %d",
			commandName.str(), (type == GameMessage::MSG_BEGIN_NETWORK_MESSAGES)?0:msg->getPlayerIndex(), m_nextFrame/*TheGameLogic->getFrame()*/));
	}
#endif

	UnsignedByte numTypes = 0;
	Int totalArgs = 0;
	if (!readReplayBytes(&numTypes, sizeof(numTypes))) {
		deleteInstance(msg);
		rollbackTornRecord(posBefore);
		return;
	}

	GameMessageParser *parser = newInstance(GameMessageParser)();
	UnsignedByte i;
	for (i = 0; i < numTypes; ++i) {
		UnsignedByte argType = (UnsignedByte)ARGUMENTDATATYPE_UNKNOWN;
		UnsignedByte numArgs = 0;
		if (!readReplayBytes(&argType, sizeof(argType))
			|| !readReplayBytes(&numArgs, sizeof(numArgs))) {
			deleteInstance(parser);
			deleteInstance(msg);
			rollbackTornRecord(posBefore);
			return;
		}
		parser->addArgType((GameMessageArgumentDataType)argType, numArgs);
		totalArgs += numArgs;
	}

	GameMessageParserArgumentType *parserArgType = parser->getFirstArgumentType();
	GameMessageArgumentDataType lasttype = ARGUMENTDATATYPE_UNKNOWN;
	Int argsLeftForType = 0;
	if (parserArgType != nullptr) {
		lasttype = parserArgType->getType();
		argsLeftForType = parserArgType->getArgCount();
	}
	for (Int j = 0; j < totalArgs; ++j) {
		readArgument(lasttype, msg);
		if (m_replayShortRead) {
			deleteInstance(parser);
			deleteInstance(msg);
			rollbackTornRecord(posBefore);
			return;
		}

		--argsLeftForType;
		if (argsLeftForType == 0) {
			DEBUG_ASSERTCRASH(parserArgType != nullptr, ("parserArgType was null when it shouldn't have been."));
			if (parserArgType == nullptr) {
				return;
			}

			parserArgType = parserArgType->getNext();
			// parserArgType is allowed to be null here, this is the case if there are no more arguments.
			if (parserArgType != nullptr) {
				argsLeftForType = parserArgType->getArgCount();
				lasttype = parserArgType->getType();
			}
		}
	}

	if (type != GameMessage::MSG_BEGIN_NETWORK_MESSAGES && type != GameMessage::MSG_CLEAR_GAME_DATA && !m_doingAnalysis)
	{
		TheCommandList->appendMessage(msg);
	}
	else
	{
		deleteInstance(msg);
		msg = nullptr;
	}

	deleteInstance(parser);
	parser = nullptr;
}

void RecorderClass::readArgument(GameMessageArgumentDataType type, GameMessage *msg) {
	switch (type) {
		case ARGUMENTDATATYPE_INTEGER: {
			Int theint;
			if (!readReplayBytes(&theint, sizeof(theint))) {
				return;
			}
			msg->appendIntegerArgument(theint);
#ifdef DEBUG_LOGGING
			if (m_doingAnalysis)
			{
				DEBUG_LOG(("Integer argument: %d (%8.8X)", theint, theint));
			}
#endif
			break;
		}
		case ARGUMENTDATATYPE_REAL: {
			Real thereal;
			if (!readReplayBytes(&thereal, sizeof(thereal))) {
				return;
			}
			msg->appendRealArgument(thereal);
#ifdef DEBUG_LOGGING
			if (m_doingAnalysis)
			{
				DEBUG_LOG(("Real argument: %g (%8.8X)", thereal, *(int *)&thereal));
			}
#endif
			break;
		}
		case ARGUMENTDATATYPE_BOOLEAN: {
			Bool thebool;
			if (!readReplayBytes(&thebool, sizeof(thebool))) {
				return;
			}
			msg->appendBooleanArgument(thebool);
#ifdef DEBUG_LOGGING
			if (m_doingAnalysis)
			{
				DEBUG_LOG(("Bool argument: %d", thebool));
			}
#endif
			break;
		}
		case ARGUMENTDATATYPE_OBJECTID: {
			ObjectID theid;
			if (!readReplayBytes(&theid, sizeof(theid))) {
				return;
			}
			msg->appendObjectIDArgument(theid);
#ifdef DEBUG_LOGGING
			if (m_doingAnalysis)
			{
				DEBUG_LOG(("Object ID argument: %d", theid));
			}
#endif
			break;
		}
		case ARGUMENTDATATYPE_DRAWABLEID: {
			DrawableID theid;
			if (!readReplayBytes(&theid, sizeof(theid))) {
				return;
			}
			msg->appendDrawableIDArgument(theid);
#ifdef DEBUG_LOGGING
			if (m_doingAnalysis)
			{
				DEBUG_LOG(("Drawable ID argument: %d", theid));
			}
#endif
			break;
		}
		case ARGUMENTDATATYPE_TEAMID: {
			UnsignedInt theid;
			if (!readReplayBytes(&theid, sizeof(theid))) {
				return;
			}
			msg->appendTeamIDArgument(theid);
#ifdef DEBUG_LOGGING
			if (m_doingAnalysis)
			{
				DEBUG_LOG(("Team ID argument: %d", theid));
			}
#endif
			break;
		}
		case ARGUMENTDATATYPE_LOCATION: {
			Coord3D loc;
			if (!readReplayBytes(&loc, sizeof(loc))) {
				return;
			}
			msg->appendLocationArgument(loc);
#ifdef DEBUG_LOGGING
			if (m_doingAnalysis)
			{
				DEBUG_LOG(("Coord3D argument: %g %g %g (%8.8X %8.8X %8.8X)", loc.x, loc.y, loc.z,
					*(int *)&loc.x, *(int *)&loc.y, *(int *)&loc.z));
			}
#endif
			break;
		}
		case ARGUMENTDATATYPE_PIXEL: {
			ICoord2D pixel;
			if (!readReplayBytes(&pixel, sizeof(pixel))) {
				return;
			}
			msg->appendPixelArgument(pixel);
#ifdef DEBUG_LOGGING
			if (m_doingAnalysis)
			{
				DEBUG_LOG(("Pixel argument: %d,%d", pixel.x, pixel.y));
			}
#endif
			break;
		}
		case ARGUMENTDATATYPE_PIXELREGION: {
			IRegion2D reg;
			if (!readReplayBytes(&reg, sizeof(reg))) {
				return;
			}
			msg->appendPixelRegionArgument(reg);
#ifdef DEBUG_LOGGING
			if (m_doingAnalysis)
			{
				DEBUG_LOG(("Pixel Region argument: %d,%d -> %d,%d", reg.lo.x, reg.lo.y, reg.hi.x, reg.hi.y));
			}
#endif
			break;
		}
		case ARGUMENTDATATYPE_TIMESTAMP: {  // Not to be confused with Terrance Stamp... Kneel before Zod!!!
			UnsignedInt stamp;
			if (!readReplayBytes(&stamp, sizeof(stamp))) {
				return;
			}
			msg->appendTimestampArgument(stamp);
#ifdef DEBUG_LOGGING
			if (m_doingAnalysis)
			{
				DEBUG_LOG(("Timestamp argument: %d", stamp));
			}
#endif
			break;
		}
		case ARGUMENTDATATYPE_WIDECHAR: {
			WideChar theid;
			if (!readReplayBytes(&theid, sizeof(theid))) {
				return;
			}
			msg->appendWideCharArgument(theid);
#ifdef DEBUG_LOGGING
			if (m_doingAnalysis)
			{
				DEBUG_LOG(("WideChar argument: %d (%lc)", theid, theid));
			}
#endif
			break;
		}
		default:
			break;
	}
}

/**
 * This needs to be called for every frame during playback. Basically it prevents the user from inserting.
 */
RecorderClass::CullBadCommandsResult RecorderClass::cullBadCommands() {
	CullBadCommandsResult result;

	if (m_doingAnalysis)
		return result;

	GameMessage *msg = TheCommandList->getFirstMessage();
	GameMessage *next = nullptr;

	while (msg != nullptr) {
		next = msg->next();
		if ((msg->getType() > GameMessage::MSG_BEGIN_NETWORK_MESSAGES) &&
				(msg->getType() < GameMessage::MSG_END_NETWORK_MESSAGES) &&
				(msg->getType() != GameMessage::MSG_LOGIC_CRC)) {

			deleteInstance(msg);
		}
		else if (msg->getType() == GameMessage::MSG_CLEAR_GAME_DATA)
		{
			result.hasClearGameDataMessage = true;
		}

		msg = next;
	}

	return result;
}

/**
 * returns the directory that holds the replay files.
 */
AsciiString RecorderClass::getReplayDir()
{
	AsciiString tmp = TheGlobalData->getPath_UserData();
	tmp.concat("Replays\\");
	return tmp;
}

/**
 * returns the directory that holds the archived replay files.
 */
AsciiString RecorderClass::getReplayArchiveDir()
{
	AsciiString tmp = TheGlobalData->getPath_UserData();
	tmp.concat("ArchivedReplays\\");
	return tmp;
}

/**
 * returns the file extension for the replay files.
 */
AsciiString RecorderClass::getReplayExtention() {
	return AsciiString(replayExtention);
}

/**
 * returns the file name used for the replay file that is recorded to.
 */
AsciiString RecorderClass::getLastReplayFileName()
{
#if defined(RTS_DEBUG)
	if (TheNetwork && TheGlobalData->m_saveStats)
	{
		GameInfo *game = nullptr;
		if (TheLAN)
			game = TheLAN->GetMyGame();
		else if (TheGameSpyInfo)
			game = TheGameSpyGame;
		if (game)
		{
			AsciiString players;
			AsciiString full;
			AsciiString fullPlusNum;
			AsciiString mapName = game->getMap();
			const char *fname = mapName.reverseFind('\\');
			if (fname)
				mapName = fname+1;
			for (Int i=0; i<MAX_SLOTS; ++i)
			{
				GameSlot *slot = game->getSlot(i);
				if (slot && slot->isHuman())
				{
					AsciiString player;
					player.format("%ls_", slot->getName().str());
					players.concat(player);
				}
			}
			full.format("%s%s_%d_%d", players.str(), mapName.str(), game->getSeed(), game->getLocalSlotNum());
			AsciiString testString;
			testString.format("%s%s%s", getReplayDir().str(), full.str(), replayExtention);

			FILE *fp;
			fp = fopen(testString.str(), "rb");
			if (fp)
			{
				fclose(fp);
			}
			else
			{
				return full;
			}
			Int test = 1;
			while (test < 20)
			{
				fullPlusNum.format("%s_%d", full.str(), test);
				testString.format("%s%s%s", getReplayDir().str(), fullPlusNum.str(), replayExtention);
				fp = fopen(testString.str(), "rb");
				if (fp)
				{
					fclose(fp);
					++test;
				}
				else
				{
					return fullPlusNum;
				}
			}
			return fullPlusNum;
		}
	}
#endif

	AsciiString filename;
	if (rts::ClientInstance::getInstanceId() > 1u)
	{
		filename.format("%s_Instance%.2u", lastReplayFileName, rts::ClientInstance::getInstanceId());
	}
	else
	{
		filename = lastReplayFileName;
	}
	return filename;
}

/**
 * return the current operating mode of TheRecorder.
 */
RecorderModeType RecorderClass::getMode() {
	return m_mode;
}

///< Show or Hide the Replay controls
void RecorderClass::initControls()
{
	NameKeyType parentReplayControlID = TheNameKeyGenerator->nameToKey( "ReplayControl.wnd:ParentReplayControl" );
	GameWindow *parentReplayControl = TheWindowManager->winGetWindowFromId( nullptr, parentReplayControlID );

	Bool show = (getMode() != RECORDERMODETYPE_PLAYBACK);
	if (parentReplayControl)
	{
		parentReplayControl->winHide(show);	// show the replay control window.
	}
}

///< is this a multiplayer game (record OR playback)?
Bool RecorderClass::isMultiplayer()
{

	if (isPlaybackMode())
	{
		GameSlot *slot;
		for (int i=0; i<MAX_SLOTS; ++i)
		{
			slot = m_gameInfo.getSlot(i);
			if (slot && slot->isOccupied())	///< slots default to closed for non-networked games
				return true;
		}
	}
	if (TheGameLogic->getGameMode()==GAME_SINGLE_PLAYER) {
		return false; // single player isn't multiplayer.
	}
	if (TheGameLogic->getGameMode()==GAME_SHELL) {
		return false; // shell isn't multiplayer.
	}
	if (TheNetwork || TheSkirmishGameInfo)
		return true;

	return false;
}

// Resume-from-replay lead-in window. Logic frames before the handoff at
// which we drop the FF rate caps AND re-enable the renderer so players
// see a realtime preview before control is handed back. 300 = 10s at
// 30 logic fps.
static const UnsignedInt FF_OFF_LEAD_FRAMES = 300;

Bool RecorderClass::isResumeCatchupLeadIn() const
{
	if (!isResumeCatchupMode())
		return false;
	const UnsignedInt curFrame = TheGameLogic ? TheGameLogic->getFrame() : 0;
	return (m_resumeHandoffFrame >= FF_OFF_LEAD_FRAMES
		&& curFrame >= m_resumeHandoffFrame - FF_OFF_LEAD_FRAMES);
}

/**
 * Resume-from-replay catchup: open the given replay file, skip past the
 * header, prime the first frame of commands, and switch the recorder into
 * RECORDERMODETYPE_RESUME_CATCHUP. While in catchup, update() calls
 * updateResumeCatchup() instead of updateRecord()/updatePlayback(), so the
 * replay's commands are injected into TheCommandList each frame while
 * network traffic continues to flow through the usual LAN path.
 */
Bool RecorderClass::startResumeCatchup(AsciiString filename, UnsignedInt handoffFrame)
{
	// Clean up any existing file handle from a prior mode.
	if (m_file != nullptr)
	{
		m_file->close();
		m_file = nullptr;
	}
	m_mode = RECORDERMODETYPE_NONE;

	ReplayHeader header;
	header.forPlayback = TRUE;
	header.filename = filename;
	if (!readReplayHeader(header))
	{
		DEBUG_LOG(("RecorderClass::startResumeCatchup - readReplayHeader failed for %s", filename.str()));
		return FALSE;
	}

	// playbackFile writes difficulty, originalGameMode, rankPoints, maxFPS
	// to the file AFTER the header and BEFORE the command stream. We must
	// consume those same bytes here or readNextFrame will interpret the
	// difficulty as a frame number and never match curFrame.
	GameDifficulty difficulty = DIFFICULTY_NORMAL;
	m_file->read(&difficulty, sizeof(difficulty));
	m_file->read(&m_originalGameMode, sizeof(m_originalGameMode));
	Int rankPoints = 0;
	m_file->read(&rankPoints, sizeof(rankPoints));
	Int maxFPS = 0;
	m_file->read(&maxFPS, sizeof(maxFPS));

	// Mirror playbackFile's handling of the Zulu extension block.
	readZuluReplayExtension();

	m_mode = RECORDERMODETYPE_RESUME_CATCHUP;
	m_resumeHandoffFrame = handoffFrame;
	m_currentReplayFilename = filename;

	// Everything up to here is the replay header, and it is the point we would keep if
	// the handoff arrived before a single command was consumed. beginRecordingAfterResume
	// truncates here and appends live commands, so this has to be the offset just past the
	// header and before the first frame number.
	m_resumeRecordPos = m_file->position();

	// Prime m_nextFrame with the frame number of the first recorded command.
	readNextFrame();

	// Disable local input so clicks during catchup don't produce stray
	// commands that would diverge from the replay.
	if (TheInGameUI)
		TheInGameUI->setInputEnabled(FALSE);

	// Raise both the FramePacer's FPS limit AND the network's per-frame
	// timing rate so logic can advance faster. Two gates were limiting us:
	// (1) renderer FPS cap, (2) Network::timeForNewFrame() which uses
	// m_frameRate as its own rate gate independent of the renderer.
	// Lockstep is unchanged — each frame still goes through full network
	// ack, just faster. On LAN this gives a real speedup; on slower
	// networks it tops out at whatever round-trip allows.
	if (TheFramePacer)
	{
		m_resumeSavedFpsLimit = TheFramePacer->getFramesPerSecondLimit();
		// Guard the value we will restore on handoff. getFramesPerSecondLimit() can
		// hand back 0 (it is an unvalidated Int), and restoring a 0 later would put
		// FrameRateLimit::wait into an infinite busy-wait. The live-observer path at
		// playbackFileLiveObserver() already does this; the resume path did not.
		if (m_resumeSavedFpsLimit <= 0)
		{
			m_resumeSavedFpsLimit = 30;
		}
		TheFramePacer->setFramesPerSecondLimit(1000);
	}
	if (TheNetwork)
	{
		m_resumeSavedNetFrameRate = TheNetwork->setLogicFrameRate(1000);
	}

	DEBUG_LOG(("RecorderClass::startResumeCatchup - catching up %s to frame %u",
		filename.str(), handoffFrame));
	return TRUE;
}

/**
 * Hand the replay file over from catchup (reading) to recording (writing), so the rest of
 * a resumed match is recorded and stopRecording() runs when it ends.
 *
 * Resume always arms the last replay, which is the same file startRecording would write,
 * and it already holds the header plus every frame up to the handoff. So rather than start
 * a fresh recording -- which would truncate the header and produce a replay that begins
 * midway through the match -- we keep the bytes we consumed and append live commands to
 * them. The result is one complete replay of the whole game.
 *
 * We truncate at the last record we actually replayed, which drops the tail of the old
 * recording (including its MSG_CLEAR_GAME_DATA). Leaving that tail in place would put
 * records for already-played frames after the ones we are about to append, and the frame
 * numbers would run backwards.
 */
Bool RecorderClass::beginRecordingAfterResume()
{
	if (m_file == nullptr || m_resumeRecordPos <= 0)
		return FALSE;

	// Replays are command logs, not state dumps, so the whole prefix is small (a long 8p
	// match is a few hundred KB). Buffering it is cheaper than any in-place truncate the
	// File interface does not offer.
	const Int prefixLen = m_resumeRecordPos;
	char *prefix = NEW char[prefixLen];
	m_file->seek(0, File::seekMode::START);
	const Int got = m_file->read(prefix, prefixLen);
	m_file->close();
	m_file = nullptr;

	if (got != prefixLen)
	{
		DEBUG_LOG(("RecorderClass::beginRecordingAfterResume - short read of replay prefix (%d of %d)", got, prefixLen));
		delete [] prefix;
		return FALSE;
	}

	AsciiString fileName = getLastReplayFileName();
	fileName.concat(getReplayExtention());

	AsciiString filepath = getReplayDir();
	TheFileSystem->createDirectory(filepath);
	filepath.concat(fileName);

	// WRITE truncates, which is what gives us the truncate-at-prefix we want.
	m_file = TheFileSystem->openFile(filepath.str(), File::WRITE | File::BINARY);
	if (m_file == nullptr)
	{
		DEBUG_LOG(("RecorderClass::beginRecordingAfterResume - could not reopen %s for recording", filepath.str()));
		delete [] prefix;
		return FALSE;
	}

	m_file->write(prefix, prefixLen);
	delete [] prefix;
	m_file->flush();

	// m_fileName is what stopRecording keys the replay/stats/log uploads off, and RECORD
	// (not NONE) is also what keeps update() pumping the observer host, so live observers
	// survive the handoff too.
	m_fileName = fileName;
	m_mode     = RECORDERMODETYPE_RECORD;

	DEBUG_LOG(("RecorderClass::beginRecordingAfterResume - recording resumed game into %s from offset %d", fileName.str(), prefixLen));
	return TRUE;
}

/**
 * Per-frame update for RESUME_CATCHUP mode. Strips any local user input
 * that slipped into TheCommandList, injects the replay's recorded commands
 * for the current logic frame, then exits catchup cleanly when the handoff
 * frame is reached (or the replay ends sooner). Unlike stopPlayback this
 * does NOT call exitGame — the LAN session continues uninterrupted.
 */
void RecorderClass::updateResumeCatchup()
{
	UnsignedInt curFrame = TheGameLogic ? TheGameLogic->getFrame() : 0;

	// Catchup progress: shows where we are without having to render the
	// game. Window title updates every 30 logic frames so the OS title bar
	// ticks visibly (1Hz at retail rate, ~30Hz during FF); DEBUG_LOG fires
	// every 300 logic frames so the timeline in DebugLogFile.txt stays
	// readable. Statics re-init when curFrame walks backward (new catchup
	// session), so back-to-back resumes report from frame 0 each time.
	static UnsignedInt s_progressStartFrame      = 0;
	static UnsignedInt s_progressStartMs         = 0;
	static UnsignedInt s_progressNextReportFrame = 0;
	if (s_progressStartFrame == 0 || curFrame < s_progressStartFrame)
	{
		s_progressStartFrame      = curFrame;
		s_progressStartMs         = timeGetTime();
		s_progressNextReportFrame = curFrame;
	}
	if (curFrame >= s_progressNextReportFrame)
	{
		const UnsignedInt nowMs        = timeGetTime();
		const UnsignedInt elapsedMs    = nowMs - s_progressStartMs;
		const UnsignedInt elapsedFrame = curFrame - s_progressStartFrame;
		const Real fps = elapsedMs > 0
			? (Real)elapsedFrame * 1000.0f / (Real)elapsedMs
			: 0.0f;
		extern HWND ApplicationHWnd;
		if (ApplicationHWnd)
		{
			char buf[128];
			snprintf(buf, sizeof(buf),
				"Generals - Catchup: frame %u / %u  (%.0f logic fps)",
				curFrame, m_resumeHandoffFrame, fps);
			::SetWindowTextA(ApplicationHWnd, buf);
		}
		// One log line per 300 frames (10 logic seconds of recorded game).
		// s_progressNextReportFrame is advanced by 30 below, so checking
		// modulo 300 lets every 10th report through.
		if (elapsedFrame == 0 || (elapsedFrame % 300) < 30)
		{
			DEBUG_LOG(("Catchup progress: frame %u / %u  (%u/%u done, %.1f logic fps wall-clock, %u ms elapsed)",
				curFrame, m_resumeHandoffFrame,
				elapsedFrame,
				m_resumeHandoffFrame > s_progressStartFrame ? m_resumeHandoffFrame - s_progressStartFrame : 0,
				fps, elapsedMs));
		}
		s_progressNextReportFrame = curFrame + 30;
	}

	// Drop both rate caps back to normal once we're within 10 seconds (300
	// logic frames at 30fps) of the handoff so players see a realtime preview
	// before control is handed back. If the handoff is closer than that to
	// the start of catchup, just stay at the elevated rate.
	const Bool inLeadIn = isResumeCatchupLeadIn();
	if (inLeadIn)
	{
		if (TheFramePacer
			&& TheFramePacer->getFramesPerSecondLimit() != m_resumeSavedFpsLimit)
		{
			TheFramePacer->setFramesPerSecondLimit(m_resumeSavedFpsLimit);
		}
		if (TheNetwork)
		{
			TheNetwork->setLogicFrameRate(m_resumeSavedNetFrameRate);
		}
	}

	// NOTE: cullBadCommands() intentionally skipped during catchup.
	// cullBadCommands strips any command in the MSG_BEGIN_NETWORK_MESSAGES
	// range from TheCommandList, which includes exactly the commands we
	// just appended via appendNextCommand. During pure single-player
	// playback the cull runs before injection so the sequence is fine, but
	// here the live LAN network layer is also writing into TheCommandList
	// each frame — culling in the middle is not safe. Local UI input is
	// already suppressed via InGameUI::setGUICommand, which is the right
	// gate for this mode.

	// Inject every command recorded for this frame. Bounded by the handoff
	// frame so we never drain past the handover even if curFrame somehow
	// runs ahead. We do this BEFORE the handoff exit check so the handoff
	// frame ITSELF gets its recorded commands injected: GameLogic's CRC
	// validator runs in processCommandList immediately after the recorder
	// update, and with inCatchup=FALSE post-exit it expects MSG_LOGIC_CRC
	// from every connected player. Live peer CRCs may not have arrived yet
	// during the FF-speed run-up, so without the .rep's recorded CRCs in
	// TheCommandList the validator fires "Not enough CRCs!" and the
	// handoff frame fails on every client.
	while (m_nextFrame == curFrame && curFrame <= m_resumeHandoffFrame)
	{
		appendNextCommand();
		// End of the last record we actually consumed. readNextFrame() below reads the
		// NEXT record's frame number, so the position after it is already inside a record
		// we are not going to replay. This is where beginRecordingAfterResume truncates.
		//
		// Only advance it on a record we read in FULL. appendNextCommand leaves the file
		// offset stranded mid-record on a short read (it can only roll back for a live
		// observer, whose file is still growing), and truncating there would splice a
		// half-written command into the replay we are about to keep recording into.
		if (!m_replayShortRead)
		{
			m_resumeRecordPos = m_file->position();
		}
		readNextFrame();
	}

	// Handoff condition: we've reached the handoff frame OR the replay file
	// ran out of commands before we got there.
	if (curFrame >= m_resumeHandoffFrame || m_nextFrame == (UnsignedInt)-1)
	{
		// Take the replay file over for RECORDING rather than just dropping it. Everything
		// that closes out a match -- the "Match end" release-log line, logGameEnd, the
		// replay upload, the stats upload and the per-player log upload -- hangs off
		// stopRecording(), and updateRecord only reaches stopRecording when m_file is
		// non-null. Going to RECORDERMODETYPE_NONE with a null file meant a resumed game
		// finished having uploaded nothing at all.
		if (!beginRecordingAfterResume())
		{
			DEBUG_LOG(("RecorderClass::updateResumeCatchup - could not take over the replay file for recording; this game will not upload"));
			if (m_file != nullptr)
			{
				m_file->close();
				m_file = nullptr;
			}
			m_mode = RECORDERMODETYPE_NONE;
		}
		m_currentReplayFilename.clear();

		// Hand input back to the players. (Network resync was needed when
		// FF was bypassing the lockstep gate; with realtime catchup the
		// network has been keeping pace and no resync is required.)
		if (TheInGameUI)
			TheInGameUI->setInputEnabled(TRUE);

		// Restore both rate caps to their pre-catchup values.
		if (TheFramePacer)
			TheFramePacer->setFramesPerSecondLimit(m_resumeSavedFpsLimit);
		if (TheNetwork)
			TheNetwork->setLogicFrameRate(m_resumeSavedNetFrameRate);

		// Show an in-game notification so every player can see that control
		// has been handed over. TheLAN->OnChat only writes to the lobby's
		// chat listboxes, which don't exist during live gameplay. Each peer
		// hits this code path in lockstep so the message renders on every
		// client.
		if (TheInGameUI)
		{
			TheInGameUI->message("GUI:ResumeHandoffComplete");
		}

		DEBUG_LOG(("RecorderClass::updateResumeCatchup - handoff at frame %u", curFrame));
		return;
	}
}

Bool isViewerOnlyClient()
{
	if (TheRecorder && TheRecorder->isPlaybackMode())
		return TRUE;
	if (ThePlayerList)
	{
		Player *local = ThePlayerList->getLocalPlayer();
		// A defeated player has no army left to command, so they are as much a
		// viewer as a lobby observer is. isPlayerActive() is false for both
		// (!m_observer && !m_isPlayerDead), which promotes the dead player to the
		// full observer UI -- production queues, garrison contents, income -- and
		// at the same time extends the command-issuance block in processCommandUI
		// to them.
		if (local && !local->isPlayerActive())
			return TRUE;
	}
	return FALSE;
}

/**
 * Create a new recorder object.
 */
RecorderClass * createRecorder() {
	return NEW RecorderClass;
}
