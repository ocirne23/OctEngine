export module Core.VirtualArray;

import Core;
import Core.Allocator; // the memory tracker hooks

// The OS calls behind VirtualArray (Private/VirtualArray.cpp): reserve address space, commit a prefix of it
// (zero-filled pages), release it.
void* virtualReserve(size_t bytes);
void virtualCommit(void* base, size_t bytes);
void virtualRelease(void* base);
constexpr size_t VIRTUAL_PAGE_SIZE = 4096; // x64 Windows

// An array that NEVER moves: its whole maximum size is RESERVED as address space up front, and pages are
// COMMITTED as it grows. Growth never reallocates under a concurrent reader or writer, and an access is a
// plain base + index (no page table) - for lock-free pools whose hot readers must not pay an indirection
// (Spatial's RecordPool). Committed memory reads as ZERO and is never constructed or destructed, so T must
// be trivial.
//
// MEMORY PANEL: it bypasses the engine allocator, so it reports itself to the tracker hooks - every commit
// step is one tracked "allocation" (its own start address, its byte delta), made under a ProfileScope named
// by reserve(): a box of that name, nested under whatever scope the growth ran in (the first commit's is
// usually the owner's initialize). The destructor frees them all.
export template<typename T>
class VirtualArray final
{
    static_assert(std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>,
        "committed pages are zero-filled, never constructed");
public:
    VirtualArray() = default;
    VirtualArray(const VirtualArray&) = delete;
    VirtualArray& operator=(const VirtualArray&) = delete;
    ~VirtualArray()
    {
        if (!m_data)
            return;
        for (uint32 i = 0; i < m_numSteps; ++i)
            callMemoryFreeHook(reinterpret_cast<uint8*>(m_data) + m_stepStart[i]);
        virtualRelease(m_data);
    }

    // `name` must be a string literal (the Memory panel's box: a profile scope name).
    void reserve(uint32 maxCount, const char* name, EProfileCategory category)
    {
        assert(!m_data && maxCount > 0);
        m_maxCount = maxCount;
        m_name = name;
        m_category = category;
        m_data = static_cast<T*>(virtualReserve(size_t(maxCount) * sizeof(T)));
    }

    // Commits at least the first `count` elements (whole pages). Thread-safe; never moves the data.
    void commit(uint32 count)
    {
        if (count <= m_committed.load(oc::memory_order_acquire))
            return;
        const std::lock_guard lock(m_commitMutex);
        if (count <= m_committed.load(oc::memory_order_relaxed))
            return;
        assert(count <= m_maxCount && "VirtualArray: past the reservation");
        const size_t page = VIRTUAL_PAGE_SIZE;
        const size_t maxBytes = (size_t(m_maxCount) * sizeof(T) + page - 1) / page * page;
        const size_t bytes = oc::min((size_t(count) * sizeof(T) + page - 1) / page * page, maxBytes);
        virtualCommit(m_data, bytes);
        if (bytes > m_committedBytes && m_numSteps < MAX_STEPS)
        {
            const ProfileScope scope(m_name, m_category);
            callMemoryAllocHook(reinterpret_cast<uint8*>(m_data) + m_committedBytes, bytes - m_committedBytes);
            m_stepStart[m_numSteps++] = m_committedBytes;
        }
        m_committedBytes = bytes;
        m_committed.store(uint32(oc::min<size_t>(bytes / sizeof(T), m_maxCount)), oc::memory_order_release);
    }

    T& operator[](uint32 idx) { return m_data[idx]; }
    const T& operator[](uint32 idx) const { return m_data[idx]; }
    T* data() { return m_data; }
    const T* data() const { return m_data; }
    uint32 committed() const { return m_committed.load(oc::memory_order_relaxed); }
    uint32 maxCount() const { return m_maxCount; }

private:
    static constexpr uint32 MAX_STEPS = 64; // tracked commit steps (a doubling owner needs ~25)

    T* m_data = nullptr;
    uint32 m_maxCount = 0;
    oc::atomic<uint32> m_committed{ 0 };
    std::mutex m_commitMutex;
    const char* m_name = "VirtualArray";
    EProfileCategory m_category = EProfileCategory::Other;
    size_t m_committedBytes = 0;           // m_commitMutex
    size_t m_stepStart[MAX_STEPS] = {};    // byte offset of each tracked step (its tracker key)
    uint32 m_numSteps = 0;
};
