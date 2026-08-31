# Architecture

## Overview

lvt is structured as a 4-stage pipeline that transforms a target window identifier into a structured element tree.

```mermaid
flowchart TB
    A["Target\nResolution\n<small>target.cpp</small>"] --> B["Framework\nDetection\n<small>framework_detector.cpp</small>"]
    B --> C["Tree\nBuilding\n<small>tree_builder.cpp\nproviders/*</small>"]
    C --> D["Serialization\n& Output\n<small>json_serializer.cpp\nscreenshot.cpp</small>"]
```

## Stage 1: Target Resolution (`target.cpp`)

Accepts one of four target specifiers and resolves to an `HWND` + `PID`:

| Input | Resolution method |
|-------|-------------------|
| `--hwnd` | Used directly; PID via `GetWindowThreadProcessId` |
| `--pid` | `EnumWindows` → filter by PID → select largest visible window |
| `--name` | Enumerate all top-level windows, match process name (case-insensitive) |
| `--title` | Enumerate all top-level windows, match title substring |

For `--name` and `--title`, if multiple windows match, lvt prints all matches and exits (lets the user refine with `--hwnd`).

## Stage 2: Framework Detection (`framework_detector.cpp`)

Enumerates modules loaded in the target process via `EnumProcessModules` and checks for known DLLs:

| Framework | Detection signal | Version source |
|-----------|-----------------|----------------|
| Win32 | Always present | N/A |
| ComCtl | Child window with known ComCtl class name | `comctl32.dll` file version |
| XAML | `Windows.UI.Xaml.dll` loaded | DLL file version |
| WinUI3 | `Microsoft.UI.Xaml.dll` loaded | DLL file version |
| WPF | `PresentationFramework.dll` or `wpfgfx_*.dll` loaded | DLL file version |
| Avalonia | `Avalonia.Base.dll` loaded (via plugin) | DLL file version |

ComCtl detection uses `EnumChildWindows` and checks against a list of known class names (`SysListView32`, `SysTreeView32`, `ToolbarWindow32`, etc.).

## Stage 3: Tree Building (`tree_builder.cpp` + providers)

### Layered provider model

The tree is always rooted in the Win32 HWND hierarchy. Framework-specific providers layer additional detail on top:

```mermaid
flowchart BT
    Win32["Win32Provider\n<small>(base HWND tree)</small>"]
    ComCtl["ComCtlProvider\n<small>(enrich controls)</small>"]
    XAML["XamlProvider\n<small>(inject TAP DLL)</small>"]
    WinUI3["WinUI3Provider\n<small>(inject TAP DLL)</small>"]
    WPF["WpfProvider\n<small>(persistent managed TAP)</small>"]
    WinForms["WinFormsProvider\n<small>(persistent managed TAP)</small>"]
    Avalonia["Avalonia plugin\n<small>(managed TAP)</small>"]
    Tree["Unified Element Tree"]

    Win32 --> ComCtl & XAML & WinUI3 & WPF & WinForms & Avalonia
    ComCtl & XAML & WinUI3 & WPF & WinForms & Avalonia --> Tree
```

1. **Win32Provider** builds the base tree by recursively enumerating child windows (`EnumChildWindows`). Each HWND becomes an `Element` with class name, text, bounds, styles.

2. **ComCtlProvider** walks the existing tree and enriches known ComCtl controls. For example, a `SysListView32` element gets child elements for its items, columns, and headers via control-specific messages (`LVM_GETITEMCOUNT`, `LVM_GETITEMTEXT`, etc.).

3. **XamlProvider / WinUI3Provider** inject the TAP DLL into the target process, receive the XAML visual tree as JSON over a persistent named pipe, and graft XAML subtrees into matching `DesktopChildSiteBridge` elements in the Win32 tree.

4. **WpfProvider / WinFormsProvider** inject architecture-matched native CLR hosts once per session. The managed tree walker connects one duplex pipe and serves correlated tree and typed-property commands until `DISCONNECT` or target/pipe failure. WPF dependency-property operations run on `Application.Dispatcher`; WinForms `TypeDescriptor` operations run through the control's owning Form. Managed object IDs are weakly assigned and the per-connection reverse map is rebuilt on every snapshot, so refreshes preserve identity without retaining dead controls. See [Managed TAP connections](managed-tap-connections.md).

