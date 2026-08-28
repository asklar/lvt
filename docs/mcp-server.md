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
| `get_uia_tree_changes` | Session-scoped UIA patches: a snapshot on first call, then added/removed/changed events |
| `get_visual_tree` | The framework-native tree — Win32/XAML/WPF/WinForms/Avalonia/Chromium. Shows *how a UI is built*; drive it from a `visual`-mode session |
| `get_visual_tree_changes` | Session-scoped visual-tree patches: a snapshot on first call, then added/removed/changed events |
| `get_editable_properties` | Provider-owned typed property schema and live values for one visual element |
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
| `set_property` | Set a writable typed property by opaque provider descriptor id |
| `clear_property` | Clear a typed property's local/provider override |
| `set_expanded` | Expand or collapse a tree item or combo box |
| `select` | Select a list item or tab (`replace`, `add`, `remove`) |
| `focus` | Give an element keyboard focus |
| `select_text` | Select a text control's contents |
| `scroll` | Scroll pattern, else mouse wheel |
| `type_text` | Type as keystrokes, optionally focusing an element first |
| `press_key` | A key or chord: `Enter`, `Ctrl+S`, `Alt+F4`, `F5` |
| `window_action` | `minimize`, `maximize`, `restore`, `close` |

### What a tool returns

Every tool answers twice, with the same content:

- a **text block** holding the JSON, for clients that predate structured output;
- **`structuredContent`**, the same JSON as data.

They are always equal, so read whichever suits you — but prefer
`structuredContent` rather than re-parsing a string that was JSON already. The
one asymmetry is `screenshot`: the image travels as an image content block, and
the base64 appears in neither the text nor the structure.

Each tool also declares an **`outputSchema`**. Because lvt reports failures as
data rather than as protocol errors, every schema is
`anyOf: [success, failure]` — a refusal comes back as
`{"ok": false, "error": "..."}` with none of the success fields, and it is still
a valid result. A schema that only described success would be violated by a
perfectly correct refusal, so none of them do. `additionalProperties` is never
closed either: adding a field to a response should not break a client that
validates.

Tools carry **annotations** as well, so a host can decide whether to confirm
before calling: everything exposed without `--allow-input` is marked
`readOnlyHint`, the tools that change the target app are marked
`destructiveHint`, and all of them set `openWorldHint` because they reach into
another process.

## Which tree to ask for

`get_uia_tree` is the right default. It carries `AutomationId`s, control types
and the patterns each element supports, it works against any process regardless
of architecture, and its elements are the ones a `uia` session acts on.

`get_visual_tree` answers a different question: *how is this UI implemented?* It
shows HWNDs, XAML types, WPF elements and Chromium DOM nodes — implementation
structure the UIA tree deliberately hides. Its elements can be driven too, but
only from a session connected with `mode: "visual"`, which acts by aiming real
input at where an element is. Because it works by injecting into the target it
needs lvt and the target to share an architecture; when they do not, it says so
and names the right binary.

On a rich XAML/WinUI3 tree, `get_visual_tree` walks every element's entire
property inheritance chain by default (`IVisualTreeService::
GetPropertyValuesChain`) — measured at ~4.5ms/element on a real app, which adds
up on a tree of hundreds or thousands of elements. Pass `fast: true` to skip
that walk and collect bounds/`Text`/`Content`/basic state the cheaper way
instead (a few direct property reads per element, no property-chain walk).
This is enough to browse or search a tree by, and to hit-test/highlight
elements, but it will not report arbitrary custom properties the way the
default (`fast: false`) walk does — use `get_element_properties` for a single
element's exhaustive property set regardless of which mode built the tree.

### Incremental tree updates

`get_uia_tree_changes` and `get_visual_tree_changes` each retain their own
previous tree per MCP session. The first call returns the current tree as flat
`added` events with `"snapshot": true`; subsequent calls return the same
`added`, `removed`, and `changed` patch events as `watch`, with
`"snapshot": false`. Disconnecting clears both baselines. Changing the UIA
view/property options or the visual `fast` setting starts a fresh snapshot,
because those choices intentionally describe different trees.

Every connected session exposes exactly one standards-compliant subscribable
MCP resource, matching the session's fixed mode:

- `lvt://session/<session>/uia-tree` for the default UIA mode
- `lvt://session/<session>/visual-tree` for visual mode

`resources/list` discovers those mode-specific resources, `resources/read`
returns `{ "tree": "uia"|"visual", "snapshot": bool, "events": [...] }`, and
`resources/subscribe` opts into `notifications/resources/updated`. The server
diff-polls the appropriate native tree about every 500 ms, with missed ticks
skipped. This is necessary for both modes: UIA event-handler integration is a
future optimization, while XAML's structural callbacks do not report
text/property/bounds changes.

