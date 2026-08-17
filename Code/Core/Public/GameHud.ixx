export module Core.GameHud;

import Core;
import Core.glm;

// The in-game HUD's data model: a 12-slot hotbar (default keys 1..9,0 -> slots 0..9 in one row; a
// game may relabel the keys and fold it into a GRID via setHotbarLayout -- the RTS "QWER/ASDF/ZXCV"
// pattern), plus bars (health/energy style, a fill against a max) and numeric counters stacked from
// the top left. PURE STATE -- gameplay (script thunks, C++) writes it from any thread, the UI's
// GameHudOverlay snapshots and draws it once per frame over the viewport. Bars and counters are keyed
// by their display name; insertion order is display order. Colors are linear 0..1 RGB, matching the
// DSL surface. The overlay reports each drawn slot's screen rect back (setSlotScreenRects) so
// gameplay can hit-test mouse clicks against the hotbar (slotAtScreenPos).
//
// Mutex-guarded because scripts tick on workers (see THUNK THREAD-SAFETY): every write is a short
// lock, the UI takes one snapshot copy per frame. A plain .CRT$XCU global, so it outlives ~World's
// script OnDestroy calls (plain XCU destructs last -- see InitSeg.h's teardown scheme).

export struct HudSlot
{
	oc::string label;
	int         count = 0; // > 0 draws a stack number, Minecraft-style
	bool        used = false;
};

export struct HudBar
{
	oc::string name; // key AND label
	float       value = 0.0f;
	float       maxValue = 1.0f;
	glm::vec3   color = glm::vec3(1.0f);
};

export struct HudCounter
{
	oc::string name; // key AND label
	float       value = 0.0f;
	int         decimals = 0; // 0 = integer display
	glm::vec3   color = glm::vec3(1.0f);
};

// World-anchored UI element (a health bar / info block over an entity). Gameplay PRE-PROJECTS the
// world position into viewport pixels (Camera::worldToScreen) and replaces the whole list every
// frame — no keys, no persistence; off-screen entities are simply not submitted.
export struct HudWorldLabel
{
	glm::vec2   screenPos{};      // viewport-space anchor: bar centered on x, stacked down from y
	oc::string title;            // line above the bar (empty = none)
	oc::string info;             // lines below the bar, '\n'-separated (empty = none)
	float       barValue = 0.0f;
	float       barMax = 0.0f;    // <= 0 = no bar
	glm::vec3   barColor = glm::vec3(1.0f);
	float       bar2Value = 0.0f; // optional second bar stacked under the first (e.g. stored energy)
	float       bar2Max = 0.0f;   // <= 0 = no second bar
	glm::vec3   bar2Color = glm::vec3(1.0f);
	bool        emphasized = false; // selected: drawn larger
};

export class GameHud final
{
public:

	static constexpr int NumSlots = 12;

	// ---- writes (any thread) ------------------------------------------------

	// Grid shape + the key label drawn in each slot's corner. columns <= 0 = one row. Labels past
	// the given span keep their defaults (1..9,0,-,=).
	void setHotbarLayout(int columns, oc::span<const oc::string_view> keyLabels)
	{
		const std::lock_guard lock(m_mutex);
		m_columns = glm::clamp(columns, 0, NumSlots);
		for (int i = 0; i < NumSlots && i < (int)keyLabels.size(); ++i)
			m_keyLabels[i] = keyLabels[i];
	}

	// The overlay reports where it drew each slot (viewport/window pixel space) -- or nothing when
	// the hotbar was not drawn -- so clicks can be resolved against the hotbar.
	void setSlotScreenRects(oc::span<const glm::vec4> minMax) // per slot: (minX, minY, maxX, maxY)
	{
		const std::lock_guard lock(m_mutex);
		m_slotRectsValid = minMax.size() == (size_t)NumSlots;
		if (m_slotRectsValid)
			for (int i = 0; i < NumSlots; ++i)
				m_slotRects[i] = minMax[i];
	}

	void setSlot(int index, oc::string_view label, int count)
	{
		if (index < 0 || index >= NumSlots)
			return;
		const std::lock_guard lock(m_mutex);
		m_slots[index].label = label;
		m_slots[index].count = glm::max(count, 0);
		m_slots[index].used = true;
	}

	void setSlotCount(int index, int count)
	{
		if (index < 0 || index >= NumSlots)
			return;
		const std::lock_guard lock(m_mutex);
		if (m_slots[index].used)
			m_slots[index].count = glm::max(count, 0);
	}

	void clearSlot(int index)
	{
		if (index < 0 || index >= NumSlots)
			return;
		const std::lock_guard lock(m_mutex);
		m_slots[index] = HudSlot();
	}

	void selectSlot(int index) // -1 = no slot highlighted
	{
		const std::lock_guard lock(m_mutex);
		m_selectedSlot = glm::clamp(index, -1, NumSlots - 1);
	}

	void setHotbarVisible(bool visible)
	{
		const std::lock_guard lock(m_mutex);
		m_hotbarVisible = visible;
	}

