# File

> Library documentation for `Code/File`.
> Read [`.claude/CLAUDE.md`](../../../.claude/CLAUDE.md) first — rules, building, style, dependency
> direction. The asset TEXT formats are listed there under **Asset files**.

Asset import plus **the engine's only disk seam**, with `.dat` packs mounted into it. Links Animation
(+ assimp, zlib, meshoptimizer, zstd PRIVATE).

## `FileSystem` — THE disk seam

[FileSystem.ixx:23](../../../Code/File/Private/FileSystem.ixx#L23). **Only File's own sources import
`<filesystem>` and `<fstream>`.** Core does not export them, so no other library can touch the disk behind
its back — a library that needs the disk links File.

Paths are plain UTF-8 `oc::string`; **no `std::filesystem::path` leaks into any consumer.**

`initialize()` walks up from the working directory to the repo root, makes `Assets/` the working
directory, and registers `Dependencies/Dll/` (`AddDllDirectory`). Main thread, at startup — exempt by
nature.

| Group | API |
|---|---|
| Reads / writes | `readFileStr`, `readFileBytes`, `writeFileStr`, `writeFileBytes`; byte ranges through `FileReader` (below) |
| Queries | `exists`, `isDirectory`, `isRegularFile`, `fileSize` (0 when missing), `lastWriteTimeSec`, `lastWriteTimeTicks` (file clock ticks, what the cooked caches stamp), `stat` (existence + size + mtime in one lookup) |
| Mutations | `createDirectories`, `remove`, `removeAll`, `rename`, `copyFile`, `copyLastWriteTime` |
| Listing | `listDirectory`, `listDirectoryRecursive` → `DirEntry{path, name, extension, isDirectory, size}`. **Unsorted**; false when the directory cannot be read; `out` is cleared either way. |
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

`assertIoThread(allow)` is the same check exposed for File's OWN internals (the pack builder, the scene cooker's
writes) and for `FileReader::open`.

### `FileReader` — byte ranges

`open(path)` through the mounts, then `read(offset, dst, bytes)` from any thread (positional: `ReadFile` with an
offset on the disk, the mapping in a pack, nothing shared), `readAll`, and `view()` — the whole file without a copy
when it is packed (empty for a disk file). **The texture streamer, the mesh streamer and the DDS load
(`Texture::initialize`) read through it**, so their mip ranges and attribute arrays come out of a pack the same way.
A packed reader holds its pack (`shared_ptr`) until it closes, even across an unmount.

**Already declared main-thread-by-design:** main()'s whole INIT phase — ONE scope (`startupIo`),
released right before the frame loop, so anything the FRAME LOOP does trips the assert — plus
`loadAssetFile`, scene load and cook, shader compiles, ScriptHost compile/load, the editor panels
(explicit user actions), the mode start (`Session::startWorldAndGame`), and game save/load.

### Profile markers

Every FileSystem IO function carries a marker named after it (`"FileSystem::readFileStr"`, ...) in
the File category, through a local `IoScope` and **NOT a plain `ProfileScope`**: IO runs on threads
the profiler deliberately leaves unregistered — the transient startup texture-bake pool — where
`ProfileScope` would dereference a null track. `IoScope` no-ops when `threadTrack()` is null.

## Packs (`.dat`)

One file that holds a folder tree (by default `Assets/`) and mounts into `FileSystem`, so every caller reads from it
with no change. Sources in `Code/File/Private/Pack/`: `NativeFile` (Win32 positional IO + mapping), `PackFormat`
(the layout), `PackFile` (a mounted pack), `PackBuilder` (build / update / verify / list, exported).

### Layout

`[header page] [file data] [dead data] [TOC]`. Raw little-endian structs, no compatibility: a layout change bumps
`Pack::VERSION`, and so does a change of `Core.BitPathTrie`'s node size or key mode (both are in the header) — an
old pack refuses to mount, rebuild it.

* **The TOC is two baked `Core.BitPathTrie` tries** — the file paths, and every folder that holds a file — plus one
  48-byte `Pack::Entry` per file (offset, stored size, size, source mtime in file-clock ticks, content hash, flags),
  **indexed by the file trie's key**. The trie storage spans point straight into the mapping: mounting reads the
  header, checks the TOC hash and the entry bounds, and copies nothing.
