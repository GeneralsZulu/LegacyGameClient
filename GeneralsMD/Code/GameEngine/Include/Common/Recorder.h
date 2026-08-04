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

#pragma once

#include "Common/MessageStream.h"
#include "GameNetwork/GameInfo.h"

class File;

/**
  * The ReplayGameInfo class holds information about the replay game and
	* the contents of its slot list for reconstructing multiplayer games.
	*/
class ReplayGameInfo : public GameInfo
{
private:
	GameSlot m_ReplaySlot[MAX_SLOTS];

public:
	ReplayGameInfo()
	{
		for (Int i = 0; i< MAX_SLOTS; ++i)
			setSlotPointer(i, &m_ReplaySlot[i]);
	}
};

enum RecorderModeType CPP_11(: Int) {
	RECORDERMODETYPE_RECORD,
	RECORDERMODETYPE_PLAYBACK,
	RECORDERMODETYPE_SIMULATION_PLAYBACK, // Play back replay without any graphics
	RECORDERMODETYPE_NONE, // this is a valid state to be in on the shell map, or in saved games
	RECORDERMODETYPE_RESUME_CATCHUP,      // Resume-from-replay: feed replay commands into an active LAN game until handoff frame.
	RECORDERMODETYPE_LIVE_OBSERVER        // LAN observer: identical to PLAYBACK except readNextFrame waits at EOF for more bytes to arrive over the network instead of stopping the playback.
};

class CRCInfo;

class RecorderClass : public SubsystemInterface {
public:
	struct ReplayHeader;

public:
	RecorderClass();																	///< Constructor.
	virtual ~RecorderClass() override;													///< Destructor.

	virtual void init() override;																			///< Initialize TheRecorder.
	virtual void reset() override;																			///< Reset the state of TheRecorder.
	virtual void update() override;																		///< General purpose update function.

	// Methods dealing with recording.
	void updateRecord();															///< The update function for recording.

	// Methods dealing with playback.
	void updatePlayback();														///< The update function for playing back a file.
	Bool playbackFile(AsciiString filename);					///< Starts playback of the specified file.
	Bool replayMatchesGameVersion(AsciiString filename); ///< Returns true if the playback is a valid playback file for this version.
	static Bool replayMatchesGameVersion(const ReplayHeader& header); ///< Returns true if the playback is a valid playback file for this version.
	AsciiString getCurrentReplayFilename();			///< valid during playback only
	UnsignedInt getPlaybackFrameCount() const { return m_playbackFrameCount; }			///< valid during playback only
	void stopPlayback();															///< Stops playback.  Its fine to call this even if not playing back a file.
	Bool simulateReplay(AsciiString filename);

	// Playback-fidelity signal: replayed commands whose object IDs did not
	// resolve in our re-simulated world. The recording client only issued
	// commands against objects that existed for it, so anything beyond the
	// occasional clicked-a-dying-unit race points at a diverged simulation.
	// Used by the replay verdict for retail-epoch replays, whose recorded
	// checksums are unusable (see handleCRCMessage).
	UnsignedInt getPlaybackStaleObjectRefs() const { return m_playbackStaleObjectRefs; }
	void notePlaybackStaleObjectRef() { ++m_playbackStaleObjectRefs; }
#if defined(RTS_DEBUG)
	Bool analyzeReplay( AsciiString filename );
#endif
	Bool isPlaybackInProgress() const;

public:
	void handleCRCMessage(UnsignedInt newCRC, Int playerIndex, Bool fromPlayback);
protected:
	CRCInfo *m_crcInfo;
public:

	// read in info relating to a replay, conditionally setting up m_file for playback
	struct ReplayHeader
	{
		AsciiString filename;
		Bool forPlayback;
		UnicodeString replayName;
		SYSTEMTIME timeVal;
		UnicodeString versionString;
		UnicodeString versionTimeString;
		UnsignedInt versionNumber;
		UnsignedInt exeCRC;
		UnsignedInt iniCRC;
		time_t startTime;
		time_t endTime;
		UnsignedInt frameCount;
		Bool quitEarly;
		Bool desyncGame;
		Bool playerDiscons[MAX_SLOTS];
		AsciiString gameOptions;
		Int localPlayerIndex;
	};
	Bool readReplayHeader( ReplayHeader& header );

