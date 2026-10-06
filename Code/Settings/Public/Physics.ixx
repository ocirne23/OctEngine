export module Settings.Physics;

import Core;
import Core.glm;

// "Physics/World", "Physics/Buoyancy", "Physics/Debug": the box3d world (PhysicsWorld). The world pushes the
// values box3d keeps itself (gravity, worker count, contact tuning) on a change.
export struct PhysicsSettings
{
    glm::vec3 gravity = glm::vec3(0.0f, -9.81f, 0.0f);
    bool paused = false;
    bool interpolate = true;
    float timeScale = 1.0f;
    int subSteps = 4;
    int stepHz = 20;
    int workerCount = 0; // box3d parallelism on the engine job system; 0 = PhysicsWorld::initialize sets the default
    // Contact tuning (b3World_SetContactTuning): hertz = box3d's default; damping and the push-out speed cap are
    // ours, set for SOFT overlap recovery (box3d's defaults unwind a deep overlap in one step - an explosion when
    // stacked unit bodies enable at the SIM LOD edge).
    float contactHertz = 0.0f; // 0 = PhysicsWorld::initialize sets box3d's default
    float contactDamping = 50.0f;
    float contactSpeed = 0.1f; // m/s: max overlap resolution speed

    float waterDensity = 200.0f;   // kg/m^3; shapes denser than this sink
    float waterLinearDrag = 0.5f;  // 1/s: drag on each submerged probe's point velocity

    bool debugDrawColliders = false;
    bool debugDrawJoints = false;
    bool debugDrawContacts = false;
    bool debugDrawBounds = false;
    float debugDrawRange = 64.0f; // draw distance around the view position (world units)
};

export namespace Settings
{
    void registerPhysics(PhysicsSettings& s);
}
