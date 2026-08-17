export module Entity:LightComponent;

import :Entity;
import Core;
import Core.glm;
import Core.Transform;
import File;
import RendererVK;

// Which LightInfo encoding a light authored on an entity resolves to (see RendererVK:Light).
export enum class ELightType : uint8
{
    Point, // omnidirectional; direction/rotation unused
    Spot,  // cone along direction, coneAngle half-angle + edgeSoftness falloff
    Area,  // rectangle of width x height facing direction, rolled by rotation
    Tube,  // capsule of radius `width` and `length` along direction, rolled by rotation
};

export const char* lightTypeToken(ELightType type);
export ELightType lightTypeFromToken(oc::string_view token);

// One or more lights attached to the entity ("Component Light" in .pre files): every update each
// enabled light is pushed to the renderer for this frame (renderer.addLightInfo, lock-free, so this
// runs inside the parallel entity pass like everything else). Lights are per-frame records, not
// renderer-owned slots, so there is nothing to create or destroy - the component only holds the
// authored descriptions and the live per-light state gameplay edits.
//
// Each light sits at `offset` in entity space (scaled+rotated by the entity) and aims along
// `direction` in entity space; `range`/`width`/`height`/`length` are WORLD units and do NOT scale
// with the entity (a light's reach is a gameplay quantity, not geometry - same rule as Force).
// `debugDraw` (per component, plus the "Lights/Debug geometry" tweak that forces it on globally)
// overlays wireframes of what each light actually covers.
export struct LightComponent
{
    static constexpr EComponentID getId() { return EComponentID_Light; }

    ~LightComponent() {}

    struct LightDesc
    {
        ELightType type = ELightType::Point;
        glm::vec3 offset = glm::vec3(0.0f);                      // entity-space position
        glm::vec3 direction = glm::vec3(0.0f, 0.0f, -1.0f);      // entity-space direction the light POINTS (Spot cone axis, Area emission normal, Tube axis)
        glm::vec3 color = glm::vec3(1.0f);
        float intensity = 20.0f;
        float range = 10.0f;         // influence radius
        float coneAngle = 25.0f;     // Spot: cone HALF-angle, degrees
        float edgeSoftness = 0.1f;   // Spot: cone edge falloff width
        float width = 1.0f;          // Area: quad width / Tube: capsule radius
        float height = 1.0f;         // Area: quad height
        float length = 1.0f;         // Tube: capsule length
        float rotation = 0.0f;       // Area: extra roll about the aim axis, degrees (a Tube is radially symmetric)
        bool enabled = true;
    };

    struct SpawnInfo
    {
        oc::vector<LightDesc> lights;
        bool debugDraw = false;
    };

    oc::vector<LightDesc> lights; // live copy of the authored set; gameplay edits these in place
    bool debugDraw = false;

    void spawn(Entity& entity, const SpawnInfo& info, const Transform& base);
    void destroy(Entity& entity, const SpawnInfo& info);
    void update(Entity& entity, Renderer& renderer, const Transform& world); // pushes this frame's lights

    // Convenience for gameplay: null when the index is out of range.
    LightDesc* find(size_t index) { return index < lights.size() ? &lights[index] : nullptr; }
};

export const LightComponent::SpawnInfo* getLightSpawnInfo(const Entity* entity);

// Serializes a light spawn recipe into a "Component Light" node.
export void writeLightSpawnInfo(const LightComponent::SpawnInfo& info, AssetNode& out);
