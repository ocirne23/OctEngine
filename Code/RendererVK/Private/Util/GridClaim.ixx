export module RendererVK:GridClaim;

import Core;
import Core.glm;

// The lock-free CPU side of a world-space hash grid, shared by the light grid and the force grid:
// world cells keyed by integer position are CLAIMED from any thread (bump-allocated slots, one CAS
// per table entry), and (slot, item) TOUCHES are appended in per-item blocks. Both pipelines then
// counting-sort the touches into per-slot lists. The claim table IS the GPU table (the hash is
// getPositionHash in hash_grid.inc.glsl, bit for bit, and the sizes are equal): the upload writes
// table() with each slot replaced by its data offset, so the shader readers are unchanged.
//
// Capacity is fixed within a frame: a claim past it returns INVALID_SLOT and a block past the touch
// capacity INVALID_BEGIN, while the cursors keep counting so the frame's DEMAND is exact and the
// owner grows everything between frames. The overflow list collects the items to re-walk serially.
export class GridClaim final
{
public:
	static constexpr uint32 INVALID_SLOT = 0xFFFFFFFFu;
	static constexpr uint32 EMPTY_ENTRY = 0xFFFFFFFFu;  // shared.inc.glsl
	static constexpr uint32 DEAD_PAYLOAD = 0xFFFFFFFFu; // a slot that lost the publish race for its position

	// getPositionHash in hash_grid.inc.glsl (uint32 wraparound == GLSL uint).
	static uint32 positionHash(const glm::ivec3& p)
	{
		const uint32 x = uint32(p.x) * 1597334673u;
		const uint32 y = uint32(p.y) * 3812015801u;
		const uint32 z = uint32(p.z) * 2798796415u;
		uint32 n = x ^ y ^ z;
		n = n * 747796405u + 2891336453u;
		n = ((n >> ((n >> 28u) + 4u)) ^ n) * 277803737u;
		n = (n >> 22u) ^ n;
		return n;
	}

	// While nothing claims. The table is 4x the slots (<= 25% load: probes never wrap). The live
	// slots are REHASHED into the new table, so a growth between a build and its upload keeps them.
	void resize(uint32 slotCapacity)
	{
		const uint32 numLive = numSlots();
		m_capacity = slotCapacity;
		m_pos.resize(slotCapacity);
		m_payload.resize(slotCapacity);
		m_table.assign(slotCapacity * 4, EMPTY_ENTRY);
		const uint32 mask = (uint32)m_table.size() - 1;
		for (uint32 slot = 0; slot < numLive; ++slot)
		{
			if (isDead(slot))
				continue;
			uint32 idx = positionHash(m_pos[slot]) & mask;
			while (m_table[idx] != EMPTY_ENTRY)
				idx = (idx + 1) & mask;
			m_table[idx] = slot;
		}
	}
	// THE hash table, slot indices: same hash, same linear probing and no deletions, so the GPU
	// table is this one with every slot replaced by its data offset - no second build.
	oc::span<const uint32> table() const { return m_table; }
	void beginFrame()
	{
		m_cursor.store(0, oc::memory_order_relaxed);
		memset(m_table.data(), 0xFF, m_table.size() * sizeof(uint32));
	}

