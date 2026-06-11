# coordinator

Online matchmaking + NAT-traversal signaling service for the Generals client.

Two binaries:

- `cmd/coord` runs the service. TCP for signaling, UDP for STUN-style public-address discovery.
- `cmd/stuntest` is a CLI that exercises the protocol without needing the game client. Used to validate hole-punching feasibility from real client networks.

## Protocol

TCP (default `:27500`), newline-delimited JSON. UDP (default `:27501`), small binary STUN packets.

TCP messages: `hello`, `hello_ok`, `host`, `hosted`, `unhost`, `list`, `games`, `join`, `peer_info`, `heartbeat`, `bye`, `error`. Each message is `{"type": "...", "data": {...}}`.

UDP STUN:

```
request:  [magic uint32][session_token 16 bytes]                = 20 bytes
response: [magic uint32][ip 4 bytes][port uint16]               = 10 bytes
```

The client must send the STUN probe from the *exact same* UDP socket it will use for gameplay. NAT mappings are per-socket; a probe from a different socket gives an unusable address.

Match flow:

```
host                 coordinator                guest
  |--- hello --------->|<------ hello -----------|
  |<-- hello_ok -------|------- hello_ok ------->|
  |--- STUN udp ------>|<------ STUN udp --------|
  |<-- STUN udp -------|------- STUN udp ------->|
  |--- host ---------->|                         |
  |<-- hosted ---------|                         |
  |                    |<------ list ------------|
  |                    |------- games ---------->|
  |                    |<------ join ------------|
  |<-- peer_info ------|------- peer_info ------>|
  |                                              |
  |======== UDP punch (public + local) =========>|
  |<====== UDP punch (public + local) ===========|
```

After punch, peers communicate directly. The coordinator's role ends.

## Build

```
go build -o coord ./cmd/coord
go build -o stuntest ./cmd/stuntest
```

## Local test

In one shell:

```
./coord -tcp :27500 -udp :27501
```

In a second:

```
./stuntest -nick alice -host -game-name "test" -coord 127.0.0.1:27500
```

In a third:

```
./stuntest -nick bob -list -coord 127.0.0.1:27500
./stuntest -nick bob -join <id> -coord 127.0.0.1:27500
```

Both stuntest processes should report `PUNCH OK` after the join.

## Deploy to cncstats.computersrfun.org

1. Cross-compile on dev box (or build on server):

   ```
   GOOS=linux GOARCH=amd64 go build -o coord ./cmd/coord
   scp coord cncstats.computersrfun.org:/tmp/coord
   ```

2. On the server, as root:

   ```
   useradd -r -s /usr/sbin/nologin coord
   install -m 0755 /tmp/coord /usr/local/bin/coord
   install -m 0644 deploy/coordinator.service /etc/systemd/system/coordinator.service
   systemctl daemon-reload
   systemctl enable --now coordinator
   ```

3. Firewall:

   ```
   ufw allow 27500/tcp
   ufw allow 27501/udp
   ```

4. Verify from a remote machine:

   ```
   ./stuntest -nick remote-test
   ```

   It should print a discovered public address that matches your home router's WAN IP.

## Validating real NAT traversal

Run `stuntest -host` on machine A (home network 1) and `stuntest -join <id>` on machine B (home network 2). If both report `PUNCH OK`, hole-punching works for that combination of NAT types. If only one side gets a packet, the other side's NAT is mapping unpredictably (symmetric NAT) and that pair will need a relay fallback.

## Notes

- No TLS in v1. The signaling traffic is non-sensitive (nicks, game names, public IPs that are already observable). Add TLS via reverse proxy or stunnel if needed.
- No persistence. Restarting the coordinator drops all sessions and game listings. In-flight games are unaffected since gameplay is P2P.
- No authentication. v1 trusts everyone. Rate limiting + a small ban list is the v2 path.
