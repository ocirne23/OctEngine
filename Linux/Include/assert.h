// Linux/Include is searched before the system headers, so every <assert.h> / <cassert> lands here first. No include
// guard: <assert.h> is meant to be included again (it re-reads NDEBUG each time).
//
// forceinclude.h makes a non-Debug assert `(void)sizeof(expression)` on Windows: the expression is still type-checked
// and a variable that only an assert reads still counts as used. glibc's NDEBUG assert is `(void)0`, so after any
// header that pulls in <assert.h> (a global module fragment does) those variables would be unused. Same form here.
#include_next <assert.h>

#ifdef NDEBUG
    #undef assert
    #define assert(expression) ((void)sizeof(expression))
#endif
