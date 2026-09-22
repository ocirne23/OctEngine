module RendererVK;

import RendererVK.fwd;

import Core;
import Core.fwd;
import Core.glm;
import Core.Window;
import Core.Frustum;
import Core.imgui;
import Core.Camera;
import Core.Tweaks;
import Core.Time;
import Core.Log;

import File;

import :RenderNode;
import :VK;
import :GpuProfiler;
import :Allocator;
import :StagingManager;
import :TextureManager;
import :TextureStreamer;
import :MeshStreamer;
import :MeshDataManager;
import :glslang;
import :Layout;
import :ObjectContainer;
import :LightingUtils;

// Renderer: COMMAND BUFFER RECORDING. The cached secondaries (one record*() per pass), THE scene
// stage table (buildSceneStages - see Renderer.ixx) and the two primaries that execute them, desktop
// and VR. recordCommandBuffers is the per-frame entry point.

vk::CommandBuffer Renderer::beginComputeSecondary(CommandBuffer& cb)
{
    vk::CommandBufferInheritanceInfo inheritance;
    return cb.begin(false, &inheritance);
}

vk::CommandBuffer Renderer::beginScenePassSecondary(uint32 frameIdx, CommandBuffer& cb)
{
    vk::CommandBufferInheritanceInfo inheritance{ .renderPass = m_perFrameData[frameIdx].sceneColor.getRenderPass() };
    return cb.begin(false, &inheritance);
}

void Renderer::setFullViewport(vk::CommandBuffer vkCb) const
{
    const glm::ivec2 vpSize = m_viewportRect.getSize();
    const vk::Viewport viewport{ .x = (float)m_viewportRect.min.x, .y = (float)m_viewportRect.max.y,
        .width = (float)vpSize.x, .height = -((float)vpSize.y), .minDepth = 0.0f, .maxDepth = 1.0f };
    const vk::Rect2D scissor{ .offset = vk::Offset2D{ 0, 0 }, .extent = m_swapChain.getLayout().extent };
    vkCb.setViewport(0, { viewport });
    vkCb.setScissor(0, { scissor });
}

void Renderer::recordIndirectCull(uint32 frameIdx)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    InstanceStream::FrameSlot& instances = m_instances.slot(frameIdx);
    CommandBuffer& cb = frameData.indirectCullCommandBuffer;
    beginComputeSecondary(cb);
    IndirectCullComputePipeline::RecordParams cullParams
    {
        .descriptorSet = frameData.indirectCullPipelineDescriptorSet,
        .ubo = frameData.ubo,
        .inRenderNodeTransformsBuffer = instances.transforms,
        .inMeshInstancesBuffer = instances.meshInstances,
        .inMeshInstanceOffsetsBuffer = m_instanceOffsets.getBuffer(),
        .inMeshInfoBuffer = m_meshInfos.getBuffer(),
        .inFirstInstancesBuffer = instances.firstInstances,
        .inNodePassMasksBuffer = instances.passMasks,
        .inMeshLodGroupIdxBuffer = m_meshLods.getGroupIdxBuffer(),
        .inMeshLodGroupsBuffer = m_meshLods.getGroupsBuffer(),
        .lodLevelStateBuffer = m_meshLods.getStateBuffer(),
        .inNodeLodStateBiasBuffer = instances.lodStateBias,
        .outLodStatsBuffer = frameData.lodStatsBuffer,
    };
    m_indirectCullComputePipeline.record(cb, frameIdx, cullParams);
    cb.end();
}


void Renderer::recordSkinning(uint32 frameIdx)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    CommandBuffer& cb = frameData.skinningCommandBuffer;
    beginComputeSecondary(cb);
    SkinningComputePipeline::RecordParams params{
        .descriptorSet = frameData.skinningDescriptorSet,
        .vertexBuffer = Globals::meshDataManager.getVertexBuffer(),
        .skinningBuffer = Globals::meshDataManager.getSkinningBuffer(),
    };
    m_skinningComputePipeline.record(cb, frameIdx, params);
    cb.end();
}

void Renderer::recordOceanSim(uint32 frameIdx)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    CommandBuffer& cb = frameData.oceanSimCommandBuffer;
    beginComputeSecondary(cb);
    const OceanSimulationPipeline::SprayParams spray{
        .particleCounters = &m_particlePipeline.getCountersBuffer(),
        .particleRequests = &m_particlePipeline.getSpawnRequestBuffer(),
        .terrainView = m_terrain.getHeightMap().getView(),
        .terrainSampler = m_terrain.getHeightMap().getSampler(),
    };
    m_oceanSimPipeline.record(cb, frameIdx, frameData.ubo, spray);
    cb.end();
}

void Renderer::recordTerrainWetness(uint32 frameIdx)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    CommandBuffer& cb = frameData.terrainWetnessCommandBuffer;
    beginComputeSecondary(cb);
    const TerrainWetnessPipeline::RecordParams params{
        .ubo = &frameData.ubo,
        .terrainView = m_terrain.getHeightMap().getView(),
        .terrainSampler = m_terrain.getHeightMap().getSampler(),
        .oceanMapsView = m_oceanSimPipeline.getMapsView(),
        .oceanMapsSampler = m_oceanSimPipeline.getMapsSampler(),
    };
    m_terrainWetnessPipeline.record(cb, frameIdx, params);
    cb.end();
}

void Renderer::recordLightGrid(uint32 frameIdx)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    FrameSubmission::FrameSlot& submission = m_submission.slot(frameIdx);
    CommandBuffer& cb = frameData.lightGridCommandBuffer;
    beginComputeSecondary(cb);
    LightGridComputePipeline::RecordParams params
    {
        .descriptorSet = frameData.lightGridPipelineDescriptorSet,
        .ubo = frameData.ubo,
        .outLightGridBuffer = submission.lightGrids,
    };
    m_lightGridComputePipeline.record(cb, frameIdx, params);
    cb.end();
}

void Renderer::recordShadowCull(uint32 frameIdx)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    InstanceStream::FrameSlot& instances = m_instances.slot(frameIdx);
    CommandBuffer& cb = frameData.shadowCullCommandBuffer;
    beginComputeSecondary(cb);

    ShadowCullComputePipeline::RecordParams params{
        .descriptorSet = frameData.shadowCullDescriptorSet,
        .ubo = frameData.ubo,
        .dispatchIndirectBuffer = m_indirectCullComputePipeline.getDispatchIndirectBuffer(frameIdx),
        .inRenderNodeTransformsBuffer = instances.transforms,
        .inMeshInstancesBuffer = instances.meshInstances,
        .inMeshInstanceOffsetsBuffer = m_instanceOffsets.getBuffer(),
        .inMeshInfoBuffer = m_meshInfos.getBuffer(),
        .inFirstInstancesBuffer = instances.firstInstances,
        .inMaterialInfoBuffer = m_materials.getBuffer(),
        .inNodePassMasksBuffer = instances.passMasks,
        .inMeshLodGroupIdxBuffer = m_meshLods.getGroupIdxBuffer(),
        .inMeshLodGroupsBuffer = m_meshLods.getGroupsBuffer(),
    };
    m_shadowCullComputePipeline.record(cb, frameIdx, params);
    cb.end();
}

void Renderer::recordShadowDraw(uint32 frameIdx)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    InstanceStream::FrameSlot& instances = m_instances.slot(frameIdx);
    ShadowMap& shadowMap = frameData.shadowMap;
    vk::CommandBufferInheritanceInfo inheritance{ .renderPass = shadowMap.getRenderPass() };
    CommandBuffer& cb = frameData.shadowDrawCommandBuffer;
    vk::CommandBuffer vkCb = cb.begin(false, &inheritance);

    // All cascades render in a single multiview render pass; gl_ViewIndex selects the layer.
    const vk::Extent2D shadowExtent{ shadowMap.getResolution(), shadowMap.getResolution() };
    const float shadowRes = (float)shadowMap.getResolution();
    const vk::Viewport viewport{ .x = 0.0f, .y = 0.0f, .width = shadowRes, .height = shadowRes, .minDepth = 0.0f, .maxDepth = 1.0f };
    const vk::Rect2D scissor{ .offset = vk::Offset2D{ 0, 0 }, .extent = shadowExtent };
    vkCb.setViewport(0, { viewport });
    vkCb.setScissor(0, { scissor });

    ShadowMapGraphicsPipeline::RecordParams params{
        .descriptorSet = frameData.shadowDrawDescriptorSet,
        .ubo = frameData.ubo,
        .meshInstanceBuffer = m_shadowCullComputePipeline.getOutMeshInstancesBuffer(frameIdx),
        .vertexBuffer = Globals::meshDataManager.getVertexBuffer(),
        .indexBuffer = Globals::meshDataManager.getIndexBuffer(),
        .instanceIdxBuffer = m_shadowCullComputePipeline.getInstanceIdxBuffer(frameIdx),
        .indirectCommandBuffer = m_shadowCullComputePipeline.getIndirectCommandBuffer(frameIdx),
        .meshCountBuffer = instances.meshCount,
    };
    m_shadowMapGraphicsPipeline.record(cb, frameIdx, params);
    cb.end();
}

// The weather volume's rain occlusion map: the shadow cull + depth pass pair in their RAIN_OCCLUSION
// variant (one view, u_rainOcclusionViewProj). Executed right after the indirect cull, BEFORE the
// particle sim, so the sim reads this frame's map.
void Renderer::recordRainOcclusionCull(uint32 frameIdx)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    InstanceStream::FrameSlot& instances = m_instances.slot(frameIdx);
    CommandBuffer& cb = frameData.rainCullCommandBuffer;
    beginComputeSecondary(cb);

    ShadowCullComputePipeline::RecordParams params{
        .descriptorSet = frameData.rainCullDescriptorSet,
        .ubo = frameData.ubo,
        .dispatchIndirectBuffer = m_indirectCullComputePipeline.getDispatchIndirectBuffer(frameIdx),
        .inRenderNodeTransformsBuffer = instances.transforms,
        .inMeshInstancesBuffer = instances.meshInstances,
        .inMeshInstanceOffsetsBuffer = m_instanceOffsets.getBuffer(),
        .inMeshInfoBuffer = m_meshInfos.getBuffer(),
        .inFirstInstancesBuffer = instances.firstInstances,
        .inMaterialInfoBuffer = m_materials.getBuffer(),
        .inNodePassMasksBuffer = instances.passMasks,
        .inMeshLodGroupIdxBuffer = m_meshLods.getGroupIdxBuffer(),
        .inMeshLodGroupsBuffer = m_meshLods.getGroupsBuffer(),
    };
    m_rainCullComputePipeline.record(cb, frameIdx, params);
    cb.end();
}

