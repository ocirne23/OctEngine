export module UI:ScriptEditor;

import Core;
import Core.SDL;
import Script;

// The DSL editor panel ("Script Editor" window). Displays a DSLScriptFile via Syntax::format and navigates it
// with a TOKEN cursor: the selection unit is "which rendered span (and therefore which DSLSymbol) is current,"
// never a text-character offset. Raw SDL keyboard events reach this class via Input::update -> UI::handleKeyEvent
// -> here, ahead of ImGui's own WantCaptureKeyboard gate, because the document view never claims keyboard
// capture itself (it's a hand-drawn, non-interactive display).
//
// Editing: typing (or Backspace) while a symbol/placeholder is selected begins composing -- the live text
// renders INLINE in place of the selected span, inside its highlight rectangle (renderTextArea), with the
// candidate list (AutoCompleteRules, filtered against whatever the selected span's SlotRef expects) floating
// below it (renderAutocompletePopup). Space advances between a line's stages; only Enter (or a closing gesture
// like a call's ')') COMMITS a line -- see confirmCompose's allowCommit. Confirming most candidates (a keyword, an existing
// variable/function, a literal) applies immediately via applyCandidate, replacing the slot's occupant wholesale
// -- which is also where the "always compilable" guarantee lives: a freshly-inserted call gets Placeholder
// arguments for everything it requires, matching its callee's declared types.
//
// Declaring a brand-new variable and building an if/while condition are both STAGED flows across the SAME
// inline text instead of a single-step replacement, and neither touches the document until their FINAL step:
// everything resolved so far lives only in m_composePrefix/m_pending*/m_condition* -- Escape (or navigating
// away) at any point cancels cleanly with nothing ever written, and there's no way to end up with a half-built
// statement sitting in the document. Declaring: pick a type keyword ("float "), free-type a name ("float
// test"), confirm again (now composing the initializer, "float test = "), then pick/type a value and confirm
// once more to apply the whole declaration in one shot, landing the cursor on the initializer just composed.
// Declaring a vec2/vec3/vec4 needs no special case: the type's registry constructor (e.g. "vec3(1.0, 2.0, 3.0)")
// is an ordinary parameterized call, staged through the SAME CallArgValue sub-flow any other call uses, so
// computed components work too -- see applyDeclareVariable.
// If/while: pick "if"/"while" ("if "), pick ANY in-scope variable or non-Void-returning function as the left
// operand ("if height "), pick a comparator ("if height <= "), then pick/type a right-hand value matching the
// left operand's type -- only THEN does the whole `if a op b` commit as one FlowControl+Expression, replacing
// the original statement slot in a single step (see applyConditionalStatement). Backspace on an empty compose
// box steps back one stage in either flow instead of losing everything typed.
//
// Enter (NOT composing) inserts a fresh blank statement slot right after the current line -- one level deeper
// if that line opens a block (if/while/function), else as a plain sibling, or (sitting on a synthetic `end`)
// as a sibling right after the block that `end` closes -- the ONLY way new lines appear; nothing is
// auto-appended on confirm. Backspace (not composing) normally begins editing the current selection (same as
// typing a character, minus the character), except on the specific span each statement kind is deletable
// from, where it removes instead -- covering every line kind, so there's no separate "delete" key/gesture at
// all. Deleting a bare assignment/compound-assign (its own operator span) or a call statement (its own name
// span) or a return/break (its own keyword span) just removes that one line outright (deleteLine) -- none of
// them open a block or declare a name, so there's nothing else to check or preserve. Selecting the TYPE portion
// of a local variable declaration's own line (e.g. "float" in `float test = 1.0` -- not a function parameter's
// or for-loop counter's type, both embedded in a different line's own head symbol) removes that whole
// declaration line too, but only when nothing else in its function still references it
// (AutoCompleteRules::isVariableReferenced) -- a still-used declaration can't be removed out from under its
// uses. Selecting an if/while's own keyword removes the whole header AND its synthetic `end`, un-nesting its
// OWN body by one scopeLevel so its contents survive as plain siblings in the enclosing scope
// (deleteBlockKeepBody) -- "remove this wrapper", not "delete everything inside it" -- but ONLY when any
// attached elseif/else chain is entirely empty too (attachedElseChainEmpty): otherwise there's no safe
// destination for THEIR content (same reasoning as an else/elseif's own deletion, below) and the whole
// if/while is left alone; when the chain IS empty, its (empty) header lines are consumed along with the
// original header rather than left dangling with no "if" left to continue from. Selecting an else/elseif's own
// keyword removes just that one header line (deleteLine is enough -- its body, if any, is already at the SAME
// scopeLevel as the branch before it, so no un-nesting/renumbering would be needed even if it weren't empty)
// but ONLY when its OWN branch is empty too -- a non-empty branch has no safe place for its content to go
// (silently merging it into the PRECEDING branch would reorder which condition guards which statements), so
// it's left alone. Selecting a function's or a for-loop's own keyword removes the whole header + body +
// synthetic `end` (deleteEmptyBlock) but ONLY when the body is empty -- unlike if/while, neither's body can be
// safely kept/un-nested (bare statements can't sit outside any function; a for-loop's body likely references
// the loop variable, which wouldn't exist anymore) -- and a function is additionally required to be uncalled
// anywhere (AutoCompleteRules::isFunctionReferenced); otherwise the header is left alone entirely rather than
// risking lost content or an accidental wholesale replace. Finer-grained deletion (a single call argument, a
// non-empty for-loop or else-chain's own content) isn't implemented yet.
//
// Selecting any VariableReference/VariableDeclaration or FunctionCall/FunctionDeclaration (not composing)
// outlines every OTHER occurrence of the SAME declaration/function across the document, in a distinct color
// from the cursor's own fill+outline highlight (renderTextArea, referenceHighlightTarget resolves a span's
// symbol to the canonical declaration it refers to -- itself, if it already is one -- so a reference and its
// declaration, or two calls to the same function, are recognized as the same target regardless of which one is
// currently selected). Purely a read-only visual aid; selecting/typing is unaffected.
//
// Declaring a new function is a top-level-only statement candidate ("function ", only offered where
// atLine.scopeLevel == 0 -- see AutoCompleteRules::candidatesFor): free-type a name (FunctionDeclareName, same
// collision-checked free-typing as a variable's DeclareName), then build its parameter list as a run of
// "type name" pairs (FunctionParamType picks a type from the same keyword list variables use, FunctionParamName
// free-types that parameter's name) -- typing ',' mid-name finalizes it and starts the next parameter's type
// pick; typing ')' (or confirming) finalizes the current parameter if named and closes the list into the
// FunctionDeclareDone stage -- Enter commits the WHOLE function in one shot (commitFunctionDeclaration), '-'
// first appends a "-> type" pick -- same "nothing touches the document until the end" guarantee as declaring
// a variable or an if/while condition. Landing spot is the new function's own body
// (seeded like completing an if/while), ready to type its first statement. A function's return type is a
// SEPARATE, always-available slot -- rendered blank when Void, selectable on any function (brand-new or
// pre-existing) at any time to set/change it via the same type-keyword list (see SlotRef::Kind::FunctionReturnType).
//
// Declaring a new for-loop ("for ", offered anywhere a statement is, like if/while) stages its loop variable
// exactly like a plain local declaration (ForVarType picks a type, ForVarName free-types the name with the same
// collision check, ForVarValue composes the initial value as a full chain, same as DeclareValue), then its
// condition (ForConditionLeft, SEEDED with the loop variable and extendable
// into a compound chain like "i + 2"; ForConditionOp picks the comparator, ForConditionValue composes the
// bound) and increment (ForIncrementOp picks a compound-assign operator -- +=/-=/*=//=/%=, never a plain `=`,
// ForIncrementValue composes the step against the implicit loop variable). Every VALUE clause is a full
// compound chain; the not-yet-committed loop variable rides through them as a SENTINEL candidate resolved at
// build time (m_forBuildLoopVar). Step-back (Backspace on an empty compose box) restores each resolved clause
// for further editing. Nothing touches the document until the very last step's confirm (commitForStatement),
// same "never half-built" guarantee as if/while/function; landing spot is the new loop's own body, seeded like
// completing an if/while/function.
//
// A value being composed for a brand-new declaration (DeclareValue) or a reassignment's value (ReassignValue,
// below) can be an ARBITRARY-length compound expression instead of a single value, with parenthesized grouping
// for precedence: once a candidate is matched, typing an arithmetic operator (+,-,*,/,%) commits it as a term
// and starts composing the next one, and this repeats indefinitely -- "float f = a + 2.0 + b + c + getValue() *
// 5" and "f = (a + b) * 2.0" both work. Nothing is a real DSLSymbol until the whole thing commits: the
// in-progress structure lives in m_exprStack (one ExprFrame per open paren depth; a term is either a resolved
// Candidate or, once a nested ')' closes, an entire PendingExprTerm sub-tree) plus m_exprPendingGroup for a
// just-closed group awaiting its own next action. '(' (only when nothing's been typed for the CURRENT term yet)
// pushes a fresh, empty ExprFrame -- a "parent edit block" that stays in progress until its ')' closes it;
// ')' (only if a paren is actually open) closes the innermost one, folding it into one atomic term for whatever
// encloses it. Confirming (Enter) only finalizes when every paren is closed AND either nothing was ever
// typed (falls back to a Placeholder, same tolerance as before) or the trailing term is fully resolved -- never
// a broken "a +" with nothing after it (exprTryFinalize) -- the "always compilable" invariant applied to
// expressions: an open paren simply cannot be committed. The final commit (buildExpressionFromTerms) stores the
// flat term/operator sequence VERBATIM as an Expression chain -- parenthesized groups become nested `grouped`
// Expressions, and precedence is the transpiler's job at emit time, never encoded into the stored structure
// (see DSL.ixx) -- which is exactly what keeps every term and operator individually addressable/editable
// afterwards. Backspace on an empty compose box undoes exactly one action at a time -- the last operator+term,
// the last '(' if nothing's inside it yet, or reopening the innermost just-closed ')' with its own last term
// restored for further editing -- before finally stepping back to DeclareName (or cancelling, for Reassign)
// once nothing remains; m_composePrefix is always fully rebuilt from m_exprStack after each such change
// (exprComposePrefixFromStack) rather than hand-patched, so forward typing and backward undo can never drift
// out of sync with each other. Reassigning an EXISTING variable ("name = ", offered as a Reassign candidate for
// every in-scope variable in the SAME Void statement list functions already appear in) works exactly like
// declaring one, minus the type/name steps -- nothing touches the document until ReassignValue's confirm
// (commitReassignStatement), landing on the assignment statement itself (no new lines seeded -- reassignment
// doesn't open a block).
//
// COMMITTED expressions stay fully editable in place, one span at a time -- and this is also how every OTHER
// value slot (call arguments, a return's value, either side of an if/while/for condition, a vector literal's
// components, a for-loop's clause values) grows into a compound chain, since their staged authoring flows stay
// single-value. Selecting any operand/value span and typing begins EditExpr: a replacement segment composed
// through the SAME m_exprStack machinery (so it can itself be compound/parenthesized), committed by
// applyEditExpr -- a single term replaces the occupant outright; a multi-term segment splices into the
// surrounding arithmetic chain flat, or becomes one nested sub-chain when the surroundings are a
// comparison/assignment side (their Expressions stay exactly binary by construction). Selecting a value and
// typing an arithmetic operator instead INSERTS: the composed segment chains in right after that value
// (splicing into its arithmetic parent chain, or wrapping a standalone/structural-side value into a fresh
// chain with itself as the first operand) -- gated on the slot's element type being numeric, so `canJump() +
// ...` is never offered. Selecting an OPERATOR span and typing an operator character begins ReplaceOperator:
// a candidate list of the SAME class (arithmetic/comparison/assign) as the current operator, confirmed into
// operators[i] in place -- flat storage means no restructuring, `a + b * c` re-reads correctly the moment `+`
// becomes `*`. Backspace on a chain operand removes it together with its adjacent arithmetic operator;
// Backspace on an arithmetic operator span removes it together with the operand AFTER it; either way a chain
// left with one operand unwraps to just that operand (repointSymbol), and a comparison/assignment's own
// operator/sides never delete this way (their Backspace keeps the statement-level behaviors below). Every
// in-place edit ends by restoring the line's post-order head and garbage-collecting symbols no longer
// reachable from it (restoreHeadAndCollect) -- replaced operands must not linger in the line's flat ownership
// list, where isVariableReferenced-style scans would still count them as uses.

