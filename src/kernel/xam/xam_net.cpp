/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

// Disable warnings about unused parameters for kernel functions
#pragma GCC diagnostic ignored "-Wunused-parameter"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/kernel/xam/xnet.h>
#include <rex/kernel/xam/module.h>
#include <rex/kernel/xam/private.h>
#include <rex/kernel/xboxkrnl/error.h>
#include <rex/kernel/xboxkrnl/threading.h>
#include <rex/logging.h>
#include <rex/ppc/function.h>
#include <rex/ppc/types.h>
#include <rex/string.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xevent.h>
#include <rex/system/xsocket.h>
#include <rex/system/xthread.h>
#include <rex/system/xtypes.h>

#if REX_PLATFORM_WIN32
// NOTE: must be included last as it expects windows.h to already be included.
#define _WINSOCK_DEPRECATED_NO_WARNINGS  // inet_addr
#include <winsock2.h>                    // NOLINT(build/include_order)
#elif REX_PLATFORM_LINUX || REX_PLATFORM_WEB
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <sys/socket.h>
#include <unistd.h>
#if REX_PLATFORM_WEB  // musl does not pull these in via sys/socket.h
#include <sys/select.h>
#include <sys/time.h>
#endif
#endif

#if REX_PLATFORM_WEB
#include <rex/system/web/net_relay.h>
#include <rex/system/web/net_relay_guest.h>
#endif

// XNet for System Link (rex/kernel/xam/xnet.h). Names start with xnet_ so they never clash
// with a game's own net_relay* cvars (Halo 3, MW2).
REXCVAR_DEFINE_BOOL(xnet_link_up, true, "Net",
                    "XNetGetEthernetLinkStatus reports an active 100 Mb/s full-duplex link (web: "
                    "only while the net relay is up). Titles such as MW2 refuse every match "
                    "without a link");
REXCVAR_DEFINE_STRING(xnet_address, "", "Net",
                      "Native: IPv4 address in the title XNADDR (empty = this host's primary "
                      "LAN address, else 127.0.0.1). Web: ignored (the relay assigns "
                      "10.80.0.<id>)");
REXCVAR_DEFINE_BOOL(xnet_deterministic_random, false, "Net",
                    "XNetRandom, XNetCreateKey and session ids/nonces return constant bytes "
                    "(0xBB) for reproducible runs; consoles on one LAN then share ids");
REXCVAR_DEFINE_BOOL(xnet_trace, false, "Net", "Log every XNet and socket export call");
REXCVAR_DEFINE_BOOL(xnet_relay, true, "Net/Web",
                    "Web: the NetDll socket and XNet exports run over the server's /net "
                    "WebSocket relay (rex/system/web/net_relay.h); false = no network");
REXCVAR_DEFINE_STRING(xnet_relay_url, "", "Net/Web",
                      "Web: relay URL (empty = same origin ws(s)://<host>/net?room=<room>). If "
                      "the game started the relay first, its URL wins");
REXCVAR_DEFINE_STRING(xnet_relay_room, "", "Net/Web",
                      "Web: relay room (empty = the page's ?room= parameter, else 'default')");
REXCVAR_DEFINE_INT32(xnet_relay_stats_interval_ms, 10000, "Net/Web",
                     "Web: log the relay's traffic counters this often while the title uses "
                     "sockets (0 = never)");
REXCVAR_DEFINE_BOOL(xnet_unique_xuid, false, "Net/Web",
                    "Web: XamUserGetXUID / XamUserGetSigninInfo give relay console N >= 2 the "
                    "profile XUID with its low 16 bits XORed with N, so tabs in one System "
                    "Link session do not share a player XUID (starts the relay on first use)");

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;
using namespace rex::system::xam;

// https://github.com/G91/TitanOffLine/blob/1e692d9bb9dfac386d08045ccdadf4ae3227bb5e/xkelib/xam/xamNet.h
enum {
  XNCALLER_INVALID = 0x0,
  XNCALLER_TITLE = 0x1,
  XNCALLER_SYSAPP = 0x2,
  XNCALLER_XBDM = 0x3,
  XNCALLER_TEST = 0x4,
  NUM_XNCALLER_TYPES = 0x4,
};

// https://github.com/pmrowla/hl2sdk-csgo/blob/master/common/xbox/xboxstubs.h
typedef struct {
  // FYI: IN_ADDR should be in network-byte order.
  in_addr ina;                    // IP address (zero if not static/DHCP)
  in_addr inaOnline;              // Online IP address (zero if not online)
  rex::be<uint16_t> wPortOnline;  // Online port
  uint8_t abEnet[6];              // Ethernet MAC address
  uint8_t abOnline[20];           // Online identification
} XNADDR;

typedef struct {
  rex::be<int32_t> status;
  rex::be<uint32_t> cina;
  in_addr aina[8];
} XNDNS;

typedef struct {
  uint8_t flags;
  uint8_t reserved;
  rex::be<uint16_t> probes_xmit;
  rex::be<uint16_t> probes_recv;
  rex::be<uint16_t> data_len;
  rex::be<uint32_t> data_ptr;
  rex::be<uint16_t> rtt_min_in_msecs;
  rex::be<uint16_t> rtt_med_in_msecs;
  rex::be<uint32_t> up_bits_per_sec;
  rex::be<uint32_t> down_bits_per_sec;
} XNQOSINFO;

typedef struct {
  rex::be<uint32_t> count;
  rex::be<uint32_t> count_pending;
  XNQOSINFO info[1];
} XNQOS;

struct Xsockaddr_t {
  rex::be<uint16_t> sa_family;
  char sa_data[14];
};

struct X_WSADATA {
  rex::be<uint16_t> version;
  rex::be<uint16_t> version_high;
  char description[256 + 1];
  char system_status[128 + 1];
  rex::be<uint16_t> max_sockets;
  rex::be<uint16_t> max_udpdg;
  rex::be<uint32_t> vendor_info_ptr;
};

struct XWSABUF {
  rex::be<uint32_t> len;
  rex::be<uint32_t> buf_ptr;
};

struct XWSAOVERLAPPED {
  rex::be<uint32_t> internal;
  rex::be<uint32_t> internal_high;
  union {
    struct {
      rex::be<uint32_t> low;
      rex::be<uint32_t> high;
    } offset;  // must be named to avoid GCC error
    rex::be<uint32_t> pointer;
  };
  rex::be<uint32_t> event_handle;
};

void LoadSockaddr(const uint8_t* ptr, sockaddr* out_addr) {
  out_addr->sa_family = memory::load_and_swap<uint16_t>(ptr + 0);
  switch (out_addr->sa_family) {
    case AF_INET: {
      auto in_addr = reinterpret_cast<sockaddr_in*>(out_addr);
      in_addr->sin_port = memory::load_and_swap<uint16_t>(ptr + 2);
      // Maybe? Depends on type.
      in_addr->sin_addr.s_addr = *(uint32_t*)(ptr + 4);
      break;
    }
    default:
      assert_unhandled_case(out_addr->sa_family);
      break;
  }
}

void StoreSockaddr(const sockaddr& addr, uint8_t* ptr) {
  switch (addr.sa_family) {
    case AF_UNSPEC:
      std::memset(ptr, 0, sizeof(addr));
      break;
    case AF_INET: {
      auto& in_addr = reinterpret_cast<const sockaddr_in&>(addr);
      memory::store_and_swap<uint16_t>(ptr + 0, in_addr.sin_family);
      memory::store_and_swap<uint16_t>(ptr + 2, in_addr.sin_port);
      // Maybe? Depends on type.
      memory::store_and_swap<uint32_t>(ptr + 4, in_addr.sin_addr.s_addr);
      break;
    }
    default:
      assert_unhandled_case(addr.sa_family);
      break;
  }
}

// --- XNet state ----------------------------------------------------------------------------

// Logs the first call of an export, and every call with --xnet_trace.
#define XNET_TRACE(fmt_str, ...)                                       \
  do {                                                                 \
    static std::atomic<bool> xnet_seen_{false};                        \
    if (!xnet_seen_.exchange(true) || REXCVAR_GET(xnet_trace)) {       \
      REXKRNL_INFO("xnet: " fmt_str, ##__VA_ARGS__);                   \
    }                                                                  \
  } while (0)
// For exports titles call every frame: logs the first call and every change of `key`.
#define XNET_TRACE_CHANGES(key, fmt_str, ...)                          \
  do {                                                                 \
    static std::atomic<uint64_t> xnet_last_{~uint64_t(0)};             \
    if (xnet_last_.exchange(uint64_t(key)) != uint64_t(key)) {         \
      REXKRNL_INFO("xnet: " fmt_str, ##__VA_ARGS__);                   \
    }                                                                  \
  } while (0)

namespace {

xnet::AddressBook g_book;
xnet::KeyRegistry g_keys;
xnet::Random g_random;
std::atomic<uint32_t> g_system_link_port{0};

std::string Ip(uint32_t a) {
  return std::to_string(a >> 24) + "." + std::to_string((a >> 16) & 255) + "." +
         std::to_string((a >> 8) & 255) + "." + std::to_string(a & 255);
}

// "a.b.c.d" -> host-order address; false if malformed.
bool ParseDottedQuad(const std::string& s, uint32_t* out) {
  uint32_t parts[4] = {};
  int n = 0;
  uint32_t cur = 0;
  bool digit = false;
  for (size_t i = 0; i <= s.size(); ++i) {
    char c = i < s.size() ? s[i] : '\0';
    if (c >= '0' && c <= '9') {
      cur = cur * 10 + uint32_t(c - '0');
      digit = true;
      if (cur > 255) return false;
    } else if (c == '.' || c == '\0') {
      if (!digit || n == 4) return false;
      parts[n++] = cur;
      cur = 0;
      digit = false;
    } else {
      return false;
    }
  }
  if (n != 4) return false;
  *out = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
  return true;
}

constexpr uint32_t kLoopback = 0x7F000001u;

#if REX_PLATFORM_WEB
namespace nr = rex::system::web::net_relay;
namespace ng = rex::system::web::net_relay::guest;

// Relay connected? The first call starts the connection (unless the game already did) and
// waits up to 3 s for the relay's welcome. Always on a guest thread, never the browser main
// thread (WaitUntilUp returns at once there).
bool RelayUp() {
  if (!REXCVAR_GET(xnet_relay)) {
    return nr::GetState() == nr::State::kUp;  // the game may run the relay itself
  }
  static std::once_flag once;
  std::call_once(once, [] {
    std::string url = REXCVAR_GET(xnet_relay_url);
    std::string room = REXCVAR_GET(xnet_relay_room);
    nr::Start(url.c_str(), room.c_str());
    if (nr::WaitUntilUp(3000)) {
      REXKRNL_INFO("xnet: relay up; this console is {} (peer id {}), {} other peer(s) in the room",
                   Ip(nr::LocalAddress()), nr::PeerId(), nr::PeerCount());
    } else {
      REXKRNL_WARN("xnet: relay not reachable (state {}): no network", int(nr::GetState()));
    }
  });
  return nr::GetState() == nr::State::kUp;
}
#else
// This host's primary LAN address: the source address the OS picks for a UDP "connect" to a
// TEST-NET-1 address (connecting a datagram socket sends nothing). 0 if there is no route.
uint32_t DetectHostAddress() {
#if REX_PLATFORM_WIN32
  WSADATA wsa;
  if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    return 0;
  }
  SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == INVALID_SOCKET) {
    return 0;
  }
  int len = sizeof(sockaddr_in);
#else
  int s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s < 0) {
    return 0;
  }
  socklen_t len = sizeof(sockaddr_in);
#endif
  sockaddr_in dst{};
  dst.sin_family = AF_INET;
  dst.sin_port = htons(9);
  dst.sin_addr.s_addr = htonl(0xC0000201u);  // 192.0.2.1
  uint32_t a = 0;
  if (::connect(s, reinterpret_cast<sockaddr*>(&dst), sizeof(dst)) == 0) {
    sockaddr_in me{};
    if (::getsockname(s, reinterpret_cast<sockaddr*>(&me), &len) == 0) {
      a = ntohl(me.sin_addr.s_addr);
    }
  }
#if REX_PLATFORM_WIN32
  ::closesocket(s);
#else
  ::close(s);
#endif
  return a;
}
#endif

