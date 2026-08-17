export module Core.LPMultiMap;

import Core;

// Hash map using open addressing with linear probing for collision resolution. All entries are
// stored inline in one flat, contiguous array (no per-entry heap allocation / node chaining),
// which keeps lookups cache-friendly. Erased entries are marked as tombstones so that probe
// sequences for other keys are not broken; tombstones are purged whenever the table grows, or
// compacted in place if growing isn't otherwise needed. Capacity is always a power of two so the
// home bucket for a hash can be computed with a bitmask instead of a modulo. Duplicate keys are
// allowed: insert()/emplace() always add a new entry, while equalRange(), count() and erase(key)
// traverse or remove every entry that shares a key. insertOrAssign() and operator[] are the
// exceptions, each only ever touching a single (the first found) entry per key.
export template<
	typename Key,
	typename Value,
	typename Hash = oc::hash<Key>,
	typename KeyEqual = oc::equal_to<Key>>
class LPMultiMap final
{
public:

	using KeyType   = Key;
	using ValueType = Value;
	using EntryType = oc::pair<const Key, Value>;

private:

	enum class ESlotState : uint8 { Empty, Occupied, Deleted };

	struct Slot
	{
		alignas(EntryType) uint8 storage[sizeof(EntryType)];
		ESlotState state = ESlotState::Empty;

		EntryType* entry() { return std::launder(reinterpret_cast<EntryType*>(storage)); }
		const EntryType* entry() const { return std::launder(reinterpret_cast<const EntryType*>(storage)); }
	};

	template<bool IsConst>
	class IteratorImpl
	{
	public:

		using SlotPtr   = std::conditional_t<IsConst, const Slot*, Slot*>;
		using reference = std::conditional_t<IsConst, const EntryType&, EntryType&>;
		using pointer   = std::conditional_t<IsConst, const EntryType*, EntryType*>;

		IteratorImpl() = default;
		IteratorImpl(SlotPtr pSlot, SlotPtr pEnd) : m_pSlot(pSlot), m_pEnd(pEnd) { skipUnoccupied(); }

		operator IteratorImpl<true>() const { return IteratorImpl<true>(m_pSlot, m_pEnd); }

		reference operator*()  const { return *m_pSlot->entry(); }
		pointer   operator->() const { return m_pSlot->entry(); }

		IteratorImpl& operator++()
		{
			++m_pSlot;
			skipUnoccupied();
			return *this;
		}

		IteratorImpl operator++(int)
		{
			IteratorImpl tmp = *this;
			++(*this);
			return tmp;
		}

		bool operator==(const IteratorImpl& other) const { return m_pSlot == other.m_pSlot; }
		bool operator!=(const IteratorImpl& other) const { return m_pSlot != other.m_pSlot; }

	private:
		friend class LPMultiMap;

		void skipUnoccupied()
		{
			while (m_pSlot != m_pEnd && m_pSlot->state != ESlotState::Occupied)
				++m_pSlot;
		}

		SlotPtr m_pSlot = nullptr;
		SlotPtr m_pEnd  = nullptr;
	};

	// Iterates every entry whose key compares equal to a target key by walking the probe
	// sequence starting at that key's home bucket. Stops at the first Empty slot; tombstones
	// and occupied slots holding a different key are skipped over rather than stopping
	// iteration, since a matching entry may have been pushed further along the sequence by an
	// earlier collision.
	template<bool IsConst>
	class KeyIteratorImpl
	{
	public:

		using SlotPtr   = std::conditional_t<IsConst, const Slot*, Slot*>;
		using reference = std::conditional_t<IsConst, const EntryType&, EntryType&>;
		using pointer   = std::conditional_t<IsConst, const EntryType*, EntryType*>;

		KeyIteratorImpl() = default;
		KeyIteratorImpl(SlotPtr pTable, size_t mask, size_t startIndex, const Key& key, const KeyEqual& keyEqual)
			: m_pTable(pTable), m_mask(mask), m_index(startIndex), m_key(key), m_keyEqual(keyEqual)
		{
			skipMismatches();
		}

		operator KeyIteratorImpl<true>() const { return KeyIteratorImpl<true>(m_pTable, m_mask, m_index, m_key, m_keyEqual); }

		reference operator*()  const { return *m_pTable[m_index].entry(); }
		pointer   operator->() const { return m_pTable[m_index].entry(); }

