#pragma once

#include <cassert>
#include <cstdlib>
#include "InitSeg.h" // OC_INIT_SEG + the engine-wide static-init ordering table

// On the definition of a type that a *.fwd.ixx forward-declares inside `export extern "C++"` (OC_FWD_ATTACH), and
// around the out-of-line definitions of its members (OC_FWD_ATTACH_BEGIN / _END). MSVC lets them stay in the module
// purview; clang (Linux, where these are `extern "C++"` and `extern "C++" {` / `}`, see Linux/Linux.cmake) requires the
// same global-module attachment as the forward declaration.
#define OC_FWD_ATTACH
#define OC_FWD_ATTACH_BEGIN
#define OC_FWD_ATTACH_END

#ifdef NDEBUG
    #undef assert
    #define assert(expression) ((void)sizeof(expression))
#endif