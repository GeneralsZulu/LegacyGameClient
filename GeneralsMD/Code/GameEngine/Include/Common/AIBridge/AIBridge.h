/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
*/

#pragma once

// AIBridge: TCP bridge that exposes per-frame game state to an external
// process and accepts player-style commands back. Bot-emitted commands are
// minted as GameMessage objects stamped with the bot's playerIndex and
// appended to TheCommandList between MessageStream::propagateMessages() and
// TheNetwork->update(), so they (a) flow over the wire to peers in MP, (b)
// land on TheCommandList of every peer, (c) are written to the replay file
// by the Recorder, and (d) execute in GameLogic identically to human input.
// Replays therefore reproduce a bridge-driven game with no bot present.
//
// Activation: command-line flags
//   -aibridgeport <port>      open localhost listener; required to enable
//   -aibridgeslot  <0..7>     slot index whose playerIndex stamps actions;
//                             omit for observation-only mode
//
// Wire protocol: see Common/AIBridge/AIBridge.cpp for the binary frame
// format. All frames are little-endian.

#include "Common/SubsystemInterface.h"
#include "Lib/BaseType.h"

class AIBridgeServer; // implementation detail; defined in AIBridge.cpp

// Free functions called by CommandLine.cpp to stash configuration before
// the AIBridge subsystem is constructed. The values are read by
// AIBridge::init().
void  AIBridge_setListenPort(Int port);
void  AIBridge_setBotSlot(Int slot);
Int   AIBridge_getListenPort();
Int   AIBridge_getBotSlot();

// TRUE when the AIBridge is driving the given engine player index. The scripted
// AISkirmishPlayer calls this to suppress its own army/combat orders for a
// bridge-controlled slot (economy/production still run). FALSE in
// observation-only mode, so ordinary AI slots are unaffected.
Bool  AIBridge_isControllingPlayer(Int playerIndex);

class AIBridge : public SubsystemInterface
{
public:
	AIBridge();
	virtual ~AIBridge() override;

	virtual void init() override;
	virtual void reset() override;
	virtual void update() override;

	// Called from GameEngine::update() AFTER TheMessageStream->propagateMessages()
	// and BEFORE TheNetwork->UPDATE(). Drains incoming actions from any
	// connected bot and appends synthesized GameMessages to TheCommandList,
	// then sends an observation snapshot for the just-completed frame.
	void tick();

	// True when a port has been configured and the listener is (or will be) up.
	Bool isEnabled() const;

	// True when a bot slot is configured AND a client is currently connected.
	// Only when this is true should action injection be honored.
	Bool isCommandSourceLive() const;

private:
	Bool              m_started;
	AIBridgeServer*   m_server;

	// Disallow copy.
	AIBridge(const AIBridge&);
	AIBridge& operator=(const AIBridge&);
};

extern AIBridge* TheAIBridge;
AIBridge* createAIBridge();
