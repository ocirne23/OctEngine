// The engine's container vocabulary. Every container, string, view, smart pointer, atomic and the
// utilities around them are spelled oc::<name> EVERYWHERE in the codebase -- this file is the only
// place that names the backing library. Swapping that backing is an edit here, not at the ~6500
// call sites: `#define EASTL` and every oc:: name resolves to eastl:: instead of std::.
//
// ALIASES ONLY -- one line per type, nothing with a body. The engine's own containers live next to
// this file in their own module (Core.SmallVector), which imports this one; never the reverse.
//
// Deliberately NOT aliased: iostreams/sstream, chrono, mutex/thread/condition_variable/future,
// type_traits, <cmath> and the other C runtime surface. Those are platform/runtime facilities, not
// container surface, and no replacement library ships them -- code keeps saying std:: for them, and
// this file re-exports them so Core.ixx has no std import of its own.
//
// Alias templates cannot be specialized: a std::hash specialization is still written as
// `namespace std { template<> struct hash<X> ... }`, and is picked up through oc::hash.
//
// NOT aliased either, because the two backings cannot express them the same way: allocator_traits
// (EASTL has none) and basic_string / basic_string_view (EASTL's take no traits parameter, so the
// arity differs). A vocabulary entry that silently means something different per backing is worse
// than no entry -- name the concrete string/string_view types instead.
module;

// THE BACKING SWITCH. Comment this out to go back to the std containers; it is deliberately a define
// in this file rather than a compiler flag, since this is the only TU that has to see it (every
// other file spells oc::, which resolves through the aliases below). Core links EASTL(d).lib and
// Core.cpp supplies the allocation + Vsnprintf hooks EASTL expects the application to provide.
#define EASTL

// ...and immediately translated, then undefined. EASTL is ALSO the include-directory name, and an
// `import <EASTL/x.h>;` macro-expands its header name (a #include does not -- there the lexer forms
// one header-name token), so leaving the macro defined rewrites every import below into `</x.h>`.
#ifdef EASTL
    #define OC_EASTL
    #undef EASTL
#endif

export module Core.OcSTL;

#ifdef OC_EASTL
// HEADER UNITS, not textual includes in the global module fragment: a GMF include leaves EASTL's
// free operators (string ==, string +, container comparisons) unreachable to every importer of
// Core, since only the class templates are decl-reachable from the aliases below. Imported this
// way the whole header is exported, operators included -- the same thing Core.SDL/Core.imgui do.
export import <EASTL/algorithm.h>;
export import <EASTL/allocator.h>;
export import <EASTL/array.h>;
export import <EASTL/atomic.h>;
export import <EASTL/bitset.h>;
export import <EASTL/deque.h>;
export import <EASTL/functional.h>;
export import <EASTL/iterator.h>;
export import <EASTL/list.h>;
export import <EASTL/map.h>;
export import <EASTL/memory.h>;
export import <EASTL/numeric_limits.h>;
export import <EASTL/optional.h>;
export import <EASTL/priority_queue.h>;
export import <EASTL/queue.h>;
export import <EASTL/set.h>;
export import <EASTL/shared_ptr.h>;
export import <EASTL/sort.h>;
export import <EASTL/span.h>;
export import <EASTL/stack.h>;
export import <EASTL/string.h>;
export import <EASTL/string_view.h>;
export import <EASTL/tuple.h>;
export import <EASTL/unique_ptr.h>;
export import <EASTL/unordered_map.h>;
export import <EASTL/unordered_set.h>;
export import <EASTL/utility.h>;
export import <EASTL/variant.h>;
export import <EASTL/vector.h>;
export import <EASTL/weak_ptr.h>;
#endif

#ifndef OC_EASTL
// PRIVATE: the headers behind the oc:: aliases. Not re-exported -- the aliases stay usable anyway,
// since an exported alias keeps its target reachable. This does NOT make std::vector unreachable
// for importers: a header unit re-exports its entire include closure, so the exported <iostream>
// below hands out <string>/<vector>/<memory> regardless. Keeping them private is tidiness; the
// oc:: rule is a convention the compiler cannot enforce.
import <algorithm>;
import <array>;
import <bitset>;
import <deque>;
import <functional>;
import <iterator>;
import <limits>;
import <list>;
import <map>;
import <memory>;
import <optional>;
import <queue>;
import <set>;
import <span>;
import <stack>;
import <string>;
import <string_view>;
import <tuple>;
import <unordered_map>;
import <unordered_set>;
import <utility>;
import <variant>;
import <vector>;
#endif

