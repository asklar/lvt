# MCP server

`lvt mcp` turns lvt into a [Model Context Protocol](https://modelcontextprotocol.io)
server, so an agent can inspect and drive Windows applications through the same
machinery the CLI uses.

It is served by `lvt.exe` itself over stdio — there is no separate binary to
install and no daemon to keep running.

```powershell
lvt mcp                  # inspection only
lvt mcp --allow-input    # also expose the tools that change the target app
```

## Configuring a host

```json
{
  "mcpServers": {
    "lvt": {
      "command": "C:\\tools\\lvt\\lvt.exe",
      "args": ["mcp", "--allow-input"]
    }
  }
}
```

Drop `--allow-input` for a read-only server. See [Input safety](#input-safety).

## The workflow

Every tool except `list_apps` and `connect` takes a `session`, so a conversation
always starts the same way:

1. **`list_apps`** — see what is running.
2. **`connect`** — open a session on one window. Returns a session id, plus the
   window's pid, architecture and detected UI frameworks.
3. **`get_uia_tree`** or **`find_elements`** — get element ids.
4. Act on those ids, or read them back with `get_element_properties`.
5. **`disconnect`** when finished.

Sessions are independent: an agent can hold several applications at once and
work across them.

## Tools

### Inspection — always available

| Tool | What it is for |
|---|---|
| `list_apps` | Top-level windows, with process name, pid, handle and title |
| `connect` | Open a session on a window, by `hwnd`, `pid`, `name` or `title` |
| `disconnect` | Close a session |
| `get_uia_tree` | The UI Automation tree — AutomationIds, control types, states, patterns. **This is the tree to automate against.** |
| `get_visual_tree` | The framework-native tree — Win32/XAML/WPF/WinForms/Avalonia/Chromium. Shows *how a UI is built*; cannot be used to drive it |
| `get_frameworks` | UI frameworks detected in the target, with versions |
| `find_elements` | Match by AutomationId, name, control type or supported pattern |
| `get_element_properties` | One element's properties, or a named subset |
| `hit_test` | The smallest element covering a screen point, plus its ancestors |
| `screenshot` | Annotated PNG, inline or written to a path |
| `wait_for` | Block until an element appears, reaches a property value, or disappears |

### Interaction — only with `--allow-input`

| Tool | What it is for |
|---|---|
| `click` | Invoke pattern, else default action, else a real mouse click |
| `invoke` | InvokePattern only — never moves the mouse |
| `toggle` | Flip a checkbox or toggle button |
| `set_value` | Set a text or numeric value outright |
| `set_expanded` | Expand or collapse a tree item or combo box |
| `select` | Select a list item or tab (`replace`, `add`, `remove`) |
| `focus` | Give an element keyboard focus |
| `select_text` | Select a text control's contents |
| `scroll` | Scroll pattern, else mouse wheel |
| `type_text` | Type as keystrokes, optionally focusing an element first |
| `press_key` | A key or chord: `Enter`, `Ctrl+S`, `Alt+F4`, `F5` |
| `window_action` | `minimize`, `maximize`, `restore`, `close` |

## Which tree to ask for

`get_uia_tree` is the right default. It carries `AutomationId`s, control types
and the patterns each element supports, it works against any process regardless
of architecture, and its elements are the ones the action tools can operate on.

`get_visual_tree` answers a different question: *how is this UI implemented?* It
shows HWNDs, XAML types, WPF elements and Chromium DOM nodes — implementation
structure the UIA tree deliberately hides. It cannot drive anything, and because
it works by injecting into the target it needs lvt and the target to share an
architecture. When they do not, it says so and names the right binary.

## Addressing elements

Every tool that takes an element accepts three forms:

- **`e12`** — the element's position in the tree you fetched. Compact, and what
  `find_elements` returns.
- **A durable key** — a path-based identifier that survives more change.
- **`uia:<RuntimeId>`** — the UIA runtime identifier.

`eN` ids are positions, so they are only valid while the tree has the shape it
had when you fetched it. Anything that changes structure — expanding a combo
box, switching a tab, loading a page — invalidates them. **Re-run
`find_elements` after an action that changes the UI** rather than reusing ids
across it.

The tree view matters too: `raw` exposes elements `control` hides, so an id from
one view will not mean the same thing in another. Pass the same `view` you
fetched with.

### The two trees are different shapes, not different numbering

This is the thing to internalise: the UIA tree and the visual tree are not one
tree numbered twice. They are **different node sets at different
granularities**. In the WinUI 3 sample the UIA tree has 74 nodes and the visual
tree 314, and a single button is:

| tree | nodes |
|---|---|
| UIA | `e6` Button "Primary action" |
| visual | `e30` Button → `e31` ContentPresenter → `e32` TextBlock "Primary action" |

So no renumbering could ever make `eN` mean the same thing in both — the
relationship is many-to-one and partial. That is why an id taken from one tree
and used against the other lands somewhere unrelated.

Two things follow.

**Every element carries a qualified `ref`** saying which tree it came from:

```json
{ "id": "e15", "ref": "uia:e15",    "type": "Button", "text": "Go back" }
{ "id": "e33", "ref": "visual:e33", "type": "Button", "text": "Go back" }
```

Prefer `ref` over `id`. Every tool accepts it, routes it to the tree it names,
and refuses one aimed at the wrong tree instead of resolving it to something
else. A bare `eN` still works and means "this tool's default tree".

**`get_visual_tree` can report the correlation directly.** Pass
`correlate: true` and each element gains a `uiaRef` naming its UI Automation
counterpart — the thing you can actually act on:

```json
{ "ref": "visual:e30", "type": "Button",           "uiaRef": "uia:e6" }
{ "ref": "visual:e31", "type": "ContentPresenter", "uiaRef": "uia:e6" }
{ "ref": "visual:e32", "type": "TextBlock",        "uiaRef": "uia:e6" }
```

All three share one counterpart, which is the many-to-one relationship made
explicit. Template children inherit their control's counterpart, since acting on
one means acting on it. Correlation needs both trees, so it costs a second walk
and is off by default.

### Acting on a visual-tree element

If you have a `uiaRef`, just use it — no bridging, nothing to declare.

Otherwise a visual reference is bridged for you. The bridge matches on
**identity** (the element's `x:Name`/`Name`, which is what a UIA `AutomationId`
is built from), falling back to visible text and preferring a candidate that
exposes an actionable pattern. It does not match on screen position, because
the two trees do not share a coordinate space at non-100% display scaling.

When several candidates fit equally well — repeated list rows, say — it
**refuses and lists them** rather than picking one. Acting on a guess is the
worst thing this tool can do.

When it bridges, the result includes `resolvedVia` naming the UIA element
actually acted on and how it was matched. A node with no actionable counterpart
is reported as such rather than as a bare "not found".

Durable keys are self-describing — they name the framework that produced them
(`wpf|…`, `uia|…`) — so they need no qualifier.

## Prefer patterns over synthetic input

`invoke`, `set_value`, `toggle` and friends go through UI Automation patterns.
They do not move the cursor, do not need the window in the foreground, and do
not race the user. `click` tries these first and only falls back to a real mouse
click when nothing else will activate the element; the `method` in the result
says which route was taken.

`type_text` and `press_key` are always synthetic — they need the window in the
foreground, and they will say so if it cannot be brought there rather than
sending keystrokes somewhere unintended. Where a control has a Value pattern,
`set_value` is more reliable than typing into it.

## Input safety

Without `--allow-input`, the mutating tools are **not registered at all** —
they do not appear in `tools/list` and calling one is rejected. A model cannot
be talked into using a tool it cannot see.

The gate is enforced in lvt itself, not only by withholding tools, so it also
covers the one read-only-looking tool that has an effect outside lvt:
`screenshot` with a `path` creates or overwrites that file, and is refused
without `--allow-input`.

This matters because these tools drive the real desktop: a click is a real
click, and `window_action close` really closes an application. Run read-only
unless the agent genuinely needs to act.

Some further properties worth knowing:

- Synthetic clicks refuse offscreen targets rather than being clamped onto
  whatever sits at the desktop corner.
- Synthetic input refuses to proceed when the target window cannot be brought to
  the foreground, instead of delivering input to the wrong window.
- Actions that fail leave the application untouched and report why.

## Screenshots

With no `path`, `screenshot` returns the PNG inline as an image block, which
most hosts display. That costs context proportional to the image, so for a large
window — or when you only need the file — pass a `path` and lvt writes it there
and returns the path instead. Writing to a path creates or overwrites that file,
so it needs `--allow-input`.

Screenshots are annotated with element ids **from the UIA tree** — the same ids
`find_elements` returns and the action tools resolve — so an id read off the
image can be acted on directly. The response reports which tree the ids came
from in `idsFrom`.

Passing `uia: false` annotates with the framework-native visual tree instead.
That is a *different* `eN` numbering over different nodes, and the action tools
do not resolve against it, so only ask for it when visual-tree ids are what you
actually want.

## Concurrency

Requests are dispatched independently, so tool calls can overlap. Two things
follow from that, both handled for you:

Reading a UI is not parallelisable — a UIA walk is answered by the target's UI
thread, which serves one caller at a time. lvt therefore serializes walks **per
target**: concurrent requests against *different* applications proceed in
parallel, while requests against the same application queue. You get a slower
answer rather than a failed one.

Contention from *other* processes reading the same app — another lvt, a screen
reader, Inspect.exe, a second MCP server — cannot be serialized away, so a walk
that collides is retried before being reported as a failure.

## Partial results

A UIA walk has a deadline (`timeoutMs`, default 10000). If it expires, the tree
is incomplete and `get_uia_tree`, `find_elements` and `hit_test` add a
`truncated` field explaining so.

**This matters most for negative answers.** "No element matched" from a
truncated walk means *the walk did not finish*, not *the element is not there*.
Raise `timeoutMs` and retry before concluding something does not exist.

## Building it

The MCP server is a Rust staticlib linked into `lvt.exe`, so it needs a Rust
toolchain and is off by default:

```powershell
cmake --preset default -DLVT_ENABLE_MCP=ON
cmake --build build
```

Install Rust from [rustup.rs](https://rustup.rs). Released binaries are built
with it enabled, so this only affects building from source. Without it, `lvt mcp`
explains what to do rather than failing obscurely.

The Rust layer owns only the MCP protocol and the tool schemas; everything else
happens in `lvt_core` behind a one-function C ABI (`src/lvt_api.h`). Diagnostics
always go to stderr, because stdout carries the JSON-RPC stream.

## Tests

- `cargo test --manifest-path mcp/Cargo.toml` — the FFI ownership rules and the
  `--allow-input` gate.
- `build\lvt_mcp_tests.exe` — drives a real `lvt mcp` process over pipes, the
  way a host does, against the WinUI 3 sample app.
