export module Settings.Tweaks;

import Core;
import Core.glm;
import Core.Log;

// A lightweight, global registry of "tweakable" variables. The Settings library registers every tweak once
// (Settings::registerAll, the variables are members of Globals::settings) together with how it should be presented;
// the UI's TweakPanel iterates the registry and renders the appropriate widget. Pointers are non-owning: the
// registered variable must outlive the registration. A system that must REACT to a change attaches a listener to the
// variable (Tweak::onChange(variable, owner, fn)) - the registration itself knows nothing of the systems.

export enum class ETweakType : uint8
{
	Float,   // single float, slider (bounded) or drag (unbounded)
	Float2,
	Float3,
	Float4,
	Color3,  // rgb color picker (+ optional intensity)
	Color4,  // rgba color picker
	Bool,
	Int,     // single int, slider (bounded) or drag (unbounded)
	Enum,    // int index into enumNames
};

// Optional behavior flags. Saved: the value persists to Assets/Local/tweaks.cfg - loaded at
// startup (TweakRegistry::loadSaved from main), written back debounced whenever a change is
// detected. Synced: in a network session the SERVER's value broadcasts to clients on change and
// at join (NetworkManager watches syncGeneration()). Identity is "Category/Name" - renaming a
// flagged tweak orphans its saved value (harmless: unknown keys are kept but never applied).
// Runtime: a LOCK never covers it (see TweakLock) - a value its owner keeps live in a locked section (the sun
// direction, the wind). It does not count as an explicit flag, so a ScopedFlags block default still applies.
export enum class ETweakFlags : uint8
{
	None    = 0,
	Saved   = 1,
	Synced  = 2,
	Runtime = 4,
};
export constexpr ETweakFlags operator|(ETweakFlags a, ETweakFlags b) { return ETweakFlags(uint8(a) | uint8(b)); }
export constexpr bool anyFlag(ETweakFlags a, ETweakFlags b) { return (uint8(a) & uint8(b)) != 0; }

// A LOCK covers whole tweak SECTIONS: its owner (the renderer bakes locked values into the shaders as constants)
// names the categories it covers. A row belongs to the NEAREST lock category above it, so "Sky/Clouds" (its own
// lock) is not covered by "Sky"'s; ETweakFlags::Runtime rows are never covered. EVERY covered row has its OWN
// locked state (the TweakPanel's lock button on the row, drawn read-only while locked); the section's toggle on
// its fold sets all of its rows. A row registered later takes the section's last state. Nothing is saved: every run
// starts at the owner's default; `--tweak "@lock/<name>=0|1"` overrides a section, `"@lock/<Category/Name>=0|1"` a row.
//
// An owner asks whether a VALUE is locked by its source variables' ADDRESSES (sourceState: the row that holds each),
// and hears about every change through a change listener (addChangeListener): a lock click fires the lock's onChange,
// a row's value change fires notifyChanged.
export struct TweakLock
{
	oc::string name;
	oc::vector<oc::string> categories;
	oc::function<void()> onChange; // optional, main thread, fired when a row's or the section's state changes
	bool locked = false;           // the section-wide state: what its toggle last set (and what a new row takes)
};

// sourceState's answer; combining sources takes the max.
export enum class ETweakSource : uint8
{
	Locked,  // locked rows only, or rows no lock covers (nothing can unlock them: the owner re-bakes on a change)
	Unknown, // an address no registered row holds (a source that is no tweak)
	Live,    // an unlocked row or a Runtime row
};

// A section toggle's look: all, some or none of its rows locked.
export enum class ETweakLockState : uint8 { None, Some, All };

// The panel's top-level folds. A category's ROOT segment ("Sky" of "Sky/Clouds") maps to one group
// through this table; the group is presentation only and is NOT part of the "Category/Name"
// identity, so grouping a category never orphans its saved value. Panel order = table order (groups,
// then the categories inside a group). A root missing from every group lands in the last entry.
export struct TweakGroup
{
	oc::string_view name;
	glm::vec4       color;      // header tint
	oc::span<const oc::string_view> categories; // root category names, in display order
};

namespace TweakGroups
{
	constexpr oc::string_view c_graphics[] = { "Sky", "Shadows", "Fog", "RT", "GI", "RTAO", "TAA", "Post", "LOD" };
	constexpr oc::string_view c_fx[]       = { "Particles", "Decals", "Force", "Ocean", "Terrain", "Clutter" };
	constexpr oc::string_view c_system[]   = { "Time", "Editor", "Audio", "Physics", "Mesh Streaming", "Texture Streaming", "Spatial", "Threading" };
	constexpr oc::string_view c_game[]     = { "Game", "HUD", "Network", "Nav" };

	inline const TweakGroup c_table[] = {
		{ "Graphics", glm::vec4(0.26f, 0.48f, 0.85f, 1.0f), c_graphics },
		{ "FX",       glm::vec4(0.80f, 0.42f, 0.20f, 1.0f), c_fx },
		{ "System",   glm::vec4(0.45f, 0.55f, 0.60f, 1.0f), c_system },
		{ "Game",     glm::vec4(0.30f, 0.65f, 0.35f, 1.0f), c_game },
		{ "Other",    glm::vec4(0.50f, 0.50f, 0.50f, 1.0f), {} }, // fallback: every root not listed above
	};
}

export namespace Tweak
{
	inline oc::span<const TweakGroup> groups() { return TweakGroups::c_table; }

