module;

#include <zstd/zstd.h>

module File;

import <filesystem>;

import Core;
import Core.Log;
import Core.BitPathTrie;

import :FileSystem;
import :NativeFile;
import :PackFormat;
import :PackFile;
import :PackBuilder;

namespace
{
    constexpr uint64 WINDOW_BYTES = 256ull << 20;   // source bytes loaded at once while writing
    constexpr uint32 WINDOW_FILES = 4096;
    constexpr uint64 KEEP_PROBED_BYTES = 512ull << 20; // probed (changed) files kept in memory for the write pass
    constexpr uint64 WRITE_BUFFER_BYTES = 8ull << 20;
    constexpr uint64 MIN_COMPRESS_BYTES = 256;

    enum class EState : uint8
    {
        New,       // not in the old pack
        Changed,   // in the old pack, other content
        Touched,   // in the old pack, stamp changed, same content
        Unchanged, // in the old pack, same stamp
    };

    struct SourceFile
    {
        oc::string packPath;
        oc::string diskPath;
        uint64 size = 0;
        int64 mtimeTicks = 0;
        uint32 oldKey = PackFile::INVALID_KEY;
        EState state = EState::New;
        bool rawOnly = false;   // extension never compressed
        bool loaded = false;     // data holds the content (raw, or compressed once `compressed`)
        bool compressed = false; // the compression decision was made
        bool loadFailed = false;
        uint64 hash = 0;
        oc::vector<uint8> data; // stored bytes (compressed when flags say so)
        Pack::Entry entry;      // in the new pack
    };

    bool reuses(const SourceFile& file) { return file.state == EState::Touched || file.state == EState::Unchanged; }

    oc::string lowered(oc::string_view text)
    {
        oc::string out(text.data(), text.size());
        for (char& c : out)
            c = Pack::lowerAscii(c);
        return out;
    }

    bool equalsNoCase(oc::string_view a, oc::string_view b)
    {
        return a.size() == b.size() && Pack::startsWithNoCase(a, b);
    }

    bool listedExtension(oc::string_view path, const oc::vector<oc::string>& extensions)
    {
        const size_t slash = path.rfind('/');
        const size_t dot = path.rfind('.');
        if (dot == oc::string_view::npos || (slash != oc::string_view::npos && dot < slash))
            return false;
        const oc::string_view extension = path.substr(dot);
        for (const oc::string& listed : extensions)
            if (equalsNoCase(extension, listed))
                return true;
        return false;
    }

    bool trieCanHold(oc::string_view path)
    {
        if (path.empty() || path.size() >= Pack::MAX_PATH_CHARS)
            return false;
        for (char c : path)
            if (oc::pathTrieDetail::charToSymbol(c) == oc::pathTrieDetail::InvalidSymbol)
                return false;
        return true;
    }

    oc::string fromFsPath(const std::filesystem::path& path)
    {
        const std::u8string text = path.generic_u8string();
        return oc::string(reinterpret_cast<const char*>(text.data()), text.size());
    }

