# Skill: Inspect Windows application UI with lvt

## When to use

Use `lvt` whenever you need to understand the visual content or structure of a running Windows application. Common scenarios:

- **UI verification** — confirm that a UI change was applied correctly (e.g. a button label changed, a dialog appeared)
- **Finding UI elements** — locate a specific control, menu item, or text field in an app's visual tree
- **Screenshot capture** — take an annotated screenshot of an app with element IDs overlaid
- **Framework detection** — determine which UI frameworks an app uses (Win32, ComCtl, XAML, WinUI 3)
- **Automated UI interaction planning** — get element IDs and bounds to plan mouse clicks or keyboard input

## Prerequisites

Download `lvt.exe` and `lvt_tap.dll` from the **[latest GitHub release](https://github.com/asklar/lvt/releases/latest)** and place them in the same directory. No build step required.

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
lvt --name notepad --screenshot out.png

# Screenshot + tree output together
lvt --name notepad --screenshot out.png --dump
```

Screenshots are annotated with element IDs (e0, e1, …) overlaid on each element, making it easy to correlate visual positions with tree nodes.

### Scope to a subtree

When the full tree is too large, scope to a specific element:

```powershell
# Only show element e5 and its descendants, up to 3 levels deep
lvt --name myapp --element e5 --depth 3
```

### Detect frameworks only

```powershell
lvt --name notepad --frameworks
```

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
3. **Take a screenshot** with `lvt --name <app> --screenshot ui.png` to see the visual layout with element IDs
4. **Drill into a subtree** with `--element <id> --depth <n>` if the tree is large
5. **Use element IDs and bounds** to plan any UI interactions (clicks, keyboard input)

## Tips

- Use `--format xml` for human-readable output and `--format json` for programmatic parsing
- If the tree is very large, use `--depth` to limit traversal depth first, then drill deeper with `--element`
- Element IDs change between invocations if the UI structure changes — always re-query before acting on stale IDs
- The tool requires no special permissions beyond being able to read the target process (same user session)
- For XAML/WinUI 3 apps, lvt injects a helper DLL into the target — this is safe and non-destructive but means `lvt_tap.dll` must be next to `lvt.exe`