	// Index into groups() for a category path; the last group when its root is unlisted.
	inline size_t groupIndexOf(oc::string_view category)
	{
		const size_t slash = category.find('/');
		const oc::string_view root = slash == oc::string_view::npos ? category : category.substr(0, slash);
		const size_t count = sizeof(TweakGroups::c_table) / sizeof(TweakGroups::c_table[0]);
		for (size_t g = 0; g + 1 < count; ++g)
			for (const oc::string_view name : TweakGroups::c_table[g].categories)
				if (name == root)
					return g;
		return count - 1;
	}
}

// A system's reaction to a tweak's change (Tweak::onChange). The owner key lets the system drop its listeners
// (Tweak::removeListeners) before it dies.
export struct TweakListener
{
	const void* owner = nullptr;
	oc::function<void()> fn;
};

export struct TweakVar
{
	oc::string_view name;
	oc::string_view category;
	ETweakType       type = ETweakType::Float;
	void*            data = nullptr;     // non-owning, points at the live variable

	float            min   = 0.0f;
	float            max   = 1.0f;
	float            speed = 0.01f;      // drag step for unbounded floats

	float*           intensity = nullptr;                 // optional, Color3 only (min/max/speed then bound the intensity drag)
	oc::span<const oc::string_view> enumNames;          // Enum only

	oc::function<void()> onChange;                       // optional, fired when the value changes (the settings' own logic)
	oc::vector<TweakListener> listeners;                 // the systems' reactions (Tweak::onChange); fired after onChange

	ETweakFlags      flags = ETweakFlags::None;

	bool isUnbounded() const { return max >= FLT_MAX * 0.5f; }

	// Main thread: the value changed (the panel's deferred flush, an override, a synced value).
	void fireChanged() const
	{
		if (onChange)
			onChange();
		for (const TweakListener& listener : listeners)
			listener.fn();
	}
};

export class TweakRegistry
{
public:

	static TweakRegistry& get()
	{
		static TweakRegistry instance;
		return instance;
	}

	~TweakRegistry()
	{
		if (m_saveDirty)
			saveFile(); // exit before the debounce elapsed
	}

	void registerVar(const TweakVar& var)
	{
		ProfileScope scope("TweakRegistry::registerVar", EProfileCategory::Core);
		// RE-registration (same "Category/Name") REPLACES in place: mode transitions reconstruct
		// systems (exit-to-menu -> a fresh GameMatch) whose registerTweaks run again - the fresh
		// pointers supersede the old ones instead of growing duplicates.
		const oc::string key = keyOf(var);
		size_t slot = m_vars.size();
		for (size_t i = 0; i < m_vars.size(); ++i)
			if (keyOf(m_vars[i]) == key)
			{
				slot = i;
				break;
			}
		if (slot == m_vars.size())
		{
			m_vars.push_back(var);
			m_snapshots.emplace_back();
			m_varLocks.push_back(c_noLock);
			m_varLocked.push_back(0);
			m_varLockable.push_back(0);
		}
		else
		{
			oc::vector<TweakListener> listeners = oc::move(m_vars[slot].listeners); // the systems' reactions stay
			m_vars[slot] = var;
			m_vars[slot].listeners = oc::move(listeners);
		}
		TweakVar& stored = m_vars[slot];
		if (!anyFlag(stored.flags, ETweakFlags::Saved | ETweakFlags::Synced))
			stored.flags = stored.flags | m_defaultFlags; // ScopedFlags block default; explicit flags win
		assignLock(slot); // a re-registration keeps its row state
		if (!applyOverride(stored) && m_savedLoaded && anyFlag(stored.flags, ETweakFlags::Saved))
			applySavedValue(stored);
		m_snapshots[slot] = readValue(stored); // taken AFTER the override/saved apply: not a "change", so nothing saves back
	}

	// ---- Listeners (see TweakListener) ------------------------------------------------------
	// Attaches fn to the row whose variable holds `address` (the data, or a Color3's intensity). Main thread. Returns
	// false when no registered row holds it.
	bool addListener(const void* address, const void* owner, oc::function<void()> fn)
	{
		for (TweakVar& var : m_vars)
			if (var.data == address || var.intensity == address)
			{
				var.listeners.push_back(TweakListener{ owner, oc::move(fn) });
				return true;
			}
		return false;
	}

	// Hears EVERY row's value change (after the row's own listeners) - an owner that watches many rows at once (the
	// renderer's baked UBO values).
	void addChangeListener(const void* owner, oc::function<void(const TweakVar&)> fn)
	{
		m_changeListeners.push_back(ChangeListener{ owner, oc::move(fn) });
	}

	// Drops every listener `owner` attached.
	void removeListeners(const void* owner)
	{
		for (TweakVar& var : m_vars)
			for (size_t i = var.listeners.size(); i-- > 0;)
				if (var.listeners[i].owner == owner)
					var.listeners.erase(var.listeners.begin() + i);
		for (size_t i = m_changeListeners.size(); i-- > 0;)
			if (m_changeListeners[i].owner == owner)
				m_changeListeners.erase(m_changeListeners.begin() + i);
	}

	// THE change path, main thread: the panel's deferred flush, an override, a synced value, and code that wrote a
	// setting (Tweak::notifyChanged). Fires the row's onChange, its listeners, then the change listeners.
	void notifyChanged(const TweakVar& var) const
	{
		var.fireChanged();
		for (const ChangeListener& listener : m_changeListeners)
			listener.fn(var);
	}

	// The row whose variable holds `address`; null when none does.
	const TweakVar* findVar(const void* address) const
	{
		for (const TweakVar& var : m_vars)
			if (var.data == address || var.intensity == address)
				return &var;
		return nullptr;
	}

