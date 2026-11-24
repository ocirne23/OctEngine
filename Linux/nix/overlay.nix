# The packages the engine links that nixpkgs does not have in the form the engine needs, under pkgs.octengine. A separate
# attribute set on purpose: replacing pkgs.glslang & co. would rebuild every nixpkgs package that uses them (the Vulkan
# validation layers, ...) against libc++.
{ steamaudio-sdk }:
final: prev:
let
  llvm = final.llvmPackages_22;
  # The libraries the engine calls through a C++ API (std::string / std::vector in the signatures) must be built against
  # libc++ like the engine; nixpkgs builds them against libstdc++, whose std:: names mangle differently.
  withLibcxx =
    drv: args:
    (drv.override ({ stdenv = llvm.libcxxStdenv; } // args)).overrideAttrs { doCheck = false; };
in
{
  octengine = rec {
    spirv-tools = withLibcxx prev.spirv-tools { };
    glslang = withLibcxx prev.glslang { inherit spirv-tools; };
    shaderc = withLibcxx prev.shaderc { inherit glslang spirv-tools; };
    pugixml = withLibcxx prev.pugixml { };
    assimp = withLibcxx prev.assimp { inherit pugixml; };

    # SDL3 without the zenity dialogs (216 MiB of GTK; the engine shows no SDL dialog, and SDL reports an error in place of
    # one) and without the tray (libayatana-appindicator). libdecor stays: it draws the Wayland window decorations.
    sdl3 = prev.sdl3.override {
      traySupport = false;
      zenity = final.writeShellScriptBin "zenity" "exit 1";
    };

    phonon = final.callPackage ./phonon.nix { src = steamaudio-sdk; };
    onnxruntime-gpu = final.callPackage ./onnxruntime.nix { };
    onnxruntime = final.callPackage ./onnxruntime.nix { cudaSupport = false; };
  };
}
