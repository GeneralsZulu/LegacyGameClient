package main

import (
	"bufio"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"flag"
	"fmt"
	"log"
	"net"
	"os"
	"strconv"
	"syscall"
	"time"

	"github.com/GeneralsZulu/LegacyGameClient/tools/coordinator"
)

// setTTL sets the IPv4 TTL on the punch socket. The first punch volley goes
// out with a low TTL: enough hops to cross our own NAT(s) and create the
// outbound mapping, but expiring in transit before it reaches the peer's
// NAT. An early full-TTL packet arriving before the peer's own first
// outbound would otherwise create an unsolicited conntrack entry on
// Linux-style NATs that occupies the peer's advertised mapping and diverts
// their SNAT to a different port (observed in the natlab: punch deadlocks
// with both sides refreshing each other's poison entries).
func setTTL(u *net.UDPConn, ttl int) {
	raw, err := u.SyscallConn()
	if err != nil {
		return
	}
	raw.Control(func(fd uintptr) {
		syscall.SetsockoptInt(int(fd), syscall.IPPROTO_IP, syscall.IP_TTL, ttl)
	})
}

// finish prints the machine-readable verdict and applies -expect.
// Exit codes: 0 = matched (or no expectation), 2 = fail verdict with no
// expectation, 3 = expectation mismatch.
func finish(verdict, expect string) {
	fmt.Printf("VERDICT %s\n", verdict)
	if expect != "" && verdict != expect {
		log.Printf("EXPECT MISMATCH: wanted %s, got %s", expect, verdict)
		os.Exit(3)
	}
	if expect == "" && verdict == "fail" {
		os.Exit(2)
	}
	os.Exit(0)
}

func envInt(name string, def int) int {
	if v := os.Getenv(name); v != "" {
		if n, err := strconv.Atoi(v); err == nil {
			return n
		}
	}
	return def
}

type client struct {
	coordAddr    string
	nick         string
	tcp          net.Conn
	reader       *bufio.Reader
	udp          *net.UDPConn
	udpLocal     *net.UDPAddr
	coordUDPAddr *net.UDPAddr
	sessionToken string
	tokenBytes   []byte
	stunMagic    uint32
	relayID      uint32
	publicAddr   string
}

func newClient(coordAddr, nick string, relay bool) (*client, error) {
	c := &client{coordAddr: coordAddr, nick: nick}

	tcp, err := net.Dial("tcp", coordAddr)
	if err != nil {
		return nil, fmt.Errorf("tcp dial: %w", err)
	}
	c.tcp = tcp
	c.reader = bufio.NewReader(tcp)

	relayFlag := 0
	if relay {
		relayFlag = 1
	}
	if err := c.send(coordinator.MsgHello, coordinator.Hello{
		Nick:    nick,
		Version: "stuntest/1",
		Relay:   relayFlag,
	}); err != nil {
		return nil, err
	}

	var helloOK coordinator.HelloOK
	if err := c.recvExpect(coordinator.MsgHelloOK, &helloOK); err != nil {
		return nil, err
	}
	c.sessionToken = helloOK.SessionToken
	c.tokenBytes, _ = hex.DecodeString(helloOK.SessionToken)
	c.stunMagic = helloOK.STUNMagic
	c.relayID = helloOK.RelayID

	host, _, err := net.SplitHostPort(coordAddr)
	if err != nil {
		return nil, fmt.Errorf("split coord addr: %w", err)
	}
	c.coordUDPAddr, err = net.ResolveUDPAddr("udp", net.JoinHostPort(host, fmt.Sprint(helloOK.UDPPort)))
	if err != nil {
		return nil, fmt.Errorf("resolve coord udp: %w", err)
	}

	udp, err := net.ListenUDP("udp", &net.UDPAddr{Port: 0})
	if err != nil {
		return nil, fmt.Errorf("listen udp: %w", err)
	}
	c.udp = udp
	c.udpLocal = udp.LocalAddr().(*net.UDPAddr)
	return c, nil
}

func (c *client) send(t string, payload any) error {
	raw, err := json.Marshal(payload)
	if err != nil {
		return err
	}
	env := coordinator.Envelope{Type: t, Data: raw}
	line, err := json.Marshal(env)
	if err != nil {
		return err
	}
	line = append(line, '\n')
	_, err = c.tcp.Write(line)
	return err
}