	// ---- Locks (see TweakLock) ------------------------------------------------------------
	static constexpr uint32 c_noLock = UINT32_MAX;

	// Returns the lock's id; it starts in lockedByDefault. Re-registering a name replaces its categories and callback
	// in place and keeps the state. An override applies here (onChange fires for it, like a tweak's override).
	uint32 registerLock(oc::string_view name, oc::span<const oc::string_view> categories, bool lockedByDefault, oc::function<void()> onChange = {})
	{
		uint32 id = 0;
		while (id < (uint32)m_locks.size() && m_locks[id].name != name)
			++id;
		if (id == (uint32)m_locks.size())
			m_locks.push_back(TweakLock{ .name = oc::string(name), .locked = lockedByDefault });
		TweakLock& lock = m_locks[id];
		lock.categories.clear();
		for (const oc::string_view category : categories)
			lock.categories.emplace_back(category);
		lock.onChange = oc::move(onChange);
		const auto overridden = m_overrides.find(lockKey(lock));
		if (overridden != m_overrides.end() && !overridden->second.empty())
			lock.locked = overridden->second[0] != 0.0f;
		for (size_t i = 0; i < m_vars.size(); ++i)
			assignLock(i);
		return id;
	}

	const oc::vector<TweakLock>& locks() const { return m_locks; }

	// The owner names the rows a lock means something on (the renderer: every source of a baked value). Only those get a
	// lock button, count in their section's state and are read-only while locked. Main thread, after the rows registered.
	void markLockable(const void* address, size_t size)
	{
		const uint8* a = static_cast<const uint8*>(address);
		const auto overlaps = [a, size](const void* begin, size_t length)
		{
			const uint8* b = static_cast<const uint8*>(begin);
			return a < b + length && b < a + size;
		};
		for (size_t i = 0; i < m_vars.size(); ++i)
			if (overlaps(m_vars[i].data, dataSize(m_vars[i])) || (m_vars[i].intensity && overlaps(m_vars[i].intensity, sizeof(float))))
				m_varLockable[i] = 1;
	}

	// How many of the section's lockable rows are locked (its toggle's look).
	ETweakLockState lockState(uint32 lock) const
	{
		bool any = false, all = true;
		for (size_t i = 0; i < m_vars.size(); ++i)
			if (m_varLocks[i] == lock && m_varLockable[i] != 0)
			{
				any |= m_varLocked[i] != 0;
				all &= m_varLocked[i] != 0;
			}
		if (!any)
			return lock < (uint32)m_locks.size() && m_locks[lock].locked && all ? ETweakLockState::All : ETweakLockState::None;
		return all ? ETweakLockState::All : ETweakLockState::Some;
	}

	// The whole section: every row under it. Main thread (the panel defers its clicks to
	// TweakPanel::flushDeferredCallbacks). Lasts for the run only.
	void setLocked(uint32 lock, bool locked)
	{
		if (lock >= (uint32)m_locks.size())
			return;
		bool changed = m_locks[lock].locked != locked;
		m_locks[lock].locked = locked;
		for (size_t i = 0; i < m_vars.size(); ++i)
			if (m_varLocks[i] == lock && (m_varLocked[i] != 0) != locked)
			{
				m_varLocked[i] = locked ? 1 : 0;
				changed = true;
			}
		if (!changed)
			return;
		if (m_locks[lock].onChange)
			m_locks[lock].onChange();
	}

	// One row. No-op for a row no lock covers or that is not lockable.
	void setVarLocked(const TweakVar& var, bool locked)
	{
		const size_t index = (size_t)(&var - m_vars.data());
		if (index >= m_vars.size() || m_varLocks[index] == c_noLock || m_varLockable[index] == 0 || (m_varLocked[index] != 0) == locked)
			return;
		m_varLocked[index] = locked ? 1 : 0;
		if (m_locks[m_varLocks[index]].onChange)
			m_locks[m_varLocks[index]].onChange();
	}

	// The row's lock (c_noLock = none covers it, or it is not lockable: no button).
	uint32 lockOf(const TweakVar& var) const
	{
		const size_t index = (size_t)(&var - m_vars.data());
		return index < m_vars.size() && m_varLockable[index] != 0 ? m_varLocks[index] : c_noLock;
	}

	// A source variable: its address and size (a vec3 over three float rows reaches all three).
	struct Source
	{
		const void* address;
		size_t size;
	};

	// Whether the value at [address, address + size) can change while its locks hold (see ETweakSource). Linear in
	// the rows: call it on a lock change, not per frame.
	ETweakSource sourceState(const void* address, size_t size) const
	{
		const uint8* a = static_cast<const uint8*>(address);
		const auto overlaps = [a, size](const void* begin, size_t length)
		{
			const uint8* b = static_cast<const uint8*>(begin);
			return a < b + length && b < a + size;
		};
		bool found = false;
		for (size_t i = 0; i < m_vars.size(); ++i)
		{
			const TweakVar& var = m_vars[i];
			if (!overlaps(var.data, dataSize(var)) && !(var.intensity && overlaps(var.intensity, sizeof(float))))
				continue;
			found = true;
			if (anyFlag(var.flags, ETweakFlags::Runtime) || (m_varLocks[i] != c_noLock && m_varLocked[i] == 0))
				return ETweakSource::Live;
		}
		return found ? ETweakSource::Locked : ETweakSource::Unknown;
	}