struct PendingExprTerm; // fwd decl -- PendingExprChain (below) needs it before PendingExprTerm needs PendingExprChain back

// One fully-resolved arithmetic chain (terms + the operators between them) a staged flow holds for a value
// position -- a comparison side, a for-loop clause, an initializer's worth of expression, or (see
// PendingExprTerm::callArgs) ONE ARGUMENT of a staged call, itself possibly compound or containing a nested
// call. `terms` is non-empty once resolved; ops.size() == terms.size() - 1. A single plain value is a
// one-term chain.
struct PendingExprChain
{
	std::vector<PendingExprTerm> terms;
	std::vector<DSLOperator> ops;
};

// One term in a compound "value [op value]*" expression being composed (see the comment above): a plain
// matched candidate, a RESOLVED parameterized call (a Function candidate plus its staged arguments -- see the
// CallArgValue value sub-flow), or (isGroup) an entire parenthesized sub-expression, itself a nested
// (groupTerms, groupOps) sequence built the exact same way. Purely transient staging data -- nothing here is a
// real DSLSymbol until the whole thing commits (see ScriptEditor::buildExpressionFromTerms).
struct PendingExprTerm
{
	bool isGroup = false;
	Candidate candidate;                     // meaningful if !isGroup
	std::vector<PendingExprTerm> groupTerms; // meaningful if isGroup
	std::vector<DSLOperator> groupOps;       // meaningful if isGroup -- groupOps.size() == groupTerms.size() - 1
	std::vector<PendingExprChain> callArgs;  // non-empty = a parameterized call's staged arguments, one FULL
	                                          // (possibly compound/nested-call) chain per callee parameter
};

// One nesting level of the expression being composed (one entry per currently-open paren depth, index 0 = the
// outermost/top level). `terms`/`ops` hold everything ALREADY resolved at this level; the term currently being
// typed/matched lives OUTSIDE this struct, in the ordinary m_candidates/m_pendingWord compose-box state (or, if
// a ')' just closed a nested group, in ScriptEditor::m_exprPendingGroup) until an operator, a ')', or the final
// confirm consumes it into `terms`/`ops`. `ops.size() == terms.size()` means "awaiting the next term" (a
// dangling trailing operator, or -- when both are empty -- nothing typed yet); `ops.size() == terms.size() - 1`
// never persists as a resting state (every action that completes a term also either adds a dangling operator or
// closes/pops the frame), so callers can treat the `ops.size() == terms.size()` check as the sole invariant.
struct ExprFrame
{
	std::vector<PendingExprTerm> terms;
	std::vector<DSLOperator> ops;
};

// One resolved term of a LOGICAL (&&/||) chain being staged through the Condition* stages -- either a full
// comparison ("i + 2 > n * 2") or a bare bool value ("canJump()"), each side itself a compound chain. Purely
// transient, like PendingExprTerm: nothing is a real DSLSymbol until the whole condition/value commits
// (ScriptEditor::buildStagedBool).
struct PendingLogicalTerm
{
	bool isComparison = false;
	PendingExprChain left;                       // the bare bool value when !isComparison
	DSLOperator comparator = DSLOperator::Equal; // meaningful if isComparison
	PendingExprChain right;                      // meaningful if isComparison
};

export class ScriptEditor
{
public:

	void render();
	void handleKeyEvent(const SDL_Event& evt);
	// True while this panel (or any of its children) holds ImGui focus -- the DSL text area handles raw SDL
	// key events itself rather than through a normal ImGui widget (see the class comment), so ImGui's own
	// WantCaptureKeyboard/WantTextInput never reflect it; callers that gate global keyboard shortcuts (e.g.
	// InputControls' demo key bindings) need this instead, or typing a digit here (a vector literal's
	// components, "1.0,2.0,3.0") also fires whatever gameplay action that key is bound to.
	bool hasFocus() const { return m_hasFocus; }

	// Script paths a save just wrote fresh generated C++ into (see saveDocument) -- UI::takeScriptReloadRequests
	// merges these in with the node editor's own "Compile & Run" queue, so main.cpp's drain loop
	// (Globals::scriptHost.getOrLoad(path, true)) picks either kind up the same way.
	std::vector<std::string> takeReloadRequests()
	{
		std::vector<std::string> requests = std::move(m_pendingReloadRequests);
		m_pendingReloadRequests.clear();
		return requests;
	}

	// Makes `path` the active document -- the ONLY way one opens: used by UI.cpp to follow the Scene hierarchy
	// selection onto a .dsl-scripted entity (mirroring the node editor's requestOpenScript) and to route the
	// asset browser's double-click/drag actions for .dsl files here instead of into the node editor. No
	// unsaved-changes prompt: unlike the node graph, this editor has no separate dirty flag to guard (every
	// edit already lives in m_document; Save is the only thing that ever touches disk). No-op if `path` is
	// empty or already the open document.
	void requestOpen(const std::string& path);

private:

	void renderTextArea();
	void renderAutocompletePopup();
	void saveDocument(); // sidebar Save / Ctrl+S: writes m_document to its OWN filePath (ScriptLoader::save).
	                     // Safe mid-compose -- a compose never touches the document, so exactly the committed
	                     // state saves. A warning when nothing is open (there is no path field to name one).
	// Replaces the document from `path` (ScriptLoader::load) -- requestOpen's engine, and the sidebar Reload's
	// (which passes the open document's own path to re-read it from disk). On success every selection/compose
	// state resets (all old symbol pointers are dead); on failure nothing changes.
	void loadDocument(const std::string& path);
	// Ctrl+C / Ctrl+V. Copy takes the cursor's LINE -- or, on a block header (or its synthetic `end`), the
	// whole block -- as plain expanded-view DSL text, tab-indented and rebased to column 0, so it also reads
	// correctly pasted into any outside text editor. Paste splices the clipboard below the cursor (into a
	// header's body when the cursor sits on one, same rule as Enter) and REPARSES the whole candidate document
	// through ScriptLoader::loadFromText: anything that doesn't resolve there -- an unknown name, bad nesting,
	// a fragment missing its `end` -- rejects the paste with the loader's own error and leaves the document
	// untouched. That loader gate is what keeps "everything always compiles" true for text from anywhere.
	void copySelection();
	void pasteClipboard();
	DSLSymbol* pushSymbol(DSLCodeLine& line, DSLSymbol::SymbolType type, DSLSymbol::Data data); // constructs a fresh symbol owned by `line`, returns the raw ptr -- shared by every apply/commit function below

	void moveHorizontal(int delta);  // Left/Right, Tab/Shift-Tab (not composing): next/prev span
	void moveVertical(int delta);    // Up/Down (not composing): the adjacent line's LAST span (end-of-line, like deleteLine's landing)
	void moveHome();
	void moveEnd();
	void clampCursor();
	void insertLineAfterCursor(); // Enter (not composing): a fresh blank statement slot after the current line
	void deleteLine(DSLCodeLine& line); // Backspace on a blank statement placeholder: remove it outright
	void deleteBlockKeepBody(DSLSymbol* headSymbol); // Backspace on an if/while keyword: remove it, keep+unindent its body
	bool attachedElseChainEmpty(const DSLSymbol* headSymbol) const; // no elseif/else chain, or every branch in it is empty
	bool isBlockBodyEmpty(const DSLSymbol* headSymbol) const; // body is nothing, or exactly one blank statement placeholder -- any block header
	// The function-deletion guard's body check: empty (isBlockBodyEmpty), or exactly one `return` line -- the
	// return a return type auto-seeds shouldn't pin an otherwise-empty function in place.
	bool isFunctionBodyDeletable(const DSLSymbol* headSymbol) const;
	// A non-Void function's BOTTOM return -- the `return` sitting as the last body-level line before `end` --
	// can never be deleted on its own (its VALUE stays freely editable; removing the whole function, or
	// clearing/changing the return type, are the sanctioned ways it goes). Earlier/nested returns stay free.
	bool isProtectedBottomReturn(const DSLCodeLine* line) const;
	// A return-type CHANGE rewrites the function's returns: every existing `return` line is removed (their
	// values were typed against the old type), and a non-Void new type seeds a fresh `return |` compose at the
	// body's end (m_pendingComposeReturnValue) -- a function with a return type always gets nudged to return
	// something. Void sets no landing; callers keep their own.
	void applyFunctionReturnChange(DSLSymbol* funcSymbol, DSLType newReturnType);
	void deleteEmptyBlock(DSLSymbol* headSymbol); // Backspace on an EMPTY block header (function or for): remove header+body+end
	DSLSymbol* referenceHighlightTarget(DSLSymbol* symbol) const; // -> the VariableDeclaration/FunctionDeclaration `symbol` refers to (itself, if it already is one), or nullptr

	// The EXPORTS sidebar toggles: one checkbox per ScriptBindings::entryPoints() entry (OnSpawn/OnDestroy/
	// Update/OnEvent/OnPhysicsEvent), checked = a matching top-level function currently exists. Checking one ON
	// seeds the function (header, its LOCKED dslParams, one blank body line) and appends it to the document;
	// checking one OFF removes it -- refused (Log::warning, checkbox stays effectively on) while it has a real
	// body or is still called, same guard/message convention as un-requiring a component still in use.
	DSLSymbol* findEntryFunctionHead(const char* name) const; // scopeLevel-0 FunctionDeclaration named `name`, or nullptr
	void toggleEntryFunction(const EntryPointDef& def, bool enable);
	// Whether `funcDecl` IS one of the 5 entry points (by name) -- gates every header-mutation guard below.
	// Deliberately name-only (no separate "owned by a toggle" flag to track): reaching this identity at all
	// means it must have come from toggleEntryFunction, since isEntryPointName blocks these names from the
	// ordinary declare-function/rename flows (see confirmCompose's FunctionDeclareName/Rename handling).
	bool isLockedEntryFunction(const DSLSymbol* funcDecl) const;
	bool isEntryPointName(const std::string& name) const;

	// FilterCandidates: normal single-step slot fill. DeclareName/DeclareValue: declaring a brand-new variable.
	// ConditionLeft/ConditionOp/ConditionRight: building an if/while condition. Rename: selecting an EXISTING
	// VariableDeclaration's own name (its declaration site, not a use of it) always free-types a replacement
	// name instead of going through slot-based candidate replacement -- a declaration's name isn't a value with
	// candidates, and AutoCompleteRules can't invent a new identifier anyway. m_composePrefix holds whatever's
	// already been resolved-but-not-yet-applied ("float ", then "float test = "; or "if height <= "); m_pendingWord
	// is always the LIVE-typed suffix after it -- the compose box shows their concatenation.
	enum class ComposeMode
	{
		None, FilterCandidates, DeclareName, DeclareValue, Rename, ConditionLeft, ConditionOp, ConditionRight,
		FunctionDeclareName, FunctionParamType, FunctionParamName, FunctionReturnType,
		ForVarType, ForVarName, ForVarValue, ForConditionOp, ForConditionValue, ForIncrementOp, ForIncrementValue,
		ReassignValue,
		EditExpr,        // in-place expression editing: replacing one value-slot occupant/chain operand, or
		                 // inserting new operator+term segments after one -- see the class comment
		ReplaceOperator, // swapping one operator of an existing Expression chain for another of the same class
		ReturnValue,     // a value-returning function's `return`: the value composes BEFORE the line commits
		CallArgValue,    // a parameterized call statement: arguments compose one by one before the call commits
		ForConditionLeft, // the for-condition's LEFT side -- seeded with the loop variable, extendable into a
		                  // compound chain ("i + 2 < ..."); a comparator advances to ForConditionOp
		ReassignOp,      // picking a reassignment statement's operator (=, +=, -=, *=, /=, %=) -- entered by
		                 // typing an operator character over a matched Reassign candidate; Space-confirming the
		                 // candidate instead skips this stage and authors a plain `=`
		CommentText,     // free-typing a comment line's text ("# ..."): every printable character -- Space
		                 // included -- is content; Enter commits, Backspace past empty deletes/cancels
		MemberSelect,    // dotted into a sidebar binding object ("physics." / "self."): picking from its
		                 // registry functions/members (AutoCompleteRules::receiverCandidates) -- a function
		                 // flows into CallArgValue staging with the receiver riding along (m_callReceiver), a
		                 // member resolves to a MemberAccess term; Backspace-empty steps back to the mode the
		                 // '.' was typed in (m_memberReturnMode) with the object's name restored
		FunctionDeclareDone,       // the ')' just closed a declared function's parameter list: Enter commits
		                           // it, '-' opens the return-type pick, Backspace reopens the parameter list
		FunctionDeclareReturnType, // picking the "-> type" for the function being declared; confirm commits the
		                           // whole function WITH it (distinct from FunctionReturnType, which edits an
		                           // already-committed header's return-type span directly)
		// "foreach [ref] <type> <name> in <sequence>", staged like every other flow header -- nothing lands in
		// the document until the sequence resolves. ForEachSource is a chain compose whose candidates are
		// filtered to values yielding the picked element type (see sequenceYields). "ref" is offered in the TYPE
		// stage as a modifier, exactly like a function parameter's (see paramTypeStagePrefix).
		ForEachType, ForEachName, ForEachSource,
		// "ifexist [ref] <type> <name> in <source> [at <key>]" -- foreach's shape plus a key stage, skipped for
		// a source whose lookup takes none (an optional, or a COMPONENT, which is fetched off an entity). The
		// DSL's only read of something that may not be there.
		IfExistType, IfExistName, IfExistSource, IfExistKey,
	};