	RecorderModeType getMode();												///< Returns the current operating mode.
	Bool isPlaybackMode() const { return m_mode == RECORDERMODETYPE_PLAYBACK || m_mode == RECORDERMODETYPE_SIMULATION_PLAYBACK || m_mode == RECORDERMODETYPE_LIVE_OBSERVER; }
	Bool isResumeCatchupMode() const { return m_mode == RECORDERMODETYPE_RESUME_CATCHUP; }
	// True during the lead-in window of resume-from-replay catchup (last
	// few seconds before handoff, currently 10 logic seconds). Callers
	// use this to re-enable normally-skipped catchup work — e.g. the
	// renderer — so players see realtime motion just before control is
	// handed back. Returns false when not in catchup, when handoff is
	// closer than the lead-in window to the start of catchup, or once
	// catchup has ended.
	Bool isResumeCatchupLeadIn() const;
	Bool isLiveObserverMode() const { return m_mode == RECORDERMODETYPE_LIVE_OBSERVER; }
	// Returns true whenever the recorder is driving local playback and local
	// user input should be suppressed / not fed into TheCommandList.
	Bool isSuppressingLocalInput() const { return isPlaybackMode() || isResumeCatchupMode(); }

	// LIVE_OBSERVER. Same as playbackFile() but flips the mode so that
	// readNextFrame waits at EOF for more bytes rather than calling
	// stopPlayback. The caller (the observer-stream client) tells us when
	// the network stream has closed so we know when to actually stop.
	Bool playbackFileLiveObserver(AsciiString filename);
	void setLiveObserverStreamOpen(Bool open) { m_liveObserverStreamOpen = open; }
	Bool isLiveObserverWaitingForBytes() const { return m_liveObserverWaitingForBytes; }

	// Resume-from-replay. Opens the given replay file, skips past the header,
	// and sets the recorder into RECORDERMODETYPE_RESUME_CATCHUP. While in
	// catchup, update() reads commands per frame from the file and injects
	// them into TheCommandList, running alongside a live LAN session. When
	// the current game frame reaches handoffFrame (or the file ends), the
	// catchup ends and the recorder returns to NONE so live input takes over.
	Bool startResumeCatchup(AsciiString filename, UnsignedInt handoffFrame);
	void updateResumeCatchup();
	// At handoff, take the replay file over for recording instead of dropping it, so the
	// resumed match is recorded and stopRecording() (which is what uploads the replay, the
	// stats and the per-player logs) actually runs when the game ends.
	Bool beginRecordingAfterResume();
	void initControls();															///< Show or Hide the Replay controls

	static AsciiString getReplayDir();								///< Returns the directory that holds the replay files.
	static AsciiString getReplayArchiveDir();					///< Returns the directory that holds the archived replay files.
	static AsciiString getReplayExtention();					///< Returns the file extention for replay files.
	static AsciiString getLastReplayFileName();				///< Returns the filename used for the default replay.

	GameInfo *getGameInfo() { return &m_gameInfo; }	///< Returns the slot list for playback game start

	Bool isMultiplayer();												///< is this a multiplayer game (record OR playback)?

	// Zulu replay extension. Replay-affecting AI features added by the Zulu
	// mod are tagged with a monotonic feature-version. New binaries playing
	// older replays disable any feature whose version exceeds what the replay
	// declares, so older replays remain deterministically replayable. Old
	// binaries cannot play new replays — that is by design.
	enum
	{
		ZULU_AI_FEATURE_NONE        = 0,
		ZULU_AI_FEATURE_IDLE_COMMIT = 1, ///< AISkirmishPlayer::commitIdleArmy()
		ZULU_AI_FEATURE_SPOT_SOLVER = 2, ///< global start-position solver in populateRandomStartPosition(), incl. teammate spot shuffle
		ZULU_AI_FEATURE_USA_DRONES  = 3, ///< TacticalAI USA vehicle drones (AISkirmishPlayer::buyObjectUpgrades)
		ZULU_AI_FEATURE_CHINA_OVERLORD = 4, ///< TacticalAI China Overlord gattling/propaganda (AISkirmishPlayer::buyObjectUpgrades)
		ZULU_AI_FEATURE_MD_LASER_LOCK = 5, ///< TacticalAI USA Missile Defender laser lock (AISkirmishPlayer::laserLockMissileDefenders)
		ZULU_AI_FEATURE_CURRENT     = 5
	};
	// True if the AI feature at the given version should run this frame.
	// Always true during live games / recording. During playback / resume-catchup
	// / simulation-playback, true only if the loaded replay declared support
	// for that feature version.
	Bool isAIFeatureEnabled(UnsignedInt featureVersion) const;

