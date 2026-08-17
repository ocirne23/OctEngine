// The engine's container vocabulary. Every container, string, view, smart pointer, atomic and the
// utilities around them are spelled oc::<name> EVERYWHERE in the codebase -- this file is the only
// place that names the std:: original. Swapping a backing implementation (EASTL, a hand-rolled one)
// is an edit here, not at the ~6500 call sites.
//
// ALIASES ONLY -- one line per type, nothing with a body. The engine's own containers live next to
// this file in their own module (Core.SmallVector), which imports this one; never the reverse.
//
// Deliberately NOT aliased: iostreams/sstream, chrono, mutex/thread/condition_variable/future,
// type_traits, <cmath> and the other C runtime surface. Those are platform/runtime facilities, not
// container surface, and no replacement library ships them -- code keeps saying std:: for them.
//
// Alias templates cannot be specialized: a std::hash specialization is still written as
// `namespace std { template<> struct hash<X> ... }`, and is picked up through oc::hash.
export module Core.OcSTL;

// PRIVATE: the headers behind the oc:: aliases. Not re-exported -- the aliases stay usable anyway,
// since an exported alias keeps its target reachable. This does NOT make std::vector unreachable
// for importers: a header unit re-exports its entire include closure, so the exported <iostream>
// below hands out <string>/<vector>/<memory> regardless. Keeping them private is tidiness; the
// oc:: rule is a convention the compiler cannot enforce.
import <algorithm>;
import <array>;
import <atomic>;
import <bitset>;
import <deque>;
import <functional>;
import <initializer_list>;
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

// EXPORTED: the std facilities with no oc:: spelling, moved here out of Core.ixx so every std
// header unit the engine imports enters through this one file. NO <fstream> / <filesystem> on
// purpose: ALL file and directory access goes through the File library's FileSystem (string paths,
// main-thread assert). Only File itself includes them; a library that needs the disk links File.
export import <iostream>;
export import <sstream>;
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

export namespace oc
{
    // ---- allocation ------------------------------------------------------------------------
    // Global new/delete already route through Globals::allocator, so the default allocator is
    // tracked and attributed by the MemoryTracker like everything else.
    template<typename T> using allocator = std::allocator<T>;
    template<typename T> using allocator_traits = std::allocator_traits<T>;

    // ---- comparators, hashing, traits ------------------------------------------------------
    template<typename T = void> using less = std::less<T>;
    template<typename T = void> using greater = std::greater<T>;
    template<typename T = void> using equal_to = std::equal_to<T>;
    template<typename T> using hash = std::hash<T>;
    template<typename T> using char_traits = std::char_traits<T>;
    template<typename T> using numeric_limits = std::numeric_limits<T>;
    template<typename T> using initializer_list = std::initializer_list<T>;

    // ---- value types -----------------------------------------------------------------------
    template<typename T1, typename T2> using pair = std::pair<T1, T2>;
    template<typename... Ts> using tuple = std::tuple<Ts...>;
    template<typename T> using optional = std::optional<T>;
    template<typename... Ts> using variant = std::variant<Ts...>;
    using nullopt_t = std::nullopt_t;
    using monostate = std::monostate;
    using std::nullopt;
    using std::get;
    using std::get_if;
    using std::holds_alternative;
    using std::visit;
    using std::make_pair;
    using std::make_tuple;
    using std::tie;

    // ---- strings ---------------------------------------------------------------------------
    template<typename C, typename Traits = char_traits<C>, typename Alloc = allocator<C>>
    using basic_string = std::basic_string<C, Traits, Alloc>;
    using string = std::string;
    using wstring = std::wstring;
    using u8string = std::u8string;
    using std::to_string;
    using std::to_wstring;

    // ---- views (non-owning, never allocate) ------------------------------------------------
    template<typename C, typename Traits = char_traits<C>>
    using basic_string_view = std::basic_string_view<C, Traits>;
    using string_view = std::string_view;
    using wstring_view = std::wstring_view;
    using u8string_view = std::u8string_view;
    using std::dynamic_extent;
    template<typename T, size_t Extent = std::dynamic_extent> using span = std::span<T, Extent>;

    // ---- sequence containers ---------------------------------------------------------------
    template<typename T, typename Alloc = allocator<T>> using vector = std::vector<T, Alloc>;
    template<typename T, size_t N> using array = std::array<T, N>;
    template<typename T, typename Alloc = allocator<T>> using deque = std::deque<T, Alloc>;
    template<typename T, typename Alloc = allocator<T>> using list = std::list<T, Alloc>;
    template<size_t N> using bitset = std::bitset<N>;

