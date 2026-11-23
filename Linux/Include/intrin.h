#pragma once

// Linux (clang) stand-in for MSVC's <intrin.h>: Linux/Include is searched before clang's own resource headers, so the
// engine's `#include <intrin.h>` lands here. The x86 intrinsics come from <x86intrin.h>; the MSVC-only names the engine
// uses are mapped onto clang builtins.
#include <x86intrin.h>
#include <cstdint>

#define __popcnt(x)          __builtin_popcount(static_cast<unsigned int>(x))
#define __popcnt64(x)        __builtin_popcountll(static_cast<unsigned long long>(x))
#define _rotl8(v, s)         __builtin_rotateleft8(static_cast<uint8_t>(v), static_cast<uint8_t>(s))
#define _rotl16(v, s)        __builtin_rotateleft16(static_cast<uint16_t>(v), static_cast<uint16_t>(s))
#define _rotr8(v, s)         __builtin_rotateright8(static_cast<uint8_t>(v), static_cast<uint8_t>(s))
#define _rotr16(v, s)        __builtin_rotateright16(static_cast<uint16_t>(v), static_cast<uint16_t>(s))
#undef _rotl
#undef _rotr
#undef _rotl64
#undef _rotr64
#define _rotl(v, s)          __builtin_rotateleft32(static_cast<uint32_t>(v), static_cast<uint32_t>(s))
#define _rotr(v, s)          __builtin_rotateright32(static_cast<uint32_t>(v), static_cast<uint32_t>(s))
#define _rotl64(v, s)        __builtin_rotateleft64(static_cast<uint64_t>(v), static_cast<uint64_t>(s))
#define _rotr64(v, s)        __builtin_rotateright64(static_cast<uint64_t>(v), static_cast<uint64_t>(s))
#define _byteswap_ushort(x)  __builtin_bswap16(x)
#define _byteswap_ulong(x)   __builtin_bswap32(x)
#define _byteswap_uint64(x)  __builtin_bswap64(x)

// MSVC semantics: compares the 16 bytes at `destination` with `comparand`; on a match stores {high, low} and returns
// 1, else returns 0. Either way `comparand` receives the value that was in `destination`. (cmpxchg16b: -march=x86-64-v3
// includes CX16.)
inline unsigned char _InterlockedCompareExchange128(volatile long long* destination, long long exchangeHigh,
                                                    long long exchangeLow, long long* comparand)
{
    using u128 = unsigned __int128;
    u128 expected = (u128(uint64_t(comparand[1])) << 64) | uint64_t(comparand[0]);
    const u128 desired = (u128(uint64_t(exchangeHigh)) << 64) | uint64_t(exchangeLow);
    const bool ok = __atomic_compare_exchange_n(reinterpret_cast<volatile u128*>(destination), &expected, desired, false,
                                                __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    comparand[0] = static_cast<long long>(uint64_t(expected));
    comparand[1] = static_cast<long long>(uint64_t(expected >> 64));
    return ok ? 1 : 0;
}