void Renderer::recordRainOcclusionDraw(uint32 frameIdx)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    InstanceStream::FrameSlot& instances = m_instances.slot(frameIdx);
    ShadowMap& map = frameData.rainOcclusionMap;
    vk::CommandBufferInheritanceInfo inheritance{ .renderPass = map.getRenderPass() };
    CommandBuffer& cb = frameData.rainDrawCommandBuffer;
    vk::CommandBuffer vkCb = cb.begin(false, &inheritance);

    const float res = (float)map.getResolution();
    const vk::Viewport viewport{ .x = 0.0f, .y = 0.0f, .width = res, .height = res, .minDepth = 0.0f, .maxDepth = 1.0f };
    const vk::Rect2D scissor{ .offset = vk::Offset2D{ 0, 0 }, .extent = vk::Extent2D{ map.getResolution(), map.getResolution() } };
    vkCb.setViewport(0, { viewport });
    vkCb.setScissor(0, { scissor });

    ShadowMapGraphicsPipeline::RecordParams params{
        .descriptorSet = frameData.rainDrawDescriptorSet,
        .ubo = frameData.ubo,
        .meshInstanceBuffer = m_rainCullComputePipeline.getOutMeshInstancesBuffer(frameIdx),
        .vertexBuffer = Globals::meshDataManager.getVertexBuffer(),
        .indexBuffer = Globals::meshDataManager.getIndexBuffer(),
        .instanceIdxBuffer = m_rainCullComputePipeline.getInstanceIdxBuffer(frameIdx),
        .indirectCommandBuffer = m_rainCullComputePipeline.getIndirectCommandBuffer(frameIdx),
        .meshCountBuffer = instances.meshCount,
    };
    m_rainMapGraphicsPipeline.record(cb, frameIdx, params);
    cb.end();
}

void Renderer::recordSceneDepthToSampled(vk::CommandBuffer cb, vk::Image sceneDepth, uint32 eyeIndex)
{
    // The depth-writing scene stages are done: flush their writes and park the depth in its sampled /
    // read-only-attachment layout for the rest of the frame (AO, force march, the later scene stages,
    // TAA) and for next frame's readers.
    const vk::ImageMemoryBarrier2 barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests,
        .srcAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests
            | vk::PipelineStageFlagBits2::eFragmentShader | vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eShaderSampledRead,
        .oldLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal,
        .newLayout = SCENE_DEPTH_SAMPLED_LAYOUT,
        .image = sceneDepth,
        .subresourceRange = { vk::ImageAspectFlagBits::eDepth, 0, 1, eyeIndex, 1 },
    };
    cb.pipelineBarrier2(vk::DependencyInfo{ .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier });
}

void Renderer::recordStaticMesh(uint32 frameIdx)
{
    CommandBuffer& cb = m_perFrameData[frameIdx].staticMeshCommandBuffer;
    beginScenePassSecondary(frameIdx, cb);
    recordStaticMeshInto(cb, frameIdx, 0);
    cb.end();
}

void Renderer::recordStaticMeshInto(CommandBuffer& cb, uint32 frameIdx, uint32 eyeIndex)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    InstanceStream::FrameSlot& instances = m_instances.slot(frameIdx);
    FrameSubmission::FrameSlot& submission = m_submission.slot(frameIdx);
    setFullViewport(cb.getCommandBuffer());
    StaticMeshGraphicsPipeline::RecordParams drawParams
    {
        .descriptorSet = frameData.staticMeshPipelineDescriptorSet[eyeIndex],
        .ubo = frameData.ubo,
        .vertexBuffer = Globals::meshDataManager.getVertexBuffer(),
        .indexBuffer = Globals::meshDataManager.getIndexBuffer(),
        .materialInfoBuffer = m_materials.getBuffer(),
        .instanceIdxBuffer = m_indirectCullComputePipeline.getInstanceIdxBuffer(frameIdx),
        .meshInstanceBuffer = m_indirectCullComputePipeline.getOutMeshInstancesBuffer(frameIdx),
        .indirectCommandBuffer = m_indirectCullComputePipeline.getIndirectCommandBuffer(frameIdx),
        .transparentIndirectCommandBuffer = m_indirectCullComputePipeline.getTransparentIndirectCommandBuffer(frameIdx),
        .terrainTessCommandBuffer = m_indirectCullComputePipeline.getTerrainTessCommandBuffer(frameIdx),
        .terrainTessOverlayCommandBuffer = m_indirectCullComputePipeline.getTerrainTessOverlayCommandBuffer(frameIdx),
        .meshCountBuffer = instances.meshCount,
        .lightInfosBuffer = submission.lightInfos,
        .lightGridsBuffer = submission.lightGrids,
        .lightTableBuffer = submission.lightTable,
        .giGridDataBuffer = m_giProbePipeline.getGiGridDataBuffer(),
        .giVolume = m_giProbePipeline.getVolumeDescriptors(),
        .meshInfoBuffer = m_meshInfos.getBuffer(),
        .rtMeshInstancesBuffer = instances.meshInstances,
        .shadowMapView = frameData.shadowMap.getSampleView(),
        .shadowMapSampler = frameData.shadowMap.getSampler(),
        .shadowMapDepthSampler = frameData.shadowMap.getDepthSampler(),
        .prevDepthView = m_perFrameData[(frameIdx + 1) % RendererVKLayout::NUM_FRAMES_IN_FLIGHT].sceneColor.getDepthView(eyeIndex),
        .prevDepthSampler = frameData.sceneColor.getDepthSampler(),
        .oceanMapsView = m_oceanSimPipeline.getMapsView(),
        .oceanMapsSampler = m_oceanSimPipeline.getMapsSampler(),
        .viewIndex = RendererVKLayout::eyeToViewIndex(eyeIndex, m_sceneViewCount),
    };
    // Each eye has its own descriptor set (per-eye AO + last frame's depth), so both eyes write their own.
    m_staticMeshGraphicsPipeline.record(cb, frameIdx, drawParams, true);
}

// AO trace+denoise for one eye (compute, no render pass); reads that eye's scene depth - so it runs
// AFTER the depth-writing scene stages - and writes that eye's AO, which NEXT frame's forward pass reads.
void Renderer::recordAOInto(CommandBuffer& cb, uint32 frameIdx, uint32 eyeIndex)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    InstanceStream::FrameSlot& instances = m_instances.slot(frameIdx);
    const vk::AccelerationStructureKHR tlas = m_rt.accel().getTlas(frameIdx);
    if (!m_rtParams.enabled || m_meshInfos.count() == 0 || !tlas) // RT off: tlas is stale, don't trace it
        return;
    const uint32 prevFrameIdx = (frameIdx + 1) % RendererVKLayout::NUM_FRAMES_IN_FLIGHT;
    RTAOPipeline::RecordParams aoParams{
        .ubo = frameData.ubo,
        .sceneDepthView = frameData.sceneColor.getDepthView(eyeIndex),
        .prevSceneDepthView = m_perFrameData[prevFrameIdx].sceneColor.getDepthView(eyeIndex),
        .sceneDepthSampler = frameData.sceneColor.getDepthSampler(),
        .tlas = tlas,
        .vertexBuffer = Globals::meshDataManager.getVertexBuffer(),
        .indexBuffer = Globals::meshDataManager.getIndexBuffer(),
        .meshInfos = m_meshInfos.getBuffer(),
        .meshInstances = instances.meshInstances,
        .materialInfos = m_materials.getBuffer(),
    };
    m_rtaoPipeline.record(cb, frameIdx, eyeIndex, aoParams);
}

// Fog apply draw for one eye, inside the eye's scene-colour render pass (viewport set by the caller).
void Renderer::recordFogApplyInto(CommandBuffer& cb, uint32 frameIdx, uint32 eyeIndex)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    vk::CommandBuffer vkCb = cb.getCommandBuffer();
    const vk::Extent2D extent = m_swapChain.getLayout().extent;
    const glm::ivec2 vpMin = m_viewportRect.min;
    const glm::ivec2 vpSize = m_viewportRect.getSize();
    vkCb.setViewport(0, vk::Viewport{ .x = 0.0f, .y = 0.0f, .width = (float)extent.width, .height = (float)extent.height, .minDepth = 0.0f, .maxDepth = 1.0f });
    vkCb.setScissor(0, vk::Rect2D{ .offset = vk::Offset2D{ vpMin.x, vpMin.y }, .extent = vk::Extent2D{ (uint32)vpSize.x, (uint32)vpSize.y } });
    VolumetricFogPipeline::ApplyParams params{
        .ubo = frameData.ubo,
        .giGridDataBuffer = m_giProbePipeline.getGiGridDataBuffer(),
        .giVolume = m_giProbePipeline.getVolumeDescriptors(),
        .sceneDepthView = frameData.sceneColor.getDepthView(eyeIndex),
        .sceneDepthLayout = SCENE_DEPTH_SAMPLED_LAYOUT, // also this stage's read-only depth attachment
        .sceneDepthSampler = frameData.sceneColor.getDepthSampler(),
    };
    m_volumetricFogPipeline.recordApply(cb, frameIdx, eyeIndex, params);
}

// TAA resolve for one eye (compute, no render pass); reads that eye's scene colour + depth, writes its history.
void Renderer::recordTaaInto(CommandBuffer& cb, uint32 frameIdx, uint32 eyeIndex)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    SceneColor& sceneColor = frameData.sceneColor;
    const uint32 prevFrameIdx = (frameIdx + 1) % RendererVKLayout::NUM_FRAMES_IN_FLIGHT;
    TaaPipeline::RecordParams taaParams{
        .ubo = frameData.ubo,
        .currentColorView = sceneColor.getColorLayerView(eyeIndex),
        .currentColorSampler = sceneColor.getSampler(),
        .sceneDepthView = sceneColor.getDepthView(eyeIndex),
        .prevSceneDepthView = m_perFrameData[prevFrameIdx].sceneColor.getDepthView(eyeIndex),
        .sceneDepthSampler = sceneColor.getDepthSampler(),
        .feedback = m_taaParams.taaEnabled ? m_taaParams.taaFeedback : 0.0f,
        .oceanFeedback = m_taaParams.taaEnabled ? m_taaParams.taaOceanFeedback : 0.0f,
    };
    m_taaPipeline.record(cb, frameIdx, eyeIndex, taaParams);
}

void Renderer::recordGiProbeDebug(uint32 frameIdx)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    CommandBuffer& cb = frameData.giProbeDebugCommandBuffer;
    setFullViewport(beginScenePassSecondary(frameIdx, cb));
    m_giProbePipeline.recordDebugDraw(cb, frameIdx, frameData.ubo);
    cb.end();
}

void Renderer::recordDebugLines(uint32 frameIdx)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    CommandBuffer& cb = frameData.debugLineCommandBuffer;
    setFullViewport(beginScenePassSecondary(frameIdx, cb));
    m_debugLinePipeline.record(cb, frameIdx, frameData.ubo);
    cb.end();
}

