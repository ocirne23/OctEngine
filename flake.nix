{
  description = "OctEngine - Linux (NixOS) dev shell";

  # The Windows build uses the prebuilt .libs in Dependencies/Lib. On Linux the system libraries come from nixpkgs, and the
  # libraries whose vendored headers carry a pinned upstream revision (or a local patch) are built from that exact
  # revision by Linux/Linux.cmake, against the vendored headers in Dependencies/Include.
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
    flake-utils.url = "github:numtide/flake-utils";

    # EASTL 3.27.01 + the OCTENGINE LOCAL PATCH (already in Dependencies/Include/EASTL; only source/*.cpp is used).
    eastl-src = { url = "github:electronicarts/EASTL/3.27.01"; flake = false; };
    # imgui docking: the revision whose headers Dependencies/Include/imgui holds (plus the inline ImRect::Floor).
    imgui-src = { url = "github:ocornut/imgui/5a76f2adf1b0403b86a45010121fb32a6bff8680"; flake = false; };
    # box3d: the revision whose include/box3d is Dependencies/Include/box3d.
    box3d-src = { url = "github:erincatto/box3d/8441b4a06d6d09dcfb0b0f704df4d847d1437b92"; flake = false; };
    # meshoptimizer v0.24 (Dependencies/Include/meshoptimizer; nixpkgs carries 1.x).
    meshoptimizer-src = { url = "github:zeux/meshoptimizer/v0.24"; flake = false; };
    # Steam Audio 4.8.1 SDK (prebuilt libphonon.so; the same version as Dependencies/Include/phonon).
    steamaudio-sdk = {
      url = "https://github.com/ValveSoftware/steam-audio/releases/download/v4.8.1/steamaudio_4.8.1.zip";
      flake = false;
    };
  };

  outputs = { self, nixpkgs, flake-utils, eastl-src, imgui-src, box3d-src, meshoptimizer-src, steamaudio-sdk }:
    flake-utils.lib.eachSystem [ "x86_64-linux" ] (system:
      let
        pkgs = import nixpkgs {
          inherit system;
          config.allowUnfree = true;
        };
        llvm = pkgs.llvmPackages_22;
        stdenv = llvm.libcxxStdenv;

        phonon = pkgs.stdenv.mkDerivation {
          pname = "steam-audio";
          version = "4.8.1";
          src = steamaudio-sdk;
          nativeBuildInputs = [ pkgs.autoPatchelfHook ];
          buildInputs = [ pkgs.stdenv.cc.cc.lib ];
          dontConfigure = true;
          dontBuild = true;
          installPhase = ''
            mkdir -p $out/lib
            cp lib/linux-x64/libphonon.so $out/lib/
          '';
        };

        libs = with pkgs; [
          vulkan-headers
          vulkan-loader
          vulkan-validation-layers
          sdl3
          zstd
          zlib
          assimp
          glslang
          shaderc
          spirv-tools
          spirv-headers
          openxr-loader
          onnxruntime
          libx11
          wayland
          libxkbcommon
        ];
      in
      {
        devShells.default = pkgs.mkShell.override { inherit stdenv; } {
          nativeBuildInputs = [
            pkgs.cmake
            pkgs.ninja
            pkgs.pkg-config
            (llvm.clang-tools.override { clang = llvm.libcxxClang; })
            llvm.lld
          ];
          buildInputs = libs ++ [ phonon ];
          # _FORTIFY_SOURCE turns glibc's printf family into internal-linkage inlines, which libc++'s std.cppm cannot
          # export (`import std;` fails to build).
          hardeningDisable = [ "fortify" "fortify3" ];

          OC_EASTL_SRC = "${eastl-src}";
          OC_IMGUI_SRC = "${imgui-src}";
          OC_BOX3D_SRC = "${box3d-src}";
          OC_MESHOPTIMIZER_SRC = "${meshoptimizer-src}";
          OC_PHONON_LIB = "${phonon}/lib";
          OC_ONNXRUNTIME = "${pkgs.onnxruntime}";
          OC_ONNXRUNTIME_DEV = "${pkgs.onnxruntime.dev}";
          # The wrapped scanner: the unwrapped one does not see the libc++ / glibc include paths the clang wrapper adds.
          OC_CLANG_SCAN_DEPS = "${llvm.clang-tools.override { clang = llvm.libcxxClang; }}/bin/clang-scan-deps";
          # std.cppm / std.compat.cppm: built as an ordinary module target (CMake's own `import std` is still behind a
          # per-version experimental gate).
          OC_LIBCXX_MODULES = "${llvm.libcxx}/share/libc++/v1";
          # Searched BEFORE Dependencies/Include: these libraries link from nixpkgs, so their headers must come from
          # nixpkgs too (the vendored copies match the Windows .libs, not these).
          OC_NIX_INCLUDES = pkgs.lib.concatMapStringsSep ";" (p: "${pkgs.lib.getDev p}/include")
            (with pkgs; [ glslang shaderc spirv-tools assimp onnxruntime sdl3 openxr-loader zstd zlib ]);
          LD_LIBRARY_PATH = pkgs.lib.makeLibraryPath (libs ++ [ phonon ]);
          VK_LAYER_PATH = "${pkgs.vulkan-validation-layers}/share/vulkan/explicit_layer.d";

          shellHook = ''
            export CC=clang
            export CXX=clang++
          '';
        };
      });
}
