export module Core.LockFreeList;

import Core;
import Core.Allocator;

export template <typename T>
class LockedList final
{
public:
    oc::list<T> m_list;
    std::mutex m_mutex;
    ~LockedList()
    {
        m_list.clear();
    }

    void push_back(T&& entry)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_list.push_back(entry);
    }

    void push_front(T&& entry)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_list.push_front(entry);
    }

    void push_list_back(oc::list<T*>& list)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (!list.empty())
        {
            m_list.splice(m_list.end(), list);
            list.clear();
        }
    }

    void push_list_front(oc::list<T>& list)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (!list.empty())
        {
            m_list.splice(m_list.begin(), list);
            list.clear();
        }
    }

    T&& pop()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_list.empty())
            return nullptr;
        T&& entry = m_list.front();
        m_list.pop_front();
        return entry;
    }

    bool empty()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_list.empty();
    }

    size_t size()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_list.size();
    }
};

template<typename DestinationType, typename SourceType> inline DestinationType alias_cast(SourceType pPtr)
{
    union
    {
        SourceType      pSrc;
        DestinationType pDst;
    } conv_union;
    conv_union.pSrc = pPtr;
    return conv_union.pDst;
}

template<typename T, typename U> U interlockedCompareExchange_128bit(T* _pAddress, const U nExchange, const U nCompare)
{
    static_assert(sizeof(T) == 16, "sizeof(T) must be 128 bit for this function to work!");
    static_assert(sizeof(U) == 16, "sizeof(T) must be 128 bit for this function to work!");
    static_assert(alignof(T) >= 16, "T must be at least 16 byte aligned for this function to work!");

    __int64 volatile* pAddress = alias_cast<__int64 volatile*>(_pAddress);
    union { U t; __int64 i64[2]; } conv_exchange = { nExchange };
    union { U t; __int64 i64[2]; } conv_compare = { nCompare };

    _InterlockedCompareExchange128(pAddress, conv_exchange.i64[1], conv_exchange.i64[0], conv_compare.i64);
    return conv_compare.t; // _InterlockedCompareExchange128 internally updates the compare values
}

template<typename T> inline T atomicLoad_128bit(T* pAddress)
{
    static_assert(sizeof(T) == 16, "sizeof(T) must be 128 bit for this function to work!");
    static_assert(alignof(T) >= 16, "T must be at least 16 byte aligned for this function to work!");

    return interlockedCompareExchange_128bit(pAddress, T(), T());
}

export struct SInterlockedLinkedListEntry
{
    SInterlockedLinkedListEntry* pNext = nullptr;
};

export struct alignas(16) SInterlockedLinkedListHeader
{
    SInterlockedLinkedListEntry* pNext = nullptr;
    uint64_t                     nSalt = 0;
};

inline void interlockedPushEntry(SInterlockedLinkedListHeader& rHeader, SInterlockedLinkedListEntry& rEntry)
{
    SInterlockedLinkedListHeader compareValue = atomicLoad_128bit(&rHeader);
    do
    {
        // Catch when code pushes the same value into a single linked list
        if (&rEntry == compareValue.pNext) __debugbreak();

        SInterlockedLinkedListHeader exchangeValue;
        exchangeValue.pNext = &rEntry;
        exchangeValue.nSalt = compareValue.nSalt + 1;
        rEntry.pNext = compareValue.pNext;

        const SInterlockedLinkedListHeader resultValue = interlockedCompareExchange_128bit(&rHeader, exchangeValue, compareValue);
        if (resultValue.pNext == compareValue.pNext && resultValue.nSalt == compareValue.nSalt)
            return; // success

        // retry with updated value
        compareValue = resultValue;

    } while (true);
}