5. **Avalonia plugin** injects an architecture-matched native TAP (`x86`,
   `x64`, or `arm64`) and loads one AnyCPU managed walker through CoreCLR's
   default component entry point. Its current ABI-v1 path is one-shot: each
   enrichment unloads the native TAP before a later refresh reconnects.
   Per-target injection ownership remains held until the remote `LoadLibraryW`
   thread completes or the target exits, even after the diagnostic timeout,
   preventing a retry from racing a delayed load. CoreCLR 8 is the supported
   floor.

### Reusable connections (`providers/framework_connection.h`, `connection_registry.h`)

Injecting the TAP DLL and calling `AdviseVisualTreeChange` is meant to happen **once** per debugging session, not on every tree refresh — see `docs/tap-dll-design.md`'s connection lifecycle section. `IFrameworkConnection` is the generic interface a provider can implement to expose that as "connect once, `get_tree()` many times"; `ConnectionRegistry` is a per-process, refcounted registry (keyed by `pid` + framework label) that lets a long-running consumer — `watch`'s tick loop, an MCP session — acquire one via a move-only `ConnectionHandle` and reuse it for its own lifetime, instead of each tree refresh re-injecting from scratch. `tree_builder.h`'s `build_tree` takes an optional `ConnectionLookup` callback for this; a caller that doesn't supply one (a one-shot `dump`/`query`/`screenshot`) sees no behavior change — providers fall back to their original one-shot `enrich()`.

XamlProvider, WinUI3Provider, WpfProvider, and WinFormsProvider use persistent
diagnostics connections. One-shot WPF/WinForms operations use the same path as
long-running sessions — open connection, `GET_TREE`, `DISCONNECT` — so
injection and teardown have one implementation. Win32Provider and
ComCtlProvider use lightweight per-session connections, not for tree
collection, but to register opaque native element identities and cache
provider-owned typed-property schemas. The Win32 connection also owns one
root-scoped WinEvent subscription that covers the native HWND subtree and
common-control logical-item notifications. UI Automation sessions own one
`IUIAutomation` client and one exact-HWND event subscription on a persistent
MTA thread; registry keys include the root HWND so two top-level windows in one
process never share callbacks or identity state. Plugins can adopt the
interface without changing how callers acquire or use them.

### Native WinEvent refresh hints (`native_win_event.*`)

Long-running visual sessions install one `SetWinEventHook` subscription on the
Win32 connection for `EVENT_OBJECT_CREATE` through
`EVENT_OBJECT_PARENTCHANGE`, filtered by the target PID. The hook is
`WINEVENT_OUTOFCONTEXT`; targets outside lvt also use
`WINEVENT_SKIPOWNPROCESS`. A dedicated thread installs the hook, owns the
required message loop, and calls `UnhookWinEvent` exactly once on that same
thread when the last connection-generation reference is released or the
target process exits.

Out-of-context callbacks route through thread-local state, so simultaneous
sessions have no global hook-to-connection map that could collide. The callback
does not send messages, inspect HWNDs, take locks, or allocate. It only copies a
small event record into a fixed 256-entry single-producer queue; reentrancy or
overflow sets a bounded reset marker. `poll_events()` drains off-callback,
filters records against the root and the HWNDs from the last published
snapshot, and coalesces any relevant native/control-item activity to one
provider-neutral `snapshotRequired` event. Destroy notifications are matched
only against stored handle values and never dereference or query the destroyed
HWND. Current HWND subtree checks for create/reparent events happen only during
the later drain.

Native eventing is an optimization signal, never a correctness source. Both
`watch` and MCP still build complete periodic snapshots and derive public
added/removed/changed output only from `diff_trees`; they drain pushed events
after that authoritative refresh instead of also emitting native structural
deltas. Missing, delayed, coalesced, or overflowed WinEvents therefore cannot
hide text, bounds, state, or structure changes and cannot create duplicate
add/remove storms. The current schedulers do not wait on a provider event
handle: `watch` latency remains bounded by `--interval`, and subscribed MCP
resources by their approximately 500 ms polling cadence.

