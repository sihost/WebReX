/**
 * @file        kernel/xboxkrnl/xecrypt_rsa.h
 * @brief       RSA public operation behind XeCryptBnQwNeRsaPubCrypt.
 *
 * All buffers use the guest "QwNe" bignum layout: `size_qw` 64-bit words, least
 * significant word first, each word stored big-endian (guest native order). The
 * pointers are plain host pointers into guest memory (or any buffer with that
 * layout) and need no particular alignment.
 */

#pragma once

#include <cstdint>

#include <rex/platform.h>

namespace rex::kernel::xboxkrnl::xecrypt {

/// Largest modulus accepted, in qwords (16384 bits; the console uses up to 4096).
inline constexpr uint32_t kRsaMaxQwords = 256;

/// out = in^exponent mod modulus, computed with portable bignum code
/// (Montgomery exponentiation for odd moduli, a bit-serial fallback for even).
///
/// Returns false, leaving `out` untouched, when `size_qw` is 0 or above
/// kRsaMaxQwords, the modulus is zero, or `in` is not below the modulus (the
/// cases the Win32 BCrypt path rejects as well). `in` and `out` may alias.
bool RsaPubCryptPortable(const uint8_t* in, uint8_t* out, uint32_t exponent,
                         const uint8_t* modulus, uint32_t size_qw);

#if REX_PLATFORM_WIN32
/// Same operation through Windows BCrypt (the original Xenia implementation).
/// Kept as the reference the portable code is tested against. Note: when the key
/// imports but BCryptEncrypt fails, `out` has already been overwritten.
bool RsaPubCryptBCrypt(const uint8_t* in, uint8_t* out, uint32_t exponent,
                       const uint8_t* modulus, uint32_t size_qw);
#endif

}  // namespace rex::kernel::xboxkrnl::xecrypt
