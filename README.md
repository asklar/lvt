# lvt — Live Visual Tree

A Windows CLI tool that inspects the visual tree of running applications. Designed for AI agents (e.g. GitHub Copilot) that need a textual representation of an app's UI content.

## What it does

- Targets any running Windows app by HWND, PID, process name, or window title
- Detects UI frameworks in use: Win32, ComCtl, Windows XAML (UWP), WinUI 3
- Outputs a unified element tree as JSON or XML markup
- Captures annotated PNG screenshots with element IDs overlaid
- Elements get stable IDs (`e0`, `e1`, …) so AI agents can reference specific parts of the UI

## Quick start

### Download

Grab the latest release from **[GitHub Releases](https://github.com/asklar/lvt/releases/latest)** — extract the zip and run `lvt.exe` from any terminal.

### Install the Copilot skill

The easiest way to add the lvt skill to GitHub Copilot CLI is to install it as a plugin:

```
/plugin install asklar/lvt
```

This gives Copilot the ability to inspect any running Windows app's UI when you ask it to. Verify with `/skills list`.

### Build from source

#### Prerequisites

- Visual Studio 2022+ (C++ Desktop workload)
- [vcpkg](https://vcpkg.io) with `VCPKG_ROOT` environment variable set
- CMake 3.20+
- x64 Developer Command Prompt

#### Build

```powershell
cmake --preset default
cmake --build build
```

Produces `build/lvt.exe` and `build/lvt_tap.dll`.

### Usage

```bash
# Dump Notepad's visual tree as JSON
lvt --name notepad

# XML output
lvt --name notepad --format xml

# Capture annotated screenshot
lvt --pid 1234 --screenshot out.png

# Just detect frameworks
lvt --hwnd 0x1A0B3C --frameworks

# Scope to a subtree
lvt --name myapp --element e5 --depth 3

# Screenshot + tree dump together
lvt --name notepad --screenshot out.png --dump
```

### Options

| Flag | Description |
|------|-------------|
| `--hwnd <handle>` | Target window by HWND (hex) |
| `--pid <pid>` | Target process by PID |
| `--name <exe>` | Target by process name (e.g. `notepad` or `notepad.exe`) |
| `--title <text>` | Target by window title substring |
| `--output <file>` | Write tree to file instead of stdout |
| `--format <fmt>` | `json` (default) or `xml` |
| `--screenshot <file>` | Capture annotated screenshot to PNG |
| `--dump` | Output the tree (default unless `--screenshot` is used) |
| `--element <id>` | Scope to a specific element subtree |
| `--frameworks` | Just list detected frameworks |
| `--depth <n>` | Max tree traversal depth |

## Output format

### JSON

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
    "children": [
      {
        "id": "e1",
        "type": "ContentPresenter",
        "framework": "winui3",
        "bounds": { "x": 108, "y": 140, "width": 784, "height": 552 }
      }
    ]
  }
}
```

### XML

```xml
<LiveVisualTree hwnd="0x001A0B3C" pid="12345" process="Notepad.exe" frameworks="win32,winui3">
  <Window id="e0" framework="win32" className="Notepad" text="Untitled - Notepad" bounds="100,100,800,600">
    <ContentPresenter id="e1" framework="winui3" bounds="108,140,784,552" />
  </Window>
</LiveVisualTree>
```

## Architecture

The tool uses a 4-stage pipeline:

1. **Target resolution** — resolve HWND/PID/name/title to a target window
2. **Framework detection** — enumerate loaded DLLs to detect UI frameworks
3. **Tree building** — Win32 HWND walk as base, framework providers layer on top
4. **Serialization** — output as JSON/XML, optionally capture screenshot

Framework providers:
- **Win32Provider** — base HWND tree (always present)
- **ComCtlProvider** — enriches ComCtl32 controls (ListView items, TreeView nodes, etc.)
- **XamlProvider** — injects TAP DLL to walk Windows XAML visual trees
- **WinUI3Provider** — injects TAP DLL to walk WinUI 3 visual trees

See [docs/architecture.md](docs/architecture.md) for details.

## Design principles

- **No UI Automation** — uses framework-native APIs directly for speed and accuracy
- **Graceful degradation** — if a framework provider fails, falls back to HWND-level info
- **AI-first** — output formats and element IDs designed for machine consumption
- **Minimal footprint** — single exe + one DLL, no installers, no runtime dependencies

## Tests

```powershell
# Run unit tests
build\lvt_unit_tests.exe

# Run integration tests (launches Notepad automatically)
build\lvt_integration_tests.exe

# Via CTest
ctest --test-dir build
```

## Future work

- WinForms provider
- WebView2 provider
- Element property querying (`--query <id> <property>`)
- Accessibility tree correlation
- Watch mode for live tree diffing

## License

MIT — see [LICENSE](LICENSE).
