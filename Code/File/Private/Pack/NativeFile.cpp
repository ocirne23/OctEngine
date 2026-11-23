module;

#ifdef _MSC_VER
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#else
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

module File;

import Core;

import :NativeFile;
import :FileSystem;

#ifndef _MSC_VER
// Linux: m_handle holds fd + 1 (so nullptr stays "closed"), m_mapping the mapped byte count.
namespace
{
    int fdOf(void* handle) { return int(reinterpret_cast<intptr_t>(handle)) - 1; }
    void* handleOf(int fd) { return reinterpret_cast<void*>(intptr_t(fd) + 1); }
}

NativeFile::~NativeFile()
{
    close();
}

NativeFile::NativeFile(NativeFile&& other) noexcept
    : m_handle(other.m_handle), m_mapping(other.m_mapping), m_view(other.m_view)
{
    other.m_handle = nullptr;
    other.m_mapping = nullptr;
    other.m_view = nullptr;
}

NativeFile& NativeFile::operator=(NativeFile&& other) noexcept
{
    if (this != &other)
    {
        close();
        m_handle = other.m_handle;
        m_mapping = other.m_mapping;
        m_view = other.m_view;
        other.m_handle = nullptr;
        other.m_mapping = nullptr;
        other.m_view = nullptr;
    }
    return *this;
}

bool NativeFile::open(oc::string_view path, EMode mode)
{
    close();
    const oc::string resolved = FileSystem::resolve(path);
    int flags = O_CLOEXEC;
    if (mode == EMode::Read)
        flags |= O_RDONLY;
    else
        flags |= O_RDWR;
    if (mode == EMode::Create)
        flags |= O_CREAT | O_TRUNC;
    const int fd = ::open(resolved.c_str(), flags, 0644);
    if (fd < 0)
        return false;
    m_handle = handleOf(fd);
    return true;
}

void NativeFile::close()
{
    unmap();
    if (m_handle)
    {
        ::close(fdOf(m_handle));
        m_handle = nullptr;
    }
}

uint64 NativeFile::size() const
{
    struct stat info{};
    if (!m_handle || fstat(fdOf(m_handle), &info) != 0)
        return 0;
    return (uint64)info.st_size;
}

bool NativeFile::readAt(uint64 offset, void* dst, uint64 bytes) const
{
    if (!m_handle)
        return false;
    uint8* out = (uint8*)dst;
    while (bytes > 0)
    {
        const ssize_t read = pread(fdOf(m_handle), out, bytes, off_t(offset));
        if (read <= 0)
        {
            if (read < 0 && errno == EINTR)
                continue;
            return false;
        }
        out += read;
        offset += uint64(read);
        bytes -= uint64(read);
    }
    return true;
}

bool NativeFile::writeAt(uint64 offset, const void* src, uint64 bytes)
{
    if (!m_handle)
        return false;
    const uint8* in = (const uint8*)src;
    while (bytes > 0)
    {
        const ssize_t written = pwrite(fdOf(m_handle), in, bytes, off_t(offset));
        if (written <= 0)
        {
            if (written < 0 && errno == EINTR)
                continue;
            return false;
        }
        in += written;
        offset += uint64(written);
        bytes -= uint64(written);
    }
    return true;
}

bool NativeFile::flush()
{
    return m_handle && fsync(fdOf(m_handle)) == 0;
}

const uint8* NativeFile::map(uint64& outSize)
{
    unmap();
    outSize = size();
    if (!m_handle || outSize == 0)
        return nullptr;
    void* view = mmap(nullptr, outSize, PROT_READ, MAP_SHARED, fdOf(m_handle), 0);
    if (view == MAP_FAILED)
        return nullptr;
    m_mapping = reinterpret_cast<void*>(uintptr_t(outSize));
    m_view = (const uint8*)view;
    return m_view;
}

void NativeFile::unmap()
{
    if (m_view)
    {
        munmap(const_cast<uint8*>(m_view), size_t(reinterpret_cast<uintptr_t>(m_mapping)));
        m_view = nullptr;
    }
    m_mapping = nullptr;
}

int64 NativeFile::lastWriteTicks(oc::string_view path)
{
    struct stat info{};
    if (stat(FileSystem::resolve(path).c_str(), &info) != 0)
        return 0;
    return (int64)info.st_mtim.tv_sec * 10000000 + info.st_mtim.tv_nsec / 100; // 100 ns ticks, like the Win32 branch
}

bool NativeFile::replaceFile(oc::string_view from, oc::string_view to)
{
    return rename(FileSystem::resolve(from).c_str(), FileSystem::resolve(to).c_str()) == 0; // atomic replace
}

bool NativeFile::removeFile(oc::string_view path)
{
    return unlink(FileSystem::resolve(path).c_str()) == 0;
}
#else
namespace
{
    // UTF-8 -> UTF-16 into a stack buffer; long paths go to the heap.
    struct WidePath
    {
        wchar_t local[512];
        oc::vector<wchar_t> heap;
        const wchar_t* str = local;

