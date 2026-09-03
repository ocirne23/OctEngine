# File

> Library documentation for `Code/File`.
> Read [`.claude/CLAUDE.md`](../../.claude/CLAUDE.md) first — rules, building, style, dependency
> direction. The asset TEXT formats are listed there under **Asset files**.

Asset import plus **the engine's only disk seam**. Links Animation (+ assimp, zlib, meshoptimizer
PRIVATE).

## `FileSystem` — THE disk seam

[FileSystem.ixx:23](Private/FileSystem.ixx#L23). **The ONLY code that includes `<filesystem>` and
`<fstream>`.** Core does not export them, so no other library can touch the disk behind its back — a
library that needs the disk links File.

Paths are plain UTF-8 `std::string`; **no `std::filesystem::path` leaks into any consumer.**

`initialize()` walks up from the working directory to the repo root, makes `Assets/` the working
directory, and registers `Dependencies/Dll/`. Main thread, at startup — exempt by nature.

| Group | API |
|---|---|
| Reads / writes | `readFileStr`, `readFileBytes`, `writeFileStr`, `writeFileBytes` |
| Queries | `exists`, `isDirectory`, `isRegularFile`, `fileSize` (0 when missing), `lastWriteTimeSec` |
| Mutations | `createDirectories`, `remove`, `removeAll`, `rename`, `copyFile`, `copyLastWriteTime` |
| Listing | `listDirectory`, `listDirectoryRecursive` → `DirEntry{path, name, extension, isDirectory, size}`. **Unsorted**; false clears `out`. |
| Working dir / resolution | `currentPath`, `setCurrentPath`, `absolutePath`, `canonicalPath` (requires the path to exist), `weaklyCanonicalPath`, `relativePath` |

`lastWriteTimeSec` is seconds since the *file clock's* epoch — **only ever compared or stored, never
formatted.** `copyLastWriteTime` stamps a destination with a source's mtime; ScriptHost marks a built
DLL as up-to-date with its source that way.

### Pure PATH math — no disk, callable anywhere, no thread rule

`join` · `parentPath` · `filename` · `stem` · `extension` (with the dot) · `replaceExtension` ·
`normalize` (lexically normal, forward slashes) · `lexicallyRelative` · `isAbsolute` · `pathEquals`
(separator- and case-insensitive on Windows).

> **`lexicallyRelative` vs `relativePath`:** the former is computed PURELY from the strings and is
> correct when both sides are already absolute and canonical — which is what the asset browser holds
> — and is **the right choice for anything per-frame**. `relativePath` resolves both sides through
> `weakly_canonical`, which hits the disk.

### The main-thread IO assert

**Every IO call asserts when it runs on the main thread** — the thread that called `initialize()` —
because disk latency there is a frame hitch. Worker threads are never restricted.

Work that legitimately blocks main must say so:

```cpp
const FileSystem::AllowMainThreadIO allowIo;   // RAII, nestable, thread_local
const oc::string text = FileSystem::readFileStr(path);
```

or per call with the trailing `allowMainThread` flag where a scope would be noise.

> **`/GT` caveat:** the scope is `thread_local`, so a fiber that PARKS inside one and resumes on
> another worker **leaves the scope behind**. Keep the scope tight around the actual call, never
> around a job wait.

`isMainThread()` is public so a caller can BRANCH — queue the work to a job instead of blocking —
rather than just silencing the assert.

`assertIoThread(allow)` is the same check exposed for File's OWN internals: `CookedSceneData` and the
mesh streamer stream binary ranges through `FILE*`/`ifstream` directly (seeks, byte ranges), which no
read-it-all API can express, so they call it at their IO entry points and stay inside the policy.

**Already declared main-thread-by-design:** main()'s whole INIT phase — ONE scope, released at
`initScope.stop()`, so anything the FRAME LOOP does trips the assert — plus `loadAssetFile`, scene
load and cook, shader compiles, ScriptHost compile/load, the editor panels (explicit user actions),
and game save/load.

### Profile markers

Every FileSystem IO function carries a marker named after it (`"FileSystem::readFileStr"`, ...) in
the File category, through a local `IoScope` and **NOT a plain `ProfileScope`**: IO runs on threads
the profiler deliberately leaves unregistered — the transient startup texture-bake pool — where
`ProfileScope` would dereference a null track. `IoScope` no-ops when `threadTrack()` is null.

## `AssetParser` (`File:AssetParser`)

The text format behind every engine text asset (`.oc`, `.pre`, `.anm`, `.apl`, `.pfx`, game saves,
tweaks files).

```
Key value value ...        -> key + values
"quoted value"             -> one value, may contain spaces
0, 0, 0                    -> commas separate (vectors)
# ... or // ...            -> comment line
```

Indentation defines hierarchy — tabs or spaces.

`AssetNode{ key, values, children }` with typed accessors (`asString` / `asBool` / `asInt` /
`asFloat` / `asVec3`), **case-insensitive** `find` / `findAll` (keys are human-authored), and `set` /
`addChild` builders for writing.

`parseAssetText` · `loadAssetFile` · `writeAssetText`.

**`File:ObjectDescription`** parses `.oc` into `ObjectContainerDesc` (`loadObjectContainerDesc`,
`toObjectContainerDesc`). It lives here so Procedural's scatter assets can import through `.oc` too:
`name`, `path`, `procedural`, `mergeNodes`, `preTransformVertices`, `decimationFactor`, plus
`MaterialOverridesDesc` (pipeline name, `excludeFromRayTracing`, `useSceneTextures`, and
diffuse/normal/metalRoughness texture index overrides where −1 keeps the default).

## `ISceneData` — the import interface

[ISceneData.ixx:54](Private/ISceneData.ixx#L54). Three loaders:

| Factory | Use |
|---|---|
| `createAssimpLoader()` | Model files. |
| `createProceduralLoader()` | Generated geometry by shape name — terrain, skysphere, debug shapes. |
| `createMeshScene(MeshGeometryDesc, colorRGBA, w, h)` | Wraps caller-supplied geometry as a single-mesh, single-material scene ready for `ObjectContainer`. Optional RGBA8 image becomes the material's diffuse texture; null gives a fallback checkerboard. **This is how a generated terrain chunk becomes renderable.** Pointers are borrowed only for the call. |

Queries: `getRootNode`, `getMesh(name|idx)`, `getMaterial`, `getTexture`, plus the skeletal set
`getSkeleton` / `getAnimations` / `getAnimation` (null and 0 when the scene has no bones).

`loadAnimations(filePath, targetSkeleton, outSet, skipName, clipNameOverride, trackName)` loads clips
from a SEPARATE file — rig in one, animations in others — **resolving each channel against the target
skeleton BY BONE NAME**, which is what makes Mixamo-style exports work. `skipName` strips a track by
substring (a "TPose" exported into every file); `trackName` selects one track from a multi-track file.

## The cooked scene cache

`ISceneData::loadCached(path, mergeNodes, preTransformVertices, SceneCookOptions)`. World uses it for
every non-procedural container.

**Serves a binary snapshot** at `Assets/Local/Cooked/<tag>.vsc` — reader `File:CookedSceneData`,
writer SceneCooker.cpp — **re-read with a single `fread`**. Raw little-endian structs, no versioning
compatibility: any layout change bumps `SCENE_CACHE_VERSION` (currently **6**) and stale files
silently re-cook.

### Validity

A cache is served only when magic, version, the source's **mtime + size**, the **options hash** and
the per-converted-texture stamps all match. Any mismatch re-imports and re-cooks in place.

**Skinned or animated scenes ALWAYS import directly and are never cooked.**

### What cooking bakes

* **meshopt LOD chains** for static meshes without authored `LodN_` chains — so changing a LOD tweak
  recooks on the next start. Up to `MAX_COOKED_LOD_LEVELS` (7) levels beyond LOD0, each with its
  index count AND its **geometric deviation from LOD0**, which is what the GPU LOD selector's
  screen-space error metric needs.
* **`decimationFactor` < 1** simplifies every mesh's BASE geometry at cook time, dropping unused
  vertices; the LOD chains then build on the result. Skinned scenes bypass the cache, so they never
  decimate.
* **Texture conversion** — non-DDS textures, loose AND embedded, into `Assets/Local/Cooked/<tag>_tex/`:

  | Usage | Format |
  |---|---|
  | Color (sRGB) | BC1, or **BC3 when the alpha channel is actually used** (any texel below 250) |
  | NormalMap | BC5, XY only — the material flags Z reconstruction |
  | Data (roughness / AO / masks) | BC1 |

  **Alpha-masked diffuse mips are alpha-coverage-rescaled**: each mip's alpha is binary-searched so
  the fraction of texels passing the material's alpha-test cutoff matches level 0. Only meaningful
  where the alpha channel survives compression (BC3) and the full-res coverage is not already
  all-or-nothing.

### Garbage collection

`gcStaleCaches()` runs **whenever anything cooks**. A `.vsc` and its `_tex` folder are deleted when
its format version is outdated, its source model no longer exists, or the source's mtime/size no
longer match the stamp. The header stores the source path for exactly this, so the pass needs no
lookup table. Files that are not ours are left alone.

> Caches whose source is intact but whose `.oc` import options changed are **indistinguishable from a
> second `.oc` variant of the same model**, so those are overwritten in place by their own recook
> instead of being collected.

Delete `Assets/Local/Cooked/` to force a full recook.

### `MeshStreamSource`

`getMeshStreamSource(meshIdx, out)` — **only cooked scenes provide one** (the default returns false).

It hands out the cooked file path plus the absolute byte offsets of every attribute array, the LOD0
indices and the concatenated generated LOD indices. **This is what lets the renderer's mesh streaming
evict cold mesh data from VRAM and re-stream it later without keeping the scene loaded** — see
`MeshStreamer` in [`Code/RendererVK/CONTEXT.md`](../RendererVK/CONTEXT.md).

### `SceneCookOptions`

Every field participates in the options hash, so changing any of them re-cooks affected scenes.

| Field | Default |
|---|---|
| `enableCache` | true — false always imports directly, no read and no write |
| `convertTextures` | true |
| `generateLods` | true |
| `lodLevels` | 4 |
| `lodReduction` | 0.25 index-count factor per level |
| `lodMinIndices` | 32 |
| `decimationFactor` | 1.0 |

## `TextureConvert`

Standalone image → BC `.dds` conversion using the **same compressor the scene cooker uses**, for
loose textures that no `ISceneData` references — the procedural terrain's biome texture sets.

* `convertToDds(src, EUsage, out)` — png/jpg/tga/... into a full mip-chain `.dds`.
* `convertPackedToDds(r, g, b, out)` — builds an RGB image from up to three GRAYSCALE sources
  (`r` required, missing channels = 0) and compresses it as Data/BC1. For packing separate AO,
  roughness and metalness maps into one ARM-style texture. All present sources must share dimensions.

Output mip chains stream through the `TextureStreamer` like any cooked `.dds`.
