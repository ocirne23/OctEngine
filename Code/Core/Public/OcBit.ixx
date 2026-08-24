// The engine's bit vocabulary -- the replacement for <bit>, which OcSTL deliberately does NOT
// export. Every one of the std spellings (std::popcount, std::countr_zero, std::bit_width, ...)
// is written to run on ANY x86 baseline, so each one either dispatches on a runtime
// __isa_available check or expands to the portable BSF/BSR sequence with its zero-input fixup --
// a branch and a fixup on paths that are one instruction wide. The engine compiles with
// /arch:AVX2, so POPCNT, LZCNT and the whole BMI1/BMI2 set are baseline instructions here: these
// wrappers name them directly and nothing tests for CPU support.
//
// Consequence of naming the instructions directly: tzcnt/lzcnt are DEFINED on zero (they return
// the operand width, matching std::countr_zero / std::countl_zero). That is a property of the
// TZCNT/LZCNT encodings, not of BSF/BSR -- do not lower the /arch level without revisiting it.
//
// The operations are templates over the unsigned builtin types and are width-exact: passing a
// uint8 to lzcnt() answers in 8-bit terms, not in the 32-bit register's terms. Signed types are
// refused (the concept does not accept them) -- bit math on a sign bit wants an explicit cast.
//
// Sits at the very bottom of Core next to Core.OcSTL and Core.Profiler: it imports nothing, and
// Core.ixx re-exports it, so every importer of Core has oc::popcnt & co.
module;

#include <intrin.h>    // the rotates
#include <immintrin.h> // POPCNT / LZCNT / BMI1 / BMI2 intrinsics
#include <cstdint>

export module Core.OcBit;

export namespace oc
{
    // The unsigned builtin types, spelled without <type_traits> (this module imports nothing).
    // `unsigned long` is a distinct type from `unsigned int` on MSVC even though both are 32 bits,
    // so it has to be listed or every DWORD-typed call site would fail to match.
    namespace bitDetail
    {
        template<typename T> inline constexpr bool isUnsignedInt = false;
        template<> inline constexpr bool isUnsignedInt<unsigned char> = true;
        template<> inline constexpr bool isUnsignedInt<unsigned short> = true;
        template<> inline constexpr bool isUnsignedInt<unsigned int> = true;
        template<> inline constexpr bool isUnsignedInt<unsigned long> = true;
        template<> inline constexpr bool isUnsignedInt<unsigned long long> = true;
    }

    template<typename T>
    concept UnsignedInt = bitDetail::isUnsignedInt<T>;

    template<UnsignedInt T> inline constexpr uint32_t bitCount = uint32_t(sizeof(T) * 8);

    // Reinterpret the object representation. Not an instruction at all -- a compiler builtin, and
    // the one piece of <bit> that never had a dispatch problem. It lives here so nothing has to
    // import <bit> for it. Usable in constant expressions, unlike everything below.
    template<typename To, typename From>
    [[nodiscard]] constexpr To bitCast(const From& from) { return __builtin_bit_cast(To, from); }

    // Number of set bits. POPCNT.
    template<UnsignedInt T>
    [[nodiscard]] inline uint32_t popcnt(T v)
    {
        if constexpr (sizeof(T) == 8) return uint32_t(__popcnt64(uint64_t(v)));
        else                          return uint32_t(__popcnt(uint32_t(v)));
    }

    // Trailing (least significant) zero bits; bitCount<T> when v == 0. TZCNT.
    template<UnsignedInt T>
    [[nodiscard]] inline uint32_t tzcnt(T v)
    {
        if constexpr (sizeof(T) == 8)      return uint32_t(_tzcnt_u64(uint64_t(v)));
        else if constexpr (sizeof(T) == 4) return uint32_t(_tzcnt_u32(uint32_t(v)));
        // 8/16-bit: park a set bit at the type's width so zero answers 8 / 16 instead of 32.
        else                               return uint32_t(_tzcnt_u32(uint32_t(v) | (1u << bitCount<T>)));
    }

    // Leading (most significant) zero bits; bitCount<T> when v == 0. LZCNT.
    template<UnsignedInt T>
    [[nodiscard]] inline uint32_t lzcnt(T v)
    {
        if constexpr (sizeof(T) == 8)      return uint32_t(_lzcnt_u64(uint64_t(v)));
        else if constexpr (sizeof(T) == 4) return uint32_t(_lzcnt_u32(uint32_t(v)));
        // 8/16-bit: the widening added (32 - bitCount) leading zeros that are not the value's.
        else                               return uint32_t(_lzcnt_u32(uint32_t(v))) - (32u - bitCount<T>);
    }

    // Trailing / leading ONE bits (std::countr_one / std::countl_one).
    template<UnsignedInt T> [[nodiscard]] inline uint32_t trailingOnes(T v) { return tzcnt(T(~v)); }
    template<UnsignedInt T> [[nodiscard]] inline uint32_t leadingOnes(T v)  { return lzcnt(T(~v)); }

