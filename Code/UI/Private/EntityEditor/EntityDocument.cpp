module UI;

import Core;
import Core.glm;
import Core.Transform;
import Entity;
import File;
import :EntityDocument;

static void readTemplateComponents(const EntitySpawnTemplate& tmpl, EntityDocNode& node, uint64& nextId)
{
    node.global = tmpl.global;
    node.cullMode = tmpl.cullMode;
    node.enabled = tmpl.enabled;

    if (const SceneComponent::SpawnInfo* scene = getTemplateSpawnInfo<SceneComponent>(tmpl))
    {
        node.hasScene = true;
        node.children.reserve(scene->children.size());
        for (const SceneComponent::SpawnInfo::ChildSpawnInfo& child : scene->children)
        {
            if (!child.tmpl)
                continue;
            EntityDocNode childNode = makeDocNode(*child.tmpl, nextId);
            childNode.transform = child.localTransform; // the reference site's placement, not the template's default
            if (!child.name.empty())
                childNode.name = child.name;
            if (!child.enabled)
                childNode.enabled = false;
            node.children.push_back(oc::move(childNode));
        }
    }
    if (const RenderComponent::SpawnInfo* info = getTemplateSpawnInfo<RenderComponent>(tmpl))    { node.hasRender = true; node.render = *info; }
    if (const AnimatorComponent::SpawnInfo* info = getTemplateSpawnInfo<AnimatorComponent>(tmpl)) { node.hasAnimator = true; node.animator = *info; }
    if (const PhysicsComponent::SpawnInfo* info = getTemplateSpawnInfo<PhysicsComponent>(tmpl))  { node.hasPhysics = true; node.physics = *info; }
    if (const AudioComponent::SpawnInfo* info = getTemplateSpawnInfo<AudioComponent>(tmpl))      { node.hasAudio = true; node.audio = *info; }
    if (const ParticleComponent::SpawnInfo* info = getTemplateSpawnInfo<ParticleComponent>(tmpl)) { node.hasParticle = true; node.particle = *info; }
    if (const ForceComponent::SpawnInfo* info = getTemplateSpawnInfo<ForceComponent>(tmpl))      { node.hasForce = true; node.force = *info; }
    if (const LightComponent::SpawnInfo* info = getTemplateSpawnInfo<LightComponent>(tmpl))      { node.hasLight = true; node.light = *info; }
    if (getTemplateSpawnInfo<NetworkComponent>(tmpl))                                            node.hasNetwork = true;
    if (const GameUnitComponent::SpawnInfo* info = getTemplateSpawnInfo<GameUnitComponent>(tmpl)) { node.hasGameUnit = true; node.gameUnit = *info; }
    if (const GameStructureComponent::SpawnInfo* info = getTemplateSpawnInfo<GameStructureComponent>(tmpl)) { node.hasGameStructure = true; node.gameStructure = *info; }
    if (const GameProjectileComponent::SpawnInfo* info = getTemplateSpawnInfo<GameProjectileComponent>(tmpl)) { node.hasGameProjectile = true; node.gameProjectile = *info; }
    if (const HumanoidAnimatorComponent::SpawnInfo* info = getTemplateSpawnInfo<HumanoidAnimatorComponent>(tmpl))
        node.humanoid = oc::make_shared<HumanoidAnimatorComponent::SpawnInfo>(*info);
    if (const ScriptComponent::SpawnInfo* info = getTemplateSpawnInfo<ScriptComponent>(tmpl))    { node.hasScript = true; node.script = *info; }
}

EntityDocNode makeDocNode(const EntitySpawnTemplate& tmpl, uint64& nextId)
{
    EntityDocNode node;
    node.id = nextId++;
    node.name = tmpl.displayName;
    node.transform = tmpl.defaultTransform;
    // A template with a registered prefab name came from a `Prefab <name>` line; an inline `Entity` has none.
    if (!tmpl.prefabName.empty() && Globals::assetRegistry.findPrefab(tmpl.prefabName))
        node.prefabRef = tmpl.prefabName;
    readTemplateComponents(tmpl, node, nextId);
    return node;
}

// ---- Serialize: the same text Prefab.cpp writes for a live entity ----

