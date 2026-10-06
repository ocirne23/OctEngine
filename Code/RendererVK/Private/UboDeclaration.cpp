module RendererVK;

import Core;
import Core.glm;
import :Layout;
import :UboBlock;

// The frame UBO's GLSL declaration: the UboBlock's entries as a FLAT block of u_<name>, every member at its byte
// offset (an explicit layout(offset)), an array as u_<name>[count]. A BAKED entry's member keeps its slot under
// another name (u_<name>Baked) and a const of its type, named like the member, carries its value - every read of it
// folds.

namespace
{
    using namespace RendererVKLayout;

    const char* glslType(EUboType type)
    {
        switch (type)
        {
        case EUboType::Float:  return "float";
        case EUboType::Uint:   return "uint";
        case EUboType::Vec2:   return "vec2";
        case EUboType::Vec3:   return "vec3";
        case EUboType::Vec4:   return "vec4";
        case EUboType::Uvec4:  return "uvec4";
        case EUboType::Mat4:   return "mat4";
        }
        return "?";
    }

    uint32 scalarCount(EUboType type)
    {
        switch (type)
        {
        case EUboType::Float: case EUboType::Uint: return 1;
        case EUboType::Vec2:  return 2;
        case EUboType::Vec3:  return 3;
        case EUboType::Vec4: case EUboType::Uvec4: return 4;
        case EUboType::Mat4:  return 16;
        }
        return 0;
    }

    // %.9g round-trips every float exactly; a GLSL float literal needs a '.' or an exponent.
    void appendFloat(oc::string& out, float value)
    {
        char text[32];
        std::snprintf(text, sizeof(text), "%.9g", value);
        out += text;
        if (std::strpbrk(text, ".eE") == nullptr)
            out += ".0";
    }

    void appendValue(oc::string& out, EUboType type, const uint8* bytes)
    {
        const uint32 scalars = scalarCount(type);
        if (scalars > 1)
            out += oc::format("{}(", glslType(type));
        for (uint32 s = 0; s < scalars; ++s)
        {
            if (s > 0)
                out += ", ";
            if (type == EUboType::Uint || type == EUboType::Uvec4)
            {
                uint32 value;
                std::memcpy(&value, bytes + s * 4, 4);
                out += oc::format("{}u", value);
            }
            else
            {
                float value;
                std::memcpy(&value, bytes + s * 4, 4);
                appendFloat(out, value);
            }
        }
        if (scalars > 1)
            out += ")";
    }
}

namespace RendererVKLayout
{
    oc::string buildUboDeclaration(const UboBlock& block, const uint8* bakedValues, oc::span<const uint8> baked)
    {
        const oc::vector<UboBlock::Entry>& entries = block.entries();
        oc::string out;
        out.reserve(64 * 1024);
        out += "// GENERATED from UboRoot + Renderer::registerUboFields (UboBlock, buildUboDeclaration).\n";
        out += "// The shader includer serves it as ubo.generated.glsl; this copy is for reading only.\n";
        out += "\nlayout (binding = UBO_BINDING, std140) uniform UBO\n{\n";
        for (size_t i = 0; i < entries.size(); ++i)
        {
            const UboBlock::Entry& e = entries[i];
            out += oc::format("    layout(offset = {}) {} u_{}{}", e.offset, glslType(e.type), e.name, baked[i] != 0 ? "Baked" : "");
            if (e.count > 0)
                out += oc::format("[{}]", e.count);
            out += ";\n";
        }
        out += "};\n";

        // UBO_LIVE_<name>: the block member in both modes - a shader reads one lockable value live even while it is
        // baked (UBO_LIVE_rt_giStrength), where a baked value costs more than it saves.
        out += "\n";
        for (size_t i = 0; i < entries.size(); ++i)
            if (entries[i].eval)
                out += oc::format("#define UBO_LIVE_{} u_{}{}\n", entries[i].name, entries[i].name, baked[i] != 0 ? "Baked" : "");

        out += "\n// LOCKED: every tweak these values come from is locked - their values as constants.\n";
        for (size_t i = 0; i < entries.size(); ++i)
        {
            if (baked[i] == 0)
                continue;
            out += oc::format("const {} u_{} = ", glslType(entries[i].type), entries[i].name);
            appendValue(out, entries[i].type, bakedValues + entries[i].offset);
            out += ";\n";
        }
        return out;
    }

    bool isUboValueFinite(EUboType type, const uint8* bytes)
    {
        if (type == EUboType::Uint || type == EUboType::Uvec4)
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
}