	// Whether a change of the row moves a baked value: it is lockable, and LOCKED or no lock covers it.
	bool isVarBakeable(const TweakVar& var) const
	{
		const size_t index = (size_t)(&var - m_vars.data());
		if (index >= m_vars.size() || m_varLockable[index] == 0 || anyFlag(var.flags, ETweakFlags::Runtime))
			return false;
		return m_varLocks[index] == c_noLock || m_varLocked[index] != 0;
	}

	// The lock that names this category EXACTLY (the fold that carries its toggle); c_noLock when none does, or when
	// none of its rows is lockable.
	uint32 lockAt(oc::string_view category) const
	{
		for (uint32 id = 0; id < (uint32)m_locks.size(); ++id)
			for (const oc::string& c : m_locks[id].categories)
				if (c == category)
				{
					for (size_t i = 0; i < m_vars.size(); ++i)
						if (m_varLocks[i] == id && m_varLockable[i] != 0)
							return id;
					return c_noLock;
				}
		return c_noLock;
	}

	// The row is read-only: its own state (a copy - the settings page - finds its row by its key).
	bool isVarLocked(const TweakVar& var) const
	{
		size_t index = (size_t)(&var - m_vars.data());
		if (index >= m_vars.size())
		{
			const oc::string key = keyOf(var);
			for (index = 0; index < m_vars.size() && keyOf(m_vars[index]) != key; ++index) {}
			if (index == m_vars.size())
				return false;
		}
		return m_varLocks[index] != c_noLock && m_varLockable[index] != 0 && m_varLocked[index] != 0;
	}

	// Mode-teardown support: removes every variable whose registered pointer (data or intensity)
	// lies inside [object, object + size) - a dying stack subsystem (GameMatch) takes its MEMBER
	// registrations with it, or update()/the panel would read freed memory every frame. Statics
	// registered by the same code stay, and re-register in place on the next construction.
	void unregisterInRange(const void* object, size_t size)
	{
		const char* begin = static_cast<const char*>(object);
		const char* end = begin + size;
		const auto inRange = [&](const void* p)
		{
			const char* c = static_cast<const char*>(p);
			return c >= begin && c < end;
		};
		for (size_t i = m_vars.size(); i-- > 0;)
			if (inRange(m_vars[i].data) || (m_vars[i].intensity && inRange(m_vars[i].intensity)))
			{
				m_vars.erase(m_vars.begin() + i);
				m_snapshots.erase(m_snapshots.begin() + i);
				m_varLocks.erase(m_varLocks.begin() + i);
				m_varLocked.erase(m_varLocked.begin() + i);
				m_varLockable.erase(m_varLockable.begin() + i);
			}
		for (TweakVar& var : m_vars) // a listener owned by the dying object would call into freed memory
			for (size_t i = var.listeners.size(); i-- > 0;)
				if (inRange(var.listeners[i].owner))
					var.listeners.erase(var.listeners.begin() + i);
		for (size_t i = m_changeListeners.size(); i-- > 0;)
			if (inRange(m_changeListeners[i].owner))
				m_changeListeners.erase(m_changeListeners.begin() + i);
	}

	// A whole FILE of overrides (`--tweaks <path>`, read by main through FileSystem): the
	// tweaks.cfg format - one `Category/Name = v [v v v]` per line, `#`/`//` comments, blank lines
	// ignored - each line applied through setOverride (wins over the saved file, never written
	// back). How an automated run pins a whole configuration, e.g. graphics features off for a
	// CPU-focused profile. Returns the number of lines applied; unparsable lines are logged.
	uint32 loadOverrides(oc::string_view content, oc::string_view sourceName)
	{
		uint32 applied = 0;
		size_t pos = 0;
		uint32 lineNo = 0;
		while (pos < content.size())
		{
			size_t end = content.find('\n', pos);
			if (end == oc::string_view::npos)
				end = content.size();
			oc::string_view line = content.substr(pos, end - pos);
			pos = end + 1;
			++lineNo;
			while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
				line.remove_suffix(1);
			while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
				line.remove_prefix(1);
			if (line.empty() || line.front() == '#' || oc::startsWith(line, "//"))
				continue;
			if (setOverride(line))
				++applied;
			else
				Log::warning(oc::format("Tweaks: {}({}): cannot parse '{}'", sourceName, lineNo, line));
		}
		return applied;
	}

	// Command-line override (`--tweak "Category/Name=v"`, up to 4 values space-separated): applies
	// to the variable now if registered, else at its registration; wins over the saved file and is
	// NEVER written back (an unattended profiling run must leave Local/tweaks.cfg alone). Works on
	// any variable, Saved or not. Returns false when the spec does not parse.
	bool setOverride(oc::string_view spec)
	{
		const size_t sep = spec.find('=');
		if (sep == oc::string_view::npos || sep == 0 || sep + 1 >= spec.size())
			return false;
		oc::string key(spec.substr(0, sep));
		while (!key.empty() && key.back() == ' ')
			key.pop_back();
		const oc::string valueText(spec.substr(sep + 1));
		oc::vector<float> values;
		const char* cursor = valueText.c_str();
		while (values.size() < 4)
		{
			char* end = nullptr;
			const float v = std::strtof(cursor, &end);
			if (end == cursor)
				break;
			values.push_back(v);
			cursor = end;
		}
		if (values.empty())
			return false;
		m_overrides[key] = oc::move(values);
		for (size_t i = 0; i < m_vars.size(); ++i)
			if (keyOf(m_vars[i]) == key)
			{
				applyOverride(m_vars[i]);
				m_snapshots[i] = readValue(m_vars[i]);
			}
		for (uint32 id = 0; id < (uint32)m_locks.size(); ++id)
			if (lockKey(m_locks[id]) == key)
				setLocked(id, m_overrides[key][0] != 0.0f);
		for (size_t i = 0; i < m_vars.size(); ++i)
			if (c_lockPrefix + keyOf(m_vars[i]) == key)
				setVarLocked(m_vars[i], m_overrides[key][0] != 0.0f);
		return true;
	}

