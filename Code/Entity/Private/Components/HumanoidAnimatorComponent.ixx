export module Entity:HumanoidAnimatorComponent;

import :Entity;
import :RenderComponent;
import :BoneModel;
import Core;
import Core.glm;
import Core.Sphere;
import Core.Transform;
import File;
import RendererVK;

// THE BARE-MINIMUM ANIMATOR, for exactly one setup (CubeGuy): a bone model (see :BoneModel) whose four
// limb bones swing about their local Z, driven by the distance moved. No layers, no tracks, no parts: one
// phase, one weight, one sine, then the four limbs as ONE SSE pass. SceneAnimatorComponent is the general
// animator; this one does nothing else, on purpose.
//
// configure() checks the setup ONCE at template build - flat rig, the four bones exist, identity bind
// rotation, a limb render recipe with NO local transform (the mesh origin is the pivot: baseshapes'
// Limb3, not Pillar3) - so the tick has no check at all. A setup that does not
// fit is a warning and NO component (World::buildHumanoidAnimatorSpawnInfo returns null): the model is
// absent, which is hard to miss.
export struct HumanoidRig : BoneModelRig
{
    enum Limb : uint32 { LeftLeg, RightLeg, LeftArm, RightArm, NumLimbs };

    struct Desc
    {
        float stride = 6.0f, fullSpeed = 4.0f, fadeSeconds = 0.125f; // in the entity's local units
        float legAngle = 0.61f, armAngle = 0.52f;                    // radians
        oc::string limbs[NumLimbs] = { "LeftLeg", "RightLeg", "LeftArm", "RightArm" };
    };

    float cyclesPerUnit = 1.0f; // 1 / stride
    float invFullSpeed = 1.0f; // the tick multiplies
    float fadeRate = 8.0f;
    // One lane per limb. Lane i IS bone i: configure() reorders `bones` so the limbs are bones 0..3 in
    // Limb order (the rest follow, in authored order) - the tick has no index table.
    // SIGNED half angle (radians, |h| <= 45 degrees so the polynomial needs no normalize): +leg, -leg,
    // -arm, +arm. sin is odd and cos is even, so the signs come out of the polynomial by themselves.
    // THE ONLY PER-LIMB DATA: the limb mesh has its origin AT the pivot (no render offset - configure()
    // requires it), so a swing moves no position. The tick writes four quaternions and nothing else.
    float halfAngle[NumLimbs] = {};

    bool configure(const Desc& desc, const oc::string& ownerName); // after the bones; false = the setup does not fit
};

export struct HumanoidAnimatorComponent
{
    static constexpr EComponentID getId() { return EComponentID_HumanoidAnimator; }

    struct Bone
    {
        Transform nodeLocal; // what the tick writes and the placement reads; there is no separate pose
        RenderNode node;
    };

    const HumanoidRig* rig = nullptr;
    oc::small_vector<Bone, 6> bones; // inline: in the entity's own allocation
    float walkPhase = 0.0f;          // the whole animation state
    float walkWeight = 0.0f;
    // >= 0: the PARENT drives (feed(), before this entity's visit): it gives the distance (metres, parent
    // space) - a model child does not move in its parent - AND the delta of its own tick. A covered model
    // child has no spatial entry, so the World hands it the full frame delta every frame; the animator
    // must follow its parent's SIM LOD cadence instead, and does nothing on a frame with no feed.
    float drivenDistance = -1.0f;
    float drivenDelta = 0.0f;
    glm::vec3 lastPos = glm::vec3(0.0f); // self-driven: the own movement is measured
    Transform placedWorld;               // the entity world transform of the last place()
    bool hasLastPos = false;
    bool placeDirty = true; // a nodeLocal changed since the last place()
    bool atRest = false;    // the rest pose, and only a distance > 0 can change it: the parent stops feeding
    bool enabled = true;

    struct SpawnInfo
    {
        oc::shared_ptr<const HumanoidRig> rig;
        AssetNode source; // the authored "Component HumanoidAnimator" node: the recipe IS the serialization
        bool enabled = true;
    };

    void spawn(Entity& entity, const SpawnInfo& info, const Transform& base);
    void destroy(Entity& entity, const SpawnInfo& info);
    void update(Entity& entity, float deltaSeconds);
    // The placement tail of Entity::updateSelf: every visit, frozen or not. passMask 0 = culled.
    void place(Renderer& renderer, const Transform& world, uint32 passMask);
    void addRestBounds(const Transform& toEntity, Sphere& bounds, bool& any) const;

    // Once per parent tick, also with 0 metres while the swing still has to fade out.
    void feed(float metres, float deltaSeconds)
    {
        drivenDistance = glm::max(drivenDistance, 0.0f) + metres;
        drivenDelta += deltaSeconds;
    }

private:
    // FALSE = no tick this frame. Else deltaSeconds is the tick's delta and outDistance the planar distance
    // moved, in the entity's LOCAL units (so a scaled model keeps its gait).
    bool beginTick(const Entity& entity, float& deltaSeconds, float& outDistance);
};

export const HumanoidAnimatorComponent::SpawnInfo* getHumanoidAnimatorSpawnInfo(const Entity* entity);
export void writeHumanoidAnimatorSpawnInfo(const HumanoidAnimatorComponent::SpawnInfo& info, AssetNode& out);
