module;

#ifndef _MSC_VER
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#endif

export module Core.Windows;

#ifndef _MSC_VER
// Linux: no Win32. This exports POSIX implementations of the few Win32 calls whose meaning carries over one to one
// (sleep, the performance counter, the thread id, reserve/commit address space), so their callers stay as they are.
// Everything with no one-to-one POSIX meaning (fibers, processes, file handles, DLLs) is under _MSC_VER at its call site.
export using BOOL = int;
export using DWORD = uint32_t;
export using SIZE_T = size_t;
export inline constexpr BOOL  FALSE = 0;
export inline constexpr BOOL  TRUE = 1;
export inline constexpr DWORD INFINITE = 0xFFFFFFFF;
export inline constexpr DWORD MEM_COMMIT = 0x00001000;
export inline constexpr DWORD MEM_RESERVE = 0x00002000;
export inline constexpr DWORD MEM_RELEASE = 0x00008000;
export inline constexpr DWORD PAGE_READWRITE = 0x04;

export union LARGE_INTEGER
{
    struct { uint32_t LowPart; int32_t HighPart; };
    long long QuadPart;
};

export inline void Sleep(DWORD milliseconds)
{
    const timespec duration{ time_t(milliseconds / 1000), long(milliseconds % 1000) * 1000000L };
    nanosleep(&duration, nullptr);
}

// CLOCK_MONOTONIC in nanoseconds: frequency 1e9.
export inline BOOL QueryPerformanceFrequency(LARGE_INTEGER* frequency)
{
    frequency->QuadPart = 1000000000LL;
    return TRUE;
}
export inline BOOL QueryPerformanceCounter(LARGE_INTEGER* counter)
{
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    counter->QuadPart = (long long)now.tv_sec * 1000000000LL + now.tv_nsec;
    return TRUE;
}

export inline DWORD GetCurrentThreadId() { return DWORD(syscall(SYS_gettid)); }

// Runtime-loaded libraries: dlopen. The optional vendor DLLs (Aftermath) are asked for by their Windows file names, which
// dlopen does not find, so those features stay off exactly as on a Windows machine without the DLL.
export using HMODULE = void*;
export inline HMODULE LoadLibraryA(const char* path) { return dlopen(path, RTLD_NOW | RTLD_LOCAL); }
export inline void* GetProcAddress(HMODULE module, const char* name) { return dlsym(module, name); }
export inline BOOL FreeLibrary(HMODULE module) { return dlclose(module) == 0 ? TRUE : FALSE; }
export inline DWORD GetEnvironmentVariableA(const char* name, char* buffer, DWORD size)
{
    const char* value = getenv(name);
    if (!value)
        return 0;
    const size_t length = strlen(value);
    if (length >= size)
        return DWORD(length + 1);
    memcpy(buffer, value, length + 1);
    return DWORD(length);
}
export inline void OutputDebugStringA(const char* text) { fputs(text, stderr); }

// The Windows scheduler tick; Linux nanosleep already has sub-millisecond resolution.
export inline unsigned timeBeginPeriod(unsigned) { return 0; }
export inline unsigned timeEndPeriod(unsigned) { return 0; }

