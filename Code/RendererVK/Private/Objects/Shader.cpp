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

bool Shader::initializeFromFile(vk::ShaderStageFlagBits stage, const oc::string& filePath, const oc::vector<ShaderDefine>& defines, bool assertOnFailure)
{
    const oc::string fileContent = FileSystem::readFileStr(filePath);
    if (fileContent.empty())
    {
        assert((!assertOnFailure) && "Failed to open shader file");
        return false;
    }

    return initialize(stage, fileContent, filePath, defines, assertOnFailure);
}

bool Shader::initialize(vk::ShaderStageFlagBits stage, const oc::string& shaderStr, const oc::string& debugFilePath, const oc::vector<ShaderDefine>& defines, bool assertOnFailure)
{
    oc::vector<unsigned int> spirv;
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
    Globals::device.setDebugName(m_shaderModule, debugFilePath.c_str());

    return true;
}

oc::string Shader::debugName(const oc::string& debugFilePath)
{
    const size_t slash = debugFilePath.find_last_of("/\\");
    return slash == oc::string::npos ? debugFilePath : debugFilePath.substr(slash + 1);
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
    explicit ShaderIncluder(const oc::string& rootFilePath)
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
        oc::vector<oc::string> candidates;
        if (includerName != nullptr && includerName[0] != '\0')
        {
            const oc::string includerDir = FileSystem::parentPath(includerName);
            if (!includerDir.empty())
                candidates.push_back(FileSystem::join(includerDir, headerName));
        }
        candidates.push_back(FileSystem::join(m_rootDir, headerName));

        for (const oc::string& candidate : candidates)
        {
            const oc::string resolvedPath = FileSystem::normalize(candidate);
            // Shader compiles run at startup and on F5 (an explicit user action) - main thread.
            oc::string content = FileSystem::readFileStr(resolvedPath, /*allowMainThread*/ true);
            if (content.empty())
                continue;

            const oc::string& stored = *m_contents.emplace_back(oc::make_unique<oc::string>(oc::move(content)));
            m_results.push_back(oc::make_unique<IncludeResult>(oc::toStd(resolvedPath), stored.data(), stored.size(), nullptr));
            return m_results.back().get();
        }
        return nullptr;
    }

    oc::string m_rootDir;
    oc::vector<oc::unique_ptr<oc::string>> m_contents;
    oc::vector<oc::unique_ptr<IncludeResult>> m_results;
};

// RendererVKLayout constants every shader compile gets, so the GLSL never duplicates Layout.ixx values.
static oc::string buildLayoutPreamble()
{
    using namespace RendererVKLayout;
    oc::string s;
    const auto def = [&s](const char* name, auto value, const char* suffix = "") {
        s += "#define "; s += name; s += " " + oc::to_string(value) + suffix + "\n";
    };
    def("NUM_SHADOW_CASCADES", NUM_SHADOW_CASCADES);
    def("GI_SH_STRIDE", GI_SH_STRIDE);
    // The LIVE grid shape (g_giGrid, the "GI" grid tweaks): baked so the addressing stays constant-folded;
    // a change reloads every shader (GIProbePipeline::registerGridTweaks).
    def("GI_NUM_CASCADES", g_giGrid.numCascades);
    def("GI_PROBE_DIM_X", g_giGrid.dimX());
    def("GI_PROBE_DIM_Y", g_giGrid.dimY());
    def("GI_PROBE_DIM_Z", g_giGrid.dimZ());
    def("GI_FOCUS_Y_OFFSET", g_giGrid.focusOffsetY); // float literal (to_string keeps the decimal point)
    def("GI_CASCADE_BASE_SPACING", GI_CASCADE_BASE_SPACING);
    def("VOL_FROXEL_X", VOL_FROXEL_X);
    def("VOL_FROXEL_Y", VOL_FROXEL_Y);
    def("VOL_FROXEL_Z", VOL_FROXEL_Z);
    def("ALPHA_MODE_OPAQUE", (uint32)EAlphaMode::Opaque, "u");
    def("ALPHA_MODE_MASK", (uint32)EAlphaMode::Mask, "u");
    def("ALPHA_MODE_BLEND", (uint32)EAlphaMode::Blend, "u");
    def("MATERIAL_FLAG_NO_RAYTRACING", MATERIAL_FLAG_NO_RAYTRACING, "u");
    def("MATERIAL_FLAG_BC5_NORMAL", MATERIAL_FLAG_BC5_NORMAL, "u");
    def("MATERIAL_FLAG_OCEAN", MATERIAL_FLAG_OCEAN, "u");
    def("PIPELINE_IDX_OCEAN", (uint32)EPipelineIndex::Ocean, "u"); // the cull pads ocean bounds by the displacement
    def("MATERIAL_FLAG_TERRAIN", MATERIAL_FLAG_TERRAIN, "u");
    def("OCEAN_FFT_SIZE", OCEAN_FFT_SIZE);
    def("OCEAN_CASCADES", OCEAN_CASCADES);
    def("MAX_TERRAIN_SPLAT_MATERIALS", MAX_TERRAIN_SPLAT_MATERIALS);
    def("TERRAIN_WET_RES", TERRAIN_WET_RES);
    def("PASS_MAIN", PASS_MAIN, "u");
    def("PASS_SHADOW", PASS_SHADOW, "u");
    def("PASS_GI", PASS_GI, "u");
    def("MAX_MESH_LODS", MAX_MESH_LODS);
    def("MAX_PARTICLES", MAX_PARTICLES, "u");
    def("MAX_PARTICLE_EMITTERS", MAX_PARTICLE_EMITTERS, "u");
    def("PARTICLE_SIM_GROUP_SIZE", PARTICLE_SIM_GROUP_SIZE, "u");
    def("MAX_PARTICLE_GPU_SPAWNS", MAX_PARTICLE_GPU_SPAWNS, "u");
    def("OCEAN_SPRAY_GRID", OCEAN_SPRAY_GRID, "u");
    def("PARTICLE_FLAG_LIT", PARTICLE_FLAG_LIT, "u");
    def("PARTICLE_FLAG_COLLIDE", PARTICLE_FLAG_COLLIDE, "u");
    def("PARTICLE_FLAG_KILL", PARTICLE_FLAG_KILL, "u");
    def("PARTICLE_FLAG_VOLUME", PARTICLE_FLAG_VOLUME, "u");
    def("PARTICLE_FLAG_OCCLUDE", PARTICLE_FLAG_OCCLUDE, "u");
    def("PARTICLE_FLAG_WATER_FLOOR", PARTICLE_FLAG_WATER_FLOOR, "u");
    def("PARTICLE_FLAG_UNDERWATER", PARTICLE_FLAG_UNDERWATER, "u");
    def("PARTICLE_FLAG_ABOVE_WATER", PARTICLE_FLAG_ABOVE_WATER, "u");
    def("PARTICLE_FLAG_GROUND_FADE", PARTICLE_FLAG_GROUND_FADE, "u");
    def("PARTICLE_TEX_NONE", PARTICLE_TEX_NONE, "u");
    def("DECAL_FLAG_LIT", DECAL_FLAG_LIT, "u");
    def("MAX_FORCE_EMITTERS", MAX_FORCE_EMITTERS, "u");
    def("MAX_FORCE_TEAMS", MAX_FORCE_TEAMS, "u");
    def("MAX_FORCE_QUERIES", MAX_FORCE_QUERIES, "u");
    def("FORCE_SIM_GROUP_SIZE", FORCE_SIM_GROUP_SIZE, "u");
    def("FORCE_BAKE_CHUNK_SAMPLES", FORCE_BAKE_CHUNK_SAMPLES, "u");
    def("FORCE_SHELL_VOLUME_GROUP", FORCE_SHELL_VOLUME_GROUP, "u");
    def("FORCE_FLAG_ACTIVE", FORCE_FLAG_ACTIVE, "u");
    def("FORCE_FLAG_READBACK", FORCE_FLAG_READBACK, "u");
    def("FORCE_CELL_MAX_EMITTERS", FORCE_CELL_MAX_EMITTERS, "u");
#ifndef NDEBUG
    def("SHADER_STATS", 1); // debug builds only: the GPU stats atomics (the cull's LOD pick counts)
#endif
    return s;
}

