# Steam Audio: the prebuilt libphonon.so of the SDK release (the same version as Dependencies/Include/phonon).
{
  stdenv,
  autoPatchelfHook,
  src,
}:
stdenv.mkDerivation {
  pname = "steam-audio";
  version = "4.8.1";
  inherit src;
  nativeBuildInputs = [ autoPatchelfHook ];
  buildInputs = [ stdenv.cc.cc.lib ];
  dontConfigure = true;
  dontBuild = true;
  installPhase = ''
    mkdir -p $out/lib
    cp lib/linux-x64/libphonon.so $out/lib/
  '';
}