	// Any thread: the slot of `pos`, claiming it if new (payloadOnInsert() runs once per NEW slot,
	// e.g. the LOD cell size - it depends only on the position, so every claimer agrees).
	template<typename PayloadFn>
	uint32 claim(const glm::ivec3& pos, PayloadFn&& payloadOnInsert)
	{
		const uint32 mask = (uint32)m_table.size() - 1;
		uint32 idx = positionHash(pos) & mask;
		uint32 mySlot = INVALID_SLOT; // allocated lazily, on the first EMPTY probe
		while (true)
		{
			oc::atomic_ref<uint32> entry(m_table[idx]);
			uint32 seen = entry.load(oc::memory_order_acquire);
			if (seen == EMPTY_ENTRY)
			{
				if (mySlot == INVALID_SLOT)
				{
					mySlot = m_cursor.fetch_add(1, oc::memory_order_relaxed);
					if (mySlot >= m_capacity)
						return INVALID_SLOT; // the cursor keeps counting: slotDemand() is the truth
					m_pos[mySlot] = pos;
					m_payload[mySlot] = payloadOnInsert();
				}
				if (entry.compare_exchange_strong(seen, mySlot, oc::memory_order_acq_rel, oc::memory_order_acquire))
					return mySlot;
				// Lost the race for this entry: `seen` holds the winner, fall through.
			}
			if (m_pos[seen] == pos)
			{
				if (mySlot != INVALID_SLOT)
					m_payload[mySlot] = DEAD_PAYLOAD; // another thread published this position first
				return seen;
			}
			idx = (idx + 1) & mask;
		}
	}

	// After the last claim.
	uint32 numSlots() const { return glm::min(m_cursor.load(oc::memory_order_acquire), m_capacity); }
	uint32 slotDemand() const { return m_cursor.load(oc::memory_order_acquire); }
	uint32 capacity() const { return m_capacity; }
	const glm::ivec3& pos(uint32 slot) const { return m_pos[slot]; }
	uint32 payload(uint32 slot) const { return m_payload[slot]; }
	bool isDead(uint32 slot) const { return m_payload[slot] == DEAD_PAYLOAD; }

private:
	uint32 m_capacity = 0;
	oc::vector<glm::ivec3> m_pos;
	oc::vector<uint32> m_payload;
	oc::vector<uint32> m_table;
	oc::atomic<uint32> m_cursor{ 0 };
};

export class GridTouches final
{
public:
	struct Touch
	{
		uint32 slot;
		uint32 item; // SKIPPED = a retracted block (the item went to the overflow list)
	};
	static constexpr uint32 INVALID_BEGIN = 0xFFFFFFFFu;
	static constexpr uint32 SKIPPED = 0xFFFFFFFFu;

	void resize(uint32 touchCapacity, uint32 overflowCapacity)
	{
		m_touches.resize(touchCapacity);
		m_overflow.resize(overflowCapacity);
	}
	void beginFrame()
	{
		m_cursor.store(0, oc::memory_order_relaxed);
		m_overflowCursor.store(0, oc::memory_order_relaxed);
	}
	// Any thread: n consecutive touches, or INVALID_BEGIN (the cursor still counts for touchDemand()).
	uint32 claimBlock(uint32 n)
	{
		const uint32 begin = m_cursor.fetch_add(n, oc::memory_order_relaxed);
		return begin + n <= (uint32)m_touches.size() ? begin : INVALID_BEGIN;
	}
	Touch* data() { return m_touches.data(); }
	void pushOverflow(uint32 item) { m_overflow[m_overflowCursor.fetch_add(1, oc::memory_order_relaxed)] = item; }

	// After the last claim.
	uint32 numTouches() const { return glm::min(m_cursor.load(oc::memory_order_acquire), (uint32)m_touches.size()); }
	uint32 touchDemand() const { return m_cursor.load(oc::memory_order_acquire); }
	oc::span<const uint32> overflow() const { return oc::span<const uint32>(m_overflow.data(), m_overflowCursor.load(oc::memory_order_acquire)); }
	// Between frames: fit the demand with headroom (memory follows the peak, never freed).
	void growToDemand()
	{
		const uint32 demand = touchDemand();
		if (demand <= (uint32)m_touches.size())
			return;
		uint32 size = (uint32)m_touches.size();
		while (size < demand + demand / 2)
			size *= 2;
		m_touches.resize(size);
	}

private:
	oc::vector<Touch> m_touches;
	oc::atomic<uint32> m_cursor{ 0 };
	oc::vector<uint32> m_overflow;
	oc::atomic<uint32> m_overflowCursor{ 0 };
};