Key-scoped watch resolves the requested `--element` only for its initial
snapshot. It records the resolved key, parent, full-tree path, base identity,
and provider identity, then builds and reconciles complete trees on every
later tick before deriving the reported scoped snapshot. An authoritative
durable-identity replacement at the anchored parent/path is surfaced as a
Removed subtree followed by an Added subtree with the fresh key. A moved
sibling merely occupying the old slot is not adopted: replacement requires
continuity from a durable identity, supported native/provider handle,
normalized UIA RuntimeId, or an explicit provider mapping whose parent
cardinality is unchanged. A positional/ambiguous scope has no such token, so a
sibling-set structural change removes it and later slot occupants are ignored
until the caller rescopes. Removal with a retained token keeps the watch alive
until that anchored identity validly reappears. Full-tree reconciliation
preserves process-wide XAML/WinUI reparent matching, while incomplete provider
markers continue to preserve the previous snapshot rather than reporting false
removals.

### UI Automation refresh hints (`uia_provider.*`)

Each persistent `UiaConnection` creates its `IUIAutomation` client, root
element, cache request, and event handlers on one connection-owned MTA thread.
The subscriptions use `TreeScope_Subtree` from the exact `ElementFromHandle`
root. `UiaTargetIdentity` treats the resolver/registry PID as authoritative:
connection creation rejects a numeric HWND currently owned by any other process
before starting the worker. Target resolution also captures the process
creation timestamp and the root UIA `RuntimeId`. Persistent connections retain
an open handle to that original process, while one-shot tree/action fallbacks
carry the same immutable PID, creation identity, and root token captured by the
CLI command or MCP session.

Because a same-process HWND can eventually recycle to the exact same numeric
value and therefore reproduce an HWND-derived UIA `RuntimeId`, identity also
owns a non-recyclable window-lifetime sentinel. lvt normally installs a
GUID-named window property whose unique value exists only for that window
generation and disappears when it is destroyed. The last identity owner removes
the property only if the original process is still live and the property still
matches. If `SetProp` is denied (for example by an integrity boundary), lvt
keeps support by falling back to an exact-root destroy WinEvent subscription;
validation synchronizes with that hook before accepting the window.

Immediately after every `ElementFromHandle`, on the owning MTA, lvt rechecks
the current HWND owner, retained process identity, window-lifetime sentinel,
UIA root `ProcessId`, and root `RuntimeId` before reading or acting. A stale
registry key therefore cannot attach to a replacement process after Windows
reuses a PID/HWND, and same-process HWND reuse cannot silently switch to a
different automation root. No desktop/global UIA
subscription is installed. Structure changes, UIA property changes affecting
tree shape/identity/bounds/enabled/offscreen state, pattern availability and
Value/RangeValue/Toggle/ExpandCollapse/Selection/Scroll state, plus relevant
automation events all feed the same provider-neutral event flow.

The COM callback object is free-threaded and deliberately does no UIA work: it
takes no lock, performs no tree walk or provider call, and allocates nothing.
It atomically coalesces any burst to one bounded `snapshotRequired` bit.
`refresh_events()` provides an MTA barrier and `poll_events()` exchanges that
bit on the owning MTA. Watch and MCP consume the bit only after a successful
authoritative UIA snapshot. MCP drain scopes are tree-specific: UIA snapshots
poll only the `uia` connection, while visual snapshots poll injected, managed,
and plugin connections but exclude UIA; native hints retain their pre-visual
build phase. Thus correlation performs exactly one visual-provider drain and
one UIA drain, a failed walk cannot lose the wake-up needed by its retry, and a
UIA hint arriving after the last UIA snapshot stays queued across unrelated
visual requests.

The MTA worker checks the exact HWND/PID every 100 ms. On disconnect, target
exit, or final release it first stops accepting callback hints, calls
`RemoveAllEventHandlers` on the same `IUIAutomation` instance/thread, releases
all COM references there, and only then reports the connection dead. UIA
remains architecture-neutral because no target-side code or pointer-sized
payload is injected.

Persistent tree failures may still fall back to a one-shot UIA client for
ordinary transient provider contention, but the fallback receives the
session's captured `UiaTargetIdentity`; it never re-derives ownership from the
current numeric HWND. Identity mismatch returns `ownershipLost` and stops the
walk/action rather than exposing a replacement process or root.

