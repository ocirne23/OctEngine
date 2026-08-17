export module File:IMaterialData;

import File.fwd;

import Core;
import Core.glm;

export class IMaterialData
{
public:
    virtual ~IMaterialData() = default;
	enum class ETextureType // see Assimp docs, matches aiTextureType
    {
        NONE = 0,
        DIFFUSE = 1,
        SPECULAR = 2,
        AMBIENT = 3,
        EMISSIVE = 4,
        HEIGHT = 5,
        NORMALS = 6,
        SHININESS = 7,
        OPACITY = 8,
        DISPLACEMENT = 9,
        LIGHTMAP = 10,
        REFLECTION = 11,
        BASE_COLOR = 12,
        NORMAL_CAMERA = 13,
        EMISSION_COLOR = 14,
        METALNESS = 15,
        DIFFUSE_ROUGHNESS = 16,
        AMBIENT_OCCLUSION = 17,
        UNKNOWN = 18,
        SHEEN = 19,
        CLEARCOAT = 20,
        TRANSMISSION = 21,
    };

    enum class EAlphaMode
    {
        Opaque, // fully opaque
        Mask,   // alpha-tested against the cutoff (rendered opaque)
        Blend,  // alpha-blended
    };

    virtual const char* getName() const = 0;
    
    virtual glm::vec3 getBaseColor() const = 0;
    virtual glm::vec3 getEmissiveColor() const = 0;
    virtual glm::vec3 getSpecularColor() const = 0;
    virtual float getRoughnessFactor() const = 0;
    virtual float getMetalnessFactor() const = 0;
    virtual float getOpacity() const = 0;
    virtual EAlphaMode getAlphaMode() const = 0;
    virtual float getAlphaCutoff() const = 0;
    virtual float getEmissiveIntensity() const = 0;
    virtual float getRefractiveIndex() const = 0;

    virtual oc::string getTexturePath(ETextureType type) const = 0;

    virtual uint32 getDiffuseTexIdx() const = 0;
    virtual uint32 getNormalTexIdx() const = 0;
    virtual uint32 getOpacityTexIdx() const = 0;
    virtual uint32 getMetalRoughnessTexIdx() const = 0;
};