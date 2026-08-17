export module RendererVK:TextureManager;

import Core;
import File.fwd;

import :VK;
import :Layout;
import :Texture;

export class TextureManager final
{
public:

    ~TextureManager();
    //uint16 upload(const oc::vector<ITextureData*>& textureData, bool generateMips);
    uint16 upload(const ITextureData& textureData, bool generateMips, bool sRGB = false);
    // Standalone texture file (e.g. a terrain biome .dds): same slot/streaming/descriptor handling as the
    // ITextureData path, resolved relative to the working directory (Assets/).
    uint16 upload(const char* filePath, bool generateMips, bool sRGB = false);
    // Destroys a texture (ObjectContainer teardown) and recycles its slot for a later upload. The caller
    // guarantees the GPU is idle (Renderer::processPendingTextureFrees); unregisters from the
    // TextureStreamer and queues a fallback rewrite of the slot's bindless descriptor entries.
    void free(uint16 idx);
    // View for descriptor-array slot idx: freed (not yet recycled) slots substitute the fallback diffuse
    // so the bindless arrays never reference a destroyed image.
    vk::ImageView getViewForDescriptor(uint16 idx) const
    {
        const vk::ImageView view = m_textures[idx].getImageView();
        return view ? view : m_textures[RendererVKLayout::FALLBACK_DIFFUSE_TEX_IDX].getImageView();
    }
    const Texture& getTexture(uint16 idx) const { assert(idx < m_textures.size()); return m_textures[idx]; }
    // Mutable access for the TextureStreamer's residency swaps (adoptStreamedImage).
    Texture& getTextureMutable(uint16 idx) { assert(idx < m_textures.size()); return m_textures[idx]; }
    const oc::vector<Texture>& getTextures() const { return m_textures; }
	size_t getNumTextures() const { return m_textures.size(); }

    // Live texture-array descriptor capacity; grown by upload() (doubling, clamped to getDescriptorCap()).
    // Descriptor sets are allocated with this as their variable descriptor count, so the Renderer watches
    // getGeneration() and re-allocates them when it changes (the pipelines/layouts stay untouched).
    uint32 getMaxTextures() const { return m_maxTextures; }
    uint32 getGeneration() const { return m_generation; }
    // Fixed upper bound the descriptor set layouts declare: the device's per-stage sampled image limit
    // (with margin for the non-array image bindings), clamped to the uint16 index space and a soft cap
    // matched to the descriptor pool sizing in Device.cpp.
    uint32 getDescriptorCap() const;

private:

    uint16 uploadImpl(const oc::function<bool(Texture&)>& initialize);

    oc::vector<Texture> m_textures;
    oc::vector<uint16> m_freeSlots; // freed by ObjectContainer teardown, recycled by upload()
    uint32 m_maxTextures = RendererVKLayout::INITIAL_TEXTURES;
    uint32 m_generation = 0;
};

export namespace Globals
{
OC_INIT_SEG(OC_SEG_VK_DATA)
    TextureManager textureManager;
}