func (c *client) recv() (*coordinator.Envelope, error) {
	line, err := c.reader.ReadBytes('\n')
	if err != nil {
		return nil, err
	}
	var env coordinator.Envelope
	if err := json.Unmarshal(line, &env); err != nil {
		return nil, err
	}
	return &env, nil
}

func (c *client) recvExpect(t string, out any) error {
	env, err := c.recv()
	if err != nil {
		return err
	}
	if env.Type == coordinator.MsgError {
		var e coordinator.Error
		json.Unmarshal(env.Data, &e)
		return fmt.Errorf("server error: %s", e.Message)
	}
	if env.Type != t {
		return fmt.Errorf("expected %q, got %q", t, env.Type)
	}
	return json.Unmarshal(env.Data, out)
}

func (c *client) discover() (string, error) {
	return c.discoverPurpose(coordinator.STUNPurposeLobby)
}

func (c *client) discoverPurpose(purpose byte) (string, error) {
	tokBytes, err := hex.DecodeString(c.sessionToken)
	if err != nil {
		return "", err
	}
	req := make([]byte, coordinator.STUNRequestSize)
	binary.BigEndian.PutUint32(req[0:4], c.stunMagic)
	copy(req[4:4+coordinator.SessionTokenBytes], tokBytes)
	req[4+coordinator.SessionTokenBytes] = purpose

	buf := make([]byte, 64)
	for range 6 {
		if _, err := c.udp.WriteToUDP(req, c.coordUDPAddr); err != nil {
			return "", err
		}
		c.udp.SetReadDeadline(time.Now().Add(500 * time.Millisecond))
		n, _, err := c.udp.ReadFromUDP(buf)
		if err == nil && n >= coordinator.STUNResponseSize {
			magic := binary.BigEndian.Uint32(buf[0:4])
			if magic == c.stunMagic {
				ip := net.IP(buf[4:8])
				port := binary.BigEndian.Uint16(buf[8:10])
				addr := fmt.Sprintf("%s:%d", ip.String(), port)
				if purpose == coordinator.STUNPurposeLobby {
					c.publicAddr = addr
				}
				c.udp.SetReadDeadline(time.Time{})
				return addr, nil
			}
		}
	}
	c.udp.SetReadDeadline(time.Time{})
	return "", fmt.Errorf("STUN: no response after 6 attempts")
}

func (c *client) localAddrString() string {
	addrs, _ := net.InterfaceAddrs()
	for _, a := range addrs {
		if ipn, ok := a.(*net.IPNet); ok {
			ip4 := ipn.IP.To4()
			if ip4 != nil && !ip4.IsLoopback() {
				return fmt.Sprintf("%s:%d", ip4.String(), c.udpLocal.Port)
			}
		}
	}
	return fmt.Sprintf("127.0.0.1:%d", c.udpLocal.Port)
}

func (c *client) host(name string) (string, error) {
	if err := c.send(coordinator.MsgHost, coordinator.Host{
		Name:           name,
		Map:            "unknown",
		MaxPlayers:     2,
		LocalAddr:      c.localAddrString(),
		PublicAddr:     c.publicAddr,
		GamePublicAddr: c.publicAddr, // stuntest reuses one socket; real client uses a second
	}); err != nil {
		return "", err
	}
	var hosted coordinator.Hosted
	if err := c.recvExpect(coordinator.MsgHosted, &hosted); err != nil {
		return "", err
	}
	return hosted.GameID, nil
}

func (c *client) list() ([]coordinator.GameInfo, error) {
	if err := c.send(coordinator.MsgList, struct{}{}); err != nil {
		return nil, err
	}
	var games coordinator.Games
	if err := c.recvExpect(coordinator.MsgGames, &games); err != nil {
		return nil, err
	}
	return games.Games, nil
}

func (c *client) join(gameID string) error {
	return c.send(coordinator.MsgJoin, coordinator.Join{
		GameID:         gameID,
		LocalAddr:      c.localAddrString(),
		PublicAddr:     c.publicAddr,
		GamePublicAddr: c.publicAddr, // stuntest reuses one socket; real client uses a second
	})
}

