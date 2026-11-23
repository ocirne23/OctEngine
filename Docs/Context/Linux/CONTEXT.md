# Linux (NixOS)

The engine also builds and runs on Linux: clang 22 + libc++ + CMake/Ninja, all from the repo's `flake.nix`. The
Windows build does not change: every Linux difference is either in `Linux/` (read only when the compiler is not MSVC),
under `#ifndef _MSC_VER`, or a macro that is empty on MSVC (`forceinclude.h`).

## Building

```sh
nix develop                     # the dev shell: compiler, libraries, the OC_* variables Linux/Linux.cmake reads
cmake -B Build-Linux -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build Build-Linux --target App
Build-Linux/Code/App/App        # from the repo root, like on Windows (FileSystem finds Assets/)
```

* `nix develop` on the repo copies the whole tracked tree (several GB, mostly `Dependencies/`) into the nix store on
  every evaluation. Copy `flake.nix` + `flake.lock` into an empty folder and run `nix develop path:<that folder>` to
  avoid it; the flake does not use the repo's own files.
* **Configure inside the dev shell**: `clang-scan-deps` must be the flake's wrapped one (it sees the libc++ include
  paths); `Linux/Linux.cmake` warns when it is not.

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
* **`import std`**: libc++'s `std.cppm` / `std.compat.cppm` built as an ordinary module target (`oc_std`).
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
* **Swapchain on Wayland**: the surface reports no current extent, so the swapchain takes the window's pixel size; an
  unsupported present mode falls back to mailbox, then FIFO.
* **Off on Linux**: DLSS / Streamline (Windows-only SDK; the not-loaded paths), Nsight Aftermath (asks for the Windows
  DLL name), script fault containment (no SEH), runtime script DLL compilation (MSVC command line; `SCRIPTS_STATIC`
  builds the scripts into App).
* **Diffusion terrain on CUDA**: ONNX Runtime 1.24.4 (the version of the vendored headers) comes as Microsoft's prebuilt
  GPU package from NuGet (`flake.nix`; the GitHub release page is gone), with the CUDA 12 / cuDNN 9 runtime from
  nixpkgs and the NVIDIA driver's libcuda (`/run/opengl-driver/lib`). `Onnx.cpp` appends the CUDA provider where
  Windows appends DirectML; the CUDA provider needs the `Ort::Env` (its logger) to exist first.

## After every upstream pull

MSVC accepts code that breaks the rules above (a missing `ImguiGmf.h`, a missing `OC_FWD_ATTACH`, a name used without
its import), so an upstream change written on Windows often breaks the Linux build in small ways. Build Linux after
every pull.
* **Network crypto** uses OpenSSL 3 with the same primitives as the CNG code (ECDH P-256, SHA-256 KDF over the secret
  plus the context, AES-128-GCM, HMAC-SHA256). Linux <-> Windows handshakes are untested: they agree only if CNG hashes
  the shared secret in the same byte order.
