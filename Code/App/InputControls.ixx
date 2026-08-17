export module App.InputControls;

import Core;
import Core.Allocator;
import Core.Log;
import Core.Window;
import Core.SDL;
import Core.GameHud;
import Core.Frustum;
import Core.Time;
import Core.glm;
import Core.Camera;
import Core.Tweaks;

import Animation;
import File;
import Input;
import UI;
import RendererVK;
import Entity;
import Script;
import Physics;
import Audio;
import Force;

export class InputControls
{
private:

    IGizmo& gizmo; // owned by the UI; only the mode hotkeys touch it from here
    FreeFlyCameraController& cameraController;
    World& world; // spawns go through the world's API; it owns the root entity lifetimes
    oc::vector<ForceEmitter> spawnedForceEmitters; // test forcefields (keys N/M/H/B); RAII, cleared before globals die
    oc::vector<ForceQuery> spawnedForceQueries;    // territory-query test grid (key J)
    struct ForceBall // physics body carrying a forcefield: enemy bubbles knock it back (key F)
    {
        EntityPtr entity;
        ForceEmitter emitter;
    };
    oc::vector<ForceBall> forceBalls;
    KeyboardListenerHandle pKeyboardListener; // unregisters itself on destruction
    
    float output = 1.0f;          // must exceed the iso threshold (default 0.15) or no bubble exists
    float reach = 4.0f;           // TOTAL extent: the bubble spans pos .. pos + dir * reach
    float focus = 0.5f;           // shape pinch: 0.5 = sphere spanning the line, 0 = cone pointed at
                                  // the emitter, 1 = cone pointed at the target
    int32 team = 0;
    float distribution = 0.5f;    // where the output density sits along the line (0 = emitter end,
                                  // 1 = target end); budget-conserving bump
    float width = 1.0f;           // lateral scale (reach untouched): 1 = round, < 1 = narrower/sharper

    bool gameMode = false;           // --game: the Game library owns player/camera/spawn keys — the
                                     // testbed spawn/possess keys are muted (F5/F6/gizmo modes stay)
    bool playerControl = false;      // key C: WASD/Space drive the player entity, camera flight paused
    bool playerJumpWasDown = false;  // Space edge detection
    float playerMoveSpeed = 8.0f;    // m/s horizontal target (keep under Network/Validation "Max speed")
    float playerAccel = 60.0f;       // m/s^2 velocity steering
    float playerJumpSpeed = 6.0f;

    // Local player capsule (single player / server): key C spawns + possesses an upright capsule body
    // (playerCapsule.pre, LockRotation), key V switches first/third person. On a network client key C
    // keeps its meaning of driving the locally-owned entity instead (see updatePlayerControl).
    EntityPtr playerEntity;
    bool playerThirdPerson = true;   // start visible; V toggles
    float playerEyeHeight = 0.55f;   // metres above the capsule center
    float playerThirdDistance = 3.5f;
    float playerSprintMult = 2.0f;   // LShift multiplier on Move speed

    oc::shared_ptr<EntitySpawnTemplate> lightTmpl; // built once, see lightTemplate()

    // ONE template for every test light: they all share an archetype (a lone Light component), and the
    // per-light values live on the component, which owns a mutable copy of the SpawnInfo. Built lazily
    // and handed to World, which keeps it alive because Entity::create only stores a raw pointer to it
    // (the same contract the Entity Editor's respawns use).
    const EntitySpawnTemplate& lightTemplate()
    {
        if (!lightTmpl)
        {
            auto info = oc::make_shared<LightComponent::SpawnInfo>();
            info->lights.emplace_back(); // placeholder; spawnLight overwrites it on the live component
            info->debugDraw = true;
            lightTmpl = oc::make_shared<EntitySpawnTemplate>();
            lightTmpl->archetype = makeEntityArchetype(uint16(1 << EComponentID_Light));
            lightTmpl->spawnInfos.push_back(oc::move(info));
            world.keepTemplateAlive(lightTmpl);
        }
        return *lightTmpl;
    }

