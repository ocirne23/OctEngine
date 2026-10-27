module File;

import Core;
import Core.Log;
import Core.Windows;
import Core.BitPathTrie;
// The ONLY place these two live now (Core deliberately stops exporting them - see Core.ixx):
// every other library reaches the disk through the FileSystem API below.
import <filesystem>;
import <fstream>;

import :FileSystem;
import :NativeFile;
import :PackFormat;
import :PackFile;

// The one place <filesystem>/<fstream> live now: Core no longer exports them, so every other
// library reaches the disk through the API below.
namespace
{
    std::thread::id g_mainThreadId;          // set by initialize()
    bool g_mainThreadKnown = false;
    thread_local int t_allowMainThreadDepth = 0;

    std::filesystem::path toPath(oc::string_view s)
    {
        return std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t*>(s.data()), s.size()));
    }
    oc::string fromPath(const std::filesystem::path& p)
    {
        const std::u8string s = p.u8string();
        oc::string out(reinterpret_cast<const char*>(s.data()), s.size());
        for (char& c : out) // one separator everywhere: paths get compared and logged as strings
            if (c == '\\')
                c = '/';
        return out;
    }
}

namespace
{
    // A ProfileScope that NO-OPS on threads the profiler does not track. Plain ProfileScope
    // dereferences the calling thread's track (and asserts), but IO legitimately runs on threads
    // the engine deliberately leaves unregistered - the transient startup texture-bake pool, for
    // instance (a track is permanent and 1 MiB, so short-lived pools do not get one).
    struct IoScope final
    {
        oc::optional<ProfileScope> scope;
        explicit IoScope(const char* name)
        {
            if (Globals::profiler.threadTrack() != nullptr)
                scope.emplace(name, EProfileCategory::File);
        }
    };
}

// ---------------------------------------------------------------- the mount table
namespace
{
    struct Mount
    {
        oc::shared_ptr<const PackFile> pack; // null = the disk
        oc::string mountPoint;               // Assets-relative, '/'-terminated; "" = Assets/ itself
        int32 priority = 0;
    };

    // Reads take it shared for the lookup AND the pack read, so a pack cannot unmap under a reader.
    std::shared_mutex g_mountMutex;
    oc::vector<Mount> g_mounts;            // highest priority first; holds the disk too
    bool g_diskEnabled = true;
    oc::atomic<bool> g_packsMounted = false; // false = every call is the plain disk path, no lookup at all

    oc::string g_rootDir;                  // absolute Assets/, '/' separated, '/'-terminated
    oc::string g_cwdRel;                   // the working directory relative to g_rootDir, '/'-terminated
    bool g_cwdInRoot = true;

    // Paths this process wrote / removed / renamed while packs were mounted: lowercase root-relative path hashes,
    // plus removed folders ('/'-terminated). They are served from the disk from then on.
    std::shared_mutex g_overrideMutex;
    oc::unordered_set<uint64> g_overrides;
    oc::vector<oc::string> g_overriddenDirs;
    oc::atomic<bool> g_hasOverrides = false;

    bool packsMounted() { return g_packsMounted.load(oc::memory_order_acquire); }

    bool isAbsoluteView(oc::string_view path)
    {
        return (path.size() >= 2 && path[1] == ':') || (!path.empty() && (path[0] == '/' || path[0] == '\\'));
    }

    // path -> Assets-relative, normalized. False when it lies outside Assets/ (the disk alone serves it).
    bool toRootRelative(oc::string_view path, char* out, uint32& outLength)
    {
        if (isAbsoluteView(path))
        {
            const oc::string_view root = g_rootDir;
            if (root.empty())
                return false;
            const size_t rootLength = root.size() - 1; // without the '/'
            if (path.size() < rootLength)
                return false;
            for (size_t i = 0; i < rootLength; ++i)
            {
                const char a = path[i] == '\\' ? '/' : Pack::lowerAscii(path[i]);
                if (a != Pack::lowerAscii(root[i]))
                    return false;
            }
            if (path.size() > rootLength && path[rootLength] != '/' && path[rootLength] != '\\')
                return false;
            return Pack::normalizeRelative(path.substr(rootLength), out, outLength);
        }
        if (!g_cwdInRoot)
            return false;
        if (g_cwdRel.empty())
            return Pack::normalizeRelative(path, out, outLength);
        char joined[Pack::MAX_PATH_CHARS * 2];
        if (g_cwdRel.size() + path.size() >= sizeof(joined))
            return false;
        memcpy(joined, g_cwdRel.data(), g_cwdRel.size());
        memcpy(joined + g_cwdRel.size(), path.data(), path.size());
        return Pack::normalizeRelative(oc::string_view(joined, g_cwdRel.size() + path.size()), out, outLength);
    }

    struct VfsPath
    {
        char rel[Pack::MAX_PATH_CHARS];
        uint32 length = 0;
        bool inRoot = false;

        explicit VfsPath(oc::string_view path) { inRoot = toRootRelative(path, rel, length); }
        oc::string_view view() const { return oc::string_view(rel, length); }
    };

    uint64 lowerHash(oc::string_view text)
    {
        char lower[Pack::MAX_PATH_CHARS];
        const size_t length = oc::min(text.size(), sizeof(lower));
        for (size_t i = 0; i < length; ++i)
            lower[i] = Pack::lowerAscii(text[i]);
        return Pack::hashBytes(lower, length);
    }

    bool isOverridden(oc::string_view rel)
    {
        if (!g_hasOverrides.load(oc::memory_order_acquire))
            return false;
        const uint64 hash = lowerHash(rel);
        std::shared_lock lock(g_overrideMutex);
        if (g_overrides.count(hash) != 0)
            return true;
        for (const oc::string& dir : g_overriddenDirs)
            if (Pack::startsWithNoCase(rel, dir) || (rel.size() + 1 == dir.size() && Pack::startsWithNoCase(dir, rel)))
                return true;
        return false;
    }

