module RendererVK;

import Core;
import Core.glm;
import :ForceFieldState;

void ForceFieldState::buildShellCull(bool enabled, const Frustum& centerFrustum, const glm::vec3& cameraPos, float pixelScale)
{
    m_shellCull = ForceFieldPipeline::ShellCull{};
    m_shellCull.sampledRadius = m_params.sampledShellRadius > 0.0f ? m_params.sampledShellRadius : FLT_MAX;
    if (enabled)
    {
        m_shellCull.enabled = true;
        m_shellCull.frustum = centerFrustum;
        m_shellCull.cameraPos = cameraPos;
        m_shellCull.pixelScale = pixelScale;
        m_shellCull.minPixels = m_params.minShellPixels;
        // One analytic march per pixel; the density DEBUG view stays per-proxy (the union FS does not
        // implement it), so it forces the old path while on.
        m_shellCull.unionPass = m_params.unionMarch && !m_params.densityView;
    }
    m_shellCull.bakeVolume = m_shellBakeActive; // set by buildUboForce (this frame's fit)
    m_shellCull.logTierDebug = m_params.logTierDebug;
}

void ForceFieldState::buildGrid(ForceFieldPipeline& pipeline, uint32 frameIdx)
{
    // Compacts the ACTIVE emitter slots + uploads query positions (this slot's fence was waited).
    pipeline.upload(frameIdx, m_emitters.slots(), m_queries.slots(), m_bakeChunks, m_bakeSampleY, m_shellCull);
    m_gridNeedsGrow = false;
    if (!m_params.enabled || !pipeline.getUseGrid())
        return;
    m_gridDemand = pipeline.buildGrid();
    m_gridNeedsGrow = !pipeline.gridFits(m_gridDemand);
    if (!m_gridNeedsGrow)
        pipeline.uploadGrid(frameIdx);
}

void ForceFieldState::applyGridGrowth(ForceFieldPipeline& pipeline, uint32 frameIdx)
{
    if (!m_gridNeedsGrow)
        return;
    m_gridNeedsGrow = false;
    m_onGpuIdle();
    pipeline.growGridBuffers(m_gridDemand);
    m_onInvalidate();
    pipeline.uploadGrid(frameIdx);
}