	// Editing.
	void beginCompose();
	void refreshCandidates();
	void cancelCompose();
	// `allowCommit` false (Space) advances STAGES within a line but never finishes/commits it -- only Enter
	// (or an explicit closing gesture like a call's ')') passes true. This is what lets a space be typed
	// harmlessly before "&&"/an operator without accidentally committing the line so far.
	void confirmCompose(bool allowCommit = true);
	const Candidate* selectedCandidate() const; // the currently-highlighted candidate, or nullptr when the list is empty
	// Space on a TERMINAL compose stage (the value/argument IS the whole line -- only Enter/')' finishes it, so
	// Space would otherwise do nothing at all): completes the box to the currently-matched candidate's full
	// text instead, mirroring the completion every OTHER stage's Space already performs as part of advancing.
	// Parameterized functions are excluded -- those resolve through tryBeginValueCallStaging instead (typed
	// text required there too, "the highlighted default must never resolve off a stray key"). Returns true if
	// it consumed the keystroke (something was completed); false = nothing to complete, caller's plain
	// `if (!allowCommit) return;` behavior is unchanged either way.
	bool tryCompleteCandidateOnSpace();
	// Space on the SAME terminal stages as tryCompleteCandidateOnSpace, but for when there's nothing left to
	// TYPE toward the highlighted candidate (nothing typed at all -- relying purely on the default/arrow-
	// navigated selection -- or it's already spelled out in full): Space must still DO something with it
	// instead of silently eating the keystroke, so this resolves it as a term right into the chain (the exact
	// rule exprTryFinalize's own "awaiting a term" branch uses) and keeps composing -- an operator, or a later
	// confirm, still finishes the value, same as MemberSelect's own value-context delivery. Tries
	// tryCompleteCandidateOnSpace first, so partially-typed text still completes instead of resolving early.
	// True when the keystroke DID something (completed a partially-typed word, or consumed the highlighted
	// candidate into the chain as a resolved term) -- a stage that has somewhere to advance to uses that to keep
	// one keystroke doing exactly one thing: resolve now, advance on the next.
	bool tryResolveCandidateOnSpace();
	// The one-stop stage transition every staged flow uses: sets the compose box to `prefix` (+ `pendingWord`,
	// for step-backs that restore previously-typed text), switches to `mode`, and rebuilds the candidate list
	// against the new mode/text (a no-op for free-typing modes, which end up with an empty list).
	void enterCompose(ComposeMode mode, std::string prefix, std::string pendingWord = {});
	bool hasCandidateList() const; // does the CURRENT compose mode show an arrow-cyclable candidate list
	void applyCandidate(const Candidate& candidate);
	DSLCodeLine* currentLineHeadOrCancel(); // resolves the LineHead slot's line at the cursor, or cancelCompose()+nullptr if not there
	DSLSymbol* buildValueFromCandidate(const Candidate& candidate, DSLCodeLine& line); // Variable/Function/Literal/true/false only
	// Resolves an initializer/assignment's right-hand value: if `terms` is non-empty, builds the full expression
	// chain from it (buildExpressionFromTerms); an empty `terms` falls back to a type-appropriate Placeholder
	// (defensive -- the confirms refuse empty values, so it shouldn't occur in practice). Shared by
	// applyDeclareVariable, the reassign/redeclare commits, and commitForStatement's loop-variable init.
	DSLSymbol* resolveValueOrPlaceholder(DSLType type,
		const std::vector<PendingExprTerm>& terms, const std::vector<DSLOperator>& ops, DSLCodeLine& line);
	DSLSymbol* buildExpressionTerm(const PendingExprTerm& term, DSLCodeLine& line); // a plain candidate, or (isGroup) a nested `grouped` chain via buildExpressionFromTerms
	// Builds a flat term/operator sequence into a real Expression chain VERBATIM -- no precedence folding
	// (DSLSymbol::Expression stores exactly what was authored; the transpiler applies precedence at emit time,
	// see DSL.ixx); parenthesized groups become nested Expressions with `grouped` set. `terms`/`ops` come from
	// a fully-finalized ExprFrame (ops.size() == terms.size() - 1), never a still-composing one. `terms` must
	// be non-empty; a single plain term returns unwrapped (no Expression node), while a single GROUP keeps its
	// authored parens -- they're the edit anchor later "chain after the parens" edits hang off, never stripped
	// as redundant.
	DSLSymbol* buildExpressionFromTerms(const std::vector<PendingExprTerm>& terms, const std::vector<DSLOperator>& ops, DSLCodeLine& line);
	// True if the compound expression can be finalized right now (every paren closed, and either nothing was
	// ever typed or the trailing term is fully resolved) -- fills `outTerms`/`outOps` and consumes any pending
	// group/candidate into them. False means "keep composing", e.g. an open paren or a dangling operator with
	// nothing typed for its right-hand term yet.
	bool exprTryFinalize(std::vector<PendingExprTerm>& outTerms, std::vector<DSLOperator>& outOps);
	std::string exprBasePrefix() const; // "type name = " (DeclareValue) or "name = " (ReassignValue) -- everything before the expression itself
	std::string exprBasePrefixFor(ComposeMode mode) const; // exprBasePrefix's worker, for a mode OTHER than the current one (the comparison-value flow's stages)
	// exprBasePrefixFor(mode) + exprComposePrefixFromStack(), generalized for a possibly-CallArgValue `mode`
	// (which has no exprBasePrefixFor case of its own -- a call's own lead-in text is per-stage cached in
	// CallStage::outerLeadText instead, via callStagePrefix()). `mode` is whichever compose is CURRENTLY
	// suspended in the live m_exprStack/m_callStack.back() at the call site -- callers capture this BEFORE
	// saving/resetting either, so the live state still reflects it.
	std::string chainLeadTextFor(ComposeMode mode) const;
	std::string conditionFlowBasePrefix() const; // what precedes the condition being staged: the flow keyword ("if "), or the value mode's own base ("bool b = ")
	// The declaration being built/re-edited through the CURRENT comparison/logical-chain handoff
	// (m_conditionValueReturnMode), if any -- excluded from ConditionLeft/Right's own candidate lists so
	// "bool test = ... && test" can never reference the not-yet-existing symbol it's building. A REASSIGNMENT's
	// own target is deliberately NOT covered here (returns nullptr for ReassignValue) -- "b = b && ready"
	// (reading the variable's OLD value before it's overwritten) is legitimate, unlike a fresh/re-declared
	// initializer, which has no prior value to read.
	DSLSymbol* conditionValueExcludeVariable() const;
	// The declaration being built/re-edited that a staged call's OWN argument list must never reference back --
	// same self-reference concern as conditionValueExcludeVariable, applied to CallArgValue's candidate list.
	// Composing "int test = testFunc(|" for the FIRST time never shows `test` regardless (it doesn't exist as a
	// symbol yet, brand-new declares never write one early), so this only matters when WIDENING back into an
	// already-committed call argument (tryWidenValueCallEdit/tryWidenCallStatementEdit set m_redeclareTarget/
	// m_editSlot exactly like a plain whole-value Backspace widen does) -- checked directly rather than switched
	// on m_callStack.back().returnMode so it still holds at any nesting depth (an inner staged call's own
	// returnMode is its immediate CallArgValue parent, not the outermost DeclareValue/EditExpr, but
	// m_redeclareTarget/m_editSlot stay set across the whole nested chain).
	DSLSymbol* callArgExcludeVariable() const;
	void commitBoolValue(const PendingLogicalTerm& finalTerm); // the staged comparison/logical chain IS a value ("bool b = i < 5") -- routes per m_conditionValueReturnMode
	std::string exprComposePrefixFromStack() const; // renders m_exprStack (+ m_exprPendingGroup, if any) back to text -- always assigned to m_composePrefix after any state change, forward or backward, so the two can never drift apart
	std::string exprTermText(const PendingExprTerm& term) const; // recursive -- "(" + ... + ")" for a group, else candidateDisplayText
	void applyDeclareVariable(const std::string& name, DSLType type, const std::vector<PendingExprTerm>& terms,
		const std::vector<DSLOperator>& ops, DSLCodeLine& line);
	// DeclareValue's confirm when m_redeclareTarget is set: applies the (possibly renamed) name + freshly
	// composed initializer IN PLACE on the existing declaration -- see m_redeclareTarget's comment.
	void commitRedeclare(const std::vector<PendingExprTerm>& terms, const std::vector<DSLOperator>& ops);
	// ConditionRight's (or a bare-bool ConditionLeft's) confirm: commits the whole if/while header (or a
	// chain's new elseif) -- the condition is `finalTerm` appended to whatever m_logicalTerms/m_logicalOps
	// accumulated ("if i > 0 && i != 42"), built via buildStagedBool.
	void applyConditionalStatement(const PendingLogicalTerm& finalTerm);
	// The staged condition/bool-value as ONE value symbol: a single comparison/bare value, or a flat logical
	// chain of them (`terms`+`finalTerm` joined by `ops`). Captured pre-cancel by every caller.
	DSLSymbol* buildStagedBool(const std::vector<PendingLogicalTerm>& terms, const std::vector<DSLOperator>& ops,
		const PendingLogicalTerm& finalTerm, DSLCodeLine& line);
	std::string stagedConditionPrefix() const; // conditionFlowBasePrefix() + every accumulated logical term + its trailing &&/||

