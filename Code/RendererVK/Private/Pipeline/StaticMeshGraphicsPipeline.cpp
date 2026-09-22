module RendererVK;

import Core;
import Core.Tweaks;

import File;

import :Buffer;
import :CommandBuffer;
import :Device;
import :Layout;
import :StagingManager;
import :IndirectCullComputePipeline;
import :TextureManager;
import :GIProbePipeline;

StaticMeshGraphicsPipeline::StaticMeshGraphicsPipeline() {}
StaticMeshGraphicsPipeline::~StaticMeshGraphicsPipeline() {}

void StaticMeshGraphicsPipeline::registerTweaks(const oc::function<void()>& onReloadShaders)
{
    Tweak::boolean("Editor", "Wireframe", &m_wireframe, onReloadShaders);
}

void StaticMeshGraphicsPipeline::buildPipelineLayout(GraphicsPipelineLayout& graphicsPipelineLayout, uint32 maxTextures)
{
    graphicsPipelineLayout.vertexShader.debugFilePath = "Shaders/instanced_indirect.vs.glsl";
    graphicsPipelineLayout.fragmentShader.debugFilePath = "Shaders/instanced_indirect.fs.glsl";

    graphicsPipelineLayout.vertexShader.text = FileSystem::readFileStr(graphicsPipelineLayout.vertexShader.debugFilePath);
    graphicsPipelineLayout.fragmentShader.text = FileSystem::readFileStr(graphicsPipelineLayout.fragmentShader.debugFilePath);

    // This pass WRITES the scene depth (there is no prepass): the layout's reversed-Z eGreater default.

    // Variant 1 (MeshShaderVariant::LitTransparent): same lit shader, alpha-blended, no depth write.
    graphicsPipelineLayout.additionalVariants.push_back(PipelineVariant{
        .fragmentShader = ShaderSource{
            .text = graphicsPipelineLayout.fragmentShader.text,
            .debugFilePath = graphicsPipelineLayout.fragmentShader.debugFilePath,
        },
        .blendEnable = true,
        .depthWrite = false,
    });
    // Variant 2 (MeshShaderVariant::UnlitOpaque): unlit opaque.
    const oc::string unlitVariantPath = "Shaders/instanced_indirect_unlit.fs.glsl";
	const oc::string unlitVariantText = FileSystem::readFileStr(unlitVariantPath);
    graphicsPipelineLayout.additionalVariants.push_back(PipelineVariant{
        .fragmentShader = ShaderSource{
            .text = unlitVariantText,
            .debugFilePath = unlitVariantPath,
        },
    });
	// Variant 3 (MeshShaderVariant::UnlitTransparent): same unlit shader, alpha-blended, no depth write.
	graphicsPipelineLayout.additionalVariants.push_back(PipelineVariant{
		.fragmentShader = ShaderSource{
			.text = unlitVariantText,
			.debugFilePath = unlitVariantPath,
		},
		.blendEnable = true,
		.depthWrite = false,
	});
	// Variant 4 (EPipelineIndex::Sky): analytic sky + sun disc, for the inside of the sky sphere.
	// NO DEPTH WRITE: a sky pixel's scene depth must stay at the cleared far plane (reversed-Z 0), which
	// is how every depth reader tells "sky" - TAA reprojects it parallax-free, AO / decals / fog / the
	// particle collision skip it. Depth-tested, so the draw order against the geometry does not matter.
	const oc::string skyVariantPath = "Shaders/sky.fs.glsl";
	const oc::string skyVariantText = FileSystem::readFileStr(skyVariantPath);
	graphicsPipelineLayout.additionalVariants.push_back(PipelineVariant{
		.fragmentShader = ShaderSource{
			.text = skyVariantText,
			.debugFilePath = skyVariantPath,
		},
		.depthWrite = false,
	});
	// Variants 5-7 (Wireframe + gizmos) all shade by vertex position (debug color).
    const oc::string& gizmoVariantPath = unlitVariantPath;
	const oc::string& gizmoVariantText = unlitVariantText;
	// Variant 5 (EPipelineIndex::WireframeTransparent): tangent-debug color, drawn as lines.
	graphicsPipelineLayout.additionalVariants.push_back(PipelineVariant{
		.fragmentShader = ShaderSource{
			.text = gizmoVariantText,
			.debugFilePath = gizmoVariantPath,
            .defines = { },
		},
		.blendEnable = false,
		.depthWrite = true,
		.polygonMode = vk::PolygonMode::eLine,
		.cullMode = vk::CullModeFlagBits::eNone, // wireframe: show every edge, both facings
	});
	// Variant 6 (EPipelineIndex::GizmoUI): tangent-debug color that stamps the nearest depth. A vertex
	// shader override (FORCE_NEAR_DEPTH) forces gl_Position.z = 0 (NDC near) without touching x/y/w, so
	// the shape is unchanged; with depth test+write on it passes eLess against any scene depth (drawn on
	// top of everything) and writes 0.0 so nothing drawn afterwards beats it. Forcing depth in the vertex
	// shader (not gl_FragDepth) keeps early-Z intact and leaves the IES fragment interface uniform.
	graphicsPipelineLayout.additionalVariants.push_back(PipelineVariant{
		.vertexShader = ShaderSource{
			.text = graphicsPipelineLayout.vertexShader.text,
			.debugFilePath = graphicsPipelineLayout.vertexShader.debugFilePath,
			.defines = { { "FORCE_NEAR_DEPTH", "1" } },
		},
		.fragmentShader = ShaderSource{
			.text = gizmoVariantText,
			.debugFilePath = gizmoVariantPath,
		},
		.blendEnable = false,
		.depthWrite = true,
		.depthTest = true,
		.cullMode = vk::CullModeFlagBits::eNone, // gizmo: double-sided so it reads from any angle
	});
	// Variant 7 (EPipelineIndex::GizmoWorld): tangent-debug color, depth tested (occluded by geometry),
	// alpha-blended, no depth write (world-space gizmo).
	graphicsPipelineLayout.additionalVariants.push_back(PipelineVariant{
		.fragmentShader = ShaderSource{
			.text = gizmoVariantText,
			.debugFilePath = gizmoVariantPath,
            .defines = { },
		},
		.cullMode = vk::CullModeFlagBits::eNone, // gizmo: double-sided so it reads from any angle
	});
	// Variant 8 (EPipelineIndex::TerrainLit): same lighting core as variant 0 (instanced_indirect_lit.inc),
	// but a dedicated fragment shader splats climate-picked procedural textures instead of sampling the
	// material's own (procedural terrain chunks carry no textures). Opaque, back-face culled, depth write
	// on (all layout defaults).
	// A dedicated vertex shader passes only world position + normal (the terrain FS builds its own tangent
	// bases and needs no UV), trimming the interpolated attributes from a full TBN+UV to two vec3s.
	const oc::string terrainVertexPath = "Shaders/instanced_indirect_terrain.vs.glsl";
	const oc::string terrainVariantPath = "Shaders/instanced_indirect_terrain.fs.glsl";
	graphicsPipelineLayout.additionalVariants.push_back(PipelineVariant{
		.vertexShader = ShaderSource{
			.text = FileSystem::readFileStr(terrainVertexPath),
			.debugFilePath = terrainVertexPath,
		},
		.fragmentShader = ShaderSource{
			.text = FileSystem::readFileStr(terrainVariantPath),
			.debugFilePath = terrainVariantPath,
		},
	});
	// Variant 9 (EPipelineIndex::Ocean): FFT/Tessendorf water. A dedicated vertex shader (same pipeline
	// layout/interface as the shared one) displaces the clipmap grid by the OceanSimulationPipeline's
	// displacement maps (binding 7); a dedicated fragment shader shades the surface (Fresnel, GGX sun
	// glint, RAY-TRACED refraction with Beer-Lambert absorption, Jacobian foam). Depth write on, and a
	// DUAL-SOURCE composite (out = ocean * a + scene * (1 - a), the alpha too - it owns TAA's ocean flag):
	// the ocean fades out over its last "Ocean edge fade (m)" of water column onto the ground and the film
	// under it. That needs the ground drawn first, so the cull routes the ocean into the TRANSPARENT sequence
	// (after the opaque execute and the tessellated ground + film). Elsewhere it writes a = 1: opaque.
	// Back-face culled: the clipmap carries
	// every triangle in both windings (OceanGenerator::rebuildGrid), so the underside draws from below.
	const oc::string oceanVertexPath = "Shaders/instanced_indirect_ocean.vs.glsl";
	const oc::string oceanVariantPath = "Shaders/ocean.fs.glsl";
	const oc::string oceanVariantText = FileSystem::readFileStr(oceanVariantPath);
	// OCEAN_HIT_LIGHTS: also evaluate the scene's grid lights at refraction/reflection ray hits
	// ("Ocean/Shading/Hit lighting" tweak; toggling reloads this pipeline via setOceanParams).
	oc::vector<ShaderDefine> oceanFragDefines;
	if (m_oceanHitLights)
		oceanFragDefines.push_back({ "OCEAN_HIT_LIGHTS", "1" });
	// OCEAN_RT_REFLECTIONS: the ray-traced scene mirror ("Ocean/RT/Reflections" tweak, same reload path).
	if (m_oceanRtReflections)
		oceanFragDefines.push_back({ "OCEAN_RT_REFLECTIONS", "1" });
	if (m_oceanDebugMode != 0) // "Ocean/Debug mode"
		oceanFragDefines.push_back({ "OCEAN_DEBUG_MODE", oc::to_string(m_oceanDebugMode) });
	graphicsPipelineLayout.additionalVariants.push_back(PipelineVariant{
		.vertexShader = ShaderSource{
			.text = FileSystem::readFileStr(oceanVertexPath),
			.debugFilePath = oceanVertexPath,
		},
		.fragmentShader = ShaderSource{
			.text = oceanVariantText,
			.debugFilePath = oceanVariantPath,
			.defines = oc::move(oceanFragDefines),
		},
		.blendEnable = true,
		.dualSourceBlend = true,
		.dualSourceAlpha = true,
		.cullMode = vk::CullModeFlagBits::eBack,
	});
	// Variant 10 (EPipelineIndex::LitMasked): the lit shader WITH the alpha-mask discard. Only this variant
	// carries a discard, so the LitOpaque variant keeps early depth writes.
	graphicsPipelineLayout.additionalVariants.push_back(PipelineVariant{
		.fragmentShader = ShaderSource{
			.text = graphicsPipelineLayout.fragmentShader.text,
			.debugFilePath = graphicsPipelineLayout.fragmentShader.debugFilePath,
			.defines = { { "ALPHA_MASK", "1" } },
		},
	});
	// Variant 11 (EPipelineIndex::TerrainOverlay): the terrain chunks inside the wetness clipmap drawn AGAIN
	// over the ground - the surface-water film today, the place for later terrain surface layers (snow ...).
	// The terrain VS and FS compiled with TERRAIN_OVERLAY_PASS: depth test EQUAL against the ground it re-draws
	// (the terrain VS's `invariant gl_Position` makes the depth bit-identical), so it shows exactly where the
	// terrain is the visible surface and anything in front - an ocean wave running up the sand - hides it as
	// it hides the ground (a lift along the normal instead put the film IN FRONT of water shallower than the
	// lift: a bright band where the waves meet the sand). The FS composites DUAL-SOURCE, out = K + ground *
	// factor, so it never reads the scene colour. No depth write. Kept out of the ground's shader so the
	// film's work no longer sets the register allocation of every terrain pixel. The main cull emits it
	// (never a material): see instanced_indirect.cs.glsl.
	graphicsPipelineLayout.additionalVariants.push_back(PipelineVariant{
		.vertexShader = ShaderSource{
			.text = FileSystem::readFileStr(terrainVertexPath),
			.debugFilePath = terrainVertexPath,
			.defines = { { "TERRAIN_OVERLAY_PASS", "1" } },
		},
		.fragmentShader = ShaderSource{
			.text = FileSystem::readFileStr(terrainVariantPath),
			.debugFilePath = terrainVariantPath,
			.defines = { { "TERRAIN_OVERLAY_PASS", "1" } },
		},
		.blendEnable = true,
		.dualSourceBlend = true,
		.depthWrite = false,
		.depthEqual = true,
	});

	// Global wireframe ("Renderer/Wireframe" tweak): rasterize every scene variant as lines. The sky and
	// gizmo overlays stay solid so the view keeps a background and the editor gizmos stay usable.
	if (m_wireframe)
	{
		graphicsPipelineLayout.polygonMode = vk::PolygonMode::eLine; // variant 0 (LitOpaque)
		for (size_t i = 0; i < graphicsPipelineLayout.additionalVariants.size(); ++i)
		{
			const auto pipelineIdx = RendererVKLayout::EPipelineIndex(i + 1);
			if (pipelineIdx == RendererVKLayout::EPipelineIndex::Sky
				|| pipelineIdx == RendererVKLayout::EPipelineIndex::GizmoUI
				|| pipelineIdx == RendererVKLayout::EPipelineIndex::GizmoWorld)
				continue;
			graphicsPipelineLayout.additionalVariants[i].polygonMode = vk::PolygonMode::eLine;
		}
	}

    // VR: every shader in this pipeline selects the per-eye view (u_views[u_viewIndex]) from one push
    // constant, gated behind STEREO. Define it once across all shader sources (and add the range once)
    // rather than per-variant; shaders that don't reference STEREO just ignore the define.
    if (m_stereo)
    {
        // The tess stages too: the tessellated terrain (buildTerrainTessLayout) shares this layout's ranges.
        graphicsPipelineLayout.pushConstantRanges.push_back(vk::PushConstantRange{
            .stageFlags = VIEW_PUSH_STAGES, .offset = 0, .size = sizeof(uint32) });

        auto defineStereo = [](ShaderSource& shader) {
            if (!shader.text.empty()) // empty = falls back to the default shader, which already gets it
                shader.defines.push_back({ "STEREO", "1" });
        };
        defineStereo(graphicsPipelineLayout.vertexShader);
        defineStereo(graphicsPipelineLayout.fragmentShader);
        for (PipelineVariant& variant : graphicsPipelineLayout.additionalVariants)
        {
            defineStereo(variant.vertexShader);
            defineStereo(variant.fragmentShader);
        }
    }

    // Debug overlays are BAKED defines, never uniforms: SHADOW_DEBUG ("Shadows/Debug mode") and
    // LIGHT_GRID_DEBUG ("Graphics/LOD/Light grid/Debug Mode") live in the lit core
    // (instanced_indirect_lit.inc.glsl / shadows.inc.glsl), and each switch reloads this pipeline
    // through the Renderer's tweak callback. Only the shaders that include the lit core get them: the
    // lit opaque/transparent fragment and the terrain fragment.
    const auto defineLitDebug = [&](const char* name, int mode)
    {
        if (mode == 0)
            return;
        const oc::string modeText = oc::to_string(mode);
        graphicsPipelineLayout.fragmentShader.defines.push_back({ name, modeText });
        for (PipelineVariant& variant : graphicsPipelineLayout.additionalVariants)
            if (variant.fragmentShader.debugFilePath == graphicsPipelineLayout.fragmentShader.debugFilePath
                || variant.fragmentShader.debugFilePath == terrainVariantPath)
                variant.fragmentShader.defines.push_back({ name, modeText });
    };
    // The terrain's surface-water film could mirror the scene with the ocean's ray, under the ocean's toggle.
    // DISABLED: that ray query set the terrain's register allocation for every pixel (see terrainFilmMirror in
    // instanced_indirect_terrain.fs.glsl); the film reflects the sky only.
    constexpr bool TERRAIN_FILM_RT_MIRROR = false;
    if (TERRAIN_FILM_RT_MIRROR && m_oceanRtReflections)
        for (PipelineVariant& variant : graphicsPipelineLayout.additionalVariants)
            if (variant.fragmentShader.debugFilePath == terrainVariantPath)
                variant.fragmentShader.defines.push_back({ "TERRAIN_FILM_RT_MIRROR", "1" });
    defineLitDebug("SHADOW_DEBUG", m_shadowDebugMode);
    defineLitDebug("LIGHT_GRID_DEBUG", m_lightGridDebugMode);
    // The RT shadow toggles, always defined (0/1) on the lit-core fragments: only the active sun-shadow
    // path and (when off) no light ray query is compiled, so the register allocation covers less code.
    const auto defineLit = [&](const char* name, bool on)
    {
        const ShaderDefine define{ name, on ? "1" : "0" };
        graphicsPipelineLayout.fragmentShader.defines.push_back(define);
        for (PipelineVariant& variant : graphicsPipelineLayout.additionalVariants)
            if (variant.fragmentShader.debugFilePath == graphicsPipelineLayout.fragmentShader.debugFilePath
                || variant.fragmentShader.debugFilePath == terrainVariantPath)
                variant.fragmentShader.defines.push_back(define);
    };
    defineLit("LIT_RT_SUN_SHADOW", m_rtSunShadow);
    defineLit("LIT_RT_LIGHT_SHADOWS", m_rtLightShadows);
    // TERRAIN_POM (0/1, "Terrain/Textures/Parallax"): the parallax march + its self-shadow, compiled in or out of
    // the terrain fragment shaders (the tessellated copies inherit it through buildTerrainTessLayout).
    for (PipelineVariant& variant : graphicsPipelineLayout.additionalVariants)
        if (variant.fragmentShader.debugFilePath == terrainVariantPath)
            variant.fragmentShader.defines.push_back({ "TERRAIN_POM", m_terrainPom ? "1" : "0" });

    auto& bindingDescriptions = graphicsPipelineLayout.vertexLayoutInfo.bindingDescriptions;
    bindingDescriptions.push_back(vk::VertexInputBindingDescription{
        .binding = 0,
        .stride = sizeof(RendererVKLayout::MeshVertex),
        .inputRate = vk::VertexInputRate::eVertex,
    });
    bindingDescriptions.push_back(vk::VertexInputBindingDescription{
        .binding = 2,
        .stride = sizeof(uint32),
        .inputRate = vk::VertexInputRate::eInstance,
    });

    auto& attributeDescriptions = graphicsPipelineLayout.vertexLayoutInfo.attributeDescriptions;
    attributeDescriptions.push_back(vk::VertexInputAttributeDescription{ // position
        .location = 0,
        .binding = 0,
        .format = vk::Format::eR32G32B32Sfloat,
        .offset = offsetof(RendererVKLayout::MeshVertex, position),
    });
    attributeDescriptions.push_back(vk::VertexInputAttributeDescription{ // normals
        .location = 1,
        .binding = 0,
        .format = vk::Format::eR32G32B32Sfloat,
        .offset = offsetof(RendererVKLayout::MeshVertex, normal),
    });
    attributeDescriptions.push_back(vk::VertexInputAttributeDescription{ // tangents
        .location = 2,
        .binding = 0,
        .format = vk::Format::eR32G32B32A32Sfloat,
        .offset = offsetof(RendererVKLayout::MeshVertex, tangent),
    });
    attributeDescriptions.push_back(vk::VertexInputAttributeDescription{ // texcoords
        .location = 3,
        .binding = 0,
        .format = vk::Format::eR32G32Sfloat,
        .offset = offsetof(RendererVKLayout::MeshVertex, texCoord),
    });
    attributeDescriptions.push_back(vk::VertexInputAttributeDescription{ // inst_idx
        .location = 4,
        .binding = 2,
        .format = vk::Format::eR32Uint,
        .offset = 0,
    });

    // The tessellated terrain's pipeline copies these bindings, so its control / evaluation stages are named on
    // the ones they read: the UBO (factors, fade, frustum) and the texture array (the height maps).
    constexpr vk::ShaderStageFlags TESS_STAGES = vk::ShaderStageFlagBits::eTessellationControl | vk::ShaderStageFlagBits::eTessellationEvaluation;
    auto& descriptorSetBindings = graphicsPipelineLayout.descriptorSetLayoutBindings;
    descriptorSetBindings.push_back(vk::DescriptorSetLayoutBinding{ // UBO
        .binding = 0,
        .descriptorType = vk::DescriptorType::eUniformBuffer,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment | TESS_STAGES
    });
    descriptorSetBindings.push_back(vk::DescriptorSetLayoutBinding{ // InMeshInstances
        .binding = 1,
        .descriptorType = vk::DescriptorType::eStorageBuffer,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eVertex
    });
    descriptorSetBindings.push_back(vk::DescriptorSetLayoutBinding{ // InMaterialInfos
        .binding = 2,
        .descriptorType = vk::DescriptorType::eStorageBuffer,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eFragment
    });
    descriptorSetBindings.push_back(vk::DescriptorSetLayoutBinding{ // InRenderNodeTransforms
        .binding = 3,
        .descriptorType = vk::DescriptorType::eStorageBuffer,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eVertex
    });
    descriptorSetBindings.push_back(vk::DescriptorSetLayoutBinding{ // InLightInfos
        .binding = 4,
        .descriptorType = vk::DescriptorType::eStorageBuffer,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eFragment
    });
    descriptorSetBindings.push_back(vk::DescriptorSetLayoutBinding{ // InLightGrid
        .binding = 5,
        .descriptorType = vk::DescriptorType::eStorageBuffer,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eFragment
    });
    descriptorSetBindings.push_back(vk::DescriptorSetLayoutBinding{ // InGridTable
        .binding = 6,
        .descriptorType = vk::DescriptorType::eStorageBuffer,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eFragment
    });
    descriptorSetBindings.push_back(vk::DescriptorSetLayoutBinding{ // u_oceanMaps (FFT displacement/gradient array)
        .binding = 7,
        .descriptorType = vk::DescriptorType::eCombinedImageSampler,
        .descriptorCount = 1,
        // The tessellated film's evaluation stage too: its surface follows the LIVE ocean at the waterline.
        .stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment | vk::ShaderStageFlagBits::eTessellationEvaluation
    });
    descriptorSetBindings.push_back(vk::DescriptorSetLayoutBinding{ // u_shadowMap (sun CSM, comparison)
        .binding = 8,
        .descriptorType = vk::DescriptorType::eCombinedImageSampler,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eFragment
    });
    descriptorSetBindings.push_back(vk::DescriptorSetLayoutBinding{ // u_shadowMapDepth (raw depth, PCSS blocker search)
        .binding = 9,
        .descriptorType = vk::DescriptorType::eCombinedImageSampler,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eFragment
    });
    descriptorSetBindings.push_back(vk::DescriptorSetLayoutBinding{ // probe_sh (GI clipmap volume)
        .binding = 10,
        .descriptorType = vk::DescriptorType::eStorageBuffer,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eFragment
    });
    descriptorSetBindings.push_back(vk::DescriptorSetLayoutBinding{ // u_tlas (ray-traced light shadows)
        .binding = 11,
        .descriptorType = vk::DescriptorType::eAccelerationStructureKHR,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eFragment
    });
    descriptorSetBindings.push_back(vk::DescriptorSetLayoutBinding{ // u_prevDepth (last frame's depth: AO bilateral upsample)
        .binding = 12,
        .descriptorType = vk::DescriptorType::eCombinedImageSampler,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eFragment
    });
    descriptorSetBindings.push_back(vk::DescriptorSetLayoutBinding{ // u_ao (denoised screen-space AO)
        .binding = 13,
        .descriptorType = vk::DescriptorType::eCombinedImageSampler,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eFragment
    });
    for (uint32 binding = 14; binding <= 17; ++binding) // shadow-ray alpha test: vertices/indices/meshInfos/instances
        descriptorSetBindings.push_back(vk::DescriptorSetLayoutBinding{
            .binding = binding,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eFragment
        });
    descriptorSetBindings.push_back(vk::DescriptorSetLayoutBinding{ // u_terrainWet (terrain wetness clipmap, GENERAL layout)
        .binding = 18,
        .descriptorType = vk::DescriptorType::eCombinedImageSampler,
        .descriptorCount = 1,
        // The tessellated film's evaluation stage reads it too: its surface stands at the water level the
        // local wetness fills the relief to.
        .stageFlags = vk::ShaderStageFlagBits::eFragment | vk::ShaderStageFlagBits::eTessellationEvaluation
    });
    descriptorSetBindings.push_back(vk::DescriptorSetLayoutBinding{ // u_terrainHeight (terrain-data cascades: ocean depth/water level)
        .binding = 19,
        .descriptorType = vk::DescriptorType::eCombinedImageSampler,
        .descriptorCount = 1,
        // + the tessellated film's evaluation stage: the baked water level under the live ocean.
        .stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment | vk::ShaderStageFlagBits::eTessellationEvaluation
    });

    descriptorSetBindings.push_back(vk::DescriptorSetLayoutBinding{ // u_skyMap (GI's per-frame sky bake: layer 0 skyRadiance, layer 1 mirror sky; GENERAL layout)
        .binding = 20,
        .descriptorType = vk::DescriptorType::eCombinedImageSampler,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eFragment
    });

    // u_giVolume: the baked GI irradiance volume + sky SH, GENERAL layout, sized for the cascade tweak's max.
    descriptorSetBindings.push_back(GiVolumeDescriptors::layoutBinding(21, vk::ShaderStageFlagBits::eFragment));

    // 22 = the set's highest binding number: required for eVariableDescriptorCount.
    descriptorSetBindings.push_back(vk::DescriptorSetLayoutBinding{ // u_textures
        .binding = 22,
        .descriptorType = vk::DescriptorType::eCombinedImageSampler,
        .descriptorCount = maxTextures,
        .stageFlags = vk::ShaderStageFlagBits::eFragment | vk::ShaderStageFlagBits::eTessellationEvaluation
    });

    // Per-binding flags (parallel to descriptorSetBindings): the AO (13), TLAS (11), terrain wetness (18),
    // baked terrain height (19) and sky map (20) bindings are refreshed after the (cached) draw CB is
    // recorded -> UPDATE_AFTER_BIND; the GI volume (21) is written at record only for the live cascades
    // (partially bound; the images change only with the grid, which re-records); the texture array (22) is
    // variable-count (allocated at the live texture capacity), only partially written, and UPDATE_AFTER_BIND
    // so the TextureStreamer can rewrite swapped slots without re-recording the cached draw CBs.
    graphicsPipelineLayout.descriptorBindingFlags.resize(descriptorSetBindings.size());
    for (size_t i = 0; i < descriptorSetBindings.size(); ++i)
    {
        if (descriptorSetBindings[i].binding == 11 || descriptorSetBindings[i].binding == 13
            || descriptorSetBindings[i].binding == 18 || descriptorSetBindings[i].binding == 19 || descriptorSetBindings[i].binding == 20)
            graphicsPipelineLayout.descriptorBindingFlags[i] = vk::DescriptorBindingFlagBits::eUpdateAfterBind;
        else if (descriptorSetBindings[i].binding == 21)
            graphicsPipelineLayout.descriptorBindingFlags[i] = vk::DescriptorBindingFlagBits::ePartiallyBound;
        else if (descriptorSetBindings[i].binding == 22)
            graphicsPipelineLayout.descriptorBindingFlags[i] = vk::DescriptorBindingFlagBits::ePartiallyBound | vk::DescriptorBindingFlagBits::eVariableDescriptorCount | vk::DescriptorBindingFlagBits::eUpdateAfterBind;
    }
}

