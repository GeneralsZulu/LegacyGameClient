package coordinator

import "encoding/json"

const ProtocolVersion = 1

const (
	MsgHello      = "hello"
	MsgHelloOK    = "hello_ok"
	MsgDiscoverOK = "discover_ok"
	MsgHost       = "host"
	MsgHosted     = "hosted"
	MsgUnhost     = "unhost"
	MsgList       = "list"
	MsgGames      = "games"
	MsgJoin       = "join"
	MsgPeerInfo   = "peer_info"
	MsgHeartbeat  = "heartbeat"
	MsgError      = "error"
	MsgBye        = "bye"
)

type Envelope struct {
	Type string          `json:"type"`
	Data json.RawMessage `json:"data,omitempty"`
}

type Hello struct {
	Nick    string `json:"nick"`
	Version string `json:"version"`
}

type HelloOK struct {
	SessionToken string `json:"session_token"`
	STUNMagic    uint32 `json:"stun_magic"`
	UDPPort      int    `json:"udp_port"`
}

type DiscoverOK struct {
	PublicAddr string `json:"public_addr"`
}

type Host struct {
	Name       string `json:"name"`
	Map        string `json:"map"`
	MaxPlayers int    `json:"max_players"`
	LocalAddr  string `json:"local_addr"`
	PublicAddr string `json:"public_addr"`
}

type Hosted struct {
	GameID string `json:"game_id"`
}

type GameInfo struct {
	ID         string `json:"id"`
	Name       string `json:"name"`
	Map        string `json:"map"`
	HostNick   string `json:"host_nick"`
	Players    int    `json:"players"`
	MaxPlayers int    `json:"max_players"`
}

type Games struct {
	Games []GameInfo `json:"games"`
}

type Join struct {
	GameID     string `json:"game_id"`
	LocalAddr  string `json:"local_addr"`
	PublicAddr string `json:"public_addr"`
}

type PeerInfo struct {
	Nick       string `json:"nick"`
	PublicAddr string `json:"public_addr"`
	LocalAddr  string `json:"local_addr"`
	PunchInMS  int    `json:"punch_in_ms"`
	Role       string `json:"role"`
}

type Error struct {
	Message string `json:"message"`
}

const (
	STUNRequestSize   = 4 + 16
	STUNResponseSize  = 4 + 4 + 2
	SessionTokenBytes = 16
)
