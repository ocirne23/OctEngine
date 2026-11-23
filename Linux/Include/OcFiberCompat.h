#pragma once

// Linux (x86-64 System V) stand-ins for the Win32 fiber and thread calls of Threading's JobSystem.cpp, under the same
// names, so the scheduler code is the same on both platforms. Included ONLY by JobSystem.cpp's global module fragment:
// the context switch below is a top-level asm definition, so a second including TU would define it twice.
//
// A fiber here is a saved stack pointer. The switch saves the callee-saved registers plus MXCSR and the x87 control
// word on the old stack (the FIBER_FLAG_FLOAT_SWITCH behaviour), stores the stack pointer, loads the target's and
// restores the same set from it. Everything else is caller-saved under the System V ABI.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <ctime>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <linux/futex.h>
#include <unistd.h>

// DWORD / SIZE_T are not declared here: Core.Windows exports them on Linux, and JobSystem.cpp imports it.
using HANDLE = void*;
using LPVOID = void*;
using LPFIBER_START_ROUTINE = void (*)(void*);

struct OcFiber
{
    void* stackPointer = nullptr;
    void* mapping = nullptr;      // nullptr for a converted thread (it runs on the thread's own stack)
    size_t mappingSize = 0;
};

extern "C" void oc_fiber_switch(void** saveStackPointer, void* loadStackPointer);
extern "C" void oc_fiber_trampoline();

__asm__(
    ".text\n"
    ".globl oc_fiber_switch\n"
    ".type oc_fiber_switch,@function\n"
    "oc_fiber_switch:\n"
    "    pushq %rbp\n"
    "    pushq %rbx\n"
    "    pushq %r12\n"
    "    pushq %r13\n"
    "    pushq %r14\n"
    "    pushq %r15\n"
    "    subq $16, %rsp\n"
    "    stmxcsr 8(%rsp)\n"
    "    fnstcw 12(%rsp)\n"
    "    movq %rsp, (%rdi)\n"
    "    movq %rsi, %rsp\n"
    "    ldmxcsr 8(%rsp)\n"
    "    fldcw 12(%rsp)\n"
    "    addq $16, %rsp\n"
    "    popq %r15\n"
    "    popq %r14\n"
    "    popq %r13\n"
    "    popq %r12\n"
    "    popq %rbx\n"
    "    popq %rbp\n"
    "    ret\n"
    ".size oc_fiber_switch, .-oc_fiber_switch\n"
    // First entry of a new fiber: r12 = start routine, r13 = its parameter. A fiber routine never returns.
    ".globl oc_fiber_trampoline\n"
    ".type oc_fiber_trampoline,@function\n"
    "oc_fiber_trampoline:\n"
    "    movq %r13, %rdi\n"
    "    andq $-16, %rsp\n"
    "    callq *%r12\n"
    "    ud2\n"
    ".size oc_fiber_trampoline, .-oc_fiber_trampoline\n");

// The fiber running on this thread. Read and written only inside the non-inlined calls below, so no caller can keep
// its address across a switch that moves the caller to another thread.
inline thread_local OcFiber* t_ocCurrentFiber = nullptr;

[[gnu::noinline]] inline void* CreateFiberEx(size_t, size_t reserveSize, uint32_t, LPFIBER_START_ROUTINE start, LPVOID parameter)
{
    const size_t page = size_t(sysconf(_SC_PAGESIZE));
    const size_t size = (reserveSize + page - 1) / page * page + page; // + a guard page at the bottom
    void* mapping = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (mapping == MAP_FAILED)
        return nullptr;
    mprotect(mapping, page, PROT_NONE);

    OcFiber* fiber = new OcFiber;
    fiber->mapping = mapping;
    fiber->mappingSize = size;
    // The initial frame oc_fiber_switch pops: [mxcsr/fcw 16][r15][r14][r13][r12][rbx][rbp][ret = trampoline][0].
    uintptr_t top = (reinterpret_cast<uintptr_t>(mapping) + size) & ~uintptr_t(15);
    uint64_t* slot = reinterpret_cast<uint64_t*>(top);
    *--slot = 0;                                                     // fake return address of the trampoline
    *--slot = reinterpret_cast<uint64_t>(&oc_fiber_trampoline);      // ret target
    *--slot = 0;                                                     // rbp
    *--slot = 0;                                                     // rbx
    *--slot = reinterpret_cast<uint64_t>(start);                     // r12
    *--slot = reinterpret_cast<uint64_t>(parameter);                 // r13
    *--slot = 0;                                                     // r14
    *--slot = 0;                                                     // r15
    *--slot = (uint64_t(0x037F) << 32) | 0x1F80;                     // mxcsr (low dword at +8), x87 cw (at +12)
    *--slot = 0;
    fiber->stackPointer = slot;
    return fiber;
}

