export module Spatial:OcclusionState;

import Core;
import Core.glm;
import Settings;
import :Types;

export class SpatialOccluder; // declared before their friend lines
export class SpatialIndexState;

// Everything of OcclusionBuffer but its public API (Occlusion.ixx): the occluder set, the depth raster and the IOcclusionTester answer the cull job uses.
export class OcclusionBufferState : public IOcclusionTester
{
protected:

    friend class SpatialOccluder;   // removeOccluder
    friend class SpatialIndexState; // the cull job: isEnabled, render, the occlusion test

    void removeOccluder(SpatialHandle handle);
    bool isEnabled() const { return m_settings.enabled; }
    // isVisible() answers against the result until the next render(). viewProjRelCamera: camera-relative.
    void render(const glm::mat4& viewProjRelCamera, const glm::dvec3& cameraPos);
    bool isVisible(const glm::vec3& centerRelCamera, const glm::vec3& halfExtent) override;

    static constexpr uint32 Width = 256;
    static constexpr uint32 Height = 144;
    static constexpr uint32 BlockSize = 8;
    static constexpr uint32 BlocksX = Width / BlockSize;
    static constexpr uint32 BlocksY = Height / BlockSize;
    static constexpr float NearEpsilon = 1e-4f;

    void clipAndRasterize(const glm::vec4 (&clip)[3]);
    void rasterizeTriangle(const glm::vec4& c0, const glm::vec4& c1, const glm::vec4& c2);

    struct Occluder
    {
        oc::vector<glm::vec3> worldVertices; // world space, transform baked at add
        uint32 gen = 0;
        bool used = false;
    };

    // addOccluder/removeOccluder run concurrently from spawn jobs; render() runs where no spawn is legal.
    std::mutex m_occluderMutex;
    oc::vector<Occluder> m_occluders;
    oc::vector<uint32> m_freeOccluders;
    oc::vector<float> m_depth;    // Width * Height, NDC z, 1e30 = no occluder
    oc::vector<float> m_blockMax; // BlocksX * BlocksY, farthest depth per block
    glm::mat4 m_viewProjRel = glm::mat4(1.0f);
    bool m_rendered = false; // valid depth data for the current frame
    SpatialOcclusionSettings& m_settings = Globals::settings.spatial.occlusion;
    // Counted atomically (isVisible runs concurrently); mirrored into the int stat once per render().
    oc::atomic<int> m_statHiddenCells = 0;
};