	// TheSuperHackers @feature Replay determinism epochs. Some sim behaviors
	// changed across Zulu versions WITHOUT being gated by the AI-feature block
	// above (they shipped ungated), so old replays don't carry a flag to
	// disable them. To re-simulate an old replay bit-exactly on a newer binary,
	// we derive a monotonic "epoch" from the replay's recorded version (header
	// version string, or the -replayEpoch override) and gate each divergent
	// path on it. Higher = newer behavior; CURRENT during live play/recording.
	enum ReplayEpoch
	{
		REPLAY_EPOCH_RETAIL = 0, ///< retail 1.04 / Zulu <= 1.2.0
		REPLAY_EPOCH_V121   = 1, ///< 1.2.1+: create-team processed at network frame for all players
		REPLAY_EPOCH_V128   = 2, ///< 1.2.8+: poison/flame XP source, crate multi-pickup guard, scaffold-resume-on-dead-builder
		REPLAY_EPOCH_V130   = 3, ///< 1.3.0+: AISkirmishPlayer surrender directive
		REPLAY_EPOCH_CURRENT = REPLAY_EPOCH_V130
	};
	// The epoch to simulate. CURRENT during live play/recording; during playback
	// it is the recorded version's epoch (override or auto-detected).
	ReplayEpoch getReplayEpoch() const;
	// True if the behavior introduced at 'epoch' should run this frame.
	Bool isReplayEpochAtLeast(ReplayEpoch epoch) const { return getReplayEpoch() >= epoch; }
	// Command-line override (-replayEpoch); <0 means "auto-detect from header".
	static void setReplayEpochOverride(Int epoch) { s_replayEpochOverride = epoch; }
	static Int getReplayEpochOverride() { return s_replayEpochOverride; }

	Int getGameMode() { return m_originalGameMode; }

	void logPlayerDisconnect(UnicodeString player, Int slot);
	void logCRCMismatch();
	Bool sawCRCMismatch() const;
	void cleanUpReplayFile();										///< after a crash, send replay/debug info to a central repository

	void setArchiveEnabled(Bool enable) { m_archiveReplays = enable; } ///< Enable or disable replay archiving.
	void stopRecording();															///< Stop recording and close m_file.
protected:
	void startRecording(GameDifficulty diff, Int originalGameMode, Int rankPoints, Int maxFPS);					///< Start recording to m_file.
	void writeToFile(GameMessage *msg);								///< Write this GameMessage to m_file.

	// Zulu replay extension block. Written immediately after maxFPS in the
	// header and consumed at the same point on read.
	void writeZuluReplayExtension();
	void readZuluReplayExtension();
	void archiveReplay(AsciiString fileName);					///< Move the specified replay file to the archive directory.

	void logGameStart(AsciiString options);
	void logGameEnd();

	AsciiString readAsciiString();										///< Read the next string from m_file using ascii characters.
	UnicodeString readUnicodeString();								///< Read the next string from m_file using unicode characters.
	void readNextFrame();															///< Read the next frame number to execute a command on.
	void appendNextCommand();													///< Read the next GameMessage and append it to TheCommandList.
	void writeArgument(GameMessageArgumentDataType type, const GameMessageArgumentType arg);
	void readArgument(GameMessageArgumentDataType type, GameMessage *msg);

