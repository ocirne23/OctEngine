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
//                      - the destination was sampled by older submissions the last time it was active,
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
    // The 8-BIT form: ONE R8 unorm channel, world-FIXED maps included (the faction map) - uploadBytes() feeds it.
    void initializeBytes(uint32 resolution, uint32 numLayers, const char* debugName);
    void uploadBytes(oc::span<const uint8> texels, const glm::vec2& centerXZ, const glm::vec2& worldSizes, float userParam, uint32 frameIdx);
    // A PARTIAL upload of the 8-bit form (one layer): `image` is the WHOLE image (resolution^2 bytes), rect the texels
    // that changed since the last call (x0, y0, x1, y1, inclusive). Only a rectangle is staged and copied - this call's
    // rect joined with the last call's, because the image it lands in (the inactive one of the pair) missed that one.
    // The first two calls after initialize copy the whole image (each image of the pair once).
    // lut (256 bytes, optional): every staged byte goes through it - a curve on the values, paid on the CPU for the
    // copied rectangle only, never in a shader.
    void uploadBytesRect(const uint8* image, const glm::ivec4& rect, const glm::vec2& centerXZ, const glm::vec2& worldSizes, float userParam, uint32 frameIdx,
        const uint8* lut = nullptr);

    // worldSizes = world meters covered per layer; userParam = one baked value carried with the flip
    // (the fog map's terrain sea level). texels are layer-major, resolution^2 * channels floats each.
    void upload(oc::span<const float> texels, const glm::vec2& centerXZ, const glm::vec2& worldSizes, float userParam, uint32 frameIdx);
    void requestFullCopies() { m_fullCopiesLeft = 2; m_lastRect = glm::ivec4(0, 0, -1, -1); } // the next two uploadBytesRect calls copy the whole image again (a new map)
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
    uint32 m_texelBytes = sizeof(float); // per texel: channels x 4 (float), or 1 (the 8-bit form)
    void create(vk::Format format, const char* debugName);

    vk::Image m_image[2]{};
    VmaAllocation m_memory[2]{};
    vk::ImageView m_view[2]{};
    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_staging; // host-visible, mapped
    oc::array<oc::span<uint8>, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_stagingMapped;
    Sampler m_sampler;

    int m_uploadSlot = -1; // staging slot holding a not-yet-recorded upload (-1 = none)
    // uploadBytesRect: the texel rectangle the pending upload copies (x0, y0, x1, y1; x1 < x0 = the whole image, the
    // other uploads), the last call's own rectangle, and how many whole-image copies are still owed.
    glm::ivec4 m_copyRect{ 0, 0, -1, -1 };
    glm::ivec4 m_lastRect{ 0, 0, -1, -1 };
    uint32 m_fullCopiesLeft = 2;
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
