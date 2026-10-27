export module File:FileSystem;

import File.fwd;

import Core;

import :NativeFile;

class PackFile;

// THE filesystem seam. Every file/IO operation in the engine goes through here - <filesystem> and
// <fstream> are NOT exported from Core, so no other library can touch the disk behind its back.
// Paths are plain UTF-8 std::strings (no std::filesystem::path leaking into consumers); the pure
// PATH helpers at the bottom are string math, not IO, and carry no thread restriction.
//
// MAIN-THREAD POLICY: every IO call asserts when it runs on the main thread - disk latency there
// is a frame hitch. Work that legitimately blocks the main thread (startup, an explicit user
// action in the editor, save/load) must say so by declaring a scope:
//
//     const FileSystem::AllowMainThreadIO allowIo;   // RAII, nestable, thread-local
//     const oc::string text = FileSystem::readFileStr(path);
//
// or per call with the trailing `allowMainThread` flag where a scope would be noise. Worker
// threads are never restricted. NOTE (/GT fiber-safe TLS): the scope is thread_local, so a fiber
// that PARKS inside one and resumes on another worker leaves the scope behind - keep the scope
// tight around the actual call, never around a job wait.
//
// PACKS: .dat packs (File:PackBuilder) mount into the same paths. Every read and query below looks a
// path up in the mounts by priority - the disk is one of them (priority 0, can be disabled) - and
// the first that has the file serves it. Writes always go to the disk, and a path this process wrote,
// removed or renamed is served from the disk from then on, so it never reads back a stale packed copy.
// Only paths under Assets/ (relative, or absolute below it) are looked up in packs.
export class FileSystem final
{
public:

    // Walks up from the working directory to the repo root, makes Assets/ the working directory
    // and registers Dependencies/Dll/. Main thread, at startup - exempt by nature.
    // NO Assets/ found (a shipped build): the executable's folder becomes the root and working
    // directory, and every .dat next to the executable mounts (mountPacksIn).
    static bool initialize();

    // The folder of the running executable, '/' separated, no trailing '/'.
    static oc::string executableDir();

    // Declares the enclosing scope as allowed to block on IO from the main thread.
    class AllowMainThreadIO final
    {
    public:
        AllowMainThreadIO();
        ~AllowMainThreadIO();
        AllowMainThreadIO(const AllowMainThreadIO&) = delete;
        AllowMainThreadIO& operator=(const AllowMainThreadIO&) = delete;
    };

    // True on the thread that called initialize(). Public so callers can branch (e.g. queue the
    // work to a job instead of blocking) rather than just silencing the assert.
    static bool isMainThread();

    // The same main-thread assert the API below runs, exposed for File's OWN internals: the
    // asset/cache readers stream binary ranges through std::ifstream directly (mesh streaming
    // seeks, cooked-scene ranges), which no read-it-all API can express - they call this at their
    // IO entry points so the policy still covers them.
    static void assertIoThread(bool allowMainThread = false);

    // ---- reads / writes -----------------------------------------------------------------------

    static oc::string readFileStr(const oc::string& path, bool allowMainThread = false);
    static bool readFileBytes(const oc::string& path, oc::vector<uint8>& out, bool allowMainThread = false);
    // Writes (creating/truncating). Returns false if the file could not be opened/written.
    static bool writeFileStr(const oc::string& path, const oc::string& content, bool allowMainThread = false);
    static bool writeFileBytes(const oc::string& path, oc::span<const uint8> data, bool allowMainThread = false);

    // ---- queries + mutations (all touch the disk) ---------------------------------------------

    static bool exists(const oc::string& path, bool allowMainThread = false);
    static bool isDirectory(const oc::string& path, bool allowMainThread = false);
    static bool isRegularFile(const oc::string& path, bool allowMainThread = false);
    static uint64 fileSize(const oc::string& path, bool allowMainThread = false); // 0 when missing
    // Seconds since the file clock's epoch - only ever compared/stored, never formatted.
    static int64 lastWriteTimeSec(const oc::string& path, bool allowMainThread = false);
    // The same in file clock ticks (std::filesystem::file_time_type's duration), what the cooked caches stamp.
    static int64 lastWriteTimeTicks(const oc::string& path, bool allowMainThread = false);

    struct FileStat
    {
        uint64 size = 0;       // files only
        int64 mtimeTicks = 0;  // file clock ticks; 0 for packed directories
        bool isDirectory = false;
    };
    // Existence, size and mtime in ONE lookup. False when the path does not exist.
    static bool stat(const oc::string& path, FileStat& out, bool allowMainThread = false);
    static bool createDirectories(const oc::string& path, bool allowMainThread = false);
    static bool remove(const oc::string& path, bool allowMainThread = false);
    static uint64 removeAll(const oc::string& path, bool allowMainThread = false);
    static bool rename(const oc::string& from, const oc::string& to, bool allowMainThread = false);
    // Stamps `to` with `from`'s modification time (ScriptHost marks a built DLL as up-to-date
    // with its source this way).
    static bool copyLastWriteTime(const oc::string& from, const oc::string& to, bool allowMainThread = false);
    static bool copyFile(const oc::string& from, const oc::string& to, bool overwrite, bool allowMainThread = false);