	// Chain-stage plumbing (the staged flows' value slots ARE m_exprStack composes -- see PendingExprChain):
	std::string chainDisplayText(const PendingExprChain& chain) const; // terms joined by their operator characters
	// Consumes the live compose (stack + matched candidate / pending group) into `out` -- false keeps composing
	// (open paren, dangling operator, or nothing typed). Same finalize rules as a declaration's value.
	bool captureComposedChain(PendingExprChain& out);
	void restoreChainIntoCompose(const PendingExprChain& chain); // the reverse: all but the last term into the stack, the last back into the box
	// One-stop entry into any chain-composing stage: resets the stack, optionally restores a prior chain, and
	// derives the prefix from exprBasePrefixFor(mode) so forward/backward transitions can't drift.
	void enterChainStage(ComposeMode mode, const PendingExprChain* restore = nullptr);
	DSLType composedChainPeekType() const; // the live compose's element type WITHOUT consuming it (first stack term, else the matched candidate)
	bool isChainComposeMode() const; // the current mode composes a value chain through m_exprStack
	PendingExprChain loopVarSeedChain() const; // a one-term chain holding the staged loop variable's SENTINEL candidate (see m_forBuildLoopVar)
	void applyElseStatement(DSLSymbol* chainHead, DSLCodeLine& originLine); // "else" picked inside a branch: appends the else at the chain's end, consuming the blank origin line
	DSLCodeLine& insertLineAfter(DSLCodeLine& afterLine, int scopeLevel);
	DSLSymbol* seedStatementPlaceholder(DSLCodeLine& line);
	std::string functionDeclarePrefix() const; // rebuilds "function name(type0 name0, type1 name1" from pending state
	// The FunctionParamType stage's lead-in: functionDeclarePrefix() + this parameter's own "ref " once picked.
	// currentParamPrefix() is this plus the resolved type -- i.e. the NAME stage's lead-in.
	std::string paramTypeStagePrefix() const;
	std::string currentParamPrefix() const;    // functionDeclarePrefix() + the CURRENT parameter's "[, ][ref ]type " lead-in
	bool isPendingParamNameTaken(const std::string& name) const;
	void commitFunctionDeclaration(); // FunctionParamType/Name's ')'-or-confirm: commits the whole new function declaration
	// Backspace on a function's (blank) return-type span: the staged Declare-function flow re-opens over the
	// EXISTING header at its last parameter's name (or the function name, when parameter-less), whole-line box
	// -- further Backspace peels parameter by parameter, then the name, then (guarded) the function itself.
	void beginWidenFunctionHeader(DSLSymbol* funcSymbol);
	// commitFunctionDeclaration when m_flowEditLine is set: re-applies the header IN PLACE -- the
	// FunctionDeclaration symbol survives (call sites reference it), and parameters keep their symbols
	// positionally wherever the type is unchanged (body statements and named call arguments reference them).
	// REFUSES (keeps composing) when the signature changed while the function is called anywhere, or when a
	// removed/retyped parameter is still referenced in the body -- nothing may dangle.
	void commitFunctionRedeclare();
	// Backspacing out of an empty argument-value edit on a committed call STATEMENT widens into the staged
	// CallArgValue flow at that argument (earlier arguments restored); false = not such a position/not
	// restorable, caller falls through.
	bool tryWidenCallStatementEdit();
	// The same argument-widening for a call VALUE -- a constructor/call occupying a declaration's whole
	// initializer ("vec3 test = vec3(1,|") or an assignment's whole right-hand side: the CallArgValue staging
	// re-opens at that argument WITH the owning flow's context (redeclare / reassign-edit) as the suspended
	// return mode, so continued Backspace peels through the call, then the value stage, the name, and beyond.
	// Calls nest to any depth ("float t = myvec.dot(vec3(1, 2, |"): the walk outward rebuilds ONE CallStage per
	// level, the same stack forward authoring would have pushed, so the peel out mirrors the way in.
	bool tryWidenValueCallEdit();
	std::string callComposePrefix() const;  // "name(param0 = <resolved>, param1 = " -- rebuilt from the staged-call state each stage
	std::string callStagePrefix() const;    // callComposePrefix, led by the suspended chain compose's own prefix when staging a call VALUE
	DSLType currentCallParamType() const;   // the parameter the CallArgValue stage is currently composing a value for
	void commitCallStatement();             // the last argument's confirm: commits the call line -- or returns the resolved call TERM to the suspended chain
	// A matched parameterized-Function candidate in a chain compose can't be consumed bare (its call needs
	// arguments, and placeholders never land) -- '(', an operator, or a confirm over it opens the CallArgValue
	// sub-flow instead. False = not such a candidate; the caller proceeds normally. `requireTypedText` guards
	// against a stray '('/operator keystroke resolving off whatever's default-highlighted with nothing typed --
	// confirmCompose's Space/Enter path passes false instead: a confirm is itself a deliberate "use the
	// highlighted entry" gesture (see tryResolveCandidateOnSpace's identical reasoning for a non-parameterized
	// pick), unlike '('/an operator, which could equally mean "start a new group" or "continue the expression".
	bool tryBeginValueCallStaging(bool requireTypedText = true);
	// Dotting into a matched BindingObject/struct-variable candidate ('.', or a confirm over it): captures the
	// current mode as the return context and opens the receiver's member/function list (ComposeMode::MemberSelect).
	void enterMemberSelect(DSLSymbol* receiverDecl);
	void restoreMemberPath(const std::string& dottedPath); // re-applies a dotted chain onto a fresh MemberSelect
	// One member-chain hop's resolved type (Void = no such member) -- self.data's OWN fields come from
	// m_document.dataFields (per-document, see DSLType::ScriptData), everything else from the static
	// ScriptBindings registry (m_bindings.findMember). The ONE lookup every chain-walking site shares
	// (restoreMemberPath, buildReceiverChain, the MemberSelect Backspace-peel walk, reassignTargetType).
	DSLType resolveMemberType(DSLType receiverType, const std::string& name) const;
	// Whether the member chain CURRENTLY walked in MemberSelect (m_memberReceiver + m_memberPath) is assignable
	// through -- false as soon as any hop is a read-only member, since everything reachable through one is
	// read-only too (`self.parent.pos = ...` would mutate an object the script only has read access to). What
	// receiverCandidates' `receiverWritable` gets, so a member under such a path is never offered as an
	// assignment lead-in (and its Candidate::memberWritable comes back false, which is what confirmCompose's
	// statement-context branch refuses on). self.data is the deliberate exception -- see resolveMemberType's
	// own ScriptData note and receiverCandidates.
	bool isMemberPathWritable() const;
	// The value constraints of the CURRENT compose stage -- what a MemberSelect entered from it filters
	// receiver candidates by (Void + !anyValue = statement context). Mirrors refreshCandidates' per-mode logic.
	DSLType valueContextExpectedType(ComposeMode mode, bool& outAnyValue) const;
	// Whether any statement in the document dots into a component-bound member of the given type (self.physics
	// and friends) -- guards un-requiring a component that's still in use.
	bool isComponentMemberReferenced(DSLType memberType) const;
	void renderSidebarPanel(); // the Entity/Engine bindings browser + required-component checkboxes
	// Whether any statement dots into self.data.<name> -- guards removing a SCRIPT DATA field still in use, the
	// per-document twin of isComponentMemberReferenced.
	bool isDataFieldReferenced(const std::string& name) const;
	// Same guard for self.events.<name> -- a named On Event entry still in use.
	bool isEventReferenced(const std::string& name) const;
	// The receiver expression a dotted path names: the root's VariableReference, wrapped in one MemberAccess
	// per path segment ("pos.x" -> self->pos->x), each hop's type stamped from the registry. Empty path = just
	// the reference. Shared by member values, member-assign targets, and dot-call receivers.
	DSLSymbol* buildReceiverChain(DSLSymbol* rootDecl, const std::string& dottedPath, DSLCodeLine& line);
	DSLSymbol* buildCallFromStagedArgs(DSLSymbol* funcSymbol, DSLSymbol* receiverDecl, const std::string& receiverPath,
		const std::vector<PendingExprChain>& argChains, DSLCodeLine& line); // shared by commitCallStatement and call-term builds
	void restoreTermIntoBox(PendingExprTerm&& term); // back into the live compose: groups/resolved calls as the pending term, plain candidates as typed text
	// A restored term that IS a member access reopens in MemberSelect (the stage that authored it) rather than
	// as an inert resolved term: its last path segment becomes the typed word, the rest the receiver path. Only
	// that state can be continued -- '.' needs typed text plus a live candidate list, and MemberSelect's own
	// Backspace ladder is what peels a chain apart segment by segment. False = not a member term; the caller
	// falls back to restoreTermIntoBox. Switches m_composeMode, which is why enterChainStage re-checks it.
	bool tryRestoreMemberTermIntoMemberSelect(const PendingExprTerm& term);
	std::string forVarPrefix() const;       // "for <type> <name> = " -- before the loop var's own initial value
	std::string forVarDeclPrefix() const;   // forVarPrefix() + the resolved initial value -- the whole loop-var clause
	std::string forConditionPrefix() const; // forVarDeclPrefix() + ", <name> <op> <value>" -- the whole condition clause too
	void commitForStatement(); // ForIncrementValue's confirm: commits the whole new for-loop
	// The TYPE stages' lead-ins: the keyword plus this binding's own "ref " once picked (see paramTypeStagePrefix).
	std::string forEachTypeStagePrefix() const;
	std::string ifExistTypeStagePrefix() const;
	std::string forEachPrefix() const; // "foreach [ref] <type> <name> in " -- everything before the sequence
	void commitForEachStatement(const PendingExprChain& sequence); // ForEachSource's confirm: commits the whole loop
	// Whether `sequenceType` iterates as `elementType` -- a registered BindingSequence (self.scene) or an array
	// of it. What ForEachSource filters its candidates by, so only iterable values of the right element type
	// are offered in the first place.
	bool sequenceYields(DSLType sequenceType, DSLType elementType) const;
	std::string ifExistPrefix() const;    // "ifexist [ref] <type> <name> in " -- everything before the container
	std::string ifExistKeyPrefix() const; // ...plus the resolved container and " at "
	// The container the staged flow has resolved so far (null until IfExistSource confirms) -- what the key
	// stage reads lookupKeyType off, and what decides whether there IS a key stage at all.
	const BindingObject* ifExistContainer() const;
	void commitIfExistStatement(const PendingExprChain& key); // IfExistSource/Key's confirm
	// The re-edit half of the commits above (foreach/ifexist headers are structurally identical): applies a
	// new bound name + source to an EXISTING header, preserving the bound declaration's symbol identity.
	void rebuildBoundHeaderSource(DSLCodeLine& line, DSLSymbol* boundVar, const std::string& boundName,
		bool boundIsRef, const PendingExprChain& source, const PendingExprChain* key = nullptr);
	DSLType reassignTargetType() const; // m_reassignTarget's own declared type
	// Same `terms`/`ops` convention as applyDeclareVariable, for a `name = value` statement instead.
	void commitReassignStatement(const std::vector<PendingExprTerm>& terms, const std::vector<DSLOperator>& ops);
	// ReassignValue's confirm when m_reassignEditExpr is set: swaps only the right-hand value in place.
	void commitReassignInPlace(const std::vector<PendingExprTerm>& terms, const std::vector<DSLOperator>& ops);
	void clearLineToBlankStatement(DSLCodeLine& line); // the staged flows' final Backspace: line becomes a selected blank statement placeholder