    void recordOverride(oc::string_view path)
    {
        if (!packsMounted())
            return;
        const VfsPath p(path);
        if (!p.inRoot)
            return;
        const uint64 hash = lowerHash(p.view());
        std::unique_lock lock(g_overrideMutex);
        g_overrides.insert(hash);
        g_hasOverrides.store(true, oc::memory_order_release);
    }

    void recordOverriddenDir(oc::string_view path)
    {
        if (!packsMounted())
            return;
        const VfsPath p(path);
        if (!p.inRoot)
            return;
        oc::string dir(p.rel, p.length);
        dir += '/';
        std::unique_lock lock(g_overrideMutex);
        g_overriddenDirs.push_back(oc::move(dir));
        g_hasOverrides.store(true, oc::memory_order_release);
    }

    // rel inside the mount's folder -> the pack's path. "Shaders" against "Shaders/" is the pack root ("").
    bool stripMount(oc::string_view rel, oc::string_view mountPoint, oc::string_view& out)
    {
        if (mountPoint.empty())
        {
            out = rel;
            return true;
        }
        if (rel.size() + 1 == mountPoint.size() && Pack::startsWithNoCase(mountPoint, rel))
        {
            out = {};
            return true;
        }
        if (!Pack::startsWithNoCase(rel, mountPoint))
            return false;
        out = rel.substr(mountPoint.size());
        return true;
    }

    bool diskExists(oc::string_view path)
    {
        std::error_code ec;
        return std::filesystem::exists(toPath(path), ec);
    }

    enum class EWhere : uint8 { Disk, Pack, Missing };

    struct Hit
    {
        const Mount* mount = nullptr;
        uint32 key = PackFile::INVALID_KEY;
        oc::string_view packPath; // into the VfsPath
    };

    // The mount that serves the FILE at path. Caller holds g_mountMutex. A disk mount with no pack below it answers
    // without touching the disk (the disk call itself then reports a miss); one with packs below checks first.
    EWhere locate(const VfsPath& p, oc::string_view path, Hit& hit)
    {
        if (!p.inRoot)
            return EWhere::Disk;
        for (size_t i = 0; i < g_mounts.size(); ++i)
        {
            const Mount& m = g_mounts[i];
            if (!m.pack)
            {
                if (!g_diskEnabled)
                    continue;
                bool packsBelow = false;
                for (size_t j = i + 1; j < g_mounts.size() && !packsBelow; ++j)
                    packsBelow = g_mounts[j].pack != nullptr;
                if (!packsBelow || diskExists(path))
                    return EWhere::Disk;
                continue;
            }
            oc::string_view packPath;
            if (!stripMount(p.view(), m.mountPoint, packPath) || packPath.empty())
                continue;
            const uint32 key = m.pack->findFile(packPath);
            if (key == PackFile::INVALID_KEY)
                continue;
            if (isOverridden(p.view()))
                return EWhere::Disk;
            hit = Hit{ &m, key, packPath };
            return EWhere::Pack;
        }
        return isOverridden(p.view()) ? EWhere::Disk : EWhere::Missing;
    }

    // A folder some pack holds (or one its mount point lies in). Caller holds g_mountMutex.
    bool packHasDirectory(const VfsPath& p)
    {
        if (!p.inRoot || isOverridden(p.view()))
            return false;
        for (const Mount& m : g_mounts)
        {
            if (!m.pack)
                continue;
            const oc::string_view mountPoint = m.mountPoint;
            if (!mountPoint.empty() && Pack::startsWithNoCase(mountPoint, p.view())
                && (p.length == 0 || mountPoint[p.length] == '/'))
                return true;
            oc::string_view packPath;
            if (stripMount(p.view(), mountPoint, packPath) && m.pack->isDirectory(packPath))
                return true;
        }
        return false;
    }

    bool diskAllowed(const VfsPath& p) { return g_diskEnabled || !p.inRoot || isOverridden(p.view()); }

    // Inserts before the first mount of the same or a lower priority: ties go to the later mount.
    void insertMount(Mount&& mount)
    {
        auto it = g_mounts.begin();
        while (it != g_mounts.end() && it->priority > mount.priority)
            ++it;
        g_mounts.insert(it, oc::move(mount));
    }

    void ensureDiskMount()
    {
        for (const Mount& m : g_mounts)
            if (!m.pack)
                return;
        insertMount(Mount{ nullptr, oc::string(), FileSystem::DISK_PRIORITY_DEFAULT });
    }

    void refreshPacksMounted()
    {
        bool any = false;
        for (const Mount& m : g_mounts)
            any |= m.pack != nullptr;
        g_packsMounted.store(any, oc::memory_order_release);
    }

    int64 ticksToSeconds(int64 ticks)
    {
        return (int64)std::chrono::duration_cast<std::chrono::seconds>(std::filesystem::file_time_type::duration(ticks)).count();
    }

    bool diskStat(oc::string_view path, FileSystem::FileStat& out)
    {
        std::error_code ec;
        const std::filesystem::path fsPath = toPath(path);
        const std::filesystem::file_status status = std::filesystem::status(fsPath, ec);
        if (ec || !std::filesystem::exists(status))
            return false;
        out.isDirectory = std::filesystem::is_directory(status);
        out.size = 0;
        if (!out.isDirectory)
        {
            const auto size = std::filesystem::file_size(fsPath, ec);
            out.size = ec ? 0ull : (uint64)size;
        }
        const auto time = std::filesystem::last_write_time(fsPath, ec);
        out.mtimeTicks = ec ? 0 : (int64)time.time_since_epoch().count();
        return true;
    }

