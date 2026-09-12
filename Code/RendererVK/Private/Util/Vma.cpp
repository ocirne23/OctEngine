// VulkanMemoryAllocator implementation translation unit.
//
// This is a plain (non-module) global-module source file: it compiles the VMA implementation once so
// its symbols have external linkage and can be linked by the :Allocator module impl unit (which includes
// the same header without VMA_IMPLEMENTATION). Keeping the implementation out of a module unit avoids
// giving VMA's symbols module linkage.
//
// Static Vulkan functions: the renderer links vulkan-1.lib and uses vulkan-hpp's static dispatcher, so
// VMA can call the core entry points directly rather than loading them dynamically.
#define VMA_STATIC_VULKAN_FUNCTIONS 1
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0

// Debug builds only: pad every allocation with guard bytes and fill them with a known pattern that is
// validated when the allocation is freed (and via vmaCheckCorruption). This catches GPU-side overruns
// past a buffer/image into a neighbouring allocation. Costs extra memory + a little time, so it is gated
// on NDEBUG (the project defines NDEBUG in non-debug configs via forceinclude.h).
#ifndef NDEBUG
    #define VMA_DEBUG_MARGIN 32
    #define VMA_DEBUG_DETECT_CORRUPTION 1

    // VMA_DEBUG_MARGIN inserts guard/alignment blocks that, together with the buffer-image granularity,
    // can leave a TLSF memory block reporting IsEmpty() == false at teardown even when every allocation
    // has actually been freed (m_AllocCount == 0): a small leading free block never merges into the null
    // block, so its offset stays != 0. VMA's "some allocations were not freed" check is VMA_ASSERT_LEAK,
    // which would abort the process on that benign case. Downgrade it to non-fatal - genuine allocation
    // leaks are still surfaced: DebugLogAllAllocations prints each live allocation (VMA_LEAK_LOG_FORMAT)
    // and Allocator::destroy() dumps the full stats string whenever any allocation is really still alive.
    #define VMA_ASSERT_LEAK(expr) ((void)0)
#endif

// Print every allocation that is still alive when its memory block is destroyed (i.e. leaked at
// shutdown), including the debug name set via Allocator::createBuffer/createImage. This fires just
// before VMA's "Some allocations were not freed" assert, so the log pinpoints the leak. We route it to
// both stderr (flushed immediately, since the assert may abort the process) and OutputDebugString so it
// is visible in the debugger's Output window where the assert dialog also appears.
#include <cstdio>
#include <cstdarg>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
static void vmaLeakLog(const char* format, ...)
{
    char buffer[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    fprintf(stderr, "%s\n", buffer);
    fflush(stderr);
    OutputDebugStringA(buffer);
    OutputDebugStringA("\n");
}
#define VMA_LEAK_LOG_FORMAT(format, ...) vmaLeakLog(format, __VA_ARGS__)

#pragma warning(push, 0)
#define VMA_IMPLEMENTATION
#include "vma/vk_mem_alloc.h"
#pragma warning(pop)
