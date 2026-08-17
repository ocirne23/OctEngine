module Entity;

import Core;
import Core.glm;
import Core.Log;
import Core.Camera;
import Core.Rect;
import Core.Transform;
import Threading;

import RendererVK;
import :Entity;
import :Component;
import File;

import :AssetRegistry;
import :AnimationDescription;
import Animation;
import Physics;
import Audio;

bool World::initialize()
{
    Globals::assetRegistry.scanDirectory();
    m_updateStaging.initialize(); // main calls this after JobSystem::initialize
    return true;
}

void World::update(Renderer& renderer, float deltaSeconds)
{
    ProfileScope updateScope("World update", EProfileCategory::Entity);
    m_updateLevel.clear();
    for (const EntityPtr& root : m_rootEntities)
        m_updateLevel.push_back({ root.get(), Transform() });

    while (!m_updateLevel.empty())
    {
        Globals::jobSystem.parallelFor(0, uint32(m_updateLevel.size()), m_updateCost,
            [this, &renderer, deltaSeconds](uint32 begin, uint32 end)
            {
                // Per-chunk (not per-entity: thousands of ~us records would flood the rings); safe
                // because updateSelf never fiber-waits - everything it touches is the audited
                // thread-safe inline set.
                ProfileScope profileScope("Entity Update", EProfileCategory::Entity);
                EntityUpdateStaging& staging = m_updateStaging.local();
                for (uint32 i = begin; i < end; ++i)
                {
                    const EntityUpdateNode& node = m_updateLevel[i];
                    node.entity->updateSelf(renderer, deltaSeconds, node.parentWorld, staging.children);
                }
            });

        {
            ProfileScope profileScope("Level merge", EProfileCategory::Entity);
            m_updateLevel.clear();
            m_updateStaging.forEach([this](EntityUpdateStaging& staging)
                {
                    m_updateLevel.insert(m_updateLevel.end(), staging.children.begin(), staging.children.end());
                    staging.children.clear();
                });
        }
    }
}

static RendererVKLayout::EPipelineIndex parsePipeline(const oc::string& name)
{
    using P = RendererVKLayout::EPipelineIndex;
    if (name == "LitTransparent")   return P::LitTransparent;
    if (name == "UnlitOpaque")      return P::UnlitOpaque;
    if (name == "UnlitTransparent") return P::UnlitTransparent;
    if (name == "Sky")              return P::Sky;
    if (name == "WireframeTransparent") return P::WireframeTransparent;
    if (name == "GizmoUI")          return P::GizmoUI;
    if (name == "GizmoWorld")       return P::GizmoWorld;
    return P::LitOpaque;
}

// Artist-authored collision proxies: "Col_Wall" collides in place of "Wall" and is never rendered
// (the renderer applies the same prefix rule in ObjectContainer::initializeNodes).
// Cook options for the scene cache: the renderer's LOD generation params get baked into the cooked
// file (and its options hash), so changing the LOD tweaks re-cooks affected scenes on the next load.
static SceneCookOptions makeSceneCookOptions()
{
    const MeshLodParams& lodParams = Globals::rendererVK.getLodParams();
    SceneCookOptions options;
    options.generateLods = lodParams.generate;
    options.lodLevels = lodParams.generateLevels;
    options.lodReduction = lodParams.generateReduction;
    options.lodMinIndices = lodParams.minIndices;
    return options;
}

static oc::unique_ptr<ISceneData> loadSceneData(const ObjectContainerDesc& desc)
{
    if (!desc.procedural)
    {
        SceneCookOptions options = makeSceneCookOptions();
        options.decimationFactor = desc.decimationFactor;
        return ISceneData::loadCached(desc.path.c_str(), desc.mergeNodes, desc.preTransformVertices, options);
    }
    oc::unique_ptr<ISceneData> sceneData = ISceneData::createProceduralLoader();
    if (!sceneData->initialize(desc.path.c_str(), desc.mergeNodes, desc.preTransformVertices))
        sceneData.reset();
    return sceneData;
}

ObjectContainer* World::loadContainer(const ObjectContainerDesc& desc, bool captureCollisionSource)
{
    if (auto it = m_containers.find(desc.name); it != m_containers.end())
        return it->second.get();

    oc::unique_ptr<ISceneData> sceneData = loadSceneData(desc);
    if (!sceneData)
    {
        Log::warning("Scene: failed to load '" + desc.path + "' for ObjectContainer '" + desc.name + "'");
        return nullptr;
    }

    if (captureCollisionSource)
        m_collision.captureSource(desc.name, *sceneData);

    auto container = oc::make_unique<ObjectContainer>();
    if (desc.materialOverrides.present)
    {
        const MaterialOverridesDesc& mo = desc.materialOverrides;
        ObjectContainer::MaterialOverrides overrides;
        if (!mo.pipeline.empty())
            overrides.pipelineIdx = parsePipeline(mo.pipeline);
        overrides.excludeFromRayTracing = mo.excludeFromRayTracing;
        overrides.useSceneTextures = mo.useSceneTextures;
        if (mo.diffuseTexIdx >= 0)         overrides.diffuseTexIdx = uint16(mo.diffuseTexIdx);
        if (mo.normalTexIdx >= 0)          overrides.normalTexIdx = uint16(mo.normalTexIdx);
        if (mo.metalRoughnessTexIdx >= 0)  overrides.metalRoughnessTexIdx = uint16(mo.metalRoughnessTexIdx);
        container->initialize(*sceneData, &overrides);
    }
    else
    {
        container->initialize(*sceneData);
    }
    ObjectContainer* ptr = container.get();
    m_containers.emplace(desc.name, oc::move(container));
    return ptr;
}

ObjectContainer* World::getOrLoadContainer(const oc::string& name, bool captureCollisionSource)
{
    if (auto it = m_containers.find(name); it != m_containers.end())
        return it->second.get(); // already loaded; a late collision request falls back to ensureCollisionSource
    if (const ObjectContainerDesc* desc = Globals::assetRegistry.findObjectContainer(name))
        return loadContainer(*desc, captureCollisionSource);
    Log::warning("Scene: unknown ObjectContainer reference '" + name + "'");
    return nullptr;
}

