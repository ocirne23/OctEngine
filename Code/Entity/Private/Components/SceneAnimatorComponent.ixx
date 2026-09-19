export module Entity:SceneAnimatorComponent;

import :Entity;
import Core;
import Core.glm;
import Core.Transform;
import File;
import Animation;

// A SceneAnimation bound to a prefab's child tree. Shared by every instance of the template.
export struct SceneAnimRig
{
    static constexpr uint16 InvalidNode = 0xFFFF;

    // Bind-time only: the entities on a path to a moved part, parent before child. slot = the index in
    // the parent entity's SceneComponent::children; parent -1 = the animator's own entity.
    struct Node { int32 parent = -1; uint16 slot = 0; };

    SceneAnimation anim;
    oc::vector<Node> nodes;
    oc::vector<uint16> partNodes; // per anim.parts entry
};

// Procedural animation of the descendant entities (Animation:Procedural): the walk follows the distance
// this entity moves, so no gameplay code drives it. A tick that does not change the pose touches no
// child at all.
// It HOLDS direct pointers to the moved parts. Every mutation of a SceneComponent::children list drops
// them (SceneComponent::childrenChanged -> unbind, on this entity and its ancestors), and the next
// pose-changing tick binds again through the rig's slot paths. A part cannot die while bound: its
// parent's list holds a reference, and leaving that list is a mutation.
export struct SceneAnimatorComponent
{
    static constexpr EComponentID getId() { return EComponentID_SceneAnimator; }

    const SceneAnimRig* rig = nullptr;
    oc::vector<Entity*> nodes; // per rig node (a part is nodes[rig->partNodes[i]]), valid while bound; null = not in the tree
    SceneAnimatorState state;
    glm::vec3 lastPos = glm::vec3(0.0f);
    uint8 hasLastPos : 1 = 0;
    uint8 bound : 1 = 0;
    uint8 enabled : 1 = 1;

    struct SpawnInfo
    {
        oc::shared_ptr<const SceneAnimRig> rig;
        AssetNode source; // the authored "Component SceneAnimator" node: the recipe IS the serialization
        bool enabled = true;
    };

    void spawn(Entity& entity, const SpawnInfo& info, const Transform& base);
    void destroy(Entity& entity, const SpawnInfo& info);
    void update(Entity& entity, float deltaSeconds);

    void unbind() { bound = false; }

    // Gameplay override of a layer's weight (an attack swing, a forced idle); < 0 hands it back.
    void setLayerWeight(uint32 layer, float weight) { if (layer < state.layers.size()) state.layers[layer].manualWeight = weight; }
    int32 findLayer(oc::string_view name) const { return rig ? rig->anim.findLayer(name) : -1; }

private:
    void bind(Entity& entity);
};

export const SceneAnimatorComponent::SpawnInfo* getSceneAnimatorSpawnInfo(const Entity* entity);
export void writeSceneAnimatorSpawnInfo(const SceneAnimatorComponent::SpawnInfo& info, AssetNode& out);