// Needed by BOTH backings: EASTL has no atomic_ref, and it does not define its own initializer_list
// (EASTL/initializer_list.h defers to std::initializer_list, which is what the compiler generates
// braced lists as anyway).
import <atomic>;
import <initializer_list>;

// EXPORTED: the std facilities with no oc:: spelling, moved here out of Core.ixx so every std header
// unit the engine imports enters through this one file. NO <fstream> / <filesystem> on purpose: ALL
// file and directory access goes through the File library's FileSystem (string paths, main-thread
// assert). Only File itself includes them; a library that needs the disk links File.
export import <iostream>;
export import <sstream>;
export import <format>;
export import <mutex>;
export import <shared_mutex>;
export import <condition_variable>;
export import <thread>;
export import <future>;
export import <coroutine>;
export import <chrono>;
export import <execution>;
export import <charconv>;
export import <type_traits>;
export import <new>;
export import <bit>;
export import <cmath>;
export import <random>;

#ifdef OC_EASTL
// EASTL knows nothing about <iostream> or <format>, so the handoff points would each need a manual
// conversion. These shims keep them compiling as written. They live in namespace eastl so ADL finds
// them from any importer; the formatters must be in std, next to the primary template.
export namespace eastl
{
    inline std::ostream& operator<<(std::ostream& stream, const eastl::string& text)
    {
        return stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    }
    inline std::ostream& operator<<(std::ostream& stream, eastl::string_view text)
    {
        return stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    }
    inline std::istream& getline(std::istream& stream, eastl::string& text, char delimiter = '\n')
    {
        std::string line;
        std::getline(stream, line, delimiter);
        text.assign(line.data(), line.size());
        return stream;
    }
}

export namespace std
{
    template<> struct formatter<eastl::string, char> : formatter<string_view, char>
    {
        template<typename Context>
        auto format(const eastl::string& text, Context& context) const
        {
            return formatter<string_view, char>::format(string_view(text.data(), text.size()), context);
        }
    };
    template<> struct formatter<eastl::string_view, char> : formatter<string_view, char>
    {
        template<typename Context>
        auto format(eastl::string_view text, Context& context) const
        {
            return formatter<string_view, char>::format(string_view(text.data(), text.size()), context);
        }
    };
}
#endif

// WHAT THE SWITCH COSTS BEYOND THIS FILE. oc::string stops BEING std::string, so every handoff to a
// std facility (the exported list above) and to every third-party API that speaks std:: has to be
// crossed explicitly. That crossing is oc::toStd / oc::fromStd, and the places that need it are:
// Vulkan-Hpp's enumerate* (they return std::vector), vk::ClearColorValue and vk::ResultValue's tuple
// conversion (std::array / std::tie), glslang's preprocess/GlslangToSpv/OutputSpvBin (std::string
// and std::vector<unsigned> by reference), ONNX Runtime's Session::Run (std::vector<Ort::Value>,
// move-only so it cannot even be converted), <filesystem> in File, the iostream/sstream parsers, and
// the Nsight Aftermath to_string overloads. All of them are marked at the call site.
//
// EASTL also lacks pieces of the C++20 library the engine used: no member .contains(), no
// starts_with/ends_with, no string_view operator+= or assign, no atomic wait/notify -- oc::contains,
// oc::startsWith, oc::endsWith and the std-pinned atomics above cover those.

// THE switch. Everything below names this, never std:: or eastl:: directly, so a name that exists in
// both backings needs no #ifdef of its own -- only the handful that genuinely differ do.
#ifdef OC_EASTL
namespace ocstl = eastl;
#else
namespace ocstl = std;
#endif

export namespace oc
{
    // ---- allocation ------------------------------------------------------------------------
    // Global new/delete already route through Globals::allocator, so the default allocator is
    // tracked and attributed by the MemoryTracker like everything else.
#ifdef OC_EASTL
    // EASTL allocators are NOT element-typed -- one allocator type serves every T, and it hands out
    // raw bytes. The parameter is ignored so both backings share one alias signature; the byte-level
    // difference is absorbed by allocateArray/deallocateArray below.
    template<typename T> using allocator = ocstl::allocator;
#else
    template<typename T> using allocator = ocstl::allocator<T>;
#endif

