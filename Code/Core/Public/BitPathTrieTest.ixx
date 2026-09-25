export module Core.BitPathTrieTest;

import Core;
import Core.Log;
import Core.BitPathTrie;

// Correctness test for Core.BitPathTrie: builds tries and checks that every inserted path is found, in every case
// spelling, with the right key and canonical case, and that iteration reports every key once. Runs on a generated set
// (every node kind, case exceptions, escaped bucket counts, split buckets) and on caller supplied paths (App passes the
// Assets/ file list, --test-path-trie). Logs a size / layout / timing report per set; time it in RelWithDebInfo.

export bool runBitPathTrieTest(oc::span<const oc::string> extraPaths);

namespace
{
    using oc::BitmapPathTrie;
    using oc::BitmapPathTrieEntry;
    using oc::BitmapPathTrieStats;
    using oc::BitmapPathTrieView;
    namespace pathTrieDetail = oc::pathTrieDetail;

    constexpr uint32 c_lookupRepeats = 4;
    constexpr uint32 c_maxLookupSamples = 100'000;

    struct TestContext
    {
        oc::string setName;
        uint32 failures = 0;

        void fail(const oc::string& what)
        {
            if (++failures <= 20)
                Log::error("[BitPathTrie " + setName + "] " + what);
        }

        void info(const oc::string& text) const
        {
            Log::info("[BitPathTrie " + setName + "] " + text);
        }
    };

    char lowerChar(char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; }
    char upperChar(char c) { return c >= 'a' && c <= 'z' ? char(c - 'a' + 'A') : c; }

    oc::string lowered(oc::string_view text)
    {
        oc::string out(text.data(), text.size());
        for (char& c : out)
            c = lowerChar(c);
        return out;
    }

    oc::string uppered(oc::string_view text)
    {
        oc::string out(text.data(), text.size());
        for (char& c : out)
            c = upperChar(c);
        return out;
    }

    bool lessNoCase(oc::string_view a, oc::string_view b)
    {
        const size_t common = oc::min(a.size(), b.size());
        for (size_t i = 0; i < common; ++i)
        {
            const uint8 x = uint8(lowerChar(a[i]));
            const uint8 y = uint8(lowerChar(b[i]));
            if (x != y)
                return x < y;
        }
        return a.size() < b.size();
    }

    double megabytes(size_t bytes) { return double(bytes) / (1024.0 * 1024.0); }
    double percent(size_t part, size_t whole) { return whole ? 100.0 * double(part) / double(whole) : 0.0; }
    double average(size_t sum, size_t count) { return count ? double(sum) / double(count) : 0.0; }
    double secondsSince(Clock::time_point start) { return std::chrono::duration<double>(Clock::now() - start).count(); }

    // OC_PATH_TRIE_IMPLICIT_KEYS is local to Core.BitPathTrie, so the test asks the type instead.
    template<typename Trie>
    constexpr bool c_implicitKeys = requires(const Trie& trie) { trie.orderedKeys(); };

    // key -> index into the accepted paths (the id passed to insert()). Implicit keys map through orderedKeys().
    template<typename Trie>
    oc::vector<uint32> idsByKey(const Trie& trie, uint32 count)
    {
        if constexpr (c_implicitKeys<Trie>)
            return trie.orderedKeys();
        else
        {
            oc::vector<uint32> ids(count);
            for (uint32 i = 0; i < count; ++i)
                ids[i] = i;
            return ids;
        }
    }

    // The bucket key array only exists with stored keys (OC_PATH_TRIE_IMPLICIT_KEYS off).
    template<typename Storage>
    size_t bucketKeyBytes(const Storage& storage)
    {
        if constexpr (requires { storage.bucketKeys; })
            return storage.bucketKeys.size_bytes();
        else
            return 0;
    }

    // Deterministic generator (a fixed LCG), so a failure reproduces.
    struct Random
    {
        uint64 state = 0x2545'f491'4f6c'dd1dull;

        uint32 next()
        {
            state = state * 6364136223846793005ull + 1442695040888963407ull;
            return uint32(state >> 33);
        }
        uint32 below(uint32 bound) { return next() % bound; }
    };