func (c *client) waitPeerInfo() (*coordinator.PeerInfo, error) {
	for {
		env, err := c.recv()
		if err != nil {
			return nil, err
		}
		switch env.Type {
		case coordinator.MsgPeerInfo:
			var pi coordinator.PeerInfo
			if err := json.Unmarshal(env.Data, &pi); err != nil {
				return nil, err
			}
			return &pi, nil
		case coordinator.MsgError:
			var e coordinator.Error
			json.Unmarshal(env.Data, &e)
			return nil, fmt.Errorf("server error: %s", e.Message)
		default:
			log.Printf("waiting for peer_info, ignoring %s", env.Type)
		}
	}
}

// relayDataFrame builds a client->server RelayData frame (channel 0; the
// stuntest uses a single socket, so everything rides the lobby channel).
func (c *client) relayDataFrame(dest uint32, payload []byte) []byte {
	frame := make([]byte, coordinator.RelayDataHeaderSize+len(payload))
	binary.BigEndian.PutUint32(frame[0:4], c.stunMagic)
	copy(frame[4:20], c.tokenBytes)
	frame[20] = coordinator.RelayPurposeData
	frame[21] = coordinator.RelayChannelLobby
	binary.BigEndian.PutUint32(frame[22:26], dest)
	copy(frame[coordinator.RelayDataHeaderSize:], payload)
	return frame
}

// converge is a miniature of the real client's path rules, run after the
// punch phase: send PINGs to the peer every 200ms on whichever path we
// currently believe in (direct if the punch delivered, otherwise the relay),
// flip to the relay on receiving a relayed frame from the peer (the sticky
// rule), and report the path we ended on. Success is having heard the peer
// at all. One-way punch pairs converge to the relay on both sides through
// exactly the mechanism the game uses.
func (c *client) converge(pi *coordinator.PeerInfo, punched bool, punchedFrom *net.UDPAddr, window time.Duration) (string, error) {
	pubAddr, err := net.ResolveUDPAddr("udp", pi.PublicAddr)
	if err != nil {
		return "fail", fmt.Errorf("resolve public: %w", err)
	}
	directAddr := pubAddr
	if punchedFrom != nil {
		directAddr = punchedFrom
	}
	viaRelay := !punched
	if viaRelay && (c.relayID == 0 || pi.RelayID == 0) {
		return "fail", fmt.Errorf("punch failed and no relay available")
	}
	gotPeer := false
	deadline := time.Now().Add(window)
	tick := time.NewTicker(200 * time.Millisecond)
	defer tick.Stop()
	buf := make([]byte, 2048)
	ping := fmt.Appendf(nil, "PING from %s", c.nick)
	// Keep going a short while after first contact so the PEER also gets a
	// stream of our pings on the converged path.
	var settleUntil time.Time

	for time.Now().Before(deadline) {
		if gotPeer && !settleUntil.IsZero() && time.Now().After(settleUntil) {
			break
		}
		if viaRelay {
			c.udp.WriteToUDP(c.relayDataFrame(pi.RelayID, ping), c.coordUDPAddr)
		} else {
			c.udp.WriteToUDP(ping, directAddr)
		}
		c.udp.SetReadDeadline(time.Now().Add(200 * time.Millisecond))
		for {
			n, from, err := c.udp.ReadFromUDP(buf)
			if err != nil {
				break
			}
			if from.IP.Equal(c.coordUDPAddr.IP) && from.Port == c.coordUDPAddr.Port {
				// From the coordinator: STUN reply (drop) or RelayDeliver.
				if n > coordinator.RelayDeliverHeaderSize &&
					binary.BigEndian.Uint32(buf[0:4]) == c.stunMagic &&
					buf[4] == coordinator.RelayPurposeDeliver {
					src := binary.BigEndian.Uint32(buf[6:10])
					if src == pi.RelayID {
						if !viaRelay {
							log.Printf("sticky flip: peer reached us via relay")
							viaRelay = true
						}
						if !gotPeer {
							gotPeer = true
							settleUntil = time.Now().Add(2 * time.Second)
							log.Printf("heard peer via RELAY: %q", string(buf[coordinator.RelayDeliverHeaderSize:n]))
						}
					}
				}
				continue
			}
			// Direct packet from the peer.
			if !gotPeer {
				gotPeer = true
				settleUntil = time.Now().Add(2 * time.Second)
				log.Printf("heard peer DIRECT from %s: %q", from, string(buf[:n]))
			}
		}
	}
	if !gotPeer {
		return "fail", fmt.Errorf("no peer contact within %v", window)
	}
	if viaRelay {
		return "relay", nil
	}
	return "direct", nil
}

