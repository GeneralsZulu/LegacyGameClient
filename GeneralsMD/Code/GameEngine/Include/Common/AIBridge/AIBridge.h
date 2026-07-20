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
//   -aibridgeport  <port>     open localhost listener (channel 0); required to enable
//   -aibridgeslot  <0..7>     slot index whose playerIndex stamps channel-0 actions;
//                             omit for observation-only mode
//   -aibridgeport2 <port>     open a SECOND independent listener (channel 1)
//   -aibridgeslot2 <0..7>     slot for channel 1
// A second channel lets two external controllers each drive one player in the
// same headless game (bot-vs-bot self-play, with no scripted AI in the loop).
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
// Channel 1 (second bot), for bot-vs-bot in one game.
void  AIBridge_setListenPort2(Int port);
void  AIBridge_setBotSlot2(Int slot);

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

	// True when a bot slot is configured AND a client is currently connected
	// on ANY channel. Only when a channel is live should its action injection
	// be honored (checked per-channel inside tick()).
	Bool isCommandSourceLive() const;

	// Number of independent bot channels. 2 is enough for bot-vs-bot self-play;
	// bump if a future mode needs more externally-driven players in one game.
	enum { MAX_BRIDGE_CH = 2 };

private:
	Bool              m_started;
	// Per-channel state. Channel i owns its own listener/client socket
	// (m_server[i]), the lobby slot it stamps actions for (m_slot[i]), its
	// listen port (m_port[i]), and a sticky "hello sent this connection" bit.
	AIBridgeServer*   m_server[MAX_BRIDGE_CH];
	Int               m_slot[MAX_BRIDGE_CH];
	Int               m_port[MAX_BRIDGE_CH];
	Bool              m_helloSent[MAX_BRIDGE_CH];

	// Disallow copy.
	AIBridge(const AIBridge&);
	AIBridge& operator=(const AIBridge&);
};

extern AIBridge* TheAIBridge;
AIBridge* createAIBridge();