    // stat through the mounts, no assert / marker.
    bool statImpl(const oc::string& path, FileSystem::FileStat& out)
    {
        out = {};
        if (packsMounted())
        {
            const VfsPath p(path);
            std::shared_lock lock(g_mountMutex);
            Hit hit;
            const EWhere where = locate(p, path, hit);
            if (where == EWhere::Pack)
            {
                const Pack::Entry& e = hit.mount->pack->entry(hit.key);
                out.size = e.size;
                out.mtimeTicks = e.mtimeTicks;
                return true;
            }
            if (where == EWhere::Disk && diskStat(path, out))
                return true;
            if (packHasDirectory(p))
            {
                out.isDirectory = true;
                return true;
            }
            return false;
        }
        return diskStat(path, out);
    }

    // The packed copy of path, or null. Caller holds g_mountMutex.
    const PackFile* packedFile(const VfsPath& p, const oc::string& path, uint32& outKey)
    {
        if (!packsMounted())
            return nullptr;
        Hit hit;
        if (locate(p, path, hit) != EWhere::Pack)
            return nullptr;
        outKey = hit.key;
        return hit.mount->pack.get();
    }

    bool readPacked(const PackFile& pack, uint32 key, oc::vector<uint8>& out)
    {
        out.resize((size_t)pack.entry(key).size);
        return out.empty() || pack.read(key, out.data());
    }

    bool writeBytes(const oc::string& path, const void* data, size_t size)
    {
        std::ofstream file(toPath(path), std::ios::out | std::ios::binary | std::ios::trunc);
        if (!file.is_open())
            return false;
        if (size > 0)
            file.write(reinterpret_cast<const char*>(data), (std::streamsize)size);
        return file.good();
    }
}

FileSystem::AllowMainThreadIO::AllowMainThreadIO() { ++t_allowMainThreadDepth; }
FileSystem::AllowMainThreadIO::~AllowMainThreadIO() { --t_allowMainThreadDepth; }

bool FileSystem::isMainThread()
{
    return g_mainThreadKnown && std::this_thread::get_id() == g_mainThreadId;
}

void FileSystem::assertIoThread(bool allowMainThread)
{
    // Blocking the main thread on the disk is a frame hitch - say so explicitly (AllowMainThreadIO
    // or the allowMainThread flag) wherever it is intended.
    assert((allowMainThread || t_allowMainThreadDepth > 0 || !isMainThread())
        && "FileSystem: IO on the main thread - wrap it in FileSystem::AllowMainThreadIO, pass "
           "allowMainThread=true, or move the call to a job");
    (void)allowMainThread;
}

bool FileSystem::initialize()
{
    ProfileScope scope("FileSystem::initialize", EProfileCategory::File);
    g_mainThreadId = std::this_thread::get_id();
    g_mainThreadKnown = true;
    const AllowMainThreadIO allowIo; // startup: finding the repo root is main-thread by definition
    const IoScope profileScope("FileSystem::initialize");

    std::cout.setf(std::ios::unitbuf);

    constexpr const char* ASSETS_DIR = "/Assets/";
    constexpr const char* DLL_DIR = "/Dependencies/Dll/";

    // From the working directory, then from the executable: a repo root is a folder holding Assets/.
    auto findRoot = [&](std::filesystem::path dir, std::filesystem::path& outRoot)
    {
        std::error_code ec;
        for (;;)
        {
            if (std::filesystem::exists(std::filesystem::path(dir.string() + ASSETS_DIR), ec))
            {
                outRoot = dir;
                return true;
            }
            if (!dir.has_parent_path() || dir.parent_path() == dir)
                return false;
            dir = dir.parent_path();
        }
    };
    const std::filesystem::path exeDir = toPath(executableDir());
    std::filesystem::path rootDir;
    const bool haveAssets = findRoot(std::filesystem::current_path(), rootDir) || findRoot(exeDir, rootDir);
    if (haveAssets)
    {
        std::filesystem::current_path(std::filesystem::path(rootDir.string() + ASSETS_DIR));
        std::filesystem::path dllDir = std::filesystem::path(rootDir.string() + DLL_DIR);
        if (std::filesystem::exists(dllDir))
        {
            AddDllDirectory(dllDir.wstring().c_str());
        }
    }
    else
    {
        // no Assets/ anywhere: a shipped build, its packs next to the executable stand in for Assets/
        std::filesystem::current_path(exeDir);
    }

    {
        std::unique_lock lock(g_mountMutex);
        g_rootDir = fromPath(std::filesystem::current_path());
        if (g_rootDir.empty() || g_rootDir.back() != '/')
            g_rootDir += '/';
        g_cwdRel.clear();
        g_cwdInRoot = true;
        ensureDiskMount();
    }

    if (!haveAssets && mountPacksIn(executableDir()) == 0)
    {
        assert(false && "Could not find project root directory, and no .dat packs next to the executable");
        return false;
    }
    return true;
}

oc::string FileSystem::executableDir()
{
    wchar_t buffer[4096];
    const DWORD length = GetModuleFileNameW(nullptr, buffer, (DWORD)oc::size(buffer));
    if (length == 0 || length >= oc::size(buffer))
        return oc::string();
    return fromPath(std::filesystem::path(std::wstring_view(buffer, length)).parent_path());
}

// ---------------------------------------------------------------- reads / writes

oc::string FileSystem::readFileStr(const oc::string& path, bool allowMainThread)
{
    assertIoThread(allowMainThread);
    const IoScope profileScope("FileSystem::readFileStr");
    if (packsMounted())
    {
        const VfsPath p(path);
        std::shared_lock lock(g_mountMutex);
        Hit hit;
        const EWhere where = locate(p, path, hit);
        if (where == EWhere::Missing)
            return oc::string();
        if (where == EWhere::Pack)
        {
            const PackFile& pack = *hit.mount->pack;
            oc::string content((size_t)pack.entry(hit.key).size, '\0');
            if (!content.empty() && !pack.read(hit.key, content.data()))
                return oc::string();
            return content;
        }
    }
    std::ifstream file(toPath(path), std::ios::in | std::ios::binary | std::ios::ate);
    if (!file.is_open())
        return oc::string();

    const std::ifstream::pos_type fileSize = file.tellg();
    file.seekg(0, std::ios::beg);
    oc::string fileContent(fileSize, '\0');
    file.read(fileContent.data(), fileSize);
    return fileContent;
}