// This console's host address (host order): web: the relay's 10.80.0.<id> (kept while the
// relay reconnects), else 127.0.0.1; native: --xnet_address, else the primary LAN address,
// else 127.0.0.1.
uint32_t LocalAddress() {
#if REX_PLATFORM_WEB
  RelayUp();
  uint32_t a = nr::LocalAddress();
  if (!a) {
    a = kLoopback;
  }
#else
  static const uint32_t a = [] {
    uint32_t v = 0;
    std::string configured = REXCVAR_GET(xnet_address);
    if (!configured.empty() && !ParseDottedQuad(configured, &v)) {
      REXKRNL_WARN("xnet: --xnet_address '{}' is not an IPv4 address; detecting", configured);
      v = 0;
    }
    if (!v) {
      v = DetectHostAddress();
    }
    if (!v) {
      v = kLoopback;
    }
    REXKRNL_INFO("xnet: title XNADDR address {}", Ip(v));
    return v;
  }();
#endif
  g_book.SetLocal(a);
  return a;
}

bool LinkUp() {
  if (!REXCVAR_GET(xnet_link_up)) {
    return false;
  }
#if REX_PLATFORM_WEB
  return RelayUp();
#else
  return true;
#endif
}

void SignalEvent(uint32_t handle) {
  if (!handle) {
    return;
  }
  auto ev = kernel_state()->object_table()->LookupObject<XEvent>(handle);
  if (ev) {
    ev->Set(0, false);
  }
}

#if REX_PLATFORM_WEB
// --- Web: the NetDll socket exports on the net relay ----------------------------------------
// Only datagram sockets carry traffic: stream sockets can be created but connect, listen and
// accept fail (the relay has no TCP; System Link is UDP/VDP). SOCKET handles are the relay's
// small integers. Addresses the title passes may be secure (0.x.y.z, see xnet.h); they map
// back through g_book, and datagrams from LAN peers are reported by their secure address.
namespace relay {

constexpr uint32_t kSocketError = 0xFFFFFFFFu;
constexpr uint32_t kWsaSysNotReady = 10091;

uint32_t Fail(int err) {
  XThread::SetLastError(uint32_t(err));
  return kSocketError;
}

uint32_t Ok(uint32_t result = 0) {
  XThread::SetLastError(0);
  return result;
}

void MaybeLogStats() {
  int32_t interval = REXCVAR_GET(xnet_relay_stats_interval_ms);
  if (interval <= 0) {
    return;
  }
  static std::atomic<int64_t> last{0};
  int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count();
  int64_t prev = last.load(std::memory_order_relaxed);
  if (now - prev < interval || !last.compare_exchange_strong(prev, now)) {
    return;
  }
  nr::Stats s = nr::GetStats();
  REXKRNL_INFO(
      "xnet: relay stats id {} peers {} sockets {}: tx {} dgrams {} B ({} broadcast, {} dropped), "
      "rx {} dgrams {} B (no socket {}, queue full {}, ring dropped {}, malformed {}), "
      "loopback {}",
      s.id, s.peers, s.sockets_open, s.tx_datagrams, s.tx_bytes, s.tx_broadcast, s.tx_dropped,
      s.rx_datagrams, s.rx_bytes, s.rx_no_socket, s.rx_queue_full, s.rx_ring_dropped,
      s.rx_malformed, s.loopback);
}

// Default destination of connect()ed datagram sockets (send / recv).
std::mutex g_connected_mutex;
std::unordered_map<int, std::pair<uint32_t, uint16_t>> g_connected;

uint32_t Socket(uint32_t af, uint32_t type, uint32_t protocol) {
  if (!REXCVAR_GET(xnet_relay) && !RelayUp()) {
    return Fail(nr::kWsaENetDown);
  }
  LocalAddress();  // starts the relay, publishes the local address to g_book
  int err = 0;
  int s = nr::Socket(int(af), int(type), int(protocol), &err);
  XNET_TRACE("socket(af {}, type {}, protocol {}) -> {:#x} (err {})", af, type, protocol,
             uint32_t(s), err);
  return s < 0 ? Fail(err) : Ok(uint32_t(s));
}

uint32_t Close(uint32_t s) {
  int err = 0;
  int r = nr::Close(int(s), &err);
  XNET_TRACE("closesocket({:#x}) -> {} (err {})", s, r, err);
  {
    std::lock_guard<std::mutex> lock(g_connected_mutex);
    g_connected.erase(int(s));
  }
  return r < 0 ? Fail(err) : Ok();
}

uint32_t Shutdown(uint32_t s, uint32_t how) {
  XNET_TRACE("shutdown({:#x}, {})", s, how);
  if (!nr::IsSocket(int(s))) {
    return Fail(nr::kWsaENotSock);
  }
  return nr::IsStream(int(s)) ? Fail(nr::kWsaENotConn) : Ok();
}

uint32_t IoctlSocket(uint32_t s, uint32_t cmd, uint8_t* argp) {
  constexpr uint32_t kFionbio = 0x8004667E;
  constexpr uint32_t kFionread = 0x4004667F;
  int err = 0;
  int r = -1;
  if (!argp) {
    err = nr::kWsaEInval;
  } else if (cmd == kFionbio) {
    r = nr::SetNonBlocking(int(s), xnet::LoadBE32(argp) != 0, &err);
  } else if (cmd == kFionread) {
    uint32_t bytes = 0;
    r = nr::PendingBytes(int(s), &bytes, &err);
    if (r == 0) {
      xnet::StoreBE32(argp, bytes);
    }
  } else {
    err = nr::kWsaEInval;
  }
  XNET_TRACE("ioctlsocket({:#x}, {:#x}) -> {} (err {})", s, cmd, r, err);
  return r < 0 ? Fail(err) : Ok();
}

uint32_t SetSockOpt(uint32_t s, uint32_t level, uint32_t optname, const uint8_t* optval,
                    uint32_t optlen) {
  constexpr uint32_t kSolSocket = 0xFFFF;
  int err = 0;
  int r = 0;
  uint32_t v = (optval && optlen >= 4)   ? xnet::LoadBE32(optval)
               : (optval && optlen >= 1) ? optval[0]
                                         : 0;
  if (!nr::IsSocket(int(s))) {
    r = -1;
    err = nr::kWsaENotSock;
  } else if (level == kSolSocket && optname == 0x0020) {  // SO_BROADCAST
    r = nr::SetBroadcast(int(s), v != 0, &err);
  } else if (level == kSolSocket && optname == 0x1006) {  // SO_RCVTIMEO (ms)
    r = nr::SetReceiveTimeout(int(s), v, &err);
  }
  // Everything else is accepted without effect: SO_REUSEADDR, SO_SNDBUF / SO_RCVBUF, the
  // Xbox-only 0x5801 / 0x5802 (insecure / VDP options), TCP_NODELAY.
  static std::mutex seen_mutex;
  static std::unordered_map<uint64_t, bool> seen;
  bool first;
  {
    std::lock_guard<std::mutex> lock(seen_mutex);
    bool& was = seen[(uint64_t(level) << 32) | optname];
    first = !was;
    was = true;
  }
  if (first || REXCVAR_GET(xnet_trace)) {
    REXKRNL_INFO("xnet: setsockopt({:#x}, level {:#x}, opt {:#x}, value {:#x}, len {}) -> {} (err {})",
                 s, level, optname, v, optlen, r, err);
  }
  return r < 0 ? Fail(err) : Ok();
}

uint32_t Bind(uint32_t s, const uint8_t* name, uint32_t namelen) {
  ng::SockAddrIn a{};
  if (!ng::LoadSockAddrIn(name, namelen, &a) || a.family != ng::kAfInet) {
    return Fail(nr::kWsaEInval);
  }
  uint32_t host = g_book.TitleToHost(a.addr);
  int err = 0;
  int r = nr::Bind(int(s), host, a.port, &err);
  if (r < 0 || REXCVAR_GET(xnet_trace)) {
    REXKRNL_INFO("xnet: bind({:#x}, {}:{}) -> {} (err {})", s, Ip(host), a.port, r, err);
  } else {
    XNET_TRACE("bind({:#x}, {}:{}) -> {}", s, Ip(host), a.port, r);
  }
  return r < 0 ? Fail(err) : Ok();
}

uint32_t GetSockName(uint32_t s, uint8_t* name, uint8_t* namelen) {
  if (!name || !namelen) {
    return Fail(10014);  // WSAEFAULT
  }
  uint32_t addr = 0;
  uint16_t port = 0;
  int err = 0;
  int r = nr::GetSockName(int(s), &addr, &port, &err);
  XNET_TRACE("getsockname({:#x}) -> {} {}:{} (err {})", s, r, Ip(addr), port, err);
  if (r < 0) {
    return Fail(err);
  }
  uint32_t w = ng::StoreSockAddrIn(name, xnet::LoadBE32(namelen), {ng::kAfInet, port, addr});
  xnet::StoreBE32(namelen, w);
  return Ok();
}

uint32_t Connect(uint32_t s, const uint8_t* name, uint32_t namelen) {
  ng::SockAddrIn a{};
  bool parsed = ng::LoadSockAddrIn(name, namelen, &a);
  XNET_TRACE("connect({:#x}, {}:{}) on a {} socket", s, Ip(a.addr), a.port,
             nr::IsStream(int(s)) ? "stream" : "datagram");
  if (!nr::IsSocket(int(s))) {
    return Fail(nr::kWsaENotSock);
  }
  if (nr::IsStream(int(s))) {
    REXKRNL_WARN("xnet: connect({:#x}, {}:{}) on a stream socket: no TCP over the relay", s,
                 Ip(a.addr), a.port);
    return Fail(nr::kWsaEConnRefused);
  }
  if (!parsed) {
    return Fail(nr::kWsaEInval);
  }
  std::lock_guard<std::mutex> lock(g_connected_mutex);
  g_connected[int(s)] = {g_book.TitleToHost(a.addr), a.port};
  return Ok();
}

uint32_t Listen(uint32_t s, int32_t backlog) {
  REXKRNL_WARN("xnet: listen({:#x}, {}): no stream sockets over the relay", s, backlog);
  return Fail(nr::IsSocket(int(s)) ? nr::kWsaEOpNotSupp : nr::kWsaENotSock);
}

uint32_t Accept(uint32_t s) {
  REXKRNL_WARN("xnet: accept({:#x}): no stream sockets over the relay", s);
  return Fail(nr::IsSocket(int(s)) ? nr::kWsaEOpNotSupp : nr::kWsaENotSock);
}

uint32_t Select(uint8_t* readfds, uint8_t* writefds, uint8_t* exceptfds, const uint8_t* timeout) {
  int rs[ng::kFdSetSize], ws[ng::kFdSetSize];
  bool rr[ng::kFdSetSize] = {}, wr[ng::kFdSetSize] = {};
  uint32_t nread = readfds ? ng::FdSetCount(readfds) : 0;
  uint32_t nwrite = writefds ? ng::FdSetCount(writefds) : 0;
  for (uint32_t i = 0; i < nread; ++i) {
    rs[i] = int(ng::FdSetGet(readfds, i));
  }
  for (uint32_t i = 0; i < nwrite; ++i) {
    ws[i] = int(ng::FdSetGet(writefds, i));
  }
  int64_t timeout_us = -1;
  if (timeout) {
    timeout_us = std::max<int64_t>(ng::LoadTimevalUs(timeout), 0);
  }
  int err = 0;
  int n = nr::Select(rs, nread, rr, ws, nwrite, wr, timeout_us, &err);
  XNET_TRACE("select(read {}, write {}, except {}, timeout {} us) -> {} (err {})", nread, nwrite,
             exceptfds ? ng::FdSetCount(exceptfds) : 0, timeout_us, n, err);
  MaybeLogStats();
  if (n < 0) {
    return Fail(err);
  }
  uint32_t out[ng::kFdSetSize];
  if (readfds) {
    uint32_t k = 0;
    for (uint32_t i = 0; i < nread; ++i) {
      if (rr[i]) out[k++] = uint32_t(rs[i]);
    }
    ng::FdSetAssign(readfds, out, k);
  }
  if (writefds) {
    uint32_t k = 0;
    for (uint32_t i = 0; i < nwrite; ++i) {
      if (wr[i]) out[k++] = uint32_t(ws[i]);
    }
    ng::FdSetAssign(writefds, out, k);
  }
  if (exceptfds) {
    ng::FdSetAssign(exceptfds, out, 0);
  }
  return Ok(uint32_t(n));
}

// recvfrom into a host buffer; the source is reported as the title sees it. Returns the
// length or -1 with *err set (WSAEMSGSIZE still fills buf and the source).
int RecvFromRaw(uint32_t s, uint8_t* buf, uint32_t len, uint32_t* from_addr, uint16_t* from_port,
                int* err) {
  *from_addr = 0;
  *from_port = 0;
  int n = nr::RecvFrom(int(s), buf, len, from_addr, from_port, err);
  if (n >= 0 || *err == nr::kWsaEMsgSize) {
    *from_addr = g_book.SourceToTitle(*from_addr);
  }
  return n;
}

void StoreFrom(uint8_t* from, uint8_t* fromlen, uint32_t addr, uint16_t port) {
  if (from && fromlen) {
    uint32_t cap = xnet::LoadBE32(fromlen);
    uint32_t w = ng::StoreSockAddrIn(from, cap, {ng::kAfInet, port, addr});
    xnet::StoreBE32(fromlen, w);
  }
}

uint32_t RecvFrom(uint32_t s, uint8_t* buf, uint32_t len, uint32_t flags, uint8_t* from,
                  uint8_t* fromlen) {
  if (nr::IsStream(int(s))) {
    return Fail(nr::kWsaENotConn);
  }
  uint32_t from_addr = 0;
  uint16_t from_port = 0;
  int err = 0;
  int n = RecvFromRaw(s, buf, len, &from_addr, &from_port, &err);
  bool got = n >= 0 || err == nr::kWsaEMsgSize;
  if (got) {
    StoreFrom(from, fromlen, from_addr, from_port);
  }
  if (got || err != nr::kWsaEWouldBlock) {
    XNET_TRACE("recvfrom({:#x}, len {}, flags {:#x}) -> {} from {}:{} (err {})", s, len, flags, n,
               Ip(from_addr), from_port, err);
  }
  MaybeLogStats();
  return n < 0 ? Fail(err) : Ok(uint32_t(n));
}

// Destination of a send: `to` if given (secure addresses mapped back), else the connect()ed
// one. Returns 0 or a WSA error.
int ResolveDestination(uint32_t s, const uint8_t* to, uint32_t tolen, uint32_t* addr,
                       uint16_t* port) {
  if (to) {
    ng::SockAddrIn a{};
    if (!ng::LoadSockAddrIn(to, tolen, &a)) {
      return nr::kWsaEInval;
    }
    *addr = g_book.TitleToHost(a.addr);
    *port = a.port;
    return 0;
  }
  std::lock_guard<std::mutex> lock(g_connected_mutex);
  auto it = g_connected.find(int(s));
  if (it == g_connected.end()) {
    return nr::kWsaENotConn;
  }
  *addr = it->second.first;
  *port = it->second.second;
  return 0;
}

uint32_t SendTo(uint32_t s, const uint8_t* buf, uint32_t len, uint32_t flags, const uint8_t* to,
                uint32_t tolen) {
  if (nr::IsStream(int(s))) {
    return Fail(nr::kWsaENotConn);
  }
  uint32_t addr = 0;
  uint16_t port = 0;
  int err = ResolveDestination(s, to, tolen, &addr, &port);
  if (err) {
    return Fail(err);
  }
  int n = nr::SendTo(int(s), buf, len, addr, port, &err);
  XNET_TRACE("sendto({:#x}, len {}, flags {:#x}, {}:{}) -> {} (err {})", s, len, flags, Ip(addr),
             port, n, err);
  MaybeLogStats();
  return n < 0 ? Fail(err) : Ok(uint32_t(n));
}

}  // namespace relay
#endif  // REX_PLATFORM_WEB

}  // namespace