void StaticMeshGraphicsPipeline::buildTerrainTessLayout(const GraphicsPipelineLayout& main, GraphicsPipelineLayout& tess)
{
    const PipelineVariant& ground = main.additionalVariants[(size_t)RendererVKLayout::EPipelineIndex::TerrainLit - 1];
    const PipelineVariant& overlay = main.additionalVariants[(size_t)RendererVKLayout::EPipelineIndex::TerrainOverlay - 1];

    tess.vertexLayoutInfo = main.vertexLayoutInfo;
    tess.descriptorSetLayoutBindings = main.descriptorSetLayoutBindings;
    tess.descriptorBindingFlags = main.descriptorBindingFlags;
    tess.pushConstantRanges = main.pushConstantRanges;
    tess.indirectBindable = false;
    tess.patchControlPoints = 3;

    // One VS / TCS / TES for both variants: the overlay's EQUAL depth test needs bit-identical positions. The
    // VS's TERRAIN_TESS path hands control points on instead of projecting.
    oc::vector<ShaderDefine> tessDefines = { { "TERRAIN_TESS", "1" } };
    if (m_stereo)
        tessDefines.push_back({ "STEREO", "1" });
    const auto source = [&](const char* path) {
        return ShaderSource{ .text = FileSystem::readFileStr(path), .debugFilePath = path, .defines = tessDefines };
    };
    tess.vertexShader = ShaderSource{ .text = ground.vertexShader.text, .debugFilePath = ground.vertexShader.debugFilePath, .defines = tessDefines };
    tess.tessControlShader = source("Shaders/terrain_tess.tcs.glsl");
    tess.tessEvalShader = source("Shaders/terrain_tess.tes.glsl");

    // Variant 0: the ground (layout defaults: opaque, depth write, back-face culled). TERRAIN_TESS on the
    // fragment shaders too: they light from the evaluation stage's undisplaced position.
    tess.fragmentShader = ground.fragmentShader;
    tess.fragmentShader.defines.push_back({ "TERRAIN_TESS", "1" });
    tess.polygonMode = ground.polygonMode;
    // Variant 1: the overlay. Its evaluation stage displaces to the film's WATER LEVEL, not to the relief, so
    // it has its own vertices and CANNOT depth-test EQUAL any more: the ordinary reversed-Z test keeps it where
    // it stands above the ground and lets the ground hide it where it does not (rock out of a puddle).
    PipelineVariant overlayTess = overlay;
    overlayTess.vertexShader = ShaderSource{}; // the layout's (TERRAIN_TESS) VS
    overlayTess.fragmentShader.defines.push_back({ "TERRAIN_TESS", "1" });
    overlayTess.tessEvalShader = tess.tessEvalShader;
    overlayTess.tessEvalShader.defines.push_back({ "TERRAIN_OVERLAY_PASS", "1" });
    overlayTess.depthEqual = false;
    tess.additionalVariants.push_back(oc::move(overlayTess));
}