EntityPtr World::spawn(const oc::string& name, const Transform& base)
{
    if (oc::shared_ptr<const EntitySpawnTemplate> tmpl = getOrBuildPrefabTemplate(name))
        return Entity::create(*tmpl, base);
    return EntityPtr{};
}

void World::reloadPrefabs()
{
    for (auto& [name, tmpl] : m_templates)
        m_retiredTemplates.push_back(oc::move(tmpl));
    m_templates.clear();
}

void World::invalidatePrefab(const oc::string& name)
{
    if (auto it = m_templates.find(name); it != m_templates.end())
    {
        m_retiredTemplates.push_back(oc::move(it->second)); // kept alive for live entities
        m_templates.erase(it);
    }
}

static const AssetNode* findComponentNode(const AssetNode& node, const char* name)
{
    for (const AssetNode* comp : node.findAll("Component"))
        if (comp->asString() == name)
            return comp;
    return nullptr;
}

static bool keyIs(const AssetNode& node, oc::string_view key)
{
    if (node.key.size() != key.size())
        return false;
    for (size_t i = 0; i < key.size(); ++i)
    {
        const char a = node.key[i] | 0x20, b = key[i] | 0x20; // ASCII lower
        if (a != b)
            return false;
    }
    return true;
}

static Transform readNodeTransform(const AssetNode& node)
{
    Transform t;
    t.scale = 1.0f;
    if (const AssetNode* n = node.find("Position")) t.pos = n->asVec3();
    if (const AssetNode* n = node.find("Rotation")) t.quat = glm::quat(glm::radians(n->asVec3()));
    if (const AssetNode* n = node.find("Scale"))    t.scale = n->asFloat(0, 1.0f);
    return t;
}

oc::shared_ptr<RenderComponent::SpawnInfo> World::buildRenderSpawnInfo(const AssetNode& renderNode, const oc::string& ownerName, bool captureCollisionSource)
{
    const AssetNode* containerNode = renderNode.find("ObjectContainer");
    if (!containerNode)
        return nullptr;
    const oc::string containerName = containerNode->asString();
    const oc::string nodePath = renderNode.find("Node") ? renderNode.find("Node")->asString() : oc::string();

    ObjectContainer* container = getOrLoadContainer(containerName, captureCollisionSource);
    if (!container)
        return nullptr;

    // Type defaults from the container itself (skinned iff its source scene had a skeleton) but can be
    // overridden explicitly — `Type StaticMesh` / `Type SkinnedMesh`, with an optional nested `Rig` token.
    bool skinned = container->isSkinned();
    oc::string rigType;
    if (const AssetNode* typeNode = renderNode.find("Type"))
    {
        skinned = typeNode->asString() == "SkinnedMesh";
        if (const AssetNode* rigNode = typeNode->find("Rig"))
            rigType = rigNode->asString();
    }

    auto info = oc::make_shared<RenderComponent::SpawnInfo>();
    info->container = container;
    info->containerName = containerName; // kept so an inline entity re-serializes its mesh
    info->skinned = skinned;
    info->rigType = rigType;

    if (nodePath.empty() || nodePath == "ROOT")
        info->nodeIdx = NodeSpawnIdx_ROOT;
    else if (NodeSpawnIdx idx = container->getSpawnIdxForPath(nodePath); idx != NodeSpawnIdx_INVALID)
        info->nodeIdx = idx;
    else
        Log::warning("Scene: entity '" + ownerName + "' references unknown node '" + nodePath + "', using ROOT");
    info->nodePath = nodePath.empty() ? "ROOT" : nodePath;
    info->localTransform = readNodeTransform(renderNode); // mesh offset within the entity
    if (const AssetNode* colorNode = renderNode.find("Color"))
        info->color = glm::clamp(colorNode->asVec3(), 0.0f, 1.0f); // solid tint (see SpawnInfo)
    return info;
}

void writeRenderSpawnInfo(const RenderComponent::SpawnInfo& info, AssetNode& out)
{
    if (!info.container)
        return;
    out.set("ObjectContainer", info.containerName);
    out.set("Node", info.nodePath);
    AssetNode& typeNode = out.addChild("Type");
    typeNode.values.emplace_back(info.skinned ? "SkinnedMesh" : "StaticMesh");
    if (!info.rigType.empty())
        typeNode.addChild("Rig").values.emplace_back(info.rigType);

    const Transform& lt = info.localTransform;
    if (lt.pos != glm::vec3(0.0f))         out.set("Position", lt.pos);
    if (lt.quat != glm::quat(1, 0, 0, 0))  out.set("Rotation", glm::degrees(glm::eulerAngles(lt.quat)));
    if (lt.scale != 1.0f)                  out.set("Scale", lt.scale);
    if (info.color.x >= 0.0f)              out.set("Color", info.color);
}

