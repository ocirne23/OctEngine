export module App.Tools;

import Core;
import Core.Log;
import Core.BitPathTrieTest;

import File;

// One-shot tools: a command-line flag that runs one job instead of the engine and exits with its result. A new tool is
// one function plus a row in c_tools.

namespace
{
    // Core.BitPathTrie's correctness test on its generated set plus every file under Assets/.
    bool testPathTrie()
    {
        oc::vector<FileSystem::DirEntry> entries;
        FileSystem::listDirectoryRecursive(".", entries, true);
        oc::vector<oc::string> paths;
        paths.reserve(entries.size());
        for (const FileSystem::DirEntry& entry : entries)
            if (!entry.isDirectory)
                paths.push_back(FileSystem::normalize(entry.path)); // "./Entities/x.pre" -> "Entities/x.pre"
        return runBitPathTrieTest(paths);
    }

    double megabytes(uint64 bytes) { return (double)bytes / (1024.0 * 1024.0); }

    // The value after `flag` on the command line, or null (also when the next argument is another flag).
    const char* argValue(int argc, char* argv[], oc::string_view flag)
    {
        for (int i = 1; i + 1 < argc; ++i)
            if (flag == argv[i])
                return oc::startsWith(argv[i + 1], "--") ? nullptr : argv[i + 1];
        return nullptr;
    }

    bool hasArg(int argc, char* argv[], oc::string_view flag)
    {
        for (int i = 1; i < argc; ++i)
            if (flag == argv[i])
                return true;
        return false;
    }

    // The pack config: --pack-config <file>, else PackBuilder::DEFAULT_CONFIG.
    bool loadPackConfig(int argc, char* argv[], oc::vector<PackConfig>& out)
    {
        const char* configArg = argValue(argc, argv, "--pack-config");
        const oc::string configPath = configArg ? configArg : PackBuilder::DEFAULT_CONFIG;
        oc::string error;
        if (!PackBuilder::loadConfig(configPath, out, error, true))
        {
            Log::error("Pack config: " + error);
            return false;
        }
        return true;
    }

    // The command-line switches on top of a pack's options.
    void applyBuildFlags(int argc, char* argv[], PackBuildOptions& options)
    {
        if (hasArg(argc, argv, "--pack-full"))
            options.incremental = false;
        if (hasArg(argc, argv, "--pack-rehash"))
            options.rehashUnchanged = true;
        for (int i = 1; i < argc; ++i)
        {
            const oc::string_view arg = argv[i];
            if (arg == "--pack-zstd")
            {
                options.compression = EPackCompression::Zstd;
                if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9')
                    options.compressionLevel = std::atoi(argv[++i]);
            }
            else if (arg == "--pack-exclude" && i + 1 < argc)
                options.excludePrefixes.push_back(argv[++i]);
        }
    }

    bool runPackBuild(const PackBuildOptions& options);

    // --pack-build [out.dat]: builds or updates .dat packs (File:PackBuilder). Without a path, every Pack of the pack
    // config (Assets/dat.cfg, or --pack-config <file>), or only --pack-name <name>. With a path, one pack of
    // --pack-source <dir> (default Assets/) with the default options. Switches for both:
    //   --pack-full          always a full rewrite (no in-place update)
    //   --pack-zstd [level]  zstd for the files it shrinks (never .dds / .vsc: they are read in ranges)
    //   --pack-rehash        also hash files whose size + mtime match
    //   --pack-exclude <p>   leave out a path prefix (repeatable, on top of the others)
    bool buildPack(int argc, char* argv[])
    {
        FileSystem::unmountAll(); // a full rewrite cannot replace a mapped pack
        if (const char* output = argValue(argc, argv, "--pack-build"))
        {
            PackBuildOptions options;
            options.outputPath = output;
            if (const char* source = argValue(argc, argv, "--pack-source"))
                options.sourceDir = source;
            applyBuildFlags(argc, argv, options);
            return runPackBuild(options);
        }

        oc::vector<PackConfig> configs;
        if (!loadPackConfig(argc, argv, configs))
            return false;
        const char* only = argValue(argc, argv, "--pack-name");
        bool ok = true;
        uint32 built = 0;
        for (PackConfig& config : configs)
        {
            if (only && !FileSystem::pathEquals(config.name, only))
                continue;
            applyBuildFlags(argc, argv, config.build);
            Log::info("Pack " + config.name + ":");
            ok = runPackBuild(config.build) && ok;
            ++built;
        }
        if (built == 0)
        {
            Log::error(only ? oc::format("Pack config has no Pack {}", only) : oc::string("Pack config has no Pack blocks"));
            return false;
        }
        return ok;
    }

