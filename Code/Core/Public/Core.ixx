module;

#ifndef _MSC_VER
#include <stdlib.h> // putenv (POSIX, not in std.compat)
#endif

export module Core;

export import Core.Profiler;
// Core.OcSTL is the ONE std seam: it owns every std header unit the engine imports, hands out the
// oc:: vocabulary (containers, strings, views, smart pointers, atomics, algorithms) and re-exports
// only the std facilities that have NO oc:: spelling -- mutex/thread/chrono/iostreams and friends.
// The container headers are imported there PRIVATELY, but that is tidiness, not a wall: a header
// unit re-exports its whole include closure, so <iostream> alone still drags std::vector into view.
// oc:: is a convention the compiler cannot enforce -- code review is what keeps std:: containers out.
export import Core.OcSTL;
export import Core.SmallVector; // oc::small_vector / oc::fixed_vector
// The bit vocabulary, in place of <bit> (which OcSTL does not export): oc::popcnt / tzcnt / lzcnt /
// bitWidth / bitCeil / bitCast / pdep / ... straight onto the /arch:AVX2 baseline instructions,
// with none of the runtime CPU-support dispatch the std spellings carry.
export import Core.OcBit;

export typedef signed char        int8;
export typedef short              int16;
export typedef int                int32;
#ifdef _MSC_VER
export typedef long long          int64;
#else
export typedef long               int64;  // LP64: int64_t is long, and the engine mixes int64 with int64_t freely
#endif
export typedef unsigned char      uint8;
export typedef unsigned short     uint16;
export typedef unsigned int       uint32;
#ifdef _MSC_VER
export typedef unsigned long long uint64;
#else
export typedef unsigned long      uint64; // LP64: uint64_t is unsigned long
#endif

#ifdef _MSC_VER
export constexpr int8  INT8_MAX  = 0x7fi8;
export constexpr int16 INT16_MAX = 0x7fffi16;
export constexpr int32 INT32_MAX = 0x7fff'ffffi32;
export constexpr int64 INT64_MAX = 0x7fff'ffff'ffff'ffffi64;
export constexpr int8  INT8_MIN  = -INT8_MAX - 1;
export constexpr int16 INT16_MIN = -INT16_MAX - 1;
export constexpr int32 INT32_MIN = -INT32_MAX - 1;
export constexpr int64 INT64_MIN = -INT64_MAX - 1;
export constexpr uint8  UINT8_MAX  = 0xffui8;
export constexpr uint16 UINT16_MAX = 0xffffui16;
export constexpr uint32 UINT32_MAX = 0xffffffffui32;
export constexpr uint64 UINT64_MAX = 0xffffffffffffffffui64;
export constexpr float FLT_MIN = 1.175494e-38f;
export constexpr float FLT_MAX = 3.402823e+38f;
export constexpr float FLT_NAN = NAN;
#else
// clang has no i8/ui64 literal suffixes and no NAN macro across `import std`.
export constexpr int8  INT8_MAX  = int8(0x7f);
export constexpr int16 INT16_MAX = int16(0x7fff);
export constexpr int32 INT32_MAX = int32(0x7fff'ffff);
export constexpr int64 INT64_MAX = int64(0x7fff'ffff'ffff'ffffl);
export constexpr int8  INT8_MIN  = -INT8_MAX - 1;
export constexpr int16 INT16_MIN = -INT16_MAX - 1;
export constexpr int32 INT32_MIN = -INT32_MAX - 1;
export constexpr int64 INT64_MIN = -INT64_MAX - 1;
export constexpr uint8  UINT8_MAX  = uint8(0xffu);
export constexpr uint16 UINT16_MAX = uint16(0xffffu);
export constexpr uint32 UINT32_MAX = uint32(0xffffffffu);
export constexpr uint64 UINT64_MAX = uint64(0xfffffffffffffffful);
export constexpr float FLT_MIN = 1.175494e-38f;
export constexpr float FLT_MAX = 3.402823e+38f;
export constexpr float FLT_NAN = __builtin_nanf("");
#endif

export template<typename T, size_t N>
constexpr size_t ARRAY_SIZE(const T(&)[N]) { return N; }

export using Clock = std::conditional_t<std::chrono::high_resolution_clock::is_steady,
    std::chrono::high_resolution_clock,
    std::chrono::steady_clock>;

#ifndef _MSC_VER
// Linux: the MSVC "secure" CRT calls the engine uses, with MSVC's truncating semantics.
export inline constexpr size_t _TRUNCATE = size_t(-1);

export template<size_t N, typename... Args>
int sprintf_s(char (&buffer)[N], const char* format, Args... args)
{
    return snprintf(buffer, N, format, args...);
}
export template<typename... Args>
int sprintf_s(char* buffer, size_t size, const char* format, Args... args)
{
    return snprintf(buffer, size, format, args...);
}
// count == _TRUNCATE: write what fits and return -1 when it did not all fit.
export template<typename... Args>
int _snprintf_s(char* buffer, size_t size, size_t count, const char* format, Args... args)
{
    const size_t limit = count == _TRUNCATE || count + 1 > size ? size : count + 1;
    const int written = snprintf(buffer, limit, format, args...);
    return written < 0 || size_t(written) >= limit ? -1 : written;
}
export inline int strncpy_s(char* destination, size_t destinationSize, const char* source, size_t count)
{
    if (destination == nullptr || destinationSize == 0)
        return 22;
    size_t length = 0;
    while (length < count && length + 1 < destinationSize && source[length] != '\0')
        ++length;
    memcpy(destination, source, length);
    destination[length] = '\0';
    return 0;
}
export inline int fopen_s(FILE** file, const char* path, const char* mode)
{
    *file = fopen(path, mode);
    return *file ? 0 : 2;
}
// putenv keeps the pointer, so the argument must outlive the process: every caller passes a string literal.
export inline int _putenv(const char* assignment) { return putenv(const_cast<char*>(assignment)); }
#endif