	const oc::vector<TweakVar>& vars() const { return m_vars; }

	// ScopedFlags support - the default applied to registrations that pass no explicit flags.
	ETweakFlags defaultFlags() const { return m_defaultFlags; }
	void setDefaultFlags(ETweakFlags flags) { m_defaultFlags = flags; }

	// ---- Saved --------------------------------------------------------------------------
	// Core sits BELOW the File library, so it cannot touch the disk itself: main() installs these
	// two hooks (FileSystem::readFileStr / writeFileStr) before loadSaved(). Without them the
	// Saved flag is simply inert - the registry keeps working in-memory.
	using ReadFileFn = oc::function<oc::string(const oc::string& path)>;
	using WriteFileFn = oc::function<bool(const oc::string& path, const oc::string& content)>;
	void setFileIo(ReadFileFn read, WriteFileFn write)
	{
		m_readFile = oc::move(read);
		m_writeFile = oc::move(write);
	}

	// Call ONCE from main() right after FileSystem::initialize() (the path is Assets/-relative).
	// Applies stored values to everything already registered; later registrations apply their
	// value inside registerVar.
	void loadSaved()
	{
		m_savedLoaded = true;
		if (!m_readFile)
			return;
		ProfileScope scope("Tweaks::loadSaved", EProfileCategory::Core);
		std::istringstream file(oc::toStd(m_readFile(c_savePath)));
		oc::string line;
		while (oc::getline(file, line))
		{
			const size_t sep = line.find(" = ");
			if (sep == oc::string::npos || sep == 0 || sep + 3 >= line.size())
				continue;
			oc::vector<float>& values = m_savedValues[line.substr(0, sep)];
			values.clear();
			const char* cursor = line.c_str() + sep + 3;
			while (values.size() < 4)
			{
				char* end = nullptr;
				const float v = std::strtof(cursor, &end);
				if (end == cursor)
					break;
				values.push_back(v);
				cursor = end;
			}
		}
		for (size_t i = 0; i < m_vars.size(); ++i)
			if (anyFlag(m_vars[i].flags, ETweakFlags::Saved) && applySavedValue(m_vars[i]))
				m_snapshots[i] = readValue(m_vars[i]); // applied values are not "changes"
		Log::info("Tweaks: loaded " + oc::to_string(m_savedValues.size()) + " saved values");
	}

	// Main loop, once per frame. The panel (and gameplay code) writes through the raw pointers,
	// so polling is the only reliable change hook: flagged vars diff against a snapshot - Saved
	// changes arm a debounced file write (sliders drag every frame), Synced changes bump the
	// generation the NetworkManager broadcasts on.
	void update(float deltaSec)
	{
		ProfileScope profileScope("Tweak Update", EProfileCategory::Core);

		for (size_t i = 0; i < m_vars.size(); ++i)
		{
			const TweakVar& var = m_vars[i];
			if (!anyFlag(var.flags, ETweakFlags::Saved | ETweakFlags::Synced))
				continue;
			const Value current = readValue(var);
			if (current == m_snapshots[i])
				continue;
			m_snapshots[i] = current;
			if (anyFlag(var.flags, ETweakFlags::Saved))
			{
				m_saveDirty = true;
				m_saveTimer = 0.5f;
			}
			if (anyFlag(var.flags, ETweakFlags::Synced))
				++m_syncGeneration;
		}
		if (m_saveDirty && (m_saveTimer -= deltaSec) <= 0.0f)
			saveFile();
	}

	// ---- Synced (transport-agnostic; NetworkManager is the consumer) ----------------------
	uint32 syncGeneration() const { return m_syncGeneration; }

	// Every Synced var as self-contained records, split into chunks that each fit one network
	// event. Record: [u8 keyLen]["Category/Name"][u8 type][u8 count][count x f32] (int/bool/enum
	// travel as floats).
	void packSynced(oc::vector<oc::vector<uint8>>& outChunks, size_t maxChunkBytes = 1000) const
	{
		ProfileScope profileScope("Tweak Sync", EProfileCategory::Core);

		oc::vector<uint8> chunk;
		for (const TweakVar& var : m_vars)
		{
			if (!anyFlag(var.flags, ETweakFlags::Synced))
				continue;
			const oc::string key = keyOf(var);
			if (key.size() > 255)
				continue;
			const Value value = readValue(var);
			const size_t recordSize = 1 + key.size() + 2 + value.count * sizeof(float);
			if (!chunk.empty() && chunk.size() + recordSize > maxChunkBytes)
			{
				outChunks.push_back(oc::move(chunk));
				chunk.clear();
			}
			chunk.push_back((uint8)key.size());
			chunk.insert(chunk.end(), key.begin(), key.end());
			chunk.push_back((uint8)var.type);
			chunk.push_back((uint8)value.count);
			const uint8* bytes = reinterpret_cast<const uint8*>(value.v);
			chunk.insert(chunk.end(), bytes, bytes + value.count * sizeof(float));
		}
		if (!chunk.empty())
			outChunks.push_back(oc::move(chunk));
	}

