// Minimal STUN binding responder (RFC 5389 Binding request -> success with XOR-MAPPED-ADDRESS).
// The relay runs it next to the WebSocket listener (README "Peer-to-peer"): browsers then gather
// server-reflexive candidates that carry the real address instead of only mDNS `.local` host
// candidates, which have to be resolved over multicast first. No auth, no TURN, no state: the
// reply (32-44 bytes) goes back to the request's source only and is barely larger than the request.
import dgram from 'node:dgram';

const MAGIC = 0x2112a442;

/** Reply to one datagram, or null if it is not a well-formed STUN Binding request. */
export function stunReply(msg, address, port) {
  if (msg.length < 20 || (msg[0] & 0xc0) !== 0) return null;
  if (msg.readUInt16BE(0) !== 0x0001 || msg.readUInt32BE(4) !== MAGIC) return null;
  if (msg.readUInt16BE(2) + 20 !== msg.length) return null;
  const v6 = address.includes(':') && !address.startsWith('::ffff:');
  const ip = address.startsWith('::ffff:') ? address.slice(7) : address;
  const addrLen = v6 ? 16 : 4;
  const out = Buffer.alloc(20 + 4 + 4 + addrLen);
  out.writeUInt16BE(0x0101, 0); // Binding success
  out.writeUInt16BE(4 + 4 + addrLen, 2);
  msg.copy(out, 4, 4, 20); // magic cookie + transaction id
  out.writeUInt16BE(0x0020, 20); // XOR-MAPPED-ADDRESS
  out.writeUInt16BE(4 + addrLen, 22);
  out.writeUInt8(v6 ? 2 : 1, 25);
  out.writeUInt16BE(port ^ (MAGIC >>> 16), 26);
  if (!v6) {
    const p = ip.split('.').map(Number);
    if (p.length !== 4 || p.some((x) => !(x >= 0 && x <= 255))) return null;
    out.writeUInt32BE((((p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]) ^ MAGIC) >>> 0, 28);
  } else {
    const raw = ipv6Bytes(ip);
    if (!raw) return null;
    for (let i = 0; i < 16; i++) out[28 + i] = raw[i] ^ msg[4 + i]; // cookie + transaction id
  }
  return out;
}

function ipv6Bytes(s) {
  s = s.replace(/%.*$/, '');
  const [head, tail] = s.split('::');
  const part = (x) => (x ? x.split(':') : []);
  const h = part(head), t = s.includes('::') ? part(tail) : [];
  const groups = s.includes('::') ? [...h, ...Array(8 - h.length - t.length).fill('0'), ...t] : h;
  if (groups.length !== 8) return null;
  const b = Buffer.alloc(16);
  groups.forEach((g, i) => b.writeUInt16BE(parseInt(g, 16) & 0xffff, i * 2));
  return b;
}

/**
 * UDP STUN responder on `port` (0 = any) / `bind`. Resolves to {port, close(), stats}.
 * A bind failure (port taken) rejects; the relay then runs without it.
 */
export function createStunResponder({ port = 0, bind = '0.0.0.0', log = () => {} } = {}) {
  const sock = dgram.createSocket({ type: bind.includes(':') ? 'udp6' : 'udp4', reuseAddr: false });
  const stats = { requests: 0, ignored: 0 };
  sock.on('message', (msg, rinfo) => {
    const r = stunReply(msg, rinfo.address, rinfo.port);
    if (!r) {
      stats.ignored++;
      return;
    }
    stats.requests++;
    sock.send(r, rinfo.port, rinfo.address, () => {});
  });
  return new Promise((resolve, reject) => {
    sock.once('error', reject);
    sock.bind(port, bind, () => {
      sock.off('error', reject);
      sock.on('error', (e) => log(`stun: ${e.message}`));
      resolve({ port: sock.address().port, stats, close: () => new Promise((r) => sock.close(() => r())) });
    });
  });
}
