/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 TheSuperHackers
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

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/ReplaySimulation.h"

#include "Common/GameEngine.h"
#include "Common/GlobalData.h"
#include "Common/LocalFileSystem.h"
#include "Common/Recorder.h"
#include "Common/StatsExporter.h"
#include "Common/WorkerProcess.h"
#include "GameLogic/GameLogic.h"
#include "GameClient/GameClient.h"


Bool ReplaySimulation::s_isRunning = false;
UnsignedInt ReplaySimulation::s_replayIndex = 0;
UnsignedInt ReplaySimulation::s_replayCount = 0;

namespace
{
int countProcessesRunning(const std::vector<WorkerProcess>& processes)
{
	int numProcessesRunning = 0;
	size_t i = 0;
	for (; i < processes.size(); ++i)
	{
		if (processes[i].isRunning())
			++numProcessesRunning;
	}
	return numProcessesRunning;
}

// Minimal JSON string escaper: escapes the characters that would otherwise
// break a JSON string literal. Windows replay paths contain backslashes, so
// this matters. Not a general-purpose escaper; sufficient for filenames.
AsciiString escapeJsonString(const AsciiString &in)
{
	AsciiString out;
	const char *s = in.str();
	for (; *s; ++s)
	{
		char c = *s;
		switch (c)
		{
			case '\\': out.concat("\\\\"); break;
			case '\"': out.concat("\\\""); break;
			case '\n': out.concat("\\n"); break;
			case '\r': out.concat("\\r"); break;
			case '\t': out.concat("\\t"); break;
			default:
			{
				char buf[2] = { c, 0 };
				out.concat(buf);
				break;
			}
		}
	}
	return out;
}
} // namespace

void ReplaySimulation::appendResultLogEntry(const AsciiString &filename, const char *verdict,
	UnsignedInt framesPlayed, UnsignedInt framesExpected, Bool statsExported, Int epoch,
	UnsignedInt staleRefs)
{
	const AsciiString &logPath = TheGlobalData->m_replayResultLog;
	if (logPath.isEmpty())
		return;

	// Append mode so that concurrent worker processes each contribute their
	// own line. Single small writes keep interleaving to whole lines in
	// practice. Open/close per line so a crash mid-batch still flushes prior
	// results to disk.
	FILE *fp = fopen(logPath.str(), "a");
	if (fp == NULL)
	{
		printf("[resultLog] ERROR: could not open %s for append\n", logPath.str());
		fflush(stdout);
		return;
	}

	fprintf(fp,
		"{\"file\":\"%s\",\"verdict\":\"%s\",\"framesPlayed\":%u,\"framesExpected\":%u,\"epoch\":%d,\"statsExported\":%s,\"staleRefs\":%u}\n",
		escapeJsonString(filename).str(),
		verdict,
		framesPlayed,
		framesExpected,
		epoch,
		statsExported ? "true" : "false",
		staleRefs);

	fclose(fp);
}