namespace xnet {

uint32_t GetTitleXnAddr(uint8_t* xnaddr_out) {
  uint32_t a = LocalAddress();
  if (xnaddr_out) {
    StoreXnAddr(xnaddr_out, a);
  }
  return kXnAddrStatic | kXnAddrEthernet;
}

Random& GlobalRandom() {
  return g_random;
}

bool DeterministicRandom() {
  return REXCVAR_GET(xnet_deterministic_random);
}

void RandomBytes(uint8_t* p, size_t n) {
  g_random.Fill(p, n, DeterministicRandom());
}

uint32_t ConsoleIdForXuid() {
#if REX_PLATFORM_WEB
  if (!REXCVAR_GET(xnet_unique_xuid)) {
    return 0;
  }
  RelayUp();
  return nr::PeerId();
#else
  return 0;
#endif
}

}  // namespace xnet

// https://github.com/joolswills/mameox/blob/master/MAMEoX/Sources/xbox_Network.cpp#L136
struct XNetStartupParams {
  uint8_t cfgSizeOfStruct;
  uint8_t cfgFlags;
  uint8_t cfgSockMaxDgramSockets;
  uint8_t cfgSockMaxStreamSockets;
  uint8_t cfgSockDefaultRecvBufsizeInK;
  uint8_t cfgSockDefaultSendBufsizeInK;
  uint8_t cfgKeyRegMax;
  uint8_t cfgSecRegMax;
  uint8_t cfgQosDataLimitDiv4;
  uint8_t cfgQosProbeTimeoutInSeconds;
  uint8_t cfgQosProbeRetries;
  uint8_t cfgQosSrvMaxSimultaneousResponses;
  uint8_t cfgQosPairWaitTimeInSeconds;
};

XNetStartupParams xnet_startup_params = {0};

ppc_u32_result_t NetDll_XNetStartup_entry(ppc_u32_t caller, ppc_ptr_t<XNetStartupParams> params) {
  if (params) {
    assert_true(params->cfgSizeOfStruct == sizeof(XNetStartupParams));
    std::memcpy(&xnet_startup_params, params, sizeof(XNetStartupParams));
  }

  auto xam = kernel_state()->GetKernelModule<XamModule>("xam.xex");

  /*
  if (!xam->xnet()) {
    auto xnet = new XNet(kernel_state());
    xnet->Initialize();

    xam->set_xnet(xnet);
  }
  */

  return 0;
}

ppc_u32_result_t NetDll_XNetCleanup_entry(ppc_u32_t caller, ppc_pvoid_t params) {
  auto xam = kernel_state()->GetKernelModule<XamModule>("xam.xex");
  // auto xnet = xam->xnet();
  // xam->set_xnet(nullptr);

  // TODO: Shut down and delete.
  // delete xnet;

  return 0;
}

ppc_u32_result_t NetDll_XNetGetOpt_entry(ppc_u32_t one, ppc_u32_t option_id, ppc_pvoid_t buffer_ptr,
                                         ppc_pu32_t buffer_size) {
  assert_true(one == 1);
  switch (option_id) {
    case 1:
      if (*buffer_size < sizeof(XNetStartupParams)) {
        *buffer_size = sizeof(XNetStartupParams);
        return 0x2738;  // WSAEMSGSIZE
      }
      std::memcpy(buffer_ptr, &xnet_startup_params, sizeof(XNetStartupParams));
      return 0;
    default:
      REXKRNL_ERROR("NetDll_XNetGetOpt: option {} unimplemented", option_id);
      return 0x2726;  // WSAEINVAL
  }
}

// Random bytes: consoles on one LAN need distinct nonces and session ids
// (--xnet_deterministic_random restores the old constant 0xBB for reproducible runs).
ppc_u32_result_t NetDll_XNetRandom_entry(ppc_u32_t caller, ppc_pvoid_t buffer_ptr,
                                         ppc_u32_t length) {
  if (buffer_ptr) {
    xnet::RandomBytes(buffer_ptr.as<uint8_t*>(), length);
  }
  return 0;
}

