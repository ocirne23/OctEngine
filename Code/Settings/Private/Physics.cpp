module Settings.Physics;

import Core;
import Core.glm;
import Settings.Tweaks;

void Settings::registerPhysics(PhysicsSettings& s)
{
    Tweak::float3("Physics/World", "Gravity", &s.gravity, 0.05f);
    Tweak::boolean("Physics/World", "Paused", &s.paused);
    Tweak::boolean("Physics/World", "Interpolate", &s.interpolate);
    Tweak::floatVar("Physics/World", "Time Scale", &s.timeScale, 0.0f, 4.0f);
    Tweak::intVar("Physics/World", "Sub Steps", &s.subSteps, 1, 16);
    Tweak::intVar("Physics/World", "Step Hz", &s.stepHz, 5, 120);
    // Live: box3d re-slices the step from this on the next b3World_Step. 1 = single threaded, which
    // is also the A/B toggle for measuring what the fan-out actually buys on a given scene.
    Tweak::intVar("Physics/World", "Worker count", &s.workerCount, 1, 32, 1.0f); // 32 = B3_MAX_WORKERS (static_asserted in PhysicsWorld.cpp)
    Tweak::floatVar("Physics/World", "Contact hertz", &s.contactHertz, 5.0f, 240.0f, 1.0f);
    Tweak::floatVar("Physics/World", "Contact damping", &s.contactDamping, 0.0f, 50.0f, 0.5f);
    Tweak::floatVar("Physics/World", "Contact push speed (m/s)", &s.contactSpeed, 0.1f, 20.0f, 0.1f);

    Tweak::floatVar("Physics/Buoyancy", "Density (kg/m3)", &s.waterDensity, 0.0f, 3000.0f, 10.0f);
    Tweak::floatVar("Physics/Buoyancy", "Linear drag", &s.waterLinearDrag, 0.0f, 20.0f, 0.1f);

    Tweak::boolean("Physics/Debug", "Draw colliders", &s.debugDrawColliders);
    Tweak::boolean("Physics/Debug", "Draw joints", &s.debugDrawJoints);
    Tweak::boolean("Physics/Debug", "Draw contacts", &s.debugDrawContacts);
    Tweak::boolean("Physics/Debug", "Draw bounds", &s.debugDrawBounds);
    Tweak::floatVar("Physics/Debug", "Range", &s.debugDrawRange, 4.0f, 1024.0f);
}