inline void* interlockedPopEntry(SInterlockedLinkedListHeader& rHeader)
{
    // NOTE: We choose to ignore the race condition mentioned here:
    // http://blogs.microsoft.co.il/sasha/2012/08/11/windows-memory-managers-preferential-treatment-of-access-faults-in-the-interlocked-singly-linked-list-pop-implementation/
    //
    // As we control our memory, we can choose ourself to not return memory we got from a interlocked list to the OS immediatly
    SInterlockedLinkedListHeader compareValue = atomicLoad_128bit(&rHeader);
    do
    {
        if (compareValue.pNext == nullptr)
            return nullptr;

        SInterlockedLinkedListHeader exchangeValue;
        // catch the race condition if compareValue.pNext was de-commited after we have read it here
        // IF you run into this crash during debugging, you have won the lottery :), and you ignore this exception (the exception handler does the same)
        // as it rHeader will be different in the next iteration 
        exchangeValue.pNext = compareValue.pNext->pNext;
        exchangeValue.nSalt = compareValue.nSalt + 1;

        const SInterlockedLinkedListHeader resultValue = interlockedCompareExchange_128bit(&rHeader, exchangeValue, compareValue);
        if (resultValue.pNext == compareValue.pNext && resultValue.nSalt == compareValue.nSalt)
            return compareValue.pNext;

        compareValue = resultValue;

    } while (true);
}

inline void* interlockedPopWholeList(SInterlockedLinkedListHeader& rHeader)
{
    SInterlockedLinkedListHeader compareValue = atomicLoad_128bit(&rHeader);
    do
    {
        if (compareValue.pNext == nullptr)
            return nullptr;

        SInterlockedLinkedListHeader exchangeValue;
        exchangeValue.pNext = nullptr;
        exchangeValue.nSalt = compareValue.nSalt + 1;

        const SInterlockedLinkedListHeader resultValue = interlockedCompareExchange_128bit(&rHeader, exchangeValue, compareValue);
        if (resultValue.pNext == compareValue.pNext && resultValue.nSalt == compareValue.nSalt)
            return compareValue.pNext;

        compareValue = resultValue;

    } while (true);
}

inline void* interlockedPopNextEntry(SInterlockedLinkedListEntry& rEntry)
{
    return rEntry.pNext;
}

inline bool interlockedIsListEmpty(SInterlockedLinkedListHeader& rHeader)
{
    const SInterlockedLinkedListHeader compareValue = atomicLoad_128bit(&rHeader);
    if (compareValue.pNext == nullptr)
        return true;
    return false;
}

export template <typename T>
class LockFreeList final
{
private:
    using Header = SInterlockedLinkedListHeader;

    Header m_listHead;
public:

    using Entry = SInterlockedLinkedListEntry;

    LockFreeList() : m_listHead() {}

    bool empty()
    {
        return interlockedIsListEmpty(m_listHead);
    }

    void push_front(T& entry)
    {
        assert(entry.pNext == nullptr);
        interlockedPushEntry(m_listHead, *(Entry*)(((char*)&entry) + offsetof(T, pNext)));
    }

    void push_list_front(LockFreeList<T>& list)
    {
        Entry* pEntry = interlockedPopWholeList(list.m_listHead);
        while (pEntry != nullptr)
        {
            assert(pEntry->pNext == nullptr);
            interlockedPushEntry(m_listHead, *(Entry*)(((char*)pEntry) + offsetof(T, pNext)));
            pEntry = pEntry->Next;
        }
    }

    void push_list_front(oc::list<T*, Allocator::toStd<T*>>& list)
    {
        for (T* pEntry : list)
        {
            assert(pEntry->pNext == nullptr);
            interlockedPushEntry(m_listHead, *(Entry*)(((char*)pEntry) + offsetof(T, pNext)));
        }
    }

    void push_list_front(LockedList<T*>& list)
    {
        if (!list.empty())
        {
            list.m_mutex.lock();
            for (T* pEntry : list.m_list)
            {
                assert(pEntry->pNext == nullptr);
                interlockedPushEntry(m_listHead, *(Entry*)(((char*)pEntry) + offsetof(T, pNext)));
            }
            list.m_list.clear();
            list.m_mutex.unlock();
        }
        assert(list.empty());
    }

    T* pop()
    {
        Entry* pEntry = (Entry*)interlockedPopEntry(m_listHead);
        if (pEntry != nullptr)
        {
            pEntry->pNext = nullptr;
            return (T*)(((char*)pEntry) - offsetof(T, pNext));
        }
        return nullptr;
    }

    Entry* pop_list()
    {
        return interlockedPopWholeList(m_listHead);
    }
};