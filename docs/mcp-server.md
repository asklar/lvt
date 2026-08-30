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
| `get_editable_properties` | Provider-owned typed property schema and live values for one element in the session's UIA or visual tree |
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
| `clear_property` | Clear/reset a typed property when its provider supports that operation |
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
input at where an element is. Injected XAML/WPF/WinForms/plugin visual trees
need lvt and the target to share an architecture. A native-only Win32/ComCtl
tree can still be read across architectures; ABI-sensitive item detail and
property operations become read-only while scalar operations remain available.

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

Changed events carry per-field `{ "old", "new" }` values. A `path` field moves
an existing key to a new absolute tree path; `parentKey` is an explicit
relocation signal for the rarer case where the parent object changed while the
child's absolute path stayed the same.

Every connected session exposes exactly one standards-compliant subscribable
MCP resource, matching the session's fixed mode:

- `lvt://session/<session>/uia-tree` for the default UIA mode
- `lvt://session/<session>/visual-tree` for visual mode

`resources/list` discovers those mode-specific resources, `resources/read`
returns `{ "tree": "uia"|"visual", "snapshot": bool, "events": [...] }`, and
`resources/subscribe` opts into `notifications/resources/updated`. The server
diff-polls the appropriate native tree about every 500 ms, with missed ticks
skipped. This remains the correctness path for both modes: XAML structural
callbacks do not report text/property/bounds changes, and UIA providers are not
required to raise every event consistently.

Visual sessions also maintain a PID- and root-scoped out-of-context WinEvent
hook for native Win32/Common Controls create, destroy, reorder, parent, state,
name, value, selection, and location notifications. Those notifications are
bounded and coalesced to an internal `snapshotRequired` hint. They do not
replace or short-circuit the resource poll, and the current resource scheduler
does not yet wait directly on the hook, so notification latency is still
bounded by the roughly 500 ms cadence. The next full tree refresh consumes the
hint; only its authoritative diff is cached/notified, avoiding a second stream
of duplicate native add/remove events.

UIA sessions similarly keep exact-HWND, subtree-scoped structure/property/
automation handlers on their persistent `IUIAutomation` connection. Callback
bursts allocate nothing and coalesce to one `snapshotRequired` hint. The MCP
bridge drains that hint only after a successful UIA snapshot. Visual snapshots
exclude the `uia` label, including when a visual response later performs a UIA
correlation walk; that correlation drains UIA separately and never polls
visual plugins a second time. Consequently a failed or unrelated refresh
cannot consume the notification and plugin/native event sources are still
polled exactly once in their existing phases. The resource scheduler
remains interval-based rather than waiting directly on provider event handles,
so UIA notification latency is bounded by the same roughly 500 ms cadence.

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

`get_editable_properties` takes an `element` reference or durable key from the
session's own UIA or visual tree. UIA positional references (`uia:eN`) are not
accepted here because they do not encode whether `eN` came from the raw,
control, or content view. Use the element's durable key or `uia:<RuntimeId>`;
the session retains key-to-RuntimeId identity from each tree it returns and
rejects a key if different views made it ambiguous.
It returns a provider-owned `schemaId`, immutable `descriptors`, and separate
per-element live `values`. Descriptors include an opaque `descriptorId`,
declared property type, provider/framework identity, editor kind
(`readonly`, `string`, `boolean`, `integer`, `number`, `enum`, or `command`),
choices, optional numeric limits, writability, and clear capability. A command
descriptor represents provider-supplied actions such as supported UIA scroll
directions; clients render its choices without inferring behavior from the
property name. Live values carry the current value, runtime value type, source,
override state, and whether that specific value can currently be cleared.

Clients never send a property index or type name. The provider resolves the
opaque descriptor id and owns conversion:

```json
{"name":"set_property","arguments":{"session":"s1","element":"winui3:0x123","descriptorId":"winui3-1:p7","value":"100"}}
{"name":"clear_property","arguments":{"session":"s1","element":"winui3:0x123","descriptorId":"winui3-1:p7"}}
```

Setting and clearing require `lvt mcp --allow-input`. Clearing has
provider-defined semantics: XAML removes a local value, while the curated
ComboBox/ListBox descriptors clear selection to the documented no-selection
state. UI Automation patterns generally have no reset operation, so their
descriptors report `supportsClear:false` and `clear_property` returns an
explicit unsupported error. Unknown, stale, element-mismatched, and read-only
descriptor ids are rejected. Successful mutations include provider readback of the effective
`value`, `runtimeType`, `source`, `overridden`, and `canClear` state. The
returned value is never the caller's input echoed back; a failed readback is
reported as an explicit mutation failure.

