module UI;

import Core;
import Core.imgui;
import :imgui_node_editor;
import :NodeDef;
import :Node;

using namespace NodeEditor;

Node& Node::initialize(ImVec2 initialPos, oc::string name, ENodeStyle style, uint32 color)
{
    m_initialPos = initialPos;
    m_name = name;
    m_style = style;
    m_color = color;
    return *this;
}

Node& Node::initFromDef(ImVec2 initialPos, const NodeDef& def)
{
    initialize(initialPos, def.displayName, ENodeStyle_Full, 0xFF333333);
    m_typeId = def.typeId;
    // Writable pins (a value you can assign to) draw as a square to set them apart from read-only circles.
    auto applyMutability = [](Pin& pin, EMutableType m)
    {
        pin.mutability = m;
        if (m != EMutableType::Readable && pin.shape == EPinShape_Circle)
            pin.shape = EPinShape_Square;
    };
    for (const PinDef& pinDef : def.inputs)
    {
        addInput(pinDef.type, pinDef.name, pinDef.defaultValue);
        m_inputPins.back()->typeGroup = pinDef.typeGroup;
        applyMutability(*m_inputPins.back(), pinDef.mutability);
    }
    for (const PinDef& pinDef : def.outputs)
    {
        addOutput(pinDef.type, pinDef.name);
        m_outputPins.back()->typeGroup = pinDef.typeGroup;
        applyMutability(*m_outputPins.back(), pinDef.mutability);
    }
    return *this;
}

static EPinShape shapeForDataType(EDataType type)
{
    return type == EDataType::Exec ? EPinShape_Flow : EPinShape_Circle;
}

Node& Node::addInput(EDataType dataType, const oc::string& name, const oc::string& defaultValue)
{
    auto pin = oc::make_unique<Pin>(name, this, EPinType_Input, shapeForDataType(dataType), dataTypeColor(dataType));
    pin->dataType = dataType;
    pin->defaultValue = defaultValue;
    m_inputPins.emplace_back(oc::move(pin));
    return *this;
}

Node& Node::addOutput(EDataType dataType, const oc::string& name)
{
    auto pin = oc::make_unique<Pin>(name, this, EPinType_Output, shapeForDataType(dataType), dataTypeColor(dataType));
    pin->dataType = dataType;
    m_outputPins.emplace_back(oc::move(pin));
    return *this;
}

// A Script Data member is a writable (square), concretely-typed output pin — writable so a Set node can
// store into the persistent field, and readable so any node can read it.
Node& Node::addMember(EDataType type, const oc::string& name)
{
    addOutput(type, name);
    Pin& pin = *m_outputPins.back();
    pin.mutability = EMutableType::ReadWritable;
    pin.shape = EPinShape_Square;
    return *this;
}

void Node::eraseOutputPin(int index)
{
    if (index >= 0 && index < (int)m_outputPins.size())
        m_outputPins.erase(m_outputPins.begin() + index);
}

// An On Event entry is a plain (unconnected-source) Exec output: something else fires it by name at runtime.
Node& Node::addEventEntry(const oc::string& name)
{
    addOutput(EDataType::Exec, name);
    return *this;
}

// A Trigger Audio alias entry is an exec INPUT pin: flow entering it plays that sound. Alias pins sit in
// front of the node's data inputs (Entity/Position/Volume/Pitch), in AudioComponent order.
Node& Node::addAudioEntry(const oc::string& name)
{
    addInput(EDataType::Exec, name, "");
    size_t pos = 0;
    while (pos < m_inputPins.size() - 1 && m_inputPins[pos]->dataType == EDataType::Exec)
        ++pos;
    oc::rotate(m_inputPins.begin() + pos, m_inputPins.end() - 1, m_inputPins.end());
    return *this;
}

// A reroute carries one value straight through: an input and an output pin of the same (link) type.
Node& Node::makeReroute(EDataType type)
{
    addInput(type, "", "");
    addOutput(type, "");
    return *this;
}

// A function parameter is a plain readable data OUTPUT pin on the Function Input node: the body reads it, and
// codegen resolves it to the C++ parameter of the generated function.
Node& Node::addParam(EDataType type, const oc::string& name)
{
    addOutput(type, name);
    return *this;
}

// A function return is a data INPUT pin on the Function Output node: whatever feeds it is assigned to the
// generated function's out-parameter when the body reaches the Function Output.
Node& Node::addReturn(EDataType type, const oc::string& name)
{
    addInput(type, name, defaultValueForType(type));
    return *this;
}

void Node::eraseInputPin(int index)
{
    if (index >= 0 && index < (int)m_inputPins.size())
        m_inputPins.erase(m_inputPins.begin() + index);
}

Node& Node::makeFunctionCall(const oc::vector<oc::pair<EDataType, oc::string>>& params,
                             const oc::vector<oc::pair<EDataType, oc::string>>& returns)
{
    addInput(EDataType::Exec, "", "");            // pin 0: exec in
    for (const auto& [type, name] : params)
        addInput(type, name, defaultValueForType(type));
    addOutput(EDataType::Exec, "");               // pin 0: exec out
    for (const auto& [type, name] : returns)
        addOutput(type, name);
    return *this;
}