Synthetic UIA fallbacks repeat the full identity check after foreground
activation and again immediately before every `SendInput` dispatch (mouse,
wheel, text, or each key chord). Ownership failures set the structured action
code `ownershipLost`, including elementless type/press-key requests, so CLI and
MCP clients can distinguish a security boundary from an ordinary action error.
Every dispatch also verifies that the target root is still the foreground
window. Multi-batch operations (focus-click then text, select-all then text, or
multi-chord key sequences) re-activate and revalidate between batches if
foreground moved; otherwise they refuse the remaining input.

UIA cache and pattern boundaries are fenced too. Every `BuildUpdatedCache`
result is followed by a fresh original-token/root validation before cached data
is traversed or returned, including when the provider call failed. Mutation
patterns, `Realize`, and `SetFocus` validate immediately before the provider
call and again afterwards; a provider failure is never classified until that
post-call validation has ruled out target replacement.

Visual MCP sessions capture the same `UiaTargetIdentity` even though their tree
comes from framework-native providers. Geometry-based input and Win32 window
commands validate that original identity before acting, after foreground
activation, and immediately before each dispatch. Visual and UIA `wait-gone`
still succeed when the original target disappears, while `wait-for` reports
structured `ownershipLost`.

The initial visual activation is unconditional: even an already-foreground
root still passes through `bring_to_foreground`, whose contract also restores a
minimized window and raises it in Z order. Subsequent batches re-activate only
when foreground moved.

If Windows temporarily reports no foreground window, `bring_to_foreground`
creates the caller's message queue, restores and raises only the intended root,
and retries activation twice before returning the system-observed result. It
never focuses an unrelated window as an intermediate step.

### Native typed-property safety (`native_message.*`,
`native_property_connection.*`)

The Win32/Common Controls property adapter is intentionally curated. Its
descriptor ids map to internal semantic operations such as `Text`,
`SelectedIndex`, or `Expanded`; they cannot encode a message id or native
pointer.

All cross-process messages go through one `SendMessageTimeoutW` wrapper with
`SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT`. Common-control messages at or above
`WM_USER` do not marshal caller pointers, so structures and strings for those
messages live in RAII `VirtualAllocEx` buffers. The owning process handle uses
only `PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE`; pointer
message lifetime uses a separate `SYNCHRONIZE`-only process handle. System
messages such as `WM_GETTEXT` and `EM_GETSEL` use User32's documented
marshalling but remain disabled across architectures in the conservative
first implementation.

Because a timed-out send can return before the target WndProc has finished,
pointer-message buffers are not released on caller timeout. A bounded
retirement manager holds them until target-process exit; exhausting the bound
refuses new pointer sends rather than accumulating unbounded remote memory.
Toolbar text avoids the no-capacity `TB_GETBUTTONTEXT` copy: it uses that
message only for the documented length query, then retrieves through
`TB_GETBUTTONINFO` with `cchText` and bounded re-query/retry.

Every mutation validates that the HWND still exists and has the same PID and
class. Item operations additionally revalidate the current index and identity
(or toolbar command id), range-constrained values are checked against live
state, and every successful write is read back. Owner-data or ambiguous
list-view/tab items and ABI-sensitive cross-bitness operations remain visible
but read-only with a reason. Native connections accept only HWNDs registered
from their root/descendant tree; compact handles do not authorize arbitrary
same-process top-level windows, and every operation repeats the root/descendant
check so reparented controls stop being actionable. Publishing a new visual
snapshot replaces, rather than accumulates, the session allow-list.
Connection lookup validates session liveness before and after connection-map
work, never while holding the connection-map lock, so disconnect cannot
recreate an erased entry and authorization publication cannot deadlock against
property connection acquisition.