const AnimationSet* World::getOrBuildClipSet(const Skeleton* skel, const AnimatorDesc& desc)
{
    const oc::string key = oc::to_string(reinterpret_cast<uintptr_t>(skel)) + "/" + desc.name;
    if (auto it = m_clipSets.find(key); it != m_clipSets.end())
        return it->second.get();

    auto set = oc::make_unique<AnimationSet>();
    AnimationSet& clips = *set;

    // Apply the .anm's loop flag + event notifies onto the clip just loaded under `localName`.
    auto applyClipMeta = [&](const AnimationClipDesc& anm, const oc::string& localName)
    {
        const auto idxIt = clips.nameToIndex.find(localName);
        if (idxIt == clips.nameToIndex.end())
            return;
        AnimationClip& clip = clips.clips[idxIt->second];
        clip.loop = anm.loop;
        clip.events.clear();
        for (const auto& [name, t] : anm.events)
            clip.events.push_back({ name, t });
    };

    // Loads one .anm into the set under `localName` (retargeted by bone name to `skel`).
    auto loadClipDesc = [&](const AnimationClipDesc& anm, const oc::string& localName)
    {
        oc::string sourcePath = anm.source;
        if (const ObjectContainerDesc* oc = Globals::assetRegistry.findObjectContainer(anm.source))
            sourcePath = oc->path; // source named a registered container; use its file
        const char* skip = anm.skip.empty() ? nullptr : anm.skip.c_str();
        const char* track = anm.track.empty() ? nullptr : anm.track.c_str();
        ISceneData::loadAnimations(sourcePath.c_str(), *skel, clips, skip, localName.c_str(), track);
        applyClipMeta(anm, localName);
    };

    // Explicit `Clip <local> Anim <anm>` declarations are optional: they alias a clip to a local name (or
    // force-load one nothing references). Anything not declared is lazy-loaded by name below.
    for (const AnimatorDesc::ClipRef& ref : desc.clips)
    {
        if (const AnimationClipDesc* anm = Globals::assetRegistry.findClip(ref.anmName))
            loadClipDesc(*anm, ref.localName);
        else
            Log::warning("Animator '" + desc.name + "': unknown Animation clip '" + ref.anmName + "'");
    }

    // Resolve a clip by name, lazily loading it from the .anm registry when it wasn't declared with `Clip`.
    auto ensureClip = [&](const oc::string& name)
    {
        if (name.empty() || clips.find(name))
            return;
        if (const AnimationClipDesc* anm = Globals::assetRegistry.findClip(name))
            loadClipDesc(*anm, name);
        else
            Log::warning("Animator '" + desc.name + "': unknown clip '" + name + "'");
    };

    oc::unordered_set<oc::string> blendNames;
    for (const AnimatorDesc::BlendSpace& bs : desc.blendSpaces)
        blendNames.insert(bs.name);
    for (const AnimatorDesc::BlendSpace& bs : desc.blendSpaces)
        for (const AnimatorDesc::BlendSample& s : bs.samples)
            ensureClip(s.clip);
    for (const AnimatorDesc::State& st : desc.stateMachine.states)
        if (!oc::contains(blendNames, st.play))
            ensureClip(st.play);

    Log::info("Animator '" + desc.name + "': built clip set (" + oc::to_string(clips.numClips()) + " clips)");

    const AnimationSet* ptr = set.get();
    m_clipSets.emplace(key, oc::move(set));
    return ptr;
}

oc::shared_ptr<AnimatorComponent::SpawnInfo> World::buildAnimatorSpawnInfo(const AssetNode& animatorNode, const oc::string& siblingContainerName, const oc::string& ownerName)
{
    const AssetNode* nameNode = animatorNode.find("Animator");
    if (!nameNode)
        return nullptr;
    const oc::string animatorName = nameNode->asString();

    const AnimatorDesc* desc = Globals::assetRegistry.findAnimator(animatorName);
    if (!desc)
    {
        Log::warning("Scene: entity '" + ownerName + "' references unknown Animator '" + animatorName + "'");
        return nullptr;
    }
    ObjectContainer* siblingContainer = siblingContainerName.empty() ? nullptr : getOrLoadContainer(siblingContainerName);
    if (!siblingContainer || !siblingContainer->isSkinned() || !siblingContainer->getSkeleton())
    {
        Log::warning("Scene: entity '" + ownerName + "' has an Animator but no sibling skinned mesh to drive");
        return nullptr;
    }

    auto info = oc::make_shared<AnimatorComponent::SpawnInfo>();
    info->desc = desc;
    info->skeleton = siblingContainer->getSkeleton();
    info->clipSet = getOrBuildClipSet(info->skeleton, *desc); // shared, imported once per skeleton+animator
    info->animatorName = animatorName;
    if (const AssetNode* n = animatorNode.find("Enabled"))
        info->enabled = n->asBool();
    return info;
}

oc::shared_ptr<SceneComponent::SpawnInfo> World::buildSceneSpawnInfo(const AssetNode& sceneNode)
{
    auto info = oc::make_shared<SceneComponent::SpawnInfo>();

    for (const AssetNode& child : sceneNode.children)
    {
        oc::shared_ptr<const EntitySpawnTemplate> childTmpl;
        if (keyIs(child, "Entity"))
            childTmpl = buildInlineTemplate(child);
        else if (keyIs(child, "Prefab"))
            childTmpl = getOrBuildPrefabTemplate(child.asString());
        else
            continue;
        if (!childTmpl)
            continue;

        SceneComponent::SpawnInfo::ChildSpawnInfo ci;
        ci.tmpl = oc::move(childTmpl);
        ci.localTransform = readNodeTransform(child);
        if (const AssetNode* n = child.find("Name")) ci.name = n->asString();
        if (const AssetNode* n = child.find("Enabled")) ci.enabled = n->asBool();
        info->children.push_back(oc::move(ci));
    }
    return info;
}

// The cache normally gets its snapshot for free while the render container loads. This is the fallback
// for the other order: the container was already loaded (or isn't loaded at all) when a Hull/Mesh shape
// asked for geometry, so the source file is imported once more and handed to the cache.
bool World::ensureCollisionSource(const oc::string& containerName)
{
    if (m_collision.hasSource(containerName))
        return true;

    const ObjectContainerDesc* desc = Globals::assetRegistry.findObjectContainer(containerName);
    if (!desc)
    {
        Log::warning("Physics: unknown ObjectContainer '" + containerName + "' for collision geometry");
        return false;
    }
    oc::unique_ptr<ISceneData> sceneData = loadSceneData(*desc);
    if (!sceneData)
    {
        Log::warning("Physics: failed to load '" + desc->path + "' for collision geometry");
        return false;
    }
    Log::info("Physics: container '" + containerName + "' re-imported for collision geometry (loaded before physics needed it)");

    m_collision.captureSource(containerName, *sceneData);
    return true;
}