		KeyIteratorImpl& operator++()
		{
			m_index = (m_index + 1) & m_mask;
			skipMismatches();
			return *this;
		}

		KeyIteratorImpl operator++(int)
		{
			KeyIteratorImpl tmp = *this;
			++(*this);
			return tmp;
		}

		bool operator==(const KeyIteratorImpl& other) const { return m_pTable == other.m_pTable && m_index == other.m_index; }
		bool operator!=(const KeyIteratorImpl& other) const { return !(*this == other); }

	private:
		friend class LPMultiMap;

		void skipMismatches()
		{
			while (m_pTable != nullptr && m_pTable[m_index].state != ESlotState::Empty)
			{
				if (m_pTable[m_index].state == ESlotState::Occupied && m_keyEqual(m_pTable[m_index].entry()->first, m_key))
					return;
				m_index = (m_index + 1) & m_mask;
			}
			m_pTable = nullptr;
			m_index = 0;
		}

		SlotPtr  m_pTable = nullptr;
		size_t   m_mask = 0;
		size_t   m_index = 0;
		Key      m_key{};
		KeyEqual m_keyEqual{};
	};

	// Lightweight begin()/end() view over a run of equal keys, returned by equalRange().
	template<bool IsConst>
	class KeyRange
	{
	public:

		using Iter = KeyIteratorImpl<IsConst>;

		KeyRange() = default;
		explicit KeyRange(Iter first) : m_first(first) {}

		Iter begin() const { return m_first; }
		Iter end()   const { return Iter(); }

	private:
		Iter m_first;
	};

public:

	using iterator       = IteratorImpl<false>;
	using const_iterator = IteratorImpl<true>;
	using KeyIterator      = KeyIteratorImpl<false>;
	using ConstKeyIterator = KeyIteratorImpl<true>;

	LPMultiMap() = default;
	explicit LPMultiMap(size_t initialCapacity) { reserve(initialCapacity); }
	LPMultiMap(oc::initializer_list<EntryType> init)
	{
		reserve(init.size());
		for (const EntryType& kv : init)
			insert(kv.first, kv.second);
	}
	LPMultiMap(const LPMultiMap& other) { *this = other; }
	LPMultiMap(LPMultiMap&& other) noexcept { *this = oc::move(other); }
	~LPMultiMap() { clear(); }

	LPMultiMap& operator=(const LPMultiMap& other)
	{
		if (this == &other)
			return *this;
		clear();
		m_slots = oc::vector<Slot>(other.m_slots.size());
		const size_t mask = m_slots.size() - 1;
		for (const Slot& srcSlot : other.m_slots) // reinsert (rather than positionally copy) so tombstones in 'other' are dropped for free
		{
			if (srcSlot.state != ESlotState::Occupied)
				continue;
			size_t index = slotIndexForHash(m_hasher(srcSlot.entry()->first));
			while (m_slots[index].state == ESlotState::Occupied)
				index = (index + 1) & mask;
			new (m_slots[index].storage) EntryType(*srcSlot.entry());
			m_slots[index].state = ESlotState::Occupied;
			++m_size;
		}
		return *this;
	}

	LPMultiMap& operator=(LPMultiMap&& other) noexcept
	{
		if (this == &other)
			return *this;
		clear();
		m_slots = oc::move(other.m_slots);
		m_size = other.m_size;
		m_tombstones = other.m_tombstones;
		other.m_size = 0;
		other.m_tombstones = 0;
		return *this;
	}

	// ---- Iteration --------------------------------------------------------------------------

	iterator begin() { return iterator(m_slots.data(), endSlot()); }
	iterator end()   { return iterator(endSlot(), endSlot()); }
	const_iterator begin() const { return const_iterator(m_slots.data(), endSlot()); }
	const_iterator end()   const { return const_iterator(endSlot(), endSlot()); }

	// ---- Capacity ---------------------------------------------------------------------------

	bool empty() const { return m_size == 0; }
	size_t size() const { return m_size; }
	size_t capacity() const { return m_slots.size(); }
	float loadFactor() const { return m_slots.empty() ? 0.0f : (float)m_size / (float)m_slots.size(); }

	void reserve(size_t minCapacity)
	{
		if (minCapacity == 0)
			return;
		const size_t wantedCapacity = (size_t)((float)minCapacity / kMaxLoadFactor) + 1;
		const size_t newCapacity = nextPowerOfTwo(wantedCapacity < kMinCapacity ? kMinCapacity : wantedCapacity);
		if (newCapacity > m_slots.size())
			rehash(newCapacity);
	}

