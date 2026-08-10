---
name: lvt
description: >
  Inspect and drive running Windows application UIs using the lvt (Live Visual Tree) CLI tool.
  Use this skill when you need to understand a Windows app's visual tree structure,
  read its UI Automation tree (AutomationIds, control types, supported patterns),
  capture annotated screenshots, detect UI frameworks, find specific UI elements,
  or interact with an app by clicking, typing, toggling and waiting on its controls.
---

# Inspect Windows application UI with lvt

## When to use

Use `lvt` whenever you need to understand the visual content or structure of a running Windows application. Common scenarios:

- **UI verification** — confirm that a UI change was applied correctly (e.g. a button label changed, a dialog appeared)
- **Finding UI elements** — locate a specific control, menu item, or text field in an app's visual tree
- **Screenshot capture** — take an annotated screenshot of an app with element IDs overlaid
- **Framework detection** — determine which UI frameworks an app uses (Win32, ComCtl, XAML, WinUI 3)
- **Automated UI interaction planning** — get element IDs and bounds to plan mouse clicks or keyboard input
- **Automation identity** — get `AutomationId`s, control types, and supported patterns with `--uia`
- **Driving an app** — click, toggle, type, set values, and wait for the UI to settle

## Prerequisites

Before using lvt, ensure `lvt.exe` and `lvt_tap.dll` are available. If they are not already on PATH or in the current directory, download and extract them automatically:

```powershell
# Download the latest release zip and extract to ~/.lvt
$lvtDir = "$env:USERPROFILE\.lvt"
if (-not (Test-Path "$lvtDir\lvt.exe")) {
  New-Item -ItemType Directory -Path $lvtDir -Force | Out-Null
  $release = Invoke-RestMethod "https://api.github.com/repos/asklar/lvt/releases/latest"
  $asset = $release.assets | Where-Object { $_.name -like "lvt-*-x64.zip" } | Select-Object -First 1
  $zip = "$env:TEMP\lvt.zip"
  Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $zip
  Expand-Archive -Path $zip -DestinationPath $lvtDir -Force
  Remove-Item $zip
}
# Run lvt from the install directory
& "$lvtDir\lvt.exe" --help
```

Once downloaded, `lvt.exe` persists in `~/.lvt/` and does not need to be downloaded again.

## Usage

### Target an application

You must specify exactly one target. Pick the most convenient option:

```powershell
# By process name (most common — omit .exe extension if you like)
lvt --name notepad

# By window title substring
lvt --title "Untitled - Notepad"

# By PID
lvt --pid 1234

# By HWND (hex)
lvt --hwnd 0x1A0B3C
```

### Get the visual tree

```powershell
# JSON output (default) — best for programmatic parsing
lvt --name notepad

# XML output — more compact, easier to read
lvt --name notepad --format xml

# Write to a file instead of stdout
lvt --name notepad --output tree.json
```

### Capture a screenshot

```powershell
# Screenshot only (no tree output)
lvt screenshot --name notepad --output out.png

# Screenshot + tree output together
lvt screenshot --name notepad --output out.png
lvt dump --name notepad
```

Screenshots are annotated with element IDs (e0, e1, …) overlaid on each element, making it easy to correlate visual positions with tree nodes.

### Scope to a subtree

When the full tree is too large, scope to a specific element:

```powershell
# Only show element e5 and its descendants, up to 3 levels deep
lvt dump --name myapp --element e5 --depth 3
```

### Detect frameworks only

```powershell
lvt frameworks --name notepad
```

### Get the UI Automation tree

Use `--uia` when you need to *act on* the app rather than just describe it. It
emits the UI Automation tree, where elements carry the identifiers and state an
automation client needs.

```powershell
# Automation-grade view
lvt dump --uia --name myapp

# Narrower (content) or wider (raw) views
lvt dump --uia --uia-view content --name myapp

# Look up one element by its UIA RuntimeId
lvt query uia:42.3150138.4.5 --name myapp
```

Prefer `--uia` when you want to answer "which control do I click, and can I?" —
`AutomationId` gives a stable handle and `SupportedPatterns` tells you what the
element can actually do (`Invoke` = clickable, `Value` = settable text,
`Toggle` = checkable, `ExpandCollapse` = expandable).

`--uia` also works when the target's architecture differs from lvt's, which the
visual tree cannot do.

### Drive the app

lvt can act on elements, not just read them. Every interaction verb resolves its
`<ref>` against a UIA walk, so it implies `--uia`.

```powershell
# Click a button. Uses the Invoke pattern where possible, which does not steal
# focus or move the cursor.
lvt click e6 --name myapp

# Flip a checkbox, set a text box, type, send a chord
lvt toggle e7 --name myapp
lvt set-value e4 "hello" --name myapp
lvt type "some text" --focus-first e4 --name myapp
lvt press-key "Ctrl+S" --name myapp

# Wait for the UI to catch up before the next step
lvt wait-for e9 --wait-prop IsEnabled=true --name myapp
```

The result JSON reports **how** the action was performed:

```json
{ "action": "click", "ok": true, "method": "InvokePattern",
  "result": { "AutomationId": "PrimaryButton", "...": "..." } }
```

- `method` distinguishes a quiet UIA pattern from `SendInput`. Synthetic input
  steals focus and needs the window on top; a pattern does not.