// Particle GPU sim (begin/emit/simulate compute chain), its own secondary outside any render pass;
// executed right after the light grid in the primary. Reads LAST frame's scene depth for depth collision.
void Renderer::recordParticleSim(uint32 frameIdx)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    CommandBuffer& cb = frameData.particleSimCommandBuffer;
    beginComputeSecondary(cb);
    const uint32 prevFrameIdx = (frameIdx + 1) % RendererVKLayout::NUM_FRAMES_IN_FLIGHT;
    ParticlePipeline::SimParams simParams{
        .ubo = frameData.ubo,
        .prevDepthView = m_perFrameData[prevFrameIdx].sceneColor.getDepthView(),
        .sceneDepthSampler = frameData.sceneColor.getDepthSampler(),
        .rainOcclusionView = frameData.rainOcclusionMap.getSampleView(),
        .rainOcclusionSampler = frameData.rainOcclusionMap.getDepthSampler(),
        .oceanMapsView = m_oceanSimPipeline.getMapsView(),
        .oceanMapsSampler = m_oceanSimPipeline.getMapsSampler(),
        .terrainView = m_terrain.getHeightMap().getView(),
        .terrainSampler = m_terrain.getHeightMap().getSampler(),
    };
    m_particlePipeline.recordSim(cb, frameIdx, simParams);
    cb.end();
}

// Particle billboard draw for one eye, inside the eye's scene-colour render pass (after the opaque
// forward + decal draws, before fog apply).
void Renderer::recordParticlesInto(CommandBuffer& cb, uint32 frameIdx, uint32 eyeIndex)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    FrameSubmission::FrameSlot& submission = m_submission.slot(frameIdx);
    setFullViewport(cb.getCommandBuffer());
    ParticlePipeline::DrawParams drawParams{
        .ubo = frameData.ubo,
        .giGridDataBuffer = m_giProbePipeline.getGiGridDataBuffer(),
        .giVolume = m_giProbePipeline.getVolumeDescriptors(),
        .sceneDepthView = frameData.sceneColor.getDepthView(eyeIndex),
        .sceneDepthLayout = SCENE_DEPTH_SAMPLED_LAYOUT, // also this stage's read-only depth attachment
        .sceneDepthSampler = frameData.sceneColor.getDepthSampler(),
        .terrainView = m_terrain.getHeightMap().getView(),
        .terrainSampler = m_terrain.getHeightMap().getSampler(),
        .lightInfosBuffer = &submission.lightInfos,
        .lightGridsBuffer = &submission.lightGrids,
        .lightTableBuffer = &submission.lightTable,
    };
    m_particlePipeline.recordDraw(cb, frameIdx, eyeIndex, drawParams);
}

void Renderer::recordParticles(uint32 frameIdx)
{
    CommandBuffer& cb = m_perFrameData[frameIdx].particleCommandBuffer;
    beginScenePassSecondary(frameIdx, cb);
    recordParticlesInto(cb, frameIdx, 0);
    cb.end();
}

// Projected decal draw for one eye, inside the eye's scene-colour render pass right after the opaque
// forward draw (so particles/fog layer on top).
void Renderer::recordDecalsInto(CommandBuffer& cb, uint32 frameIdx, uint32 eyeIndex)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    setFullViewport(cb.getCommandBuffer());
    DecalPipeline::DrawParams drawParams{
        .ubo = frameData.ubo,
        .giGridDataBuffer = m_giProbePipeline.getGiGridDataBuffer(),
        .giVolume = m_giProbePipeline.getVolumeDescriptors(),
        .sceneDepthView = frameData.sceneColor.getDepthView(eyeIndex),
        .sceneDepthLayout = SCENE_DEPTH_SAMPLED_LAYOUT, // also this stage's read-only depth attachment
        .sceneDepthSampler = frameData.sceneColor.getDepthSampler(),
    };
    m_decalPipeline.recordDraw(cb, frameIdx, eyeIndex, drawParams);
}

void Renderer::recordDecals(uint32 frameIdx)
{
    CommandBuffer& cb = m_perFrameData[frameIdx].decalCommandBuffer;
    beginScenePassSecondary(frameIdx, cb);
    recordDecalsInto(cb, frameIdx, 0);
    cb.end();
}

// Forcefield shell draw for one eye, inside the eye's scene-colour render pass after the debug
// overlays (so particles/fog layer on top of the bubbles). part splits the proxy draw and the
// union march into their own scene stages on desktop (see recordForceField); VR records Both.
void Renderer::recordForceFieldInto(CommandBuffer& cb, uint32 frameIdx, uint32 eyeIndex,
    ForceFieldPipeline::EDrawPart part)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    setFullViewport(cb.getCommandBuffer());
    ForceFieldPipeline::DrawParams drawParams{
        .ubo = frameData.ubo,
        .sceneDepthView = frameData.sceneColor.getDepthView(eyeIndex),
        .sceneDepthLayout = SCENE_DEPTH_SAMPLED_LAYOUT, // also this stage's read-only depth attachment
        .sceneDepthSampler = frameData.sceneColor.getDepthSampler(),
    };
    m_forceFieldPipeline.recordDraw(cb, frameIdx, eyeIndex, drawParams, part);
}

void Renderer::recordForceShells(uint32 frameIdx)
{
    CommandBuffer& cb = m_perFrameData[frameIdx].forceFieldCommandBuffer;
    beginScenePassSecondary(frameIdx, cb);
    recordForceFieldInto(cb, frameIdx, 0, ForceFieldPipeline::EDrawPart::Proxies);
    cb.end();
}

void Renderer::recordForceUnion(uint32 frameIdx)
{
    CommandBuffer& cb = m_perFrameData[frameIdx].forceUnionCommandBuffer;
    beginScenePassSecondary(frameIdx, cb);
    recordForceFieldInto(cb, frameIdx, 0, ForceFieldPipeline::EDrawPart::UnionMarch);
    cb.end();
}

// Force grid build + per-emitter force / point-query dispatches (outside any render pass, after the
// light grid). All dispatches ride mapped indirect buffers, so emitter/query changes never re-record.
void Renderer::recordForceCompute(uint32 frameIdx)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    CommandBuffer& cb = frameData.forceComputeCommandBuffer;
    beginComputeSecondary(cb);
    m_forceFieldPipeline.recordCompute(cb, frameIdx, frameData.ubo);
    cb.end();
}

// The union march's interval pass + the HALF-RES march itself (each its own render pass, executed
// before the scene stages on desktop): the analytic-tier proxies MIN-blend their ray intervals at
// half res, the march walks each covered half-res pixel once, and the "Force union blend" scene
// stage upsamples the result depth-aware into scene color. The render passes themselves begin and
// end in the PRIMARY (a secondary cannot begin one); these two render-pass-continue secondaries hold
// the draws, one per pass so the GPU profiler scopes them separately. Cached: every input is a
// per-slot handle (UBO, emitter/grid/indirect buffers, interval target, scene depth), the
// half-res toggle and the resizes already force a re-record, and the draws are indirect.
// Viewport/scissor are HALVED to match the targets (the FS maps uv back with x2). The march samples
// THIS frame's scene depth, so both passes run after the depth-writing scene stages.
void Renderer::recordForceMarch(uint32 frameIdx)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    const bool halfRes = m_forceFieldPipeline.getUnionHalfRes();
    const float vpScale = halfRes ? 0.5f : 1.0f;
    const glm::ivec2 vpSize = m_viewportRect.getSize();
    const vk::Viewport marchViewport{ .x = (float)m_viewportRect.min.x * vpScale, .y = (float)m_viewportRect.max.y * vpScale,
        .width = (float)vpSize.x * vpScale, .height = -((float)vpSize.y * vpScale), .minDepth = 0.0f, .maxDepth = 1.0f };
    const vk::Extent2D fullExtent = m_swapChain.getLayout().extent;
    const vk::Rect2D marchScissor{ .offset = vk::Offset2D{ 0, 0 },
        .extent = halfRes ? vk::Extent2D{ glm::max(fullExtent.width / 2u, 1u), glm::max(fullExtent.height / 2u, 1u) } : fullExtent };
    vk::CommandBufferInheritanceInfo intervalInheritance{ .renderPass = m_forceFieldPipeline.getIntervalRenderPass(), .framebuffer = m_forceFieldPipeline.getIntervalFramebuffer() };
    CommandBuffer& intervalCb = frameData.forceIntervalCommandBuffer;
    intervalCb.begin(false, &intervalInheritance);
    m_forceFieldPipeline.recordIntervalDraw(intervalCb, frameIdx, frameData.ubo, marchViewport, marchScissor);
    intervalCb.end();
    if (halfRes) // full-res mode has no march target (the secondary is then never executed)
    {
        vk::CommandBufferInheritanceInfo marchInheritance{ .renderPass = m_forceFieldPipeline.getMarchRenderPass(), .framebuffer = m_forceFieldPipeline.getMarchFramebuffer() };
        CommandBuffer& marchCb = frameData.forceMarchCommandBuffer;
        marchCb.begin(false, &marchInheritance);
        m_forceFieldPipeline.recordUnionMarchDraw(marchCb, frameIdx, frameData.ubo,
            marchViewport, marchScissor, frameData.sceneColor.getDepthView(0), frameData.sceneColor.getDepthSampler());
        marchCb.end();
    }
}

void Renderer::recordAO(uint32 frameIdx)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    InstanceStream::FrameSlot& instances = m_instances.slot(frameIdx);
    CommandBuffer& cb = frameData.aoCommandBuffer;
    beginComputeSecondary(cb);
    const vk::AccelerationStructureKHR tlas = m_rt.accel().getTlas(frameIdx);
    if (m_rtParams.enabled && m_rtaoParams.enabled && m_meshInfos.count() > 0 && tlas)
    {
        const uint32 prevFrameIdx = (frameIdx + 1) % RendererVKLayout::NUM_FRAMES_IN_FLIGHT;
        RTAOPipeline::RecordParams aoParams{
            .ubo = frameData.ubo,
            .sceneDepthView = frameData.sceneColor.getDepthView(),
            .prevSceneDepthView = m_perFrameData[prevFrameIdx].sceneColor.getDepthView(),
            .sceneDepthSampler = frameData.sceneColor.getDepthSampler(),
            .tlas = tlas,
            .vertexBuffer = Globals::meshDataManager.getVertexBuffer(),
            .indexBuffer = Globals::meshDataManager.getIndexBuffer(),
            .meshInfos = m_meshInfos.getBuffer(),
            .meshInstances = instances.meshInstances,
            .materialInfos = m_materials.getBuffer(),
        };
        m_rtaoPipeline.record(cb, frameIdx, 0, aoParams);
    }
    cb.end();
}