	// ---- Modifiers --------------------------------------------------------------------------

	void clear()
	{
		for (Slot& slot : m_slots)
		{
			if (slot.state == ESlotState::Occupied)
				slot.entry()->~EntryType();
			slot.state = ESlotState::Empty;
		}
		m_size = 0;
		m_tombstones = 0;
	}

	// Always inserts a new entry, even if 'key' already exists - duplicate keys are allowed, so
	// unlike insertOrAssign()/operator[] this never looks for (or overwrites) an existing match.
	// Use equalRange()/find() afterwards to look up or enumerate entries sharing a key.
	iterator insert(const Key& key, const Value& value) { return emplace(key, value); }
	iterator insert(const Key& key, Value&& value)      { return emplace(key, oc::move(value)); }

	template<typename... Args>
	iterator emplace(const Key& key, Args&&... args)
	{
		Slot* pSlot = prepareSlotForInsert(key);
		new (pSlot->storage) EntryType(key, Value(oc::forward<Args>(args)...));
		pSlot->state = ESlotState::Occupied;
		++m_size;
		return iterator(pSlot, endSlot());
	}

	// Assigns to the first existing entry matching 'key', or inserts a new one if there is no
	// match; never creates a second entry for a key that is already present.
	template<typename... Args>
	oc::pair<iterator, bool> insertOrAssign(const Key& key, Args&&... args)
	{
		auto [pSlot, found] = prepareSlotForUpsert(key);
		if (found)
		{
			pSlot->entry()->second = Value(oc::forward<Args>(args)...);
			return { iterator(pSlot, endSlot()), false };
		}
		new (pSlot->storage) EntryType(key, Value(oc::forward<Args>(args)...));
		pSlot->state = ESlotState::Occupied;
		++m_size;
		return { iterator(pSlot, endSlot()), true };
	}

	// Gets the value of the first entry matching 'key', default-constructing (and inserting) one
	// if there is no match. As with insertOrAssign(), this only ever touches a single entry.
	Value& operator[](const Key& key)
	{
		auto [pSlot, found] = prepareSlotForUpsert(key);
		if (!found)
		{
			new (pSlot->storage) EntryType(key, Value());
			pSlot->state = ESlotState::Occupied;
			++m_size;
		}
		return pSlot->entry()->second;
	}

	// Erases every entry matching 'key' and returns how many were removed. Walks the same probe
	// sequence as findSlotForLookup(), but keeps going (skipping past mismatches and tombstones)
	// instead of stopping at the first match, since duplicate entries may be present.
	size_t erase(const Key& key)
	{
		if (m_slots.empty())
			return 0;
		const size_t mask = m_slots.size() - 1;
		size_t index = slotIndexForHash(m_hasher(key));
		size_t erasedCount = 0;
		for (size_t probes = 0; probes <= mask; ++probes)
		{
			const ESlotState state = m_slots[index].state;
			if (state == ESlotState::Empty)
				break;
			if (state == ESlotState::Occupied && m_keyEqual(m_slots[index].entry()->first, key))
			{
				eraseSlot(index);
				++erasedCount;
			}
			index = (index + 1) & mask;
		}
		return erasedCount;
	}

	iterator erase(iterator it)
	{
		const size_t index = it.m_pSlot - m_slots.data();
		eraseSlot(index);
		return iterator(m_slots.data() + index, endSlot()); // ctor skips forward past the now-deleted slot
	}

	// Erases the entry currently referenced by a key-scoped iterator (see equalRange()) and
	// returns an iterator to the next entry sharing that same key, if any. Distinct from
	// erase(iterator) because KeyIterator is a different type from iterator: it only walks one
	// key's probe chain rather than the whole table, so advancing past the erased slot means
	// resuming that same key-scoped traversal rather than the full-table one.
	KeyIterator erase(KeyIterator it)
	{
		const size_t index = it.m_index;
		eraseSlot(index);
		it.m_index = (index + 1) & it.m_mask;
		it.skipMismatches();
		return it;
	}

	void eraseOne(KeyIterator it)
	{
		const size_t index = it.m_index;
		eraseSlot(index);
	}

	void swap(LPMultiMap& other) noexcept
	{
		oc::swap(m_slots, other.m_slots);
		oc::swap(m_size, other.m_size);
		oc::swap(m_tombstones, other.m_tombstones);
	}

