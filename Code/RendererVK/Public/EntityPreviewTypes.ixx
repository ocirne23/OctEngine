export module RendererVK:EntityPreviewTypes;

import Core;
import Core.glm;
import Core.Transform;

// What the Entity Editor's preview draws (Renderer::submitEntityPreview): plain data, NOTHING of it is in the live scene.
export struct EntityPreviewMesh
{
    Transform world;
    glm::vec3 tint{ -1.0f };       // x >= 0: a flat colour instead of the material's
    bool highlighted = false;      // the selected node: a rim light
    uint16 materialIdx = 0;
    // A static mesh names its MeshInfo (it may stream: read at record time). A skinned one draws its bind pose from the
    // source ranges directly (meshIdx = UINT16_MAX).
    uint16 meshIdx = UINT16_MAX;
    uint32 indexCount = 0;
    uint32 firstIndex = 0;
    int32 vertexOffset = 0;
};

export struct EntityPreviewFrame
{
    glm::mat4 viewProj{ 1.0f };    // REVERSED-Z, y flipped: the pass uses a plain positive-height viewport
    glm::vec3 cameraPos{ 0.0f };
    glm::uvec2 size{ 0u };         // pixels drawn in the target, from its top-left corner
    float gridY = 0.0f;
    bool grid = true;
    bool wireframe = false;
    oc::vector<EntityPreviewMesh> meshes;
};