void StaticMeshGraphicsPipeline::updateTextureDescriptor(vk::DescriptorSet descriptorSet, uint32 slotIdx, vk::ImageView view)
{
    // Streamed texture slot rewrite (same recorded-once CB situation as the AO/TLAS bindings above).
    vk::DescriptorImageInfo imageInfo{ .sampler = m_sampler.getSampler(), .imageView = view, .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal };
    vk::WriteDescriptorSet write{ .dstSet = descriptorSet, .dstBinding = 22, .dstArrayElement = slotIdx, .descriptorCount = 1,
        .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &imageInfo };
    Globals::device.getDevice().updateDescriptorSets(1, &write, 0, nullptr);
}

void StaticMeshGraphicsPipeline::updateSkyMapDescriptor(vk::DescriptorSet descriptorSet, vk::ImageView skyView, vk::Sampler skySampler)
{
    // The GI sky bake (20) is rewritten every frame by GIProbePipeline::recordSkyMap and lives in GENERAL;
    // the ocean / terrain-film mirror rays and the constant skyRadiance(up) ambient sample it.
    vk::DescriptorImageInfo imageInfo{ .sampler = skySampler, .imageView = skyView, .imageLayout = vk::ImageLayout::eGeneral };
    vk::WriteDescriptorSet write{ .dstSet = descriptorSet, .dstBinding = 20, .descriptorCount = 1,
        .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &imageInfo };
    Globals::device.getDevice().updateDescriptorSets(1, &write, 0, nullptr);
}