bool FileSystem::readFileBytes(const oc::string& path, oc::vector<uint8>& out, bool allowMainThread)
{
    assertIoThread(allowMainThread);
    const IoScope profileScope("FileSystem::readFileBytes");
    out.clear();
    if (packsMounted())
    {
        const VfsPath p(path);
        std::shared_lock lock(g_mountMutex);
        Hit hit;
        const EWhere where = locate(p, path, hit);
        if (where == EWhere::Missing)
            return false;
        if (where == EWhere::Pack)
            return readPacked(*hit.mount->pack, hit.key, out);
    }
    std::ifstream file(toPath(path), std::ios::in | std::ios::binary | std::ios::ate);
    if (!file.is_open())
        return false;
    const std::ifstream::pos_type size = file.tellg();
    file.seekg(0, std::ios::beg);
    out.resize((size_t)size);
    if (!out.empty())
        file.read(reinterpret_cast<char*>(out.data()), (std::streamsize)out.size());
    return file.good() || file.eof();
}

bool FileSystem::writeFileStr(const oc::string& path, const oc::string& content, bool allowMainThread)
{
    assertIoThread(allowMainThread);
    const IoScope profileScope("FileSystem::writeFileStr");
    const bool ok = writeBytes(path, content.data(), content.size());
    if (ok)
        recordOverride(path);
    return ok;
}

bool FileSystem::writeFileBytes(const oc::string& path, oc::span<const uint8> data, bool allowMainThread)
{
    assertIoThread(allowMainThread);
    const IoScope profileScope("FileSystem::writeFileBytes");
    const bool ok = writeBytes(path, data.data(), data.size());
    if (ok)
        recordOverride(path);
    return ok;
}

// ---------------------------------------------------------------- queries + mutations

bool FileSystem::exists(const oc::string& path, bool allowMainThread)
{
    assertIoThread(allowMainThread);
    const IoScope profileScope("FileSystem::exists");
    if (packsMounted())
    {
        const VfsPath p(path);
        std::shared_lock lock(g_mountMutex);
        Hit hit;
        const EWhere where = locate(p, path, hit);
        if (where == EWhere::Pack)
            return true;
        if (where == EWhere::Disk && diskExists(path))
            return true;
        return packHasDirectory(p);
    }
    return diskExists(path);
}

bool FileSystem::isDirectory(const oc::string& path, bool allowMainThread)
{
    assertIoThread(allowMainThread);
    const IoScope profileScope("FileSystem::isDirectory");
    if (packsMounted())
    {
        const VfsPath p(path);
        std::shared_lock lock(g_mountMutex);
        if (packHasDirectory(p))
            return true;
        if (!diskAllowed(p))
            return false;
    }
    std::error_code ec;
    return std::filesystem::is_directory(toPath(path), ec);
}

bool FileSystem::isRegularFile(const oc::string& path, bool allowMainThread)
{
    assertIoThread(allowMainThread);
    const IoScope profileScope("FileSystem::isRegularFile");
    if (packsMounted())
    {
        const VfsPath p(path);
        std::shared_lock lock(g_mountMutex);
        Hit hit;
        const EWhere where = locate(p, path, hit);
        if (where != EWhere::Disk)
            return where == EWhere::Pack;
    }
    std::error_code ec;
    return std::filesystem::is_regular_file(toPath(path), ec);
}

uint64 FileSystem::fileSize(const oc::string& path, bool allowMainThread)
{
    assertIoThread(allowMainThread);
    const IoScope profileScope("FileSystem::fileSize");
    FileStat stat;
    return statImpl(path, stat) ? stat.size : 0ull;
}

int64 FileSystem::lastWriteTimeSec(const oc::string& path, bool allowMainThread)
{
    assertIoThread(allowMainThread);
    const IoScope profileScope("FileSystem::lastWriteTimeSec");
    FileStat stat;
    return statImpl(path, stat) ? ticksToSeconds(stat.mtimeTicks) : 0;
}

int64 FileSystem::lastWriteTimeTicks(const oc::string& path, bool allowMainThread)
{
    assertIoThread(allowMainThread);
    const IoScope profileScope("FileSystem::lastWriteTimeTicks");
    FileStat stat;
    return statImpl(path, stat) ? stat.mtimeTicks : 0;
}

bool FileSystem::stat(const oc::string& path, FileStat& out, bool allowMainThread)
{
    assertIoThread(allowMainThread);
    const IoScope profileScope("FileSystem::stat");
    return statImpl(path, out);
}

bool FileSystem::createDirectories(const oc::string& path, bool allowMainThread)
{
    assertIoThread(allowMainThread);
    const IoScope profileScope("FileSystem::createDirectories");
    std::error_code ec;
    std::filesystem::create_directories(toPath(path), ec);
    return !ec;
}

bool FileSystem::remove(const oc::string& path, bool allowMainThread)
{
    assertIoThread(allowMainThread);
    const IoScope profileScope("FileSystem::remove");
    bool packed = false;
    if (packsMounted())
    {
        const VfsPath p(path);
        std::shared_lock lock(g_mountMutex);
        uint32 key = 0;
        packed = packedFile(p, path, key) != nullptr;
    }
    std::error_code ec;
    const bool removed = std::filesystem::remove(toPath(path), ec);
    recordOverride(path); // hides a packed copy too
    return removed || packed;
}

uint64 FileSystem::removeAll(const oc::string& path, bool allowMainThread)
{
    assertIoThread(allowMainThread);
    const IoScope profileScope("FileSystem::removeAll");
    std::error_code ec;
    const auto count = std::filesystem::remove_all(toPath(path), ec);
    recordOverride(path);
    recordOverriddenDir(path);
    return ec ? 0ull : (uint64)count;
}