    // Spawns a bare entity whose only component is one light, and tracks it for the clear key. The
    // component draws its own debug wireframes, so there are no proxy meshes to place any more. The
    // light's values are written onto the live component, NOT into the shared template - so these
    // entities deliberately don't serialize their light; they are debug spawns, not authoring.
    EntityPtr spawnLight(const char* name, const glm::vec3& pos, const LightComponent::LightDesc& light)
    {
        // Spawned unrotated, so the light's entity-space Direction is already the world one.
        EntityPtr entity = Entity::create(lightTemplate(), Transform(pos, 1.0f, glm::quat(1.0f, 0.0f, 0.0f, 0.0f)));
        entity->setName(name);
        if (LightComponent* lc = getComponent<LightComponent>(entity))
            lc->lights[0] = light;
        world.addRootEntity(entity);
        return entity;
    }

public:

    InputControls(
        IGizmo& gizmo,
        FreeFlyCameraController& cameraController,
        World& world)
        : gizmo(gizmo), cameraController(cameraController), world(world)
    {
		Tweak::floatVar("Force/Emitter", "Output", &output, 0.2f, 10.0f, 0.01f); // stay above iso (0.15)
		Tweak::floatVar("Force/Emitter", "Reach", &reach, 0.1f, 100.0f, 0.1f);
		Tweak::floatVar("Force/Emitter", "Focus", &focus, 0.0f, 1.0f, 0.01f);
		Tweak::intVar("Force/Emitter", "Team", &team, 0, 7, 1);
		Tweak::floatVar("Force/Emitter", "Distribution", &distribution, 0.0f, 1.0f, 0.01f);
		Tweak::floatVar("Force/Emitter", "Width", &width, 0.05f, 2.0f, 0.01f);

		Tweak::floatVar("Network/Player", "Move speed", &playerMoveSpeed, 0.5f, 30.0f, 0.1f);
		Tweak::floatVar("Network/Player", "Accel", &playerAccel, 1.0f, 200.0f, 0.5f);
		Tweak::floatVar("Network/Player", "Jump speed", &playerJumpSpeed, 0.5f, 20.0f, 0.1f);

		Tweak::floatVar("Player", "Eye height", &playerEyeHeight, 0.0f, 2.0f, 0.05f);
		Tweak::floatVar("Player", "Third person dist", &playerThirdDistance, 0.5f, 10.0f, 0.1f);
		Tweak::floatVar("Player", "Sprint mult", &playerSprintMult, 1.0f, 5.0f, 0.1f);

        auto& input = Globals::input;
        pKeyboardListener = input.addKeyboardListener();
        pKeyboardListener->onKeyPressed = [this](const SDL_KeyboardEvent& evt) { handleKeyEvent(evt); };
        pKeyboardListener->onKeyReleased = [this](const SDL_KeyboardEvent& evt) { handleKeyEvent(evt); };
    }

    // Per-frame maintenance of the test force-balls: the emitter follows the simulated body and the
    // GPU-read-back applied force (opposing bubbles pressing on it) feeds back as a physics impulse.
    // Call after world.update (body poses synced), before forceSystem.update pushes emitter state.
    void setGameMode(bool enabled) { gameMode = enabled; }

    void update(float deltaSec)
    {
        ProfileScope profileScope("Input controls", EProfileCategory::App);
        if (gameMode)
            return; // the Game library drives the player; no testbed force balls
        if (playerControl)
        {
            if (Globals::networkManager.role() == ENetRole::Client)
                updatePlayerControl(deltaSec);
            else
                updateLocalPlayer(deltaSec);
        }
        for (ForceBall& ball : forceBalls)
        {
            if (!ball.entity || !ball.emitter.isValid())
                continue;
            PhysicsComponent* pc = getComponent<PhysicsComponent>(ball.entity);
            if (!pc || !pc->body.isValid())
                continue;
            ball.emitter.setPosition(pc->body.getPosition() - glm::vec3(0, 1, 0)); // keep the span centered on the ball
            const glm::vec3 force = ball.emitter.getAppliedForce();
            const float pressure = ball.emitter.getPressure();
            if (glm::dot(force, force) > 1e-8f)
                pc->body.applyImpulse(force * deltaSec * 10000.0f * pressure);
        }
    }

    // Distance from the body center to the shape's lowest point, world units — grounds the jump
    // raycast for any player shape (capsule, cube, sphere).
    static float shapeBottomDistance(const Entity* entity, const PhysicsComponent& pc)
    {
        const PhysicsComponent::SpawnInfo* si = getPhysicsSpawnInfo(entity);
        float d = 1.0f;
        if (si)
            switch (si->shape.type)
            {
            case EPhysicsShapeType::Box:     d = si->shape.halfExtents.y; break;
            case EPhysicsShapeType::Sphere:  d = si->shape.radius; break;
            case EPhysicsShapeType::Capsule: d = si->shape.halfHeight + si->shape.radius; break;
            default: break;
            }
        return d * pc.shapeScale;
    }