* Paths are relative to the packed folder and case-insensitive, like Windows; the stored case comes back in listings
  and `canonicalPath`. A path with characters outside the trie's 64-symbol alphabet (non-ASCII, `\`, ...) or 512+
  characters cannot be packed: the builder leaves it out with a warning.
* File data is 16-byte aligned, files of 64 KiB and up page aligned. Files are written in key order, so a folder's
  files sit together.

### Reads

The whole pack is memory mapped read-only. A read of under 64 KiB is a `memcpy` from the mapping; a larger one is
ONE positional `ReadFile` on the pack's handle (one IO request instead of a page fault per page). Compressed entries
(zstd, optional) decompress on read; a range of one decompresses the whole file, which is why the builder never
compresses `.dds` / `.vsc` (the streamers read those in ranges).

### Mounting and priority

`mountPack(path, priority, mountPoint)`; both optional — left out, the pack's own (the header stores the `Priority` /
`MountPoint` it was built with). `mountPacksIn(dir)` mounts every `.dat` in a folder that way, in name order. The disk
is a mount too (`DISK_PRIORITY_DEFAULT` 0); packs default to 100.

**Packs live next to the executable** (`dat.cfg`'s `Output $(ExeDir)/...`; `executableDir()`). `--packs` mounts them.
`initialize()` looks for `Assets/` above the working directory, then above the executable; **when there is none (a
shipped build) the executable's folder becomes the root and working directory and its packs mount by themselves** —
it fails only when there are none either. With `Assets/` present nothing mounts on its own, so a development run never
reads a stale pack over edited loose files.
**A lookup walks the mounts highest priority first and the first that has the file serves it**; ties go to the later
mount. So:

| Setup | Effect |
|---|---|
| pack 100, disk 0 | pack first, loose files fill the gaps (the default) |
| pack -10, disk 0 (or `setDiskPriority`) | loose files first, the pack is the fallback (development over a shipped pack) |
| `setDiskEnabled(false)` | pack only |
| two packs, 100 and 200 | the 200 pack patches the 100 one |

* A disk mount with packs BELOW it costs one `exists` per lookup; one with none below costs nothing (the disk call
  itself reports the miss). With no pack mounted at all, every call is the plain disk path after one atomic load.
* Only paths under `Assets/` reach the packs (relative, or absolute below it). Anything else is disk only, also with
  the disk disabled. `mountPoint` places the pack's root at an `Assets/`-relative folder.
* **Writes always go to the disk.** A path this process wrote, removed, renamed or copied onto while packs are
  mounted is OVERRIDDEN: served from the disk from then on (a removed one then reads as missing), so a stale packed
  copy never comes back. `rename` / `copyFile` from a packed file copy it out. Listings merge every mount, each path
  once, the highest priority mount's entry winning; overridden packed entries are skipped.
* Reads hold a shared lock on the mount table for the lookup and the pack read; mount / unmount take it exclusively.
* `mounts()` and `sourceOf(path)` say what is mounted and who serves a path.

**Not covered — these read by path themselves and need the loose file:** the Assimp import (and so cooking, and
skinned scenes), the onnxruntime model loads, the script DLLs (`LoadLibrary`), and the scene
cooker's writes and `gcStaleCaches` (disk only). The cooked cache READ (`CookedSceneData::load`, `loadCached`'s
source stamp and its "source missing" scan) goes through `FileSystem`, so a pack holding `Local/Cooked/` serves cooked
scenes without their source models.

### Building and updating

`PackBuilder::build(PackBuildOptions)` (App: `--pack-build`). **Default: UPDATE in place.**

