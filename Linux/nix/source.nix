# The files the engine build reads, out of the whole repo (Assets/ alone is 8 GB): the code, the vendored headers, the Linux
# build files, and the two Assets folders CMake reads (the shaders: a header file set of RendererVK; the .dsl scripts that
# the static AppScripts target compiles in). A change anywhere else does not rebuild the package.
{ lib }:
root:
lib.fileset.toSource {
  inherit root;
  fileset = lib.fileset.unions [
    (root + "/CMakeLists.txt")
    (root + "/Utils.cmake")
    (root + "/Code")
    (root + "/Dependencies/Include")
    (root + "/Linux/Linux.cmake")
    (root + "/Linux/Include")
    (root + "/Linux/Ngx")
    (root + "/Assets/Shaders")
    (lib.fileset.fileFilter (f: f.hasExt "dsl") (root + "/Assets/Scripts"))
  ];
}
