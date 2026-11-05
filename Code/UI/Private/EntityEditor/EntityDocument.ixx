export module UI:EntityDocument;

import Core;
import Core.Transform;
import Entity;
import File;

// THE ENTITY EDITOR'S DOCUMENT: one node per entity of a .pre, as plain values. Nothing here is in the world: no spawn, no body, no script run, no render node.
// A value type on purpose: an undo step is a copy, and the preview walks it directly.
export struct EntityDocNode
{
    uint64 id = 0; // stable across undo; the tree's selection and open state key on it
    oc::string name;
    Transform transform;
    bool enabled = true;
    bool global = false; // root only
    EEntityCullMode cullMode = EEntityCullMode::PerEntity;

    // Non-empty: a `Prefab <name>` reference. It saves as the reference plus the overrides above; its components and children stay in the document so that the
    // preview shows them, but they are read-only and never written. Unpacking = clearing this, which makes them an inline part of this file.
    oc::string prefabRef;

    bool hasScene = false;
    bool hasRender = false;
    bool hasAnimator = false;
    bool hasPhysics = false;
    bool hasAudio = false;
    bool hasParticle = false;
    bool hasForce = false;
    bool hasLight = false;
    bool hasNetwork = false;
    bool hasGameUnit = false;
    bool hasGameStructure = false;
    bool hasGameProjectile = false;
    bool hasScript = false;

    RenderComponent::SpawnInfo render;
    AnimatorComponent::SpawnInfo animator;
    PhysicsComponent::SpawnInfo physics;
    AudioComponent::SpawnInfo audio;
    ParticleComponent::SpawnInfo particle;
    ForceComponent::SpawnInfo force;
    LightComponent::SpawnInfo light;
    GameUnitComponent::SpawnInfo gameUnit;
    GameStructureComponent::SpawnInfo gameStructure;
    GameProjectileComponent::SpawnInfo gameProjectile;
    ScriptComponent::SpawnInfo script;
    oc::shared_ptr<const HumanoidAnimatorComponent::SpawnInfo> humanoid; // no editor section for the bones: carried through, null = none

    oc::vector<EntityDocNode> children;

    bool isReference() const { return !prefabRef.empty(); }
};

export EntityDocNode makeDocNode(const EntitySpawnTemplate& tmpl, uint64& nextId);
export oc::string serializeEntityDoc(const EntityDocNode& root, const oc::string& id);
export EntityDocNode* findDocNode(EntityDocNode& root, uint64 id);
export EntityDocNode* findDocParent(EntityDocNode& root, uint64 id); // null for the root itself
export bool docSubtreeContains(const EntityDocNode& root, uint64 id);
export void assignDocIds(EntityDocNode& node, uint64& nextId); // new ids for the whole subtree: a paste or a duplicate
// What would not work at spawn: a name that resolves to nothing, a component missing what it needs. One line each, appended to `out`.
export void collectDocProblems(const EntityDocNode& node, oc::vector<oc::string>& out);
