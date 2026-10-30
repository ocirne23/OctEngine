export module RendererVK:RenderTexture;

import Core;

import :RenderTextureState; // the state
import :RenderMaterial;     // ETextureEncoding

// RAII owner of a bare bindless texture (move-only, like RenderMesh): its index in the shaders' u_textures. Destroying it retires the image once the GPU has
// drained. MAIN THREAD. The state is in RenderTextureState.ixx; the multi-line bodies are below the class.
export class RenderTexture final : public RenderTextureState
{
public:

    // sRGB from the caller's mip chain (level 0 first, each level half the previous; the ground clutter's fern fronds), RGBA8 or the BC
    // blocks the caller compressed (`encoding`).
    static RenderTexture create(uint32 width, uint32 height, const oc::vector<oc::span<uint8>>& mips, const char* debugName,
                                RenderMaterial::ETextureEncoding encoding = RenderMaterial::ETextureEncoding::Rgba8);

    RenderTexture() = default;
    RenderTexture(RenderTexture&& other) noexcept { moveFrom(other); }
    RenderTexture& operator=(RenderTexture&& other) noexcept;
    ~RenderTexture() { destroy(); }

    bool isValid() const { return m_index != UINT16_MAX; }
    uint16 index() const { return m_index; } // the shaders' u_textures index
    void destroy();

    RenderTexture(const RenderTexture&) = delete;
    RenderTexture& operator=(const RenderTexture&) = delete;
};

// ---- Inline bodies ----

inline RenderTexture& RenderTexture::operator=(RenderTexture&& other) noexcept
{
    if (this != &other)
    {
        destroy();
        moveFrom(other);
    }
    return *this;
}