void StaticMeshGraphicsPipeline::updateTerrainHeightDescriptor(vk::DescriptorSet descriptorSet, vk::ImageView terrainView, vk::Sampler terrainSampler)
{
    // Refreshed every frame (UPDATE_AFTER_BIND): the terrain-data cascades (19) are a ping-pong pair
    // whose active image swaps when the CPU re-bakes it, without touching the cached draw CBs. The
    // ocean VS/FS read them for water depth/level.
    vk::DescriptorImageInfo imageInfo{ .sampler = terrainSampler, .imageView = terrainView, .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal };
    vk::WriteDescriptorSet write{ .dstSet = descriptorSet, .dstBinding = 19, .descriptorCount = 1,
        .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &imageInfo };
    Globals::device.getDevice().updateDescriptorSets(1, &write, 0, nullptr);
}

void StaticMeshGraphicsPipeline::updateTerrainWetnessDescriptor(vk::DescriptorSet descriptorSet, vk::ImageView wetView, vk::Sampler wetSampler)
{
    // The wetness clipmap (18) is written in place by the wetness compute pass and lives in GENERAL for
    // its whole life; the terrain fragment shader texelFetches it (terrain_wetness.inc.glsl).
    vk::DescriptorImageInfo imageInfo{ .sampler = wetSampler, .imageView = wetView, .imageLayout = vk::ImageLayout::eGeneral };
    vk::WriteDescriptorSet write{ .dstSet = descriptorSet, .dstBinding = 18, .descriptorCount = 1,
        .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &imageInfo };
    Globals::device.getDevice().updateDescriptorSets(1, &write, 0, nullptr);
}