    bool runPackBuild(const PackBuildOptions& options)
    {
        const PackBuildResult r = PackBuilder::build(options, true);
        if (!r.ok)
        {
            Log::error("Pack build failed: " + r.error);
            return false;
        }
        if (r.upToDate)
            Log::info(oc::format("Pack {} is up to date: {} files, {:.1f} s", options.outputPath, r.files, r.seconds));
        else
            Log::info(oc::format("Pack {} {}: {} files ({} added, {} updated, {} touched, {} unchanged, {} removed, {} skipped), "
                "read {:.1f} MB, wrote {:.1f} MB, {:.1f} s",
                options.outputPath, r.fullRewrite ? "written whole" : "updated in place", r.files, r.added, r.updated, r.touched,
                r.unchanged, r.removed, r.skipped, megabytes(r.bytesRead), megabytes(r.bytesWritten), r.seconds));
        Log::info(oc::format("Pack size {:.1f} MB: {:.1f} MB content stored as {:.1f} MB, {:.1f} MB dead",
            megabytes(r.packSize), megabytes(r.contentBytes), megabytes(r.liveBytes), megabytes(r.deadBytes)));
        return true;
    }

    // --pack-verify <pack.dat>: re-hashes every file in the pack.
    bool verifyPack(int argc, char* argv[])
    {
        const char* path = argValue(argc, argv, "--pack-verify");
        oc::string error;
        if (!path || !PackBuilder::verify(path, error, true))
        {
            Log::error("Pack verify failed: " + (path ? error : oc::string("no pack path")));
            return false;
        }
        Log::info(oc::format("Pack {} verified", path));
        return true;
    }

    // --pack-list <pack.dat>: every file with its size, stored size and compression.
    bool listPack(int argc, char* argv[])
    {
        const char* path = argValue(argc, argv, "--pack-list");
        oc::string error;
        uint32 files = 0;
        uint64 bytes = 0;
        const bool ok = path && PackBuilder::list(path, [&](const PackFileInfo& file)
        {
            ++files;
            bytes += file.size;
            Log::info(oc::format("{:>12} {:>12} {} {}", file.size, file.storedSize, file.compressed ? "z" : " ", file.path));
        }, error, true);
        if (!ok)
        {
            Log::error("Pack list failed: " + (path ? error : oc::string("no pack path")));
            return false;
        }
        Log::info(oc::format("{} files, {:.1f} MB", files, megabytes(bytes)));
        return true;
    }