MemberEdit Node::takeMemberEdit()
{
    const MemberEdit e = m_pendingEdit;
    m_pendingEdit = MemberEdit{};
    return e;
}

namespace
{
    constexpr float kPinIcon = 16.0f;        // reserved square for a pin's shape marker
    constexpr float kColumnGap = 24.0f;      // gap between the input and output columns
    constexpr float kDefaultFieldWidth = 120.0f;
    constexpr float kVec3FieldWidth = 150.0f; // three drag-float boxes side by side (see renderInputRow)

    bool hasInlineDefault(const Pin& pin)
    {
        return pin.dataType != EDataType::Exec && pin.type == EPinType_Input && pin.numConnections == 0;
    }

    // Numeric/bool literals are short (~5 chars), so give them a narrow box; strings keep the wide one; a
    // Vec3 gets three narrow boxes (x/y/z) side by side.
    float defaultFieldWidth(const Pin& pin)
    {
        switch (pin.dataType)
        {
            case EDataType::Int:
            case EDataType::Float:
            case EDataType::Bool: return 52.0f;
            case EDataType::Vec3: return kVec3FieldWidth;
            default:              return kDefaultFieldWidth;
        }
    }

    // Reserves a fixed square and draws the pin's shape into it, registering it as the pin's hit-rect.
    void drawPinIcon(const Pin& pin)
    {
        const ImVec2 topLeft = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(kPinIcon, kPinIcon));
        ed::PinRect(topLeft, ImVec2(topLeft.x + kPinIcon, topLeft.y + kPinIcon));

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImVec2 c = ImVec2(topLeft.x + kPinIcon * 0.5f, topLeft.y + kPinIcon * 0.5f);
        const bool filled = pin.numConnections > 0;
        switch (pin.shape)
        {
            case EPinShape_Flow:
                drawList->AddTriangleFilled(ImVec2(c.x - 5, c.y - 5), ImVec2(c.x + 5, c.y), ImVec2(c.x - 5, c.y + 5), pin.color);
                break;
            case EPinShape_Circle:
                filled ? drawList->AddCircleFilled(c, 5.0f, pin.color, 12)
                       : drawList->AddCircle(c, 5.0f, pin.color, 12, 1.5f);
                break;
            case EPinShape_Square:
                filled ? drawList->AddRectFilled(ImVec2(c.x - 5, c.y - 5), ImVec2(c.x + 5, c.y + 5), pin.color)
                       : drawList->AddRect(ImVec2(c.x - 5, c.y - 5), ImVec2(c.x + 5, c.y + 5), pin.color);
                break;
            case EPinShape_None:
                break;
        }
    }

    float measureRow(const Pin& pin, float spacing)
    {
        float w = pin.shape != EPinShape_None ? kPinIcon : 0.0f;
        if (!pin.name.empty())
            w += (w > 0.0f ? spacing : 0.0f) + ImGui::CalcTextSize(pin.name.c_str()).x;
        if (hasInlineDefault(pin))
            w += spacing + defaultFieldWidth(pin);
        return w;
    }

    // Input row, left aligned at leftX: [icon] name [default field]. No trailing SameLine.
    void renderInputRow(Pin& pin, float leftX, float spacing)
    {
        bool firstElement = true;
        auto sameLine = [&] { if (!firstElement) ImGui::SameLine(0.0f, spacing); firstElement = false; };

        ImGui::SetCursorPosX(leftX);
        if (pin.shape != EPinShape_None)
        {
            sameLine();
            ed::BeginPin(pin, ed::PinKind::Input);
            drawPinIcon(pin);
            ed::EndPin();
        }
        if (!pin.name.empty())
        {
            sameLine();
            ImGui::TextUnformatted(pin.name.c_str());
        }
        if (hasInlineDefault(pin) && pin.dataType == EDataType::Vec3)
        {
            sameLine();
            oc::array<float, 3> v = parseVec3Literal(pin.defaultValue);
            ImGui::PushID(&pin);
            ImGui::SetNextItemWidth(defaultFieldWidth(pin));
            if (ImGui::DragFloat3("##v3", v.data(), 0.05f, 0.0f, 0.0f, "%.2f"))
                pin.defaultValue = formatVec3Literal(v);
            ImGui::PopID();
        }
        else if (hasInlineDefault(pin))
        {
            sameLine();
            char buf[96];
            const oc::string& value = pin.defaultValue;
            for (size_t k = 0; k < sizeof(buf); ++k)
                buf[k] = (k + 1 < sizeof(buf) && k < value.size()) ? value[k] : '\0';
            ImGui::PushID(&pin);
            ImGui::SetNextItemWidth(defaultFieldWidth(pin));
            // An Entity pin defaults to `self` and a Pointer pin to `nullptr`; there's no literal to type, you
            // retarget it by dragging a link into the pin. Show the value read-only so it can't be edited into
            // an invalid expression.
            const ImGuiInputTextFlags flags =
                (pin.dataType == EDataType::Entity || pin.dataType == EDataType::Pointer) ? ImGuiInputTextFlags_ReadOnly : 0;
            if (ImGui::InputText("##v", buf, sizeof(buf), flags) && !(flags & ImGuiInputTextFlags_ReadOnly))
                pin.defaultValue = buf; // echoed live as typed; validated/reformatted once editing ends below
            // Type-check on commit (not per-keystroke, so partial input like "-" or "3." can still be typed):
            // reformat into a canonical literal ("3" -> "3.0f", "TRUE" -> "true"), or fall back to the type's
            // default if what's there doesn't parse as this pin's type. "default" always passes through as-is.
            if (ImGui::IsItemDeactivatedAfterEdit())
            {
                if (auto formatted = formatLiteral(pin.dataType, pin.defaultValue))
                    pin.defaultValue = *formatted;
                else
                    pin.defaultValue = defaultValueForType(pin.dataType);
            }
            ImGui::PopID();
        }
    }

    // Output row, right aligned so its right edge lands on rightX: name [icon].
    void renderOutputRow(Pin& pin, float rightX, float spacing)
    {
        ImGui::SetCursorPosX(rightX - measureRow(pin, spacing));
        bool firstElement = true;
        auto sameLine = [&] { if (!firstElement) ImGui::SameLine(0.0f, spacing); firstElement = false; };

        if (!pin.name.empty())
        {
            sameLine();
            ImGui::TextUnformatted(pin.name.c_str());
        }
        if (pin.shape != EPinShape_None)
        {
            sameLine();
            ed::BeginPin(pin, ed::PinKind::Output);
            drawPinIcon(pin);
            ed::EndPin();
        }
    }
}

