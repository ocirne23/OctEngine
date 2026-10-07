module RendererVK;

import Core;
import Core.Log;
import Settings.Tweaks;
import :PushBlock;

namespace
{
    using EType = PushBlockDetail::EType;

    uint32 scalarCount(EType type)
    {
        switch (type)
        {
        case EType::Vec2: case EType::Ivec2: case EType::Uvec2: return 2;
        case EType::Vec3: return 3;
        case EType::Vec4: return 4;
        case EType::Mat4: return 16;
        default: return 1;
        }
    }

    bool isFloat(EType type) { return type == EType::Float || type == EType::Vec2 || type == EType::Vec3 || type == EType::Vec4 || type == EType::Mat4; }

    bool isFinite(EType type, const uint8* bytes)
    {
        if (!isFloat(type))
            return true;
        for (uint32 s = 0; s < scalarCount(type); ++s)
        {
            float value;
            std::memcpy(&value, bytes + s * 4, 4);
            if (!std::isfinite(value))
                return false;
        }
        return true;
    }

    // %.9g round-trips every float exactly; a GLSL float literal needs a '.' or an exponent.
    void appendValue(oc::string& out, const char* glslType, EType type, const uint8* bytes)
    {
        const uint32 scalars = scalarCount(type);
        if (scalars > 1)
            out += oc::format("{}(", glslType);
        for (uint32 s = 0; s < scalars; ++s)
        {
            if (s > 0)
                out += ", ";
            if (type == EType::Uint || type == EType::Uvec2)
            {
                uint32 value;
                std::memcpy(&value, bytes + s * 4, 4);
                out += oc::format("{}u", value);
            }
            else if (type == EType::Int || type == EType::Ivec2)
            {
                int32 value;
                std::memcpy(&value, bytes + s * 4, 4);
                out += oc::format("{}", value);
            }
            else
            {
                float value;
                std::memcpy(&value, bytes + s * 4, 4);
                char text[32];
                std::snprintf(text, sizeof(text), "%.9g", value);
                out += text;
                if (std::strpbrk(text, ".eE") == nullptr)
                    out += ".0";
            }
        }
        if (scalars > 1)
            out += ")";
    }
}

const char* PushBlock::typeName(EType type)
{
    switch (type)
    {
    case EType::Float: return "float";
    case EType::Int:   return "int";
    case EType::Uint:  return "uint";
    case EType::Vec2:  return "vec2";
    case EType::Vec3:  return "vec3";
    case EType::Vec4:  return "vec4";
    case EType::Ivec2: return "ivec2";
    case EType::Uvec2: return "uvec2";
    case EType::Mat4:  return "mat4";
    case EType::Address: return "uint64_t";
    }
    return "?";
}

// Each lockable value's lock from its sources: locked while every source row is locked (or no lock covers it).
void PushBlock::resolve()
{
    const TweakRegistry& tweaks = TweakRegistry::get();
    for (Entry& e : m_entries)
    {
        if (!e.eval)
            continue;
        ETweakSource source = ETweakSource::Locked;
        for (const TweakRegistry::Source& s : e.sources)
        {
            const ETweakSource state = tweaks.sourceState(s.address, s.size);
            if (state == ETweakSource::Unknown)
                Log::warning(oc::format("Renderer: a source of push value {} is no registered tweak - it is ignored", e.name));
            source = oc::max(source, state == ETweakSource::Unknown ? ETweakSource::Locked : state);
        }
        e.locked = source == ETweakSource::Locked;
    }
}

bool PushBlock::update()
{
    if (m_resolveDirty)
        resolve();
    m_dirty = m_resolveDirty = false;
    bool changed = false;
    for (Entry& e : m_entries)
    {
        if (!e.eval)
            continue;
        e.eval->write(e.value.data());
        const bool bake = e.locked && isFinite(e.type, e.value.data());
        if (!bake)
        {
            changed |= e.baked;
            e.baked = false;
            continue;
        }
        if (e.baked && std::memcmp(e.value.data(), e.bakedValue.data(), e.size) == 0)
            continue;
        e.bakedValue = e.value;
        e.baked = true;
        changed = true;
    }
    return changed;
}

oc::string PushBlock::declaration() const
{
    oc::string out = "// GENERATED (PushBlock::declaration): the compiling pipeline's push block.\n";
    out += "layout (push_constant, scalar) uniform PC\n{\n";
    for (const Entry& e : m_entries)
        out += oc::format("    layout(offset = {}) {} pc_{}{};\n", e.offset, e.glslType, e.name, e.baked ? "Baked" : "");
    out += "};\n";
    for (const Entry& e : m_entries)
        if (e.eval)
            out += oc::format("#define PC_LIVE_{} pc_{}{}\n", e.name, e.name, e.baked ? "Baked" : "");
    for (const Entry& e : m_entries)
    {
        if (!e.baked)
            continue;
        out += oc::format("const {} pc_{} = ", e.glslType, e.name);
        appendValue(out, e.glslType.c_str(), e.type, e.bakedValue.data());
        out += ";\n";
    }
    return out;
}
