module UI;

import Core;
import File; // disk access goes through FileSystem
import Core.imgui;
import Core.SDL;
import :imgui_node_editor;
import :Node;
import :Link;
import :NodeDef;
import :Scene;

using namespace NodeEditor;

namespace
{
    // ---- graph traversal helpers (links store input pin in Id1, output pin in Id2) ----

    Pin* sourceOfInput(const std::vector<std::unique_ptr<Link>>& links, const Pin* inputPin)
    {
        for (const auto& link : links)
            if (link->getInputId().AsPointer<Pin>() == inputPin)
                return link->getOutputId().AsPointer<Pin>();
        return nullptr;
    }

    Pin* targetOfOutput(const std::vector<std::unique_ptr<Link>>& links, const Pin* outputPin)
    {
        for (const auto& link : links)
            if (link->getOutputId().AsPointer<Pin>() == outputPin)
                return link->getInputId().AsPointer<Pin>();
        return nullptr;
    }

    int indexOfPin(const std::vector<std::unique_ptr<Pin>>& pins, const Pin* pin)
    {
        for (int i = 0; i < (int)pins.size(); ++i)
            if (pins[i].get() == pin)
                return i;
        return -1;
    }

    // Reroute-transparent traversal for codegen: a Reroute is a pure waypoint (one in, one out), so follow
    // straight through it to the real source output / real target input.
    Pin* realSourceOfInput(const std::vector<std::unique_ptr<Link>>& links, const Pin* inputPin)
    {
        Pin* src = sourceOfInput(links, inputPin);
        while (src && isRerouteType(src->node->getTypeId()))
        {
            const auto& ins = src->node->getInputPins();
            src = ins.empty() ? nullptr : sourceOfInput(links, ins[0].get());
        }
        return src;
    }

    Pin* realTargetOfOutput(const std::vector<std::unique_ptr<Link>>& links, const Pin* outputPin)
    {
        Pin* tgt = targetOfOutput(links, outputPin);
        while (tgt && isRerouteType(tgt->node->getTypeId()))
        {
            const auto& outs = tgt->node->getOutputPins();
            tgt = outs.empty() ? nullptr : targetOfOutput(links, outs[0].get());
        }
        return tgt;
    }

    // Like realTargetOfOutput, but also collects every reroute waypoint passed through into `chain` (instead
    // of skipping straight past them) — used by addReroutesBetweenSelected to decide which waypoints belong
    // to a copy/paste. Returns the first non-reroute node reached, or nullptr at a dead end or a cyclic chain.
    Node* traceForwardThroughReroutes(const std::vector<std::unique_ptr<Link>>& links, Pin* outputPin, std::vector<Node*>& chain)
    {
        Pin* current = outputPin;
        std::set<Node*> visited;
        for (;;)
        {
            Pin* next = targetOfOutput(links, current);
            if (!next) return nullptr;
            Node* nextNode = next->node;
            if (!nextNode->isReroute())
                return nextNode;
            if (!visited.insert(nextNode).second)
                return nullptr; // cyclic reroute chain — bail out rather than loop forever
            chain.push_back(nextNode);
            if (nextNode->getOutputPins().empty())
                return nullptr;
            current = nextNode->getOutputPins()[0].get();
        }
    }

    // Extends `selected`/`nodes` with every reroute waypoint sitting on a link chain between two nodes that
    // are already both selected, so copying "node A to node B" preserves the routing shape instead of
    // silently dropping the connection: a reroute's two links each have exactly one endpoint on it, so
    // neither passes the "both ends selected" filter collectSelection applies afterward unless the reroute
    // is folded into the selection too. A single pass over each originally-selected node's outputs is enough
    // — the destination side of a qualifying chain is always one of those same originally-selected nodes,
    // never a reroute newly added by this function.
    void addReroutesBetweenSelected(std::set<Node*>& selected, std::vector<Node*>& nodes, const std::vector<std::unique_ptr<Link>>& links)
    {
        const std::vector<Node*> originalSelection(nodes.begin(), nodes.end());
        for (Node* node : originalSelection)
            for (const auto& outPin : node->getOutputPins())
            {
                std::vector<Node*> chain;
                Node* landedOn = traceForwardThroughReroutes(links, outPin.get(), chain);
                if (!landedOn || chain.empty() || !selected.count(landedOn))
                    continue;
                for (Node* reroute : chain)
                    if (selected.insert(reroute).second)
                        nodes.push_back(reroute);
            }
    }

    // The Function Output that belongs to a given Function Input: the first one reachable by walking exec
    // links forward from the Input (through branches/loops). Function Output carries no name of its own, so
    // this reachability is what pairs it to its Input.
    Node* reachableFunctionOutput(const std::vector<std::unique_ptr<Link>>& links, Node* inputNode)
    {
        std::set<Node*> visited;
        std::vector<Node*> stack{ inputNode };
        while (!stack.empty())
        {
            Node* n = stack.back();
            stack.pop_back();
            if (!visited.insert(n).second)
                continue;
            if (n != inputNode && n->isFunctionOutput())
                return n;
            for (const auto& pin : n->getOutputPins())
                if (pin->dataType == EDataType::Exec)
                    if (Pin* tgt = realTargetOfOutput(links, pin.get()))
                        stack.push_back(tgt->node);
        }
        return nullptr;
    }

    // ---- add-node popup search ----
    char lowerChar(char c) { return (c >= 'A' && c <= 'Z') ? char(c + 32) : c; }

    // Focuses the add-node search box AND makes sure SDL is actually ready to feed it text on the very next
    // keystroke. ImGui only calls SDL_StartTextInput() once its own backend notices a text field is active,
    // which (per this app's per-frame order — Input::update()'s SDL_PollEvent runs before UI::update()'s
    // ImGui_ImplSDL3_NewFrame(), the thing that actually calls it) normally lags a full frame behind the
    // widget gaining focus, silently swallowing whatever's typed during that frame. Calling it here too, the
    // instant the box is about to become active, closes that gap immediately instead of one frame late.
    // SDL_GetKeyboardFocus() sidesteps needing this deeply-nested UI code to carry an SDL_Window* around —
    // there's only ever the one app window anyway.
    void focusSearchBox()
    {
        ImGui::SetKeyboardFocusHere();
        SDL_StartTextInput(SDL_GetKeyboardFocus());
    }

    // Case-insensitive substring test, used to filter the add-node popup's palette/import-function list.
    bool containsCI(std::string_view haystack, std::string_view needle)
    {
        if (needle.empty()) return true;
        if (needle.size() > haystack.size()) return false;
        for (size_t i = 0; i + needle.size() <= haystack.size(); ++i)
        {
            bool ok = true;
            for (size_t j = 0; j < needle.size() && ok; ++j)
                ok = lowerChar(haystack[i + j]) == lowerChar(needle[j]);
            if (ok) return true;
        }
        return false;
    }

    // Best-effort ImGuiKey -> ASCII char for a letter/digit key, used to reconstruct the keystroke that
    // switches focus into the add-node search box the same frame it's pressed (see the AddNodePopup body):
    // SDL only generates text-input events while some text field already has focus, so the very keystroke
    // that hands focus to the search box never arrives through that path — this rebuilds it directly instead
    // of losing it. Only handles plain letters/digits (matching the "start typing" detection below); shifted
    // digit-row symbols ('!', '@', ...) aren't reconstructed since their layout varies.
    char charForTypingKey(ImGuiKey key, bool shift)
    {
        if (key >= ImGuiKey_A && key <= ImGuiKey_Z)
            return char((shift ? 'A' : 'a') + (key - ImGuiKey_A));
        if (key >= ImGuiKey_0 && key <= ImGuiKey_9)
            return char('0' + (key - ImGuiKey_0));
        return 0;
    }

    // Splits a clipping's text blob into lines, used by both Ctrl+V and node-context-menu Duplicate.
    std::vector<std::string> splitLines(const std::string& text)
    {
        std::vector<std::string> lines;
        size_t pos = 0;
        while (pos <= text.size())
        {
            const size_t nl = text.find('\n', pos);
            if (nl == std::string::npos) { lines.push_back(text.substr(pos)); break; }
            lines.push_back(text.substr(pos, nl - pos));
            pos = nl + 1;
        }
        return lines;
    }

    // ---- function codegen naming ----
    // A generated C++ function is named <fileStem>_<funcName>, sanitized to a valid identifier. The stem
    // disambiguates same-named functions from different files; codegen dedups by this full name.
    std::string sanitizeIdent(const std::string& in)
    {
        std::string out;
        for (char c : in)
        {
            const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
            out += ok ? c : '_';
        }
        if (out.empty() || (out[0] >= '0' && out[0] <= '9'))
            out = "_" + out;
        return out;
    }

    std::string pathStem(const std::string& path)
    {
        const size_t slash = path.find_last_of("/\\");
        const size_t start = (slash == std::string::npos) ? 0 : slash + 1;
        size_t dot = path.find_last_of('.');
        if (dot == std::string::npos || dot < start) dot = path.size();
        return path.substr(start, dot - start);
    }

    std::string funcCppName(const std::string& scriptPath, const std::string& funcName)
    {
        return sanitizeIdent(pathStem(scriptPath) + "_" + funcName);
    }

    // Splits an output pin's value template into an optional hoisted declaration and its reference/inline
    // expression, returning true when a declaration is present. Two forms produce a hoist:
    //   - the pin's own expr carries a HOIST marker: <decl> \x03 <ref>  (e.g. a Var node)
    //   - the pin has a plain expr AND the node's emit carries a HOIST marker: the node emit supplies a
    //     single declaration shared by all the node's outputs, and the pin expr is the reference into it
    //     (e.g. SplitVec3 declares `glm::vec3 v@` once, then x/y/z read `v@.x` / `v@.y` / `v@.z`).
    bool hoistParts(const Pin* outPin, std::string& decl, std::string& ref)
    {
        decl.clear(); ref.clear();
        const NodeDef* def = findNodeDef(outPin->node->getTypeId());
        if (!def) return false;
        const int outIdx = indexOfPin(outPin->node->getOutputPins(), outPin);
        if (outIdx < 0 || outIdx >= (int)def->outputs.size()) return false;
        const std::string& e = def->outputs[outIdx].expr;

        if (const auto hp = e.find(HOIST); hp != std::string::npos)
        {
            decl = e.substr(0, hp);
            ref  = e.substr(hp + 1);
            return true;
        }
        if (!e.empty())
            if (const auto hp = def->emit.find(HOIST); hp != std::string::npos)
            {
                decl = def->emit.substr(0, hp);
                ref  = e;
                return true;
            }
        return false;
    }

    // Can these two pins be linked? Exec only joins exec; the output's read/write capability must cover what
    // the input requires; an unresolved wildcard accepts any value type, otherwise the concrete types match.
    bool pinsCompatible(const Pin* input, const Pin* output)
    {
        if (input->dataType == EDataType::Exec || output->dataType == EDataType::Exec)
            return input->dataType == EDataType::Exec && output->dataType == EDataType::Exec && output->numConnections == 0;

        // Mutability: e.g. a Writable input (writes to its source) can't take a read-only rvalue output.
        if ((mutableBits(input->mutability) & ~mutableBits(output->mutability)) != 0)
            return false;

        const bool inputWild = input->typeGroup != 0 && input->dataType == EDataType::Wildcard;
        const bool outputWild = output->typeGroup != 0 && output->dataType == EDataType::Wildcard;
        if (inputWild || outputWild)
            return true;
        return input->dataType == output->dataType;
    }

    // ---- code generation ----
    //
    // Exec traversal and data resolution keep SEPARATE recursion stacks: a node that sits in the exec
    // flow can still feed its data output into a downstream node (e.g. Conditional), which is not a cycle.
    //
    // Hoisting is explicit only: a data output whose expr carries a HOIST marker (e.g. a Var node) is
    // declared once at function scope and referenced by name; every other data output is expanded inline.

    using HoistMap = std::map<const Pin*, std::string>; // HOIST output pin -> variable name it resolves to

    struct Codegen
    {
        const std::vector<std::unique_ptr<Link>>& links;
        std::string         globalDecls;    // HOIST variable declarations, emitted once at the top of the function
        std::set<std::string> globalDeclared; // declaration texts already added to globalDecls (dedups shared decls)
    };

    std::string emitDataExpr(Codegen& cg, const Pin* inputPin, std::set<const Node*>& dataStack, const HoistMap& hoist);
    // enteredPin = the exec INPUT pin the flow arrived through; only Trigger Audio (whose entry pins select
    // the sound to play) cares, every other node emits the same statement regardless of entry.
    std::string emitExecChain(Codegen& cg, Node* node, std::set<const Node*>& execStack, const Pin* enteredPin = nullptr);
    std::string expandOutput(Codegen& cg, const Pin* outPin, std::set<const Node*>& dataStack, const HoistMap& hoist);

    // @ -> unique node idx
	void appendNodeIdx(Codegen& cg, std::string& out, Node* node)
	{
        out += std::to_string(node->getNodeIdx());
    }

    // ENUM_TOKEN -> the node's selected dropdown-property code token.
    void appendEnumToken(Codegen& cg, std::string& out, Node* node)
    {
        const NodeDef* def = findNodeDef(node->getTypeId());
        const int sel = node->getEnumSelection();
        if (def && sel >= 0 && sel < (int)def->enumTokens.size())
            out += def->enumTokens[sel];
    }

    std::string inputExpr(Codegen& cg, Node* node, int idx, std::set<const Node*>& dataStack, const HoistMap& hoist)
    {
        const auto& inputs = node->getInputPins();
        return (idx >= 0 && idx < (int)inputs.size()) ? emitDataExpr(cg, inputs[idx].get(), dataStack, hoist) : "0";
    }

    // Value-expression template ($k = data input k, ENUM_TOKEN = enum token). Data resolution threads dataStack.
    std::string substituteData(Codegen& cg, const std::string& tmpl, Node* node, std::set<const Node*>& dataStack, const HoistMap& hoist)
    {
        std::string out;
        for (size_t i = 0; i < tmpl.size();)
        {
            const char c = tmpl[i];
            if (c == '$' && i + 1 < tmpl.size() && tmpl[i + 1] >= '0' && tmpl[i + 1] <= '9')
            {
                int idx = 0; size_t j = i + 1;
                while (j < tmpl.size() && tmpl[j] >= '0' && tmpl[j] <= '9') { idx = idx * 10 + (tmpl[j] - '0'); ++j; }
                out += inputExpr(cg, node, idx, dataStack, hoist);
                i = j;
            }
            else if (c == '@') { appendNodeIdx(cg, out, node); ++i; }
            else if (c == ENUM_TOKEN) { appendEnumToken(cg, out, node); ++i; }
            else { out += c; ++i; }
        }
        return out;
    }

