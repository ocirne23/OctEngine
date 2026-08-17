module Entity;

import Core;
import Core.glm;
import Core.Log;
import File;
import Physics;
import Spatial;

// Stripped CPU snapshot of a container's scene: mesh positions/indices plus the node tree. Captured from
// the same ISceneData the render container is built from (World::loadContainer), so the source file is
// imported once; per-node collision geometry is then derived from this without touching the file.
struct CollisionSource
{
    struct Mesh
    {
        oc::string name;
        oc::vector<glm::vec3> vertices;
        oc::vector<uint32> indices;
    };
    struct Node
    {
        oc::string name;
        glm::mat4 localTransform = glm::mat4(1.0f);
        oc::vector<uint32> meshIndices;
        oc::vector<Node> children;
    };
    oc::vector<Mesh> meshes;
    Node root;
    oc::unordered_set<oc::string> proxiedNames; // names that have a "Col_" proxy (prefix stripped)
};

// Flattened collision geometry for one spawnable node (positions + triangle indices, node transforms
// applied), built transiently from a CollisionSource for hull/mesh physics shapes.
struct PhysicsGeometry
{
    oc::vector<glm::vec3> vertices;
    oc::vector<uint32> indices;
};

static constexpr oc::string_view COLLISION_MESH_PREFIX = "Col_";

static bool isCollisionName(oc::string_view name) { return name.starts_with(COLLISION_MESH_PREFIX); }

static glm::mat4 nodeLocalTransform(const INodeData& node)
{
    glm::vec3 pos, scale;
    glm::quat rot;
    node.getTransform(pos, scale, rot);
    // The renderer flattens node scale to uniform scale.x (ObjectContainer::initializeNodes); collision
    // must match what is drawn, not the source data.
    return glm::translate(glm::mat4(1.0f), pos) * glm::mat4_cast(rot) * glm::scale(glm::mat4(1.0f), glm::vec3(scale.x));
}

static void buildCollisionSourceNode(const INodeData& node, CollisionSource::Node& out)
{
    out.name = node.getName();
    out.localTransform = nodeLocalTransform(node);
    for (uint32 m = 0; m < node.getNumMeshes(); ++m)
        out.meshIndices.push_back(node.getMeshIndex(m));
    out.children.resize(node.getNumChildren());
    for (uint32 c = 0; c < node.getNumChildren(); ++c)
        buildCollisionSourceNode(*node.getChild(c), out.children[c]);
}

static void gatherProxiedNames(const CollisionSource::Node& node, oc::unordered_set<oc::string>& outNames)
{
    if (isCollisionName(node.name))
        outNames.insert(node.name.substr(COLLISION_MESH_PREFIX.size()));
    for (const CollisionSource::Node& child : node.children)
        gatherProxiedNames(child, outNames);
}

// Appends a snapshot node subtree's meshes into collision space. Mirrors ObjectContainer's spawn
// rebasing: a ROOT spawn keeps the scene root's own transform (baked into the render offsets), while a
// sub-node spawn excludes the start node's transform (the entity transform places the node's pivot).
static void appendNodeGeometry(const CollisionSource& source, const CollisionSource::Node& node,
    const glm::mat4& parentTransform, bool skipOwnTransform, PhysicsGeometry& outGeometry)
{
    const glm::mat4 transform = skipOwnTransform ? parentTransform : parentTransform * node.localTransform;
    const bool nodeIsProxy = isCollisionName(node.name);
    for (uint32 meshIdx : node.meshIndices)
    {
        const CollisionSource::Mesh& mesh = source.meshes[meshIdx];
        // "Col_*" proxy meshes always collide; a render mesh is skipped when a proxy exists for its
        // (or its node's) name — the proxy replaces it.
        if (!nodeIsProxy && !isCollisionName(mesh.name)
            && (source.proxiedNames.contains(mesh.name) || source.proxiedNames.contains(node.name)))
            continue;
        const uint32 baseVertex = uint32(outGeometry.vertices.size());
        for (const glm::vec3& v : mesh.vertices)
            outGeometry.vertices.push_back(glm::vec3(transform * glm::vec4(v, 1.0f)));
        for (uint32 index : mesh.indices)
            outGeometry.indices.push_back(baseVertex + index);
    }
    for (const CollisionSource::Node& child : node.children)
        appendNodeGeometry(source, child, transform, false, outGeometry);
}

