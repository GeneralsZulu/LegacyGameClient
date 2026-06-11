package coordinator

import (
	"bufio"
	"crypto/rand"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"net"
	"strings"
	"sync"
	"time"
)

const (
	DefaultSTUNMagic = uint32(0x5A554C55) // "ZULU"
	PunchDelayMS     = 750
	SessionTTL       = 5 * time.Minute
	ReapInterval     = 30 * time.Second
	WriteTimeout     = 5 * time.Second
)

type Session struct {
	Token          string
	Conn           net.Conn
	Nick           string
	Version        string
	RemoteAddr     string
	PublicAddr     string // lobby (8086) public addr, discovered via STUN purpose=0
	GamePublicAddr string // in-game (8088) public addr, discovered via STUN purpose=1
	LocalAddr      string
	HostingGame    string
	LastSeen       time.Time
	writeMu        sync.Mutex
}

func (sess *Session) send(msgType string, payload any) error {
	raw, err := json.Marshal(payload)
	if err != nil {
		return err
	}
	env := Envelope{Type: msgType, Data: raw}
	line, err := json.Marshal(env)
	if err != nil {
		return err
	}
	line = append(line, '\n')
	sess.writeMu.Lock()
	defer sess.writeMu.Unlock()
	sess.Conn.SetWriteDeadline(time.Now().Add(WriteTimeout))
	_, err = sess.Conn.Write(line)
	sess.Conn.SetWriteDeadline(time.Time{})
	return err
}

type gameState struct {
	info     GameInfo
	hostSess *Session
	created  time.Time
}

type Server struct {
	Magic   uint32
	UDPPort int

	mu       sync.Mutex
	sessions map[string]*Session
	games    map[string]*gameState
}

func NewServer() *Server {
	return &Server{
		Magic:    DefaultSTUNMagic,
		sessions: make(map[string]*Session),
		games:    make(map[string]*gameState),
	}
}

func (s *Server) Run(tcpAddr, udpAddr string) error {
	tcpL, err := net.Listen("tcp", tcpAddr)
	if err != nil {
		return fmt.Errorf("listen tcp: %w", err)
	}
	defer tcpL.Close()
	log.Printf("TCP signaling listening on %s", tcpL.Addr())

	udpResAddr, err := net.ResolveUDPAddr("udp", udpAddr)
	if err != nil {
		return fmt.Errorf("resolve udp: %w", err)
	}
	udpL, err := net.ListenUDP("udp", udpResAddr)
	if err != nil {
		return fmt.Errorf("listen udp: %w", err)
	}
	defer udpL.Close()
	log.Printf("UDP STUN listening on %s", udpL.LocalAddr())
	s.UDPPort = udpL.LocalAddr().(*net.UDPAddr).Port

	go s.handleUDP(udpL)
	go s.reapLoop()
	return s.acceptTCP(tcpL)
}

func (s *Server) acceptTCP(l net.Listener) error {
	for {
		conn, err := l.Accept()
		if err != nil {
			if errors.Is(err, net.ErrClosed) {
				return nil
			}
			log.Printf("accept: %v", err)
			continue
		}
		go s.handleTCPConn(conn)
	}
}

func newToken() (string, error) {
	b := make([]byte, SessionTokenBytes)
	if _, err := rand.Read(b); err != nil {
		return "", err
	}
	return hex.EncodeToString(b), nil
}

