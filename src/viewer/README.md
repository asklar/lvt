# lvt Viewer

A Windows desktop GUI for lvt: a live, graphical view of a running app's
element tree, in the spirit of Visual Studio's **Live Visual Tree** tool or
the Windows SDK's **Inspect.exe** — a crosshair you drag onto a window, a
tree on one side, a property panel on the other, and both update live as the
target app's UI changes.

## Quick start

```powershell
# Build lvt.exe first (see the repo root README.md), then:
cd src\viewer\LvtViewer
dotnet build -c Release
.\bin\Release\net10.0-windows\LvtViewer.exe
```

Or build both together via CMake:

```powershell
cmake --preset default -DLVT_BUILD_VIEWER=ON
cmake --build build
.\build\viewer\LvtViewer.exe
```

`LVT_BUILD_VIEWER` is `OFF` by default (see [Locating lvt.exe](#locating-lvtexe)
below for why it's a separate opt-in, mirroring `LVT_BUILD_MANAGED`).

**Using it:** press and drag the crosshair icon in the toolbar onto any
window; release to target it. The tree populates on the left, live; select
an element to see its properties on the right. Toggle "UI Automation tree" to
switch between the UIA view (AutomationId, ControlType, patterns — the
default, and the richer view for most apps) and the framework-native visual
view (HWNDs, XAML/WPF/WinForms types).

## Architecture

### It speaks MCP to lvt.exe — it never links lvt_core

The viewer is a plain WPF (.NET) app that launches
`lvt.exe mcp --allow-input` and speaks MCP JSON-RPC over stdio. It does not
link `lvt_core`, embed a copy of it, or use P/Invoke into it. This keeps the
GUI in WPF while every tree walk, connection, action, property edit, and
notification uses the same public session API an agent uses.

### Live data source: subscribed MCP tree resources

After `connect`, the viewer subscribes to the resource for that fixed-mode
session:

- `lvt://session/sN/uia-tree`
- `lvt://session/sN/visual-tree`

The first `resources/read` returns a complete snapshot expressed as `added`
events. Later changes arrive as standards-compliant unsolicited
`notifications/resources/updated`; the viewer reads the resource again and
receives the cached `added`/`removed`/`changed` patch. Snapshot/diff state
lives in lvt, not in a second independent dump, so the initial tree and every
later patch form one consistent sequence.

See `Services/McpSession.cs` for the transport and
`Services/LiveTree.cs` for targeted patch application. The resource producer
currently diff-polls each session tree internally so property, text, state,
and bounds changes are all observable; framework-native event sources can
reduce that polling later without changing the Viewer protocol.

### Locating lvt.exe

The viewer needs to find `lvt.exe` (`Services/LvtLocator.cs`), tried in order:

1. `LVT_EXE` environment variable, if set.
2. Next to `LvtViewer.exe` itself — this is why the CMake target
   (`LVT_BUILD_VIEWER=ON`) copies `lvt.exe` into `build\viewer\` alongside the
   viewer: a packaged/xcopy-deployed build works with zero setup.
3. Walking up from the viewer's own build output looking for this repo's
   `build\lvt.exe` — what makes `dotnet build`/`dotnet run` from
   `src\viewer\LvtViewer` work immediately in a normal dev checkout, with no
   env var or copy step, as long as `cmake --build build` has been run once.
4. `%USERPROFILE%\.lvt\lvt.exe`.
5. Bare `lvt.exe`, falling through to a PATH search.

### Live tree: keyed by lvt's durable key, not by `eN` position

`docs/mcp-server.md` is explicit that `eN` ids are positions, invalidated by
any structural change, and that a durable key is the one to hold onto across
ticks. `LiveTree` (`Services/LiveTree.cs`) indexes nodes by lvt's `key`
(never changes for a given node) rather than by `id`. A `changed` event's
fields are applied straight onto the existing `ElementNodeViewModel`'s bound
properties — that's what makes the TreeView/property panel update live
without disturbing selection or expansion. `added`/`removed` (or a `changed`
event whose `path` moved — a reorder) trigger a small, targeted rebuild of
just the parent/child edges, reusing view-model instances by key so already-
expanded nodes stay expanded.

### Property editing

All editing uses tools on the same MCP session:

- UIA mode offers `toggle` and `set_value` for properties backed by those
  UI Automation patterns.
- Visual mode calls `get_editable_properties` and renders only the generic
  descriptor metadata returned by the provider: read-only text, strings,
  booleans, integers, numbers, and enums. `set_property` and `clear_property`
  use opaque descriptor ids; the Viewer never sends a framework property
  index or runtime type.

Schemas are cached by `schemaId` while values are refreshed for the selected
element, so controls sharing a schema reuse editor presentation. UIA's existing
Toggle/Value rows remain temporary legacy templates until its provider exposes
the same descriptor contract. Elements are addressed by durable keys.

## Crosshair targeting

`Interop/CrosshairPicker.cs` implements the same viewfinder gesture as
Inspect.exe: press-drag the toolbar's crosshair, and whichever top-level
window is under the cursor is highlighted (a borderless, click-through,
topmost overlay window — `Interop/HighlightOverlay.xaml`); on release, that
window is resolved via `WindowFromPoint` → `GetAncestor(GA_ROOT)` →
`GetWindowThreadProcessId` (`Interop/NativeMethods.cs`) and targeted by its
exact HWND (`lvt ... --hwnd 0x...`), not by process name/PID, so the specific
window under the cursor is what gets inspected even for a multi-window
process.

## What's verified

The Viewer migration is validated against the MCP resource subscription and
property-editing integration tests described in `tests/mcp_tests.cpp`, plus
live UI testing of both modes.

The crosshair-drag gesture itself has been verified with a real mouse on an
unlocked desktop: a genuine mouse-down on the crosshair, drag over a real
window, and mouse-up correctly starts the drag, tracks the window under the
cursor while held, and resolves + connects on release — done twice against
two different Notepad windows, with the resolved HWND/PID cross-checked both
times against an independent, ground-truth `WindowFromPoint`/
`GetAncestor(GA_ROOT)`/`GetWindowThreadProcessId` call at the same screen
point (exact match both times).

One thing worth knowing rather than a bug: if another window is on top of
the viewer's own crosshair at the moment you press down (e.g. a just-opened
window happens to cover it), the mouse-down goes to that window instead —
ordinary window z-order, identical to Inspect.exe's own constraint. Bring
the viewer to the front before starting the drag if that happens.

(This was first verified with the resolution logic alone, independent of a
literal mouse drag, in a sandbox whose interactive desktop was locked at the
time — locked-desktop sessions block all real input injection, which was
confirmed by testing multiple paths. That limitation no longer applies; the
full gesture has since been exercised directly.)

## Stretch/incomplete

- UIA property editing intentionally covers Value/RangeValue and Toggle
  patterns. Other actions remain available through MCP but are not property
  row editors.
- No icon/color legend for the framework color dots in the tree (Win32/XAML/
  WPF/WinForms/Avalonia/Chromium/UIA each get a distinct color —
  `Converters/FrameworkToBrushConverter.cs` — but there's no key explaining
  them in the UI yet).
- Visual property editing initially supports XAML/WinUI dependency properties.
  Other visual providers are explicitly read-only until they expose an
  equivalent typed property capability.
