module RendererVK;

import Core;
import File; // disk access goes through FileSystem (Core no longer exports <filesystem>)
import Core.Windows;
import :Aftermath;

//*********************************************************
//
// Copyright (c) 2019-2025, NVIDIA CORPORATION. All rights reserved.
//
//  Permission is hereby granted, free of charge, to any person obtaining a
//  copy of this software and associated documentation files (the "Software"),
//  to deal in the Software without restriction, including without limitation
//  the rights to use, copy, modify, merge, publish, distribute, sublicense,
//  and/or sell copies of the Software, and to permit persons to whom the
//  Software is furnished to do so, subject to the following conditions:
//
//  The above copyright notice and this permission notice shall be included in
//  all copies or substantial portions of the Software.
//
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
//  THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
//  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
//  DEALINGS IN THE SOFTWARE.
//
//*********************************************************


//*********************************************************
// Some std::to_string overloads for some Nsight Aftermath
// API types.
//

namespace std
{
    template<typename T>
    inline std::string to_hex_string(T n)
    {
        std::stringstream stream;
        stream << std::setfill('0') << std::setw(2 * sizeof(T)) << std::hex << n;
        return stream.str();
    }

    inline std::string to_string(GFSDK_Aftermath_Result result)
    {
        return std::string("0x") + to_hex_string(static_cast<uint32_t>(result));
    }

    inline std::string to_string(const GFSDK_Aftermath_ShaderDebugInfoIdentifier& identifier)
    {
        return to_hex_string(identifier.id[0]) + "-" + to_hex_string(identifier.id[1]);
    }

    inline std::string to_string(const GFSDK_Aftermath_ShaderBinaryHash& hash)
    {
        return to_hex_string(hash.hash);
    }
} // namespace std

//*********************************************************
// Helper for comparing shader hashes and debug info identifier.
//

// Helper for comparing GFSDK_Aftermath_ShaderDebugInfoIdentifier.
inline bool operator<(const GFSDK_Aftermath_ShaderDebugInfoIdentifier& lhs, const GFSDK_Aftermath_ShaderDebugInfoIdentifier& rhs)
{
    if (lhs.id[0] == rhs.id[0])
    {
        return lhs.id[1] < rhs.id[1];
    }
    return lhs.id[0] < rhs.id[0];
}

// Helper for comparing GFSDK_Aftermath_ShaderBinaryHash.
inline bool operator<(const GFSDK_Aftermath_ShaderBinaryHash& lhs, const GFSDK_Aftermath_ShaderBinaryHash& rhs)
{
    return lhs.hash < rhs.hash;
}

// Helper for comparing GFSDK_Aftermath_ShaderDebugName.
inline bool operator<(const GFSDK_Aftermath_ShaderDebugName& lhs, const GFSDK_Aftermath_ShaderDebugName& rhs)
{
    return strncmp(lhs.name, rhs.name, sizeof(lhs.name)) < 0;
}

//*********************************************************
// Helper for checking Nsight Aftermath failures.
//

inline std::string  AftermathErrorMessage(GFSDK_Aftermath_Result result)
{
    switch (result)
    {
    case GFSDK_Aftermath_Result_FAIL_DriverVersionNotSupported:
        return "Unsupported driver version - requires an NVIDIA R495 display driver or newer.";
    default:
        return "Aftermath Error 0x" + std::to_hex_string(result);
    }
}

// Helper macro for checking Nsight Aftermath results and throwing exception
// in case of a failure.
#ifdef _WIN32
#define AFTERMATH_CHECK_ERROR(FC)                                                                       \
[&]() {                                                                                                 \
    GFSDK_Aftermath_Result _result = FC;                                                                \
    if (!GFSDK_Aftermath_SUCCEED(_result))                                                              \
    {                                                                                                   \
        MessageBoxA(0, AftermathErrorMessage(_result).c_str(), "Aftermath Error", MB_OK);               \
        exit(1);                                                                                        \
    }                                                                                                   \
}()
#else
#define AFTERMATH_CHECK_ERROR(FC)                                                                       \
[&]() {                                                                                                 \
    GFSDK_Aftermath_Result _result = FC;                                                                \
    if (!GFSDK_Aftermath_SUCCEED(_result))                                                              \
    {                                                                                                   \
        printf("%s\n", AftermathErrorMessage(_result).c_str());                                         \
        fflush(stdout);                                                                                 \
        exit(1);                                                                                        \
    }                                                                                                   \
}()
#endif
const std::string dumpFolder = "Local/";

