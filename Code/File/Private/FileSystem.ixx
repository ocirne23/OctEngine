export module File:FileSystem;

import File.fwd;

import Core;

// THE filesystem seam. Every file/IO operation in the engine goes through here — <filesystem> and
// <fstream> are NOT exported from Core, so no other library can touch the disk behind its back.
// Paths are plain UTF-8 std::strings (no std::filesystem::path leaking into consumers); the pure
// PATH helpers at the bottom are string math, not IO, and carry no thread restriction.
//
// MAIN-THREAD POLICY: every IO call asserts when it runs on the main thread — disk latency there
// is a frame hitch. Work that legitimately blocks the main thread (startup, an explicit user
// action in the editor, save/load) must say so by declaring a scope:
//
//     const FileSystem::AllowMainThreadIO allowIo;   // RAII, nestable, thread-local
//     const std::string text = FileSystem::readFileStr(path);
//
// or per call with the trailing `allowMainThread` flag where a scope would be noise. Worker
// threads are never restricted. NOTE (/GT fiber-safe TLS): the scope is thread_local, so a fiber
// that PARKS inside one and resumes on another worker leaves the scope behind — keep the scope
// tight around the actual call, never around a job wait.
export class FileSystem final
{
public:

    // Walks up from the working directory to the repo root, makes Assets/ the working directory
    // and registers Dependencies/Dll/. Main thread, at startup — exempt by nature.
    static bool initialize();

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
    // seeks, cooked-scene ranges), which no read-it-all API can express — they call this at their
    // IO entry points so the policy still covers them.
    static void assertIoThread(bool allowMainThread = false);

    // ---- reads / writes -----------------------------------------------------------------------

    static std::string readFileStr(const std::string& path, bool allowMainThread = false);
    static bool readFileBytes(const std::string& path, std::vector<uint8>& out, bool allowMainThread = false);
    // Writes (creating/truncating). Returns false if the file could not be opened/written.
    static bool writeFileStr(const std::string& path, const std::string& content, bool allowMainThread = false);
    static bool writeFileBytes(const std::string& path, std::span<const uint8> data, bool allowMainThread = false);

    // ---- queries + mutations (all touch the disk) ---------------------------------------------

    static bool exists(const std::string& path, bool allowMainThread = false);
    static bool isDirectory(const std::string& path, bool allowMainThread = false);
    static bool isRegularFile(const std::string& path, bool allowMainThread = false);
    static uint64 fileSize(const std::string& path, bool allowMainThread = false); // 0 when missing
    // Seconds since the file clock's epoch — only ever compared/stored, never formatted.
    static int64 lastWriteTimeSec(const std::string& path, bool allowMainThread = false);
    static bool createDirectories(const std::string& path, bool allowMainThread = false);
    static bool remove(const std::string& path, bool allowMainThread = false);
    static uint64 removeAll(const std::string& path, bool allowMainThread = false);
    static bool rename(const std::string& from, const std::string& to, bool allowMainThread = false);
    // Stamps `to` with `from`'s modification time (ScriptHost marks a built DLL as up-to-date
    // with its source this way).
    static bool copyLastWriteTime(const std::string& from, const std::string& to, bool allowMainThread = false);
    static bool copyFile(const std::string& from, const std::string& to, bool overwrite, bool allowMainThread = false);

    struct DirEntry
    {
        std::string path;      // full path as listed
        std::string name;      // filename component
        std::string extension; // ".ext", empty for directories without one
        bool isDirectory = false;
        uint64 size = 0;       // files only
    };
    // Unsorted; false when the directory could not be read (out is cleared either way).
    static bool listDirectory(const std::string& dir, std::vector<DirEntry>& out, bool allowMainThread = false);
    static bool listDirectoryRecursive(const std::string& dir, std::vector<DirEntry>& out, bool allowMainThread = false);

    static std::string currentPath(bool allowMainThread = false);
    static bool setCurrentPath(const std::string& path, bool allowMainThread = false);
    // Resolve against the working directory / symlinks (canonical requires the path to exist).
    static std::string absolutePath(const std::string& path, bool allowMainThread = false);
    static std::string canonicalPath(const std::string& path, bool allowMainThread = false);
    static std::string weaklyCanonicalPath(const std::string& path, bool allowMainThread = false);
    static std::string relativePath(const std::string& path, const std::string& base = std::string(),
        bool allowMainThread = false);

    // ---- pure PATH math (no disk access, callable anywhere) -----------------------------------

    static std::string join(std::string_view a, std::string_view b);
    static std::string parentPath(std::string_view path);
    static std::string filename(std::string_view path);
    static std::string stem(std::string_view path);
    static std::string extension(std::string_view path);      // ".ext" (with the dot), lowercase-preserving
    static std::string replaceExtension(std::string_view path, std::string_view ext);
    static std::string normalize(std::string_view path);      // lexically normal, forward slashes
    static bool isAbsolute(std::string_view path);
    static bool pathEquals(std::string_view a, std::string_view b); // separator- and case-insensitive (Windows)

private:
    FileSystem() {};
    ~FileSystem() {};
};