    // Statement template ($k = data input k via a fresh data recursion, #k = exec continuation k, ENUM_TOKEN = enum).
    std::string substituteExec(Codegen& cg, const std::string& tmpl, Node* node, std::set<const Node*>& execStack, const HoistMap& hoist)
    {
        std::string out;
        for (size_t i = 0; i < tmpl.size();)
        {
            const char c = tmpl[i];
            if ((c == '$' || c == '#') && i + 1 < tmpl.size() && tmpl[i + 1] >= '0' && tmpl[i + 1] <= '9')
            {
                int idx = 0; size_t j = i + 1;
                while (j < tmpl.size() && tmpl[j] >= '0' && tmpl[j] <= '9') { idx = idx * 10 + (tmpl[j] - '0'); ++j; }
                if (c == '$')
                {
                    std::set<const Node*> dataStack; // data resolution is independent of the exec path
                    out += inputExpr(cg, node, idx, dataStack, hoist);
                }
                else
                {
                    const auto& outputs = node->getOutputPins();
                    Pin* tgt = (idx >= 0 && idx < (int)outputs.size()) ? realTargetOfOutput(cg.links, outputs[idx].get()) : nullptr;
                    out += tgt ? emitExecChain(cg, tgt->node, execStack, tgt) : std::string();
                }
                i = j;
            }
            else if (c == '@') { appendNodeIdx(cg, out, node); ++i; }
			else if (c == ENUM_TOKEN) { appendEnumToken(cg, out, node); ++i; }
            // ?k{block}: emit the block (with $-substitution) only if input pin k is connected.
            else if (c == '?' && i + 1 < tmpl.size() && tmpl[i + 1] >= '0' && tmpl[i + 1] <= '9')
            {
                int idx = 0; size_t j = i + 1;
                while (j < tmpl.size() && tmpl[j] >= '0' && tmpl[j] <= '9') { idx = idx * 10 + (tmpl[j] - '0'); ++j; }
                if (j < tmpl.size() && tmpl[j] == '{')
                {
                    int depth = 1; size_t k = j + 1;
                    for (; k < tmpl.size() && depth > 0; ++k)
                    {
                        if (tmpl[k] == '{') ++depth;
                        else if (tmpl[k] == '}') { if (--depth == 0) break; }
                    }
                    const std::string block = tmpl.substr(j + 1, k - (j + 1));
                    const auto& inputs = node->getInputPins();
                    const Pin* in = (idx >= 0 && idx < (int)inputs.size()) ? inputs[idx].get() : nullptr;
                    if (in && realSourceOfInput(cg.links, in))
                        out += substituteExec(cg, block, node, execStack, hoist);
                    i = (k < tmpl.size()) ? k + 1 : k;
                }
                else { out += c; ++i; }
            }
            else { out += c; ++i; }
        }
        return out;
    }

    // Picks the value template for an output pin and substitutes it (per-output expr, else the node emit).
    std::string expandOutput(Codegen& cg, const Pin* outPin, std::set<const Node*>& dataStack, const HoistMap& hoist)
    {
        Node* node = outPin->node;
        // A Script Data member is a field of the persistent struct: read/written directly through `data`.
        if (isScriptDataType(node->getTypeId()))
            return "data->" + outPin->name;
        // A Function Input parameter resolves to the generated function's C++ parameter, named by position
        // (param<k>) — the pin's editable label is display-only and never reaches the code.
        if (isFunctionInputType(node->getTypeId()))
        {
            const int o = indexOfPin(node->getOutputPins(), outPin); // pin 0 is exec-out; params start at 1
            return "param" + std::to_string(o - 1);
        }
        // A Function Call return resolves to the local the call wrote it into (declared at the call site).
        if (isFunctionCallType(node->getTypeId()))
        {
            const int o = indexOfPin(node->getOutputPins(), outPin);
            return "fret" + std::to_string(node->getNodeIdx()) + "_" + std::to_string(o - 1);
        }

        const NodeDef* def = findNodeDef(node->getTypeId());
        if (!def || dataStack.count(node))
            return "0";

        const int outIdx = indexOfPin(node->getOutputPins(), outPin);
        std::string tmpl;
        if (outIdx >= 0 && outIdx < (int)def->outputs.size() && !def->outputs[outIdx].expr.empty())
        {
            const std::string& e = def->outputs[outIdx].expr;
            const auto hp = e.find(HOIST);
            tmpl = (hp == std::string::npos) ? e : e.substr(hp + 1); // inline-expand only the reference part
        }
        else
            tmpl = def->emit; // pure data node: emit is the value expression
        if (tmpl.empty())
            return "0";

        dataStack.insert(node);
        std::string result = substituteData(cg, tmpl, node, dataStack, hoist);
        dataStack.erase(node);
        return result;
    }

    // Wraps a String pin's raw default text (the user edits it without quotes) into a C++ string literal,
    // escaping backslashes and quotes so arbitrary text stays valid source.
    std::string quoteStringLiteral(const std::string& raw)
    {
        std::string out = "\"";
        for (char c : raw)
        {
            if (c == '\\' || c == '"') out += '\\';
            out += c;
        }
        out += '"';
        return out;
    }

    std::string emitDataExpr(Codegen& cg, const Pin* inputPin, std::set<const Node*>& dataStack, const HoistMap& hoist)
    {
        Pin* src = realSourceOfInput(cg.links, inputPin);
        if (!src)
        {
            // "default" resolves to this pin's concrete type's engine default literal at codegen time (see
            // isDefaultToken); otherwise the typed literal is used as-is.
            const std::string& lit = isDefaultToken(inputPin->defaultValue) ? defaultValueForType(inputPin->dataType) : inputPin->defaultValue;
            // A String pin's default is stored as raw text (no quotes in the editor); make it a literal here.
            if (inputPin->dataType == EDataType::String)
                return quoteStringLiteral(lit);
            return lit.empty() ? std::string("0") : lit;
        }
        if (auto it = hoist.find(src); it != hoist.end())
            return it->second; // already computed into a local
        return expandOutput(cg, src, dataStack, hoist);
    }

    // Collects every data output pin reachable (through data nodes) from inputPin, so the HOIST variables a
    // statement reads can be discovered and declared at function scope.
    void collectDataOutputs(Codegen& cg, const Pin* inputPin, std::set<const Pin*>& out, std::set<const Node*>& path)
    {
        Pin* src = realSourceOfInput(cg.links, inputPin);
        if (!src) return;
        Node* node = src->node;
        if (!findNodeDef(node->getTypeId())) return;
        out.insert(src);
        if (path.count(node)) return; // cycle guard
        path.insert(node);
        for (const auto& pin : node->getInputPins())
            if (pin->dataType != EDataType::Exec)
                collectDataOutputs(cg, pin.get(), out, path);
        path.erase(node);
    }

    // Emits a HOIST variable's declaration once at function scope, after declaring any HOIST deps it reads.
    void declareHoist(Codegen& cg, const Pin* outPin, const HoistMap& hoist, std::set<const Pin*>& declared)
    {
        if (!declared.insert(outPin).second) return;
        for (const auto& pin : outPin->node->getInputPins())
        {
            if (pin->dataType == EDataType::Exec) continue;
            if (Pin* s = realSourceOfInput(cg.links, pin.get()); s && hoist.count(s))
                declareHoist(cg, s, hoist, declared);
        }
        std::string decl, ref;
        if (hoistParts(outPin, decl, ref))
        {
            std::set<const Node*> dataStack;
            const std::string text = substituteData(cg, decl, outPin->node, dataStack, hoist);
            if (cg.globalDeclared.insert(text).second) // dedup by text: SplitVec3's x/y/z share one decl
                cg.globalDecls += text;
        }
    }

    // A Function Call statement: declare a local per return value, call the generated C++ function passing the
    // arg expressions plus those locals by reference, then continue the exec flow. Downstream reads of the
    // call's data outputs resolve to these same locals (see expandOutput).
    std::string emitFunctionCallStmt(Codegen& cg, Node* node, std::set<const Node*>& execStack, const HoistMap& hoist)
    {
        const auto& inputs = node->getInputPins();
        const auto& outputs = node->getOutputPins();
        const std::string cpp = funcCppName(node->getFunctionScriptPath(), node->getFunctionName());
        const std::string idx = std::to_string(node->getNodeIdx());

        std::string out;
        for (size_t o = 1; o < outputs.size(); ++o) // outputs[0] is the exec-out pin
            out += std::string(memberCppType(outputs[o]->dataType)) + " fret" + idx + "_" + std::to_string(o - 1) + ";\n";

        out += cpp + "(ctx, self";
        for (size_t k = 1; k < inputs.size(); ++k) // inputs[0] is the exec-in pin; the rest are arguments
        {
            std::set<const Node*> dataStack;
            out += ", " + emitDataExpr(cg, inputs[k].get(), dataStack, hoist);
        }
        for (size_t o = 1; o < outputs.size(); ++o) // return values passed as out-params (by reference)
            out += ", fret" + idx + "_" + std::to_string(o - 1);
        out += ");\n";

        if (!outputs.empty())
            if (Pin* tgt = realTargetOfOutput(cg.links, outputs[0].get()))
                out += emitExecChain(cg, tgt->node, execStack, tgt);
        return out;
    }

    // A Trigger Audio statement: plays the alias whose exec entry pin the flow arrived through. The override
    // mask is settled at CODEGEN time from which override inputs are connected — unconnected ones pass their
    // (ignored) defaults, so the sound keeps its authored settings for those.
    std::string emitTriggerAudioStmt(Codegen& cg, Node* node, std::set<const Node*>& execStack, const HoistMap& hoist, const Pin* enteredPin)
    {
        const auto& inputs = node->getInputPins();
        auto findInput = [&](std::string_view name) -> const Pin*
        {
            for (const auto& pin : inputs)
                if (pin->dataType != EDataType::Exec && pin->name == name)
                    return pin.get();
            return nullptr;
        };
        const Pin* componentPin = findInput("Component");
        const Pin* entityPin = findInput("Entity");
        const Pin* positionPin = findInput("Position");
        const Pin* volumePin = findInput("Volume");
        const Pin* pitchPin = findInput("Pitch");

        std::string alias = enteredPin ? enteredPin->name : std::string();
        if (alias.empty()) // entry-less reach (shouldn't happen): fall back to the first alias pin
            for (const auto& pin : inputs)
                if (pin->dataType == EDataType::Exec && !pin->name.empty()) { alias = pin->name; break; }

        int overrideMask = 0;

        if (positionPin && (!isDefaultToken(positionPin->defaultValue) || realSourceOfInput(cg.links, positionPin))) overrideMask |= 1;
        if (volumePin && (!isDefaultToken(volumePin->defaultValue) || realSourceOfInput(cg.links, volumePin)))       overrideMask |= 2;
        if (pitchPin && (!isDefaultToken(pitchPin->defaultValue) || realSourceOfInput(cg.links, pitchPin)))          overrideMask |= 4;

        std::set<const Node*> dataStack;
        auto expr = [&](const Pin* pin, const char* fallback)
        {
            return (pin && !isDefaultToken(pin->defaultValue)) ? emitDataExpr(cg, pin, dataStack, hoist) : std::string(fallback);
        };
        // Component comes from Get Audio Component (a Pointer pin; nullptr default when unconnected). Entity is
        // still passed so a spatial sound can follow it.
        const std::string componentExpr = componentPin ? emitDataExpr(cg, componentPin, dataStack, hoist) : std::string("nullptr");
        std::string out = "ctx->audioTrigger(" + componentExpr + ", " + expr(entityPin, "self") + ", " + quoteStringLiteral(alias) + ", " +
            std::to_string(overrideMask) + ", " + expr(positionPin, "glm::vec3(0,0,0)") + ", " +
            expr(volumePin, "1.0f") + ", " + expr(pitchPin, "1.0f") + ");\n";

        for (const auto& outPin : node->getOutputPins())
            if (outPin->dataType == EDataType::Exec)
            {
                if (Pin* tgt = realTargetOfOutput(cg.links, outPin.get()))
                    out += emitExecChain(cg, tgt->node, execStack, tgt);
                break;
            }
        return out;
    }

    // A Function Output statement: assign each connected return input to the generated function's out-param,
    // named by position (ret<k>) — the pin's editable label is display-only. It has no exec continuation
    // (it's the end of the function body).
    std::string emitFunctionOutputStmt(Codegen& cg, Node* node, const HoistMap& hoist)
    {
        const auto& inputs = node->getInputPins();
        std::string out;
        for (size_t j = 1; j < inputs.size(); ++j) // inputs[0] is the exec-in pin; returns start at 1
        {
            std::set<const Node*> dataStack;
            out += "ret" + std::to_string(j - 1) + " = " + emitDataExpr(cg, inputs[j].get(), dataStack, hoist) + ";\n";
        }
        return out;
    }

    std::string emitExecChain(Codegen& cg, Node* node, std::set<const Node*>& execStack, const Pin* enteredPin)
    {
        if (!node) return std::string();
        const NodeDef* def = findNodeDef(node->getTypeId());
        if (!def || execStack.count(node)) return std::string();

        execStack.insert(node);

        // Map each HOIST-marked data output this statement reads to its variable name, so uses resolve to it
        // and its declaration is emitted once at function scope (declareHoist). Everything else inlines.
        std::set<const Pin*> reachable;
        {
            std::set<const Node*> path;
            for (const auto& pin : node->getInputPins())
                if (pin->dataType != EDataType::Exec)
                    collectDataOutputs(cg, pin.get(), reachable, path);
        }
        HoistMap hoist;
        for (const Pin* pin : reachable)
        {
            std::string decl, ref;
            if (hoistParts(pin, decl, ref))
            {
                std::set<const Node*> ds;
                hoist[pin] = substituteData(cg, ref, pin->node, ds, hoist); // reference, e.g. "f3" or "v3.x"
            }
        }
        {
            std::set<const Pin*> declared;
            for (const auto& [pin, name] : hoist)
                declareHoist(cg, pin, hoist, declared);
        }

        std::string out;
        if (node->isFunctionCall())        out = emitFunctionCallStmt(cg, node, execStack, hoist);
        else if (node->isFunctionOutput()) out = emitFunctionOutputStmt(cg, node, hoist);
        else if (node->isTriggerAudio())   out = emitTriggerAudioStmt(cg, node, execStack, hoist, enteredPin);
        else                               out = substituteExec(cg, def->emit, node, execStack, hoist);
        execStack.erase(node);
        return out;
    }

    // Convert the indent-control markers emitted by block nodes into real leading whitespace: \x01 raises
    // the indent level for subsequent lines, \x02 lowers it (the markers themselves are stripped). Each line
    // is prefixed with one pad per active level, so nested Branch/ForLoop bodies step in correctly.
    std::string applyIndent(const std::string& body, const char* pad)
    {
        std::string out;
        int depth = 0;
        bool atLineStart = true;
        for (char c : body)
        {
            if (c == INDENT_UP)   { ++depth; continue; }
            if (c == INDENT_DOWN) { if (depth > 0) --depth; continue; }
            if (atLineStart && c != '\n')
            {
                for (int i = 0; i < depth; ++i) out += pad;
                atLineStart = false;
            }
            out += c;
            if (c == '\n') atLineStart = true;
        }
        return out;
    }

    std::string indentLines(const std::string& body, const char* pad)
    {
        std::string out = pad;
        for (char c : body)
        {
            out += c;
            if (c == '\n') out += pad;
        }
        while (!out.empty() && out.back() == ' ') out.pop_back(); // drop the trailing pad before the closing brace
        return out;
    }
}

