// Reference WebSocket relay for the ReXGlue web net shim (src/system/web/net_relay.{cpp,js}):
// rooms of browser tabs exchanging binary datagrams. Protocol: ../README.md. No dependencies.
import crypto from 'node:crypto';
import http from 'node:http';
import https from 'node:https';
import { acceptUpgrade, rejectUpgrade } from './ws.mjs';

export const HEADER_SIZE = 10; // u32 src, u32 dst, u16 port (big-endian)
export const BROADCAST = 0xffffffff;
export const MAX_PAYLOAD = 65536;
export const DEFAULTS = Object.freeze({
  maxPeersPerRoom: 16,
  maxRooms: 64,
  maxBuffered: 4 << 20, // drop datagrams to a peer this far behind (UDP semantics)
  keepaliveMs: 20000, // WebSocket ping to every peer (proxies close idle WebSockets: Cloudflare 100 s)
  idleTimeoutMs: 90000, // close a peer that sent nothing, not even a pong, for this long (0 = never)
  progressTtlMs: 30000,
  maxProgressPerRoom: 32,
  // A peer whose socket ended without a clean close keeps its id this long; a reconnect with
  // its resume token gets the same id back and the room never sees it leave (0 = off).
  resumeGraceMs: 30000,
});
const ROOM_RE = /^[A-Za-z0-9_.-]{1,64}$/;
const CLIENT_RE = /^[A-Za-z0-9_-]{1,40}$/;

/** Valid room name ([A-Za-z0-9_.-]{1,64}). */
export function isRoomName(name) {
  return typeof name === 'string' && ROOM_RE.test(name);
}

/**
 * Validate a download-progress report {client, name, have, total, ready} (an optional side
 * channel some pages use to show who is still loading). Returns the normalised record or null.
 */
