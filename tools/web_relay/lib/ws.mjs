// Minimal RFC 6455 WebSocket server endpoint (no extensions, no subprotocols): masked client
// frames, fragmentation, ping/pong, close, message size limit. No dependencies.
import crypto from 'node:crypto';
import { EventEmitter } from 'node:events';

const GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';

/** Answer an HTTP upgrade request with an error status and close the socket. */
export function rejectUpgrade(socket, status, reason) {
  if (socket.destroyed) return;
  const body = `${reason}\n`;
  socket.end(
    `HTTP/1.1 ${status} ${reason}\r\nConnection: close\r\nContent-Type: text/plain\r\n` +
      `Content-Length: ${Buffer.byteLength(body)}\r\n\r\n${body}`,
  );
}

/** Complete the opening handshake; returns a WsConnection or null (socket already rejected). */
export function acceptUpgrade(req, socket, head, { maxMessage = 1 << 20 } = {}) {
  const key = req.headers['sec-websocket-key'];
  if (
    req.method !== 'GET' ||
    (req.headers.upgrade || '').toLowerCase() !== 'websocket' ||
    !key ||
    req.headers['sec-websocket-version'] !== '13'
  ) {
    rejectUpgrade(socket, 400, 'Bad Request');
    return null;
  }
  const accept = crypto.createHash('sha1').update(key + GUID).digest('base64');
  socket.write(
    'HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n' +
      `Sec-WebSocket-Accept: ${accept}\r\n\r\n`,
  );
  return new WsConnection(socket, head, maxMessage);
}

/**
 * One accepted WebSocket. Events: 'message' (Buffer, isBinary), 'close' (code).
 * lastSeen: Date.now() of the last frame received (any opcode, pongs included).
 */
export class WsConnection extends EventEmitter {
  constructor(socket, head, maxMessage) {
    super();
    this.socket = socket;
    this.maxMessage = maxMessage;
    this.buf = head && head.length ? Buffer.from(head) : Buffer.alloc(0);
    this.frags = [];
    this.fragOp = 0;
    this.fragLen = 0;
    this.closed = false;
    this.finished = false;
    this.lastSeen = Date.now();
    socket.setNoDelay(true);
    socket.on('data', (d) => {
      this.buf = this.buf.length ? Buffer.concat([this.buf, d]) : d;
      this._parse();
    });
    socket.on('close', () => this._finish(1006));
    // The http server's sockets allow half-open connections, and after an upgrade nothing
    // handles 'end': a client whose TCP side closed without a close frame (killed process, a
    // proxy dropping the connection) would stay a peer until the idle timeout.
    socket.on('end', () => {
      this._finish(1006);
      socket.destroy();
    });
    socket.on('error', () => socket.destroy());
    if (this.buf.length) setImmediate(() => this._parse());
  }

  /** Bytes queued in the kernel/user-space socket buffers, not yet sent. */
  get bufferedAmount() {
    return this.socket.writableLength;
  }

  _parse() {
    while (!this.closed) {
      const b = this.buf;
      if (b.length < 2) return;
      const fin = (b[0] & 0x80) !== 0;
      const rsv = b[0] & 0x70;
      const op = b[0] & 0x0f;
      const masked = (b[1] & 0x80) !== 0;
      let len = b[1] & 0x7f;
      let off = 2;
      if (rsv || !masked) return this.close(1002);
      if (len === 126) {
        if (b.length < 4) return;
        len = b.readUInt16BE(2);
        off = 4;
      } else if (len === 127) {
        if (b.length < 10) return;
        const big = b.readBigUInt64BE(2);
        if (big > BigInt(this.maxMessage)) return this.close(1009);
        len = Number(big);
        off = 10;
      }
      if (len > this.maxMessage) return this.close(1009);
      if (b.length < off + 4 + len) return;
      const mask = b.subarray(off, off + 4);
      const payload = Buffer.allocUnsafe(len);
      const src = off + 4;
      for (let i = 0; i < len; i++) payload[i] = b[src + i] ^ mask[i & 3];
      this.buf = b.subarray(src + len);
      this.lastSeen = Date.now();

      if (op >= 0x8) {
        if (!fin || len > 125) return this.close(1002);
        if (op === 0x8) {
          const code = len >= 2 ? payload.readUInt16BE(0) : 1005;
          this.closedByPeer = true; // the client sent a close frame (vs. TCP loss or our close())
          this._frame(0x8, len >= 2 ? payload.subarray(0, 2) : Buffer.alloc(0));
          this.closed = true;
          this.socket.end();
          this._finish(code);
          return;
        }
        if (op === 0x9) this._frame(0xa, payload);
        else if (op !== 0xa) return this.close(1002);
        continue;
      }
      if (op === 0x0) {
        if (!this.fragOp) return this.close(1002);
      } else if (op === 0x1 || op === 0x2) {
        if (this.fragOp) return this.close(1002);
        this.fragOp = op;
      } else {
        return this.close(1002);
      }
      this.fragLen += len;
      if (this.fragLen > this.maxMessage) return this.close(1009);
      this.frags.push(payload);
      if (fin) {
        const msg = this.frags.length === 1 ? this.frags[0] : Buffer.concat(this.frags);
        const isBinary = this.fragOp === 0x2;
        this.frags = [];
        this.fragOp = 0;
        this.fragLen = 0;
        this.emit('message', msg, isBinary);
      }
    }
  }

  _frame(op, payload) {
    const s = this.socket;
    if (s.destroyed || !s.writable) return false;
    const len = payload.length;
    let hdr;
    if (len < 126) {
      hdr = Buffer.from([0x80 | op, len]);
    } else if (len < 65536) {
      hdr = Buffer.alloc(4);
      hdr[0] = 0x80 | op;
      hdr[1] = 126;
      hdr.writeUInt16BE(len, 2);
    } else {
      hdr = Buffer.alloc(10);
      hdr[0] = 0x80 | op;
      hdr[1] = 127;
      hdr.writeBigUInt64BE(BigInt(len), 2);
    }
    s.cork();
    s.write(hdr);
    s.write(payload);
    s.uncork();
    return true;
  }

  sendBinary(buf) {
    return !this.closed && this._frame(0x2, buf);
  }

  sendText(str) {
    return !this.closed && this._frame(0x1, Buffer.from(str, 'utf8'));
  }

  /** WebSocket ping frame; browsers answer with a pong on their own (also in background tabs). */
  ping() {
    return !this.closed && this._frame(0x9, Buffer.alloc(0));
  }

  close(code = 1000) {
    if (this.closed) return;
    this.closed = true;
    const p = Buffer.alloc(2);
    p.writeUInt16BE(code);
    this._frame(0x8, p);
    this.socket.end();
    setTimeout(() => this.socket.destroy(), 1000).unref();
    this._finish(code);
  }

  _finish(code) {
    if (this.finished) return;
    this.finished = true;
    this.closed = true;
    this.emit('close', code);
  }
}
