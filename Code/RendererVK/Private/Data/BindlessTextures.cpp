module RendererVK;

import Core;
import :BindlessTextures;

bool BindlessTextures::syncCapacity(const oc::function<void()>& onGpuIdle)
{
    if (Globals::textureManager.getGeneration() == m_generation)
        return false;
    m_generation = Globals::textureManager.getGeneration();
    m_descriptorCount = Globals::textureManager.getMaxTextures();
    onGpuIdle();
    m_onCapacityChanged(m_descriptorCount);
    return true;
}

void BindlessTextures::applyPendingWrites(uint32 frameIdx)
{
    if (Globals::textureStreamer.debugRewriteAllSlots())
        for (uint16 i = 0; i < (uint16)Globals::textureManager.getNumTextures(); ++i)
            Globals::textureStreamer.queueDescriptorWrite(i);

    const oc::span<const uint16> writes = Globals::textureStreamer.getPendingDescriptorWrites(frameIdx);
    if (writes.empty())
        return;
    for (const uint16 texIdx : writes)
    {
        // Slots beyond the live descriptor count appear when a texture upload outgrew the arrays this
        // frame; the pending capacity growth re-allocates + fully refills every set, so drop them.
        if (texIdx >= m_descriptorCount || texIdx >= Globals::textureManager.getNumTextures())
            continue;
        // Freed slots (destroyed container, not yet recycled) resolve to the fallback view.
        m_onSlotWrite(frameIdx, texIdx, Globals::textureManager.getViewForDescriptor(texIdx));
    }
    Globals::textureStreamer.clearPendingDescriptorWrites(frameIdx);
}

void BindlessTextures::processPendingFrees(uint32 frameCounter, bool gpuIdle)
{
    // A slot freed in frame N is rewritten to the fallback in frame N's own record; the other frame slot's sets keep the
    // view until that slot records in frame N + 1, after the fence of frame N - 1. Destroying at N + NUM_FRAMES_IN_FLIGHT
    // keeps the TextureStreamer's margin.
    if (gpuIdle)
        m_retired.clear();
    else
        oc::erase_if(m_retired, [frameCounter](const RetiredTexture& retired) { return int32(frameCounter - retired.destroyFrame) >= 0; });

    for (const uint16 texIdx : m_pendingFrees)
    {
        Texture texture = Globals::textureManager.free(texIdx);
        if (texture.getImageView() && !gpuIdle)
            m_retired.push_back({ oc::move(texture), frameCounter + RendererVKLayout::NUM_FRAMES_IN_FLIGHT });
        // else: already free, or the GPU is idle - the texture dies here
    }
    m_pendingFrees.clear();
}