Visual resources always poll `get_visual_tree_changes` with `fast: true`,
matching the Viewer's former `watch --fast` path. The live stream still carries
the bounds, text, content, and basic state needed to render and search the tree;
full selected-node dependency properties come separately from
`get_editable_properties`, avoiding multi-second full-property walks every tick.
UIA resources use their normal default options.

The poll result is cached before the notification is sent, so the following
`resources/read` is fast and drains that cached patch rather than walking the
application again. The initial subscribe/read is always a full snapshot.
Several patches arriving before a read are appended in order; a newer snapshot
replaces older queued patches. Pending diffs are capped at 10,000 events. If
appending would exceed that cap, the server replaces the queue with a fresh
snapshot, preserving a recoverable current state instead of dropping arbitrary
events. Unsubscribe, disconnect, transport errors, and tree-read errors cancel
the resource task and clear its cache.

With rmcp 3.1 this is implemented using
`ServerCapabilities::builder().enable_resources().enable_resources_subscribe()`,
the `ServerHandler::{list_resources,read_resource,subscribe,unsubscribe}`
methods, and `Peer<RoleServer>::notify_resource_updated` for MCP 2025-06-18.
The newer 2026-07-28 subscription lifecycle is supported too through
`ServerHandler::{accepted_subscription_filter,listen}` and
`SubscriptionContext::sink().notify_resource_updated`; no custom JSON-RPC
method or notification is introduced.

### Typed property schemas and mutation

`get_editable_properties` takes an `element` reference or durable visual key.
It returns a provider-owned `schemaId`, immutable `descriptors`, and separate
per-element live `values`. Descriptors include an opaque `descriptorId`,
declared property type, provider/framework identity, editor kind
(`readonly`, `string`, `boolean`, `integer`, `number`, or `enum`), choices,
optional numeric limits, writability, and clear capability. Live values carry
the current value, runtime value type, source, override state, and whether that
specific value can currently be cleared.

Clients never send a property index or type name. The provider resolves the
opaque descriptor id and owns conversion:

```json
{"name":"set_property","arguments":{"session":"s1","element":"winui3:0x123","descriptorId":"winui3-1:p7","value":"100"}}
{"name":"clear_property","arguments":{"session":"s1","element":"winui3:0x123","descriptorId":"winui3-1:p7"}}
```

Setting and clearing require `lvt mcp --allow-input`. Clearing removes the
local/provider value, allowing inherited, style, or default resolution to
resume. Unknown, stale, element-mismatched, and read-only descriptor ids are
rejected.

The provider-neutral contract is implemented by the XAML/WinUI3 adapter today.
Its schema cache is connection-scoped and contains metadata only, never live
values. For xamlOM properties, the descriptor's declared `propertyType` comes
from `PropertyChainValue.Type` and drives editor selection and `CreateInstance`;
the evaluated value's `ValueType` is reported only as live `runtimeType` and
never trusted for mutation. Other built-in provider adapters can implement the
same contract without adding framework catalogs to clients. External plugin ABI
support is not part of this contract.

Tree reads and all three property operations share the session's existing
persistent connection: there is no second injection or side protocol.

## Addressing elements

Every tool that takes an element accepts these forms:

- **`visual:e33` / `uia:e15`** — a qualified reference. This is what every tool
  hands back as `ref`, and the form to prefer: it names the tree it came from,
  so it can be checked rather than assumed.
- **`e12`** — the element's position in the tree you fetched, read against the
  session's own tree.
- **A durable key** — a framework-native identifier that survives more change.
  XAML/WinUI3 use compact diagnostics handles (`xaml:0x…`, `winui3:0x…`);
  providers without a process-wide handle use a structural path. Both forms are
  self-describing.
- **`uia:<RuntimeId>`** — the UIA runtime identifier.