    oc::string randomName(Random& random, uint32 length, bool capitalize)
    {
        static constexpr char c_chars[] = "abcdefghijklmnopqrstuvwxyz0123456789._- ()[]{}+&~$#@!',;=%^`";
        oc::string name;
        for (uint32 i = 0; i < length; ++i)
        {
            char c = c_chars[random.below(sizeof(c_chars) - 1)];
            if (capitalize && (i == 0 || random.below(8) == 0))
                c = upperChar(c);
            name += c;
        }
        return name;
    }

    // Every node kind and bucket layout the trie has: shared directory prefixes, prefix keys (a.txt / a.txt.bin),
    // inline and tail leaves, buckets up to MaxBucketKeys and past it, long entries that need escaped counts, split
    // entries, uppercase with and without case exceptions, paths at the length limit.
    oc::vector<oc::string> generatePaths()
    {
        Random random;
        oc::vector<oc::string> paths;
        const uint32 maxLength = pathTrieDetail::MaxPath - 1;

        paths.push_back("a");
        paths.push_back("a.txt");
        paths.push_back("a.txt.bin");
        paths.push_back("a.txt.bin.old");
        paths.push_back("ab");
        paths.push_back("Textures/Stone/Albedo.dds");
        paths.push_back("textures/stone/normal.dds"); // shares the "textures/stone/" nodes in the other case: a case exception
        paths.push_back("TEXTURES/STONE/rough.dds");
        paths.push_back(oc::string(maxLength, 'x'));
        paths.push_back(oc::string(maxLength - 1, 'x') + "Y");

        oc::vector<oc::string> directories = { "" };
        for (uint32 d = 0; d < 400; ++d)
        {
            const oc::string& parent = directories[random.below(uint32(directories.size()))];
            const bool capitalize = random.below(3) == 0;
            oc::string directory = parent + randomName(random, 1 + random.below(random.below(4) == 0 ? 70 : 12), capitalize) + "/";
            if (directory.size() > 300)
                continue;
            directories.push_back(directory);

            // file counts around the bucket limit, plus plenty of single files (leaves)
            const uint32 shape = random.below(10);
            const uint32 fileCount = shape == 0 ? 100 + random.below(200) : shape < 4 ? 1 : 1 + random.below(40);
            const oc::string stem = randomName(random, random.below(6), capitalize);
            for (uint32 f = 0; f < fileCount; ++f)
            {
                // long names (80+ new symbols) need escaped bucket counts; shared stems make front coding matter
                const uint32 nameLength = random.below(12) == 0 ? 60 + random.below(150) : 1 + random.below(16);
                oc::string path = directory + (random.below(2) == 0 ? stem : oc::string()) + randomName(random, nameLength, capitalize);
                if (path.size() < pathTrieDetail::MaxPath)
                    paths.push_back(oc::move(path));
            }
        }
        return paths;
    }

    // The directory entries and the file count a non recursive forEachChild(prefix) has to report.
    void expectedChildren(const oc::vector<oc::string>& lowerPaths, const oc::string& lowerPrefix, oc::unordered_set<oc::string>& outDirectories, uint32& outFiles)
    {
        outDirectories.clear();
        outFiles = 0;
        for (const oc::string& path : lowerPaths)
        {
            if (!oc::startsWith(path, lowerPrefix))
                continue;
            const size_t slash = path.find('/', lowerPrefix.size());
            if (slash == oc::string::npos)
                ++outFiles;
            else
                outDirectories.insert(path.substr(0, slash));
        }
    }