ppc_u32_result_t NetDll_WSAStartup_entry(ppc_u32_t caller, ppc_u16_t version,
                                         ppc_ptr_t<X_WSADATA> data_ptr) {
#if REX_PLATFORM_WEB
  {
    bool net = REXCVAR_GET(xnet_relay) ? RelayUp() : nr::GetState() == nr::State::kUp;
    XNET_TRACE("WSAStartup(version {:#06x}) relay {}", uint16_t(version), net ? "up" : "down");
    if (!REXCVAR_GET(xnet_relay) && !net) {
      return relay::kWsaSysNotReady;
    }
    if (data_ptr) {
      // lpVendorInfo is left untouched (some titles round-trip it).
      data_ptr->version = uint16_t(version);
      data_ptr->version_high = 0x0202;
      std::memset(data_ptr->description, 0, sizeof(data_ptr->description));
      std::memset(data_ptr->system_status, 0, sizeof(data_ptr->system_status));
      std::strcpy(data_ptr->description, "ReXGlue web net relay");
      std::strcpy(data_ptr->system_status, net ? "Running" : "Relay unreachable");
      data_ptr->max_sockets = 256;
      data_ptr->max_udpdg = 0xFFFF;
    }
    return 0;
  }
#endif
// TODO(benvanik): abstraction layer needed.
#if REX_PLATFORM_WIN32
  WSADATA wsaData;
  ZeroMemory(&wsaData, sizeof(WSADATA));
  int ret = WSAStartup(version, &wsaData);

  auto data_out = kernel_state()->memory()->TranslateVirtual(data_ptr.guest_address());

  if (data_ptr) {
    data_ptr->version = wsaData.wVersion;
    data_ptr->version_high = wsaData.wHighVersion;
    std::memcpy(&data_ptr->description, wsaData.szDescription, 0x100);
    std::memcpy(&data_ptr->system_status, wsaData.szSystemStatus, 0x80);
    data_ptr->max_sockets = wsaData.iMaxSockets;
    data_ptr->max_udpdg = wsaData.iMaxUdpDg;

    // Some games (5841099F) want this value round-tripped - they'll compare if
    // it changes and bugcheck if it does.
    uint32_t vendor_ptr = memory::load_and_swap<uint32_t>(data_out + 0x190);
    memory::store_and_swap<uint32_t>(data_out + 0x190, vendor_ptr);
  }
#else
  int ret = 0;
  if (data_ptr) {
    // Guess these values!
    data_ptr->version = version.value();
    data_ptr->description[0] = '\0';
    data_ptr->system_status[0] = '\0';
    data_ptr->max_sockets = 100;
    data_ptr->max_udpdg = 1024;
  }
#endif

  // DEBUG
  /*
  auto xam = kernel_state()->GetKernelModule<XamModule>("xam.xex");
  if (!xam->xnet()) {
    auto xnet = new XNet(kernel_state());
    xnet->Initialize();

    xam->set_xnet(xnet);
  }
  */

  return ret;
}

ppc_u32_result_t NetDll_WSACleanup_entry(ppc_u32_t caller) {
  // This does nothing. Xenia needs WSA running.
  return 0;
}

ppc_u32_result_t NetDll_WSAGetLastError_entry() {
  return XThread::GetLastError();
}

// Completes a WSA overlapped operation at once (the relay calls are synchronous).
void CompleteWsaOverlapped(XWSAOVERLAPPED* overlapped, uint32_t error, uint32_t bytes) {
  if (!overlapped) {
    return;
  }
  overlapped->internal = error;
  overlapped->internal_high = bytes;
  SignalEvent(overlapped->event_handle);
}

// WSARecvFrom(s, lpBuffers, dwBufferCount, lpNumberOfBytesRecvd, lpFlags, lpFrom, lpFromlen,
// lpOverlapped, lpCompletionRoutine).
ppc_u32_result_t NetDll_WSARecvFrom_entry(ppc_u32_t caller, ppc_u32_t socket,
                                          ppc_ptr_t<XWSABUF> buffers_ptr, ppc_u32_t buffer_count,
                                          ppc_pu32_t num_bytes_recv, ppc_pu32_t flags_ptr,
                                          ppc_ptr_t<XSOCKADDR_IN> from_addr,
                                          ppc_pu32_t from_len,
                                          ppc_ptr_t<XWSAOVERLAPPED> overlapped_ptr,
                                          ppc_pvoid_t completion_routine_ptr) {
#if REX_PLATFORM_WEB
  {
    // Gather into one buffer, receive one datagram, scatter it over the title's buffers.
    uint32_t total = 0;
    for (uint32_t i = 0; i < buffer_count; ++i) {
      total += buffers_ptr[i].len;
    }
    std::vector<uint8_t> tmp(std::max<uint32_t>(total, 1));
    uint32_t addr = 0;
    uint16_t port = 0;
    int err = 0;
    int n = relay::RecvFromRaw(socket, tmp.data(), total, &addr, &port, &err);
    bool got = n >= 0 || err == nr::kWsaEMsgSize;
    if (got) {
      uint32_t left = n >= 0 ? uint32_t(n) : total;
      uint32_t off = 0;
      for (uint32_t i = 0; i < buffer_count && left; ++i) {
        uint32_t c = std::min<uint32_t>(left, buffers_ptr[i].len);
        std::memcpy(kernel_memory()->TranslateVirtual(buffers_ptr[i].buf_ptr), tmp.data() + off, c);
        off += c;
        left -= c;
      }
      relay::StoreFrom(from_addr.as<uint8_t*>(), from_len.as<uint8_t*>(), addr, port);
    }
    XNET_TRACE("WSARecvFrom({:#x}, {} buffers, {} bytes) -> {} from {}:{} (err {})",
               uint32_t(socket), uint32_t(buffer_count), total, n, Ip(addr), port, err);
    if (n < 0) {
      return relay::Fail(err);
    }
    if (num_bytes_recv) {
      *num_bytes_recv = uint32_t(n);
    }
    if (flags_ptr) {
      *flags_ptr = 0;
    }
    CompleteWsaOverlapped(overlapped_ptr, 0, uint32_t(n));
    return relay::Ok(0);
  }
#endif
  if (overlapped_ptr) {
    // auto evt = kernel_state()->object_table()->LookupObject<XEvent>(
    //    overlapped_ptr->event_handle);

    // if (evt) {
    //  //evt->Set(0, false);
    //}
  }

  // we're not going to be receiving packets any time soon
  // return error so we don't wait on that - Cancerous
  return -1;
}

// If the socket is a VDP socket, buffer 0 is the game data length, and buffer 1
// is the unencrypted game data.
ppc_u32_result_t NetDll_WSASendTo_entry(ppc_u32_t caller, ppc_u32_t socket_handle,
                                        ppc_ptr_t<XWSABUF> buffers, ppc_u32_t num_buffers,
                                        ppc_pu32_t num_bytes_sent, ppc_u32_t flags,
                                        ppc_ptr_t<XSOCKADDR_IN> to_ptr, ppc_u32_t to_len,
                                        ppc_ptr_t<XWSAOVERLAPPED> overlapped,
                                        ppc_pvoid_t completion_routine) {
  assert(!completion_routine);

#if REX_PLATFORM_WEB
  {
    std::vector<uint8_t> tmp;
    for (uint32_t i = 0; i < num_buffers; i++) {
      const uint8_t* p = kernel_memory()->TranslateVirtual(buffers[i].buf_ptr);
      tmp.insert(tmp.end(), p, p + uint32_t(buffers[i].len));
    }
    uint32_t r = relay::SendTo(socket_handle, tmp.data(), uint32_t(tmp.size()), flags,
                               to_ptr.as<uint8_t*>(), to_len);
    if (r == relay::kSocketError) {
      return r;
    }
    if (num_bytes_sent) {
      *num_bytes_sent = r;
    }
    CompleteWsaOverlapped(overlapped, 0, r);
    return 0;
  }
#endif

  auto socket = kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  // Our sockets implementation doesn't support multiple buffers, so we need
  // to combine the buffers the game has given us!
  std::vector<uint8_t> combined_buffer_mem;
  uint32_t combined_buffer_size = 0;
  uint32_t combined_buffer_offset = 0;
  for (uint32_t i = 0; i < num_buffers; i++) {
    combined_buffer_size += buffers[i].len;
    combined_buffer_mem.resize(combined_buffer_size);
    uint8_t* combined_buffer = combined_buffer_mem.data();

    std::memcpy(combined_buffer + combined_buffer_offset,
                kernel_memory()->TranslateVirtual(buffers[i].buf_ptr), buffers[i].len);
    combined_buffer_offset += buffers[i].len;
  }

  N_XSOCKADDR_IN native_to(to_ptr);
  native_to.sin_addr = g_book.TitleToHost(uint32_t(native_to.sin_addr));
  int sent =
      socket->SendTo(combined_buffer_mem.data(), combined_buffer_size, flags, &native_to, to_len);
  if (sent >= 0 && num_bytes_sent) {
    *num_bytes_sent = uint32_t(sent);
  }
  CompleteWsaOverlapped(overlapped, sent >= 0 ? 0 : 0x2746, sent >= 0 ? uint32_t(sent) : 0);

  return 0;
}

ppc_u32_result_t NetDll_WSAWaitForMultipleEvents_entry(ppc_u32_t num_events, ppc_pu32_t events,
                                                       ppc_u32_t wait_all, ppc_u32_t timeout,
                                                       ppc_u32_t alertable) {
  if (num_events > 64) {
    XThread::SetLastError(87);  // ERROR_INVALID_PARAMETER
    return ~0u;
  }

  uint64_t timeout_wait = (uint64_t)timeout;

  X_STATUS result = 0;
  do {
    result = xboxkrnl::xeNtWaitForMultipleObjectsEx(num_events, events, wait_all, 1, alertable,
                                                    timeout != -1 ? &timeout_wait : nullptr);
  } while (result == X_STATUS_ALERTED);

  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return ~0u;
  }
  return 0;
}

ppc_u32_result_t NetDll_WSACreateEvent_entry() {
  XEvent* ev = new XEvent(kernel_state());
  ev->Initialize(true, false);
  return ev->handle();
}

ppc_u32_result_t NetDll_WSACloseEvent_entry(ppc_u32_t event_handle) {
  X_STATUS result = kernel_state()->object_table()->ReleaseHandle(event_handle);
  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return 0;
  }
  return 1;
}

ppc_u32_result_t NetDll_WSAResetEvent_entry(ppc_u32_t event_handle) {
  X_STATUS result = xboxkrnl::xeNtClearEvent(event_handle);
  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return 0;
  }
  return 1;
}

ppc_u32_result_t NetDll_WSASetEvent_entry(ppc_u32_t event_handle) {
  X_STATUS result = xboxkrnl::xeNtSetEvent(event_handle, nullptr);
  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return 0;
  }
  return 1;
}

struct XnAddrStatus {
  // Address acquisition is not yet complete
  static const uint32_t XNET_GET_XNADDR_PENDING = 0x00000000;
  // XNet is uninitialized or no debugger found
  static const uint32_t XNET_GET_XNADDR_NONE = 0x00000001;
  // Host has ethernet address (no IP address)
  static const uint32_t XNET_GET_XNADDR_ETHERNET = 0x00000002;
  // Host has statically assigned IP address
  static const uint32_t XNET_GET_XNADDR_STATIC = 0x00000004;
  // Host has DHCP assigned IP address
  static const uint32_t XNET_GET_XNADDR_DHCP = 0x00000008;
  // Host has PPPoE assigned IP address
  static const uint32_t XNET_GET_XNADDR_PPPOE = 0x00000010;
  // Host has one or more gateways configured
  static const uint32_t XNET_GET_XNADDR_GATEWAY = 0x00000020;
  // Host has one or more DNS servers configured
  static const uint32_t XNET_GET_XNADDR_DNS = 0x00000040;
  // Host is currently connected to online service
  static const uint32_t XNET_GET_XNADDR_ONLINE = 0x00000080;
  // Network configuration requires troubleshooting
  static const uint32_t XNET_GET_XNADDR_TROUBLESHOOT = 0x00008000;
};