    struct DirEntry
    {
        oc::string path;      // full path as listed
        oc::string name;      // filename component
        oc::string extension; // ".ext", empty for directories without one
        bool isDirectory = false;
        uint64 size = 0;       // files only
    };
    // Unsorted; false when the directory could not be read (out is cleared either way).
    static bool listDirectory(const oc::string& dir, oc::vector<DirEntry>& out, bool allowMainThread = false);
    static bool listDirectoryRecursive(const oc::string& dir, oc::vector<DirEntry>& out, bool allowMainThread = false);

    static oc::string currentPath(bool allowMainThread = false);
    static bool setCurrentPath(const oc::string& path, bool allowMainThread = false);
    // Resolve against the working directory / symlinks (canonical requires the path to exist).
    static oc::string absolutePath(const oc::string& path, bool allowMainThread = false);
    static oc::string canonicalPath(const oc::string& path, bool allowMainThread = false);
    static oc::string weaklyCanonicalPath(const oc::string& path, bool allowMainThread = false);
    static oc::string relativePath(const oc::string& path, const oc::string& base = oc::string(),
        bool allowMainThread = false);

    // ---- pure PATH math (no disk access, callable anywhere) -----------------------------------

    static oc::string join(oc::string_view a, oc::string_view b);
    static oc::string parentPath(oc::string_view path);
    static oc::string filename(oc::string_view path);
    static oc::string stem(oc::string_view path);
    static oc::string extension(oc::string_view path);      // ".ext" (with the dot), lowercase-preserving
    static oc::string replaceExtension(oc::string_view path, oc::string_view ext);
    static oc::string normalize(oc::string_view path);      // lexically normal, forward slashes
    // Relative path computed PURELY from the strings - no disk access, unlike relativePath() which
    // resolves both sides through weakly_canonical. Correct when both are already absolute+canonical
    // (which is what the asset browser holds), and the right choice for anything per-frame.
    static oc::string lexicallyRelative(oc::string_view path, oc::string_view base);
    static bool isAbsolute(oc::string_view path);
    static bool pathEquals(oc::string_view a, oc::string_view b); // separator- and case-insensitive (Windows)

    // ---- packs --------------------------------------------------------------------------------

    // Mounts a .dat pack. A higher priority wins; the disk has DISK_PRIORITY_DEFAULT, so the default puts the pack
    // above the loose files, and a negative one makes it the fallback below them. Equal priorities: the later
    // mount wins. mountPoint: the Assets-relative folder the pack's root appears at ("" = Assets/ itself).
    // Either one left out = the value the pack was built with (dat.cfg's Priority / MountPoint).
    // Mounting maps the file and reads only its header; any thread, but not while reads of the same pack run.
    static constexpr int32 DISK_PRIORITY_DEFAULT = 0;
    static constexpr int32 PACK_PRIORITY_DEFAULT = 100;
    static bool mountPack(const oc::string& packPath, oc::optional<int32> priority = {},
        oc::optional<oc::string> mountPoint = {}, bool allowMainThread = false);
    // Mounts every .dat in dir (not recursive) with its built-in priority and mount point. Returns the count.
    static uint32 mountPacksIn(const oc::string& dir, bool allowMainThread = false);
    static bool unmountPack(const oc::string& packPath);
    static void unmountAll();
    static void setDiskPriority(int32 priority);
    // false = the loose files under Assets/ are not read (pack only). Files this process wrote stay readable.
    static void setDiskEnabled(bool enabled);
    static bool isDiskEnabled();

    struct MountInfo
    {
        oc::string packPath;   // empty for the disk
        oc::string mountPoint;
        int32 priority = 0;
        uint32 fileCount = 0;
        uint64 packBytes = 0;
        uint64 deadBytes = 0;  // reclaimable by a full rewrite
        bool isDisk = false;
        bool enabled = true;
    };
    static oc::vector<MountInfo> mounts(); // highest priority first

    enum class ESource : uint8 { Missing, Disk, Pack };
    // Which mount serves path; outPackPath receives the pack's path for ESource::Pack.
    static ESource sourceOf(const oc::string& path, oc::string* outPackPath = nullptr, bool allowMainThread = false);

private:
    FileSystem() {};
    ~FileSystem() {};
};

// Random-access reads of one file, from a pack or the disk: what the streamers use for byte ranges (mip ranges,
// mesh attribute arrays). Open once, then read() any range from any thread - reads are positional, nothing is
// shared. A packed file keeps its pack mapped until the reader closes, even across an unmount.
export class FileReader final
{
public:

    FileReader() = default;
    ~FileReader() { close(); }
    FileReader(FileReader&& other) noexcept;
    FileReader& operator=(FileReader&& other) noexcept;
    FileReader(const FileReader&) = delete;
    FileReader& operator=(const FileReader&) = delete;

    bool open(const oc::string& path, bool allowMainThread = false);
    void close();

    bool isOpen() const { return m_open; }
    bool isPacked() const { return m_pack != nullptr; }
    uint64 size() const { return m_size; }

    bool read(uint64 offset, void* dst, uint64 bytes) const;
    bool readAll(oc::vector<uint8>& out) const;
    // The whole file in memory without a copy: a raw packed file points into the mapping, a compressed one into
    // its decompressed buffer. Empty for a file on the disk.
    oc::span<const uint8> view() const;

private:

    oc::shared_ptr<const PackFile> m_pack;
    uint32 m_key = 0;
    const uint8* m_data = nullptr;   // packed: the whole file
    oc::vector<uint8> m_decompressed;
    NativeFile m_native;              // disk
    uint64 m_size = 0;
    bool m_open = false;
};