bool FileSystem::rename(const oc::string& from, const oc::string& to, bool allowMainThread)
{
    assertIoThread(allowMainThread);
    const IoScope profileScope("FileSystem::rename");
    std::error_code ec;
    std::filesystem::rename(toPath(from), toPath(to), ec);
    if (ec && packsMounted())
    {
        // a packed file: copy it out, then hide the packed source
        oc::vector<uint8> content;
        {
            const VfsPath p(from);
            std::shared_lock lock(g_mountMutex);
            uint32 key = 0;
            const PackFile* pack = packedFile(p, from, key);
            if (!pack || !readPacked(*pack, key, content))
                return false;
        }
        if (!writeBytes(to, content.data(), content.size()))
            return false;
        ec.clear();
    }
    if (!ec)
    {
        recordOverride(from);
        recordOverride(to);
    }
    return !ec;
}

bool FileSystem::copyLastWriteTime(const oc::string& from, const oc::string& to, bool allowMainThread)
{
    assertIoThread(allowMainThread);
    const IoScope profileScope("FileSystem::copyLastWriteTime");
    FileStat stat;
    if (!statImpl(from, stat))
        return false;
    std::error_code ec;
    std::filesystem::last_write_time(toPath(to),
        std::filesystem::file_time_type(std::filesystem::file_time_type::duration(stat.mtimeTicks)), ec);
    if (!ec)
        recordOverride(to);
    return !ec;
}

bool FileSystem::copyFile(const oc::string& from, const oc::string& to, bool overwrite, bool allowMainThread)
{
    assertIoThread(allowMainThread);
    const IoScope profileScope("FileSystem::copyFile");
    if (packsMounted())
    {
        oc::vector<uint8> content;
        bool packed = false;
        {
            const VfsPath p(from);
            std::shared_lock lock(g_mountMutex);
            uint32 key = 0;
            if (const PackFile* pack = packedFile(p, from, key))
            {
                packed = true;
                if (!readPacked(*pack, key, content))
                    return false;
            }
        }
        if (packed)
        {
            FileStat existing;
            if (!overwrite && statImpl(to, existing))
                return false;
            if (!writeBytes(to, content.data(), content.size()))
                return false;
            recordOverride(to);
            return true;
        }
    }
    std::error_code ec;
    const bool copied = std::filesystem::copy_file(toPath(from), toPath(to), overwrite
        ? std::filesystem::copy_options::overwrite_existing : std::filesystem::copy_options::none, ec);
    if (copied)
        recordOverride(to);
    return copied;
}

namespace
{
    template <typename Iterator>
    bool listImpl(Iterator begin, oc::vector<FileSystem::DirEntry>& out)
    {
        std::error_code ec;
        for (auto it = begin; it != Iterator(); it.increment(ec))
        {
            if (ec)
                return false;
            const std::filesystem::directory_entry& e = *it;
            FileSystem::DirEntry entry;
            entry.path = fromPath(e.path());
            entry.name = fromPath(e.path().filename());
            entry.extension = fromPath(e.path().extension());
            std::error_code sec;
            entry.isDirectory = e.is_directory(sec);
            if (!entry.isDirectory)
            {
                const auto size = e.file_size(sec);
                entry.size = sec ? 0ull : (uint64)size;
            }
            out.push_back(oc::move(entry));
        }
        return true;
    }

    bool listDisk(const oc::string& dir, bool recursive, oc::vector<FileSystem::DirEntry>& out)
    {
        std::error_code ec;
        if (recursive)
        {
            std::filesystem::recursive_directory_iterator it(toPath(dir), ec);
            return !ec && listImpl(it, out);
        }
        std::filesystem::directory_iterator it(toPath(dir), ec);
        return !ec && listImpl(it, out);
    }

    // A packed entry in the format the disk listing produces: the listed folder as given + '/' + the relative path.
    FileSystem::DirEntry packedDirEntry(oc::string_view dir, oc::string_view relToDir, bool isDirectory, uint64 size)
    {
        FileSystem::DirEntry entry;
        entry.path.reserve(dir.size() + 1 + relToDir.size());
        for (char c : dir)
            entry.path.push_back(c == '\\' ? '/' : c);
        if (!entry.path.empty() && entry.path.back() != '/')
            entry.path.push_back('/');
        entry.path.append(relToDir.data(), relToDir.size());

        const size_t slash = relToDir.rfind('/');
        const oc::string_view name = slash == oc::string_view::npos ? relToDir : relToDir.substr(slash + 1);
        entry.name = oc::string(name.data(), name.size());
        const size_t dot = name.rfind('.');
        if (dot != oc::string_view::npos && dot != 0) // like std::filesystem: ".gitignore" has no extension
            entry.extension = oc::string(name.data() + dot, name.size() - dot);
        entry.isDirectory = isDirectory;
        entry.size = isDirectory ? 0 : size;
        return entry;
    }

    bool isOverriddenPacked(oc::string_view mountPoint, oc::string_view packPath)
    {
        char rel[Pack::MAX_PATH_CHARS * 2];
        if (mountPoint.size() + packPath.size() > sizeof(rel))
            return false;
        memcpy(rel, mountPoint.data(), mountPoint.size());
        memcpy(rel + mountPoint.size(), packPath.data(), packPath.size());
        return isOverridden(oc::string_view(rel, mountPoint.size() + packPath.size()));
    }

