export module File:PackFile;

import Core;
import Core.BitPathTrie;

import :NativeFile;
import :PackFormat;

// One mounted .dat pack: the file memory mapped read-only, the TOC used in place (no copy, no parse beyond the
// header checks). Every const member is safe to call from any number of threads at once. Internal to File (not
// exported): FileSystem and PackBuilder are its only users.
class PackFile final
{
public:

    static constexpr uint32 INVALID_KEY = oc::BitmapPathTrieView::InvalidKey;

    PackFile() = default;
    ~PackFile() = default;
    PackFile(const PackFile&) = delete;
    PackFile& operator=(const PackFile&) = delete;

    // False with a reason when the file is missing, not a pack, of another version, or its TOC is damaged.
    bool open(const oc::string& path, oc::string& outError);
    void close();
    bool isOpen() const { return m_base != nullptr; }

    const oc::string& path() const { return m_path; }
    const Pack::Header& header() const { return m_header; }
    uint64 fileSize() const { return m_size; }
    uint32 fileCount() const { return m_header.fileCount; }
    uint64 deadBytes() const;

    // packPath: normalized ('/' separated, no leading / trailing '/'), case-insensitive.
    uint32 findFile(oc::string_view packPath) const { return m_files.find(packPath); }
    bool isDirectory(oc::string_view packPath) const { return packPath.empty() || m_dirs.contains(packPath); }
    // The path in its stored case; out must hold packPath.size() chars. False when the file is not in the pack.
    bool canonicalCase(oc::string_view packPath, oc::span<char> out) const;

    const Pack::Entry& entry(uint32 key) const { return m_entries[key]; }
    oc::span<const uint8> storedBytes(uint32 key) const;
    bool isRaw(uint32 key) const { return (m_entries[key].flags & Pack::ENTRY_ZSTD) == 0; }

    // The whole file (decompressed) into dst, which holds entry(key).size bytes.
    bool read(uint32 key, void* dst) const;
    // A byte range of the file. Raw entries read only the range; compressed ones decompress the whole file first.
    bool readRange(uint32 key, uint64 offset, void* dst, uint64 bytes) const;

    const oc::BitmapPathTrieView& files() const { return m_files; }
    const oc::BitmapPathTrieView& directories() const { return m_dirs; }

    // Re-hashes every file against its stored hash; the first failure goes to outError.
    bool verify(oc::string& outError) const;

private:

    bool copyOut(uint64 fileOffset, void* dst, uint64 bytes) const;

    oc::string m_path;
    NativeFile m_file;
    const uint8* m_base = nullptr;
    uint64 m_size = 0;
    Pack::Header m_header;
    oc::BitmapPathTrieView m_files;
    oc::BitmapPathTrieView m_dirs;
    const Pack::Entry* m_entries = nullptr;
};
