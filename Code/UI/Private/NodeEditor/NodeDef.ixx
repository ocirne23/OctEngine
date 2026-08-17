export module UI:NodeDef;

import Core;

namespace NodeEditor
{

export const char INDENT_UP   = '\x01';
export const char INDENT_DOWN = '\x02';
export const char HOIST = '\x03';
export const char ENUM_TOKEN = '\x04';

// Data carried on a pin / connection. Exec is control flow; the rest are value types.
// Wildcard is an unresolved generic pin that adopts the concrete type it gets connected to.
export enum EDataType : uint8
{
    Exec,
    Bool,
    Int,
    Float,
    Vec3,
    Quat,
    String,
    Entity,   // opaque Entity* handle (script-side pointer, never dereferenced by graph code directly)
    Pointer,  // opaque component pointer (e.g. ScriptComponent*), produced by Get Component; only in scope on its true branch
    Wildcard,
};

// Read/write capability of a pin's value. On an INPUT it is what the node requires of its source; on an
// OUTPUT it is what the produced value supports (a plain rvalue is Readable; a variable is ReadWritable).
// A link is allowed only if the output provides every capability the input requires (see mutableBits).
export enum class EMutableType : uint8
{
    Readable,
    Writable,
    ReadWritable
};

// read = 1, write = 2. A connection output->input is allowed when the output provides every capability the
// input requires: (inputBits & ~outputBits) == 0.
export uint8 mutableBits(EMutableType m)
{
    switch (m)
    {
        case EMutableType::Readable:     return 1;
        case EMutableType::Writable:     return 2;
        case EMutableType::ReadWritable: return 3;
    }
    return 1;
}

// One pin on a node definition. defaultValue is the C++ literal used for an unconnected data input â€” EXCEPT
// for a String pin, whose default is stored as raw text (no surrounding quotes; the editor edits it directly)
// and gets wrapped into a C++ string literal at codegen time (see emitDataExpr).
// typeGroup > 0 marks a wildcard pin; all pins on a node sharing a group resolve to the same concrete type.
export struct PinDef
{
    oc::string  name;
    EDataType    type;
    oc::string  defaultValue;
    int          typeGroup = 0;
    oc::string  expr;        // for an output pin: its value expression (overrides the node-level emit)
    EMutableType mutability = EMutableType::Readable; // read/write capability; restricts what may connect here
};

// A node type the editor can instantiate. `emit` is a C++ template:
//   $k  -> expression for input pin k (the connected output's expression, else its default literal)
//   #k  -> the statement block produced by following exec output pin k
//   ?k  {...} -> conditional block: emit the contents if input pin k is connected, else skip it
//   @   -> replaced with unique node idx
// Data nodes (isExec == false) have no exec pins and `emit` is a single value expression for output 0.
// An exec node that also produces a value (e.g. Conditional) puts that value expression on the data output
// pin's `expr` (PinDef::expr), since `emit` on an exec node is a statement, not an expression.
export struct NodeDef
{
    oc::string          typeId;       // stable id stored in the file (no spaces)
    oc::string          displayName;  // shown on the node + palette
    oc::string          category;     // palette grouping
    bool                 isExec = false;
    oc::vector<PinDef>  inputs;
    oc::vector<PinDef>  outputs;
    oc::string          emit;
    oc::vector<oc::string> enumOptions; // dropdown labels for the node's property (empty = no property)
    oc::vector<oc::string> enumTokens;  // code token per option, parallel to enumOptions, substituted for ENUM_TOKEN
};

export uint32 dataTypeColor(EDataType type)
{
    auto rgb = [](uint8 r, uint8 g, uint8 b) -> uint32 { return 0xFF000000u | (uint32(b) << 16) | (uint32(g) << 8) | uint32(r); };
    switch (type)
    {
        case EDataType::Exec:   return rgb(255, 255, 255);
        case EDataType::Bool:   return rgb(255, 80, 80);
        case EDataType::Int:    return rgb(80, 220, 220);
        case EDataType::Float:  return rgb(140, 255, 140);
        case EDataType::Vec3:   return rgb(255, 215, 80);
        case EDataType::Quat:   return rgb(180, 130, 255);
        case EDataType::String: return rgb(255, 120, 255);
        case EDataType::Entity: return rgb(255, 150, 60);
        case EDataType::Pointer: return rgb(110, 140, 200);
        case EDataType::Wildcard: return rgb(150, 150, 150);
    }
    return rgb(200, 200, 200);
}

// The Script Data node is special: user-defined members instead of static pins (see nodeRegistry / codegen).
export bool isScriptDataType(oc::string_view typeId) { return typeId == "ScriptData"; }

// The Label node is a resizable comment box (a group). It has no pins and emits no code â€” it only organizes
// and documents the graph, and drags any nodes inside it along when moved.
export bool isLabelType(oc::string_view typeId) { return typeId == "Label"; }

// The Reroute node is a draggable waypoint on a link: one input + one output pin of the carried type, drawn
// as a small dot. It emits no code â€” codegen passes straight through it (see realSourceOfInput). Created by
// double-clicking a link, not from the palette.
export bool isRerouteType(oc::string_view typeId) { return typeId == "Reroute"; }

// The On Event node is special like Script Data: user-defined named entries instead of static pins, each one
// a plain Exec output. Multiple On Event nodes stay in sync (same entry set); codegen dispatches a fired
// event name to the matching entry's exec chain (see Scene::generateCpp / applyEventEdit).
export bool isEventEntryType(oc::string_view typeId) { return typeId == "OnEvent"; }

// The Trigger Audio node is special like On Event, but with dynamic exec INPUT pins: one per Sound alias
// of the target entity's AudioComponent (synced from the selected entity by the UI, serialized as
// //@audioentry lines). Flow entering an alias pin plays that sound; codegen picks the alias from the
// entered pin (see emitExecChain's enteredPin).
export bool isTriggerAudioType(oc::string_view typeId) { return typeId == "TriggerAudio"; }

// Function boundary + call nodes. A .scr can define multiple named functions; each is a (Function Input,
// Function Output) pair sharing a function name. Function Input holds a fixed Exec output plus user-defined
// typed OUTPUT pins (the parameters); Function Output holds a fixed Exec input plus user-defined typed INPUT
// pins (the return values). A Function Call node (created by importing a function, hidden from the palette)
// mirrors that signature and, at codegen, expands to a real C++ function call. See Scene::generateCpp.
export bool isFunctionInputType(oc::string_view typeId)  { return typeId == "FunctionInput"; }
export bool isFunctionOutputType(oc::string_view typeId) { return typeId == "FunctionOutput"; }
export bool isFunctionCallType(oc::string_view typeId)   { return typeId == "FunctionCall"; }

// Serialization token for any pin data type (covers Exec/String too, unlike memberTypeToken).
export const char* dataTypeToken(EDataType type)
{
    switch (type)
    {
        case EDataType::Exec:     return "Exec";
        case EDataType::Bool:     return "Bool";
        case EDataType::Int:      return "Int";
        case EDataType::Float:    return "Float";
        case EDataType::Vec3:     return "Vec3";
        case EDataType::Quat:     return "Quat";
        case EDataType::String:   return "String";
        case EDataType::Entity:   return "Entity";
        case EDataType::Pointer:  return "Pointer";
        case EDataType::Wildcard: return "Wild";
    }
    return "Float";
}

export EDataType dataTypeFromToken(oc::string_view token)
{
    for (int i = 0; i <= (int)EDataType::Wildcard; ++i)
        if (token == dataTypeToken((EDataType)i))
            return (EDataType)i;
    return EDataType::Float;
}

// Value types a Script Data member (persistent per-instance struct field) may have. Deliberately excluded:
// String (an oc::string can't live in the POD block that crosses the script ABI) and the raw handle types
// Entity / Pointer — a component/entity handle persisted across frames would dangle once its target is
// destroyed, so handles must be re-fetched each frame (see entityGetForceComponent), never stored.
export inline constexpr EDataType memberTypes[] = { EDataType::Int, EDataType::Float, EDataType::Bool, EDataType::Vec3 };

// Enforce that exclusion so a handle can never be persisted: adding Entity/Pointer (or Exec/String) to
// memberTypes above would let a graph stash a pointer that outlives its target, and fails this build instead.
constexpr bool memberTypesAreStorablePod()
{
    for (EDataType t : memberTypes)
        if (t == EDataType::Exec || t == EDataType::String || t == EDataType::Entity || t == EDataType::Pointer)
            return false;
    return true;
}
static_assert(memberTypesAreStorablePod(), "Script Data members must be POD value types — no handles (Entity/Pointer) or Exec/String");

// C++ type name for a member field, used both for the struct declaration and the in-node type button label.
export const char* memberCppType(EDataType type)
{
    switch (type)
    {
        case EDataType::Int:  return "int";
        case EDataType::Float: return "float";
        case EDataType::Bool: return "bool";
        case EDataType::Vec3: return "glm::vec3";
        default:              return "float";
    }
}

// Short token stored in the //@member serialization line (round-trips a member's type).
export const char* memberTypeToken(EDataType type)
{
    switch (type)
    {
        case EDataType::Int:  return "int";
        case EDataType::Float: return "float";
        case EDataType::Bool: return "bool";
        case EDataType::Vec3: return "Vec3";
        default:              return "float";
    }
}

export EDataType memberTypeFromToken(oc::string_view token)
{
    for (EDataType t : memberTypes)
        if (token == memberTypeToken(t))
            return t;
    return EDataType::Float;
}

// C++ literal used for an unconnected data input of the given type. A String's default is RAW text (no
// quotes) â€” codegen wraps it into a literal (see emitDataExpr) â€” so its default here is the empty string.
export oc::string defaultValueForType(EDataType type)
{
    switch (type)
    {
        case EDataType::Bool:   return "false";
        case EDataType::Int:    return "0";
        case EDataType::Float:  return "0.0f";
        case EDataType::Vec3:   return "glm::vec3{ 0.0f, 0.0f, 0.0f }";
        case EDataType::Quat:   return "glm::quat{ 1.0f, 0.0f, 0.0f, 0.0f }"; // identity (w,x,y,z)
        case EDataType::String: return "";
        case EDataType::Entity: return "self";
        case EDataType::Pointer: return "nullptr"; // Pointer pins are output-only; no input default is ever emitted
        default:                return "0.0f";
    }
}

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? char(c + 32) : c; }

