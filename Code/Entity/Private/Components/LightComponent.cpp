module Entity;

import Core;
import Core.glm;
import Core.Transform;
import Core.Tweaks;
import :Entity;
import RendererVK;

// Forces every LightComponent's debug wireframes on, regardless of the per-component Debug flag.
static bool s_debugDrawAllLights = false;
static const struct LightTweaks
{
    LightTweaks() { Tweak::boolean("Lights", "Debug geometry", &s_debugDrawAllLights); }
} s_lightTweaks; // TweakRegistry is a function-local static, so registering during static init is safe

const char* lightTypeToken(ELightType type)
{
    switch (type)
    {
    case ELightType::Spot: return "Spot";
    case ELightType::Area: return "Area";
    case ELightType::Tube: return "Tube";
    default:               return "Point";
    }
}

ELightType lightTypeFromToken(oc::string_view token)
{
    if (token == "Spot") return ELightType::Spot;
    if (token == "Area") return ELightType::Area;
    if (token == "Tube") return ELightType::Tube;
    return ELightType::Point;
}

static uint32 packLightColor(const glm::vec3& color, float scale = 1.0f)
{
    const glm::vec3 s = glm::clamp(color * scale, 0.0f, 1.0f) * 255.0f;
    return uint32(s.x) | (uint32(s.y) << 8) | (uint32(s.z) << 16) | 0xFF000000u;
}

// The canonical tangent LightInfo::rotation is measured from (mirrors the convention the renderer's
// area/tube encoding expects, see InputControls' spawns).
static glm::vec3 canonicalTangent(const glm::vec3& axis)
{
    const glm::vec3 ref = glm::abs(axis.y) < 0.999f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
    return glm::normalize(glm::cross(axis, ref));
}

// Roll of `tangent` around `axis`, measured from that canonical frame.
static float rollAroundAxis(const glm::vec3& axis, const glm::vec3& tangent)
{
    const glm::vec3 ref = canonicalTangent(axis);
    return atan2f(glm::dot(glm::cross(ref, tangent), axis), glm::dot(ref, tangent));
}

static glm::vec3 normalizedOr(const glm::vec3& v, const glm::vec3& fallback)
{
    const float len2 = glm::dot(v, v);
    return len2 > 1e-12f ? v * glm::inversesqrt(len2) : fallback;
}

static void drawCircle(Renderer& renderer, const glm::vec3& center, const glm::vec3& u, const glm::vec3& v, float radius, uint32 color)
{
    constexpr int segments = 24;
    glm::vec3 prev = center + u * radius;
    for (int i = 1; i <= segments; ++i)
    {
        const float a = float(i) * (2.0f * glm::pi<float>() / float(segments));
        const glm::vec3 p = center + (u * cosf(a) + v * sinf(a)) * radius;
        renderer.addDebugLine(prev, p, color);
        prev = p;
    }
}

static void drawWireSphere(Renderer& renderer, const glm::vec3& center, float radius, uint32 color)
{
    drawCircle(renderer, center, glm::vec3(1, 0, 0), glm::vec3(0, 1, 0), radius, color);
    drawCircle(renderer, center, glm::vec3(0, 1, 0), glm::vec3(0, 0, 1), radius, color);
    drawCircle(renderer, center, glm::vec3(0, 0, 1), glm::vec3(1, 0, 0), radius, color);
}

// The light's world-space frame. `axis` is what the light POINTS ALONG (cone axis, quad emission
// normal, tube axis); `right`/`up` span the plane across it. For an area light these are exactly the
// quad's width and height axes, which is what the renderer's encoding is built from.
struct LightFrame
{
    glm::vec3 pos;
    glm::vec3 axis;
    glm::vec3 right;
    glm::vec3 up;
};