    // The client's own player entity: the ONE locally-owned primary (proximity-transferred objects
    // are owned too, but they are not the player).
    Entity* findLocalNetPlayer()
    {
        for (const EntityPtr& root : world.rootEntities())
        {
            NetworkComponent* net = getComponent<NetworkComponent>(root.get());
            if (net && net->state && net->authority() == ENetAuthority::LocalOwner && !net->state->transferredOwnership)
                return root.get();
        }
        return nullptr;
    }

    // Drives every locally-owned dynamic-body entity with WASD/Space while player control (key C)
    // is on. The owning client simulates its own body — full responsiveness at any RTT: horizontal
    // velocity steering (frame-rate independent, snappy), Space jumps off a ground raycast. The
    // sampled intent lands in comp->input (streamed with the claims, mirrored server-side for
    // gameplay), and the claim stream carries the resulting body state to the server. Main thread,
    // before physics.update, so the direct body setters are the sanctioned path.
    void updatePlayerControl(float deltaSec)
    {
        if (Globals::networkManager.role() != ENetRole::Client)
            return; // only clients own entities
        Input& input = Globals::input;
        if (!input.isWindowHasFocus() || !Globals::ui.isViewportFocused())
            return;

        glm::vec3 forward = cameraController.getDirection();
        forward.y = 0.0f; // camera-relative planar movement
        const float forwardLen = glm::length(forward);
        forward = forwardLen > 1e-4f ? forward / forwardLen : glm::vec3(0.0f, 0.0f, -1.0f);
        const glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));

        glm::vec3 move(0.0f);
        if (input.isKeyDown(SDL_SCANCODE_W)) move += forward;
        if (input.isKeyDown(SDL_SCANCODE_S)) move -= forward;
        if (input.isKeyDown(SDL_SCANCODE_D)) move += right;
        if (input.isKeyDown(SDL_SCANCODE_A)) move -= right;
        if (glm::dot(move, move) > 1e-8f)
            move = glm::normalize(move);
        const bool jumpDown = input.isKeyDown(SDL_SCANCODE_SPACE);
        const bool jumpPressed = jumpDown && !playerJumpWasDown;
        playerJumpWasDown = jumpDown;

        for (const EntityPtr& root : world.rootEntities())
        {
            NetworkComponent* net = getComponent<NetworkComponent>(root.get());
            if (!net || !net->state || net->authority() != ENetAuthority::LocalOwner)
                continue;
            if (net->state->transferredOwnership)
                continue; // proximity-transferred objects: our sim drives them, but they are not the player
            net->state->input.buttons = jumpDown ? NetInputButton_Jump : 0u;
            net->state->input.move = move;
            net->state->input.look = cameraController.getDirection();

            // Server reasserting itself (claims rejected): YIELD — steering against the forced
            // correction keeps the error large, breeds fresh rejections, and turns the resync into a
            // tug-of-war that only ends when the player releases the keys. Yielding lets it resolve
            // in one quick snap; records stop carrying Forced the moment claims are accepted again.
            if (net->state->client.targetFlags & NetRecFlag_Forced)
                continue;

            PhysicsComponent* pc = getComponent<PhysicsComponent>(root.get());
            if (!pc || pc->bodyType != EPhysicsBodyType::Dynamic || !pc->body.isValid())
                continue;
            glm::vec3 vel = pc->body.getLinearVelocity();
            glm::vec3 dv = glm::vec3(move.x * playerMoveSpeed - vel.x, 0.0f, move.z * playerMoveSpeed - vel.z);
            const float maxDv = playerAccel * deltaSec;
            const float dvLen = glm::length(dv);
            if (dvLen > maxDv && dvLen > 1e-6f)
                dv *= maxDv / dvLen;
            vel.x += dv.x;
            vel.z += dv.z;
            if (jumpPressed)
            {
                // grounded = something under the shape's bottom within a small margin; the ray
                // ignores the body's own shapes so it can start inside them
                const glm::vec3 pos = pc->body.getPosition();
                const float bottom = shapeBottomDistance(root.get(), *pc);
                if (Globals::physics.castRayClosest(pos, glm::vec3(0.0f, -(bottom + 0.3f), 0.0f), PhysicsLayers::All, &pc->body).hit)
                    vel.y = playerJumpSpeed;
            }
            pc->body.setLinearVelocity(vel);
        }
    }

    // Drives the local player capsule (single player / server): camera-relative WASD velocity
    // steering on the upright capsule body, Space jumps off a ground raycast, LShift sprints.
    // Main thread, before physics.update, so the direct body setters are the sanctioned path.
    void updateLocalPlayer(float deltaSec)
    {
        Input& input = Globals::input;
        if (!input.isWindowHasFocus() || !Globals::ui.isViewportFocused())
            return;
        PhysicsComponent* pc = playerEntity ? getComponent<PhysicsComponent>(playerEntity.get()) : nullptr;
        if (!pc || pc->bodyType != EPhysicsBodyType::Dynamic || !pc->body.isValid())
            return;

        glm::vec3 forward = cameraController.getDirection();
        forward.y = 0.0f; // camera-relative planar movement
        const float forwardLen = glm::length(forward);
        forward = forwardLen > 1e-4f ? forward / forwardLen : glm::vec3(0.0f, 0.0f, -1.0f);
        const glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));

        glm::vec3 move(0.0f);
        if (input.isKeyDown(SDL_SCANCODE_W)) move += forward;
        if (input.isKeyDown(SDL_SCANCODE_S)) move -= forward;
        if (input.isKeyDown(SDL_SCANCODE_D)) move += right;
        if (input.isKeyDown(SDL_SCANCODE_A)) move -= right;
        if (glm::dot(move, move) > 1e-8f)
            move = glm::normalize(move);
        const float speed = playerMoveSpeed * (input.isKeyDown(SDL_SCANCODE_LSHIFT) ? playerSprintMult : 1.0f);

        glm::vec3 vel = pc->body.getLinearVelocity();
        glm::vec3 dv = glm::vec3(move.x * speed - vel.x, 0.0f, move.z * speed - vel.z);
        const float maxDv = playerAccel * deltaSec;
        const float dvLen = glm::length(dv);
        if (dvLen > maxDv && dvLen > 1e-6f)
            dv *= maxDv / dvLen;
        vel.x += dv.x;
        vel.z += dv.z;

        const bool jumpDown = input.isKeyDown(SDL_SCANCODE_SPACE);
        if (jumpDown && !playerJumpWasDown)
        {
            // grounded = something under the capsule bottom within a small margin; the ray ignores
            // the capsule's own shapes so it can start inside them
            const float bottom = shapeBottomDistance(playerEntity.get(), *pc);
            const glm::vec3 pos = pc->body.getPosition();
            if (Globals::physics.castRayClosest(pos, glm::vec3(0.0f, -(bottom + 0.3f), 0.0f), PhysicsLayers::All, &pc->body).hit)
                vel.y = playerJumpSpeed;
        }
        playerJumpWasDown = jumpDown;
        pc->body.setLinearVelocity(vel);
    }

    // Key C outside a client session: spawn + possess the capsule, or hand the camera back.
    void setLocalPlayerPossessed(bool possess)
    {
        if (possess)
        {
            const glm::vec3 dir = cameraController.getDirection();
            playerEntity = world.spawnAssetFile("Entities/Debug/playerCapsule.pre",
                Transform(cameraController.getPosition() + dir * 2.0f), true);
            if (playerEntity)
                world.addRootEntity(playerEntity);
            playerJumpWasDown = true; // swallow a Space held through the toggle
        }
        else if (playerEntity)
        {
            if (PhysicsComponent* pc = getComponent<PhysicsComponent>(playerEntity.get()); pc && pc->body.isValid())
                cameraController.setPosition(pc->body.getPosition() + glm::vec3(0.0f, playerEyeHeight, 0.0f));
            world.removeRootEntity(playerEntity);
            playerEntity = EntityPtr();
        }
    }

    // Replaces the frame camera with the possessed capsule's view: first person at eye height, or a
    // third-person follow pulled in by a wall raycast. Called from main right after the fly camera
    // produced the frame camera, so everything downstream (UI picking, audio listener, renderer)
    // sees the player view. The pose interpolates with the same prev/curr/alpha the render mesh
    // used LAST frame - one sim step behind this frame's mesh, a smooth constant offset.
    void applyPlayerCamera(Camera& camera)
    {
        if (!playerControl)
            return;
        Entity* player = Globals::networkManager.role() == ENetRole::Client
            ? findLocalNetPlayer() // the client-owned primary the server spawned for us
            : playerEntity.get();
        PhysicsComponent* pc = player ? getComponent<PhysicsComponent>(player) : nullptr;
        if (!pc || !pc->body.isValid())
            return;
        const float alpha = Globals::physics.getInterpolationAlpha();
        const glm::vec3 pos = glm::mix(pc->prevPos, pc->currPos, alpha);
        const glm::vec3 dir = cameraController.getDirection();
        glm::vec3 eye = pos + glm::vec3(0.0f, playerEyeHeight, 0.0f);
        if (playerThirdPerson)
        {
            const glm::vec3 back = -dir * playerThirdDistance;
            const PhysicsWorld::RayHit hit = Globals::physics.castRayClosest(eye, back, PhysicsLayers::All, &pc->body);
            eye += hit.hit ? back * glm::max(hit.fraction - 0.1f, 0.0f) : back;
        }
        camera.position = eye;
        camera.viewMatrix = glm::lookAt(eye, eye + dir, glm::vec3(0.0f, 1.0f, 0.0f));
    }

    void handleKeyEvent(const SDL_KeyboardEvent& evt)
    {
        ScriptEventManager& scriptEvents = Globals::scriptEvents;
        Renderer& renderer = Globals::rendererVK;

        if (evt.scancode == SDL_Scancode::SDL_SCANCODE_W) scriptEvents.fireEvent(evt.type == SDL_EventType::SDL_EVENT_KEY_DOWN ? "W Down" : "W Up");
        if (evt.scancode == SDL_Scancode::SDL_SCANCODE_A) scriptEvents.fireEvent(evt.type == SDL_EventType::SDL_EVENT_KEY_DOWN ? "A Down" : "A Up");
        if (evt.scancode == SDL_Scancode::SDL_SCANCODE_S) scriptEvents.fireEvent(evt.type == SDL_EventType::SDL_EVENT_KEY_DOWN ? "S Down" : "S Up");
        if (evt.scancode == SDL_Scancode::SDL_SCANCODE_D) scriptEvents.fireEvent(evt.type == SDL_EventType::SDL_EVENT_KEY_DOWN ? "D Down" : "D Up");
        if (evt.scancode == SDL_Scancode::SDL_SCANCODE_LSHIFT) scriptEvents.fireEvent(evt.type == SDL_EventType::SDL_EVENT_KEY_DOWN ? "LShift Down" : "LShift Up");
        if (evt.scancode == SDL_Scancode::SDL_SCANCODE_SPACE)  scriptEvents.fireEvent(evt.type == SDL_EventType::SDL_EVENT_KEY_DOWN ? "Space Down" : "Space Up");

        // Hotbar slot selection: while the game HUD's hotbar is ACTIVE (visible + any slot assigned by a
        // script), keys 1..9,0 select slots 0..9 and fire the global "Hotbar Select" script event; the
        // testbed spawn keys on those scancodes are skipped via the return. With no hotbar the testbed
        // keys keep their meaning. SDL scancodes 1..9,0 are contiguous (SDL_SCANCODE_1 .. SDL_SCANCODE_0).
        // In --game mode the Game library drives the hotbar itself (grid hotkeys QWER/ASDF/ZXCV +
        // slot clicks), so the number keys stay out of it.
        if (evt.type == SDL_EventType::SDL_EVENT_KEY_DOWN && !evt.repeat && !gameMode
            && evt.scancode >= SDL_Scancode::SDL_SCANCODE_1 && evt.scancode <= SDL_Scancode::SDL_SCANCODE_0
            && (evt.mod & SDL_KMOD_CTRL) == 0 && Globals::gameHud.isHotbarActive())
        {
            Globals::gameHud.selectSlot(int(evt.scancode) - int(SDL_Scancode::SDL_SCANCODE_1));
            scriptEvents.fireEvent("Hotbar Select"); // re-pressing the selected slot fires again ("use item")
            return;
        }

        if (evt.type == SDL_EventType::SDL_EVENT_KEY_DOWN)
        {
            if (evt.scancode == SDL_Scancode::SDL_SCANCODE_T) gizmo.setMode(EGizmoMode::Translate);
            if (evt.scancode == SDL_Scancode::SDL_SCANCODE_R) gizmo.setMode(EGizmoMode::Rotate);
            if (evt.scancode == SDL_Scancode::SDL_SCANCODE_G) gizmo.setMode(EGizmoMode::Scale);
        }
        if (evt.scancode == SDL_Scancode::SDL_SCANCODE_F5 && evt.type == SDL_EventType::SDL_EVENT_KEY_DOWN)
            renderer.reloadShaders();
        if (evt.scancode == SDL_Scancode::SDL_SCANCODE_F6 && evt.type == SDL_EventType::SDL_EVENT_KEY_DOWN)
			Globals::scriptHost.reloadCurrentScript(); // F6: recompile + hot-reload the script the Script panel is editing
        if (gameMode)
            return; // game mode: everything below is testbed spawns/possession — the Game library owns those keys
        if (evt.scancode == SDL_Scancode::SDL_SCANCODE_K && evt.type == SDL_EventType::SDL_EVENT_KEY_DOWN)
            Globals::networkManager.fireNetworkEvent("NetPing"); // K: network-event smoke test (fires on every connected instance)
        if (evt.scancode == SDL_Scancode::SDL_SCANCODE_C && evt.type == SDL_EventType::SDL_EVENT_KEY_DOWN
            && !evt.repeat && (evt.mod & SDL_KMOD_CTRL) == 0) // plain C only — Ctrl+C stays copy
        {
            // C: toggle player control — WASD/Space drive the player instead of flying the camera
            // (mouse-look stays). On a client that is the locally-owned networked entity (see
            // updatePlayerControl); otherwise a local upright capsule is spawned and possessed.
            playerControl = !playerControl;
            cameraController.setMovementEnabled(!playerControl);
            if (Globals::networkManager.role() == ENetRole::Client)
                Log::info(playerControl ? "Player control ON: WASD/Space drive your entity (C to release)"
                                        : "Player control OFF: camera flight restored");
            else
            {
                setLocalPlayerPossessed(playerControl);
                Log::info(playerControl ? "Player control ON: WASD/Space/LShift drive the capsule, V toggles first/third person (C to release)"
                                        : "Player control OFF: camera flight restored");
            }
        }
        if (evt.scancode == SDL_Scancode::SDL_SCANCODE_V && evt.type == SDL_EventType::SDL_EVENT_KEY_DOWN
            && !evt.repeat && (evt.mod & SDL_KMOD_CTRL) == 0) // plain V only — Ctrl+V stays paste
        {
            playerThirdPerson = !playerThirdPerson;
            if (playerControl)
                Log::info(playerThirdPerson ? "Player camera: third person" : "Player camera: first person");
        }
        if (evt.scancode == SDL_Scancode::SDL_SCANCODE_U && evt.type == SDL_EventType::SDL_EVENT_KEY_DOWN)
        {
            // U: runtime-spawn a networked physics cube thrown from the camera — on a server the spawn
            // replicates to every client (Spawn message + physics sync + Despawn). Clients can't author
            // server state, so it's refused there rather than spawning a local unsynced ghost.
            if (Globals::networkManager.role() == ENetRole::Client)
                Log::info("Network: U ignored on a client (spawns are server-authoritative)");
            else
            {
                const glm::vec3 dir = cameraController.getDirection();
                EntityPtr cube = world.spawnAssetFile("Entities/Debug/netPhysCube.pre", Transform(cameraController.getPosition() + dir), true);
                if (PhysicsComponent* pc = getComponent<PhysicsComponent>(cube.get()))
                    pc->body.setLinearVelocity(dir * 12.0f);
                world.addRootEntity(oc::move(cube));
            }
        }
        if (evt.scancode == SDL_Scancode::SDL_SCANCODE_P && evt.type == SDL_EventType::SDL_EVENT_KEY_DOWN)
            renderer.toggleGiProbeDebug();          // P: show/hide GI probe debug cubes
        if (evt.scancode == SDL_Scancode::SDL_SCANCODE_O && evt.type == SDL_EventType::SDL_EVENT_KEY_DOWN)
            renderer.cycleGiProbeDebugMode();        // O: cycle irradiance <-> cellSize/LOD color
        if (evt.scancode == SDL_Scancode::SDL_SCANCODE_L && evt.type == SDL_EventType::SDL_EVENT_KEY_DOWN)
        {
            renderer.setSunLight(-cameraController.getDirection(), glm::vec3(1.0f), 5.0f); // aim the sun along the camera forward
        }
        if (evt.scancode == SDL_Scancode::SDL_SCANCODE_2 && evt.type == SDL_EventType::SDL_EVENT_KEY_DOWN)
        {
            LightComponent::LightDesc light;
            light.range = 25.0f;
            light.color = glm::abs(glm::sphericalRand(1.0f));
            light.intensity = 50.0f;
            spawnLight("TestPointLight", cameraController.getPosition(), light);
        }
        if (evt.scancode == SDL_Scancode::SDL_SCANCODE_3 && evt.type == SDL_EventType::SDL_EVENT_KEY_DOWN)
        {
            LightComponent::LightDesc light;
            light.range = 15.0f + glm::linearRand(0.5f, 1.5f);
            light.color = glm::abs(glm::sphericalRand(1.0f));
            light.intensity = 30.0f;
            spawnLight("TestPointLight", cameraController.getPosition(), light);
        }
        if (evt.scancode == SDL_Scancode::SDL_SCANCODE_4 && evt.type == SDL_EventType::SDL_EVENT_KEY_DOWN)
        {
            LightComponent::LightDesc light;
            light.type = ELightType::Spot;
            light.direction = cameraController.getDirection();
            light.range = 12.5f;
            light.color = glm::vec3(1.0f, 0.95f, 0.8f);
            light.intensity = 20.0f;
            light.coneAngle = 25.0f;
            light.edgeSoftness = 0.25f;
            spawnLight("TestSpotLight", cameraController.getPosition(), light);
        }
        if (evt.scancode == SDL_Scancode::SDL_SCANCODE_5 && evt.type == SDL_EventType::SDL_EVENT_KEY_DOWN)
        {
            LightComponent::LightDesc light;
            light.type = ELightType::Area;
            light.direction = cameraController.getDirection(); // the quad emits along its Direction
            light.range = 10.0f;
            light.intensity = 10.0f;
            light.width = 1.0f;
            light.height = 1.0f;
            spawnLight("TestAreaLight", cameraController.getPosition(), light);
        }
        if (evt.scancode == SDL_Scancode::SDL_SCANCODE_6 && evt.type == SDL_EventType::SDL_EVENT_KEY_DOWN)
        {
            LightComponent::LightDesc light;
            light.type = ELightType::Tube;
            light.direction = cameraController.getUp(); // tube axis
            light.range = 10.0f;
            light.color = glm::vec3(1.0f, 0.9f, 0.7f);
            light.intensity = 10.0f;
            light.width = 0.1f; // tube radius
            light.length = 1.0f;
            spawnLight("TestTubeLight", cameraController.getPosition(), light);
        }
        // 8/9: throw a dynamic physics cube/sphere from the camera
        if ((evt.scancode == SDL_Scancode::SDL_SCANCODE_8 || evt.scancode == SDL_Scancode::SDL_SCANCODE_9) && evt.type == SDL_EventType::SDL_EVENT_KEY_DOWN)
        {
            const bool cube = evt.scancode == SDL_Scancode::SDL_SCANCODE_8;
            const glm::vec3 dir = cameraController.getDirection();
            const Transform spawnAt(cameraController.getPosition() + dir, 0.25f, glm::normalize(cameraController.getOrientation()));
            if (EntityPtr e = world.spawn(cube ? "physicsCube" : "physicsSphere", spawnAt))
            {
                if (PhysicsComponent* pc = getComponent<PhysicsComponent>(e))
                    pc->body.setLinearVelocity(dir * 12.0f);
                world.addRootEntity(oc::move(e));
            }
        }
        if ((evt.scancode == SDL_Scancode::SDL_SCANCODE_N)
            && evt.type == SDL_EventType::SDL_EVENT_KEY_DOWN && !evt.repeat)
        {
            const glm::vec3 dir = cameraController.getDirection();
            const glm::vec3 pos = cameraController.getPosition() - (dir * reach * 0.5f);
            ForceEmitter emitter = Globals::forceSystem.createEmitter(team, pos, dir, output, reach, focus, distribution, width);
            spawnedForceEmitters.push_back(oc::move(emitter));
        }
        if ((evt.scancode == SDL_Scancode::SDL_SCANCODE_M)
            && evt.type == SDL_EventType::SDL_EVENT_KEY_DOWN && !evt.repeat)
        {
			if (spawnedForceEmitters.empty())
				return;
			ForceEmitter& emitter = spawnedForceEmitters.back();
			emitter.setOutput(output);
			emitter.setReach(reach);
			emitter.setFocus(focus);
			emitter.setTeam(team);
			emitter.setDistribution(distribution);
			emitter.setWidth(width);
        }
        if (evt.scancode == SDL_Scancode::SDL_SCANCODE_B && evt.type == SDL_EventType::SDL_EVENT_KEY_DOWN)
        {
            spawnedForceEmitters.clear();
            spawnedForceQueries.clear();
            forceBalls.clear();
        }
        // H: forcefield stress burst — 500 random emitters (random team/reach/focus) in a 150 m disc
        // around the camera. Press repeatedly to stack toward the 5000-emitter target; B clears.
        if (evt.scancode == SDL_Scancode::SDL_SCANCODE_H && evt.type == SDL_EventType::SDL_EVENT_KEY_DOWN)
        {
            const glm::vec3 center = cameraController.getPosition();
            for (int i = 0; i < 500; ++i)
            {
                const glm::vec2 disc = glm::diskRand(150.0f);
                const glm::vec3 pos = center + glm::vec3(disc.x, glm::linearRand(-4.0f, 12.0f), disc.y);
                const uint32 team2 = (uint32)glm::linearRand(0.0f, 3.99f);
                const float focus2 = glm::linearRand(0.0f, 1.0f) < 0.25f ? glm::linearRand(0.7f, 1.0f) : 0.5f;
                spawnedForceEmitters.push_back(Globals::forceSystem.createEmitter(team2, pos,
                    glm::sphericalRand(1.0f), glm::linearRand(0.5f, 1.5f), glm::linearRand(1.5f, 5.0f), focus2));
            }
        }
        // F: throw a physics sphere carrying a small team-1 forcefield — enemy bubbles knock it back
        // via the GPU force readback (see update()).
        if (evt.scancode == SDL_Scancode::SDL_SCANCODE_F && evt.type == SDL_EventType::SDL_EVENT_KEY_DOWN)
        {
            const glm::vec3 dir = cameraController.getDirection();
            const Transform spawnAt(cameraController.getPosition() + dir, 0.25f, glm::normalize(cameraController.getOrientation()));
            if (EntityPtr e = world.spawn("physicsSphere", spawnAt))
            {
                if (PhysicsComponent* pc = getComponent<PhysicsComponent>(e))
                    pc->body.setLinearVelocity(dir * 12.0f);
                ForceBall ball;
                // Sphere spanning pos .. pos + dir * reach: offset the emitter down so it CENTERS on the ball.
                ball.emitter = Globals::forceSystem.createEmitter(1u, spawnAt.pos - glm::vec3(0, 1, 0), glm::vec3(0, 1, 0), 1.0f, 2.0f);
                ball.entity = e;
                forceBalls.push_back(oc::move(ball));
                world.addRootEntity(oc::move(e));
            }
        }
        // J: drop an 11x11 territory-query grid at the camera's height in front of it ("Force/Debug/
        // Draw queries" colors each point by the team whose bubble contains it, grey = uncontrolled).
        if (evt.scancode == SDL_Scancode::SDL_SCANCODE_J && evt.type == SDL_EventType::SDL_EVENT_KEY_DOWN)
        {
            const glm::vec3 center = cameraController.getPosition() + cameraController.getDirection() * 10.0f;
            for (int x = -5; x <= 5; ++x)
                for (int z = -5; z <= 5; ++z)
                    spawnedForceQueries.push_back(Globals::forceSystem.createQuery(center + glm::vec3(x * 2.0f, 0.0f, z * 2.0f)));
        }

        if (evt.scancode == SDL_Scancode::SDL_SCANCODE_7 && evt.type == SDL_EventType::SDL_EVENT_KEY_DOWN)
        {
            for (int i = 0; i < 100; ++i)
            {
                int x = 0; int y = 0;
                glm::vec3 position = glm::vec3(x * 30.0f + glm::linearRand(-11.0f, 11.0f), glm::linearRand(0.0f, 7.0f), y * 20.0f + glm::linearRand(-5.0f, 4.5f));
                LightComponent::LightDesc light;
                light.range = glm::linearRand(0.5f, 2.0f);
                light.color = glm::abs(glm::sphericalRand(1.0f));
                light.intensity = glm::linearRand(7.0f, 10.0f);
                spawnLight("TestPointLight", position, light);
            }
        }
    }
};