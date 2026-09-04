export module RendererVK:Aftermath;

import Core;
import Core.Windows;

export import <Aftermath/GFSDK_Aftermath_Defines.h>;
export import <Aftermath/GFSDK_Aftermath.h>;
export import <Aftermath/GFSDK_Aftermath_GpuCrashDump.h>;
export import <Aftermath/GFSDK_Aftermath_GpuCrashDumpDecoding.h>;

#undef GFSDK_Aftermath_SUCCEED
export constexpr bool GFSDK_Aftermath_SUCCEED(int value)
{
    return (((value) & 0xFFF00000) != GFSDK_Aftermath_Result_Fail);
}

#undef MB_OK
export long MB_OK = 0x00000000L;

// GFSDK_Aftermath_Lib.x64.dll is resolved at runtime, never linked: App.exe starts without it and
// simply has no GPU crash dumps. Every entry point the engine uses is a pointer here, filled by load().
export namespace Aftermath
{
    inline PFN_GFSDK_Aftermath_EnableGpuCrashDumps             EnableGpuCrashDumps = nullptr;
    inline PFN_GFSDK_Aftermath_DisableGpuCrashDumps            DisableGpuCrashDumps = nullptr;
    inline PFN_GFSDK_Aftermath_GetShaderDebugInfoIdentifier    GetShaderDebugInfoIdentifier = nullptr;
    inline PFN_GFSDK_Aftermath_GetShaderHashSpirv              GetShaderHashSpirv = nullptr;
    inline PFN_GFSDK_Aftermath_GetShaderDebugNameSpirv         GetShaderDebugNameSpirv = nullptr;
    inline PFN_GFSDK_Aftermath_GpuCrashDump_CreateDecoder      GpuCrashDump_CreateDecoder = nullptr;
    inline PFN_GFSDK_Aftermath_GpuCrashDump_DestroyDecoder     GpuCrashDump_DestroyDecoder = nullptr;
    inline PFN_GFSDK_Aftermath_GpuCrashDump_GetBaseInfo        GpuCrashDump_GetBaseInfo = nullptr;
    inline PFN_GFSDK_Aftermath_GpuCrashDump_GetDescriptionSize GpuCrashDump_GetDescriptionSize = nullptr;
    inline PFN_GFSDK_Aftermath_GpuCrashDump_GetDescription     GpuCrashDump_GetDescription = nullptr;
    inline PFN_GFSDK_Aftermath_GpuCrashDump_GenerateJSON       GpuCrashDump_GenerateJSON = nullptr;
    inline PFN_GFSDK_Aftermath_GpuCrashDump_GetJSON            GpuCrashDump_GetJSON = nullptr;

    inline HMODULE g_module = nullptr;
    inline bool g_loadAttempted = false;

    inline bool loaded() { return g_module != nullptr; }

    // Main thread only (Renderer::initialize). Returns false when the DLL or any symbol is missing.
    inline bool load()
    {
        if (g_loadAttempted)
            return loaded();
        g_loadAttempted = true;

        HMODULE module = LoadLibraryA("GFSDK_Aftermath_Lib.x64.dll");
        if (!module)
            return false;

        bool ok = true;
        auto resolve = [&](auto& fn, const char* name)
        {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(module, name));
            ok &= fn != nullptr;
        };
        resolve(EnableGpuCrashDumps,             "GFSDK_Aftermath_EnableGpuCrashDumps");
        resolve(DisableGpuCrashDumps,            "GFSDK_Aftermath_DisableGpuCrashDumps");
        resolve(GetShaderDebugInfoIdentifier,    "GFSDK_Aftermath_GetShaderDebugInfoIdentifier");
        resolve(GetShaderHashSpirv,              "GFSDK_Aftermath_GetShaderHashSpirv");
        resolve(GetShaderDebugNameSpirv,         "GFSDK_Aftermath_GetShaderDebugNameSpirv");
        resolve(GpuCrashDump_CreateDecoder,      "GFSDK_Aftermath_GpuCrashDump_CreateDecoder");
        resolve(GpuCrashDump_DestroyDecoder,     "GFSDK_Aftermath_GpuCrashDump_DestroyDecoder");
        resolve(GpuCrashDump_GetBaseInfo,        "GFSDK_Aftermath_GpuCrashDump_GetBaseInfo");
        resolve(GpuCrashDump_GetDescriptionSize, "GFSDK_Aftermath_GpuCrashDump_GetDescriptionSize");
        resolve(GpuCrashDump_GetDescription,     "GFSDK_Aftermath_GpuCrashDump_GetDescription");
        resolve(GpuCrashDump_GenerateJSON,       "GFSDK_Aftermath_GpuCrashDump_GenerateJSON");
        resolve(GpuCrashDump_GetJSON,            "GFSDK_Aftermath_GpuCrashDump_GetJSON");

        if (!ok)
        {
            FreeLibrary(module);
            EnableGpuCrashDumps = nullptr;
            DisableGpuCrashDumps = nullptr;
            return false;
        }
        g_module = module;
        return true;
    }
}
