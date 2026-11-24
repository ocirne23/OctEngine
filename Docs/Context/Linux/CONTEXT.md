# Linux (NixOS)

The engine also builds and runs on Linux: clang 22 + libc++ + CMake/Ninja, all from `Linux/flake.nix`. The
Windows build does not change: every Linux difference is either in `Linux/` (read only when the compiler is not MSVC),
under `#ifndef _MSC_VER`, or a macro that is empty on MSVC (`forceinclude.h`).

## Building

```sh
nix develop path:./Linux        # the dev shell: compiler, libraries, the OC_* variables Linux/Linux.cmake reads
cmake -B Build-Linux -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build Build-Linux --target App
Build-Linux/Code/App/App        # from the repo root, like on Windows (FileSystem finds Assets/)
```

* **Use the `path:` form for the shell.** `path:./Linux` copies only the `Linux/` folder into the store; the shell needs
  no other file. `./Linux` (no `path:`) is the git form, which copies the repo: see [Packaging](#packaging) for the cost.
* **Keep the shell as a GC root**: `nix develop path:./Linux --profile .devshell` (the repo root, gitignored). Without a
  root, `nix-collect-garbage` deletes the libc++ builds of glslang / shaderc / SPIRV-Tools / assimp, and the next shell
  builds them again from source (they are in no public binary cache). `nix develop ./.devshell` opens the kept shell
  without evaluating the flake; run the `--profile` command again after a flake change, BEFORE a garbage collection.
  The root is outside `Build-Linux/` on purpose: deleting the build folder keeps the shell.
* **Configure inside the dev shell**: `clang-scan-deps` must be the flake's wrapped one (it sees the libc++ include
  paths); `Linux/Linux.cmake` warns when it is not. The shell exports `CMAKE_EXPORT_COMPILE_COMMANDS`, so a NEW build
  folder gets `compile_commands.json` (an existing one: `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` once).
* **Two shells**: `default`, and `path:./Linux#build`: the same without the CUDA provider of ONNX Runtime (no 3 GiB of
  CUDA libraries, which the public binary cache does not have; App builds, the diffusion terrain has no provider).
* **The shell's `LD_LIBRARY_PATH` is only `/run/opengl-driver/lib`** (the driver's libraries): nix store paths there would
  also reach every other program started from the shell. App finds the nix libraries through its runpath (`ldd` resolves
  all of them without the variable), and `libocngx.so` carries `/run/opengl-driver/lib` in its own runpath for the NGX
  core.

## The flake (`Linux/`)

| File | Holds |
|---|---|
| `flake.nix` | The inputs (nixpkgs, the pinned sources), the outputs, the two shells |
| `nix/common.nix` | What the shells and the package share: compiler, libraries, hardening flags, the `OC_*` variables. One list, so a shell builds what the package builds |
| `nix/overlay.nix` | `pkgs.octengine`: the libc++ rebuilds, SDL3 without zenity / tray, Steam Audio, ONNX Runtime |
| `nix/onnxruntime.nix`, `nix/phonon.nix` | The two prebuilt binary packages (`callPackage` style) |
| `nix/source.nix` | The files the package build reads (see Packaging) |
| `package.nix` | The engine package |

* **The overlay adds `pkgs.octengine`; it never replaces a nixpkgs name.** Replacing `pkgs.glslang` & co. would rebuild
  every nixpkgs package that uses them (the Vulkan validation layers, ...) against libc++.
* **Unfree**: only the CUDA runtime, by name (`allowUnfreePredicate`). A new unfree package fails evaluation until it is
  added to that list.
