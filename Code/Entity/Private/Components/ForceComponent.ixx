export module Entity:ForceComponent;

import :Entity;
import Core;
import Core.glm;
import Core.Transform;
import File;
import Force;

// A forcefield bubble attached to the entity ("Component Force" in .pre files): creates a ForceEmitter
// (Force library) and keeps it on the entity's world transform every update. The field spans
// `Offset` .. `Offset + Direction * Reach` from the emitter, which sits at the START of that span. When
// `centered` is true (the default) the emitter is pulled back half a reach along the axis, so a zero Local
// Offset puts the bubble's centre on the entity (a non-zero offset then shifts it from there); when false the
// span starts at the entity. Reach is world units and does NOT scale with the entity (a gameplay volume, not
// geometry). Gameplay retunes the field and reads the GPU-latched applied force / pressure through `emitter`;
// there is no authored on/off, so a script silences one with emitter.setOutput(0).
export struct ForceComponent
{
    static constexpr EComponentID getId() { return EComponentID_Force; }

    ~ForceComponent() {}

    ForceEmitter emitter;
    glm::vec3 localDirection = glm::vec3(0.0f, 0.0f, -1.0f); // bubble axis in entity space (entity forward)
    glm::vec3 localOffset    = glm::vec3(0.0f);              // offset in entity space, scaled by the entity
    bool centered = true;                                   // true: pull the emitter back Reach/2 so Offset 0 centres the bubble on the entity

    struct SpawnInfo
    {
        uint32 team = 0;                                    // [0, MAX_FORCE_TEAMS); same-team fields merge
        glm::vec3 direction = glm::vec3(0.0f, 0.0f, -1.0f); // local bubble axis; only matters when focus != 0.5
        glm::vec3 offset = glm::vec3(0.0f);                 // local offset from the (centered or span-start) anchor
        float output = 1.0f;       // total field strength; must beat the iso threshold to show a bubble
        float reach = 5.0f;        // total extent along the axis
        float focus = 0.5f;        // 0.5 = sphere spanning the line, 0/1 = cones pointed at emitter/target
        float distribution = 0.5f; // where the density sits along the line (0 = emitter end, 1 = target end)
        float width = 1.0f;        // lateral scale, reach untouched
        bool centered = true;      // pull the emitter back Reach/2 so a zero offset centres the bubble on the entity
        bool mergeable = true;     // the Force library's bubble MERGING (same-team overlapping mergeable
                                   // bubbles ride one GPU emitter; see "Force/Merge" tweaks); false opts out
    };

    void spawn(Entity& entity, const SpawnInfo& info, const Transform& base);
    void destroy(Entity& entity, const SpawnInfo& info);
    void update(Entity& entity, const Transform& world); // emitter follows the entity (position + axis)
};

export const ForceComponent::SpawnInfo* getForceSpawnInfo(const Entity* entity);

// Serializes a force spawn recipe into a "Component Force" node.
export void writeForceSpawnInfo(const ForceComponent::SpawnInfo& info, AssetNode& out);
