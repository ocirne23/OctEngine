export module Entity:SceneAnimatorComponent;

import :Entity;
import :RenderComponent;
import :BoneModel;
import Core;
import Core.glm;
import Core.Sphere;
import Core.Transform;
import File;
import RendererVK;

// The authored model: the bones (BoneModelRig) and the closed-form motion that moves them. Shared by
// every instance of the template, immutable after finalize().
// No keys, no sampled pose, no blend of poses: a LAYER is one phase + one weight (both per instance), a
// TRACK is
//   value = layerWeight * amplitude * sin(2 pi * (harmonic * layerPhase + trackPhase))
// turned into a rotation about a fixed bone-local axis around the bind rotation, or a translation along
// a fixed offset. A blend IS the weight.
// The fixed four-limb walk is NOT here: HumanoidAnimatorComponent.
export struct SceneAnimRig : BoneModelRig
{
    struct Layer
    {
        oc::string name;
        // Stride layer (cyclesPerMetre > 0): the phase follows the DISTANCE moved - feet do not slide - and
        // the weight follows the speed, 1 at fullSpeed. Else a timed layer: cyclesPerSecond at `weight`.
        float cyclesPerMetre = 0.0f;
        float fullSpeed = 1.0f;
        float cyclesPerSecond = 1.0f;
        float weight = 1.0f;
        float fadeRate = 8.0f; // weight change per second
    };

    struct Track
    {
        glm::vec3 axis = glm::vec3(0.0f, 0.0f, 1.0f); // rotation: unit, bone-local. translation: direction * metres
        float amplitude = 0.0f;                       // rotation: radians. translation: 1
        float sinPhase = 0.0f;                        // of 2 pi * trackPhase, so a track needs no trig of its own
        float cosPhase = 1.0f;
        uint16 layer = 0;
        uint8 harmonic = 1; // 1 or 2 cycles per layer cycle
        bool translate = false;
    };

    // A bone that tracks move. Its tracks are one run: the rotations first, then the translations.
    struct Part
    {
        uint16 bone = 0;
        uint16 firstTrack = 0;
        uint16 numRotations = 0;
        uint16 numTranslations = 0;
        bool bindRotIdentity = true;
    };

    // The usual limb - ONE rotation track, no translation, identity bind rotation - does not become a
    // Part: up to four of them are one SoA batch, evaluated in one SSE pass (the limb walk is one batch).
    struct SwingBatch
    {
        float halfAmplitude[4] = {};
        float cosPhase[4] = {};
        float sinPhase[4] = {};
        float axisX[4] = {};
        float axisY[4] = {};
        float axisZ[4] = {};
        uint16 bone[4] = {};
        uint16 layer[4] = {};
        uint8 harmonic[4] = { 1, 1, 1, 1 };
        uint8 count = 0;
        bool uniformLayer = true; // every lane reads the same layer + harmonic: broadcast, no gather
    };

    // A limb walk: legs in opposite phase, each arm opposite to the leg on its side. An empty bone name
    // leaves that limb out. bobBone (optional) moves up twice per cycle.
    struct WalkCycle
    {
        glm::vec3 axis = glm::vec3(0.0f, 0.0f, 1.0f);
        float legAngle = 0.61f; // radians
        float armAngle = 0.52f;
        oc::string leftLeg = "LeftLeg";
        oc::string rightLeg = "RightLeg";
        oc::string leftArm = "LeftArm";
        oc::string rightArm = "RightArm";
        oc::string bobBone;
        float bobHeight = 0.0f;
    };

    oc::vector<Layer> layers;
    oc::vector<SwingBatch> swingBatches;
    oc::vector<Part> parts; // what the batches do not cover
    oc::vector<Track> tracks;

    // AUTHORING (World's parse): tracks name their bone; trackPhase in 0..1 cycles, harmonic 1 or 2.
    uint32 addLayer(const Layer& layer);
    void swing(uint32 layer, const oc::string& bone, const glm::vec3& axis, float angleRadians, float trackPhase = 0.0f, uint32 harmonic = 1);
    void bob(uint32 layer, const oc::string& bone, const glm::vec3& offset, float trackPhase = 0.0f, uint32 harmonic = 2);
    void walkCycle(uint32 layer, const WalkCycle& walk);
    // After the bones and every track: resolves the bone names (an unknown one is a warning) and groups
    // the tracks into `swingBatches` + `parts`.
    void finalize(const oc::string& ownerName);

    int32 findLayer(oc::string_view name) const;

private:
    struct Pending { oc::string bone; Track track; };
    void add(uint32 layer, const oc::string& bone, Track track, float trackPhase, uint32 harmonic);
    oc::vector<Pending> m_pending;
};

