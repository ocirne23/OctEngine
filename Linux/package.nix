# The engine (App) built in the nix sandbox. App finds Assets/ by walking up from the working directory, so run it from
# the repo root: `nix run ./Linux`.
{
  common,
  src,
  addDriverRunpath,
}:
common.stdenv.mkDerivation {
  pname = "octengine";
  version = "0-unstable";
  inherit src;
  inherit (common) buildInputs hardeningDisable env;
  nativeBuildInputs = common.nativeBuildInputs ++ [ addDriverRunpath ];

  cmakeBuildType = "RelWithDebInfo";
  ninjaFlags = [ "App" ];

  # App, with the DLSS shim and model next to it (App and NGX look for them there).
  installPhase = ''
    runHook preInstall
    mkdir -p $out/bin
    cp Code/App/App Code/App/libocngx.so $out/bin/
    runHook postInstall
  '';
  # The shim loads the driver's NGX core (libnvidia-ngx.so.1, /run/opengl-driver/lib on NixOS). The DLSS model goes in
  # after the fixup: patchelf and strip would change NVIDIA's file.
  postFixup = ''
    addDriverRunpath $out/bin/libocngx.so
    cp $OC_NGX_SDK/lib/Linux_x86_64/rel/libnvidia-ngx-dlss.so.* $out/bin/
  '';

  meta = {
    description = "OctEngine testbed (App)";
    mainProgram = "App";
    platforms = [ "x86_64-linux" ];
  };
}
