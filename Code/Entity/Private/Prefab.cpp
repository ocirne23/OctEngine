module Entity;

import Core;
import Core.Log;
import Core.glm;
import :Component;
import :AssetRegistry;
import File;

static void writeSceneChild(Entity* child, AssetNode& sceneComp);

static void writeOverrides(Entity* entity, AssetNode& node, const std::string& tokenName)
{
    if (entity->hasName() && entity->getName() != tokenName)
        node.set("Name", entity->getName());
    if (!entity->isEnabled())
        node.set("Enabled", false); // enabled is the default, so only the disabled state is authored
    node.set("Position", entity->pos);
    node.set("Rotation", glm::degrees(glm::eulerAngles(entity->rot)));
    node.set("Scale", entity->scale);
}

static void writeEntityBody(Entity* entity, AssetNode& node, const std::string& tokenName)
{
    writeOverrides(entity, node, tokenName);

    for (uint16 i = 0; i < MaxInlineComponentTypes; ++i)
    {
        if (!(entity->typeBits & (1 << i)))
            continue;
        const EComponentID id = EComponentID(i);

        AssetNode comp;
        comp.key = "Component";
        comp.values.emplace_back(componentTypeName(id));

        if (id == EComponentID_Render)
        {
            if (const RenderComponent::SpawnInfo* si = getRenderSpawnInfo(entity))
                writeRenderSpawnInfo(*si, comp);
        }
        else if (id == EComponentID_Animator)
        {
            if (const AnimatorComponent::SpawnInfo* ai = getAnimatorSpawnInfo(entity))
                writeAnimatorSpawnInfo(*ai, comp);
        }
        else if (id == EComponentID_Physics)
        {
            if (const PhysicsComponent::SpawnInfo* pi = getPhysicsSpawnInfo(entity))
                writePhysicsSpawnInfo(*pi, comp);
        }
        else if (id == EComponentID_Audio)
        {
            if (const AudioComponent::SpawnInfo* ai = getAudioSpawnInfo(entity))
                writeAudioSpawnInfo(*ai, comp);
        }
        else if (id == EComponentID_Particle)
        {
            if (const ParticleComponent::SpawnInfo* pi = getParticleSpawnInfo(entity))
                writeParticleSpawnInfo(*pi, comp);
        }
        else if (id == EComponentID_Force)
        {
            if (const ForceComponent::SpawnInfo* fi = getForceSpawnInfo(entity))
                writeForceSpawnInfo(*fi, comp);
        }
        else if (id == EComponentID_Light)
        {
            if (const LightComponent::SpawnInfo* li = getLightSpawnInfo(entity))
                writeLightSpawnInfo(*li, comp);
        }
        else if (id == EComponentID_GameUnit)
        {
            if (const GameUnitComponent::SpawnInfo* gi = getGameUnitSpawnInfo(entity))
                writeGameUnitSpawnInfo(*gi, comp);
        }
        else if (id == EComponentID_GameStructure)
        {
            if (const GameStructureComponent::SpawnInfo* gi = getGameStructureSpawnInfo(entity))
                writeGameStructureSpawnInfo(*gi, comp);
        }
        else if (id == EComponentID_GameProjectile)
        {
            if (const GameProjectileComponent::SpawnInfo* gi = getGameProjectileSpawnInfo(entity))
                writeGameProjectileSpawnInfo(*gi, comp);
        }
        else if (id == EComponentID_Network)
        {
            if (const NetworkComponent::SpawnInfo* ni = getNetworkSpawnInfo(entity))
                writeNetworkSpawnInfo(*ni, comp);
        }
        else if (id == EComponentID_Script)
        {
            if (const ScriptComponent::SpawnInfo* si = getScriptSpawnInfo(entity))
            {
                if (!si->scriptPath.empty())
                    comp.set("Path", si->scriptPath);
                comp.set("Enabled", si->enabled);
                // The AUTHORED initial values only (the Entity Editor's). A live edit from the Properties panel
                // writes the running block and never reaches SpawnInfo, so it deliberately isn't saved here.
                if (!si->initialValues.empty())
                {
                    AssetNode& dataNode = comp.addChild("Data");
                    for (const ScriptComponent::InitialFieldValue& field : si->initialValues)
                        dataNode.set(field.name, field.value);
                }
            }
        }
        else if (id == EComponentID_Scene)
            if (SceneComponent* sc = getComponent<SceneComponent>(entity))
                for (const EntityPtr& child : sc->children)
                    writeSceneChild(child, comp);

        // Scene and Network are written even when empty: Scene is what makes the entity able to hold
        // children, and Network is pure presence (netIds are code-assigned, nothing to serialize).
        if (comp.children.empty() && id != EComponentID_Scene && id != EComponentID_Network)
            continue;
        node.children.push_back(std::move(comp));
    }
}