Scene::~Scene()
{
}

void Scene::initialize()
{
    ed::Config config;
    config.SettingsFile = nullptr;
    // NavigateButtonIndex stays the default (right-click), which also drives the context menu — a real drag
    // still pans, a near-stationary click still opens the popup (see the ContextMenuAction threshold fix and
    // NavigateAction's added middle-mouse-button support in imgui_node_editor.cpp).
    //
    // EnableSmoothZoom: with it off (the library default), NavigateAction::GetNextZoom truncates the wheel
    // delta to an int before applying it — any accumulated scroll below a full 1.0 step is silently dropped,
    // every frame, forever. A physical mouse locally almost always clears 1.0 per notch within one frame, so
    // this goes unnoticed; over Remote Desktop the same notch's sub-events arrive spread across several
    // frames, so each frame's delta is usually a fraction below 1.0 and gets truncated to zero — scrolling
    // only "works" once it's fast enough that one frame's accumulated delta happens to clear 1.0. Smooth zoom
    // scales continuously (powf(power, steps)) instead of truncating, so it has no such threshold to miss.
    config.EnableSmoothZoom = true;
    m_nodeEditorContext = ed::CreateEditor(&config);
    ed::SetCurrentEditor(m_nodeEditorContext);

    if (!loadFromFile(m_scriptPath))
    {
        newGraph();
        saveToFile(m_scriptPath);
    }
}

Node& Scene::createNode()
{
    return *m_nodes.emplace_back(std::make_unique<Node>((uint32)m_nodes.size()));
}

Node& Scene::addNodeOfType(const std::string& typeId, ImVec2 pos)
{
    Node& node = createNode();
    if (const NodeDef* def = findNodeDef(typeId))
        node.initFromDef(pos, *def);
    if (m_nodeEditorContext)
        ed::SetNodePosition(node.getId(), pos); // so generateCpp can read positions before the first render
    return node;
}

int Scene::indexOfNode(const Node* node) const
{
    for (int i = 0; i < (int)m_nodes.size(); ++i)
        if (m_nodes[i].get() == node)
            return i;
    return -1;
}

Node* Scene::findEntry(const char* nodeName) const
{
    for (const auto& node : m_nodes)
        if (node->getTypeId() == nodeName)
            return node.get();
    return nullptr;
}

Node* Scene::findScriptData() const
{
    for (const auto& node : m_nodes)
        if (node->isDynamic())
            return node.get();
    return nullptr;
}

Node* Scene::findEventEntry() const
{
    for (const auto& node : m_nodes)
        if (node->isEventNode())
            return node.get();
    return nullptr;
}

void Scene::connectNodes(Node* from, int outIdx, Node* to, int inIdx)
{
    if (!from || !to) return;
    const auto& outputs = from->getOutputPins();
    const auto& inputs = to->getInputPins();
    if (outIdx < 0 || outIdx >= (int)outputs.size() || inIdx < 0 || inIdx >= (int)inputs.size())
        return;

    Pin* outPin = outputs[outIdx].get();
    Pin* inPin = inputs[inIdx].get();
    inPin->numConnections++;
    outPin->numConnections++;
    m_links.emplace_back(std::make_unique<Link>())->initialize(ed::PinId(inPin), ed::PinId(outPin));
}

void Scene::resolveNodeTypes(Node* node)
{
    if (!node) return;

    std::set<int> groups;
    for (const auto& pin : node->getInputPins())  if (pin->typeGroup != 0) groups.insert(pin->typeGroup);
    for (const auto& pin : node->getOutputPins()) if (pin->typeGroup != 0) groups.insert(pin->typeGroup);

    for (int group : groups)
    {
        // The group's concrete type is whatever a connected pin in it links to.
        EDataType resolved = EDataType::Wildcard;
        for (const auto& pin : node->getInputPins())
            if (pin->typeGroup == group)
                if (Pin* src = sourceOfInput(m_links, pin.get()); src && src->dataType != EDataType::Wildcard)
                    resolved = src->dataType;
        for (const auto& pin : node->getOutputPins())
            if (pin->typeGroup == group)
                for (const auto& link : m_links)
                    if (link->getOutputId().AsPointer<Pin>() == pin.get())
                        if (Pin* dst = link->getInputId().AsPointer<Pin>(); dst && dst->dataType != EDataType::Wildcard)
                            resolved = dst->dataType;

        auto apply = [&](Pin& pin)
        {
            if (pin.dataType == resolved) return;
            pin.dataType = resolved;
            pin.color = dataTypeColor(resolved);
            pin.shape = resolved == EDataType::Exec ? EPinShape_Flow
                      : pin.mutability != EMutableType::Readable ? EPinShape_Square // keep writable pins square
                      : EPinShape_Circle;
            // "default" is valid for any type, so a pin left on that sentinel keeps it across the type
            // change instead of being reformatted to the new type's concrete default literal.
            if (pin.type == EPinType_Input && pin.numConnections == 0 && !isDefaultToken(pin.defaultValue))
                pin.defaultValue = defaultValueForType(resolved);
        };
        for (const auto& pin : node->getInputPins())  if (pin->typeGroup == group) apply(*pin);
        for (const auto& pin : node->getOutputPins()) if (pin->typeGroup == group) apply(*pin);
    }
}

void Scene::removeNode(ed::NodeId nodeId)
{
    Node* node = nodeId.AsPointer<Node>();
    if (!node) return;

    // Deleting a reroute waypoint should keep the connection: capture its immediate source output and target
    // input so they can be re-linked once the reroute's own links are gone.
    Node* rejoinFromNode = nullptr; int rejoinFromIdx = -1;
    Node* rejoinToNode = nullptr;   int rejoinToIdx = -1;
    if (node->isReroute() && !node->getInputPins().empty() && !node->getOutputPins().empty())
    {
        if (Pin* s = sourceOfInput(m_links, node->getInputPins()[0].get()))
        { rejoinFromNode = s->node; rejoinFromIdx = indexOfPin(s->node->getOutputPins(), s); }
        if (Pin* d = targetOfOutput(m_links, node->getOutputPins()[0].get()))
        { rejoinToNode = d->node; rejoinToIdx = indexOfPin(d->node->getInputPins(), d); }
    }
    else if (!node->isReroute())
    {
        // A reroute chain feeding (or fed by) this node is meaningless once the node is gone — delete the
        // whole waypoint line, not just the segment adjacent to the node. Repeat until no chain remains
        // (deleteRerouteChain removes links, so re-scan after each).
        for (;;)
        {
            Link* chainLink = nullptr;
            for (const auto& link : m_links)
            {
                Pin* in = link->getInputId().AsPointer<Pin>();
                Pin* out = link->getOutputId().AsPointer<Pin>();
                Pin* other = (in && in->node == node) ? out : (out && out->node == node) ? in : nullptr;
                if (other && other->node->isReroute()) { chainLink = link.get(); break; }
            }
            if (!chainLink)
                break;
            deleteRerouteChain(chainLink);
        }
    }

    // Drop any links touching this node's pins, tracking the neighbours so their wildcard types re-resolve.
    std::set<Node*> affected;
    std::erase_if(m_links, [&](const std::unique_ptr<Link>& link)
    {
        Pin* in = link->getInputId().AsPointer<Pin>();
        Pin* out = link->getOutputId().AsPointer<Pin>();
        if (in && in->node == node) { if (out) { out->numConnections--; affected.insert(out->node); } return true; }
        if (out && out->node == node) { if (in) { in->numConnections--; affected.insert(in->node); } return true; }
        return false;
    });

    std::erase_if(m_nodes, [&](const std::unique_ptr<Node>& n) { return n.get() == node; });

    // Re-link across the removed reroute (its endpoints survive as they belong to other nodes).
    if (rejoinFromNode && rejoinToNode)
        connectNodes(rejoinFromNode, rejoinFromIdx, rejoinToNode, rejoinToIdx);

    for (Node* neighbour : affected)
        resolveNodeTypes(neighbour);
}

// Splits a link with a reroute waypoint at `canvasPos`: the link becomes source -> reroute -> target, so the
// connection is preserved but the line now routes through a draggable dot.
void Scene::insertReroute(Link* link, ImVec2 canvasPos)
{
    Pin* out = link->getOutputId().AsPointer<Pin>(); // the source (output) pin
    Pin* in  = link->getInputId().AsPointer<Pin>();  // the destination (input) pin
    if (!out || !in)
        return;

    const EDataType type = out->dataType;
    const int outIdx = indexOfPin(out->node->getOutputPins(), out);
    const int inIdx  = indexOfPin(in->node->getInputPins(), in);

    // Remove the original link.
    out->numConnections--;
    in->numConnections--;
    std::erase_if(m_links, [&](const std::unique_ptr<Link>& l) { return l.get() == link; });

    Node& reroute = addNodeOfType("Reroute", canvasPos);
    reroute.makeReroute(type);

    connectNodes(out->node, outIdx, &reroute, 0);
    connectNodes(&reroute, 0, in->node, inIdx);
}

// Removes an entire routed connection given one of its links: gathers every reroute waypoint in the chain
// (walking reroute-to-reroute through their single in/out links) and erases all of them plus every link that
// touches them, leaving the real source and target disconnected.
void Scene::deleteRerouteChain(Link* link)
{
    std::set<Node*> reroutes;
    std::vector<Node*> stack;
    auto addIfReroute = [&](Pin* pin)
    {
        if (pin && pin->node->isReroute() && reroutes.insert(pin->node).second)
            stack.push_back(pin->node);
    };
    addIfReroute(link->getOutputId().AsPointer<Pin>());
    addIfReroute(link->getInputId().AsPointer<Pin>());

    while (!stack.empty())
    {
        Node* r = stack.back(); stack.pop_back();
        if (!r->getInputPins().empty())
            addIfReroute(sourceOfInput(m_links, r->getInputPins()[0].get()));
        if (!r->getOutputPins().empty())
            addIfReroute(targetOfOutput(m_links, r->getOutputPins()[0].get()));
    }
    if (reroutes.empty())
        return;

    // Drop every link touching a reroute in the chain (plus the triggering link), noting non-reroute
    // endpoints so their wildcard types can re-resolve once disconnected.
    std::set<Node*> affected;
    std::erase_if(m_links, [&](const std::unique_ptr<Link>& l)
    {
        Pin* in = l->getInputId().AsPointer<Pin>();
        Pin* out = l->getOutputId().AsPointer<Pin>();
        const bool touches = l.get() == link
            || (in && reroutes.count(in->node)) || (out && reroutes.count(out->node));
        if (!touches)
            return false;
        if (in)  { in->numConnections--;  if (!reroutes.count(in->node))  affected.insert(in->node); }
        if (out) { out->numConnections--; if (!reroutes.count(out->node)) affected.insert(out->node); }
        return true;
    });

    std::erase_if(m_nodes, [&](const std::unique_ptr<Node>& n) { return reroutes.count(n.get()) != 0; });

    for (Node* nb : affected)
        resolveNodeTypes(nb);
}

void Scene::removeMemberPin(Node* node, int index)
{
    const auto& outs = node->getOutputPins();
    if (index < 0 || index >= (int)outs.size())
        return;
    Pin* pin = outs[index].get();

    std::set<Node*> affected;
    std::erase_if(m_links, [&](const std::unique_ptr<Link>& link)
    {
        Pin* out = link->getOutputId().AsPointer<Pin>();
        Pin* in = link->getInputId().AsPointer<Pin>();
        if (out == pin) { if (in) { in->numConnections--; affected.insert(in->node); } return true; }
        return false;
    });

    node->eraseOutputPin(index);
    for (Node* n : affected)
        resolveNodeTypes(n);
}

// Replays one member edit on every Script Data node so they all carry the same member set (which becomes the
// single generated struct). Add/Remove keep counts equal across nodes; Rename/Retype touch the same row on
// each. Remove and Retype can invalidate links, which are pruned per node.
void Scene::applyMemberEdit(const MemberEdit& edit)
{
    std::vector<Node*> nodes;
    for (const auto& n : m_nodes)
        if (n->isDynamic())
            nodes.push_back(n.get());

    switch (edit.op)
    {
        case EMemberOp::Add:
        {
            // All nodes are in sync, so their counts match — derive one shared name from the first.
            const size_t count = nodes.empty() ? 0 : nodes[0]->getOutputPins().size();
            const std::string name = "member" + std::to_string(count);
            for (Node* n : nodes)
                n->addMember(edit.type, name);
            break;
        }
        case EMemberOp::Remove:
            for (Node* n : nodes)
                removeMemberPin(n, edit.index); // also drops that member's links
            break;
        case EMemberOp::Rename:
            for (Node* n : nodes)
                if (edit.index >= 0 && edit.index < (int)n->getOutputPins().size())
                    n->getOutputPins()[edit.index]->name = edit.name; // links unaffected (codegen keys off name)
            break;
        case EMemberOp::Retype:
            for (Node* n : nodes)
            {
                if (edit.index >= 0 && edit.index < (int)n->getOutputPins().size())
                {
                    Pin* pin = n->getOutputPins()[edit.index].get();
                    pin->dataType = edit.type;
                    pin->color = dataTypeColor(edit.type);
                }
                pruneIncompatibleLinks(n); // a type change can invalidate a connected link
            }
            break;
        default:
            break;
    }
}

// A freshly added Script Data node starts empty; seed it from an existing one so it matches the shared set.
void Scene::syncNewMemberNode(Node& newNode)
{
    for (const auto& n : m_nodes)
        if (n.get() != &newNode && n->isDynamic())
        {
            for (const auto& pin : n->getOutputPins())
                newNode.addMember(pin->dataType, pin->name);
            return; // every dynamic node holds the same set, so the first is enough
        }
}

// Replays one entry edit on every On Event node so they all carry the same named-entry set. Mirrors
// applyMemberEdit; entries have no type to retype, only Add/Remove/Rename.
void Scene::applyEventEdit(const MemberEdit& edit)
{
    std::vector<Node*> nodes;
    for (const auto& n : m_nodes)
        if (n->isEventNode())
            nodes.push_back(n.get());

    switch (edit.op)
    {
        case EMemberOp::Add:
        {
            const size_t count = nodes.empty() ? 0 : nodes[0]->getOutputPins().size();
            const std::string name = "Event" + std::to_string(count);
            for (Node* n : nodes)
                n->addEventEntry(name);
            break;
        }
        case EMemberOp::Remove:
            for (Node* n : nodes)
                removeMemberPin(n, edit.index); // also drops that entry's links
            break;
        case EMemberOp::Rename:
            for (Node* n : nodes)
                if (edit.index >= 0 && edit.index < (int)n->getOutputPins().size())
                    n->getOutputPins()[edit.index]->name = edit.name;
            break;
        default:
            break;
    }
}

// A freshly added On Event node starts empty; seed it from an existing one so it matches the shared set.
void Scene::syncNewEventNode(Node& newNode)
{
    for (const auto& n : m_nodes)
        if (n.get() != &newNode && n->isEventNode())
        {
            for (const auto& pin : n->getOutputPins())
                newNode.addEventEntry(pin->name);
            return; // every On Event node holds the same set, so the first is enough
        }
}