    std::filesystem::path toFsPath(oc::string_view path)
    {
        return std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t*>(path.data()), path.size()));
    }

    // Runs func(i) for every i in [0, count) on up to `threads` threads (this one included).
    template<typename Func>
    void parallelFor(uint32 count, uint32 threads, const Func& func)
    {
        if (count == 0)
            return;
        oc::atomic<uint32> next = 0;
        auto worker = [&]
        {
            for (uint32 i = next.fetch_add(1); i < count; i = next.fetch_add(1))
                func(i);
        };
        const uint32 helpers = oc::min(threads, count) - 1;
        oc::vector<std::jthread> pool;
        pool.reserve(helpers);
        for (uint32 t = 0; t < helpers; ++t)
            pool.emplace_back(worker);
        worker();
    }

    bool loadSource(SourceFile& file, oc::vector<uint8>& out)
    {
        NativeFile source;
        if (!source.open(file.diskPath, NativeFile::EMode::Read))
            return false;
        out.resize((size_t)source.size());
        return out.empty() || source.readAt(0, out.data(), out.size());
    }

    // Loads and hashes one file into file.data (raw) / file.entry.
    void loadAndHash(SourceFile& file, oc::atomic<uint64>& bytesRead)
    {
        if (file.loaded)
            return;
        if (!loadSource(file, file.data))
        {
            file.loadFailed = true;
            file.data = {};
            return;
        }
        bytesRead += file.data.size();
        file.size = file.data.size();
        file.hash = Pack::hashBytes(file.data.data(), file.data.size());
        file.entry.size = file.data.size();
        file.entry.storedSize = file.data.size();
        file.entry.hash = file.hash;
        file.entry.mtimeTicks = file.mtimeTicks;
        file.entry.flags = 0;
        file.loaded = true;
        file.compressed = false;
    }

    // loadAndHash, then replaces file.data with its zstd frame when that saves enough.
    void prepare(SourceFile& file, const PackBuildOptions& options, oc::atomic<uint64>& bytesRead)
    {
        loadAndHash(file, bytesRead);
        if (!file.loaded || file.compressed)
            return;
        file.compressed = true;
        if (options.compression != EPackCompression::Zstd || file.rawOnly || file.data.size() < MIN_COMPRESS_BYTES)
            return;
        oc::vector<uint8> compressed(ZSTD_compressBound(file.data.size()));
        const size_t bytes = ZSTD_compress(compressed.data(), compressed.size(), file.data.data(), file.data.size(), options.compressionLevel);
        if (ZSTD_isError(bytes) || (double)bytes > (double)file.data.size() * (1.0 - (double)options.minCompressionGain))
            return;
        compressed.resize(bytes);
        compressed.shrink_to_fit();
        file.data = oc::move(compressed);
        file.entry.storedSize = bytes;
        file.entry.flags = Pack::ENTRY_ZSTD;
    }

    template<typename T>
    oc::span<const uint8> bytesOf(oc::span<const T> items)
    {
        return oc::span<const uint8>(reinterpret_cast<const uint8*>(items.data()), items.size_bytes());
    }

    // Buffered positional writes.
    class PackWriter
    {
    public:
        PackWriter(NativeFile& file, uint64 position) : m_file(file), m_position(position), m_bufferStart(position)
        {
            m_buffer.reserve(WRITE_BUFFER_BYTES);
        }

        uint64 position() const { return m_position; }
        uint64 written() const { return m_written; }
        bool ok() const { return m_ok; }

        void write(const void* data, uint64 bytes)
        {
            if (m_buffer.size() + bytes > WRITE_BUFFER_BYTES)
                flush();
            if (bytes >= WRITE_BUFFER_BYTES)
            {
                m_ok = m_ok && m_file.writeAt(m_position, data, bytes);
                m_position += bytes;
                m_bufferStart = m_position;
            }
            else
            {
                const uint8* in = (const uint8*)data;
                m_buffer.insert(m_buffer.end(), in, in + bytes);
                m_position += bytes;
            }
            m_written += bytes;
        }

        void padTo(uint64 alignment)
        {
            static constexpr uint8 zeros[Pack::PAGE_ALIGN] = {};
            const uint64 padding = Pack::alignUp(m_position, alignment) - m_position;
            if (padding > 0)
                write(zeros, padding);
        }

        bool flush()
        {
            if (!m_buffer.empty())
            {
                m_ok = m_ok && m_file.writeAt(m_bufferStart, m_buffer.data(), m_buffer.size());
                m_buffer.clear();
            }
            m_bufferStart = m_position;
            return m_ok;
        }

    private:
        NativeFile& m_file;
        uint64 m_position = 0;
        uint64 m_bufferStart = 0;
        uint64 m_written = 0;
        oc::vector<uint8> m_buffer;
        bool m_ok = true;
    };

    // Appends one trie's six sections to the TOC buffer.
    void appendTrie(oc::vector<uint8>& toc, uint64 tocOffset, const oc::BitmapPathTrieStorage& storage, Pack::Section* sections)
    {
        const oc::span<const uint8> parts[6] =
        {
            bytesOf(storage.nodes),
            storage.poolSymbols,
            storage.poolCase,
            bytesOf(storage.caseExceptions),
            bytesOf(storage.casePositions),
            Pack::bucketKeysOf(storage),
        };
        for (uint32 i = 0; i < 6; ++i)
        {
            toc.resize((size_t)Pack::alignUp(toc.size(), Pack::SECTION_ALIGN), 0);
            sections[i] = { tocOffset + toc.size(), parts[i].size() };
            toc.insert(toc.end(), parts[i].begin(), parts[i].end());
        }
    }

    // key -> index into the inserted paths.
    oc::vector<uint32> idsByKey(const oc::BitmapPathTrie& trie, uint32 count)
    {
        if constexpr (Pack::IMPLICIT_KEYS)
            return trie.orderedKeys();
        else
        {
            oc::vector<uint32> ids(count);
            for (uint32 i = 0; i < count; ++i)
                ids[i] = i;
            return ids;
        }
    }

    int64 nowTicks()
    {
        return (int64)std::filesystem::file_time_type::clock::now().time_since_epoch().count();
    }
}