	// Receive side. Unknown keys, un-Synced vars and type mismatches are IGNORED - the sender
	// cannot modify anything the receiver did not itself flag as Synced; values clamp to the
	// receiver's own bounds. Applied values refresh the snapshot, so they neither re-save the
	// client's file nor bounce a sync generation.
	void applySyncedBlob(oc::span<const uint8> blob)
	{
		ProfileScope profileScope("Tweak Sync", EProfileCategory::Core);

		size_t cursor = 0;
		while (cursor < blob.size())
		{
			const uint8 keyLen = blob[cursor++];
			if (cursor + keyLen + 2 > blob.size())
				return; // malformed - drop the rest
			const oc::string_view key(reinterpret_cast<const char*>(blob.data() + cursor), keyLen);
			cursor += keyLen;
			const uint8 type = blob[cursor++];
			const uint8 count = blob[cursor++];
			if (count > 4 || cursor + count * sizeof(float) > blob.size())
				return;
			Value value;
			value.count = count;
			std::memcpy(value.v, blob.data() + cursor, count * sizeof(float));
			cursor += count * sizeof(float);
			for (size_t i = 0; i < m_vars.size(); ++i)
			{
				TweakVar& var = m_vars[i];
				if (!anyFlag(var.flags, ETweakFlags::Synced) || uint8(var.type) != type || keyOf(var) != key)
					continue;
				writeValue(var, value);
				m_snapshots[i] = readValue(var);
				break;
			}
		}
	}

private:

	struct Value
	{
		float v[4] = {};
		int count = 0;
		bool operator==(const Value& o) const { return count == o.count && std::memcmp(v, o.v, count * sizeof(float)) == 0; }
	};

	static oc::string keyOf(const TweakVar& var) { return oc::string(var.category) + "/" + oc::string(var.name); }
	static constexpr const char* c_lockPrefix = "@lock/";
	static oc::string lockKey(const TweakLock& lock) { return c_lockPrefix + lock.name; }

	// Row i's lock, after a registration or a new lock: a row that changes lock takes that section's state; a row
	// override (@lock/<Category/Name>) wins.
	void assignLock(size_t i)
	{
		const uint32 lock = findLockFor(m_vars[i]);
		if (lock != m_varLocks[i])
		{
			m_varLocks[i] = lock;
			m_varLocked[i] = lock != c_noLock && m_locks[lock].locked ? 1 : 0;
		}
		if (lock == c_noLock || m_overrides.empty())
			return;
		const auto overridden = m_overrides.find(c_lockPrefix + keyOf(m_vars[i]));
		if (overridden != m_overrides.end() && !overridden->second.empty())
			m_varLocked[i] = overridden->second[0] != 0.0f ? 1 : 0;
	}

	// The bytes a row's variable spans (Color3's intensity is checked apart).
	static size_t dataSize(const TweakVar& var)
	{
		switch (var.type)
		{
		case ETweakType::Bool: return sizeof(bool);
		case ETweakType::Int:
		case ETweakType::Enum: return sizeof(int);
		case ETweakType::Color3: return 3 * sizeof(float);
		default: return size_t(componentCount(var)) * sizeof(float);
		}
	}

	// The NEAREST lock category on the var's path (the longest one that is its category or a parent of it).
	uint32 findLockFor(const TweakVar& var) const
	{
		if (anyFlag(var.flags, ETweakFlags::Runtime))
			return c_noLock;
		uint32 best = c_noLock;
		size_t bestLength = 0;
		for (uint32 id = 0; id < (uint32)m_locks.size(); ++id)
			for (const oc::string& c : m_locks[id].categories)
			{
				const oc::string_view category(c.data(), c.size());
				const bool covers = var.category == category
					|| (var.category.size() > category.size() && oc::startsWith(var.category, category) && var.category[category.size()] == '/');
				if (covers && category.size() > bestLength)
				{
					best = id;
					bestLength = category.size();
				}
			}
		return best;
	}

	static int componentCount(const TweakVar& var)
	{
		switch (var.type)
		{
		case ETweakType::Float:  return 1;
		case ETweakType::Float2: return 2;
		case ETweakType::Float3: return 3;
		case ETweakType::Float4: return 4;
		case ETweakType::Color3: return var.intensity ? 4 : 3;
		case ETweakType::Color4: return 4;
		case ETweakType::Bool:   return 1;
		case ETweakType::Int:    return 1;
		case ETweakType::Enum:   return 1;
		}
		return 0;
	}

	static Value readValue(const TweakVar& var)
	{
		Value value;
		value.count = componentCount(var);
		switch (var.type)
		{
		case ETweakType::Bool:
			value.v[0] = *static_cast<const bool*>(var.data) ? 1.0f : 0.0f;
			break;
		case ETweakType::Int:
		case ETweakType::Enum:
			value.v[0] = float(*static_cast<const int*>(var.data));
			break;
		default:
			std::memcpy(value.v, var.data, (var.type == ETweakType::Color3 ? 3 : value.count) * sizeof(float));
			if (var.type == ETweakType::Color3 && var.intensity)
				value.v[3] = *var.intensity;
			break;
		}
		return value;
	}

