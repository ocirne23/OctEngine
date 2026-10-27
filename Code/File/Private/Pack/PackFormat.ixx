export module File:PackFormat;

import Core;
import Core.BitPathTrie;

// The .dat pack on disk. Raw little-endian structs, no compatibility: a layout change bumps VERSION and an old pack
// refuses to mount (rebuild it).
//
//   [Header, one page] [file data ...] [dead data of older versions ...] [TOC]
//
// The TOC (every section the Header lists) sits at the END, after the data, so an update appends the new and changed
// files plus a new TOC behind the old one and then rewrites the header page: the header write is the commit, and the
// old TOC stays intact until then. File data is never overwritten in place, so a process that has the pack mounted
// keeps reading the version it mounted. Dead bytes (old versions, old TOCs) are reclaimed by a full rewrite.
//
// The TOC is two Core.BitPathTrie tries baked as-is (the file paths and every directory that holds a file, no
// trailing '/'), plus one Entry per file, indexed by the file trie's key. The trie storage spans point straight into
// the mapped pack: mounting copies nothing.
export namespace Pack
{
    inline constexpr uint32 MAGIC = 0x4B50434Fu; // "OCPK"
    inline constexpr uint32 VERSION = 2;
    inline constexpr uint64 HEADER_SIZE = 4096;  // the header page; data starts page aligned
    inline constexpr uint64 SECTION_ALIGN = 16;  // every TOC section and every small file
    inline constexpr uint64 PAGE_ALIGN = 4096;
    inline constexpr uint64 PAGE_ALIGN_MIN = 64 * 1024; // files at least this big start on a page (streamed ranges)

    // Implicit keys (Core.BitPathTrie's OC_PATH_TRIE_IMPLICIT_KEYS): a key is the path's position in symbol order.
    inline constexpr bool IMPLICIT_KEYS = requires(const oc::BitmapPathTrie& trie) { trie.orderedKeys(); };

    enum ESection : uint32
    {
        FILE_NODES, FILE_POOL_SYMBOLS, FILE_POOL_CASE, FILE_CASE_EXCEPTIONS, FILE_CASE_POSITIONS, FILE_BUCKET_KEYS,
        DIR_NODES, DIR_POOL_SYMBOLS, DIR_POOL_CASE, DIR_CASE_EXCEPTIONS, DIR_CASE_POSITIONS, DIR_BUCKET_KEYS,
        ENTRIES,
        SECTION_COUNT,
    };

    struct Section
    {
        uint64 offset = 0; // absolute file offset
        uint64 size = 0;   // bytes
    };

    enum EEntryFlags : uint32
    {
        ENTRY_ZSTD = 1u << 0, // stored bytes are one zstd frame of `size` bytes
    };

    struct Entry
    {
        uint64 offset = 0;     // absolute file offset of the stored bytes
        uint64 storedSize = 0; // bytes in the pack
        uint64 size = 0;       // bytes of the file (after decompression)
        int64 mtimeTicks = 0;  // the source's modification time, file clock ticks (FileSystem::lastWriteTimeTicks)
        uint64 hash = 0;       // hashBytes() of the file content
        uint32 flags = 0;      // EEntryFlags
        uint32 reserved = 0;
    };
    static_assert(sizeof(Entry) == 48);

    enum EHeaderFlags : uint32
    {
        HEADER_IMPLICIT_KEYS = 1u << 0,
    };

    struct Header
    {
        uint32 magic = MAGIC;
        uint32 version = VERSION;
        uint32 flags = 0;          // EHeaderFlags
        uint32 trieNodeSize = 0;   // sizeof(BitmapPathTrie::Node): a trie layout change refuses old packs
        uint32 fileCount = 0;
        uint32 dirCount = 0;
        uint32 fileBucketKeyBits = 0; // stored keys only
        uint32 dirBucketKeyBits = 0;
        uint64 tocOffset = 0;
        uint64 tocSize = 0;
        uint64 tocHash = 0;        // hashBytes() of the TOC
        uint64 liveBytes = 0;      // stored bytes the entries reference (+ alignment is NOT included)
        uint64 dataEnd = 0;        // end of the last data written; the TOC follows, aligned
        int64 buildTicks = 0;      // file clock ticks of the build / last update
        uint32 updateCount = 0;    // incremental updates since the last full write
        int32 mountPriority = 100; // how the pack mounts when the mounter does not say (FileSystem::mountPack)
        Section sections[SECTION_COUNT];
        char mountPoint[512] = {}; // Assets-relative folder, '\0'-terminated, "" = Assets/ itself
    };
    static_assert(sizeof(Header) <= HEADER_SIZE);

