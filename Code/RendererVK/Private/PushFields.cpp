module RendererVK;

import Core;
import Core.Log;
import Settings.Tweaks;
import :PushFields;

namespace
{
    using EType = PushFieldsDetail::EType;

    const char* glslType(EType type)
    {
        switch (type)
        {
        case EType::Float: return "float";
        case EType::Int:   return "int";
        case EType::Uint:  return "uint";
        case EType::Vec2:  return "vec2";
        case EType::Vec3:  return "vec3";
        case EType::Vec4:  return "vec4";
        case EType::Uvec2: return "uvec2";
        }
        return "?";
    }

    uint32 scalarCount(EType type)
    {
        switch (type)
        {
        case EType::Vec2: case EType::Uvec2: return 2;
        case EType::Vec3: return 3;
        case EType::Vec4: return 4;
        default: return 1;
        }
    }

    bool isFloat(EType type) { return type == EType::Float || type == EType::Vec2 || type == EType::Vec3 || type == EType::Vec4; }

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
    void appendValue(oc::string& out, EType type, const uint8* bytes)
    {
        const uint32 scalars = scalarCount(type);
        if (scalars > 1)
            out += oc::format("{}(", glslType(type));
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
            else if (type == EType::Int)
            {
                int32 value;
                std::memcpy(&value, bytes, 4);
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

// Each entry's lock from its sources: locked while every source row is locked (or no lock covers it).
void PushFieldList::resolve()
{
    const TweakRegistry& tweaks = TweakRegistry::get();
    for (Entry& e : m_entries)
    {
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

bool PushFieldList::update()
{
    if (m_resolveDirty)
        resolve();
    m_dirty = m_resolveDirty = false;
    bool changed = false;
    for (Entry& e : m_entries)
    {
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

void PushFieldList::appendDefines(oc::vector<ShaderDefine>& defines) const
{
    oc::string consts;
    for (const Entry& e : m_entries)
    {
        const char* suffix = e.baked ? "Baked" : "";
        defines.push_back(ShaderDefine{ "PC_DECL_" + e.name, oc::format("{} pc_{}{}", glslType(e.type), e.name, suffix) });
        defines.push_back(ShaderDefine{ "PC_LIVE_" + e.name, oc::format("pc_{}{}", e.name, suffix) });
        if (!e.baked)
            continue;
        consts += oc::format("const {} pc_{} = ", glslType(e.type), e.name);
        appendValue(consts, e.type, e.bakedValue.data());
        consts += "; ";
    }
    defines.push_back(ShaderDefine{ "PC_CONSTS", consts });
}