	/// Read exactly size bytes from m_file. On a short read, set m_replayShortRead and
	/// return FALSE. Every read that parses a replay record must go through this: a live
	/// observer is parsing a file another thread is still appending to, so records can
	/// and do end mid-write.
	Bool readReplayBytes(void *dst, Int size);
	void rollbackTornRecord(Int posBefore);						///< Rewind to the start of a record that was only partially available.
	Bool m_replayShortRead;														///< Set when a record could not be read in full.

	struct CullBadCommandsResult
	{
		CullBadCommandsResult() : hasClearGameDataMessage(false) {}
		Bool hasClearGameDataMessage;
	};

	CullBadCommandsResult cullBadCommands(); ///< prevent the user from giving mouse commands that he shouldn't be able to do during playback.

	File* m_file;
	AsciiString m_fileName;
	Int m_currentFilePosition;
	RecorderModeType m_mode;
	AsciiString m_currentReplayFilename;							///< valid during playback only
	UnsignedInt m_playbackFrameCount;

	ReplayGameInfo m_gameInfo;
	Bool m_wasDesync;

	Bool m_doingAnalysis;
	Bool m_archiveReplays;														///< if true, each replay is archived to the replay archive folder after recording

	Int m_originalGameMode; // valid in replays

	UnsignedInt m_nextFrame;												///< The Frame that the next message is to be executed on.  This can be -1.
	UnsignedInt m_resumeHandoffFrame;								///< During RESUME_CATCHUP, the frame at which to stop injecting replay commands and return to NONE.
	Int m_resumeSavedFpsLimit;											///< During RESUME_CATCHUP, the FPS limit at start so we can restore at handoff.
	Int m_resumeSavedNetFrameRate;									///< During RESUME_CATCHUP, the network's logic frame rate at start so we can restore at handoff.
	Int m_resumeRecordPos;													///< During RESUME_CATCHUP, byte offset just past the last record we replayed; where beginRecordingAfterResume truncates.

	UnsignedInt m_replayAIFeatureVersion;						///< CURRENT for live games; loaded from the replay's extension block during playback.

	ReplayEpoch m_replayEpoch;										///< CURRENT for live games; derived from the replay's recorded version during playback.
	static Int s_replayEpochOverride;								///< -replayEpoch command-line override; <0 = auto-detect from header.

	UnsignedInt m_playbackStaleObjectRefs;					///< Replayed commands referencing objects missing from our world; see getPlaybackStaleObjectRefs().

	// LIVE_OBSERVER state. m_liveObserverStreamOpen is owned by the observer
	// network client and tells us whether to expect more bytes after EOF.
	// m_liveObserverWaitingForBytes is set when readNextFrame hit EOF and
	// updatePlayback should retry on the next tick instead of advancing.
	// m_liveObserverRetryPos is the file position to reopen-and-seek-to on
	// retry (the engine File's stdio buffer otherwise hides new bytes written
	// by the host after our EOF).
	// m_liveObserverFpsBoosted is TRUE while we're draining the initial
	// snapshot at high FPS; cleared on first EOF when we drop back to
	// m_liveObserverSavedFpsLimit and play in real-time.
	Bool        m_liveObserverStreamOpen;
	// TRUE only while playbackFileLiveObserver is inside its playbackFile
	// call: makes the open-time first readNextFrame treat EOF as "wait for
	// the host's first command" instead of stopPlayback. Needed when
	// observing a game in its first seconds, where the streamed snapshot
	// holds a header but no commands yet. Deliberately NOT reset in init()
	// (which runs mid-playbackFile via clearGameData); constructor and
	// playbackFileLiveObserver are its only writers.
	Bool        m_liveObserverArming;
	Bool        m_liveObserverWaitingForBytes;
	Int         m_liveObserverRetryPos;
	Bool        m_liveObserverFpsBoosted;
	Int         m_liveObserverSavedFpsLimit;
	UnsignedInt m_liveObserverStarvedSinceMs; ///< wall clock when the byte starvation began, 0 while fed
};

extern RecorderClass *TheRecorder;
RecorderClass *createRecorder();

// True when the local client is viewing a game it doesn't control: any
// playback flavor (regular, simulation, live-observer) or when the local
// player occupies an observer slot. Used by ControlBar/InGameUI to surface
// production queues and garrison contents on non-friendly buildings.
Bool isViewerOnlyClient();