    // --pack-check <pack.dat> [--pack-mount <folder>]: mounts the pack alone (no loose files) and reads every file back
    // through FileSystem - whole, through a FileReader in ranges, the queries, and the merged listing.
    bool checkPack(int argc, char* argv[])
    {
        const char* path = argValue(argc, argv, "--pack-check");
        const char* mountArg = argValue(argc, argv, "--pack-mount");
        const oc::string mountPoint = mountArg ? mountArg : "";
        const oc::string prefix = mountPoint.empty() ? oc::string() : FileSystem::normalize(mountPoint) + "/";
        oc::vector<oc::pair<oc::string, uint64>> files;
        oc::string error;
        if (!path || !PackBuilder::list(path, [&](const PackFileInfo& file) { files.push_back({ prefix + oc::string(file.path), file.size }); }, error, true))
        {
            Log::error("Pack check failed: " + (path ? error : oc::string("no pack path")));
            return false;
        }
        FileSystem::unmountAll();
        if (!FileSystem::mountPack(path, FileSystem::PACK_PRIORITY_DEFAULT, mountPoint, true))
            return false;
        FileSystem::setDiskEnabled(false);

        uint32 failures = 0;
        auto fail = [&](const oc::string& what)
        {
            if (++failures <= 20)
                Log::error("Pack check: " + what);
        };
        oc::vector<uint8> whole;
        oc::vector<uint8> ranged;
        oc::unordered_set<oc::string> dirs;
        for (const auto& [file, size] : files)
        {
            if (!FileSystem::exists(file, true) || !FileSystem::isRegularFile(file, true) || FileSystem::isDirectory(file, true))
                fail(file + ": queries");
            FileSystem::FileStat stat;
            if (!FileSystem::stat(file, stat, true) || stat.size != size || stat.isDirectory || FileSystem::fileSize(file, true) != size)
                fail(file + ": stat");
            if (FileSystem::sourceOf(file, nullptr, true) != FileSystem::ESource::Pack)
                fail(file + ": not served by the pack");
            if (!FileSystem::readFileBytes(file, whole, true) || whole.size() != size)
            {
                fail(file + ": readFileBytes");
                continue;
            }
            if (FileSystem::readFileStr(file, true).size() != size)
                fail(file + ": readFileStr");
            FileReader reader;
            if (!reader.open(file, true) || reader.size() != size)
            {
                fail(file + ": FileReader::open");
                continue;
            }
            // three ranges: the head, the middle, the tail
            ranged.resize(whole.size());
            const uint64 third = size / 3;
            const bool rangesOk = reader.read(0, ranged.data(), third) && reader.read(third, ranged.data() + third, third)
                && reader.read(2 * third, ranged.data() + 2 * third, size - 2 * third);
            if (!rangesOk || memcmp(ranged.data(), whole.data(), whole.size()) != 0)
                fail(file + ": FileReader ranges differ from the whole read");
            if (reader.read(size, ranged.data(), 1))
                fail(file + ": FileReader read past the end");
            for (size_t slash = file.find('/'); slash != oc::string::npos; slash = file.find('/', slash + 1))
                dirs.insert(file.substr(0, slash));
        }
        for (const oc::string& dir : dirs)
            if (!FileSystem::isDirectory(dir, true) || !FileSystem::exists(dir, true))
                fail(dir + ": not a directory");

        oc::vector<FileSystem::DirEntry> listed;
        const oc::string listRoot = mountPoint.empty() ? oc::string(".") : mountPoint;
        FileSystem::listDirectoryRecursive(listRoot, listed, true);
        uint32 listedFiles = 0;
        for (const FileSystem::DirEntry& entry : listed)
            listedFiles += entry.isDirectory ? 0 : 1;
        if (listedFiles != files.size())
            fail(oc::format("recursive listing of {} has {} files, the pack {}", listRoot, listedFiles, files.size()));
        if (FileSystem::exists("does/not/exist.txt", true))
            fail("a missing file exists");

        FileSystem::setDiskEnabled(true);
        FileSystem::unmountAll();
        if (failures == 0)
            Log::info(oc::format("Pack check: PASS, {} files, {} folders, {} listed entries", files.size(), dirs.size(), listed.size()));
        else
            Log::error(oc::format("Pack check: FAIL, {} failures", failures));
        return failures == 0;
    }

    struct Tool
    {
        oc::string_view flag;
        bool (*run)(int argc, char* argv[]); // true = success
    };

    constexpr Tool c_tools[] =
    {
        { "--test-path-trie", [](int, char*[]) { return testPathTrie(); } },
        { "--pack-build", buildPack },
        { "--pack-verify", verifyPack },
        { "--pack-list", listPack },
        { "--pack-check", checkPack },
    };
}

// --pack <path>[@priority] (repeatable; without @, the priority the pack was built with), --packs (every .dat next to
// the executable, as FileSystem::initialize does by itself when there is no Assets/), --pack-only (no loose files),
// --disk-priority <n>. main calls it right after FileSystem::initialize, before the first asset or settings read.
export void mountCommandLinePacks(int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i)
    {
        const oc::string_view arg = argv[i];
        if (arg == "--packs")
        {
            const oc::string dir = FileSystem::executableDir();
            if (FileSystem::mountPacksIn(dir, true) == 0)
                Log::warning("--packs: no .dat next to the executable (" + dir + "), build them with --pack-build");
        }
        else if (arg == "--pack" && i + 1 < argc)
        {
            oc::string path = argv[++i];
            oc::optional<int32> priority;
            const size_t at = path.rfind('@');
            if (at != oc::string::npos && at + 1 < path.size())
            {
                priority = std::atoi(path.c_str() + at + 1);
                path.resize(at);
            }
            FileSystem::mountPack(path, priority, {}, true);
        }
        else if (arg == "--pack-only")
            FileSystem::setDiskEnabled(false);
        else if (arg == "--disk-priority" && i + 1 < argc)
            FileSystem::setDiskPriority(std::atoi(argv[++i]));
    }
}

// Runs the tool whose flag is on the command line and exits with 0 (success) or 1. Returns when there is none.
// main calls it after FileSystem::initialize and before anything else initializes, so the exit skips the global
// destructors: they assume a live renderer (~StagingManager destroys its fences on the device).
export void runCommandLineTool(int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i)
    {
        for (const Tool& tool : c_tools)
        {
            if (tool.flag != argv[i])
                continue;
            const bool succeeded = tool.run(argc, argv);
            fflush(nullptr);
            _Exit(succeeded ? 0 : 1);
        }
    }
}