static void writeOverrides(const EntityDocNode& node, AssetNode& out, const oc::string& tokenName)
{
    if (!node.name.empty() && node.name != tokenName)
        out.set("Name", node.name);
    if (!node.enabled)
        out.set("Enabled", false); // enabled is the default, so only the disabled state is authored
    if (node.global)
        out.set("Global", true);
    if (node.cullMode == EEntityCullMode::RootOnly)
        out.set("CullMode", "RootOnly");
    else if (node.cullMode == EEntityCullMode::None)
        out.set("CullMode", "None");
    out.set("Position", node.transform.pos);
    out.set("Rotation", glm::degrees(glm::eulerAngles(node.transform.quat)));
    out.set("Scale", node.transform.scale);
}

static void writeBody(const EntityDocNode& node, AssetNode& out, const oc::string& tokenName);

static void writeChild(const EntityDocNode& child, AssetNode& scene)
{
    if (child.isReference())
    {
        AssetNode& out = scene.addChild("Prefab");
        out.values.emplace_back(child.prefabRef);
        writeOverrides(child, out, child.prefabRef);
    }
    else
    {
        const oc::string token = child.name.empty() ? oc::string("Entity") : child.name;
        AssetNode& out = scene.addChild("Entity");
        out.values.emplace_back(token);
        writeBody(child, out, token);
    }
}

static AssetNode& addComponent(AssetNode& out, const char* typeName)
{
    AssetNode& component = out.addChild("Component");
    component.values.emplace_back(typeName);
    return component;
}

// A component that wrote nothing is dropped, as in Prefab.cpp: an empty one means nothing at the next load. Scene and Network are pure presence.
static void dropIfEmpty(AssetNode& out)
{
    if (out.children.back().children.empty())
        out.children.pop_back();
}

static void writeBody(const EntityDocNode& node, AssetNode& out, const oc::string& tokenName)
{
    writeOverrides(node, out, tokenName);

    // In component bit order, like every other writer.
    if (node.hasScene)
    {
        AssetNode& scene = addComponent(out, "Scene");
        for (const EntityDocNode& child : node.children)
            writeChild(child, scene);
    }
    if (node.hasRender)    { writeRenderSpawnInfo(node.render, addComponent(out, "Render")); dropIfEmpty(out); }
    if (node.hasAnimator)  { writeAnimatorSpawnInfo(node.animator, addComponent(out, "Animator")); dropIfEmpty(out); }
    if (node.hasPhysics)   { writePhysicsSpawnInfo(node.physics, addComponent(out, "Physics")); dropIfEmpty(out); }
    if (node.hasAudio)     { writeAudioSpawnInfo(node.audio, addComponent(out, "Audio")); dropIfEmpty(out); }
    if (node.hasParticle)  { writeParticleSpawnInfo(node.particle, addComponent(out, "Particle")); dropIfEmpty(out); }
    if (node.hasForce)     { writeForceSpawnInfo(node.force, addComponent(out, "Force")); dropIfEmpty(out); }
    if (node.hasLight)     { writeLightSpawnInfo(node.light, addComponent(out, "Light")); dropIfEmpty(out); }
    if (node.hasNetwork)   addComponent(out, "Network");
    if (node.hasGameUnit)  { writeGameUnitSpawnInfo(node.gameUnit, addComponent(out, "GameUnit")); dropIfEmpty(out); }
    if (node.hasGameStructure)  { writeGameStructureSpawnInfo(node.gameStructure, addComponent(out, "GameStructure")); dropIfEmpty(out); }
    if (node.hasGameProjectile) { writeGameProjectileSpawnInfo(node.gameProjectile, addComponent(out, "GameProjectile")); dropIfEmpty(out); }
    if (node.humanoid)     { writeHumanoidAnimatorSpawnInfo(*node.humanoid, addComponent(out, "HumanoidAnimator")); dropIfEmpty(out); }
    if (node.hasScript)    { writeScriptSpawnInfo(node.script, addComponent(out, "Script")); dropIfEmpty(out); }
}