// A rigid-part model inside ONE entity (see :BoneModel for what a bone is), moved by the GENERAL layer /
// track motion above. The walk follows the distance this entity moves (or the distance a parent feeds
// it). A tick that does not change the pose writes no bone, and a frame where neither the pose nor the
// entity's world transform changed composes nothing: the bones are only submitted.
// Culling is the ENTITY's: the bones ride its pass mask, and CullMode RootOnly measures them into the
// tree bounds (addRestBounds). A bone model on a PerEntity entity with no Render of its own has a point
// entry and is never culled - author RootOnly, or put it under a RootOnly root.
//
// PARKED. Nothing uses it now (the game units carry a HumanoidAnimatorComponent), so it holds NO component
// id and NO type bit, and the Entity class does not know it: no create / destroy case, no update or place
// call, no bounds, no prefab branch. The code is kept compiling so it does not rot. To bring it back:
// an EComponentID + getId(), the Component.ixx table entries, the Entity.cpp create / destroy cases +
// the update / place / addRestBounds calls, World::buildTemplate's `SceneAnimator` branch (the builder,
// World::buildSceneAnimatorSpawnInfo, is still there), Prefab.cpp, the Entity Editor pass-through, the
// natvis entry, GameUnitComponent's tintSubtree + tickModel, and the SIM LOD kind.
export struct SceneAnimatorComponent
{
    struct Bone
    {
        Transform local;     // the animated pose; starts at the rig's bind
        Transform nodeLocal; // local x the render recipe's local transform: refreshed only when `local` changes,
                             // so a flat rig places a bone with ONE compose
        RenderNode node;     // invalid = a pure joint
    };

    struct LayerState
    {
        float phase = 0.0f;         // 0..1
        float weight = 0.0f;
        float manualWeight = -1.0f; // >= 0 replaces the layer's own weight rule
        float s[2] = {};            // sin / cos of harmonic 1 / 2 at `phase`: ONE trig pair per active layer
        float c[2] = {};            // per tick, written by advance(), read by every track of the layer
    };

    const SceneAnimRig* rig = nullptr;
    // Per rig bone / layer, sized at spawn. Inline for the usual rig: they sit in the entity's own
    // allocation - no allocation per spawn, and the visit stays in the entity's memory.
    oc::small_vector<Bone, 6> bones;
    oc::small_vector<LayerState, 2> layers;
    // >= 0: the PARENT drives (feed(), before this entity's visit): it gives the distance (metres, parent
    // space) - a model child does not move in its parent - AND the delta of its own tick. A covered model
    // child has no spatial entry, so the World hands it the full frame delta every frame; the animator
    // must follow its parent's SIM LOD cadence instead, and does nothing on a frame with no feed.
    float drivenDistance = -1.0f;
    float drivenDelta = 0.0f;
    glm::vec3 lastPos = glm::vec3(0.0f); // self-driven: the own movement is measured
    Transform placedWorld;               // the entity world transform of the last place()
    bool hasLastPos = false;
    bool placeDirty = true; // a bone's local changed since the last place()
    bool enabled = true;
    // A timed layer moves a standing model, so the general animator never reports a rest pose: a parent
    // feeds it every tick. (The HumanoidAnimator's is a real flag.)
    static constexpr bool atRest = false;

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
    void addRestBounds(const Transform& toEntity, Sphere& bounds, bool& any) const;

    // Gameplay override of a layer's weight (an attack swing, a forced idle); < 0 hands it back.
    void setLayerWeight(uint32 layer, float weight) { if (layer < layers.size()) layers[layer].manualWeight = weight; }
    int32 findLayer(oc::string_view name) const { return rig ? rig->findLayer(name) : -1; }

    // Once per parent tick, also with 0 metres: a standing unit's swing still has to fade out.
    void feed(float metres, float deltaSeconds)
    {
        drivenDistance = glm::max(drivenDistance, 0.0f) + metres;
        drivenDelta += deltaSeconds;
    }

private:
    // FALSE = no tick this frame. Else deltaSeconds is the tick's delta and outDistance the planar distance
    // moved, in the entity's LOCAL units (so a scaled model keeps its gait).
    bool beginTick(const Entity& entity, float& deltaSeconds, float& outDistance);
    // Phases + weights, one tick (distance in the entity's local units). FALSE = the pose is the same as
    // after the last tick: no evaluation, no write.
    bool advance(float deltaSeconds, float distance);
    // In place, and only the channels the part's tracks move.
    void evaluate(const SceneAnimRig::Part& part, Transform& local) const;
    void evaluateSwings(const SceneAnimRig::SwingBatch& batch); // four limb quaternions in one SSE pass
    void refreshNodeLocal(uint32 boneIdx);
};

export void writeSceneAnimatorSpawnInfo(const SceneAnimatorComponent::SpawnInfo& info, AssetNode& out);
