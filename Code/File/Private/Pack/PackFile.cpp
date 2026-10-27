module;

#include <zstd/zstd.h>

module File;

import Core;
import Core.BitPathTrie;

import :NativeFile;
import :PackFormat;
import :PackFile;

namespace
{
    // Copies at least this big go through ReadFile instead of memcpy from the mapping: one IO request for the
    // whole range instead of a page fault per mapped page.
    constexpr uint64 LARGE_COPY_BYTES = 64 * 1024;

    template<typename T>
    oc::span<const T> sectionSpan(const uint8* base, const Pack::Section& section)
    {
        return oc::span<const T>(reinterpret_cast<const T*>(base + section.offset), (size_t)(section.size / sizeof(T)));
    }

    oc::BitmapPathTrieStorage trieStorage(const uint8* base, const Pack::Section* sections, uint32 bucketKeyBits)
    {
        oc::BitmapPathTrieStorage storage;
        storage.nodes = sectionSpan<oc::pathTrieDetail::Node>(base, sections[0]);
        storage.poolSymbols = sectionSpan<uint8>(base, sections[1]);
        storage.poolCase = sectionSpan<uint8>(base, sections[2]);
        storage.caseExceptions = sectionSpan<oc::BitmapPathTrieCaseException>(base, sections[3]);
        storage.casePositions = sectionSpan<uint16>(base, sections[4]);
        Pack::setBucketKeys(storage, sectionSpan<uint8>(base, sections[5]), bucketKeyBits);
        return storage;
    }
}

bool PackFile::open(const oc::string& path, oc::string& outError)
{
    close();
    if (!m_file.open(path, NativeFile::EMode::Read))
    {
        outError = "cannot open " + path;
        return false;
    }
    const uint8* base = m_file.map(m_size);
    if (!base || m_size < Pack::HEADER_SIZE)
    {
        outError = path + " is empty or cannot be mapped";
        close();
        return false;
    }

    const Pack::Header& header = *reinterpret_cast<const Pack::Header*>(base);
    const uint32 expectedFlags = Pack::IMPLICIT_KEYS ? Pack::HEADER_IMPLICIT_KEYS : 0;
    auto fail = [&](const char* reason)
    {
        outError = oc::format("{}: {}", path, reason);
        close();
        return false;
    };
    if (header.magic != Pack::MAGIC)
        return fail("not a pack");
    if (header.version != Pack::VERSION || header.trieNodeSize != sizeof(oc::pathTrieDetail::Node)
        || (header.flags & Pack::HEADER_IMPLICIT_KEYS) != expectedFlags)
        return fail("built by another pack version, rebuild it");
    if (memchr(header.mountPoint, 0, sizeof(header.mountPoint)) == nullptr)
        return fail("mount point not terminated");
    if (header.tocOffset < Pack::HEADER_SIZE || header.tocOffset > m_size || header.tocSize > m_size - header.tocOffset)
        return fail("TOC out of bounds");
    if (Pack::hashBytes(base + header.tocOffset, header.tocSize) != header.tocHash)
        return fail("TOC hash mismatch");

    const uint64 tocEnd = header.tocOffset + header.tocSize;
    for (const Pack::Section& section : header.sections)
        if (section.size > 0 && (section.offset % Pack::SECTION_ALIGN != 0 || section.offset < header.tocOffset
            || section.offset > tocEnd || section.size > tocEnd - section.offset))
            return fail("TOC section out of bounds");
    if (header.sections[Pack::ENTRIES].size != (uint64)header.fileCount * sizeof(Pack::Entry))
        return fail("entry count mismatch");

    m_base = base;
    m_header = header;
    m_path = path;
    m_files = oc::BitmapPathTrieView(trieStorage(base, &header.sections[Pack::FILE_NODES], header.fileBucketKeyBits));
    m_dirs = oc::BitmapPathTrieView(trieStorage(base, &header.sections[Pack::DIR_NODES], header.dirBucketKeyBits));
    m_entries = reinterpret_cast<const Pack::Entry*>(base + header.sections[Pack::ENTRIES].offset);

    for (uint32 key = 0; key < header.fileCount; ++key)
    {
        const Pack::Entry& e = m_entries[key];
        if (e.offset < Pack::HEADER_SIZE || e.offset > header.tocOffset || e.storedSize > header.tocOffset - e.offset
            || ((e.flags & Pack::ENTRY_ZSTD) == 0 && e.storedSize != e.size))
            return fail("file entry out of bounds");
    }
    return true;
}

void PackFile::close()
{
    m_file.close();
    m_base = nullptr;
    m_size = 0;
    m_header = {};
    m_files = {};
    m_dirs = {};
    m_entries = nullptr;
}

uint64 PackFile::deadBytes() const
{
    if (!m_base)
        return 0;
    const uint64 dataBytes = m_header.tocOffset - Pack::HEADER_SIZE;
    return dataBytes > m_header.liveBytes ? dataBytes - m_header.liveBytes : 0;
}

bool PackFile::canonicalCase(oc::string_view packPath, oc::span<char> out) const
{
    return m_files.find(packPath, false, out) != INVALID_KEY;
}

oc::span<const uint8> PackFile::storedBytes(uint32 key) const
{
    const Pack::Entry& e = m_entries[key];
    return oc::span<const uint8>(m_base + e.offset, (size_t)e.storedSize);
}

bool PackFile::copyOut(uint64 fileOffset, void* dst, uint64 bytes) const
{
    if (bytes >= LARGE_COPY_BYTES)
        return m_file.readAt(fileOffset, dst, bytes);
    memcpy(dst, m_base + fileOffset, (size_t)bytes);
    return true;
}

bool PackFile::read(uint32 key, void* dst) const
{
    const Pack::Entry& e = m_entries[key];
    if (isRaw(key))
        return copyOut(e.offset, dst, e.size);
    const size_t written = ZSTD_decompress(dst, (size_t)e.size, m_base + e.offset, (size_t)e.storedSize);
    return !ZSTD_isError(written) && written == e.size;
}

bool PackFile::readRange(uint32 key, uint64 offset, void* dst, uint64 bytes) const
{
    const Pack::Entry& e = m_entries[key];
    if (offset > e.size || bytes > e.size - offset)
        return false;
    if (isRaw(key))
        return copyOut(e.offset + offset, dst, bytes);
    oc::vector<uint8> whole((size_t)e.size);
    if (!read(key, whole.data()))
        return false;
    memcpy(dst, whole.data() + offset, (size_t)bytes);
    return true;
}

bool PackFile::verify(oc::string& outError) const
{
    oc::vector<uint8> buffer;
    bool ok = true;
    m_files.forEach([&](const oc::BitmapPathTrieEntry& file)
    {
        if (!ok)
            return;
        const Pack::Entry& e = m_entries[file.key];
        const uint8* data = m_base + e.offset;
        if (!isRaw(file.key))
        {
            buffer.resize((size_t)e.size);
            if (!read(file.key, buffer.data()))
            {
                outError = oc::format("{}: cannot decompress", file.path);
                ok = false;
                return;
            }
            data = buffer.data();
        }
        if (Pack::hashBytes(data, e.size) != e.hash)
        {
            outError = oc::format("{}: content hash mismatch", file.path);
            ok = false;
        }
    });
    return ok;
}
