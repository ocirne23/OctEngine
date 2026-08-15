module RendererVK;

import Core;
import File; // disk access goes through FileSystem (Core no longer exports <filesystem>)
import Core.Windows;
import :Aftermath;

//*********************************************************
//
// Copyright (c) 2019-2022, NVIDIA CORPORATION. All rights reserved.
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


//*********************************************************
// ShaderDatabase implementation
//*********************************************************

ShaderDatabase::ShaderDatabase()
    : m_shaderBinaries()
    , m_shaderBinariesWithDebugInfo()
{
}

ShaderDatabase::~ShaderDatabase()
{
}

// Initialize the shader database with application shader configuration
void ShaderDatabase::Initialize(bool applicationUsesStrippedShaders)
{
    // Clear any existing data
    m_shaderBinaries.clear();
    m_shaderBinariesWithDebugInfo.clear();

    if (applicationUsesStrippedShaders)
    {
        assert(false);
    }
    else
    {
        // If the application passes shaders with debug information to the Vulkan API,
        // only those shader binaries need to be registered with the database to allow
        // them to be correlated when decoding a GPU crash dump.

        // for each file in Assets/Shaders, add
        const std::string shaderFolder = "Local/spv";
		if (!FileSystem::isDirectory(shaderFolder, /*allowMainThread*/ true))
			return;

        std::vector<FileSystem::DirEntry> entries;
        FileSystem::listDirectory(shaderFolder, entries, /*allowMainThread*/ true);
        for (const FileSystem::DirEntry& entry : entries)
            if (!entry.isDirectory && entry.extension == ".spv")
                AddShaderBinary(FileSystem::absolutePath(entry.path, true).c_str());
    }
}

bool ShaderDatabase::ReadFile(const char* filename, std::vector<uint8_t>& data)
{
    // Aftermath crash-dump bookkeeping: runs at init/crash time on the main thread.
    return FileSystem::readFileBytes(filename, data, /*allowMainThread*/ true);
}

void ShaderDatabase::AddShaderBinary(const char* shaderFilePath)
{
    // Read the shader binary code from the file
    std::vector<uint8_t> data;
    if (!ReadFile(shaderFilePath, data))
    {
        return;
    }

    // Create shader hash for the shader
    const GFSDK_Aftermath_SpirvCode shader{ data.data(), uint32_t(data.size()) };
    GFSDK_Aftermath_ShaderBinaryHash shaderHash;
    AFTERMATH_CHECK_ERROR(GFSDK_Aftermath_GetShaderHashSpirv(
        GFSDK_Aftermath_Version_API,
        &shader,
        &shaderHash));

    // Store the data for shader mapping when decoding GPU crash dumps.
    // cf. FindShaderBinary()
    m_shaderBinaries[shaderHash].swap(data);
}

void ShaderDatabase::AddShaderBinaryWithDebugInfo(const char* strippedShaderFilePath, const char* shaderFilePath)
{
    // Read the shader debug data from the file
    std::vector<uint8_t> data;
    if (!ReadFile(shaderFilePath, data))
    {
        return;
    }
    std::vector<uint8_t> strippedData;
    if (!ReadFile(strippedShaderFilePath, strippedData))
    {
        return;
    }

    // Generate shader debug name.
    GFSDK_Aftermath_ShaderDebugName debugName;
    const GFSDK_Aftermath_SpirvCode shader{ data.data(), uint32_t(data.size()) };
    const GFSDK_Aftermath_SpirvCode strippedShader{ strippedData.data(), uint32_t(strippedData.size()) };
    AFTERMATH_CHECK_ERROR(GFSDK_Aftermath_GetShaderDebugNameSpirv(
        GFSDK_Aftermath_Version_API,
        &shader,
        &strippedShader,
        &debugName));

    // Store the data for shader instruction address mapping when decoding GPU crash dumps.
    // cf. FindShaderBinaryWithDebugData()
    m_shaderBinariesWithDebugInfo[debugName].swap(data);
}

// Find a shader binary by shader hash.
bool ShaderDatabase::FindShaderBinary(const GFSDK_Aftermath_ShaderBinaryHash& shaderHash, std::vector<uint8_t>& shader) const
{
    // Find shader binary data for the shader hash
    auto i_shader = m_shaderBinaries.find(shaderHash);
    if (i_shader == m_shaderBinaries.end())
    {
        // Nothing found.
        return false;
    }

    shader = i_shader->second;
    return true;
}

// Find a shader binary with debug information by shader debug name.
bool ShaderDatabase::FindShaderBinaryWithDebugData(const GFSDK_Aftermath_ShaderDebugName& shaderDebugName, std::vector<uint8_t>& shader) const
{
    // Find shader binary for the shader debug name.
    auto i_shader = m_shaderBinariesWithDebugInfo.find(shaderDebugName);
    if (i_shader == m_shaderBinariesWithDebugInfo.end())
    {
        // Nothing found.
        return false;
    }

    shader = i_shader->second;
    return true;
}