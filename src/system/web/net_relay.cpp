/**
 * ReXGlue web port (Workstream G): virtual UDP stack over the /net WebSocket relay.
 * See rex/system/web/net_relay.h. Depends only on Emscripten and the C++ library, so the
 * browser test (tools/net_tests in the Halo 3 project) links this file alone.
 */

#include <rex/system/web/net_relay.h>

#include <emscripten/em_js.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>

#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace rex::system::web::net_relay {

namespace {

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
constexpr char kJsSource[] = {
#embed "net_relay.js"
    , 0};
#pragma clang diagnostic pop

constexpr uint32_t kRingSize = 4u << 20;  // power of two (JS masks with size - 1)
constexpr uint32_t kWrapMarker = 0xFFFFFFFFu;
constexpr size_t kMaxQueueDatagrams = 512;
constexpr size_t kMaxQueueBytes = 2u << 20;
constexpr uint16_t kEphemeralFirst = 49152;

enum Ctl : int {
  kHead = 0,
  kTail = 1,
  kSeq = 2,
  kState = 3,
  kId = 4,
  kPeers = 5,
  kRingDropped = 6,
  kRxFrames = 7,
  kTxFrames = 8,
  kCloseCode = 9,
  kTxDroppedJs = 10,
  kCtlCount = 16,
};

static_assert(sizeof(std::atomic<int32_t>) == 4 && std::atomic<int32_t>::is_always_lock_free,
              "the control block is shared with JS Int32Array Atomics");

std::atomic<int32_t>* g_ctl = nullptr;
uint8_t* g_ring = nullptr;
std::once_flag g_start_once;

struct Datagram {
  uint32_t from_addr;
  uint16_t from_port;
  std::vector<uint8_t> data;
};

struct Sock {
  int type = 2;
  int proto = 17;
  bool bound = false;
  uint32_t addr = 0;
  uint16_t port = 0;
  bool non_blocking = false;
  bool broadcast = false;
  uint32_t rcv_timeout_ms = 0;
  std::deque<Datagram> queue;
  size_t queue_bytes = 0;
};

std::mutex g_mutex;
std::unordered_map<int, Sock> g_socks;
std::unordered_map<uint16_t, int> g_ports;
int g_next_handle = 0x100;
uint16_t g_next_ephemeral = kEphemeralFirst;
Stats g_stats = {};

inline uint16_t BE16(const uint8_t* p) {
  return uint16_t((p[0] << 8) | p[1]);
}
inline uint32_t BE32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
inline void PutBE16(uint8_t* p, uint16_t v) {
  p[0] = uint8_t(v >> 8);
  p[1] = uint8_t(v);
}
inline void PutBE32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}

State LoadState() {
  return g_ctl ? State(g_ctl[kState].load(std::memory_order_acquire)) : State::kIdle;
}

uint32_t LoadId() {
  return g_ctl && LoadState() == State::kUp ? uint32_t(g_ctl[kId].load(std::memory_order_acquire))
                                            : 0;
}

void WakeWaiters() {
  if (!g_ctl) return;
  g_ctl[kSeq].fetch_add(1, std::memory_order_acq_rel);
  emscripten_futex_wake(&g_ctl[kSeq], INT_MAX);
}

// Waits until SEQ moves away from `seen` or timeout_ms elapses (< 0 = forever).
void WaitSeq(int32_t seen, double timeout_ms) {
  if (!g_ctl || emscripten_is_main_runtime_thread()) return;
  emscripten_futex_wait(&g_ctl[kSeq], uint32_t(seen), timeout_ms < 0 ? INFINITY : timeout_ms);
}

int32_t LoadSeq() {
  return g_ctl ? g_ctl[kSeq].load(std::memory_order_acquire) : 0;
}

Sock* Find(int s) {
  auto it = g_socks.find(s);
  return it == g_socks.end() ? nullptr : &it->second;
}

bool Enqueue(Sock& sock, uint32_t from_addr, uint16_t from_port, const uint8_t* data,
             uint32_t len) {
  if (sock.queue.size() >= kMaxQueueDatagrams || sock.queue_bytes + len > kMaxQueueBytes) {
    g_stats.rx_queue_full++;
    return false;
  }
  sock.queue.push_back(Datagram{from_addr, from_port, std::vector<uint8_t>(data, data + len)});
  sock.queue_bytes += len;
  return true;
}

