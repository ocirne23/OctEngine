export module RendererVK:RendererTypes;

import Core;
import Core.glm;

import :Layout;
import :TerrainResources;

// The value types of the Renderer's API (Renderer.ixx), named Renderer::X outside: a base of RendererState, because the state and the helpers use them too.
export struct RendererTypes
{
    // ---- Terrain (setTerrainSplatMaterials) ----

    using TerrainSplatMaterial = ::TerrainSplatMaterial;
    using TerrainSplatCounts = ::TerrainSplatCounts;

    // ---- Procedural grass (setGrassGround) ----

    // One BASE chunk coordinate (the finest drawn terrain node over it): its first vertex and its cells per side, inside a node mesh whose rows are
    // `vertexStride` vertices apart (a base chunk inside a bigger quadtree node is a sub-grid of it).
    struct GrassGroundChunk { glm::ivec2 coord; uint32 firstVertex = 0; uint32 res = 0; };

    // ---- Ground clutter (setClutterAssets) ----

    // One rigid variant mesh: its LOD levels (level 0 first; an empty level = none), y up, the lowest point at 0.
    struct ClutterMesh
    {
        oc::vector<RendererVKLayout::ClutterVertexGpu> vertices[RendererVKLayout::CLUTTER_LODS];
        oc::vector<uint32> indices[RendererVKLayout::CLUTTER_LODS];
    };
};
