export module RendererVK:BindlessTextures;

import Core;
import :VK;
import :Layout;
import :Texture;
import :TextureManager;
import :TextureStreamer;

// The Renderer's side of the BINDLESS texture arrays. Two counts matter and they are not the same:
//  * the LAYOUT CAP (`getLayoutCap`) is the device limit baked into every pipeline layout at creation
//    and passed to every reloadShaders - it never changes;
//  * the LIVE DESCRIPTOR COUNT (`getDescriptorCount`) is what the variable-count sets are actually
//    allocated with, and it follows TextureManager's generation.
//
// Everything that consumes the arrays - six pipelines and the per-frame descriptor sets - is reached
// through two callbacks, because which sets exist is the Renderer's business, not this class's:
//  * onCapacityChanged(count): the GPU is already idle; re-allocate every consuming set for `count`.
//  * onSlotWrite(frameIdx, texIdx, view): point one slot of every consuming set at one view.
//
// It also holds the DEFERRED FREE queue: a destroyed container's images may still be sampled by an
// in-flight frame. present() frees the queued slots (they point at the fallback from that frame's record
// on) and keeps their images here until NUM_FRAMES_IN_FLIGHT frames later, when no frame can sample them -
// no GPU drain.
export class BindlessTextures final
{
public:
    void initialize(oc::function<void(uint32)> onCapacityChanged,
        oc::function<void(uint32 frameIdx, uint16 texIdx, vk::ImageView view)> onSlotWrite)
    {
        m_onCapacityChanged = oc::move(onCapacityChanged);
        m_onSlotWrite = oc::move(onSlotWrite);
        m_layoutCap = Globals::textureManager.getDescriptorCap(); // fixed layout cap; the live count grows separately
    }

    uint32 getLayoutCap() const { return m_layoutCap; }
    uint32 getDescriptorCount() const { return m_descriptorCount; }

    // Re-allocates the variable-count sets when the live texture count outgrew them. Runs at
    // beginFrame AND again in present() before recording, because containers loaded after beginFrame
    // (terrain streaming, mid-frame spawns) upload textures that this frame's record would otherwise
    // write past the descriptor capacity. `onGpuIdle` runs before the sets are re-created.
    // Returns true when it grew (the caller re-records).
    bool syncCapacity(const oc::function<void()>& onGpuIdle);

    // Drains the streamer's queued slot writes into this frame slot's sets. Call before anything
    // records against them.
    void applyPendingWrites(uint32 frameIdx);

    // ---- Deferred frees (container teardown, splat set replacement) ----
    void queueFree(oc::span<const uint16> texIndices) { m_pendingFrees.insert(m_pendingFrees.end(), texIndices.begin(), texIndices.end()); }
    // Once per present(), before the record: frees the queued slots (each slot's bindless entries rewrite to the
    // fallback when its frame slot next records) and destroys the retired images whose frames are done. `gpuIdle`
    // (a capacity growth's drain) destroys every retired image at once.
    void processPendingFrees(uint32 frameCounter, bool gpuIdle = false);

private:
    struct RetiredTexture
    {
        Texture texture;
        uint32 destroyFrame; // the renderer frame from which no frame in flight can sample it
    };

    oc::function<void(uint32)> m_onCapacityChanged;
    oc::function<void(uint32, uint16, vk::ImageView)> m_onSlotWrite;

    uint32 m_layoutCap = RendererVKLayout::INITIAL_TEXTURES;
    uint32 m_descriptorCount = RendererVKLayout::INITIAL_TEXTURES;
    uint32 m_generation = 0; // last seen TextureManager::getGeneration()
    oc::vector<uint16> m_pendingFrees;
    oc::vector<RetiredTexture> m_retired; // destroyed by ~RendererState's members after its device drain at the latest
};