PackBuildResult PackBuilder::build(const PackBuildOptions& options, bool allowMainThread)
{
    FileSystem::assertIoThread(allowMainThread);
    ProfileScope profileScope("PackBuilder::build", EProfileCategory::File);
    const Clock::time_point start = Clock::now();
    PackBuildResult result;
    auto fail = [&](oc::string error)
    {
        result.error = oc::move(error);
        result.seconds = std::chrono::duration<double>(Clock::now() - start).count();
        return result;
    };
    if (options.outputPath.empty())
        return fail("no output path");

    const uint32 threads = options.threads ? options.threads : oc::max(1u, std::thread::hardware_concurrency());
    std::error_code ec;
    const std::filesystem::path sourceRoot = std::filesystem::weakly_canonical(toFsPath(options.sourceDir), ec);
    if (ec || !std::filesystem::is_directory(sourceRoot, ec))
        return fail("source folder " + options.sourceDir + " does not exist");
    const oc::string outputAbs = fromFsPath(std::filesystem::weakly_canonical(toFsPath(options.outputPath), ec));
    const oc::string tmpPath = options.outputPath + ".tmp";
    const oc::string tmpAbs = outputAbs + ".tmp";

    // ---- scan
    oc::vector<SourceFile> files;
    {
        ProfileScope scanScope("PackBuilder scan", EProfileCategory::File);
        auto excluded = [&](oc::string_view rel)
        {
            for (const oc::string& prefix : options.excludePrefixes)
                if (Pack::startsWithNoCase(rel, prefix))
                    return true;
            return false;
        };
        auto included = [&](oc::string_view rel)
        {
            if (options.includePrefixes.empty())
                return true;
            for (const oc::string& prefix : options.includePrefixes)
                if (Pack::startsWithNoCase(rel, prefix))
                    return true;
            return false;
        };
        // a folder worth walking: inside an included prefix, or on the way to one
        auto walkable = [&](oc::string_view dirWithSlash)
        {
            if (included(dirWithSlash))
                return true;
            for (const oc::string& prefix : options.includePrefixes)
                if (Pack::startsWithNoCase(prefix, dirWithSlash))
                    return true;
            return false;
        };
        const auto scanOptions = std::filesystem::directory_options::skip_permission_denied;
        for (auto it = std::filesystem::recursive_directory_iterator(sourceRoot, scanOptions, ec);
             !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
        {
            const std::filesystem::directory_entry& e = *it;
            const oc::string rel = fromFsPath(e.path().lexically_relative(sourceRoot));
            std::error_code statEc;
            if (e.is_directory(statEc))
            {
                const oc::string dir = rel + "/";
                if (excluded(dir) || !walkable(dir))
                    it.disable_recursion_pending();
                continue;
            }
            if (!e.is_regular_file(statEc) || excluded(rel) || !included(rel))
                continue;
            if (listedExtension(rel, options.excludeExtensions)
                || (!options.includeExtensions.empty() && !listedExtension(rel, options.includeExtensions)))
                continue;
            const oc::string abs = fromFsPath(e.path());
            if (equalsNoCase(abs, outputAbs) || equalsNoCase(abs, tmpAbs))
                continue;
            if (options.filter && !options.filter(rel))
                continue;
            if (!trieCanHold(rel))
            {
                if (++result.skipped <= 10)
                    Log::warning("Pack: '" + rel + "' left out: characters outside the path alphabet, or too long");
                continue;
            }
            SourceFile file;
            file.packPath = rel;
            file.diskPath = abs;
            file.size = e.file_size(statEc);
            file.mtimeTicks = (int64)e.last_write_time(statEc).time_since_epoch().count();
            file.rawOnly = listedExtension(rel, options.rawExtensions);
            files.push_back(oc::move(file));
        }
        if (ec)
            return fail("cannot scan " + options.sourceDir + ": " + oc::fromStd(ec.message()));
    }
    const uint32 count = (uint32)files.size();
    if (count >= oc::pathTrieDetail::MaxChildOrKey)
        return fail("too many files");

    // ---- the tries; data is written in key order, so a folder's files sit together
    oc::BitmapPathTrie fileTrie;
    oc::BitmapPathTrie dirTrie;
    uint32 dirCount = 0;
    {
        oc::unordered_set<oc::string> dirSeen;
        for (uint32 i = 0; i < count; ++i)
        {
            const oc::string& path = files[i].packPath;
            fileTrie.insert(path, i);
            for (size_t slash = path.find('/'); slash != oc::string::npos; slash = path.find('/', slash + 1))
            {
                const oc::string_view dir(path.data(), slash);
                if (dirSeen.insert(lowered(dir)).second)
                    dirTrie.insert(dir, dirCount++);
            }
        }
        if (!fileTrie.build() || !dirTrie.build())
            return fail("the path trie refused the file set (a path twice in another case?)");
    }
    const oc::vector<uint32> idOfKey = idsByKey(fileTrie, count);

    // ---- the old pack
    PackFile old;
    bool haveOld = false;
    if (options.incremental && std::filesystem::exists(toFsPath(options.outputPath), ec))
    {
        oc::string error;
        haveOld = old.open(options.outputPath, error);
        if (!haveOld)
            Log::warning("Pack: existing pack not reused (" + error + "), writing it whole");
    }

    char mountPoint[Pack::MAX_PATH_CHARS] = {};
    uint32 mountPointLength = 0;
    if (!Pack::normalizeRelative(options.mountPoint, mountPoint, mountPointLength))
        return fail("bad mount point " + options.mountPoint);

    bool tocDirty = !haveOld || old.fileCount() != count || old.header().dirCount != dirCount
        || old.header().mountPriority != options.mountPriority || strcmp(old.header().mountPoint, mountPoint) != 0;
    oc::vector<uint32> probe;
    if (haveOld)
    {
        char canonical[Pack::MAX_PATH_CHARS];
        for (uint32 i = 0; i < count; ++i)
        {
            SourceFile& file = files[i];
            file.oldKey = old.findFile(file.packPath);
            if (file.oldKey == PackFile::INVALID_KEY)
                continue;
            const Pack::Entry& e = old.entry(file.oldKey);
            // a case-only rename keeps the data, but the trie must change
            old.canonicalCase(file.packPath, oc::span<char>(canonical, file.packPath.size()));
            tocDirty |= memcmp(canonical, file.packPath.data(), file.packPath.size()) != 0;
            if (!options.rehashUnchanged && file.size == e.size && file.mtimeTicks == e.mtimeTicks)
            {
                file.state = EState::Unchanged;
                file.entry = e;
                file.hash = e.hash;
            }
            else
                probe.push_back(i);
        }
    }

    // ---- probe the files whose stamp changed: same content keeps the old bytes
    oc::atomic<uint64> bytesRead = 0;
    {
        ProfileScope probeScope("PackBuilder probe", EProfileCategory::File);
        oc::atomic<uint64> kept = 0;
        parallelFor((uint32)probe.size(), threads, [&](uint32 p)
        {
            SourceFile& file = files[probe[p]];
            const Pack::Entry& e = old.entry(file.oldKey);
            loadAndHash(file, bytesRead);
            if (file.loadFailed || (file.size == e.size && file.hash == e.hash))
            {
                // unreadable now: keep the version the pack has
                file.state = file.loadFailed ? EState::Unchanged : EState::Touched;
                file.entry = e;
                if (!file.loadFailed)
                    file.entry.mtimeTicks = file.mtimeTicks;
                file.loadFailed = false;
                file.loaded = false;
                file.data = {};
                return;
            }
            file.state = EState::Changed;
            if (kept.fetch_add(file.data.size()) + file.data.size() > KEEP_PROBED_BYTES)
            {
                file.loaded = false; // re-read in the write pass
                file.data = {};
            }
        });
    }

    oc::vector<bool> oldUsed(haveOld ? old.fileCount() : 0, false);
    uint64 reusedLive = 0;
    uint64 newBytes = 0;
    for (SourceFile& file : files)
    {
        if (file.oldKey != PackFile::INVALID_KEY)
            oldUsed[file.oldKey] = true;
        switch (file.state)
        {
        case EState::New:       ++result.added; newBytes += file.size; break;
        case EState::Changed:   ++result.updated; newBytes += file.size; break;
        case EState::Touched:   ++result.touched; reusedLive += file.entry.storedSize; break;
        case EState::Unchanged: ++result.unchanged; reusedLive += file.entry.storedSize; break;
        }
    }
    for (bool used : oldUsed)
        result.removed += used ? 0 : 1;
    result.files = count;

    if (haveOld && !tocDirty && result.added == 0 && result.updated == 0 && result.touched == 0 && result.removed == 0)
    {
        result.ok = true;
        result.upToDate = true;
        result.packSize = old.fileSize();
        result.liveBytes = old.header().liveBytes;
        result.deadBytes = old.deadBytes();
        result.bytesRead = bytesRead;
        for (const SourceFile& file : files)
            result.contentBytes += file.size;
        result.seconds = std::chrono::duration<double>(Clock::now() - start).count();
        return result;
    }

    // ---- incremental append, or a full rewrite
    bool full = !haveOld;
    uint64 appendStart = 0;
    if (haveOld)
    {
        appendStart = Pack::alignUp(old.fileSize(), Pack::SECTION_ALIGN);
        const double dataAfter = (double)(appendStart - Pack::HEADER_SIZE + newBytes);
        const double dead = (double)(appendStart - Pack::HEADER_SIZE - reusedLive);
        full = !options.incremental || (dataAfter > 0.0 && dead / dataAfter > (double)options.compactThreshold);
    }
    result.fullRewrite = full;

    const std::filesystem::path outputDir = toFsPath(options.outputPath).parent_path();
    if (!outputDir.empty())
        std::filesystem::create_directories(outputDir, ec);
    NativeFile out;
    if (!out.open(full ? tmpPath : options.outputPath, full ? NativeFile::EMode::Create : NativeFile::EMode::ReadWrite))
        return fail("cannot open " + (full ? tmpPath : options.outputPath) + " for writing");
    PackWriter writer(out, full ? Pack::HEADER_SIZE : appendStart);

    // files whose bytes get written, in key order
    oc::vector<uint32> writeOrder;
    writeOrder.reserve(count);
    for (uint32 key = 0; key < count; ++key)
    {
        const SourceFile& file = files[idOfKey[key]];
        if (full || !reuses(file))
            writeOrder.push_back(idOfKey[key]);
    }

    {
        ProfileScope writeScope("PackBuilder write", EProfileCategory::File);
        oc::vector<uint32> toLoad;
        size_t next = 0;
        while (next < writeOrder.size())
        {
            // a window of files, loaded in parallel, then written in order
            size_t end = next;
            uint64 windowBytes = 0;
            toLoad.clear();
            while (end < writeOrder.size() && toLoad.size() < WINDOW_FILES && (windowBytes < WINDOW_BYTES || toLoad.empty()))
            {
                SourceFile& file = files[writeOrder[end]];
                if (!reuses(file) && (!file.loaded || !file.compressed))
                {
                    toLoad.push_back(writeOrder[end]);
                    windowBytes += file.size;
                }
                ++end;
            }
            parallelFor((uint32)toLoad.size(), threads, [&](uint32 i) { prepare(files[toLoad[i]], options, bytesRead); });

            for (size_t w = next; w < end; ++w)
            {
                SourceFile& file = files[writeOrder[w]];
                if (file.loadFailed)
                {
                    if (file.oldKey == PackFile::INVALID_KEY)
                        return fail("cannot read " + file.diskPath);
                    Log::warning("Pack: cannot read '" + file.packPath + "', keeping the packed version");
                    file.entry = old.entry(file.oldKey);
                    file.state = EState::Unchanged;
                    if (!full)
                        continue; // the old bytes stay where they are
                }
                oc::span<const uint8> stored = reuses(file) ? old.storedBytes(file.oldKey) : oc::span<const uint8>(file.data);
                writer.padTo(Pack::dataAlignment(stored.size()));
                file.entry.offset = writer.position();
                file.entry.storedSize = stored.size();
                writer.write(stored.data(), stored.size());
                file.data = {};
                file.loaded = false;
            }
            if (!writer.ok())
                return fail("write failed (disk full?)");
            next = end;
        }
    }
    const uint64 dataEnd = writer.position();

    // ---- TOC
    Pack::Header header;
    header.flags = Pack::IMPLICIT_KEYS ? Pack::HEADER_IMPLICIT_KEYS : 0;
    header.trieNodeSize = sizeof(oc::pathTrieDetail::Node);
    header.fileCount = count;
    header.dirCount = dirCount;
    header.fileBucketKeyBits = Pack::bucketKeyBitsOf(fileTrie.storage());
    header.dirBucketKeyBits = Pack::bucketKeyBitsOf(dirTrie.storage());
    header.dataEnd = dataEnd;
    header.buildTicks = nowTicks();
    header.updateCount = full ? 0 : old.header().updateCount + 1;
    header.mountPriority = options.mountPriority;
    memcpy(header.mountPoint, mountPoint, mountPointLength);
    header.tocOffset = Pack::alignUp(dataEnd, Pack::PAGE_ALIGN);

    oc::vector<uint8> toc;
    appendTrie(toc, header.tocOffset, fileTrie.storage(), &header.sections[Pack::FILE_NODES]);
    appendTrie(toc, header.tocOffset, dirTrie.storage(), &header.sections[Pack::DIR_NODES]);
    toc.resize((size_t)Pack::alignUp(toc.size(), Pack::SECTION_ALIGN), 0);
    header.sections[Pack::ENTRIES] = { header.tocOffset + toc.size(), (uint64)count * sizeof(Pack::Entry) };
    for (uint32 key = 0; key < count; ++key)
    {
        const Pack::Entry& e = files[idOfKey[key]].entry;
        header.liveBytes += e.storedSize;
        result.contentBytes += e.size;
        const uint8* bytes = reinterpret_cast<const uint8*>(&e);
        toc.insert(toc.end(), bytes, bytes + sizeof(e));
    }
    header.tocSize = toc.size();
    header.tocHash = Pack::hashBytes(toc.data(), toc.size());

    writer.padTo(Pack::PAGE_ALIGN);
    writer.write(toc.data(), toc.size());
    // the header is the commit: everything it points at must be on disk first
    if (!writer.flush() || !out.flush())
        return fail("write failed (disk full?)");
    uint8 headerPage[Pack::HEADER_SIZE] = {};
    memcpy(headerPage, &header, sizeof(header));
    if (!out.writeAt(0, headerPage, sizeof(headerPage)) || !out.flush())
        return fail("header write failed");

    result.packSize = out.size();
    result.bytesWritten = writer.written() + sizeof(headerPage);
    result.liveBytes = header.liveBytes;
    const uint64 dataBytes = header.tocOffset - Pack::HEADER_SIZE;
    result.deadBytes = dataBytes > header.liveBytes ? dataBytes - header.liveBytes : 0;
    out.close();

    if (full)
    {
        old.close();
        if (!NativeFile::replaceFile(tmpPath, options.outputPath))
        {
            NativeFile::removeFile(tmpPath);
            return fail("cannot replace " + options.outputPath + " (mounted by a running process?)");
        }
    }
    result.bytesRead = bytesRead;
    result.ok = true;
    result.seconds = std::chrono::duration<double>(Clock::now() - start).count();
    return result;
}

bool PackBuilder::loadConfig(const oc::string& path, oc::vector<PackConfig>& out, oc::string& outError, bool allowMainThread)
{
    FileSystem::assertIoThread(allowMainThread);
    out.clear();
    AssetNode root;
    oc::string parseError;
    if (!loadAssetFile(path, root, parseError))
    {
        outError = path + ": " + parseError;
        return false;
    }
    auto appendValues = [](const AssetNode& node, oc::vector<oc::string>& list)
    {
        for (const oc::string& value : node.values)
            list.push_back(value);
    };
    for (const AssetNode& block : root.children)
    {
        if (!equalsNoCase(block.key, "Pack"))
        {
            outError = oc::format("{}: '{}' at the top level, only Pack blocks go there", path, block.key);
            return false;
        }
        PackConfig config;
        config.name = block.asString();
        PackBuildOptions& o = config.build;
        auto expand = [](const oc::string& value)
        {
            constexpr oc::string_view EXE_DIR = "$(ExeDir)";
            if (!Pack::startsWithNoCase(value, EXE_DIR))
                return value;
            return FileSystem::executableDir() + value.substr(EXE_DIR.size());
        };
        for (const AssetNode& n : block.children)
        {
            const oc::string_view key = n.key;
            if (equalsNoCase(key, "Output"))                  o.outputPath = expand(n.asString());
            else if (equalsNoCase(key, "Source"))             o.sourceDir = expand(n.asString());
            else if (equalsNoCase(key, "Include"))            appendValues(n, o.includePrefixes);
            else if (equalsNoCase(key, "Exclude"))            appendValues(n, o.excludePrefixes);
            else if (equalsNoCase(key, "IncludeExtension"))   appendValues(n, o.includeExtensions);
            else if (equalsNoCase(key, "ExcludeExtension"))   appendValues(n, o.excludeExtensions);
            else if (equalsNoCase(key, "Raw"))                appendValues(n, o.rawExtensions);
            else if (equalsNoCase(key, "Incremental"))        o.incremental = n.asBool(0, true);
            else if (equalsNoCase(key, "CompactThreshold"))   o.compactThreshold = n.asFloat(0, o.compactThreshold);
            else if (equalsNoCase(key, "Rehash"))             o.rehashUnchanged = n.asBool(0, false);
            else if (equalsNoCase(key, "MinCompressionGain")) o.minCompressionGain = n.asFloat(0, o.minCompressionGain);
            else if (equalsNoCase(key, "Threads"))            o.threads = (uint32)oc::max(n.asInt(0, 0), 0);
            else if (equalsNoCase(key, "Priority"))           o.mountPriority = n.asInt(0, o.mountPriority);
            else if (equalsNoCase(key, "MountPoint"))         o.mountPoint = n.asString();
            else if (equalsNoCase(key, "Compression"))
            {
                if (equalsNoCase(n.asString(), "Zstd"))
                    o.compression = EPackCompression::Zstd;
                else if (equalsNoCase(n.asString(), "None"))
                    o.compression = EPackCompression::None;
                else
                {
                    outError = oc::format("{}: Pack {}: Compression '{}', expected None or Zstd", path, config.name, n.asString());
                    return false;
                }
                o.compressionLevel = n.asInt(1, o.compressionLevel);
            }
            else
            {
                outError = oc::format("{}: Pack {}: unknown key '{}'", path, config.name, n.key);
                return false;
            }
        }
        if (o.outputPath.empty())
        {
            outError = oc::format("{}: Pack {} has no Output", path, config.name);
            return false;
        }
        out.push_back(oc::move(config));
    }
    return true;
}

bool PackBuilder::verify(const oc::string& packPath, oc::string& outError, bool allowMainThread)
{
    FileSystem::assertIoThread(allowMainThread);
    PackFile pack;
    return pack.open(packPath, outError) && pack.verify(outError);
}

bool PackBuilder::list(const oc::string& packPath, const oc::function<void(const PackFileInfo&)>& func,
    oc::string& outError, bool allowMainThread)
{
    FileSystem::assertIoThread(allowMainThread);
    PackFile pack;
    if (!pack.open(packPath, outError))
        return false;
    pack.files().forEach([&](const oc::BitmapPathTrieEntry& file)
    {
        const Pack::Entry& e = pack.entry(file.key);
        func(PackFileInfo{ file.path, e.size, e.storedSize, e.mtimeTicks, (e.flags & Pack::ENTRY_ZSTD) != 0 });
    });
    return true;
}