void StaticMeshGraphicsPipeline::updateAODescriptor(vk::DescriptorSet descriptorSet, vk::ImageView aoView, vk::Sampler aoSampler)
{
    // The cached draw command buffer binds this set by handle, so the AO image binding is rewritten when
    // the slot re-records: it points at the PREVIOUS slot's denoised AO image (the forward pass reads
    // last frame's AO, reprojected). The AO images stay in GENERAL layout (written by the compute
    // denoise, sampled here).
    vk::DescriptorImageInfo imageInfo{ .sampler = aoSampler, .imageView = aoView, .imageLayout = vk::ImageLayout::eGeneral };
    vk::WriteDescriptorSet write{ .dstSet = descriptorSet, .dstBinding = 13, .descriptorCount = 1,
        .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &imageInfo };
    Globals::device.getDevice().updateDescriptorSets(1, &write, 0, nullptr);
}

void StaticMeshGraphicsPipeline::updateTlasDescriptor(vk::DescriptorSet descriptorSet, vk::AccelerationStructureKHR tlas)
{
    // Same recorded-once CB situation as the AO image: the TLAS is double-buffered and rebuilt every frame
    // (its handle can change on resize), so the binding is refreshed per frame via UPDATE_AFTER_BIND.
    vk::WriteDescriptorSetAccelerationStructureKHR asInfo{ .accelerationStructureCount = 1, .pAccelerationStructures = &tlas };
    vk::WriteDescriptorSet write{ .pNext = &asInfo, .dstSet = descriptorSet, .dstBinding = 11, .descriptorCount = 1,
        .descriptorType = vk::DescriptorType::eAccelerationStructureKHR };
    Globals::device.getDevice().updateDescriptorSets(1, &write, 0, nullptr);
}