	// Values arrive off the wire and out of a hand-editable file: finite-check everything, clamp
	// to the var's OWN bounds, fire onChange only on a real change.
	static void writeValue(TweakVar& var, const Value& value)
	{
		for (int i = 0; i < value.count; ++i)
			if (!std::isfinite(value.v[i]))
				return;
		const Value before = readValue(var);
		const bool bounded = !var.isUnbounded();
		const auto clamped = [&](float v) { return bounded ? glm::clamp(v, var.min, var.max) : v; };
		switch (var.type)
		{
		case ETweakType::Float:
			*static_cast<float*>(var.data) = clamped(value.v[0]);
			break;
		case ETweakType::Bool:
			*static_cast<bool*>(var.data) = value.v[0] != 0.0f;
			break;
		case ETweakType::Int:
			*static_cast<int*>(var.data) = int(std::lround(clamped(value.v[0])));
			break;
		case ETweakType::Enum:
		{
			const int hi = var.enumNames.empty() ? 0 : int(var.enumNames.size()) - 1;
			*static_cast<int*>(var.data) = glm::clamp(int(std::lround(value.v[0])), 0, hi);
			break;
		}
		case ETweakType::Color3:
			std::memcpy(var.data, value.v, size_t(glm::min(value.count, 3)) * sizeof(float));
			if (var.intensity && value.count >= 4)
				*var.intensity = clamped(value.v[3]); // min/max bound the intensity drag
			break;
		default: // Float2/3/4, Color4 - component count from the TYPE, never from the record
			std::memcpy(var.data, value.v, size_t(glm::min(value.count, componentCount(var))) * sizeof(float));
			break;
		}
		if (!(readValue(var) == before))
			get().notifyChanged(var);
	}

	bool applySavedValue(TweakVar& var)
	{
		const auto it = m_savedValues.find(keyOf(var));
		if (it == m_savedValues.end() || it->second.empty())
			return false;
		Value value;
		value.count = glm::min(int(it->second.size()), 4);
		std::memcpy(value.v, it->second.data(), size_t(value.count) * sizeof(float));
		writeValue(var, value);
		return true;
	}

	bool applyOverride(TweakVar& var)
	{
		if (m_overrides.empty())
			return false;
		const auto it = m_overrides.find(keyOf(var));
		if (it == m_overrides.end())
			return false;
		Value value;
		value.count = glm::min(int(it->second.size()), 4);
		std::memcpy(value.v, it->second.data(), size_t(value.count) * sizeof(float));
		writeValue(var, value);
		return true;
	}

	void saveFile()
	{
		m_saveDirty = false;
		m_saveTimer = 0.0f;
		for (const TweakVar& var : m_vars)
			if (anyFlag(var.flags, ETweakFlags::Saved) && (m_overrides.empty() || m_overrides.find(keyOf(var)) == m_overrides.end())) // overridden: the file keeps its own value
			{
				const Value value = readValue(var);
				m_savedValues[keyOf(var)].assign(value.v, value.v + value.count);
			}
		for (auto it = m_savedValues.begin(); it != m_savedValues.end();) // locks are not saved (an older file may still hold their state)
			it = oc::startsWith(oc::string_view(it->first.data(), it->first.size()), c_lockPrefix) ? m_savedValues.erase(it) : oc::next(it);
		if (!m_writeFile)
			return; // no IO hook installed (headless tooling): Saved is inert
		std::ostringstream file;
		file << std::setprecision(9);
		for (const auto& [key, values] : m_savedValues) // unknown keys kept - other run modes
		{
			if (values.empty())
				continue;
			file << key << " =";
			for (const float v : values)
				file << ' ' << v;
			file << '\n';
		}
		if (!m_writeFile(c_savePath, oc::fromStd(file.str())))
			Log::warning("Tweaks: cannot write " + oc::string(c_savePath));
	}

	static constexpr const char* c_savePath = "Local/tweaks.cfg";

	oc::vector<TweakVar> m_vars;
	oc::vector<Value> m_snapshots; // parallel to m_vars - last value seen by update()
	oc::vector<uint32> m_varLocks;  // parallel to m_vars - the nearest lock above each (c_noLock = none)
	oc::vector<uint8> m_varLocked;  // parallel to m_vars - the row's own lock state (meaningful under a lock)
	oc::vector<uint8> m_varLockable; // parallel to m_vars - the owner said a lock means something here (markLockable)
	oc::vector<TweakLock> m_locks;
	struct ChangeListener
	{
		const void* owner;
		oc::function<void(const TweakVar&)> fn;
	};
	oc::vector<ChangeListener> m_changeListeners;
	oc::map<oc::string, oc::vector<float>> m_savedValues; // the file image, unknown keys preserved
	oc::map<oc::string, oc::vector<float>> m_overrides;   // --tweak command-line overrides: win over the file, never saved
	ETweakFlags m_defaultFlags = ETweakFlags::None;
	uint32 m_syncGeneration = 0;
	float m_saveTimer = 0.0f;
	bool m_saveDirty = false;
	bool m_savedLoaded = false;
	ReadFileFn m_readFile;   // installed by main (FileSystem) - Core cannot import File
	WriteFileFn m_writeFile;
};

// ---- Convenience registration helpers --------------------------------------------------
// Pass a string range of "0-inf" by using FLT_MAX as the max (renders as a drag instead of a slider).
// Every helper takes optional flags LAST (after onChange - pass {} for onChange when only flags are
// wanted); to flag a whole registerTweaks() block, put one Tweak::ScopedFlags at its top instead.

export namespace Tweak
{
	// RAII block default: registrations while alive that pass no explicit flags get these.
	//   const Tweak::ScopedFlags scoped(ETweakFlags::Saved | ETweakFlags::Synced);
	class ScopedFlags
	{
	public:
		explicit ScopedFlags(ETweakFlags flags) : m_previous(TweakRegistry::get().defaultFlags())
		{
			TweakRegistry::get().setDefaultFlags(flags);
		}
		~ScopedFlags() { TweakRegistry::get().setDefaultFlags(m_previous); }
		ScopedFlags(const ScopedFlags&) = delete;
		ScopedFlags& operator=(const ScopedFlags&) = delete;
	private:
		ETweakFlags m_previous;
	};

