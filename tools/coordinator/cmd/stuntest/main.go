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
	stunMagic    uint32
	publicAddr   string
}

func newClient(coordAddr, nick string) (*client, error) {
	c := &client{coordAddr: coordAddr, nick: nick}

	tcp, err := net.Dial("tcp", coordAddr)
	if err != nil {
		return nil, fmt.Errorf("tcp dial: %w", err)
	}
	c.tcp = tcp
	c.reader = bufio.NewReader(tcp)

	if err := c.send(coordinator.MsgHello, coordinator.Hello{
		Nick:    nick,
		Version: "stuntest/1",
	}); err != nil {
		return nil, err
	}

	var helloOK coordinator.HelloOK
	if err := c.recvExpect(coordinator.MsgHelloOK, &helloOK); err != nil {
		return nil, err
	}
	c.sessionToken = helloOK.SessionToken
	c.stunMagic = helloOK.STUNMagic

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

func (c *client) punch(pi *coordinator.PeerInfo) error {
	pubAddr, err := net.ResolveUDPAddr("udp", pi.PublicAddr)
	if err != nil {
		return fmt.Errorf("resolve public: %w", err)
	}
	var locAddr *net.UDPAddr
	if pi.LocalAddr != "" {
		locAddr, _ = net.ResolveUDPAddr("udp", pi.LocalAddr)
	}

	delay := time.Duration(pi.PunchInMS) * time.Millisecond
	log.Printf("punch scheduled in %v -> public=%v local=%v", delay, pubAddr, locAddr)
	time.Sleep(delay)

	msg := fmt.Appendf(nil, "PUNCH from %s", c.nick)
	rx := make(chan string, 4)
	stop := make(chan struct{})

	go func() {
		buf := make([]byte, 1500)
		for {
			select {
			case <-stop:
				return
			default:
			}
			c.udp.SetReadDeadline(time.Now().Add(250 * time.Millisecond))
			n, from, err := c.udp.ReadFromUDP(buf)
			if err != nil {
				continue
			}
			rx <- fmt.Sprintf("%d bytes from %s: %q", n, from, string(buf[:n]))
		}
	}()

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
	tick := time.NewTicker(200 * time.Millisecond)
	defer tick.Stop()

	for time.Now().Before(deadline) {
		if !ttlRestored && time.Since(start) >= time.Duration(lowTTLMs)*time.Millisecond {
			restoreTTL()
		}
		c.udp.WriteToUDP(msg, pubAddr)
		if locAddr != nil {
			c.udp.WriteToUDP(msg, locAddr)
		}
		select {
		case got := <-rx:
			close(stop)
			restoreTTL()
			log.Printf("PUNCH OK: %s", got)
			c.udp.WriteToUDP(fmt.Appendf(nil, "ACK from %s", c.nick), pubAddr)
			c.send(coordinator.MsgPunchOutcome, coordinator.PunchOutcome{
				OK: true, LobbyOK: true, GameOK: true,
				MS:   int(time.Since(start).Milliseconds()),
				Role: pi.Role,
			})
			return nil
		case <-tick.C:
		}
	}
	close(stop)
	c.send(coordinator.MsgPunchOutcome, coordinator.PunchOutcome{
		OK: false, MS: int(time.Since(start).Milliseconds()), Role: pi.Role,
	})
	return fmt.Errorf("no packet received within 8s")
}

func main() {
	addr := flag.String("coord", "cncstats.computersrfun.org:27500", "coordinator host:port (tcp)")
	nick := flag.String("nick", "stuntest", "nickname")
	doList := flag.Bool("list", false, "list games and exit")
	doHost := flag.Bool("host", false, "host a game and wait for joiner")
	doJoin := flag.String("join", "", "join game by ID")
	gameName := flag.String("game-name", "stuntest", "game name when hosting")
	flag.Parse()

	c, err := newClient(*addr, *nick)
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
		log.Printf("peer info: nick=%s public=%s local=%s role=%s punch_in_ms=%d",
			pi.Nick, pi.PublicAddr, pi.LocalAddr, pi.Role, pi.PunchInMS)
		if err := c.punch(pi); err != nil {
			log.Fatalf("punch: %v", err)
		}
		log.Printf("DONE")
	case *doJoin != "":
		if err := c.join(*doJoin); err != nil {
			log.Fatalf("join: %v", err)
		}
		pi, err := c.waitPeerInfo()
		if err != nil {
			log.Fatalf("wait peer: %v", err)
		}
		log.Printf("peer info: nick=%s public=%s local=%s role=%s punch_in_ms=%d",
			pi.Nick, pi.PublicAddr, pi.LocalAddr, pi.Role, pi.PunchInMS)
		if err := c.punch(pi); err != nil {
			log.Fatalf("punch: %v", err)
		}
		log.Printf("DONE")
	default:
		log.Printf("no -list, -host, or -join specified; exiting after discovery")
		os.Exit(0)
	}
}
