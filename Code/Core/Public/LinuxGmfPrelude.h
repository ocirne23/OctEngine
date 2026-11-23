#pragma once

// Linux (clang) only: the C and C++ system headers that third-party headers pull in, included TEXTUALLY in a global
// module fragment before a module includes such a header inside its purview (`export extern "C++" { #include ... }`,
// the clang stand-in for MSVC's header units). Their include guards then keep the system headers out of the module
// purview, where they would clash with `import std;`.
#ifndef _MSC_VER
#include <cassert>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <ctime>
#include <functional>
#include <initializer_list>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>
#endif