void Renderer::recordVolumetricFog(uint32 frameIdx)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    FrameSubmission::FrameSlot& submission = m_submission.slot(frameIdx);
    CommandBuffer& cb = frameData.volumetricFogCommandBuffer;
    beginComputeSecondary(cb);
    const vk::AccelerationStructureKHR tlas = m_rt.accel().getTlas(frameIdx);
    if (m_rtParams.enabled && m_meshInfos.count() > 0 && tlas)
    {
        VolumetricFogPipeline::RecordParams params{
            .ubo = frameData.ubo,
            .lightInfosBuffer = submission.lightInfos,
            .lightGridsBuffer = submission.lightGrids,
            .lightTableBuffer = submission.lightTable,
            .fogVolumesBuffer = submission.fogVolumes,
            .giGridDataBuffer = m_giProbePipeline.getGiGridDataBuffer(),
            .giVolume = m_giProbePipeline.getVolumeDescriptors(),
            .shadowMapView = frameData.shadowMap.getSampleView(),
            .shadowMapSampler = frameData.shadowMap.getSampler(),
            .oceanMapsView = m_oceanSimPipeline.getMapsView(),
            .oceanMapsSampler = m_oceanSimPipeline.getMapsSampler(),
            .tlas = tlas,
        };
        m_volumetricFogPipeline.record(cb, frameIdx, params);
    }
    cb.end();
}

void Renderer::recordFogApply(uint32 frameIdx)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    CommandBuffer& cb = frameData.fogApplyCommandBuffer;
    vk::CommandBuffer vkCb = beginScenePassSecondary(frameIdx, cb);

    // Fullscreen triangle in full-render-target UV space (like the composite), scissored to the viewport.
    const vk::Extent2D extent = m_swapChain.getLayout().extent;
    const glm::ivec2 vpMin = m_viewportRect.min;
    const glm::ivec2 vpSize = m_viewportRect.getSize();
    vkCb.setViewport(0, vk::Viewport{ .x = 0.0f, .y = 0.0f, .width = (float)extent.width, .height = (float)extent.height, .minDepth = 0.0f, .maxDepth = 1.0f });
    vkCb.setScissor(0, vk::Rect2D{ .offset = vk::Offset2D{ vpMin.x, vpMin.y }, .extent = vk::Extent2D{ (uint32)vpSize.x, (uint32)vpSize.y } });

    VolumetricFogPipeline::ApplyParams params{
        .ubo = frameData.ubo,
        .giGridDataBuffer = m_giProbePipeline.getGiGridDataBuffer(),
        .giVolume = m_giProbePipeline.getVolumeDescriptors(),
        .sceneDepthView = frameData.sceneColor.getDepthView(),
        .sceneDepthLayout = SCENE_DEPTH_SAMPLED_LAYOUT, // also this stage's read-only depth attachment
        .sceneDepthSampler = frameData.sceneColor.getDepthSampler(),
    };
    m_volumetricFogPipeline.recordApply(cb, frameIdx, 0, params);
    cb.end();
}

void Renderer::recordTaa(uint32 frameIdx)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    SceneColor& sceneColor = frameData.sceneColor;
    CommandBuffer& cb = frameData.taaCommandBuffer;
    beginComputeSecondary(cb);
    const uint32 prevFrameIdx = (frameIdx + 1) % RendererVKLayout::NUM_FRAMES_IN_FLIGHT;
    TaaPipeline::RecordParams taaParams{
        .ubo = frameData.ubo,
        .currentColorView = sceneColor.getColorView(),
        .currentColorSampler = sceneColor.getSampler(),
        .sceneDepthView = sceneColor.getDepthView(),
        .prevSceneDepthView = m_perFrameData[prevFrameIdx].sceneColor.getDepthView(),
        .sceneDepthSampler = sceneColor.getDepthSampler(),
        .feedback = m_taaParams.taaEnabled ? m_taaParams.taaFeedback : 0.0f,
        .oceanFeedback = m_taaParams.taaEnabled ? m_taaParams.taaOceanFeedback : 0.0f,
    };
    m_taaPipeline.record(cb, frameIdx, 0, taaParams);
    cb.end();
}

void Renderer::recordEyeAdaptation(uint32 frameIdx)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    CommandBuffer& cb = frameData.eyeAdaptCommandBuffer;
    beginComputeSecondary(cb);
    // TAA OFF: the pass is not recorded or executed at all (see recordCommandBuffers), so the
    // post chain reads this frame's scene colour directly instead of TAA's resolved image.
    const bool taaOn = m_taaParams.taaEnabled;
    EyeAdaptationPipeline::RecordParams params{
        .resolvedView = taaOn ? m_taaPipeline.getResolvedView(frameIdx, 0) : frameData.sceneColor.getColorLayerView(0),
        .resolvedLayout = taaOn ? vk::ImageLayout::eGeneral : vk::ImageLayout::eShaderReadOnlyOptimal,
        .sampler = taaOn ? m_taaPipeline.getSampler() : frameData.sceneColor.getSampler(),
        .viewportMin = m_viewportRect.min,
        .viewportSize = m_viewportRect.getSize(),
    };
    m_eyeAdaptationPipeline.record(cb, frameIdx, params);
    cb.end();
}

void Renderer::recordComposite(uint32 frameIdx)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    vk::CommandBufferInheritanceInfo inheritance { .renderPass = m_renderPass.getRenderPass(), };
    CommandBuffer& cb = frameData.compositeCommandBuffer;
    vk::CommandBuffer vkCb = cb.begin(false, &inheritance);

    const vk::Extent2D extent = m_swapChain.getLayout().extent;
    const glm::ivec2 vpMin = m_viewportRect.min;
    const glm::ivec2 vpSize = m_viewportRect.getSize();
    vkCb.setViewport(0, vk::Viewport{.x = 0.0f, .y = 0.0f, .width = (float)extent.width, .height = (float)extent.height, .minDepth = 0.0f, .maxDepth = 1.0f });
    vkCb.setScissor(0, vk::Rect2D{.offset = vk::Offset2D{ vpMin.x, vpMin.y }, .extent = vk::Extent2D{ (uint32)vpSize.x, (uint32)vpSize.y } });

    const bool taaOn = m_taaParams.taaEnabled; // TAA bypassed: tonemap the scene colour directly
    CompositePipeline::RecordParams params{
        .descriptorSet = frameData.compositeDescriptorSet,
        .resolvedView = taaOn ? m_taaPipeline.getResolvedView(frameIdx, 0) : frameData.sceneColor.getColorLayerView(0),
        .resolvedLayout = taaOn ? vk::ImageLayout::eGeneral : vk::ImageLayout::eShaderReadOnlyOptimal,
        .sampler = taaOn ? m_taaPipeline.getSampler() : frameData.sceneColor.getSampler(),
        .exposureBuffer = m_eyeAdaptationPipeline.getExposureBuffer().getBuffer(),
        .exposureEV = m_postParams.exposureEV,
        .tonemapper = m_postParams.tonemapper,
        .autoExposure = m_postParams.autoExposure ? 1 : 0,
    };
    m_compositePipeline.record(cb, params);
    cb.end();
}

// The PER-FRAME half of GI: the work whose content changes frame to frame - one-shot static BLAS builds
// for meshes added since last frame, compaction copies whose size queries matured, the skinned BLAS rebuild
// from this frame's deformed vertices, and the one-time probe-volume clear. Empty on most frames. Executed
// by the primary right before the cached GI secondary (recordGlobalIllum), which builds the TLAS from
// these BLASes and traces.
void Renderer::recordGlobalIllumPrep(uint32 frameIdx)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    CommandBuffer& prepCommandBuffer = frameData.giPrepCommandBuffer;
    vk::CommandBuffer vkPrepCommandBuffer = beginComputeSecondary(prepCommandBuffer);

    // RT master toggle off: nothing is built (no acceleration-structure churn - diagnostic A/B); the
    // cached secondary then holds only the sky map bake.
    if (!m_rtParams.enabled)
    {
        prepCommandBuffer.end();
        return;
    }

    auto fullBarrier = [&](vk::PipelineStageFlags2 srcStage, vk::AccessFlags2 srcAccess,
        vk::PipelineStageFlags2 dstStage, vk::AccessFlags2 dstAccess)
        {
            vk::MemoryBarrier2 bar{ .srcStageMask = srcStage, .srcAccessMask = srcAccess, .dstStageMask = dstStage, .dstAccessMask = dstAccess };
            vkPrepCommandBuffer.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &bar });
        };

    // The skinning compute (executed earlier in the primary) wrote the deformed vertices that both the
    // one-time static build (for skinned output regions) and the per-frame skinned rebuild read.
    if (!m_skinned.getBlasBuilds().empty())
        fullBarrier(vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageWrite,
            vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR, vk::AccessFlagBits2::eShaderRead);

    // 1. Build BLASes for meshes added since last frame (one-time per mesh; LOD levels alias their
    // chain's RT-level BLAS and build nothing) plus any re-streamed meshes queued for a rebuild.
    if (m_rt.hasPendingBuilds(m_meshInfos.count()))
    {
        // Skinned output regions never build static BLASes, and a recycled slot below the watermark
        // had its build queued at addMeshInfos time; takeBuildList handles both and moves the watermark.
        const oc::vector<uint32> buildList = m_rt.takeBuildList(m_meshInfos.count());
        m_rt.accel().recordBuildBlas(frameIdx, vkPrepCommandBuffer, Globals::meshDataManager.getVertexBuffer(), Globals::meshDataManager.getIndexBuffer(),
            m_meshInfos.getBuffer().getBackingStoreAs<RendererVKLayout::MeshInfo>().data(), m_rt.getVertexCounts(), buildList,
            m_rtParams.blasCompaction);
        // No GI clear here: new meshes (terrain streaming!) leave the persistent probe volume intact.
        // Stale irradiance around new geometry self-corrects - embedded probes relocate out the next
        // frame (with a fast history re-sync), the rest re-converge at the temporal blend rate.
    }

    // 1a. Copy-compact BLASes whose size queries matured, and retire replaced originals.
    m_rt.accel().recordCompaction(frameIdx, vkPrepCommandBuffer);

    // Publish this frame slot's pending static BLAS-address changes (build/compaction/eviction) into its
    // own fenced address buffer. BEFORE the skinned rebuild, so a slot reused static->skinned keeps the
    // skinned per-slot address (written next), not a stale static value. Other slots pick up their copies
    // in their own frames -- static slots are never written here while they may be in flight.
    m_rt.accel().syncFrameAddresses(frameIdx);

    // 1b. Rebuild skinned meshes' BLASes every frame from this frame's deformed vertices, into this frame's
    // double-buffered slot (the other slot may still be referenced by the previous frame's in-flight TLAS).
    if (!m_skinned.getBlasBuilds().empty())
    {
        m_rt.accel().recordBuildSkinnedBlas(vkPrepCommandBuffer, frameIdx,
            Globals::meshDataManager.getVertexBuffer(), Globals::meshDataManager.getIndexBuffer(), m_skinned.getBlasBuilds());
        // Skinned BLAS builds -> TLAS build reads them (in the cached secondary executed next).
        fullBarrier(vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR, vk::AccessFlagBits2::eAccelerationStructureWriteKHR,
            vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR, vk::AccessFlagBits2::eAccelerationStructureReadKHR);
    }

    // 2. One-time clear of the persistent probe table/SH (it accumulates across frames thereafter).
    if (m_rtParams.giEnabled && m_giProbePipeline.needsClear())
    {
        m_giProbePipeline.recordClearPersistent(prepCommandBuffer);
        m_giProbePipeline.markCleared();
    }
    prepCommandBuffer.end();
}