func (s *Server) handleTCPConn(conn net.Conn) {
	defer conn.Close()
	r := bufio.NewReaderSize(conn, 8192)

	conn.SetReadDeadline(time.Now().Add(15 * time.Second))
	line, err := r.ReadBytes('\n')
	conn.SetReadDeadline(time.Time{})
	if err != nil {
		return
	}
	var env Envelope
	if err := json.Unmarshal(line, &env); err != nil || env.Type != MsgHello {
		return
	}
	var hello Hello
	if err := json.Unmarshal(env.Data, &hello); err != nil {
		return
	}

	token, err := newToken()
	if err != nil {
		return
	}
	sess := &Session{
		Token:      token,
		Conn:       conn,
		Nick:       sanitizeNick(hello.Nick),
		Version:    hello.Version,
		RemoteAddr: conn.RemoteAddr().String(),
		LastSeen:   time.Now(),
	}

	s.mu.Lock()
	s.sessions[token] = sess
	s.mu.Unlock()
	defer s.dropSession(sess)

	if err := sess.send(MsgHelloOK, HelloOK{
		SessionToken: token,
		STUNMagic:    s.Magic,
		UDPPort:      s.UDPPort,
	}); err != nil {
		return
	}
	log.Printf("session %s nick=%q version=%s from %s", token[:8], sess.Nick, sess.Version, sess.RemoteAddr)

	for {
		line, err := r.ReadBytes('\n')
		if err != nil {
			if err != io.EOF {
				log.Printf("session %s read: %v", token[:8], err)
			}
			return
		}
		sess.LastSeen = time.Now()
		var env Envelope
		if err := json.Unmarshal(line, &env); err != nil {
			sess.send(MsgError, Error{Message: "bad envelope"})
			continue
		}
		if err := s.handleMessage(sess, &env); err != nil {
			if err == io.EOF {
				return
			}
			sess.send(MsgError, Error{Message: err.Error()})
		}
	}
}

func (s *Server) handleMessage(sess *Session, env *Envelope) error {
	switch env.Type {
	case MsgHeartbeat:
		return nil
	case MsgHost:
		var m Host
		if err := json.Unmarshal(env.Data, &m); err != nil {
			return err
		}
		return s.handleHost(sess, &m)
	case MsgUnhost:
		return s.handleUnhost(sess)
	case MsgList:
		return s.handleList(sess)
	case MsgJoin:
		var m Join
		if err := json.Unmarshal(env.Data, &m); err != nil {
			return err
		}
		return s.handleJoin(sess, &m)
	case MsgBye:
		return io.EOF
	default:
		return fmt.Errorf("unknown message type %q", env.Type)
	}
}

func (s *Server) handleHost(sess *Session, m *Host) error {
	if m.PublicAddr == "" {
		return fmt.Errorf("public_addr required (do STUN discovery first)")
	}
	if m.GamePublicAddr == "" {
		return fmt.Errorf("game_public_addr required (do STUN discovery on game port first)")
	}
	s.mu.Lock()
	if sess.HostingGame != "" {
		gid := sess.HostingGame
		s.mu.Unlock()
		return fmt.Errorf("already hosting game %s", gid)
	}
	gameID, err := newToken()
	if err != nil {
		s.mu.Unlock()
		return err
	}
	gameID = gameID[:12]
	sess.LocalAddr = m.LocalAddr
	sess.PublicAddr = m.PublicAddr
	sess.GamePublicAddr = m.GamePublicAddr
	sess.HostingGame = gameID
	s.games[gameID] = &gameState{
		info: GameInfo{
			ID:         gameID,
			Name:       sanitizeName(m.Name),
			Map:        m.Map,
			HostNick:   sess.Nick,
			Players:    1,
			MaxPlayers: clampMaxPlayers(m.MaxPlayers),
		},
		hostSess: sess,
		created:  time.Now(),
	}
	s.mu.Unlock()
	log.Printf("session %s hosting game %s name=%q", sess.Token[:8], gameID, m.Name)
	return sess.send(MsgHosted, Hosted{GameID: gameID})
}

func (s *Server) handleUnhost(sess *Session) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if sess.HostingGame == "" {
		return nil
	}
	delete(s.games, sess.HostingGame)
	sess.HostingGame = ""
	return nil
}

func (s *Server) handleList(sess *Session) error {
	s.mu.Lock()
	out := Games{Games: make([]GameInfo, 0, len(s.games))}
	for _, g := range s.games {
		out.Games = append(out.Games, g.info)
	}
	s.mu.Unlock()
	return sess.send(MsgGames, out)
}