// Mutex held. One relay frame: u32 src, u32 dst, u16 port | u16 src_port, u8 proto, u8 ver | data.
void Deliver(const uint8_t* frame, uint32_t n) {
  if (n < kRelayHeaderSize + kShimHeaderSize || frame[13] != kShimVersion) {
    g_stats.rx_malformed++;
    return;
  }
  uint32_t src = BE32(frame + 0);
  uint16_t dst_port = BE16(frame + 8);
  uint16_t src_port = BE16(frame + 10);
  const uint8_t* data = frame + kRelayHeaderSize + kShimHeaderSize;
  uint32_t len = n - kRelayHeaderSize - kShimHeaderSize;
  auto it = g_ports.find(dst_port);
  Sock* sock = it == g_ports.end() ? nullptr : Find(it->second);
  if (!sock || sock->type != 2) {
    g_stats.rx_no_socket++;
    return;
  }
  if (Enqueue(*sock, kSubnet | (src & 0xFFu), src_port, data, len)) {
    g_stats.rx_datagrams++;
    g_stats.rx_bytes += len;
  }
}

// Mutex held: moves every complete record from the JS ring into the socket queues.
void Pump() {
  if (!g_ctl) return;
  uint32_t head = uint32_t(g_ctl[kHead].load(std::memory_order_acquire));
  uint32_t tail = uint32_t(g_ctl[kTail].load(std::memory_order_relaxed));
  if (head == tail) return;
  while (tail != head) {
    uint32_t off = tail & (kRingSize - 1);
    uint32_t len;
    std::memcpy(&len, g_ring + off, 4);  // little-endian host, written by DataView(..., true)
    if (len == kWrapMarker) {
      tail += kRingSize - off;
      continue;
    }
    Deliver(g_ring + off + 4, len);
    tail += (4 + len + 3) & ~3u;
  }
  g_ctl[kTail].store(int32_t(tail), std::memory_order_release);
}

bool BindLocked(Sock& sock, int handle, uint32_t addr, uint16_t port, int* err) {
  if (sock.bound) {
    *err = kWsaEInval;
    return false;
  }
  if (port == 0) {
    for (uint32_t tries = 0; tries < 16384; ++tries) {
      uint16_t p = g_next_ephemeral++;
      if (g_next_ephemeral == 0) g_next_ephemeral = kEphemeralFirst;
      if (!g_ports.count(p)) {
        port = p;
        break;
      }
    }
    if (port == 0) {
      *err = kWsaEAddrInUse;
      return false;
    }
  } else if (g_ports.count(port)) {
    *err = kWsaEAddrInUse;
    return false;
  }
  sock.bound = true;
  sock.addr = addr;
  sock.port = port;
  g_ports[port] = handle;
  return true;
}

using Clock = std::chrono::steady_clock;

double RemainingMs(Clock::time_point deadline) {
  auto left = std::chrono::duration<double, std::milli>(deadline - Clock::now()).count();
  return left > 0 ? left : 0;
}

}  // namespace

// --- browser main thread -------------------------------------------------------------

// Pointers as doubles: exact below 2^53, no wasm64 BigInt question (as in web_audio_output).
EM_JS(void, rex_net_js_start,
      (double ctl_ptr, double ring_ptr, int ring_size, double src_ptr, int src_len,
       double url_ptr, int url_len, double room_ptr, int room_len),
      {
        const log = (m) => (Module['print'] || console.log)(m);
        const str = (p, n) => new TextDecoder().decode(HEAPU8.slice(p, p + n));
        try {
          const src = str(src_ptr, src_len);
          const factory = (0, eval)(src);
          const net = factory({
            log,
            heap : () => HEAPU8,
            ctlPtr : ctl_ptr,
            ringPtr : ring_ptr,
            ringSize : ring_size
          });
          Module['rexNet'] = net;
          net.connect(str(url_ptr, url_len), str(room_ptr, room_len));
        } catch (e) {
          log('[net] relay setup failed: ' + e);
          const ctl = new Int32Array(HEAPU8.buffer, ctl_ptr, 16);
          Atomics.store(ctl, 3, 3);
          Atomics.add(ctl, 2, 1);
          Atomics.notify(ctl, 2);
        }
      });