// The CACHED half of GI (recorded only on invalidation frames, with the scene secondaries): the sky map
// bake, the TLAS-instance write, the TLAS build and the probe trace. Everything per-frame rides the UBO
// (u_giTlasNumInstances, u_giTrace0/1, u_frameIndex, u_sceneFocus); the TLAS handle and the instance
// buffers are stable per slot between invalidations (ensureTlasCapacity / the instance-capacity growth
// both invalidate), and the RT / GI toggles re-record through their tweak callbacks.
void Renderer::recordGlobalIllum(uint32 frameIdx)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    InstanceStream::FrameSlot& instances = m_instances.slot(frameIdx);
    FrameSubmission::FrameSlot& submission = m_submission.slot(frameIdx);
    CommandBuffer& globalIllumCommandBuffer = frameData.globalIllumCommandBuffer;
    vk::CommandBufferInheritanceInfo globalIllumInheritanceInfo;
    vk::CommandBuffer vkGlobalIllumCommandBuffer = globalIllumCommandBuffer.begin(false, &globalIllumInheritanceInfo);

    // The sky map (GI miss rays + the ocean / terrain-film mirror rays + the skyRadiance(up) ambient in the
    // forward pass) is baked on EVERY frame, ahead of the RT toggle: the forward shaders sample it whether
    // or not anything is ray traced. Its own barriers order last frame's reads before the write and the
    // write before this frame's compute + fragment reads.
    m_giProbePipeline.recordSkyMap(globalIllumCommandBuffer, frameIdx, frameData.ubo);

    // RT master toggle off, or no TLAS yet (no instances when this slot last invalidated): the sky map alone.
    const vk::AccelerationStructureKHR tlas = m_rt.accel().getTlas(frameIdx);
    if (!m_rtParams.enabled || !tlas)
    {
        globalIllumCommandBuffer.end();
        return;
    }

    auto fullBarrier = [&](vk::PipelineStageFlags2 srcStage, vk::AccessFlags2 srcAccess,
        vk::PipelineStageFlags2 dstStage, vk::AccessFlags2 dstAccess)
        {
            vk::MemoryBarrier2 bar{ .srcStageMask = srcStage, .srcAccessMask = srcAccess, .dstStageMask = dstStage, .dstAccessMask = dstAccess };
            vkGlobalIllumCommandBuffer.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &bar });
        };

    // Make prior writes visible to the GI compute passes:
    //  - the light grid (compute storage writes) that the trace reuses to shade hits, and
    //  - the sun cascade shadow map depth writes (late/early fragment tests) that the trace samples.
    // The shadow render pass only synchronizes its depth writes to the FRAGMENT stage (for the main
    // pass); without this the compute trace samples the depth image with no dependency, which faults
    // NVIDIA (depth-compression metadata read from the wrong stage).
    fullBarrier(vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests,
        vk::AccessFlagBits2::eShaderStorageWrite | vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
        vk::PipelineStageFlagBits2::eComputeShader,
        vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite | vk::AccessFlagBits2::eShaderSampledRead);

    // 3. Write the per-instance TLAS records on the GPU (over the whole instance capacity; the live count and
    // the range bound around the scene focus come from the UBO).
    GIProbePipeline::TlasInstanceParams tlasParams{
        .renderNodeTransforms = instances.transforms,
        .meshInstances = instances.meshInstances,
        .instanceOffsets = m_instanceOffsets.getBuffer(),
        .blasAddresses = m_rt.accel().getBlasAddressBuffer(frameIdx),
        .rtMeshAlias = m_rt.accel().getMeshAliasBuffer(),
        .materialInfos = m_materials.getBuffer(),
        .nodePassMasks = instances.passMasks,
        .ubo = frameData.ubo,
        .capacity = m_rt.getMaxTlasInstances(),
    };
    m_giProbePipeline.recordTlasInstances(globalIllumCommandBuffer, frameIdx, tlasParams);

    // instance write -> TLAS build read. The build reads the instance buffer as SHADER_READ (AS_READ
    // covers the source acceleration structures, not the instance data), so the dst access must include it.
    fullBarrier(vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageWrite,
        vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR,
        vk::AccessFlagBits2::eAccelerationStructureReadKHR | vk::AccessFlagBits2::eShaderRead);

    // 4. Rebuild this frame's TLAS (double-buffered) over the whole capacity; ensureTlasCapacity (in
    // recordCommandBuffers, before anything records) sized it and invalidated on a handle change.
    m_rt.accel().recordBuildTlas(vkGlobalIllumCommandBuffer, frameIdx, m_giProbePipeline.getTlasInstanceBuffer(frameIdx));

    // TLAS build -> ray-query read (GI/AO compute, and the forward fragment pass for RT light shadows)
    fullBarrier(vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR, vk::AccessFlagBits2::eAccelerationStructureWriteKHR,
        vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eFragmentShader,
        vk::AccessFlagBits2::eAccelerationStructureReadKHR | vk::AccessFlagBits2::eShaderStorageRead);

    // 5. Trace rays per clipmap probe and temporally blend irradiance into the SH. The probe set and
    // its toroidal window are derived from the SCENE FOCUS (this frame's u_sceneFocus in the UBO - the
    // player in game mode, else the camera); probes that scrolled in since last frame (relative to
    // u_giTrace1.xyz, last frame's focus, written by buildUbo) are full-replaced rather than blended.
    // Gated by the GI toggle - the TLAS built above still serves RTAO and RT shadows when GI is off.
    if (m_rtParams.giEnabled)
    {
    GIProbePipeline::TraceParams traceParams{
        .ubo = frameData.ubo,
        .lightInfos = submission.lightInfos,
        .lightGrid = submission.lightGrids,
        .lightTable = submission.lightTable,
        .vertexBuffer = Globals::meshDataManager.getVertexBuffer(),
        .indexBuffer = Globals::meshDataManager.getIndexBuffer(),
        .meshInfos = m_meshInfos.getBuffer(),
        .meshInstances = instances.meshInstances,
        .materialInfos = m_materials.getBuffer(),
        .tlas = tlas,
        .shadowMapView = frameData.shadowMap.getSampleView(),
        .shadowMapSampler = frameData.shadowMap.getSampler(),
    };
    m_giProbePipeline.recordTrace(globalIllumCommandBuffer, frameIdx, traceParams);

    // 6. Bake the traced probes into the irradiance volume the forward lit shaders sample (no-op while the
    // volume is off). Its own barriers: trace write -> bake read, bake write -> fragment read.
    m_giProbePipeline.recordVolumeBake(globalIllumCommandBuffer, frameIdx, frameData.ubo);

    // trace (SH write) -> fragment read in the main pass + vertex read (per-particle lighting)
    fullBarrier(vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageWrite,
        vk::PipelineStageFlagBits2::eFragmentShader | vk::PipelineStageFlagBits2::eVertexShader, vk::AccessFlagBits2::eShaderStorageRead);
    }

    globalIllumCommandBuffer.end();
}


namespace
{
    // Reversed-Z: the far plane / "no geometry" depth is 0.0 (shadow maps stay standard, cleared 1.0).
    constexpr oc::array<vk::ClearValue, 2> s_sceneClears{ vk::ClearColorValue{ std::array<float, 4>{ 0.0f, 0.0f, 0.0f, 1.0f } }, vk::ClearDepthStencilValue{ 0.0f, 0 } };

    // Between two scene render-pass instances: the previous instance's attachment writes -> this
    // instance's loadOp reads + writes. The stage passes cannot carry it - their dependency arrays
    // must stay identical for render-pass compatibility (see SceneColor).
    void sceneInstanceBarrier(vk::CommandBuffer cb)
    {
        const vk::PipelineStageFlags2 attStages = vk::PipelineStageFlagBits2::eColorAttachmentOutput
            | vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests;
        vk::MemoryBarrier2 barrier{
            .srcStageMask = attStages,
            .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite | vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
            .dstStageMask = attStages,
            .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite
                | vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
        };
        cb.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &barrier });
    }
}

void Renderer::executeScoped(vk::CommandBuffer primary, const char* scope, vk::CommandBuffer secondary)
{
    m_gpuProfiler.beginScope(primary, scope);
    primary.executeCommands(1, &secondary);
    m_gpuProfiler.endScope(primary);
}

// Every cached secondary, on invalidation frames only (setHaveToRecordCommandBuffers).
// THE scene stage table (see Renderer.ixx): name, gate, cached secondary and per-eye inline recorder.
// Table order is draw order - the opaque group first (it writes the depth), then the layered group.
// The GI probe impostors WRITE depth (they sort among themselves through gl_FragDepth), so they run
// with the opaque scene; the AO trace and the decals then see them as geometry (debug only).
oc::array<Renderer::SceneStage, 8> Renderer::buildSceneStages(uint32 frameIdx)
{
    PerFrameData& f = m_perFrameData[frameIdx];
    const bool force = m_force.isEnabled();
    return {
        SceneStage{ "Static meshes",     true,  true,                             false, &f.staticMeshCommandBuffer,   &Renderer::recordStaticMesh,   &Renderer::recordStaticMeshInto },
        SceneStage{ "GI probe debug",    true,  m_giProbePipeline.isDebugEnabled(), false, &f.giProbeDebugCommandBuffer, &Renderer::recordGiProbeDebug, nullptr },
        SceneStage{ "Decals",            false, m_decalPipeline.isEnabled(),      false, &f.decalCommandBuffer,        &Renderer::recordDecals,       &Renderer::recordDecalsInto },
        SceneStage{ "Debug lines",       false, m_debugLinePipeline.hasBuffers(), true,  &f.debugLineCommandBuffer,    &Renderer::recordDebugLines,   nullptr },
        SceneStage{ "Force shells",      false, force,                            false, &f.forceFieldCommandBuffer,   &Renderer::recordForceShells,  &Renderer::recordForceFieldBothInto },
        SceneStage{ "Force union blend", false, force,                            false, &f.forceUnionCommandBuffer,   &Renderer::recordForceUnion,   nullptr },
        SceneStage{ "Particles",         false, m_particles.isEnabled(),         false, &f.particleCommandBuffer,     &Renderer::recordParticles,    &Renderer::recordParticlesInto },
        SceneStage{ "Fog apply",         false, m_fogParams.enabled,              false, &f.fogApplyCommandBuffer,     &Renderer::recordFogApply,     &Renderer::recordFogApplyInto },
    };
}

