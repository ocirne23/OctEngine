module;

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

module File;

import Core;

import :NativeFile;

namespace
{
    // UTF-8 -> UTF-16 into a stack buffer; long paths go to the heap.
    struct WidePath
    {
        wchar_t local[512];
        oc::vector<wchar_t> heap;
        const wchar_t* str = local;

        explicit WidePath(oc::string_view path)
        {
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
