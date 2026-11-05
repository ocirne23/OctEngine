import Core;

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