// The UI feeds this every frame with the sound aliases of the entity that owns the open script (null while
// no owning entity is selected, so a standalone-edited script keeps the pins its file declared). Known
// aliases are reconciled onto every Trigger Audio node immediately; the sync is idempotent and cheap.
void Scene::setAudioAliases(const std::vector<std::string>* aliases)
{
    m_audioAliasesKnown = aliases != nullptr;
    if (aliases)
        m_audioAliases = *aliases;
    if (!m_audioAliasesKnown)
        return;
    for (const auto& node : m_nodes)
        if (node->isTriggerAudio())
            syncTriggerAudioPins(*node);
}

// Reconciles one Trigger Audio node's exec entry pins with m_audioAliases: stale aliases are dropped (with
// their links), missing ones appended. Pins whose alias still exists keep their links untouched.
void Scene::syncTriggerAudioPins(Node& node)
{
    const auto& inputs = node.getInputPins();
    for (int i = (int)inputs.size() - 1; i >= 0; --i)
    {
        const Pin* pin = inputs[i].get();
        if (pin->dataType != EDataType::Exec)
            continue;
        if (std::find(m_audioAliases.begin(), m_audioAliases.end(), pin->name) == m_audioAliases.end())
            removeInputPin(&node, i);
    }
    for (const std::string& alias : m_audioAliases)
    {
        bool present = false;
        for (const auto& pin : node.getInputPins())
            if (pin->dataType == EDataType::Exec && pin->name == alias) { present = true; break; }
        if (!present)
            node.addAudioEntry(alias);
    }
}

// Dragging a link onto empty canvas opens the add-node popup with m_pendingLinkPin set to its dangling end;
// once the user picks a node type, this wires that end to the new node's first pin (of the opposite kind)
// that accepts it, in pin order. Leaves the link unconnected if none does — the node is still created either
// way, but pruneOrphanedReroute cleans up a reroute chain left with nothing downstream (see there). A no-op
// if the popup was opened normally (right-click), which clears m_pendingLinkPin beforehand.
void Scene::autoConnectPending(Node& node)
{
    Pin* danglingPin = m_pendingLinkPin;
    m_pendingLinkPin = nullptr; // consumed by this popup selection either way
    if (!danglingPin)
        return;

    if (danglingPin->type == EPinType_Output)
    {
        const auto& inputs = node.getInputPins();
        for (int i = 0; i < (int)inputs.size(); ++i)
            if (inputs[i]->numConnections == 0 && pinsCompatible(inputs[i].get(), danglingPin))
            {
                connectNodes(danglingPin->node, indexOfPin(danglingPin->node->getOutputPins(), danglingPin), &node, i);
                resolveNodeTypes(danglingPin->node);
                resolveNodeTypes(&node);
                return;
            }
    }
    else
    {
        const auto& outputs = node.getOutputPins();
        for (int i = 0; i < (int)outputs.size(); ++i)
            if (pinsCompatible(danglingPin, outputs[i].get()))
            {
                connectNodes(&node, i, danglingPin->node, indexOfPin(danglingPin->node->getInputPins(), danglingPin));
                resolveNodeTypes(&node);
                resolveNodeTypes(danglingPin->node);
                return;
            }
    }

    // The new node has no pin compatible with the dropped end, so it stays unconnected — prune it if it's a
    // dead reroute stub (see pruneOrphanedReroute); a no-op otherwise (e.g. a plain unconnected input pin
    // freshly dragged out, never routed through anything).
    pruneOrphanedReroute(danglingPin);
}

// Deletes an orphaned reroute chain starting at `pin` — an output pin left with no downstream connection
// (numConnections == 0) after redirectIfRewiring picked up an existing link's input end and dropped the
// segment immediately below it, or after autoConnectPending failed to find anywhere to reconnect it. Walks
// upstream through each further reroute that becomes orphaned in turn (removeNode strips a reroute's own
// remaining link when it's deleted), stopping at the first non-reroute node or one still feeding something
// else. A no-op if `pin` isn't a dead reroute output to begin with.
void Scene::pruneOrphanedReroute(Pin* pin)
{
    while (pin && pin->node->isReroute() && pin->numConnections == 0)
    {
        Node* reroute = pin->node;
        const auto& inputs = reroute->getInputPins();
        Pin* upstream = inputs.empty() ? nullptr : sourceOfInput(m_links, inputs[0].get());
        removeNode(reroute->getId());
        pin = upstream;
    }
}

// Drops an input pin of `node` (and any link feeding it). Mirror of removeMemberPin, which does outputs.
void Scene::removeInputPin(Node* node, int index)
{
    const auto& ins = node->getInputPins();
    if (index < 0 || index >= (int)ins.size())
        return;
    Pin* pin = ins[index].get();

    std::set<Node*> affected;
    std::erase_if(m_links, [&](const std::unique_ptr<Link>& link)
    {
        Pin* in = link->getInputId().AsPointer<Pin>();
        Pin* out = link->getOutputId().AsPointer<Pin>();
        if (in == pin) { if (out) { out->numConnections--; affected.insert(out->node); } return true; }
        return false;
    });

    node->eraseInputPin(index);
    for (Node* n : affected)
        resolveNodeTypes(n);
}

// Applies one param/return edit to a single Function Input/Output node (not synced across nodes — each
// function is independent). Function Input edits its OUTPUT pins (parameters); Function Output edits its
// INPUT pins (return values). pin 0 is the fixed exec pin, so edited indices are always >= 1.
void Scene::applyFunctionEdit(Node* node, const MemberEdit& edit)
{
    const bool inputSide = node->isFunctionOutput();
    const auto& pins = inputSide ? node->getInputPins() : node->getOutputPins();

    switch (edit.op)
    {
        case EMemberOp::Add:
        {
            const int count = (int)pins.size() - 1; // minus the exec pin
            const std::string name = (inputSide ? "ret" : "arg") + std::to_string(count < 0 ? 0 : count);
            if (inputSide) node->addReturn(edit.type, name);
            else           node->addParam(edit.type, name);
            break;
        }
        case EMemberOp::Remove:
            if (inputSide) removeInputPin(node, edit.index);
            else           removeMemberPin(node, edit.index);
            break;
        case EMemberOp::Rename:
            if (edit.index >= 1 && edit.index < (int)pins.size())
                pins[edit.index]->name = edit.name;
            break;
        case EMemberOp::Retype:
            if (edit.index >= 1 && edit.index < (int)pins.size())
            {
                Pin* pin = pins[edit.index].get();
                pin->dataType = edit.type;
                pin->color = dataTypeColor(edit.type);
                pin->defaultValue = defaultValueForType(edit.type);
                // Drop a now-incompatible link on this pin (source->return input, or param output->consumer).
                std::set<Node*> affected;
                std::erase_if(m_links, [&](const std::unique_ptr<Link>& link)
                {
                    Pin* in = link->getInputId().AsPointer<Pin>();
                    Pin* out = link->getOutputId().AsPointer<Pin>();
                    if ((in == pin || out == pin) && in && out && !pinsCompatible(in, out))
                    {
                        in->numConnections--; out->numConnections--;
                        affected.insert(in->node); affected.insert(out->node);
                        return true;
                    }
                    return false;
                });
                for (Node* n : affected)
                    resolveNodeTypes(n);
            }
            break;
        default:
            break;
    }
}

// Loads `path` into a throwaway headless Scene and reads the (Function Input, Function Output) pair named
// `funcName`: params come from the Input node's output pins (after the exec pin), returns from the Output
// node's input pins. Returns false if the file has no such function.
bool Scene::readFunctionSignature(const std::string& path, const std::string& funcName,
                                  PinSig& params, PinSig& returns) const
{
    Scene tmp;
    if (!tmp.loadFromFile(path))
        return false;

    Node* inputNode = nullptr;
    for (const auto& node : tmp.m_nodes)
        if (node->isFunctionInput() && node->getFunctionName() == funcName) { inputNode = node.get(); break; }
    if (!inputNode)
        return false;

    const auto& outs = inputNode->getOutputPins();
    for (size_t i = 1; i < outs.size(); ++i)
        params.push_back({ outs[i]->dataType, outs[i]->name });

    // The paired Function Output is the one reachable by exec flow from the Input (it carries no name).
    if (Node* outputNode = reachableFunctionOutput(tmp.m_links, inputNode))
    {
        const auto& ins = outputNode->getInputPins();
        for (size_t i = 1; i < ins.size(); ++i)
            returns.push_back({ ins[i]->dataType, ins[i]->name });
    }
    return true;
}

// Scans Assets/Scripts for every function defined in any .scr (except the file being edited) so the add-node
// popup can offer them for import. A function is any Function Input node's name.
std::vector<Scene::FunctionRef> Scene::scanImportableFunctions() const
{
    std::vector<FunctionRef> refs;
    const std::string dir = "Scripts";
    std::vector<FileSystem::DirEntry> entries;
    if (!FileSystem::listDirectory(dir, entries, /*allowMainThread*/ true))
        return refs;

    for (const FileSystem::DirEntry& entry : entries)
    {
        if (entry.isDirectory || entry.extension != ".scr")
            continue;
        const std::string rel = "Scripts/" + entry.name;
        if (rel == m_scriptPath)
            continue; // don't import yourself

        Scene tmp;
        if (!tmp.loadFromFile(rel))
            continue;
        const std::string stem = FileSystem::stem(entry.name);
        for (const auto& node : tmp.m_nodes)
            if (node->isFunctionInput() && !node->getFunctionName().empty())
                refs.push_back({ rel, node->getFunctionName(), stem + ": " + node->getFunctionName() });
    }
    return refs;
}

// Creates a Function Call node mirroring `ref`'s signature (exec in/out + one input per param, one output per
// return). The node remembers (scriptPath, funcName) so codegen can emit the call and re-read the signature.
Node& Scene::importFunction(const FunctionRef& ref, ImVec2 pos)
{
    PinSig params, returns;
    readFunctionSignature(ref.scriptPath, ref.funcName, params, returns);

    Node& node = addNodeOfType("FunctionCall", pos);
    node.setFunctionScriptPath(ref.scriptPath);
    node.setFunctionName(ref.funcName);
    node.makeFunctionCall(params, returns);
    return node;
}

void Scene::pruneIncompatibleLinks(Node* node)
{
    std::set<Node*> affected;
    std::erase_if(m_links, [&](const std::unique_ptr<Link>& link)
    {
        Pin* out = link->getOutputId().AsPointer<Pin>();
        Pin* in = link->getInputId().AsPointer<Pin>();
        if (out && out->node == node && in && !pinsCompatible(in, out))
        {
            in->numConnections--;
            out->numConnections--;
            affected.insert(in->node);
            return true;
        }
        return false;
    });
    for (Node* n : affected)
        resolveNodeTypes(n);
}

void Scene::newGraph()
{
    m_links.clear();
    m_nodes.clear();
    Node& entry = addNodeOfType("EventUpdate", ImVec2(40.0f, 60.0f));
    Node& spawn = addNodeOfType("SpawnPointLight", ImVec2(340.0f, 60.0f));
    connectNodes(&entry, 0, &spawn, 0);
    m_firstFrame = true;
    m_hasBaseline = false;
}

bool Scene::isFunctionScript() const
{
    for (const auto& node : m_nodes)
        if (node->isFunctionInput())
            return true;
    return false;
}

// Emits a static C++ function for every function this script defines (its Function Input/Output pairs) and
// for every function reachable through Function Call nodes, transitively across files. Forward declarations
// are emitted first so ordering and recursion don't matter. A function's body is the exec chain from its
// Function Input node; its Function Output assigns the return out-params.
void Scene::emitFunctions(std::string& code)
{
    // Owning graph for a referenced path: this Scene for the current file, else a lazily-loaded headless copy.
    std::map<std::string, std::unique_ptr<Scene>> loaded;
    auto sceneForPath = [&](const std::string& path) -> Scene*
    {
        if (path.empty() || path == m_scriptPath) return this;
        if (auto it = loaded.find(path); it != loaded.end()) return it->second.get();
        auto s = std::make_unique<Scene>();
        Scene* p = s->loadFromFile(path) ? s.get() : nullptr;
        loaded[path] = p ? std::move(s) : nullptr;
        return p;
    };

    struct Discovered { Scene* scene = nullptr; std::string funcName; PinSig params, returns; Node* inputNode = nullptr; };
    std::map<std::string, Discovered> byCpp; // cppName -> resolved info (inputNode null = couldn't resolve)
    std::vector<std::pair<std::string, std::string>> worklist; // (scriptPath, funcName) still to resolve

    if (isFunctionScript())
        for (const auto& node : m_nodes)
            if (node->isFunctionInput())
                worklist.push_back({ m_scriptPath, node->getFunctionName() });
    for (const auto& node : m_nodes)
        if (node->isFunctionCall())
            worklist.push_back({ node->getFunctionScriptPath(), node->getFunctionName() });

    while (!worklist.empty())
    {
        const auto [path, name] = worklist.back();
        worklist.pop_back();
        const std::string cpp = funcCppName(path, name);
        if (byCpp.count(cpp)) continue;

        Discovered d;
        Scene* s = sceneForPath(path);
        if (s)
        {
            d.scene = s;
            d.funcName = name;
            for (const auto& n : s->m_nodes)
                if (n->isFunctionInput() && n->getFunctionName() == name) { d.inputNode = n.get(); break; }
            Node* outputNode = d.inputNode ? reachableFunctionOutput(s->m_links, d.inputNode) : nullptr;
            if (d.inputNode)
                for (size_t i = 1; i < d.inputNode->getOutputPins().size(); ++i)
                    d.params.push_back({ d.inputNode->getOutputPins()[i]->dataType, d.inputNode->getOutputPins()[i]->name });
            if (outputNode)
                for (size_t i = 1; i < outputNode->getInputPins().size(); ++i)
                    d.returns.push_back({ outputNode->getInputPins()[i]->dataType, outputNode->getInputPins()[i]->name });
            for (const auto& n : s->m_nodes) // nested calls this function makes
                if (n->isFunctionCall())
                    worklist.push_back({ n->getFunctionScriptPath(), n->getFunctionName() });
        }
        byCpp[cpp] = std::move(d);
    }

    // Parameters/returns are named by position (param<k> / ret<k>), matching how the body reads them
    // (expandOutput / emitFunctionOutputStmt) and how a call passes them (positionally). The pins' editable
    // labels are purely a node-editor display and deliberately don't reach the generated code.
    auto signature = [](const Discovered& d, const std::string& cpp)
    {
        std::string s = "void " + cpp + "(const ScriptContext* ctx, Entity* self";
        for (size_t i = 0; i < d.params.size(); ++i)  s += std::string(", ") + memberCppType(d.params[i].first) + " param" + std::to_string(i);
        for (size_t i = 0; i < d.returns.size(); ++i) s += std::string(", ") + memberCppType(d.returns[i].first) + "& ret" + std::to_string(i);
        s += ")";
        return s;
    };

    bool any = false;
    for (const auto& [cpp, d] : byCpp)
        if (d.inputNode) { code += "static " + signature(d, cpp) + ";\n"; any = true; }
    if (any) code += "\n";

    for (const auto& [cpp, d] : byCpp)
    {
        if (!d.inputNode) continue;
        code += "static " + signature(d, cpp) + "\n{\n";

        Codegen cg{ d.scene->m_links };
        std::set<const Node*> execStack;
        std::string chain;
        Pin* execOut = d.inputNode->getOutputPins().empty() ? nullptr : d.inputNode->getOutputPins()[0].get();
        if (execOut)
            if (Pin* tgt = realTargetOfOutput(cg.links, execOut))
                chain = emitExecChain(cg, tgt->node, execStack, tgt);

        const std::string body = applyIndent(cg.globalDecls + chain, "    ");
        if (!body.empty())
            code += indentLines(body, "    ");
        code += "}\n\n";
    }
}