void StaticMeshGraphicsPipeline::initialize(vk::RenderPass renderPass, uint32 maxUniqueMeshes, uint32 maxTextures, bool stereo)
{
    m_renderPass = renderPass;
    m_stereo = stereo;
    m_sampler.initialize();

    GraphicsPipelineLayout graphicsPipelineLayout;
    buildPipelineLayout(graphicsPipelineLayout, maxTextures);
    m_graphicsPipeline.initialize(renderPass, graphicsPipelineLayout);
    if (m_terrainTess)
    {
        GraphicsPipelineLayout terrainTessLayout;
        buildTerrainTessLayout(graphicsPipelineLayout, terrainTessLayout);
        m_terrainTessBuilt = m_terrainTessPipeline.initialize(renderPass, terrainTessLayout);
    }

    m_indirectExecutionSet.initialize(m_graphicsPipeline, "StaticMesh.executionSet");
    m_indirectCommandsLayout.initialize("StaticMesh.dgcLayout", m_graphicsPipeline.getPipelineLayout(),
        vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment);

    createPreprocessBuffers(maxUniqueMeshes);
}

void StaticMeshGraphicsPipeline::resizeMeshCapacity(uint32 maxUniqueMeshes)
{
    createPreprocessBuffers(maxUniqueMeshes);
}

void StaticMeshGraphicsPipeline::createPreprocessBuffers(uint32 maxUniqueMeshes)
{
    // Size the preprocess scratch buffer for the worst case (one sequence per unique mesh).
    vk::GeneratedCommandsMemoryRequirementsInfoEXT memReqInfo{
        .indirectExecutionSet = m_indirectExecutionSet.getHandle(),
        .indirectCommandsLayout = m_indirectCommandsLayout.getHandle(),
        .maxSequenceCount = maxUniqueMeshes,
        .maxDrawCount = maxUniqueMeshes,
    };
    vk::MemoryRequirements2 memReq;
    Globals::device.getDevice().getGeneratedCommandsMemoryRequirementsEXT(&memReqInfo, &memReq);
    m_preprocessSize = memReq.memoryRequirements.size;
    if (m_preprocessSize > 0)
    {
        // Separate scratch per pass so the opaque and transparent executes don't alias preprocess memory.
        for (uint32 i = 0; i < RendererVKLayout::NUM_FRAMES_IN_FLIGHT; i++)
        {
            m_preprocessBuffers[i].initialize(m_preprocessSize,
                vk::BufferUsageFlagBits2::ePreprocessBufferEXT | vk::BufferUsageFlagBits2::eShaderDeviceAddress,
                vk::MemoryPropertyFlagBits::eDeviceLocal, false, "StaticMesh.dgcPreprocess");
            m_transparentPreprocessBuffers[i].initialize(m_preprocessSize,
                vk::BufferUsageFlagBits2::ePreprocessBufferEXT | vk::BufferUsageFlagBits2::eShaderDeviceAddress,
                vk::MemoryPropertyFlagBits::eDeviceLocal, false, "StaticMesh.dgcPreprocessTransparent");
        }
    }
}

