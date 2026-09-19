export module Entity:SceneAnimatorComponent;

import :Entity;
import :RenderComponent;
import Core;
import Core.glm;
import Core.Sphere;
import Core.Transform;
import File;
import Animation;
import RendererVK;

// The authored bones + the SceneAnimation that moves them. Shared by every instance of the template.
export struct SceneAnimRig
{
    static constexpr uint16 InvalidBone = 0xFFFF;

    struct Bone
    {
        oc::string name;
        int32 parent = -1;                                       // -1 = the entity; bones are parent before child
        Transform bind;                                          // authored local transform
        oc::shared_ptr<const RenderComponent::SpawnInfo> render; // the bone's `Component Render`; null = a pure joint
    };

    oc::vector<Bone> bones;
    SceneAnimation anim;
    oc::vector<uint16> partBones; // per anim.parts entry
};

// A rigid-part model inside ONE entity (Animation:Procedural moves it). A BONE is what an entity with
// only a transform and a RenderComponent would be, minus the entity: a local transform and a RenderNode,
// in a vector of this component - no allocation of its own, no spatial entry, no visit, no child list.
// The walk follows the distance this entity moves (or the distance a parent feeds it). A tick that does
// not change the pose writes no bone, and a frame where neither the pose nor the entity's world
// transform changed composes nothing: the bones are only submitted.
// Culling is the ENTITY's: the bones ride its pass mask, and CullMode RootOnly measures them into the
// tree bounds (addRestBounds). A bone model on a PerEntity entity with no Render of its own has a point
// entry and is never culled - author RootOnly, or put it under a RootOnly root.
export struct SceneAnimatorComponent
{
    static constexpr EComponentID getId() { return EComponentID_SceneAnimator; }

    struct Bone
    {
        Transform local; // the animated pose; starts at the rig's bind
        Transform world; // of the last place()
        RenderNode node; // invalid = a pure joint
    };

    const SceneAnimRig* rig = nullptr;
    oc::vector<Bone> bones; // per rig bone, sized at spawn
    SceneAnimatorState state;
    Transform placedWorld;  // the entity world transform of the last place()
    glm::vec3 lastPos = glm::vec3(0.0f);
    // >= 0: the PARENT drives (drive(), before this entity's visit): it feeds the distance (metres, parent
    // space) - a model child does not move in its parent - AND the delta of its own tick. A covered model
    // child has no spatial entry, so the World hands it the full frame delta every frame; the animator
    // must follow its parent's SIM LOD cadence instead, and does nothing on a frame with no feed.
    float drivenDistance = -1.0f;
    float drivenDelta = 0.0f;
    uint8 hasLastPos : 1 = 0;
    uint8 placeDirty : 1 = 1; // a bone's local changed since the last place()
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
    // The placement tail of Entity::updateSelf: every visit, frozen or not. passMask 0 = culled.
    void place(Renderer& renderer, const Transform& world, uint32 passMask);
    // REST-pose render bounds of every bone, in the space `toEntity` maps the entity's local space into.
    void addRestBounds(const Transform& toEntity, Sphere& bounds, bool& any) const;

    // Once per parent tick, also with 0 metres: a standing unit's swing still has to fade out.
    void drive(float metres, float deltaSeconds)
    {
        drivenDistance = glm::max(drivenDistance, 0.0f) + metres;
        drivenDelta += deltaSeconds;
    }

    // Gameplay override of a layer's weight (an attack swing, a forced idle); < 0 hands it back.
    void setLayerWeight(uint32 layer, float weight) { if (layer < state.layers.size()) state.layers[layer].manualWeight = weight; }
    int32 findLayer(oc::string_view name) const { return rig ? rig->anim.findLayer(name) : -1; }
};

export const SceneAnimatorComponent::SpawnInfo* getSceneAnimatorSpawnInfo(const Entity* entity);
export void writeSceneAnimatorSpawnInfo(const SceneAnimatorComponent::SpawnInfo& info, AssetNode& out);
