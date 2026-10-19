# web_relay: reference relay for the web net shim

WebReX builds have no sockets: on the web, the SDK's net shim
(`src/system/web/net_relay.{cpp,js}`, `include/rex/system/web/net_relay.h`) turns guest UDP/VDP
sockets into datagrams carried over **one WebSocket per browser tab** to a relay. Every tab in a
relay *room* is one console on a virtual LAN `10.80.0.0/24` (relay peer id N = `10.80.0.N`), so
system link works between tabs on different machines.

This directory is a small, dependency-free Node (22+) implementation of that relay. A game's own
web server can embed `lib/relay.mjs` (Halo 3's `server/lib/relay.mjs` speaks the same protocol), or
it can run on its own, e.g. next to a static host that cannot do WebSockets.

```bat
node tools/web_relay/server.mjs --port 8090
rem relay listening on ws://0.0.0.0:8090/net?room=<name> (origins: *)
```

| Option | Default | |
|---|---|---|
| `--port N` / `--bind ADDR` | `8090` / `0.0.0.0` | Listen address. |
| `--allow-origin O` (repeat) | `*` | Browser `Origin`s allowed to open `/net` (e.g. `https://game.example`). Clients that send no Origin (tools, tests) are always allowed. List your page's origin on a public relay so other sites cannot use it through visitors' browsers. |
| `--default-room NAME` | `default` | Room of a client that names none. |
| `--max-peers N` / `--max-rooms N` | `16` / `64` | Over the limit the handshake gets HTTP 503. |
| `--keepalive-ms N` | `20000` | WebSocket ping to every peer (0 = off). |
| `--idle-timeout-ms N` | `90000` | Close a peer that sent nothing, not even a pong, for this long (0 = never). |
| `--resume-grace-ms N` | `30000` | How long a lost peer keeps its id for a resume (0 = off; see Resume). |
| `--latency MS[:JITTER]` | `0` | Testing: deliver each datagram MS + [0, JITTER) ms late (order kept per receiver), like a tunnel. |
| `--tls-pfx F --tls-pass P` or `--tls-cert F --tls-key F` | off | Serve `wss://` (pages served over https can only open `wss://`). |
| `--quiet` | | No per-peer log lines. |
| `--no-p2p` / `--ice-server URL[\|user\|credential]` (repeat) | P2P on, no ICE servers | Peer-to-peer data path, see [Peer-to-peer](#peer-to-peer-webrtc-data-path). |

Public-hosting limits (see [Hardening](#hardening)):

| Option | Default | |
|---|---|---|
| `--access-key K` (repeat) | none | Every `/net`, `/net/status`, `/net/progress` request needs `?key=K` (401 otherwise). |
| `--room-pass ROOM=PW` (repeat) / `--require-room-pass` | none / off | Fixed password of a room / refuse rooms without a password (403). |
| `--max-datagram N` | `65536` | Largest datagram payload; larger ones are dropped (the socket stays open). Sent in the welcome as `maxPayload`. |
| `--rate-datagrams N` / `--rate-bytes N` | `2000` / `8388608` | Per-peer token buckets per second (0 = off); a broadcast costs one per receiver. The excess is dropped (UDP semantics). |
| `--rate-burst-ms N` | `1000` | Bucket depth in ms of the rate. |
| `--rate-text N` | `20` | Control (text) messages per second per peer; text over 4096 bytes is ignored. |
| `--rate-kick N` | `0` (off) | Close a peer (1008) after N rate-limited datagrams within 10 s. |
| `--max-peers-per-ip N` | `0` (off) | Live peers per client address (429 over it). |
| `--auth-fail-limit N` | `10` | After N wrong keys/room passwords from one address within 60 s that address gets 429 for the rest of the minute. |
| `--trust-proxy` | off | Take the client address from `CF-Connecting-IP` / `X-Forwarded-For` (behind a tunnel or reverse proxy; without it every peer behind the proxy shares one address). |
| `--no-room-list` | off | `/net/status` does not list the other rooms. |

Endpoints: `GET /net?room=<name>` (WebSocket upgrade; a plain GET gets 426), `GET /net/status?room=<name>`
(`{room, peers, maxPeers, players, defaultRoom, rooms}`; `rooms` = every room with peers, fullest
first), `GET /net/progress?room=&client=&name=&have=&total=&ready=` (optional download-progress side
channel, 204/400), `GET /healthz` (counters). HTTP responses carry `Cross-Origin-Resource-Policy:
cross-origin` and CORS for allowed origins, so a cross-origin-isolated page (COEP `require-corp`) on
another origin can poll the status. WebSockets are not subject to COEP.

## Pointing a game at it

The game starts the shim with `rex::system::web::net_relay::Start(url, room)`: an empty `url`
means same origin, `ws(s)://<page host>/net?room=<room>`; an empty `room` means the page's
`?room=`, else `default`. Expose both as cvars so players can pick a relay; Halo 3's project does
(`source/main.cpp`: `net_relay_url`, `net_relay_room`, passed on the web as `?arg=--name=value`):

```
https://game.example/?room=friday&arg=--net_relay_url=wss://relay.example/net?room%3Dfriday
```

Players who should see each other use the same relay and room.

## Protocol

Connect: `ws://<host>/net?room=<name>[&pass=<room password>][&key=<access key>][&resume=<token>][&id=<previous id>]`,
`name` = `[A-Za-z0-9_.-]{1,64}`. Max 16 peers per room (peers in their resume grace count),
64 rooms (HTTP 503 at the handshake over the limit; 400 for a bad name; 403 for a refused Origin
or a wrong room password; 401 for a missing access key; 429 for an address over its limits;
404 for another path).

**Text frames = JSON control.** Server → client:
- `{"type":"welcome","id":<u32>,"room":"<name>","peers":[<ids already present>],"maxPayload":65536,
  "resume":"<token>","resumeGraceMs":30000,"resumed":false,"p2p":true[,"iceServers":[...]]}`, always the
  first message. `p2p` false asks clients to keep every datagram on the relay; `iceServers` (only
  when configured) are `RTCIceServer` objects for the peer-to-peer path. Older relays send neither. `id` is the
  lowest free id ≥ 1 in the room (ids are reused). `peers` includes peers in their resume grace
  (nobody was told they left).
- `{"type":"join","id":n}` / `{"type":"leave","id":n}` when other peers come and go.
- `{"type":"pong","t":<echo>}` answering a client `{"type":"ping","t":any}` (application-level
  RTT probe; optional).
- `{"type":"progress","id":n,"client","name","have","total","ready"}`: another peer's
  `{"type":"progress",...}` report, forwarded (optional side channel; pages may ignore it).

Other text is ignored.

**Binary frames = datagrams**, a 10-byte big-endian header then 0..65536 payload bytes:

| offset | size | field |
|---|---|---|
| 0 | u32 | `src`: sender's peer id. **Overwritten by the relay** (no spoofing). |
| 4 | u32 | `dst`: peer id, or `0xFFFFFFFF` = every other peer in the room. |
| 8 | u16 | `port`: destination game port; opaque to the relay, delivered unchanged. |
| 10 | … | payload |

UDP-like semantics: unknown `dst` and frames shorter than 10 bytes are dropped silently; a datagram
to a receiver with more than 4 MiB queued is dropped; per sender→receiver pair the order is kept
(one TCP stream), nothing else is ordered. A message over 65546 bytes closes the socket (1009).

The SDK shim's payload starts with its own 4-byte header (`net_relay.h`: u16 source port, u8 proto
17 = UDP / 254 = VDP, u8 version 1), then the guest datagram, so guest datagrams are at most
65532 bytes. The relay does not look at it.

**Resume.** Every welcome carries a new one-time `resume` token (128-bit hex). When a socket ends
*without* a client close frame of code 1000 (or no code), e.g. TCP loss, a proxy timeout, the
relay's idle close, page unload's 1001, the peer's id stays reserved for `resumeGraceMs`: the
others get no `leave`, datagrams to it are dropped (counted in `/healthz` `droppedResuming`).
Reconnecting with `&resume=<latest token>` within the grace gives the same id back
(`"resumed":true`, current `peers`), and nobody gets a `leave`/`join`. This also works when the
relay has not noticed yet that the old socket is dead: that socket is closed with 4001 and the id
moves to the new one. After the grace the room gets the `leave` and the id is free again. An
unknown or expired token is a normal join (`"resumed":false`); then `&id=<previous id>` (1..254)
is honoured if that id is free, so a tab keeps its `10.80.0.<id>` across a relay restart. A client
that leaves on purpose closes with 1000 and the others see the `leave` at once.

**Keepalive.** A tab loading a level can send nothing for minutes, and proxies close idle
WebSockets (Cloudflare after 100 s). The relay sends a WebSocket ping frame to every peer every
`--keepalive-ms`; browsers answer pings with pongs themselves (also in background tabs), so pages
need no code for it. A peer from which nothing at all arrived for `--idle-timeout-ms` (half-open
TCP: network gone, machine asleep) is closed with 1001 and the others get its `leave`.

## Peer-to-peer (WebRTC data path)

The SDK shim (`net_relay.js`) also tries a direct WebRTC DataChannel (negotiated id 0, unordered,
`maxRetransmits: 0`) to every peer that runs the same code. The relay carries the signalling as
ordinary datagrams: `dst` = the peer, `port` 0, shim header src port 0 and proto `0x50`, JSON payload
(`hello`, `offer`/`answer` with the full SDP, late `ice` candidates, `bye`, RTT probes `pq`/`pr`). Relays
need no change (this one, Halo 3's and MW2's servers forward them as datagrams), and older clients drop
them (no socket on port 0). The channel carries the same frames as the WebSocket; the receiver sets
`src` to the channel's peer, so the guest's virtual LAN is unchanged. Per peer, datagrams use the
channel while it is open and answers probes, else the relay; a broadcast goes over each open channel
and as relay unicast to the peers without one. A failed attempt is retried with backoff (2 s doubling to
60 s); nothing waits for it. `Module.rexNet.stats().p2p` shows per peer the path, the candidate types
and probe RTT on both paths (p50/p95/p99); the page logs a summary every 30 s.

Options: page `?p2p=0` (off), `?p2p_ice=stun:host:port[,...]`, `?p2p_probe_ms=N`, `?p2p_debug=1`;
`Module.rexNetOptions = {p2p, iceServers, probeMs, statsLogMs}`; relay `--no-p2p` (welcome `p2p:false`)
and `--ice-server` (used when the page sets none). Without ICE servers only host candidates exist
(same LAN). Chrome hides LAN addresses behind mDNS names, and a host firewall without an inbound rule
for the browser blocks unsolicited UDP; then the channel may not open (seen between two tabs on one
Windows PC) and the peers stay on the relay.

Test without a game: `node tools/web_relay/test/p2p_browser.mjs [--latency MS[:J]] [--count N]`
(Playwright; two Chrome contexts on a fake heap plus an old-style client; per-datagram echo RTT).

## Hardening

For a relay on the public internet:

- **Who may connect.** `--allow-origin https://your.game` (pages of other sites cannot use the relay
  through visitors' browsers; tools without an Origin still can), and `--access-key` if the relay is
  not meant for everyone (pass it on the page as part of the relay URL, e.g.
  `--net_relay_url=wss://relay.example/net?key=K`).
- **Who may join a room.** A room password: the first peer that opens a room with `&pass=PW` sets it, and
  every later peer must send the same `pass` (403 otherwise, also for a password sent to a room that
  has none, so nobody can squat an open room under a protected room's name). The password ends with
  the room (all peers gone and their resume grace over). `--room-pass ROOM=PW` fixes one, and
  `--require-room-pass` refuses rooms without one. Wrong keys and passwords count against the
  address (`--auth-fail-limit`), which then gets 429 for the rest of the minute. Secrets are compared in
  constant time and are at most 256 characters.
- **How much a peer may send.** Per-peer token buckets for datagrams/s and payload bytes/s
  (`--rate-datagrams`, `--rate-bytes`, `--rate-burst-ms`), charged once per receiver, so a broadcast
  in a full room costs 15. The excess is dropped like lost UDP. `--rate-kick` closes peers that keep
  flooding. The defaults (2000 datagrams/s, 8 MiB/s) are far above what system link uses: MW2 and Halo 3
  send about 10-60 datagrams/s per peer of at most ~1.3 KB. Control messages: 20/s, at most 4 KiB.
  `--max-datagram` lowers the largest datagram (e.g. 4096: both games stay below 1500 bytes).
- **How many connections.** `--max-peers`, `--max-rooms`, `--max-peers-per-ip` (with `--trust-proxy`
  behind Cloudflare or a reverse proxy).
- **Privacy.** With the peer-to-peer path the peers learn each other's IP addresses (mDNS hides LAN
  addresses from other machines, but STUN shows public ones). `--no-p2p` keeps all traffic on
  the relay; players can also open the page with `?p2p=0`.

`/healthz` counts `authFailed`, `authThrottled`, `rateLimited`, `oversize`, `textLimited` and `kicked`.

## Tests

```bat
node --test tools/web_relay/test/*.test.mjs
```

Welcome/join/leave and id reuse, unicast with src rewrite, echo, broadcast, room isolation,
dropped unknown-dst and runt frames, a 64 KiB datagram, shim-format payloads passed through,
text ping/pong, 1009 on oversize messages, `/net/status`, progress over HTTP and over the relay,
426/404, room-full and room-count 503, Origin allowlist (403) and CORS headers, default room,
injected latency keeping order, keepalive (server pings arrive, a peer that never answers is
closed and seen leaving after its grace, browser-like and pong-answering clients stay connected),
and resume (lost connection keeps the id with no leave/join, datagrams to it dropped, same id
back with the token, spent token refused, grace expiry, takeover of a live socket with 4001,
`&id=` after a restart, clean close and grace 0 leave at once). `hardening.test.mjs`: token bucket,
room passwords (first peer, operator, required, open room refuses a password, forgotten when
empty), access key with 401/429 lockout, datagram/byte/text rate limits with broadcast fan-out
charged per receiver, rate kick 1008, max datagram (dropped, socket kept), peers per address,
hidden room list, welcome `p2p`/`iceServers`. 24/24.
