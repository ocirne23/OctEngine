export module Physics:Types;

import Core;
import Core.glm;
import Core.Transform;

import :Mesh;
import :Layers;

export enum class EPhysicsBodyType : uint8
{
    Static,
    Kinematic,
    Dynamic,
};

export enum class EPhysicsShapeType : uint8
{
    Box,
    Sphere,
    Capsule,
    Hull, // convex hull reduced from a point cloud (hullPoints)
    Mesh, // triangle-mesh BVH (mesh), static bodies only
};

export struct PhysicsShape
{
    EPhysicsShapeType type = EPhysicsShapeType::Box;
    glm::vec3 halfExtents = glm::vec3(0.5f); // Box
    float radius = 0.5f;                     // Sphere / Capsule
    float halfHeight = 0.5f;                 // Capsule: center to hemisphere center, along local Y
    glm::vec3 offset = glm::vec3(0.0f);      // placement within the body's local space
    float density = 1000.0f;                 // kg/m^3
    float friction = 0.6f;
    float restitution = 0.0f;
    uint64 categoryBits = 1;                 // the layer(s) this shape is on (bit 0 = "Default")
    uint64 maskBits = PhysicsLayers::All;    // the layers this shape collides with
    int groupIndex = 0;                      // box3d collision group override, 0 = none

    oc::vector<glm::vec3> hullPoints;       // Hull: point cloud, reduced to a convex hull at body creation
    int maxHullVertices = 32;                // Hull: vertex budget for the reduction (clamped to [4, 64])
    const PhysicsMesh* mesh = nullptr;       // Mesh: shared triangle BVH, must outlive the body

    bool isSensor = false;                   // overlap events instead of collision response
    bool contactEvents = false;              // report begin/end touch events for this shape
};

export struct PhysicsBodyDesc
{
    EPhysicsBodyType type = EPhysicsBodyType::Dynamic;
    Transform transform;                          // world placement; transform.scale is baked into the shape dimensions
    glm::vec3 linearVelocity = glm::vec3(0.0f);
    void* userData = nullptr;                     // reported back through ContactEvent (the engine stores Entity* here)
    bool lockRotation = false;                    // solver-level lock on all angular axes: the body never rotates
                                                  // (upright character capsules); contacts apply no torque
};
