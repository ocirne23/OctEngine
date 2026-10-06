module RendererVK;

import Core;
import Core.glm;
import :Layout;
import :UboFields;

// The frame UBO's GLSL declaration. The block is FLAT: the root struct's members (OC_UBO_* field tables, Layout.ixx)
// with a non-array struct member opened into its fields (u_<member>_<field>), then the lockable values of the
// UboFieldList (u_<name>), every member at its byte offset (an explicit layout(offset)). Only a struct ARRAY
// (u_views[]) keeps a struct type. A BAKED entry's member keeps its slot under another name (u_<name>Baked) and a
// const of its type, named like the member, carries its value - every read of it folds.

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
        case EUboType::Struct: return "?";
        }
        return "?";
    }
    const char* glslType(const UboField& field)
    {
        return field.type == EUboType::Struct ? field.structInfo()->name : glslType(field.type);
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
        case EUboType::Struct: return 0;
        }
        return 0;
    }

    // A struct an array member needs (and the structs inside it), once.
    void declareStruct(const UboStructInfo& info, oc::string& out, oc::vector<const UboStructInfo*>& declared)
    {
        for (const UboStructInfo* done : declared)
            if (done == &info)
                return;
        for (uint32 i = 0; i < info.numFields; ++i)
            if (info.fields[i].type == EUboType::Struct)
                declareStruct(*info.fields[i].structInfo(), out, declared);
        declared.push_back(&info);
        out += oc::format("struct {}\n{{\n", info.name);
        for (uint32 i = 0; i < info.numFields; ++i)
        {
            const UboField& field = info.fields[i];
            out += oc::format("    {} {}", glslType(field), field.name);
            if (field.count > 0)
                out += oc::format("[{}]", field.count);
            out += ";\n";
        }
        out += "};\n";
    }

    // The root's block members, flattened: a non-array struct field opens its own members (name joined by '_').
    void appendRootMembers(const UboStructInfo& info, uint32 base, const oc::string& prefix, oc::string& structs,
        oc::string& members, oc::vector<const UboStructInfo*>& declared)
    {
        for (uint32 i = 0; i < info.numFields; ++i)
        {
            const UboField& field = info.fields[i];
            const uint32 offset = base + field.offset;
            const oc::string name = prefix + field.name;
            if (field.type == EUboType::Struct && field.count == 0)
            {
                appendRootMembers(*field.structInfo(), offset, name + "_", structs, members, declared);
                continue;
            }
            if (field.type == EUboType::Struct)
                declareStruct(*field.structInfo(), structs, declared);
            members += oc::format("    layout(offset = {}) {} u_{}", offset, glslType(field), name);
            if (field.count > 0)
                members += oc::format("[{}]", field.count);
            members += ";\n";
        }
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
    oc::string buildUboDeclaration(const UboFieldList& fields, const uint8* bakedValues, oc::span<const uint8> baked)
    {
        const oc::vector<UboFieldList::Entry>& entries = fields.entries();
        oc::string out;
        out.reserve(64 * 1024);
        out += "// GENERATED from the OC_UBO_* field lists (Layout.ixx) and Renderer::registerUboFields (buildUboDeclaration).\n";
        out += "// The shader includer serves it as ubo.generated.glsl; this copy is for reading only.\n";
        oc::string structs, members;
        oc::vector<const UboStructInfo*> declared;
        appendRootMembers(*Ubo::info(), 0, "", structs, members, declared);
        for (size_t i = 0; i < entries.size(); ++i)
            members += oc::format("    layout(offset = {}) {} u_{}{};\n", UBO_FIELDS_OFFSET + entries[i].offset, glslType(entries[i].type),
                entries[i].name, baked[i] != 0 ? "Baked" : "");
        out += structs;
        out += "\nlayout (binding = UBO_BINDING, std140) uniform UBO\n{\n";
        out += members;
        out += "};\n";

        // UBO_LIVE_<name>: the block member in both modes - a shader reads one lockable value live even while it is
        // baked (UBO_LIVE_rt_giStrength), where a baked value costs more than it saves.
        out += "\n";
        for (size_t i = 0; i < entries.size(); ++i)
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