// punch blasts the synchronized hole punch and reports whether any peer
// packet arrived, plus the source address it arrived from (which on a
// port-drifting NAT differs from the advertised addr). It does NOT send
// punch_outcome; the caller does, because with the relay in play the
// outcome message depends on what happens next.
func (c *client) punch(pi *coordinator.PeerInfo) (bool, *net.UDPAddr) {
	pubAddr, err := net.ResolveUDPAddr("udp", pi.PublicAddr)
	if err != nil {
		log.Printf("resolve public: %v", err)
		return false, nil
	}
	var locAddr *net.UDPAddr
	if pi.LocalAddr != "" {
		locAddr, _ = net.ResolveUDPAddr("udp", pi.LocalAddr)
	}

	delay := time.Duration(pi.PunchInMS) * time.Millisecond
	log.Printf("punch scheduled in %v -> public=%v local=%v", delay, pubAddr, locAddr)
	time.Sleep(delay)

	msg := fmt.Appendf(nil, "PUNCH from %s", c.nick)

	// Low-TTL first phase; see setTTL. PUNCH_LOW_TTL=0 disables.
	lowTTL := envInt("PUNCH_LOW_TTL", 4)
	lowTTLMs := envInt("PUNCH_LOW_TTL_MS", 600)
	start := time.Now()
	if lowTTL > 0 {
		setTTL(c.udp, lowTTL)
		log.Printf("punching with TTL=%d for the first %dms", lowTTL, lowTTLMs)
	}
	ttlRestored := lowTTL <= 0
	restoreTTL := func() {
		if !ttlRestored {
			setTTL(c.udp, 64)
			ttlRestored = true
		}
	}
	defer restoreTTL()

	deadline := time.Now().Add(8 * time.Second)
	buf := make([]byte, 1500)
	for time.Now().Before(deadline) {
		if !ttlRestored && time.Since(start) >= time.Duration(lowTTLMs)*time.Millisecond {
			restoreTTL()
		}
		c.udp.WriteToUDP(msg, pubAddr)
		if locAddr != nil {
			c.udp.WriteToUDP(msg, locAddr)
		}
		c.udp.SetReadDeadline(time.Now().Add(200 * time.Millisecond))
		n, from, err := c.udp.ReadFromUDP(buf)
		if err != nil {
			continue
		}
		// Anything from the coordinator (relay deliver from an
		// already-flipped peer, stray STUN reply) is NOT punch evidence.
		if from.IP.Equal(c.coordUDPAddr.IP) && from.Port == c.coordUDPAddr.Port {
			continue
		}
		restoreTTL()
		log.Printf("PUNCH OK: %d bytes from %s: %q", n, from, string(buf[:n]))
		c.udp.WriteToUDP(fmt.Appendf(nil, "ACK from %s", c.nick), from)
		return true, from
	}
	return false, nil
}

// runPair drives the full post-peer_info flow: punch, punch_outcome (with
// relayed=true when the relay saves a failed punch, so the server issues
// grants exactly as it would for the real client), then converge. Returns
// the verdict: direct, relay, or fail.
func (c *client) runPair(pi *coordinator.PeerInfo) string {
	start := time.Now()
	punched, punchedFrom := c.punch(pi)
	relayAvailable := c.relayID != 0 && pi.RelayID != 0
	switch {
	case punched:
		c.send(coordinator.MsgPunchOutcome, coordinator.PunchOutcome{
			OK: true, LobbyOK: true, GameOK: true,
			MS: int(time.Since(start).Milliseconds()), Role: pi.Role,
		})
	case relayAvailable:
		log.Printf("punch failed; flipping to relay (peer id %d)", pi.RelayID)
		c.send(coordinator.MsgPunchOutcome, coordinator.PunchOutcome{
			OK: false, MS: int(time.Since(start).Milliseconds()), Role: pi.Role,
			Relayed: true, PeerRelayID: pi.RelayID,
		})
	default:
		c.send(coordinator.MsgPunchOutcome, coordinator.PunchOutcome{
			OK: false, MS: int(time.Since(start).Milliseconds()), Role: pi.Role,
		})
		return "fail"
	}
	verdict, err := c.converge(pi, punched, punchedFrom, 14*time.Second)
	if err != nil {
		log.Printf("converge: %v", err)
	}
	return verdict
}