    // Every mount's entries in dir, each path once: the highest priority mount that lists it wins.
    bool listMerged(const oc::string& dir, bool recursive, oc::vector<FileSystem::DirEntry>& out)
    {
        const VfsPath p(dir);
        if (!p.inRoot)
            return listDisk(dir, recursive, out);

        std::shared_lock lock(g_mountMutex);
        char prefixBuffer[Pack::MAX_PATH_CHARS + 1];
        memcpy(prefixBuffer, p.rel, p.length);
        uint32 prefixLength = p.length;
        if (prefixLength > 0)
            prefixBuffer[prefixLength++] = '/';
        const oc::string_view dirRel(prefixBuffer, prefixLength); // "Entities/", or "" for Assets/ itself

        oc::unordered_set<oc::string> seen;
        auto add = [&](FileSystem::DirEntry&& entry)
        {
            oc::string key = entry.path;
            for (char& c : key)
                c = Pack::lowerAscii(c);
            if (seen.insert(oc::move(key)).second)
                out.push_back(oc::move(entry));
        };

        const bool overrides = g_hasOverrides.load(oc::memory_order_acquire);
        bool any = false;
        oc::vector<FileSystem::DirEntry> disk;
        for (const Mount& m : g_mounts)
        {
            if (!m.pack)
            {
                if (g_diskEnabled && listDisk(dir, recursive, disk))
                {
                    any = true;
                    for (FileSystem::DirEntry& entry : disk)
                        add(oc::move(entry));
                }
                continue;
            }

            const PackFile& pack = *m.pack;
            const oc::string_view mountPoint = m.mountPoint;
            if (Pack::startsWithNoCase(dirRel, mountPoint))
            {
                // the folder lies inside the pack
                const oc::string_view packPrefix = dirRel.substr(mountPoint.size());
                const oc::string_view packDir = packPrefix.empty() ? packPrefix : packPrefix.substr(0, packPrefix.size() - 1);
                if (!pack.isDirectory(packDir))
                    continue;
                any = true;
                auto report = [&](const oc::BitmapPathTrieEntry& e, bool isDirectory)
                {
                    if (overrides && isOverriddenPacked(mountPoint, e.path))
                        return;
                    add(packedDirEntry(dir, e.path.substr(packPrefix.size()), isDirectory, isDirectory ? 0 : pack.entry(e.key).size));
                };
                pack.files().forEachChild(packPrefix, recursive, [&](const oc::BitmapPathTrieEntry& e) { report(e, e.isDirectory); });
                if (recursive)
                    pack.directories().forEachChild(packPrefix, true, [&](const oc::BitmapPathTrieEntry& e) { report(e, true); });
            }
            else if (Pack::startsWithNoCase(mountPoint, dirRel))
            {
                // the folder lies above the mount point: the mount point's folders appear in it
                any = true;
                const oc::string_view below = mountPoint.substr(dirRel.size()); // "Shaders/", "A/B/"
                for (size_t slash = below.find('/'); slash != oc::string_view::npos; slash = below.find('/', slash + 1))
                {
                    add(packedDirEntry(dir, below.substr(0, slash), true, 0));
                    if (!recursive)
                        break;
                }
                if (!recursive)
                    continue;
                oc::string rel;
                auto report = [&](const oc::BitmapPathTrieEntry& e, bool isDirectory)
                {
                    if (overrides && isOverriddenPacked(mountPoint, e.path))
                        return;
                    rel.assign(below.data(), below.size());
                    rel.append(e.path.data(), e.path.size());
                    add(packedDirEntry(dir, rel, isDirectory, isDirectory ? 0 : pack.entry(e.key).size));
                };
                pack.files().forEach([&](const oc::BitmapPathTrieEntry& e) { report(e, false); });
                pack.directories().forEach([&](const oc::BitmapPathTrieEntry& e) { report(e, true); });
            }
        }
        return any;
    }
}

bool FileSystem::listDirectory(const oc::string& dir, oc::vector<DirEntry>& out, bool allowMainThread)
{
    assertIoThread(allowMainThread);
    const IoScope profileScope("FileSystem::listDirectory");
    out.clear();
    return packsMounted() ? listMerged(dir, false, out) : listDisk(dir, false, out);
}

bool FileSystem::listDirectoryRecursive(const oc::string& dir, oc::vector<DirEntry>& out, bool allowMainThread)
{
    assertIoThread(allowMainThread);
    const IoScope profileScope("FileSystem::listDirectoryRecursive");
    out.clear();
    return packsMounted() ? listMerged(dir, true, out) : listDisk(dir, true, out);
}

oc::string FileSystem::currentPath(bool allowMainThread)
{
    assertIoThread(allowMainThread);
    const IoScope profileScope("FileSystem::currentPath");
    std::error_code ec;
    return fromPath(std::filesystem::current_path(ec));
}

bool FileSystem::setCurrentPath(const oc::string& path, bool allowMainThread)
{
    assertIoThread(allowMainThread);
    const IoScope profileScope("FileSystem::setCurrentPath");
    std::error_code ec;
    std::filesystem::current_path(toPath(path), ec);
    if (ec)
        return false;
    // relative paths now resolve against the new folder: keep the pack lookups in step
    const oc::string cwd = fromPath(std::filesystem::current_path(ec));
    std::unique_lock lock(g_mountMutex);
    g_cwdRel.clear();
    g_cwdInRoot = false;
    char rel[Pack::MAX_PATH_CHARS];
    uint32 length = 0;
    if (toRootRelative(cwd, rel, length))
    {
        g_cwdInRoot = true;
        g_cwdRel.assign(rel, length);
        if (length > 0)
            g_cwdRel += '/';
    }
    return true;
}

oc::string FileSystem::absolutePath(const oc::string& path, bool allowMainThread)
{
    assertIoThread(allowMainThread);
    const IoScope profileScope("FileSystem::absolutePath");
    std::error_code ec;
    const std::filesystem::path result = std::filesystem::absolute(toPath(path), ec);
    return ec ? path : fromPath(result);
}

oc::string FileSystem::canonicalPath(const oc::string& path, bool allowMainThread)
{
    assertIoThread(allowMainThread);
    const IoScope profileScope("FileSystem::canonicalPath");
    std::error_code ec;
    const std::filesystem::path result = std::filesystem::canonical(toPath(path), ec);
    if (!ec)
        return fromPath(result);
    if (packsMounted())
    {
        // only in a pack: the absolute path in the pack's stored case
        const VfsPath p(path);
        std::shared_lock lock(g_mountMutex);
        Hit hit;
        if (locate(p, path, hit) == EWhere::Pack)
        {
            char canonical[Pack::MAX_PATH_CHARS];
            hit.mount->pack->canonicalCase(hit.packPath, oc::span<char>(canonical, hit.packPath.size()));
            return g_rootDir + hit.mount->mountPoint + oc::string(canonical, hit.packPath.size());
        }
        if (packHasDirectory(p))
        {
            oc::string dir = g_rootDir + oc::string(p.rel, p.length);
            if (!dir.empty() && dir.back() == '/')
                dir.pop_back();
            return dir;
        }
    }
    return oc::string();
}