void Renderer::recordSceneSecondaries(uint32 frameIdx)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    // Recorded even with no jobs: the dispatch is indirect (CPU-written dims per frame), so skinned
    // instances spawned later run without a re-record.
    recordSkinning(frameIdx);
    recordOceanSim(frameIdx); // executed only while an ocean is active (OceanParams::enabled)
    recordIndirectCull(frameIdx);
    recordLightGrid(frameIdx);
    recordForceCompute(frameIdx); // indirect dispatches: emitter/query changes never re-record
    recordRainOcclusionCull(frameIdx); // executed only while a weather volume requested the map
    recordRainOcclusionDraw(frameIdx);
    recordParticleSim(frameIdx); // indirect dispatches: emitter/spawn changes never re-record
    recordTerrainWetness(frameIdx); // executed only while enabled (TerrainWetTweaks::enabled)
    recordShadowCull(frameIdx);
    recordShadowDraw(frameIdx);
    recordVolumetricFog(frameIdx); // shared scatter/integrate (center view in VR)
    recordEyeAdaptation(frameIdx); // shared (samples the left eye's resolved colour in VR)
    // Composite secondary draws into the desktop swapchain (the left eye / TAA-resolved colour); in VR
    // it's the desktop-window mirror, so it's recorded in both modes.
    recordComposite(frameIdx);
    recordGlobalIllum(frameIdx); // the cached GI half (sky map, TLAS instances + build, trace)
    // Per-eye forward set: the PREVIOUS slot's AO view (the forward pass reads last frame's AO,
    // reprojected - see sampleAOBilateral) + this slot's TLAS (UPDATE_AFTER_BIND). Both are stable per slot
    // between re-records: every RTAO recreateImages site (and the blur-radius tweak, which switches
    // the returned view) forces a re-record, and a TLAS handle change (first build / capacity growth)
    // is caught by ensureTlasCapacity in recordCommandBuffers, which invalidates.
    for (uint32 eye = 0; eye < m_sceneViewCount; ++eye)
    {
        m_staticMeshGraphicsPipeline.updateAODescriptor(frameData.staticMeshPipelineDescriptorSet[eye].getDescriptorSet(),
            m_rtaoPipeline.getAOView((frameIdx + 1) % RendererVKLayout::NUM_FRAMES_IN_FLIGHT, eye), m_rtaoPipeline.getAOSampler());
        if (const vk::AccelerationStructureKHR tlas = m_rt.accel().getTlas(frameIdx))
            m_staticMeshGraphicsPipeline.updateTlasDescriptor(frameData.staticMeshPipelineDescriptorSet[eye].getDescriptorSet(), tlas);
    }
    // The remaining per-eye screen-space passes (scene stages, AO, fog apply, TAA) are recorded
    // inline in the primary in VR (recordPrimaryVR); on desktop they stay cached secondaries (one eye).
    if (m_sceneViewCount == 1)
    {
        for (const SceneStage& stage : buildSceneStages(frameIdx))
            if (!stage.gateRecording || stage.enabled)
                (this->*stage.recordCached)(frameIdx);
        recordForceMarch(frameIdx);
        recordAO(frameIdx);
        if (m_taaParams.taaEnabled) // bypassed entirely when off - nothing to record or execute
            recordTaa(frameIdx);
    }
}

// The primary's pre-scene stages, shared by both view modes: skinning, ocean sim, the culls, the light
// grid, force + particle compute, terrain wetness, the sun shadow cascades. Enable toggles are tested
// here (the primary is re-recorded every frame) so they take effect at once; the cached secondaries
// then just go unexecuted.
void Renderer::recordPrimaryPreScene(uint32 frameIdx, vk::CommandBuffer primary)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    // Skin first: deforms skinned meshes into their output vertex regions, which the cull / forward /
    // shadow passes then consume as ordinary static geometry.
    if (m_skinned.hasJobs())
        executeScoped(primary, "Skinning", frameData.skinningCommandBuffer.getCommandBuffer());
    // FFT ocean simulation (spectrum -> IFFT -> maps + mips); the forward ocean vertex shader and
    // the ocean fragment shader sample the maps. Skipped entirely while no ocean is active (the maps
    // rest in SHADER_READ_ONLY, so the samplers stay valid).
    if (m_oceanSimPipeline.isOceanEnabled())
        executeScoped(primary, "Ocean sim", frameData.oceanSimCommandBuffer.getCommandBuffer());
    executeScoped(primary, "Indirect cull", frameData.indirectCullCommandBuffer.getCommandBuffer());
    executeScoped(primary, "Light grid", frameData.lightGridCommandBuffer.getCommandBuffer());
    // Forcefield grid build + force/query compute (Force library readbacks land ~2 frames later).
    if (m_force.isEnabled())
        executeScoped(primary, "Force compute", frameData.forceComputeCommandBuffer.getCommandBuffer());
    // The weather volume's rain occlusion map (cull + top-down depth), gated on the UBO this frame was
    // built with (buildUboRainOcclusion) so the pass and the sim's shelter test always agree.
    if (m_particles.isEnabled() && m_ubo.rainOcclusionParams.x > 0.5f)
    {
        executeScoped(primary, "Rain occlusion cull", frameData.rainCullCommandBuffer.getCommandBuffer());
        m_gpuProfiler.beginScope(primary, "Rain occlusion draw");
        ShadowMap& map = frameData.rainOcclusionMap;
        vk::ClearValue clear;
        clear.depthStencil = vk::ClearDepthStencilValue{ .depth = 1.0f, .stencil = 0 };
        const vk::RenderPassBeginInfo rpBegin{
            .renderPass = map.getRenderPass(),
            .framebuffer = map.getFramebuffer(),
            .renderArea = vk::Rect2D{ .offset = vk::Offset2D{ 0, 0 }, .extent = vk::Extent2D{ map.getResolution(), map.getResolution() } },
            .clearValueCount = 1,
            .pClearValues = &clear,
        };
        vk::CommandBuffer vkRainDraw = frameData.rainDrawCommandBuffer.getCommandBuffer();
        primary.beginRenderPass(rpBegin, vk::SubpassContents::eSecondaryCommandBuffers);
        primary.executeCommands(1, &vkRainDraw);
        primary.endRenderPass();
        m_gpuProfiler.endScope(primary);
    }
    // Particle emit/simulate (outside any render pass; reads LAST frame's scene depth for collision and
    // THIS frame's rain occlusion map, writes the alive list + indirect draw args the in-pass billboard
    // draw consumes).
    if (m_particles.isEnabled())
        executeScoped(primary, "Particle sim", frameData.particleSimCommandBuffer.getCommandBuffer());
    // Terrain wetness clipmap: decay + re-wet under this frame's live ocean surface (after the ocean
    // sim, before the forward pass samples it). Runs on tick frames only (the wetness tick, decided in
    // the UBO build that this frame carries); skipped while disabled: the shader presence flag is 0.
    if (m_terrain.getWetTweaks().enabled && m_terrain.isWetnessTicking())
        executeScoped(primary, "Terrain wetness", frameData.terrainWetnessCommandBuffer.getCommandBuffer());
    // RT sun shadows replace the cascades entirely (forward pass traces, GI uses per-probe sun rays),
    // so skip the shadow cull + cascade render.
    if (!m_rtParams.effectiveSunShadow())
    {
        executeScoped(primary, "Shadow cull", frameData.shadowCullCommandBuffer.getCommandBuffer());
        m_gpuProfiler.beginScope(primary, "Shadow draw");
        ShadowMap& shadowMap = frameData.shadowMap;
        vk::ClearValue shadowClear;
        shadowClear.depthStencil = vk::ClearDepthStencilValue{ .depth = 1.0f, .stencil = 0 };
        const vk::RenderPassBeginInfo shadowRpBegin{
            .renderPass = shadowMap.getRenderPass(),
            .framebuffer = shadowMap.getFramebuffer(),
            .renderArea = vk::Rect2D{.offset = vk::Offset2D{ 0, 0 }, .extent = vk::Extent2D{ shadowMap.getResolution(), shadowMap.getResolution() } },
            .clearValueCount = 1,
            .pClearValues = &shadowClear,
        };
        vk::CommandBuffer vkShadowDrawCommandBuffer = frameData.shadowDrawCommandBuffer.getCommandBuffer();
        primary.beginRenderPass(shadowRpBegin, vk::SubpassContents::eSecondaryCommandBuffers);
        primary.executeCommands(1, &vkShadowDrawCommandBuffer);
        primary.endRenderPass();
        m_gpuProfiler.endScope(primary);
    }
}

