/**
 * @file        kernel/xboxkrnl/xecrypt_rsa.cpp
 * @brief       RSA public operation behind XeCryptBnQwNeRsaPubCrypt (portable
 *              bignum code, plus the Win32 BCrypt reference).
 *
 * The portable code uses 32-bit limbs with 64-bit intermediates so it builds the
 * same on every target (x64, arm64, wasm64) without __int128 or intrinsics.
 */

#include "xecrypt_rsa.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

#if REX_PLATFORM_WIN32
#include <windows.h>

#include <bcrypt.h>
#endif

namespace rex::kernel::xboxkrnl::xecrypt {

namespace {

using Limb = uint32_t;

// Guest QwNe layout -> little-endian 32-bit limbs (2 per qword).
void LoadQwNe(const uint8_t* g, uint32_t size_qw, Limb* limbs) {
  for (uint32_t i = 0; i < size_qw; ++i) {
    uint64_t q = 0;
    for (uint32_t j = 0; j < 8; ++j) {
      q = (q << 8) | g[i * 8 + j];
    }
    limbs[2 * i] = static_cast<Limb>(q);
    limbs[2 * i + 1] = static_cast<Limb>(q >> 32);
  }
}

void StoreQwNe(const Limb* limbs, uint32_t size_qw, uint8_t* g) {
  for (uint32_t i = 0; i < size_qw; ++i) {
    const uint64_t q = (uint64_t(limbs[2 * i + 1]) << 32) | limbs[2 * i];
    for (uint32_t j = 0; j < 8; ++j) {
      g[i * 8 + j] = static_cast<uint8_t>(q >> (56 - 8 * j));
    }
  }
}

int Compare(const Limb* a, const Limb* b, size_t k) {
  for (size_t i = k; i-- > 0;) {
    if (a[i] != b[i]) {
      return a[i] < b[i] ? -1 : 1;
    }
  }
  return 0;
}

bool IsZero(const Limb* a, size_t k) {
  return std::all_of(a, a + k, [](Limb x) { return x == 0; });
}

// a -= b (mod 2^(32k)).
void SubInPlace(Limb* a, const Limb* b, size_t k) {
  uint64_t borrow = 0;
  for (size_t i = 0; i < k; ++i) {
    const uint64_t d = uint64_t(a[i]) - b[i] - borrow;
    a[i] = static_cast<Limb>(d);
    borrow = (d >> 32) & 1;
  }
}

// r = 2r mod n, for r < n.
void DoubleMod(Limb* r, const Limb* n, size_t k) {
  Limb carry = 0;
  for (size_t i = 0; i < k; ++i) {
    const Limb top = r[i] >> 31;
    r[i] = (r[i] << 1) | carry;
    carry = top;
  }
  if (carry || Compare(r, n, k) >= 0) {
    SubInPlace(r, n, k);  // wraps correctly when carry is set: 2r - n < n
  }
}

// r = (r + a) mod n, for r, a < n.
void AddMod(Limb* r, const Limb* a, const Limb* n, size_t k) {
  uint64_t carry = 0;
  for (size_t i = 0; i < k; ++i) {
    const uint64_t s = uint64_t(r[i]) + a[i] + carry;
    r[i] = static_cast<Limb>(s);
    carry = s >> 32;
  }
  if (carry || Compare(r, n, k) >= 0) {
    SubInPlace(r, n, k);
  }
}

// out = a * b * R^-1 mod n, R = 2^(32k), n odd, a * b < n * R.
// CIOS Montgomery multiplication; t is scratch of k + 2 limbs. out may alias a/b.
void MontMul(const Limb* a, const Limb* b, const Limb* n, Limb n0inv, size_t k, Limb* t,
             Limb* out) {
  std::fill(t, t + k + 2, 0);
  for (size_t i = 0; i < k; ++i) {
    uint64_t c = 0;
    const uint64_t bi = b[i];
    for (size_t j = 0; j < k; ++j) {
      const uint64_t s = uint64_t(a[j]) * bi + t[j] + c;
      t[j] = static_cast<Limb>(s);
      c = s >> 32;
    }
    uint64_t s = uint64_t(t[k]) + c;
    t[k] = static_cast<Limb>(s);
    t[k + 1] = static_cast<Limb>(s >> 32);

    const uint64_t m = static_cast<Limb>(t[0] * n0inv);
    s = m * n[0] + t[0];
    c = s >> 32;
    for (size_t j = 1; j < k; ++j) {
      s = m * n[j] + t[j] + c;
      t[j - 1] = static_cast<Limb>(s);
      c = s >> 32;
    }
    s = uint64_t(t[k]) + c;
    t[k - 1] = static_cast<Limb>(s);
    t[k] = t[k + 1] + static_cast<Limb>(s >> 32);
  }
  // t < 2n here.
  if (t[k] || Compare(t, n, k) >= 0) {
    SubInPlace(t, n, k);
  }
  std::copy(t, t + k, out);
}

// -n0^-1 mod 2^32 for odd n0 (Newton iteration: 3 -> 6 -> 12 -> 24 -> 48 bits).
Limb MontInverse(Limb n0) {
  Limb x = n0;
  for (int i = 0; i < 4; ++i) {
    x *= 2 - n0 * x;
  }
  return static_cast<Limb>(0u - x);
}

// out = a^e mod n, n odd, a < n.
void ModExpOdd(const Limb* a, uint32_t e, const Limb* n, size_t k, Limb* out) {
  std::vector<Limb> r2(k, 0), am(k), acc(k), one(k, 0), t(k + 2);
  const Limb n0inv = MontInverse(n[0]);

  // r2 = R^2 mod n by doubling 1 (n > 1 here, so 1 < n).
  r2[0] = 1;
  for (size_t i = 0; i < 64 * k; ++i) {
    DoubleMod(r2.data(), n, k);
  }
  one[0] = 1;
  MontMul(a, r2.data(), n, n0inv, k, t.data(), am.data());    // a * R mod n
  MontMul(one.data(), r2.data(), n, n0inv, k, t.data(), acc.data());  // R mod n
  for (int bit = 31; bit >= 0; --bit) {
    MontMul(acc.data(), acc.data(), n, n0inv, k, t.data(), acc.data());
    if ((e >> bit) & 1) {
      MontMul(acc.data(), am.data(), n, n0inv, k, t.data(), acc.data());
    }
  }
  MontMul(acc.data(), one.data(), n, n0inv, k, t.data(), out);
}

// out = a * b mod n for a, b < n (bit-serial, any n > 0).
void ModMulSlow(const Limb* a, const Limb* b, const Limb* n, size_t k, Limb* out) {
  std::vector<Limb> r(k, 0);
  for (size_t i = 32 * k; i-- > 0;) {
    DoubleMod(r.data(), n, k);
    if ((b[i / 32] >> (i % 32)) & 1) {
      AddMod(r.data(), a, n, k);
    }
  }
  std::copy(r.begin(), r.end(), out);
}

// out = a^e mod n for even n > 1, a < n.
void ModExpSlow(const Limb* a, uint32_t e, const Limb* n, size_t k, Limb* out) {
  std::vector<Limb> acc(k, 0);
  acc[0] = 1;
  for (int bit = 31; bit >= 0; --bit) {
    ModMulSlow(acc.data(), acc.data(), n, k, acc.data());
    if ((e >> bit) & 1) {
      ModMulSlow(acc.data(), a, n, k, acc.data());
    }
  }
  std::copy(acc.begin(), acc.end(), out);
}

}  // namespace

bool RsaPubCryptPortable(const uint8_t* in, uint8_t* out, uint32_t exponent,
                         const uint8_t* modulus, uint32_t size_qw) {
  if (size_qw == 0 || size_qw > kRsaMaxQwords) {
    return false;
  }
  const size_t k = size_t(size_qw) * 2;
  std::vector<Limb> a(k), n(k), r(k, 0);
  LoadQwNe(in, size_qw, a.data());
  LoadQwNe(modulus, size_qw, n.data());
  if (IsZero(n.data(), k) || Compare(a.data(), n.data(), k) >= 0) {
    return false;
  }

  std::vector<Limb> unit(k, 0);
  unit[0] = 1;
  if (Compare(n.data(), unit.data(), k) == 0) {
    // mod 1: everything is 0 (unreachable after the a < n check, kept for clarity).
  } else if (n[0] & 1) {
    ModExpOdd(a.data(), exponent, n.data(), k, r.data());
  } else {
    ModExpSlow(a.data(), exponent, n.data(), k, r.data());
  }
  StoreQwNe(r.data(), size_qw, out);
  return true;
}

#if REX_PLATFORM_WIN32

namespace {
// Guest QwNe <-> one big-endian byte string (most significant byte first): the
// qword order is reversed, the bytes inside each qword are already big-endian.
// The transform is its own inverse.
void QwNeToBigEndian(const uint8_t* src, uint32_t size_qw, uint8_t* dst) {
  for (uint32_t i = 0; i < size_qw; ++i) {
    std::memcpy(dst + 8 * i, src + 8 * (size_qw - 1 - i), 8);
  }
}
}  // namespace

bool RsaPubCryptBCrypt(const uint8_t* in, uint8_t* out, uint32_t exponent,
                       const uint8_t* modulus, uint32_t size_qw) {
  const uint32_t modulus_size = size_qw * 8;

  // Convert XECRYPT blob into BCrypt format: header, exponent, modulus, all
  // big-endian inside the blob.
  ULONG key_size = sizeof(BCRYPT_RSAKEY_BLOB) + sizeof(uint32_t) + modulus_size;
  auto key_buf = std::make_unique<uint8_t[]>(key_size);
  auto* key_header = reinterpret_cast<BCRYPT_RSAKEY_BLOB*>(key_buf.get());

  key_header->Magic = BCRYPT_RSAPUBLIC_MAGIC;
  key_header->BitLength = modulus_size * 8;
  key_header->cbPublicExp = sizeof(uint32_t);
  key_header->cbModulus = modulus_size;
  key_header->cbPrime1 = key_header->cbPrime2 = 0;

  uint8_t* key_exponent = reinterpret_cast<uint8_t*>(&key_header[1]);
  key_exponent[0] = static_cast<uint8_t>(exponent >> 24);
  key_exponent[1] = static_cast<uint8_t>(exponent >> 16);
  key_exponent[2] = static_cast<uint8_t>(exponent >> 8);
  key_exponent[3] = static_cast<uint8_t>(exponent);
  QwNeToBigEndian(modulus, size_qw, key_exponent + 4);

  BCRYPT_ALG_HANDLE hAlgorithm = NULL;
  NTSTATUS status =
      BCryptOpenAlgorithmProvider(&hAlgorithm, BCRYPT_RSA_ALGORITHM, MS_PRIMITIVE_PROVIDER, 0);
  if (!BCRYPT_SUCCESS(status)) {
    return false;
  }

  BCRYPT_KEY_HANDLE hKey = NULL;
  status = BCryptImportKeyPair(hAlgorithm, NULL, BCRYPT_RSAPUBLIC_BLOB, &hKey, key_buf.get(),
                               key_size, 0);
  if (!BCRYPT_SUCCESS(status)) {
    BCryptCloseAlgorithmProvider(hAlgorithm, 0);
    return false;
  }

  // Input as one big-endian number, written to the output buffer (as the
  // original implementation did; in and out may alias).
  std::vector<uint8_t> tmp(modulus_size);
  QwNeToBigEndian(in, size_qw, tmp.data());
  std::memcpy(out, tmp.data(), modulus_size);

  // BCryptDecrypt only works with private keys, fortunately BCryptEncrypt
  // performs the right actions needed for us to decrypt the input.
  ULONG result_size = 0;
  status = BCryptEncrypt(hKey, out, modulus_size, nullptr, nullptr, 0, out, modulus_size,
                         &result_size, BCRYPT_PAD_NONE);
  if (BCRYPT_SUCCESS(status)) {
    std::memcpy(tmp.data(), out, modulus_size);
    QwNeToBigEndian(tmp.data(), size_qw, out);
  }

  BCryptDestroyKey(hKey);
  BCryptCloseAlgorithmProvider(hAlgorithm, 0);
  return BCRYPT_SUCCESS(status);
}

#endif  // REX_PLATFORM_WIN32

}  // namespace rex::kernel::xboxkrnl::xecrypt