oc::string FileSystem::weaklyCanonicalPath(const oc::string& path, bool allowMainThread)
{
    assertIoThread(allowMainThread);
    const IoScope profileScope("FileSystem::weaklyCanonicalPath");
    std::error_code ec;
    const std::filesystem::path result = std::filesystem::weakly_canonical(toPath(path), ec);
    return ec ? oc::string() : fromPath(result);
}

oc::string FileSystem::relativePath(const oc::string& path, const oc::string& base, bool allowMainThread)
{
    assertIoThread(allowMainThread);
    const IoScope profileScope("FileSystem::relativePath");
    std::error_code ec;
    const std::filesystem::path result = base.empty()
        ? std::filesystem::relative(toPath(path), ec)
        : std::filesystem::relative(toPath(path), toPath(base), ec);
    return ec ? oc::string() : fromPath(result);
}

// ---------------------------------------------------------------- pure path math (no IO)

oc::string FileSystem::join(oc::string_view a, oc::string_view b)
{
    if (a.empty())
        return oc::string(b);
    if (b.empty())
        return oc::string(a);
    return fromPath(toPath(a) / toPath(b));
}

oc::string FileSystem::parentPath(oc::string_view path)  { return fromPath(toPath(path).parent_path()); }
oc::string FileSystem::filename(oc::string_view path)    { return fromPath(toPath(path).filename()); }
oc::string FileSystem::stem(oc::string_view path)        { return fromPath(toPath(path).stem()); }
oc::string FileSystem::extension(oc::string_view path)   { return fromPath(toPath(path).extension()); }

oc::string FileSystem::replaceExtension(oc::string_view path, oc::string_view ext)
{
    std::filesystem::path p = toPath(path);
    p.replace_extension(toPath(ext));
    return fromPath(p);
}

oc::string FileSystem::normalize(oc::string_view path)
{
    return fromPath(toPath(path).lexically_normal());
}

oc::string FileSystem::lexicallyRelative(oc::string_view path, oc::string_view base)
{
    return fromPath(toPath(path).lexically_relative(toPath(base)));
}

bool FileSystem::isAbsolute(oc::string_view path)
{
    return toPath(path).is_absolute();
}

bool FileSystem::pathEquals(oc::string_view a, oc::string_view b)
{
    const oc::string na = normalize(a);
    const oc::string nb = normalize(b);
    if (na.size() != nb.size())
        return false;
    for (size_t i = 0; i < na.size(); ++i) // Windows: case- and separator-insensitive
    {
        const char ca = na[i] >= 'A' && na[i] <= 'Z' ? (char)(na[i] + 32) : na[i];
        const char cb = nb[i] >= 'A' && nb[i] <= 'Z' ? (char)(nb[i] + 32) : nb[i];
        if (ca != cb)
            return false;
    }
    return true;
}

// ---------------------------------------------------------------- packs

bool FileSystem::mountPack(const oc::string& packPath, oc::optional<int32> priorityArg, oc::optional<oc::string> mountPointArg,
    bool allowMainThread)
{
    assertIoThread(allowMainThread);
    const IoScope profileScope("FileSystem::mountPack");
    oc::shared_ptr<PackFile> pack = oc::make_shared<PackFile>();
    oc::string error;
    if (!pack->open(packPath, error))
    {
        Log::error("FileSystem: cannot mount " + error);
        return false;
    }
    const int32 priority = priorityArg ? *priorityArg : pack->header().mountPriority;
    const oc::string mountPoint = mountPointArg ? *mountPointArg : oc::string(pack->header().mountPoint);

    char point[Pack::MAX_PATH_CHARS];
    uint32 pointLength = 0;
    if (!Pack::normalizeRelative(mountPoint, point, pointLength))
    {
        Log::error("FileSystem: bad mount point '" + mountPoint + "' for " + packPath);
        return false;
    }
    Mount mount;
    mount.mountPoint.assign(point, pointLength);
    if (pointLength > 0)
        mount.mountPoint += '/';
    mount.priority = priority;
    const uint32 fileCount = pack->fileCount();
    const uint64 deadBytes = pack->deadBytes();
    mount.pack = oc::move(pack);

    unmountPack(packPath); // mounting again replaces
    {
        std::unique_lock lock(g_mountMutex);
        ensureDiskMount();
        insertMount(oc::move(mount));
        refreshPacksMounted();
    }
    Log::info(oc::format("FileSystem: mounted {} at Assets/{} ({} files, priority {}, {:.1f} MB dead)",
        packPath, oc::string_view(point, pointLength), fileCount, priority, (double)deadBytes / (1024.0 * 1024.0)));
    return true;
}

uint32 FileSystem::mountPacksIn(const oc::string& dir, bool allowMainThread)
{
    assertIoThread(allowMainThread);
    oc::vector<oc::string> packs;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(toPath(dir), ec))
    {
        std::error_code fileEc;
        if (entry.is_regular_file(fileEc) && pathEquals(fromPath(entry.path().extension()), ".dat"))
            packs.push_back(fromPath(entry.path()));
    }
    oc::sort(packs.begin(), packs.end()); // equal priorities: the later name wins, the same on every machine
    uint32 mounted = 0;
    for (const oc::string& pack : packs)
        mounted += mountPack(pack, {}, {}, allowMainThread) ? 1 : 0;
    return mounted;
}