Native durable keys are deliberately independent of those mutation handles.
Every HWND-backed Win32/common-control node uses the same compact
`win32:0x…`/`comctl:0x…` key in one-shot and persistent trees, derived from its
HWND plus an architecture-neutral window-lifetime sentinel. Exact numeric HWND
reuse receives a fresh key. When UIPI prevents sentinel installation, the node
remains inspectable but receives no mutable provider handle. Logical
common-control children use public-safe identities scoped to that lifetime: unique
list/tab text fingerprints, a fingerprint of the documented tree-item handle,
and unique toolbar command ids. Duplicate/unsafe identities, toolbar
separators, and status-bar parts fall back to parent-local structural slots;
their mutable indices are never promoted to durable identity. The provider's
opaque session handle remains in `Element::providerHandle` only for property
routing and is never substituted into the public key. Parsing an HWND key
identifies a candidate; the connection's published-root allow-list still
authorizes it.
List and toolbar uniqueness scans cover the complete control, not only the
first 50 children emitted in a dump. The scan is safety-bounded at 256 items;
failure, timeout, or a larger control suppresses the durable logical identity
and keeps property mutation read-only rather than claiming false uniqueness.
Tabs apply the same bound while scanning every label. Tree item handles are
intrinsically unique within their owning control, so output truncation does
not weaken their identity proof. Status-part mutation safety remains separately
guarded by index/count/text snapshot validation despite its structural key.

Watch reconciliation treats a provider-supplied `durableIdentity` as
authoritative. It is inherited only when equal and unique in both snapshots;
change, loss, or ambiguity emits removal of the old key and addition of the
freshly assigned key (including the corresponding subtree), rather than
falling back to shape or position and silently retaining stale identity.

List-view, tree-view, and tab text reads grow their capacity and retry up to the
provider safety limit; setters reject values beyond that same readback limit
before mutation. Tab reads request `TCIF_TEXT` only: `TCIF_PARAM` is
intentionally avoided because `TCM_SETITEMEXTRA` replaces the `TCITEM` ABI with
an application-defined `TCITEMHEADER`-based structure. Toolbar buttons require
both a stable index and a unique command id. Status parts inspect the returned
`SBT_*` type before any copy; `SBT_OWNERDRAW` is item data, never a string, so
those parts are unavailable/read-only and their flag is never combined with a
temporary text pointer.

### Persistent plugin connections

Plugin ABI v2 maps its optional `lvt_connection_open` / `lvt_connection_get_tree` / `lvt_connection_close` exports onto the same interface. Core enables that path only when the complete lifetime group and `lvt_plugin_free` are present; event polling is a separate complete pair (`lvt_connection_poll_events` + `lvt_connection_events_free`). Framework detection carries an opaque token for the exact `LoadedPlugin` that reported it, so an alias differing from plugin metadata — or two plugins reporting the same framework name — cannot be resolved to the wrong DLL later. Registry keys include that plugin's loaded module and source path plus the target HWND, preventing collisions with built-ins, other plugin instances, and another window in the same process. Provider options such as Chromium's tab selector are forwarded on every persistent `get_tree`.

An explicitly persistent lookup never falls back to one-shot injection when its connection is missing or dies. The refresh is marked incomplete, the old generation is released, and watch/MCP reacquire through `ConnectionRegistry`; generation checks keep stale handles from releasing a replacement. Watch re-runs framework detection before every snapshot, adding newly available v2 plugin slots and releasing obsolete ones before building that tick. MCP disconnect takes the per-target guard before erasing the session, so a read either finishes with its retained handle snapshot or observes a disconnected session — it cannot cross the boundary and one-shot inject.

Plugins that expose only ABI v1, or an incomplete v2 group, remain on `lvt_enrich_tree` for every refresh and therefore stay backward-compatible. Both watch and MCP drain the provider-neutral event queue after successful snapshots; a persistent plugin without the optional poll pair still refreshes normally through `get_tree`. Plugin tree JSON is schema-checked before grafting and bounded by nesting/node limits. Event arrays are count-bounded, each record's `struct_size` is checked before reading it, and plugin-owned tree/event allocations are released on every success and failure path. Invalid persistent output marks that provider refresh incomplete and triggers normal reconnection rather than being accepted as an empty tree.

### Element ID assignment

After the full tree is built, `assign_element_ids()` walks the tree in depth-first order and assigns IDs: `e0`, `e1`, `e2`, …. These IDs are:
- Stable within a single invocation
- Deterministic (same tree structure → same IDs)
- Used by `--element` for subtree scoping and by screenshot annotations

Durable `key` identity is separate. XAML/WinUI instance handles and managed
WPF/WinForms handles use compact framework-qualified keys (`wpf:0x…`,
`winforms:0x…`, and so on), remaining stable across refreshes on the same live
target. Native HWND keys are also compact and connection-independent; only
XAML/WinUI identities opt into process-wide reparent reconciliation.