int ReplaySimulation::simulateReplaysInThisProcess(const std::vector<AsciiString> &filenames)
{
	int numErrors = 0;

	if (!TheGlobalData->m_headless)
	{
		s_isRunning = true;
		s_replayIndex = 0;
		s_replayCount = static_cast<UnsignedInt>(filenames.size());

		// If we are not in headless mode, we need to run the replay in the engine.
		for (; s_replayIndex < s_replayCount; ++s_replayIndex)
		{
			TheRecorder->playbackFile(filenames[s_replayIndex]);
			TheGameEngine->execute();
			if (TheRecorder->sawCRCMismatch())
				numErrors++;
			if (!s_isRunning)
				break;
			TheGameEngine->setQuitting(FALSE);
		}
		s_isRunning = false;
		s_replayIndex = 0;
		s_replayCount = 0;
		return numErrors != 0 ? 1 : 0;
	}
	// Note that we use printf here because this is run from cmd.
	DWORD totalStartTimeMillis = GetTickCount();
	for (size_t i = 0; i < filenames.size(); i++)
	{
		AsciiString filename = filenames[i];
		printf("Simulating Replay \"%s\"\n", filename.str());
		fflush(stdout);
		// TheSuperHackers @feature Crash-safety marker. A desync corrupts the
		// game state, and with crash-handling compiled out the next UPDATE()
		// can hard-crash mid-frame before we get to write the final verdict.
		// Write a STARTED line up front so a crashed replay still leaves a
		// trace: post-processing treats a file whose last line is STARTED as a
		// crash (usually a hard desync). A clean/desynced run appends a final
		// line that supersedes it.
		appendResultLogEntry(filename, "STARTED", 0, 0, FALSE, -1, 0);
		DWORD startTimeMillis = GetTickCount();
		if (TheGlobalData->m_exportStats)
			StatsExporterBeginRecording();
		if (TheRecorder->simulateReplay(filename))
		{
			UnsignedInt framesExpected = TheRecorder->getPlaybackFrameCount();
			UnsignedInt totalTimeSec = framesExpected / LOGICFRAMES_PER_SECOND;
			Bool desynced = FALSE;
			while (TheRecorder->isPlaybackInProgress())
			{
				TheGameClient->updateHeadless();

				const int progressFrameInterval = 10*60*LOGICFRAMES_PER_SECOND;
				if (TheGameLogic->getFrame() != 0 && TheGameLogic->getFrame() % progressFrameInterval == 0)
				{
					// Print progress report
					UnsignedInt gameTimeSec = TheGameLogic->getFrame() / LOGICFRAMES_PER_SECOND;
					UnsignedInt realTimeSec = (GetTickCount()-startTimeMillis) / 1000;
					printf("Elapsed Time: %02d:%02d Game Time: %02d:%02d/%02d:%02d\n",
							realTimeSec/60, realTimeSec%60, gameTimeSec/60, gameTimeSec%60, totalTimeSec/60, totalTimeSec%60);
					fflush(stdout);
				}
				TheGameLogic->UPDATE();
				// Check for desync BEFORE snapshotting: after a mismatch the game
				// state is diverged/garbage, so collecting a stats snapshot from
				// it is both useless and crash-prone.
				if (TheRecorder->sawCRCMismatch())
				{
					desynced = TRUE;
					numErrors++;
					break;
				}
				if (TheGlobalData->m_exportStats)
					StatsExporterCollectSnapshot();
			}
			UnsignedInt framesPlayed = TheGameLogic->getFrame();
			UnsignedInt gameTimeSec = framesPlayed / LOGICFRAMES_PER_SECOND;
			UnsignedInt realTimeSec = (GetTickCount()-startTimeMillis) / 1000;
			printf("Elapsed Time: %02d:%02d Game Time: %02d:%02d/%02d:%02d\n",
					realTimeSec/60, realTimeSec%60, gameTimeSec/60, gameTimeSec%60, totalTimeSec/60, totalTimeSec%60);
			fflush(stdout);

			// Classify the outcome. The playback loop only exits by finishing
			// (isPlaybackInProgress() == false) or by breaking on a CRC
			// mismatch, so a clean run that reached the recorded frame count is
			// OK; anything short without a mismatch is treated as INCOMPLETE
			// (allow a 1-second slack for benign end-of-replay off-by-a-few).
			//
			// Retail-epoch replays can never be checksum-verified: their
			// recorded Checksum values don't match a re-simulated CRC on any
			// engine (see RecorderClass::handleCRCMessage), so the recorder
			// skips that check for them and a completed run is reported as
			// OK_RETAIL_UNCHECKED. Consumers should weigh the staleRefs count:
			// zero means every recorded command resolved against our world,
			// the strongest fidelity evidence available without checksums.
			const Bool retailEpoch =
				(TheRecorder->getReplayEpoch() == RecorderClass::REPLAY_EPOCH_RETAIL);
			const Bool incomplete = !desynced && framesExpected != 0
				&& framesPlayed + LOGICFRAMES_PER_SECOND < framesExpected;
			const Bool isOk = !desynced && !incomplete;
			const char *verdict = desynced ? "DESYNC"
				: (incomplete ? "INCOMPLETE"
				: (retailEpoch ? "OK_RETAIL_UNCHECKED" : "OK"));

			// TheSuperHackers @feature Only export/upload stats for a clean,
			// complete simulation. A desynced or truncated playback produces a
			// divergent (garbage) game state, so its stats must never reach
			// cncstats.
			Bool statsExported = FALSE;
			if (TheGlobalData->m_exportStats && isOk)
			{
				ExportGameStatsJSON(TheRecorder->getReplayDir(), filename);
				statsExported = TRUE;
			}

			appendResultLogEntry(filename, verdict, framesPlayed, framesExpected, statsExported,
				(Int)TheRecorder->getReplayEpoch(), TheRecorder->getPlaybackStaleObjectRefs());
		}
		else
		{
			printf("Cannot open replay\n");
			numErrors++;
			appendResultLogEntry(filename, "CANT_OPEN", 0, 0, FALSE, -1, 0);
		}
	}
	if (filenames.size() > 1)
	{
		printf("Simulation of all replays completed. Errors occurred: %d\n", numErrors);

		UnsignedInt realTime = (GetTickCount()-totalStartTimeMillis) / 1000;
		printf("Total Time: %d:%02d:%02d\n", realTime/60/60, realTime/60%60, realTime%60);
		fflush(stdout);
	}

	return numErrors != 0 ? 1 : 0;
}

