export module RendererVK:BakedWorldMap;

import Core;
import :VK;
import :Allocator;
import :Sampler;

// A CPU-baked world-region snapshot on the GPU: an R32F / RG32F / RGBA32F or R8 image (2D, or a 2D array with one layer
// per cascade). Used for the fog terrain data cascades (RGBA: height, water level, packed climate, macro altitude, around
// the camera: a PAIR) and the game's faction map (R8, fixed in the world: ONE image).
//
// Flow (all cheap: no GPU sync, no command-buffer re-record). The frame's UBO is built in the BEGIN-FRAME job
// (RendererState::beginFrame), before the game tick and the procedural update stage their uploads - so an upload's
// region can only go live in the NEXT frame's UBO:
//   1. upload*()        writes the texels straight into the shared staging ring (StagingManager::uploadImageRegion) - into
//                       the INACTIVE image of a pair, into THE image of a single one - together with the region they
//                       describe. The ring's batch runs before this frame's primary (the primary waits on its semaphore);
//                       its barrier waits for the older submissions' reads of the destination (fences order the CPU, not
//                       the GPU).
//   2. flipIfPending()  the next frame's UBO build: the region goes live (and a pair's new image with it).
// A PAIR keeps the contents and the region coherent in every frame: a map whose REGION MOVES (the camera-centred terrain
// data - one frame of the new texels under the old centre flickered every re-centre). A SINGLE image shows its new texels
// one frame before a region change - right for a world-fixed map (the region never changes), and half the memory.
// Consumers bind getView() through UPDATE_AFTER_BIND bindings rewritten per frame slot when getGeneration() changed (a
// pair's flip), so the flip never re-records cached command buffers.
// Main thread (upload*), the begin-frame job (flipIfPending): ordered by the job's kick and join.
export class BakedWorldMap final
{
public:
    BakedWorldMap() = default;
    ~BakedWorldMap();
    BakedWorldMap(const BakedWorldMap&) = delete;

    // channels: 1 = R32F, 2 = RG32F, 4 = RGBA32F (interleaved floats per texel in upload()). pair: see above.
    void initialize(uint32 resolution, uint32 numLayers, uint32 channels, bool pair, const char* debugName);
    // The 8-BIT form: ONE R8 unorm channel, world-FIXED maps included (the faction map) - uploadBytesRect() feeds it.
    void initializeBytes(uint32 resolution, uint32 numLayers, bool pair, const char* debugName);
    // A PARTIAL upload of the 8-bit form (one layer): `image` is the WHOLE image (resolution^2 bytes), rect the texels
    // that changed since the last call (x0, y0, x1, y1, inclusive). Only that rectangle is staged (tightly packed) and
    // copied - for a pair joined with what the destination image missed while the other one was written. The first call
    // after initialize / requestFullCopies copies the whole image (each image once).
    // lut (256 bytes, optional): every staged byte goes through it - a curve on the values, paid on the CPU for the
    // copied rectangle only, never in a shader.
    void uploadBytesRect(const uint8* image, const glm::ivec4& rect, const glm::vec2& centerXZ, const glm::vec2& worldSizes, float userParam,
        const uint8* lut = nullptr);

    // worldSizes = world meters covered per layer; userParam = one baked value carried with the flip
    // (the fog map's terrain sea level). texels are layer-major, resolution^2 * channels floats each.
    void upload(oc::span<const float> texels, const glm::vec2& centerXZ, const glm::vec2& worldSizes, float userParam);
    // The next uploadBytesRect calls copy the whole image again (a new map).
    void requestFullCopies() { m_needsFull = { true, true }; m_missed = { EMPTY_RECT, EMPTY_RECT }; }
    void flipIfPending(); // the UBO build: the last upload's region (and a pair's image) goes live
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
    static constexpr glm::ivec4 EMPTY_RECT{ 0, 0, -1, -1 }; // x1 < x0

    uint32 m_resolution = 0;
    uint32 m_numLayers = 0;
    uint32 m_channels = 1;
    uint32 m_texelBytes = sizeof(float); // per texel: channels x 4 (float), or 1 (the 8-bit form)
    uint32 m_numImages = 2;              // 2 = a pair, 1 = a single image
    void create(vk::Format format, const char* debugName);
    uint32 uploadImage() const { return m_numImages == 2 ? m_active ^ 1u : 0u; } // where an upload lands
    void stage(const glm::vec2& centerXZ, const glm::vec2& worldSizes, float userParam); // an upload's region, live at the flip

    vk::Image m_image[2]{};
    VmaAllocation m_memory[2]{};
    vk::ImageView m_view[2]{};
    Sampler m_sampler;

    // uploadBytesRect, per image: a whole-image copy still owed, and the texels changed while the OTHER image of a pair was
    // written (copied with the next rectangle into this one).
    oc::array<bool, 2> m_needsFull{ true, true };
    oc::array<glm::ivec4, 2> m_missed{ EMPTY_RECT, EMPTY_RECT };
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