std::string Scene::generateCpp()
{
    if (m_nodeEditorContext)
        ed::SetCurrentEditor(m_nodeEditorContext);

    std::string code;
    // The .scr holds ONLY namespace-body-safe code: an optional Script Data struct, helper functions, and the
    // SCRIPT_EXPORT entry-point functions, followed by the //@graph metadata (all comments). It does NOT
    // #include ScriptAPI.h or pick a build mode — the compiler wrapper supplies those, so the same file serves
    // both back-ends. ScriptHost force-includes ScriptAPI.h (+ /DSCRIPT_BUILD) when building the hot-reload DLL;
    // the App-Scripts aggregate (cooked build) includes ScriptAPI.h once and #includes each .scr straight into
    // its own namespace (real imported Entity, SCRIPT_STATIC_BUILD) so many scripts link into the engine binary
    // with no name clashes. Keeping the file body-only is exactly what makes that namespace #include legal.
    const bool functionScript = isFunctionScript();

    // The Script Data node becomes a persistent struct. The host reads its byte size through ScriptDataSize()
    // (letting the compiler settle padding/alignment) to allocate the block handed back in as `scriptData`.
    Node* dataNode = findScriptData();
    if (dataNode)
    {
        code += "struct ScriptData\n{\n";
        for (const auto& pin : dataNode->getOutputPins())
            if (!pin->name.empty())
                code += std::string("    ") + memberCppType(pin->dataType) + " " + pin->name + ";\n";
        code += "};\n";
        code += "SCRIPT_EXPORT unsigned int ScriptDataSize() { return (unsigned int)sizeof(ScriptData); }\n";
    }

    // Helper functions (uniquely named <stem>_<func>) before the entry points that call them.
    code += "\n";
    emitFunctions(code);

    if (Node* entry = !functionScript ? findEntry("OnSpawn") : nullptr)
    {
        code += "SCRIPT_EXPORT void OnSpawn(const ScriptContext* ctx, Entity* self, void* scriptData)\n{\n";

        Codegen cg{ m_links };
        std::set<const Node*> execStack;
        const std::string chain = emitExecChain(cg, entry, execStack); // also collects cg.globalDecls
        std::string bodyRaw;
        if (dataNode)
            bodyRaw += "ScriptData* data = (ScriptData*)scriptData;\n"; // members resolve to data->field
        bodyRaw += cg.globalDecls + chain;                             // variable decls first, at function scope
        const std::string body = applyIndent(bodyRaw, "    ");
        if (!body.empty())
            code += indentLines(body, "    ");

        code += "}\n\n";
    }

    if (Node* entry = !functionScript ? findEntry("OnDestroy") : nullptr)
    {
        code += "SCRIPT_EXPORT void OnDestroy(const ScriptContext* ctx, Entity* self, void* scriptData)\n{\n";

        Codegen cg{ m_links };
        std::set<const Node*> execStack;
        const std::string chain = emitExecChain(cg, entry, execStack); // also collects cg.globalDecls
        std::string bodyRaw;
        if (dataNode)
            bodyRaw += "ScriptData* data = (ScriptData*)scriptData;\n"; // members resolve to data->field
        bodyRaw += cg.globalDecls + chain;                             // variable decls first, at function scope
        const std::string body = applyIndent(bodyRaw, "    ");
        if (!body.empty())
            code += indentLines(body, "    ");

        code += "}\n\n";
    }

    // On Physics Event: a fixed entry point (unlike On Event, which dispatches user-named entries by index).
    // Parameter names here (physOther etc.) must match what the OnPhysicsEvent NodeDef's output pins expand to.
    if (Node* entry = !functionScript ? findEntry("OnPhysicsEvent") : nullptr)
    {
        code += "SCRIPT_EXPORT void OnPhysicsEvent(const ScriptContext* ctx, Entity* self, Entity* physOther, "
                "int physBegin, int physSensor, long long physContactId, void* scriptData)\n{\n";

        Codegen cg{ m_links };
        std::set<const Node*> execStack;
        const std::string chain = emitExecChain(cg, entry, execStack); // also collects cg.globalDecls
        std::string bodyRaw;
        if (dataNode)
            bodyRaw += "ScriptData* data = (ScriptData*)scriptData;\n"; // members resolve to data->field
        bodyRaw += cg.globalDecls + chain;                             // variable decls first, at function scope
        const std::string body = applyIndent(bodyRaw, "    ");
        if (!body.empty())
            code += indentLines(body, "    ");

        code += "}\n\n";
    }

    // On Event dispatches a runtime-fired event by index (not name — the host resolves a name to an index via
    // ScriptEventCount/ScriptEventName and caches it, keeping the fire-time call a plain int compare). The
    // index is the entry's position among the On Event node's output pins, so it lines up with ScriptEventName.
    if (Node* eventNode = !functionScript ? findEventEntry() : nullptr)
    {
        const auto& entries = eventNode->getOutputPins();

        code += "SCRIPT_EXPORT int ScriptEventCount() { return " + std::to_string(entries.size()) + "; }\n";
        code += "SCRIPT_EXPORT const char* ScriptEventName(int eventIdx)\n{\n    switch (eventIdx)\n    {\n";
        for (int i = 0; i < (int)entries.size(); ++i)
            code += "    case " + std::to_string(i) + ": return \"" + entries[i]->name + "\";\n";
        code += "    default: return \"\";\n    }\n}\n";

        code += "SCRIPT_EXPORT void OnEvent(const ScriptContext* ctx, Entity* self, int eventIdx, void* scriptData)\n{\n";

        Codegen cg{ m_links };
        std::string dispatch;
        dispatch += "switch (eventIdx) {\n";
        for (int i = 0; i < (int)entries.size(); ++i)
        {
            Pin* tgt = realTargetOfOutput(m_links, entries[i].get());
            if (!tgt) continue;
            std::set<const Node*> execStack;
            const std::string chain = emitExecChain(cg, tgt->node, execStack, tgt); // also collects cg.globalDecls
            dispatch += "case " + std::to_string(i) + ":\n{\n" +
                std::string(1, INDENT_UP) + chain + std::string(1, INDENT_DOWN) + "} break;\n";
        }
        dispatch += "}\n";

        std::string bodyRaw;
        if (dataNode)
            bodyRaw += "ScriptData* data = (ScriptData*)scriptData;\n";
        bodyRaw += cg.globalDecls + dispatch;
        const std::string body = applyIndent(bodyRaw, "    ");
        if (!body.empty())
            code += indentLines(body, "    ");

        code += "}\n\n";
    }

    if (Node* entry = !functionScript ? findEntry("Update") : nullptr)
    {
        code += "SCRIPT_EXPORT void Update(const ScriptContext* ctx, Entity* self, float dt, void* scriptData)\n{\n";

        Codegen cg{ m_links };
        std::set<const Node*> execStack;
        const std::string chain = emitExecChain(cg, entry, execStack); // also collects cg.globalDecls
        std::string bodyRaw;
        if (dataNode)
            bodyRaw += "ScriptData* data = (ScriptData*)scriptData;\n"; // members resolve to data->field
        bodyRaw += cg.globalDecls + chain;                             // variable decls first, at function scope
        const std::string body = applyIndent(bodyRaw, "    ");
        if (!body.empty())
            code += indentLines(body, "    ");

        code += "}\n\n";
    }

    // Self-describe which entry points this file defines. Empty in the DLL build (the entries are C exports found
    // by name); in the cooked build each expands to a static registration of that function. Placed after the
    // definitions (so the macros can take their addresses) and before the //@graph comments.
    code += "\n";
    if (dataNode)                                       code += "REGISTER_SCRIPT_DATA_SIZE()\n";
    if (!functionScript && findEntry("OnSpawn"))        code += "REGISTER_ON_SPAWN()\n";
    if (!functionScript && findEntry("OnDestroy"))      code += "REGISTER_ON_DESTROY()\n";
    if (!functionScript && findEntry("OnPhysicsEvent")) code += "REGISTER_ON_PHYSICS_EVENT()\n";
    if (!functionScript && findEventEntry())            code += "REGISTER_ON_EVENT()\n";
    if (!functionScript && findEntry("Update"))         code += "REGISTER_UPDATE()\n";
    code += "\n";

    code += serializeGraph();
    return code;
}

std::string Scene::serializeGraph()
{
    std::string s = "//@graph 1\n";

    for (int i = 0; i < (int)m_nodes.size(); ++i)
    {
        const Node* node = m_nodes[i].get();
        const ImVec2 pos = m_nodeEditorContext ? ed::GetNodePosition(node->getId()) : ImVec2(0.0f, 0.0f);
        s += "//@node " + std::to_string(i) + " " + node->getTypeId() + " " +
             std::to_string((int)pos.x) + " " + std::to_string((int)pos.y) + "\n";
    }

    // Label boxes: their size and caption (caption is rest-of-line, so it may contain spaces).
    for (int i = 0; i < (int)m_nodes.size(); ++i)
        if (m_nodes[i]->isLabel())
        {
            const ImVec2 size = m_nodes[i]->getLabelSize();
            s += "//@labelsize " + std::to_string(i) + " " + std::to_string((int)size.x) + " " + std::to_string((int)size.y) + "\n";
            s += "//@labeltext " + std::to_string(i) + " " + m_nodes[i]->getLabelText() + "\n";
        }

    // Reroute waypoints: their carried type, so their pins can be rebuilt before links are restored.
    for (int i = 0; i < (int)m_nodes.size(); ++i)
        if (m_nodes[i]->isReroute() && !m_nodes[i]->getInputPins().empty())
            s += "//@reroute " + std::to_string(i) + " " + dataTypeToken(m_nodes[i]->getInputPins()[0]->dataType) + "\n";

    // Script Data members (a dynamic node's output pins). Emitted before links so a load recreates them
    // first — links reference these output pins by index.
    for (int i = 0; i < (int)m_nodes.size(); ++i)
        if (m_nodes[i]->isDynamic())
            for (const auto& pin : m_nodes[i]->getOutputPins())
                s += "//@member " + std::to_string(i) + " " + memberTypeToken(pin->dataType) + " " + pin->name + "\n";

    // On Event entries (a dynamic node's output pins). Emitted before links for the same reason as members.
    for (int i = 0; i < (int)m_nodes.size(); ++i)
        if (m_nodes[i]->isEventNode())
            for (const auto& pin : m_nodes[i]->getOutputPins())
                s += "//@event " + std::to_string(i) + " " + pin->name + "\n";

    // Trigger Audio alias entries (dynamic exec input pins). Emitted before links for the same reason.
    for (int i = 0; i < (int)m_nodes.size(); ++i)
        if (m_nodes[i]->isTriggerAudio())
            for (const auto& pin : m_nodes[i]->getInputPins())
                if (pin->dataType == EDataType::Exec)
                    s += "//@audioentry " + std::to_string(i) + " " + pin->name + "\n";

    // Function boundary + call nodes. All pins are emitted before links so a load recreates them first.
    // Function Input params are its output pins after the exec pin; Function Output returns are its input
    // pins after the exec pin; a Function Call stores its target (path + name) and mirrors the signature.
    for (int i = 0; i < (int)m_nodes.size(); ++i)
    {
        const Node* node = m_nodes[i].get();
        if (node->isFunctionInput())
        {
            s += "//@funcname " + std::to_string(i) + " " + node->getFunctionName() + "\n";
            const auto& outs = node->getOutputPins();
            for (size_t j = 1; j < outs.size(); ++j)
                s += "//@param " + std::to_string(i) + " " + memberTypeToken(outs[j]->dataType) + " " + outs[j]->name + "\n";
        }
        else if (node->isFunctionOutput())
        {
            // No //@funcname: a Function Output has no name — codegen pairs it to its Input by exec reachability.
            const auto& ins = node->getInputPins();
            for (size_t j = 1; j < ins.size(); ++j)
                s += "//@return " + std::to_string(i) + " " + memberTypeToken(ins[j]->dataType) + " " + ins[j]->name + "\n";
        }
        else if (node->isFunctionCall())
        {
            s += "//@funccall " + std::to_string(i) + " " + node->getFunctionScriptPath() + " " + node->getFunctionName() + "\n";
            const auto& ins = node->getInputPins();
            for (size_t j = 1; j < ins.size(); ++j)
                s += "//@callparam " + std::to_string(i) + " " + memberTypeToken(ins[j]->dataType) + " " + ins[j]->name + "\n";
            const auto& outs = node->getOutputPins();
            for (size_t j = 1; j < outs.size(); ++j)
                s += "//@callret " + std::to_string(i) + " " + memberTypeToken(outs[j]->dataType) + " " + outs[j]->name + "\n";
        }
    }

    for (int i = 0; i < (int)m_nodes.size(); ++i)
    {
        const auto& inputs = m_nodes[i]->getInputPins();
        for (int j = 0; j < (int)inputs.size(); ++j)
            if (inputs[j]->dataType != EDataType::Exec)
                s += "//@pin " + std::to_string(i) + " " + std::to_string(j) + " " + inputs[j]->defaultValue + "\n";
    }

    for (int i = 0; i < (int)m_nodes.size(); ++i)
        if (m_nodes[i]->getEnumSelection() != 0)
            s += "//@enum " + std::to_string(i) + " " + std::to_string(m_nodes[i]->getEnumSelection()) + "\n";

    for (const auto& link : m_links)
    {
        Pin* out = link->getOutputId().AsPointer<Pin>();
        Pin* in = link->getInputId().AsPointer<Pin>();
        if (!out || !in) continue;
        const int fromIdx = indexOfNode(out->node);
        const int toIdx = indexOfNode(in->node);
        const int fo = indexOfPin(out->node->getOutputPins(), out);
        const int ti = indexOfPin(in->node->getInputPins(), in);
        if (fromIdx < 0 || toIdx < 0 || fo < 0 || ti < 0) continue;
        s += "//@link " + std::to_string(fromIdx) + " " + std::to_string(fo) + " " +
             std::to_string(toIdx) + " " + std::to_string(ti) + "\n";
    }

    return s;
}

bool Scene::saveToFile(const std::string& path)
{
    const std::string code = generateCpp();
    // Editor save: an explicit user action on the main thread.
    if (!FileSystem::writeFileStr(path, code, /*allowMainThread*/ true))
        return false;
    m_hasBaseline = false; // re-baseline against the just-saved state next frame
    return true;
}