static const CollisionSource::Node* findNodeByName(const CollisionSource::Node& node, oc::string_view name)
{
    for (const CollisionSource::Node& child : node.children)
    {
        if (name == child.name)
            return &child;
        if (const CollisionSource::Node* found = findNodeByName(child, name))
            return found;
    }
    return nullptr;
}

static PhysicsGeometry buildCollisionGeometry(const CollisionSource& source, const oc::string& containerName, const oc::string& nodePath)
{
    const CollisionSource::Node* startNode = &source.root;
    if (!nodePath.empty() && nodePath != "ROOT")
    {
        if (const CollisionSource::Node* found = findNodeByName(source.root, nodePath.substr(nodePath.find_last_of('/') + 1)))
            startNode = found;
        else
            Log::warning("Physics: node '" + nodePath + "' not found in '" + containerName + "', using ROOT");
    }
    PhysicsGeometry geometry;
    appendNodeGeometry(source, *startNode, glm::mat4(1.0f), startNode != &source.root, geometry);
    return geometry;
}

void CollisionCache::captureSource(const oc::string& containerName, const ISceneData& sceneData)
{
    if (m_sources.contains(containerName))
        return;

    auto source = oc::make_shared<CollisionSource>();
    source->meshes.resize(sceneData.getNumMeshes());
    for (uint32 i = 0; i < sceneData.getNumMeshes(); ++i)
    {
        const IMeshData* mesh = sceneData.getMesh(i);
        if (!mesh)
            continue;
        CollisionSource::Mesh& outMesh = source->meshes[i];
        outMesh.name = mesh->getName();
        outMesh.vertices.assign(mesh->getVertices(), mesh->getVertices() + mesh->getNumVertices());
        outMesh.indices.assign(mesh->getIndices(), mesh->getIndices() + mesh->getNumIndices());
        if (isCollisionName(outMesh.name))
            source->proxiedNames.insert(outMesh.name.substr(COLLISION_MESH_PREFIX.size()));
    }
    buildCollisionSourceNode(sceneData.getRootNode(), source->root);
    gatherProxiedNames(source->root, source->proxiedNames);
    m_sources.emplace(containerName, oc::move(source));
}

oc::vector<glm::vec3> CollisionCache::buildHullPoints(const oc::string& containerName, const oc::string& nodePath) const
{
    auto it = m_sources.find(containerName);
    if (it == m_sources.end())
        return {};
    return buildCollisionGeometry(*it->second, containerName, nodePath).vertices;
}

oc::shared_ptr<PhysicsMesh> CollisionCache::getOrBuildMesh(const oc::string& containerName, const oc::string& nodePath)
{
    const oc::string key = meshKey(containerName, nodePath);
    if (auto it = m_meshes.find(key); it != m_meshes.end())
        return it->second;
    auto sourceIt = m_sources.find(containerName);
    if (sourceIt == m_sources.end())
        return nullptr;

    const PhysicsGeometry geometry = buildCollisionGeometry(*sourceIt->second, containerName, nodePath);
    if (geometry.indices.size() < 3)
        return nullptr;
    glm::vec3 boundsMin(FLT_MAX), boundsMax(-FLT_MAX);
    for (const glm::vec3& v : geometry.vertices)
    {
        boundsMin = glm::min(boundsMin, v);
        boundsMax = glm::max(boundsMax, v);
    }
    Log::info(std::format("Physics: collision mesh '{}': {} verts, {} tris, bounds ({:.2f}, {:.2f}, {:.2f}) - ({:.2f}, {:.2f}, {:.2f})",
        key, geometry.vertices.size(), geometry.indices.size() / 3,
        boundsMin.x, boundsMin.y, boundsMin.z, boundsMax.x, boundsMax.y, boundsMax.z));
    auto mesh = oc::make_shared<PhysicsMesh>(Globals::physics.createCollisionMesh(geometry.vertices, geometry.indices));
    if (!mesh->isValid())
        return nullptr;
    m_meshes.emplace(key, mesh);
    // The same flattened geometry doubles as the occlusion-culling occluder source (largest triangles).
    m_occluders.emplace(key, OcclusionBuffer::extractOccluders(geometry.vertices, geometry.indices,
        uint32(Globals::occlusionBuffer.getMaxTriangles())));
    return mesh;
}

oc::shared_ptr<const OccluderData> CollisionCache::getOccluders(const oc::string& containerName, const oc::string& nodePath) const
{
    auto it = m_occluders.find(meshKey(containerName, nodePath));
    return it != m_occluders.end() ? it->second : nullptr;
}