// A unique, routable XNADDR: ina = this console's host address (web: the relay's
// 10.80.0.<id>; native: --xnet_address or the primary LAN address; 127.0.0.1 without either),
// abEnet a locally administered MAC derived from it. abEnet must not be zero: RakNet's 360
// version derives "random" 64-bit ids from it and RakPeer::Startup fails on zero (58411436).
ppc_u32_result_t NetDll_XNetGetTitleXnAddr_entry(ppc_u32_t caller, ppc_ptr_t<XNADDR> addr_ptr) {
  uint8_t xna[xnet::kXnAddrSize];
  uint32_t r = xnet::GetTitleXnAddr(xna);
  if (addr_ptr) {
    std::memcpy(addr_ptr.as<uint8_t*>(), xna, xnet::kXnAddrSize);
  }
  uint32_t a = xnet::XnAddrIna(xna);
  XNET_TRACE_CHANGES((uint64_t(r) << 32) | a, "XNetGetTitleXnAddr() -> {:#x}, ina {}", r, Ip(a));
  return r;
}

ppc_u32_result_t NetDll_XNetGetDebugXnAddr_entry(ppc_u32_t caller, ppc_ptr_t<XNADDR> addr_ptr) {
  addr_ptr.Zero();

  // XNET_GET_XNADDR_NONE causes caller to gracefully return.
  return XnAddrStatus::XNET_GET_XNADDR_NONE;
}

ppc_u32_result_t NetDll_XNetXnAddrToMachineId_entry(ppc_u32_t caller, ppc_ptr_t<XNADDR> addr_ptr,
                                                    ppc_pu32_t id_ptr) {
  // Tell the caller we're not signed in to live (non-zero ret)
  return 1;
}

void NetDll_XNetInAddrToString_entry(ppc_u32_t caller, ppc_u32_t in_addr, ppc_pchar_t string_out,
                                     ppc_u32_t string_size) {
  // IN_ADDR arrives as a big-endian register value: a.b.c.d == 0xAABBCCDD.
  rex::string::rex_strcpy(string_out, string_size, Ip(uint32_t(in_addr)).c_str());
}

// XNetXnAddrToInAddr(const XNADDR*, const XNKID*, IN_ADDR*): the secure IN_ADDR the title uses
// in socket calls for this peer (its host address with the first byte cleared, xnet.h). The
// peer's XNADDR and XNKID are remembered for XNetInAddrToXnAddr.
ppc_u32_result_t NetDll_XNetXnAddrToInAddr_entry(ppc_u32_t caller, ppc_pvoid_t xn_addr,
                                                 ppc_pvoid_t xid, ppc_pvoid_t in_addr) {
  if (!xn_addr || !in_addr) {
    return 0x2726;  // WSAEINVAL
  }
  LocalAddress();  // the book needs the local subnet
  uint32_t secure = g_book.Register(xn_addr.as<uint8_t*>(), xid ? xid.as<uint8_t*>() : nullptr);
  xnet::StoreBE32(in_addr.as<uint8_t*>(), secure);
  XNET_TRACE("XNetXnAddrToInAddr(ina {}) -> 0, {}", Ip(xnet::XnAddrIna(xn_addr.as<uint8_t*>())),
             Ip(secure));
  return 0;
}

// XNetInAddrToXnAddr(IN_ADDR, XNADDR*, XNKID*): the XNADDR / XNKID the peer was registered
// with; an unknown address gets a LAN XNADDR built from it and the last registered key id.
ppc_u32_result_t NetDll_XNetInAddrToXnAddr_entry(ppc_u32_t caller, ppc_u32_t in_addr,
                                                 ppc_pvoid_t xn_addr, ppc_pvoid_t xid) {
  uint32_t ina = in_addr;
  uint8_t xna[xnet::kXnAddrSize];
  uint8_t kid[xnet::kXnKidSize] = {};
  bool has_kid = false;
  bool known = g_book.Lookup(ina, xna, kid, &has_kid);
  if (!known) {
    xnet::StoreXnAddr(xna, g_book.TitleToHost(ina));
  }
  if (!has_kid) {
    g_keys.Last(kid);
  }
  if (xn_addr) {
    std::memcpy(xn_addr.as<uint8_t*>(), xna, xnet::kXnAddrSize);
  }
  if (xid) {
    std::memcpy(xid.as<uint8_t*>(), kid, xnet::kXnKidSize);
  }
  XNET_TRACE("XNetInAddrToXnAddr({}) -> 0 ({})", Ip(ina), known ? "registered" : "unregistered");
  return 0;
}

// https://www.google.com/patents/WO2008112448A1?cl=en
// Reserves a port for use by system link
ppc_u32_result_t NetDll_XNetSetSystemLinkPort_entry(ppc_u32_t caller, ppc_u32_t port) {
  g_system_link_port = uint32_t(port);
  XNET_TRACE("XNetSetSystemLinkPort({:#x}) -> 0", uint32_t(port));
  return 0;
}

ppc_u32_result_t NetDll_XNetConnect_entry(ppc_u32_t caller, ppc_u32_t in_addr) {
  XNET_TRACE("XNetConnect({}) -> 0", Ip(in_addr));
  return 0;
}

// Every console on the (virtual) LAN is reachable: no key exchange to wait for.
ppc_u32_result_t NetDll_XNetGetConnectStatus_entry(ppc_u32_t caller, ppc_u32_t in_addr) {
  uint32_t r = uint32_t(in_addr) ? xnet::kConnectConnected : xnet::kConnectIdle;
  XNET_TRACE_CHANGES(uint32_t(in_addr), "XNetGetConnectStatus({}) -> {}", Ip(in_addr), r);
  return r;
}

ppc_u32_result_t NetDll_XNetUnregisterInAddr_entry(ppc_u32_t caller, ppc_u32_t in_addr) {
  bool known = g_book.Unregister(in_addr);
  XNET_TRACE("XNetUnregisterInAddr({}) -> 0 ({})", Ip(in_addr), known ? "registered" : "unknown");
  return 0;
}

// XNetCreateKey(XNKID*, XNKEY*): a new system-link key id (top nibble clear) and key.
ppc_u32_result_t NetDll_XNetCreateKey_entry(ppc_u32_t caller, ppc_pvoid_t xid, ppc_pvoid_t key) {
  bool det = xnet::DeterministicRandom();
  if (xid) {
    xnet::MakeXnKid(xid.as<uint8_t*>(), g_random, xnet::kXnKidSystemLink, det);
  }
  if (key) {
    xnet::MakeXnKey(key.as<uint8_t*>(), g_random, det);
  }
  XNET_TRACE("XNetCreateKey() -> 0");
  return 0;
}

ppc_u32_result_t NetDll_XNetRegisterKey_entry(ppc_u32_t caller, ppc_pvoid_t xid, ppc_pvoid_t key) {
  if (!xid) {
    return 0x2726;  // WSAEINVAL
  }
  g_keys.Register(xid.as<uint8_t*>(), key ? key.as<uint8_t*>() : nullptr);
  XNET_TRACE("XNetRegisterKey() -> 0 ({} registered)", g_keys.size());
  return 0;
}

ppc_u32_result_t NetDll_XNetUnregisterKey_entry(ppc_u32_t caller, ppc_pvoid_t xid) {
  if (xid) {
    g_keys.Unregister(xid.as<uint8_t*>());
  }
  XNET_TRACE("XNetUnregisterKey() -> 0");
  return 0;
}

// XNetQosLookup(cxna, apxna, apxnkid, apxnkey, cina, aina, adwServiceId, cProbes,
// dwBitsPerSec, dwFlags, hEvent, ppxnqos): every console is one LAN hop away, so each target
// is answered at once as contacted (1 ms, 100 Mbit/s). Titles that probe session peers
// (Halo 3's observer) never finish a join without this. XNQOS lives on the system heap like
// XNetQosServiceLookup's, so XNetQosRelease frees it.
ppc_u32_result_t NetDll_XNetQosLookup_entry(ppc_u32_t caller, ppc_u32_t cxna, ppc_pvoid_t apxna,
                                            ppc_pvoid_t apxnkid, ppc_pvoid_t apxnkey,
                                            ppc_u32_t cina, ppc_pvoid_t aina,
                                            ppc_pvoid_t service_ids, ppc_u32_t probes,
                                            ppc_u32_t bits_per_sec, ppc_u32_t flags,
                                            ppc_u32_t event_handle, ppc_pu32_t pqos) {
  uint32_t n = uint32_t(cxna) + uint32_t(cina);
  if (!pqos || n > 64) {
    return 0x2726;  // WSAEINVAL
  }
  uint32_t size = xnet::QosBlockSize(n);
  uint32_t guest = kernel_memory()->SystemHeapAlloc(size);
  if (!guest) {
    return 0x8007000E;  // E_OUTOFMEMORY
  }
  xnet::FillQosAllContacted(kernel_memory()->TranslateVirtual<uint8_t*>(guest), n, probes);
  *pqos = guest;
  SignalEvent(event_handle);
  XNET_TRACE_CHANGES((uint64_t(uint32_t(cxna)) << 32) | uint32_t(cina),
                     "XNetQosLookup({} xnaddr, {} inaddr, {} probes, flags {:#x}) -> 0, all "
                     "complete",
                     uint32_t(cxna), uint32_t(cina), uint32_t(probes), uint32_t(flags));
  return 0;
}

// https://github.com/ILOVEPIE/Cxbx-Reloaded/blob/master/src/CxbxKrnl/EmuXOnline.h#L39
struct XEthernetStatus {
  static const uint32_t XNET_ETHERNET_LINK_ACTIVE = 0x01;
  static const uint32_t XNET_ETHERNET_LINK_100MBPS = 0x02;
  static const uint32_t XNET_ETHERNET_LINK_10MBPS = 0x04;
  static const uint32_t XNET_ETHERNET_LINK_FULL_DUPLEX = 0x08;
  static const uint32_t XNET_ETHERNET_LINK_HALF_DUPLEX = 0x10;
};

// Link up (--xnet_link_up; web: while the relay is up). MW2's multiplayer refuses every match,
// including a console `map`, without a link.
ppc_u32_result_t NetDll_XNetGetEthernetLinkStatus_entry(ppc_u32_t caller) {
  uint32_t r = LinkUp() ? xnet::kLinkUp : 0;
  XNET_TRACE_CHANGES(r, "XNetGetEthernetLinkStatus() -> {:#x}", r);
  return r;
}

ppc_u32_result_t NetDll_XNetDnsLookup_entry(ppc_u32_t caller, ppc_pchar_t host,
                                            ppc_u32_t event_handle, ppc_pu32_t pdns) {
  // TODO(gibbed): actually implement this
  if (pdns) {
    auto dns_guest = kernel_memory()->SystemHeapAlloc(sizeof(XNDNS));
    auto dns = kernel_memory()->TranslateVirtual<XNDNS*>(dns_guest);
    dns->status = 1;  // non-zero = error
    *pdns = dns_guest;
  }
  if (event_handle) {
    auto ev = kernel_state()->object_table()->LookupObject<XEvent>(event_handle);
    assert_not_null(ev);
    ev->Set(0, false);
  }
  return 0;
}

