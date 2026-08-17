export module File:ITextureData;

import File.fwd;

import Core;

export class ITextureData
{
public:
    virtual ~ITextureData() = default;

    struct Pixel
    {
        uint8 r, g, b, a;
    };

    virtual const char* getFileName() const = 0;
    virtual const Pixel* getPixels() const = 0;
	virtual uint32 getWidth() const = 0;
    virtual uint32 getHeight() const = 0; // if 0, pixels is a raw data buffer of size width
    virtual const char* getFormatInfo() const = 0;

    // Folder of the scene file (.fbx/.glb) this texture belongs to, used to resolve loose texture files
    // relative to the model first, before falling back to the asset-root-relative path. Empty by default.
    void setRootFolder(oc::string_view rootFolder) { m_rootFolder = rootFolder; }
    const oc::string& getRootFolder() const { return m_rootFolder; }

    static oc::unique_ptr<ITextureData> createFallbackDiffuseTexture();
    static oc::unique_ptr<ITextureData> createFallbackWhiteTexture();
    static oc::unique_ptr<ITextureData> createFallbackNormalTexture();

private:
    oc::string m_rootFolder;
};