[[gnu::noinline]] inline void* ConvertThreadToFiberEx(LPVOID, uint32_t)
{
    OcFiber* fiber = new OcFiber;
    t_ocCurrentFiber = fiber;
    return fiber;
}

[[gnu::noinline]] inline int ConvertFiberToThread()
{
    delete t_ocCurrentFiber;
    t_ocCurrentFiber = nullptr;
    return 1;
}

[[gnu::noinline]] inline void SwitchToFiber(void* target)
{
    OcFiber* self = t_ocCurrentFiber;
    OcFiber* next = static_cast<OcFiber*>(target);
    t_ocCurrentFiber = next;
    oc_fiber_switch(&self->stackPointer, next->stackPointer);
    // resumed here, possibly on another thread: nothing below may touch thread-local state
}

inline void DeleteFiber(void* handle)
{
    OcFiber* fiber = static_cast<OcFiber*>(handle);
    if (fiber->mapping)
        munmap(fiber->mapping, fiber->mappingSize);
    delete fiber;
}

// WaitOnAddress with a millisecond timeout. libc++'s atomic notify on a 4-byte atomic wakes a futex on the atomic's own
// address, so the engine's oc::atomic::notify_all reaches this wait.
inline int WaitOnAddress(volatile void* address, void* compare, size_t size, uint32_t milliseconds)
{
    if (size != 4)
        return 0;
    const timespec timeout{ time_t(milliseconds / 1000), long(milliseconds % 1000) * 1000000L };
    syscall(SYS_futex, const_cast<void*>(address), FUTEX_WAIT_PRIVATE, *static_cast<uint32_t*>(compare), &timeout, nullptr, 0);
    return 1;
}

inline HANDLE GetCurrentThread() { return reinterpret_cast<HANDLE>(pthread_self()); }
// Raising a thread's priority needs CAP_SYS_NICE on Linux; the workers and main share one priority.
inline int SetThreadPriority(HANDLE, int) { return 1; }
inline long SetThreadDescription(HANDLE thread, const wchar_t* name)
{
    char narrow[16] = {}; // the kernel's limit, terminator included
    for (int i = 0; i < 15 && name[i]; ++i)
        narrow[i] = char(name[i]);
    pthread_setname_np(reinterpret_cast<pthread_t>(thread), narrow);
    return 0;
}

// A thread_local whose every access is a fresh, non-inlined TLS lookup: the Linux answer to MSVC's /GT for the job
// system's own thread state. A fiber resumed on another worker re-reads the slot of the thread it now runs on.
template<typename T, int Id>
struct OcFiberSafeTls
{
    [[gnu::noinline]] static T& slot() noexcept
    {
        static thread_local T value{};
        asm volatile("" ::: "memory");
        return value;
    }
    operator T() const noexcept { return slot(); }
    OcFiberSafeTls& operator=(T value) noexcept { slot() = value; return *this; }
    T operator->() const noexcept { return slot(); }
    T* operator&() const noexcept { return &slot(); } // this thread's slot, valid until the next park
    T operator++() noexcept { return ++slot(); }
    T operator--() noexcept { return --slot(); }
    T operator++(int) noexcept { return slot()++; }
    T operator--(int) noexcept { return slot()--; }
    OcFiberSafeTls& operator+=(T value) noexcept { slot() += value; return *this; }
};
