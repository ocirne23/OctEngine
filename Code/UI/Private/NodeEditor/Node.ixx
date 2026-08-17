export module UI:Node;

import Core;
import Core.imgui;
import :imgui_node_editor;
import :NodeDef;

namespace NodeEditor
{
class Node;

export enum EPinShape : uint8
{
    EPinShape_Flow,
    EPinShape_Circle,
    EPinShape_Square,
    EPinShape_None,
};

export enum EPinType : uint8
{
    EPinType_Input,
    EPinType_Output,
};

export enum ENodeStyle : uint8
{
    ENodeStyle_Minimal,      // First input & output pins will be on title line
    ENodeStyle_MinSeparator, // First input & output pins will be on title line the rest underneath with separator
    ENodeStyle_Full,         // All pins underneath title with separator
};

// A structural edit the Script Data in-node editor recorded this frame. Scene consumes it and replays it on
// every Script Data node so they all keep the same member set (one shared, generated struct). One op/frame
// (the user does one thing at a time); the node applies nothing itself.
export enum class EMemberOp : uint8 { None, Add, Remove, Rename, Retype };
export struct MemberEdit
{
    EMemberOp   op = EMemberOp::None;
    int         index = -1;               // member row for Remove / Rename / Retype
    EDataType   type = EDataType::Float;   // new type for Add / Retype
    oc::string name;                      // new name for Rename
};

export struct Pin
{
    oc::string name;
    Node* node = nullptr;
    EPinType type;
    EPinShape shape;
    uint32 color;
    EDataType dataType = EDataType::Exec;
    oc::string defaultValue;          // C++ literal used for an unconnected data input
    int typeGroup = 0;                 // wildcard pins sharing a group resolve to the same concrete type
    EMutableType mutability = EMutableType::Readable; // input: required of its source; output: what it provides
    uint8 numConnections = 0;

    operator ed::PinId() const { return ed::PinId(this); }
};

export class Node
{
public:

	Node(uint32 nodeIdx) : m_nodeIdx(nodeIdx) {}
    Node& initialize(ImVec2 initialPos, oc::string name, ENodeStyle style, uint32 color);
    Node& initFromDef(ImVec2 initialPos, const NodeDef& def);
    Node& addInput(EDataType dataType, const oc::string& name, const oc::string& defaultValue);
    Node& addOutput(EDataType dataType, const oc::string& name);

    // Script Data node only: its output pins are user-defined persistent members. A member is a writable,
    // concretely-typed output pin. eraseOutputPin drops one (Scene fixes up its links first).
    bool isDynamic() const { return isScriptDataType(m_typeId); }
    Node& addMember(EDataType type, const oc::string& name);
    void  eraseOutputPin(int index);

    // The structural member edit recorded by the in-node editor this frame (op == None if none). Consumed by
    // Scene, which replays it across every Script Data node and fixes up the links it owns.
    MemberEdit takeMemberEdit();

    // Reroute waypoint: one input + one output pin of the carried type (added on creation / load).
    bool isReroute() const { return isRerouteType(m_typeId); }
    Node& makeReroute(EDataType type);

    // On Event node only: its exec output pins are user-named event entries. Uses the same MemberEdit/
    // takeMemberEdit machinery as Script Data (Scene replays edits across every On Event node); `type` in the
    // edit is unused here since every entry is Exec.
    bool isEventNode() const { return isEventEntryType(m_typeId); }
    Node& addEventEntry(const oc::string& name);

    // Trigger Audio node only: its exec INPUT pins are the target AudioComponent's sound aliases, kept in
    // front of the data inputs (synced from the entity by Scene::setAudioAliases, not user-edited).
    bool isTriggerAudio() const { return isTriggerAudioType(m_typeId); }
    Node& addAudioEntry(const oc::string& name);

    // Function boundary + call nodes. Function Input/Output edit their own typed pins (params/returns) via the
    // MemberEdit machinery, but are not synced across nodes. A Function Call mirrors an imported function's
    // signature and remembers which (script, function) it targets.
    bool isFunctionInput() const  { return isFunctionInputType(m_typeId); }
    bool isFunctionOutput() const { return isFunctionOutputType(m_typeId); }
    bool isFunctionCall() const   { return isFunctionCallType(m_typeId); }

    const oc::string& getFunctionName() const { return m_funcName; }
    void setFunctionName(const oc::string& name) { m_funcName = name; m_name = name.empty() ? m_name : name; }
    const oc::string& getFunctionScriptPath() const { return m_funcScriptPath; }
    void setFunctionScriptPath(const oc::string& path) { m_funcScriptPath = path; }

    Node& addParam(EDataType type, const oc::string& name);  // Function Input: a typed data OUTPUT pin
    Node& addReturn(EDataType type, const oc::string& name); // Function Output: a typed data INPUT pin
    void  eraseInputPin(int index);

    // Function Call: builds the fixed exec in/out plus one input per param and one output per return, in the
    // order the signature lists them.
    Node& makeFunctionCall(const oc::vector<oc::pair<EDataType, oc::string>>& params,
                           const oc::vector<oc::pair<EDataType, oc::string>>& returns);

    // Label (comment box) accessors, used by save/load to persist the caption and box size.
    bool isLabel() const { return isLabelType(m_typeId); }
    const oc::string& getLabelText() const { return m_labelText; }
    void setLabelText(const oc::string& text) { m_labelText = text; }
    ImVec2 getLabelSize() const { return m_labelSize; }
    void setLabelSize(ImVec2 size) { m_labelSize = size; }

    void update(double deltaSec, bool firstFrame);

	uint32 getNodeIdx() const { return m_nodeIdx; }
    ed::NodeId getId() const { return ed::NodeId(this); }
    operator ed::NodeId() const { return ed::NodeId(this); }

    const oc::string& getTypeId() const { return m_typeId; }
    const oc::vector<oc::unique_ptr<Pin>>& getInputPins() const { return m_inputPins; }
    const oc::vector<oc::unique_ptr<Pin>>& getOutputPins() const { return m_outputPins; }

    int getEnumSelection() const { return m_enumSelection; }
    void setEnumSelection(int selection) { m_enumSelection = selection; }

private:

	void updateDynamic(bool firstFrame); // in-node editor body for the Script Data node
	void updateEvent(bool firstFrame);   // in-node editor body for the On Event node
	void updateFunctionIO(bool firstFrame, bool inputSide); // in-node editor for Function Input/Output
	void updateLabel(bool firstFrame);   // group/comment box body for the Label node
	void updateReroute(bool firstFrame); // small draggable dot for the Reroute waypoint

	uint32 m_nodeIdx;
    ImVec2 m_initialPos;
    uint32 m_color;
    ENodeStyle m_style;
    oc::string m_name;
    oc::string m_typeId;
    int m_enumSelection = 0;             // index into the node def's enumOptions (dropdown property)
    MemberEdit m_pendingEdit;            // Script Data: structural member edit recorded this frame
    oc::string m_funcName;              // Function Input/Output/Call: the function's name (pairs I/O, titles Call)
    oc::string m_funcScriptPath;        // Function Call: the .scr the imported function lives in
    oc::string m_labelText = "Label";   // Label: title-bar caption
    ImVec2 m_labelSize = ImVec2(0.0f, 0.0f); // Label: current group box size (updated as the user resizes)
    bool m_editingLabel = false;         // Label: title bar is in rename (InputText) mode
    bool m_labelFocusPending = false;    // Label: focus the rename field on the frame editing begins
    oc::vector<oc::unique_ptr<Pin>> m_inputPins;
    oc::vector<oc::unique_ptr<Pin>> m_outputPins;
};

} // namespace NodeEditor