// Wireframe of what the light actually covers: the marker sphere in the light's colour, its reach in a
// dimmed one.
static void drawLightDebug(Renderer& renderer, const LightComponent::LightDesc& desc, const LightFrame& frame)
{
    const uint32 color = packLightColor(desc.color);
    const uint32 dim = packLightColor(desc.color, 0.35f);
    const glm::vec3& pos = frame.pos;
    const glm::vec3& axis = frame.axis;
    const glm::vec3& tangent = frame.right;
    const glm::vec3& bitangent = frame.up;

    drawWireSphere(renderer, pos, 0.1f, color);

    switch (desc.type)
    {
    case ELightType::Point:
        drawWireSphere(renderer, pos, desc.range, dim);
        break;
    case ELightType::Spot:
    {
        const float half = glm::radians(glm::clamp(desc.coneAngle, 0.0f, 89.0f));
        const glm::vec3 capCenter = pos + axis * (desc.range * cosf(half));
        const float capRadius = desc.range * sinf(half);
        drawCircle(renderer, capCenter, tangent, bitangent, capRadius, dim);
        for (int i = 0; i < 4; ++i)
        {
            const float a = float(i) * (glm::pi<float>() * 0.5f);
            renderer.addDebugLine(pos, capCenter + (tangent * cosf(a) + bitangent * sinf(a)) * capRadius, dim);
        }
        break;
    }
    case ELightType::Area:
    {
        const glm::vec3 hu = tangent * (desc.width * 0.5f);
        const glm::vec3 hv = bitangent * (desc.height * 0.5f);
        const glm::vec3 c0 = pos - hu - hv, c1 = pos + hu - hv, c2 = pos + hu + hv, c3 = pos - hu + hv;
        renderer.addDebugLine(c0, c1, color);
        renderer.addDebugLine(c1, c2, color);
        renderer.addDebugLine(c2, c3, color);
        renderer.addDebugLine(c3, c0, color);
        renderer.addDebugLine(pos, pos + axis * glm::min(desc.range, 1.0f), color); // emission normal
        drawWireSphere(renderer, pos, desc.range, dim);
        break;
    }
    case ELightType::Tube:
    {
        const glm::vec3 a = pos - axis * (desc.length * 0.5f);
        const glm::vec3 b = pos + axis * (desc.length * 0.5f);
        drawCircle(renderer, a, tangent, bitangent, desc.width, color);
        drawCircle(renderer, b, tangent, bitangent, desc.width, color);
        for (int i = 0; i < 4; ++i)
        {
            const float t = float(i) * (glm::pi<float>() * 0.5f);
            const glm::vec3 off = (tangent * cosf(t) + bitangent * sinf(t)) * desc.width;
            renderer.addDebugLine(a + off, b + off, color);
        }
        renderer.addDebugLine(a, b, color);
        drawWireSphere(renderer, pos, desc.range, dim);
        break;
    }
    }
}

void LightComponent::spawn(Entity& entity, const SpawnInfo& info, const Transform& base)
{
    lights = info.lights; // live copy: gameplay retunes these without touching the shared template
    debugDraw = info.debugDraw;
}

void LightComponent::destroy(Entity& entity, const SpawnInfo&)
{
    lights.clear(); // lights are per-frame records; nothing is owned on the renderer side
}

