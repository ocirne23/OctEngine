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

    struct Tool
    {
        oc::string_view flag;
        bool (*run)(); // true = success
    };

    constexpr Tool c_tools[] =
    {
        { "--test-path-trie", testPathTrie },
    };
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
            const bool succeeded = tool.run();
            fflush(nullptr);
            _Exit(succeeded ? 0 : 1);
        }
    }
}
