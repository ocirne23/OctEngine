# What the dev shells and the engine package share: the compiler, the libraries, the hardening flags and the OC_* variables
# that Linux/Linux.cmake reads. One list, so a shell builds what the package builds.
{
  lib,
  llvmPackages_22,
  cmake,
  ninja,
  pkg-config,
  gcc,
  fetchgit,
  vulkan-headers,
  vulkan-loader,
  zstd,
  zlib,
  spirv-headers,
  openxr-loader,
  openssl,
  libx11,
  wayland,
  libxkbcommon,
  octengine,
  # The flake inputs: { eastl-src, imgui-src, box3d-src, meshoptimizer-src }.
  sources,
  # false: ONNX Runtime without the CUDA provider (see onnxruntime.nix).
  cudaSupport ? true,
}:
let
  llvm = llvmPackages_22;
  # The wrapped scanner: the unwrapped one does not see the libc++ / glibc include paths the clang wrapper adds.
  clang-tools = llvm.clang-tools.override { clang = llvm.libcxxClang; };
  onnxruntime = if cudaSupport then octengine.onnxruntime-gpu else octengine.onnxruntime;

  # NVIDIA DLSS SDK (NGX) 310.9.16, the Linux parts only: the headers, the static NGX loader and the release DLSS model.
  # Linux/Linux.cmake builds Linux/Ngx/ocngx.cpp against it with GCC (the loader needs libstdc++) and puts the shim and
  # the model next to App. Distributed under the NVIDIA RTX SDK license (Dependencies/Dll/nvngx_dlss.license.txt).
  ngx-sdk = fetchgit {
    url = "https://github.com/NVIDIA/DLSS.git";
    rev = "refs/tags/v310.9.16";
    sparseCheckout = [
      "include"
      "lib/Linux_x86_64/libnvsdk_ngx.a"
      "lib/Linux_x86_64/rel/libnvidia-ngx-dlss.so.310.9.16"
    ];
    nonConeMode = true;
    hash = "sha256-Avu0jXlZv70+iS9A6IDF4RGhRiDWy3QbpgflipAItKw=";
  };
in
{
  stdenv = llvm.libcxxStdenv;

  nativeBuildInputs = [
    cmake
    ninja
    pkg-config
    clang-tools
  ];

  buildInputs = [
    vulkan-headers
    vulkan-loader
    octengine.sdl3
    zstd
    zlib
    octengine.assimp
    octengine.glslang
    octengine.shaderc
    octengine.spirv-tools
    spirv-headers
    openxr-loader
    onnxruntime
    openssl
    libx11
    wayland
    libxkbcommon
    octengine.phonon
  ];

  # _FORTIFY_SOURCE turns glibc's printf family into internal-linkage inlines, which libc++'s std.cppm cannot export
  # (`import std;` fails to build).
  # format: -Werror=format-security trips over the engine's printf(text) logging; stackprotector: the engine builds with
  # /GS- on Windows (no stack cookies), -fno-stack-protector here.
  # zerocallusedregs, stackclashprotection, libcxxhardeningfast: code the MSVC build does not have either. pic: the engine
  # links no shared library (clang's default -fPIE stays; the NGX shim passes its own -fPIC).
  # strictoverflow (-fwrapv) stays: it is closer to MSVC than the standard's undefined signed overflow.
  hardeningDisable = [
    "fortify"
    "fortify3"
    "format"
    "stackprotector"
    "zerocallusedregs"
    "stackclashprotection"
    "pic"
    "libcxxhardeningfast"
  ];

  env = {
    OC_EASTL_SRC = "${sources.eastl-src}";
    OC_IMGUI_SRC = "${sources.imgui-src}";
    OC_BOX3D_SRC = "${sources.box3d-src}";
    OC_MESHOPTIMIZER_SRC = "${sources.meshoptimizer-src}";
    OC_PHONON_LIB = "${octengine.phonon}/lib";
    OC_ONNXRUNTIME_LIB = "${onnxruntime}/lib";
    OC_NGX_SDK = "${ngx-sdk}";
    OC_NGX_CXX = "${gcc}/bin/g++";
    OC_CLANG_SCAN_DEPS = "${clang-tools}/bin/clang-scan-deps";
    # std.cppm / std.compat.cppm: built as an ordinary module target (CMake's own `import std` is still behind a
    # per-version experimental gate).
    OC_LIBCXX_MODULES = "${llvm.libcxx}/share/libc++/v1";
    # The include folders of the libraries that link from nixpkgs (their vendored copies match the Windows .libs).
    OC_NIX_INCLUDES = lib.concatMapStringsSep ";" (p: "${lib.getDev p}/include") [
      octengine.glslang
      octengine.spirv-tools
      octengine.assimp
      octengine.shaderc
      octengine.sdl3
      openxr-loader
      zstd
      zlib
      openssl
    ];
    # The linker: lld through its nix wrapper, which writes the runpath of every nix library the binary links (a bare
    # -fuse-ld=lld bypasses the wrapper, and App then cannot find libc++.so at start).
    OC_LLD = "${llvm.bintools}/bin/ld.lld";
  };
}