//*********************************************************
// GpuCrashTracker implementation
//*********************************************************
static GpuCrashTracker::MarkerMap s_emptyMarkerMap;
GpuCrashTracker::GpuCrashTracker()
	: m_initialized(false)
	, m_mutex()
	, m_shaderDebugInfo()
	, m_shaderDatabase()
	, m_markerMap(s_emptyMarkerMap)
{
}

GpuCrashTracker::GpuCrashTracker(const MarkerMap& markerMap)
    : m_initialized(false)
    , m_mutex()
    , m_shaderDebugInfo()
    , m_shaderDatabase()
    , m_markerMap(markerMap)
{
}

GpuCrashTracker::~GpuCrashTracker()
{
    // If initialized, disable GPU crash dumps
    if (m_initialized)
    {
        GFSDK_Aftermath_DisableGpuCrashDumps();
    }
}

// Initialize the GPU Crash Dump Tracker
void GpuCrashTracker::Initialize(bool applicationUsesStrippedShaders)
{
    // Initialize the shader database with the application's shader configuration
    m_shaderDatabase.Initialize(applicationUsesStrippedShaders);

    // Enable GPU crash dumps and set up the callbacks for crash dump notifications,
    // shader debug information notifications, and providing additional crash
    // dump description data.Only the crash dump callback is mandatory. The other two
    // callbacks are optional and can be omitted, by passing nullptr, if the corresponding
    // functionality is not used.
    // The DeferDebugInfoCallbacks flag enables caching of shader debug information data
    // in memory. If the flag is set, ShaderDebugInfoCallback will be called only
    // in the event of a crash, right before GpuCrashDumpCallback. If the flag is not set,
    // ShaderDebugInfoCallback will be called for every shader that is compiled.
    AFTERMATH_CHECK_ERROR(GFSDK_Aftermath_EnableGpuCrashDumps(
        GFSDK_Aftermath_Version_API,
        GFSDK_Aftermath_GpuCrashDumpWatchedApiFlags_Vulkan,
        GFSDK_Aftermath_GpuCrashDumpFeatureFlags_DeferDebugInfoCallbacks, // Let the Nsight Aftermath library cache shader debug information.
        GpuCrashDumpCallback,                                             // Register callback for GPU crash dumps.
        ShaderDebugInfoCallback,                                          // Register callback for shader debug information.
        CrashDumpDescriptionCallback,                                     // Register callback for GPU crash dump description.
        ResolveMarkerCallback,                                            // Register callback for resolving application-managed markers.
        this));                                                           // Set the GpuCrashTracker object as user data for the above callbacks.

    m_initialized = true;
}

// Handler for GPU crash dump callbacks from Nsight Aftermath
void GpuCrashTracker::OnCrashDump(const void* pGpuCrashDump, const uint32_t gpuCrashDumpSize)
{
    printf("GpuCrashTracker::OnCrashDump\n");
    // Make sure only one thread at a time...
    std::lock_guard<std::mutex> lock(m_mutex);

    // Write to file for later in-depth analysis with Nsight Graphics.
    WriteGpuCrashDumpToFile(pGpuCrashDump, gpuCrashDumpSize);
}

// Handler for shader debug information callbacks
void GpuCrashTracker::OnShaderDebugInfo(const void* pShaderDebugInfo, const uint32_t shaderDebugInfoSize)
{
    // Make sure only one thread at a time...
    std::lock_guard<std::mutex> lock(m_mutex);

    // Get shader debug information identifier
    GFSDK_Aftermath_ShaderDebugInfoIdentifier identifier = {};
    AFTERMATH_CHECK_ERROR(GFSDK_Aftermath_GetShaderDebugInfoIdentifier(
        GFSDK_Aftermath_Version_API,
        pShaderDebugInfo,
        shaderDebugInfoSize,
        &identifier));

    // Store information for decoding of GPU crash dumps with shader address mapping
    // from within the application.
    std::vector<uint8_t> data((uint8_t*)pShaderDebugInfo, (uint8_t*)pShaderDebugInfo + shaderDebugInfoSize);
    m_shaderDebugInfo[identifier].swap(data);

    // Write to file for later in-depth analysis of crash dumps with Nsight Graphics
    WriteShaderDebugInformationToFile(identifier, pShaderDebugInfo, shaderDebugInfoSize);
}