oc::shared_ptr<PhysicsComponent::SpawnInfo> World::buildPhysicsSpawnInfo(const AssetNode& physicsNode,
    const oc::string& containerName, const oc::string& nodePath, const oc::string& ownerName)
{
    auto info = oc::make_shared<PhysicsComponent::SpawnInfo>();
    if (const AssetNode* n = physicsNode.find("Body"))
    {
        const oc::string& type = n->asString();
        if (type == "Static")         info->bodyType = EPhysicsBodyType::Static;
        else if (type == "Kinematic") info->bodyType = EPhysicsBodyType::Kinematic;
        else                          info->bodyType = EPhysicsBodyType::Dynamic;
    }
    if (const AssetNode* n = physicsNode.find("Shape"))
    {
        const oc::string& type = n->asString();
        if (type == "Sphere")       info->shape.type = EPhysicsShapeType::Sphere;
        else if (type == "Capsule") info->shape.type = EPhysicsShapeType::Capsule;
        else if (type == "Hull")    info->shape.type = EPhysicsShapeType::Hull;
        else if (type == "Mesh")    info->shape.type = EPhysicsShapeType::Mesh;
        else                        info->shape.type = EPhysicsShapeType::Box;
    }
    PhysicsShape& shape = info->shape;

    // Hull/Mesh pull their geometry from the sibling render mesh's container.
    if (shape.type == EPhysicsShapeType::Hull)
    {
        if (!containerName.empty() && ensureCollisionSource(containerName))
            shape.hullPoints = m_collision.buildHullPoints(containerName, nodePath);
        if (shape.hullPoints.size() < 4)
        {
            Log::warning("Scene: entity '" + ownerName + "' has a Hull physics shape but no render geometry, using Box");
            shape.type = EPhysicsShapeType::Box;
        }
    }
    else if (shape.type == EPhysicsShapeType::Mesh)
    {
        if (!containerName.empty() && ensureCollisionSource(containerName))
            info->mesh = m_collision.getOrBuildMesh(containerName, nodePath);
        if (info->mesh)
        {
            shape.mesh = info->mesh.get();
            info->occluders = m_collision.getOccluders(containerName, nodePath); // static mesh colliders double as occlusion occluders
        }
        else
        {
            Log::warning("Scene: entity '" + ownerName + "' has a Mesh physics shape but no render geometry, using Box");
            shape.type = EPhysicsShapeType::Box;
        }
    }
    if (const AssetNode* n = physicsNode.find("HalfExtents")) shape.halfExtents = n->asVec3(shape.halfExtents);
    if (const AssetNode* n = physicsNode.find("Radius"))      shape.radius = n->asFloat(0, shape.radius);
    if (const AssetNode* n = physicsNode.find("HalfHeight"))  shape.halfHeight = n->asFloat(0, shape.halfHeight);
    if (const AssetNode* n = physicsNode.find("Offset"))      shape.offset = n->asVec3(shape.offset);
    if (const AssetNode* n = physicsNode.find("Density"))     shape.density = n->asFloat(0, shape.density);
    if (const AssetNode* n = physicsNode.find("Friction"))    shape.friction = n->asFloat(0, shape.friction);
    if (const AssetNode* n = physicsNode.find("Restitution")) shape.restitution = n->asFloat(0, shape.restitution);
    if (const AssetNode* n = physicsNode.find("Layer"))
    {
        info->layer = n->asString();
        shape.categoryBits = PhysicsLayers::bit(info->layer);
    }
    if (const AssetNode* n = physicsNode.find("CollidesWith"))
    {
        uint64 mask = 0;
        for (size_t i = 0; i < n->numValues(); ++i)
        {
            const oc::string& name = n->asString(i);
            if (name == "All")        mask = PhysicsLayers::All;
            else if (name != "None")  mask |= PhysicsLayers::bit(name);
            info->collidesWith.push_back(name);
        }
        shape.maskBits = mask;
    }
    if (const AssetNode* n = physicsNode.find("Group"))       shape.groupIndex = n->asInt();
    if (const AssetNode* n = physicsNode.find("MaxHullVertices")) shape.maxHullVertices = n->asInt(0, shape.maxHullVertices);
    if (const AssetNode* n = physicsNode.find("LockRotation"))  info->lockRotation = n->asBool();
    if (const AssetNode* n = physicsNode.find("Sensor"))        shape.isSensor = n->asBool();
    if (const AssetNode* n = physicsNode.find("ContactEvents")) shape.contactEvents = n->asBool();
    if (const AssetNode* n = physicsNode.find("Enabled"))     info->enabled = n->asBool();
    return info;
}

oc::shared_ptr<AudioBuffer> World::getOrLoadAudioBuffer(const oc::string& path)
{
    if (auto it = m_audioBuffers.find(path); it != m_audioBuffers.end())
        return it->second;
    auto buffer = oc::make_shared<AudioBuffer>(Globals::audio.loadSound(path));
    m_audioBuffers.emplace(path, buffer);
    return buffer;
}

oc::shared_ptr<AudioComponent::SpawnInfo> World::buildAudioSpawnInfo(const AssetNode& audioNode, const oc::string& ownerName)
{
    auto info = oc::make_shared<AudioComponent::SpawnInfo>();
    for (const AssetNode& soundNode : audioNode.children)
    {
        if (!keyIs(soundNode, "Sound"))
            continue;
        AudioComponent::SoundDesc sound;
        sound.alias = soundNode.asString();
        if (const AssetNode* n = soundNode.find("Select"))
            sound.select = audioSelectFromToken(n->asString());

        // Each `Path` child is one clip; its own child lines (Volume/Pitch/...) are the clip's settings.
        for (const AssetNode& pathNode : soundNode.children)
        {
            if (!keyIs(pathNode, "Path"))
                continue;
            AudioComponent::Clip clip;
            clip.path = pathNode.asString();
            if (clip.path.empty())
                continue;
            if (const AssetNode* n = pathNode.find("Volume"))            clip.volume = n->asFloat(0, clip.volume);
            if (const AssetNode* n = pathNode.find("Pitch"))             clip.pitch = n->asFloat(0, clip.pitch);
            if (const AssetNode* n = pathNode.find("Loop"))              clip.loop = n->asBool();
            if (const AssetNode* n = pathNode.find("Relative"))          clip.relative = n->asBool();
            if (const AssetNode* n = pathNode.find("ReferenceDistance")) clip.referenceDistance = n->asFloat(0, clip.referenceDistance);
            if (const AssetNode* n = pathNode.find("MaxDistance"))       clip.maxDistance = n->asFloat(0, clip.maxDistance);
            if (const AssetNode* n = pathNode.find("Rolloff"))           clip.rolloff = n->asFloat(0, clip.rolloff);
            clip.buffer = getOrLoadAudioBuffer(clip.path); // may be invalid (load failure logged); alias stays triggerable as a no-op
            sound.clips.push_back(oc::move(clip));
        }
        if (sound.alias.empty() || sound.clips.empty())
        {
            Log::warning("Scene: entity '" + ownerName + "' has an audio Sound entry without an alias or Path, skipping");
            continue;
        }
        info->sounds.push_back(oc::move(sound));
    }
    if (info->sounds.empty())
        return nullptr;
    return info;
}

