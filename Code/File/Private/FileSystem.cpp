module File;

import Core;
import Core.Windows;
// The ONLY place these two live now (Core deliberately stops exporting them — see Core.ixx):
// every other library reaches the disk through the FileSystem API below.
import <filesystem>;
import <fstream>;

import :FileSystem;

// The one place <filesystem>/<fstream> live now: Core no longer exports them, so every other
// library reaches the disk through the API below.
namespace
{
    std::thread::id g_mainThreadId;          // set by initialize()
    bool g_mainThreadKnown = false;
    thread_local int t_allowMainThreadDepth = 0;

    std::filesystem::path toPath(std::string_view s)
    {
        return std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t*>(s.data()), s.size()));
    }
    std::string fromPath(const std::filesystem::path& p)
    {
        const std::u8string s = p.u8string();
        std::string out(reinterpret_cast<const char*>(s.data()), s.size());
        for (char& c : out) // one separator everywhere: paths get compared and logged as strings
            if (c == '\\')
                c = '/';
        return out;
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
    // Blocking the main thread on the disk is a frame hitch — say so explicitly (AllowMainThreadIO
    // or the allowMainThread flag) wherever it is intended.
    assert((allowMainThread || t_allowMainThreadDepth > 0 || !isMainThread())
        && "FileSystem: IO on the main thread — wrap it in FileSystem::AllowMainThreadIO, pass "
           "allowMainThread=true, or move the call to a job");
    (void)allowMainThread;
}

bool FileSystem::initialize()
{
    g_mainThreadId = std::this_thread::get_id();
    g_mainThreadKnown = true;
    const AllowMainThreadIO allowIo; // startup: finding the repo root is main-thread by definition

    std::cout.setf(std::ios::unitbuf);

    constexpr const char* ASSETS_DIR = "/Assets/";
    constexpr const char* DLL_DIR = "/Dependencies/Dll/";

    const std::filesystem::path currDir = std::filesystem::current_path();
    std::filesystem::path rootDir = currDir;
    while (!std::filesystem::exists(std::filesystem::path(rootDir.string() + ASSETS_DIR)))
    {
        if (!rootDir.has_parent_path())
        {
            assert(false && "Could not find project root directory");
            return false;
        }
        rootDir = rootDir.parent_path();
    }
    std::filesystem::path assetsDir = std::filesystem::path(rootDir.string() + ASSETS_DIR);
    std::filesystem::current_path(assetsDir);
    std::filesystem::path dllDir = std::filesystem::path(rootDir.string() + DLL_DIR);
    if (std::filesystem::exists(dllDir))
    {
        AddDllDirectory(dllDir.wstring().c_str());
    }
    return true;
}

// ---------------------------------------------------------------- reads / writes

std::string FileSystem::readFileStr(const std::string& path, bool allowMainThread)
{
    assertIoThread(allowMainThread);
    std::ifstream file(toPath(path), std::ios::in | std::ios::binary | std::ios::ate);
    if (!file.is_open())
        return std::string();

    const std::ifstream::pos_type fileSize = file.tellg();
    file.seekg(0, std::ios::beg);
    std::string fileContent(fileSize, '\0');
    file.read(fileContent.data(), fileSize);
    return fileContent;
}

bool FileSystem::readFileBytes(const std::string& path, std::vector<uint8>& out, bool allowMainThread)
{
    assertIoThread(allowMainThread);
    out.clear();
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

bool FileSystem::writeFileStr(const std::string& path, const std::string& content, bool allowMainThread)
{
    assertIoThread(allowMainThread);
    std::ofstream file(toPath(path), std::ios::out | std::ios::binary | std::ios::trunc);
    if (!file.is_open())
        return false;
    file.write(content.data(), content.size());
    return file.good();
}

bool FileSystem::writeFileBytes(const std::string& path, std::span<const uint8> data, bool allowMainThread)
{
    assertIoThread(allowMainThread);
    std::ofstream file(toPath(path), std::ios::out | std::ios::binary | std::ios::trunc);
    if (!file.is_open())
        return false;
    if (!data.empty())
        file.write(reinterpret_cast<const char*>(data.data()), (std::streamsize)data.size());
    return file.good();
}

// ---------------------------------------------------------------- queries + mutations

bool FileSystem::exists(const std::string& path, bool allowMainThread)
{
    assertIoThread(allowMainThread);
    std::error_code ec;
    return std::filesystem::exists(toPath(path), ec);
}

bool FileSystem::isDirectory(const std::string& path, bool allowMainThread)
{
    assertIoThread(allowMainThread);
    std::error_code ec;
    return std::filesystem::is_directory(toPath(path), ec);
}

bool FileSystem::isRegularFile(const std::string& path, bool allowMainThread)
{
    assertIoThread(allowMainThread);
    std::error_code ec;
    return std::filesystem::is_regular_file(toPath(path), ec);
}

uint64 FileSystem::fileSize(const std::string& path, bool allowMainThread)
{
    assertIoThread(allowMainThread);
    std::error_code ec;
    const auto size = std::filesystem::file_size(toPath(path), ec);
    return ec ? 0ull : (uint64)size;
}

int64 FileSystem::lastWriteTimeSec(const std::string& path, bool allowMainThread)
{
    assertIoThread(allowMainThread);
    std::error_code ec;
    const auto time = std::filesystem::last_write_time(toPath(path), ec);
    if (ec)
        return 0;
    return (int64)std::chrono::duration_cast<std::chrono::seconds>(time.time_since_epoch()).count();
}

bool FileSystem::createDirectories(const std::string& path, bool allowMainThread)
{
    assertIoThread(allowMainThread);
    std::error_code ec;
    std::filesystem::create_directories(toPath(path), ec);
    return !ec;
}

bool FileSystem::remove(const std::string& path, bool allowMainThread)
{
    assertIoThread(allowMainThread);
    std::error_code ec;
    return std::filesystem::remove(toPath(path), ec);
}

uint64 FileSystem::removeAll(const std::string& path, bool allowMainThread)
{
    assertIoThread(allowMainThread);
    std::error_code ec;
    const auto count = std::filesystem::remove_all(toPath(path), ec);
    return ec ? 0ull : (uint64)count;
}

bool FileSystem::rename(const std::string& from, const std::string& to, bool allowMainThread)
{
    assertIoThread(allowMainThread);
    std::error_code ec;
    std::filesystem::rename(toPath(from), toPath(to), ec);
    return !ec;
}

bool FileSystem::copyLastWriteTime(const std::string& from, const std::string& to, bool allowMainThread)
{
    assertIoThread(allowMainThread);
    std::error_code ec;
    const auto time = std::filesystem::last_write_time(toPath(from), ec);
    if (ec)
        return false;
    std::filesystem::last_write_time(toPath(to), time, ec);
    return !ec;
}

bool FileSystem::copyFile(const std::string& from, const std::string& to, bool overwrite, bool allowMainThread)
{
    assertIoThread(allowMainThread);
    std::error_code ec;
    return std::filesystem::copy_file(toPath(from), toPath(to), overwrite
        ? std::filesystem::copy_options::overwrite_existing : std::filesystem::copy_options::none, ec);
}

namespace
{
    template <typename Iterator>
    bool listImpl(Iterator begin, std::vector<FileSystem::DirEntry>& out)
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
            out.push_back(std::move(entry));
        }
        return true;
    }
}