// Reserve / commit / release, for VirtualArray. A reservation keeps its size in a header page in front of the base
// that is returned, because munmap needs the size and VirtualFree(MEM_RELEASE) does not pass it.
export inline void* VirtualAlloc(void* address, SIZE_T bytes, DWORD type, DWORD)
{
    const size_t page = size_t(sysconf(_SC_PAGESIZE));
    if (address == nullptr)
    {
        const size_t total = bytes + page;
        void* mapping = mmap(nullptr, total, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (mapping == MAP_FAILED)
            return nullptr;
        mprotect(mapping, page, PROT_READ | PROT_WRITE);
        *static_cast<size_t*>(mapping) = total;
        char* base = static_cast<char*>(mapping) + page;
        if ((type & MEM_COMMIT) && mprotect(base, bytes, PROT_READ | PROT_WRITE) != 0)
            return nullptr;
        return base;
    }
    if (type & MEM_COMMIT)
        return mprotect(address, bytes, PROT_READ | PROT_WRITE) == 0 ? address : nullptr;
    return address;
}
export inline BOOL VirtualFree(void* address, SIZE_T, DWORD type)
{
    if (!(type & MEM_RELEASE) || address == nullptr)
        return FALSE;
    char* mapping = static_cast<char*>(address) - size_t(sysconf(_SC_PAGESIZE));
    return munmap(mapping, *reinterpret_cast<size_t*>(mapping)) == 0 ? TRUE : FALSE;
}

// privateBytes = resident anonymous memory (RssAnon), workingSetBytes = resident set, both from /proc/self/status.
export inline bool getProcessMemoryUsage(SIZE_T& privateBytes, SIZE_T& workingSetBytes)
{
    FILE* status = fopen("/proc/self/status", "r");
    if (!status)
        return false;
    char line[256];
    size_t rssKb = 0, anonKb = 0;
    while (fgets(line, sizeof(line), status))
    {
        sscanf(line, "VmRSS: %zu kB", &rssKb);
        sscanf(line, "RssAnon: %zu kB", &anonKb);
    }
    fclose(status);
    privateBytes = anonKb * 1024;
    workingSetBytes = rssKb * 1024;
    return rssKb != 0;
}
#else
export import <Windows.h>;

// Win32 #defines don't cross the named-module boundary, so re-expose the ones we use as
// typed constants. (#undef first in case the header unit makes them active macros here.)
#undef FALSE
#undef TRUE
#undef INFINITE
#undef CREATE_NO_WINDOW
#undef MEM_COMMIT
#undef MEM_RESERVE
#undef MEM_RELEASE
#undef PAGE_READWRITE

export inline constexpr BOOL  FALSE = 0;
export inline constexpr BOOL  TRUE = 1;
export inline constexpr DWORD INFINITE = 0xFFFFFFFF;
export inline constexpr DWORD CREATE_NO_WINDOW = 0x08000000;
export inline constexpr DWORD MEM_COMMIT = 0x00001000;
export inline constexpr DWORD MEM_RESERVE = 0x00002000;
export inline constexpr DWORD MEM_RELEASE = 0x00008000;
export inline constexpr DWORD PAGE_READWRITE = 0x04;

// Whole-process memory counters (the Memory panel's header line). K32GetProcessMemoryInfo lives in
// kernel32 but is DECLARED in <Psapi.h>, which cannot be its own header unit (it needs Windows.h
// included first) - so declare the little we use here, layout-matching PROCESS_MEMORY_COUNTERS_EX.
struct ProcessMemoryCountersEx
{
    DWORD cb;
    DWORD PageFaultCount;
    SIZE_T PeakWorkingSetSize;
    SIZE_T WorkingSetSize;
    SIZE_T QuotaPeakPagedPoolUsage;
    SIZE_T QuotaPagedPoolUsage;
    SIZE_T QuotaPeakNonPagedPoolUsage;
    SIZE_T QuotaNonPagedPoolUsage;
    SIZE_T PagefileUsage;
    SIZE_T PeakPagefileUsage;
    SIZE_T PrivateUsage;
};
extern "C" __declspec(dllimport) BOOL __stdcall K32GetProcessMemoryInfo(HANDLE process, ProcessMemoryCountersEx* counters, DWORD cb);

// privateBytes = committed private memory (Task Manager's "Commit"), workingSetBytes = resident
// physical (Task Manager's default "Memory" column is the private working set, slightly below this).
export inline bool getProcessMemoryUsage(SIZE_T& privateBytes, SIZE_T& workingSetBytes)
{
    ProcessMemoryCountersEx counters{ .cb = sizeof(ProcessMemoryCountersEx) };
    if (!K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
        return false;
    privateBytes = counters.PrivateUsage;
    workingSetBytes = counters.WorkingSetSize;
    return true;
}
#endif