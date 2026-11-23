export module RendererVK:BakedWorldMap;

import Core;
import :VK;
import :Allocator;
import :Buffer;
import :CommandBuffer;
import :Sampler;
import :Layout;

// A CPU-baked world-region snapshot on the GPU: an R32F / RG32F / RGBA32F or R8 image (2D, or a 2D array with one layer
// per cascade). Used for the fog terrain data cascades (RGBA: height, water level, packed climate, macro altitude, around
// the camera: a PAIR) and the game's faction map (R8, fixed in the world: ONE image).
//
// Flow (all cheap: no GPU sync, no command-buffer re-record). The frame's UBO is built in the BEGIN-FRAME job
// (RendererState::beginFrame), before the game tick and the procedural update stage their uploads - so an upload's
// region can only go live in the NEXT frame's UBO:
//   1. upload*()        stages the texels into this frame slot's host-visible buffer (between beginFrame's fence wait
//                       and present) together with the region they describe.
//   2. recordUpload()   copies them at the top of this frame's primary - into the INACTIVE image of a pair, into THE image
//                       of a single one. The destination was sampled by older submissions, so the transition needs a
//                       real execution dependency on those reads (fences order the CPU, not the GPU).
//   3. flipIfPending()  the next frame's UBO build: the region goes live (and a pair's new image with it).
// A PAIR keeps the contents and the region coherent in every frame: a map whose REGION MOVES (the camera-centred terrain
// data - one frame of the new texels under the old centre flickered every re-centre). A SINGLE image shows its new texels
// one frame before a region change - right for a world-fixed map (the region never changes), and half the memory.
// Consumers bind getView() through UPDATE_AFTER_BIND bindings rewritten per frame slot when getGeneration() changed (a
// pair's flip), so the flip never re-records cached command buffers.
export class BakedWorldMap final
{
public:
    BakedWorldMap() = default;
    ~BakedWorldMap();
    BakedWorldMap(const BakedWorldMap&) = delete;

    // channels: 1 = R32F, 2 = RG32F, 4 = RGBA32F (interleaved floats per texel in upload()). pair: see above.
    void initialize(uint32 resolution, uint32 numLayers, uint32 channels, bool pair, const char* debugName);
    // The 8-BIT form: ONE R8 unorm channel, world-FIXED maps included (the faction map) - uploadBytes() feeds it.
    void initializeBytes(uint32 resolution, uint32 numLayers, bool pair, const char* debugName);
    void uploadBytes(oc::span<const uint8> texels, const glm::vec2& centerXZ, const glm::vec2& worldSizes, float userParam, uint32 frameIdx);
    // A PARTIAL upload of the 8-bit form (one layer): `image` is the WHOLE image (resolution^2 bytes), rect the texels
    // that changed since the last call (x0, y0, x1, y1, inclusive). Only a rectangle is staged and copied - joined with an
    // upload not yet copied, and for a pair with the last call's (the inactive image missed that one). The first call
    // after initialize / requestFullCopies copies the whole image (each image once).
    // lut (256 bytes, optional): every staged byte goes through it - a curve on the values, paid on the CPU for the
    // copied rectangle only, never in a shader.
    void uploadBytesRect(const uint8* image, const glm::ivec4& rect, const glm::vec2& centerXZ, const glm::vec2& worldSizes, float userParam, uint32 frameIdx,
        const uint8* lut = nullptr);

    // worldSizes = world meters covered per layer; userParam = one baked value carried with the flip
    // (the fog map's terrain sea level). texels are layer-major, resolution^2 * channels floats each.
    void upload(oc::span<const float> texels, const glm::vec2& centerXZ, const glm::vec2& worldSizes, float userParam, uint32 frameIdx);
    // The next uploadBytesRect calls copy the whole image again (a new map).
    void requestFullCopies() { m_fullCopiesLeft = m_numImages; m_lastRect = glm::ivec4(0, 0, -1, -1); }
    void recordUpload(CommandBuffer& commandBuffer); // no-op unless an upload is pending
    void flipIfPending(); // the UBO build: the last copied upload's region (and a pair's image) goes live
    void clear() { m_worldSizes = glm::vec2(0.0f); } // consumers' 1/size params drop to 0; images sit unused

    vk::ImageView getView() const { return m_view[m_active]; }
    // Bumped on a pair's flip; starts at 1 so a zero-initialized per-slot mirror always writes once.
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
    uint32 m_numImages = 2;              // 2 = a pair, 1 = a single image
    void create(vk::Format format, const char* debugName);
    void stage(uint32 frameIdx, const glm::vec2& centerXZ, const glm::vec2& worldSizes, float userParam); // an upload's bookkeeping

    vk::Image m_image[2]{};
    VmaAllocation m_memory[2]{};
    vk::ImageView m_view[2]{};
    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_staging; // host-visible, mapped
    oc::array<oc::span<uint8>, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_stagingMapped;
    Sampler m_sampler;

    int m_uploadSlot = -1; // staging slot holding a not-yet-recorded upload (-1 = none)
    // The texel rectangle the pending upload copies (x0, y0, x1, y1; x1 < x0 = the whole image, the other uploads), the
    // last uploadBytesRect call's own rectangle, and how many whole-image copies are still owed (one per image).
    glm::ivec4 m_copyRect{ 0, 0, -1, -1 };
    glm::ivec4 m_lastRect{ 0, 0, -1, -1 };
    uint32 m_fullCopiesLeft = 2;
    uint32 m_active = 0; // a pair's live image (a single image: always 0)
    uint32 m_generation = 1;
    bool m_flipPending = false;
    glm::vec2 m_center = glm::vec2(0.0f);     // LIVE region (UBO writers read these)
    glm::vec2 m_worldSizes = glm::vec2(0.0f);
    float m_userParam = 0.0f;
    glm::vec2 m_pendingCenter = glm::vec2(0.0f); // region of the staged (not yet flipped) map
    glm::vec2 m_pendingSizes = glm::vec2(0.0f);
    float m_pendingUserParam = 0.0f;
};