    // One path: every case spelling, the canonical case, near misses. False on any mismatch.
    bool checkLookup(TestContext& context, const BitmapPathTrieView& view, const oc::unordered_set<oc::string>& lowerSet, const oc::string& path, uint32 expected)
    {
        const uint32 failuresBefore = context.failures;
        char canonical[pathTrieDetail::MaxPath];

        if (const uint32 key = view.find(path); key != expected)
            context.fail(oc::format("find(\"{}\") = {}, expected {}", path, key, expected));

        if (const uint32 key = view.find(path, true); key != expected)
            context.fail(oc::format("case sensitive find(\"{}\") = {}, expected {}", path, key, expected));

        const uint32 canonicalKey = view.find(path, false, oc::span<char>(canonical, path.size()));
        if (canonicalKey != expected || memcmp(canonical, path.data(), path.size()) != 0)
            context.fail(oc::format("canonical find(\"{}\") = {} \"{}\"", path, canonicalKey, oc::string_view(canonical, path.size())));

        for (const oc::string& variant : { lowered(path), uppered(path) })
        {
            if (view.find(variant) != expected)
                context.fail(oc::format("find(\"{}\") misses \"{}\"", variant, path));
            if (variant != path && view.find(variant, true) != BitmapPathTrieView::InvalidKey)
                context.fail(oc::format("case sensitive find(\"{}\") matches \"{}\"", variant, path));
        }

        // one symbol more or less is a different path, found only if it was inserted as well
        for (const oc::string& nearMiss : { path + "0", path + "/", path.substr(0, path.size() - 1) })
            if (!lowerSet.count(lowered(nearMiss)) && view.find(nearMiss) != BitmapPathTrieView::InvalidKey)
                context.fail(oc::format("find(\"{}\") matches, but only \"{}\" was inserted", nearMiss, path));

        return context.failures == failuresBefore;
    }

    // forEachChild on the root and a spread of directories, both modes, against a brute force count.
    void checkChildren(TestContext& context, const BitmapPathTrieView& view, const oc::vector<oc::string>& paths)
    {
        const uint32 count = uint32(paths.size());
        oc::vector<oc::string> lowerPaths;
        lowerPaths.reserve(count);
        for (const oc::string& path : paths)
            lowerPaths.push_back(lowered(path));

        oc::vector<oc::string> prefixes = { "" };
        for (uint32 id = 0; id < count; id += oc::max(count / 40, 1u))
        {
            const size_t slash = paths[id].rfind('/');
            if (slash != oc::string::npos)
                prefixes.push_back(paths[id].substr(0, slash + 1));
        }

        oc::unordered_set<oc::string> expectedDirectories;
        for (const oc::string& prefix : prefixes)
        {
            const oc::string lowerPrefix = lowered(prefix);

            uint32 expectedRecursive = 0;
            for (const oc::string& path : lowerPaths)
                expectedRecursive += oc::startsWith(path, lowerPrefix) ? 1 : 0;

            uint32 recursive = 0;
            view.forEachChild(prefix, true, [&](const BitmapPathTrieEntry& entry)
            {
                ++recursive;
                if (entry.isDirectory || !oc::startsWith(lowered(entry.path), lowerPrefix))
                    context.fail(oc::format("forEachChild(\"{}\") reported \"{}\"", prefix, entry.path));
            });
            if (recursive != expectedRecursive)
                context.fail(oc::format("forEachChild(\"{}\", recursive) reported {} keys, expected {}", prefix, recursive, expectedRecursive));

            uint32 expectedFiles = 0;
            expectedChildren(lowerPaths, lowerPrefix, expectedDirectories, expectedFiles);
            uint32 files = 0;
            uint32 directories = 0;
            view.forEachChild(prefix, false, [&](const BitmapPathTrieEntry& entry)
            {
                if (!entry.isDirectory)
                {
                    ++files;
                    return;
                }
                ++directories;
                if (!expectedDirectories.count(lowered(entry.path)))
                    context.fail(oc::format("forEachChild(\"{}\") reported directory \"{}\"", prefix, entry.path));
            });
            if (files != expectedFiles || directories != expectedDirectories.size())
                context.fail(oc::format("forEachChild(\"{}\") reported {} files + {} directories, expected {} + {}",
                    prefix, files, directories, expectedFiles, expectedDirectories.size()));
        }
    }

