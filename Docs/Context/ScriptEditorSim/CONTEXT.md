# ScriptEditorSim — the Script Editor with simulated keys

> Read [`.claude/CLAUDE.md`](../../../.claude/CLAUDE.md) first — rules, building, style, dependency direction.
> The editor is in [`Docs/Context/UI/CONTEXT.md`](../UI/CONTEXT.md), the language in
> [`Docs/Context/DslCompiler/CONTEXT.md`](../DslCompiler/CONTEXT.md).

The way Claude iterates on the editor's UX: the REAL panel (`Code/UI/Private/ScriptEditor/`) gets simulated key
presses and shows the result as text or as an image. No window, GPU or initialized engine system. An edit to the
editor rebuilds in about 5 s, and a run takes less than 0.1 s.

```
cmake --build Build --config Debug --target ScriptEditorSim
Build/Code/Tools/ScriptEditorSim/Debug/ScriptEditorSim.exe <options>
```

## How it is built

* **The editor's source files compile into this exe a second time.** `SimUIModule.ixx` is a one-line `export module UI`
  that stands in for `UI.ixx`, so nothing else of the UI library (which imports the renderer) compiles here. Every
  change to the editor is a change to the engine's editor: there is no copy.
* `ScriptEditor` has a small test surface (`loadText`, `documentText`, `documentLoads`, `describeState`,
  `describeStateShort`).
* Entity is linked for `registerScriptDslBindings` and never initialized, as in DslCompiler. A build of Entity's
  dependencies still counts when they changed.
* ImGui runs headless: no backend, and the frame's draw data goes through a small software rasterizer for `--png`.
  The clipboard is a private string, and `loadText` leaves the document with no path, so **a simulated Ctrl+S or Ctrl+C
  can never touch an asset or the real clipboard.**
* App's `installUnattendedFailureHandling` is on: an assert or a crash prints a symbolized stack and exits, with no dialog.

## Options

| Option | Effect |
|---|---|
| `--doc <path>` | The start document: a `.dsl`, relative to `Assets/`. It is read only. |
| `--text "<dsl>"` | The start document as raw DSL, `\n` and `\t` spelled out. Default: an empty `Update`. |
| `--keys <program>`, `--keys-file <path>` | The keys (below). |
| `--trace` | One line per key: the key, the compose mode, the cursor line and the candidates. **The usual way to see a flow.** |
| `--context <n>` | The final state shows `n` lines around the cursor. Default: the whole document. |
| `--png <path>` | The final frame as an image, relative to `Assets/` (use `Local/...`). `--size <w>x<h>` sets its size. |
| `--out <path>` | The final document as `.dsl` text, relative to `Assets/`, for DslCompiler. Its `--compile` needs a tree configured with `SCRIPTS_STATIC=OFF`; otherwise run `cl` on the output with the flags in [Script](../Script/CONTEXT.md). |
| `--save-as <path>` | Gives the document a path, relative to `Assets/` (use `Local/...`), so `{c-s}{wait-compile}` runs the whole save: the real file, the real `cl`, and the verdict in the editor. **The one option that writes where you say and runs the compiler.** |
| `--burst` | All keys in one frame: typing faster than the widget pass. |
| `--monkey <n> [--seed <s>]` | Random keys. Fails on a crash, or on a document that does not load back. The keys so far are in `Assets/Local/ScriptEditorSim-monkey.keys`. |
| `--check <path>` | The regression file: `Scenarios/script-editor-typing.txt`. **Run it after every change to the editor.** |

The state text marks the selection `[like this]` and the compose box `<<like this|>>`, with `*` on the highlighted
candidate, `status:` for a notice in the status line and `error box:` for the box at the cursor. After a run the document must load back (`ROUND TRIP FAILED` otherwise).

## Key programs

A plain character presses its key, with shift where US QWERTY needs it. `{name}` presses a named key: `{enter}` `{bs}`
`{del}` `{esc}` `{tab}` `{up}` `{down}` `{left}` `{right}` `{home}` `{end}` `{pgup}` `{pgdn}`, or a letter for a shortcut.
The modifiers are `c-`, `s-` and `a-` (`{c-z}`, `{s-tab}`, `{a-down}`), each sent as a key press of its own first, as a
keyboard does, and `{bs*5}` repeats. The mouse aims at formatted
lines: `{mdown:3}` `{mmove:7}` `{mup}` `{sclick:7}`. `{show}` prints the state now, `{png:path}` writes the image now,
`{wait-compile}` hands a save to `ScriptHost` as App's main loop does and waits for the verdict, `{{` types a `{`. Line
breaks mean nothing. In the state text, `| ` marks a selected line.

```
ScriptEditorSim --trace --keys "{down*2}float speed = 1.5{enter}{enter}if speed < 3{enter}self.pos.y += speed{enter}"
```

## The regression file

`Assets/Scenarios/script-editor-typing.txt`: `doc <raw dsl>` sets the document, and each `<keys> ==> <lines>` runs on a
fresh copy of it and requires those lines in the result, one after the other (` ;; ` between them, indentation ignored).
A line with no ` ==> ` is a comment. **It holds the rule the editor's flows must keep: code typed the way a person types
it becomes exactly that code, and a move that would not compile leaves the order alone.** The cursor starts on line 1,
because line 0 is the document's fixed blank line. Add a line for every flow that is fixed.