void World::buildTemplate(const AssetNode& node, EntitySpawnTemplate& tmpl)
{
    tmpl.defaultTransform = readNodeTransform(node); // the declaration's authored placement
    const AssetNode* nameNode = node.find("Name");
    tmpl.displayName = nameNode ? nameNode->asString() : node.asString();
    if (const AssetNode* n = node.find("Enabled")) tmpl.enabled = n->asBool();

    uint16 typeBits = 0;

    static_assert(EComponentID_Scene == 0);
    if (const AssetNode* sceneNode = findComponentNode(node, "Scene"))
        if (oc::shared_ptr<SceneComponent::SpawnInfo> info = buildSceneSpawnInfo(*sceneNode))
        {
            typeBits |= uint16(1 << EComponentID_Scene);
            tmpl.spawnInfos.emplace_back(oc::move(info));
        }

    // A Hull/Mesh physics shape (parsed below) sources its geometry from the render container, so the
    // collision snapshot is captured while the container's scene data is loaded — one import, not two.
    const AssetNode* physicsNode = findComponentNode(node, "Physics");
    bool wantsCollisionGeometry = false;
    if (physicsNode)
        if (const AssetNode* n = physicsNode->find("Shape"))
            wantsCollisionGeometry = n->asString() == "Hull" || n->asString() == "Mesh";

    oc::string renderContainerName; // physics hull/mesh shapes (below), and the animator, source from here
    oc::string renderNodePath;
    if (const AssetNode* renderNode = findComponentNode(node, "Render"))
    {
        if (m_headless)
        {
            // no RenderComponent (no ObjectContainer load — that path is all renderer), but a Hull/Mesh
            // physics shape still needs to know WHERE its geometry lives: the names are enough, the
            // geometry itself comes from ensureCollisionSource's renderer-free import below
            if (const AssetNode* containerNode = renderNode->find("ObjectContainer"))
            {
                renderContainerName = containerNode->asString();
                renderNodePath = renderNode->find("Node") ? renderNode->find("Node")->asString() : oc::string();
            }
        }
        else if (oc::shared_ptr<RenderComponent::SpawnInfo> info = buildRenderSpawnInfo(*renderNode, tmpl.displayName, wantsCollisionGeometry))
        {
            renderContainerName = info->containerName;
            renderNodePath = info->nodePath;
            typeBits |= uint16(1 << EComponentID_Render);
            tmpl.spawnInfos.emplace_back(oc::move(info));
        }
    }

    if (const AssetNode* animatorNode = findComponentNode(node, "Animator"); animatorNode && !m_headless)
        if (oc::shared_ptr<AnimatorComponent::SpawnInfo> info = buildAnimatorSpawnInfo(*animatorNode, renderContainerName, tmpl.displayName))
        {
            typeBits |= uint16(1 << EComponentID_Animator);
            tmpl.spawnInfos.emplace_back(oc::move(info));
        }

    if (physicsNode) // found above, before the render container load
    {
        typeBits |= uint16(1 << EComponentID_Physics);
        tmpl.spawnInfos.emplace_back(buildPhysicsSpawnInfo(*physicsNode, renderContainerName, renderNodePath, tmpl.displayName));
    }

    // Headless drops everything below except Network/Script: Audio would load buffers on an
    // uninitialized device, Particle/Force/Light create renderer emitter slots / push to mapped
    // buffers. Scripts reaching an absent component no-op through the null-handle ABI contract.
    if (const AssetNode* audioNode = findComponentNode(node, "Audio"); audioNode && !m_headless)
        if (oc::shared_ptr<AudioComponent::SpawnInfo> info = buildAudioSpawnInfo(*audioNode, tmpl.displayName))
        {
            typeBits |= uint16(1 << EComponentID_Audio);
            tmpl.spawnInfos.emplace_back(oc::move(info));
        }

    if (const AssetNode* particleNode = findComponentNode(node, "Particle"); particleNode && !m_headless)
    {
        auto info = oc::make_shared<ParticleComponent::SpawnInfo>();
        if (const AssetNode* n = particleNode->find("Effect"))   info->effectPath = n->asString();
        if (const AssetNode* n = particleNode->find("Emitting")) info->emitting = n->asBool(0, true);
        if (!info->effectPath.empty())
        {
            typeBits |= uint16(1 << EComponentID_Particle);
            tmpl.spawnInfos.emplace_back(oc::move(info));
        }
        else
            Log::warning("Scene: entity '" + tmpl.displayName + "' has a Particle component without an Effect path, skipping");
    }

    if (const AssetNode* forceNode = findComponentNode(node, "Force"); forceNode && !m_headless)
    {
        auto info = oc::make_shared<ForceComponent::SpawnInfo>();
        if (const AssetNode* n = forceNode->find("Team"))         info->team = uint32(glm::max(n->asInt(), 0));
        if (const AssetNode* n = forceNode->find("Direction"))    info->direction = n->asVec3(info->direction);
        if (const AssetNode* n = forceNode->find("Offset"))       info->offset = n->asVec3(info->offset);
        if (const AssetNode* n = forceNode->find("Output"))       info->output = n->asFloat(0, info->output);
        if (const AssetNode* n = forceNode->find("Reach"))        info->reach = n->asFloat(0, info->reach);
        if (const AssetNode* n = forceNode->find("Focus"))        info->focus = n->asFloat(0, info->focus);
        if (const AssetNode* n = forceNode->find("Distribution")) info->distribution = n->asFloat(0, info->distribution);
        if (const AssetNode* n = forceNode->find("Width"))        info->width = n->asFloat(0, info->width);
        if (const AssetNode* n = forceNode->find("Centered"))     info->centered = n->asBool(0, true);
        typeBits |= uint16(1 << EComponentID_Force);
        tmpl.spawnInfos.emplace_back(oc::move(info));
    }

    if (const AssetNode* lightNode = findComponentNode(node, "Light"); lightNode && !m_headless)
    {
        auto info = oc::make_shared<LightComponent::SpawnInfo>();
        if (const AssetNode* n = lightNode->find("Debug")) info->debugDraw = n->asBool();
        // One "Light <Type>" child per light; a component with none is pointless, so it's skipped below.
        for (const AssetNode& entry : lightNode->children)
        {
            if (!keyIs(entry, "Light"))
                continue;
            LightComponent::LightDesc desc;
            desc.type = lightTypeFromToken(entry.asString());
            if (const AssetNode* n = entry.find("Offset"))       desc.offset = n->asVec3(desc.offset);
            if (const AssetNode* n = entry.find("Direction"))    desc.direction = n->asVec3(desc.direction);
            if (const AssetNode* n = entry.find("Color"))        desc.color = n->asVec3(desc.color);
            if (const AssetNode* n = entry.find("Intensity"))    desc.intensity = n->asFloat(0, desc.intensity);
            if (const AssetNode* n = entry.find("Range"))        desc.range = n->asFloat(0, desc.range);
            if (const AssetNode* n = entry.find("ConeAngle"))    desc.coneAngle = n->asFloat(0, desc.coneAngle);
            if (const AssetNode* n = entry.find("EdgeSoftness")) desc.edgeSoftness = n->asFloat(0, desc.edgeSoftness);
            if (const AssetNode* n = entry.find("Width"))        desc.width = n->asFloat(0, desc.width);
            if (const AssetNode* n = entry.find("Radius"))       desc.width = n->asFloat(0, desc.width); // Tube spells it Radius
            if (const AssetNode* n = entry.find("Height"))       desc.height = n->asFloat(0, desc.height);
            if (const AssetNode* n = entry.find("Length"))       desc.length = n->asFloat(0, desc.length);
            if (const AssetNode* n = entry.find("Rotation"))     desc.rotation = n->asFloat(0, desc.rotation);
            if (const AssetNode* n = entry.find("Enabled"))      desc.enabled = n->asBool(0, true);
            info->lights.push_back(desc);
        }
        if (!info->lights.empty())
        {
            typeBits |= uint16(1 << EComponentID_Light);
            tmpl.spawnInfos.emplace_back(oc::move(info));
        }
        else
            Log::warning("Scene: entity '" + tmpl.displayName + "' has a Light component without any Light entries, skipping");
    }

    if (findComponentNode(node, "Network")) // pure presence: netIds are assigned in code, never authored
    {
        typeBits |= uint16(1 << EComponentID_Network);
        tmpl.spawnInfos.emplace_back(oc::make_shared<NetworkComponent::SpawnInfo>());
    }

    // GAME components (Components/GameComponents.ixx) — !m_headless like Force: they create Force
    // queries/read readbacks, and --game refuses headless anyway.
    if (const AssetNode* unitNode = findComponentNode(node, "GameUnit"); unitNode && !m_headless)
    {
        auto info = oc::make_shared<GameUnitComponent::SpawnInfo>();
        if (const AssetNode* n = unitNode->find("Team"))          info->team = uint32(glm::max(n->asInt(), 0));
        if (const AssetNode* n = unitNode->find("Puppet"))        info->puppet = n->asBool();
        if (const AssetNode* n = unitNode->find("HealthMax"))     info->healthMax = n->asFloat(0, info->healthMax);
        if (const AssetNode* n = unitNode->find("EnergyMax"))     info->energyMax = n->asFloat(0, info->energyMax);
        if (const AssetNode* n = unitNode->find("ShieldOutput"))  info->shieldOutput = n->asFloat(0, info->shieldOutput);
        if (const AssetNode* n = unitNode->find("MoveSpeed"))     info->moveSpeed = n->asFloat(0, info->moveSpeed);
        if (const AssetNode* n = unitNode->find("Accel"))         info->accel = n->asFloat(0, info->accel);
        if (const AssetNode* n = unitNode->find("AttackRange"))   info->attackRange = n->asFloat(0, info->attackRange);
        if (const AssetNode* n = unitNode->find("AttackDps"))     info->attackDps = n->asFloat(0, info->attackDps);
        if (const AssetNode* n = unitNode->find("PlayerDps"))     info->playerDps = n->asFloat(0, info->playerDps);
        if (const AssetNode* n = unitNode->find("EmitterDrain"))  info->emitterDrain = n->asFloat(0, info->emitterDrain);
        if (const AssetNode* n = unitNode->find("Ranged"))        info->ranged = n->asBool();
        if (const AssetNode* n = unitNode->find("StandoffRange")) info->standoffRange = n->asFloat(0, info->standoffRange);
        if (const AssetNode* n = unitNode->find("FireInterval"))  info->fireInterval = n->asFloat(0, info->fireInterval);
        typeBits |= uint16(1 << EComponentID_GameUnit);
        tmpl.spawnInfos.emplace_back(oc::move(info));
    }
    if (const AssetNode* structNode = findComponentNode(node, "GameStructure"); structNode && !m_headless)
    {
        auto info = oc::make_shared<GameStructureComponent::SpawnInfo>();
        if (const AssetNode* n = structNode->find("Team"))         info->team = uint32(glm::max(n->asInt(), 0));
        if (const AssetNode* n = structNode->find("HealthMax"))    info->healthMax = n->asFloat(0, info->healthMax);
        if (const AssetNode* n = structNode->find("Invulnerable")) info->invulnerable = n->asBool();
        if (const AssetNode* n = structNode->find("MeleeRadius"))  info->meleeRadius = n->asFloat(0, info->meleeRadius);
        typeBits |= uint16(1 << EComponentID_GameStructure);
        tmpl.spawnInfos.emplace_back(oc::move(info));
    }
    if (const AssetNode* projNode = findComponentNode(node, "GameProjectile"); projNode && !m_headless)
    {
        auto info = oc::make_shared<GameProjectileComponent::SpawnInfo>();
        if (const AssetNode* n = projNode->find("Team"))            info->team = uint32(glm::max(n->asInt(), 0));
        if (const AssetNode* n = projNode->find("UnitDamage"))      info->unitDamage = n->asFloat(0, info->unitDamage);
        if (const AssetNode* n = projNode->find("StructureDamage")) info->structureDamage = n->asFloat(0, info->structureDamage);
        if (const AssetNode* n = projNode->find("Lifetime"))        info->lifetime = n->asFloat(0, info->lifetime);
        if (const AssetNode* n = projNode->find("EmitterDrain"))    info->emitterDrain = n->asFloat(0, info->emitterDrain);
        if (const AssetNode* n = projNode->find("EmitterDrainRadius")) info->emitterDrainRadius = n->asFloat(0, info->emitterDrainRadius);
        typeBits |= uint16(1 << EComponentID_GameProjectile);
        tmpl.spawnInfos.emplace_back(oc::move(info));
    }

    if (const AssetNode* scriptNode = findComponentNode(node, "Script"))
    {
        auto info = oc::make_shared<ScriptComponent::SpawnInfo>();
        if (const AssetNode* n = scriptNode->find("Path"))    info->scriptPath = n->asString();
        if (const AssetNode* n = scriptNode->find("Enabled")) info->enabled = n->asBool();
        // Authored INITIAL values for the script's exposed fields, one "<name> <value>" child each. Kept as
        // TEXT: the script hasn't compiled at this point, so nothing here knows what type any of them are --
        // ScriptComponent::applyInitialValues resolves them against the module's field table once it has.
        if (const AssetNode* dataNode = scriptNode->find("Data"))
            for (const AssetNode& fieldNode : dataNode->children)
            {
                // Re-joined with ", ": the parser splits a vector value into separate entries, and the text form
                // is what applyInitialValues parses back -- the same spelling the writer produces.
                oc::string text;
                for (const oc::string& value : fieldNode.values)
                    text += (text.empty() ? "" : ", ") + value;
                info->initialValues.push_back({ fieldNode.key, oc::move(text) });
            }
        typeBits |= uint16(1 << EComponentID_Script);
        tmpl.spawnInfos.emplace_back(oc::move(info));
    }

    tmpl.archetype = makeEntityArchetype(typeBits);
}