    // The path set itself: lengths, file names, directory depth.
    void reportPaths(const TestContext& context, const oc::vector<oc::string>& paths, size_t trieBytes)
    {
        size_t totalLength = 0;
        size_t maxLength = 0;
        size_t fileNameLength = 0;
        size_t totalDepth = 0;
        size_t maxDepth = 0;
        size_t lengthBands[5] = {}; // < 32, 32-63, 64-127, 128-255, >= 256
        for (const oc::string& path : paths)
        {
            const size_t length = path.size();
            totalLength += length;
            maxLength = oc::max(maxLength, length);
            ++lengthBands[length < 32 ? 0 : length < 64 ? 1 : length < 128 ? 2 : length < 256 ? 3 : 4];

            const size_t slash = path.rfind('/');
            fileNameLength += slash == oc::string::npos ? length : length - slash - 1;
            const size_t depth = size_t(oc::count(path.begin(), path.end(), '/'));
            totalDepth += depth;
            maxDepth = oc::max(maxDepth, depth);
        }

        const size_t count = paths.size();
        context.info(oc::format("path length: avg {:.1f}, max {} characters, {:.2f} MB raw ({:.2f} bits per character in the bitmap trie), "
            "file name avg {:.1f} characters, directory depth avg {:.1f}, max {}",
            average(totalLength, count), maxLength, megabytes(totalLength), totalLength ? 8.0 * double(trieBytes) / double(totalLength) : 0.0,
            average(fileNameLength, count), average(totalDepth, count), maxDepth));
        context.info(oc::format("path lengths: < 32: {}, 32-63: {}, 64-127: {}, 128-255: {}, >= 256: {}",
            lengthBands[0], lengthBands[1], lengthBands[2], lengthBands[3], lengthBands[4]));
    }

    struct LookupBench
    {
        size_t sortedBytes = 0;
        uint32 samples = 0;
        double sortedNs = 0.0; // per lookup
        double trieNs = 0.0;
    };

    volatile uint64 g_lookupSink = 0; // keeps the timed loops from being optimized away