    inline constexpr uint64 alignUp(uint64 value, uint64 alignment) { return (value + alignment - 1) & ~(alignment - 1); }
    inline uint64 dataAlignment(uint64 storedSize) { return storedSize >= PAGE_ALIGN_MIN ? PAGE_ALIGN : SECTION_ALIGN; }

    // 64-bit content hash: 8 bytes per step, multiply-rotate mixing, murmur3 finalizer. Change detection and
    // integrity, not cryptography. Changing it invalidates every stored hash: bump VERSION.
    inline uint64 hashBytes(const void* data, uint64 size, uint64 seed = 0)
    {
        constexpr uint64 K1 = 0x9E3779B185EBCA87ull;
        constexpr uint64 K2 = 0xC2B2AE3D27D4EB4Full;
        const uint8* p = (const uint8*)data;
        uint64 h = seed ^ (size * K1);
        uint64 a = h;
        uint64 b = ~h;
        uint64 i = 0;
        for (; i + 16 <= size; i += 16)
        {
            uint64 w0, w1;
            memcpy(&w0, p + i, 8);
            memcpy(&w1, p + i + 8, 8);
            a = oc::rotl(a ^ (w0 * K2), 31) * K1;
            b = oc::rotl(b ^ (w1 * K2), 29) * K1;
        }
        uint64 tail = 0;
        if (i < size)
        {
            uint8 bytes[16] = {};
            memcpy(bytes, p + i, size - i);
            uint64 w0, w1;
            memcpy(&w0, bytes, 8);
            memcpy(&w1, bytes + 8, 8);
            a = oc::rotl(a ^ (w0 * K2), 31) * K1;
            tail = w1;
        }
        h = a ^ oc::rotl(b, 17) ^ (tail * K2);
        h ^= h >> 33;
        h *= 0xFF51AFD7ED558CCDull;
        h ^= h >> 33;
        h *= 0xC4CEB9FE1A85EC53ull;
        h ^= h >> 33;
        return h;
    }

    inline constexpr uint32 MAX_PATH_CHARS = oc::pathTrieDetail::MaxPath; // the trie's limit, terminator included

    inline char lowerAscii(char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; }

    // Lexically normalizes a RELATIVE path into out: '\' -> '/', empty and "." segments dropped, ".." pops one
    // segment, no leading / trailing '/'. False when ".." climbs above the start or the result does not fit.
    inline bool normalizeRelative(oc::string_view path, char* out, uint32& outLength)
    {
        uint32 length = 0;
        size_t i = 0;
        while (i < path.size())
        {
            size_t end = i;
            while (end < path.size() && path[end] != '/' && path[end] != '\\')
                ++end;
            const size_t segment = end - i;
            if (segment == 0 || (segment == 1 && path[i] == '.'))
            {
            }
            else if (segment == 2 && path[i] == '.' && path[i + 1] == '.')
            {
                if (length == 0)
                    return false;
                while (length > 0 && out[length - 1] != '/')
                    --length;
                if (length > 0)
                    --length; // the '/' before the popped segment
            }
            else
            {
                if (length + (length ? 1 : 0) + segment >= MAX_PATH_CHARS)
                    return false;
                if (length)
                    out[length++] = '/';
                memcpy(out + length, path.data() + i, segment);
                length += (uint32)segment;
            }
            i = end + 1;
        }
        outLength = length;
        return true;
    }

    // Case-insensitive "path starts with prefix", ASCII only (the trie's alphabet is ASCII).
    inline bool startsWithNoCase(oc::string_view path, oc::string_view prefix)
    {
        if (path.size() < prefix.size())
            return false;
        for (size_t i = 0; i < prefix.size(); ++i)
            if (lowerAscii(path[i]) != lowerAscii(prefix[i]))
                return false;
        return true;
    }

    // The trie storage spans of a pack's TOC, for either key mode.
    template<typename Storage>
    oc::span<const uint8> bucketKeysOf(const Storage& storage)
    {
        if constexpr (requires { storage.bucketKeys; })
            return storage.bucketKeys;
        else
            return {};
    }

    template<typename Storage>
    uint32 bucketKeyBitsOf(const Storage& storage)
    {
        if constexpr (requires { storage.bucketKeyBits; })
            return storage.bucketKeyBits;
        else
            return 0;
    }

    template<typename Storage>
    void setBucketKeys(Storage& storage, oc::span<const uint8> keys, uint32 bits)
    {
        if constexpr (requires { storage.bucketKeys; })
        {
            storage.bucketKeys = keys;
            storage.bucketKeyBits = bits;
        }
        else
        {
            (void)keys;
            (void)bits;
        }
    }
}
