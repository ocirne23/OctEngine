export module RendererVK:EntityPreviewTypes;

import Core;
import Core.glm;
import Core.Transform;

// The editor previews (Renderer::submitEntityPreview): plain data, NOTHING of it is in the live scene. Each panel draws into a target of its own.
export enum class EPreviewTarget : uint8 { EntityEditor, AnimationEditor };
export constexpr uint32 PREVIEW_TARGET_COUNT = 2;

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
    // A skinned source: its influences (SkinningVertex units). With a paletteOffset (into the frame's palette, in bones) the vertex shader skins it with that
    // palette; UINT32_MAX = the bind pose.
    uint32 skinVertexOffset = UINT32_MAX;
    uint32 paletteOffset = UINT32_MAX;
};

export struct EntityPreviewFrame
{
    EPreviewTarget target = EPreviewTarget::EntityEditor;
    glm::mat4 viewProj{ 1.0f };    // REVERSED-Z, y flipped: the pass uses a plain positive-height viewport
    glm::vec3 cameraPos{ 0.0f };
    glm::uvec2 size{ 0u };         // pixels drawn in the target, from its top-left corner
    float gridY = 0.0f;
    bool grid = true;
    bool wireframe = false;
    oc::vector<EntityPreviewMesh> meshes;
    oc::vector<glm::mat4> palette; // skinning matrices (global * inverseBind) the skinned meshes index
};