oc::string serializeEntityDoc(const EntityDocNode& root, const oc::string& id)
{
    AssetNode doc; // synthetic root; its children are the top-level declarations
    AssetNode& out = doc.addChild("Prefab");
    out.values.emplace_back(id);
    writeBody(root, out, id);
    return writeAssetText(doc);
}

// ---- Tree helpers ----

EntityDocNode* findDocNode(EntityDocNode& root, uint64 id)
{
    if (root.id == id)
        return &root;
    for (EntityDocNode& child : root.children)
        if (EntityDocNode* found = findDocNode(child, id))
            return found;
    return nullptr;
}

EntityDocNode* findDocParent(EntityDocNode& root, uint64 id)
{
    for (EntityDocNode& child : root.children)
    {
        if (child.id == id)
            return &root;
        if (EntityDocNode* found = findDocParent(child, id))
            return found;
    }
    return nullptr;
}

bool docSubtreeContains(const EntityDocNode& root, uint64 id)
{
    if (root.id == id)
        return true;
    for (const EntityDocNode& child : root.children)
        if (docSubtreeContains(child, id))
            return true;
    return false;
}

void collectDocProblems(const EntityDocNode& node, oc::vector<oc::string>& out)
{
    if (node.isReference())
        return; // authored in its own file
    if (node.hasRender)
    {
        const oc::string& name = node.render.containerName;
        if (name.empty())
            out.push_back("Render: no container picked. It will not be saved.");
        else if (!Globals::assetRegistry.findObjectContainer(name))
            out.push_back("Render: unknown container '" + name + "'.");
        else if (ObjectContainer* container = Globals::world.findLoadedContainer(name))
        {
            const oc::string& path = node.render.nodePath;
            if (!path.empty() && path != "ROOT" && container->getSpawnIdxForPath(path) == NodeSpawnIdx_INVALID)
                out.push_back("Render: container has no node '" + path + "'; ROOT is used.");
            if (node.render.skinned && !container->isSkinned())
                out.push_back("Render: SkinnedMesh on a container with no skeleton; a static node spawns.");
        }
    }
    if (node.hasAnimator)
    {
        if (!node.hasRender || !node.render.skinned)
            out.push_back("Animator: needs a SkinnedMesh Render on the same entity.");
        if (node.animator.animatorName.empty())
            out.push_back("Animator: no graph picked. It will not be saved.");
        else if (Globals::assetRegistry.getAnimators().find(node.animator.animatorName) == Globals::assetRegistry.getAnimators().end())
            out.push_back("Animator: unknown graph '" + node.animator.animatorName + "'.");
    }
    if (node.hasPhysics)
    {
        const EPhysicsShapeType shape = node.physics.shape.type;
        if ((shape == EPhysicsShapeType::Hull || shape == EPhysicsShapeType::Mesh) && !node.hasRender)
            out.push_back("Physics: Hull and Mesh take their geometry from a Render on the same entity; a Box spawns.");
        if (shape == EPhysicsShapeType::Mesh && node.physics.bodyType != EPhysicsBodyType::Static)
            out.push_back("Physics: a Mesh shape works on Static bodies only.");
    }
    if (node.hasAudio)
    {
        if (node.audio.sounds.empty())
            out.push_back("Audio: no sounds. It will not be saved.");
        for (const AudioComponent::SoundDesc& sound : node.audio.sounds)
            if (sound.alias.empty() || sound.clips.empty())
                out.push_back("Audio: sound '" + sound.alias + "' needs an alias and a clip, or it is skipped at load.");
    }
    if (node.hasParticle && node.particle.effectPath.empty())
        out.push_back("Particle: no effect picked. It will not be saved.");
    if (node.hasLight && node.light.lights.empty())
        out.push_back("Light: no lights. It will not be saved.");
    if (node.hasScript && node.script.scriptPath.empty())
        out.push_back("Script: no script picked.");
    if (node.hasGameUnit && node.hasGameStructure)
        out.push_back("A GameUnit and a GameStructure on one entity: the game treats them as exclusive.");
}

void assignDocIds(EntityDocNode& node, uint64& nextId)
{
    node.id = nextId++;
    for (EntityDocNode& child : node.children)
        assignDocIds(child, nextId);
}
