# Unused: all dependencies are prebuilt into Include/Lib/Dll, and NO source is kept in this folder.
# What follows are the recipes to rebuild the prebuilt libs from an upstream clone.
#
# box3d (box3d.lib / box3dd.lib in Lib, headers in Include/box3d): build it standalone with the DLL
# CRT ("MultiThreaded$<$<CONFIG:Debug>:Debug>DLL", its own CMakeLists defaults to the static CRT) and
# copy the outputs over.
#
# Steam Audio (phonon) (miniaudio needs no build, it's a single header in Include/miniaudio).
# Rebuilding Lib/phonon(d).lib from a steam-audio clone:
# 1. Provision core/deps (gitignored) — build each at the pinned SHA from core/build/dependencies.json
#    with the DLL CRT (their CMake default; old CMakeLists need "-DCMAKE_POLICY_VERSION_MINIMUM=3.5"):
#    zlib (zlibstatic target), pffft (PFFFT target, -DTARGET_C_ARCH=AVX), mysofa (point ZLIB_* at the
#    zlib dep; also copy the generated mysofa_export.h next to mysofa.h), flatbuffers (flatc only).
#    Layout: deps/<name>/include + deps/<name>/lib/windows-x64/{release,debug}; flatc goes in
#    deps/flatbuffers/bin/windows-x64. See FindXXX.cmake in core/build for what is expected where.
# 2. cmake -B core/build-x64 -S core -DBUILD_SHARED_LIBS=OFF -DSTEAMAUDIO_STATIC_RUNTIME=OFF
#      -DSTEAMAUDIO_BUILD_TESTS=OFF -DSTEAMAUDIO_BUILD_BENCHMARKS=OFF -DSTEAMAUDIO_BUILD_SAMPLES=OFF
#      -DSTEAMAUDIO_BUILD_ITESTS=OFF -DSTEAMAUDIO_BUILD_DOCS=OFF -DSTEAMAUDIO_ENABLE_IPP=OFF
#      -DSTEAMAUDIO_ENABLE_MKL=OFF -DSTEAMAUDIO_ENABLE_EMBREE=OFF -DSTEAMAUDIO_ENABLE_RADEONRAYS=OFF
#      -DSTEAMAUDIO_ENABLE_TRUEAUDIONEXT=OFF -DZLIB_INCLUDE_DIR=... -DZLIB_LIBRARY_RELEASE=... -DZLIB_LIBRARY_DEBUG=...
#    then build the phonon target in Release + Debug.
# 3. lib.exe-merge phonon.lib + pffft.lib + mysofa.lib + zlibstatic.lib per config into
#    Lib/phonon.lib / Lib/phonond.lib, and copy src/core/phonon.h + the generated
#    build-x64/src/core/phonon_version.h to Include/phonon.
#
# meshoptimizer (v0.24, https://github.com/zeux/meshoptimizer). Rebuilding
# Lib/meshoptimizer(d).lib: configure its own CMakeLists with the DLL CRT
# ("-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"), build the meshoptimizer
# target in Release + Debug, copy the .libs over (Debug -> meshoptimizerd.lib) and src/meshoptimizer.h
# to Include/meshoptimizer.
#
# zstd source is NOT kept here (v1.5.7, https://github.com/facebook/zstd). Rebuilding Lib/zstd(d).lib:
# clone the tag, then
# cmake -B <tmp> -S zstd/build/cmake -DCMAKE_MSVC_RUNTIME_LIBRARY="MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
#   -DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_TESTS=OFF -DZSTD_BUILD_SHARED=OFF -DZSTD_BUILD_STATIC=ON
#   -DZSTD_LEGACY_SUPPORT=OFF -DZSTD_MULTITHREAD_SUPPORT=OFF
# then build the libzstd_static target in Release + Debug and copy lib/<Config>/zstd_static.lib over
# (Debug -> zstdd.lib), plus lib/zstd.h and lib/zstd_errors.h to Include/zstd.
