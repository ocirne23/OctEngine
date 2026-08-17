export module Core.CheckedPtr;

import Core;

constexpr static size_t MAX_ITEMS_PER_BUCKET = 20;

export template<size_t Size>
class RefCountTracker
{
private:

	struct Bucket
	{
		std::shared_mutex mutex;
		oc::vector<uint64> data;
	};
	Bucket m_buckets[Size];

public:

	void incrementRefCount(const void* obj)
	{
		const size_t key = reinterpret_cast<size_t>(obj) & 0x0000'FFFF'FFFF'FFFF;
		const size_t h = oc::hash<size_t>{}(key);
		const size_t bucketIdx = h % Size;
		Bucket& bucket = m_buckets[bucketIdx];
		std::shared_lock readLock(bucket.mutex);
		for (uint64& item : bucket.data)
		{
			if ((item & 0x0000'FFFF'FFFF'FFFF) == key)
			{
				assert(item < 0xFFFF'0000'0000'0000 && "RefCheckable reference count overflow!");
				item += 0x0001'0000'0000'0000;
				return;
			}
		}
		assert(false && "RefCheckable object not registered!");
	}

	void decrementRefCount(const void* obj)
	{
		const size_t key = reinterpret_cast<size_t>(obj) & 0x0000'FFFF'FFFF'FFFF;
		const size_t h = oc::hash<size_t>{}(key);
		const size_t bucketIdx = h % Size;
		Bucket& bucket = m_buckets[bucketIdx];
		std::shared_lock readLock(bucket.mutex);
		for (uint64& item : bucket.data)
		{
			if ((item & 0x0000'FFFF'FFFF'FFFF) == key)
			{
				assert(item >= 0x0001'0000'0000'0000 && "RefCheckable reference count overflow!");
				item -= 0x0001'0000'0000'0000;
				return;
			}
		}
		assert(false && "RefCheckable object not registered!");
	}

	void insert(const void* obj)
	{
		const size_t key = reinterpret_cast<size_t>(obj) & 0x0000'FFFF'FFFF'FFFF;
		const size_t h = oc::hash<size_t>{}(key);
		const size_t bucketIdx = h % Size;
		Bucket& bucket = m_buckets[bucketIdx];
		{
			std::lock_guard writeLock(bucket.mutex);
			bucket.data.emplace_back(key);
			// maybe assert for unique
		}
		assert(bucket.data.size() < MAX_ITEMS_PER_BUCKET && "Too many items in bucket, consider increasing num buckets");
	}

	void remove(const void* obj)
	{
		const size_t key = reinterpret_cast<size_t>(obj) & 0x0000'FFFF'FFFF'FFFF;
		const size_t h = oc::hash<size_t>{}(key);
		const size_t bucketIdx = h % Size;
		Bucket& bucket = m_buckets[bucketIdx];
		{
			std::lock_guard writeLock(bucket.mutex);
			const int size = (int)bucket.data.size();
			for (int i = 0; i < size; ++i)
			{
				const uint64 item = bucket.data[i];
				if ((item & 0x0000'FFFF'FFFF'FFFF) == key)
				{
					assert(item <= 0x0000'FFFF'FFFF'FFFF && "RefCheckable object destroyed while still having references!");
					if (i != size - 1)
						bucket.data[i] = bucket.data[size - 1];
					bucket.data.pop_back();
					return;
				}
			}
			assert(false && "RefCheckable object has already been destroyed!");
		}
	}

	uint16 getRefCount(const void* obj)
	{
		const size_t key = reinterpret_cast<size_t>(obj) & 0x0000'FFFF'FFFF'FFFF;
		const size_t h = oc::hash<size_t>{}(key);
		const size_t bucketIdx = h % Size;
		Bucket& bucket = m_buckets[bucketIdx];
		std::shared_lock readLock(bucket.mutex);
		for (uint64& item : bucket.data)
		{
			if ((item & 0x0000'FFFF'FFFF'FFFF) == key)
			{
				return uint16((item & 0xFFFF'0000'0000'0000) >> 48);
			}
		}
		assert(false && "RefCheckable object not registered!");
		return uint16(0);
	}
};

// Fit to 4k page
alignas(4096) RefCountTracker<102> g_refCounter;
static_assert(sizeof(g_refCounter) <= 4096);

inline void registerRefCheckable(const void* obj)
{
	g_refCounter.insert(obj);
}

inline void unregisterRefCheckable(const void* obj)
{
	g_refCounter.remove(obj);
}

inline void addRefCount(const void* obj)
{
	g_refCounter.incrementRefCount(obj);
}

inline void removeRefCount(const void* obj)
{
	g_refCounter.decrementRefCount(obj);
}

inline uint16 getRefCount(const void* obj)
{
	return g_refCounter.getRefCount(obj);
}

export class RefCheckable
{
public:

	RefCheckable()                                { registerRefCheckable(this); }
	RefCheckable(const RefCheckable& copy)        { registerRefCheckable(this); }
	RefCheckable(const RefCheckable&& move)       { registerRefCheckable(this); }
	RefCheckable& operator=(const RefCheckable&)  { return *this; }
	RefCheckable& operator=(const RefCheckable&&) { return *this; }
	~RefCheckable()                               { unregisterRefCheckable(this); }
};

export template <typename T>
concept IsRefCheckable = std::is_base_of<RefCheckable, T>::value;

export template<IsRefCheckable T>
class CheckedPtr
{
public:
	CheckedPtr()                       : m_ref(nullptr)    {}
	CheckedPtr(T& ref)                 : m_ref(&ref)       { addRefCount(m_ref); }
	CheckedPtr(T* ref)                 : m_ref(ref)        { addRefCount(m_ref); }
	CheckedPtr(const CheckedPtr& copy) : m_ref(copy.m_ref) { addRefCount(m_ref); }
	CheckedPtr(CheckedPtr&& move)      : m_ref(move.m_ref) { move.m_ref = nullptr; }
	~CheckedPtr()                                          { if (m_ref) removeRefCount(m_ref); }
	T& operator*() const                                   { return *m_ref; }
	T* operator->() const                                  { return m_ref; }

	CheckedPtr& operator=(const CheckedPtr& copy)
	{
		if (m_ref != nullptr && m_ref != copy.m_ref)
		{
			removeRefCount(m_ref);
		}
		m_ref = copy.m_ref;
		if (copy.m_ref != nullptr)
		{
			addRefCount(m_ref);
		}
		return *this;
	}

	CheckedPtr& operator=(CheckedPtr&& move)
	{
		if (m_ref != nullptr && m_ref != move.m_ref)
		{
			removeRefCount(m_ref);
		}
		m_ref = move.m_ref;
		move.m_ref = nullptr;
		return *this;
	}

private:
	T* m_ref;
};

struct Test : RefCheckable
{
	int foo = 0;
	uint16 blah() { return getRefCount(this); }
};

export void testCheckedPtr()
{
	{
		Test t1;
		CheckedPtr<Test> ref1(t1);
		assert(getRefCount(&t1) == 1);
		{
			CheckedPtr<Test> ref2 = ref1;
			assert(getRefCount(&t1) == 2);
		}
		Test t2 = t1;
		{
			CheckedPtr<Test> ref2(t2);
			assert(getRefCount(&t2) == 1);
			{
				CheckedPtr<Test> ref3(t2);
				assert(getRefCount(&t2) == 2);
			}
		}
		{
			Test t3 = oc::move(t2);
			{
				CheckedPtr<Test> ref3(t3);
				assert(getRefCount(&t3) == 1);
				CheckedPtr<Test> ref4 = oc::move(ref3);
				assert(getRefCount(&t3) == 1);
			}
			assert(getRefCount(&t3) == 0);
		}

		CheckedPtr<Test> ref4;
		{
			Test t4;
			ref4 = t4;
		}
	}
}