/**
 * @file        rex/kernel/xam/xnet.h
 * @brief       XNet / XSession model shared by the XAM network exports (xam_net.cpp) and the
 *              XGI app (XSessionCreate, apps/xgi_app.cpp).
 *
 * What a System Link title needs from XNet, without Xbox Live:
 *   - a title XNADDR whose `ina` is a routable host address (web: the relay's 10.80.0.<id>),
 *   - "secure" IN_ADDRs for peers: XNetXnAddrToInAddr hands the title an opaque address in
 *     0.0.0.0/8 (the host address with its first byte cleared, as on a console the title only
 *     ever sees 0.x.y.z for secure peers), and the socket exports translate it back,
 *   - the XNKID / XNADDR each peer was registered with (XNetInAddrToXnAddr returns them),
 *   - random session ids, keys and nonces that differ between consoles on one LAN,
 *   - a filled XSESSION_INFO for a hosted session.
 *
 * Everything inline here is header-only and platform independent (no kernel state), so it is
 * unit tested natively (tests/unit/kernel/xnet_test.cpp) and under wasm64
 * (tests/web/xnet_web_test.cpp). The functions declared at the end are implemented in
 * src/kernel/xam/xam_net.cpp.
 *
 * Guest layouts (xnet.h / xonline.h, big-endian):
 *   XNADDR        36 bytes: IN_ADDR ina, IN_ADDR inaOnline, u16 wPortOnline, u8 abEnet[6],
 *                 u8 abOnline[20]
 *   XNKID 8 bytes (first byte: kind in its top bits), XNKEY 16 bytes
 *   XSESSION_INFO 60 bytes: XNKID sessionID, XNADDR hostAddress, XNKEY keyExchangeKey
 *   XNQOS         u32 cxnqos, u32 cxnqosPending, XNQOSINFO[n] (24 bytes each)
 *
 * @license     BSD 3-Clause License
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <random>

namespace rex::kernel::xam::xnet {

// --- sizes -------------------------------------------------------------------------------
constexpr uint32_t kXnAddrSize = 36;
constexpr uint32_t kXnKidSize = 8;
constexpr uint32_t kXnKeySize = 16;
constexpr uint32_t kSessionInfoSize = kXnKidSize + kXnAddrSize + kXnKeySize;  // 60
constexpr uint32_t kSessionCreateArgsSize = 28;
constexpr uint32_t kXnQosHeaderSize = 8;
constexpr uint32_t kXnQosInfoSize = 24;

// --- XNetGetTitleXnAddr result flags (XNET_GET_XNADDR_*) ---------------------------------
constexpr uint32_t kXnAddrPending = 0x00;
constexpr uint32_t kXnAddrNone = 0x01;
constexpr uint32_t kXnAddrEthernet = 0x02;
constexpr uint32_t kXnAddrStatic = 0x04;
constexpr uint32_t kXnAddrDhcp = 0x08;

// --- XNetGetEthernetLinkStatus (XNET_ETHERNET_LINK_*) ------------------------------------
constexpr uint32_t kLinkActive = 0x01;
constexpr uint32_t kLink100Mbps = 0x02;
constexpr uint32_t kLink10Mbps = 0x04;
constexpr uint32_t kLinkFullDuplex = 0x08;
constexpr uint32_t kLinkHalfDuplex = 0x10;
constexpr uint32_t kLinkUp = kLinkActive | kLink100Mbps | kLinkFullDuplex;

// --- XNetGetConnectStatus (XNET_CONNECT_STATUS_*) ----------------------------------------
constexpr uint32_t kConnectIdle = 0;
constexpr uint32_t kConnectPending = 1;
constexpr uint32_t kConnectConnected = 2;
constexpr uint32_t kConnectLost = 3;

// --- XNKID kind: top bits of byte 0 (XNET_XNKID_*) ---------------------------------------
constexpr uint8_t kXnKidSystemLink = 0x00;
constexpr uint8_t kXnKidOnlinePeer = 0x80;

// --- XSESSION_CREATE_* -------------------------------------------------------------------
constexpr uint32_t kSessionHost = 0x00000001;
constexpr uint32_t kSessionUsesPresence = 0x00000002;
constexpr uint32_t kSessionUsesStats = 0x00000004;
constexpr uint32_t kSessionUsesMatchmaking = 0x00000008;
constexpr uint32_t kSessionUsesArbitration = 0x00000010;
constexpr uint32_t kSessionUsesPeerNetwork = 0x00000020;
// Features only Xbox Live provides; an offline console refuses a session that asks for them.
constexpr uint32_t kSessionLiveFeatures = kSessionUsesPresence | kSessionUsesStats |
                                          kSessionUsesMatchmaking | kSessionUsesArbitration;

constexpr uint32_t kXOnlineESessionNotLoggedOn = 0x80155209u;  // X_ONLINE_E_SESSION_NOT_LOGGED_ON

// --- big-endian guest memory -------------------------------------------------------------
inline uint16_t LoadBE16(const uint8_t* p) {
  return uint16_t((uint16_t(p[0]) << 8) | p[1]);
}
inline uint32_t LoadBE32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
inline void StoreBE16(uint8_t* p, uint16_t v) {
  p[0] = uint8_t(v >> 8);
  p[1] = uint8_t(v);
}
inline void StoreBE32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}
inline void StoreBE64(uint8_t* p, uint64_t v) {
  StoreBE32(p, uint32_t(v >> 32));
  StoreBE32(p + 4, uint32_t(v));
}

inline bool AllZero(const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    if (p[i]) return false;
  }
  return true;
}

// --- XNADDR ------------------------------------------------------------------------------

// Deterministic, locally administered MAC for a host address (02:52:a:b:c:d); the same bytes
// as rex/system/web/net_relay_guest.h VirtualMac, never all zero (RakNet-style titles derive
// ids from abEnet and fail on zero).
inline void VirtualMac(uint32_t ina, uint8_t mac[6]) {
  mac[0] = 0x02;
  mac[1] = 0x52;  // 'R'
  mac[2] = uint8_t(ina >> 24);
  mac[3] = uint8_t(ina >> 16);
  mac[4] = uint8_t(ina >> 8);
  mac[5] = uint8_t(ina);
}

// XNADDR of a console on the LAN: ina = host address (host order in, big-endian out), no
// online address, MAC from the address.
inline void StoreXnAddr(uint8_t* p, uint32_t ina) {
  std::memset(p, 0, kXnAddrSize);
  StoreBE32(p + 0, ina);
  VirtualMac(ina, p + 10);
}
inline uint32_t XnAddrIna(const uint8_t* p) {
  return LoadBE32(p + 0);
}

// --- secure IN_ADDRs ---------------------------------------------------------------------

inline bool IsSecureAddr(uint32_t a) {
  return a != 0 && (a >> 24) == 0;
}
// Secure address of a host address: the first byte cleared (10.80.0.3 -> 0.80.0.3).
inline uint32_t ToSecure(uint32_t ina) {
  return (ina >> 24) ? (ina & 0x00FFFFFFu) : ina;
}

// The peers this console knows: secure address -> host address, plus the XNADDR and XNKID
// the title registered them with. Thread-safe.
class AddressBook {
 public:
  // This console's host address and LAN mask (host order). Addresses in that subnet,
  // loopback and the address itself are LAN peers.
  void SetLocal(uint32_t ina, uint32_t mask = 0xFFFFFF00u) {
    std::lock_guard<std::mutex> lock(mutex_);
    local_ = ina;
    mask_ = mask;
  }
  uint32_t local() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return local_;
  }

  // XNetXnAddrToInAddr: the secure address for a peer's XNADDR; remembers the XNADDR and,
  // if given, the XNKID. Returns 0 for an XNADDR without an address.
  uint32_t Register(const uint8_t* xnaddr, const uint8_t* xnkid) {
    uint32_t ina = XnAddrIna(xnaddr);
    if (ina == 0) return 0;
    uint32_t secure = ToSecure(ina);
    std::lock_guard<std::mutex> lock(mutex_);
    Peer& p = peers_[secure];
    p.ina = IsSecureAddr(ina) ? (p.ina ? p.ina : ina) : ina;
    std::memcpy(p.xnaddr, xnaddr, kXnAddrSize);
    p.has_xnaddr = true;
    if (xnkid) {
      std::memcpy(p.xnkid, xnkid, kXnKidSize);
      p.has_xnkid = true;
    }
    return secure;
  }

  // XNetInAddrToXnAddr: what the peer was registered with. False if unknown.
  bool Lookup(uint32_t a, uint8_t* xnaddr_out, uint8_t* xnkid_out, bool* has_xnkid) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = peers_.find(ToSecure(a));
    if (it == peers_.end() || !it->second.has_xnaddr) return false;
    if (xnaddr_out) std::memcpy(xnaddr_out, it->second.xnaddr, kXnAddrSize);
    if (xnkid_out && it->second.has_xnkid) std::memcpy(xnkid_out, it->second.xnkid, kXnKidSize);
    if (has_xnkid) *has_xnkid = it->second.has_xnkid;
    return true;
  }

  // XNetUnregisterInAddr.
  bool Unregister(uint32_t a) {
    std::lock_guard<std::mutex> lock(mutex_);
    return peers_.erase(ToSecure(a)) != 0;
  }

  bool IsLanPeer(uint32_t ina) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return IsLanPeerLocked(ina);
  }

  // A received datagram's source address as the title sees it: LAN peers by their secure
  // address (so it matches what XNetXnAddrToInAddr gave the title), anything else as is.
  uint32_t SourceToTitle(uint32_t ina) {
    if (ina == 0 || IsSecureAddr(ina)) return ina;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!IsLanPeerLocked(ina)) return ina;
    uint32_t secure = ToSecure(ina);
    Peer& p = peers_[secure];
    p.ina = ina;
    return secure;
  }

  // Destination address the title gave a socket call -> host address. Secure addresses map
  // back through the book, else onto the local address's first byte; others pass through.
  uint32_t TitleToHost(uint32_t a) const {
    if (!IsSecureAddr(a)) return a;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = peers_.find(a);
    if (it != peers_.end() && it->second.ina && !IsSecureAddr(it->second.ina)) return it->second.ina;
    if (local_ >> 24) return (local_ & 0xFF000000u) | a;
    return a;
  }

  size_t size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return peers_.size();
  }

 private:
  struct Peer {
    uint32_t ina = 0;
    bool has_xnaddr = false;
    bool has_xnkid = false;
    uint8_t xnaddr[kXnAddrSize] = {};
    uint8_t xnkid[kXnKidSize] = {};
  };
  bool IsLanPeerLocked(uint32_t ina) const {
    if ((ina >> 24) == 127) return true;
    if (!local_) return false;
    return ina == local_ || (ina & mask_) == (local_ & mask_);
  }

  mutable std::mutex mutex_;
  uint32_t local_ = 0;
  uint32_t mask_ = 0xFFFFFF00u;
  std::map<uint32_t, Peer> peers_;
};

// --- keys --------------------------------------------------------------------------------

// XNetRegisterKey / XNetUnregisterKey bookkeeping. Thread-safe.
class KeyRegistry {
 public:
  // Returns true if the key id was new (re-registering replaces the key).
  bool Register(const uint8_t* xnkid, const uint8_t* xnkey) {
    std::lock_guard<std::mutex> lock(mutex_);
    Kid kid;
    std::memcpy(kid.b, xnkid, kXnKidSize);
    Key key{};
    if (xnkey) std::memcpy(key.b, xnkey, kXnKeySize);
    bool fresh = keys_.find(kid) == keys_.end();
    keys_[kid] = key;
    last_ = kid;
    has_last_ = true;
    return fresh;
  }
  bool Unregister(const uint8_t* xnkid) {
    std::lock_guard<std::mutex> lock(mutex_);
    Kid kid;
    std::memcpy(kid.b, xnkid, kXnKidSize);
    return keys_.erase(kid) != 0;
  }
  bool IsRegistered(const uint8_t* xnkid) const {
    std::lock_guard<std::mutex> lock(mutex_);
    Kid kid;
    std::memcpy(kid.b, xnkid, kXnKidSize);
    return keys_.find(kid) != keys_.end();
  }
  // The most recently registered key id (also after it was unregistered).
  bool Last(uint8_t* xnkid_out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!has_last_) return false;
    std::memcpy(xnkid_out, last_.b, kXnKidSize);
    return true;
  }
  size_t size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return keys_.size();
  }

 private:
  struct Kid {
    uint8_t b[kXnKidSize] = {};
    bool operator<(const Kid& o) const { return std::memcmp(b, o.b, kXnKidSize) < 0; }
  };
  struct Key {
    uint8_t b[kXnKeySize] = {};
  };
  mutable std::mutex mutex_;
  std::map<Kid, Key> keys_;
  Kid last_;
  bool has_last_ = false;
};

// --- randomness --------------------------------------------------------------------------

// Thread-safe byte source. Deterministic mode fills 0xBB (the SDK's old XNetRandom), which
// makes runs reproducible but gives every console the same ids.
class Random {
 public:
  Random() : rng_(std::random_device{}()) {}
  explicit Random(uint64_t seed) : rng_(seed) {}
  void Fill(uint8_t* p, size_t n, bool deterministic = false) {
    if (deterministic) {
      std::memset(p, 0xBB, n);
      return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i < n; ++i) p[i] = uint8_t(rng_());
  }

 private:
  std::mutex mutex_;
  std::mt19937_64 rng_;
};

// XNKID of a key / session this console creates: random, `kind` in the top nibble of byte 0
// (system link: clear), never all zero (titles read an all-zero id as "no session").
inline void MakeXnKid(uint8_t* kid, Random& rng, uint8_t kind = kXnKidSystemLink,
                      bool deterministic = false) {
  rng.Fill(kid, kXnKidSize, deterministic);
  kid[0] = uint8_t((kid[0] & 0x0F) | (kind & 0xF0));
  if (AllZero(kid, kXnKidSize)) kid[kXnKidSize - 1] = 1;
}
inline void MakeXnKey(uint8_t* key, Random& rng, bool deterministic = false) {
  rng.Fill(key, kXnKeySize, deterministic);
}
inline uint64_t MakeNonce(Random& rng, bool deterministic = false) {
  uint8_t b[8];
  rng.Fill(b, 8, deterministic);
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v = (v << 8) | b[i];
  return v ? v : 1;
}

// --- XSessionCreate (XGI 0x000B0010) -----------------------------------------------------

// The 28-byte message buffer: session handle, flags, public slots, private slots, user,
// XSESSION_INFO* (out for a host, in for a joining client), u64* nonce.
struct SessionCreateArgs {
  uint32_t session = 0;
  uint32_t flags = 0;
  uint32_t public_slots = 0;
  uint32_t private_slots = 0;
  uint32_t user = 0;
  uint32_t session_info_ptr = 0;
  uint32_t nonce_ptr = 0;
};
inline bool ParseSessionCreateArgs(const uint8_t* buf, uint32_t len, SessionCreateArgs* out) {
  if (!buf || (len != 0 && len < kSessionCreateArgsSize)) return false;
  out->session = LoadBE32(buf + 0x00);
  out->flags = LoadBE32(buf + 0x04);
  out->public_slots = LoadBE32(buf + 0x08);
  out->private_slots = LoadBE32(buf + 0x0C);
  out->user = LoadBE32(buf + 0x10);
  out->session_info_ptr = LoadBE32(buf + 0x14);
  out->nonce_ptr = LoadBE32(buf + 0x18);
  return true;
}

enum class SessionCreateAction {
  kRefuseLive,  // asks for Live features without Live: X_ONLINE_E_SESSION_NOT_LOGGED_ON
  kHost,        // this console hosts: fill XSESSION_INFO and the nonce
  kJoin,        // joining a session the title found: keep what it passed in
};
inline SessionCreateAction DecideSessionCreate(uint32_t flags, bool live_available) {
  if ((flags & kSessionLiveFeatures) && !live_available) return SessionCreateAction::kRefuseLive;
  return (flags & kSessionHost) ? SessionCreateAction::kHost : SessionCreateAction::kJoin;
}

// XSESSION_INFO of a session this console hosts: a new system-link (or Live) session id, the
// host's XNADDR and a new key-exchange key.
inline void FillHostSessionInfo(uint8_t* info, const uint8_t* host_xnaddr, Random& rng,
                                bool live = false, bool deterministic = false) {
  MakeXnKid(info, rng, live ? kXnKidOnlinePeer : kXnKidSystemLink, deterministic);
  std::memcpy(info + kXnKidSize, host_xnaddr, kXnAddrSize);
  MakeXnKey(info + kXnKidSize + kXnAddrSize, rng, deterministic);
}

// Applies XSessionCreate to the guest's XSESSION_INFO / nonce (either may be null). A host
// gets new values; a joining client keeps what the title passed in, except an all-zero
// session id / nonce (the title passed nothing), which is filled like a host's.
// Returns false (and writes nothing) for kRefuseLive.
inline bool ApplySessionCreate(SessionCreateAction action, uint8_t* info, uint8_t* nonce,
                               const uint8_t* host_xnaddr, Random& rng,
                               bool deterministic = false) {
  if (action == SessionCreateAction::kRefuseLive) return false;
  bool host = action == SessionCreateAction::kHost;
  if (info && (host || AllZero(info, kXnKidSize))) {
    FillHostSessionInfo(info, host_xnaddr, rng, false, deterministic);
  }
  if (nonce && (host || AllZero(nonce, 8))) {
    StoreBE64(nonce, MakeNonce(rng, deterministic));
  }
  return true;
}

// --- QoS ---------------------------------------------------------------------------------

inline uint32_t QosBlockSize(uint32_t targets) {
  return kXnQosHeaderSize + kXnQosInfoSize * (targets ? targets : 1);
}
// XNQOS answering every target at once as contacted over a LAN: `probes` probes sent and
// received, 1 ms RTT, 100 Mbit/s up and down, no listen data. `q` holds QosBlockSize(n).
inline void FillQosAllContacted(uint8_t* q, uint32_t targets, uint32_t probes) {
  constexpr uint8_t kComplete = 0x01, kContacted = 0x02;
  std::memset(q, 0, QosBlockSize(targets));
  StoreBE32(q + 0, targets);
  StoreBE32(q + 4, 0);
  uint16_t p = uint16_t(probes == 0 ? 1 : (probes > 0xFFFF ? 0xFFFF : probes));
  for (uint32_t i = 0; i < targets; ++i) {
    uint8_t* e = q + kXnQosHeaderSize + kXnQosInfoSize * i;
    e[0] = kComplete | kContacted;
    StoreBE16(e + 2, p);
    StoreBE16(e + 4, p);
    StoreBE16(e + 12, 1);
    StoreBE16(e + 14, 1);
    StoreBE32(e + 16, 100000000u);
    StoreBE32(e + 20, 100000000u);
  }
}

// --- XUIDs -------------------------------------------------------------------------------

// Every tab / instance has the SDK's one default profile; consoles in one System Link
// session must not share a player XUID. Console 1 keeps the XUID (single-instance saves are
// unchanged); console N >= 2 gets the low 16 bits XORed with N.
inline uint64_t ConsoleXuid(uint64_t xuid, uint32_t console_id) {
  return console_id >= 2 ? (xuid ^ uint64_t(console_id & 0xFFFFu)) : xuid;
}

// --- implemented in src/kernel/xam/xam_net.cpp (need kernel state) -----------------------

// Fills the title XNADDR (36 bytes) and returns the XNET_GET_XNADDR_* flags.
uint32_t GetTitleXnAddr(uint8_t* xnaddr_out);
// The process-wide byte source; RandomBytes honours --xnet_deterministic_random (pass
// DeterministicRandom() to the Make* helpers when using GlobalRandom() directly).
Random& GlobalRandom();
void RandomBytes(uint8_t* p, size_t n);
bool DeterministicRandom();
// The console number on the virtual LAN for --xnet_unique_xuid (web: relay peer id; 0 when
// off or unknown).
uint32_t ConsoleIdForXuid();

}  // namespace rex::kernel::xam::xnet