    // Bits needed to represent v; 0 for v == 0 (std::bit_width).
    template<UnsignedInt T>
    [[nodiscard]] inline uint32_t bitWidth(T v) { return bitCount<T> - lzcnt(v); }

    // Smallest / largest power of two >= v resp. <= v (std::bit_ceil / std::bit_floor).
    // bitCeil overflows for v above the type's highest power of two, exactly like std::bit_ceil.
    template<UnsignedInt T>
    [[nodiscard]] inline T bitCeil(T v) { return v <= T(1) ? T(1) : T(T(1) << bitWidth(T(v - 1))); }
    template<UnsignedInt T>
    [[nodiscard]] inline T bitFloor(T v) { return v == T(0) ? T(0) : T(T(1) << (bitWidth(v) - 1)); }

    template<UnsignedInt T>
    [[nodiscard]] inline bool hasSingleBit(T v) { return popcnt(v) == 1u; }

    // Rotates. The MSVC intrinsics are width-exact, so each width names its own.
    [[nodiscard]] inline uint8_t  rotl(uint8_t v, int s)  { return _rotl8(v, uint8_t(s)); }
    [[nodiscard]] inline uint16_t rotl(uint16_t v, int s) { return _rotl16(v, uint8_t(s)); }
    [[nodiscard]] inline uint32_t rotl(uint32_t v, int s) { return _rotl(v, s); }
    [[nodiscard]] inline uint64_t rotl(uint64_t v, int s) { return _rotl64(v, s); }
    [[nodiscard]] inline uint8_t  rotr(uint8_t v, int s)  { return _rotr8(v, uint8_t(s)); }
    [[nodiscard]] inline uint16_t rotr(uint16_t v, int s) { return _rotr16(v, uint8_t(s)); }
    [[nodiscard]] inline uint32_t rotr(uint32_t v, int s) { return _rotr(v, s); }
    [[nodiscard]] inline uint64_t rotr(uint64_t v, int s) { return _rotr64(v, s); }

    // Byte order reversal (std::byteswap). BSWAP.
    [[nodiscard]] inline uint16_t byteSwap(uint16_t v) { return _byteswap_ushort(v); }
    [[nodiscard]] inline uint32_t byteSwap(uint32_t v) { return _byteswap_ulong(v); }
    [[nodiscard]] inline uint64_t byteSwap(uint64_t v) { return _byteswap_uint64(v); }

    // BMI1. The idioms these replace (v & (v - 1), v & -v, ~a & b) compile to the same single
    // instruction; they are named here so a bit-twiddling site can say what it means.
    template<UnsignedInt T> [[nodiscard]] inline T blsr(T v)   // clear the lowest set bit
    {
        if constexpr (sizeof(T) == 8) return T(_blsr_u64(uint64_t(v)));
        else                          return T(_blsr_u32(uint32_t(v)));
    }
    template<UnsignedInt T> [[nodiscard]] inline T blsi(T v)   // isolate the lowest set bit
    {
        if constexpr (sizeof(T) == 8) return T(_blsi_u64(uint64_t(v)));
        else                          return T(_blsi_u32(uint32_t(v)));
    }
    template<UnsignedInt T> [[nodiscard]] inline T blsmsk(T v) // mask up to AND including it
    {
        if constexpr (sizeof(T) == 8) return T(_blsmsk_u64(uint64_t(v)));
        else                          return T(_blsmsk_u32(uint32_t(v)));
    }
    // ANDN has no MSVC intrinsic of its own; the idiom is what the compiler pattern-matches.
    template<UnsignedInt T> [[nodiscard]] inline T andn(T a, T b) { return T(~a & b); }
    template<UnsignedInt T> [[nodiscard]] inline T bextr(T v, uint32_t start, uint32_t len)
    {
        if constexpr (sizeof(T) == 8) return T(_bextr_u64(uint64_t(v), start, len));
        else                          return T(_bextr_u32(uint32_t(v), start, len));
    }

    // BMI2. bzhi zeroes everything from bit `index` up; pdep/pext scatter/gather bits through a
    // mask (the Morton encode/decode primitives).
    template<UnsignedInt T> [[nodiscard]] inline T bzhi(T v, uint32_t index)
    {
        if constexpr (sizeof(T) == 8) return T(_bzhi_u64(uint64_t(v), index));
        else                          return T(_bzhi_u32(uint32_t(v), index));
    }
    [[nodiscard]] inline uint32_t pdep(uint32_t v, uint32_t mask) { return _pdep_u32(v, mask); }
    [[nodiscard]] inline uint64_t pdep(uint64_t v, uint64_t mask) { return _pdep_u64(v, mask); }
    [[nodiscard]] inline uint32_t pext(uint32_t v, uint32_t mask) { return _pext_u32(v, mask); }
    [[nodiscard]] inline uint64_t pext(uint64_t v, uint64_t mask) { return _pext_u64(v, mask); }
}