bool FileSystem::listDirectory(const std::string& dir, std::vector<DirEntry>& out, bool allowMainThread)
{
    assertIoThread(allowMainThread);
    out.clear();
    std::error_code ec;
    std::filesystem::directory_iterator it(toPath(dir), ec);
    if (ec)
        return false;
    return listImpl(it, out);
}

bool FileSystem::listDirectoryRecursive(const std::string& dir, std::vector<DirEntry>& out, bool allowMainThread)
{
    assertIoThread(allowMainThread);
    out.clear();
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(toPath(dir), ec);
    if (ec)
        return false;
    return listImpl(it, out);
}

std::string FileSystem::currentPath(bool allowMainThread)
{
    assertIoThread(allowMainThread);
    std::error_code ec;
    return fromPath(std::filesystem::current_path(ec));
}

bool FileSystem::setCurrentPath(const std::string& path, bool allowMainThread)
{
    assertIoThread(allowMainThread);
    std::error_code ec;
    std::filesystem::current_path(toPath(path), ec);
    return !ec;
}

std::string FileSystem::absolutePath(const std::string& path, bool allowMainThread)
{
    assertIoThread(allowMainThread);
    std::error_code ec;
    const std::filesystem::path result = std::filesystem::absolute(toPath(path), ec);
    return ec ? path : fromPath(result);
}

std::string FileSystem::canonicalPath(const std::string& path, bool allowMainThread)
{
    assertIoThread(allowMainThread);
    std::error_code ec;
    const std::filesystem::path result = std::filesystem::canonical(toPath(path), ec);
    return ec ? std::string() : fromPath(result);
}

std::string FileSystem::weaklyCanonicalPath(const std::string& path, bool allowMainThread)
{
    assertIoThread(allowMainThread);
    std::error_code ec;
    const std::filesystem::path result = std::filesystem::weakly_canonical(toPath(path), ec);
    return ec ? std::string() : fromPath(result);
}

std::string FileSystem::relativePath(const std::string& path, const std::string& base, bool allowMainThread)
{
    assertIoThread(allowMainThread);
    std::error_code ec;
    const std::filesystem::path result = base.empty()
        ? std::filesystem::relative(toPath(path), ec)
        : std::filesystem::relative(toPath(path), toPath(base), ec);
    return ec ? std::string() : fromPath(result);
}

// ---------------------------------------------------------------- pure path math (no IO)

std::string FileSystem::join(std::string_view a, std::string_view b)
{
    if (a.empty())
        return std::string(b);
    if (b.empty())
        return std::string(a);
    return fromPath(toPath(a) / toPath(b));
}

std::string FileSystem::parentPath(std::string_view path)  { return fromPath(toPath(path).parent_path()); }
std::string FileSystem::filename(std::string_view path)    { return fromPath(toPath(path).filename()); }
std::string FileSystem::stem(std::string_view path)        { return fromPath(toPath(path).stem()); }
std::string FileSystem::extension(std::string_view path)   { return fromPath(toPath(path).extension()); }

std::string FileSystem::replaceExtension(std::string_view path, std::string_view ext)
{
    std::filesystem::path p = toPath(path);
    p.replace_extension(toPath(ext));
    return fromPath(p);
}

std::string FileSystem::normalize(std::string_view path)
{
    return fromPath(toPath(path).lexically_normal());
}

bool FileSystem::isAbsolute(std::string_view path)
{
    return toPath(path).is_absolute();
}

bool FileSystem::pathEquals(std::string_view a, std::string_view b)
{
    const std::string na = normalize(a);
    const std::string nb = normalize(b);
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