        explicit WidePath(oc::string_view relativeOrAbsolute)
        {
            const oc::string path = FileSystem::resolve(relativeOrAbsolute);
            const int length = path.empty() ? 0 : MultiByteToWideChar(CP_UTF8, 0, path.data(), (int)path.size(), nullptr, 0);
            wchar_t* out = local;
            if (length + 1 > (int)oc::size(local))
            {
                heap.resize((size_t)length + 1);
                out = heap.data();
                str = out;
            }
            if (length > 0)
                MultiByteToWideChar(CP_UTF8, 0, path.data(), (int)path.size(), out, length);
            out[length] = 0;
        }
    };

    constexpr uint64 MAX_IO_CHUNK = 1ull << 30; // ReadFile / WriteFile take a DWORD count
}

NativeFile::~NativeFile()
{
    close();
}

NativeFile::NativeFile(NativeFile&& other) noexcept
    : m_handle(other.m_handle), m_mapping(other.m_mapping), m_view(other.m_view)
{
    other.m_handle = nullptr;
    other.m_mapping = nullptr;
    other.m_view = nullptr;
}

NativeFile& NativeFile::operator=(NativeFile&& other) noexcept
{
    if (this != &other)
    {
        close();
        m_handle = other.m_handle;
        m_mapping = other.m_mapping;
        m_view = other.m_view;
        other.m_handle = nullptr;
        other.m_mapping = nullptr;
        other.m_view = nullptr;
    }
    return *this;
}

bool NativeFile::open(oc::string_view path, EMode mode)
{
    close();
    const WidePath wide(path);
    DWORD access = GENERIC_READ;
    DWORD disposition = OPEN_EXISTING;
    if (mode != EMode::Read)
        access |= GENERIC_WRITE;
    if (mode == EMode::Create)
        disposition = CREATE_ALWAYS;
    // Readers share delete too, so a pack can be replaced by rename once its mapping is gone.
    const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | (mode == EMode::Read ? FILE_SHARE_DELETE : 0);
    const HANDLE handle = CreateFileW(wide.str, access, share, nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return false;
    m_handle = handle;
    return true;
}

void NativeFile::close()
{
    unmap();
    if (m_handle)
    {
        CloseHandle((HANDLE)m_handle);
        m_handle = nullptr;
    }
}

uint64 NativeFile::size() const
{
    LARGE_INTEGER size{};
    if (!m_handle || !GetFileSizeEx((HANDLE)m_handle, &size))
        return 0;
    return (uint64)size.QuadPart;
}

bool NativeFile::readAt(uint64 offset, void* dst, uint64 bytes) const
{
    if (!m_handle)
        return false;
    uint8* out = (uint8*)dst;
    while (bytes > 0)
    {
        const DWORD chunk = (DWORD)oc::min(bytes, MAX_IO_CHUNK);
        OVERLAPPED overlapped{};
        overlapped.Offset = (DWORD)offset;
        overlapped.OffsetHigh = (DWORD)(offset >> 32);
        DWORD read = 0;
        if (!ReadFile((HANDLE)m_handle, out, chunk, &read, &overlapped) || read != chunk)
            return false;
        out += chunk;
        offset += chunk;
        bytes -= chunk;
    }
    return true;
}

bool NativeFile::writeAt(uint64 offset, const void* src, uint64 bytes)
{
    if (!m_handle)
        return false;
    const uint8* in = (const uint8*)src;
    while (bytes > 0)
    {
        const DWORD chunk = (DWORD)oc::min(bytes, MAX_IO_CHUNK);
        OVERLAPPED overlapped{};
        overlapped.Offset = (DWORD)offset;
        overlapped.OffsetHigh = (DWORD)(offset >> 32);
        DWORD written = 0;
        if (!WriteFile((HANDLE)m_handle, in, chunk, &written, &overlapped) || written != chunk)
            return false;
        in += chunk;
        offset += chunk;
        bytes -= chunk;
    }
    return true;
}

bool NativeFile::flush()
{
    return m_handle && FlushFileBuffers((HANDLE)m_handle) != 0;
}

const uint8* NativeFile::map(uint64& outSize)
{
    unmap();
    outSize = size();
    if (!m_handle || outSize == 0)
        return nullptr;
    const HANDLE mapping = CreateFileMappingW((HANDLE)m_handle, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!mapping)
        return nullptr;
    const void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (!view)
    {
        CloseHandle(mapping);
        return nullptr;
    }
    m_mapping = mapping;
    m_view = (const uint8*)view;
    return m_view;
}

void NativeFile::unmap()
{
    if (m_view)
    {
        UnmapViewOfFile(m_view);
        m_view = nullptr;
    }
    if (m_mapping)
    {
        CloseHandle((HANDLE)m_mapping);
        m_mapping = nullptr;
    }
}

int64 NativeFile::lastWriteTicks(oc::string_view path)
{
    const WidePath wide(path);
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(wide.str, GetFileExInfoStandard, &data))
        return 0;
    // FILETIME = 100 ns ticks since 1601, which is exactly MSVC's file_clock
    return (int64)(((uint64)data.ftLastWriteTime.dwHighDateTime << 32) | data.ftLastWriteTime.dwLowDateTime);
}

bool NativeFile::replaceFile(oc::string_view from, oc::string_view to)
{
    const WidePath wideFrom(from);
    const WidePath wideTo(to);
    return MoveFileExW(wideFrom.str, wideTo.str, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

bool NativeFile::removeFile(oc::string_view path)
{
    const WidePath wide(path);
    return DeleteFileW(wide.str) != 0;
}
#endif