void StaticMeshGraphicsPipeline::reloadShaders(vk::RenderPass renderPass, uint32 maxTextures)
{
    // sceneColor's render pass is recreated on window resize, so refresh the cached handle rather than
    // reloading against the (possibly dangling) one captured at initialize().
    m_renderPass = renderPass;
    if (!m_renderPass)
        return;

    GraphicsPipelineLayout graphicsPipelineLayout;
    buildPipelineLayout(graphicsPipelineLayout, maxTextures);
    if (!m_graphicsPipeline.reloadShaders(m_renderPass, graphicsPipelineLayout))
    {
        printf("StaticMeshGraphicsPipeline: shader reload failed, keeping previous pipeline\n");
        return;
    }
    // The tess pipeline only while tessellation is on (off: kept as last built, never recorded).
    if (m_terrainTess)
    {
        GraphicsPipelineLayout terrainTessLayout;
        buildTerrainTessLayout(graphicsPipelineLayout, terrainTessLayout);
        if (!m_terrainTessBuilt)
            m_terrainTessBuilt = m_terrainTessPipeline.initialize(m_renderPass, terrainTessLayout);
        else if (!m_terrainTessPipeline.reloadShaders(m_renderPass, terrainTessLayout))
            printf("StaticMeshGraphicsPipeline: terrain tess shader reload failed, keeping previous pipeline\n");
    }

    m_indirectExecutionSet.destroy();
    m_indirectExecutionSet.initialize(m_graphicsPipeline, "StaticMesh.executionSet");
}

void StaticMeshGraphicsPipeline::record(CommandBuffer& commandBuffer, uint32 frameIdx, RecordParams& params, bool updateDescriptors)
{
    oc::array<DescriptorSetUpdateInfo, 18> graphicsDescriptorSetUpdateInfos
    {
        DescriptorSetUpdateInfo{
            .binding = 0,
            .type = vk::DescriptorType::eUniformBuffer,
            .bufferInfos = {
                vk::DescriptorBufferInfo {
                    .buffer = params.ubo.getBuffer(),
                    .range = sizeof(RendererVKLayout::Ubo),
                }
            }
        },
        DescriptorSetUpdateInfo{
            .binding = 1,
            .type = vk::DescriptorType::eStorageBuffer,
            .bufferInfos = {
                vk::DescriptorBufferInfo {
                    .buffer = params.meshInstanceBuffer.getBuffer(),
                    .range = params.meshInstanceBuffer.getSize(),
                }
            }
        },
        DescriptorSetUpdateInfo{
            .binding = 2,
            .type = vk::DescriptorType::eStorageBuffer,
            .bufferInfos = {
                vk::DescriptorBufferInfo {
                    .buffer = params.materialInfoBuffer.getBuffer(),
                    .range = params.materialInfoBuffer.getSize(),
                }
            }
        },
        DescriptorSetUpdateInfo{
            .binding = 4,
            .type = vk::DescriptorType::eStorageBuffer,
            .bufferInfos = {
                vk::DescriptorBufferInfo {
                    .buffer = params.lightInfosBuffer.getBuffer(),
                    .range = params.lightInfosBuffer.getSize(),
                }
            }
        },
        DescriptorSetUpdateInfo{
            .binding = 5,
            .type = vk::DescriptorType::eStorageBuffer,
            .bufferInfos = {
                vk::DescriptorBufferInfo {
                    .buffer = params.lightGridsBuffer.getBuffer(),
                    .range = params.lightGridsBuffer.getSize(),
                }
            }
        },
        DescriptorSetUpdateInfo{
            .binding = 6,
            .type = vk::DescriptorType::eStorageBuffer,
            .bufferInfos = {
                vk::DescriptorBufferInfo {
                    .buffer = params.lightTableBuffer.getBuffer(),
                    .range = params.lightTableBuffer.getSize(),
                }
            }
        },
        DescriptorSetUpdateInfo{ // FFT ocean displacement/gradient maps (VS displacement + FS shading)
            .binding = 7,
            .type = vk::DescriptorType::eCombinedImageSampler,
            .imageInfos = {
                vk::DescriptorImageInfo {
                    .sampler = params.oceanMapsSampler,
                    .imageView = params.oceanMapsView,
                    .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                }
            }
        },
        DescriptorSetUpdateInfo{
            .binding = 8,
            .type = vk::DescriptorType::eCombinedImageSampler,
            .imageInfos = {
                vk::DescriptorImageInfo {
                    .sampler = params.shadowMapSampler,
                    .imageView = params.shadowMapView,
                    .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                }
            }
        },
        DescriptorSetUpdateInfo{
            .binding = 9,
            .type = vk::DescriptorType::eCombinedImageSampler,
            .imageInfos = {
                vk::DescriptorImageInfo {
                    .sampler = params.shadowMapDepthSampler,
                    .imageView = params.shadowMapView,
                    .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                }
            }
        },
        DescriptorSetUpdateInfo{
            .binding = 10,
            .type = vk::DescriptorType::eStorageBuffer,
            .bufferInfos = { vk::DescriptorBufferInfo { .buffer = params.giGridDataBuffer.getBuffer(), .range = params.giGridDataBuffer.getSize() } }
        },
        DescriptorSetUpdateInfo{
            .binding = 12,
            .type = vk::DescriptorType::eCombinedImageSampler,
            .imageInfos = {
                vk::DescriptorImageInfo {
                    // LAST frame's scene depth: the other slot's image, parked in its sampled layout.
                    .sampler = params.prevDepthSampler,
                    .imageView = params.prevDepthView,
                    .imageLayout = SCENE_DEPTH_SAMPLED_LAYOUT,
                }
            }
        },
        DescriptorSetUpdateInfo{
            .binding = 14,
            .type = vk::DescriptorType::eStorageBuffer,
            .bufferInfos = { vk::DescriptorBufferInfo { .buffer = params.vertexBuffer.getBuffer(), .range = params.vertexBuffer.getSize() } }
        },
        DescriptorSetUpdateInfo{
            .binding = 15,
            .type = vk::DescriptorType::eStorageBuffer,
            .bufferInfos = { vk::DescriptorBufferInfo { .buffer = params.indexBuffer.getBuffer(), .range = params.indexBuffer.getSize() } }
        },
        DescriptorSetUpdateInfo{
            .binding = 16,
            .type = vk::DescriptorType::eStorageBuffer,
            .bufferInfos = { vk::DescriptorBufferInfo { .buffer = params.meshInfoBuffer.getBuffer(), .range = params.meshInfoBuffer.getSize() } }
        },
        DescriptorSetUpdateInfo{
            .binding = 17,
            .type = vk::DescriptorType::eStorageBuffer,
            .bufferInfos = { vk::DescriptorBufferInfo { .buffer = params.rtMeshInstancesBuffer.getBuffer(), .range = params.rtMeshInstancesBuffer.getSize() } }
        },
        DescriptorSetUpdateInfo{ // [15] the texture array
            .binding = 22,
            .type = vk::DescriptorType::eCombinedImageSampler,
        },
        DescriptorSetUpdateInfo{}, // [16] + [17] the GI volume cascades + sky: written only while it exists
        DescriptorSetUpdateInfo{},
    };
    if (!params.giVolume.empty())
        params.giVolume.fillUpdates(21, graphicsDescriptorSetUpdateInfos[16], graphicsDescriptorSetUpdateInfos[17]);
    const size_t numUpdates = params.giVolume.empty() ? 16 : 18;

    const size_t numTextures = Globals::textureManager.getNumTextures();
    for (uint16 texIdx = 0; texIdx < (uint16)numTextures; ++texIdx)
    {
        graphicsDescriptorSetUpdateInfos[15].imageInfos.push_back(
            vk::DescriptorImageInfo{
                .sampler = m_sampler.getSampler(),
                .imageView = Globals::textureManager.getViewForDescriptor(texIdx), // freed slots -> fallback
                .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            }
        );
    }

    vk::CommandBuffer vkCommandBuffer = commandBuffer.getCommandBuffer();
    vk::DescriptorSet descriptorSet = params.descriptorSet.getDescriptorSet();
    if (updateDescriptors)
        commandBuffer.cmdUpdateDescriptorSets(m_graphicsPipeline.getPipelineLayout(), vk::PipelineBindPoint::eGraphics, descriptorSet,
            oc::span<DescriptorSetUpdateInfo>(graphicsDescriptorSetUpdateInfos.data(), numUpdates));
    // Pipeline, set, view index and vertex/index buffers: bound again after every generated-commands execute,
    // which leaves the graphics state it bound undefined.
    const auto bindForDraws = [&](vk::Pipeline pipeline, vk::PipelineLayout layout)
    {
        vkCommandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
        vkCommandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, layout, 0, 1, &descriptorSet, 0, nullptr);
        if (m_stereo) // select the eye matrix/view pos for the draws (every stage that reads a view)
            vkCommandBuffer.pushConstants(layout, VIEW_PUSH_STAGES, 0, sizeof(uint32), &params.viewIndex);
        vkCommandBuffer.bindVertexBuffers(0, { params.vertexBuffer.getBuffer() }, { 0 });
        vkCommandBuffer.bindVertexBuffers(2, { params.instanceIdxBuffer.getBuffer() }, { 0 });
        vkCommandBuffer.bindIndexBuffer(params.indexBuffer.getBuffer(), 0, vk::IndexType::eUint32);
    };
    bindForDraws(m_graphicsPipeline.getPipeline(), m_graphicsPipeline.getPipelineLayout());
    recordExecuteGeneratedCommands(vkCommandBuffer, params.indirectCommandBuffer, m_preprocessBuffers[frameIdx], params.drawCountBuffer, 0);

    // The TESSELLATED terrain: ground, then its overlay (EQUAL depth against the ground just drawn), before the
    // transparent execute - where the untessellated overlay runs. Plain indexed indirect draws over the cull's
    // compacted sequences (offset 4 skips the pipelineIndex word); with tessellation off the cull routes nothing
    // there, so the counts are 0.
    constexpr vk::DeviceSize sequenceStride = sizeof(RendererVKLayout::IndirectDrawSequence);
    const auto drawTerrainTess = [&](uint32 variant, Buffer& sequences, uint32 countIdx)
    {
        bindForDraws(m_terrainTessPipeline.getPipelineVariant(variant), m_terrainTessPipeline.getPipelineLayout());
        vkCommandBuffer.drawIndexedIndirectCount(sequences.getBuffer(), offsetof(RendererVKLayout::IndirectDrawSequence, indexCount),
            params.drawCountBuffer.getBuffer(), countIdx * sizeof(uint32), (uint32)(sequences.getSize() / sequenceStride), (uint32)sequenceStride);
    };
    if (m_terrainTess && m_terrainTessBuilt)
    {
        drawTerrainTess(0, params.terrainTessCommandBuffer, 2);
        drawTerrainTess(1, params.terrainTessOverlayCommandBuffer, 3);
        bindForDraws(m_graphicsPipeline.getPipeline(), m_graphicsPipeline.getPipelineLayout());
    }
    recordExecuteGeneratedCommands(vkCommandBuffer, params.transparentIndirectCommandBuffer, m_transparentPreprocessBuffers[frameIdx], params.drawCountBuffer, 1);
}