	// In-place expression editing (EditExpr/ReplaceOperator -- see the class comment).
	void beginEditExprReplace(const SyntaxSpan& span); // compose a replacement for an existing value-slot occupant / chain operand
	void beginEditExprInsert(const SyntaxSpan& span, DSLOperator leadOp, const std::string& anchorText); // an operator typed over a selected value: compose the term(s) to chain in after it
	// Backspace on a committed group's ')' span: reopens the group for re-composing -- its contents seed the
	// expression stack exactly as they were mid-authoring right before the ')' was typed (paren open, last term
	// restored into the box), so the whole group must be re-closed and re-confirmed as one unit. False if any
	// inner term can't round-trip into compose form (a Placeholder, or a call whose arguments can't re-stage) --
	// caller falls back to plain replace-editing.
	bool beginReopenGroup(const SyntaxSpan& span);
	void computeComposeCover(const SyntaxSpan& groupCloseSpan); // the reopened group's full rendered column range -- what the compose box replaces
	// Backspacing out of an empty in-place edit on a flow-control header's value span widens into the matching
	// STAGED flow (if/elseif/while condition -> ConditionLeft/Right, return -> ReturnValue, for clauses -> the
	// matching For* stage, with the whole staged state restored from the committed clauses). False = not such a
	// position, or content that can't round-trip into staged form (compound sides, member accesses) -- caller
	// falls through to the other step-back behaviors.
	bool tryWidenFlowHeaderEdit();
	// Same widening for a COMPARISON that is a bool VALUE ("bool test = i < 5"'s operands): re-enters the
	// comparison-value staged flow (ConditionLeft/Right with m_conditionValueReturnMode) with the owning
	// declaration's/assignment's re-edit context restored, so the peel continues out through "bool test = |"
	// and beyond exactly like every other value.
	bool tryWidenComparisonValueEdit();
	// EditExpr's confirm: splice/wrap/replace per m_edit* state.
	void applyEditExpr(const std::vector<PendingExprTerm>& terms, const std::vector<DSLOperator>& ops);
	void writeSlot(const SlotRef& slot, DSLSymbol* newSymbol); // repoints the slot's owning field at newSymbol (never LineHead)
	void repointSymbol(DSLCodeLine& line, DSLSymbol* oldSymbol, DSLSymbol* newSymbol); // every structural field in `line` pointing at oldSymbol -> newSymbol (chain unwrap)
	// After any in-line structural edit: moves `originalHead` back to symbols.back() (the post-order convention,
	// see DSL.ixx) and sweeps symbols no longer reachable from it -- replaced operands/operators must not linger
	// in the line's flat ownership list, where isVariableReferenced-style scans would still count them as uses.
	void restoreHeadAndCollect(DSLCodeLine& line, DSLSymbol* originalHead);
	void deleteChainOperand(DSLSymbol* exprSymbol, int operandIndex);   // Backspace on a chain term: remove it + its adjacent arithmetic operator
	void deleteChainOperator(DSLSymbol* exprSymbol, int operatorIndex); // Backspace on a chain operator: remove it + the operand AFTER it
	// Lands the cursor on the END of a just-committed value -- a grouped chain's own ')' span, else an
	// ungrouped chain's last operand (recursively), else the value itself -- matching where typing left off.
	void selectExpressionTail(DSLSymbol* value);
	void finishChainShrink(DSLSymbol* exprSymbol, DSLSymbol* originalHead, int selectOperand); // shared tail: unwrap a 1-operand chain, restore head order, GC, land the cursor

	DSL m_document;
	ScriptBindings& m_bindings = Globals::scriptBindings; // THE engine-exposure registry (global -- dslTypeName
	                           // and the loader/transpiler consult it too); builds m_document.sidebar +
	                           // m_builtins once (stable symbol identity), answers receiver/member/emit lookups
	std::vector<std::unique_ptr<DSLSymbol>> m_builtins; // every registry FunctionDeclaration (engine free
	                                                     // functions + requiresReceiver object functions)
	std::vector<SyntaxLine> m_formatted; // rebuilt from m_document each render() -- cheap at this document size
	bool m_compact = false;              // Syntax::format supports compact rendering; no toolbar toggle wires it up yet -- always expanded

	int m_cursorLine = 0; // index into m_formatted
	int m_cursorSpan = 0; // index into m_formatted[m_cursorLine].spans

