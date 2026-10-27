export module File:PackBuilder;

import Core;

// Builds and updates .dat packs (format: File:PackFormat; mounting: FileSystem::mountPack).
//
// UPDATE, the default: an existing pack at outputPath is compared against the source folder. A file whose size and
// mtime match its entry is kept without being read; one whose stamp changed is read and hashed, and kept when the
// content is the same. Only new and changed files are written: appended behind the old data, then a new TOC, then the
// header (the commit). Removed and replaced files leave dead bytes; when they pass compactThreshold of the data, the
// pack is rewritten whole instead - unchanged files are copied over from the old pack as stored (no re-read, no
// recompression) into <outputPath>.tmp, which then replaces the pack.
//
// An update keeps working while a process has the pack mounted (that process keeps its mounted version); a full
// rewrite needs the pack unmounted everywhere, since the rename cannot replace a mapped file.
export enum class EPackCompression : uint8
{
    None,
    Zstd,
};

export struct PackBuildOptions
{
    oc::string sourceDir = ".";  // the folder packed (relative to Assets/ or absolute); pack paths are relative to it
    oc::string outputPath;       // the .dat; its folder is created when missing
    // Path prefixes, relative to sourceDir, that are packed: a folder ("Entities/") or one file. Empty = everything.
    oc::vector<oc::string> includePrefixes;
    // Path prefixes, relative to sourceDir, that are left out, case-insensitive ("Local/Scripts/" = that folder).
    // None built in: the pack config says what stays out. (The pack being written and its .tmp are always skipped.)
    oc::vector<oc::string> excludePrefixes;
    oc::vector<oc::string> excludeExtensions;
    oc::vector<oc::string> includeExtensions; // empty = every extension
    oc::function<bool(oc::string_view packPath)> filter; // optional, false = leave the file out

    bool incremental = true;        // false = always a full rewrite
    float compactThreshold = 0.25f; // dead fraction of the data region that triggers a full rewrite
    bool rehashUnchanged = false;   // also read + hash files whose size and mtime match (edits that kept the stamp)

    EPackCompression compression = EPackCompression::None;
    int32 compressionLevel = 9;
    float minCompressionGain = 0.1f; // keep the compressed bytes only when they save at least this fraction
    // Never compressed: read in ranges by the streamers (compressed entries can only be read whole) or already compressed.
    oc::vector<oc::string> rawExtensions = { ".dds", ".vsc", ".ktx", ".ktx2", ".png", ".jpg", ".jpeg", ".ogg", ".mp3", ".zip", ".zst", ".onnx" };

    uint32 threads = 0; // file reads, hashing, compression; 0 = the hardware threads

    // Stored in the header: how the pack mounts when the mounter does not say (the packs next to the executable).
    int32 mountPriority = 100;   // FileSystem::PACK_PRIORITY_DEFAULT; the loose files are 0, higher wins
    oc::string mountPoint;       // the Assets-relative folder the pack's root appears at, "" = Assets/ itself
};

export struct PackBuildResult
{
    bool ok = false;
    oc::string error;

    uint32 files = 0;     // in the pack now
    uint32 added = 0;
    uint32 updated = 0;   // content changed
    uint32 touched = 0;   // stamp changed, same content
    uint32 unchanged = 0;
    uint32 removed = 0;
    uint32 skipped = 0;   // names the path trie cannot hold (non-ASCII, too long)
    bool fullRewrite = false;
    bool upToDate = false; // nothing changed: the pack was not touched

    uint64 bytesRead = 0;    // source bytes read
    uint64 bytesWritten = 0; // pack bytes written
    uint64 packSize = 0;
    uint64 liveBytes = 0;
    uint64 deadBytes = 0;
    uint64 contentBytes = 0; // the files' uncompressed size
    double seconds = 0.0;
};

export struct PackFileInfo
{
    oc::string_view path;   // valid during the callback only
    uint64 size = 0;
    uint64 storedSize = 0;
    int64 mtimeTicks = 0;
    bool compressed = false;
};

// One `Pack` block of a pack config (Assets/dat.cfg).
export struct PackConfig
{
    oc::string name;
    PackBuildOptions build;
};

export class PackBuilder final
{
public:

    static constexpr const char* DEFAULT_CONFIG = "dat.cfg"; // Assets-relative

    static PackBuildResult build(const PackBuildOptions& options, bool allowMainThread = false);

    // Parses a pack config (grammar at the top of Assets/dat.cfg); `$(ExeDir)` in Output / Source is the folder of the
    // running executable. False with the first problem in outError: a parse error, an unknown key (a typo must not
    // silently pack the wrong set), a block without Output.
    static bool loadConfig(const oc::string& path, oc::vector<PackConfig>& out, oc::string& outError, bool allowMainThread = false);

    // Opens the pack and re-hashes every file. False with the first problem in outError.
    static bool verify(const oc::string& packPath, oc::string& outError, bool allowMainThread = false);

    // Every file in the pack, in path order. False when the pack cannot be opened.
    static bool list(const oc::string& packPath, const oc::function<void(const PackFileInfo&)>& func,
        oc::string& outError, bool allowMainThread = false);

private:
    PackBuilder() {}
};
