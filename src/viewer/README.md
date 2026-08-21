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

### It drives lvt.exe as a subprocess — it never links lvt_core

The viewer is a plain WPF (.NET) app that shells out to `lvt.exe` and parses
its stdout. It does not link `lvt_core`, embed a copy of it, or use P/Invoke
into it. This keeps the GUI in the ecosystem it's easiest to build fast,
iterable UI in (WPF), while lvt's actual tree-walking/UIA/injection logic
stays exactly where it already lives and is already tested.

### Data source: `lvt watch`, not `lvt dump` + polling, and not MCP

Three ways to get data out of lvt were on the table: shelling out to `dump`
repeatedly, shelling out to `watch` once and reading its live diff stream, or
launching `lvt mcp` and speaking MCP over stdio. **The viewer uses `watch`.**

MCP was the obvious first thing to check, since it's the more modern,
structured integration point — but reading `docs/mcp-server.md` and
`mcp/src/server.rs` closely shows it has **no push/subscribe tool**. Its
inspection tools (`get_uia_tree`, `get_visual_tree`, `find_elements`) are all
pull; `wait_for` blocks on one condition and returns once. Getting live
updates over MCP would mean polling `get_uia_tree` on an interval and
diffing the results client-side — strictly worse than the CLI's `watch`
verb, which already does exactly that diffing *inside* lvt (see
`src/watch_diff.h`/`.cpp`) and streams the result as one JSON event per line.
So MCP was set aside for this feature; it would be the right choice for a
future request/response-shaped feature (e.g. a "run a query" panel), just
not this one.

Between `dump` and `watch`: a naive design seeds the tree with one `dump`
call and then layers `watch`'s diffs on top. That races two independent
walks of the target — `watch`'s *own* internal walk (what it diffs against)
is a separate one from the external `dump` call, taken at a slightly
different instant. A node that appeared or disappeared in that gap would
never be corrected, because `watch`'s first tick only ever emits `added`
events for its own starting snapshot, never `removed` events for state it
never saw.

**The viewer uses `lvt watch` as its only tree data source.** A freshly
started `watch` process's first burst of `added` events already *is* a
complete, self-consistent snapshot (`run_watch_loop` builds a tree, then
calls `snapshot_added_events` on that exact tree before entering its diff
loop), and every line after that is a true incremental diff against that
same walk. There is only ever one walk in play, so there is nothing to race.
See `Services/WatchSession.cs` and `Services/LiveTree.cs` for the
implementation and a longer version of this reasoning in code comments.

`lvt watch`'s trade-off is that it's poll-driven internally too (an interval
loop, default 500ms; see `--interval`), not a true OS-level change
notification — but it's lvt's own established mechanism for this, it needs
no protocol beyond stdout, and 500ms is imperceptible for the demo scenarios
this was built against (typing into a text box, resizing a window).

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

Editing goes through the same action verbs the CLI already exposes
(`toggle`, `set-value` — `src/providers/uia_actions.cpp`), invoked as
one-shot subprocesses (`Services/LvtCli.cs`) exactly as documented in
`docs/mcp-server.md`'s action tools. Only two properties are ever offered as
editable, because they're the ones lvt already knows how to write:
`Toggle.ToggleState` (via `toggle`) and `Value.Value`/`RangeValue.Value` (via
`set-value`). Every other property is read-only. The element is addressed by
its **durable key**, not its `eN` id, for the same reason `LiveTree` keys by
it — an id can go stale between "user opens the property panel" and "user
clicks Set".

After an edit, the viewer does not manually re-read anything: the next
`watch` tick reports the change as a normal `changed` event, and the same
live-update path that handles the target's own changes reflects it.

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

## What's verified, and a sandbox caveat

Verified end-to-end against a real, live Notepad instance: connecting,
live tree population, live property-panel population (including
`SupportedPatterns`, `AutomationId`, `Toggle.ToggleState` where present), a
live text-value change (via `set-value`, reflected in the property panel with
no manual refresh), and a live bounds update (resizing the target window,
reflected the same way). The crosshair's own *resolution* logic
(`WindowFromPoint`/`GetAncestor`/`GetWindowThreadProcessId`) was verified
directly against a real, moved/resized window and resolves to the exact
right HWND/PID whenever that window is actually topmost at the tested point.

What could **not** be verified in the sandbox this was built in: physically
dragging the crosshair with the mouse. That sandbox's interactive desktop is
locked (a secure desktop is active), which blocks real input end-to-end —
confirmed by testing multiple injection paths (`mouse_event`/`SendInput`-style
hardware injection, and direct `PostMessage`/`SendMessage` to the window)
against the same window, none of which were delivered. The gesture itself
(`Mouse.Capture` + `MouseMove` + `MouseLeftButtonUp`) is a standard, widely
used WPF idiom with nothing target-specific about it; reviewing it and
independently confirming the resolution logic it calls into was the
available substitute for a literal drag in that environment. **Please
re-verify the physical drag on a normal, unlocked desktop before relying on
this** — it is the one piece of the feature that could not be exercised
end-to-end here.

## Stretch/incomplete

- Property editing covers exactly the two verbs above; nothing else in
  `uia_actions.cpp` (`click`, `select`, `expand`, `scroll`, ...) is wired up
  from the property panel. Extending it means adding another
  `PropertyEditKind` and a small UI affordance per verb — the plumbing
  (`LvtCli`, durable-key addressing) already generalizes.
- No icon/color legend for the framework color dots in the tree (Win32/XAML/
  WPF/WinForms/Avalonia/Chromium/UIA each get a distinct color —
  `Converters/FrameworkToBrushConverter.cs` — but there's no key explaining
  them in the UI yet).
- Visual-tree mode (unchecking "UI Automation tree") reads and live-updates
  identically to UIA mode, but property editing is UIA-only — visual-mode
  sessions in lvt itself don't support `toggle`/`set-value` (see "Modes" in
  `docs/mcp-server.md`), so the edit affordances simply won't apply to
  visual-tree properties (they aren't named `Toggle.ToggleState`/`Value.Value`
  there, so this falls out naturally rather than needing special-casing).
