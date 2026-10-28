export module RendererVK:RenderMaterial;

import Core;
import Core.glm;

import :Layout;
import :RenderMaterialState; // the state

// RAII owner of a material slot (move-only, like RenderMesh), for meshes without an ObjectContainer (Renderer::createMesh / spawnMeshNode). Destroying it
// frees the slot, and a textured material's own textures (retired once the GPU has drained). A DERIVED material shares its source's textures: destroy it
// first (declare it after its source). Destroy every RenderNode drawing it before it. MAIN THREAD. The state is in RenderMaterialState.ixx; the multi-line
// bodies are below the class.
export class RenderMaterial final : public RenderMaterialState
{
public:

    // What the mip bytes ARE: RGBA8, or BC blocks the caller compressed (File TextureConvert::compressBlockRows).
    enum class ETextureEncoding : uint8 { Rgba8, BC1, BC3, BC5 };

    struct TextureDesc
    {
        uint32 width = 0;
        uint32 height = 0;
        const oc::vector<oc::span<uint8>>* albedoMips = nullptr;  // sRGB, level 0 first, each level half the previous
        const oc::vector<oc::span<uint8>>* normalMips = nullptr;  // optional, the same size: a LINEAR RGB tangent-space normal map (x along U, y along V)
        ETextureEncoding albedoEncoding = ETextureEncoding::Rgba8;
        ETextureEncoding normalEncoding = ETextureEncoding::Rgba8; // BC5 sets MATERIAL_FLAG_BC5_NORMAL (Z rebuilt)
        float alphaCutoff = 0.0f;   // > 0: alpha-tested (EAlphaMode::Mask, the cutoff rides `opacity`): draw it on LitMasked
        uint32 extraFlags = 0;      // RendererVKLayout::MATERIAL_FLAG_* to add (e.g. MATERIAL_FLAG_BILLBOARD)
        const char* debugName = ""; // its textures are "<debugName>/albedo" and "/normal": name it "<group>/<item>" for a box per item in the VRAM view
    };

    // ---- Creating ----

    // The fallback textures, opaque, onto `pipeline` (Ocean / TerrainLit add their material flag, like an ObjectContainer override). One per system, shared
    // by its meshes.
    static RenderMaterial createForMesh(RendererVKLayout::EPipelineIndex pipeline, bool rayTraced);
    static RenderMaterial createTextured(const TextureDesc& desc); // its own textures, from the caller's mip chains
    // A copy of material `source` (any index: a solid colour too) with `extraFlags` added, SHARING its textures: the handle owns the slot only.
    static RenderMaterial derive(uint16 source, uint32 extraFlags);
    // The per-entity TINT (RenderNode::setMaterialOverride): a plain index, cached and shared, never freed. [Concurrency: LOCK-FREE on a hit]
    static uint16 getOrCreateSolidColor(const glm::vec3& color);

    // ---- The handle ----

    RenderMaterial() = default;
    RenderMaterial(RenderMaterial&& other) noexcept { moveFrom(other); }
    RenderMaterial& operator=(RenderMaterial&& other) noexcept;
    ~RenderMaterial() { destroy(); }

    bool isValid() const { return m_index != UINT16_MAX; }
    uint16 index() const { return m_index; } // what Renderer::spawnMeshNode and RenderTreeSet::Rep take
    void destroy();

    // Its RendererVKLayout::MATERIAL_FLAG_* bits, rewritten in place (e.g. a distance-fade band). No re-record: a contents upload.
    uint32 getFlags() const;
    void setFlags(uint32 flags);

    // ---- Not copyable ----

    RenderMaterial(const RenderMaterial&) = delete;
    RenderMaterial& operator=(const RenderMaterial&) = delete;
};

// ---- Inline bodies ----

inline RenderMaterial& RenderMaterial::operator=(RenderMaterial&& other) noexcept
{
    if (this != &other)
    {
        destroy();
        moveFrom(other);
    }
    return *this;
}
