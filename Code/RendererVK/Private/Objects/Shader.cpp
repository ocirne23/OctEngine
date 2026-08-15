module RendererVK;

import Core;
import File;
import :VK;
import :Device;
import :glslang;
import :Layout;

Shader::Shader() {}
Shader::~Shader()
{
    if (m_shaderModule)
        Globals::device.getDevice().destroyShaderModule(m_shaderModule);
}

bool Shader::initializeFromFile(vk::ShaderStageFlagBits stage, const std::string& filePath, const std::vector<ShaderDefine>& defines, bool assertOnFailure)
{
    const std::string fileContent = FileSystem::readFileStr(filePath);
    if (fileContent.empty())
    {
        assert((!assertOnFailure) && "Failed to open shader file");
        return false;
    }

    return initialize(stage, fileContent, filePath, defines, assertOnFailure);
}

bool Shader::initialize(vk::ShaderStageFlagBits stage, const std::string& shaderStr, const std::string& debugFilePath, const std::vector<ShaderDefine>& defines, bool assertOnFailure)
{
    std::vector<unsigned int> spirv;
    if (!GLSLtoSPV(stage, shaderStr, spirv, debugFilePath, defines))
    {
        assert((!assertOnFailure) && "Failed to compile shader SPIRV");
        return false;
    }

    vk::ShaderModuleCreateInfo createInfo;
    createInfo.codeSize = spirv.size() * sizeof(unsigned int);
    createInfo.pCode = spirv.data();

    auto createResult = Globals::device.getDevice().createShaderModule(createInfo);
    if (createResult.result != vk::Result::eSuccess)
    {
        assert((!assertOnFailure) && "Failed to create shader module");
        return false;
    }
    m_shaderModule = createResult.value;

    return true;
}

EShLanguage translateShaderStage(vk::ShaderStageFlagBits stage)
{
    switch (stage)
    {
    case vk::ShaderStageFlagBits::eVertex: return EShLangVertex;
    case vk::ShaderStageFlagBits::eTessellationControl: return EShLangTessControl;
    case vk::ShaderStageFlagBits::eTessellationEvaluation: return EShLangTessEvaluation;
    case vk::ShaderStageFlagBits::eGeometry: return EShLangGeometry;
    case vk::ShaderStageFlagBits::eFragment: return EShLangFragment;
    case vk::ShaderStageFlagBits::eCompute: return EShLangCompute;
    case vk::ShaderStageFlagBits::eRaygenNV: return EShLangRayGenNV;
    case vk::ShaderStageFlagBits::eAnyHitNV: return EShLangAnyHitNV;
    case vk::ShaderStageFlagBits::eClosestHitNV: return EShLangClosestHitNV;
    case vk::ShaderStageFlagBits::eMissNV: return EShLangMissNV;
    case vk::ShaderStageFlagBits::eIntersectionNV: return EShLangIntersectNV;
    case vk::ShaderStageFlagBits::eCallableNV: return EShLangCallableNV;
    case vk::ShaderStageFlagBits::eTaskNV: return EShLangTaskNV;
    case vk::ShaderStageFlagBits::eMeshNV: return EShLangMeshNV;
    default: assert(false && "Unknown shader stage"); return EShLangVertex;
    }
}

class ShaderIncluder final : public glslang::TShader::Includer
{
public:
    explicit ShaderIncluder(const std::string& rootFilePath)
    {
        m_rootDir = FileSystem::parentPath(rootFilePath);
    }

    IncludeResult* includeLocal(const char* headerName, const char* includerName, size_t /*depth*/) override
    {
        return resolve(headerName, includerName);
    }

    IncludeResult* includeSystem(const char* headerName, const char* includerName, size_t /*depth*/) override
    {
        return resolve(headerName, includerName);
    }

    void releaseInclude(IncludeResult* /*result*/) override {}

private:
    IncludeResult* resolve(const char* headerName, const char* includerName)
    {
        std::vector<std::string> candidates;
        if (includerName != nullptr && includerName[0] != '\0')
        {
            const std::string includerDir = FileSystem::parentPath(includerName);
            if (!includerDir.empty())
                candidates.push_back(FileSystem::join(includerDir, headerName));
        }
        candidates.push_back(FileSystem::join(m_rootDir, headerName));

        for (const std::string& candidate : candidates)
        {
            const std::string resolvedPath = FileSystem::normalize(candidate);
            // Shader compiles run at startup and on F5 (an explicit user action) — main thread.
            std::string content = FileSystem::readFileStr(resolvedPath, /*allowMainThread*/ true);
            if (content.empty())
                continue;

            const std::string& stored = *m_contents.emplace_back(std::make_unique<std::string>(std::move(content)));
            m_results.push_back(std::make_unique<IncludeResult>(resolvedPath, stored.data(), stored.size(), nullptr));
            return m_results.back().get();
        }
        return nullptr;
    }

