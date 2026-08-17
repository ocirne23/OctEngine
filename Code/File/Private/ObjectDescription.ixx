export module File:ObjectDescription;

import Core;
import :AssetParser;

export struct MaterialOverridesDesc
{
    bool present = false;
    oc::string pipeline;                // "LitOpaque" / "UnlitOpaque" / "Sky" / ... (empty = default)
    bool excludeFromRayTracing = false;
    bool useSceneTextures = true;
    int diffuseTexIdx = -1;              // -1 leaves the override default in place
    int normalTexIdx = -1;
    int metalRoughnessTexIdx = -1;
};

export struct ObjectContainerDesc
{
    oc::string name;
    oc::string path;                    // model file path, or procedural shape name
    bool procedural = false;             // Loader: Procedural vs Assimp (default)
    bool mergeNodes = false;
    bool preTransformVertices = false;
    float decimationFactor = 1.0f;       // < 1 simplifies base geometry at cook time (see SceneCookOptions)
    MaterialOverridesDesc materialOverrides;
};

export bool toObjectContainerDesc(const AssetNode& node, ObjectContainerDesc& out);

export bool loadObjectContainerDesc(const oc::string& path, ObjectContainerDesc& out, oc::string& outError);