// ---- VR: per-eye screen-space chain (opaque scene -> AO -> layered scene stages -> TAA), recorded inline ----
// GI (TLAS build + probe trace) and fog scatter/integrate are shared (built once for the centre view);
// each eye's scene/AO/TAA then runs against its own images, then eye adaptation and the per-eye LDR
// composites. Two scene render-pass instances per eye, the same split as the desktop path: the static
// meshes WRITE the depth, then it turns read-only + sampled for the AO trace and the layered stages.
void Renderer::recordPrimaryVR(uint32 frameIdx, CommandBuffer& commandBuffer)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    vk::CommandBuffer vkCommandBuffer = commandBuffer.getCommandBuffer();
    SceneColor& sceneColor = frameData.sceneColor;
    const vk::Rect2D sceneArea{ .offset = vk::Offset2D{ m_viewportRect.min.x, m_viewportRect.min.y }, .extent = vk::Extent2D{ sceneColor.getWidth() - m_viewportRect.min.x, sceneColor.getHeight() - m_viewportRect.min.y } };

    m_gpuProfiler.beginScope(vkCommandBuffer, "GI");
    vk::CommandBuffer vkGiPrepCommandBuffer = frameData.giPrepCommandBuffer.getCommandBuffer();
    vk::CommandBuffer vkGlobalIllumCommandBuffer = frameData.globalIllumCommandBuffer.getCommandBuffer();
    vkCommandBuffer.executeCommands(1, &vkGiPrepCommandBuffer); // per-frame BLAS work, then the cached rest
    vkCommandBuffer.executeCommands(1, &vkGlobalIllumCommandBuffer);
    m_gpuProfiler.endScope(vkCommandBuffer);
    if (m_fogParams.enabled)
        executeScoped(vkCommandBuffer, "Volumetric fog", frameData.volumetricFogCommandBuffer.getCommandBuffer());

    // The same stage table the desktop path executes; VR records the stages inline instead, skipping
    // the ones with no inline recorder (the debug overlays).
    const oc::array<SceneStage, 8> stages = buildSceneStages(frameIdx);
    bool layered = false;
    for (const SceneStage& stage : stages)
        layered = layered || (!stage.opaque && stage.enabled && stage.recordInline);

    for (uint32 eye = 0; eye < m_sceneViewCount; ++eye)
    {
        m_gpuProfiler.beginScope(vkCommandBuffer, eye == 0 ? "Eye L" : "Eye R");
        // This eye's forward set (last frame's AO view + TLAS) is written at scene-record time (recordSceneSecondaries).
        vk::RenderPassBeginInfo eyeRpBegin{
            .renderPass = sceneColor.getStageRenderPass(true, !layered, false),
            .framebuffer = sceneColor.getFramebuffer(eye),
            .renderArea = sceneArea,
            .clearValueCount = (uint32)s_sceneClears.size(),
            .pClearValues = s_sceneClears.data(),
        };
        vkCommandBuffer.beginRenderPass(eyeRpBegin, vk::SubpassContents::eInline); // opaque: writes this eye's depth
        for (const SceneStage& stage : stages)
            if (stage.opaque && stage.enabled && stage.recordInline)
                (this->*stage.recordInline)(commandBuffer, frameIdx, eye);
        vkCommandBuffer.endRenderPass();
        recordSceneDepthToSampled(vkCommandBuffer, sceneColor.getDepthImage(), eye);

        if (m_rtaoParams.enabled)
            recordAOInto(commandBuffer, frameIdx, eye); // compute AO for this eye (NEXT frame's forward pass reads it)

        if (layered)
        { // the stages layered over the opaque scene: read-only depth, which they also sample
            sceneInstanceBarrier(vkCommandBuffer);
            eyeRpBegin.renderPass = sceneColor.getStageRenderPass(false, true, true);
            vkCommandBuffer.beginRenderPass(eyeRpBegin, vk::SubpassContents::eInline);
            for (const SceneStage& stage : stages)
                if (!stage.opaque && stage.enabled && stage.recordInline)
                    (this->*stage.recordInline)(commandBuffer, frameIdx, eye);
            vkCommandBuffer.endRenderPass();
        }

        // Scene colour -> TAA compute sampled read. Explicit image barrier (not a global memory barrier -
        // see the desktop path) naming this eye's colour layer so the finalLayout transition at
        // endRenderPass is actually resolved for the compute read.
        vk::ImageMemoryBarrier2 colorToTaa{
            .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
            .oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .image = sceneColor.getColorImage(),
            .subresourceRange = { vk::ImageAspectFlagBits::eColor, 0, 1, eye, 1 },
        };
        vkCommandBuffer.pipelineBarrier2(vk::DependencyInfo{ .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &colorToTaa });
        recordTaaInto(commandBuffer, frameIdx, eye); // resolve into this eye's history
        m_gpuProfiler.endScope(vkCommandBuffer);
    }

    // Eye adaptation samples the left eye's resolved colour (shared exposure, no per-eye flicker).
    executeScoped(vkCommandBuffer, "Eye adaptation", frameData.eyeAdaptCommandBuffer.getCommandBuffer());

    // Tonemap each eye's TAA-resolved colour into its LDR composite target (copied into the OpenXR
    // eye swapchains in present()). TAA left the resolved images in GENERAL with a write->read barrier.
    m_gpuProfiler.beginScope(vkCommandBuffer, "VR composite");
    const vk::Extent2D ext = m_swapChain.getLayout().extent;
    for (uint32 eye = 0; eye < 2; ++eye)
    {
        const vk::RenderPassBeginInfo eyeCompositeBegin{
            .renderPass = m_renderPass.getRenderPass(),
            .framebuffer = m_vrEyes.getFramebuffer(eye),
            .renderArea = vk::Rect2D{ .offset = vk::Offset2D{ 0, 0 }, .extent = ext },
            .clearValueCount = (uint32)s_sceneClears.size(),
            .pClearValues = s_sceneClears.data(),
        };
        vkCommandBuffer.beginRenderPass(eyeCompositeBegin, vk::SubpassContents::eInline);
        vkCommandBuffer.setViewport(0, vk::Viewport{ .x = 0.0f, .y = 0.0f, .width = (float)ext.width, .height = (float)ext.height, .minDepth = 0.0f, .maxDepth = 1.0f });
        vkCommandBuffer.setScissor(0, vk::Rect2D{ .offset = vk::Offset2D{ 0, 0 }, .extent = ext });
        CompositePipeline::RecordParams eyeComposite{
            .descriptorSet = m_vrEyes.getCompositeSet(eye),
            .resolvedView = m_taaParams.taaEnabled ? m_taaPipeline.getResolvedView(frameIdx, eye)
                : frameData.sceneColor.getColorLayerView(eye),
            .resolvedLayout = m_taaParams.taaEnabled ? vk::ImageLayout::eGeneral : vk::ImageLayout::eShaderReadOnlyOptimal,
            .sampler = m_taaParams.taaEnabled ? m_taaPipeline.getSampler() : frameData.sceneColor.getSampler(),
            .exposureBuffer = m_eyeAdaptationPipeline.getExposureBuffer().getBuffer(),
            .exposureEV = m_postParams.exposureEV,
            .tonemapper = m_postParams.tonemapper,
            .autoExposure = m_postParams.autoExposure ? 1 : 0,
        };
        m_compositePipeline.record(commandBuffer, eyeComposite);
        vkCommandBuffer.endRenderPass();
    }
    m_gpuProfiler.endScope(vkCommandBuffer); // VR composite
}

// ---- Desktop: GI -> fog -> scene opaque (writes the depth) -> RTAO -> force passes -> scene forward
// (the layered stages, depth read-only + sampled) -> TAA -> eye adaptation ----
// THERE IS NO DEPTH PREPASS. The opaque scene stages write THE scene depth; one barrier then parks it
// read-only, and everything that reads this frame's depth runs after that point: the AO trace (whose
// result the NEXT frame's forward pass reads), the force march, the layered scene stages and TAA.
void Renderer::recordPrimaryDesktop(uint32 frameIdx, vk::CommandBuffer vkCommandBuffer)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    SceneColor& sceneColor = frameData.sceneColor;
    const vk::Rect2D sceneArea{ .offset = vk::Offset2D{ m_viewportRect.min.x, m_viewportRect.min.y }, .extent = vk::Extent2D{ sceneColor.getWidth() - m_viewportRect.min.x, sceneColor.getHeight() - m_viewportRect.min.y } };

    m_gpuProfiler.beginScope(vkCommandBuffer, "GI");
    vk::CommandBuffer vkGiPrepCommandBuffer = frameData.giPrepCommandBuffer.getCommandBuffer();
    vk::CommandBuffer vkGlobalIllumCommandBuffer = frameData.globalIllumCommandBuffer.getCommandBuffer();
    vkCommandBuffer.executeCommands(1, &vkGiPrepCommandBuffer); // per-frame BLAS work, then the cached rest
    vkCommandBuffer.executeCommands(1, &vkGlobalIllumCommandBuffer);
    m_gpuProfiler.endScope(vkCommandBuffer);
    // Fog scatter/integrate compute (the integrated grid was cleared to "no fog" at init when disabled).
    if (m_fogParams.enabled)
        executeScoped(vkCommandBuffer, "Volumetric fog", frameData.volumetricFogCommandBuffer.getCommandBuffer());
    // The forward set's AO view (LAST frame's) + TLAS are written at scene-record time (recordSceneSecondaries).

    // The scene renders as ONE render-pass instance per stage: timestamps are illegal inside a
    // SECONDARY_COMMAND_BUFFERS subpass (so the GPU profiler needs the split), and the depth switches
    // from written to read-only between the two groups. SceneColor's stage variants - the first clears,
    // the last hands colour to TAA - are compatible with the pass the secondaries/pipelines were built
    // against, since only load/store ops and layouts differ. The deps must stay identical for that
    // compatibility, so the inter-instance attachment hazards get an explicit barrier.
    const oc::array<SceneStage, 8> stages = buildSceneStages(frameIdx);
    const SceneStage* lastStage = &stages[0]; // static meshes are always on
    for (const SceneStage& stage : stages)
        if (stage.enabled)
            lastStage = &stage;
    bool firstInstance = true;
    const auto runStage = [&](const SceneStage& stage)
    {
        if (!stage.enabled)
            return;
        if (!firstInstance)
            sceneInstanceBarrier(vkCommandBuffer);
        const vk::RenderPassBeginInfo sceneRpBegin{
            .renderPass = sceneColor.getStageRenderPass(firstInstance, &stage == lastStage, !stage.opaque),
            .framebuffer = sceneColor.getFramebuffer(),
            .renderArea = sceneArea,
            .clearValueCount = (uint32)s_sceneClears.size(), // ignored by the loadOp LOAD variants
            .pClearValues = s_sceneClears.data(),
        };
        const vk::CommandBuffer stageCb = stage.cb->getCommandBuffer();
        m_gpuProfiler.beginScope(vkCommandBuffer, stage.name);
        vkCommandBuffer.beginRenderPass(sceneRpBegin, vk::SubpassContents::eSecondaryCommandBuffers);
        vkCommandBuffer.executeCommands(1, &stageCb);
        vkCommandBuffer.endRenderPass();
        m_gpuProfiler.endScope(vkCommandBuffer);
        firstInstance = false;
    };

    m_gpuProfiler.beginScope(vkCommandBuffer, "Scene opaque");
    for (const SceneStage& stage : stages)
        if (stage.opaque)
            runStage(stage);
    recordSceneDepthToSampled(vkCommandBuffer, sceneColor.getDepthImage(), 0);
    m_gpuProfiler.endScope(vkCommandBuffer); // Scene opaque

    if (m_rtaoParams.enabled)
        executeScoped(vkCommandBuffer, "RTAO", frameData.aoCommandBuffer.getCommandBuffer());

    // The union march's interval pass + the half-res march: their render passes begin/end HERE (a
    // secondary cannot begin one), the draws are cached secondaries (recordForceMarch). Gated like the
    // force compute and the two force scene stages: with the field off the passes would only clear their
    // targets for a 0-vertex draw.
    if (m_force.isEnabled())
    {
        vk::CommandBuffer vkForceIntervalCommandBuffer = frameData.forceIntervalCommandBuffer.getCommandBuffer();
        m_gpuProfiler.beginScope(vkCommandBuffer, "Force intervals");
        m_forceFieldPipeline.beginIntervalPass(vkCommandBuffer);
        vkCommandBuffer.executeCommands(1, &vkForceIntervalCommandBuffer);
        vkCommandBuffer.endRenderPass();
        m_gpuProfiler.endScope(vkCommandBuffer);
        if (m_forceFieldPipeline.getUnionHalfRes())
        {
            vk::CommandBuffer vkForceMarchCommandBuffer = frameData.forceMarchCommandBuffer.getCommandBuffer();
            m_gpuProfiler.beginScope(vkCommandBuffer, "Force union march");
            m_forceFieldPipeline.beginUnionMarchPass(vkCommandBuffer);
            vkCommandBuffer.executeCommands(1, &vkForceMarchCommandBuffer);
            vkCommandBuffer.endRenderPass();
            m_gpuProfiler.endScope(vkCommandBuffer);
        }
    }

    // The stages layered over the opaque scene: the depth is their READ-ONLY attachment, which they
    // also sample (decals, soft particles, force shells, fog apply). None of their pipelines writes depth.
    m_gpuProfiler.beginScope(vkCommandBuffer, "Scene forward");
    for (const SceneStage& stage : stages)
        if (!stage.opaque)
            runStage(stage);
    m_gpuProfiler.endScope(vkCommandBuffer); // Scene forward

    // SceneColor's render pass has no 0->EXTERNAL dependency of its own (must stay dependency-identical
    // to the swapchain pass, see SceneColor.cpp), so its finalLayout->SHADER_READ_ONLY transition at
    // endRenderPass is only ordered by the implicit (no-access) end dependency. An explicit image
    // barrier naming the colour image is what actually resolves that transition for TAA's compute read
    // (a global vk::MemoryBarrier2 was insufficient - validation still saw it as an unsynchronized
    // layout-transition read). Same-layout SHADER_READ_ONLY->SHADER_READ_ONLY, sync-only.
    vk::ImageMemoryBarrier2 colorToTaaImg{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
        .oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        .image = sceneColor.getColorImage(),
        .subresourceRange = { vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 },
    };
    // TAA OFF: eye adaptation (compute) and the composite (fragment) sample this image
    // instead of TAA's resolved one, so the read must be visible to both stages.
    if (!m_taaParams.taaEnabled)
        colorToTaaImg.dstStageMask |= vk::PipelineStageFlagBits2::eFragmentShader;
    vkCommandBuffer.pipelineBarrier2(vk::DependencyInfo{ .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &colorToTaaImg });
    // Disabled TAA is skipped outright (it used to run a full-screen copy with feedback 0).
    if (m_taaParams.taaEnabled)
        executeScoped(vkCommandBuffer, "TAA", frameData.taaCommandBuffer.getCommandBuffer());
    // Eye adaptation: reads the resolved colour (TAA barrier above), writes the exposure the composite reads.
    executeScoped(vkCommandBuffer, "Eye adaptation", frameData.eyeAdaptCommandBuffer.getCommandBuffer());
}