	void setBar(oc::string_view name, float value, float maxValue, const glm::vec3& color)
	{
		const std::lock_guard lock(m_mutex);
		HudBar& bar = findOrAdd(m_bars, name);
		bar.maxValue = maxValue > 0.0f ? maxValue : 1.0f;
		bar.value = glm::clamp(value, 0.0f, bar.maxValue);
		bar.color = glm::clamp(color, 0.0f, 1.0f);
	}

	void removeBar(oc::string_view name)
	{
		const std::lock_guard lock(m_mutex);
		oc::erase_if(m_bars, [name](const HudBar& b) { return b.name == name; });
	}

	void setCounter(oc::string_view name, float value, int decimals, const glm::vec3& color)
	{
		const std::lock_guard lock(m_mutex);
		HudCounter& counter = findOrAdd(m_counters, name);
		counter.value = value;
		counter.decimals = glm::clamp(decimals, 0, 6);
		counter.color = glm::clamp(color, 0.0f, 1.0f);
	}

	void removeCounter(oc::string_view name)
	{
		const std::lock_guard lock(m_mutex);
		oc::erase_if(m_counters, [name](const HudCounter& c) { return c.name == name; });
	}

	// Replaces the whole world-label list (per-frame rebuild; pass {} to clear).
	void setWorldLabels(oc::vector<HudWorldLabel>&& labels)
	{
		const std::lock_guard lock(m_mutex);
		m_worldLabels = oc::move(labels);
	}

	// Removes every slot, bar and counter (a script's OnDestroy typically calls this).
	void clearAll()
	{
		const std::lock_guard lock(m_mutex);
		for (HudSlot& slot : m_slots)
			slot = HudSlot();
		m_bars.clear();
		m_counters.clear();
		m_worldLabels.clear();
		m_selectedSlot = 0;
	}

	// ---- reads --------------------------------------------------------------

	int getSelectedSlot() const
	{
		const std::lock_guard lock(m_mutex);
		return m_selectedSlot;
	}

	// The hotbar draws (and the number keys select) only while it is visible AND anything is assigned --
	// so an empty HUD leaves the testbed's number-key spawns untouched.
	bool isHotbarActive() const
	{
		const std::lock_guard lock(m_mutex);
		return hotbarActiveLocked();
	}

	// The slot drawn under a screen position last frame (-1 = none / hotbar not drawn). Any slot
	// counts, assigned or not: a click on an empty slot must still not fall through to the world.
	int slotAtScreenPos(const glm::vec2& pos) const
	{
		const std::lock_guard lock(m_mutex);
		if (!m_slotRectsValid || !hotbarActiveLocked())
			return -1;
		for (int i = 0; i < NumSlots; ++i)
		{
			const glm::vec4& r = m_slotRects[i];
			if (pos.x >= r.x && pos.y >= r.y && pos.x <= r.z && pos.y <= r.w)
				return i;
		}
		return -1;
	}

	struct Snapshot
	{
		HudSlot slots[NumSlots];
		oc::string keyLabels[NumSlots];
		int     columns = 0;
		int     selectedSlot = 0;
		bool    hotbarActive = false;
		oc::vector<HudBar> bars;
		oc::vector<HudCounter> counters;
		oc::vector<HudWorldLabel> worldLabels;
	};

	// One copy per frame for the overlay (UI thread).
	Snapshot snapshot() const
	{
		const std::lock_guard lock(m_mutex);
		Snapshot out;
		for (int i = 0; i < NumSlots; ++i)
		{
			out.slots[i] = m_slots[i];
			out.keyLabels[i] = m_keyLabels[i];
		}
		out.columns = m_columns;
		out.selectedSlot = m_selectedSlot;
		out.hotbarActive = hotbarActiveLocked();
		out.bars = m_bars;
		out.counters = m_counters;
		out.worldLabels = m_worldLabels;
		return out;
	}

private:

	bool hotbarActiveLocked() const
	{
		if (!m_hotbarVisible)
			return false;
		for (const HudSlot& slot : m_slots)
			if (slot.used)
				return true;
		return false;
	}

	template<class T> static T& findOrAdd(oc::vector<T>& list, oc::string_view name)
	{
		for (T& entry : list)
			if (entry.name == name)
				return entry;
		T& added = list.emplace_back();
		added.name = name;
		return added;
	}

	mutable std::mutex m_mutex;
	HudSlot m_slots[NumSlots];
	oc::string m_keyLabels[NumSlots] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "-", "=" };
	glm::vec4 m_slotRects[NumSlots] = {};
	bool m_slotRectsValid = false;
	int  m_columns = 0; // 0 = single row
	int  m_selectedSlot = 0;
	bool m_hotbarVisible = true; // an explicit off-switch; slots being assigned is what shows it
	oc::vector<HudBar> m_bars;
	oc::vector<HudCounter> m_counters;
	oc::vector<HudWorldLabel> m_worldLabels;
};

export namespace Globals
{
	GameHud gameHud;
}
