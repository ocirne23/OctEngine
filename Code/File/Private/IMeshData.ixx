export module File:IMeshData;

import File.fwd;

import Core;
import Core.glm;
import Core.AABB;

export OC_FWD_ATTACH class IMeshData
{
public:
    virtual ~IMeshData() = default;


    virtual const glm::vec3* getVertices() const = 0;
    virtual const glm::vec3* getNormals() const = 0;
    virtual const glm::vec3* getTangents() const = 0;
    virtual const glm::vec3* getBitangents() const = 0;
    virtual const glm::vec3* getTexCoords() const = 0;
    virtual const uint32* getIndices() const = 0;
    virtual uint32 getNumVertices() const = 0;
    virtual uint32 getNumIndices() const = 0;
    virtual uint32 getMaterialIndex() const = 0;
    virtual AABB getAABB() const = 0;
    virtual const char* getName() const = 0;

    // Skinning: per-vertex bone influences (up to 4). Indices reference the scene Skeleton's bone array
    // (see ISceneData::getSkeleton). Default: not skinned. Arrays, when present, are getNumVertices() long.
    virtual bool isSkinned() const { return false; }
    virtual const glm::uvec4* getBoneIndices() const { return nullptr; }
    virtual const glm::vec4* getBoneWeights() const { return nullptr; }

    // Baked LOD chains (cooked scene cache): index-only levels beyond LOD0 over this mesh's vertices,
    // pre-simplified at cook time. Level 0 here = the first REDUCED level. Default: none (the renderer
    // meshopt-generates chains at load instead).
    virtual uint32 getNumLodLevels() const { return 0; }
    virtual const uint32* getLodIndices(uint32 /*level*/, uint32& outNumIndices) const { outNumIndices = 0; return nullptr; }
    // Geometric deviation of the level from LOD0 in mesh-local units (meshopt simplify error x mesh
    // extents); drives the renderer's screen-space-error LOD selection.
    virtual float getLodError(uint32 /*level*/) const { return 0.0f; }
};