namespace
{
    struct LineReader
    {
        const std::string& s;
        size_t pos = 0;
        void skipWs() { while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t')) ++pos; }
        std::string token() { skipWs(); size_t start = pos; while (pos < s.size() && s[pos] != ' ' && s[pos] != '\t') ++pos; return s.substr(start, pos - start); }
        std::string rest() { skipWs(); std::string r = s.substr(pos); while (!r.empty() && (r.back() == '\r' || r.back() == '\n')) r.pop_back(); return r; }
    };

    int toInt(const std::string& s)
    {
        int sign = 1; size_t i = 0;
        if (i < s.size() && (s[i] == '-' || s[i] == '+')) { sign = s[i] == '-' ? -1 : 1; ++i; }
        int v = 0;
        for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) v = v * 10 + (s[i] - '0');
        return v * sign;
    }
}

// Core //@-tag parser shared by loadFromFile (full replace) and pasteFromClipboard (adds onto the live graph).
// Node positions are shifted by `offset` as they're created (0,0 for a normal load). Does NOT clear the
// existing graph or touch first-frame/baseline bookkeeping — callers own that. `byIndex` is filled in,
// indexed by the snippet's own local //@node indices (0..N-1 for a clipboard paste, whatever a file uses
// for a full load), so callers can find the newly created nodes afterward (e.g. to select them).
void Scene::loadLinesIntoGraph(const std::vector<std::string>& lines, ImVec2 offset, std::vector<Node*>& byIndex)
{
    // pass 1: nodes
    for (const std::string& ln : lines)
    {
        LineReader r{ ln };
        if (r.token() != "//@node") continue;
        const int idx = toInt(r.token());
        const std::string typeId = r.token();
        const int x = toInt(r.token());
        const int y = toInt(r.token());
        if (!findNodeDef(typeId)) continue;
        Node& n = addNodeOfType(typeId, ImVec2((float)x + offset.x, (float)y + offset.y));
        if ((int)byIndex.size() <= idx) byIndex.resize(idx + 1, nullptr);
        byIndex[idx] = &n;
    }

    // pass 1a: Reroute pins (before links, which reference these pins by index)
    for (const std::string& ln : lines)
    {
        LineReader r{ ln };
        if (r.token() != "//@reroute") continue;
        const int ni = toInt(r.token());
        const std::string typeTok = r.token();
        if (ni >= 0 && ni < (int)byIndex.size() && byIndex[ni] && byIndex[ni]->isReroute())
            byIndex[ni]->makeReroute(dataTypeFromToken(typeTok));
    }

    // pass 1b: Script Data members (before links, which reference these output pins by index)
    for (const std::string& ln : lines)
    {
        LineReader r{ ln };
        if (r.token() != "//@member") continue;
        const int ni = toInt(r.token());
        const std::string typeTok = r.token();
        const std::string name = r.token();
        if (ni < 0 || ni >= (int)byIndex.size() || !byIndex[ni] || !byIndex[ni]->isDynamic()) continue;
        byIndex[ni]->addMember(memberTypeFromToken(typeTok), name);
    }

    // pass 1b2: On Event entries (before links, which reference these output pins by index)
    for (const std::string& ln : lines)
    {
        LineReader r{ ln };
        if (r.token() != "//@event") continue;
        const int ni = toInt(r.token());
        const std::string name = r.rest();
        if (ni < 0 || ni >= (int)byIndex.size() || !byIndex[ni] || !byIndex[ni]->isEventNode()) continue;
        byIndex[ni]->addEventEntry(name);
    }

    // pass 1b2b: Trigger Audio alias entries (before links, which reference these input pins by index)
    for (const std::string& ln : lines)
    {
        LineReader r{ ln };
        if (r.token() != "//@audioentry") continue;
        const int ni = toInt(r.token());
        const std::string name = r.rest();
        if (ni < 0 || ni >= (int)byIndex.size() || !byIndex[ni] || !byIndex[ni]->isTriggerAudio()) continue;
        byIndex[ni]->addAudioEntry(name);
    }

    // pass 1b3: Function boundary + call nodes (before links, which reference these pins by index)
    for (const std::string& ln : lines) // function names first (pairs Input/Output, titles Call)
    {
        LineReader r{ ln };
        if (r.token() != "//@funcname") continue;
        const int ni = toInt(r.token());
        const std::string name = r.token();
        if (ni >= 0 && ni < (int)byIndex.size() && byIndex[ni])
            byIndex[ni]->setFunctionName(name);
    }
    for (const std::string& ln : lines) // Function Input parameters (its output pins)
    {
        LineReader r{ ln };
        if (r.token() != "//@param") continue;
        const int ni = toInt(r.token());
        const std::string typeTok = r.token();
        const std::string name = r.token();
        if (ni >= 0 && ni < (int)byIndex.size() && byIndex[ni] && byIndex[ni]->isFunctionInput())
            byIndex[ni]->addParam(memberTypeFromToken(typeTok), name);
    }
    for (const std::string& ln : lines) // Function Output returns (its input pins)
    {
        LineReader r{ ln };
        if (r.token() != "//@return") continue;
        const int ni = toInt(r.token());
        const std::string typeTok = r.token();
        const std::string name = r.token();
        if (ni >= 0 && ni < (int)byIndex.size() && byIndex[ni] && byIndex[ni]->isFunctionOutput())
            byIndex[ni]->addReturn(memberTypeFromToken(typeTok), name);
    }
    {
        // Function Call: gather each call's signature (file order) then rebuild its pins in one shot. The
        // signature is stored inline (not re-read from the referenced file) so a load is self-contained.
        std::map<int, PinSig> callParams, callReturns;
        std::map<int, std::pair<std::string, std::string>> callTargets; // node -> (scriptPath, funcName)
        for (const std::string& ln : lines)
        {
            LineReader r{ ln };
            const std::string tag = r.token();
            if (tag == "//@funccall")
            {
                const int ni = toInt(r.token());
                const std::string path2 = r.token();
                const std::string name = r.token();
                callTargets[ni] = { path2, name };
            }
            else if (tag == "//@callparam")
            {
                const int ni = toInt(r.token());
                const std::string typeTok = r.token();
                const std::string name = r.token();
                callParams[ni].push_back({ memberTypeFromToken(typeTok), name });
            }
            else if (tag == "//@callret")
            {
                const int ni = toInt(r.token());
                const std::string typeTok = r.token();
                const std::string name = r.token();
                callReturns[ni].push_back({ memberTypeFromToken(typeTok), name });
            }
        }
        for (const auto& [ni, target] : callTargets)
        {
            if (ni < 0 || ni >= (int)byIndex.size() || !byIndex[ni] || !byIndex[ni]->isFunctionCall())
                continue;
            byIndex[ni]->setFunctionScriptPath(target.first);
            byIndex[ni]->setFunctionName(target.second);
            byIndex[ni]->makeFunctionCall(callParams[ni], callReturns[ni]);
        }
    }

    // pass 1c: Label box size + caption
    for (const std::string& ln : lines)
    {
        LineReader r{ ln };
        const std::string tag = r.token();
        if (tag == "//@labelsize")
        {
            const int ni = toInt(r.token());
            const int w = toInt(r.token());
            const int h = toInt(r.token());
            if (ni >= 0 && ni < (int)byIndex.size() && byIndex[ni] && byIndex[ni]->isLabel())
                byIndex[ni]->setLabelSize(ImVec2((float)w, (float)h));
        }
        else if (tag == "//@labeltext")
        {
            const int ni = toInt(r.token());
            const std::string text = r.rest();
            if (ni >= 0 && ni < (int)byIndex.size() && byIndex[ni] && byIndex[ni]->isLabel())
                byIndex[ni]->setLabelText(text);
        }
    }

    // pass 2: links (before defaults, so wildcard groups can resolve from them first)
    for (const std::string& ln : lines)
    {
        LineReader r{ ln };
        if (r.token() != "//@link") continue;
        const int a = toInt(r.token());
        const int b = toInt(r.token());
        const int c = toInt(r.token());
        const int d = toInt(r.token());
        Node* from = (a >= 0 && a < (int)byIndex.size()) ? byIndex[a] : nullptr;
        Node* to = (c >= 0 && c < (int)byIndex.size()) ? byIndex[c] : nullptr;
        connectNodes(from, b, to, d);
    }

    // pass 3: resolve wildcard types from the now-restored links (may reset unconnected defaults)
    for (Node* node : byIndex)
        resolveNodeTypes(node);

    // pass 4: input pin defaults (after resolution, so saved literals win over type defaults)
    for (const std::string& ln : lines)
    {
        LineReader r{ ln };
        if (r.token() != "//@pin") continue;
        const int ni = toInt(r.token());
        const int pi = toInt(r.token());
        const std::string lit = r.rest();
        if (ni < 0 || ni >= (int)byIndex.size() || !byIndex[ni]) continue;
        const auto& inputs = byIndex[ni]->getInputPins();
        if (pi >= 0 && pi < (int)inputs.size())
            inputs[pi]->defaultValue = lit;
    }

    // pass 5: dropdown-property selections
    for (const std::string& ln : lines)
    {
        LineReader r{ ln };
        if (r.token() != "//@enum") continue;
        const int ni = toInt(r.token());
        const int sel = toInt(r.token());
        if (ni >= 0 && ni < (int)byIndex.size() && byIndex[ni])
            byIndex[ni]->setEnumSelection(sel);
    }
}

// A reroute copied without the node on one end of its link (paste/duplicate only clips links where BOTH
// endpoints are selected — see collectSelection) comes out of loadLinesIntoGraph missing that connection: an
// orphaned waypoint routing nothing. Delete those so a paste/duplicate doesn't litter the graph with dead
// dots. removeNode's own reroute-rejoin logic only fires when BOTH sides are still connected, so this can't
// accidentally reconnect something that shouldn't be. Nulls out the pruned entries in byIndex so a caller's
// later "select every pasted node" loop (which already checks for null) skips them.
void Scene::pruneDanglingReroutes(std::vector<Node*>& byIndex)
{
    for (Node*& n : byIndex)
    {
        if (!n || !n->isReroute())
            continue;
        const auto& inputs = n->getInputPins();
        const auto& outputs = n->getOutputPins();
        const bool bothConnected = !inputs.empty() && !outputs.empty() &&
            inputs[0]->numConnections > 0 && outputs[0]->numConnections > 0;
        if (!bothConnected)
        {
            removeNode(n->getId());
            n = nullptr;
        }
    }
}

bool Scene::loadFromFile(const std::string& path)
{
    if (!FileSystem::exists(path, /*allowMainThread*/ true))
        return false;
    std::istringstream file(FileSystem::readFileStr(path, /*allowMainThread*/ true));

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(file, line))
        lines.push_back(line);

    m_links.clear();
    m_nodes.clear();
    std::vector<Node*> byIndex;
    loadLinesIntoGraph(lines, ImVec2(0.0f, 0.0f), byIndex);

    m_firstFrame = true;
    m_hasBaseline = false; // re-baseline against the freshly loaded graph once it renders
    return !m_nodes.empty();
}

// Scoped serializeGraph: only `nodes` (with fresh local indices 0..N-1, independent of their position in the
// live m_nodes) and only `links` (the caller is expected to have already filtered these to ones internal to
// the selection — see copySelectedToClipboard). Mirrors serializeGraph's per-node-type sections exactly, just
// keyed by local index instead of indexOfNode, so a paste elsewhere reconstructs the same nodes/links/data.
std::string Scene::serializeSubset(const std::vector<Node*>& nodes, const std::vector<Link*>& links)
{
    std::map<const Node*, int> localIndex;
    for (int i = 0; i < (int)nodes.size(); ++i)
        localIndex[nodes[i]] = i;

    std::string s = "//@nodeclip 1\n";

    for (int i = 0; i < (int)nodes.size(); ++i)
    {
        const Node* node = nodes[i];
        const ImVec2 pos = m_nodeEditorContext ? ed::GetNodePosition(node->getId()) : ImVec2(0.0f, 0.0f);
        s += "//@node " + std::to_string(i) + " " + node->getTypeId() + " " +
             std::to_string((int)pos.x) + " " + std::to_string((int)pos.y) + "\n";
    }

    for (int i = 0; i < (int)nodes.size(); ++i)
        if (nodes[i]->isLabel())
        {
            const ImVec2 size = nodes[i]->getLabelSize();
            s += "//@labelsize " + std::to_string(i) + " " + std::to_string((int)size.x) + " " + std::to_string((int)size.y) + "\n";
            s += "//@labeltext " + std::to_string(i) + " " + nodes[i]->getLabelText() + "\n";
        }

    for (int i = 0; i < (int)nodes.size(); ++i)
        if (nodes[i]->isReroute() && !nodes[i]->getInputPins().empty())
            s += "//@reroute " + std::to_string(i) + " " + dataTypeToken(nodes[i]->getInputPins()[0]->dataType) + "\n";

    for (int i = 0; i < (int)nodes.size(); ++i)
        if (nodes[i]->isDynamic())
            for (const auto& pin : nodes[i]->getOutputPins())
                s += "//@member " + std::to_string(i) + " " + memberTypeToken(pin->dataType) + " " + pin->name + "\n";

    for (int i = 0; i < (int)nodes.size(); ++i)
        if (nodes[i]->isEventNode())
            for (const auto& pin : nodes[i]->getOutputPins())
                s += "//@event " + std::to_string(i) + " " + pin->name + "\n";

    for (int i = 0; i < (int)nodes.size(); ++i)
        if (nodes[i]->isTriggerAudio())
            for (const auto& pin : nodes[i]->getInputPins())
                if (pin->dataType == EDataType::Exec)
                    s += "//@audioentry " + std::to_string(i) + " " + pin->name + "\n";

    for (int i = 0; i < (int)nodes.size(); ++i)
    {
        const Node* node = nodes[i];
        if (node->isFunctionInput())
        {
            s += "//@funcname " + std::to_string(i) + " " + node->getFunctionName() + "\n";
            const auto& outs = node->getOutputPins();
            for (size_t j = 1; j < outs.size(); ++j)
                s += "//@param " + std::to_string(i) + " " + memberTypeToken(outs[j]->dataType) + " " + outs[j]->name + "\n";
        }
        else if (node->isFunctionOutput())
        {
            const auto& ins = node->getInputPins();
            for (size_t j = 1; j < ins.size(); ++j)
                s += "//@return " + std::to_string(i) + " " + memberTypeToken(ins[j]->dataType) + " " + ins[j]->name + "\n";
        }
        else if (node->isFunctionCall())
        {
            s += "//@funccall " + std::to_string(i) + " " + node->getFunctionScriptPath() + " " + node->getFunctionName() + "\n";
            const auto& ins = node->getInputPins();
            for (size_t j = 1; j < ins.size(); ++j)
                s += "//@callparam " + std::to_string(i) + " " + memberTypeToken(ins[j]->dataType) + " " + ins[j]->name + "\n";
            const auto& outs = node->getOutputPins();
            for (size_t j = 1; j < outs.size(); ++j)
                s += "//@callret " + std::to_string(i) + " " + memberTypeToken(outs[j]->dataType) + " " + outs[j]->name + "\n";
        }
    }

    for (int i = 0; i < (int)nodes.size(); ++i)
    {
        const auto& inputs = nodes[i]->getInputPins();
        for (int j = 0; j < (int)inputs.size(); ++j)
            if (inputs[j]->dataType != EDataType::Exec)
                s += "//@pin " + std::to_string(i) + " " + std::to_string(j) + " " + inputs[j]->defaultValue + "\n";
    }

    for (int i = 0; i < (int)nodes.size(); ++i)
        if (nodes[i]->getEnumSelection() != 0)
            s += "//@enum " + std::to_string(i) + " " + std::to_string(nodes[i]->getEnumSelection()) + "\n";

    for (const Link* link : links)
    {
        Pin* out = link->getOutputId().AsPointer<Pin>();
        Pin* in = link->getInputId().AsPointer<Pin>();
        if (!out || !in) continue;
        const auto fromIt = localIndex.find(out->node);
        const auto toIt = localIndex.find(in->node);
        if (fromIt == localIndex.end() || toIt == localIndex.end()) continue; // not internal to the subset
        const int fo = indexOfPin(out->node->getOutputPins(), out);
        const int ti = indexOfPin(in->node->getInputPins(), in);
        if (fo < 0 || ti < 0) continue;
        s += "//@link " + std::to_string(fromIt->second) + " " + std::to_string(fo) + " " +
             std::to_string(toIt->second) + " " + std::to_string(ti) + "\n";
    }

    return s;
}