	inline void floatVar(oc::string_view category, oc::string_view name, float* value, float min = 0.0f, float max = 1.0f, float speed = 0.01f, oc::function<void()> onChange = {}, ETweakFlags flags = ETweakFlags::None)
	{
		TweakVar var{ name, category, ETweakType::Float, value, min, max, speed };
		var.onChange = oc::move(onChange);
		var.flags = flags;
		TweakRegistry::get().registerVar(var);
	}

	inline void float2(oc::string_view category, oc::string_view name, glm::vec2* value, float speed = 0.01f, oc::function<void()> onChange = {}, ETweakFlags flags = ETweakFlags::None)
	{
		TweakVar var{ name, category, ETweakType::Float2, value, 0.0f, FLT_MAX, speed };
		var.onChange = oc::move(onChange);
		var.flags = flags;
		TweakRegistry::get().registerVar(var);
	}

	inline void float3(oc::string_view category, oc::string_view name, glm::vec3* value, float speed = 0.01f, oc::function<void()> onChange = {}, ETweakFlags flags = ETweakFlags::None)
	{
		TweakVar var{ name, category, ETweakType::Float3, value, 0.0f, FLT_MAX, speed };
		var.onChange = oc::move(onChange);
		var.flags = flags;
		TweakRegistry::get().registerVar(var);
	}

	inline void float4(oc::string_view category, oc::string_view name, glm::vec4* value, float speed = 0.01f, oc::function<void()> onChange = {}, ETweakFlags flags = ETweakFlags::None)
	{
		TweakVar var{ name, category, ETweakType::Float4, value, 0.0f, FLT_MAX, speed };
		var.onChange = oc::move(onChange);
		var.flags = flags;
		TweakRegistry::get().registerVar(var);
	}

	// intensityMin/Max/Speed only affect the intensity drag (ignored when intensity == nullptr).
	inline void color3(oc::string_view category, oc::string_view name, glm::vec3* color, float* intensity = nullptr,
		float intensityMin = 0.0f, float intensityMax = FLT_MAX, float intensitySpeed = 0.05f, oc::function<void()> onChange = {}, ETweakFlags flags = ETweakFlags::None)
	{
		TweakVar var{ name, category, ETweakType::Color3, color, intensityMin, intensityMax, intensitySpeed };
		var.intensity = intensity;
		var.onChange = oc::move(onChange);
		var.flags = flags;
		TweakRegistry::get().registerVar(var);
	}

	inline void color4(oc::string_view category, oc::string_view name, glm::vec4* color, oc::function<void()> onChange = {}, ETweakFlags flags = ETweakFlags::None)
	{
		TweakVar var{ name, category, ETweakType::Color4, color };
		var.onChange = oc::move(onChange);
		var.flags = flags;
		TweakRegistry::get().registerVar(var);
	}

	// Integer slider (bounded) or drag (max == FLT_MAX). min/max/speed are stored as floats and cast.
	inline void intVar(oc::string_view category, oc::string_view name, int* value, int min = 0, int max = 100, float speed = 1.0f, oc::function<void()> onChange = {}, ETweakFlags flags = ETweakFlags::None)
	{
		TweakVar var{ name, category, ETweakType::Int, value, float(min), float(max), speed };
		var.onChange = oc::move(onChange);
		var.flags = flags;
		TweakRegistry::get().registerVar(var);
	}

	inline void boolean(oc::string_view category, oc::string_view name, bool* value, oc::function<void()> onChange = {}, ETweakFlags flags = ETweakFlags::None)
	{
		TweakVar var{ name, category, ETweakType::Bool, value };
		var.onChange = oc::move(onChange);
		var.flags = flags;
		TweakRegistry::get().registerVar(var);
	}

	inline void enumVar(oc::string_view category, oc::string_view name, int* value, oc::span<const oc::string_view> names, oc::function<void()> onChange = {}, ETweakFlags flags = ETweakFlags::None)
	{
		TweakVar var{ name, category, ETweakType::Enum, value };
		var.enumNames = names;
		var.onChange = oc::move(onChange);
		var.flags = flags;
		TweakRegistry::get().registerVar(var);
	}

	// A lock over whole sections (see TweakLock); returns its id for TweakRegistry::setLocked / lockState.
	inline uint32 lock(oc::string_view name, oc::span<const oc::string_view> categories, bool lockedByDefault, oc::function<void()> onChange = {})
	{
		return TweakRegistry::get().registerLock(name, categories, lockedByDefault, oc::move(onChange));
	}

	// A system's reaction to `variable` changing (a registered tweak's variable, e.g. Globals::settings.grass.bladesPerPatch).
	// Main thread, after the change. The owner must call removeListeners(owner) before it dies (a member capture).
	template<typename T>
	void onChange(const T& variable, const void* owner, oc::function<void()> fn)
	{
		[[maybe_unused]] const bool found = TweakRegistry::get().addListener(&variable, owner, oc::move(fn));
		assert(found && "Tweak::onChange: no tweak registered on this variable (Settings::registerAll ran first?)");
	}

	inline void removeListeners(const void* owner) { TweakRegistry::get().removeListeners(owner); }

	// Code wrote a setting (a game preset, a stat): the same change path as a panel edit (TweakRegistry::notifyChanged).
	// Main thread. A variable no row holds is ignored.
	template<typename T>
	void notifyChanged(const T& variable)
	{
		if (const TweakVar* var = TweakRegistry::get().findVar(&variable))
			TweakRegistry::get().notifyChanged(*var);
	}
}