func main() {
	addr := flag.String("coord", "cncstats.computersrfun.org:27500", "coordinator host:port (tcp)")
	nick := flag.String("nick", "stuntest", "nickname")
	doList := flag.Bool("list", false, "list games and exit")
	doHost := flag.Bool("host", false, "host a game and wait for joiner")
	doJoin := flag.String("join", "", "join game by ID")
	joinName := flag.String("join-name", "", "poll the game list and join the game with this name (lab automation)")
	gameName := flag.String("game-name", "stuntest", "game name when hosting")
	relay := flag.Bool("relay", true, "advertise relay support in hello")
	expect := flag.String("expect", "", "assert the final path: direct|relay|fail (exit 3 on mismatch)")
	flag.Parse()

	c, err := newClient(*addr, *nick, *relay)
	if err != nil {
		log.Fatalf("connect: %v", err)
	}
	log.Printf("connected to %s, session=%s", *addr, c.sessionToken[:8])
	log.Printf("local UDP bound to port %d", c.udpLocal.Port)

	public, err := c.discover()
	if err != nil {
		log.Fatalf("discover: %v", err)
	}
	log.Printf("discovered public addr: %s", public)
	log.Printf("our advertised local addr: %s", c.localAddrString())

	switch {
	case *doList:
		games, err := c.list()
		if err != nil {
			log.Fatalf("list: %v", err)
		}
		if len(games) == 0 {
			fmt.Println("(no open games)")
		}
		for _, g := range games {
			fmt.Printf("  %s  %q by %s  %d/%d  map=%s\n", g.ID, g.Name, g.HostNick, g.Players, g.MaxPlayers, g.Map)
		}
	case *doHost:
		gameID, err := c.host(*gameName)
		if err != nil {
			log.Fatalf("host: %v", err)
		}
		log.Printf("hosting game ID: %s", gameID)
		log.Printf("share this with the joiner: stuntest -coord %s -nick <yourname> -join %s", *addr, gameID)
		log.Printf("waiting for peer to join...")
		pi, err := c.waitPeerInfo()
		if err != nil {
			log.Fatalf("wait peer: %v", err)
		}
		log.Printf("peer info: nick=%s public=%s local=%s role=%s punch_in_ms=%d relay_id=%d",
			pi.Nick, pi.PublicAddr, pi.LocalAddr, pi.Role, pi.PunchInMS, pi.RelayID)
		finish(c.runPair(pi), *expect)
	case *doJoin != "" || *joinName != "":
		gameID := *doJoin
		if gameID == "" {
			deadline := time.Now().Add(15 * time.Second)
			for gameID == "" {
				if time.Now().After(deadline) {
					log.Fatalf("join-name: game %q never appeared in the list", *joinName)
				}
				games, err := c.list()
				if err != nil {
					log.Fatalf("list: %v", err)
				}
				for _, g := range games {
					if g.Name == *joinName {
						gameID = g.ID
						break
					}
				}
				if gameID == "" {
					time.Sleep(500 * time.Millisecond)
				}
			}
			log.Printf("join-name: found %q as %s", *joinName, gameID)
		}
		if err := c.join(gameID); err != nil {
			log.Fatalf("join: %v", err)
		}
		pi, err := c.waitPeerInfo()
		if err != nil {
			log.Fatalf("wait peer: %v", err)
		}
		log.Printf("peer info: nick=%s public=%s local=%s role=%s punch_in_ms=%d relay_id=%d",
			pi.Nick, pi.PublicAddr, pi.LocalAddr, pi.Role, pi.PunchInMS, pi.RelayID)
		finish(c.runPair(pi), *expect)
	default:
		log.Printf("no -list, -host, or -join specified; exiting after discovery")
		os.Exit(0)
	}
}
