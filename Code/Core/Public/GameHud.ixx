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
	// HOVER CARD: '\n'-separated lines the overlay draws in a box beside the slot while the cursor
	// is on it (the build menu's "what does this do / cost / produce"). Empty = no card.
	oc::string tooltip;
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
	oc::string  text;         // non-empty = shown INSTEAD of the number (a clock, a state word)
	glm::vec3   color = glm::vec3(1.0f);
};

// World-anchored UI element (a health bar / info block over an entity). Gameplay PRE-PROJECTS the
// world position into viewport pixels (Camera::worldToScreen) and replaces the whole list every
// frame - no keys, no persistence; off-screen entities are simply not submitted.
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
	float       bar3Value = 0.0f; // optional third bar (the Base: health + energy + minerals)
	float       bar3Max = 0.0f;   // <= 0 = no third bar
	glm::vec3   bar3Color = glm::vec3(1.0f);
	oc::string warning;          // non-empty = a badge BUBBLE above the title (a problem to fix)
	glm::vec3   warningColor = glm::vec3(1.0f, 0.5f, 0.25f);
	bool        emphasized = false; // selected: drawn larger
};

// A world-anchored BUTTON ROW (the barracks' unit-type picker): drawn as a box above the anchor,
// one button per entry. Gameplay rebuilds it every frame like the labels; the overlay reports each
// button's screen rect back (setPopupButtonRects) so clicks resolve through popupButtonAtScreenPos.
export struct HudPopupButton
{
	oc::string label;          // big caption
	oc::string sub;            // small line under it (empty = none)
	bool        selected = false;
};

export struct HudPopup
{
	bool        active = false;
	glm::vec2   screenPos{};   // viewport-space anchor: the box sits centered above it
	oc::string title;          // line at the top of the box (empty = none)
	oc::vector<HudPopupButton> buttons;
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
		// (the hover card is NOT cleared here: gameplay re-states it right after, and dropping it
		//  first would make every re-state a fresh allocation - clearSlot is what wipes a slot)
	}

	// The slot's hover card (see HudSlot::tooltip). Unchanged text is a no-op, so a hotbar that
	// re-states itself every frame allocates nothing.
	void setSlotTooltip(int index, oc::string_view text)
	{
		if (index < 0 || index >= NumSlots)
			return;
		const std::lock_guard lock(m_mutex);
		oc::string& tip = m_slots[index].tooltip;
		if (tip.size() == text.size() && (text.empty() || memcmp(tip.data(), text.data(), text.size()) == 0))
			return;
		tip.assign(text.data(), text.size());
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
		oc::erase_if(m_bars, [name](const HudBar& b) { return name == b.name; });
	}

	void setCounter(oc::string_view name, float value, int decimals, const glm::vec3& color)
	{
		const std::lock_guard lock(m_mutex);
		HudCounter& counter = findOrAdd(m_counters, name);
		counter.value = value;
		counter.decimals = glm::clamp(decimals, 0, 6);
		counter.text.clear();
		counter.color = glm::clamp(color, 0.0f, 1.0f);
	}

	// A counter that shows TEXT instead of a number (the match clock as h:mm:ss). Same key space
	// and stacking order as the numeric ones.
	void setCounterText(oc::string_view name, oc::string_view text, const glm::vec3& color)
	{
		const std::lock_guard lock(m_mutex);
		HudCounter& counter = findOrAdd(m_counters, name);
		counter.text.assign(text.data(), text.size());
		counter.color = glm::clamp(color, 0.0f, 1.0f);
	}

	void removeCounter(oc::string_view name)
	{
		const std::lock_guard lock(m_mutex);
		oc::erase_if(m_counters, [name](const HudCounter& c) { return name == c.name; });
	}

	// Replaces the popup (per-frame rebuild; an inactive one clears it). A SWAP: the caller gets
	// the previous popup back - the buttons vector keeps its capacity across frames (clear() it).
	void swapPopup(HudPopup& popup)
	{
		const std::lock_guard lock(m_mutex);
		oc::swap(m_popup, popup);
	}

	// The overlay reports where it drew each popup button (or nothing when no popup was drawn).
	void setPopupButtonRects(oc::span<const glm::vec4> minMax)
	{
		const std::lock_guard lock(m_mutex);
		m_popupRects.assign(minMax.begin(), minMax.end());
	}

	// The popup button drawn under a screen position last frame (-1 = none).
	int popupButtonAtScreenPos(const glm::vec2& pos) const
	{
		const std::lock_guard lock(m_mutex);
		for (int i = 0; i < (int)m_popupRects.size(); ++i)
		{
			const glm::vec4& r = m_popupRects[i];
			if (pos.x >= r.x && pos.y >= r.y && pos.x <= r.z && pos.y <= r.w)
				return i;
		}
		return -1;
	}

	// Replaces the whole world-label list (per-frame rebuild). A SWAP: the caller gets last
	// frame's list back, capacity intact, so two vectors ping-pong between the builder and the HUD
	// and no frame allocates the list (clear() it before the next build). Pass an empty one to clear.
	void swapWorldLabels(oc::vector<HudWorldLabel>& labels)
	{
		const std::lock_guard lock(m_mutex);
		m_worldLabels.swap(labels);
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
		m_popup = HudPopup();
		m_popupRects.clear();
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
		HudPopup popup;
	};

	// One copy per frame for the overlay (UI thread), INTO a snapshot the caller keeps: the copy
	// assignments reuse the vectors' and strings' capacity, so a steady HUD copies without allocating.
	void snapshot(Snapshot& out) const
	{
		const std::lock_guard lock(m_mutex);
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
		out.popup = m_popup;
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
	HudPopup m_popup;
	oc::vector<glm::vec4> m_popupRects; // per button: (minX, minY, maxX, maxY), last frame
};

export namespace Globals
{
	GameHud gameHud;
}