    // Lookup speed against the plain alternative: a sorted (path, key) array searched with a case insensitive
    // lower_bound. Also its memory: the array, plus every path that does not fit the string's inline buffer.
    LookupBench benchLookups(const BitmapPathTrieView& view, const oc::vector<oc::string>& paths)
    {
        LookupBench bench;
        const uint32 count = uint32(paths.size());
        if (count == 0)
            return bench;

        oc::vector<oc::pair<oc::string, uint32>> sorted;
        sorted.reserve(count);
        for (uint32 id = 0; id < count; ++id)
            sorted.push_back({ paths[id], id });
        oc::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return lessNoCase(a.first, b.first); });

        bench.sortedBytes = sorted.capacity() * sizeof(sorted[0]);
        for (const auto& entry : sorted)
        {
            const char* data = entry.first.data();
            const bool inlineBuffer = data >= reinterpret_cast<const char*>(&entry.first) && data < reinterpret_cast<const char*>(&entry.first + 1);
            bench.sortedBytes += inlineBuffer ? 0 : entry.first.capacity() + 1;
        }

        Random random;
        oc::vector<oc::string_view> queries;
        bench.samples = oc::min(count, c_maxLookupSamples);
        queries.reserve(bench.samples);
        for (uint32 i = 0; i < bench.samples; ++i)
            queries.push_back(paths[random.below(count)]);

        uint64 checksum = 0;
        const Clock::time_point trieStart = Clock::now();
        for (uint32 repeat = 0; repeat < c_lookupRepeats; ++repeat)
            for (const oc::string_view query : queries)
                checksum += view.find(query);
        const double trieSec = secondsSince(trieStart);

        const Clock::time_point sortedStart = Clock::now();
        for (uint32 repeat = 0; repeat < c_lookupRepeats; ++repeat)
        {
            for (const oc::string_view query : queries)
            {
                const auto found = oc::lower_bound(sorted.begin(), sorted.end(), query,
                    [](const auto& entry, oc::string_view value) { return lessNoCase(entry.first, value); });
                checksum += found != sorted.end() && !lessNoCase(query, found->first) ? found->second : BitmapPathTrieView::InvalidKey;
            }
        }
        const double sortedSec = secondsSince(sortedStart);
        g_lookupSink = g_lookupSink + checksum;

        const double lookups = double(bench.samples) * c_lookupRepeats;
        bench.sortedNs = sortedSec * 1e9 / lookups;
        bench.trieNs = trieSec * 1e9 / lookups;
        return bench;
    }

    // Sizes against the sorted array, the trie's memory, lookup speed, and its layout from its stats.
    void reportLayout(const TestContext& context, const BitmapPathTrieView& view, size_t count, double buildSec, const LookupBench& bench)
    {
        const BitmapPathTrieStats stats = view.stats();
        const oc::BitmapPathTrieStorage& storage = view.storage();
        const size_t trieBytes = view.memoryUsage();

        context.info(oc::format("sorted array: {:8.2f} MB ({:5.1f} bytes/path)", megabytes(bench.sortedBytes), average(bench.sortedBytes, count)));
        context.info(oc::format("bitmap trie:  {:8.2f} MB ({:5.1f} bytes/path), buckets <= {} keys, {} case exceptions, built in {:.2f} s",
            megabytes(trieBytes), average(trieBytes, count), BitmapPathTrie::MaxBucketKeys, stats.caseExceptionKeys, buildSec));
        context.info(oc::format("memory: nodes {:.2f} MB, symbol pool {:.2f} MB (+ {:.2f} MB case bits), case exceptions {:.2f} MB ({})",
            megabytes(storage.nodes.size_bytes()), megabytes(storage.poolSymbols.size_bytes()), megabytes(storage.poolCase.size_bytes()),
            megabytes(storage.caseExceptions.size_bytes() + storage.casePositions.size_bytes()),
            c_implicitKeys<BitmapPathTrie> ? oc::string("keys are implicit") : oc::format("bucket keys {:.2f} MB", megabytes(bucketKeyBytes(storage)))));
        context.info(oc::format("lookup ({} random paths x {}): sorted array {:.0f} ns, bitmap trie {:.0f} ns",
            bench.samples, c_lookupRepeats, bench.sortedNs, bench.trieNs));

        const size_t nodes = view.nodeCount();
        const size_t leaves = stats.leafNodes;
        context.info(oc::format("nodes: {} total, {} branch ({:.1f}%), {} chain ({:.1f}%), {} bucket ({:.1f}%), {} leaf ({:.1f}%)",
            nodes, stats.branchNodes, percent(stats.branchNodes, nodes), stats.chainNodes, percent(stats.chainNodes, nodes),
            stats.bucketNodes, percent(stats.bucketNodes, nodes), leaves, percent(leaves, nodes)));
        context.info(oc::format("buckets: {} keys, avg {:.1f} keys per bucket, avg {:.1f} pool symbols per key ({:.2f} header, split per bucket), {:.1f}% with a split entry",
            stats.bucketKeys, average(stats.bucketKeys, stats.bucketNodes), average(stats.bucketSymbols, stats.bucketKeys),
            average(stats.bucketHeaderSymbols, stats.bucketKeys), percent(stats.splitBucketNodes, stats.bucketNodes)));
        context.info(oc::format("leaves: {} end-of-key, {} with tail (avg {:.1f} tail symbols), inline leaves avg {:.1f} / {} symbols",
            stats.endLeafNodes, stats.tailLeafNodes, average(stats.tailSymbols, stats.tailLeafNodes),
            average(stats.inlineLeafSymbols, leaves - stats.tailLeafNodes), BitmapPathTrie::Node::LabelSymbols + BitmapPathTrie::Node::InlineLeaf::MaxSymbols));
        context.info(oc::format("children per branch node: 2: {}, 3-4: {}, 5-8: {}, 9-16: {}, 17-32: {}, 33-64: {}",
            stats.branchChildCounts[1], stats.branchChildCounts[2], stats.branchChildCounts[3],
            stats.branchChildCounts[4], stats.branchChildCounts[5], stats.branchChildCounts[6]));
        context.info(oc::format("lookup depth: avg {:.1f} nodes, max {}", average(stats.keyDepthSum, stats.bucketKeys + leaves), stats.maxKeyDepth));
    }

    void checkSet(TestContext& context, const oc::vector<oc::string>& candidates)
    {
        BitmapPathTrie trie;
        oc::vector<oc::string> paths;
        oc::unordered_set<oc::string> lowerSet;
        uint32 refused = 0;
        bool hasUpper = false;
        for (const oc::string& candidate : candidates)
        {
            if (!lowerSet.insert(lowered(candidate)).second)
                continue; // the same path in another case: build() would refuse the whole set
            if (!trie.insert(candidate, uint32(paths.size())))
            {
                ++refused;
                continue;
            }
            paths.push_back(candidate);
            hasUpper = hasUpper || lowered(candidate) != candidate;
        }

        const Clock::time_point buildStart = Clock::now();
        if (!trie.build())
        {
            context.fail("build() failed");
            return;
        }
        const double buildSec = secondsSince(buildStart);

        const uint32 count = uint32(paths.size());
        const oc::vector<uint32> ids = idsByKey(trie, count);
        oc::vector<uint32> keyOfId(count, BitmapPathTrieView::InvalidKey);
        for (uint32 key = 0; key < ids.size(); ++key)
            keyOfId[ids[key]] = key;

        const BitmapPathTrieView view = trie.view();

        uint32 lookupMismatches = 0;
        for (uint32 id = 0; id < count; ++id)
            lookupMismatches += checkLookup(context, view, lowerSet, paths[id], keyOfId[id]) ? 0 : 1;

        // iteration: every key once, with the canonical path
        oc::vector<bool> seen(count, false);
        uint32 iterated = 0;
        uint32 iterationMismatches = 0;
        view.forEach([&](const BitmapPathTrieEntry& entry)
        {
            ++iterated;
            if (entry.isDirectory || entry.key >= count || seen[entry.key])
            {
                ++iterationMismatches;
                context.fail(oc::format("forEach reported \"{}\" key {}", entry.path, entry.key));
                return;
            }
            seen[entry.key] = true;
            const oc::string& path = paths[ids[entry.key]];
            if (entry.path != oc::string_view(path))
            {
                ++iterationMismatches;
                context.fail(oc::format("forEach key {} path \"{}\", expected \"{}\"", entry.key, entry.path, path));
            }
        });
        if (iterated != count)
            context.fail(oc::format("forEach visited {} of {} keys", iterated, count));

        checkChildren(context, view, paths);

        context.info(oc::format("{} paths ({} trie), {} not insertable, {} lookup mismatches, {} iterated ({} mismatches)",
            count, hasUpper ? "mixed case" : "lowercase", refused, lookupMismatches, iterated, iterationMismatches));
        reportPaths(context, paths, view.memoryUsage());
        reportLayout(context, view, count, buildSec, benchLookups(view, paths));
    }

    // insert() and build() refusals, and lookups on an empty trie.
    void checkRefusals(TestContext& context)
    {
        BitmapPathTrie trie;
        if (trie.insert("bad\\path", 0) || trie.insert("tab\tpath", 0) || trie.insert(oc::string(pathTrieDetail::MaxPath, 'x'), 0))
            context.fail("insert() took a path with a character outside the alphabet, or a path of MaxPath characters");
        if (!trie.build() || trie.view().find("a") != BitmapPathTrieView::InvalidKey || !trie.view().empty())
            context.fail("an empty trie did not build empty");

        trie.clear();
        trie.insert("Dir/File", 0);
        trie.insert("dir/file", 1);
        if (trie.build())
            context.fail("build() took the same path twice in another case");
    }
}

bool runBitPathTrieTest(oc::span<const oc::string> extraPaths)
{
    uint32 failures = 0;

    TestContext refusals{ "refusals" };
    checkRefusals(refusals);
    failures += refusals.failures;

    TestContext generated{ "generated" };
    checkSet(generated, generatePaths());
    failures += generated.failures;

    if (!extraPaths.empty())
    {
        TestContext extra{ "supplied" };
        checkSet(extra, oc::vector<oc::string>(extraPaths.begin(), extraPaths.end()));
        failures += extra.failures;
    }

    if (failures == 0)
        Log::info("BitPathTrie test: PASS");
    else
        Log::error(oc::format("BitPathTrie test: FAIL, {} failures", failures));
    return failures == 0;
}