// Gathers every currently-selected node (plus any reroute waypoint that sits between two selected nodes —
// see addReroutesBetweenSelected), and every link where both endpoints are in that set — the same subset
// copySelectedToClipboard and duplicateSelection each turn into a serialized clipping.
void Scene::collectSelection(std::vector<Node*>& nodes, std::vector<Link*>& links) const
{
    const int selCount = ed::GetSelectedObjectCount();
    if (selCount <= 0)
        return;
    std::vector<ed::NodeId> nodeIds(selCount);
    const int nodeCount = ed::GetSelectedNodes(nodeIds.data(), selCount);

    std::set<Node*> selected;
    for (int i = 0; i < nodeCount; ++i)
        if (Node* n = nodeIds[i].AsPointer<Node>())
            if (selected.insert(n).second)
                nodes.push_back(n);
    if (nodes.empty())
        return;

    addReroutesBetweenSelected(selected, nodes, m_links);

    for (const auto& link : m_links)
    {
        Pin* out = link->getOutputId().AsPointer<Pin>();
        Pin* in = link->getInputId().AsPointer<Pin>();
        if (out && in && selected.count(out->node) && selected.count(in->node))
            links.push_back(link.get());
    }
}

// Copies the current selection to the OS clipboard (so paste also works across different open scripts). Ctrl+V
// elsewhere reconstructs the same nodes/links/relative layout via loadLinesIntoGraph.
void Scene::copySelectedToClipboard()
{
    std::vector<Node*> nodes;
    std::vector<Link*> links;
    collectSelection(nodes, links);
    if (nodes.empty())
        return;

    ImGui::SetClipboardText(serializeSubset(nodes, links).c_str());
}

// Ctrl+V: reconstructs whatever copySelectedToClipboard last put on the OS clipboard, offsetting every node
// so the selection's top-left corner lands at `canvasPos` (the mouse), preserving relative positions. Ignores
// the clipboard if it doesn't hold one of our clippings (e.g. plain text was copied from elsewhere).
void Scene::pasteFromClipboard(ImVec2 canvasPos)
{
    const char* clip = ImGui::GetClipboardText();
    if (!clip)
        return;
    const std::string text(clip);
    if (text.rfind("//@nodeclip", 0) != 0)
        return;

    std::vector<std::string> lines = splitLines(text);

    // Offset so the pasted selection's centroid (average node position) lands at the cursor — every node
    // keeps its position relative to that centroid, so the layout copied is preserved exactly, just recentered.
    float sumX = 0.0f, sumY = 0.0f;
    int nodeCount = 0;
    for (const std::string& ln : lines)
    {
        LineReader r{ ln };
        if (r.token() != "//@node") continue;
        r.token(); r.token(); // skip index, typeId
        sumX += (float)toInt(r.token());
        sumY += (float)toInt(r.token());
        ++nodeCount;
    }
    if (nodeCount == 0)
        return; // no nodes in the clipping

    const ImVec2 offset = ImVec2(canvasPos.x - sumX / nodeCount, canvasPos.y - sumY / nodeCount);

    std::vector<Node*> byIndex;
    loadLinesIntoGraph(lines, offset, byIndex);
    pruneDanglingReroutes(byIndex);

    ed::ClearSelection();
    for (Node* n : byIndex)
        if (n)
            ed::SelectNode(n->getId(), true);
}

// Node context menu "Duplicate": clones clickedId plus whatever else is already selected, offset a little
// from the originals (unlike Ctrl+V, which recenters on the mouse), and selects the clones so they can be
// dragged into place together. Self-contained — it doesn't touch the OS clipboard.
void Scene::duplicateSelection(ed::NodeId clickedId)
{
    ed::SelectNode(clickedId, true); // fold the right-clicked node into whatever's already selected

    std::vector<Node*> nodes;
    std::vector<Link*> links;
    collectSelection(nodes, links);
    if (nodes.empty())
        return;

    const std::vector<std::string> lines = splitLines(serializeSubset(nodes, links));
    constexpr ImVec2 kDuplicateOffset(30.0f, 30.0f);

    std::vector<Node*> byIndex;
    loadLinesIntoGraph(lines, kDuplicateOffset, byIndex);
    pruneDanglingReroutes(byIndex);

    ed::ClearSelection();
    for (Node* n : byIndex)
        if (n)
            ed::SelectNode(n->getId(), true);
}

// Node context menu "Reset": restores every input pin's literal (and, for a wildcard pin, its pin-group type)
// back to what NodeDef declares, then disconnects the node entirely. Pins are reset by position against
// def->inputs, which only lines up for a plain NodeDef-spawned node — dynamic-pin node types (Script Data,
// On Event, Function I/O, Function Call, Reroute, Trigger Audio's synced alias pins) skip the literal reset
// and just get disconnected.
void Scene::resetNodeToSpawnDefaults(ed::NodeId nodeId)
{
    Node* node = nodeId.AsPointer<Node>();
    if (!node)
        return;

    if (const NodeDef* def = findNodeDef(node->getTypeId());
        def && !node->isDynamic() && !node->isEventNode() && !node->isFunctionInput() &&
        !node->isFunctionOutput() && !node->isFunctionCall() && !node->isReroute() && !node->isTriggerAudio())
    {
        const auto& inputs = node->getInputPins();
        for (size_t i = 0; i < inputs.size() && i < def->inputs.size(); ++i)
        {
            Pin& pin = *inputs[i];
            const PinDef& pinDef = def->inputs[i];
            // Set the pin's dataType up front (not just its default) so that when the link deletions below
            // drain next frame, resolveNodeTypes sees "no change" for this pin and doesn't clobber the
            // literal just restored here with its own generic defaultValueForType(resolved) reset.
            pin.dataType = pinDef.type;
            pin.color = dataTypeColor(pinDef.type);
            pin.shape = pinDef.type == EDataType::Exec ? EPinShape_Flow
                      : pin.mutability != EMutableType::Readable ? EPinShape_Square
                      : EPinShape_Circle;
            pin.defaultValue = pinDef.defaultValue;
        }
    }

    for (const auto& link : m_links)
    {
        Pin* in = link->getInputId().AsPointer<Pin>();
        Pin* out = link->getOutputId().AsPointer<Pin>();
        if ((in && in->node == node) || (out && out->node == node))
            ed::DeleteLink(link->getId());
    }
}

