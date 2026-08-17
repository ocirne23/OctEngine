#ifdef SCRIPTS_STATIC
module;
// Cooked build: the registry the compiled-in scripts self-register with needs the ABI structs (ScriptStaticEntries).
#include "ScriptAPI.h"
#endif
module Script;

import Core;
import Core.Windows;
import Core.Log;
import File; // ALL disk access goes through FileSystem (Core no longer exports <filesystem>)

#ifdef SCRIPTS_STATIC
// Cooked build: scripts compiled into the engine (App-Scripts aggregate) register their entry-point functions here
// at static-init via the REGISTER_*() macros in each .scr. A Meyers singleton so it is constructed before any
// registrar runs, whatever the cross-TU init order. ScriptHost::getOrLoad resolves script paths against this
// instead of invoking cl / LoadLibrary.
struct StaticScriptFns { void* fns[OC_SCRIPT_ENTRY_COUNT] = {}; };

static oc::unordered_map<oc::string, StaticScriptFns>& staticScriptRegistry()
{
    static oc::unordered_map<oc::string, StaticScriptFns> reg;
    return reg;
}

static const StaticScriptFns* findStaticEntries(const oc::string& path)
{
    auto& reg = staticScriptRegistry();
    if (auto it = reg.find(path); it != reg.end())
        return &it->second;
    // Lenient match: prefabs spell the path "Scripts/X.scr"; the aggregate registered whatever OC_CURRENT_SCRIPT
    // held. Compare on forward-slash-normalized paths, allowing one to be a tail of the other (rel vs abs).
    oc::string norm = path;
    for (char& c : norm) if (c == '\\') c = '/';
    for (auto& [k, v] : reg)
    {
        oc::string nk = k;
        for (char& c : nk) if (c == '\\') c = '/';
        if (norm == nk || norm.ends_with(nk) || nk.ends_with(norm))
            return &v;
    }
    return nullptr;
}

// Called from each script's REGISTER_*() macro (declared in ScriptAPI.h). extern "C" + file scope so it is one
// external symbol the App-Scripts aggregate links against.
extern "C" void ocRegisterScriptEntry(const char* scriptPath, int kind, void* fn)
{
    if (scriptPath && fn && kind >= 0 && kind < OC_SCRIPT_ENTRY_COUNT)
        staticScriptRegistry()[scriptPath].fns[kind] = fn;
}
#endif

namespace
{