The provider-neutral contract is implemented by XAML/WinUI3, UI Automation,
WPF, WinForms, and curated Win32/Common Controls adapters. WPF exposes writable scalar
dependency properties and preserves local value precedence through
`SetValue`/`ClearValue`. WinForms uses `TypeDescriptor`, including custom
descriptors, but admits only a conservative scalar/converter allowlist and
uses `ResetValue` only when reset is supported. Native support deliberately
exposes semantic properties rather than styles or messages:

| Native target | Typed properties |
|---|---|
| Any supported HWND | Text and enabled state |
| Button | Check state when the button style is checkable |
| Edit | Text, selection start/end, and read-only state |
| ComboBox / single-select ListBox | Selected index; clear removes selection |
| ScrollBar | Minimum, maximum, position; page size is read-only |
| SysListView32 | View mode; bounded grow/retry item selected/focused/text when identity is verified and it is not owner-data |
| SysTreeView32 | Item selected, expanded, and bounded grow/retry text |
| ToolbarWindow32 | Button checked/enabled/text after index and unique command-id revalidation |
| Status bar | Text parts only; owner-drawn item-data parts are unavailable/read-only |
| Tab control | Selected index and bounded tab text when the label/order identity is unique |

When a native operation is not safe for a particular class/style, owner-data
control, stale item, or architecture combination, the descriptor/value is
read-only with an explicit reason. Native descriptors never contain styles,
message ids, `wParam`/`lParam`, or caller-controlled pointers.

Schema caches are connection-scoped and contain metadata only, never live
values. Native schemas are keyed by normalized class, common-control version,
capability style bits, item kind, and host/target architecture. XAML schemas
use the provider's declared property metadata. For xamlOM properties, the
descriptor's declared `propertyType` comes from `PropertyChainValue.Type` and
drives editor selection and `CreateInstance`; the evaluated value's
`ValueType` is reported only as live `runtimeType` and never trusted for
mutation. Each persistent XAML connection fetches its runtime enum catalog
once. Flags values accept comma-separated members only when WinRT metadata
confirms `System.FlagsAttribute`; ordinary and unresolved enum numerics are
never fabricated into flag combinations. The UIA adapter derives editors and
choices from cached supported patterns and capability properties:
Value/RangeValue read-only state, RangeValue bounds, deterministic Toggle and
ExpandCollapse states, Selection-container capabilities, and supported Scroll
directions. External plugin ABI support is not part of this contract.

Native messaging is bounded with `SendMessageTimeoutW` using
`SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT`. Pointer-bearing common-control
structures and strings use target-process buffers opened with only VM
read/write/operation rights. Before every write lvt revalidates the HWND,
owner PID, class, item index/identity or toolbar command, and value range, then
reads the property back before reporting success. ABI-sensitive pointer
operations are read-only across process architectures; tested scalar message
operations remain available. This architecture check is also applied to
one-shot tree enrichment, even when no property connection exists.

`SendMessageTimeoutW` can return while a target WndProc is still using a
pointer. Pointer operations therefore carry a bounded lifetime token; after a
timeout their local or remote buffer is retained until the target exits rather
than being freed underneath the target. Once the bounded retirement capacity is
full, lvt refuses another pointer message instead of leaking without limit.
Toolbar text is length-queried, safety-capped, and retrieved through a
capacity-bearing `TB_GETBUTTONINFO` buffer with bounded growth rechecks.

Native HWND keys are stable across one-shot commands and persistent sessions:
`win32:0x…` and `comctl:0x…` always come from the HWND, never from whether a
property adapter happened to be connected. Logical common-control item keys
similarly use public-safe text/handle fingerprints or toolbar command ids;
toolbar separators and status parts remain structural because their indices
are mutable positions, not durable identities. Opaque mutation handles remain
private to the provider.
List and toolbar uniqueness is checked across the full control rather than the
50-item output window. Scans above the 256-item safety bound, or scans that
fail, suppress that logical identity and expose the item read-only. Tab labels
use the same bounded full-control proof; tree handles are already unique within
their owning control.

Those compact keys identify but do not authorize. They are valid for native
properties only after that exact target was registered and published while
building the session's root tree. A guessed same-process HWND, a sibling
top-level window, a reparented child that left the root, or a key from a
reconnected session before its first tree refresh is rejected. Each complete
published snapshot replaces the native allow-list. Tab identity never requests
`TCIF_PARAM`, because
`TCM_SETITEMEXTRA` changes that ABI; it instead requires unique text and
revalidates the complete ordered label fingerprint. Duplicate toolbar command
ids similarly make affected buttons read-only.

Tree reads and all three property operations share the session's existing
persistent connection: there is no second injection or side protocol. UIA
connections are scoped to the session's target HWND, not merely its process,
so two windows owned by one process cannot resolve or mutate through each
other's UIA root.

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