namespace
{
    // Keep member names valid C++ identifiers as the user types: strip anything but [A-Za-z0-9_] and don't
    // let a digit lead. Empty stays empty (the row still renders; codegen just yields a broken field the
    // compiler flags, which surfaces in the log).
    oc::string sanitizeIdentifier(const char* text)
    {
        oc::string out;
        for (const char* p = text; *p; ++p)
        {
            const char c = *p;
            const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
            if (!ok) continue;
            if (out.empty() && c >= '0' && c <= '9') continue; // no leading digit
            out += c;
        }
        return out;
    }
}

// Script Data node: an editable list of persistent members. Each row is [type][name][x] with the member's
// output pin on the right edge. Structural edits are recorded (m_memberRemoveRequest / m_membersDirty) for
// Scene to apply, since it owns the links that touch these pins.
void Node::updateDynamic(bool firstFrame)
{
    if (firstFrame)
        ed::SetNodePosition(*this, m_initialPos);

    const ImGuiStyle& style = ImGui::GetStyle();
    const float spacing = style.ItemSpacing.x;

    // Column widths: a fixed type button (widest token), a fixed name field, a square remove button, the pin.
    float typeBtnW = 0.0f;
    for (EDataType t : memberTypes)
        typeBtnW = fmaxf(typeBtnW, ImGui::CalcTextSize(memberTypeToken(t)).x);
    typeBtnW += style.FramePadding.x * 2.0f + 8.0f;
    const float nameW = 110.0f;
    const float removeW = ImGui::GetFrameHeight();
    const float rowW = typeBtnW + spacing + nameW + spacing + removeW + spacing + kPinIcon;

    const float titleW = ImGui::CalcTextSize(m_name.c_str()).x;
    const char* addLabel = "+ Add Member";
    const float addW = ImGui::CalcTextSize(addLabel).x + style.FramePadding.x * 2.0f;
    const float nodeW = fmaxf(fmaxf(rowW, titleW), addW);

    ed::BeginNode(*this);
    ImGui::PushID(this);

    const ImVec2 nodeOriginScreen = ImGui::GetCursorScreenPos();
    const float nodeLeftX = ImGui::GetCursorPosX();

    ImGui::SetCursorPosX(nodeLeftX + fmaxf(0.0f, (nodeW - titleW) * 0.5f));
    ImGui::TextUnformatted(m_name.c_str());

    const float dividerY = ImGui::GetItemRectMax().y + 2.0f;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(nodeOriginScreen.x, dividerY),
        ImVec2(nodeOriginScreen.x + nodeW, dividerY), ImColor(255, 255, 255, 40), 1.0f);
    ImGui::Dummy(ImVec2(nodeW, 4.0f));

    for (int i = 0; i < (int)m_outputPins.size(); ++i)
    {
        Pin& pin = *m_outputPins[i];
        ImGui::PushID(i);
        ImGui::SetCursorPosX(nodeLeftX);

        // Every edit is recorded as an op; Scene applies it to all Script Data nodes so their member sets stay
        // identical. The pins update when Scene replays the op (same frame), not here.

        // Type button: click to cycle to the next member type.
        if (ImGui::Button(memberTypeToken(pin.dataType), ImVec2(typeBtnW, 0.0f)))
        {
            constexpr int typeCount = (int)(sizeof(memberTypes) / sizeof(memberTypes[0]));
            int idx = 0;
            for (int k = 0; k < typeCount; ++k)
                if (memberTypes[k] == pin.dataType) { idx = k; break; }
            m_pendingEdit = { EMemberOp::Retype, i, memberTypes[(idx + 1) % typeCount], "" };
        }

        ImGui::SameLine(0.0f, spacing);
        char buf[64];
        for (size_t k = 0; k < sizeof(buf); ++k)
            buf[k] = (k + 1 < sizeof(buf) && k < pin.name.size()) ? pin.name[k] : '\0';
        ImGui::SetNextItemWidth(nameW);
        if (ImGui::InputText("##name", buf, sizeof(buf)))
            m_pendingEdit = { EMemberOp::Rename, i, pin.dataType, sanitizeIdentifier(buf) };

        ImGui::SameLine(0.0f, spacing);
        if (ImGui::Button("x", ImVec2(removeW, 0.0f)))
            m_pendingEdit = { EMemberOp::Remove, i, pin.dataType, "" };

        ImGui::SameLine(0.0f, 0.0f);
        ImGui::SetCursorPosX(nodeLeftX + nodeW - kPinIcon);
        ed::BeginPin(pin, ed::PinKind::Output);
        drawPinIcon(pin);
        ed::EndPin();

        ImGui::PopID();
    }

    ImGui::SetCursorPosX(nodeLeftX);
    if (ImGui::Button(addLabel, ImVec2(nodeW, 0.0f)))
        m_pendingEdit = { EMemberOp::Add, -1, EDataType::Float, "" };

    ImGui::PopID();
    ed::EndNode();
}

