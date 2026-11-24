{
  description = "OctEngine - Linux (NixOS) dev shell";

  # The Windows build uses the prebuilt .libs in Dependencies/Lib. On Linux the system libraries come from nixpkgs, and the
  # libraries whose vendored headers carry a pinned upstream revision (or a local patch) are built from that exact
  # revision by Linux/Linux.cmake, against the vendored headers in Dependencies/Include.
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";

    # EASTL 3.27.01 + the OCTENGINE LOCAL PATCH (already in Dependencies/Include/EASTL; only source/*.cpp is used).
    eastl-src = {
      url = "github:electronicarts/EASTL/3.27.01";
      flake = false;
    };
    # imgui docking: the revision whose headers Dependencies/Include/imgui holds (plus the inline ImRect::Floor).
    imgui-src = {
      url = "github:ocornut/imgui/5a76f2adf1b0403b86a45010121fb32a6bff8680";
      flake = false;
    };
    # box3d: the revision whose include/box3d is Dependencies/Include/box3d.
    box3d-src = {
      url = "github:erincatto/box3d/8441b4a06d6d09dcfb0b0f704df4d847d1437b92";
      flake = false;
    };
    # meshoptimizer v0.24 (Dependencies/Include/meshoptimizer; nixpkgs carries 1.x).
    meshoptimizer-src = {
      url = "github:zeux/meshoptimizer/v0.24";
      flake = false;
    };
    # Steam Audio 4.8.1 SDK (prebuilt libphonon.so; the same version as Dependencies/Include/phonon).
    steamaudio-sdk = {
      url = "https://github.com/ValveSoftware/steam-audio/releases/download/v4.8.1/steamaudio_4.8.1.zip";
      flake = false;
    };
  };

  outputs =
    {
      self,
      nixpkgs,
      eastl-src,
      imgui-src,
      box3d-src,
      meshoptimizer-src,
      steamaudio-sdk,
    }:
    let
      system = "x86_64-linux";
      lib = nixpkgs.lib;
      pkgs = import nixpkgs {
        inherit system;
        overlays = [ self.overlays.default ];
        # The only unfree packages: the CUDA runtime that the ONNX Runtime CUDA provider loads.
        config.allowUnfreePredicate =
          pkg:
          builtins.elem (lib.getName pkg) [
            "cuda_cccl"
            "cuda_compat"
            "cuda_cudart"
            "cuda_nvcc"
            "cuda_nvrtc"
            "cudnn"
            "libcublas"
            "libcufft"
            "libcurand"
            "libnvjitlink"
          ];
      };
      sources = {
        inherit
          eastl-src
          imgui-src
          box3d-src
          meshoptimizer-src
          ;
      };
      common = cudaSupport: pkgs.callPackage ./nix/common.nix { inherit sources cudaSupport; };

      # The build reads the repo outside Linux/, so it needs the git form of the flake (`nix build ./Linux`); the path:
      # form cannot see ../. and fails here. The git form copies the repo into the store first: a clean tree as its commit
      # (about 100 MB, the Git LFS files as pointers), but a tree with ANY uncommitted change as its working tree with the
      # real LFS content (about 10.7 GB), again for every change. Commit or stash first. See Docs/Context/Linux (Packaging).
      engine =
        cudaSupport:
        pkgs.callPackage ./package.nix {
          common = common cudaSupport;
          src = import ./nix/source.nix { inherit lib; } ../.;
        };

      shell =
        cudaSupport:
        let
          c = common cudaSupport;
        in
        pkgs.mkShell.override { inherit (c) stdenv; } {
          inherit (c) nativeBuildInputs hardeningDisable env;
          buildInputs = c.buildInputs ++ [ pkgs.vulkan-validation-layers ];
          # Only the driver's folder (/run/opengl-driver/lib on NixOS: libcuda for the CUDA provider, the NGX core for
          # DLSS). App finds the nix libraries through its runpath; nix store paths here would also reach every other
          # program started from the shell (seen: the system curl loaded the flake's openssl).
          LD_LIBRARY_PATH = "/run/opengl-driver/lib";
          # mkShell sets $out to ./outputs/out, and the wrapper would add its lib folder to every runpath.
          NIX_NO_SELF_RPATH = true;
          # compile_commands.json in the build folder, for clangd (CMake reads this as the default of the cache variable).
          CMAKE_EXPORT_COMPILE_COMMANDS = "1";
          VK_LAYER_PATH = "${pkgs.vulkan-validation-layers}/share/vulkan/explicit_layer.d";

          shellHook = ''
            export CC=clang
            export CXX=clang++
          '';
        };
    in
    {
      overlays.default = import ./nix/overlay.nix { inherit steamaudio-sdk; };

      packages.${system} = {
        default = engine true;
        # Without the CUDA provider: no 3 GiB of CUDA libraries (not in the public binary cache), no diffusion terrain.
        nocuda = engine false;
        inherit (pkgs.octengine)
          spirv-tools
          glslang
          shaderc
          pugixml
          assimp
          sdl3
          phonon
          onnxruntime
          onnxruntime-gpu
          ;
      };

      apps.${system}.default = {
        type = "app";
        program = "${engine true}/bin/App";
        meta.description = "OctEngine testbed (App); run it from the repo root";
      };

      devShells.${system} = {
        default = shell true;
        # The build tools without CUDA, for a machine that only builds (CI).
        build = shell false;
      };

      checks.${system} = {
        build = engine false;
        format = pkgs.runCommand "nixfmt-check" { nativeBuildInputs = [ pkgs.nixfmt ]; } ''
          find ${./.} -name '*.nix' -exec nixfmt --check {} +
          touch $out
        '';
      };

      formatter.${system} = pkgs.nixfmt;
    };
}
