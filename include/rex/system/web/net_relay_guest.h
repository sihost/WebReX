/**
 * @file        rex/system/web/net_relay_guest.h
 * @brief       Guest-ABI (Xbox 360, big-endian, 32-bit) Winsock/XNet structure helpers for
 *              the net relay shim. Header-only and platform independent, so the byte-order
 *              handling can be unit tested anywhere.
 *
 * Layouts (Xbox 360 winsock.h / xnet.h):
 *   sockaddr_in  16 bytes: u16 family, u16 port (network order), u32 addr (network order),
 *                8 zero bytes. Big-endian guest memory = network order, so both are read as
 *                plain big-endian values.
 *   fd_set       u32 fd_count, u32 fd_array[64]   (FD_SETSIZE 64)
 *   timeval      i32 tv_sec, i32 tv_usec
 *   XNADDR       36 bytes: IN_ADDR ina, IN_ADDR inaOnline, u16 wPortOnline, u8 abEnet[6],
 *                u8 abOnline[20]
 *   XNKID 8 bytes, XNKEY 16 bytes.
 *
 * @license     BSD 3-Clause License
 */

#pragma once

#include <cstdint>
#include <cstring>

namespace rex::system::web::net_relay::guest {

constexpr uint32_t kSockAddrInSize = 16;
constexpr uint32_t kFdSetSize = 64;
constexpr uint32_t kFdSetBytes = 4 + 4 * kFdSetSize;
constexpr uint32_t kXnAddrSize = 36;
constexpr uint16_t kAfInet = 2;

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

struct SockAddrIn {
  uint16_t family;
  uint16_t port;  // host order
  uint32_t addr;  // host order (10.80.0.1 == 0x0A500001)
};

// Returns false if the buffer is too short for the address fields.
inline bool LoadSockAddrIn(const uint8_t* p, uint32_t len, SockAddrIn* out) {
  if (!p || len < 8) {
    return false;
  }
  out->family = LoadBE16(p + 0);
  out->port = LoadBE16(p + 2);
  out->addr = LoadBE32(p + 4);
  return true;
}

// Writes min(len, 16) bytes of a sockaddr_in; returns the size written.
inline uint32_t StoreSockAddrIn(uint8_t* p, uint32_t len, const SockAddrIn& a) {
  uint8_t tmp[kSockAddrInSize] = {};
  StoreBE16(tmp + 0, a.family);
  StoreBE16(tmp + 2, a.port);
  StoreBE32(tmp + 4, a.addr);
  uint32_t n = len < kSockAddrInSize ? len : kSockAddrInSize;
  std::memcpy(p, tmp, n);
  return n;
}

inline uint32_t FdSetCount(const uint8_t* set) {
  uint32_t n = LoadBE32(set);
  return n < kFdSetSize ? n : kFdSetSize;
}
inline uint32_t FdSetGet(const uint8_t* set, uint32_t i) {
  return LoadBE32(set + 4 + 4 * i);
}
// Rewrites the set to contain exactly fds[0..n).
inline void FdSetAssign(uint8_t* set, const uint32_t* fds, uint32_t n) {
  if (n > kFdSetSize) n = kFdSetSize;
  StoreBE32(set, n);
  for (uint32_t i = 0; i < n; ++i) StoreBE32(set + 4 + 4 * i, fds[i]);
}
inline bool FdIsSet(const uint8_t* set, uint32_t fd) {
  uint32_t n = FdSetCount(set);
  for (uint32_t i = 0; i < n; ++i) {
    if (FdSetGet(set, i) == fd) return true;
  }
  return false;
}

// timeval -> microseconds (negative = invalid -> treated as 0 by the caller).
inline int64_t LoadTimevalUs(const uint8_t* tv) {
  int32_t sec = int32_t(LoadBE32(tv + 0));
  int32_t usec = int32_t(LoadBE32(tv + 4));
  return int64_t(sec) * 1000000 + usec;
}

// Deterministic virtual MAC for a virtual IPv4 address (locally administered, 02:52:58:...).
inline void VirtualMac(uint32_t addr, uint8_t mac[6]) {
  mac[0] = 0x02;
  mac[1] = 0x52;  // 'R'
  mac[2] = uint8_t(addr >> 24);
  mac[3] = uint8_t(addr >> 16);
  mac[4] = uint8_t(addr >> 8);
  mac[5] = uint8_t(addr);
}

// XNADDR for a virtual host: ina = addr, no online address, MAC from the address.
inline void StoreXnAddr(uint8_t* p, uint32_t addr) {
  std::memset(p, 0, kXnAddrSize);
  StoreBE32(p + 0, addr);
  VirtualMac(addr, p + 10);
}
inline uint32_t LoadXnAddrIna(const uint8_t* p) {
  return LoadBE32(p + 0);
}

}  // namespace rex::system::web::net_relay::guest