// On Event node: an editable list of named entries. Each row is [name][x] with the entry's Exec output pin
// on the right edge; something else fires an entry by name at runtime (ScriptComponent::fireEvent).
void Node::updateEvent(bool firstFrame)
{
    if (firstFrame)
        ed::SetNodePosition(*this, m_initialPos);

    const ImGuiStyle& style = ImGui::GetStyle();
    const float spacing = style.ItemSpacing.x;

    const float nameW = 110.0f;
    const float removeW = ImGui::GetFrameHeight();
    const float rowW = nameW + spacing + removeW + spacing + kPinIcon;

    const float titleW = ImGui::CalcTextSize(m_name.c_str()).x;
    const char* addLabel = "+ Add Event";
    const float addW = ImGui::CalcTextSize(addLabel).x + style.FramePadding.x * 2.0f;
    const float nodeW = fmaxf(fmaxf(rowW, titleW), addW);

    ed::BeginNode(*this);
    ImGui::PushID(this);

    const ImVec2 nodeOriginScreen = ImGui::GetCursorScreenPos();
    const float nodeLeftX = ImGui::GetCursorPosX();

    ImGui::SetCursorPosX(nodeLeftX + fmaxf(0.0f, (nodeW - titleW) * 0.5f));
    ImGui::TextUnformatted(m_name.c_str());

    const float dividerY = ImGui::GetItemRectMax().y + 2.0f;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(nodeOriginScreen.x, dividerY),
        ImVec2(nodeOriginScreen.x + nodeW, dividerY), ImColor(255, 255, 255, 40), 1.0f);
    ImGui::Dummy(ImVec2(nodeW, 4.0f));

    for (int i = 0; i < (int)m_outputPins.size(); ++i)
    {
        Pin& pin = *m_outputPins[i];
        ImGui::PushID(i);
        ImGui::SetCursorPosX(nodeLeftX);

        // Every edit is recorded as an op; Scene applies it to all On Event nodes so their entry sets stay
        // identical. The pins update when Scene replays the op (same frame), not here.
        char buf[64];
        for (size_t k = 0; k < sizeof(buf); ++k)
            buf[k] = (k + 1 < sizeof(buf) && k < pin.name.size()) ? pin.name[k] : '\0';
        ImGui::SetNextItemWidth(nameW);
        if (ImGui::InputText("##name", buf, sizeof(buf)))
            m_pendingEdit = { EMemberOp::Rename, i, EDataType::Exec, buf };

        ImGui::SameLine(0.0f, spacing);
        if (ImGui::Button("x", ImVec2(removeW, 0.0f)))
            m_pendingEdit = { EMemberOp::Remove, i, EDataType::Exec, "" };

        ImGui::SameLine(0.0f, 0.0f);
        ImGui::SetCursorPosX(nodeLeftX + nodeW - kPinIcon);
        ed::BeginPin(pin, ed::PinKind::Output);
        drawPinIcon(pin);
        ed::EndPin();

        ImGui::PopID();
    }

    ImGui::SetCursorPosX(nodeLeftX);
    if (ImGui::Button(addLabel, ImVec2(nodeW, 0.0f)))
        m_pendingEdit = { EMemberOp::Add, -1, EDataType::Exec, "" };

    ImGui::PopID();
    ed::EndNode();
}