int ReplaySimulation::simulateReplaysInWorkerProcesses(const std::vector<AsciiString> &filenames, int maxProcesses)
{
	DWORD totalStartTimeMillis = GetTickCount();

	WideChar exePath[1024];
	GetModuleFileNameW(nullptr, exePath, ARRAY_SIZE(exePath));

	std::vector<WorkerProcess> processes;
	int filenamePositionStarted = 0;
	int filenamePositionDone = 0;
	int numErrors = 0;

	while (true)
	{
		int i;
		for (i = 0; i < processes.size(); i++)
			processes[i].update();

		// Get result of finished processes and print output in order
		while (!processes.empty())
		{
			if (!processes[0].isDone())
				break;
			AsciiString stdOutput = processes[0].getStdOutput();
			printf("%d/%d %s", filenamePositionDone+1, (int)filenames.size(), stdOutput.str());
			DWORD exitcode = processes[0].getExitCode();
			if (exitcode != 0)
				printf("Error!\n");
			fflush(stdout);
			numErrors += exitcode == 0 ? 0 : 1;
			processes.erase(processes.begin());
			filenamePositionDone++;
		}

		int numProcessesRunning = countProcessesRunning(processes);

		// Add new processes when we are below the limit and there are replays left
		while (numProcessesRunning < maxProcesses && filenamePositionStarted < filenames.size())
		{
			UnicodeString filenameWide;
			filenameWide.translate(filenames[filenamePositionStarted]);
			UnicodeString command;
			command.format(L"\"%s\"%s%s%s -replay \"%s\"",
				exePath,
				TheGlobalData->m_windowed ? L" -win" : L"",
				TheGlobalData->m_headless ? L" -headless" : L"",
				TheGlobalData->m_exportStats ? L" -exportStats" : L"",
				filenameWide.str());
			if (!TheGlobalData->m_statsUrl.isEmpty())
			{
				UnicodeString statsUrlWide;
				statsUrlWide.translate(TheGlobalData->m_statsUrl);
				command.concat(L" -statsUrl \"");
				command.concat(statsUrlWide);
				command.concat(L"\"");
			}
			// Forward the result-log path so each worker appends its own verdict line.
			if (!TheGlobalData->m_replayResultLog.isEmpty())
			{
				UnicodeString resultLogWide;
				resultLogWide.translate(TheGlobalData->m_replayResultLog);
				command.concat(L" -resultLog \"");
				command.concat(resultLogWide);
				command.concat(L"\"");
			}
			// Forward -mod so workers mount the same data set as this process.
			// Without this, workers run on retail-only data: replays referencing
			// mod-only content (e.g. Zulu color indices 8+) fail to open, and the
			// ones that do open simulate against the wrong INIs and desync.
			// parseMod stores the path fully resolved, so it is safe to pass on.
			const AsciiString &modPath = !TheGlobalData->m_modBIG.isEmpty()
				? TheGlobalData->m_modBIG
				: TheGlobalData->m_modDir;
			if (!modPath.isEmpty())
			{
				UnicodeString modPathWide;
				modPathWide.translate(modPath);
				command.concat(L" -mod \"");
				command.concat(modPathWide);
				command.concat(L"\"");
			}
			// Forward the -replayEpoch override, otherwise workers silently fall
			// back to auto-detection from each replay's header.
			if (RecorderClass::getReplayEpochOverride() >= 0)
			{
				UnicodeString epochWide;
				epochWide.format(L" -replayEpoch %d", RecorderClass::getReplayEpochOverride());
				command.concat(epochWide);
			}

			processes.push_back(WorkerProcess());
			processes.back().startProcess(command);

			filenamePositionStarted++;
			numProcessesRunning++;
		}

		if (processes.empty())
			break;

		// Don't waste CPU here, our workers need every bit of CPU time they can get
		Sleep(100);
	}

	DEBUG_ASSERTCRASH(filenamePositionStarted == filenames.size(), ("inconsistent file position 1"));
	DEBUG_ASSERTCRASH(filenamePositionDone == filenames.size(), ("inconsistent file position 2"));

	printf("Simulation of all replays completed. Errors occurred: %d\n", numErrors);

	UnsignedInt realTime = (GetTickCount()-totalStartTimeMillis) / 1000;
	printf("Total Wall Time: %d:%02d:%02d\n", realTime/60/60, realTime/60%60, realTime%60);
	fflush(stdout);

	return numErrors != 0 ? 1 : 0;
}

