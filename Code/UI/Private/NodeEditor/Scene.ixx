export module UI:Scene;

import Core;

import :imgui_node_editor;
import :Node;
import :Link;
import :NodeDef;

namespace NodeEditor
{

export class Scene
{
public:

    ~Scene();
    void initialize();
    void update(double deltaSec);

    void processInteractions();
    Node& createNode();
    Node& addNodeOfType(const oc::string& typeId, ImVec2 pos);

    void newGraph();                                  // reset to a minimal default graph
    bool loadFromFile(const oc::string& path);       // rebuild graph from the //@graph metadata
    bool saveToFile(const oc::string& path);         // write generated source (code + metadata)
    oc::string generateCpp();                        // node graph -> C++ source

    const oc::string& scriptPath() const { return m_scriptPath; }
    void setScriptPath(const oc::string& path) { m_scriptPath = path; }
    bool open(const oc::string& path) { setScriptPath(path); return loadFromFile(path); } // load + make current
    bool save() { return saveToFile(m_scriptPath); }                                       // write the current file

    bool isDirty(); // graph changed since it was last loaded/saved (nodes/links/pins/positions differ)

    // Queues a copy/paste for the next update() (which runs inside the editor's own Begin/End, where mouse
    // position and canvas transform are valid) — lets a caller outside the UI frame, e.g. a global keyboard
    // hook in main.cpp, trigger the same action as the in-editor Ctrl+C/Ctrl+V shortcut.
    void requestCopy() { m_pendingCopyRequest = true; }
    void requestPaste() { m_pendingPasteRequest = true; }

    // The sound aliases of the entity that owns the open script (its AudioComponent). Trigger Audio nodes
    // sync their exec entry pins to this list. null = unknown (no owning entity selected): pins stay as
    // loaded from the file, so editing a script standalone never strips them.
    void setAudioAliases(const oc::vector<oc::string>* aliases);

private:

    Node* findEntry(const char* nodeName) const;
    Node* findScriptData() const;                     // the Script Data node (persistent members), if any
    Node* findEventEntry() const;                      // the On Event node (named entries), if any
    void connectNodes(Node* from, int outIdx, Node* to, int inIdx);
    void resolveNodeTypes(Node* node); // propagate concrete types through the node's wildcard pin groups
    void removeNode(ed::NodeId nodeId);
    void insertReroute(Link* link, ImVec2 canvasPos); // split a link with a draggable reroute waypoint
    void deleteRerouteChain(Link* link);              // delete a whole routed connection from one of its links
    void removeMemberPin(Node* node, int index);      // drop a Script Data member + its links
    void pruneIncompatibleLinks(Node* node);          // drop links a member type-change made invalid
    void applyMemberEdit(const MemberEdit& edit);     // replay a member edit across all Script Data nodes
    void syncNewMemberNode(Node& newNode);            // seed a new Script Data node from the shared member set
    void applyEventEdit(const MemberEdit& edit);      // replay an entry edit across all On Event nodes
    void syncNewEventNode(Node& newNode);             // seed a new On Event node from the shared entry set
    void syncTriggerAudioPins(Node& node);            // reconcile a Trigger Audio node's alias pins with m_audioAliases
    void autoConnectPending(Node& node);               // wire m_pendingLinkPin to node's first matching pin, if any
    void pruneOrphanedReroute(Pin* pin);               // walk + delete a reroute chain left with no downstream link

    // ---- functions (imported reusable subgraphs) ----
    using PinSig = oc::vector<oc::pair<EDataType, oc::string>>; // ordered (type, name) list

    struct FunctionRef
    {
        oc::string scriptPath;   // .scr the function is defined in (relative, e.g. "Scripts/Math.scr")
        oc::string funcName;     // the function's name (its Function Input/Output pair share it)
        oc::string displayLabel; // "<file>: <func>" shown in the import menu
    };