// Function Input / Function Output node: a function-name field plus an editable list of typed pins (the
// parameters or return values). pin 0 is the fixed exec pin (output for Input, input for Output) that rides
// the title line; the editable pins follow. Edits are recorded as MemberEdits with absolute pin indices and
// applied by Scene (which owns the links). inputSide picks which pin list is the editable one.
void Node::updateFunctionIO(bool firstFrame, bool inputSide)
{
    if (firstFrame)
        ed::SetNodePosition(*this, m_initialPos);

    const ImGuiStyle& style = ImGui::GetStyle();
    const float spacing = style.ItemSpacing.x;
    auto& editPins = inputSide ? m_inputPins : m_outputPins;

    float typeBtnW = 0.0f;
    for (EDataType t : memberTypes)
        typeBtnW = fmaxf(typeBtnW, ImGui::CalcTextSize(memberTypeToken(t)).x);
    typeBtnW += style.FramePadding.x * 2.0f + 8.0f;
    const float nameW = 100.0f;
    const float removeW = ImGui::GetFrameHeight();
    const float rowW = kPinIcon + spacing + typeBtnW + spacing + nameW + spacing + removeW;

    const char* title = inputSide ? "Function Output" : "Function Input";
    const float titleW = ImGui::CalcTextSize(title).x + kPinIcon + spacing;
    const char* addLabel = inputSide ? "+ Add Return" : "+ Add Param";
    const float addW = ImGui::CalcTextSize(addLabel).x + style.FramePadding.x * 2.0f;
    const float nodeW = fmaxf(fmaxf(fmaxf(rowW, titleW), 140.0f), addW);

    ed::BeginNode(*this);
    ImGui::PushID(this);

    const ImVec2 nodeOriginScreen = ImGui::GetCursorScreenPos();
    const float nodeLeftX = ImGui::GetCursorPosX();

    // Title line with the fixed exec pin: left (exec-in) for Function Output, right (exec-out) for Function Input.
    ImGui::BeginGroup();
    ImGui::SetCursorPosX(nodeLeftX);
    if (inputSide && !m_inputPins.empty())
    {
        ed::BeginPin(*m_inputPins[0], ed::PinKind::Input);
        drawPinIcon(*m_inputPins[0]);
        ed::EndPin();
        ImGui::SameLine(0.0f, spacing);
    }
    ImGui::TextUnformatted(title);
    if (!inputSide && !m_outputPins.empty())
    {
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::SetCursorPosX(nodeLeftX + nodeW - kPinIcon);
        ed::BeginPin(*m_outputPins[0], ed::PinKind::Output);
        drawPinIcon(*m_outputPins[0]);
        ed::EndPin();
    }
    ImGui::EndGroup();

    const float dividerY = ImGui::GetItemRectMax().y + 2.0f;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(nodeOriginScreen.x, dividerY),
        ImVec2(nodeOriginScreen.x + nodeW, dividerY), ImColor(255, 255, 255, 40), 1.0f);
    ImGui::Dummy(ImVec2(nodeW, 4.0f));

    // Function name field — only on Function Input (it names the function). Function Output has no name; it
    // pairs to the Input whose exec flow reaches it (see Scene codegen), so it needs no identity of its own.
    if (!inputSide)
    {
        char buf[64];
        for (size_t k = 0; k < sizeof(buf); ++k)
            buf[k] = (k + 1 < sizeof(buf) && k < m_funcName.size()) ? m_funcName[k] : '\0';
        ImGui::SetCursorPosX(nodeLeftX);
        ImGui::SetNextItemWidth(nodeW);
        ImGui::PushID("fn");
        if (ImGui::InputText("##fnname", buf, sizeof(buf)))
            setFunctionName(sanitizeIdentifier(buf));
        ImGui::PopID();
    }

    // Editable typed pins (pin 0 is the exec pin, skipped).
    for (int i = 1; i < (int)editPins.size(); ++i)
    {
        Pin& pin = *editPins[i];
        ImGui::PushID(i);
        ImGui::SetCursorPosX(nodeLeftX);

        if (inputSide) // Function Output: the return's input pin sits on the left, before the controls.
        {
            ed::BeginPin(pin, ed::PinKind::Input);
            drawPinIcon(pin);
            ed::EndPin();
            ImGui::SameLine(0.0f, spacing);
        }

        if (ImGui::Button(memberTypeToken(pin.dataType), ImVec2(typeBtnW, 0.0f)))
        {
            constexpr int typeCount = (int)(sizeof(memberTypes) / sizeof(memberTypes[0]));
            int idx = 0;
            for (int k = 0; k < typeCount; ++k)
                if (memberTypes[k] == pin.dataType) { idx = k; break; }
            m_pendingEdit = { EMemberOp::Retype, i, memberTypes[(idx + 1) % typeCount], "" };
        }

        ImGui::SameLine(0.0f, spacing);
        char buf[64];
        for (size_t k = 0; k < sizeof(buf); ++k)
            buf[k] = (k + 1 < sizeof(buf) && k < pin.name.size()) ? pin.name[k] : '\0';
        ImGui::SetNextItemWidth(nameW);
        if (ImGui::InputText("##name", buf, sizeof(buf)))
            m_pendingEdit = { EMemberOp::Rename, i, pin.dataType, sanitizeIdentifier(buf) };

        ImGui::SameLine(0.0f, spacing);
        if (ImGui::Button("x", ImVec2(removeW, 0.0f)))
            m_pendingEdit = { EMemberOp::Remove, i, pin.dataType, "" };

        if (!inputSide) // Function Input: the param's output pin sits on the right, after the controls.
        {
            ImGui::SameLine(0.0f, 0.0f);
            ImGui::SetCursorPosX(nodeLeftX + nodeW - kPinIcon);
            ed::BeginPin(pin, ed::PinKind::Output);
            drawPinIcon(pin);
            ed::EndPin();
        }

        ImGui::PopID();
    }

    ImGui::SetCursorPosX(nodeLeftX);
    if (ImGui::Button(addLabel, ImVec2(nodeW, 0.0f)))
        m_pendingEdit = { EMemberOp::Add, -1, EDataType::Float, "" };

    ImGui::PopID();
    ed::EndNode();
}

