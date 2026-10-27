module RendererVK;

import Core;
import Core.glm;
import Settings;
import :ForceFieldState;

ForceFieldState::ForceFieldState()
    : m_params(oc::make_unique<ForceFieldParams>())
{
}

ForceFieldState::~ForceFieldState() = default;

void ForceFieldState::setParams(const ForceFieldParams& params) { *m_params = params; }
bool ForceFieldState::isEnabled() const { return m_params->enabled; }

void ForceFieldState::buildShellCull(bool enabled, const Frustum& centerFrustum, const glm::vec3& cameraPos, float pixelScale)
{
    const ForceFieldParams& params = *m_params;
    m_shellCull = ForceFieldPipeline::ShellCull{};
    m_shellCull.sampledRadius = glm::max(params.sampledShellRadius, 1.0f); // the tweak min
    if (enabled)
    {
        m_shellCull.enabled = true;
        m_shellCull.frustum = centerFrustum;
        m_shellCull.cameraPos = cameraPos;
        m_shellCull.pixelScale = pixelScale;
        m_shellCull.minPixels = params.minShellPixels;
        // One analytic march per pixel; the density DEBUG view stays per-proxy (the union FS does not
        // implement it), so it forces the old path while on.
        m_shellCull.unionPass = params.unionMarch && !params.densityView;
    }
    m_shellCull.bakeVolume = m_shellBakeActive; // set by buildUboForce (this frame's fit)
    m_shellCull.logTierDebug = params.logTierDebug;
}

void ForceFieldState::buildGrid(ForceFieldPipeline& pipeline, uint32 frameIdx)
{
    // Compacts the ACTIVE emitter slots + uploads query positions (this slot's fence was waited).
    pipeline.upload(frameIdx, m_emitters.slots(), m_queries.slots(), m_bakeChunks, m_bakeSampleY, m_shellCull);
    m_gridNeedsGrow = false;
    if (!m_params->enabled || !pipeline.getUseGrid())
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