EM_JS(void, rex_net_js_send, (double ptr, int len), {
  const net = Module['rexNet'];
  if (net) net.send(ptr, len);
});

namespace {

struct StartArgs {
  std::string url;
  std::string room;
};

void StartOnMainThread(void* p) {
  auto* a = static_cast<StartArgs*>(p);
  rex_net_js_start(double(uintptr_t(g_ctl)), double(uintptr_t(g_ring)), int(kRingSize),
                   double(uintptr_t(kJsSource)), int(sizeof(kJsSource) - 1),
                   double(uintptr_t(a->url.data())), int(a->url.size()),
                   double(uintptr_t(a->room.data())), int(a->room.size()));
  delete a;
}

// p: u32 frame length, then the frame. Freed here.
void SendOnMainThread(void* p) {
  uint32_t len;
  std::memcpy(&len, p, 4);
  rex_net_js_send(double(uintptr_t(p) + 4), int(len));
  std::free(p);
}

void RunOnMainThread(void (*fn)(void*), void* arg) {
  if (emscripten_is_main_runtime_thread()) {
    fn(arg);
    return;
  }
  emscripten_proxy_async(emscripten_proxy_get_system_queue(), emscripten_main_runtime_thread_id(),
                         fn, arg);
}

}  // namespace

// --- public API -------------------------------------------------------------------------

void Start(const char* url, const char* room) {
  std::call_once(g_start_once, [&] {
    void* block = std::aligned_alloc(16, kCtlCount * 4 + kRingSize);
    if (!block) return;
    std::memset(block, 0, kCtlCount * 4);
    g_ctl = reinterpret_cast<std::atomic<int32_t>*>(block);
    g_ring = static_cast<uint8_t*>(block) + kCtlCount * 4;
    g_ctl[kState].store(int32_t(State::kConnecting), std::memory_order_release);
    RunOnMainThread(StartOnMainThread, new StartArgs{url ? url : "", room ? room : ""});
  });
}

bool WaitUntilUp(uint32_t timeout_ms) {
  auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
  for (;;) {
    int32_t seq = LoadSeq();
    State st = LoadState();
    if (st == State::kUp) return true;
    if (st != State::kConnecting) return false;
    double left = RemainingMs(deadline);
    if (left <= 0 || emscripten_is_main_runtime_thread()) return false;
    WaitSeq(seq, left);
  }
}

State GetState() {
  return LoadState();
}

uint32_t PeerId() {
  return LoadId();
}

uint32_t LocalAddress() {
  uint32_t id = LoadId();
  return id ? (kSubnet | (id & 0xFFu)) : 0;
}

uint32_t PeerCount() {
  return g_ctl ? uint32_t(g_ctl[kPeers].load(std::memory_order_acquire)) : 0;
}

int Socket(int af, int type, int protocol, int* err) {
  if (af != 2) {
    *err = kWsaEAfNoSupport;
    return -1;
  }
  if (type != 1 && type != 2) {
    *err = kWsaEProtoNoSupport;
    return -1;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_socks.size() >= 256) {
    *err = kWsaEMFile;
    return -1;
  }
  int h = g_next_handle++;
  Sock& s = g_socks[h];
  s.type = type;
  s.proto = protocol ? protocol : (type == 2 ? 17 : 6);
  *err = 0;
  return h;
}

int Close(int s, int* err) {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    Sock* sock = Find(s);
    if (!sock) {
      *err = kWsaENotSock;
      return -1;
    }
    if (sock->bound) {
      auto it = g_ports.find(sock->port);
      if (it != g_ports.end() && it->second == s) g_ports.erase(it);
    }
    g_socks.erase(s);
  }
  WakeWaiters();  // blocked recvfrom/select on this handle re-check and fail
  *err = 0;
  return 0;
}

bool IsSocket(int s) {
  std::lock_guard<std::mutex> lock(g_mutex);
  return Find(s) != nullptr;
}

bool IsStream(int s) {
  std::lock_guard<std::mutex> lock(g_mutex);
  Sock* sock = Find(s);
  return sock && sock->type == 1;
}