    // Raw array allocation, the one place the two allocator models have to be spelled apart. This is
    // what Core.SmallVector allocates through -- it must not care which backing is active.
    template<typename T> T* allocateArray(size_t count)
    {
#ifdef OC_EASTL
        return static_cast<T*>(allocator<T>().allocate(count * sizeof(T), alignof(T), 0));
#else
        return allocator<T>().allocate(count);
#endif
    }
    template<typename T> void deallocateArray(T* pointer, size_t count)
    {
#ifdef OC_EASTL
        allocator<T>().deallocate(pointer, count * sizeof(T));
#else
        allocator<T>().deallocate(pointer, count);
#endif
    }

    // ---- comparators, hashing, traits ------------------------------------------------------
    template<typename T = void> using less = ocstl::less<T>;
    template<typename T = void> using greater = ocstl::greater<T>;
    template<typename T = void> using equal_to = ocstl::equal_to<T>;
    template<typename T> using hash = ocstl::hash<T>;
    template<typename T> using numeric_limits = ocstl::numeric_limits<T>;
    template<typename T> using initializer_list = std::initializer_list<T>; // std in both backings
    // std in both backings too: EASTL ships no char_traits (its basic_string takes no traits
    // parameter), and this one is a trait over raw char buffers -- it does not care who owns them.
    template<typename T> using char_traits = std::char_traits<T>;

    // ---- value types -----------------------------------------------------------------------
    template<typename T1, typename T2> using pair = ocstl::pair<T1, T2>;
    template<typename... Ts> using tuple = ocstl::tuple<Ts...>;
    template<typename T> using optional = ocstl::optional<T>;
    template<typename... Ts> using variant = ocstl::variant<Ts...>;
    using nullopt_t = ocstl::nullopt_t;
    using monostate = ocstl::monostate;
    using ocstl::nullopt;
    using ocstl::get;
    using ocstl::get_if;
    using ocstl::holds_alternative;
    using ocstl::visit;
    using ocstl::make_pair;
    using ocstl::make_tuple;
    using ocstl::tie;

    // ---- strings ---------------------------------------------------------------------------
    using string = ocstl::string;
    using wstring = ocstl::wstring;
    using u8string = ocstl::u8string;
    using ocstl::to_string;
    using ocstl::to_wstring;
    using ocstl::getline; // std::getline, or the eastl shim above

    // ---- views (non-owning, never allocate) ------------------------------------------------
    using string_view = ocstl::string_view;
    using wstring_view = ocstl::wstring_view;
    using u8string_view = ocstl::u8string_view;
#ifdef OC_EASTL
    // eastl::dynamic_extent is declared `static`, i.e. internal linkage, which a header unit does
    // not export (a textual include would have carried it). Same value, spelled once here.
    inline constexpr size_t dynamic_extent = size_t(-1);
#else
    using ocstl::dynamic_extent;
#endif
    template<typename T, size_t Extent = dynamic_extent> using span = ocstl::span<T, Extent>;

    // Byte view over any span. Ours in BOTH backings: std::as_bytes does not accept an eastl::span,
    // and EASTL ships no equivalent of its own.
    template<typename T, size_t Extent>
    span<const std::byte> as_bytes(span<T, Extent> source)
    {
        return span<const std::byte>(reinterpret_cast<const std::byte*>(source.data()), source.size() * sizeof(T));
    }

    // ---- sequence containers ---------------------------------------------------------------
    template<typename T, typename Alloc = allocator<T>> using vector = ocstl::vector<T, Alloc>;
    template<typename T, size_t N> using array = ocstl::array<T, N>;
    template<typename T, typename Alloc = allocator<T>> using deque = ocstl::deque<T, Alloc>;
    template<typename T, typename Alloc = allocator<T>> using list = ocstl::list<T, Alloc>;
    template<size_t N> using bitset = ocstl::bitset<N>;

    // ---- container adaptors ----------------------------------------------------------------
    template<typename T, typename Container = deque<T>> using queue = ocstl::queue<T, Container>;
    template<typename T, typename Container = deque<T>> using stack = ocstl::stack<T, Container>;
    template<typename T, typename Container = vector<T>, typename Compare = less<typename Container::value_type>>
    using priority_queue = ocstl::priority_queue<T, Container, Compare>;

