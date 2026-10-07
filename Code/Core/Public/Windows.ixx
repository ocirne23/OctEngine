export module Core.Windows;

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