oc::shared_ptr<const EntitySpawnTemplate> World::cacheTemplate(const oc::string& name, const oc::string& sourceFile, const AssetNode& node)
{
    if (auto it = m_templates.find(name); it != m_templates.end())
        return it->second;

    m_buildingTemplates.insert(name);
    auto tmpl = oc::make_shared<EntitySpawnTemplate>();
    tmpl->sourceFile = sourceFile;
    tmpl->prefabName = name; // a registered prefab name, so a re-serialized instance writes "Prefab <name>"
    buildTemplate(node, *tmpl);
    m_buildingTemplates.erase(name);

    m_templates.emplace(name, tmpl); // heap-owned: address stays stable for child back-pointers
    return tmpl;
}

oc::shared_ptr<const EntitySpawnTemplate> World::buildInlineTemplate(const AssetNode& node)
{
    auto tmpl = oc::make_shared<EntitySpawnTemplate>();
    buildTemplate(node, *tmpl);
    return tmpl;
}

oc::shared_ptr<const EntitySpawnTemplate> World::buildFileTemplate(const oc::string& path)
{
    AssetNode doc;
    oc::string error;
    if (!loadAssetFile(path, doc, error))
    {
        Log::warning("Scene: asset load failed: " + error);
        return nullptr;
    }

    for (const AssetNode& decl : doc.children)
        if (keyIs(decl, "Prefab"))
            return cacheTemplate(decl.asString(), path, decl);

    Log::warning("Scene: asset '" + path + "' declared no prefab");
    return nullptr;
}