// Handler for GPU crash dump description callbacks
void GpuCrashTracker::OnDescription(PFN_GFSDK_Aftermath_AddGpuCrashDumpDescription addDescription)
{
    // Add some basic description about the crash. This is called after the GPU crash happens, but before
    // the actual GPU crash dump callback. The provided data is included in the crash dump and can be
    // retrieved using GFSDK_Aftermath_GpuCrashDump_GetDescription().
    addDescription(GFSDK_Aftermath_GpuCrashDumpDescriptionKey_ApplicationName, "VkHelloNsightAftermath");
    addDescription(GFSDK_Aftermath_GpuCrashDumpDescriptionKey_ApplicationVersion, "v1.0");
    addDescription(GFSDK_Aftermath_GpuCrashDumpDescriptionKey_UserDefined, "This is a GPU crash dump example.");
    addDescription(GFSDK_Aftermath_GpuCrashDumpDescriptionKey_UserDefined + 1, "Engine State: Rendering.");
    addDescription(GFSDK_Aftermath_GpuCrashDumpDescriptionKey_UserDefined + 2, "More user-defined information...");

    // Include command line if provided
    if (!m_commandLine.empty())
    {
        std::string clPrefixed = std::string("CommandLine: ") + m_commandLine;
        addDescription(GFSDK_Aftermath_GpuCrashDumpDescriptionKey_UserDefined + 3, clPrefixed.c_str());
    }
}

// Handler for app-managed marker resolve callback
void GpuCrashTracker::OnResolveMarker(const void* pMarkerData, const uint32_t markerDataSize, PFN_GFSDK_Aftermath_ResolveMarker resolveMarker)
{
    for (auto& map : m_markerMap)
    {
        const auto& foundMarker = map.find((uint64_t)pMarkerData);
        if (foundMarker != map.end())
        {
            const std::string& foundMarkerData = foundMarker->second;
            resolveMarker(foundMarkerData.data(), (uint32_t)foundMarkerData.length());
            return;
        }
    }
}

// Helper for writing a GPU crash dump to a file
void GpuCrashTracker::WriteGpuCrashDumpToFile(const void* pGpuCrashDump, const uint32_t gpuCrashDumpSize)
{
    // Create a GPU crash dump decoder object for the GPU crash dump.
    GFSDK_Aftermath_GpuCrashDump_Decoder decoder = {};
    AFTERMATH_CHECK_ERROR(GFSDK_Aftermath_GpuCrashDump_CreateDecoder(
        GFSDK_Aftermath_Version_API,
        pGpuCrashDump,
        gpuCrashDumpSize,
        &decoder));

    // Use the decoder object to read basic information, like application
    // name, PID, etc. from the GPU crash dump.
    GFSDK_Aftermath_GpuCrashDump_BaseInfo baseInfo = {};
    AFTERMATH_CHECK_ERROR(GFSDK_Aftermath_GpuCrashDump_GetBaseInfo(decoder, &baseInfo));

    // Use the decoder object to query the application name that was set
    // in the GPU crash dump description.
    uint32_t applicationNameLength = 0;
    AFTERMATH_CHECK_ERROR(GFSDK_Aftermath_GpuCrashDump_GetDescriptionSize(
        decoder,
        GFSDK_Aftermath_GpuCrashDumpDescriptionKey_ApplicationName,
        &applicationNameLength));

    std::vector<char> applicationName(applicationNameLength, '\0');

    AFTERMATH_CHECK_ERROR(GFSDK_Aftermath_GpuCrashDump_GetDescription(
        decoder,
        GFSDK_Aftermath_GpuCrashDumpDescriptionKey_ApplicationName,
        uint32_t(applicationName.size()),
        applicationName.data()));

    // Create a unique file name for writing the crash dump data to a file.
    // Note: due to an Nsight Aftermath bug (will be fixed in an upcoming
    // driver release) we may see redundant crash dumps. As a workaround,
    // attach a unique count to each generated file name.
    static int count = 0;
    const std::string baseFileName =
        std::string(applicationName.data())
        + "-"
        + std::to_string(baseInfo.pid)
        + "-"
        + std::to_string(++count);

    // Write the crash dump data to a file using the .nv-gpudmp extension
    // registered with Nsight Graphics.
    const std::string crashDumpFileName = dumpFolder + baseFileName + ".nv-gpudmp";
	printf("Crash dump file: %s", crashDumpFileName.c_str());
    // A GPU crash dump: the process is dying, main-thread IO is fine and wanted.
    FileSystem::writeFileBytes(crashDumpFileName,
        std::span<const uint8>((const uint8*)pGpuCrashDump, gpuCrashDumpSize), /*allowMainThread*/ true);

    // Decode the crash dump to a JSON string.
    // Step 1: Generate the JSON and get the size.
    uint32_t jsonSize = 0;
    AFTERMATH_CHECK_ERROR(GFSDK_Aftermath_GpuCrashDump_GenerateJSON(
        decoder,
        GFSDK_Aftermath_GpuCrashDumpDecoderFlags_ALL_INFO,
        GFSDK_Aftermath_GpuCrashDumpFormatterFlags_NONE,
        ShaderDebugInfoLookupCallback,
        ShaderLookupCallback,
        ShaderSourceDebugInfoLookupCallback,
        this,
        &jsonSize));
    // Step 2: Allocate a buffer and fetch the generated JSON.
    std::vector<char> json(jsonSize);
    AFTERMATH_CHECK_ERROR(GFSDK_Aftermath_GpuCrashDump_GetJSON(
        decoder,
        uint32_t(json.size()),
        json.data()));

    // Write the crash dump data as JSON to a file.
    const std::string jsonFileName = crashDumpFileName + ".json";
    // (excluding string termination)
    FileSystem::writeFileBytes(jsonFileName,
        std::span<const uint8>((const uint8*)json.data(), json.size() - 1), /*allowMainThread*/ true);

    // Destroy the GPU crash dump decoder object.
    AFTERMATH_CHECK_ERROR(GFSDK_Aftermath_GpuCrashDump_DestroyDecoder(decoder));
}

