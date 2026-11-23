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
* **No global teardown.** clang ignores `[[gnu::init_priority]]` in a module interface, where every engine global is
  defined, and neither link order nor module import order gives InitSeg.h's order (~EntityAllocator ran before ~World).
  `main()` ends in `_Exit` after a flush and a flush of a pending Saved tweak (the one thing a global dtor persists).
  `OC_INIT_SEG` is empty on Linux.
* **Off on Linux**: DLSS / Streamline (Windows-only SDK; the not-loaded paths), Nsight Aftermath (asks for the Windows
  DLL name), script fault containment (no SEH), runtime script DLL compilation (MSVC command line; `SCRIPTS_STATIC`
  builds the scripts into App).
* **Diffusion terrain: no GPU inference.** DirectML is Windows-only, so the generator (which asks for the GPU) fails and
  the sandbox world stays empty. A CUDA build of onnxruntime would be the Linux equivalent.
* **Network crypto** uses OpenSSL 3 with the same primitives as the CNG code (ECDH P-256, SHA-256 KDF over the secret
  plus the context, AES-128-GCM, HMAC-SHA256). Linux <-> Windows handshakes are untested: they agree only if CNG hashes
  the shared secret in the same byte order.