    std::string m_rootDir;
    std::vector<std::unique_ptr<std::string>> m_contents;
    std::vector<std::unique_ptr<IncludeResult>> m_results;
};

// RendererVKLayout constants every shader compile gets, so the GLSL never duplicates Layout.ixx values.
static std::string buildLayoutPreamble()
{
    using namespace RendererVKLayout;
    std::string s;
    const auto def = [&s](const char* name, auto value, const char* suffix = "") {
        s += "#define "; s += name; s += " " + std::to_string(value) + suffix + "\n";
    };
    def("NUM_SHADOW_CASCADES", NUM_SHADOW_CASCADES);
    def("GI_SH_STRIDE", GI_SH_STRIDE);
    def("GI_NUM_CASCADES", GI_NUM_CASCADES);
    def("GI_CASCADE_PROBE_DIM", GI_CASCADE_PROBE_DIM);
    def("GI_CASCADE_BASE_SPACING", GI_CASCADE_BASE_SPACING);
    def("VOL_FROXEL_X", VOL_FROXEL_X);
    def("VOL_FROXEL_Y", VOL_FROXEL_Y);
    def("VOL_FROXEL_Z", VOL_FROXEL_Z);
    def("ALPHA_MODE_OPAQUE", (uint32)EAlphaMode::Opaque, "u");
    def("ALPHA_MODE_MASK", (uint32)EAlphaMode::Mask, "u");
    def("ALPHA_MODE_BLEND", (uint32)EAlphaMode::Blend, "u");
    def("MATERIAL_FLAG_NO_RAYTRACING", MATERIAL_FLAG_NO_RAYTRACING, "u");
    def("MATERIAL_FLAG_SKY", MATERIAL_FLAG_SKY, "u");
    def("MATERIAL_FLAG_BC5_NORMAL", MATERIAL_FLAG_BC5_NORMAL, "u");
    def("MATERIAL_FLAG_OCEAN", MATERIAL_FLAG_OCEAN, "u");
    def("MATERIAL_FLAG_TERRAIN", MATERIAL_FLAG_TERRAIN, "u");
    def("OCEAN_FFT_SIZE", OCEAN_FFT_SIZE);
    def("OCEAN_CASCADES", OCEAN_CASCADES);
    def("MAX_TERRAIN_SPLAT_MATERIALS", MAX_TERRAIN_SPLAT_MATERIALS);
    def("PASS_MAIN", PASS_MAIN, "u");
    def("PASS_SHADOW", PASS_SHADOW, "u");
    def("PASS_GI", PASS_GI, "u");
    def("MAX_MESH_LODS", MAX_MESH_LODS);
    def("MAX_PARTICLES", MAX_PARTICLES, "u");
    def("MAX_PARTICLE_EMITTERS", MAX_PARTICLE_EMITTERS, "u");
    def("PARTICLE_SIM_GROUP_SIZE", PARTICLE_SIM_GROUP_SIZE, "u");
    def("PARTICLE_FLAG_LIT", PARTICLE_FLAG_LIT, "u");
    def("PARTICLE_FLAG_COLLIDE", PARTICLE_FLAG_COLLIDE, "u");
    def("PARTICLE_FLAG_KILL", PARTICLE_FLAG_KILL, "u");
    def("PARTICLE_TEX_NONE", PARTICLE_TEX_NONE, "u");
    def("DECAL_FLAG_LIT", DECAL_FLAG_LIT, "u");
    def("MAX_FORCE_EMITTERS", MAX_FORCE_EMITTERS, "u");
    def("MAX_FORCE_TEAMS", MAX_FORCE_TEAMS, "u");
    def("MAX_FORCE_QUERIES", MAX_FORCE_QUERIES, "u");
    def("MAX_FORCE_BIG_EMITTERS", MAX_FORCE_BIG_EMITTERS, "u");
    def("FORCE_SIM_GROUP_SIZE", FORCE_SIM_GROUP_SIZE, "u");
    def("FORCE_FLAG_ACTIVE", FORCE_FLAG_ACTIVE, "u");
    def("FORCE_FLAG_BIG", FORCE_FLAG_BIG, "u");
    def("FORCE_CELL_MAX_EMITTERS", FORCE_CELL_MAX_EMITTERS, "u");
    return s;
}

static std::string buildPreamble(const std::vector<ShaderDefine>& defines)
{
    std::string preamble = "#extension GL_ARB_shading_language_include : require\n";
    preamble += buildLayoutPreamble();
    for (const ShaderDefine& define : defines)
    {
        preamble += "#define " + define.name;
        if (!define.value.empty())
            preamble += " " + define.value;
        preamble += "\n";
    }
    return preamble;
}

