// oc::small_vector / oc::fixed_vector -- the engine's own contiguous containers, kept out of
// Core.OcSTL because that file is aliases ONLY (it must stay a pure one-line-per-type mapping onto
// whatever implementation backs oc::). This one imports it for oc::allocator / oc::move / friends,
// so Core.OcSTL must never import back.
export module Core.SmallVector;

import <memory>; // size_t
import <new>;    // placement new

import Core.OcSTL;

export namespace oc
{
    // A vector whose first N elements live INSIDE the object. Growth past N spills to the heap
    // (small_vector) or is refused (fixed_vector, AllowHeap = false -- asserts and drops, so a
    // release build never writes out of bounds). The inline block is what earns its keep: the
    // per-frame scratch lists that almost never exceed N do zero allocations.
    template<typename T, size_t N, bool AllowHeap = true>
    class small_vector
    {
        static_assert(N > 0, "small_vector needs at least one inline element");

    public:
        using value_type = T;
        using size_type = size_t;
        using iterator = T*;
        using const_iterator = const T*;

        small_vector() = default;
        explicit small_vector(size_type count) { resize(count); }
        small_vector(size_type count, const T& value) { reserve(count); for (size_type i = 0; i < count; ++i) pushBackUnchecked(value); }
        small_vector(initializer_list<T> values) { reserve(values.size()); for (const T& v : values) pushBackUnchecked(v); }
        small_vector(const small_vector& other) { reserve(other.m_size); for (size_type i = 0; i < other.m_size; ++i) pushBackUnchecked(other.m_data[i]); }
        small_vector(small_vector&& other) noexcept { takeFrom(other); }
        ~small_vector() { clear(); releaseHeap(); }

        small_vector& operator=(const small_vector& other)
        {
            if (this == &other) return *this;
            clear();
            reserve(other.m_size);
            for (size_type i = 0; i < other.m_size; ++i) pushBackUnchecked(other.m_data[i]);
            return *this;
        }
        small_vector& operator=(small_vector&& other) noexcept
        {
            if (this == &other) return *this;
            clear();
            releaseHeap();
            m_data = inlineData();
            m_capacity = N;
            takeFrom(other);
            return *this;
        }

        T* data() { return m_data; }
        const T* data() const { return m_data; }
        size_type size() const { return m_size; }
        size_type capacity() const { return m_capacity; }
        bool empty() const { return m_size == 0; }
        bool isInline() const { return m_data == inlineData(); }

        iterator begin() { return m_data; }
        iterator end() { return m_data + m_size; }
        const_iterator begin() const { return m_data; }
        const_iterator end() const { return m_data + m_size; }

        T& operator[](size_type index) { assert(index < m_size); return m_data[index]; }
        const T& operator[](size_type index) const { assert(index < m_size); return m_data[index]; }
        T& front() { assert(m_size > 0); return m_data[0]; }
        const T& front() const { assert(m_size > 0); return m_data[0]; }
        T& back() { assert(m_size > 0); return m_data[m_size - 1]; }
        const T& back() const { assert(m_size > 0); return m_data[m_size - 1]; }

        void reserve(size_type wanted)
        {
            if (wanted <= m_capacity) return;
            if constexpr (!AllowHeap)
            {
                assert(false && "fixed_vector capacity exceeded");
                return;
            }
            else
            {
                const size_type grown = wanted > m_capacity * 2 ? wanted : m_capacity * 2;
                T* fresh = allocator<T>().allocate(grown);
                for (size_type i = 0; i < m_size; ++i)
                {
                    ::new (static_cast<void*>(fresh + i)) T(move(m_data[i]));
                    m_data[i].~T();
                }
                releaseHeap();
                m_data = fresh;
                m_heap = fresh;
                m_capacity = grown;
            }
        }

        void push_back(const T& value) { if (!makeRoom()) return; pushBackUnchecked(value); }
        void push_back(T&& value) { if (!makeRoom()) return; ::new (static_cast<void*>(m_data + m_size)) T(move(value)); ++m_size; }

        template<typename... Args>
        T& emplace_back(Args&&... args)
        {
            if (!makeRoom()) return back();
            ::new (static_cast<void*>(m_data + m_size)) T(forward<Args>(args)...);
            return m_data[m_size++];
        }

        void pop_back() { assert(m_size > 0); m_data[--m_size].~T(); }

        void resize(size_type wanted)
        {
            if (wanted > m_size)
            {
                reserve(wanted);
                if (wanted > m_capacity) wanted = m_capacity; // refused by fixed_vector
                while (m_size < wanted) { ::new (static_cast<void*>(m_data + m_size)) T(); ++m_size; }
            }
            else while (m_size > wanted) pop_back();
        }

        void clear() { while (m_size > 0) pop_back(); }

        iterator erase(iterator at)
        {
            assert(at >= begin() && at < end());
            for (T* it = at; it + 1 != end(); ++it) *it = move(*(it + 1));
            pop_back();
            return at;
        }

    private:
        T* inlineData() { return reinterpret_cast<T*>(m_inline); }
        const T* inlineData() const { return reinterpret_cast<const T*>(m_inline); }

        void pushBackUnchecked(const T& value) { ::new (static_cast<void*>(m_data + m_size)) T(value); ++m_size; }
        bool makeRoom()
        {
            if (m_size < m_capacity) return true;
            reserve(m_size + 1);
            return m_size < m_capacity;
        }
        void releaseHeap()
        {
            if (m_heap == nullptr) return;
            allocator<T>().deallocate(m_heap, m_capacity);
            m_heap = nullptr;
        }
        // A heap block is stolen outright; inline elements have to be moved one by one.
        void takeFrom(small_vector& other) noexcept
        {
            if (other.m_heap != nullptr)
            {
                m_data = other.m_data;
                m_heap = other.m_heap;
                m_capacity = other.m_capacity;
                m_size = other.m_size;
                other.m_data = other.inlineData();
                other.m_heap = nullptr;
                other.m_capacity = N;
                other.m_size = 0;
                return;
            }
            for (size_type i = 0; i < other.m_size; ++i)
                ::new (static_cast<void*>(m_data + i)) T(move(other.m_data[i]));
            m_size = other.m_size;
            other.clear();
        }

        alignas(T) unsigned char m_inline[N * sizeof(T)];
        T* m_data = inlineData();
        T* m_heap = nullptr;          // null while the inline block is in use
        size_type m_capacity = N;
        size_type m_size = 0;
    };

    template<typename T, size_t N> using fixed_vector = small_vector<T, N, false>;
}