std::vector<AsciiString> ReplaySimulation::resolveFilenameWildcards(const std::vector<AsciiString> &filenames)
{
	// If some filename contains wildcards, search for actual filenames.
	// Note that we cannot do this in parseReplay because we require TheLocalFileSystem initialized.
	std::vector<AsciiString> filenamesResolved;
	for (std::vector<AsciiString>::const_iterator filename = filenames.begin(); filename != filenames.end(); ++filename)
	{
		if (filename->find('*') || filename->find('?'))
		{
			AsciiString dir1 = TheRecorder->getReplayDir();
			AsciiString dir2 = *filename;
			AsciiString wildcard = *filename;
			{
				int len = dir2.getLength();
				while (len)
				{
					char c = dir2.getCharAt(len-1);
					if (c == '/' || c == '\\')
					{
						wildcard.set(wildcard.str()+dir2.getLength());
						break;
					}
					dir2.removeLastChar();
					len--;
				}
			}

			FilenameList files;
			TheLocalFileSystem->getFileListInDirectory(dir2.str(), dir1.str(), wildcard, files, FALSE);
			for (FilenameList::iterator it = files.begin(); it != files.end(); ++it)
			{
				AsciiString file;
				file.set(it->str() + dir1.getLength());
				filenamesResolved.push_back(file);
			}
		}
		else
			filenamesResolved.push_back(*filename);
	}
	return filenamesResolved;
}

int ReplaySimulation::simulateReplays(const std::vector<AsciiString> &filenames, int maxProcesses)
{
	std::vector<AsciiString> filenamesResolved = resolveFilenameWildcards(filenames);
	if (maxProcesses == SIMULATE_REPLAYS_SEQUENTIAL)
		return simulateReplaysInThisProcess(filenamesResolved);
	else
		return simulateReplaysInWorkerProcesses(filenamesResolved, maxProcesses);
}