### Bridge-to-XAML matching

WinUI 3 apps use `DesktopChildSiteBridge` windows to host XAML content inside Win32 HWNDs. The XAML provider:
1. Collects all bridge elements from the Win32 tree (in enumeration order)
2. Collects XAML root elements from the TAP DLL output (in order)
3. Matches them 1:1 by position
4. Grafts each XAML subtree as children of the corresponding bridge element

## Stage 3 (alternate): UI Automation tree (`providers/uia_provider.cpp`)

`--uia` swaps out Stage 3 entirely. Instead of the layered Win32-plus-providers
build, it walks the target's UI Automation tree with an `IUIAutomation` client
and returns the same `lvt::Element` type, so Stages 1 and 4 are untouched.

```mermaid
flowchart LR
    A["Target"] --> B{"--uia?"}
    B -- no --> C["Framework detection\n+ layered providers"]
    B -- yes --> D["UiaProvider\n<small>IUIAutomation client</small>"]
    C --> E["Element tree"]
    D --> E
    E --> F["Serialization"]
```

Differences that matter:

| | Visual tree | UIA tree |
|---|---|---|
| Mechanism | Native APIs + DLL injection | `IUIAutomation` client, no injection |
| Architecture | Must match for injected providers; native-only cross-architecture is scalar/partial | Cross-architecture |
| Identity | Class names, x:Name | `AutomationId`, `RuntimeId` |
| Actionability | none | `SupportedPatterns` + pattern state |

Three implementation constraints shape the provider:

1. **Everything goes through one cache request.** Each UIA property read is a
   cross-process call, so the provider builds an `IUIAutomationCacheRequest`
   with `TreeScope_Subtree` and every property and pattern it wants, calls
   `BuildUpdatedCache` once, then walks with `GetCachedChildren()` /
   `GetCachedPropertyValue()`. This is the difference between one round trip and
   thousands.

2. **Pattern-backed properties are read with the `Ex` accessor.** UIA's
   `GetCachedPropertyValue` substitutes the property type's *default* when an
   element does not support the owning pattern, so a `Window` answers
   `Toggle.ToggleState` with `2` (`ToggleState_Indeterminate`) exactly as a real
   checkbox answers `1`. Properties named `Pattern.Member` are therefore read
   with `GetCachedPropertyValueEx(id, ignoreDefaultValue=TRUE, …)`, whose
   reserved "not supported" return is the provider-authoritative signal — no
   property-to-pattern table needed. Core properties keep the plain accessor,
   because for them "not supported" also fires whenever a provider simply did
   not set the value, which would drop useful state such as `IsControlElement`.
   Separately, `uia_props.cpp` suppresses framework-specific "unset" sentinels
   (Win32 uses `0` where XAML uses `-1` for an unset `Level`).

3. **It runs in the MTA.** UIA clients want an MTA; `screenshot.cpp` initializes
   an STA on the calling thread. A thread cannot be both, so one-shot walks use
   `run_on_mta()`. Long-running watch/MCP sessions instead keep one dedicated
   MTA worker for the lifetime of `UiaConnection`; every reused-client tree
   read, typed-property mutation, event registration/drain, and COM release is
   dispatched to that same worker.

The walk is bounded by `--uia-timeout`, which drives UIA's transaction timeout —
the thing that actually bounds a wedged target, since every cross-process call
happens inside the single `BuildUpdatedCache`. The per-element deadline check
additionally limits the in-process traversal of the materialised cache; when it
fires, the root carries a `Truncated` property so a consumer reading only the
document can tell the tree is incomplete.

It caps each provider response rather than total wall time, since one walk
involves many responses. The 10s default sits comfortably above a real app's
walk — the heaviest measured, a WebView2 host with ~380 elements, takes about
1.3s — while staying well under UIA's own 20s default.

## MCP server (`lvt_api.cpp` + `mcp/`)

`lvt mcp` serves the Model Context Protocol over stdio from inside `lvt.exe`.
It is not a fifth pipeline stage but a second front end: it drives the same
target resolution, tree building and action machinery the CLI does, only with a
session held open between requests instead of one process per command.