	// ---- Lookup -----------------------------------------------------------------------------

	// Returns the first entry matching 'key' encountered along its probe sequence, or end() if
	// there is no match. If duplicate keys are present, use equalRange() to reach the others.
	iterator find(const Key& key)
	{
		if (m_slots.empty())
			return end();
		const size_t index = findSlotForLookup(key);
		return index == kInvalidIndex ? end() : iterator(&m_slots[index], endSlot());
	}

	const_iterator find(const Key& key) const
	{
		if (m_slots.empty())
			return end();
		const size_t index = findSlotForLookup(key);
		return index == kInvalidIndex ? end() : const_iterator(&m_slots[index], endSlot());
	}

	// Returns a range over every entry whose key compares equal to 'key' (see KeyIteratorImpl
	// above for how the traversal itself works). Since insert()/emplace() allow duplicate keys,
	// this may yield more than one entry, in the order they occupy the shared probe sequence.
	KeyRange<false> equalRange(const Key& key)
	{
		if (m_slots.empty())
			return {};
		return KeyRange<false>(KeyIteratorImpl<false>(m_slots.data(), m_slots.size() - 1, slotIndexForHash(m_hasher(key)), key, m_keyEqual));
	}

	KeyRange<true> equalRange(const Key& key) const
	{
		if (m_slots.empty())
			return {};
		return KeyRange<true>(KeyIteratorImpl<true>(m_slots.data(), m_slots.size() - 1, slotIndexForHash(m_hasher(key)), key, m_keyEqual));
	}

	bool contains(const Key& key) const { return !m_slots.empty() && findSlotForLookup(key) != kInvalidIndex; }

	// Counts every entry matching 'key' by walking its probe sequence (see erase() above).
	size_t count(const Key& key) const
	{
		if (m_slots.empty())
			return 0;
		const size_t mask = m_slots.size() - 1;
		size_t index = slotIndexForHash(m_hasher(key));
		size_t result = 0;
		for (size_t probes = 0; probes <= mask; ++probes)
		{
			const Slot& slot = m_slots[index];
			if (slot.state == ESlotState::Empty)
				break;
			if (slot.state == ESlotState::Occupied && m_keyEqual(slot.entry()->first, key))
				++result;
			index = (index + 1) & mask;
		}
		return result;
	}

	// Returns the value of the first entry matching 'key' (see find()); asserts if there is no
	// match.
	Value& at(const Key& key)
	{
		iterator it = find(key);
		assert(it != end() && "LPMultiMap::at: key not found!");
		return it->second;
	}

	const Value& at(const Key& key) const
	{
		const_iterator it = find(key);
		assert(it != end() && "LPMultiMap::at: key not found!");
		return it->second;
	}

private:

	static constexpr size_t kInvalidIndex  = ~size_t(0);
	static constexpr size_t kMinCapacity   = 16;
	static constexpr float  kMaxLoadFactor = 0.7f;

	Slot* endSlot() { return m_slots.data() + m_slots.size(); }
	const Slot* endSlot() const { return m_slots.data() + m_slots.size(); }

	static size_t nextPowerOfTwo(size_t n)
	{
		size_t p = 1;
		while (p < n)
			p <<= 1;
		return p;
	}

	size_t slotIndexForHash(size_t hash) const { return hash & (m_slots.size() - 1); }

	// Returns the index of the occupied slot holding 'key', or kInvalidIndex if not present.
	// Probing stops at the first Empty slot: tombstones (Deleted) never terminate a probe
	// sequence, since a key may have been pushed past them by an earlier collision.
	size_t findSlotForLookup(const Key& key) const
	{
		const size_t mask = m_slots.size() - 1;
		size_t index = slotIndexForHash(m_hasher(key));
		for (size_t probes = 0; probes <= mask; ++probes)
		{
			const Slot& slot = m_slots[index];
			if (slot.state == ESlotState::Empty)
				return kInvalidIndex;
			if (slot.state == ESlotState::Occupied && m_keyEqual(slot.entry()->first, key))
				return index;
			index = (index + 1) & mask;
		}
		return kInvalidIndex;
	}

