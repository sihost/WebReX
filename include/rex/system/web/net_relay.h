/**
 * @file        rex/system/web/net_relay.h
 * @brief       Web port (Workstream G): a virtual UDP stack tunnelled over the server's /net
 *              WebSocket relay (server/README.md "/net relay protocol"), for system link
 *              between browser tabs.
 *
 * Model: every tab is one host on a virtual LAN 10.80.0.0/24; the relay's peer id N is the
 * host 10.80.0.N. Datagrams to 255.255.255.255 or 10.80.0.255 fan out to every other peer
 * in the room; datagrams to the tab's own address or 127.x are delivered locally.
 *
 * Threads: the WebSocket lives on the browser main thread (JS, see net_relay.js). Inbound
 * datagrams are written by that thread into a SharedArrayBuffer ring in wasm memory
 * (Atomics only, it never blocks); callers on pthreads drain the ring under a mutex and
 * block on a futex for recvfrom/select. Outbound datagrams are copied and proxied
 * asynchronously to the main thread. Nothing here may be called *from* the main thread
 * except Start() (which proxies anyway).
 *
 * All addresses and ports in this API are host byte order. Errors are Winsock codes
 * (WSAE*), returned through `err`; the caller (guest Winsock hooks) stores them as the
 * thread's last error. The guest-ABI (big-endian) marshalling helpers are in
 * net_relay_guest.h.
 *
 * @license     BSD 3-Clause License
 */

#pragma once

#include <cstdint>

namespace rex::system::web::net_relay {

// Winsock error codes used by the shim.
enum WsaError : int {
  kWsaOk = 0,
  kWsaEInval = 10022,
  kWsaEMFile = 10024,
  kWsaEWouldBlock = 10035,
  kWsaENotSock = 10038,
  kWsaEMsgSize = 10040,
  kWsaEProtoNoSupport = 10043,
  kWsaEOpNotSupp = 10045,
  kWsaEAfNoSupport = 10047,
  kWsaEAddrInUse = 10048,
  kWsaEAddrNotAvail = 10049,
  kWsaENetDown = 10050,
  kWsaENotConn = 10057,
  kWsaEConnRefused = 10061,
};

constexpr uint32_t kSubnet = 0x0A500000u;     // 10.80.0.0
constexpr uint32_t kSubnetMask = 0xFFFFFF00u;  // /24
constexpr uint32_t kSubnetBroadcast = kSubnet | 0xFFu;
constexpr uint32_t kLimitedBroadcast = 0xFFFFFFFFu;
constexpr uint32_t kRelayBroadcastId = 0xFFFFFFFFu;

// Payload prefix the shim adds inside a relay datagram (the relay header has no source
// port). Big-endian: u16 src_port, u8 proto (17 = UDP, 254 = VDP), u8 version (1).
constexpr uint32_t kRelayHeaderSize = 10;
constexpr uint32_t kShimHeaderSize = 4;
constexpr uint8_t kShimVersion = 1;
constexpr uint32_t kMaxDatagram = 65536 - kShimHeaderSize;

enum class State : int32_t { kIdle = 0, kConnecting = 1, kUp = 2, kDown = 3 };

// Starts connecting (once; later calls are no-ops). url: full ws:// or wss:// URL, or
// empty for same-origin "/net?room=<room>"; room: empty = the page's ?room= parameter,
// else "default". Safe from any thread.
void Start(const char* url, const char* room);
// Blocks (not on the browser main thread) until the relay assigned an id, the connection
// failed, or timeout_ms elapsed. Returns true if up.
bool WaitUntilUp(uint32_t timeout_ms);
State GetState();
uint32_t PeerId();        // 0 until up
uint32_t LocalAddress();  // 10.80.0.<id>, 0 until up
uint32_t PeerCount();     // other peers currently in the room

// Sockets. type: 1 = SOCK_STREAM, 2 = SOCK_DGRAM. Stream sockets can be created but every
// connection-oriented call fails (system link is UDP/VDP only; see docs/port/STUBS.md).
int Socket(int af, int type, int protocol, int* err);  // handle >= 1, or -1
int Close(int s, int* err);
bool IsSocket(int s);
bool IsStream(int s);
int Bind(int s, uint32_t addr, uint16_t port, int* err);  // port 0 = ephemeral
int GetSockName(int s, uint32_t* addr, uint16_t* port, int* err);
int SetNonBlocking(int s, bool non_blocking, int* err);
int SetBroadcast(int s, bool on, int* err);
int SetReceiveTimeout(int s, uint32_t timeout_ms, int* err);  // 0 = infinite
// Returns len, or -1. Unbound sockets get an ephemeral port first (like Winsock).
int SendTo(int s, const uint8_t* data, uint32_t len, uint32_t addr, uint16_t port, int* err);
// Returns the datagram length (truncated copies fail with WSAEMSGSIZE like Winsock), or -1.
int RecvFrom(int s, uint8_t* buf, uint32_t len, uint32_t* from_addr, uint16_t* from_port,
             int* err);
int PendingBytes(int s, uint32_t* bytes, int* err);  // FIONREAD: size of the next datagram

// select(): read_ready[i] / write_ready[i] are set for each handle. timeout_us < 0 waits
// forever. Returns the number of ready handles (0 on timeout), or -1.
int Select(const int* read_socks, uint32_t nread, bool* read_ready, const int* write_socks,
           uint32_t nwrite, bool* write_ready, int64_t timeout_us, int* err);

struct Stats {
  uint64_t tx_datagrams;
  uint64_t tx_bytes;
  uint64_t tx_broadcast;
  uint64_t tx_dropped;  // relay not up / unroutable address
  uint64_t rx_datagrams;
  uint64_t rx_bytes;
  uint64_t rx_no_socket;    // no socket bound to the destination port
  uint64_t rx_queue_full;   // socket queue over its limit
  uint64_t rx_ring_dropped; // JS ring full (counted by the main thread)
  uint64_t rx_malformed;
  uint64_t loopback;
  uint32_t sockets_open;
  uint32_t peers;
  int32_t state;
  uint32_t id;
};
Stats GetStats();

}  // namespace rex::system::web::net_relay