void Scene::update(double deltaSec)
{
    ed::SetCurrentEditor(m_nodeEditorContext);
    ed::Begin("Script Editor", ImVec2(0.0, 0.0f));
    ed::GetStyle().NodePadding = ImVec4(8, 8, 8, 8);

    for (auto& node : m_nodes)
        node->update(deltaSec, m_firstFrame);

    // Apply a structural member edit a Script Data node recorded this frame. It's replayed on every Script
    // Data node so they share one member set, fixing up the links that touch their pins (Scene owns links).
    for (auto& node : m_nodes)
    {
        if (!node->isDynamic())
            continue;
        if (const MemberEdit edit = node->takeMemberEdit(); edit.op != EMemberOp::None)
        {
            applyMemberEdit(edit);
            break; // only one node is edited per frame
        }
    }

    // Same as above, for On Event nodes' named entries.
    for (auto& node : m_nodes)
    {
        if (!node->isEventNode())
            continue;
        if (const MemberEdit edit = node->takeMemberEdit(); edit.op != EMemberOp::None)
        {
            applyEventEdit(edit);
            break;
        }
    }

    // Same as above, for Function Input/Output params & returns (each function is independent, not synced).
    for (auto& node : m_nodes)
    {
        if (!node->isFunctionInput() && !node->isFunctionOutput())
            continue;
        if (const MemberEdit edit = node->takeMemberEdit(); edit.op != EMemberOp::None)
        {
            applyFunctionEdit(node.get(), edit);
            break;
        }
    }

    processInteractions();

    for (auto& link : m_links)
        link->update(deltaSec, m_firstFrame);

    // Ctrl+C/Ctrl+V: copy the current selection (nodes + internal links) to the OS clipboard and back, so
    // pasting also works across different open scripts. BeginShortcut() already gates on editor focus.
    //
    // ed::Suspend() is required before reading the mouse position here: while the canvas is in its normal
    // (un-suspended) "local space" rendering mode, the library temporarily overwrites ImGui's own io.MousePos
    // with the already-canvas-local coordinate (see EnterLocalSpace() in imgui_canvas.cpp), so every node's
    // internal ImGui widgets see zoomed/panned-correct positions without converting per-widget. Calling
    // ed::ScreenToCanvas() on that already-local value double-applies the pan/zoom transform. Suspend()
    // restores the real screen-space mouse position first, matching how the AddNodePopup code below does it.
    if (ed::BeginShortcut())
    {
        if (ed::AcceptCopy())
            copySelectedToClipboard();
        if (ed::AcceptPaste())
        {
            ed::Suspend();
            const ImVec2 canvasPos = ed::ScreenToCanvas(ImGui::GetMousePos());
            ed::Resume();
            pasteFromClipboard(canvasPos);
        }
        ed::EndShortcut();
    }

    // Same two actions, requested from outside this frame (e.g. main.cpp's global keyboard hook) via
    // requestCopy()/requestPaste() — consumed here, where mouse position and the canvas transform are valid.
    if (m_pendingCopyRequest)
    {
        m_pendingCopyRequest = false;
        copySelectedToClipboard();
    }
    if (m_pendingPasteRequest)
    {
        m_pendingPasteRequest = false;
        ed::Suspend();
        const ImVec2 canvasPos = ed::ScreenToCanvas(ImGui::GetMousePos());
        ed::Resume();
        pasteFromClipboard(canvasPos);
    }

    // Right-click the canvas to add a node from the palette.
    ed::Suspend();
    if (ed::ShowBackgroundContextMenu())
    {
        m_pendingAddPos = ed::ScreenToCanvas(ImGui::GetMousePos());
        m_pendingAddScreenPos = ImGui::GetMousePos(); // captured once, at the click — not recomputed while open
        m_pendingLinkPin = nullptr; // plain right-click add, not tied to a dropped link
        m_importFunctions = scanImportableFunctions(); // refresh the importable-function list while it's open
        ImGui::OpenPopup("AddNodePopup");
    }
    // Pin the popup a bit right of the click (ImGuiCond_Always: reasserted every frame it's open, so it can't
    // land back on the default mouse-cursor position) so it doesn't sit directly on top of whatever pin/link
    // was under the right-click.
    ImGui::SetNextWindowPos(ImVec2(m_pendingAddScreenPos.x + 24.0f, m_pendingAddScreenPos.y), ImGuiCond_Always);
    // A bit of left padding before the menu text/submenu arrows. Pushed for the whole popup's lifetime so
    // every submenu opened from it (BeginMenu spawns its own window) inherits the same padding.
    //ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, ImGui::GetStyle().WindowPadding.y));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(5.0f, 5.0f));
    if (ImGui::BeginPopup("AddNodePopup"))
    {
        const bool hadQuery = m_addNodeSearchBuf[0] != '\0';
        bool justSeededChar = false; // set below when a keystroke seeds the buffer as the box becomes active
        if (ImGui::IsWindowAppearing())
        {
            // Clear the search box the instant the popup opens (IsWindowAppearing is true exactly that one
            // frame) and focus it right away, so the popup opens ready to type with no click needed.
            m_addNodeSearchBuf[0] = '\0';
            m_addNodeSearchSelected = 0;
            focusSearchBox();
        }
        else if (hadQuery)
        {
            // With a query typed, Up/Down/Enter drive the filtered results list below via raw key checks (see
            // the "!search.empty()" branch further down), not ImGui's own keyboard nav — so focus must stay
            // glued to the search box, or typing would stop reaching it. Only reasserted once it's confirmed
            // lost (last frame's IsItemActive(), captured below) AND nothing else is currently active either:
            // clicking a result row makes IT briefly active (mouse held down) before returning true on
            // release, and forcing focus back onto the search box mid-click would cancel that click before it
            // completes — skip reasserting while any such click is in progress and let it finish naturally.
            if (!m_addNodeSearchWasActive && !ImGui::IsAnyItemActive())
                focusSearchBox();
        }
        else
        {
            // Empty query: leave focus alone so ImGui's own keyboard nav can move Up/Down/Enter through the
            // plain category tree below (BeginMenu/MenuItem are natively nav-able) — only steal focus back
            // once the user actually starts typing a letter/digit, so a fresh query still "just works".
            ImGuiKey typedKey = ImGuiKey_None;
            for (int k = ImGuiKey_A; k <= ImGuiKey_Z && typedKey == ImGuiKey_None; ++k)
                if (ImGui::IsKeyPressed((ImGuiKey)k, false)) typedKey = (ImGuiKey)k;
            for (int k = ImGuiKey_0; k <= ImGuiKey_9 && typedKey == ImGuiKey_None; ++k)
                if (ImGui::IsKeyPressed((ImGuiKey)k, false)) typedKey = (ImGuiKey)k;

            if (typedKey != ImGuiKey_None)
            {
                // The search box is about to become active for the first time this frame, so InputText will
                // pick up whatever's already in the buffer as its starting content (same trick the pin
                // default-value boxes rely on) — seed it with the key that triggered the switch so it isn't
                // silently dropped waiting for SDL's text-input mode to catch up next frame (see
                // focusSearchBox for why that catch-up now happens immediately anyway, closing this gap too).
                if (const char c = charForTypingKey(typedKey, ImGui::GetIO().KeyShift))
                {
                    m_addNodeSearchBuf[0] = c;
                    m_addNodeSearchBuf[1] = '\0';
                    m_addNodeSearchSelected = 0;
                    justSeededChar = true; // see the ReloadUserBufAndMoveToEnd() call below
                }
                focusSearchBox();
            }
        }
        ImGui::SetNextItemWidth(220.0f);
        const ImGuiID searchId = ImGui::GetID("##addNodeSearch");
        if (ImGui::InputText("##addNodeSearch", m_addNodeSearchBuf, sizeof(m_addNodeSearchBuf)))
            m_addNodeSearchSelected = 0; // query changed: re-highlight the top result
        // Activating a text field via code (SetKeyboardFocusHere, just above) selects all of its existing
        // text by default — same as tabbing into a field. That's exactly wrong here: we just seeded that
        // text ourselves and want the cursor placed after it, not the whole thing selected (or the very next
        // keystroke would overwrite it instead of appending). ReloadUserBufAndMoveToEnd is the documented way
        // to tell an active InputText "the buffer was changed externally, re-sync and put the cursor at the
        // end" (see imgui_internal.h) instead of leaving whatever selection activation produced.
        if (justSeededChar)
            if (ImGuiInputTextState* state = ImGui::GetInputTextState(searchId))
                state->ReloadUserBufAndMoveToEnd();
        m_addNodeSearchWasActive = ImGui::IsItemActive(); // read by the hadQuery branch above, next frame

        // Spawns a node def or imports a function at the click position and closes the popup — shared by the
        // plain category tree below and the search results list.
        auto place = [&](const NodeDef* def, const FunctionRef* funcRef)
        {
            if (def)
            {
                Node& added = addNodeOfType(def->typeId, m_pendingAddPos);
                if (added.isDynamic())
                    syncNewMemberNode(added); // keep a new Script Data node in sync with the others
                if (added.isEventNode())
                    syncNewEventNode(added); // keep a new On Event node in sync with the others
                if (added.isTriggerAudio() && m_audioAliasesKnown)
                    syncTriggerAudioPins(added); // seed the alias entry pins from the owning entity
                autoConnectPending(added); // wire a link dropped here to its first matching pin, if any
            }
            else if (funcRef)
                autoConnectPending(importFunction(*funcRef, m_pendingAddPos));
            ImGui::CloseCurrentPopup();
        };

        const std::string_view search(m_addNodeSearchBuf);
        if (!search.empty())
        {
            // A flat, filtered list across every category + importable function. Up/Down move the highlight,
            // Enter places it; hovering or clicking with the mouse still works too.
            struct Result { std::string label; const NodeDef* def; const FunctionRef* funcRef; };
            std::vector<Result> results;
            for (const NodeDef& def : nodeRegistry())
            {
                if (isRerouteType(def.typeId) || isFunctionCallType(def.typeId)) // created by dbl-click / import, not the palette
                    continue;
                if (containsCI(def.displayName, search) || containsCI(def.category, search))
                    results.push_back({ def.category + ": " + def.displayName, &def, nullptr });
            }
            for (const FunctionRef& ref : m_importFunctions)
                if (containsCI(ref.displayLabel, search))
                    results.push_back({ ref.displayLabel, nullptr, &ref });

            if (results.empty())
            {
                ImGui::TextDisabled("No matches");
            }
            else
            {
                if (m_addNodeSearchSelected < 0) m_addNodeSearchSelected = 0;
                if (m_addNodeSearchSelected >= (int)results.size()) m_addNodeSearchSelected = (int)results.size() - 1;

                if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) && m_addNodeSearchSelected + 1 < (int)results.size())
                    ++m_addNodeSearchSelected;
                if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) && m_addNodeSearchSelected > 0)
                    --m_addNodeSearchSelected;
                const bool confirmed = ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);

                ImGui::BeginChild("##addNodeResults", ImVec2(260.0f, 200.0f), ImGuiChildFlags_Borders);
                bool placed = false;
                for (int i = 0; i < (int)results.size() && !placed; ++i)
                {
                    const Result& r = results[i];
                    const bool isSelected = (i == m_addNodeSearchSelected);
                    if (ImGui::Selectable(r.label.c_str(), isSelected))
                        { place(r.def, r.funcRef); placed = true; }
                    if (ImGui::IsItemHovered())
                        m_addNodeSearchSelected = i;
                    if (isSelected && confirmed)
                        { place(r.def, r.funcRef); placed = true; }
                }
                ImGui::EndChild();
            }
        }
        else
        {
            // Group node defs by category (first-seen order); each category folds into its own submenu (opens
            // on hover, standard ImGui BeginMenu behavior) so the top-level list stays one line per category
            // instead of growing into one very tall flat list.
            struct Category { std::string name; std::vector<const NodeDef*> defs; };
            std::vector<Category> categories;
            for (const NodeDef& def : nodeRegistry())
            {
                if (isRerouteType(def.typeId) || isFunctionCallType(def.typeId)) // created by dbl-click / import, not the palette
                    continue;
                if (categories.empty() || categories.back().name != def.category)
                    categories.push_back({ def.category, {} });
                categories.back().defs.push_back(&def);
            }

            for (const Category& category : categories)
            {
                if (ImGui::BeginMenu(category.name.c_str()))
                {
                    for (const NodeDef* def : category.defs)
                        if (ImGui::MenuItem(def->displayName.c_str()))
                            place(def, nullptr);
                    ImGui::EndMenu();
                }
            }

            // Importable functions defined in other .scr files, folded the same way.
            if (!m_importFunctions.empty() && ImGui::BeginMenu("Import Function"))
            {
                for (const FunctionRef& ref : m_importFunctions)
                    if (ImGui::MenuItem(ref.displayLabel.c_str()))
                        place(nullptr, &ref);
                ImGui::EndMenu();
            }
        }

        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();

    // The popup was opened by dropping a link on empty canvas (m_pendingLinkPin set) and just closed without
    // picking anything (Escape / click elsewhere) — autoConnectPending never ran to consume/clear it, so the
    // dropped end is abandoned outright. Same cleanup as autoConnectPending's own no-match fallthrough.
    if (!ImGui::IsPopupOpen("AddNodePopup") && m_pendingLinkPin)
    {
        pruneOrphanedReroute(m_pendingLinkPin);
        m_pendingLinkPin = nullptr;
    }

    // Right-click a node for Delete/Copy/Duplicate/Reset.
    ed::NodeId contextNodeId;
    if (ed::ShowNodeContextMenu(&contextNodeId))
    {
        m_contextNodeId = contextNodeId;
        m_nodeContextScreenPos = ImGui::GetMousePos();
        ImGui::OpenPopup("NodeContextMenu");
    }
    ImGui::SetNextWindowPos(ImVec2(m_nodeContextScreenPos.x + 24.0f, m_nodeContextScreenPos.y), ImGuiCond_Always);
    if (ImGui::BeginPopup("NodeContextMenu"))
    {
        if (ImGui::MenuItem("Delete"))
            ed::DeleteNode(m_contextNodeId); // queued; drained by processInteractions() next frame
        if (ImGui::MenuItem("Copy"))
        {
            ed::SelectNode(m_contextNodeId, true); // fold into whatever's already selected, don't replace it
            copySelectedToClipboard();
        }
        if (ImGui::MenuItem("Duplicate"))
            duplicateSelection(m_contextNodeId);
        if (ImGui::MenuItem("Reset"))
            resetNodeToSpawnDefaults(m_contextNodeId);
        ImGui::EndPopup();
    }
    ed::Resume();

    if (m_firstFrame)
        ed::NavigateToContent(0.0f);

    ed::End();

    // Double-click a link to drop a reroute waypoint on it at the cursor (splits it through a draggable dot).
    if (ed::LinkId doubleClicked = ed::GetDoubleClickedLink())
        if (Link* link = doubleClicked.AsPointer<Link>())
            insertReroute(link, ed::ScreenToCanvas(ImGui::GetMousePos()));

    // Capture the clean-state baseline once the graph has rendered (node positions are now valid).
    if (!m_hasBaseline)
    {
        m_baselineState = serializeGraph();
        m_hasBaseline = true;
    }

    m_firstFrame = false;
}

bool Scene::isDirty()
{
    if (!m_hasBaseline)
        return false; // baseline not captured yet (just loaded) — treat as clean until it settles
    if (m_nodeEditorContext)
        ed::SetCurrentEditor(m_nodeEditorContext);
    return serializeGraph() != m_baselineState;
}

void Scene::processInteractions()
{
    // Re-plugging: grabbing an already-connected input pin picks up that link. Delete it and redirect the
    // in-progress drag to originate from its source instead, so from that point on — this frame and every
    // subsequent one, no matter what's currently hovered — it behaves exactly like a normal drag started at
    // that output pin (same preview, hover snapping, accept/reject path below). Called from both branches
    // below (hovering a pin vs. hovering empty canvas) so the switch happens the instant the drag starts,
    // not only once the cursor happens to first reach another pin. A no-op (returns `anchor` unchanged) once
    // already redirected, since `anchor` is then the source itself (Output), or if it never had a link.
    auto redirectIfRewiring = [&](Pin* anchor) -> Pin*
    {
        if (anchor->type != EPinType_Input) return anchor;
        Pin* source = sourceOfInput(m_links, anchor);
        if (!source) return anchor;
        std::erase_if(m_links, [&](const std::unique_ptr<Link>& link)
        {
            if (link->getInputId().AsPointer<Pin>() != anchor) return false;
            source->numConnections--;
            anchor->numConnections--;
            return true;
        });
        resolveNodeTypes(anchor->node); // the vacated node may revert a wildcard group
        ed::RedirectLinkDrag(ed::PinId(source));
        return source;
    };

    if (ed::BeginCreate())
    {
        ed::PinId pin1Id, pin2Id;
        if (ed::QueryNewLink(&pin1Id, &pin2Id))
        {
            if (pin1Id && pin2Id && pin1Id != pin2Id)
            {
                // pin1 is always the pin the drag STARTED from (the library's m_LinkStart), pin2 the one
                // currently under the cursor — regardless of drag direction.
                Pin* pin1 = redirectIfRewiring(pin1Id.AsPointer<Pin>());
                Pin* pin2 = pin2Id.AsPointer<Pin>();

                if (pin1->type == EPinType_Output)
                    std::swap(pin1, pin2);

                if (pin1->type == EPinType_Input && pin2->type == EPinType_Output &&
                    pin1->numConnections == 0 && pinsCompatible(pin1, pin2))
                {
                    if (ed::AcceptNewItem())
                    {
                        pin1->numConnections++;
                        pin2->numConnections++;
                        m_links.emplace_back(std::make_unique<Link>())->initialize(ed::PinId(pin1), ed::PinId(pin2));
                        resolveNodeTypes(pin1->node); // a new connection may resolve a wildcard group
                        resolveNodeTypes(pin2->node);
                    }
                }
                else
                {
                    ed::RejectNewItem();
                }
            }
        }
        else
        {
            // Not hovering any pin (empty canvas, or a node body — Process() treats both the same). Still
            // check for a rewire so the redirect fires immediately rather than waiting for the cursor to
            // reach another pin; and on the actual drop (mouse release), open the add-node popup with the
            // dangling end remembered so the picked node gets auto-wired to it (autoConnectPending).
            ed::PinId loosePinId;
            if (ed::QueryNewNode(&loosePinId) && loosePinId)
            {
                Pin* danglingPin = redirectIfRewiring(loosePinId.AsPointer<Pin>());
                if (ed::AcceptNewItem())
                {
                    m_pendingLinkPin = danglingPin;
                    m_pendingAddPos = ed::ScreenToCanvas(ImGui::GetMousePos());
                    m_pendingAddScreenPos = ImGui::GetMousePos();
                    m_importFunctions = scanImportableFunctions();
                    ed::Suspend();
                    ImGui::OpenPopup("AddNodePopup");
                    ed::Resume();
                }
            }
        }
    }
    ed::EndCreate();

    // The library's own Delete-key handling (inside BeginDelete/QueryDeletedNode below) requires the canvas
    // to be both focused AND hovered (EditorContext::CanAcceptUserInput()) -- so pressing Delete right after
    // clicking a node, before the mouse drifts back over the canvas, silently does nothing. Queue the current
    // selection through ed::DeleteNode/DeleteLink instead: that feeds a separate list (m_ManuallyDeletedObjects)
    // the library's Accept() drains unconditionally, so it only needs the window focused, not hovered.
    //
    // Only take this path when the canvas ISN'T hovered: when it is, the library's own interactive check
    // (CanAcceptUserInput()) fires for the same keypress and takes priority in its Accept()'s if/else-if chain,
    // leaving our queued entry in m_ManuallyDeletedObjects un-drained. A few frames later, once the library's
    // action state cycles back to idle, that stale entry gets replayed -- pointing at an object the interactive
    // path already deleted -- and QueryItem() reads freed memory. Gating on "not hovered" keeps the two paths
    // mutually exclusive. !IsAnyItemActive() keeps a Delete keystroke meant for a node's rename/text field from
    // also deleting the node.
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        !ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows) &&
        !ImGui::IsAnyItemActive() && ImGui::IsKeyPressed(ImGuiKey_Delete, false))
    {
        const int selCount = ed::GetSelectedObjectCount();
        if (selCount > 0)
        {
            std::vector<ed::NodeId> nodeIds(selCount);
            const int nodeCount = ed::GetSelectedNodes(nodeIds.data(), selCount);
            for (int i = 0; i < nodeCount; ++i)
                ed::DeleteNode(nodeIds[i]);

            std::vector<ed::LinkId> linkIds(selCount);
            const int linkCount = ed::GetSelectedLinks(linkIds.data(), selCount);
            for (int i = 0; i < linkCount; ++i)
                ed::DeleteLink(linkIds[i]);
        }
    }

    if (ed::BeginDelete())
    {
        ed::LinkId deletedLinkId;
        while (ed::QueryDeletedLink(&deletedLinkId))
        {
            if (ed::AcceptDeletedItem())
            {
                Link* link = deletedLinkId.AsPointer<Link>();
                if (!link)
                    continue;

                Pin* pin1 = link->getInputId().AsPointer<Pin>();
                Pin* pin2 = link->getOutputId().AsPointer<Pin>();

                // Deleting any segment of a routed connection tears down the whole thing: every reroute
                // waypoint in the chain and all their links (so the source and target end up disconnected).
                if ((pin1 && pin1->node->isReroute()) || (pin2 && pin2->node->isReroute()))
                {
                    deleteRerouteChain(link);
                    continue;
                }

                if (pin1) pin1->numConnections--;
                if (pin2) pin2->numConnections--;
                Node* node1 = pin1 ? pin1->node : nullptr;
                Node* node2 = pin2 ? pin2->node : nullptr;
                std::erase_if(m_links, [&](const std::unique_ptr<Link>& l) { return l.get() == link; });
                resolveNodeTypes(node1); // a removed connection may revert a wildcard group
                resolveNodeTypes(node2);
            }
        }

        ed::NodeId deletedNodeId;
        while (ed::QueryDeletedNode(&deletedNodeId))
            if (ed::AcceptDeletedItem())
                removeNode(deletedNodeId);
    }
    ed::EndDelete();
}