    // ---- associative containers ------------------------------------------------------------
    template<typename K, typename V, typename Compare = less<K>, typename Alloc = allocator<pair<const K, V>>>
    using map = ocstl::map<K, V, Compare, Alloc>;
    template<typename K, typename V, typename Compare = less<K>, typename Alloc = allocator<pair<const K, V>>>
    using multimap = ocstl::multimap<K, V, Compare, Alloc>;
    template<typename K, typename Compare = less<K>, typename Alloc = allocator<K>>
    using set = ocstl::set<K, Compare, Alloc>;
    template<typename K, typename Compare = less<K>, typename Alloc = allocator<K>>
    using multiset = ocstl::multiset<K, Compare, Alloc>;
    template<typename K, typename V, typename Hash = hash<K>, typename Eq = equal_to<K>,
             typename Alloc = allocator<pair<const K, V>>>
    using unordered_map = ocstl::unordered_map<K, V, Hash, Eq, Alloc>;
    template<typename K, typename V, typename Hash = hash<K>, typename Eq = equal_to<K>,
             typename Alloc = allocator<pair<const K, V>>>
    using unordered_multimap = ocstl::unordered_multimap<K, V, Hash, Eq, Alloc>;
    template<typename K, typename Hash = hash<K>, typename Eq = equal_to<K>, typename Alloc = allocator<K>>
    using unordered_set = ocstl::unordered_set<K, Hash, Eq, Alloc>;
    template<typename K, typename Hash = hash<K>, typename Eq = equal_to<K>, typename Alloc = allocator<K>>
    using unordered_multiset = ocstl::unordered_multiset<K, Hash, Eq, Alloc>;

    // ---- smart pointers --------------------------------------------------------------------
    template<typename T> using default_delete = ocstl::default_delete<T>;
    template<typename T, typename Deleter = default_delete<T>> using unique_ptr = ocstl::unique_ptr<T, Deleter>;
    template<typename T> using shared_ptr = ocstl::shared_ptr<T>;
    template<typename T> using weak_ptr = ocstl::weak_ptr<T>;
    template<typename T> using enable_shared_from_this = ocstl::enable_shared_from_this<T>;
#ifdef OC_EASTL
    // NOT ocstl::make_unique for arrays. EASTL's is `new T[n]` -- DEFAULT-initialized -- where std's
    // is `new T[n]()`, value-initialized. That divergence is silent and vicious: BitRangeAllocator's
    // grown tail came back as garbage bits (i.e. fully allocated), and the script-data / profiler
    // record / char buffers all assume zeroed memory too. The oc:: vocabulary must mean the same
    // thing under either backing, so both forms are spelled out here.
    template<typename T, typename... Args>
    ocstl::enable_if_t<!std::is_array_v<T>, unique_ptr<T>> make_unique(Args&&... args)
    {
        return unique_ptr<T>(new T(ocstl::forward<Args>(args)...)); // qualified: ADL also finds std::forward
    }
    template<typename T>
    ocstl::enable_if_t<std::is_unbounded_array_v<T>, unique_ptr<T>> make_unique(size_t count)
    {
        return unique_ptr<T>(new std::remove_extent_t<T>[count]()); // the () is the whole point
    }
#else
    using ocstl::make_unique;
#endif
    using ocstl::make_shared;
    using ocstl::static_pointer_cast;
    using ocstl::dynamic_pointer_cast;
    using ocstl::const_pointer_cast;
    using ocstl::addressof;

    // ---- callables -------------------------------------------------------------------------
    template<typename Sig> using function = ocstl::function<Sig>;
    template<typename T> using reference_wrapper = ocstl::reference_wrapper<T>;
    using ocstl::ref;
    using ocstl::cref;
    using ocstl::invoke;