void Renderer::recordCommandBuffers()
{
    const uint32 frameIdx = m_swapChain.getCurrentFrameIndex();
    PerFrameData& frameData = m_perFrameData[frameIdx];

    // Streamed textures: refresh swapped bindless slots in this frame slot's sets before anything
    // records against them (safe here: acquireNextImage waited this slot's fence, and the texture
    // arrays are UPDATE_AFTER_BIND for the cached CBs).
    ProfileScope descriptorScope("Bindless descriptor writes", EProfileCategory::Renderer);
    m_textures.applyPendingWrites(frameIdx);

    // This slot's TLAS, sized to the instance capacity, BEFORE anything records: every secondary that bakes
    // the handle (GI, RTAO, fog) and the forward set's descriptor are written from it below, and a handle
    // change (first build / capacity growth) invalidates them all. Nothing to trace without RT or instances.
    if (m_rtParams.enabled && m_instances.getInstanceCount() > 0 && m_rt.accel().ensureTlasCapacity(frameIdx, m_rt.getMaxTlasInstances()))
        setHaveToRecordCommandBuffers();

    const bool recordScene = !frameData.updated && m_instances.getInstanceCount() > 0;

    // Baked terrain-data cascades: point this slot's sets at the active ping-pong image
    // (UPDATE_AFTER_BIND, like the AO/TLAS bindings - a re-bake swaps images without re-recording
    // anything). The ocean passes read them for water depth/level (shoaling, surf, swash, land cull).
    // Rewritten only when this slot's sets re-record (recreated sets always come with a
    // setHaveToRecordCommandBuffers) or the ping-pong flipped since this slot last wrote (generation).
    if (recordScene || m_terrain.getDescGen(frameIdx) != m_terrain.getHeightMap().getGeneration())
    {
        m_terrain.setDescGen(frameIdx, m_terrain.getHeightMap().getGeneration());
        for (uint32 eye = 0; eye < m_sceneViewCount; ++eye)
            m_staticMeshGraphicsPipeline.updateTerrainHeightDescriptor(frameData.staticMeshPipelineDescriptorSet[eye].getDescriptorSet(),
                m_terrain.getHeightMap().getView(), m_terrain.getHeightMap().getSampler());
        m_volumetricFogPipeline.updateTerrainDescriptor(frameIdx, m_terrain.getHeightMap().getView(), m_terrain.getHeightMap().getSampler());
        m_terrainWetnessPipeline.updateTerrainDescriptor(frameIdx, m_terrain.getHeightMap().getView(), m_terrain.getHeightMap().getSampler());
        m_oceanSimPipeline.updateTerrainDescriptor(frameIdx, m_terrain.getHeightMap().getView(), m_terrain.getHeightMap().getSampler());
        m_particlePipeline.updateTerrainDescriptor(frameIdx, m_terrain.getHeightMap().getView(), m_terrain.getHeightMap().getSampler());
        // The wetness clipmap and the GI sky map never change handle; rewritten alongside so a recreated
        // set gets them.
        for (uint32 eye = 0; eye < m_sceneViewCount; ++eye)
        {
            m_staticMeshGraphicsPipeline.updateTerrainWetnessDescriptor(frameData.staticMeshPipelineDescriptorSet[eye].getDescriptorSet(),
                m_terrainWetnessPipeline.getView(), m_terrainWetnessPipeline.getSampler());
            m_staticMeshGraphicsPipeline.updateSkyMapDescriptor(frameData.staticMeshPipelineDescriptorSet[eye].getDescriptorSet(),
                m_giProbePipeline.getSkyMapView(), m_giProbePipeline.getSkyMapSampler());
        }
    }

    descriptorScope.stop();

    if (recordScene)
    {
        // Only on invalidation frames (setHaveToRecordCommandBuffers) - the secondaries are cached.
        ProfileScope sceneScope("Record scene secondaries", EProfileCategory::Renderer);
        recordSceneSecondaries(frameIdx);
        frameData.updated = true;
    }

    {
        // The per-frame GI half: one-shot BLAS builds / compaction / the skinned rebuild (empty most frames).
        ProfileScope giScope("Record GI", EProfileCategory::Renderer);
        if (m_instances.getInstanceCount() > 0)
            recordGlobalIllumPrep(frameIdx);
    }

    // Live tunables + delta time into the mapped params buffer (no command-buffer re-record needed).
    if (m_instances.getInstanceCount() > 0)
    {
        const Clock::time_point now = Clock::now();
        const float deltaSeconds = m_haveEyeAdaptTime ? oc::min(std::chrono::duration<float>(now - m_eyeAdaptLastTime).count(), 0.25f) : 0.0f;
        m_eyeAdaptLastTime = now;
        m_haveEyeAdaptTime = true;
        m_eyeAdaptationPipeline.updateParams(frameIdx, m_postParams, deltaSeconds);
    }

    vk::CommandBufferInheritanceInfo inheritance{ .renderPass = m_renderPass.getRenderPass() };
    ProfileScope imguiScope("Record ImGui", EProfileCategory::Renderer); // RenderDrawData copies every UI vertex
    vk::CommandBuffer vkImguiCommandBuffer = frameData.imguiCommandBuffer.begin(true, &inheritance);
    // From the UI's deep-copied snapshot, never ImGui::GetDrawData(): the next widget pass (a job)
    // calls ImGui::NewFrame mid-frame, which invalidates the live draw lists. Null until the first
    // UI::render (frame 0) - the empty secondary CB still executes fine.
    if (m_imguiDrawData)
        ImGui_ImplVulkan_RenderDrawData(static_cast<ImDrawData*>(const_cast<void*>(m_imguiDrawData)), vkImguiCommandBuffer, nullptr);
    frameData.imguiCommandBuffer.end();

    imguiScope.stop();

    // The primary is re-recorded EVERY frame (timestamps, per-frame barriers, executeCommands of the
    // cached secondaries) - the steady-state cost of this function lives here.
    ProfileScope primaryScope("Record primary", EProfileCategory::Renderer);
    CommandBuffer& commandBuffer = frameData.primaryCommandBuffer;
    vk::CommandBuffer vkCommandBuffer = commandBuffer.begin(true);
    // GPU pass timings: timestamps live in the primary (re-recorded every frame) and OUTSIDE render
    // passes only; results are collected by a job kicked in beginFrame when this slot's fence is next
    // waited - join it before beginRecord resets the slot's scope list + query pool it reads.
    Globals::jobSystem.wait(m_gpuCollectCounter);
    m_gpuProfiler.beginRecord(vkCommandBuffer, frameIdx);
    m_gpuProfiler.beginScope(vkCommandBuffer, "GPU Frame");
    // Pending baked-map uploads (fog terrain cascades): copied here in the primary (re-recorded
    // every frame) because the destination ping-pong images were sampled by older submissions - the
    // transitions need an execution dependency on those reads, which the StagingManager's fresh-image
    // upload path doesn't emit.
    m_terrain.getHeightMap().recordUpload(commandBuffer);
    { // Sync for ubo copy
        vk::MemoryBarrier2 memoryBarrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eVertexShader | vk::PipelineStageFlagBits2::eFragmentShader,
            .dstAccessMask = vk::AccessFlagBits2::eUniformRead,
        };
        vkCommandBuffer.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &memoryBarrier });
    }

    if (m_instances.getInstanceCount() > 0)
    {
        recordPrimaryPreScene(frameIdx, vkCommandBuffer);
        if (m_sceneViewCount > 1)
            recordPrimaryVR(frameIdx, commandBuffer);
        else
            recordPrimaryDesktop(frameIdx, vkCommandBuffer);
    }

    // Swapchain render pass: composite the resolved scene into the swapchain, then ImGui on top.
    constexpr oc::array<vk::ClearValue, 2> clearValues{ vk::ClearColorValue{ std::array<float, 4>{ 0.0f, 0.0f, 0.0f, 0.0f } }, vk::ClearDepthStencilValue{ 0.0f, 0 } };
    const vk::RenderPassBeginInfo renderPassBeginInfo{
        .renderPass = m_renderPass.getRenderPass(),
        .framebuffer = m_framebuffers.getFramebuffer(m_swapChain.getCurrentImageIdx()),
        .renderArea = vk::Rect2D { .offset = vk::Offset2D { 0, 0 }, .extent = m_swapChain.getLayout().extent },
        .clearValueCount = (uint32)clearValues.size(),
        .pClearValues = clearValues.data(),
    };
    m_gpuProfiler.beginScope(vkCommandBuffer, "Composite + UI");
    vkCommandBuffer.beginRenderPass(renderPassBeginInfo, vk::SubpassContents::eSecondaryCommandBuffers);
    if (m_instances.getInstanceCount() > 0)
    {
        vk::CommandBuffer vkCompositeCommandBuffer = frameData.compositeCommandBuffer.getCommandBuffer();
        vkCommandBuffer.executeCommands(1, &vkCompositeCommandBuffer);
    }
    vkCommandBuffer.executeCommands(1, &vkImguiCommandBuffer);
    vkCommandBuffer.endRenderPass();
    m_gpuProfiler.endScope(vkCommandBuffer);
    m_gpuProfiler.endScope(vkCommandBuffer); // GPU Frame
    commandBuffer.end();
}

void Renderer::setHaveToRecordCommandBuffers()
{
    for (PerFrameData& perFrame : m_perFrameData)
        perFrame.updated = false;
}
