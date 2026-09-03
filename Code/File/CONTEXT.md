# File

> Library documentation for `Code/File`.
> Read [`.claude/CLAUDE.md`](../../.claude/CLAUDE.md) first — rules, building, style, dependency
> direction. The asset text formats are listed there under **Asset files**.

Asset and filesystem functionality.

## Scene loading

* `ISceneData::createAssimpLoader()` — model files.
* `ISceneData::createProceduralLoader()` — generated geometry (terrain, skysphere, debug shapes).

## Cooked scene cache

`ISceneData::loadCached(path, mergeNodes, preTransform, SceneCookOptions)` — World uses it for every
non-procedural container.

* Serves a binary snapshot at `Assets/Local/Cooked/<stem>_<hash>.vsc`
  (reader: `File:CookedSceneData`; writer: SceneCooker.cpp).
* Validated by magic/version, source mtime + size, options hash, and per-converted-texture stamps.
  Any mismatch re-imports and re-cooks in place.
* Cooking bakes meshopt LOD chains, so changing a LOD tweak recooks on the next start.
* Cooking converts non-DDS textures — loose AND embedded — to BC1 / BC3 (alpha) / BC5 (normals)
  `.dds` so they mip-stream. Alpha-masked diffuse mips are alpha-coverage-rescaled.
* Skinned/animated scenes always bypass the cache.
* Delete `Assets/Local/Cooked/` to force a recook.

## `AssetParser` (`File:AssetParser`)

The text format behind all engine text assets.

* Indentation-defined hierarchy of `AssetNode` (key + values + children), `#` and `//` comments,
  quoted strings, comma vectors.
* `loadAssetFile` / `parseAssetText` / `writeAssetText`, case-insensitive `find`, typed accessors.
* `File:ObjectDescription` parses `.oc` into `ObjectContainerDesc` (`loadObjectContainerDesc`). It
  lives here so Procedural's scatter assets import through `.oc` too.

## `FileSystem` (`File:FileSystem`)

**THE disk seam** — the ONLY code that includes `<filesystem>` / `<fstream>`, which Core stopped
exporting. Paths are plain UTF-8 `std::string`; no `std::filesystem::path` reaches any consumer.

| Group | API |
|---|---|
| Reads / writes | `readFileStr`, `readFileBytes`, `writeFileStr`, `writeFileBytes` |
| Queries + mutations | `exists`, `isDirectory`, `isRegularFile`, `fileSize`, `lastWriteTimeSec`, `copyLastWriteTime`, `createDirectories`, `remove`, `removeAll`, `rename`, `copyFile`, `listDirectory` (+`Recursive`, returns `DirEntry{path,name,extension,isDirectory,size}`), `currentPath`, `setCurrentPath`, `absolutePath`, `canonicalPath`, `weaklyCanonicalPath`, `relativePath` |
| Pure PATH MATH — touches no disk, carries no thread rule | `join`, `parentPath`, `filename`, `stem`, `extension`, `replaceExtension`, `normalize`, `isAbsolute`, `pathEquals` |

`FileSystem::initialize()` sets the working directory to `Assets/` and records the main thread.

### Main-thread IO assert

Every FileSystem IO call asserts when it runs on the main thread — the thread that called
`initialize()` — because disk latency there is a frame hitch.

* Intentional main-thread IO must say so, either with the RAII `FileSystem::AllowMainThreadIO` scope
  (thread_local, nestable; do NOT hold one across a fiber wait — `/GT`) or the trailing
  `allowMainThread` flag per call.
* `FileSystem::assertIoThread(allow)` is the same check, exposed so File's OWN internals declare
  their policy too: CookedSceneData and SceneCooker stream binary ranges through `FILE*` / ifstream,
  which no read-it-all API can express.

Already marked as main-thread-by-design:

* main()'s whole INIT phase — one scope, released at `initScope.stop()`, so anything the FRAME LOOP
  does trips the assert
* `loadAssetFile` (prefab and `.oc` text, read once per asset at first spawn, then cached)
* scene load and cook, shader compiles, ScriptHost compile/load
* the editor panels (explicit user actions), and game save/load

### Profile markers

Every FileSystem IO function carries a profile marker named after it (`"FileSystem::readFileStr"`,
...) in the File category — through a local `IoScope`, **not** a plain `ProfileScope`. IO runs on
threads the profiler deliberately leaves unregistered (the transient startup texture-bake pool),
where `ProfileScope` would dereference a null track; `IoScope` no-ops when `threadTrack()` is null.
