export module Entity:AnimatorComponent;

import :Entity;
import :AnimationDescription;
import Core;
import Core.Transform;
import File;
import Animation;
import RendererVK;

// An entity HIERARCHY as a skeleton: one bone per descendant of the animator's entity (DFS, so
// parent-before-child), named by the entity name, bind = the authored local transform. Built from the
// template's Scene spawn info and shared by every instance (World-cached). The animator's own entity is
// NOT a bone - its transform belongs to gameplay / physics.
export struct EntityRig
{
    Skeleton skeleton;
    oc::vector<uint16> childSlots; // per bone: index in its parent entity's SceneComponent::children
};

// Drives a sibling skinned RenderComponent: instantiates an AnimationPlayer + AnimStateMachine from a
// .apl AnimatorDesc, retargets its clips against the rig skeleton, ticks them each frame, and pushes the
// resulting bone palette to the renderer. Gameplay sets parameters via stateMachine.setFloat/Bool/Trigger.
// With NO sibling skinned mesh it drives its descendant entities instead (see EntityRig): the local pose
// is written to their pos / rot / scale each tick.
export struct AnimatorComponent
{
    static constexpr EComponentID getId() { return EComponentID_Animator; }

    ~AnimatorComponent();

    AnimationPlayer player;
    AnimStateMachine stateMachine;
    const AnimationSet* clipSet = nullptr;  // shared, World-cached clip library (retargeted to the rig)
    const EntityRig* rig = nullptr;         // set = hierarchy mode
    oc::vector<Entity*> boneEntities;      // hierarchy mode scratch: resolved again every tick, never held
    oc::vector<BlendSpace1D> blendSpaces;  // stable storage referenced by the state machine
    oc::vector<AnimatorDesc::SpeedBinding> stateSpeeds; // playback-speed config per StateId
    AnimatorDesc::SpeedBinding defaultSpeed;             // animator-wide playback-speed fallback
    oc::function<void(const oc::string&)> onEvent;    // gameplay hook for clip event notifies
    bool enabled = true;
    bool hasStateMachine = false;
    bool built = false;

    struct SpawnInfo
    {
        const AnimatorDesc* desc = nullptr;     // parsed .apl graph (owned by AssetRegistry)
        const Skeleton* skeleton = nullptr;     // rig skeleton from the sibling render mesh's container, or the EntityRig's
        const EntityRig* rig = nullptr;         // hierarchy mode (World-cached, shared)
        const AnimationSet* clipSet = nullptr;  // shared clip library (World-cached per skeleton+animator)
        oc::string animatorName;               // kept for re-serialization
        bool enabled = true;
    };

    void spawn(Entity& entity, const SpawnInfo& info, const Transform& base);
    void destroy(Entity& entity, const SpawnInfo& info);
    void update(Entity& entity, Renderer& renderer, float deltaSeconds);
    float resolvePlaybackSpeed() const; // playback rate for the current state (param-driven or constant)
    void applyPoseToHierarchy(Entity& entity);
};

export const AnimatorComponent::SpawnInfo* getAnimatorSpawnInfo(const Entity* entity);

// Serializes an animator spawn recipe into a "Component Animator" node.
export void writeAnimatorSpawnInfo(const AnimatorComponent::SpawnInfo& info, AssetNode& out);
