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

        # ONNX Runtime 1.24.4 with the CUDA execution provider, Microsoft's prebuilt Linux binaries (the NuGet package; the
        # GitHub release page is gone). 1.24 is the version of Dependencies/Include/onnxruntime (ORT_API_VERSION 24), so
        # the vendored headers match it. The diffusion terrain runs on the CUDA provider where Windows uses DirectML.
        # TensorRT's provider is left out (it needs TensorRT).
        cuda = pkgs.cudaPackages_12;
        onnxruntime-gpu = pkgs.stdenv.mkDerivation {
          pname = "onnxruntime-gpu-bin";
          version = "1.24.4";
          src = pkgs.fetchurl {
            url = "https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime.gpu.linux/1.24.4/microsoft.ml.onnxruntime.gpu.linux.1.24.4.nupkg";
            hash = "sha256-BlQIR7T4PMX9klYiYxImWUUs9ieD6aFHe2AAstlHtUI=";
          };
          nativeBuildInputs = [ pkgs.unzip pkgs.autoPatchelfHook pkgs.addDriverRunpath ];
          buildInputs = [ pkgs.stdenv.cc.cc.lib cuda.cuda_cudart cuda.libcublas cuda.libcurand cuda.libcufft cuda.cudnn ];
          unpackPhase = "unzip -q $src 'runtimes/linux-x64/native/*'";
          dontConfigure = true;
          dontBuild = true;
          installPhase = ''
            mkdir -p $out/lib
            cp runtimes/linux-x64/native/libonnxruntime.so \
               runtimes/linux-x64/native/libonnxruntime_providers_shared.so \
               runtimes/linux-x64/native/libonnxruntime_providers_cuda.so $out/lib/
            ln -s libonnxruntime.so $out/lib/libonnxruntime.so.1
          '';
          # libcuda.so.1 is the NVIDIA driver's (/run/opengl-driver/lib on NixOS)
          postFixup = "addDriverRunpath $out/lib/libonnxruntime_providers_cuda.so";
        };

        # NVIDIA DLSS SDK (NGX) 310.9.16, the Linux parts only: the headers, the static NGX loader and the release DLSS model.
        # Linux/Linux.cmake builds Linux/Ngx/ocngx.cpp against it with GCC (the loader needs libstdc++) and puts the shim and
        # the model next to App. Distributed under the NVIDIA RTX SDK license (Dependencies/Dll/nvngx_dlss.license.txt).
        ngx-sdk = pkgs.fetchgit {
          url = "https://github.com/NVIDIA/DLSS.git";
          rev = "refs/tags/v310.9.16";
          sparseCheckout = [ "include" "lib/Linux_x86_64/libnvsdk_ngx.a" "lib/Linux_x86_64/rel/libnvidia-ngx-dlss.so.310.9.16" ];
          nonConeMode = true;
          hash = "sha256-Avu0jXlZv70+iS9A6IDF4RGhRiDWy3QbpgflipAItKw=";
        };

        # The libraries the engine calls through a C++ API (std::string / std::vector in the signatures) must be built
        # against libc++ like the engine; nixpkgs builds them against libstdc++, whose std:: names mangle differently.
        withLibcxx = drv: (drv.override { stdenv = llvm.libcxxStdenv; }).overrideAttrs { doCheck = false; };
        spirv-tools = withLibcxx pkgs.spirv-tools;
        glslang = (pkgs.glslang.override { stdenv = llvm.libcxxStdenv; inherit spirv-tools; }).overrideAttrs { doCheck = false; };
        shaderc = (pkgs.shaderc.override { stdenv = llvm.libcxxStdenv; inherit glslang spirv-tools; }).overrideAttrs { doCheck = false; };
        assimp = (pkgs.assimp.override { stdenv = llvm.libcxxStdenv; pugixml = withLibcxx pkgs.pugixml; }).overrideAttrs { doCheck = false; };

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
          onnxruntime-gpu
          openssl
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
          # format: -Werror=format-security trips over the engine's printf(text) logging; stackprotector: the engine
          # builds with /GS- on Windows (no stack cookies), -fno-stack-protector here.
          hardeningDisable = [ "fortify" "fortify3" "format" "stackprotector" ];

          OC_EASTL_SRC = "${eastl-src}";
          OC_IMGUI_SRC = "${imgui-src}";
          OC_BOX3D_SRC = "${box3d-src}";
          OC_MESHOPTIMIZER_SRC = "${meshoptimizer-src}";
          OC_PHONON_LIB = "${phonon}/lib";
          OC_ONNXRUNTIME_LIB = "${onnxruntime-gpu}/lib";
          OC_NGX_SDK = "${ngx-sdk}";
          OC_NGX_CXX = "${pkgs.gcc}/bin/g++";
          # The wrapped scanner: the unwrapped one does not see the libc++ / glibc include paths the clang wrapper adds.
          OC_CLANG_SCAN_DEPS = "${llvm.clang-tools.override { clang = llvm.libcxxClang; }}/bin/clang-scan-deps";
          # std.cppm / std.compat.cppm: built as an ordinary module target (CMake's own `import std` is still behind a
          # per-version experimental gate).
          OC_LIBCXX_MODULES = "${llvm.libcxx}/share/libc++/v1";
          # The include folders of the libraries that link from nixpkgs (their vendored copies match the Windows .libs).
          OC_NIX_INCLUDES = pkgs.lib.concatMapStringsSep ";" (p: "${pkgs.lib.getDev p}/include")
            ([ glslang spirv-tools assimp shaderc ] ++ (with pkgs; [ sdl3 openxr-loader zstd zlib openssl ]));
          # /run/opengl-driver/lib: the NVIDIA driver's libcuda for the CUDA provider (NixOS).
          LD_LIBRARY_PATH = pkgs.lib.makeLibraryPath (libs ++ [ phonon ]) + ":/run/opengl-driver/lib";
          VK_LAYER_PATH = "${pkgs.vulkan-validation-layers}/share/vulkan/explicit_layer.d";

          shellHook = ''
            export CC=clang
            export CXX=clang++
          '';
        };
      });
}