static oc::string buildPreamble(const oc::vector<ShaderDefine>& defines)
{
    oc::string preamble = "#extension GL_ARB_shading_language_include : require\n";
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

bool Shader::GLSLtoSPV(const vk::ShaderStageFlagBits type, const oc::string& source, oc::vector<unsigned int>& spirv, const oc::string& debugFilePath, const oc::vector<ShaderDefine>& defines)
{
    const oc::string spvBinPath = "Local/" + debugFilePath + ".spv";
    const oc::string spvBinFolder = FileSystem::parentPath(spvBinPath);
    if (!FileSystem::exists(spvBinFolder, /*allowMainThread*/ true))
        FileSystem::createDirectories(spvBinFolder, true);

    // ONE parse with the includer, never preprocess() + a re-parse of its output: the debug info then
    // embeds the original text of the file and of every include, at their true lines. A re-parse
    // embedded the flattened text, and its "#line N 0" include epilogues kept the LAST include's file
    // name, so all code after an #include (main included) was attributed to that include - Nsight reads
    // that as the old glslang "incorrect function definition locations" bug.
    EShMessages messages = (EShMessages)(EShMsgSpvRules | EShMsgVulkanRules);
    EShLanguage stage = translateShaderStage(type);
    glslang::TShader shader(stage);
    const char* shaderStrings[1] = { source.c_str() };
    const int shaderLengths[1] = { (int)source.length() };
    const char* shaderNames[1] = { debugFilePath.c_str() }; // same name as the source file: one DebugSource
    shader.setStringsWithLengthsAndNames(shaderStrings, shaderLengths, shaderNames, 1);
    shader.addSourceText(source.c_str(), source.length());
    shader.setSourceFile(debugFilePath.c_str());
    shader.setDebugInfo(true);

    // Target Vulkan 1.3 / SPIR-V 1.6 so modern extensions compile (notably GL_EXT_ray_query, which
    // needs the RayQueryKHR capability and SPIR-V 1.4+). Without this glslang defaults to SPIR-V 1.0.
    shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_3);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_6);

    // The preamble's #extension may precede the file's #version: glslang scans the version from the
    // shader strings alone. The string must outlive parse().
    const oc::string preamble = buildPreamble(defines);
    shader.setPreamble(preamble.c_str());

    ShaderIncluder includer(debugFilePath);
    if (!shader.parse(GetDefaultResources(), 100, ENoProfile, false, false, messages, includer))
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
    std::vector<unsigned int> spirvStd; // GlslangToSpv/OutputSpvBin take std::vector by reference
    glslang::GlslangToSpv(*program.getIntermediate(stage), spirvStd, &spvOptions);

    glslang::OutputSpvBin(spirvStd, spvBinPath.c_str());
    spirv = oc::fromStd(spirvStd);


    return true;
}