oc::shared_ptr<const EntitySpawnTemplate> World::getOrBuildPrefabTemplate(const oc::string& name)
{
    if (auto it = m_templates.find(name); it != m_templates.end())
        return it->second; // cache hit: no asset file touched

    if (oc::contains(m_buildingTemplates, name))
    {
        Log::warning("Scene: prefab cycle detected at '" + name + "', skipping");
        return nullptr;
    }

    if (const oc::string* prefabPath = Globals::assetRegistry.findPrefab(name))
    {
        if (oc::shared_ptr<const EntitySpawnTemplate> tmpl = buildFileTemplate(*prefabPath))
            return tmpl; // its declared root name == `name`
        Log::warning("Scene: prefab '" + name + "' not declared in '" + *prefabPath + "', skipping");
        return nullptr;
    }

    Log::warning("Scene: references unknown prefab '" + name + "', skipping");
    return nullptr;
}

EntityPtr World::spawnAssetFile(const oc::string& path, const Transform& base, bool overrideDefaultTransform)
{
    // Spawning from a path resolves it against the working directory (Assets/); spawns happen on
    // the main thread and on jobs alike, so the query is allowed either way.
    const oc::string relativePath = FileSystem::relativePath(path, oc::string(), /*allowMainThread*/ true);
    const oc::string fileName = relativePath.empty() ? path : relativePath;

    const oc::string* rootName = Globals::assetRegistry.findRootForFile(fileName);
    oc::shared_ptr<const EntitySpawnTemplate> tmpl = rootName ? getOrBuildPrefabTemplate(*rootName) : buildFileTemplate(fileName);
    if (!tmpl)
        return EntityPtr{};

    const Transform& dt = tmpl->defaultTransform;
    // Override replaces the POSITION and COMPOSES the caller's rotation onto the authored default
    // (identity callers keep the authored rotation exactly). The rotation used to be dropped
    // outright — aimed Lances and replicated spawns silently spawned with the prefab default.
    const glm::vec3 pos = overrideDefaultTransform ? base.pos : dt.pos;
    const glm::quat quat = overrideDefaultTransform ? base.quat * dt.quat : dt.quat;
    return Entity::create(*tmpl, Transform(pos, dt.scale, quat));
}