    // ---- atomics ---------------------------------------------------------------------------
    // std in BOTH backings, deliberately. An atomic is not a container: it allocates nothing and is
    // a thin wrapper over compiler intrinsics, so there is nothing to swap. EASTL's atomic is also a
    // strict subset -- no atomic_ref, no C++20 wait/notify (the JobSystem's eventcount and JobMutex
    // are built on it), and its memory orders are distinct TAG TYPES rather than one memory_order
    // enum, which would not mix with any std::atomic the engine still has to touch.
    template<typename T> using atomic = std::atomic<T>;
    template<typename T> using atomic_ref = std::atomic_ref<T>;
    using atomic_flag = std::atomic_flag;
    using memory_order = std::memory_order;
    using std::memory_order_relaxed;
    using std::memory_order_consume;
    using std::memory_order_acquire;
    using std::memory_order_release;
    using std::memory_order_acq_rel;
    using std::memory_order_seq_cst;
    using std::atomic_thread_fence;
    using std::atomic_signal_fence;

    // ---- utilities -------------------------------------------------------------------------
    using ocstl::move;
    using ocstl::forward;
    using ocstl::swap;
    using ocstl::exchange;
    using ocstl::as_const;
    using ocstl::min;
    using ocstl::max;
    using ocstl::minmax;
    using ocstl::clamp;
    using ocstl::next;
    using ocstl::prev;
    using ocstl::distance;
    using ocstl::advance;
    using ocstl::make_move_iterator;
    using ocstl::back_inserter;
    using ocstl::begin;
    using ocstl::end;
    using ocstl::size;
    using ocstl::data;

    // ---- algorithms ------------------------------------------------------------------------
    using ocstl::sort;
    using ocstl::stable_sort;
    using ocstl::partial_sort;
    using ocstl::nth_element;
    using ocstl::find;
    using ocstl::find_if;
    using ocstl::find_if_not;
    using ocstl::count;
    using ocstl::count_if;
    using ocstl::any_of;
    using ocstl::all_of;
    using ocstl::none_of;
    using ocstl::for_each;
    using ocstl::fill;
    using ocstl::fill_n;
    using ocstl::copy;
    using ocstl::copy_n;
    using ocstl::transform;
    using ocstl::remove;
    using ocstl::remove_if;
    using ocstl::unique;
    using ocstl::reverse;
    using ocstl::rotate;
    using ocstl::lower_bound;
    using ocstl::upper_bound;
    using ocstl::binary_search;
    using ocstl::partition;
    using ocstl::stable_partition;
    using ocstl::search;
    using ocstl::min_element;
    using ocstl::max_element;
    using ocstl::erase;
    using ocstl::erase_if;

    // Member .contains() is C++20 and EASTL's associative containers do not have it; this reads the
    // same at the call site and works on both backings.
    template<typename Container, typename Key>
    bool contains(const Container& container, const Key& key) { return container.find(key) != container.end(); }

    // Likewise C++20 members EASTL's basic_string does not have.
    inline bool startsWith(string_view text, string_view prefix)
    {
        return text.size() >= prefix.size() && string_view(text.data(), prefix.size()) == prefix;
    }
    inline bool endsWith(string_view text, string_view suffix)
    {
        return text.size() >= suffix.size() && string_view(text.data() + text.size() - suffix.size(), suffix.size()) == suffix;
    }

    // ---- std interop -----------------------------------------------------------------------
    // Under the EASTL backing oc::string stops BEING std::string, but the std facilities the engine
    // kept (iostreams, format) and every third-party API that speaks std::string still have to be
    // fed. These two are the sanctioned crossing; they are a no-op copy under the std backing, so
    // call sites stay backing-agnostic.
    inline std::string toStd(string_view text) { return std::string(text.data(), text.size()); }
    inline string fromStd(const std::string& text) { return string(text.data(), text.size()); }

    // Same crossing for sequences: Vulkan-Hpp, assimp and friends hand back std::vector by value.
    // Through POINTERS, not iterators: EASTL's iterator-range constructor does not accept MSVC's
    // checked std iterators. Both sides are contiguous, so data()..data()+size() is exact.
    template<typename T> vector<T> fromStd(const std::vector<T>& source) { return vector<T>(source.data(), source.data() + source.size()); }
    template<typename T> std::vector<T> toStd(const vector<T>& source) { return std::vector<T>(source.data(), source.data() + source.size()); }

    // std::format hands back a std::string; this hands back an oc::string, so a formatted result
    // feeds straight into the engine's own vocabulary. Under the EASTL backing the formatter
    // specializations above are what make an oc::string a valid ARGUMENT here too.
    template<typename... Args>
    string format(std::format_string<Args...> spec, Args&&... args)
    {
        return fromStd(std::format(spec, std::forward<Args>(args)...));
    }
}