static bool iequals(oc::string_view a, oc::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (lower(a[i]) != lower(b[i]))
            return false;
    return true;
}

// "default" is a sentinel literal accepted in any inline default box, regardless of the pin's type: it
// means "use this pin's/type's engine default" and stays valid across a wildcard pin's type changing (see
// resolveNodeTypes) since it is resolved to a concrete literal only at codegen time (see emitDataExpr).
export bool isDefaultToken(const oc::string& text)
{
    return iequals(text, "default");
}

// Canonical C++ float literal for a value (e.g. 5.0f -> "5.0f"). to_chars omits the decimal point for whole
// numbers (e.g. "5"), but a bare digit-sequence plus an 'f' suffix isn't a valid C++ floating literal
// (needs a '.' or exponent), so one is added when missing.
export oc::string formatFloatLiteral(float value)
{
    char buf[32];
    const auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), value);
    oc::string formatted(buf, ptr);
    if (formatted.find_first_of(".eE") == oc::string::npos)
        formatted += ".0";
    return formatted + "f";
}

// Parses one float literal token (an optional trailing f/F suffix, otherwise whatever std::from_chars
// accepts). Used both for a plain Float pin and for each component of a Vec3 pin's literal.
export bool parseFloatToken(oc::string_view text, float& outValue)
{
    if (!text.empty() && (text.back() == 'f' || text.back() == 'F'))
        text.remove_suffix(1);
    if (text.empty())
        return false;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), outValue);
    return result.ec == std::errc() && result.ptr == text.data() + text.size();
}

// Extracts the x/y/z components out of a Vec3 pin's default text ("glm::vec3{ x, y, z }") for the inline
// X/Y/Z boxes. Tolerant of the "default" sentinel and anything else that doesn't parse â€” falls back to 0.
export oc::array<float, 3> parseVec3Literal(const oc::string& text)
{
    oc::array<float, 3> v{ 0.0f, 0.0f, 0.0f };
    const size_t open = text.find('{');
    const size_t close = text.find('}');
    if (open == oc::string::npos || close == oc::string::npos || close <= open)
        return v;
    oc::string_view inner(text.data() + open + 1, close - open - 1);
    for (int comp = 0; comp < 3; ++comp)
    {
        const size_t comma = inner.find(',');
        oc::string_view tok = inner.substr(0, comma);
        while (!tok.empty() && tok.front() == ' ') tok.remove_prefix(1);
        while (!tok.empty() && tok.back() == ' ') tok.remove_suffix(1);
        parseFloatToken(tok, v[comp]);
        if (comma == oc::string_view::npos) break;
        inner.remove_prefix(comma + 1);
    }
    return v;
}

// Formats three components back into the canonical Vec3 literal (see defaultValueForType).
export oc::string formatVec3Literal(const oc::array<float, 3>& v)
{
    return "glm::vec3{ " + formatFloatLiteral(v[0]) + ", " + formatFloatLiteral(v[1]) + ", " + formatFloatLiteral(v[2]) + " }";
}

// Validates and reformats text typed into an inline default box into a canonical literal for `type`
// (e.g. "3" -> "3.0f", "TRUE" -> "true"). Returns nullopt if the text isn't a valid literal for that type,
// so the caller can reject the edit instead of committing something that breaks codegen.
export oc::optional<oc::string> formatLiteral(EDataType type, const oc::string& text)
{
    if (isDefaultToken(text))
        return oc::string("default");

    switch (type)
    {
        case EDataType::Bool:
        {
            if (iequals(text, "true"))  return oc::string("true");
            if (iequals(text, "false")) return oc::string("false");
            return oc::nullopt;
        }
        case EDataType::Int:
        {
            int value = 0;
            const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
            if (result.ec != std::errc() || result.ptr != text.data() + text.size())
                return oc::nullopt;
            return oc::to_string(value);
        }
        case EDataType::Float:
        {
            float value = 0.0f;
            if (!parseFloatToken(text, value))
                return oc::nullopt;
            return formatFloatLiteral(value);
        }
        case EDataType::String:
            return text; // any raw text is a valid string literal body
        default:
            return text; // Vec3/Quat/Entity/Wildcard: free-form expression / fixed value, not validated here
    }
}