ppc_u32_result_t NetDll_XNetDnsRelease_entry(ppc_u32_t caller, ppc_ptr_t<XNDNS> dns) {
  if (!dns) {
    return X_STATUS_INVALID_PARAMETER;
  }
  kernel_memory()->SystemHeapFree(dns.guest_address());
  return 0;
}

ppc_u32_result_t NetDll_XNetQosServiceLookup_entry(ppc_u32_t caller, ppc_u32_t flags,
                                                   ppc_u32_t event_handle, ppc_pu32_t pqos) {
  // Set pqos as some games will try accessing it despite non-successful result
  if (pqos) {
    auto qos_guest = kernel_memory()->SystemHeapAlloc(sizeof(XNQOS));
    auto qos = kernel_memory()->TranslateVirtual<XNQOS*>(qos_guest);
    qos->count = qos->count_pending = 0;
    *pqos = qos_guest;
  }
  if (event_handle) {
    auto ev = kernel_state()->object_table()->LookupObject<XEvent>(event_handle);
    assert_not_null(ev);
    ev->Set(0, false);
  }
  return 0;
}

ppc_u32_result_t NetDll_XNetQosRelease_entry(ppc_u32_t caller, ppc_ptr_t<XNQOS> qos) {
  if (!qos) {
    return X_STATUS_INVALID_PARAMETER;
  }
  kernel_memory()->SystemHeapFree(qos.guest_address());
  return 0;
}

// Hosts publish their QoS listen data here; nothing probes it over the LAN, so accept it.
ppc_u32_result_t NetDll_XNetQosListen_entry(ppc_u32_t caller, ppc_pvoid_t id, ppc_pvoid_t data,
                                            ppc_u32_t data_size, ppc_u32_t r7, ppc_u32_t flags) {
  XNET_TRACE_CHANGES((uint64_t(uint32_t(data_size)) << 32) | uint32_t(flags),
                     "XNetQosListen(size {}, flags {:#x}) -> 0", uint32_t(data_size),
                     uint32_t(flags));
  return 0;
}

ppc_u32_result_t NetDll_inet_addr_entry(ppc_pchar_t addr_ptr) {
  if (!addr_ptr) {
    return -1;
  }

  uint32_t addr = inet_addr(addr_ptr);
  // https://docs.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-inet_addr#return-value
  // Based on console research it seems like x360 uses old version of inet_addr
  // In case of empty string it return 0 instead of -1
  if (addr == -1 && !addr_ptr.value().length()) {
    return 0;
  }

  return rex::byte_swap(addr);
}

ppc_u32_result_t NetDll_socket_entry(ppc_u32_t caller, ppc_u32_t af, ppc_u32_t type,
                                     ppc_u32_t protocol) {
#if REX_PLATFORM_WEB
  return relay::Socket(af, type, protocol);
#endif
  LocalAddress();  // publishes the local subnet to the address book
  XSocket* socket = new XSocket(kernel_state());
  X_STATUS result =
      socket->Initialize(XSocket::AddressFamily((uint32_t)af), XSocket::Type((uint32_t)type),
                         XSocket::Protocol((uint32_t)protocol));

  if (XFAILED(result)) {
    socket->Release();

    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return -1;
  }

  return socket->handle();
}

ppc_u32_result_t NetDll_closesocket_entry(ppc_u32_t caller, ppc_u32_t socket_handle) {
#if REX_PLATFORM_WEB
  return relay::Close(socket_handle);
#endif
  auto socket = kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  // TODO: Absolutely delete this object. It is no longer valid after calling
  // closesocket.
  socket->Close();
  socket->ReleaseHandle();
  return 0;
}

ppc_i32_result_t NetDll_shutdown_entry(ppc_u32_t caller, ppc_u32_t socket_handle, ppc_i32_t how) {
#if REX_PLATFORM_WEB
  return int32_t(relay::Shutdown(socket_handle, uint32_t(int32_t(how))));
#endif
  auto socket = kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  auto ret = socket->Shutdown(how);
  if (ret == -1) {
#if REX_PLATFORM_WIN32
    uint32_t error_code = WSAGetLastError();
    XThread::SetLastError(error_code);
#else
    XThread::SetLastError(0x0);
#endif
  }
  return ret;
}

ppc_u32_result_t NetDll_setsockopt_entry(ppc_u32_t caller, ppc_u32_t socket_handle, ppc_u32_t level,
                                         ppc_u32_t optname, ppc_pvoid_t optval_ptr,
                                         ppc_u32_t optlen) {
#if REX_PLATFORM_WEB
  return relay::SetSockOpt(socket_handle, level, optname, optval_ptr.as<uint8_t*>(), optlen);
#endif
  auto socket = kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  X_STATUS status = socket->SetOption(level, optname, optval_ptr, optlen);
  return XSUCCEEDED(status) ? 0 : -1;
}

ppc_u32_result_t NetDll_ioctlsocket_entry(ppc_u32_t caller, ppc_u32_t socket_handle, ppc_u32_t cmd,
                                          ppc_pvoid_t arg_ptr) {
#if REX_PLATFORM_WEB
  return relay::IoctlSocket(socket_handle, cmd, arg_ptr.as<uint8_t*>());
#endif
  auto socket = kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  X_STATUS status = socket->IOControl(cmd, arg_ptr);
  if (XFAILED(status)) {
    XThread::SetLastError(xboxkrnl::xeRtlNtStatusToDosError(status));
    return -1;
  }

  // TODO
  return 0;
}

ppc_u32_result_t NetDll_bind_entry(ppc_u32_t caller, ppc_u32_t socket_handle,
                                   ppc_ptr_t<XSOCKADDR_IN> name, ppc_u32_t namelen) {
#if REX_PLATFORM_WEB
  return relay::Bind(socket_handle, name.as<uint8_t*>(), namelen);
#endif
  auto socket = kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR_IN native_name(name);
  native_name.sin_addr = g_book.TitleToHost(uint32_t(native_name.sin_addr));
  X_STATUS status = socket->Bind(&native_name, namelen);
  if (XFAILED(status)) {
    XThread::SetLastError(xboxkrnl::xeRtlNtStatusToDosError(status));
    return -1;
  }

  return 0;
}

ppc_u32_result_t NetDll_connect_entry(ppc_u32_t caller, ppc_u32_t socket_handle,
                                      ppc_ptr_t<XSOCKADDR> name, ppc_u32_t namelen) {
#if REX_PLATFORM_WEB
  return relay::Connect(socket_handle, name.as<uint8_t*>(), namelen);
#endif
  auto socket = kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR native_name(name);
  if (native_name.address_family == AF_INET) {  // sa_data: u16 port, u32 address (big-endian)
    auto* a = reinterpret_cast<uint8_t*>(native_name.sa_data) + 2;
    xnet::StoreBE32(a, g_book.TitleToHost(xnet::LoadBE32(a)));
  }
  X_STATUS status = socket->Connect(&native_name, namelen);
  if (XFAILED(status)) {
    XThread::SetLastError(xboxkrnl::xeRtlNtStatusToDosError(status));
    return -1;
  }

  return 0;
}

ppc_u32_result_t NetDll_listen_entry(ppc_u32_t caller, ppc_u32_t socket_handle, ppc_i32_t backlog) {
#if REX_PLATFORM_WEB
  return relay::Listen(socket_handle, int32_t(backlog));
#endif
  auto socket = kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  X_STATUS status = socket->Listen(backlog);
  if (XFAILED(status)) {
    XThread::SetLastError(xboxkrnl::xeRtlNtStatusToDosError(status));
    return -1;
  }

  return 0;
}

ppc_u32_result_t NetDll_accept_entry(ppc_u32_t caller, ppc_u32_t socket_handle,
                                     ppc_ptr_t<XSOCKADDR> addr_ptr, ppc_pu32_t addrlen_ptr) {
#if REX_PLATFORM_WEB
  return relay::Accept(socket_handle);
#endif
  if (!addr_ptr) {
    // WSAEFAULT
    XThread::SetLastError(0x271E);
    return -1;
  }

  auto socket = kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR native_addr(addr_ptr);
  int native_len = *addrlen_ptr;
  auto new_socket = socket->Accept(&native_addr, &native_len);
  if (new_socket) {
    addr_ptr->address_family = native_addr.address_family;
    std::memcpy(addr_ptr->sa_data, native_addr.sa_data, *addrlen_ptr - 2);
    *addrlen_ptr = native_len;

    return new_socket->handle();
  } else {
    return -1;
  }
}

struct x_fd_set {
  rex::be<uint32_t> fd_count;
  rex::be<uint32_t> fd_array[64];
};

struct host_set {
  uint32_t count;
  object_ref<XSocket> sockets[64];

  void Load(const x_fd_set* guest_set) {
    assert_true(guest_set->fd_count < 64);
    this->count = guest_set->fd_count;
    for (uint32_t i = 0; i < this->count; ++i) {
      auto socket_handle = static_cast<X_HANDLE>(guest_set->fd_array[i]);
      if (socket_handle == -1) {
        this->count = i;
        break;
      }
      // Convert from Xenia -> native
      auto socket = kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
      assert_not_null(socket);
      this->sockets[i] = socket;
    }
  }

  void Store(x_fd_set* guest_set) {
    guest_set->fd_count = 0;
    for (uint32_t i = 0; i < this->count; ++i) {
      auto socket = this->sockets[i];
      guest_set->fd_array[guest_set->fd_count++] = socket->handle();
    }
  }

  void Store(fd_set* native_set) {
    FD_ZERO(native_set);
    for (uint32_t i = 0; i < this->count; ++i) {
      FD_SET(this->sockets[i]->native_handle(), native_set);
    }
  }

  void UpdateFrom(fd_set* native_set) {
    uint32_t new_count = 0;
    for (uint32_t i = 0; i < this->count; ++i) {
      auto socket = this->sockets[i];
      if (FD_ISSET(socket->native_handle(), native_set)) {
        this->sockets[new_count++] = socket;
      }
    }
    this->count = new_count;
  }
};