bool Shader::GLSLtoSPV(const vk::ShaderStageFlagBits type, const std::string& source, std::vector<unsigned int>& spirv, const std::string& debugFilePath, const std::vector<ShaderDefine>& defines)
{
    std::string debugOutputFolder = "Local/";
	// make folder if it doesn't exist
	if (!FileSystem::exists(debugOutputFolder, /*allowMainThread*/ true))
		FileSystem::createDirectories(debugOutputFolder, true);
    std::string spvBinPath = debugOutputFolder + debugFilePath + ".spv";
	std::string preprocessFilePath = debugOutputFolder + debugFilePath;
    const std::string preprocessFileFolder = FileSystem::parentPath(preprocessFilePath);
    if (!FileSystem::exists(preprocessFileFolder, /*allowMainThread*/ true))
        FileSystem::createDirectories(preprocessFileFolder, true);

    // Enable SPIR-V and Vulkan rules when parsing GLSL
    EShMessages messages = (EShMessages)(EShMsgSpvRules | EShMsgVulkanRules);
    EShLanguage stage = translateShaderStage(type);
    std::string preprocessed;
    {
        glslang::TShader preprocessShader(stage);
        const char* shaderStrings[1] = { source.data() };
        preprocessShader.setStrings(shaderStrings, 1);
        preprocessShader.addSourceText(source.c_str(), source.length());
        preprocessShader.setSourceFile(debugFilePath.c_str());
        preprocessShader.setDebugInfo(false);

        // Target Vulkan 1.3 / SPIR-V 1.6 so modern extensions compile (notably GL_EXT_ray_query, which
        // needs the RayQueryKHR capability and SPIR-V 1.4+). Without this glslang defaults to SPIR-V 1.0.
        preprocessShader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 100);
        preprocessShader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_3);
        preprocessShader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_6);

        const std::string preamble = buildPreamble(defines);
        if (!preamble.empty())
            preprocessShader.setPreamble(preamble.c_str());

        // Enable SPIR-V and Vulkan rules when parsing GLSL
        ShaderIncluder includer(debugFilePath);
        if (!preprocessShader.preprocess(GetDefaultResources(), 100, ENoProfile, false, false, messages, &preprocessed, includer))
        {
			puts(preprocessShader.getInfoLog());
			puts(preprocessShader.getInfoDebugLog());
			std::cout.flush();
            assert(false);
			return false;  // something didn't work
        }
    }

    // swap the line with #version with the top line in preprocessed
    size_t versionPos = preprocessed.find("#version");
    if (versionPos != std::string::npos)
    {
        size_t lineEnd = preprocessed.find('\n', versionPos);
        if (lineEnd != std::string::npos)
        {
            std::string versionLine = preprocessed.substr(versionPos, lineEnd - versionPos + 1);
            preprocessed.erase(versionPos, lineEnd - versionPos + 1);
            preprocessed = versionLine + preprocessed;
        }
    }

    glslang::TShader shader(stage);
    //std::ofstream f(preprocessFilePath, std::ios::out | std::ios::binary);
    //if (f)
    //{
    //    f.write((const char*)preprocessed.data(), preprocessed.size());
    //}

    const char* shaderStrings[1] = { preprocessed.data() };
    shader.setStrings(shaderStrings, 1);
    shader.addSourceText(preprocessed.c_str(), preprocessed.length());
    shader.setSourceFile(preprocessFilePath.c_str());
    shader.setDebugInfo(true);

    // Target Vulkan 1.3 / SPIR-V 1.6 so modern extensions compile (notably GL_EXT_ray_query, which
    // needs the RayQueryKHR capability and SPIR-V 1.4+). Without this glslang defaults to SPIR-V 1.0.
    shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_3);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_6);

    if (!shader.parse(GetDefaultResources(), 100, ENoProfile, false, false, messages))
    {
        puts(shader.getInfoLog());
        puts(shader.getInfoDebugLog());
        std::cout.flush();
        assert(false);
        return false;  // something didn't work
    }

    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(messages))
    {
        puts(program.getInfoLog());
        puts(program.getInfoDebugLog());
        std::cout.flush();
        assert(false);
        return false;
    }

    glslang::SpvOptions spvOptions;
    spvOptions.generateDebugInfo = true;
    spvOptions.stripDebugInfo = false;
    spvOptions.disableOptimizer = false;
    spvOptions.optimizeSize = false;
    spvOptions.disassemble = false;
    spvOptions.validate = false;
    spvOptions.emitNonSemanticShaderDebugInfo = true;
    spvOptions.emitNonSemanticShaderDebugSource = true;
    spvOptions.compileOnly = false;
    glslang::GlslangToSpv(*program.getIntermediate(stage), spirv, &spvOptions);

    glslang::OutputSpvBin(spirv, spvBinPath.c_str());


    return true;
}