// The full palette of node types. Returned by const ref so pointers into it stay stable.
export const oc::vector<NodeDef>& nodeRegistry()
{
    static const oc::vector<NodeDef> registry = []{
    using D = EDataType;
    oc::vector<NodeDef> r;

    // ---- organization ----
    // Comment/label box (isLabelType): a movable, resizable group with an editable caption. Pure annotation,
    // no pins, no generated code â€” nodes dragged inside it move with it.
    r.push_back({ "Label", "Label", "Debug", false, {}, {}, "" });

    // Reroute waypoint (isRerouteType): pins are added dynamically to match the link it's placed on; created
    // by double-clicking a link, so it's hidden from the palette.
    r.push_back({ "Reroute", "Reroute", "", false, {}, {}, "" });

    // ---- events / control flow ----
    r.push_back({ "Update", "Update", "Events", true,
        {}, { { "", D::Exec, "" } },
        "#0" });

    r.push_back({ "OnSpawn", "On Spawn", "Events", true,
        {}, { { "", D::Exec, "" } },
        "#0" });

    r.push_back({ "OnDestroy", "On Destroy", "Events", true,
        {}, { { "", D::Exec, "" } },
        "#0" });

    // On Event is special-cased (isEventEntryType): its exec output pins are user-defined named entries,
    // added/removed/renamed through the editor and fired at runtime by name (ScriptComponent::fireEvent).
    r.push_back({ "OnEvent", "On Event", "Events", true, {}, {}, "" });

    // On Physics Event: a fixed entry point (like OnSpawn/Update, not a named-entry node like On Event) for
    // this entity's PhysicsComponent contact/sensor begin/end events (dispatchPhysicsContactEvents /
    // ScriptComponent::firePhysicsEvent). ContactId feeds Get Contact Point for the world-space hit position,
    // valid only until the next physics step. Pin names ("physOther" etc) must match the parameter names
    // Scene::generateCpp gives the generated OnPhysicsEvent function.
    r.push_back({ "OnPhysicsEvent", "On Physics Event", "Events", true,
        {},
        { { "", D::Exec, "" },
            { "Other",     D::Entity, "", 0, "physOther" },
            { "Begin",     D::Bool,   "", 0, "(physBegin != 0)" },
            { "Sensor",    D::Bool,   "", 0, "(physSensor != 0)" },
            { "ContactId", D::Int,    "", 0, "physContactId" } },
        "#0" });

    r.push_back({ "If", "If", "Flow", true,
        { { "", D::Exec, "" }, { "Cond", D::Wildcard, "0.0f", 1 }, { "Comp", D::Wildcard, "0.0f", 1 } },
        { { "true", D::Exec, "" }, { "break", D::Exec, "" } },
        "if ($1 " + oc::string(1, ENUM_TOKEN) + " $2)\n{\n" + oc::string(1, INDENT_UP) + "#0" + oc::string(1, INDENT_DOWN) + "}\n#1",
        { "Less than", "Greater than", "Equals", "Not Equals" },
        { "<", ">", "==", "!=" } });

    r.push_back({ "IfElse", "If Else", "Flow", true,
        { { "", D::Exec, "" }, { "Cond", D::Wildcard, "0.0f", 1 }, { "Comp", D::Wildcard, "0.0f", 1 } },
        { { "true", D::Exec, "" }, { "false", D::Exec, "" }, { "break", D::Exec, "" } },
        "if ($1 " + oc::string(1, ENUM_TOKEN) + " $2)\n{\n" + oc::string(1, INDENT_UP) + "#0" + oc::string(1, INDENT_DOWN) + "}\nelse\n{\n" + oc::string(1, INDENT_UP) + "#1" + oc::string(1, INDENT_DOWN) + "}\n#2",
        { "Less than", "Greater than", "Equals", "Not Equals" },
        { "<", ">", "==", "!=" } });
    
    r.push_back({ "ForLoop", "For Loop", "Flow", true,
        { { "", D::Exec, "" }, { "start", D::Int, "0" }, { "count", D::Int, "10" } },
        { { "body", D::Exec, "" }, { "break", D::Exec, "" }, { "idx", D::Int, "", 0, "i@"}},
        "for (int i@ = $1; i@ " + oc::string(1, ENUM_TOKEN) + " $2; ++i@)\n{\n" + oc::string(1, INDENT_UP) + "#0" + oc::string(1, INDENT_DOWN) + "}\n#1",
        { "Less than", "Less or Equals", "Equals", "Greater or Equals", "Greater than", "Not Equals" },
        { "<", "<=", "==", ">=", ">", "!=" } });

    r.push_back({ "Break", "Break", "Flow", true,
        {{ "", D::Exec, "" }}, 
        {},
        "break;\n"});

    r.push_back({ "Conditional", "Conditional", "Flow", true,
        { { "", D::Exec, "" }, { "Cond", D::Wildcard, "0.0f", 1 }, { "Comp", D::Wildcard, "0.0f", 1 }, { "A", D::Wildcard, "0.0f", 2 }, { "B", D::Wildcard, "0.0f", 2 } },
        { { "", D::Exec, "" }, { "res", D::Wildcard, "", 2, "(($1 @ $2) ? $3 : $4)" } },
        "#0",
        { "Less than", "Greater than", "Equals", "Not Equals" },
        { "<", ">", "==", "!=" } });

    r.push_back({ "Cast", "Cast", "Flow", false,
        { { "Cast",   D::Wildcard, "0.0f", 1 } },
        { { "res", D::Wildcard, "", 2 } },
        "((" + oc::string(1, ENUM_TOKEN) + ")$0)",
        { "int", "float", "bool" },
        { "int", "float", "bool" }});

    // ---- functions ----
    // Function boundary nodes (special-cased in codegen). Function Input is the entry: a fixed Exec output
    // (pin 0) plus user-added typed parameter outputs. Function Output is the exit: a fixed Exec input (pin 0)
    // plus user-added typed return inputs. Both carry a function name so one .scr can hold several functions.
    r.push_back({ "FunctionInput",  "Function Input",  "Functions", true, {}, { { "", D::Exec, "" } }, "" });
    r.push_back({ "FunctionOutput", "Function Output", "Functions", true, { { "", D::Exec, "" } }, {}, "" });

    // Function Call: created by importing a function (hidden from the palette, like Reroute). Its pins are
    // rebuilt from the referenced function's signature; codegen turns it into a C++ call.
    r.push_back({ "FunctionCall", "Function Call", "Functions", true, {}, {}, "" });

    // ---- variables ----
    // Script Data is special-cased everywhere (isScriptDataType): its output pins are user-defined members
    // edited in the node, and codegen turns them into a persistent `struct ScriptData`. It carries no static
    // pins here â€” the members are added through the editor and serialized as //@member lines.
    r.push_back({ "ScriptData", "Script Data", "Variables", false, {}, {}, "" });

    r.push_back({ "Float", "Var Float", "Variables", false,
        { { "val", D::Float, "0.0f" }},
        { { "f@", D::Float, "", 0, oc::string("float f@ = $0;\n") + HOIST + "f@", EMutableType::ReadWritable } },
        "" });

    r.push_back({ "Int", "Var Int", "Variables", false,
        { { "val", D::Int, "0" }},
        { { "i@", D::Int, "", 0, oc::string("int i@ = $0;\n") + HOIST + "i@", EMutableType::ReadWritable } },
        "" });

    r.push_back({ "Bool", "Var Bool", "Variables", false,
        { { "val", D::Bool, "false" }},
        { { "b@", D::Bool, "", 0, oc::string("bool b@ = $0;\n") + HOIST + "b@", EMutableType::ReadWritable } },
        "" });

    r.push_back({ "ConstFloat", "Const Float", "Variables", false,
        { { "val", D::Float, "0.0f" } }, { { "res", D::Float, "" } },
        "$0" });

    r.push_back({ "ConstInt", "Const Int", "Variables", false,
        { { "val", D::Int, "0" } }, { { "res", D::Int, "" } },
        "$0" });

    r.push_back({ "ConstBool", "Const Bool", "Variables", false,
        { { "val", D::Bool, "false" } }, { { "res", D::Bool, "" } },
        "$0" });

    // ---- actions ----
    r.push_back({ "Print", "Print", "Debug", true,
        { { "", D::Exec, "" }, { "message", D::String, "hello" } },
        { { "", D::Exec, "" } },
        "ctx->log($1);\n#0" });

    r.push_back({ "Printf", "Printf", "Debug", true,
        { { "", D::Exec, "" },
            { "message", D::String, "%f" },
            { "message", D::Wildcard, "0.0f", 1 } },
        { { "", D::Exec, "" } },
        "ctx->logf($1, $2);\n#0" });

    r.push_back({ "SpawnPointLight", "Spawn Point Light", "Rendering", true,
        { { "", D::Exec, "" },
            { "position",  D::Vec3,  "glm::vec3{ 0.0f, 2.0f, 0.0f }" },
            { "range",     D::Float, "25.0f" },
            { "color",     D::Vec3,  "glm::vec3{ 1.0f, 0.6f, 0.2f }" },
            { "intensity", D::Float, "60.0f" } },
        { { "", D::Exec, "" } },
        "ctx->spawnPointLight($1, $2, $3, $4);\n#0" });

    r.push_back({ "SetSun", "Set Sun", "Rendering", true,
        { { "", D::Exec, "" },
            { "direction", D::Vec3,  "glm::vec3{ -0.3f, -1.0f, -0.2f }" },
            { "color",     D::Vec3,  "glm::vec3{ 1.0f, 1.0f, 1.0f }" },
            { "intensity", D::Float, "3.0f" } },
        { { "", D::Exec, "" } },
        "ctx->setSun($1, $2, $3);\n#0" });

    // ---- inputs / time ----
    // deltaSeconds/elapsedSeconds are plain fields on ScriptContext (refreshed once a frame), not calls.
    r.push_back({ "GetElapsedTime", "Get Elapsed Time", "Time", false,
        {}, { { "sec", D::Float, "" } },
        "ctx->elapsedSeconds" });

    r.push_back({ "GetDeltaTime", "Get Delta Time", "Time", false,
        {}, { { "sec", D::Float, "" } },
        "ctx->deltaSeconds" });

    r.push_back({ "IsKeyDown", "Is Key Down", "Events", false,
        { { "key", D::String, "Space" } }, { { "down", D::Bool, "" } },
        "(ctx->isKeyDown($0) != 0)" });

    // ---- camera (the active render camera, snapshotted once a frame onto ScriptContext) ----
    r.push_back({ "GetCamera", "Get Camera", "Rendering", false,
        {},
        { { "Position",  D::Vec3,  "", 0, "ctx->cameraPosition" },
            { "Direction", D::Vec3,  "", 0, "ctx->cameraDirection" },
            { "Up",        D::Vec3,  "", 0, "ctx->cameraUp" },
            { "Fov",       D::Float, "", 0, "ctx->cameraFovDeg" },
            { "Near",       D::Float, "", 0, "ctx->cameraNear" },
            { "Far",       D::Float, "", 0, "ctx->cameraFar" },
        }, "" });

    // ---- math ----
    // -- mutable math nodes -- 
    r.push_back({ "Set", "Set", "Math", true,
        { { "", D::Exec, "" }, { "a", D::Wildcard, "0.0f", 1, "", EMutableType::ReadWritable}, {"b", D::Wildcard, "0.0f", 1}},
        { { "", D::Exec, "" } },
        "$1 = $2;\n#0" });

    r.push_back({ "SetAdd", "Set Add", "Math", true,
        { { "", D::Exec, "" }, { "a", D::Wildcard, "0.0f", 1, "", EMutableType::ReadWritable}, {"b", D::Wildcard, "0.0f", 1}},
        { { "", D::Exec, "" } },
        "$1 += $2;\n#0" });

    r.push_back({ "SetSub", "Set Sub", "Math", true,
        { { "", D::Exec, "" }, { "a", D::Wildcard, "0.0f", 1, "", EMutableType::ReadWritable}, {"b", D::Wildcard, "0.0f", 1}},
        { { "", D::Exec, "" } },
        "$1 -= $2;\n#0" });

    r.push_back({ "SetMul", "Set Mul", "Math", true,
        { { "", D::Exec, "" }, { "a", D::Wildcard, "0.0f", 1, "", EMutableType::ReadWritable}, {"b", D::Wildcard, "0.0f", 1}},
        { { "", D::Exec, "" } },
        "$1 *= $2;\n#0" });

    r.push_back({ "SetDiv", "Set Div", "Math", true,
        { { "", D::Exec, "" }, { "a", D::Wildcard, "0.0f", 1, "", EMutableType::ReadWritable}, {"b", D::Wildcard, "0.0f", 1}},
        { { "", D::Exec, "" } },
        "$1 /= $2;\n#0" });

	// -- immutable math nodes --
    r.push_back({ "Add", "Add", "Math", false,
        { { "a", D::Wildcard, "0.0f", 1 }, { "b", D::Wildcard, "0.0f", 1 } }, { { "res", D::Wildcard, "", 1 } },
        "($0 + $1)" });

    r.push_back({ "Sub", "Subtract", "Math", false,
        { { "a", D::Wildcard, "0.0f", 1 }, { "b", D::Wildcard, "0.0f", 1 } }, { { "res", D::Wildcard, "", 1 } },
        "($0 - $1)" });

    r.push_back({ "Mul", "Multiply", "Math", false,
        { { "a", D::Wildcard, "1.0f", 1 }, { "b", D::Wildcard, "1.0f", 1 } }, { { "res", D::Wildcard, "", 1 } },
        "($0 * $1)" });

    r.push_back({ "Div", "Divide", "Math", false,
        { { "a", D::Wildcard, "1.0f", 1 }, { "b", D::Wildcard, "1.0f", 1 } }, { { "res", D::Wildcard, "", 1 } },
        "($0 / $1)" });

    r.push_back({ "Modulo", "Modulo", "Math", false,
        { { "a", D::Wildcard, "1.0f", 1 }, { "b", D::Wildcard, "1.0f", 1 } }, { { "res", D::Wildcard, "", 1 } },
        "($0 % $1)" });

    r.push_back({ "Sin", "Sin", "Math", false,
        { { "x", D::Float, "0.0f" } }, { { "res", D::Float, "" } },
        "sinf($0)" });

    r.push_back({ "Cos", "Cos", "Math", false,
        { { "x", D::Float, "0.0f" } }, { { "res", D::Float, "" } },
        "cosf($0)" });

    // ---- vector math ----
    r.push_back({ "MakeVec3", "Make Vec3", "Math", false,
        { { "x", D::Float, "0.0f" }, { "y", D::Float, "0.0f" }, { "z", D::Float, "0.0f" } },
        { { "vec", D::Vec3, "" } },
        "glm::vec3{ $0, $1, $2 }" });


    r.push_back({ "SplitVec3", "Split Vec3", "Math", false,
        { { "vec", D::Vec3, "" } },
        { { "x", D::Float, "", 0, "v@.x" }, { "y", D::Float, "", 0, "v@.y" }, { "z", D::Float, "", 0, "v@.z" } },
        oc::string("glm::vec3 v@ = $0;\n") + HOIST + "v@"});

    // Dot/Normalize are wildcarded (Vec3 or Quat â€” glm::dot/normalize support both); Cross stays Vec3-only
    // (undefined for quaternions).
    r.push_back({ "DotVec3", "Dot", "Math", false,
        { { "a", D::Wildcard, "glm::vec3{ 0.0f, 0.0f, 0.0f }", 1 }, { "b", D::Wildcard, "glm::vec3{ 0.0f, 0.0f, 0.0f }", 1 } },
        { { "res", D::Float, "" } },
        "glm::dot($0, $1)" });

    r.push_back({ "CrossVec3", "Cross", "Math", false,
        { { "a", D::Vec3, "" }, { "b", D::Vec3, "" } }, { { "res", D::Vec3, "" } },
        "glm::cross($0, $1)" });

    r.push_back({ "LengthVec3", "Length", "Math", false,
        { { "vec", D::Vec3, "" } }, { { "res", D::Float, "" } },
        "glm::length($0)" });

    r.push_back({ "NormalizeVec3", "Normalize", "Math", false,
        { { "vec", D::Wildcard, "glm::vec3{ 0.0f, 0.0f, 0.0f }", 1 } },
        { { "res", D::Wildcard, "", 1 } },
        "glm::normalize($0)" });

    // ---- quaternion math ----
    r.push_back({ "MakeQuatFromEuler", "Make Quat (Euler)", "Math", false,
        { { "eulerDeg", D::Vec3, "glm::vec3{ 0.0f, 0.0f, 0.0f }" } }, { { "quat", D::Quat, "" } },
        "glm::quat(glm::radians($0))" });

    r.push_back({ "QuatToEuler", "Quat To Euler", "Math", false,
        { { "quat", D::Quat, "" } }, { { "eulerDeg", D::Vec3, "" } },
        "glm::degrees(glm::eulerAngles($0))" });

    r.push_back({ "QuatFromAxisAngle", "Quat From Axis Angle", "Math", false,
        { { "axis", D::Vec3, "glm::vec3{ 0.0f, 1.0f, 0.0f }" }, { "angleDeg", D::Float, "0.0f" } },
        { { "quat", D::Quat, "" } },
        "glm::angleAxis(glm::radians($1), $0)" });

    r.push_back({ "MulQuat", "Multiply Quat", "Math", false,
        { { "a", D::Quat, "" }, { "b", D::Quat, "" } }, { { "res", D::Quat, "" } },
        "($0 * $1)" });

    r.push_back({ "RotateVec3ByQuat", "Rotate Vec3 By Quat", "Math", false,
        { { "quat", D::Quat, "" }, { "vec", D::Vec3, "" } }, { { "res", D::Vec3, "" } },
        "($0 * $1)" });

    r.push_back({ "InverseQuat", "Inverse Quat", "Math", false,
        { { "quat", D::Quat, "" } }, { { "res", D::Quat, "" } },
        "glm::inverse($0)" });

    r.push_back({ "SlerpQuat", "Slerp Quat", "Math", false,
        { { "a", D::Quat, "" }, { "b", D::Quat, "" }, { "t", D::Float, "0.5f" } }, { { "res", D::Quat, "" } },
        "glm::slerp($0, $1, $2)" });

    // ---- generic math (wildcard: works with Float/Int/Vec3, whatever the connected type resolves to) ----
    r.push_back({ "Min", "Min", "Math", false,
        { { "a", D::Wildcard, "0.0f", 1 }, { "b", D::Wildcard, "0.0f", 1 } }, { { "res", D::Wildcard, "", 1 } },
        "glm::min($0, $1)" });

    r.push_back({ "Max", "Max", "Math", false,
        { { "a", D::Wildcard, "0.0f", 1 }, { "b", D::Wildcard, "0.0f", 1 } }, { { "res", D::Wildcard, "", 1 } },
        "glm::max($0, $1)" });

    r.push_back({ "Clamp", "Clamp", "Math", false,
        { { "x", D::Wildcard, "0.0f", 1 }, { "min", D::Wildcard, "0.0f", 1 }, { "max", D::Wildcard, "1.0f", 1 } },
        { { "res", D::Wildcard, "", 1 } },
        "glm::clamp($0, $1, $2)" });

    r.push_back({ "Smoothstep", "Smoothstep", "Math", false,
        { { "edge0", D::Wildcard, "0.0f", 1 }, { "edge1", D::Wildcard, "1.0f", 1 }, { "x", D::Wildcard, "0.5f", 1 } },
        { { "res", D::Wildcard, "", 1 } },
        "glm::smoothstep($0, $1, $2)" });

    r.push_back({ "Ceil", "Ceil", "Math", false,
        { { "x", D::Wildcard, "0.0f", 1 } }, { { "res", D::Wildcard, "", 1 } },
        "glm::ceil($0)" });

    r.push_back({ "Floor", "Floor", "Math", false,
        { { "x", D::Wildcard, "0.0f", 1 } }, { { "res", D::Wildcard, "", 1 } },
        "glm::floor($0)" });

    r.push_back({ "Round", "Round", "Math", false,
        { { "x", D::Wildcard, "0.0f", 1 } }, { { "res", D::Wildcard, "", 1 } },
        "glm::round($0)" });

    // ---- entity (defaults to the script's owning entity, `self`, but any node reachable through an Entity
    // pin can be targeted instead) ----
    // Position/Scale/Rotation read straight off the Entity mirror ($0->pos/scale/rot) instead of going
    // through the ABI; Rotation/Forward/Right/Up derive from $0->rot with glm:: helpers (see ScriptAPI.h).
    r.push_back({ "GetEntity", "Get Entity", "Entity", false,
        { { "Entity", D::Entity, "self" } },
        { { "Position",  D::Vec3,   "", 0, "$0->pos" },
            { "Scale",     D::Float,  "", 0, "$0->scale" },
            { "Rotation",  D::Vec3,   "", 0, "glm::degrees(glm::eulerAngles($0->rot))" },
            { "Forward",   D::Vec3,   "", 0, "($0->rot * glm::vec3(0.0f, 0.0f, -1.0f))" },
            { "Right",     D::Vec3,   "", 0, "($0->rot * glm::vec3(1.0f, 0.0f, 0.0f))" },
            { "Up",        D::Vec3,   "", 0, "($0->rot * glm::vec3(0.0f, 1.0f, 0.0f))" },
            { "Name",      D::String, "", 0, "ctx->entityGetName($0)" },
            { "Enabled",   D::Bool,   "", 0, "(ctx->entityGetEnabled($0) != 0)" },
            { "Children",  D::Int,    "", 0, "ctx->entityGetChildCount($0)" },
            { "Bounds R",  D::Float,  "", 0, "ctx->entityGetBoundsRadius($0)" } },
        "" });

    // ---- force component (all via ABI thunks, so they work in a sandboxed script DLL) ----
    // Get Force Component: resolves the entity's ForceComponent ONCE in the if-condition and branches on it —
    // true runs when the entity has one (the Component handle is bound and in scope on that chain), break
    // continues past the if whether or not it was present (like the If node, no else). The handle is a single
    // lookup shared by every Get/Set Force that reads it, and it is out of scope on the break chain, so it can
    // only be used where the component is known to exist. Feed the Component pin into Get/Set Force.
    r.push_back({ "GetForceComponent", "Get Force Component", "Force", true,
        { { "", D::Exec, "" }, { "Entity", D::Entity, "self" } },
        { { "true", D::Exec, "" }, { "break", D::Exec, "" },
            { "Component", D::Pointer, "", 0, "forceComp@" } },
        "if (void* forceComp@ = ctx->entityGetForceComponent($1))\n{\n"
        + oc::string(1, INDENT_UP) + "#0" + oc::string(1, INDENT_DOWN) + "}\n#1" });

    // Get Force: reads the live field values off the Component handle (from Get Force Component). Valid is true
    // when the handle is non-null (the entity has a ForceComponent); the getters return type defaults for null.
    // Output/Reach/Focus/Distribution/Width/Team are the authored values; Applied Force/Pressure are the
    // ~2-frame-old GPU readbacks; Local Direction/Offset are the emitter's entity-space placement.
    r.push_back({ "GetForce", "Get Force", "Force", false,
        { { "Component", D::Pointer, "nullptr" } },
        { { "Valid",         D::Bool,  "", 0, "($0 != nullptr)" },
            { "Output",        D::Float, "", 0, "ctx->forceGetOutput($0)" },
            { "Reach",         D::Float, "", 0, "ctx->forceGetReach($0)" },
            { "Focus",         D::Float, "", 0, "ctx->forceGetFocus($0)" },
            { "Distribution",  D::Float, "", 0, "ctx->forceGetDistribution($0)" },
            { "Width",         D::Float, "", 0, "ctx->forceGetWidth($0)" },
            { "Team",          D::Int,   "", 0, "ctx->forceGetTeam($0)" },
            { "Applied Force", D::Vec3,  "", 0, "ctx->forceGetAppliedForce($0)" },
            { "Pressure",      D::Float, "", 0, "ctx->forceGetPressure($0)" },
            { "Local Direction", D::Vec3, "", 0, "ctx->forceGetLocalDirection($0)" },
            { "Local Offset",  D::Vec3,  "", 0, "ctx->forceGetLocalOffset($0)" },
            { "Centered",      D::Bool,  "", 0, "(ctx->forceGetCentered($0) != 0)" } },
        "" });

    // Set Force: writes only the fields you actually connect (?k conditional blocks, like Set Entity), through
    // the Component handle. Output/Reach/Focus/Distribution/Width/Team hit the live emitter; Local Direction/
    // Offset write the component fields that reposition the emitter relative to the entity next update. A null
    // handle no-ops. Team clamps a negative to 0; Local Direction is re-normalized host-side.
    r.push_back({ "SetForce", "Set Force", "Force", true,
        { { "", D::Exec, "" },
            { "Component",    D::Pointer, "nullptr" },
            { "Output",       D::Float, "1.0f" },
            { "Reach",        D::Float, "5.0f" },
            { "Focus",        D::Float, "0.5f" },
            { "Distribution", D::Float, "0.5f" },
            { "Width",        D::Float, "1.0f" },
            { "Team",         D::Int,   "0" },
            { "Local Direction", D::Vec3, "glm::vec3{ 0.0f, 0.0f, -1.0f }" },
            { "Local Offset",    D::Vec3, "glm::vec3{ 0.0f, 0.0f, 0.0f }" },
            { "Centered",        D::Bool, "true" } },
        { { "", D::Exec, "" } },
        "?2{ctx->forceSetOutput($1, $2);\n}?3{ctx->forceSetReach($1, $3);\n}?4{ctx->forceSetFocus($1, $4);\n}"
        "?5{ctx->forceSetDistribution($1, $5);\n}?6{ctx->forceSetWidth($1, $6);\n}?7{ctx->forceSetTeam($1, $7);\n}"
        "?8{ctx->forceSetLocalDirection($1, $8);\n}?9{ctx->forceSetLocalOffset($1, $9);\n}?10{ctx->forceSetCentered($1, $10);\n}#0" });

    // ---- light component (same shape as the force nodes: resolve the handle once, then indexed get/set) ----
    // Get Light Component: resolves the entity's LightComponent ONCE in the if-condition and branches on it —
    // true runs when the entity has one, break continues past the if either way. Feed the Component pin into
    // Get/Set Light; Count is the number of lights on it, the valid Index range for both.
    r.push_back({ "GetLightComponent", "Get Light Component", "Light", true,
        { { "", D::Exec, "" }, { "Entity", D::Entity, "self" } },
        { { "true", D::Exec, "" }, { "break", D::Exec, "" },
            { "Component", D::Pointer, "", 0, "lightComp@" },
            { "Count", D::Int, "", 0, "ctx->lightGetCount(lightComp@)" } },
        "if (void* lightComp@ = ctx->entityGetLightComponent($1))\n{\n"
        + oc::string(1, INDENT_UP) + "#0" + oc::string(1, INDENT_DOWN) + "}\n#1" });

    // Get Light: reads one light (by Index) off the Component handle. Valid is true when the handle is non-null
    // AND the index is in range; the pins read type defaults otherwise (ocLightAt's contract). Each connected
    // pin copies the light out and picks its field; Offset/Direction are entity-space.
    r.push_back({ "GetLight", "Get Light", "Light", false,
        { { "Component", D::Pointer, "nullptr" }, { "Index", D::Int, "0" } },
        { { "Valid",     D::Bool,  "", 0, "($1 >= 0 && $1 < ctx->lightGetCount($0))" },
            { "Enabled",   D::Bool,  "", 0, "ocLightAt(ctx, $0, $1).enabled" },
            { "Color",     D::Vec3,  "", 0, "ocLightAt(ctx, $0, $1).color" },
            { "Intensity", D::Float, "", 0, "ocLightAt(ctx, $0, $1).intensity" },
            { "Range",     D::Float, "", 0, "ocLightAt(ctx, $0, $1).range" },
            { "Offset",    D::Vec3,  "", 0, "ocLightAt(ctx, $0, $1).offset" },
            { "Direction", D::Vec3,  "", 0, "ocLightAt(ctx, $0, $1).direction" },
            // Shape params -- each read by ONE type (Spot: Cone Angle/Edge Softness; Area: Width/Height/
            // Rotation; Tube: Width = radius, Length); the others report the stored-but-unused value.
            { "Cone Angle",    D::Float, "", 0, "ocLightAt(ctx, $0, $1).coneAngle" },
            { "Edge Softness", D::Float, "", 0, "ocLightAt(ctx, $0, $1).edgeSoftness" },
            { "Width",         D::Float, "", 0, "ocLightAt(ctx, $0, $1).width" },
            { "Height",        D::Float, "", 0, "ocLightAt(ctx, $0, $1).height" },
            { "Length",        D::Float, "", 0, "ocLightAt(ctx, $0, $1).length" },
            { "Rotation",      D::Float, "", 0, "ocLightAt(ctx, $0, $1).rotation" } },
        "" });

    // Set Light: copies the light out, overwrites only the fields you actually connect (?k conditional blocks,
    // like Set Force), and copies it back through lightSetAt -- the one write path, where the host clamps and
    // re-normalizes. A null handle or out-of-range index no-ops on both sides of the round trip.
    r.push_back({ "SetLight", "Set Light", "Light", true,
        { { "", D::Exec, "" },
            { "Component", D::Pointer, "nullptr" },
            { "Index",     D::Int,   "0" },
            { "Enabled",   D::Bool,  "true" },
            { "Color",     D::Vec3,  "glm::vec3{ 1.0f, 1.0f, 1.0f }" },
            { "Intensity", D::Float, "20.0f" },
            { "Range",     D::Float, "10.0f" },
            { "Offset",    D::Vec3,  "glm::vec3{ 0.0f, 0.0f, 0.0f }" },
            { "Direction", D::Vec3,  "glm::vec3{ 0.0f, 0.0f, -1.0f }" },
            { "Cone Angle",    D::Float, "25.0f" },
            { "Edge Softness", D::Float, "0.1f" },
            { "Width",         D::Float, "1.0f" },
            { "Height",        D::Float, "1.0f" },
            { "Length",        D::Float, "1.0f" },
            { "Rotation",      D::Float, "0.0f" } },
        { { "", D::Exec, "" } },
        "OcLight light@ = ocLightAt(ctx, $1, $2);\n"
        "?3{light@.enabled = $3;\n}?4{light@.color = $4;\n}?5{light@.intensity = $5;\n}"
        "?6{light@.range = $6;\n}?7{light@.offset = $7;\n}?8{light@.direction = $8;\n}"
        "?9{light@.coneAngle = $9;\n}?10{light@.edgeSoftness = $10;\n}?11{light@.width = $11;\n}"
        "?12{light@.height = $12;\n}?13{light@.length = $13;\n}?14{light@.rotation = $14;\n}"
        "ctx->lightSetAt($1, $2, &light@);\n#0" });

    // Set Entity: writes only the inputs you actually connect (the ?k{...} conditional blocks). Position/
    // Scale/Rotation assign straight into the Entity mirror; Rotation converts degrees -> quat with glm::.
    r.push_back({ "SetEntity", "Set Entity", "Entity", true,
        { { "", D::Exec, "" },
            { "Entity",   D::Entity, "self" },
            { "Position", D::Vec3,  "glm::vec3{ 0.0f, 0.0f, 0.0f }" },
            { "Scale",    D::Float, "1.0f" },
            { "Rotation", D::Vec3,  "glm::vec3{ 0.0f, 0.0f, 0.0f }" },
            { "Enabled",  D::Bool,  "true" } },
        { { "", D::Exec, "" } },
        "?2{$1->pos = $2;\n}?3{$1->scale = $3;\n}"
        "?4{$1->rot = glm::quat(glm::radians($4));\n}?5{ctx->entitySetEnabled($1, $5);\n}#0" });

    // Spawn Entity: spawns an asset/prefab at a world position immediately and returns it, so it can be used
    // right away (e.g. AddChild it, or SetEntity its transform) in the same exec chain. Not targeted at an
    // existing entity, so it has no Entity input.
    r.push_back({ "SpawnEntity", "Spawn Entity", "Entity", true,
        { { "", D::Exec, "" },
            { "asset",    D::String, "Entities/character.pre" },
            { "position", D::Vec3,   "glm::vec3{ 0.0f, 0.0f, 0.0f }" } },
        { { "", D::Exec, "" }, { "Entity", D::Entity, "", 0, "spawned@" } },
        "Entity* spawned@ = ctx->spawnEntity($1, $2);\n#0" });

    // Destroy Entity: queues the given entity (self by default) for removal.
    r.push_back({ "DestroyEntity", "Destroy Entity", "Entity", true,
        { { "", D::Exec, "" }, { "Entity", D::Entity, "self" } },
        { { "", D::Exec, "" } },
        "ctx->destroyEntity($1);\n#0" });

    r.push_back({ "SetAnimFloat", "Set Anim Float", "Entity", true,
        { { "", D::Exec, "" }, { "Entity", D::Entity, "self" }, { "param", D::String, "speed" }, { "val", D::Float, "0.0f" } },
        { { "", D::Exec, "" } },
        "ctx->entitySetAnimFloat($1, $2, $3);\n#0" });

    r.push_back({ "SetAnimTrigger", "Set Anim Trigger", "Entity", true,
        { { "", D::Exec, "" }, { "Entity", D::Entity, "self" }, { "param", D::String, "attack" } },
        { { "", D::Exec, "" } },
        "ctx->entitySetAnimTrigger($1, $2);\n#0" });

    // ---- scene component (child hierarchy) ----
    // Get Scene Component: resolves the entity's SceneComponent ONCE in the if-condition and branches on it
    // (same shape as Get Force Component). The Component handle is a single lookup shared by every child
    // read/write below, in scope only on the true chain. Feed the Component pin into the child nodes.
    r.push_back({ "GetSceneComponent", "Get Scene Component", "Entity", true,
        { { "", D::Exec, "" }, { "Entity", D::Entity, "self" } },
        { { "true", D::Exec, "" }, { "break", D::Exec, "" },
            { "Component", D::Pointer, "", 0, "sceneComp@" } },
        "if (void* sceneComp@ = ctx->entityGetSceneComponent($1))\n{\n"
        + oc::string(1, INDENT_UP) + "#0" + oc::string(1, INDENT_DOWN) + "}\n#1" });

    // Get Child Entity: looks up a direct child by name on the given SceneComponent (from Get Scene Component)
    // and branches on whether it exists; the found child is exposed as an Entity output only valid on the
    // Found branch (see the emitted `child@` local).
    r.push_back({ "GetChildEntity", "Get Child Entity", "Entity", true,
        { { "", D::Exec, "" }, { "Component", D::Pointer, "nullptr" }, { "Name", D::String, "" } },
        { { "Found", D::Exec, "" }, { "Not Found", D::Exec, "" }, { "Child", D::Entity, "", 0, "child@" } },
        "Entity* child@ = ctx->sceneFindChild($1, $2);\n"
        "if (child@)\n{\n" + oc::string(1, INDENT_UP) + "#0" + oc::string(1, INDENT_DOWN) + "}\n"
        "else\n{\n" + oc::string(1, INDENT_UP) + "#1" + oc::string(1, INDENT_DOWN) + "}\n" });

    // Add Child: reparents Child under the SceneComponent's owner entity (the parent). Component from Get
    // Scene Component; Child self by default -- connect a real value before using.
    r.push_back({ "AddChild", "Add Child", "Entity", true,
        { { "", D::Exec, "" }, { "Component", D::Pointer, "nullptr" }, { "Child", D::Entity, "self" } },
        { { "", D::Exec, "" } },
        "ctx->sceneAddChild($1, $2);\n#0" });

    // Remove Child: detaches Child from the SceneComponent's owner, making it a root entity (no-op if Child
    // isn't parented there).
    r.push_back({ "RemoveChild", "Remove Child", "Entity", true,
        { { "", D::Exec, "" }, { "Component", D::Pointer, "nullptr" }, { "Child", D::Entity, "self" } },
        { { "", D::Exec, "" } },
        "ctx->sceneRemoveChild($1, $2);\n#0" });

    // Remove Child At Index: same as Remove Child, but looks the child up by index on the SceneComponent.
    r.push_back({ "RemoveChildIdx", "Remove Child At Index", "Entity", true,
        { { "", D::Exec, "" }, { "Component", D::Pointer, "nullptr" }, { "Index", D::Int, "0" } },
        { { "", D::Exec, "" } },
        "ctx->sceneRemoveChildAt($1, $2);\n#0" });

    // ---- physics (target the entity's PhysicsComponent body; entities without one no-op / read zero) ----
    // Get Physics Component: resolves the entity's PhysicsComponent (with a valid body) ONCE and branches on
    // it (same shape as Get Force Component). Feed the Component pin into the physics read/write nodes.
    r.push_back({ "GetPhysicsComponent", "Get Physics Component", "Physics", true,
        { { "", D::Exec, "" }, { "Entity", D::Entity, "self" } },
        { { "true", D::Exec, "" }, { "break", D::Exec, "" },
            { "Component", D::Pointer, "", 0, "physComp@" } },
        "if (void* physComp@ = ctx->entityGetPhysicsComponent($1))\n{\n"
        + oc::string(1, INDENT_UP) + "#0" + oc::string(1, INDENT_DOWN) + "}\n#1" });

    // Get Physics: reads off the PhysicsComponent handle (from Get Physics Component). Valid is true when the
    // handle is non-null (the entity has a body); the getters return zero for null.
    r.push_back({ "GetPhysics", "Get Physics", "Physics", false,
        { { "Component", D::Pointer, "nullptr" } },
        { { "Valid",    D::Bool,  "", 0, "($0 != nullptr)" },
            { "Velocity", D::Vec3,  "", 0, "ctx->physicsGetVelocity($0)" },
            { "Speed",    D::Float, "", 0, "glm::length(ctx->physicsGetVelocity($0))" },
            { "Awake",    D::Bool,  "", 0, "(ctx->physicsIsAwake($0) != 0)" } },
        "" });

    r.push_back({ "SetVelocity", "Set Velocity", "Physics", true,
        { { "", D::Exec, "" }, { "Component", D::Pointer, "nullptr" }, { "velocity", D::Vec3, "glm::vec3{ 0.0f, 0.0f, 0.0f }" } },
        { { "", D::Exec, "" } },
        "ctx->physicsSetVelocity($1, $2);\n#0" });

    r.push_back({ "ApplyImpulse", "Apply Impulse", "Physics", true,
        { { "", D::Exec, "" }, { "Component", D::Pointer, "nullptr" }, { "impulse", D::Vec3, "glm::vec3{ 0.0f, 0.0f, 0.0f }" } },
        { { "", D::Exec, "" } },
        "ctx->physicsApplyImpulse($1, $2);\n#0" });

    // Teleport Body: queues an absolute pose, applied before the next physics step. The only way to move
    // a body of any type — Set Entity moves the mesh and leaves the collider behind.
    r.push_back({ "TeleportBody", "Teleport Body", "Physics", true,
        { { "", D::Exec, "" },
            { "Component", D::Pointer, "nullptr" },
            { "position", D::Vec3, "glm::vec3{ 0.0f, 0.0f, 0.0f }" },
            { "rotation", D::Vec3, "glm::vec3{ 0.0f, 0.0f, 0.0f }" } },
        { { "", D::Exec, "" } },
        "ctx->physicsTeleport($1, $2, $3);\n#0" });

    // Ray Cast: closest hit against the physics world; the hit outputs are only valid on the Hit branch.
    r.push_back({ "RayCast", "Ray Cast", "Physics", true,
        { { "", D::Exec, "" },
            { "origin",    D::Vec3,  "glm::vec3{ 0.0f, 0.0f, 0.0f }" },
            { "direction", D::Vec3,  "glm::vec3{ 0.0f, 0.0f, -1.0f }" },
            { "maxDist",   D::Float, "100.0f" } },
        { { "Hit", D::Exec, "" }, { "Miss", D::Exec, "" },
            { "Point",    D::Vec3,  "", 0, "hitPoint@" },
            { "Normal",   D::Vec3,  "", 0, "hitNormal@" },
            { "Distance", D::Float, "", 0, "(hitFraction@ * $3)" } },
        "glm::vec3 hitPoint@{ 0.0f, 0.0f, 0.0f };\nglm::vec3 hitNormal@{ 0.0f, 0.0f, 0.0f };\nfloat hitFraction@ = 0.0f;\n"
        "if (ctx->physicsRayCast($1, glm::normalize($2) * $3, &hitPoint@, &hitNormal@, &hitFraction@) != 0)\n{\n"
        + oc::string(1, INDENT_UP) + "#0" + oc::string(1, INDENT_DOWN) + "}\nelse\n{\n"
        + oc::string(1, INDENT_UP) + "#1" + oc::string(1, INDENT_DOWN) + "}\n" });

    // Get Contact Point: resolves an On Physics Event node's ContactId to its world-space hit position/normal
    // (ctx->physicsContactGetPoint). Hit is false (Point/Normal stay zero) for a stale id or a sensor event.
    r.push_back({ "GetContactPoint", "Get Contact Point", "Physics", false,
        { { "Contact Id", D::Int, "0" } },
        { { "Hit",    D::Bool, "", 0, "contactHit@" },
            { "Point",  D::Vec3, "", 0, "contactPoint@" },
            { "Normal", D::Vec3, "", 0, "contactNormal@" } },
        oc::string("glm::vec3 contactPoint@{ 0.0f, 0.0f, 0.0f };\nglm::vec3 contactNormal@{ 0.0f, 0.0f, 0.0f };\n"
            "const bool contactHit@ = (ctx->physicsContactGetPoint($0, &contactPoint@, &contactNormal@) != 0);\n")
        + HOIST + "contactHit@" });

    r.push_back({ "SetGravity", "Set Gravity", "Physics", true,
        { { "", D::Exec, "" }, { "gravity", D::Vec3, "glm::vec3{ 0.0f, -9.81f, 0.0f }" } },
        { { "", D::Exec, "" } },
        "ctx->physicsSetGravity($1);\n#0" });

    r.push_back({ "GetParent", "Get Parent", "Entity", false,
        { { "Entity", D::Entity, "self" } },
        { { "Parent", D::Entity, "" } },
        "$0->parent" });

    // Get Child Count / Get Child At Index: read off the SceneComponent handle (from Get Scene Component).
    r.push_back({ "GetChildCount", "Get Child Count", "Entity", false,
        { { "Component", D::Pointer, "nullptr" } },
        { { "Count", D::Int, "" } },
        "ctx->sceneGetChildCount($0)" });

    r.push_back({ "GetChildIdx", "Get Child At Index", "Entity", false,
        { { "Component", D::Pointer, "nullptr" }, { "Index", D::Int, "0" } },
        { { "Child", D::Entity, "" } },
        "ctx->sceneGetChildAt($0, $1)" });

    // ---- spatial queries (backed by the SpatialIndex) ----

    // Get Nearest Entity: the entity with the nearest origin within Max Radius, skipping Exclude
    // (self by default, so "the nearest OTHER entity"). Null when nothing is in range.
    r.push_back({ "SpatialGetNearest", "Get Nearest Entity", "Spatial", false,
        { { "Position", D::Vec3, "self->pos" }, { "Max Radius", D::Float, "100.0f" }, { "Exclude", D::Entity, "self" } },
        { { "Entity", D::Entity, "" } },
        "ctx->spatialGetNearestEntity($0, $1, $2)" });

    // For Each Entity In Radius: runs Each once per entity whose render bounds intersect the sphere
    // (up to 64); the Entity output is only valid on the Each branch.
    r.push_back({ "SpatialForEachInRadius", "For Each Entity In Radius", "Spatial", true,
        { { "", D::Exec, "" },
            { "Position", D::Vec3,  "self->pos" },
            { "Radius",   D::Float, "10.0f" } },
        { { "Each", D::Exec, "" }, { "Done", D::Exec, "" },
            { "Entity", D::Entity, "", 0, "spatialFound@[spatialIt@]" } },
        oc::string("Entity* spatialFound@[64];\nconst int spatialCount@ = ctx->spatialQueryRadius($1, $2, spatialFound@, 64);\n"
            "for (int spatialIt@ = 0; spatialIt@ < spatialCount@; ++spatialIt@)\n{\n")
        + oc::string(1, INDENT_UP) + "#0" + oc::string(1, INDENT_DOWN) + "}\n#1" });

    // ---- audio (target the entity's AudioComponent; entities without one no-op) ----
    // Get Audio Component: resolves the entity's AudioComponent ONCE and branches on it (same shape as Get
    // Force Component). Feed the Component pin into Trigger Audio / Stop Audio to skip the per-call lookup.
    r.push_back({ "GetAudioComponent", "Get Audio Component", "Audio", true,
        { { "", D::Exec, "" }, { "Entity", D::Entity, "self" } },
        { { "true", D::Exec, "" }, { "break", D::Exec, "" },
            { "Component", D::Pointer, "", 0, "audioComp@" } },
        "if (void* audioComp@ = ctx->entityGetAudioComponent($1))\n{\n"
        + oc::string(1, INDENT_UP) + "#0" + oc::string(1, INDENT_DOWN) + "}\n#1" });

    // Trigger Audio is special-cased (isTriggerAudioType): its exec input pins are the alias entries of the
    // target entity's AudioComponent â€” flow entering an alias pin plays that sound. Component (from Get Audio
    // Component) is the resolved AudioComponent; Entity is still needed so a spatial sound can follow it. The
    // Position/Volume/Pitch inputs override the sound's authored settings only when connected (a connected
    // Position also pins the sound at that world position instead of following the entity). Codegen is
    // emitTriggerAudioStmt.
    r.push_back({ "TriggerAudio", "Trigger Audio", "Audio", true,
        { { "Component", D::Pointer, "nullptr" },
            { "Entity",   D::Entity, "self" },
            { "Position", D::Vec3,   "default" },
            { "Volume",   D::Float,  "default" },
            { "Pitch",    D::Float,  "default" } },
        { { "", D::Exec, "" } },
        "" });

    // Stop Audio: stops a playing sound by alias on the AudioComponent handle (looping sounds keep playing
    // until stopped); an empty alias stops every sound on the component.
    r.push_back({ "StopAudio", "Stop Audio", "Audio", true,
        { { "", D::Exec, "" }, { "Component", D::Pointer, "nullptr" }, { "alias", D::String, "" } },
        { { "", D::Exec, "" } },
        "ctx->audioStop($1, $2);\n#0" });

    // Send Event: fires a named On Event entry on every script listening for it (matched by name at runtime).
    r.push_back({ "SendEvent", "Send Event", "Events", true,
        { { "", D::Exec, "" }, { "Event", D::String, "" } },
        { { "", D::Exec, "" } },
        "ctx->sendEvent($1);\n#0" });

    // Send Event to Entity: same, but delivered only to the given entity's script (self by default).
    r.push_back({ "SendEventToEntity", "Send Event to Entity", "Events", true,
        { { "", D::Exec, "" }, { "Entity", D::Entity, "self" }, { "Event", D::String, "" } },
        { { "", D::Exec, "" } },
        "ctx->sendEventToEntity($1, $2);\n#0" });

    // Send Network Event: fires locally like Send Event AND across the network (server -> all clients,
    // client -> server which relays to the other clients); in single player it degrades to Send Event.
    r.push_back({ "SendNetworkEvent", "Send Network Event", "Events", true,
        { { "", D::Exec, "" }, { "Event", D::String, "" } },
        { { "", D::Exec, "" } },
        "ctx->sendNetworkEvent($1);\n#0" });

    return r;
    }();
    return registry;
}

export const NodeDef* findNodeDef(const oc::string& typeId)
{
    for (const NodeDef& def : nodeRegistry())
        if (def.typeId == typeId)
            return &def;
    return nullptr;
}

} // namespace NodeEditor
