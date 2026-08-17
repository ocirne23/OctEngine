// The one EASTL hook that is not an allocation: EASTL declares it and never defines it, so it lives
// here in Core's single plain TU. Its two operator new[] forms live next to the engine's other
// global operator new definitions, in Core.Allocator.
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

// EASTL's string formatting (basic_string::sprintf, to_string) calls this; it normally comes from
// EAStdC, which is not vendored. The CRT does the job.
namespace EA
{
namespace StdC
{
int Vsnprintf(char* __restrict destination, size_t count, const char* __restrict format, va_list arguments)
{
    return vsnprintf(destination, count, format, arguments);
}
}
}