	// Set by applyCandidate/applyDeclareVariable/applyConditionalStatement to whatever should be selected once
	// m_formatted is recomputed against the just-mutated document (resolved by symbol-pointer scan in render()).
	// m_pendingSelectOperatorIndex additionally picks WHICH span of that symbol when it's an Expression with
	// several operator spans (>= 0 after replacing an operator; -1 = first span wins, the common case), and
	// m_pendingSelectGroupClose targets a grouped Expression's own closing-')' span instead (typing ')' on a
	// group's last operand steps the selection out there -- see handleKeyEvent).
	DSLSymbol* m_pendingSelectTarget = nullptr;
	int m_pendingSelectOperatorIndex = -1;
	bool m_pendingSelectGroupClose = false;
	// Set alongside m_pendingSelectTarget when the selected-to line is a freshly seeded RETURN slot: once the
	// re-format lands the cursor there, the ReturnValue compose opens immediately ("return |") -- see
	// applyFunctionReturnChange. Escape leaves the blank statement line.
	bool m_pendingComposeReturnValue = false;
	// >= 0: instead of a symbol scan, select the LAST span of this formatted-line index once m_formatted is
	// recomputed -- how deleteLine lands the cursor at the END of the line above the one removed (the spot
	// typing would continue from), which a symbol-based select can't express (a line's head span is its name/
	// keyword, not its last element).
	int m_pendingSelectLineEnd = -1;

	ComposeMode m_composeMode = ComposeMode::None;
	std::string m_composePrefix;
	std::string m_pendingWord;
	std::vector<Candidate> m_candidates;
	int m_candidateSelected = 0;
	DSLType m_pendingDeclareType = DSLType::Void; // DeclareName/DeclareValue: which type was picked in step 1
	std::string m_pendingDeclareName;             // DeclareValue: the name resolved in step 2
	DSLSymbol* m_renameTarget = nullptr;           // Rename: the VariableDeclaration being renamed

	// Non-null: the DeclareName/DeclareValue staged flow is RE-authoring this EXISTING declaration instead of
	// building a brand-new one -- entered by Backspacing out of an empty whole-initializer edit (see
	// handleKeyEvent's EditExpr step-back), widening the compose box to the ENTIRE line ("float test = |",
	// exactly like a new declaration mid-flow) so continued Backspace peels further: into the name (rename),
	// then -- only when nothing references the variable -- clearing the whole line back to a blank statement.
	// Committing goes through commitRedeclare, never applyDeclareVariable: the existing VariableDeclaration
	// symbol's IDENTITY must survive (name/initializer swapped in place) so every VariableReference elsewhere
	// keeps pointing at it. Cancelling anywhere leaves the document untouched, same as every staged flow.
	DSLSymbol* m_redeclareTarget = nullptr;

	// The compound expression being composed for DeclareValue's initializer OR ReassignValue's value -- shared
	// by both (see the class comment above and ScriptEditor.cpp's expr* helpers). Reset to one empty frame
	// whenever either mode is (re-)entered. m_exprPendingGroup/m_exprHasPendingGroup hold whatever a ')' most
	// recently resolved, until an operator, an enclosing ')', or the final confirm consumes it.
	std::vector<ExprFrame> m_exprStack;
	PendingExprTerm m_exprPendingGroup;
	bool m_exprHasPendingGroup = false;

	// Reassigning an EXISTING variable (`name op value`) -- picked from the Void statement list's Reassign
	// candidates. ReassignValue stages the value (via the SAME m_exprStack mechanism as DeclareValue); nothing
	// touches the document until it confirms (commitReassignStatement). m_reassignOp is the assignment operator
	// the statement authors with -- plain `=` unless the ReassignOp stage picked a compound one ("i += 1").
	DSLSymbol* m_reassignTarget = nullptr;
	DSLOperator m_reassignOp = DSLOperator::Assign;

	// Non-null: ReassignValue is RE-authoring this EXISTING assignment statement (the line-head Expression)
	// instead of building a new line -- the assignment-statement counterpart of m_redeclareTarget, entered the
	// same way (Backspacing out of an empty right-hand-value edit widens the compose box to the whole line,
	// showing the statement's ACTUAL operator -- `thing += |`, not a hardcoded `=`). Commits through
	// commitReassignInPlace, which swaps ONLY the right-hand value on the existing statement (target and
	// operator untouched); one more Backspace from the widened dialog clears the line to a blank statement
	// (no reference guard needed -- an assignment declares nothing).
	DSLSymbol* m_reassignEditExpr = nullptr;

	// EditExpr: editing an existing expression in place. m_editSlot is where the edited value lives (captured
	// at entry; the document never changes while composing, so it can't go stale). m_editChainExpr is the
	// Expression whose operand list gets spliced (null = the slot's occupant is treated as a standalone value:
	// replaced wholesale, or -- for an insert -- wrapped into a fresh chain with itself as the first operand,
	// which is also how a comparison/assignment SIDE grows a nested sub-chain). The composed segment itself
	// lives in the same m_exprStack machinery DeclareValue/ReassignValue use, so parens/operators behave
	// identically; nothing touches the document until applyEditExpr.
	SlotRef m_editSlot;
	DSLSymbol* m_editChainExpr = nullptr;
	int m_editOperandIndex = 0;                  // operand being replaced, or the insert-after anchor's index
	bool m_editInsert = false;                   // false: replace the anchor; true: insert after it (m_editLeadOp leads the new segment)
	DSLOperator m_editLeadOp = DSLOperator::Add;
	DSLSymbol* m_editAnchorSymbol = nullptr;     // the slot's current occupant (standalone insert wraps around it)
	std::string m_editAnchorText;                // its rendered text -- the compose box shows "anchor op ..." while inserting
	DSLType m_editValueType = DSLType::Void;     // the chain's element type, constraining every composed term's candidates

	// ReplaceOperator: which operator of which Expression chain is being swapped (candidates come from the
	// CURRENT operator's own class -- arithmetic/comparison/assign -- so a `+` can become `*` but never `<=`).
	DSLSymbol* m_replaceOpExpr = nullptr;
	int m_replaceOpIndex = 0;

	// Non-null: a Condition*/ReturnValue/For* staged flow is RE-authoring this EXISTING flow-control header
	// line -- entered by Backspacing out of an empty in-place edit on one of its value spans (see
	// tryWidenFlowHeaderEdit), the header-line counterpart of m_redeclareTarget/m_reassignEditExpr. The header
	// only changes when the staged flow re-confirms IN FULL (no body is seeded -- the block already has one);
	// Backspacing past the first stage applies the same guarded deletion as Backspacing the keyword itself
	// (if/while: empty else-chain, body kept un-nested; elseif: own branch empty; for/foreach/ifexist: empty
	// body, plus an empty attached else for ifexist; return: unconditional). m_flowEditLoopVar is the
	// declaration the header BINDS (a for-loop's counter, a foreach element, an ifexist binding),
	// preserved IN PLACE on commit -- body statements reference it, so its identity must survive re-authoring;
	// its TYPE stays fixed for the same reason (the type stage is never re-entered on a re-edit).
	DSLCodeLine* m_flowEditLine = nullptr;
	DSLSymbol* m_flowEditLoopVar = nullptr;

	// >= 0: the compose box replaces this COLUMN RANGE of the cursor line instead of just the selected span --
	// set by beginReopenGroup (the box must cover the group's whole "(...)" render, not only its ')' span).
	// Stable while composing: the document doesn't change mid-compose, so the line renders identically each
	// frame. The whole-line flows (m_redeclareTarget/m_reassignEditExpr) take precedence over this in
	// renderTextArea when a step-back transitions from one into the other.
	int m_composeCoverStart = -1;
	int m_composeCoverEnd = -1;

	// One level of a (possibly NESTED) parameterized call's argument staging -- CallArgValue is a proper
	// chain-composing mode (see isChainComposeMode): each argument is a FULL chain (m_exprStack), which may
	// itself contain a call, so a call passed AS an argument to another call pushes its own CallStage here (via
	// tryBeginValueCallStaging, generic to every chain mode) instead of clobbering the enclosing one.
	struct CallStage
	{
		DSLSymbol* func = nullptr;
		DSLSymbol* receiver = nullptr;  // the receiver chain's ROOT declaration a dot-call stages against
		                                 // ("physics.applyImpulse(...)"); null = a free call
		std::string receiverPath;       // dotted member path root->call, "" for a direct dot-call ("pos" in
		                                 // `self.pos.length()`)
		std::vector<PendingExprChain> argChains; // confirmed arguments so far, one full chain each
		ComposeMode returnMode = ComposeMode::None; // None = a STATEMENT call (completion commits the line);
		                                             // else the mode THIS call's resolved value returns into
		                                             // once every argument is in hand (may itself be
		                                             // CallArgValue -- a call nested in another call's argument)
		std::string outerLeadText;              // the SUSPENDED context's own lead-in text ("bool b = ", or an
		                                         // enclosing call's own "outer(arg0 = "), captured ONCE at push
		                                         // time (see chainLeadTextFor) -- constant for this stage's whole
		                                         // lifetime; callStagePrefix() prepends it to this call's own text
		std::vector<ExprFrame> savedExprStack;  // the ENCLOSING compose's m_exprStack, frozen while THIS call's
		                                         // own arguments stage -- restored once it resolves or is abandoned
		PendingExprTerm savedPendingGroup;
		bool savedHasPendingGroup = false;
	};
	// Building a parameterized call: m_callStack.back() is always the level CURRENTLY staging (its own
	// m_exprStack composes the argument in progress); an argument placeholder never appears in the document --
	// the call only completes once EVERY argument is in hand (commitCallStatement), which pops this level and,
	// if CallStage::returnMode != None, delivers the resolved call as a PendingExprTerm into the level (or
	// chain mode) beneath, restoring ITS OWN m_exprStack exactly where it left off.
	std::vector<CallStage> m_callStack;