    // ---- container adaptors ----------------------------------------------------------------
    template<typename T, typename Container = deque<T>> using queue = std::queue<T, Container>;
    template<typename T, typename Container = deque<T>> using stack = std::stack<T, Container>;
    template<typename T, typename Container = vector<T>, typename Compare = less<typename Container::value_type>>
    using priority_queue = std::priority_queue<T, Container, Compare>;

    // ---- associative containers ------------------------------------------------------------
    template<typename K, typename V, typename Compare = less<K>, typename Alloc = allocator<pair<const K, V>>>
    using map = std::map<K, V, Compare, Alloc>;
    template<typename K, typename V, typename Compare = less<K>, typename Alloc = allocator<pair<const K, V>>>
    using multimap = std::multimap<K, V, Compare, Alloc>;
    template<typename K, typename Compare = less<K>, typename Alloc = allocator<K>>
    using set = std::set<K, Compare, Alloc>;
    template<typename K, typename Compare = less<K>, typename Alloc = allocator<K>>
    using multiset = std::multiset<K, Compare, Alloc>;
    template<typename K, typename V, typename Hash = hash<K>, typename Eq = equal_to<K>,
             typename Alloc = allocator<pair<const K, V>>>
    using unordered_map = std::unordered_map<K, V, Hash, Eq, Alloc>;
    template<typename K, typename V, typename Hash = hash<K>, typename Eq = equal_to<K>,
             typename Alloc = allocator<pair<const K, V>>>
    using unordered_multimap = std::unordered_multimap<K, V, Hash, Eq, Alloc>;
    template<typename K, typename Hash = hash<K>, typename Eq = equal_to<K>, typename Alloc = allocator<K>>
    using unordered_set = std::unordered_set<K, Hash, Eq, Alloc>;
    template<typename K, typename Hash = hash<K>, typename Eq = equal_to<K>, typename Alloc = allocator<K>>
    using unordered_multiset = std::unordered_multiset<K, Hash, Eq, Alloc>;

    // ---- smart pointers --------------------------------------------------------------------
    template<typename T> using default_delete = std::default_delete<T>;
    template<typename T, typename Deleter = default_delete<T>> using unique_ptr = std::unique_ptr<T, Deleter>;
    template<typename T> using shared_ptr = std::shared_ptr<T>;
    template<typename T> using weak_ptr = std::weak_ptr<T>;
    template<typename T> using enable_shared_from_this = std::enable_shared_from_this<T>;
    using std::make_unique;
    using std::make_shared;
    using std::static_pointer_cast;
    using std::dynamic_pointer_cast;
    using std::const_pointer_cast;
    using std::addressof;

    // ---- callables -------------------------------------------------------------------------
    template<typename Sig> using function = std::function<Sig>;
    template<typename T> using reference_wrapper = std::reference_wrapper<T>;
    using std::ref;
    using std::cref;
    using std::invoke;

    // ---- atomics ---------------------------------------------------------------------------
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
    using std::move;
    using std::forward;
    using std::swap;
    using std::exchange;
    using std::as_const;
    using std::min;
    using std::max;
    using std::minmax;
    using std::clamp;
    using std::next;
    using std::prev;
    using std::distance;
    using std::advance;
    using std::make_move_iterator;
    using std::back_inserter;
    using std::begin;
    using std::end;
    using std::size;
    using std::data;

    // ---- algorithms ------------------------------------------------------------------------
    using std::sort;
    using std::stable_sort;
    using std::partial_sort;
    using std::nth_element;
    using std::find;
    using std::find_if;
    using std::find_if_not;
    using std::count;
    using std::count_if;
    using std::any_of;
    using std::all_of;
    using std::none_of;
    using std::for_each;
    using std::fill;
    using std::fill_n;
    using std::copy;
    using std::copy_n;
    using std::transform;
    using std::remove;
    using std::remove_if;
    using std::unique;
    using std::reverse;
    using std::rotate;
    using std::lower_bound;
    using std::upper_bound;
    using std::binary_search;
    using std::partition;
    using std::stable_partition;
    using std::search;
    using std::min_element;
    using std::max_element;
    using std::erase;
    using std::erase_if;
}