* **The hardening flags**: off are `fortify` (libc++'s `std.cppm` cannot export glibc's fortified printf inlines),
  `format` (the engine's `printf(text)` logging), `stackprotector` (`/GS-`), and `zerocallusedregs`,
  `stackclashprotection`, `libcxxhardeningfast`, `pic` (code the MSVC build has none of; App stays a PIE). On:
  `strictoverflow` (`-fwrapv`, closer to MSVC than undefined overflow), `bindnow`, `relro`, `strictflexarrays1`. A change
  here rebuilds everything: clang rejects a BMI built with other overflow handling.
* **Format**: `nix run path:./Linux#formatter.x86_64-linux -- Linux/*.nix Linux/nix/*.nix` (nixfmt). Not `nix fmt`: it
  takes the flake from the working directory in the git form, with its copy.

## Packaging

```sh
nix build ./Linux               # the engine with CUDA: result/bin/App, libocngx.so, the DLSS model
nix build ./Linux#nocuda        # without the CUDA provider
nix run ./Linux                 # from the repo root: App finds Assets/ by walking up from the working directory
nix flake check ./Linux         # builds #nocuda and checks the .nix formatting
```

**The cost: these need the git form, and it copies the repo into the store first.** The package build reads `Code/`,
`Dependencies/Include`, `Linux/` and two `Assets/` folders, which the `path:` form cannot see. What the copy holds
depends on the working tree:

| Working tree | Nix copies | Size |
|---|---|---|
| Clean (everything committed) | the commit, from git: the Git LFS files arrive as their 134-byte pointers | about 100 MB (measured) |
| Any uncommitted change, anywhere in the repo | the tracked files of the WORKING TREE, with the real LFS content | about 10.7 GB (the tracked files on disk, mostly `Assets/`), and a NEW copy each time the tree content changes |

So **commit or stash before `nix build ./Linux`**, and work day to day in the dev shell. The copies stay in the store
until `nix-collect-garbage`. No file the build reads is in LFS, so the pointers do not matter to it.

* **`nix/source.nix` limits what the BUILD depends on** (a change to a texture does not rebuild App), not the copy above.
* **Iterating on the package without the copy**: the impure form reads only the filtered files (about 70 MB):
  ```sh
  nix build --impure --expr 'let f = builtins.getFlake "path:'$PWD'/Linux"; lib = f.inputs.nixpkgs.lib;
    in f.packages.x86_64-linux.nocuda.override { src = import ./Linux/nix/source.nix { inherit lib; } ./.; }'
  ```
* The package builds the `App` target only, `RelWithDebInfo`, with the debug sections stripped (`strip -S`).
* **CI**: `.github/workflows/linux.yml` runs `nix flake check ./Linux` on a checkout without LFS (the clean-commit case:
  the small copy). With no binary cache, every run builds the libc++ libraries and SDL3 from source.

## What `Linux/Linux.cmake` does

* **The Windows link names** (`EASTL$<$<CONFIG:Debug>:d>`, `vulkan-1`, `Ws2_32`, ...) are defined as targets mapped to
  their Linux library, or to an empty target. The library `CMakeLists.txt` files are unchanged.
* **Pinned dependencies are built from source** at the revision whose headers `Dependencies/Include` holds (flake
  inputs): EASTL 3.27.01, imgui docking `5a76f2ad`, box3d `8441b4a`, meshoptimizer v0.24. Steam Audio 4.8.1 is the
  prebuilt SDK `.so`.
* **glslang, SPIRV-Tools, shaderc, assimp (+ pugixml) are rebuilt against libc++** in the flake: the engine calls them
  through C++ APIs with `std::` types, and nixpkgs builds them against libstdc++ (different mangled names).
* **`Build-Linux/VendoredInclude`** replaces `Dependencies/Include` on every target: symlinks to every vendored folder
  except the libraries that link from nixpkgs (glslang, shaderc, spirv-tools, assimp, onnxruntime, SDL3, openxr). clang
  drops a `-I` folder that the nix wrapper also passes as `-isystem`, so the nixpkgs headers could never win otherwise.
* **`import std`**: libc++'s `std.cppm` / `std.compat.cppm` built as an ordinary module target (`oc_std`), with the
  engine's own flags. With any other flags CMake builds a second std BMI for the engine targets (`oc_std@synth_0`).
* **The linker is lld through its nix wrapper** (`--ld-path=$OC_LLD`). A bare `-fuse-ld=lld` bypasses the wrapper, which
  writes the runpath of every nix library: App then cannot find `libc++.so`.
* **App relinks when the shell's libraries change.** The wrapper passes the library folders through `NIX_LDFLAGS`, not
  the command line, so Ninja cannot see a flake change; `Build-Linux/nix-ldflags.stamp` changes with `NIX_LDFLAGS`, and
  App's link depends on it. Objects are not recompiled: after a nixpkgs update, build from a clean folder.
* **forceinclude.h** cannot be `-include`d (clang rejects anything in front of `module;`), so its macros are `-D`s.
* **The flags**: `/arch:AVX2` = `-march=x86-64-v3`; `/fp:fast` = the fast-math parts WITHOUT finite-math;
  `/Zc:threadSafeInit-` = `-fno-threadsafe-statics`; `/GS-` = `-fno-stack-protector`. No `/GT` and no tlsGuards
  equivalent (see Fibers).

## Warnings

A clean Linux build has **zero warnings**; keep it that way (fix a new one, do not silence it). `-Wall -Wextra`, with the
root CMakeLists' MSVC exceptions (`/wd4100` unused parameter, `/wd5311`) plus a short, commented list in
`Linux/Linux.cmake`, each with its reason: clang's `static __inline__` x86 intrinsics ("TU-local entity exposed"),
lambda captures MSVC requires and clang calls unnecessary, designated initializers that leave members out on purpose,
and flags the port or the nix wrapper itself causes. Third-party headers are system headers (`--system-header-prefix`
per vendored folder; nixpkgs headers arrive as `-isystem`), and the vendored imgui-node-editor sources are third-party
code. `-ftabstop=4`: the sources are written with Visual Studio's 4-column tabs (`-Wmisleading-indentation`).
`Linux/Include/assert.h` keeps the non-Debug `assert` the type-checked `(void)sizeof(e)` of `forceinclude.h` after any
`<assert.h>` (so an assert-only variable is used on both platforms); Vulkan-Hpp's own asserts stay unevaluated there
(they name Debug-only members). The purview includes of the wrapper modules are written quoted (`#include "x/y.h"`):
clang warns about an ANGLED include in a module purview.

## Rules for code that must keep building on Linux

* **No header units on clang.** CMake drives header units only for MSVC. A wrapper module that `export import <hdr>;`s
  has a clang branch: either `export extern "C++" { #include <hdr> }` in the purview (C APIs and glm: `Core.SDL`,
  `Core.glm`, `RendererVK:VK/glslang/DDS/stb_image/Aftermath`), or, when the header defines members out of class (which
  an export block rejects), a GMF include in EVERY importer: **`#include "ImguiGmf.h"`** in each unit that imports
  `Core.imgui`, **`#include "AssimpGmf.h"`** in each unit that imports `File:Assimp`. A new importer needs the line.
* **std headers**: `import <x>;` is `#ifdef _MSC_VER`; on clang `Core.OcSTL` exports `std.compat`. EASTL comes in
  through the GMF and its free operators are re-exported by name (`export namespace eastl { using eastl::operator==; ...}`
  in OcSTL) - a new EASTL free function used from outside Core needs a line there.
* **`module;` is never inside `#if`** (clang): a file that needs a GMF only on Linux has an unconditional `module;`.
* **A type a `*.fwd.ixx` forward-declares** (they are `extern "C++"`, global module) is defined with
  `export OC_FWD_ATTACH class X`, and its out-of-class member definitions sit between `OC_FWD_ATTACH_BEGIN` /
  `OC_FWD_ATTACH_END`. The same holds for a class another module befriends (`ScriptEventManager`) and for the friend /
  forward declarations of these types inside partitions. MSVC accepts a mismatch here; clang rejects it.
* **Visible, not just reachable**: clang wants the import of every module whose names a file spells
  (`import Core.Transform;` where `Transform` is named), and an `export` on the first declaration of a name that is
  exported later.
* **`int64` / `uint64` are `long` / `unsigned long` on Linux** (LP64: the `<cstdint>` types), `long long` on Windows.
* **Win32**: `Core.Windows` exports POSIX versions of the one-to-one calls (`Sleep`, `QueryPerformanceCounter`,
  `GetCurrentThreadId`, `VirtualAlloc`, `LoadLibraryA` = dlopen, ...); `Core` exports `sprintf_s` / `strncpy_s` /
  `fopen_s` / `_snprintf_s` / `_putenv`; `Linux/Include/intrin.h` maps the MSVC intrinsics. Anything else is an
  `#ifdef _MSC_VER` with a POSIX branch at its call site.

## Platform differences

* **Fibers** (`Linux/Include/OcFiberCompat.h`): an x86-64 System V context switch under the Win32 fiber names. clang
  has no `/GT`, so `JobSystem.cpp`'s thread-locals are `OcFiberSafeTls` (every access a fresh, non-inlined lookup). A
  `thread_local` in OTHER job code read again after a wait in the same function may still be cached by clang: the
  ThreadLocalScope rules (Threading) matter more here, not less.
* **Global teardown: InitSeg.h's order is written as imports.** ELF has no init_seg, and `[[gnu::init_priority]]`
  orders only WITHIN one module unit (clang emits no `.init_array.N` for a module interface: the unit initializes as
  one block, when its first importer does). Across units a global is constructed after the globals of the units its
  own unit imports, so destructed before them. `OC_INIT_SEG` is empty on Linux; instead the units that define the
  globals carry `#ifndef _MSC_VER` imports that state the teardown: `World.ixx` imports the Entity partitions and the
  modules ~World releases into, `PhysicsWorld.ixx` imports Threading, and the RendererVK chain (renderer -> data
  managers / XR -> GPU allocator -> device -> instance) imports downward. **A new global whose dtor uses another global
  needs such an import on Linux.** One order cannot be stated: the data managers outlive the renderer (the reverse
  would be an import cycle); their dtors use only the device.
  A global in a module IMPLEMENTATION unit (`.cpp`) is outside that cascade: it is constructed after every interface
  global and destroyed before them. The diffusion runtime is such a global, and the procedural systems' jobs sample
  it until their dtors join them, so on Linux its lifetime is `g_diffusionRuntimeLifetime` in `GeneratorV3.ixx`
  (trivial storage in the `.cpp`), and the partitions of the procedural globals import `:GeneratorV3`.
  **Libraries loaded at run time come last of all**: glibc runs exit handlers newest first, and cuDNN registers its
  statics during the first inferences, so they die before any App global; the driver's NGX core (DLSS) does the same
  at its init. `main()` therefore ends, on Linux only, with the explicit teardown calls, while those libraries are
  whole: `TerrainGenV3::shutdownInference()` (unloads the models, waiting out an inference in flight, and destroys the
  `Ort::Env`) and `Renderer::releaseDriverLibraries()` (NGX shutdown; `~Device`'s own call then has nothing to do).
* **Crash reports**: the unattended fatal-signal handler (`UnattendedRun.ixx`) prints the faulting thread's stack, then
  re-raises the signal, so systemd-coredump keeps a core with every thread (`coredumpctl debug <pid>`).
* **GPU profiler timeline**: the `QueryPerformanceCounter` shim reads `CLOCK_MONOTONIC`, so `GpuProfiler` calibrates
  against `VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR`, not the QPC domain. The NVIDIA driver accepts a QPC request on Linux and
  returns `VK_SUCCESS` with a host time of 0, which put every GPU scope one system uptime in the past. `CLOCK_MONOTONIC_RAW`
  is a different clock (measured: 0.37 s apart), so the shim and the domain must change together.
* **Swapchain on Wayland**: the surface reports no current extent, so the swapchain takes the window's pixel size; an
  unsupported present mode falls back to mailbox, then FIFO.
* **DLSS through NGX, not Streamline** (Streamline is Windows-only): the `Streamline::` functions keep their contract,
  and on Linux `Streamline.cpp` drives NVIDIA's NGX SDK through `libocngx.so`, a C shim (`Linux/Include/ocngx.h`,
  `Linux/Ngx/ocngx.cpp`). The SDK's static loader needs libstdc++, so `Linux/Linux.cmake` builds the shim with the
  flake's GCC (`OC_NGX_CXX`) against the pinned SDK (`OC_NGX_SDK`, 310.9.16), with libstdc++ linked statically and
  hidden, and puts it and the DLSS model (`libnvidia-ngx-dlss.so.*`) next to App; App `dlopen`s it `RTLD_LOCAL`, and
  NGX finds the model next to the shim. The driver supplies the NGX core (`libnvidia-ngx.so.1`). No swapchain proxies,
  no extra queues: the upscale is recorded into the frame's command buffer, and the feature is (re)created only when
  the mode, preset or a size changes, on an idle queue. Motion vectors go in as render pixels (scale 1). NGX rejects an
  output under 32x32, so such a frame (one at startup) falls back to the engine's own resolve. Without the shim, on
  another GPU or an old driver, DLSS is off as on Windows without the Streamline DLLs.
* **Off on Linux**: Nsight Aftermath (asks for the Windows DLL name), script fault containment (no SEH), runtime script
  DLL compilation (MSVC command line; `SCRIPTS_STATIC` builds the scripts into App).
* **Diffusion terrain on CUDA**: ONNX Runtime 1.24.4 (the version of the vendored headers) comes as Microsoft's prebuilt
  GPU package from NuGet (`flake.nix`; the GitHub release page is gone), with the CUDA 12 / cuDNN 9 runtime from
  nixpkgs and the NVIDIA driver's libcuda (`/run/opengl-driver/lib`). `Onnx.cpp` appends the CUDA provider where
  Windows appends DirectML; the CUDA provider needs the `Ort::Env` (its logger) to exist first.
  **VRAM is shared with the renderer, with no oversubscription** (WDDM pages video memory to system RAM on Windows; the
  Linux driver fails the allocation). Measured on the 8 GB RTX 3080 Laptop in the sandbox: the desktop 1.6 - 2.0 GB, the
  renderer about 3.9 GB, so CUDA gets about 2 GB. The precision stays fp32 as on Windows, so tiles generated there are
  read here; `Terrain/V3/FP16 inference` halves the models' VRAM (its tiles go to `<seed>_fp16`). On Linux only: latent and coarse
  batches of 2 (the noise is per window, so the batch size does not change the tiles), the sequential executor, the
  cuDNN heuristic search without the maximum workspace, an arena that grows by the request and shrinks after every
  run. The starting mesh pools are the Windows ones (`INITIAL_VERTEX_DATA` 320 MB, `INITIAL_INDEX_DATA` 64 MB in
  `Layout.ixx`; the sandbox's load reaches ~245 / ~58 MB on Windows - check Linux with `--vram-dump`). The streaming budgets are no lever here: meshes stream inside those fixed
  pools, and a freed texture stays in its VMA block. Inference can still run out of memory when the desktop or the
  resolution takes more; a failed inference is then retried and never saved ("A failed inference never reaches a
  cache" in [`Procedural/Generator.md`](../Procedural/Generator.md)). DLSS lowers the render resolution, and with it
  the renderer's share.

## After every upstream pull

MSVC accepts code that breaks the rules above (a missing `ImguiGmf.h`, a missing `OC_FWD_ATTACH`, a name used without
its import), so an upstream change written on Windows often breaks the Linux build in small ways. Build Linux after
every pull.
* **Network crypto** uses OpenSSL 3 with the same primitives as the CNG code (ECDH P-256, SHA-256 KDF over the secret
  plus the context, AES-128-GCM, HMAC-SHA256). Linux <-> Windows handshakes are untested: they agree only if CNG hashes
  the shared secret in the same byte order.