void StaticMeshGraphicsPipeline::recordExecuteGeneratedCommands(vk::CommandBuffer vkCommandBuffer, Buffer& indirectCommandBuffer, Buffer& preprocessBuffer, Buffer& drawCountBuffer, uint32 countIdx)
{
    // The sequence count is the cull's compacted count (DrawCompactPipeline), clamped by maxSequenceCount = the
    // buffer capacity the preprocess scratch was sized for, so registering meshes never re-records.
    const uint32 maxSequences = (uint32)(indirectCommandBuffer.getSize() / sizeof(RendererVKLayout::IndirectDrawSequence));
    vk::GeneratedCommandsInfoEXT generatedCommandsInfo{
        .shaderStages = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
        .indirectExecutionSet = m_indirectExecutionSet.getHandle(),
        .indirectCommandsLayout = m_indirectCommandsLayout.getHandle(),
        .indirectAddress = indirectCommandBuffer.getDeviceAddress(),
        .indirectAddressSize = indirectCommandBuffer.getSize(),
        .preprocessAddress = m_preprocessSize > 0 ? preprocessBuffer.getDeviceAddress() : 0,
        .preprocessSize = m_preprocessSize,
        .maxSequenceCount = maxSequences,
        .sequenceCountAddress = drawCountBuffer.getDeviceAddress() + countIdx * sizeof(uint32),
        .maxDrawCount = maxSequences,
    };
    vkCommandBuffer.executeGeneratedCommandsEXT(vk::False, generatedCommandsInfo);
}

void StaticMeshGraphicsPipeline::update(uint32 frameIdx, oc::vector<ObjectContainer*>& objectContainers)
{
}
