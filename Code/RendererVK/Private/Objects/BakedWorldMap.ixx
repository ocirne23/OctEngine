export module RendererVK:BakedWorldMap;

import Core;
import :VK;
import :Allocator;
import :Buffer;
import :CommandBuffer;
import :Sampler;
import :Layout;

// A CPU-baked, camera-centered world-region snapshot on the GPU: an R32F or RG32F image (2D, or a 2D
// array with one layer per cascade) as a ping-pong pair so a re-bake never touches an image in flight.
// Used for the ocean shore map (RG: terrain height + water level) and the fog terrain height map (R:
// terrain height), both raw world-space heights around the camera.
//
// Flow (all cheap: no GPU sync, no command-buffer re-record):
//   1. upload()        stages the texels into this frame slot's host-visible buffer (call between
//                      beginFrame's fence wait and submit) together with the region they describe.
//   2. recordUpload()  copies them into the INACTIVE image inside the re-recorded-every-frame primary CB
//                      — the destination was sampled by older submissions the last time it was active,
//                      so the discard transition needs a real execution dependency on those reads
//                      (fences order the CPU, not the GPU).
//   3. flipIfPending() called where the UBO is written: activates the new image together with its
//                      center/sizes, so the shader params and the image contents stay coherent.
// Consumers bind getView() through UPDATE_AFTER_BIND bindings rewritten per frame slot when
// getGeneration() changed, so the flip never re-records cached command buffers.
export class BakedWorldMap final
{
public:
    BakedWorldMap() = default;
    ~BakedWorldMap();
    BakedWorldMap(const BakedWorldMap&) = delete;

    // channels: 1 = R32F, 2 = RG32F, 4 = RGBA32F (interleaved floats per texel in upload()).
    void initialize(uint32 resolution, uint32 numLayers, uint32 channels, const char* debugName);

    // worldSizes = world meters covered per layer; userParam = one baked value carried with the flip
    // (the fog map's terrain sea level). texels are layer-major, resolution^2 * channels floats each.
    void upload(oc::span<const float> texels, const glm::vec2& centerXZ, const glm::vec2& worldSizes, float userParam, uint32 frameIdx);
    void recordUpload(CommandBuffer& commandBuffer); // no-op unless an upload is pending
    void flipIfPending();
    void clear() { m_worldSizes = glm::vec2(0.0f); } // consumers' 1/size params drop to 0; images sit unused

    vk::ImageView getView() const { return m_view[m_active]; }
    // Bumped on every flip; starts at 1 so a zero-initialized per-slot mirror always writes once.
    // Lets consumers change-detect getView() instead of rewriting descriptors every frame.
    uint32 getGeneration() const { return m_generation; }
    vk::Sampler getSampler() const { return m_sampler.getSampler(); } // clamp-to-edge (region snapshot, never tile)
    glm::vec2 getCenter() const { return m_center; }
    glm::vec2 getWorldSizes() const { return m_worldSizes; } // .x == 0 = no map
    float getUserParam() const { return m_userParam; }

private:
    uint32 m_resolution = 0;
    uint32 m_numLayers = 0;
    uint32 m_channels = 1;

    vk::Image m_image[2]{};
    VmaAllocation m_memory[2]{};
    vk::ImageView m_view[2]{};
    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_staging; // host-visible, mapped
    oc::array<oc::span<uint8>, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_stagingMapped;
    Sampler m_sampler;

    int m_uploadSlot = -1; // staging slot holding a not-yet-recorded upload (-1 = none)
    uint32 m_active = 0;
    uint32 m_generation = 1;
    bool m_flipPending = false;
    glm::vec2 m_center = glm::vec2(0.0f);     // ACTIVE region (UBO writers read these)
    glm::vec2 m_worldSizes = glm::vec2(0.0f);
    float m_userParam = 0.0f;
    glm::vec2 m_pendingCenter = glm::vec2(0.0f); // region of the staged (not yet flipped) map
    glm::vec2 m_pendingSizes = glm::vec2(0.0f);
    float m_pendingUserParam = 0.0f;
};