- `result` is the element *after* the action, so the effect can be confirmed
  without a second walk.
- On failure `ok` is false, `error` explains whether the pattern was missing or
  present but refused, and the exit code is non-zero.

Use `SupportedPatterns` from the tree to choose the verb: `Invoke` means
clickable, `Toggle` checkable, `Value` settable, `ExpandCollapse` expandable.

After any action that changes the UI, prefer `wait-for` over sleeping.

Choosing a reference matters when the UI changes shape: `eN` is positional and
`uia:<RuntimeId>` is tied to the element's current host window, so expanding a
combo box (which reparents it into a popup) invalidates both. The durable `key`
survives. Use `eN` for one-shot commands against a static UI, and the durable
key when acting across a structural change.
## Interpreting the output

### Element IDs

Every element gets a stable ID like `e0`, `e1`, `e2`, etc., assigned in depth-first order. These IDs are consistent within a single invocation — use them to:

- Reference specific elements in follow-up commands (`--element e5`)
- Correlate screenshot annotations with tree nodes
- Identify click targets by combining element ID with its `bounds`

### Key element properties

| Property | Description |
|----------|-------------|
| `id` | Stable element ID (e.g. `e0`) |
| `type` | Element type name (e.g. `Window`, `Button`, `TextBlock`) |
| `framework` | Which framework owns this element (`win32`, `comctl`, `xaml`, `winui3`) |
| `className` | Win32 window class name (Win32/ComCtl elements) |
| `text` | Visible text content or window title |
| `bounds` | Screen-relative bounding rectangle `{x, y, width, height}` |
| `children` | Nested child elements |

### Key `--uia` element properties

With `--uia`, elements carry automation identity instead of framework internals.
These live under `properties` in JSON output:

| Property | Description |
|----------|-------------|
| `AutomationId` | Stable, developer-assigned identifier — the best handle for a control |
| `ControlType` | UIA control type (`Button`, `Edit`, `CheckBox`, `TreeItem`, …) |
| `SupportedPatterns` | What the element can do (`Invoke`, `Value`, `Toggle`, `ExpandCollapse`, `Scroll`, …) |
| `RuntimeId` | Per-element handle usable as `query uia:<RuntimeId>` |
| `FrameworkId` | Underlying framework (`Win32`, `XAML`, `WPF`, `WinForm`, …) |
| `IsEnabled`, `IsOffscreen`, `HasKeyboardFocus` | Whether the element is actually actionable right now |
| `Value.Value`, `Toggle.ToggleState`, `ExpandCollapse.State` | Current state, present only when the owning pattern is supported |

Pattern state is only emitted where the pattern is supported, so the presence of
`Toggle.ToggleState` is itself a reliable signal that the element is checkable.

### JSON example

```json
{
  "target": { "hwnd": "0x001A0B3C", "pid": 12345, "processName": "Notepad.exe" },
  "frameworks": ["win32", "winui3"],
  "root": {
    "id": "e0",
    "type": "Window",
    "framework": "win32",
    "className": "Notepad",
    "text": "Untitled - Notepad",
    "bounds": { "x": 100, "y": 100, "width": 800, "height": 600 },
    "children": [ ... ]
  }
}
```

### XML example

```xml
<LiveVisualTree hwnd="0x001A0B3C" pid="12345" process="Notepad.exe" frameworks="win32,winui3">
  <Window id="e0" framework="win32" className="Notepad" text="Untitled - Notepad" bounds="100,100,800,600">
    <ContentPresenter id="e1" framework="winui3" bounds="108,140,784,552" />
  </Window>
</LiveVisualTree>
```

## Recommended workflow

1. **Start the target app** if it isn't already running
2. **Run `lvt --name <app> --format xml`** to get a quick overview of the UI tree
3. **Take a screenshot** with `lvt screenshot --name <app> --output ui.png` to see the visual layout with element IDs
4. **Drill into a subtree** with `--element <id> --depth <n>` if the tree is large
5. **Use element IDs and bounds** to plan any UI interactions (clicks, keyboard input)

## MCP server mode

`lvt mcp` serves the Model Context Protocol over stdio, which is usually a
better fit than shelling out repeatedly: it keeps a session open on the target,
so it walks the tree once per request rather than once per command, and it
returns screenshots as inline images.

```powershell
lvt mcp                  # inspection only
lvt mcp --allow-input    # also expose click, type, set-value and the rest
```

Configure it in an MCP host with `"command": "lvt.exe", "args": ["mcp", "--allow-input"]`.

The flow is `connect` (returns a session id) → `get_uia_tree` or `find_elements`
(returns element ids) → act on those ids. Without `--allow-input` the tools that
change the target app are not registered at all.

See `docs/mcp-server.md` in the repository for the full tool reference.

## Tips

- Use `--format xml` for human-readable output and `--format json` for programmatic parsing
- If the tree is very large, use `--depth` to limit traversal depth first, then drill deeper with `--element`
- Element IDs change between invocations if the UI structure changes — always re-query before acting on stale IDs
- The tool requires no special permissions beyond being able to read the target process (same user session)
- For XAML/WinUI 3 apps, lvt injects a helper DLL into the target — this is safe and non-destructive but means `lvt_tap.dll` must be next to `lvt.exe`