    // Runs a full command line synchronously, no console window. Returns the child exit code, or -1.
    int runProcess(const oc::string& cmdLine)
    {
        STARTUPINFOA si{}; si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        oc::vector<char> buffer(cmdLine.begin(), cmdLine.end());
        buffer.push_back('\0');
        if (!CreateProcessA(nullptr, buffer.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
            return -1;
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return (int)code;
    }

    oc::string readTextFile(const oc::string& path)
    {
        // Compiling a script is an explicit, blocking user action (F6 / editor save / a spawn that
        // needs its module), so the whole path is main-thread by design.
        return FileSystem::readFileStr(path, /*allowMainThread*/ true);
    }

    // Canonical cache key for a script. The same file reaches getOrLoad spelled different ways — the prefab
    // stores a relative "Scripts/Foo.scr", the asset browser hands over an absolute backslash path — so key
    // by the resolved absolute path. Otherwise a panel reload and the owning entity land in different slots
    // and the entity never sees the recompile (hot reload silently no-ops).
    oc::string cacheKey(const oc::string& path)
    {
        const oc::string resolved = FileSystem::weaklyCanonicalPath(FileSystem::absolutePath(path, true), true);
        return resolved.empty() ? FileSystem::absolutePath(path, true) : resolved;
    }

    struct PdbScan
    {
        int                   maxSerial = -1; // highest serial found on disk (-1 if none)
        oc::vector<oc::string> files;      // every "<stem>.<serial>.pdb" in the directory
    };

    // Finds every program PDB named "<stem>.<serial>.pdb" (serial = decimal digits) in `dir`. The compiler
    // PDB "<stem>.building.pdb" is skipped (its middle segment isn't all digits), as is any other script's.
    PdbScan scanPdbs(const oc::string& dir, const oc::string& stem)
    {
        PdbScan out;
        const oc::string prefix = stem + ".";
        oc::vector<FileSystem::DirEntry> entries;
        FileSystem::listDirectory(dir, entries, /*allowMainThread*/ true);
        for (const FileSystem::DirEntry& entry : entries)
        {
            if (entry.isDirectory || entry.extension != ".pdb")
                continue;
            const oc::string& name = entry.name;
            if (name.size() < prefix.size() + 5)                         // need at least "<prefix>0.pdb"
                continue;
            if (name.compare(0, prefix.size(), prefix) != 0)
                continue;
            const oc::string mid = name.substr(prefix.size(), name.size() - prefix.size() - 4); // strip ".pdb"
            int serial = 0;
            bool digits = !mid.empty();
            for (char c : mid) { if (c < '0' || c > '9') { digits = false; break; } serial = serial * 10 + (c - '0'); }
            if (!digits)
                continue;
            out.files.push_back(entry.path);
            if (serial > out.maxSerial) out.maxSerial = serial;
        }
        return out;
    }
}

// A loaded module plus the DLL handle/path needed to unload and recompile it.
struct ScriptHost::CachedScript
{
    ScriptModule entries;
    void*        module = nullptr; // HMODULE
    oc::string  dllPath;
    oc::string  pdbPath;          // program PDB of the loaded build
    int          pdbSerial = -1;   // monotonic build counter; next build uses pdbSerial+1 for a fresh PDB name
};


// Try to delete every superseded PDB; drop the ones now gone. The VS debugger keeps PDBs cached after a
// module unloads, so a just-superseded PDB often can't be deleted until a later build/shutdown.
void ScriptHost::sweepPendingPdbs()
{
    oc::erase_if(pendingPdbDeletes, [](const oc::string& p)
    {
        FileSystem::remove(p, /*allowMainThread*/ true);
        return !FileSystem::exists(p, true); // removed (or already gone) -> stop tracking it
    });
}

// Best-effort deletion of every "<stem>.<serial>.pdb" in `dir` except `keep` (the live PDB, matched by
// filename). Includes orphans from earlier runs at any index. Files still locked (typically by the VS
// debugger) are queued in pendingPdbDeletes for a later sweep.
void ScriptHost::retirePdbs(const oc::string& dir, const oc::string& stem, const oc::string& keep)
{
    const oc::string keepName = FileSystem::filename(keep);
    for (const oc::string& p : scanPdbs(dir, stem).files)
    {
        if (FileSystem::filename(p) == keepName)
            continue;
        FileSystem::remove(p, /*allowMainThread*/ true);
        if (FileSystem::exists(p, true))
        {
            bool tracked = false;
            for (const oc::string& q : pendingPdbDeletes) if (q == p) { tracked = true; break; }
            if (!tracked) pendingPdbDeletes.push_back(p);
        }
    }
}

// Locates vcvars64.bat via vswhere (once, then cached). Empty string on failure.
const oc::string& ScriptHost::findVcvars()
{
    if (!vcvarsPath.empty()) return vcvarsPath;

    char pfBuffer[512];
    const DWORD len = GetEnvironmentVariableA("ProgramFiles(x86)", pfBuffer, sizeof(pfBuffer));
    const oc::string programFiles = (len > 0 && len < sizeof(pfBuffer)) ? oc::string(pfBuffer, len) : oc::string("C:\\Program Files (x86)");
    const oc::string vswhere = programFiles + "\\Microsoft Visual Studio\\Installer\\vswhere.exe";
    if (FileSystem::exists(vswhere, /*allowMainThread*/ true))
    {
        const oc::string outDir = FileSystem::join(FileSystem::currentPath(true), "Local/Scripts");
        FileSystem::createDirectories(outDir, true);
        const oc::string outFile = FileSystem::join(outDir, "vswhere.out");

        const oc::string inner = "\"" + vswhere + "\" -latest -prerelease -products * -property installationPath > \"" + outFile + "\"";
        runProcess("cmd.exe /c \"" + inner + "\"");

        oc::string installPath = readTextFile(outFile);
        while (!installPath.empty() && (installPath.back() == '\r' || installPath.back() == '\n' || installPath.back() == ' ' || installPath.back() == '\t'))
            installPath.pop_back();

        if (!installPath.empty())
        {
            const oc::string vcvars = installPath + "\\VC\\Auxiliary\\Build\\vcvars64.bat";
            if (FileSystem::exists(vcvars, true)) { vcvarsPath = vcvars; return vcvarsPath; }
        }
    }

    const char* const fallbacks[] = {
        "C:\\Program Files\\Microsoft Visual Studio\\18\\Insiders\\VC\\Auxiliary\\Build\\vcvars64.bat",
        "C:\\Program Files\\Microsoft Visual Studio\\2022\\Enterprise\\VC\\Auxiliary\\Build\\vcvars64.bat",
        "C:\\Program Files\\Microsoft Visual Studio\\2022\\Professional\\VC\\Auxiliary\\Build\\vcvars64.bat",
        "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat",
    };
    for (const char* candidate : fallbacks)
        if (FileSystem::exists(candidate, true)) { vcvarsPath = candidate; return vcvarsPath; }

    Log::error("Script: could not locate vcvars64.bat (vswhere found nothing and no fallback path exists)");
    return vcvarsPath;
}

bool ScriptHost::compile(const oc::string& sourcePath, const oc::string& pdbPath, oc::string& outDll, oc::string& outErrors)
{
    const oc::string& vcvars = findVcvars();
    if (vcvars.empty()) { outErrors = "MSVC toolchain not found (see log)"; return false; }

    // Compiling blocks the main thread by design (F6 / editor save / a spawn needing its module).
    const FileSystem::AllowMainThreadIO allowIo;
    const oc::string assetsDir = FileSystem::currentPath();
    const oc::string outDir = FileSystem::join(assetsDir, "Local/Scripts");
    const oc::string repoDir = FileSystem::parentPath(assetsDir);
    const oc::string includeDir = FileSystem::join(repoDir, "Code/Script/Public"); // the one ScriptAPI.h (ABI source of truth)
    const oc::string glmInclude = FileSystem::join(repoDir, "Dependencies/Include"); // header-only glm for Vec3 math
    FileSystem::createDirectories(outDir);

    if (!FileSystem::exists(sourcePath)) { outErrors = "Script source not found: " + sourcePath; return false; }

    // Build to a temp "<stem>.building.dll": the live "<stem>.dll" is file-locked while loaded, so we
    // can't overwrite it directly. getOrLoad frees the old module then renames this temp over it on
    // success, leaving exactly one DLL per script.
    const oc::string stem = FileSystem::stem(sourcePath);
    const oc::string base = stem + ".building";
    const oc::string dllPath = FileSystem::join(outDir, base + ".dll");
    const oc::string objPath = FileSystem::join(outDir, base + ".obj");
    const oc::string compPdb = FileSystem::join(outDir, base + ".pdb");  // compiler PDB (temp, discarded)
    const oc::string logPath = FileSystem::join(outDir, "build.log");
    const oc::string batPath = FileSystem::join(outDir, "_build.bat");
    const oc::string srcAbs = FileSystem::absolutePath(sourcePath);

    // /Zi + /Od + /link /DEBUG produce a usable PDB (set breakpoints in the .scr, step, inspect locals).
    // /Od (no optimization) is deliberate: scripts are tiny, and it keeps locals/line info intact. The
    // linker writes the PDB to the caller-chosen ping-pong name (absolute path embedded in the DLL), so
    // a rebuild never targets the PDB the debugger is currently holding open (avoids LNK1201).
    oc::string bat;
    bat += "@echo off\r\n";
    bat += "call \"" + vcvars + "\" >nul 2>&1\r\n";
    bat += "cl /nologo /LD /std:c++20 /Zc:preprocessor /MD /Od /Zi /EHsc /arch:AVX2 /wd4100 /DSCRIPT_BUILD"; // /wd4100: unused params; /DSCRIPT_BUILD: Entity is the layout mirror; /Zc:preprocessor: the conformant preprocessor ScriptCtxMacros.h's variadic-arity dispatch needs (the main engine build gets it implicitly via the VS project settings, this standalone cl invocation does not)
    bat += " /I\"" + includeDir + "\"";
    bat += " /I\"" + glmInclude + "\""; // ScriptAPI.h includes <glm/glm.hpp> (Vec3 = glm::vec3)
    bat += " /FI\"" + FileSystem::join(includeDir, "ScriptAPI.h") + "\""; // the .scr is body-only; force-include the ABI header (also cooked into the App-Scripts aggregate)
    bat += " /Fo\"" + objPath + "\"";
    bat += " /Fd\"" + compPdb + "\"";
    bat += " /Fe\"" + dllPath + "\"";
    bat += " /Tp\"" + srcAbs + "\""; // /Tp: compile as C++ regardless of the .scr extension
    bat += " /link /DEBUG /INCREMENTAL:NO /PDB:\"" + pdbPath + "\"";
    bat += " > \"" + logPath + "\" 2>&1\r\n";
    if (!FileSystem::writeFileStr(batPath, bat))
    {
        outErrors = "Could not write build script: " + batPath;
        return false;
    }

    const int code = runProcess("cmd.exe /c \"" + batPath + "\"");

    // cl drops <base>.obj/.pdb and the linker <base>.lib/.exp next to the DLL; clear those intermediates.
    // The kept artifacts are "<stem>.dll" (after rename) and "<stem>.pdb".
    for (const char* ext : { ".obj", ".lib", ".exp", ".pdb" })
    {
        FileSystem::remove(FileSystem::join(outDir, base + ext));
    }

    if (code != 0)
    {
        const oc::string log = readTextFile(logPath);
        outErrors = log.empty() ? ("compiler exited with code " + oc::to_string(code)) : log;
        FileSystem::remove(dllPath); // drop any half-written temp DLL
        return false;
    }
    outDll = dllPath;
    return true;
}

oc::string ScriptHost::scriptDllPath(const oc::string& sourcePath) const
{
    return FileSystem::join(FileSystem::join(FileSystem::currentPath(/*allowMainThread*/ true), "Local/Scripts"),
        FileSystem::stem(sourcePath) + ".dll");
}

// LoadLibrary `dll` and resolve the entry points into `slot`, freeing any module it previously held.
bool ScriptHost::loadDll(CachedScript& slot, const oc::string& dll)
{
    HMODULE m = LoadLibraryA(dll.c_str());

    void* update = m ? (void*)GetProcAddress(m, "Update") : nullptr;
    void* onSpawn = m ? (void*)GetProcAddress(m, "OnSpawn") : nullptr;
    void* onDestroy = m ? (void*)GetProcAddress(m, "OnDestroy") : nullptr;
    void* onEvent = m ? (void*)GetProcAddress(m, "OnEvent") : nullptr;
    void* onPhysicsEvent = m ? (void*)GetProcAddress(m, "OnPhysicsEvent") : nullptr;

    if (!update && !onSpawn && !onDestroy && !onEvent && !onPhysicsEvent) { if (m) FreeLibrary(m); return false; }
    if (slot.module) FreeLibrary((HMODULE)slot.module);
    slot.module = m;
    slot.dllPath = dll;
    slot.entries.dllPath = dll;
    slot.entries.update = update;
    slot.entries.onSpawn = onSpawn;
    slot.entries.onDestroy = onDestroy;
    slot.entries.onEvent = onEvent;
    slot.entries.onPhysicsEvent = onPhysicsEvent;
    slot.entries.faulted = false; // a successful (re)load is the one thing that clears a fault
    slot.entries.dataSize = 0;
    if (auto sizeFn = (uint32(*)())GetProcAddress(m, "ScriptDataSize"))
        slot.entries.dataSize = sizeFn();
    slot.entries.dataLayoutId = 0;
    if (auto layoutFn = (uint32(*)())GetProcAddress(m, "ScriptDataLayoutId"))
        slot.entries.dataLayoutId = layoutFn();
    slot.entries.dataFields = nullptr;
    slot.entries.numDataFields = 0;
    // Cast inline rather than through ScriptAPI.h's typedef: this path deliberately doesn't include the ABI
    // header (only the cooked build does), same as every GetProcAddress above. The table is OcScriptField[]
    // to whoever consumes it -- void* here, exactly like the entry points.
    using ScriptDataFieldsPtr = const void* (*)(int*);
    if (auto fieldsFn = (ScriptDataFieldsPtr)GetProcAddress(m, "ScriptDataFields"))
        slot.entries.dataFields = fieldsFn(&slot.entries.numDataFields);
    slot.entries.requiredComponents = 0;
    if (auto reqFn = (uint32(*)())GetProcAddress(m, "ScriptRequiredComponents"))
        slot.entries.requiredComponents = reqFn();

    const auto oldEventNames = oc::move(slot.entries.eventNames);

    // Resolve On Event entry names once at load, indexed the same way OnEvent(eventIdx) expects, so the
    // host maps a fired event name to its index without the script ever seeing a string.
    slot.entries.eventNames.clear();
    if (auto countFn = (int(*)())GetProcAddress(m, "ScriptEventCount"))
    {
        if (auto nameFn = (const char* (*)(int))GetProcAddress(m, "ScriptEventName"))
        {
            const int count = countFn();
            slot.entries.eventNames.reserve(count > 0 ? count : 0);
            for (int i = 0; i < count; ++i)
                slot.entries.eventNames.emplace_back(nameFn(i));
        }
    }

    // Null when no ScriptEventManager wired one in (DslCompiler's check-compile) -- same guard as the
    // static-registry path.
    if (m_scriptLoadedCallback)
        m_scriptLoadedCallback(&slot.entries, oldEventNames);

    return true;
}

const ScriptModule* ScriptHost::getOrLoad(const oc::string& path, bool forceRecompile)
{
    const oc::string key = cacheKey(path);
    auto it = scripts.find(key);
    if (it != scripts.end() && !forceRecompile)
        return &it->second.entries;

    // Only actual (re)compiles/loads are recorded - the cached path above returns before this.
    ProfileScope profileScope("Script compile", EProfileCategory::Script);

#ifdef SCRIPTS_STATIC
    // Cooked build: the script is baked into the engine binary and self-registered — resolve it from the registry
    // (no cl, no LoadLibrary). forceRecompile is meaningless here (nothing to rebuild at runtime).
    if (const StaticScriptFns* e = findStaticEntries(path))
    {
        CachedScript& slot = scripts.emplace(key, CachedScript{}).first->second;
        slot.entries.scriptPath     = path;
        slot.entries.onSpawn        = e->fns[OC_SCRIPT_ON_SPAWN];
        slot.entries.update         = e->fns[OC_SCRIPT_UPDATE];
        slot.entries.onDestroy      = e->fns[OC_SCRIPT_ON_DESTROY];
        slot.entries.onEvent        = e->fns[OC_SCRIPT_ON_EVENT];
        slot.entries.onPhysicsEvent = e->fns[OC_SCRIPT_ON_PHYSICS_EVENT];
        slot.entries.dataSize       = e->fns[OC_SCRIPT_DATA_SIZE] ? reinterpret_cast<unsigned int(*)()>(e->fns[OC_SCRIPT_DATA_SIZE])() : 0;
        slot.entries.dataLayoutId   = e->fns[OC_SCRIPT_DATA_LAYOUT_ID] ? reinterpret_cast<unsigned int(*)()>(e->fns[OC_SCRIPT_DATA_LAYOUT_ID])() : 0;
        slot.entries.dataFields = nullptr;
        slot.entries.numDataFields = 0;
        if (e->fns[OC_SCRIPT_DATA_FIELDS])
            slot.entries.dataFields = reinterpret_cast<const void* (*)(int*)>(e->fns[OC_SCRIPT_DATA_FIELDS])(&slot.entries.numDataFields);
        slot.entries.requiredComponents = e->fns[OC_SCRIPT_REQUIRED_COMPONENTS] ? reinterpret_cast<unsigned int(*)()>(e->fns[OC_SCRIPT_REQUIRED_COMPONENTS])() : 0;
        slot.entries.eventNames.clear();
        if (void* ec = e->fns[OC_SCRIPT_EVENT_COUNT]; ec && e->fns[OC_SCRIPT_EVENT_NAME])
        {
            auto nameFn = reinterpret_cast<const char*(*)(int)>(e->fns[OC_SCRIPT_EVENT_NAME]);
            for (int i = 0, n = reinterpret_cast<int(*)()>(ec)(); i < n; ++i)
                slot.entries.eventNames.emplace_back(nameFn(i));
        }
        if (m_scriptLoadedCallback)
            m_scriptLoadedCallback(&slot.entries, {});
        Log::info("Static script resolved from the compiled-in registry: " + path);
        return &slot.entries;
    }
    Log::error("Static script not found in the compiled-in registry (rebuild App-Scripts after adding/renaming a .scr?): " + path);
    return nullptr;
#else

    // First load this session and not forced: if an existing DLL's mtime matches the source's (we stamp
    // it to match after each compile), the source is unchanged since it was built — load it, skip cl.
    if (!forceRecompile && it == scripts.end())
    {
        const oc::string dll = scriptDllPath(path);
        const int64 srcTime = FileSystem::lastWriteTimeSec(path, /*allowMainThread*/ true);
        const int64 dllTime = FileSystem::lastWriteTimeSec(dll, true);
        if (srcTime != 0 && srcTime == dllTime)
        {
            CachedScript& slot = scripts.emplace(key, CachedScript{}).first->second;
            slot.entries.scriptPath = path;
            if (loadDll(slot, dll))
            {
                Log::info("Script unchanged; loaded cached DLL: " + path);
                return &slot.entries;
            }
            scripts.erase(key); // cached DLL was unusable; fall through and recompile
        }
    }

    it = scripts.find(key);

    // Each build writes a FRESH, never-reused program PDB "<stem>.<serial>.pdb". The VS debugger caches
    // PDBs even after a module unloads, so reusing a name (even ping-ponging two) eventually collides
    // with a held handle (LNK1201). Name the new PDB with a serial ABOVE every "<stem>.<N>.pdb" already
    // on disk — not just the last one this session made: a prior run (or a build whose PDB the debugger
    // still holds) can leave orphans at any index, and picking max+1 guarantees the linker never targets
    // a name still open, so a delete that failed earlier can never block this build.
    const oc::string outDir = FileSystem::parentPath(scriptDllPath(path));
    const oc::string stem = FileSystem::stem(path);
    int serial = scanPdbs(outDir, stem).maxSerial + 1;
    if (it != scripts.end() && it->second.pdbSerial + 1 > serial)
        serial = it->second.pdbSerial + 1;
    const oc::string pdbPath = FileSystem::join(outDir, stem + "." + oc::to_string(serial) + ".pdb");

    oc::string tempDll, errors;
    if (!compile(path, pdbPath, tempDll, errors))
    {
        Log::error("Script compile failed (" + path + "):\n" + errors);
        if (it != scripts.end()) return &it->second.entries;             // keep previous build
        ScriptModule* scriptModule = &scripts.emplace(key, CachedScript{}).first->second.entries; // cache the failure
        scriptModule->scriptPath = path;
        return scriptModule;
    }

    // Compile succeeded: free the previous build (unlocking its <stem>.dll) and promote the temp DLL
    // into its place, so each script keeps exactly one DLL on disk.
    CachedScript& slot = (it != scripts.end()) ? it->second : scripts.emplace(key, CachedScript{}).first->second;
    slot.entries.scriptPath = path;
    if (slot.module) { FreeLibrary((HMODULE)slot.module); slot.module = nullptr; }

    const oc::string finalDll = scriptDllPath(path);
    FileSystem::remove(finalDll, /*allowMainThread*/ true); // the old build is freed, so this unlinks cleanly
    if (!FileSystem::rename(tempDll, finalDll, true))
    {
        FileSystem::remove(tempDll, true);
        Log::error("Script: could not place DLL (" + finalDll + ")");
        slot.entries = ScriptModule{}; slot.dllPath.clear();
        return &slot.entries;
    }

    // Stamp the DLL with the source's mtime (before loading, while the file is still unlocked) so a
    // later startup recognises an unchanged script and reuses this DLL instead of recompiling.
    FileSystem::copyLastWriteTime(path, finalDll, /*allowMainThread*/ true);

    if (!loadDll(slot, finalDll))
    {
        Log::info("Script: DLL load failed, no exported symbols (" + finalDll + ")");
        FileSystem::remove(finalDll, /*allowMainThread*/ true);
        slot.entries = ScriptModule{}; slot.dllPath.clear();
        retirePdbs(outDir, stem, pdbPath);
        sweepPendingPdbs();
        return &slot.entries;
    }

    // Retire every PDB except the one just loaded: the previous build's plus any orphans left by earlier
    // runs (higher-indexed included). Ones the debugger still holds get queued for a later sweep.
    retirePdbs(outDir, stem, pdbPath);
    slot.pdbPath = pdbPath;
    slot.pdbSerial = serial;
    sweepPendingPdbs();

    Log::info("Script loaded: " + path);
    return &slot.entries;
#endif
}

void ScriptHost::unloadAll()
{
    const oc::string outDir = FileSystem::join(FileSystem::currentPath(/*allowMainThread*/ true), "Local/Scripts");
    for (auto& [path, script] : scripts)
    {
        if (script.module) FreeLibrary((HMODULE)script.module);
        if (!script.dllPath.empty()) FileSystem::remove(script.dllPath, /*allowMainThread*/ true);
        // Delete this script's PDBs — the live one plus any orphans on disk (keep nothing).
        retirePdbs(outDir, FileSystem::stem(path), oc::string());
    }
    scripts.clear();
    sweepPendingPdbs();
}

ScriptHost::ScriptHost() {}
ScriptHost::~ScriptHost() { unloadAll(); }