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

export typedef signed char        int8;
export typedef short              int16;
export typedef int                int32;
export typedef long long          int64;
export typedef unsigned char      uint8;
export typedef unsigned short     uint16;
export typedef unsigned int       uint32;
export typedef unsigned long long uint64;

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

export template<typename T, size_t N>
constexpr size_t ARRAY_SIZE(const T(&)[N]) { return N; }

export using Clock = std::conditional_t<std::chrono::high_resolution_clock::is_steady,
    std::chrono::high_resolution_clock,
    std::chrono::steady_clock>;