static void writeSceneChild(Entity* child, AssetNode& sceneComp)
{
    const std::string& source = child->getPrefabName(); // the prefab name it spawned from, or empty if inline
    if (child->isPrefabInstance() && !source.empty() && Globals::assetRegistry.findPrefab(source))
    {
        AssetNode& node = sceneComp.addChild("Prefab");
        node.values.emplace_back(source);
        writeOverrides(child, node, source);
    }
    else
    {
        const std::string token = child->hasName() ? std::string(child->getName()) : std::string("Entity");
        AssetNode& node = sceneComp.addChild("Entity");
        node.values.emplace_back(token);
        writeEntityBody(child, node, token);
    }
}

static bool keyEq(const std::string& a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < b.size(); ++i)
        if ((a[i] | 0x20) != (b[i] | 0x20)) // ASCII lower; keys are human-authored
            return false;
    return true;
}

// Gather every prefab reference (`Prefab <name>`) nested anywhere under `node`. References carry only
// overrides, never nested declarations, so we don't descend into a reference node itself.
static void collectAssetPrefabRefs(const AssetNode& node, std::vector<std::string>& out)
{
    for (const AssetNode& child : node.children)
    {
        if (keyEq(child.key, "Prefab"))
            out.push_back(child.asString());
        else
            collectAssetPrefabRefs(child, out);
    }
}

// Does prefab `name` (transitively, via its on-disk file) reach `target`?
static bool prefabFileReaches(const std::string& name, const std::string& target, std::unordered_set<std::string>& visited)
{
    if (name == target)
        return true;
    if (!visited.insert(name).second)
        return false; // already explored (guards against a pre-existing cyclic graph on disk)

    const std::string* path = Globals::assetRegistry.findPrefab(name);
    if (!path)
        return false;

    AssetNode doc;
    std::string error;
    if (!loadAssetFile(*path, doc, error))
        return false;

    std::vector<std::string> refs;
    for (const AssetNode& decl : doc.children)
        if (keyEq(decl.key, "Prefab"))
            collectAssetPrefabRefs(decl, refs);

    for (const std::string& ref : refs)
        if (prefabFileReaches(ref, target, visited))
            return true;
    return false;
}

// Prefab instances serialize as a bare reference (not their subtree), so collect the names they spawn;
// descend only through inline entities, whose bodies are written out in full.
static void collectLivePrefabRefs(Entity* entity, std::vector<std::string>& out)
{
    SceneComponent* sc = getComponent<SceneComponent>(entity);
    if (!sc)
        return;
    for (const EntityPtr& child : sc->children)
    {
        if (child->isPrefabInstance() && !child->getPrefabName().empty())
            out.push_back(child->getPrefabName());
        else
            collectLivePrefabRefs(child.get(), out);
    }
}

bool prefabWouldCycle(Entity* root, const std::string& id)
{
    std::vector<std::string> refs;
    collectLivePrefabRefs(root, refs);

    std::unordered_set<std::string> visited;
    for (const std::string& ref : refs)
        if (prefabFileReaches(ref, id, visited)) // direct (ref == id) or transitive through ref's file
            return true;
    return false;
}

std::string serializePrefabText(Entity* root, const std::string& id)
{
    if (!root)
        return {};

    AssetNode doc; // synthetic root; its children are the top-level declarations
    AssetNode& node = doc.addChild("Prefab");
    node.values.emplace_back(id);
    writeEntityBody(root, node, id);

    return writeAssetText(doc);
}

bool savePrefab(Entity* root, const std::string& path, const std::string& text)
{
    if (!root)
        return false;
    const std::string id = FileSystem::stem(path);
    if (prefabWouldCycle(root, id))
    {
        Log::warning("Prefab: refusing to save '" + id + "' â€” it contains a prefab instance of itself (cycle)");
        return false;
    }

    if (!FileSystem::writeFileStr(path, text.empty() ? serializePrefabText(root, id) : text))
        return false;

    Globals::assetRegistry.addPrefab(id, path);
    return true;
}