export function parseProgress(p) {
  if (!p || typeof p !== 'object') return null;
  const client = String(p.client ?? '');
  if (!CLIENT_RE.test(client)) return null;
  const num = (v) => {
    const n = Number(v);
    return Number.isFinite(n) && n >= 0 ? Math.floor(n) : 0;
  };
  const name = String(p.name ?? '').replace(/[^\w .@'()-]/g, '').trim().slice(0, 32) || client.slice(0, 8);
  const ready = p.ready === true || p.ready === 1 || p.ready === '1' || p.ready === 'true';
  return { client, name, have: num(p.have), total: num(p.total), ready };
}

export class Relay {
  /**
   * Options (all optional): log(line), defaultRoom (room of a client that names none),
   * latencyMs / jitterMs (testing: every datagram reaches each receiver latencyMs + [0, jitterMs)
   * later, in order per receiver), and the limits of DEFAULTS.
   */
  constructor(options = {}) {
    const o = { ...DEFAULTS, ...options };
    this.log = o.log ?? (() => {});
    this.defaultRoom = isRoomName(o.defaultRoom) ? o.defaultRoom : 'default';
    this.latencyMs = Math.max(0, Number(o.latencyMs) || 0);
    this.jitterMs = Math.max(0, Number(o.jitterMs) || 0);
    this.maxPeersPerRoom = o.maxPeersPerRoom;
    this.maxRooms = o.maxRooms;
    this.maxBuffered = o.maxBuffered;
    this.keepaliveMs = o.keepaliveMs;
    this.idleTimeoutMs = o.idleTimeoutMs;
    this.progressTtlMs = o.progressTtlMs;
    this.maxProgressPerRoom = o.maxProgressPerRoom;
    this.resumeGraceMs = Math.max(0, Number(o.resumeGraceMs) || 0);
    // name -> {name, peers: Map(id -> WsConnection), ghosts: Map(id -> {timer, since}),
    //          tokens: Map(resume token -> id)}
    this.rooms = new Map();
    this.progress = new Map(); // name -> Map(client -> record)
    this.stats = {
      connections: 0, rejected: 0, datagrams: 0, bytes: 0, dropped: 0, idleClosed: 0,
      resumed: 0, takeovers: 0, graceExpired: 0, droppedResuming: 0,
    };
  }

  /** Store a progress report for room `name`. relayId: the reporting peer, or null (HTTP). */
  reportProgress(name, report, relayId = null) {
    if (!isRoomName(name)) return null;
    const rec = parseProgress(report);
    if (!rec) return null;
    let room = this.progress.get(name);
    if (!room) {
      if (this.progress.size >= this.maxRooms) this.pruneProgress();
      if (this.progress.size >= this.maxRooms) return null;
      room = new Map();
      this.progress.set(name, room);
    }
    const prev = room.get(rec.client);
    if (!prev && room.size >= this.maxProgressPerRoom) return null;
    const out = { ...rec, relayId: relayId ?? prev?.relayId ?? null, t: Date.now() };
    room.set(rec.client, out);
    return out;
  }

  pruneProgress(now = Date.now()) {
    for (const [name, room] of this.progress) {
      const live = new Set(this.rooms.get(name)?.peers.keys() ?? []);
      for (const [client, r] of room) {
        if (r.relayId !== null && !live.has(r.relayId)) room.delete(client);
        else if (now - r.t > this.progressTtlMs) room.delete(client);
      }
      if (room.size === 0) this.progress.delete(name);
    }
  }

  /** Fresh progress records of room `name`. */
  players(name) {
    if (!isRoomName(name)) return [];
    this.pruneProgress();
    const room = this.progress.get(name);
    if (!room) return [];
    const now = Date.now();
    return [...room.values()].map(({ client, name: n, have, total, ready, relayId, t }) => ({
      client, name: n, have, total, ready, relayId, age: now - t,
    }));
  }

  /** Every room with peers: [{room, peers}], fullest first. */
  roomList() {
    return [...this.rooms.values()].filter((r) => r.peers.size > 0)
      .map((r) => ({ room: r.name, peers: r.peers.size })).sort((a, b) => b.peers - a.peers);
  }

  /** Peers currently in room `name` (0 if it does not exist or the name is invalid). */
  peerCount(name) {
    if (!isRoomName(name)) return 0;
    return this.rooms.get(name)?.peers.size ?? 0;
  }

  /** JSON body of GET /net/status?room=<name>. */
  status(name) {
    const room = isRoomName(name) ? name : this.defaultRoom;
    return {
      room, peers: this.peerCount(room), maxPeers: this.maxPeersPerRoom, players: this.players(room),
      defaultRoom: this.defaultRoom, rooms: this.roomList(),
    };
  }

  // One datagram to peer p, after the injected latency if any (order kept per receiver).
  forward(p, msg) {
    if (p.bufferedAmount >= this.maxBuffered) {
      this.stats.dropped++;
      return;
    }
    this.stats.datagrams++;
    this.stats.bytes += msg.length;
    if (!this.latencyMs && !this.jitterMs) {
      p.sendBinary(msg);
      return;
    }
    const at = Math.max(p.relayNextAt ?? 0, Date.now() + this.latencyMs + Math.random() * this.jitterMs);
    p.relayNextAt = at;
    const copy = Buffer.from(msg);
    setTimeout(() => p.sendBinary(copy), at - Date.now()).unref?.();
  }

  // Peer `id` of `room` is gone for good: free its id and token, tell the others.
  leave(room, id, why) {
    for (const [t, tid] of room.tokens) if (tid === id) room.tokens.delete(t);
    const prog = this.progress.get(room.name);
    if (prog) for (const [client, r] of prog) if (r.relayId === id) prog.delete(client);
    for (const p of room.peers.values()) p.sendText(JSON.stringify({ type: 'leave', id }));
    if (room.peers.size === 0 && room.ghosts.size === 0 && this.rooms.get(room.name) === room) this.rooms.delete(room.name);
    this.log(`room ${room.name} -peer ${id} (${room.peers.size}) ${why}`);
  }

  /**
   * Handle an HTTP upgrade to /net?room=<name>[&resume=<token>] (the caller checked path and
   * Origin).
   */
  handleUpgrade(req, socket, head, searchParams) {
    const name = searchParams.get('room') || this.defaultRoom;
    if (!isRoomName(name)) {
      this.stats.rejected++;
      return rejectUpgrade(socket, 400, 'Bad Request');
    }
    let room = this.rooms.get(name);
    if (!room) {
      if (this.rooms.size >= this.maxRooms) {
        this.stats.rejected++;
        return rejectUpgrade(socket, 503, 'Service Unavailable');
      }
      room = { name, peers: new Map(), ghosts: new Map(), tokens: new Map() };
    }
    // Resume: the token of a peer in its grace period, or of a live peer whose socket the
    // client gave up on before this server noticed it was dead.
    const token = searchParams.get('resume');
    const resumeId = token ? room.tokens.get(token) : undefined;
    if (resumeId === undefined && room.peers.size + room.ghosts.size >= this.maxPeersPerRoom) {
      this.stats.rejected++;
      return rejectUpgrade(socket, 503, 'Service Unavailable');
    }
    const conn = acceptUpgrade(req, socket, head, { maxMessage: HEADER_SIZE + MAX_PAYLOAD });
    if (!conn) return;
    this.stats.connections++;
    this.rooms.set(name, room);
    let id;
    const resumed = resumeId !== undefined;
    if (resumed) {
      id = resumeId;
      room.tokens.delete(token);
      const ghost = room.ghosts.get(id);
      if (ghost) {
        clearTimeout(ghost.timer);
        room.ghosts.delete(id);
      }
      const old = room.peers.get(id);
      room.peers.set(id, conn);
      if (old) {
        // Its close handler sees it is no longer the peer and stays quiet.
        this.stats.takeovers++;
        old.close(4001);
      }
      this.stats.resumed++;
    } else {
      // &id=<previous id>: after a relay restart (tokens lost) a reconnecting client keeps its
      // address 10.80.0.<id> if nobody holds that id. 1..254 only (the /24 host part).
      const want = Number(searchParams.get('id'));
      const free = (n) => !room.peers.has(n) && !room.ghosts.has(n);
      if (Number.isInteger(want) && want >= 1 && want <= 254 && free(want)) {
        id = want;
      } else {
        id = 1;
        while (!free(id)) id++;
      }
      room.peers.set(id, conn);
    }
    // One-time token: every welcome carries a new one.
    conn.resumeToken = crypto.randomBytes(16).toString('hex');
    room.tokens.set(conn.resumeToken, id);
    const others = [...room.peers.keys(), ...room.ghosts.keys()].filter((p) => p !== id).sort((a, b) => a - b);
    conn.sendText(JSON.stringify({
      type: 'welcome', id, room: name, peers: others, maxPayload: MAX_PAYLOAD,
      resume: conn.resumeToken, resumeGraceMs: this.resumeGraceMs, resumed,
    }));
    if (!resumed) for (const p of room.peers.values()) if (p !== conn) p.sendText(JSON.stringify({ type: 'join', id }));
    this.log(`room ${name} ${resumed ? 'resumed' : '+'}peer ${id} (${room.peers.size})`);
    // A tab loading a level sends nothing for minutes; the ping keeps proxies from closing the
    // idle WebSocket, and the pong (sent by the browser itself) proves the peer is alive. A
    // peer silent for idleTimeoutMs (half-open TCP: laptop lid closed, network gone) is closed
    // so the others see it leave.
    const keepalive = this.keepaliveMs > 0 ? setInterval(() => {
      if (this.idleTimeoutMs > 0 && Date.now() - conn.lastSeen > this.idleTimeoutMs) {
        this.stats.idleClosed++;
        this.log(`room ${name} peer ${id}: nothing received for ${Date.now() - conn.lastSeen} ms, closing`);
        conn.close(1001);
        return;
      }
      conn.ping();
    }, this.keepaliveMs) : null;
    keepalive?.unref?.();

    conn.on('message', (msg, isBinary) => {
      if (!isBinary) {
        let j;
        try {
          j = JSON.parse(msg.toString('utf8'));
        } catch {
          return;
        }
        if (j && j.type === 'ping') {
          conn.sendText(JSON.stringify({ type: 'pong', t: j.t ?? null }));
        } else if (j && j.type === 'progress') {
          const rec = this.reportProgress(name, j, id);
          if (rec) {
            const fwd = JSON.stringify({ type: 'progress', id, client: rec.client, name: rec.name, have: rec.have, total: rec.total, ready: rec.ready });
            for (const p of room.peers.values()) if (p !== conn) p.sendText(fwd);
          }
        }
        return;
      }
      if (msg.length < HEADER_SIZE) return;
      msg.writeUInt32BE(id, 0); // src is always the sender's id (no spoofing)
      const dst = msg.readUInt32BE(4);
      if (dst === BROADCAST) {
        for (const [pid, p] of room.peers) if (pid !== id) this.forward(p, msg);
        this.stats.droppedResuming += room.ghosts.size;
      } else {
        const p = room.peers.get(dst);
        if (p) this.forward(p, msg);
        else if (room.ghosts.has(dst)) this.stats.droppedResuming++;
      }
    });
    conn.on('close', (code) => {
      if (keepalive) clearInterval(keepalive);
      if (room.peers.get(id) !== conn) return; // taken over by a resume, or shut down
      room.peers.delete(id);
      room.tokens.delete(conn.resumeToken);
      // A client that closes with 1000 (or no code) leaves now. Anything else (TCP loss,
      // proxy timeouts, our idle close, page unload's 1001) keeps the id for the grace period.
      const clean = conn.closedByPeer && (code === 1000 || code === 1005);
      if (clean || this.resumeGraceMs === 0) {
        this.leave(room, id, `code ${code}`);
        return;
      }
      room.tokens.set(conn.resumeToken, id);
      const timer = setTimeout(() => {
        if (room.ghosts.get(id)?.timer !== timer) return;
        room.ghosts.delete(id);
        this.stats.graceExpired++;
        this.leave(room, id, `code ${code}, not resumed within ${this.resumeGraceMs} ms`);
      }, this.resumeGraceMs);
      timer.unref?.();
      room.ghosts.set(id, { timer, since: Date.now() });
      this.log(`room ${name} peer ${id} lost (code ${code}), id kept ${this.resumeGraceMs} ms for a resume`);
    });
  }

  closeAll() {
    const rooms = [...this.rooms.values()];
    this.rooms.clear();
    for (const room of rooms) {
      for (const g of room.ghosts.values()) clearTimeout(g.timer);
      room.ghosts.clear();
      const peers = [...room.peers.values()];
      room.peers.clear();
      for (const p of peers) p.close(1001);
    }
    this.progress.clear();
  }
}

/**
 * Origin check for WebSocket upgrades. allowOrigins: ['*'] = any; otherwise exact origins
 * ("https://game.example"). Clients that send no Origin (not browsers) are always allowed.
 */
export function originAllowed(origin, allowOrigins) {
  if (!origin) return true;
  if (!allowOrigins || allowOrigins.includes('*')) return true;
  return allowOrigins.includes(origin);
}

/**
 * HTTP(S) server with the relay at /net and its status at /net/status; everything else 404.
 * Options: relay options (see Relay) plus allowOrigins (default ['*']) and tls ({pfx,
 * passphrase} or {cert, key}; omitted = plain HTTP). Returns {server, relay, shutdown}:
 * shutdown() closes every peer (1001) and the listener and resolves when the server closed
 * (server.close alone waits for upgraded sockets forever).
 */
export function createRelayServer(options = {}) {
  const relay = options.relay ?? new Relay(options);
  const allowOrigins = options.allowOrigins ?? ['*'];
  const cors = (req) => {
    const origin = req.headers.origin;
    return origin && originAllowed(origin, allowOrigins) ? { 'access-control-allow-origin': allowOrigins.includes('*') ? '*' : origin, vary: 'Origin' } : {};
  };
  const onRequest = (req, res) => {
    const url = new URL(req.url, 'http://relay');
    const send = (status, body, type = 'text/plain') => {
      // Cross-origin-isolated pages (COEP require-corp) may fetch the status from another origin.
      res.writeHead(status, { 'content-type': type, 'cache-control': 'no-store', 'cross-origin-resource-policy': 'cross-origin', ...cors(req) });
      res.end(body);
    };
    if (req.method !== 'GET' && req.method !== 'HEAD') return send(405, 'Method Not Allowed\n');
    if (url.pathname === '/net/status') return send(200, JSON.stringify(relay.status(url.searchParams.get('room') ?? '')), 'application/json');
    if (url.pathname === '/net/progress') {
      const room = url.searchParams.get('room') || relay.defaultRoom;
      const rec = relay.reportProgress(room, Object.fromEntries(url.searchParams));
      return rec ? send(204, '') : send(400, 'Bad Request\n');
    }
    if (url.pathname === '/net') return send(426, 'Upgrade Required: connect with a WebSocket\n');
    if (url.pathname === '/healthz') return send(200, JSON.stringify({ ok: true, rooms: relay.rooms.size, ...relay.stats }), 'application/json');
    return send(404, 'Not Found\n');
  };
  const server = options.tls ? https.createServer(options.tls, onRequest) : http.createServer(onRequest);
  const upgraded = new Set();
  server.on('upgrade', (req, socket, head) => {
    upgraded.add(socket);
    socket.on('close', () => upgraded.delete(socket));
    socket.on('error', () => socket.destroy());
    const url = new URL(req.url, 'http://relay');
    if (url.pathname !== '/net') return rejectUpgrade(socket, 404, 'Not Found');
    if (!originAllowed(req.headers.origin, allowOrigins)) {
      relay.stats.rejected++;
      return rejectUpgrade(socket, 403, 'Forbidden');
    }
    relay.handleUpgrade(req, socket, head, url.searchParams);
  });
  const shutdown = () => new Promise((resolve) => {
    relay.closeAll();
    server.close(() => resolve());
    server.closeAllConnections?.();
    // Peers got a close frame; give them a moment to answer, then drop the sockets.
    setTimeout(() => { for (const s of upgraded) s.destroy(); }, 100).unref();
  });
  return { server, relay, shutdown };
}
