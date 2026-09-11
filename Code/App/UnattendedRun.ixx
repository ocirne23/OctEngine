export module App.UnattendedRun;

import Core;
import Core.Windows;

// FAILURE HANDLING FOR UNATTENDED RUNS (--quit-after / --profile-after: Tools/profile.ps1 and Claude's
// runs, nobody at the screen). Two problems it solves:
//   1. A Debug assert / abort / crash parked the process on a modal message box (Abort / Retry / Ignore,
//      or the OS fault box) until someone clicked it — the run hung instead of failing.
//   2. A crash died into a redirected log with its block buffer lost: no stack, often not even the last
//      lines.
// installUnattendedFailureHandling() routes the CRT's assert text to stderr, disables every dialog, and
// installs an unhandled-exception filter that prints the faulting thread's stack, symbolized from the
// PDB with file:line, to stderr. Interactive runs never call it, so a debugger session still gets the
// dialog / first-chance break as before.
//
// DbgHelp is declared here by hand: its header cannot be a header unit (it needs Windows.h included
// first), so the two structs below layout-match SYMBOL_INFO / IMAGEHLP_LINE64 and the lib rides a
// pragma so the App link stays untouched.
#pragma comment(lib, "dbghelp.lib")

namespace
{
    struct DbgSymbolInfo // SYMBOL_INFO
    {
        ULONG SizeOfStruct; ULONG TypeIndex; ULONG64 Reserved[2]; ULONG Index; ULONG Size; ULONG64 ModBase;
        ULONG Flags; ULONG64 Value; ULONG64 Address; ULONG Register; ULONG Scope; ULONG Tag; ULONG NameLen; ULONG MaxNameLen;
        char Name[256];
    };
    struct DbgLine64 // IMAGEHLP_LINE64
    {
        DWORD SizeOfStruct; void* Key; DWORD LineNumber; char* FileName; DWORD64 Address;
    };
}
extern "C" __declspec(dllimport) BOOL  __stdcall SymInitialize(HANDLE process, const char* searchPath, BOOL invadeProcess);
extern "C" __declspec(dllimport) DWORD __stdcall SymSetOptions(DWORD options);
extern "C" __declspec(dllimport) BOOL  __stdcall SymFromAddr(HANDLE process, DWORD64 address, DWORD64* displacement, DbgSymbolInfo* symbol);
extern "C" __declspec(dllimport) BOOL  __stdcall SymGetLineFromAddr64(HANDLE process, DWORD64 address, DWORD* displacement, DbgLine64* line);

namespace
{
    LONG __stdcall unattendedCrashFilter(EXCEPTION_POINTERS* info)
    {
        // ONE report: several job threads can fault together (the same bad pointer, every worker), and
        // DbgHelp is single-threaded — the second SymInitialize fails and the two reports interleave.
        // Later faulting threads park until the first one's report terminates the process.
        static oc::atomic<bool> s_reporting = false;
        if (s_reporting.exchange(true))
            Sleep(INFINITE);
        // stdout / stderr are <stdio.h> MACROS (they do not cross the module boundary); this is what they expand to.
        FILE* const out = __acrt_iob_func(1);
        FILE* const err = __acrt_iob_func(2);
        fflush(out);
        fprintf(err, "\nUNHANDLED EXCEPTION 0x%08X at %p\n", (unsigned)info->ExceptionRecord->ExceptionCode, info->ExceptionRecord->ExceptionAddress);
        const HANDLE process = GetCurrentProcess();
        SymSetOptions(0x00000002u /*SYMOPT_UNDNAME*/ | 0x00000010u /*SYMOPT_LOAD_LINES*/);
        if (!SymInitialize(process, nullptr, TRUE))
            fprintf(err, "  (SymInitialize failed: %lu)\n", (unsigned long)GetLastError());
        fprintf(err, "  module base %p (offsets below are absolute)\n", (void*)GetModuleHandleA(nullptr));
        void* frames[64];
        const USHORT count = RtlCaptureStackBackTrace(0, 64, frames, nullptr);
        for (USHORT i = 0; i < count; ++i)
        {
            DbgSymbolInfo sym{};
            sym.SizeOfStruct = 88; // sizeof(SYMBOL_INFO) without the name tail
            sym.MaxNameLen = sizeof(sym.Name) - 1;
            DWORD64 symDisp = 0;
            const bool haveSym = SymFromAddr(process, (DWORD64)frames[i], &symDisp, &sym) != 0;
            if (!haveSym && i == 0)
                fprintf(err, "  (SymFromAddr failed: %lu)\n", (unsigned long)GetLastError());
            DbgLine64 line{};
            line.SizeOfStruct = sizeof(DbgLine64);
            DWORD lineDisp = 0;
            const bool haveLine = SymGetLineFromAddr64(process, (DWORD64)frames[i], &lineDisp, &line) != 0;
            fprintf(err, "  #%02u %p %s+0x%llx%s%s:%lu\n", (unsigned)i, frames[i], haveSym ? sym.Name : "?", (unsigned long long)symDisp,
                haveLine ? "  " : "", haveLine ? line.FileName : "", haveLine ? (unsigned long)line.LineNumber : 0ul);
        }
        fflush(err);
        return 1; // EXCEPTION_EXECUTE_HANDLER: terminate
    }
}

// Call ONCE, early in main, only for an unattended run (see the header comment).
export void installUnattendedFailureHandling()
{
    _set_error_mode(_OUT_TO_STDERR);                                  // assert text -> stderr, no box
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);     // abort(): no box, no WER
    SetErrorMode(SetErrorMode(0) | 0x0001u /*SEM_FAILCRITICALERRORS*/ | 0x0002u /*SEM_NOGPFAULTERRORBOX*/);
    SetUnhandledExceptionFilter(unattendedCrashFilter);               // a crash prints its symbolized stack
}
