export module Entity:BoneModel;

import :Entity;
import :RenderComponent;
import Core;
import Core.glm;
import Core.Sphere;
import Core.Transform;
import Animation;
import RendererVK;

// What SceneAnimatorComponent and HumanoidAnimatorComponent have in common: a rigid-part model made of
// BONES inside one entity. A bone is what an entity with only a transform and a RenderComponent would be,
// minus the entity: a transform and a RenderNode in the component - no allocation of its own, no spatial
// entry, no visit, no child list.
// NOT a component and NOT a base class with virtuals: plain structs and TEMPLATES over the component's own
// bone type (it needs `nodeLocal` and `node`), so each animator compiles its own copy with nothing between
// it and the code - the sharing costs no call, no branch and no indirection.

// The authored bones. Shared by every instance of the template; the animators' rigs derive from it.
export struct BoneModelRig
{
    struct Bone
    {
        oc::string name;
        int32 parent = -1;                                       // -1 = the entity; bones are parent before child
        Transform bind;                                          // authored local transform
        oc::shared_ptr<const RenderComponent::SpawnInfo> render; // the bone's `Component Render`; null = a pure joint
        bool renderOffsetOnly = false;                           // the render recipe's local transform is a pure offset
                                                                 // (no rotation, scale 1): nodeLocal needs no quaternion product
    };

    oc::vector<Bone> bones;
    bool flat = true; // no bone has a parent bone: placing needs no world chain
};

// The per-instance state (where a tick's distance comes from, whether the bones must be composed again) is
// NOT here: each animator holds those few fields itself and has its own beginTick() / place().

// Sizes `bones` to the rig and spawns the render nodes: the same recipe as RenderComponent::spawn (static
// nodes only - a bone IS the rigid part). nodeLocal = bind x the render recipe's local transform.
export template <typename Bones>
void spawnBoneNodes(const BoneModelRig& rig, const Transform& base, Bones& bones)
{
    const uint32 numBones = (uint32)rig.bones.size();
    bones.resize(numBones);
    for (uint32 i = 0; i < numBones; ++i)
    {
        const BoneModelRig::Bone& src = rig.bones[i];
        const RenderComponent::SpawnInfo* render = src.render.get();
        if (!render || !render->container)
            continue;
        bones[i].nodeLocal = composeTransform(src.bind, render->localTransform);
        // The first placement puts it where it belongs.
        bones[i].node = render->container->spawnNodeForIdx(render->nodeIdx, composeTransform(base, bones[i].nodeLocal));
        if (bones[i].node.isValid() && render->color.x >= 0.0f)
            bones[i].node.setMaterialOverride(Globals::rendererVK.createSolidColorMaterial(render->color));
    }
}

// A FLAT rig: every node transform is world x nodeLocal, with `world` prepared once (BoneMath, SSE).
export template <typename Bones>
inline void placeFlatBones(Bones& bones, const Transform& world)
{
    const BoneMath::ParentCompose compose(world);
    for (auto& bone : bones)
    {
        if (!bone.node.isValid())
            continue;
        Transform placed;
        compose.apply(bone.nodeLocal, placed);
        bone.node.setTransform(placed);
    }
}

export template <typename Bones>
inline void submitBones(Renderer& renderer, const Bones& bones, uint32 passMask)
{
    for (const auto& bone : bones)
        if (bone.node.isValid())
            renderer.renderNode(bone.node, passMask); // lock-free (the parallel entity pass)
}

// REST-pose render bounds of every bone, in the space `toEntity` maps the entity's local space into
// (CullMode RootOnly's tree bounds).
export template <typename Bones>
void addBoneRestBounds(const BoneModelRig& rig, const Bones& bones, const Transform& toEntity, Sphere& bounds, bool& any)
{
    oc::vector<Transform> rest(rig.bones.size());
    for (size_t i = 0; i < rig.bones.size(); ++i)
    {
        const BoneModelRig::Bone& src = rig.bones[i];
        rest[i] = composeTransform(src.parent < 0 ? toEntity : rest[src.parent], src.bind);
        if (i >= bones.size() || !bones[i].node.isValid())
            continue;
        const Sphere& local = bones[i].node.getLocalBounds();
        if (!(local.radius >= 0.0f) || !std::isfinite(local.radius) || glm::any(glm::isnan(local.pos)) || glm::any(glm::isinf(local.pos)))
            continue;
        const Transform t = composeTransform(rest[i], src.render->localTransform);
        Sphere placed(t.quat * (local.pos * t.scale) + t.pos, local.radius * t.scale);
        if (any)
            bounds.combineSphere(placed);
        else
            bounds = placed;
        any = true;
    }
}