	// MemberSelect (dotted into a binding object or a struct-typed variable): the chain's ROOT declaration,
	// the member PATH walked so far ("pos" after `self.pos.` -- each '.' over a struct-typed member appends),
	// the type at the path's end (what the candidate list keys on), the mode the first '.' was typed in
	// (stage results return there; Backspace-empty peels the path then restores the root), and the value
	// constraints captured at entry (see valueContextExpectedType).
	DSLSymbol* m_memberReceiver = nullptr;
	std::vector<std::string> m_memberPath;
	DSLType m_memberReceiverType = DSLType::Void;
	ComposeMode m_memberReturnMode = ComposeMode::None;
	DSLType m_memberExpectedType = DSLType::Void;
	bool m_memberAnyValue = false;

	// Non-empty: the Reassign flow targets a MEMBER of m_reassignTarget instead of the variable itself
	// ("self.pos.x = ..."): the dotted path from the root declaration to the written member. Entered by
	// confirming a writable Member candidate in a STATEMENT MemberSelect; commitReassignStatement builds the
	// MemberAccess chain as the assignment's target.
	std::vector<std::string> m_reassignMemberPath;

	std::string m_pendingFunctionName;            // FunctionDeclareName's resolved name, once confirmed
	std::vector<DSLType> m_pendingParamTypes;     // accumulated parameter types, parallel to m_pendingParamNames
	std::vector<std::string> m_pendingParamNames; // accumulated parameter names, parallel to m_pendingParamTypes
	std::vector<bool> m_pendingParamRefs;         // parallel `ref` flags -- always false when AUTHORING (the flow can't
	                                               // create ref params); restored from an existing header on a re-edit
	                                               // so its display and commit round-trip them (see beginWidenFunctionHeader)
	DSLType m_pendingParamType = DSLType::Void;    // FunctionParamName: type just picked for the CURRENT (not yet named) parameter
	bool m_pendingParamRef = false;                // FunctionParamName: the CURRENT parameter's restored ref flag (false when authoring)
	DSLType m_pendingReturnType = DSLType::Void;   // the staged flow's "-> type" (FunctionDeclareReturnType); Void when
	                                                // never picked; restored from the header on a re-edit so it round-trips

	// Building a new for-loop: ForVarType/Name/Value stage the loop variable exactly like declaring a plain
	// local (type, then name, then initial value); ForConditionLeft (seeded with the loop variable, extendable
	// to "i + 2") + ForConditionOp/Value stage the condition; the increment's left stays implicitly the loop
	// variable, ForIncrementOp/Value pick the compound-assign operator + step. Every VALUE clause is a full
	// compound chain (PendingExprChain, composed through m_exprStack). Nothing touches the document until
	// ForIncrementValue's confirm (commitForStatement) -- and since the loop variable doesn't EXIST until then,
	// its own appearances inside the clauses ride as SENTINEL Variable candidates (refSymbol null, label =
	// m_forVarName), resolved to the real symbol via m_forBuildLoopVar at build time.
	DSLType m_forVarType = DSLType::Void;
	std::string m_forVarName;
	PendingExprChain m_forVarInitChain;
	PendingExprChain m_forConditionLeftChain;
	Candidate m_forConditionOpCandidate;    // keeps both .op and .label -- the label re-displays in the compose box on step-back
	PendingExprChain m_forConditionValueChain;
	Candidate m_forIncrementOpCandidate;
	PendingExprChain m_forIncrementValueChain;
	DSLSymbol* m_forBuildLoopVar = nullptr; // build-time only: what sentinel loop-var candidates resolve to

	// The staged foreach flow (ForEachType -> ForEachName -> ForEachSource, committed by commitForEachStatement).
	// Simpler than the for-loop's: the element declaration has no initializer, and the sequence is a single
	// chain compose. Nothing touches the document until the sequence resolves. The element variable can't be
	// referenced by the sequence (it isn't in scope until the body), so no sentinel candidate is needed here.
	DSLType m_forEachElementType = DSLType::Void;
	std::string m_forEachElementName;
	bool m_forEachRef = false; // the element binds by reference (write-back on assignment, see the Transpiler)

	// The staged ifexist flow (IfExistType -> IfExistName -> IfExistSource -> IfExistKey, committed by
	// commitIfExistStatement). The container resolves BEFORE the key, because the container is what declares
	// the key's type (BindingObject::lookupKeyType) -- and a container that declares none skips the key stage
	// and commits straight from the source.
	DSLType m_ifExistType = DSLType::Void;
	std::string m_ifExistName;
	bool m_ifExistRef = false;
	PendingExprChain m_ifExistContainerChain;

	// Non-null: the condition flow is authoring a NEW elseif branch for the chain this If/ElseIf head opens --
	// picked as an "elseif" statement candidate INSIDE that branch (m_conditionOriginLine is the blank line it
	// was typed on, consumed at commit). applyConditionalStatement then inserts the new header AFTER the
	// enclosing branch's block at its level, instead of replacing the cursor line. The parallel "else" pick
	// needs no staging at all (no condition) -- it applies immediately via applyElseStatement.
	DSLSymbol* m_conditionChainHeader = nullptr;
	DSLCodeLine* m_conditionOriginLine = nullptr;

	// Not None: the ConditionOp/ConditionRight stages are building a COMPARISON VALUE for that value mode
	// ("bool b = i < 5" -- entered by typing a comparator over a matched candidate in a Bool-typed
	// DeclareValue/ReassignValue/EditExpr), not a flow-control header. ConditionRight's confirm then routes to
	// commitComparisonValue, and Backspacing out of ConditionOp returns to the value mode with the lead
	// candidate restored. The value modes' own context members (m_pendingDeclare*/m_reassign*/m_edit*) stay
	// intact across the mode switch -- only cancelCompose clears them.
	ComposeMode m_conditionValueReturnMode = ComposeMode::None;

	DSLFlowControl m_conditionControl = DSLFlowControl::If; // ConditionLeft/Op/Right: If, While, or a chain's new ElseIf
	PendingExprChain m_conditionLeftChain;                  // ConditionOp/Right: the resolved (possibly compound) left side

	// The LOGICAL (&&/||) chain accumulated so far while staging a condition or bool value: typing '&'/'|'
	// after a resolved term (ConditionRight's comparison, or a bool-valued ConditionLeft pick) pushes it here
	// with its operator and loops back to ConditionLeft for the next term; the final term rides into
	// applyConditionalStatement/commitBoolValue instead. Sizes stay equal while awaiting the next term.
	// Cleared at every condition-flow entry; Backspace at an empty ConditionLeft pops one term back open.
	std::vector<PendingLogicalTerm> m_logicalTerms;
	std::vector<DSLOperator> m_logicalOps;
	DSLOperator m_conditionOp = DSLOperator::Equal;         // ConditionRight: the resolved comparator

	bool m_hasFocus = false;
	bool m_built = false;

	std::vector<std::string> m_pendingReloadRequests; // paths saveDocument just wrote, pending takeReloadRequests

	float m_sidebarWidth = 240.0f; // drag-resizable via the splitter between the sidebar and the text area

	// SCRIPT DATA sidebar section: the pending new-field controls (type combo + free-typed name), reset once
	// added. Immediate-mode ImGui state, unrelated to the main text area's ComposeMode staging.
	DSLType m_pendingDataFieldType = DSLType::Int;
	char m_pendingDataFieldName[64] = "";
	// EVENTS sidebar section: the pending new-event-name field, same idiom (no type -- an event is just a name).
	char m_pendingEventName[64] = "";

	float m_fontScale = 1.0f;
	float m_textOriginX = 0.0f, m_textOriginY = 0.0f;
	float m_cursorScreenX = 0.0f, m_cursorScreenY = 0.0f; // current selection's screen rect, for popup placement
	float m_lineHeight = 0.0f;
};