It is split across a language boundary:

```
mcp/src/server.rs     tool schemas, MCP protocol      (Rust, rmcp)
mcp/src/ffi.rs        the only unsafe code            (Rust)
        ↕  extern "C" — one function, JSON in, JSON out
src/lvt_api.cpp       sessions, dispatch, all logic   (C++)
src/providers/…       unchanged
```

**Why Rust.** `lvt mcp` has to be served by `lvt.exe` itself — a second binary
or a DLL would defeat the point of a single-executable release. That rules out
running the MCP SDK as a separate process, and of the options that link into an
existing MSVC binary, a Rust `staticlib` is the mainstream one. The alternative
considered, C# NativeAOT with `NativeLib=Static`, is documented by Microsoft as
implemented but unsupported.

**Why the seam is one function.** `lvt_api_call(method, params_json, &result)`
is the entire ABI. Adding a tool never changes it, the FFI surface is small
enough to audit at a glance, and if the Rust layer ever needed replacing the C++
side would not move.

**Memory ownership.** Rust always links the release CRT while lvt may be built
against the debug one, so the two can genuinely have different heaps. The rule
is therefore absolute: each side frees only what it allocated. `result_json` is
malloc'd by lvt and released only through `lvt_api_free`, which on the Rust side
is enforced by a `Drop` impl rather than by remembering to call it.

**Neither runtime unwinds into the other.** `lvt_api_call` wraps its body so no
C++ exception escapes; the Rust entry point uses `catch_unwind` and the crate is
built with `panic = "abort"`.

**stdout is the protocol.** Every diagnostic goes to stderr — this is why the
WIL logging callback was made stderr-only before any of this was written.

**Input gating** happens at registration, not at call time: `--allow-input`
selects which tool routers are composed, so a withheld tool is absent from
`tools/list` and not merely refused.

**Sessions** live in `lvt_api.cpp` behind a mutex, keyed `s1`, `s2`, …. Each
holds a resolved HWND, pid and architecture, so later calls skip target
resolution — which is ambiguous when several windows match a process name.

## Stage 4: Serialization (`json_serializer.cpp`, `screenshot.cpp`)
### JSON output

Standard JSON with `target` metadata, `frameworks` array, and `root` element tree. Uses nlohmann/json for serialization.

### XML output

XML markup where each element's type becomes the tag name. Attributes include `id`, `framework`, `className`, `text`, `bounds`, and any framework-specific properties.

### Screenshot capture

Uses `Windows.Graphics.Capture` APIs:
1. Create a `GraphicsCaptureItem` from the target HWND
2. Capture a frame via `Direct3D11CaptureFramePool`
3. Convert to a CPU-accessible bitmap
4. Annotate with bounding boxes and element ID labels using GDI
5. Fix alpha channel (GDI doesn't set alpha; must post-process to set all alpha bytes to 255)
6. Encode to PNG via `WICBitmapEncoder`

Annotation skips elements with zero bounds. XAML element bounds are computed from the bridge window's screen position plus per-element offsets and dimensions.

## Element model (`element.h`)

```cpp
struct Bounds {
    int x = 0, y = 0, width = 0, height = 0;
};

struct Element {
    std::string id;           // "e0", "e1", ...
    std::string type;         // Friendly name ("Button", "StackPanel")
    std::string framework;    // "win32", "comctl", "xaml", "winui3"
    std::string className;    // Full class/type name
    std::string text;         // Visible text or accessible name
    Bounds bounds;            // Screen coordinates
    std::map<std::string, std::string> properties;
    std::vector<Element> children;
    uintptr_t nativeHandle;   // Pointer-sized native identity (e.g. HWND)
    uint64_t providerHandle;  // Fixed-width provider object identity
};
```

## Dependencies

| Dependency | Purpose | Source |
|-----------|---------|--------|
| WIL | Smart pointers, error handling | vcpkg |
| nlohmann/json | JSON serialization | vcpkg |
| GoogleTest | Unit and integration tests | vcpkg |
| Windows SDK | Win32 APIs, XAML Diagnostics, Graphics.Capture | System |
| C++/WinRT | WinRT APIs (Graphics.Capture, BitmapEncoder) | Windows SDK |