void LightComponent::update(Entity& entity, Renderer& renderer, const Transform& world)
{
    const bool drawDebug = debugDraw || s_debugDrawAllLights;
    for (const LightDesc& desc : lights)
    {
        if (!desc.enabled)
            continue;

        LightFrame frame;
        frame.pos = world.pos + world.quat * (desc.offset * world.scale);
        // `Direction` always means the direction the light POINTS, for every type.
        frame.axis = glm::normalize(world.quat * normalizedOr(desc.direction, glm::vec3(0.0f, 0.0f, -1.0f)));

        if (desc.type == ELightType::Area)
        {
            // The quad's height axis follows the entity's own up, projected into the quad plane, so
            // rotating the entity rolls its area light; `rotation` is extra roll about the NORMAL on top.
            // Degenerate only when the light aims straight along the entity's up - fall back to the
            // canonical tangent there so the frame stays continuous.
            glm::vec3 up = world.quat * glm::vec3(0.0f, 1.0f, 0.0f);
            up -= frame.axis * glm::dot(up, frame.axis);
            up = glm::dot(up, up) > 1e-8f ? glm::normalize(up) : canonicalTangent(frame.axis);
            const float roll = glm::radians(desc.rotation);
            frame.up = glm::normalize(up * cosf(roll) + glm::cross(frame.axis, up) * sinf(roll));
            frame.right = glm::cross(frame.axis, frame.up); // up x right == axis, the normal the shader rebuilds
        }
        else
        {
            frame.right = canonicalTangent(frame.axis);
            frame.up = glm::cross(frame.axis, frame.right);
        }

        switch (desc.type)
        {
        case ELightType::Point:
            renderer.addLightInfo(PointLight(frame.pos, desc.range, desc.color, desc.intensity));
            break;
        case ELightType::Spot:
            renderer.addLightInfo(SpotLight(frame.pos, desc.range, desc.color, desc.intensity, frame.axis,
                glm::radians(desc.coneAngle), desc.edgeSoftness));
            break;
        case ELightType::Area:
            // The renderer encodes an area light as its HEIGHT axis (direction, magnitude = height) plus
            // the roll of the width axis about it - the emission normal is cross(up, right), rebuilt from
            // those. So the authored aim goes in through the frame, not through `direction`.
            renderer.addLightInfo(AreaLight(frame.pos, desc.range, desc.color, desc.intensity, frame.up,
                desc.width, desc.height, rollAroundAxis(frame.up, frame.right)));
            break;
        case ELightType::Tube:
        {
            // A capsule is radially symmetric, so `rotation` is unused here (the shader never reads it).
            renderer.addLightInfo(TubeLight(frame.pos, desc.range, desc.color, desc.intensity, frame.axis,
                desc.width, desc.length));
            break;
        }
        }

        if (drawDebug)
            drawLightDebug(renderer, desc, frame);
    }
}

const LightComponent::SpawnInfo* getLightSpawnInfo(const Entity* entity)
{
    if (!entity->spawnTemplate || !hasComponent<LightComponent>(entity))
        return nullptr;

    size_t idx = 0;
    for (uint16 i = 0; i < uint16(EComponentID_Light); ++i)
        if (entity->typeBits & (1 << i))
            ++idx;
    if (idx >= entity->spawnTemplate->spawnInfos.size())
        return nullptr;
    return static_cast<const LightComponent::SpawnInfo*>(entity->spawnTemplate->spawnInfos[idx].get());
}

void writeLightSpawnInfo(const LightComponent::SpawnInfo& info, AssetNode& out)
{
    const LightComponent::LightDesc defaults;
    if (info.debugDraw)
        out.set("Debug", info.debugDraw);
    for (const LightComponent::LightDesc& desc : info.lights)
    {
        AssetNode& lightNode = out.addChild("Light");
        lightNode.values.emplace_back(lightTypeToken(desc.type));
        lightNode.set("Color", desc.color);
        lightNode.set("Intensity", desc.intensity);
        lightNode.set("Range", desc.range);
        if (desc.offset != defaults.offset)       lightNode.set("Offset", desc.offset);
        if (desc.type != ELightType::Point && desc.direction != defaults.direction)
            lightNode.set("Direction", desc.direction);
        if (desc.type == ELightType::Spot)
        {
            lightNode.set("ConeAngle", desc.coneAngle);
            if (desc.edgeSoftness != defaults.edgeSoftness) lightNode.set("EdgeSoftness", desc.edgeSoftness);
        }
        if (desc.type == ELightType::Area)
        {
            lightNode.set("Width", desc.width);
            lightNode.set("Height", desc.height);
        }
        if (desc.type == ELightType::Tube)
        {
            lightNode.set("Radius", desc.width);
            lightNode.set("Length", desc.length);
        }
        if (desc.type == ELightType::Area && desc.rotation != defaults.rotation)
            lightNode.set("Rotation", desc.rotation);
        if (!desc.enabled)
            lightNode.set("Enabled", desc.enabled);
    }
}
