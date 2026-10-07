module Core.VirtualArray;

import Core;
import Core.Windows;

void* virtualReserve(size_t bytes)
{
    void* base = VirtualAlloc(nullptr, bytes, MEM_RESERVE, PAGE_READWRITE);
    if (!base)
    {
        printf("VirtualArray: reserving %zu bytes of address space failed\n", bytes);
        std::abort();
    }
    return base;
}

void virtualCommit(void* base, size_t bytes)
{
    if (!VirtualAlloc(base, bytes, MEM_COMMIT, PAGE_READWRITE))
    {
        printf("VirtualArray: committing %zu bytes failed (out of commit charge)\n", bytes);
        std::abort();
    }
}

void virtualRelease(void* base)
{
    VirtualFree(base, 0, MEM_RELEASE);
}
