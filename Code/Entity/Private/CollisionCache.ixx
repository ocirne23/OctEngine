export module Entity:CollisionCache;

import Core;
import Core.glm;
import File.fwd;
import Physics;
import Spatial;

// CPU snapshot of one container's collision-relevant scene data. Defined in CollisionCache.cpp: nothing
// outside the cache ever names it, so the "Col_" proxy rules and the node tree stay an implementation detail.
struct CollisionSource;

// The asset -> physics bridge: turns loaded scene data into collision shapes, and caches the results.
// Physics only speaks vertex/index spans and File only speaks scene data, so this translation has to live
// on the Entity side; keeping it in one object is what stops it from smearing across World.
//
// Meshes/nodes named "Col_*" are artist-authored collision proxies: physics collides against them (at
// their own placement) INSTEAD of the same-named render mesh ("Col_Wall" replaces "Wall"), and the
// renderer never draws them. Meshes without a proxy collide as themselves.
export class CollisionCache
{
public:

    // Snapshots a container's geometry at load time (see World::loadContainer) so hull/mesh shapes never
    // need the source file again. No-op when that container is already captured.
    void captureSource(const oc::string& containerName, const ISceneData& sceneData);
    bool hasSource(const oc::string& containerName) const { return m_sources.contains(containerName); }

    // Hull shape: the flattened point cloud for one spawnable node. Empty when the container wasn't captured.
    oc::vector<glm::vec3> buildHullPoints(const oc::string& containerName, const oc::string& nodePath) const;

    // Mesh shape: the triangle BVH for one spawnable node, cached per container|node. Null when the
    // container wasn't captured or the node has no triangles. Building one also extracts its occluders.
    oc::shared_ptr<PhysicsMesh> getOrBuildMesh(const oc::string& containerName, const oc::string& nodePath);

    // Occluder triangles for a node whose mesh was built above: the same flattened geometry doubles as the
    // CPU occlusion-culling source, so a static mesh collider is an occluder for free. Null if never built.
    oc::shared_ptr<const OccluderData> getOccluders(const oc::string& containerName, const oc::string& nodePath) const;

private:

    static oc::string meshKey(const oc::string& containerName, const oc::string& nodePath) { return containerName + "|" + nodePath; }

    oc::unordered_map<oc::string, oc::shared_ptr<const CollisionSource>> m_sources; // key: container name
    oc::unordered_map<oc::string, oc::shared_ptr<PhysicsMesh>> m_meshes;            // key: container|node
    oc::unordered_map<oc::string, oc::shared_ptr<const OccluderData>> m_occluders;  // key: container|node
};