	// Returns the first empty/tombstone slot along key's probe sequence (reusing the earliest
	// tombstone seen, if any), without regard to whether an existing entry already matches 'key'
	// - duplicate keys are allowed, so a new entry is simply placed at the next free slot in the
	// same probe chain as any existing entries for that key.
	size_t findSlotForNewEntry(const Key& key) const
	{
		const size_t mask = m_slots.size() - 1;
		size_t index = slotIndexForHash(m_hasher(key));
		for (size_t probes = 0; probes <= mask; ++probes)
		{
			if (m_slots[index].state != ESlotState::Occupied)
				return index;
			index = (index + 1) & mask;
		}
		return kInvalidIndex;
	}

	// Returns { index, found }: if found, index names the slot holding the first existing entry
	// matching 'key'; otherwise it names the first empty/tombstone slot along the probe sequence
	// (reusing the earliest tombstone seen, if any).
	oc::pair<size_t, bool> findSlotForUpsert(const Key& key) const
	{
		const size_t mask = m_slots.size() - 1;
		size_t index = slotIndexForHash(m_hasher(key));
		size_t firstAvailable = kInvalidIndex;
		for (size_t probes = 0; probes <= mask; ++probes)
		{
			const Slot& slot = m_slots[index];
			if (slot.state == ESlotState::Empty)
				return { firstAvailable == kInvalidIndex ? index : firstAvailable, false };
			if (slot.state == ESlotState::Occupied)
			{
				if (m_keyEqual(slot.entry()->first, key))
					return { index, true };
			}
			else if (firstAvailable == kInvalidIndex) // Deleted (tombstone)
			{
				firstAvailable = index;
			}
			index = (index + 1) & mask;
		}
		return { firstAvailable, false };
	}

	// Grows the table if needed, then returns the slot a new entry for 'key' should occupy. Used
	// by insert()/emplace(), which allow duplicate keys and so never check for a match.
	Slot* prepareSlotForInsert(const Key& key)
	{
		growIfNeeded();
		const size_t index = findSlotForNewEntry(key);
		assert(index != kInvalidIndex && "LPMultiMap: no available slot (table unexpectedly full)");
		Slot& slot = m_slots[index];
		if (slot.state == ESlotState::Deleted)
			--m_tombstones;
		return &slot;
	}

	// Grows the table if needed, then locates the slot 'key' belongs in for an upsert: an
	// existing matching entry if one exists, otherwise a free slot. Used by insertOrAssign() and
	// operator[], which only ever touch a single entry per key.
	oc::pair<Slot*, bool> prepareSlotForUpsert(const Key& key)
	{
		growIfNeeded();
		const auto [index, found] = findSlotForUpsert(key);
		Slot& slot = m_slots[index];
		if (!found && slot.state == ESlotState::Deleted)
			--m_tombstones;
		return { &slot, found };
	}

	void eraseSlot(size_t index)
	{
		Slot& slot = m_slots[index];
		assert(slot.state == ESlotState::Occupied);
		slot.entry()->~EntryType();
		slot.state = ESlotState::Deleted;
		--m_size;
		++m_tombstones;
	}

	void growIfNeeded()
	{
		if (m_slots.empty())
		{
			rehash(kMinCapacity);
		}
		else if ((float)(m_size + m_tombstones + 1) > (float)m_slots.size() * kMaxLoadFactor)
		{
			// If real occupancy alone would still fit under the load factor, this rehash is only
			// needed to purge tombstones, so it compacts at the same capacity instead of growing.
			const bool needsMoreRoom = (float)(m_size + 1) > (float)m_slots.size() * kMaxLoadFactor;
			rehash(needsMoreRoom ? m_slots.size() * 2 : m_slots.size());
		}
	}

	void rehash(size_t newCapacity)
	{
		oc::vector<Slot> oldSlots = oc::move(m_slots);
		m_slots = oc::vector<Slot>(newCapacity);
		m_tombstones = 0;
		const size_t mask = newCapacity - 1;
		for (Slot& oldSlot : oldSlots)
		{
			if (oldSlot.state != ESlotState::Occupied)
				continue;
			size_t index = slotIndexForHash(m_hasher(oldSlot.entry()->first));
			while (m_slots[index].state == ESlotState::Occupied)
				index = (index + 1) & mask;
			new (m_slots[index].storage) EntryType(oc::move(*oldSlot.entry()));
			m_slots[index].state = ESlotState::Occupied;
			oldSlot.entry()->~EntryType();
		}
	}

	oc::vector<Slot> m_slots;
	size_t m_size = 0;
	size_t m_tombstones = 0;
	Hash m_hasher;
	KeyEqual m_keyEqual;
};