func (s *Server) handleJoin(sess *Session, m *Join) error {
	if m.PublicAddr == "" {
		return fmt.Errorf("public_addr required (do STUN discovery first)")
	}
	if m.GamePublicAddr == "" {
		return fmt.Errorf("game_public_addr required (do STUN discovery on game port first)")
	}
	s.mu.Lock()
	g, ok := s.games[m.GameID]
	if !ok {
		s.mu.Unlock()
		return fmt.Errorf("game %s not found", m.GameID)
	}
	host := g.hostSess
	sess.LocalAddr = m.LocalAddr
	sess.PublicAddr = m.PublicAddr
	sess.GamePublicAddr = m.GamePublicAddr
	s.mu.Unlock()

	if host == sess {
		return fmt.Errorf("cannot join your own game")
	}

	if err := host.send(MsgPeerInfo, PeerInfo{
		Nick:           sess.Nick,
		PublicAddr:     sess.PublicAddr,
		GamePublicAddr: sess.GamePublicAddr,
		LocalAddr:      sess.LocalAddr,
		PunchInMS:      PunchDelayMS,
		Role:           "host",
	}); err != nil {
		return fmt.Errorf("host send: %w", err)
	}
	if err := sess.send(MsgPeerInfo, PeerInfo{
		Nick:           host.Nick,
		PublicAddr:     host.PublicAddr,
		GamePublicAddr: host.GamePublicAddr,
		LocalAddr:      host.LocalAddr,
		PunchInMS:      PunchDelayMS,
		Role:           "guest",
	}); err != nil {
		return fmt.Errorf("guest send: %w", err)
	}
	log.Printf("matched host=%s(lobby=%s game=%s) <-> guest=%s(lobby=%s game=%s) game=%s",
		host.Nick, host.PublicAddr, host.GamePublicAddr,
		sess.Nick, sess.PublicAddr, sess.GamePublicAddr, m.GameID)
	return nil
}

func (s *Server) dropSession(sess *Session) {
	s.mu.Lock()
	delete(s.sessions, sess.Token)
	if sess.HostingGame != "" {
		delete(s.games, sess.HostingGame)
	}
	s.mu.Unlock()
}

func (s *Server) handleUDP(udpL *net.UDPConn) {
	buf := make([]byte, 2048)
	for {
		n, src, err := udpL.ReadFromUDP(buf)
		if err != nil {
			if errors.Is(err, net.ErrClosed) {
				return
			}
			log.Printf("udp read: %v", err)
			continue
		}
		// Accept the legacy 20-byte probe (assume lobby) and the current
		// 21-byte probe with a purpose byte.
		if n < 4+SessionTokenBytes {
			continue
		}
		magic := binary.BigEndian.Uint32(buf[0:4])
		if magic != s.Magic {
			continue
		}
		ip4 := src.IP.To4()
		if ip4 == nil {
			continue
		}
		token := hex.EncodeToString(buf[4 : 4+SessionTokenBytes])
		purpose := STUNPurposeLobby
		if n >= STUNRequestSize {
			purpose = buf[4+SessionTokenBytes]
		}
		s.mu.Lock()
		sess, ok := s.sessions[token]
		s.mu.Unlock()
		if !ok {
			log.Printf("STUN probe with unknown token from %s", src)
			continue
		}

		public := fmt.Sprintf("%s:%d", ip4.String(), src.Port)
		switch purpose {
		case STUNPurposeGame:
			sess.GamePublicAddr = public
		default:
			sess.PublicAddr = public
		}

		resp := make([]byte, STUNResponseSize)
		binary.BigEndian.PutUint32(resp[0:4], s.Magic)
		copy(resp[4:8], ip4)
		binary.BigEndian.PutUint16(resp[8:10], uint16(src.Port))
		udpL.WriteToUDP(resp, src)

		log.Printf("session %s STUN purpose=%d public=%s", token[:8], purpose, public)
	}
}

func (s *Server) reapLoop() {
	t := time.NewTicker(ReapInterval)
	defer t.Stop()
	for range t.C {
		cutoff := time.Now().Add(-SessionTTL)
		s.mu.Lock()
		for tok, sess := range s.sessions {
			if sess.LastSeen.Before(cutoff) {
				log.Printf("reaping idle session %s", tok[:8])
				sess.Conn.Close()
				delete(s.sessions, tok)
				if sess.HostingGame != "" {
					delete(s.games, sess.HostingGame)
				}
			}
		}
		s.mu.Unlock()
	}
}

func sanitizeNick(s string) string {
	s = strings.TrimSpace(s)
	if len(s) > 32 {
		s = s[:32]
	}
	if s == "" {
		s = "anonymous"
	}
	return s
}

func sanitizeName(s string) string {
	s = strings.TrimSpace(s)
	if len(s) > 64 {
		s = s[:64]
	}
	if s == "" {
		s = "unnamed"
	}
	return s
}

func clampMaxPlayers(n int) int {
	if n < 2 {
		return 2
	}
	if n > 8 {
		return 8
	}
	return n
}