ppc_i32_result_t NetDll_select_entry(ppc_i32_t caller, ppc_i32_t nfds, ppc_ptr_t<x_fd_set> readfds,
                                     ppc_ptr_t<x_fd_set> writefds, ppc_ptr_t<x_fd_set> exceptfds,
                                     ppc_pvoid_t timeout_ptr) {
#if REX_PLATFORM_WEB
  return int32_t(relay::Select(readfds.as<uint8_t*>(), writefds.as<uint8_t*>(),
                               exceptfds.as<uint8_t*>(), timeout_ptr.as<uint8_t*>()));
#endif
  host_set host_readfds = {0};
  fd_set native_readfds = {0};
  if (readfds) {
    host_readfds.Load(readfds);
    host_readfds.Store(&native_readfds);
  }
  host_set host_writefds = {0};
  fd_set native_writefds = {0};
  if (writefds) {
    host_writefds.Load(writefds);
    host_writefds.Store(&native_writefds);
  }
  host_set host_exceptfds = {0};
  fd_set native_exceptfds = {0};
  if (exceptfds) {
    host_exceptfds.Load(exceptfds);
    host_exceptfds.Store(&native_exceptfds);
  }
  timeval* timeout_in = nullptr;
  timeval timeout;
  if (timeout_ptr) {
    timeout = {static_cast<int32_t>(timeout_ptr.as_array<int32_t>()[0]),
               static_cast<int32_t>(timeout_ptr.as_array<int32_t>()[1])};
    chrono::Clock::ScaleGuestDurationTimeval(reinterpret_cast<int32_t*>(&timeout.tv_sec),
                                             reinterpret_cast<int32_t*>(&timeout.tv_usec));
    timeout_in = &timeout;
  }
  int ret = select(nfds, readfds ? &native_readfds : nullptr, writefds ? &native_writefds : nullptr,
                   exceptfds ? &native_exceptfds : nullptr, timeout_in);
  if (readfds) {
    host_readfds.UpdateFrom(&native_readfds);
    host_readfds.Store(readfds);
  }
  if (writefds) {
    host_writefds.UpdateFrom(&native_writefds);
    host_writefds.Store(writefds);
  }
  if (exceptfds) {
    host_exceptfds.UpdateFrom(&native_exceptfds);
    host_exceptfds.Store(exceptfds);
  }

  // TODO(gibbed): modify ret to be what's actually copied to the guest fd_sets?
  return ret;
}

ppc_u32_result_t NetDll_recv_entry(ppc_u32_t caller, ppc_u32_t socket_handle, ppc_pvoid_t buf_ptr,
                                   ppc_u32_t buf_len, ppc_u32_t flags) {
#if REX_PLATFORM_WEB
  return relay::RecvFrom(socket_handle, buf_ptr.as<uint8_t*>(), buf_len, flags, nullptr, nullptr);
#endif
  auto socket = kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  return socket->Recv(buf_ptr, buf_len, flags);
}

ppc_u32_result_t NetDll_recvfrom_entry(ppc_u32_t caller, ppc_u32_t socket_handle,
                                       ppc_pvoid_t buf_ptr, ppc_u32_t buf_len, ppc_u32_t flags,
                                       ppc_ptr_t<XSOCKADDR_IN> from_ptr, ppc_pu32_t fromlen_ptr) {
#if REX_PLATFORM_WEB
  return relay::RecvFrom(socket_handle, buf_ptr.as<uint8_t*>(), buf_len, flags,
                         from_ptr.as<uint8_t*>(), fromlen_ptr.as<uint8_t*>());
#endif
  auto socket = kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR_IN native_from;
  if (from_ptr) {
    native_from = *from_ptr;
  }
  uint32_t native_fromlen = fromlen_ptr ? fromlen_ptr.value() : 0;
  int ret =
      socket->RecvFrom(buf_ptr, buf_len, flags, &native_from, fromlen_ptr ? &native_fromlen : 0);

  if (from_ptr) {
    from_ptr->sin_family = native_from.sin_family;
    from_ptr->sin_port = native_from.sin_port;
    // LAN peers by their secure address, as XNetXnAddrToInAddr gave them to the title.
    from_ptr->sin_addr =
        ret >= 0 ? g_book.SourceToTitle(uint32_t(native_from.sin_addr))
                 : uint32_t(native_from.sin_addr);
    std::memset(from_ptr->x_sin_zero, 0, sizeof(from_ptr->x_sin_zero));
  }
  if (fromlen_ptr) {
    *fromlen_ptr = native_fromlen;
  }

  if (ret == -1) {
// TODO: Better way of getting the error code
#if REX_PLATFORM_WIN32
    uint32_t error_code = WSAGetLastError();
    XThread::SetLastError(error_code);
#else
    XThread::SetLastError(0x0);
#endif
  }

  return ret;
}

ppc_u32_result_t NetDll_send_entry(ppc_u32_t caller, ppc_u32_t socket_handle, ppc_pvoid_t buf_ptr,
                                   ppc_u32_t buf_len, ppc_u32_t flags) {
#if REX_PLATFORM_WEB
  return relay::SendTo(socket_handle, buf_ptr.as<uint8_t*>(), buf_len, flags, nullptr, 0);
#endif
  auto socket = kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  return socket->Send(buf_ptr, buf_len, flags);
}

ppc_u32_result_t NetDll_sendto_entry(ppc_u32_t caller, ppc_u32_t socket_handle, ppc_pvoid_t buf_ptr,
                                     ppc_u32_t buf_len, ppc_u32_t flags,
                                     ppc_ptr_t<XSOCKADDR_IN> to_ptr, ppc_u32_t to_len) {
#if REX_PLATFORM_WEB
  return relay::SendTo(socket_handle, buf_ptr.as<uint8_t*>(), buf_len, flags,
                       to_ptr.as<uint8_t*>(), to_len);
#endif
  auto socket = kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR_IN native_to(to_ptr);
  native_to.sin_addr = g_book.TitleToHost(uint32_t(native_to.sin_addr));
  return socket->SendTo(buf_ptr, buf_len, flags, &native_to, to_len);
}

// getsockname(s, name, namelen). Web: the relay socket's bound address and port. Native: not
// implemented (as before: returns 0 without writing).
ppc_u32_result_t NetDll_getsockname_entry(ppc_u32_t caller, ppc_u32_t socket_handle,
                                          ppc_pvoid_t name, ppc_pvoid_t namelen) {
#if REX_PLATFORM_WEB
  return relay::GetSockName(socket_handle, name.as<uint8_t*>(), namelen.as<uint8_t*>());
#else
  return 0;
#endif
}

ppc_u32_result_t NetDll___WSAFDIsSet_entry(ppc_u32_t socket_handle, ppc_ptr_t<x_fd_set> fd_set) {
  const uint8_t max_fd_count = std::min((uint32_t)fd_set->fd_count, uint32_t(64));
  for (uint8_t i = 0; i < max_fd_count; i++) {
    if (fd_set->fd_array[i] == socket_handle) {
      return 1;
    }
  }
  return 0;
}

