export module File:NativeFile;

import Core;

// A raw Win32 file handle with POSITIONAL reads and writes (no shared file pointer, so concurrent readAt calls on one
// handle are safe) plus a read-only memory mapping. The pack reader, the pack builder and FileReader's disk path sit
// on it; Windows.h stays in NativeFile.cpp. No main-thread assert here: the callers own the policy.
export class NativeFile final
{
public:

    enum class EMode
    {
        Read,       // existing file, shared with readers AND writers (a mounted pack stays mounted while it is updated)
        ReadWrite,  // existing file, shared with readers and writers
        Create,     // create or truncate, read + write
    };

    NativeFile() = default;
    ~NativeFile();
    NativeFile(NativeFile&& other) noexcept;
    NativeFile& operator=(NativeFile&& other) noexcept;
    NativeFile(const NativeFile&) = delete;
    NativeFile& operator=(const NativeFile&) = delete;

    bool open(oc::string_view path, EMode mode);
    void close();
    bool isOpen() const { return m_handle != nullptr; }

    uint64 size() const;
    bool readAt(uint64 offset, void* dst, uint64 bytes) const;
    bool writeAt(uint64 offset, const void* src, uint64 bytes);
    bool flush();

    // Maps the whole file read-only. The view stays valid until unmap() / close(), also while another handle appends
    // to the file (the mapping keeps the size it was made with).
    const uint8* map(uint64& outSize);
    void unmap();

    // File clock ticks (the std::filesystem::file_time_type duration), 0 on failure.
    static int64 lastWriteTicks(oc::string_view path);
    // MoveFileEx with replace; fails while the destination is open or mapped without FILE_SHARE_DELETE.
    static bool replaceFile(oc::string_view from, oc::string_view to);
    static bool removeFile(oc::string_view path);

private:

    void* m_handle = nullptr;  // HANDLE, null when closed
    void* m_mapping = nullptr; // file mapping HANDLE
    const uint8* m_view = nullptr;
};