int Bind(int s, uint32_t addr, uint16_t port, int* err) {
  std::lock_guard<std::mutex> lock(g_mutex);
  Sock* sock = Find(s);
  if (!sock) {
    *err = kWsaENotSock;
    return -1;
  }
  if (addr != 0 && (addr >> 24) != 127 && (addr & kSubnetMask) != kSubnet) {
    *err = kWsaEAddrNotAvail;
    return -1;
  }
  if (!BindLocked(*sock, s, addr, port, err)) return -1;
  *err = 0;
  return 0;
}

int GetSockName(int s, uint32_t* addr, uint16_t* port, int* err) {
  std::lock_guard<std::mutex> lock(g_mutex);
  Sock* sock = Find(s);
  if (!sock) {
    *err = kWsaENotSock;
    return -1;
  }
  if (!sock->bound) {
    *err = kWsaEInval;
    return -1;
  }
  *addr = sock->addr;
  *port = sock->port;
  *err = 0;
  return 0;
}

int SetNonBlocking(int s, bool non_blocking, int* err) {
  std::lock_guard<std::mutex> lock(g_mutex);
  Sock* sock = Find(s);
  if (!sock) {
    *err = kWsaENotSock;
    return -1;
  }
  sock->non_blocking = non_blocking;
  *err = 0;
  return 0;
}

int SetBroadcast(int s, bool on, int* err) {
  std::lock_guard<std::mutex> lock(g_mutex);
  Sock* sock = Find(s);
  if (!sock) {
    *err = kWsaENotSock;
    return -1;
  }
  sock->broadcast = on;
  *err = 0;
  return 0;
}

int SetReceiveTimeout(int s, uint32_t timeout_ms, int* err) {
  std::lock_guard<std::mutex> lock(g_mutex);
  Sock* sock = Find(s);
  if (!sock) {
    *err = kWsaENotSock;
    return -1;
  }
  sock->rcv_timeout_ms = timeout_ms;
  *err = 0;
  return 0;
}

int SendTo(int s, const uint8_t* data, uint32_t len, uint32_t addr, uint16_t port, int* err) {
  void* out = nullptr;
  bool wake = false;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    Sock* sock = Find(s);
    if (!sock) {
      *err = kWsaENotSock;
      return -1;
    }
    if (sock->type != 2) {
      *err = kWsaENotConn;
      return -1;
    }
    if (len > kMaxDatagram) {
      *err = kWsaEMsgSize;
      return -1;
    }
    if (!sock->bound && !BindLocked(*sock, s, 0, 0, err)) return -1;
    uint32_t local = LocalAddress();
    bool broadcast = addr == kLimitedBroadcast || addr == kSubnetBroadcast;
    if (!broadcast && ((addr >> 24) == 127 || (local && addr == local))) {
      // Loopback: straight into the local socket bound to `port`.
      g_stats.loopback++;
      auto it = g_ports.find(port);
      Sock* dst = it == g_ports.end() ? nullptr : Find(it->second);
      if (dst && dst->type == 2) {
        if (Enqueue(*dst, (addr >> 24) == 127 ? addr : local, sock->port, data, len)) {
          g_stats.rx_datagrams++;
          g_stats.rx_bytes += len;
        }
        wake = true;
      } else {
        g_stats.rx_no_socket++;
      }
    } else if (LoadState() != State::kUp ||
               (!broadcast && ((addr & kSubnetMask) != kSubnet || (addr & 0xFFu) == 0))) {
      g_stats.tx_dropped++;  // UDP: unroutable datagrams vanish silently
    } else {
      uint32_t frame_len = kRelayHeaderSize + kShimHeaderSize + len;
      auto* p = static_cast<uint8_t*>(std::malloc(4 + frame_len));
      if (!p) {
        g_stats.tx_dropped++;
      } else {
        std::memcpy(p, &frame_len, 4);
        uint8_t* f = p + 4;
        PutBE32(f + 0, LoadId());
        PutBE32(f + 4, broadcast ? kRelayBroadcastId : (addr & 0xFFu));
        PutBE16(f + 8, port);
        PutBE16(f + 10, sock->port);
        f[12] = uint8_t(sock->proto);
        f[13] = kShimVersion;
        if (len) std::memcpy(f + kRelayHeaderSize + kShimHeaderSize, data, len);
        out = p;
        g_stats.tx_datagrams++;
        g_stats.tx_bytes += len;
        if (broadcast) g_stats.tx_broadcast++;
      }
    }
  }
  if (out) RunOnMainThread(SendOnMainThread, out);
  if (wake) WakeWaiters();
  *err = 0;
  return int(len);
}

