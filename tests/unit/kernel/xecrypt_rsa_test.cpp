/**
 * @file        tests/unit/kernel/xecrypt_rsa_test.cpp
 * @brief       XeCryptBnQwNeRsaPubCrypt: portable bignum code vs known vectors and,
 *              on Windows, vs the BCrypt reference implementation.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "kernel/xboxkrnl/xecrypt_rsa.h"

#if REX_PLATFORM_WIN32
#include <windows.h>

#include <bcrypt.h>
#endif

using namespace rex::kernel::xboxkrnl::xecrypt;

namespace {

// Guest QwNe buffer from host qwords (least significant first).
std::vector<uint8_t> QwNe(const std::vector<uint64_t>& qw) {
  std::vector<uint8_t> g(qw.size() * 8);
  for (size_t i = 0; i < qw.size(); ++i) {
    for (int j = 0; j < 8; ++j) {
      g[i * 8 + j] = uint8_t(qw[i] >> (56 - 8 * j));
    }
  }
  return g;
}

// Guest QwNe buffer <-> one big-endian byte string (MSB first).
std::vector<uint8_t> QwNeFromBigEndian(const uint8_t* be, size_t size_qw) {
  std::vector<uint8_t> g(size_qw * 8);
  for (size_t i = 0; i < size_qw; ++i) {
    std::memcpy(&g[8 * i], be + 8 * (size_qw - 1 - i), 8);
  }
  return g;
}

bool Pub(const std::vector<uint8_t>& in, std::vector<uint8_t>& out, uint32_t e,
         const std::vector<uint8_t>& n) {
  out.assign(n.size(), 0xCD);
  return RsaPubCryptPortable(in.data(), out.data(), e, n.data(), uint32_t(n.size() / 8));
}

}  // namespace

TEST_CASE("xecrypt rsa: textbook vector, one qword", "[xecrypt]") {
  // n = 61 * 53, e = 17, d = 2753.
  auto n = QwNe({3233});
  std::vector<uint8_t> c, m;
  REQUIRE(Pub(QwNe({65}), c, 17, n));
  CHECK(c == QwNe({2790}));
  REQUIRE(Pub(c, m, 2753, n));
  CHECK(m == QwNe({65}));
}

TEST_CASE("xecrypt rsa: qword order and carries across qwords", "[xecrypt]") {
  std::vector<uint8_t> out;
  // a = 2^64, n = 2^127 + 1 (odd): a^2 = 2^128 = -2 mod n -> 2^127 - 1.
  REQUIRE(Pub(QwNe({0, 1}), out, 2, QwNe({1, 0x8000000000000000ull})));
  CHECK(out == QwNe({~0ull, 0x7FFFFFFFFFFFFFFFull}));
  // Even modulus path: n = 2^127 + 2: 2^128 = -4 mod n -> 2^127 - 2.
  REQUIRE(Pub(QwNe({0, 1}), out, 2, QwNe({2, 0x8000000000000000ull})));
  CHECK(out == QwNe({~0ull - 1, 0x7FFFFFFFFFFFFFFFull}));
  // e = 0 -> 1, e = 1 -> a.
  REQUIRE(Pub(QwNe({5, 7}), out, 0, QwNe({1, 0x8000000000000000ull})));
  CHECK(out == QwNe({1, 0}));
  REQUIRE(Pub(QwNe({5, 7}), out, 1, QwNe({1, 0x8000000000000000ull})));
  CHECK(out == QwNe({5, 7}));
}

TEST_CASE("xecrypt rsa: rejects", "[xecrypt]") {
  std::vector<uint8_t> out;
  CHECK_FALSE(Pub(QwNe({3233}), out, 17, QwNe({3233})));  // a == n
  CHECK_FALSE(Pub(QwNe({4000}), out, 17, QwNe({3233})));  // a > n
  CHECK_FALSE(Pub(QwNe({0}), out, 17, QwNe({0})));        // n == 0
  CHECK(out == std::vector<uint8_t>(8, 0xCD));             // untouched on failure
  uint8_t dummy[8] = {};
  CHECK_FALSE(RsaPubCryptPortable(dummy, dummy, 3, dummy, 0));
}

TEST_CASE("xecrypt rsa: in place (in == out)", "[xecrypt]") {
  auto n = QwNe({3233});
  auto buf = QwNe({65});
  REQUIRE(RsaPubCryptPortable(buf.data(), buf.data(), 17, n.data(), 1));
  CHECK(buf == QwNe({2790}));
}

#if REX_PLATFORM_WIN32

namespace {

struct Stats {
  int both_ok = 0;
  int both_fail = 0;
  int bcrypt_only_fail = 0;
  int portable_only_fail = 0;
  std::set<std::string> bcrypt_only_fail_cases;  // "what (size_qw)"
};

// Runs one vector through both implementations. Requires identical output
// whenever both succeed; returns which side failed for the statistics.
void Compare(const std::vector<uint8_t>& a, uint32_t e, const std::vector<uint8_t>& n,
             Stats& stats, const char* what) {
  const uint32_t size_qw = uint32_t(n.size() / 8);
  std::vector<uint8_t> out_p(n.size(), 0xCD), out_b(n.size(), 0xCD);
  const bool ok_p = RsaPubCryptPortable(a.data(), out_p.data(), e, n.data(), size_qw);
  const bool ok_b = RsaPubCryptBCrypt(a.data(), out_b.data(), e, n.data(), size_qw);
  INFO(what << ": size_qw=" << size_qw << " e=" << e << " portable=" << ok_p
            << " bcrypt=" << ok_b);
  if (ok_p && ok_b) {
    ++stats.both_ok;
    REQUIRE(out_p == out_b);
  } else if (!ok_p && !ok_b) {
    ++stats.both_fail;
  } else if (!ok_b) {
    // BCrypt's only extra rejection: keys below 256 bits.
    CHECK(size_qw < 4);
    ++stats.bcrypt_only_fail;
    stats.bcrypt_only_fail_cases.insert(std::string(what) + " (" + std::to_string(size_qw) +
                                        " qw)");
  } else {
    ++stats.portable_only_fail;
    FAIL("portable rejected a vector BCrypt accepts");
  }
}

std::vector<uint8_t> RandomBytes(std::mt19937_64& rng, size_t n) {
  std::vector<uint8_t> v(n);
  for (auto& b : v) b = uint8_t(rng());
  return v;
}

// Random modulus (big-endian), top bit set, given parity; random a < n.
void RandomVector(std::mt19937_64& rng, size_t size_qw, bool odd, std::vector<uint8_t>& a,
                  std::vector<uint8_t>& n) {
  auto nbe = RandomBytes(rng, size_qw * 8);
  nbe[0] |= 0x80;
  nbe.back() = odd ? (nbe.back() | 1) : (nbe.back() & ~1);
  auto abe = RandomBytes(rng, size_qw * 8);
  abe[0] = uint8_t(rng() % nbe[0]);  // strictly below n's top byte
  n = QwNeFromBigEndian(nbe.data(), size_qw);
  a = QwNeFromBigEndian(abe.data(), size_qw);
}

void Print(const char* name, const Stats& s) {
  std::printf("[xecrypt] %s: both ok %d, both rejected %d, only BCrypt rejected %d\n", name,
              s.both_ok, s.both_fail, s.bcrypt_only_fail);
  for (const auto& c : s.bcrypt_only_fail_cases) {
    std::printf("[xecrypt]   only BCrypt rejected: %s\n", c.c_str());
  }
}

}  // namespace

TEST_CASE("xecrypt rsa: portable == BCrypt on random odd moduli", "[xecrypt][bcrypt]") {
  std::mt19937_64 rng(0x4D5307E6);
  Stats stats;
  const uint32_t fixed_e[] = {3, 17, 65537};
  for (size_t size_qw : {1, 2, 3, 4, 8, 16, 24, 32, 48, 64}) {
    for (int iter = 0; iter < 24; ++iter) {
      std::vector<uint8_t> a, n;
      RandomVector(rng, size_qw, true, a, n);
      uint32_t e;
      switch (iter % 4) {
        case 0: e = fixed_e[(iter / 4) % 3]; break;
        case 1: e = uint32_t(rng()) | 1; break;     // random odd
        case 2: e = uint32_t(rng()) | 0x80000000u; break;  // random, top bit set
        default: e = uint32_t(rng()); break;
      }
      Compare(a, e, n, stats, "random odd");
    }
  }
  Print("random odd moduli", stats);
  CHECK(stats.both_ok >= 168);
}

TEST_CASE("xecrypt rsa: portable vs BCrypt edge cases", "[xecrypt][bcrypt]") {
  std::mt19937_64 rng(7);
  Stats stats;
  for (size_t size_qw : {1, 2, 16, 32, 64}) {
    std::vector<uint8_t> a, n;
    // Even moduli.
    RandomVector(rng, size_qw, false, a, n);
    Compare(a, 65537, n, stats, "even modulus");
    Compare(a, 4, n, stats, "even modulus, even exponent");
    // Small exponents.
    RandomVector(rng, size_qw, true, a, n);
    Compare(a, 0, n, stats, "e=0");
    Compare(a, 1, n, stats, "e=1");
    Compare(a, 2, n, stats, "e=2");
    // a = 0, a = 1, a = n - 1.
    std::vector<uint8_t> zero(n.size(), 0), one = zero;
    one[7] = 1;  // qword 0 is stored first, big-endian: byte 7 is the LSB
    Compare(zero, 65537, n, stats, "a=0");
    Compare(one, 65537, n, stats, "a=1");
    auto nm1 = n;
    nm1[7] -= 1;  // n is odd, so no borrow
    Compare(nm1, 65537, n, stats, "a=n-1");
    // a == n, a > n.
    Compare(n, 65537, n, stats, "a=n");
    auto big = n;
    big[8 * (size_qw - 1)] = 0xFF;
    big[7] |= 1;
    Compare(big, 65537, n, stats, "a>n");
    // Modulus top bit clear (top byte 0x01..0x7F).
    {
      auto nt = n, at = a;
      nt[8 * (size_qw - 1)] = uint8_t(1 + rng() % 0x7F);
      at[8 * (size_qw - 1)] = 0;
      Compare(at, 65537, nt, stats, "modulus top bit clear");
    }
    // Modulus with a zero top qword. BCrypt is handed BitLength = 64 * size_qw
    // with a leading zero qword and returns an unreduced value (>= n), so this
    // is checked against BCrypt on the same numbers with the zero qword
    // trimmed, i.e. against the mathematically correct result.
    if (size_qw > 2) {
      auto nz = n;
      std::memset(&nz[8 * (size_qw - 1)], 0, 8);
      nz[8 * (size_qw - 2)] |= 0x80;
      auto az = a;
      std::memset(&az[8 * (size_qw - 1)], 0, 8);
      az[8 * (size_qw - 2)] = 0;
      std::vector<uint8_t> out_p(nz.size()), out_b(nz.size() - 8), out_untrimmed(nz.size());
      REQUIRE(RsaPubCryptPortable(az.data(), out_p.data(), 65537, nz.data(), uint32_t(size_qw)));
      REQUIRE(RsaPubCryptBCrypt(az.data(), out_b.data(), 65537, nz.data(), uint32_t(size_qw - 1)));
      CHECK(std::vector<uint8_t>(out_p.begin(), out_p.end() - 8) == out_b);
      CHECK(std::vector<uint8_t>(out_p.end() - 8, out_p.end()) == std::vector<uint8_t>(8, 0));
      const bool untrimmed_ok = RsaPubCryptBCrypt(az.data(), out_untrimmed.data(), 65537,
                                                  nz.data(), uint32_t(size_qw));
      std::printf("[xecrypt]   zero top qword (%zu qw): untrimmed BCrypt %s\n", size_qw,
                  !untrimmed_ok              ? "rejects"
                  : out_untrimmed == out_p ? "matches"
                                           : "differs (unreduced result)");
    }
  }
  Print("edge cases", stats);
}

TEST_CASE("xecrypt rsa: matches BCrypt in place", "[xecrypt][bcrypt]") {
  std::mt19937_64 rng(11);
  std::vector<uint8_t> a, n;
  RandomVector(rng, 32, true, a, n);
  auto bp = a, bb = a;
  REQUIRE(RsaPubCryptPortable(bp.data(), bp.data(), 65537, n.data(), 32));
  REQUIRE(RsaPubCryptBCrypt(bb.data(), bb.data(), 65537, n.data(), 32));
  CHECK(bp == bb);
}

TEST_CASE("xecrypt rsa: real 2048-bit key round trip through BCrypt private key",
          "[xecrypt][bcrypt]") {
  BCRYPT_ALG_HANDLE alg = nullptr;
  REQUIRE(BCRYPT_SUCCESS(
      BCryptOpenAlgorithmProvider(&alg, BCRYPT_RSA_ALGORITHM, MS_PRIMITIVE_PROVIDER, 0)));
  BCRYPT_KEY_HANDLE key = nullptr;
  REQUIRE(BCRYPT_SUCCESS(BCryptGenerateKeyPair(alg, &key, 2048, 0)));
  REQUIRE(BCRYPT_SUCCESS(BCryptFinalizeKeyPair(key, 0)));

  ULONG blob_size = 0;
  REQUIRE(BCRYPT_SUCCESS(
      BCryptExportKey(key, nullptr, BCRYPT_RSAPUBLIC_BLOB, nullptr, 0, &blob_size, 0)));
  std::vector<uint8_t> blob(blob_size);
  REQUIRE(BCRYPT_SUCCESS(BCryptExportKey(key, nullptr, BCRYPT_RSAPUBLIC_BLOB, blob.data(),
                                         blob_size, &blob_size, 0)));
  auto* hdr = reinterpret_cast<BCRYPT_RSAKEY_BLOB*>(blob.data());
  REQUIRE(hdr->cbModulus == 256);
  REQUIRE(hdr->cbPublicExp <= 4);
  const uint8_t* exp_be = blob.data() + sizeof(BCRYPT_RSAKEY_BLOB);
  uint32_t e = 0;
  for (ULONG i = 0; i < hdr->cbPublicExp; ++i) e = (e << 8) | exp_be[i];
  const uint8_t* mod_be = exp_be + hdr->cbPublicExp;
  auto n = QwNeFromBigEndian(mod_be, 32);

  std::mt19937_64 rng(13);
  for (int iter = 0; iter < 8; ++iter) {
    auto mbe = RandomBytes(rng, 256);
    mbe[0] = uint8_t(rng() % mod_be[0]);
    auto m = QwNeFromBigEndian(mbe.data(), 32);

    std::vector<uint8_t> c(256), cb(256);
    REQUIRE(RsaPubCryptPortable(m.data(), c.data(), e, n.data(), 32));
    REQUIRE(RsaPubCryptBCrypt(m.data(), cb.data(), e, n.data(), 32));
    REQUIRE(c == cb);

    // Decrypt c with the private key: must give m back.
    std::vector<uint8_t> cbe(256), dec(256);
    for (int i = 0; i < 32; ++i) std::memcpy(&cbe[8 * i], &c[8 * (31 - i)], 8);
    ULONG out_size = 0;
    REQUIRE(BCRYPT_SUCCESS(BCryptDecrypt(key, cbe.data(), 256, nullptr, nullptr, 0, dec.data(),
                                         256, &out_size, BCRYPT_PAD_NONE)));
    REQUIRE(out_size == 256);
    CHECK(dec == mbe);
  }
  BCryptDestroyKey(key);
  BCryptCloseAlgorithmProvider(alg, 0);
}

#endif  // REX_PLATFORM_WIN32