void NetDll_WSASetLastError_entry(ppc_u32_t error_code) {
  XThread::SetLastError(error_code);
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

XAM_EXPORT(__imp__NetDll_XNetStartup, rex::kernel::xam::NetDll_XNetStartup_entry)
XAM_EXPORT(__imp__NetDll_XNetCleanup, rex::kernel::xam::NetDll_XNetCleanup_entry)
XAM_EXPORT(__imp__NetDll_XNetGetOpt, rex::kernel::xam::NetDll_XNetGetOpt_entry)
XAM_EXPORT(__imp__NetDll_XNetRandom, rex::kernel::xam::NetDll_XNetRandom_entry)
XAM_EXPORT(__imp__NetDll_WSAStartup, rex::kernel::xam::NetDll_WSAStartup_entry)
XAM_EXPORT(__imp__NetDll_WSACleanup, rex::kernel::xam::NetDll_WSACleanup_entry)
XAM_EXPORT(__imp__NetDll_WSAGetLastError, rex::kernel::xam::NetDll_WSAGetLastError_entry)
XAM_EXPORT(__imp__NetDll_WSARecvFrom, rex::kernel::xam::NetDll_WSARecvFrom_entry)
XAM_EXPORT(__imp__NetDll_WSASendTo, rex::kernel::xam::NetDll_WSASendTo_entry)
XAM_EXPORT(__imp__NetDll_WSAWaitForMultipleEvents,
           rex::kernel::xam::NetDll_WSAWaitForMultipleEvents_entry)
XAM_EXPORT(__imp__NetDll_WSACreateEvent, rex::kernel::xam::NetDll_WSACreateEvent_entry)
XAM_EXPORT(__imp__NetDll_WSACloseEvent, rex::kernel::xam::NetDll_WSACloseEvent_entry)
XAM_EXPORT(__imp__NetDll_WSAResetEvent, rex::kernel::xam::NetDll_WSAResetEvent_entry)
XAM_EXPORT(__imp__NetDll_WSASetEvent, rex::kernel::xam::NetDll_WSASetEvent_entry)
XAM_EXPORT(__imp__NetDll_XNetGetTitleXnAddr, rex::kernel::xam::NetDll_XNetGetTitleXnAddr_entry)
XAM_EXPORT(__imp__NetDll_XNetGetDebugXnAddr, rex::kernel::xam::NetDll_XNetGetDebugXnAddr_entry)
XAM_EXPORT(__imp__NetDll_XNetXnAddrToMachineId,
           rex::kernel::xam::NetDll_XNetXnAddrToMachineId_entry)
XAM_EXPORT(__imp__NetDll_XNetInAddrToString, rex::kernel::xam::NetDll_XNetInAddrToString_entry)
XAM_EXPORT(__imp__NetDll_XNetXnAddrToInAddr, rex::kernel::xam::NetDll_XNetXnAddrToInAddr_entry)
XAM_EXPORT(__imp__NetDll_XNetInAddrToXnAddr, rex::kernel::xam::NetDll_XNetInAddrToXnAddr_entry)
XAM_EXPORT(__imp__NetDll_XNetSetSystemLinkPort,
           rex::kernel::xam::NetDll_XNetSetSystemLinkPort_entry)
XAM_EXPORT(__imp__NetDll_XNetGetEthernetLinkStatus,
           rex::kernel::xam::NetDll_XNetGetEthernetLinkStatus_entry)
XAM_EXPORT(__imp__NetDll_XNetDnsLookup, rex::kernel::xam::NetDll_XNetDnsLookup_entry)
XAM_EXPORT(__imp__NetDll_XNetDnsRelease, rex::kernel::xam::NetDll_XNetDnsRelease_entry)
XAM_EXPORT(__imp__NetDll_XNetQosServiceLookup, rex::kernel::xam::NetDll_XNetQosServiceLookup_entry)
XAM_EXPORT(__imp__NetDll_XNetQosRelease, rex::kernel::xam::NetDll_XNetQosRelease_entry)
XAM_EXPORT(__imp__NetDll_XNetQosListen, rex::kernel::xam::NetDll_XNetQosListen_entry)
XAM_EXPORT(__imp__NetDll_inet_addr, rex::kernel::xam::NetDll_inet_addr_entry)
XAM_EXPORT(__imp__NetDll_socket, rex::kernel::xam::NetDll_socket_entry)
XAM_EXPORT(__imp__NetDll_closesocket, rex::kernel::xam::NetDll_closesocket_entry)
XAM_EXPORT(__imp__NetDll_shutdown, rex::kernel::xam::NetDll_shutdown_entry)
XAM_EXPORT(__imp__NetDll_setsockopt, rex::kernel::xam::NetDll_setsockopt_entry)
XAM_EXPORT(__imp__NetDll_ioctlsocket, rex::kernel::xam::NetDll_ioctlsocket_entry)
XAM_EXPORT(__imp__NetDll_bind, rex::kernel::xam::NetDll_bind_entry)
XAM_EXPORT(__imp__NetDll_connect, rex::kernel::xam::NetDll_connect_entry)
XAM_EXPORT(__imp__NetDll_listen, rex::kernel::xam::NetDll_listen_entry)
XAM_EXPORT(__imp__NetDll_accept, rex::kernel::xam::NetDll_accept_entry)
XAM_EXPORT(__imp__NetDll_select, rex::kernel::xam::NetDll_select_entry)
XAM_EXPORT(__imp__NetDll_recv, rex::kernel::xam::NetDll_recv_entry)
XAM_EXPORT(__imp__NetDll_recvfrom, rex::kernel::xam::NetDll_recvfrom_entry)
XAM_EXPORT(__imp__NetDll_send, rex::kernel::xam::NetDll_send_entry)
XAM_EXPORT(__imp__NetDll_sendto, rex::kernel::xam::NetDll_sendto_entry)
XAM_EXPORT(__imp__NetDll___WSAFDIsSet, rex::kernel::xam::NetDll___WSAFDIsSet_entry)
XAM_EXPORT(__imp__NetDll_WSASetLastError, rex::kernel::xam::NetDll_WSASetLastError_entry)
XAM_EXPORT(__imp__NetDll_getsockname, rex::kernel::xam::NetDll_getsockname_entry)
XAM_EXPORT(__imp__NetDll_XNetConnect, rex::kernel::xam::NetDll_XNetConnect_entry)
XAM_EXPORT(__imp__NetDll_XNetGetConnectStatus, rex::kernel::xam::NetDll_XNetGetConnectStatus_entry)
XAM_EXPORT(__imp__NetDll_XNetUnregisterInAddr, rex::kernel::xam::NetDll_XNetUnregisterInAddr_entry)
XAM_EXPORT(__imp__NetDll_XNetCreateKey, rex::kernel::xam::NetDll_XNetCreateKey_entry)
XAM_EXPORT(__imp__NetDll_XNetRegisterKey, rex::kernel::xam::NetDll_XNetRegisterKey_entry)
XAM_EXPORT(__imp__NetDll_XNetUnregisterKey, rex::kernel::xam::NetDll_XNetUnregisterKey_entry)
XAM_EXPORT(__imp__NetDll_XNetQosLookup, rex::kernel::xam::NetDll_XNetQosLookup_entry)

XAM_EXPORT_STUB(__imp__NetDll_UpnpActionCalculateWorkBufferSize);
XAM_EXPORT_STUB(__imp__NetDll_UpnpActionCreate);
XAM_EXPORT_STUB(__imp__NetDll_UpnpActionGetResults);
XAM_EXPORT_STUB(__imp__NetDll_UpnpCleanup);
XAM_EXPORT_STUB(__imp__NetDll_UpnpCloseHandle);
XAM_EXPORT_STUB(__imp__NetDll_UpnpDescribeCreate);
XAM_EXPORT_STUB(__imp__NetDll_UpnpDescribeGetResults);
XAM_EXPORT_STUB(__imp__NetDll_UpnpDoWork);
XAM_EXPORT_STUB(__imp__NetDll_UpnpEventCreate);
XAM_EXPORT_STUB(__imp__NetDll_UpnpEventGetCurrentState);
XAM_EXPORT_STUB(__imp__NetDll_UpnpEventUnsubscribe);
XAM_EXPORT_STUB(__imp__NetDll_UpnpSearchCreate);
XAM_EXPORT_STUB(__imp__NetDll_UpnpSearchGetDevices);
XAM_EXPORT_STUB(__imp__NetDll_UpnpStartup);
XAM_EXPORT_STUB(__imp__NetDll_WSACancelOverlappedIO);
XAM_EXPORT_STUB(__imp__NetDll_WSAEventSelect);
XAM_EXPORT_STUB(__imp__NetDll_WSAGetOverlappedResult);
XAM_EXPORT_STUB(__imp__NetDll_WSARecv);
XAM_EXPORT_STUB(__imp__NetDll_WSASend);
XAM_EXPORT_STUB(__imp__NetDll_WSAStartupEx);
XAM_EXPORT_STUB(__imp__NetDll_XHttpCloseHandle);
XAM_EXPORT_STUB(__imp__NetDll_XHttpConnect);
XAM_EXPORT_STUB(__imp__NetDll_XHttpCrackUrl);
XAM_EXPORT_STUB(__imp__NetDll_XHttpCrackUrlW);
XAM_EXPORT_STUB(__imp__NetDll_XHttpCreateUrl);
XAM_EXPORT_STUB(__imp__NetDll_XHttpCreateUrlW);
XAM_EXPORT_STUB(__imp__NetDll_XHttpDoWork);
XAM_EXPORT_STUB(__imp__NetDll_XHttpGetPerfCounters);
XAM_EXPORT_STUB(__imp__NetDll_XHttpOpen);
XAM_EXPORT_STUB(__imp__NetDll_XHttpOpenRequest);
XAM_EXPORT_STUB(__imp__NetDll_XHttpOpenRequestUsingMemory);
XAM_EXPORT_STUB(__imp__NetDll_XHttpQueryAuthSchemes);
XAM_EXPORT_STUB(__imp__NetDll_XHttpQueryHeaders);
XAM_EXPORT_STUB(__imp__NetDll_XHttpQueryOption);
XAM_EXPORT_STUB(__imp__NetDll_XHttpReadData);
XAM_EXPORT_STUB(__imp__NetDll_XHttpReceiveResponse);
XAM_EXPORT_STUB(__imp__NetDll_XHttpResetPerfCounters);
XAM_EXPORT_STUB(__imp__NetDll_XHttpSendRequest);
XAM_EXPORT_STUB(__imp__NetDll_XHttpSetCredentials);
XAM_EXPORT_STUB(__imp__NetDll_XHttpSetOption);
XAM_EXPORT_STUB(__imp__NetDll_XHttpSetStatusCallback);
XAM_EXPORT_STUB(__imp__NetDll_XHttpShutdown);
XAM_EXPORT_STUB(__imp__NetDll_XHttpStartup);
XAM_EXPORT_STUB(__imp__NetDll_XHttpWriteData);

XAM_EXPORT_STUB(__imp__NetDll_XNetDnsReverseLookup);
XAM_EXPORT_STUB(__imp__NetDll_XNetDnsReverseRelease);
XAM_EXPORT_STUB(__imp__NetDll_XNetGetBroadcastVersionStatus);

XAM_EXPORT_STUB(__imp__NetDll_XNetGetSystemLinkPort);
XAM_EXPORT_STUB(__imp__NetDll_XNetGetXnAddrPlatform);
XAM_EXPORT_STUB(__imp__NetDll_XNetInAddrToServer);
XAM_EXPORT_STUB(__imp__NetDll_XNetQosGetListenStats);

XAM_EXPORT_STUB(__imp__NetDll_XNetReplaceKey);
XAM_EXPORT_STUB(__imp__NetDll_XNetServerToInAddr);
XAM_EXPORT_STUB(__imp__NetDll_XNetSetOpt);
XAM_EXPORT_STUB(__imp__NetDll_XNetStartupEx);
XAM_EXPORT_STUB(__imp__NetDll_XNetTsAddrToInAddr);

XAM_EXPORT_STUB(__imp__NetDll_XmlDownloadContinue);
XAM_EXPORT_STUB(__imp__NetDll_XmlDownloadGetParseTime);
XAM_EXPORT_STUB(__imp__NetDll_XmlDownloadGetReceivedDataSize);
XAM_EXPORT_STUB(__imp__NetDll_XmlDownloadStart);
XAM_EXPORT_STUB(__imp__NetDll_XmlDownloadStop);
XAM_EXPORT_STUB(__imp__NetDll_XnpCapture);
XAM_EXPORT_STUB(__imp__NetDll_XnpConfig);
XAM_EXPORT_STUB(__imp__NetDll_XnpConfigUPnP);
XAM_EXPORT_STUB(__imp__NetDll_XnpConfigUPnPPortAndExternalAddr);
XAM_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptRecv);
XAM_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptSetCallbacks);
XAM_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptSetExtendedReceiveCallback);
XAM_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptXmit);
XAM_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptXmitAsIp);
XAM_EXPORT_STUB(__imp__NetDll_XnpGetActiveSocketList);
XAM_EXPORT_STUB(__imp__NetDll_XnpGetConfigStatus);
XAM_EXPORT_STUB(__imp__NetDll_XnpGetKeyList);
XAM_EXPORT_STUB(__imp__NetDll_XnpGetQosLookupList);
XAM_EXPORT_STUB(__imp__NetDll_XnpGetSecAssocList);
XAM_EXPORT_STUB(__imp__NetDll_XnpGetVlanXboxName);
XAM_EXPORT_STUB(__imp__NetDll_XnpLoadConfigParams);
XAM_EXPORT_STUB(__imp__NetDll_XnpLoadMachineAccount);
XAM_EXPORT_STUB(__imp__NetDll_XnpLogonClearChallenge);
XAM_EXPORT_STUB(__imp__NetDll_XnpLogonClearQEvent);
XAM_EXPORT_STUB(__imp__NetDll_XnpLogonGetChallenge);
XAM_EXPORT_STUB(__imp__NetDll_XnpLogonGetQFlags);
XAM_EXPORT_STUB(__imp__NetDll_XnpLogonGetQVals);
XAM_EXPORT_STUB(__imp__NetDll_XnpLogonGetStatus);
XAM_EXPORT_STUB(__imp__NetDll_XnpLogonSetChallengeResponse);
XAM_EXPORT_STUB(__imp__NetDll_XnpLogonSetPState);
XAM_EXPORT_STUB(__imp__NetDll_XnpLogonSetQEvent);
XAM_EXPORT_STUB(__imp__NetDll_XnpLogonSetQFlags);
XAM_EXPORT_STUB(__imp__NetDll_XnpLogonSetQVals);
XAM_EXPORT_STUB(__imp__NetDll_XnpNoteSystemTime);
XAM_EXPORT_STUB(__imp__NetDll_XnpPersistTitleState);
XAM_EXPORT_STUB(__imp__NetDll_XnpQosHistoryGetAggregateMeasurement);
XAM_EXPORT_STUB(__imp__NetDll_XnpQosHistoryGetEntries);
XAM_EXPORT_STUB(__imp__NetDll_XnpQosHistoryLoad);
XAM_EXPORT_STUB(__imp__NetDll_XnpQosHistorySaveMeasurements);
XAM_EXPORT_STUB(__imp__NetDll_XnpRegisterKeyForCallerType);
XAM_EXPORT_STUB(__imp__NetDll_XnpReplaceKeyForCallerType);
XAM_EXPORT_STUB(__imp__NetDll_XnpSaveConfigParams);
XAM_EXPORT_STUB(__imp__NetDll_XnpSaveMachineAccount);
XAM_EXPORT_STUB(__imp__NetDll_XnpSetVlanXboxName);
XAM_EXPORT_STUB(__imp__NetDll_XnpToolIpProxyInject);
XAM_EXPORT_STUB(__imp__NetDll_XnpToolSetCallbacks);
XAM_EXPORT_STUB(__imp__NetDll_XnpUnregisterKeyForCallerType);
XAM_EXPORT_STUB(__imp__NetDll_XnpUpdateConfigParams);
XAM_EXPORT_STUB(__imp__NetDll_getpeername);

XAM_EXPORT_STUB(__imp__NetDll_getsockopt);