// Helper for writing shader debug information to a file
void GpuCrashTracker::WriteShaderDebugInformationToFile(
    GFSDK_Aftermath_ShaderDebugInfoIdentifier identifier,
    const void* pShaderDebugInfo,
    const uint32_t shaderDebugInfoSize)
{
    // Create a unique file name.
    const std::string filePath = dumpFolder + "shader-" + std::to_string(identifier) + ".nvdbg";

    FileSystem::writeFileBytes(filePath,
        std::span<const uint8>((const uint8*)pShaderDebugInfo, shaderDebugInfoSize), /*allowMainThread*/ true);
}

// Handler for shader debug information lookup callbacks.
// This is used by the JSON decoder for mapping shader instruction
// addresses to SPIR-V IL lines or GLSL source lines.
void GpuCrashTracker::OnShaderDebugInfoLookup(
    const GFSDK_Aftermath_ShaderDebugInfoIdentifier& identifier,
    PFN_GFSDK_Aftermath_SetData setShaderDebugInfo) const
{
    // Search the list of shader debug information blobs received earlier.
    auto i_debugInfo = m_shaderDebugInfo.find(identifier);
    if (i_debugInfo == m_shaderDebugInfo.end())
    {
        // Early exit, nothing found. No need to call setShaderDebugInfo.
        return;
    }

    // Let the GPU crash dump decoder know about the shader debug information
    // that was found.
    setShaderDebugInfo(i_debugInfo->second.data(), uint32_t(i_debugInfo->second.size()));
}

// Handler for shader lookup callbacks.
// This is used by the JSON decoder for mapping shader instruction
// addresses to SPIR-V IL lines or GLSL source lines.
// NOTE: If the application loads stripped shader binaries (ie; --strip-all in spirv-remap),
// Aftermath will require access to both the stripped and the not stripped
// shader binaries.
void GpuCrashTracker::OnShaderLookup(
    const GFSDK_Aftermath_ShaderBinaryHash& shaderHash,
    PFN_GFSDK_Aftermath_SetData setShaderBinary) const
{
    // Find shader binary data for the shader hash in the shader database.
    std::vector<uint8_t> shaderBinary;
    if (!m_shaderDatabase.FindShaderBinary(shaderHash, shaderBinary))
    {
        // Early exit, nothing found. No need to call setShaderBinary.
        return;
    }

    // Let the GPU crash dump decoder know about the shader data
    // that was found.
    setShaderBinary(shaderBinary.data(), uint32_t(shaderBinary.size()));
}