**The pack config `Assets/dat.cfg`** lists the packs to build: one `Pack <name>` block each (`AssetParser` text, the
grammar is in the file's header) with `Output`, `Source`, `Include` / `Exclude` prefixes, extension filters,
`Compression`, the update knobs, and the `Priority` / `MountPoint` stored in the pack. `$(ExeDir)` at the start of
`Output` / `Source` is the running executable's folder. `PackBuilder::loadConfig` turns it into `PackConfig`s; an
unknown key is an error, so a typo never packs the wrong set. The checked-in config splits the assets in two:
`Content` (everything authored minus `Models/`, `Project/`, `Scenarios/`, `TerrainDiffusion/`, `Textures/boat/`,
`Textures/Terrain/`; zstd) and `Generated` (the `Local/` caches that ship: cooked scenes, SPIR-V, procedural meshes /
textures, and the diffusion tile cache `Local/Diffusion/` - terrain without the ONNX models; priority 110, a patch
over Content), both next to the executable.

1. Scan the source folder (include prefixes, exclude prefixes / extensions, include extensions, a filter callback;
   a folder that no include prefix lies in or below is not walked). **No exclude is built in**: the config lists every
   one. The only file always skipped is the pack being written (and its `.tmp`).
2. A file whose size and mtime match its old entry is kept **without being read**. One whose stamp changed is read
   and hashed: same content = only its stamp changes ("touched").
3. Nothing changed, no case rename = the pack is not touched at all.
4. Otherwise the new and changed files are appended behind the old TOC, then the new TOC, then the header page is
   rewritten — **the header write is the commit**; everything before it is flushed first, so a crash leaves the old
   version. File data is never overwritten, so a process that has the pack mounted keeps reading the version it
   mounted (remount to see the update).
5. When the dead bytes (replaced and removed files, old TOCs) would pass `compactThreshold` (25%) of the data, or with
   `incremental = false`, the pack is **rewritten whole** into `<pack>.tmp`, unchanged files copied from the old pack
   as stored (no re-read, no recompression), and the temp file replaces the pack. That rename fails while a process
   has the pack mapped: unmount first.

Loads, hashing and compression run on `threads` plain threads (File cannot link Threading) in windows of 256 MB.
An unreadable file keeps its packed version; an unreadable NEW file fails the build.

## `ImageIO` (`File:ImageIO`)

`writePngRgba8` / `readImageRgba8`: PNG out / any stb-decodable image in, RGBA8, for GENERATED content
(the procedural tree textures in `Assets/Local/Trees/Textures`, which rocks read back for their bark; the
renderer's screenshot). stb encodes / decodes in memory and the bytes go through
`FileSystem::writeFileBytes` / `readFileBytes`, so the disk seam and its main-thread rule hold
(`allowMainThread`). Its stb implementations are STATIC (internal linkage), like SceneCooker.cpp's —
RendererVK owns the external-linkage `stb_image`.

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
`toObjectContainerDesc`). It lives here so a library below Entity can read `.oc` too:
`name`, `path`, `procedural`, `mergeNodes`, `preTransformVertices`, `decimationFactor`, plus
`MaterialOverridesDesc` (pipeline name, `excludeFromRayTracing`, `useSceneTextures`, and
diffuse/normal/metalRoughness texture index overrides where −1 keeps the default).

## `ISceneData` — the import interface

[ISceneData.ixx:54](../../../Code/File/Private/ISceneData.ixx#L54). Three loaders:

| Factory | Use |
|---|---|
| `createAssimpLoader()` | Model files. |
| `createProceduralLoader()` | Generated geometry by shape name — terrain, skysphere, debug shapes. |
| `createMeshScene(MeshGeometryDesc, colorRGBA, w, h)` | Wraps caller-supplied geometry as a single-mesh, single-material scene ready for `ObjectContainer`. Optional RGBA8 image becomes the material's diffuse texture; null leaves the material on the renderer's shared fallback textures. Pointers are borrowed only for the call. **No engine code calls it at present**: terrain chunks and ocean sectors build RendererVK's `RenderMeshData` from the same `MeshGeometryDesc` (no container). |

Queries: `getRootNode`, `getMesh(name|idx)`, `getMaterial`, `getTexture`, plus the skeletal set
`getSkeleton` / `getAnimations` / `getAnimation` (null when the scene has no bones).

`loadAnimations(filePath, targetSkeleton, outSet, skipName, clipNameOverride, trackName)` loads clips
from a SEPARATE file — rig in one, animations in others — **resolving each channel against the target
skeleton BY BONE NAME**, which is what makes Mixamo-style exports work. `skipName` strips a track by
substring (a "TPose" exported into every file); `trackName` selects one track from a multi-track file.

## The cooked scene cache

`ISceneData::loadCached(path, mergeNodes, preTransformVertices, SceneCookOptions)`. World uses it for
every non-procedural container.

**Serves a binary snapshot** at `Assets/Local/Cooked/<tag>.vsc` — reader `File:CookedSceneData`,
writer SceneCooker.cpp — **re-read with a single `fread`**. Raw little-endian structs, no versioning
compatibility: any layout change bumps `SCENE_CACHE_VERSION` (currently **7**) and stale files
silently re-cook. So does a change in what the cooked textures guarantee (v7: the roughness floor).

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
  | Height (displacement; `TextureConvert` only) | BC4, the source's R |
  | ColorAlpha (`TextureConvert` only: sRGB RGB + a LINEAR data alpha, e.g. albedo + roughness) | BC3 always; mips `STBIR_RGBA_NO_AW` (no alpha weighting, alpha linear) |
  | TwoChannel (`TextureConvert` only: two linear data channels, e.g. height + AO) | BC5 |

  **Alpha-masked diffuse mips are alpha-coverage-rescaled**: each mip's alpha is binary-searched so
  the fraction of texels passing the material's alpha-test cutoff matches level 0. Only meaningful
  where the alpha channel survives compression (BC3) and the full-res coverage is not already
  all-or-nothing.

* **The roughness floor — THE SHADERS RELY ON IT.** The lit / tree / foliage FS read the metal-roughness G as
  the GGX alpha with NO `max(.., 0.01)` of their own. Every texture in a metal-roughness slot gets, in every BC
  colour block of every mip, both 565 endpoints raised to G6 >= 1 (`ROUGHNESS_FLOOR_8` = 4/255 decoded;
  >= ~3.5/255 = 0.0137 after the palette rounding). The palette interpolates the endpoints, and filtering and
  mips are convex, so every sample holds it. A BC1 block's mode IS its endpoint order: when the raise flips it,
  the endpoints swap and the indices remap to the same entries; a 3-colour block that uses index 3 (black) is
  re-encoded. Covered: the cooked Data BC1 AND authored BC1 / BC2 / BC3 `.dds` files (Bistro's
  `*_Specular.dds`), which a roughness slot COPIES into `_tex/` with the fix (other slots reference authored
  `.dds` in place). **NOT covered — each logs a warning at cook:** an authored BC7 / `_SRGB` / other-format
  `.dds`, and a texture shared by a roughness slot and a diffuse / normal slot (it cooks for the other slot).
  Also not covered: the uncooked import (skinned scenes, `convertTextures` false, a failed conversion) and
  `.oc` `MaterialOverrides MetalRoughnessTexIdx`.

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
`MeshStreamer` in [`Docs/Context/RendererVK/Scene.md`](../RendererVK/Scene.md).

### `SceneCookOptions`

Every field participates in the options hash, so changing any of them re-cooks affected scenes.

| Field | Default |
|---|---|
| `enableCache` | true — false always imports directly, no read and no write |
| `convertTextures` | true |
| `generateLods` | true |
| `lodLevels` | 4 (capped at `MAX_COOKED_LOD_LEVELS`) |
| `lodReduction` | 0.25 index-count factor per level (clamped 0.05..0.75) |
| `lodMinIndices` | 32 |
| `decimationFactor` | 1.0 |

## `TextureConvert`

Standalone image → BC `.dds` conversion using the **same compressor the scene cooker uses** (it is
implemented in SceneCooker.cpp), for loose textures that no `ISceneData` references — the procedural
terrain's biome texture sets.

* `convertToDds(src, EUsage, out)` — png/jpg/tga/... into a full mip-chain `.dds`.
* `convertPackedToDds(r, g, b, out)` — builds an RGB image from up to three GRAYSCALE sources
  (`r` required, missing channels = 0) and compresses it as Data/BC1. For packing separate AO,
  roughness and metalness maps into one ARM-style texture. All present sources must share dimensions.
* `convertChannelsToDds(PackChannel[4], EUsage, out)` — the general packer: output channel i = channel
  `srcChannel` of image `path`, or the constant `fill` without a path; `scale` scales its contrast about 128 (the
  terrain's per-set height scale); `minValue` floors the channel at level 0 AND after every mip resize (the
  Mitchell filter undershoots) - the terrain diffr roughness alpha's 3/255. Each distinct source is decoded once;
  all must share dimensions. The terrain splat bake uses it for its packed set (albedo + roughness as
  ColorAlpha, height + AO as TwoChannel).

Output mip chains stream through the `TextureStreamer` like any cooked `.dds`.

* **In memory** (no file): `compressBlockRows(rgba, w, h, EBlockFormat, rowBegin, rowEnd, dst)` compresses block
  rows of one RGBA8 image into the whole image's output buffer (`compressedSize`), so a caller can split the rows
  over jobs (File cannot link Threading). BC1 / BC3 / BC4 / BC5; the cooker's `compressMip` uses it too. User: the
  procedural trees' runtime textures ("Texture files" in [`Docs/Context/Procedural/Trees.md`](../Procedural/Trees.md)).
