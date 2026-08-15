export module Core;

export import Core.Profiler;

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

export import <span>;
export import <vector>;
export import <array>;
export import <list>;
export import <map>;
export import <set>;
export import <unordered_map>;
export import <unordered_set>;
export import <string>;
export import <string_view>;
export import <variant>;
export import <tuple>;
// NO <fstream> / <filesystem> here on purpose: ALL file and directory access goes through the File
// library's FileSystem (string paths, main-thread assert). Only File itself includes them; a
// library that needs the disk links File and calls FileSystem.
export import <iostream>;
export import <functional>;
export import <memory>;
export import <mutex>;
export import <shared_mutex>;
export import <condition_variable>;
export import <deque>;
export import <queue>;
export import <thread>;
export import <future>;
export import <bitset>;
export import <coroutine>;
export import <atomic>;
export import <type_traits>;
export import <chrono>;
export import <initializer_list>;
export import <execution>;
export import <charconv>;
export import <optional>;
export import <new>;
export import <bit>;
export import <cmath>;
export import <random>;
export import <sstream>;

export template<typename T, size_t N>
constexpr size_t ARRAY_SIZE(const T(&)[N]) { return N; }

export using Clock = std::conditional_t<std::chrono::high_resolution_clock::is_steady,
    std::chrono::high_resolution_clock,
    std::chrono::steady_clock>;