// Handler for shader source debug info lookup callbacks.
// This is used by the JSON decoder for mapping shader instruction addresses to
// GLSL source lines, if the shaders used by the application were compiled with
// separate debug info data files.
void GpuCrashTracker::OnShaderSourceDebugInfoLookup(
    const GFSDK_Aftermath_ShaderDebugName& shaderDebugName,
    PFN_GFSDK_Aftermath_SetData setShaderBinary) const
{
    // Find source debug info for the shader DebugName in the shader database.
    std::vector<uint8_t> shaderBinary;
    if (!m_shaderDatabase.FindShaderBinaryWithDebugData(shaderDebugName, shaderBinary))
    {
        // Early exit, nothing found. No need to call setShaderBinary.
        return;
    }

    // Let the GPU crash dump decoder know about the shader debug data that was
    // found.
    setShaderBinary(shaderBinary.data(), uint32_t(shaderBinary.size()));
}

// Static callback wrapper for OnCrashDump
void GpuCrashTracker::GpuCrashDumpCallback(
    const void* pGpuCrashDump,
    const uint32_t gpuCrashDumpSize,
    void* pUserData)
{
    GpuCrashTracker* pGpuCrashTracker = reinterpret_cast<GpuCrashTracker*>(pUserData);
    pGpuCrashTracker->OnCrashDump(pGpuCrashDump, gpuCrashDumpSize);
}

// Static callback wrapper for OnShaderDebugInfo
void GpuCrashTracker::ShaderDebugInfoCallback(
    const void* pShaderDebugInfo,
    const uint32_t shaderDebugInfoSize,
    void* pUserData)
{
    GpuCrashTracker* pGpuCrashTracker = reinterpret_cast<GpuCrashTracker*>(pUserData);
    pGpuCrashTracker->OnShaderDebugInfo(pShaderDebugInfo, shaderDebugInfoSize);
}

// Static callback wrapper for OnDescription
void GpuCrashTracker::CrashDumpDescriptionCallback(
    PFN_GFSDK_Aftermath_AddGpuCrashDumpDescription addDescription,
    void* pUserData)
{
    GpuCrashTracker* pGpuCrashTracker = reinterpret_cast<GpuCrashTracker*>(pUserData);
    pGpuCrashTracker->OnDescription(addDescription);
}

// Static callback wrapper for OnResolveMarker
void GpuCrashTracker::ResolveMarkerCallback(
    const void* pMarkerData,
    const uint32_t markerDataSize,
    void* pUserData,
    PFN_GFSDK_Aftermath_ResolveMarker resolveMarker)
{
    GpuCrashTracker* pGpuCrashTracker = reinterpret_cast<GpuCrashTracker*>(pUserData);
    pGpuCrashTracker->OnResolveMarker(pMarkerData, markerDataSize, resolveMarker);
}

// Static callback wrapper for OnShaderDebugInfoLookup
void GpuCrashTracker::ShaderDebugInfoLookupCallback(
    const GFSDK_Aftermath_ShaderDebugInfoIdentifier* pIdentifier,
    PFN_GFSDK_Aftermath_SetData setShaderDebugInfo,
    void* pUserData)
{
    GpuCrashTracker* pGpuCrashTracker = reinterpret_cast<GpuCrashTracker*>(pUserData);
    pGpuCrashTracker->OnShaderDebugInfoLookup(*pIdentifier, setShaderDebugInfo);
}

// Static callback wrapper for OnShaderLookup
void GpuCrashTracker::ShaderLookupCallback(
    const GFSDK_Aftermath_ShaderBinaryHash* pShaderHash,
    PFN_GFSDK_Aftermath_SetData setShaderBinary,
    void* pUserData)
{
    GpuCrashTracker* pGpuCrashTracker = reinterpret_cast<GpuCrashTracker*>(pUserData);
    pGpuCrashTracker->OnShaderLookup(*pShaderHash, setShaderBinary);
}

// Static callback wrapper for OnShaderSourceDebugInfoLookup
void GpuCrashTracker::ShaderSourceDebugInfoLookupCallback(
    const GFSDK_Aftermath_ShaderDebugName* pShaderDebugName,
    PFN_GFSDK_Aftermath_SetData setShaderBinary,
    void* pUserData)
{
    GpuCrashTracker* pGpuCrashTracker = reinterpret_cast<GpuCrashTracker*>(pUserData);
    pGpuCrashTracker->OnShaderSourceDebugInfoLookup(*pShaderDebugName, setShaderBinary);
}