int RecvFrom(int s, uint8_t* buf, uint32_t len, uint32_t* from_addr, uint16_t* from_port,
             int* err) {
  Clock::time_point deadline{};
  bool have_deadline = false;
  for (;;) {
    int32_t seq = LoadSeq();
    double wait_ms = -1;
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      Pump();
      Sock* sock = Find(s);
      if (!sock) {
        *err = kWsaENotSock;
        return -1;
      }
      if (sock->type != 2) {
        *err = kWsaENotConn;
        return -1;
      }
      if (!sock->bound) {
        *err = kWsaEInval;
        return -1;
      }
      if (!sock->queue.empty()) {
        Datagram d = std::move(sock->queue.front());
        sock->queue.pop_front();
        sock->queue_bytes -= d.data.size();
        uint32_t n = uint32_t(d.data.size());
        uint32_t copy = n < len ? n : len;
        if (copy) std::memcpy(buf, d.data.data(), copy);
        if (from_addr) *from_addr = d.from_addr;
        if (from_port) *from_port = d.from_port;
        if (n > len) {
          *err = kWsaEMsgSize;
          return -1;
        }
        *err = 0;
        return int(n);
      }
      if (sock->non_blocking) {
        *err = kWsaEWouldBlock;
        return -1;
      }
      if (sock->rcv_timeout_ms) {
        if (!have_deadline) {
          deadline = Clock::now() + std::chrono::milliseconds(sock->rcv_timeout_ms);
          have_deadline = true;
        }
        wait_ms = RemainingMs(deadline);
        if (wait_ms <= 0) {
          *err = 10060;  // WSAETIMEDOUT
          return -1;
        }
      }
    }
    if (emscripten_is_main_runtime_thread()) {
      *err = kWsaEWouldBlock;
      return -1;
    }
    WaitSeq(seq, wait_ms);
  }
}

int PendingBytes(int s, uint32_t* bytes, int* err) {
  std::lock_guard<std::mutex> lock(g_mutex);
  Pump();
  Sock* sock = Find(s);
  if (!sock) {
    *err = kWsaENotSock;
    return -1;
  }
  *bytes = sock->queue.empty() ? 0 : uint32_t(sock->queue.front().data.size());
  *err = 0;
  return 0;
}

int Select(const int* read_socks, uint32_t nread, bool* read_ready, const int* write_socks,
           uint32_t nwrite, bool* write_ready, int64_t timeout_us, int* err) {
  auto deadline = Clock::now() + std::chrono::microseconds(timeout_us > 0 ? timeout_us : 0);
  for (;;) {
    int32_t seq = LoadSeq();
    int ready = 0;
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      Pump();
      for (uint32_t i = 0; i < nread; ++i) {
        Sock* sock = Find(read_socks[i]);
        if (!sock) {
          *err = kWsaENotSock;
          return -1;
        }
        read_ready[i] = !sock->queue.empty();
        ready += read_ready[i];
      }
      for (uint32_t i = 0; i < nwrite; ++i) {
        Sock* sock = Find(write_socks[i]);
        if (!sock) {
          *err = kWsaENotSock;
          return -1;
        }
        write_ready[i] = sock->type == 2;  // datagram sockets are always writable
        ready += write_ready[i];
      }
    }
    *err = 0;
    if (ready || timeout_us == 0 || emscripten_is_main_runtime_thread()) return ready;
    double wait_ms = -1;
    if (timeout_us > 0) {
      wait_ms = RemainingMs(deadline);
      if (wait_ms <= 0) return 0;
    }
    WaitSeq(seq, wait_ms);
  }
}

Stats GetStats() {
  std::lock_guard<std::mutex> lock(g_mutex);
  Stats s = g_stats;
  s.sockets_open = uint32_t(g_socks.size());
  s.peers = PeerCount();
  s.state = int32_t(LoadState());
  s.id = LoadId();
  s.rx_ring_dropped = g_ctl ? uint32_t(g_ctl[kRingDropped].load(std::memory_order_relaxed)) : 0;
  s.tx_dropped += g_ctl ? uint32_t(g_ctl[kTxDroppedJs].load(std::memory_order_relaxed)) : 0;
  return s;
}

}  // namespace rex::system::web::net_relay