bool FileSystem::unmountPack(const oc::string& packPath)
{
    std::unique_lock lock(g_mountMutex);
    for (auto it = g_mounts.begin(); it != g_mounts.end(); ++it)
    {
        if (it->pack && pathEquals(it->pack->path(), packPath))
        {
            g_mounts.erase(it);
            refreshPacksMounted();
            return true;
        }
    }
    return false;
}

void FileSystem::unmountAll()
{
    std::unique_lock lock(g_mountMutex);
    for (size_t i = g_mounts.size(); i-- > 0;)
        if (g_mounts[i].pack)
            g_mounts.erase(g_mounts.begin() + i);
    refreshPacksMounted();
}

void FileSystem::setDiskPriority(int32 priority)
{
    std::unique_lock lock(g_mountMutex);
    ensureDiskMount();
    for (auto it = g_mounts.begin(); it != g_mounts.end(); ++it)
    {
        if (!it->pack)
        {
            Mount disk = oc::move(*it);
            g_mounts.erase(it);
            disk.priority = priority;
            insertMount(oc::move(disk));
            return;
        }
    }
}

void FileSystem::setDiskEnabled(bool enabled)
{
    std::unique_lock lock(g_mountMutex);
    g_diskEnabled = enabled;
}

bool FileSystem::isDiskEnabled()
{
    std::shared_lock lock(g_mountMutex);
    return g_diskEnabled;
}

oc::vector<FileSystem::MountInfo> FileSystem::mounts()
{
    std::shared_lock lock(g_mountMutex);
    oc::vector<MountInfo> out;
    out.reserve(g_mounts.size());
    for (const Mount& m : g_mounts)
    {
        MountInfo info;
        info.mountPoint = m.mountPoint;
        info.priority = m.priority;
        info.isDisk = m.pack == nullptr;
        info.enabled = m.pack != nullptr || g_diskEnabled;
        if (m.pack)
        {
            info.packPath = m.pack->path();
            info.fileCount = m.pack->fileCount();
            info.packBytes = m.pack->fileSize();
            info.deadBytes = m.pack->deadBytes();
        }
        out.push_back(oc::move(info));
    }
    return out;
}

FileSystem::ESource FileSystem::sourceOf(const oc::string& path, oc::string* outPackPath, bool allowMainThread)
{
    assertIoThread(allowMainThread);
    if (packsMounted())
    {
        const VfsPath p(path);
        std::shared_lock lock(g_mountMutex);
        Hit hit;
        const EWhere where = locate(p, path, hit);
        if (where == EWhere::Pack)
        {
            if (outPackPath)
                *outPackPath = hit.mount->pack->path();
            return ESource::Pack;
        }
        if (where == EWhere::Missing)
            return ESource::Missing;
    }
    return diskExists(path) ? ESource::Disk : ESource::Missing;
}

// ---------------------------------------------------------------- FileReader

FileReader::FileReader(FileReader&& other) noexcept
    : m_pack(oc::move(other.m_pack)), m_key(other.m_key), m_data(other.m_data), m_decompressed(oc::move(other.m_decompressed)),
      m_native(oc::move(other.m_native)), m_size(other.m_size), m_open(other.m_open)
{
    if (!m_decompressed.empty())
        m_data = m_decompressed.data();
    other.m_data = nullptr;
    other.m_size = 0;
    other.m_open = false;
}

FileReader& FileReader::operator=(FileReader&& other) noexcept
{
    if (this != &other)
    {
        close();
        m_pack = oc::move(other.m_pack);
        m_key = other.m_key;
        m_data = other.m_data;
        m_decompressed = oc::move(other.m_decompressed);
        if (!m_decompressed.empty())
            m_data = m_decompressed.data();
        m_native = oc::move(other.m_native);
        m_size = other.m_size;
        m_open = other.m_open;
        other.m_data = nullptr;
        other.m_size = 0;
        other.m_open = false;
    }
    return *this;
}

bool FileReader::open(const oc::string& path, bool allowMainThread)
{
    FileSystem::assertIoThread(allowMainThread);
    const IoScope profileScope("FileReader::open");
    close();
    if (packsMounted())
    {
        const VfsPath p(path);
        std::shared_lock lock(g_mountMutex);
        Hit hit;
        const EWhere where = locate(p, path, hit);
        if (where == EWhere::Missing)
            return false;
        if (where == EWhere::Pack)
        {
            m_pack = hit.mount->pack;
            m_key = hit.key;
            m_size = m_pack->entry(m_key).size;
            if (m_pack->isRaw(m_key))
                m_data = m_pack->storedBytes(m_key).data();
            else
            {
                m_decompressed.resize((size_t)m_size);
                if (!m_pack->read(m_key, m_decompressed.data()))
                {
                    close();
                    return false;
                }
                m_data = m_decompressed.data();
            }
            m_open = true;
            return true;
        }
    }
    if (!m_native.open(path, NativeFile::EMode::Read))
        return false;
    m_size = m_native.size();
    m_open = true;
    return true;
}

void FileReader::close()
{
    m_pack.reset();
    m_data = nullptr;
    m_decompressed = {};
    m_native.close();
    m_size = 0;
    m_open = false;
}

bool FileReader::read(uint64 offset, void* dst, uint64 bytes) const
{
    if (!m_open || offset > m_size || bytes > m_size - offset)
        return false;
    if (bytes == 0)
        return true;
    const IoScope profileScope("FileReader::read");
    if (m_pack)
    {
        if (m_pack->isRaw(m_key))
            return m_pack->readRange(m_key, offset, dst, bytes);
        memcpy(dst, m_data + offset, (size_t)bytes);
        return true;
    }
    return m_native.readAt(offset, dst, bytes);
}

bool FileReader::readAll(oc::vector<uint8>& out) const
{
    out.resize((size_t)m_size);
    return read(0, out.data(), m_size);
}

oc::span<const uint8> FileReader::view() const
{
    return m_pack ? oc::span<const uint8>(m_data, (size_t)m_size) : oc::span<const uint8>();
}