void Node::updateLabel(bool firstFrame)
{
    if (firstFrame)
        ed::SetNodePosition(*this, m_initialPos);

    ed::PushStyleVar(ed::StyleVar_NodePadding,     ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ed::PushStyleColor(ed::StyleColor_NodeBg,      ImVec4(1.0f, 1.0f, 1.0f, 0.05f)); // faint fill over the whole box
    ed::PushStyleColor(ed::StyleColor_NodeBorder,  ImVec4(1.0f, 1.0f, 1.0f, 0.35f)); // the box outline
    ed::PushStyleColor(ed::StyleColor_GroupBg,     ImVec4(0.0f, 0.0f, 0.0f, 0.0f));   // transparent body (no black box)
    ed::PushStyleColor(ed::StyleColor_GroupBorder, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));   // node border already frames it

    ImVec2 nodeSize = ed::GetNodeSize(*this);
    const float headerH = ImGui::GetFrameHeight() + 8.0f;
    nodeSize.y -= headerH;

    // When the script window isn't visible.. node size explodes for some reason.. work around it by forcing size back down..
    const float maxSizeChangeHack = 200.0f;
    if (m_labelSize.x != 0.0f && m_labelSize.y != 0.0f)
    {
        if (!firstFrame)
        {
            if (nodeSize.x > (m_labelSize.x + maxSizeChangeHack) || nodeSize.y > (m_labelSize.y + maxSizeChangeHack))
                ed::SetGroupSize(*this, m_labelSize);
            else
                m_labelSize = nodeSize;
        }
    }
    else
    {
        m_labelSize = ImVec2(220.0f, 140.0f); // initial size
    }

    ed::BeginNode(*this);
    ImGui::PushID(this);
    const ImVec2 headerMin = ImGui::GetCursorScreenPos();

    // Title bar caption, overlaid inside the header strip (drawn via an absolute cursor, so it never adds to
    // the node's measured height). Plain text keeps the bar grabbable for dragging; double-click to rename.
    ImGui::SetCursorScreenPos(ImVec2(headerMin.x + 10.0f, headerMin.y + 4.0f));
    if (m_editingLabel)
    {
        char buf[128];
        for (size_t k = 0; k < sizeof(buf); ++k)
            buf[k] = (k + 1 < sizeof(buf) && k < m_labelText.size()) ? m_labelText[k] : '\0';
        ImGui::SetNextItemWidth(fmaxf(m_labelSize.x - 20.0f, 40.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(1.0f, 1.0f, 1.0f, 0.10f));
        if (m_labelFocusPending) { ImGui::SetKeyboardFocusHere(); m_labelFocusPending = false; }
        if (ImGui::InputText("##label", buf, sizeof(buf)))
            m_labelText = buf;
        ImGui::PopStyleColor();
        if (ImGui::IsItemDeactivated())
            m_editingLabel = false; // Enter or focus loss ends the rename
    }
    else
    {
        ImGui::TextUnformatted(m_labelText.empty() ? " " : m_labelText.c_str());
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        {
            m_editingLabel = true;
            m_labelFocusPending = true;
        }
    }

    // Divider under the title bar.
    ImGui::GetWindowDrawList()->AddLine(
        ImVec2(headerMin.x, headerMin.y + headerH - 1.0f),
        ImVec2(headerMin.x + m_labelSize.x, headerMin.y + headerH - 1.0f),
        ImColor(255, 255, 255, 40), 1.0f);

    // Reserve the body exactly headerH below the top (explicit position → no auto-spacing added).
    ImGui::SetCursorScreenPos(ImVec2(headerMin.x, headerMin.y + headerH));
    
    ed::Group(m_labelSize); // the resizable body (uses m_labelSize on the first frame, its own size after)

    ImGui::PopID();
    ed::EndNode();
    ed::PopStyleColor(4);
    ed::PopStyleVar();
}

// Reroute waypoint: a small dot with its input pin on the left edge and output pin on the right edge, leaving
// the middle grabbable so the node can be dragged. Links route into the left and out of the right.
void Node::updateReroute(bool firstFrame)
{
    if (firstFrame)
        ed::SetNodePosition(*this, m_initialPos);

    Pin* in  = m_inputPins.empty()  ? nullptr : m_inputPins[0].get();
    Pin* out = m_outputPins.empty() ? nullptr : m_outputPins[0].get();

    constexpr float w = 18.0f, h = 18.0f; // square + full rounding below => the selection border is a circle
    ed::PushStyleVar(ed::StyleVar_NodePadding, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ed::PushStyleVar(ed::StyleVar_NodeRounding, w * 0.5f);
    ed::PushStyleVar(ed::StyleVar_LinkStrength, 0.0f); // let links meet the dot at their true angle, not forced horizontal
    ed::PushStyleColor(ed::StyleColor_NodeBg,     ImVec4(0.0f, 0.0f, 0.0f, 0.0f)); // no node body — just the dot
    ed::PushStyleColor(ed::StyleColor_NodeBorder, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ed::BeginNode(*this);
    ImGui::PushID(this);

    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(w, h));

    const ImVec2 c(p.x + w * 0.5f, p.y + h * 0.5f);
    const uint32 col = in ? in->color : 0xFFFFFFFFu;
    ImGui::GetWindowDrawList()->AddCircleFilled(c, 5.0f, col, 12);

    // Pins pivot at the dot centre (so links meet at the dot) but keep only a tiny hit rect tucked in the far
    // corner — the whole centre stays draggable as the node body, and the pins are effectively not grabbable.
    // Overlaid back at the dummy's top-left so their (empty) groups add no height, keeping the bounds square.
    constexpr float pinW = 2.0f;
    if (in)
    {
        ImGui::SetCursorScreenPos(p);
        ed::BeginPin(*in, ed::PinKind::Input);
        ed::PinPivotRect(c, c);
        ed::PinRect(ImVec2(p.x, p.y), ImVec2(p.x + pinW, p.y + pinW));
        ed::EndPin();
    }
    if (out)
    {
        ImGui::SetCursorScreenPos(p);
        ed::BeginPin(*out, ed::PinKind::Output);
        ed::PinPivotRect(c, c);
        ed::PinRect(ImVec2(p.x + w - pinW, p.y), ImVec2(p.x + w, p.y + pinW));
        ed::EndPin();
    }

    ImGui::PopID();
    ed::EndNode();
    ed::PopStyleColor(2);
    ed::PopStyleVar(3);
}

void Node::update(double /*deltaSec*/, bool firstFrame)
{
    if (isDynamic())
    {
        updateDynamic(firstFrame);
        return;
    }
    if (isEventNode())
    {
        updateEvent(firstFrame);
        return;
    }
    if (isFunctionInput())
    {
        updateFunctionIO(firstFrame, false);
        return;
    }
    if (isFunctionOutput())
    {
        updateFunctionIO(firstFrame, true);
        return;
    }
    if (isLabel())
    {
        updateLabel(firstFrame);
        return;
    }
    if (isReroute())
    {
        updateReroute(firstFrame);
        return;
    }

    if (firstFrame)
        ed::SetNodePosition(*this, m_initialPos);

    ed::BeginNode(*this);
    ImGui::PushID(this);

    const float spacing = ImGui::GetStyle().ItemSpacing.x;

    // A single exec pin rides on the title line (input left, output right). Data pins go in the body
    // columns; so do exec pins when there are 2+ of them on that side (e.g. Branch's true/false outputs,
    // Trigger Audio's alias inputs) so each keeps its label instead of losing it to the title line.
    int execInputCount = 0;
    for (const auto& pinPtr : m_inputPins)
        if (pinPtr->dataType == EDataType::Exec)
            ++execInputCount;

    // Trigger Audio always body-routes its exec inputs: they are named sound aliases, and the label
    // matters even when the component declares just one sound.
    const bool titleInputExecAllowed = execInputCount == 1 && !isTriggerAudio();

    Pin* inputExec = nullptr;
    oc::vector<Pin*> bodyInputs;
    for (const auto& pinPtr : m_inputPins)
    {
        if (titleInputExecAllowed && pinPtr->dataType == EDataType::Exec && !inputExec)
            inputExec = pinPtr.get();
        else
            bodyInputs.push_back(pinPtr.get());
    }

    int execOutputCount = 0;
    for (const auto& pinPtr : m_outputPins)
        if (pinPtr->dataType == EDataType::Exec)
            ++execOutputCount;

    Pin* titleOutputExec = nullptr;
    oc::vector<Pin*> bodyOutputs;
    for (const auto& pinPtr : m_outputPins)
    {
        if (execOutputCount == 1 && pinPtr->dataType == EDataType::Exec && !titleOutputExec)
            titleOutputExec = pinPtr.get();
        else
            bodyOutputs.push_back(pinPtr.get());
    }

    // Measure first so the layout is stable on the very first frame (no feedback from GetNodeSize).
    float inputColW = 0.0f;
    for (Pin* pin : bodyInputs)
        inputColW = fmaxf(inputColW, measureRow(*pin, spacing));
    float outputColW = 0.0f;
    for (Pin* pin : bodyOutputs)
        outputColW = fmaxf(outputColW, measureRow(*pin, spacing));

    // Optional dropdown property (e.g. the Conditional's comparison operator), shown as a cycle button.
    const NodeDef* def = findNodeDef(m_typeId);
    const bool hasProperty = def && !def->enumOptions.empty();
    float propW = 0.0f;
    if (hasProperty)
    {
        for (const oc::string& option : def->enumOptions)
            propW = fmaxf(propW, ImGui::CalcTextSize(option.c_str()).x);
        propW += ImGui::GetStyle().FramePadding.x * 2.0f + 8.0f;
        if (m_enumSelection < 0 || m_enumSelection >= (int)def->enumOptions.size())
            m_enumSelection = 0;
    }

    const float titleW = ImGui::CalcTextSize(m_name.c_str()).x;
    const float leftExecW = inputExec ? (kPinIcon + spacing) : 0.0f;
    const float rightExecW = titleOutputExec ? (spacing + kPinIcon) : 0.0f;
    const float titleLineW = leftExecW + titleW + rightExecW;
    const float bodyW = inputColW + ((inputColW > 0.0f && outputColW > 0.0f) ? kColumnGap : 0.0f) + outputColW;
    const float nodeW = fmaxf(fmaxf(titleLineW, bodyW), propW);

    const ImVec2 nodeOriginScreen = ImGui::GetCursorScreenPos();
    const float nodeLeftX = ImGui::GetCursorPosX();

    // ---- title line: [exec in] title [exec out] ----
    ImGui::BeginGroup();
    ImGui::SetCursorPosX(nodeLeftX);
    if (inputExec)
    {
        ed::BeginPin(*inputExec, ed::PinKind::Input);
        drawPinIcon(*inputExec);
        ed::EndPin();
        ImGui::SameLine(0.0f, 0.0f);
    }
    ImGui::SetCursorPosX(nodeLeftX + leftExecW + fmaxf(0.0f, (nodeW - leftExecW - rightExecW - titleW) * 0.5f));
    ImGui::TextUnformatted(m_name.c_str());
    if (titleOutputExec)
    {
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::SetCursorPosX(nodeLeftX + nodeW - kPinIcon);
        ed::BeginPin(*titleOutputExec, ed::PinKind::Output);
        drawPinIcon(*titleOutputExec);
        ed::EndPin();
    }
    ImGui::EndGroup();

    // Divider + spacing only when there's a body beneath the title. A node that carries nothing but its
    // title-line exec pins (e.g. Update, Break) stays compact — no line, no gap.
    const bool hasBody = !bodyInputs.empty() || !bodyOutputs.empty() || hasProperty;
    if (hasBody)
    {
        const float dividerY = ImGui::GetItemRectMax().y + 2.0f;
        ImGui::GetWindowDrawList()->AddLine(ImVec2(nodeOriginScreen.x, dividerY),
            ImVec2(nodeOriginScreen.x + nodeW, dividerY), ImColor(255, 255, 255, 40), 1.0f);
        ImGui::Dummy(ImVec2(nodeW, 4.0f)); // reserve full width + a little breathing room below the title
    }

    if (hasProperty)
    {
        const int count = (int)def->enumOptions.size();
        ImGui::SetCursorPosX(nodeLeftX + (nodeW - propW) * 0.5f);
        if (ImGui::Button(def->enumOptions[m_enumSelection].c_str(), ImVec2(propW, 0.0f)))
            m_enumSelection = (m_enumSelection + 1) % count; // click to cycle through the operators
        ImGui::Dummy(ImVec2(nodeW, 2.0f));
    }

    const float bodyTopY = ImGui::GetCursorPosY();

    // Input column (left), then output column (right) starting back at the same Y.
    for (Pin* pin : bodyInputs)
        renderInputRow(*pin, nodeLeftX, spacing);

    if (!bodyOutputs.empty())
        ImGui::SetCursorPosY(bodyTopY);
    for (Pin* pin : bodyOutputs)
        renderOutputRow(*pin, nodeLeftX + nodeW, spacing);

    ImGui::PopID();
    ed::EndNode();
}