**A session only accepts references from its own tree.** The other tree's are
refused with a note, never matched to something similar — see
[Modes](#modes-how-a-session-drives-the-app).

`eN` ids are positions, so they are only valid while the tree has the shape it
had when you fetched it. Anything that changes structure — expanding a combo
box, switching a tab, loading a page — invalidates them. **Re-run
`find_elements` after an action that changes the UI** rather than reusing ids
across it.

The tree view matters too: `raw` exposes elements `control` hides, so an id from
one view will not mean the same thing in another. Pass the same `view` you
fetched with.

### Modes: how a session drives the app

A session declares which tree it speaks, and that determines how it acts.

| | `mode: "uia"` (default) | `mode: "visual"` |
|---|---|---|
| references come from | `get_uia_tree`, `find_elements` | `get_visual_tree` |
| a click is | the control's Invoke pattern | a real mouse click at its centre |
| typing is | the Value pattern, or keystrokes | real keystrokes |
| needs the window in front | no | yes |
| works across architectures | yes | no — it injects |
| available actions | all of them | click, scroll, type, press_key, focus, wait_for, window_action |

```json
{ "name": "connect", "arguments": { "hwnd": "0x1A0B3C", "mode": "visual" } }
```

**Use `uia` unless you have a reason not to.** Patterns do not move the cursor,
do not need focus, and do not race the user.

**Use `visual` when UI Automation cannot see the UI properly** — custom-drawn
controls, canvas, games — or when the app must observe genuine input.

The two are kept separate on purpose: a reference from one tree is *refused* in
a session of the other mode, rather than being matched to something that looks
similar. That is what stops an action landing on the wrong element.

Modes are per session, so you can hold one of each — read an app's structure
through the visual tree while driving it through UIA, or drive a custom-drawn
app by geometry while driving a normal one by patterns.

**A session's mode is fixed once it is connected**, and there is no tool to
change it. To work the other way round, call `connect` again with the other
mode and keep both sessions; `disconnect` whichever you finish with. Connecting
is cheap — it resolves the window and detects frameworks, and injects nothing —
so a second session costs about as much as one extra call.

The mode is fixed on purpose. Flipping a live session would silently invalidate
every reference already handed out: `e30` is a `Button` in the visual tree and
something else entirely in the UIA tree, so an id that was correct when issued
would quietly start meaning a different control. That is precisely the confusion
modes were introduced to remove. Two sessions keep two id-spaces, both valid.

`toggle`, `set_value`, `select`, `expand` and `invoke` describe what a control
*means*, which geometry cannot express. In visual mode they are refused with a
note pointing at `uia` mode, rather than approximated by a click that might do
something else.

`find_elements` follows the same rule. Its `pattern` filter asks what a control
can *do*, which only UI Automation knows, so in a visual session it is refused
with a note rather than answered with an empty list — "no matches" would read
as a statement about the app when it is really a statement about the tree.
`automationId`, `name` and `type` work in both modes.

## The two trees are different shapes, not different numbering

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
`correlate: true` and each element gains:

- **`uiaRef`** — this element's *own* UI Automation counterpart, matched by
  identity.
- **`uiaAncestorRef`** — the counterpart of the control it sits *inside*, for
  elements that have none of their own. Context, not a target.

```json
{ "ref": "visual:e30", "type": "Button",           "uiaRef": "uia:e6" }
{ "ref": "visual:e31", "type": "ContentPresenter", "uiaAncestorRef": "uia:e6" }
{ "ref": "visual:e32", "type": "TextBlock",        "uiaAncestorRef": "uia:e6" }
```

The distinction matters. Reporting an inherited counterpart as `uiaRef` meant
every one of a ListView's 28 item nodes advertised the *ListView* as the thing
to act on, so "click item 002" clicked the middle of the whole list. A template
part genuinely has no counterpart of its own, and saying so is more useful than
pointing at its container.

Correlation needs both trees, so it costs a second walk and is off by default.
If the UIA side cannot be read, the response says so (`correlationFailed`)
rather than reporting zero matches.

`correlate` belongs to `get_visual_tree` alone — a UIA element is already the
thing you act on, so there is nothing to correlate it to. Scoping the request
with `element` does not change any element's counterpart: correlation is
computed over the whole tree and then reported for the part you asked for, so a
subtree says the same thing about its nodes as the full tree does. The
`correlated` count describes the response you got, not the whole-tree pass
behind it.

### What correlation is for

It answers questions *about* an app, not "which element should this click go
to". Two it answers well:

**"Why can't I automate this control?"** A visual element with no `uiaRef` is
not exposed to UI Automation at all — a missing `AutomationProperties.Name`, an
element in the wrong `AccessibilityView`, a custom-drawn surface with no
automation peer. That is an accessibility gap in the app, and lvt can point at
it precisely because it reads both trees. The WinUI 3 sample has 74 UIA nodes
against 314 visual ones; correlation is how you see which of the 314 made it
across.

**"Which mode should I use?"** One read tells you whether the thing you want to
drive is in the UIA tree. If it is, connect `uia` and use patterns. If it is
not, connect `visual` and drive it by geometry.

What it is deliberately *not* for is translating a reference so another tool
will accept it. lvt used to do that — a visual reference passed to an action in
a `uia` session was matched to a UIA element by identity, then text, then screen
position. It was a heuristic making a choice inside an action, where the caller
could not see it, and when it chose wrong it clicked a different control and
reported success. Modes replaced it: **each session speaks one tree, and refuses
the other's references** rather than guessing what you meant. If you want to
work the other way round, open a second session — they are independent and cheap.

Durable keys are self-describing — they name the framework that produced them
(`winui3:0x…`, `wpf|…`, `uia|…`) — so they need no qualifier, and they are
refused by the wrong session just as `eN` refs are.

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
