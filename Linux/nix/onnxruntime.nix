# ONNX Runtime 1.24.4, Microsoft's prebuilt Linux GPU binaries (the NuGet package; the GitHub release page is gone). 1.24 is
# the version of Dependencies/Include/onnxruntime (ORT_API_VERSION 24), so the vendored headers match it. The diffusion
# terrain runs on the CUDA provider where Windows uses DirectML. TensorRT's provider is left out (it needs TensorRT).
#
# cudaSupport = false: the same libonnxruntime.so without the CUDA provider and the ~3 GiB of CUDA libraries it loads
# (libonnxruntime.so itself links no CUDA library). Enough to build App; the diffusion terrain then has no provider.
{
  lib,
  stdenv,
  fetchurl,
  unzip,
  autoPatchelfHook,
  addDriverRunpath,
  cudaPackages_12,
  cudaSupport ? true,
}:
let
  cuda = cudaPackages_12;
in
stdenv.mkDerivation (
  {
    pname = if cudaSupport then "onnxruntime-gpu-bin" else "onnxruntime-bin";
    version = "1.24.4";
    src = fetchurl {
      url = "https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime.gpu.linux/1.24.4/microsoft.ml.onnxruntime.gpu.linux.1.24.4.nupkg";
      hash = "sha256-BlQIR7T4PMX9klYiYxImWUUs9ieD6aFHe2AAstlHtUI=";
    };
    nativeBuildInputs = [
      unzip
      autoPatchelfHook
    ]
    ++ lib.optional cudaSupport addDriverRunpath;
    buildInputs = [
      stdenv.cc.cc.lib
    ]
    ++ lib.optionals cudaSupport [
      cuda.cuda_cudart
      cuda.libcublas
      cuda.libcurand
      cuda.libcufft
      cuda.cudnn
    ];
    unpackPhase = "unzip -q $src 'runtimes/linux-x64/native/*'";
    dontConfigure = true;
    dontBuild = true;
    installPhase =
      if cudaSupport then
        ''
          mkdir -p $out/lib
          cp runtimes/linux-x64/native/libonnxruntime.so \
             runtimes/linux-x64/native/libonnxruntime_providers_shared.so \
             runtimes/linux-x64/native/libonnxruntime_providers_cuda.so $out/lib/
          ln -s libonnxruntime.so $out/lib/libonnxruntime.so.1
        ''
      else
        ''
          mkdir -p $out/lib
          cp runtimes/linux-x64/native/libonnxruntime.so \
             runtimes/linux-x64/native/libonnxruntime_providers_shared.so $out/lib/
          ln -s libonnxruntime.so $out/lib/libonnxruntime.so.1
        '';
  }
  // lib.optionalAttrs cudaSupport {
    # libcuda.so.1 is the NVIDIA driver's (/run/opengl-driver/lib on NixOS)
    postFixup = "addDriverRunpath $out/lib/libonnxruntime_providers_cuda.so";
  }
)
