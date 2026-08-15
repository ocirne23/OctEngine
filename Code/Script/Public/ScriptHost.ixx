export module Script;

import Core;

// The DSL scripting-language subsystem (editor-agnostic: data model, autocomplete rules, the engine-exposure
// registry, native save/load, and the C++ transpiler) -- UI's Script Editor panel is the only consumer today,
// but none of this depends on UI/ImGui, so it lives here alongside the rest of the script pipeline.
export import :DSL;
export import :ScriptLang;
export import :ScriptBindings;
export import :ScriptLoader;
export import :Transpiler;

// Compiled-and-loaded script DLL: the raw entry-point function pointers. Typed as void* so this library
// stays decoupled from the script ABI (ScriptAPI.h) and from the engine — the caller (Entity) owns the
// ScriptContext and casts these. A null `update` marks a script that failed to compile.
export struct ScriptModule
{
    std::string dllPath;
    std::string scriptPath;
    // Do not cache these pointers, they can get swapped when scripts reload!
    void* onSpawn = nullptr;   // ScriptInitFn
    void* update = nullptr;    // ScriptUpdateFn
    void* onDestroy = nullptr; // ScriptShutdownFn
    void* onEvent = nullptr;   // ScriptOnEventFn
    void* onPhysicsEvent = nullptr; // ScriptOnPhysicsEventFn
    uint32 dataSize = 0;       // bytes of persistent ScriptData the script declares (0 = none), from ScriptDataSize()
    uint32 dataLayoutId = 0;   // hash of ScriptData's field layout, from ScriptDataLayoutId() -- a reload that
                                // changes it can't reuse an existing block (see ScriptDataLayoutIdFn)
    // The script's EXPOSED ScriptData fields, from ScriptDataFields() -- an array of ScriptAPI.h's
    // OcScriptField, typed as void* here for the same reason the entry points are: this library doesn't depend
    // on the script ABI, and the consumer (the editor) casts. Points into the module's own static storage, so it
    // is valid exactly as long as this module is -- never cache it across a reload. Null when the script exposes
    // nothing, which is every script that never marks a field private/public.
    const void* dataFields = nullptr;
    int numDataFields = 0;
    uint32 requiredComponents = 0; // EComponentID bitmask (0 = none), from ScriptRequiredComponents()
	std::vector<std::string> eventNames; // in the order the script declared them

	// Set when an entry point hardware-faulted (caught by Entity's SEH-guarded invokers -- integer divide by
	// zero, a stale pointer). Every entry-point gate skips a faulted module; the next successful (re)load
	// clears it, so F6 / Compile & Run is also the recovery path. Mutable for the same reason as the map
	// below; a plain bool because concurrent sets from parallel-ticking scripts are a benign race.
	mutable bool faulted = false;

	// Global EventKey -> this script's local OnEvent index (the position its compiled OnEvent switch expects).
	// Owned/filled by ScriptEventManager in onScriptLoadedCallback and rebuilt on every reload (indices can
	// shift when events are added/removed/reordered); mutable so it can be written through the const ScriptModule*
	// the load callback receives. Lets fireEvent translate a fired key into this script's eventIdx with one lookup.
	mutable std::unordered_map<uint32, int> eventKeyToIndex;
};

export typedef void(*ScriptLoadedCallback)(const ScriptModule* script, const std::vector<std::string>& oldEventNames);

// Compiles visual scripts to self-contained DLLs via the installed MSVC toolchain and caches them by path.
// Extension-agnostic: a .scr (node-graph-generated) and a .dsl (DSL-editor-generated, see Code/Script/Private/
// DSL/Transpiler.ixx) are both just a source file whose body-only C++ compiles under the same ScriptAPI.h ABI --
// getOrLoad never inspects the extension, only the file's content. Pure compile/load only: it knows nothing
// about the engine — no renderer, entity or input.
export class ScriptHost final
{
public:

    ScriptHost();
    ~ScriptHost();

    // Returns the cached module for `path`, compiling it on first use (or when forced). Returns null and
    // keeps the previous build on failure; first-time failures are cached so they aren't retried each frame.
    // The returned pointer is stable until shutdown().
    const ScriptModule* getOrLoad(const std::string& path, bool forceRecompile = false);

	void setCurrentScriptPath(const std::string& path) { m_currentScriptPath = path; }
	void reloadCurrentScript() { if (!m_currentScriptPath.empty()) getOrLoad(m_currentScriptPath, true); }

private:

    void sweepPendingPdbs();
    void retirePdbs(const std::string& dir, const std::string& stem, const std::string& keep);
    const std::string& findVcvars();
    bool compile(const std::string& sourcePath, const std::string& pdbPath, std::string& outDll, std::string& outErrors);
    std::string scriptDllPath(const std::string& sourcePath) const;
    struct CachedScript;
    bool loadDll(CachedScript& slot, const std::string& dll);
    void unloadAll();

private:

    ScriptHost(const ScriptHost&) = delete;
    ScriptHost& operator=(const ScriptHost&) = delete;

    friend class ScriptEventManager;

    ScriptLoadedCallback m_scriptLoadedCallback = nullptr;

    std::string vcvarsPath;                                 // cached after first lookup
    std::unordered_map<std::string, CachedScript> scripts;  // keyed by canonical source path
    std::vector<std::string> pendingPdbDeletes;             // superseded PDBs the debugger still holds; retried later

	std::string m_currentScriptPath;                        // the script the editor panel is currently editing (F6 recompiles it)
};

export namespace Globals
{
    ScriptHost scriptHost;
}