EntityPtr World::createEmptyEntity(const oc::string& name)
{
    // A blank Scene-only template with no prefabName: Entity::create leaves prefabInstance false, so the
    // entity is editable and serializes inline. Cached (and kept across reloadPrefabs) so its address
    // stays stable for the entities that point at it.
    if (!m_emptyTemplate)
    {
        m_emptyTemplate = oc::make_shared<EntitySpawnTemplate>();
        m_emptyTemplate->archetype = makeEntityArchetype(0);
        m_emptyTemplate->displayName = "Entity";
    }
    EntityPtr entity = Entity::create(*m_emptyTemplate, Transform());
    entity->setName(name);
    return entity;
}

void World::handleEntityChange(EntityChange& change, const Camera& camera, const Rect& viewportRect)
{
    ProfileScope profileScope("Entity change", EProfileCategory::Entity);
    if (auto* cv = oc::get_if<EntityChange::CreateViewport>(&change.type))
    {
        const glm::vec3 worldPos = camera.screenToWorld(viewportRect, cv->screenPos);
        addRootEntity(spawnAssetFile(cv->path, Transform(worldPos, 1.0f, glm::quat(1.0f, 0.0f, 0.0f, 0.0f))));
    }
    else if (auto* ch = oc::get_if<EntityChange::CreateHierarchy>(&change.type))
    {
        EntityPtr e = spawnAssetFile(ch->path, Transform(), false);
        if (ch->parent && hasComponent<SceneComponent>(ch->parent))
            e->reparentEntity(ch->parent);   // parent's SceneComponent takes ownership
        else
            addRootEntity(oc::move(e));
    }
    else if (auto* sap = oc::get_if<EntityChange::SpawnAtPosition>(&change.type))
        addRootEntity(spawnAssetFile(sap->path, Transform(sap->position, 1.0f, glm::quat(1.0f, 0.0f, 0.0f, 0.0f))));
    else if (auto* as = oc::get_if<EntityChange::AddSceneEntity>(&change.type))
    {
        EntityPtr e = createEmptyEntity(as->displayName);
        if (as->parent && hasComponent<SceneComponent>(as->parent))
            e->reparentEntity(as->parent);
        else
            addRootEntity(oc::move(e));
    }
    else if (auto* del = oc::get_if<EntityChange::Delete>(&change.type))
        removeRootEntity(del->entity.get());
    else if (auto* rep = oc::get_if<EntityChange::Reparent>(&change.type))
        rep->newParent ? removeRootEntity(rep->entity.get()) : addRootEntity(oc::move(rep->entity));
    else if (auto* sp = oc::get_if<EntityChange::SavePrefab>(&change.type))
    {
        if (savePrefab(sp->root.get(), sp->path, sp->text))
            invalidatePrefab(FileSystem::stem(sp->path));
    }
    else if (auto* op = oc::get_if<EntityChange::OpenPrefabForEdit>(&change.type))
    {
        EntityPtr e = spawnAssetFile(op->path, Transform(), false);
        if (e)
        {
            e->setPrefabInstance(false); // unpack: the Entity Editor edits it freely
            addRootEntity(e);
            if (m_onPrefabOpened)
                m_onPrefabOpened(e, op->path);
        }
    }
    else if (auto* np = oc::get_if<EntityChange::NewPrefab>(&change.type))
    {
        EntityPtr e = createEmptyEntity(np->displayName);
        addRootEntity(e);
        if (m_onPrefabOpened)
            m_onPrefabOpened(e, "");
    }
    else if (auto* rs = oc::get_if<EntityChange::RespawnEntity>(&change.type))
    {
        // Entity::create() only stores a raw, non-owning pointer to the template — keep it alive for
        // as long as the entity might reference it (World's own template caches do the same for
        // prefabs; this one is ad-hoc, so nothing else would hold onto it).
        keepTemplateAlive(rs->tmpl);

        Transform t(rs->oldEntity->pos, rs->oldEntity->scale, rs->oldEntity->rot);
        // Pre-set the frozen flag so component spawn already sees it — the respawn attaches to its parent
        // only further down, so create() can't inherit it from there.
        const uint8 initialFlags = rs->oldEntity->isFrozen() ? uint8(EEntityFlag_Frozen) : uint8(0);
        EntityPtr newEntity = Entity::create(*rs->tmpl, t, initialFlags);
        newEntity->setPrefabInstance(rs->oldEntity->isPrefabInstance()); // keep the editor's unpacked state despite the template's prefabName

        // Preserve any existing children (the Entity Editor commits one entity's own component set at
        // a time; whatever was already parented under it stays put).
        if (SceneComponent* oldSc = getComponent<SceneComponent>(rs->oldEntity.get()))
            if (SceneComponent* newSc = getComponent<SceneComponent>(newEntity.get()))
            {
                newSc->children = oc::move(oldSc->children);
                for (EntityPtr& child : newSc->children)
                    child->parent = newEntity.get();
            }

        // Re-attach where the old entity was: a root (in m_rootEntities) or a child (in its parent's
        // SceneComponent::children) — replace it in place so siblings/order aren't disturbed.
        if (Entity* parent = rs->oldEntity->parent)
        {
            newEntity->parent = parent;
            if (SceneComponent* parentSc = getComponent<SceneComponent>(parent))
                for (EntityPtr& child : parentSc->children)
                    if (child.get() == rs->oldEntity.get())
                    {
                        child = newEntity;
                        break;
                    }
        }
        else
        {
            oc::erase_if(m_rootEntities, [&](const EntityPtr& e) { return e.get() == rs->oldEntity.get(); });
            addRootEntity(newEntity);
        }

        if (m_onEntityRespawned)
            m_onEntityRespawned(rs->oldEntity, newEntity);
    }
}