    void applyFunctionEdit(Node* node, const MemberEdit& edit); // add/remove/rename/retype one param/return
    void removeInputPin(Node* node, int index);                 // drop an input pin + its links
    bool readFunctionSignature(const oc::string& path, const oc::string& funcName,
                               PinSig& params, PinSig& returns) const; // read a function's I/O from its file
    oc::vector<FunctionRef> scanImportableFunctions() const;   // every function defined in Assets/Scripts/*.scr
    Node& importFunction(const FunctionRef& ref, ImVec2 pos);   // create a Function Call node for a function
    void emitFunctions(oc::string& code);                      // emit C++ defs for every function used/defined here
    bool isFunctionScript() const;                              // true if this graph defines a function (Function Input)

    int  indexOfNode(const Node* node) const;
    oc::string serializeGraph();

    // ---- copy/paste (Ctrl+C/Ctrl+V), via the OS clipboard so it also works across different open scripts ----
    oc::string serializeSubset(const oc::vector<Node*>& nodes, const oc::vector<Link*>& links);
    void loadLinesIntoGraph(const oc::vector<oc::string>& lines, ImVec2 offset, oc::vector<Node*>& byIndex);
    void collectSelection(oc::vector<Node*>& nodes, oc::vector<Link*>& links) const; // every selected node + fully-selected link
    void pruneDanglingReroutes(oc::vector<Node*>& byIndex); // drop a pasted/duplicated reroute missing either link
    void copySelectedToClipboard();
    void pasteFromClipboard(ImVec2 canvasPos);

    // ---- node context menu (right-click a node) ----
    void duplicateSelection(ed::NodeId clickedId);        // clone clickedId + current selection, offset, select the clones
    void resetNodeToSpawnDefaults(ed::NodeId nodeId);     // restore input defaults to NodeDef's authored literals + disconnect

    ed::EditorContext* m_nodeEditorContext = nullptr;

    oc::vector<oc::unique_ptr<Node>> m_nodes;
    oc::vector<oc::unique_ptr<Link>> m_links;

    bool m_firstFrame = true;
    uint64_t m_idCounter = 1;
    oc::string m_scriptPath = "Scripts/Graph.scr";
    ImVec2 m_pendingAddPos = ImVec2(0.0f, 0.0f);
    ImVec2 m_pendingAddScreenPos = ImVec2(0.0f, 0.0f); // screen-space click pos, so the popup can offset from it
    Pin* m_pendingLinkPin = nullptr; // dangling end of a link dropped on canvas, to auto-connect to the spawned node
    bool m_pendingCopyRequest = false;  // set by requestCopy(), consumed + cleared at the top of the next update()
    bool m_pendingPasteRequest = false; // set by requestPaste(), consumed + cleared at the top of the next update()

    ed::NodeId m_contextNodeId;                                // node the right-click context menu targets
    ImVec2 m_nodeContextScreenPos = ImVec2(0.0f, 0.0f);        // screen-space click pos, so the popup can offset from it

    // Dirty tracking: the serialized graph captured right after a load/save, compared against the live
    // graph to detect unsaved edits. Captured lazily (once the graph has rendered a frame so node
    // positions are valid) — m_hasBaseline gates that.
    oc::string m_baselineState;
    bool m_hasBaseline = false;

    oc::vector<FunctionRef> m_importFunctions; // cached function list, refreshed when the add-node popup opens

    // ---- add-node popup search box ----
    char m_addNodeSearchBuf[64] = {}; // cleared + focused each time the popup opens (see IsWindowAppearing)
    int  m_addNodeSearchSelected = 0; // highlighted index into that frame's filtered results list
    bool m_addNodeSearchWasActive = false; // IsItemActive(), captured after last frame's InputText call

    oc::vector<oc::string> m_audioAliases; // Trigger Audio entry pins sync to this (see setAudioAliases)
    bool m_audioAliasesKnown = false;
};

} // namespace NodeEditor
