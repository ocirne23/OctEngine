export module Core.Tweaks;

import Core;
import Core.glm;
import Core.Log;

// A lightweight, global registry of "tweakable" variables. Any module can register a
// pointer to a live variable together with how it should be presented; the UI's TweakPanel
// iterates the registry and renders the appropriate widget. Pointers are non-owning: the
// registered variable must outlive the registration (use globals / long-lived members).

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

// Optional behavior flags. Saved: the value persists to Assets/Local/tweaks.cfg — loaded at
// startup (TweakRegistry::loadSaved from main), written back debounced whenever a change is
// detected. Synced: in a network session the SERVER's value broadcasts to clients on change and
// at join (NetworkManager watches syncGeneration()). Identity is "Category/Name" — renaming a
// flagged tweak orphans its saved value (harmless: unknown keys are kept but never applied).
export enum class ETweakFlags : uint8
{
	None   = 0,
	Saved  = 1,
	Synced = 2,
};
export constexpr ETweakFlags operator|(ETweakFlags a, ETweakFlags b) { return ETweakFlags(uint8(a) | uint8(b)); }
export constexpr bool anyFlag(ETweakFlags a, ETweakFlags b) { return (uint8(a) & uint8(b)) != 0; }

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

	oc::function<void()> onChange;                       // optional, fired when the value changes

	ETweakFlags      flags = ETweakFlags::None;

	bool isUnbounded() const { return max >= FLT_MAX * 0.5f; }
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
		m_vars.push_back(var);
		TweakVar& stored = m_vars.back();
		if (stored.flags == ETweakFlags::None)
			stored.flags = m_defaultFlags; // ScopedFlags block default; explicit flags win
		if (m_savedLoaded && anyFlag(stored.flags, ETweakFlags::Saved))
			applySavedValue(stored);
		m_snapshots.push_back(readValue(stored));
	}

	const oc::vector<TweakVar>& vars() const { return m_vars; }

	// ScopedFlags support — the default applied to registrations that pass no explicit flags.
	ETweakFlags defaultFlags() const { return m_defaultFlags; }
	void setDefaultFlags(ETweakFlags flags) { m_defaultFlags = flags; }

	// ---- Saved --------------------------------------------------------------------------
	// Core sits BELOW the File library, so it cannot touch the disk itself: main() installs these
	// two hooks (FileSystem::readFileStr / writeFileStr) before loadSaved(). Without them the
	// Saved flag is simply inert — the registry keeps working in-memory.
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
	// so polling is the only reliable change hook: flagged vars diff against a snapshot — Saved
	// changes arm a debounced file write (sliders drag every frame), Synced changes bump the
	// generation the NetworkManager broadcasts on.
	void update(float deltaSec)
	{
		for (size_t i = 0; i < m_vars.size(); ++i)
		{
			const TweakVar& var = m_vars[i];
			if (var.flags == ETweakFlags::None)
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

	// Receive side. Unknown keys, un-Synced vars and type mismatches are IGNORED — the sender
	// cannot modify anything the receiver did not itself flag as Synced; values clamp to the
	// receiver's own bounds. Applied values refresh the snapshot, so they neither re-save the
	// client's file nor bounce a sync generation.
	void applySyncedBlob(oc::span<const uint8> blob)
	{
		size_t cursor = 0;
		while (cursor < blob.size())
		{
			const uint8 keyLen = blob[cursor++];
			if (cursor + keyLen + 2 > blob.size())
				return; // malformed — drop the rest
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
		default: // Float2/3/4, Color4 — component count from the TYPE, never from the record
			std::memcpy(var.data, value.v, size_t(glm::min(value.count, componentCount(var))) * sizeof(float));
			break;
		}
		if (var.onChange && !(readValue(var) == before))
			var.onChange();
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

	void saveFile()
	{
		m_saveDirty = false;
		m_saveTimer = 0.0f;
		for (const TweakVar& var : m_vars)
			if (anyFlag(var.flags, ETweakFlags::Saved))
			{
				const Value value = readValue(var);
				m_savedValues[keyOf(var)].assign(value.v, value.v + value.count);
			}
		if (!m_writeFile)
			return; // no IO hook installed (headless tooling): Saved is inert
		std::ostringstream file;
		file << std::setprecision(9);
		for (const auto& [key, values] : m_savedValues) // unknown keys kept — other run modes
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
	oc::vector<Value> m_snapshots; // parallel to m_vars — last value seen by update()
	oc::map<oc::string, oc::vector<float>> m_savedValues; // the file image, unknown keys preserved
	ETweakFlags m_defaultFlags = ETweakFlags::None;
	uint32 m_syncGeneration = 0;
	float m_saveTimer = 0.0f;
	bool m_saveDirty = false;
	bool m_savedLoaded = false;
	ReadFileFn m_readFile;   // installed by main (FileSystem) — Core cannot import File
	WriteFileFn m_writeFile;
};

// ---- Convenience registration helpers --------------------------------------------------
// Pass a string range of "0-inf" by using FLT_MAX as the max (renders as a drag instead of a slider).
// Every helper takes optional flags LAST (after onChange — pass {} for onChange when only flags are
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
}
