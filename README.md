# lvt — Live Visual Tree

A Windows CLI tool that inspects the visual tree of running applications. Designed for AI agents (e.g. GitHub Copilot) that need a textual representation of an app's UI content.

![lvt inspecting File Explorer](docs/hero.png)

![Annotated screenshot of Notepad](docs/notepad-lorem.png)

## What it does

- Targets any running Windows app by HWND, PID, process name, or window title
- Detects UI frameworks in use: Win32, ComCtl, Windows XAML (UWP), WinUI 3, WPF, [Avalonia](docs/avalonia-plugin.md), [Chrome/Edge](docs/chromium-plugin.md)
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
# x64 build (default)
cmake --preset default
cmake --build build

# ARM64 build
cmake --preset arm64
cmake --build build-arm64
```

Produces `build/lvt.exe` and `build/lvt_tap_x64.dll` (or `build-arm64/lvt.exe` and `build-arm64/lvt_tap_arm64.dll` for ARM64).

> **Note:** lvt.exe must match the target process's architecture. If you target an ARM64 app, use the ARM64 build. A clear error message is shown on mismatch.

#### Build options

Framework support is per-provider, so you can drop pieces you don't need. Win32 and ComCtl are always built — every other provider enriches that base layer.

| Option | Default | Effect |
| --- | --- | --- |
| `LVT_ENABLE_XAML` | ON | System XAML (UWP / DesktopWindowXamlSource) |
| `LVT_ENABLE_WINUI3` | ON | WinUI 3 (Windows App SDK) |
| `LVT_ENABLE_WPF` | ON | WPF |
| `LVT_ENABLE_WINFORMS` | ON | WinForms |
| `LVT_ENABLE_AVALONIA` | ON | Avalonia plugin |
| `LVT_ENABLE_CHROMIUM` | ON | Chromium (Chrome/Edge DOM) plugin |
| `LVT_BUILD_TOOL` | ON | Build the `lvt` CLI as well as the library |
| `LVT_BUILD_MANAGED` | ON | Build the managed .NET helper assemblies |
| `LVT_BUILD_TESTS` | ON | Build the test executables |

`LVT_BUILD_MANAGED` is the only thing that requires the .NET SDK. WPF, WinForms and Avalonia each have a native half that hosts the CLR plus a managed tree-walker assembly; only the latter needs `dotnet`. With `-DLVT_BUILD_MANAGED=OFF` the whole native build still works, including those TAP DLLs — you just lose managed enrichment for those three frameworks. XAML and WinUI 3 are pure C++ either way.

#### C++/WinRT projection

The XAML and WinUI 3 providers need C++/WinRT headers. These come from two places:

- `winrt/base.h` and the `Windows.*` projection — from the **`cppwinrt` vcpkg port**
- the `Microsoft.*` (WinUI 3) projection — generated at configure time into `src/tap/winui3/` from the **Windows App SDK NuGet package**, which has no vcpkg port

Both halves must be produced by the same cppwinrt version, or the generated headers fail to compile against `base.h`'s macros ("Mismatched C++/WinRT headers"). Taking the generator from the `cppwinrt` port rather than the Windows SDK keeps them in lockstep — and because vcpkg installs only one version of a port per triplet, a consumer that already depends on `cppwinrt` picks the version for both.

The generated projection is cached in `src/tap/winui3/` (gitignored) and regenerated whenever the generator or the Windows App SDK inputs change, tracked via `src/tap/winui3/.cppwinrt-signature`. Two escape hatches:

```powershell
# Use a specific generator or Windows App SDK package
cmake --preset default -DLVT_CPPWINRT_EXE=... -DLVT_WASDK_WINMD_DIR=...

# Force a full regeneration
Remove-Item -Recurse src/tap/winui3
```

## Using lvt from another project

lvt installs a CMake package exposing `lvt::core`, the same library the CLI is built on.

### With vcpkg

This repository doubles as a [vcpkg registry](https://learn.microsoft.com/vcpkg/consume/git-registries). Add a `vcpkg-configuration.json` next to your `vcpkg.json`:

```json
{
  "default-registry": {
    "kind": "git",
    "repository": "https://github.com/microsoft/vcpkg",
    "baseline": "<a microsoft/vcpkg commit sha>"
  },
  "registries": [
    {
      "kind": "git",
      "repository": "https://github.com/asklar/lvt",
      "baseline": "<an asklar/lvt commit sha>",
      "packages": [ "lvt" ]
    }
  ]
}
```

then depend on it from your `vcpkg.json`:

```json
{
  "dependencies": [
    { "name": "lvt", "features": [ "winui3", "wpf" ] }
  ]
}
```

Features map one-to-one onto the framework options above: `xaml`, `winui3`, `wpf`, `winforms`, `avalonia`, `chromium`, plus `tools` for the CLI. All but `avalonia` are on by default. The port always builds with `LVT_BUILD_MANAGED=OFF`, because vcpkg builds have neither network access nor a .NET SDK — see [`ports/lvt/usage`](ports/lvt/usage).

### In CMake

```cmake
find_package(lvt CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE lvt::core)

# lvt looks for its injectable TAP DLLs next to the binary it is linked into,
# so copy them beside your executable:
lvt_copy_tap_dlls(my_app)
```

```cpp
#include <lvt/target.h>
#include <lvt/framework_detector.h>
#include <lvt/tree_builder.h>
#include <lvt/json_serializer.h>
#include <lvt/lvt_config.h>   // which frameworks were compiled in

auto matches    = lvt::find_by_process_name("notepad");
auto target     = lvt::resolve_target(matches[0].hwnd, matches[0].pid);
auto frameworks = lvt::detect_frameworks(target.hwnd, target.pid);
auto tree       = lvt::build_tree(target.hwnd, target.pid, frameworks);
lvt::assign_element_ids(tree);
```

Instead of `lvt_copy_tap_dlls()` you can call `lvt::set_tap_directory()` at runtime, or point the `LVT_TAP_DIR` environment variable at the installed tools directory. Plugins are searched for in `$LVT_PLUGIN_DIR`, then `<module dir>/plugins`, then `%USERPROFILE%\.lvt\plugins`.

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

# Query an element by durable key or eN id
lvt --name myapp --query "win32|Window|MyWindow/win32|Button|Button|Name:OK" text

# Screenshot + tree dump together
lvt --name notepad --screenshot out.png --dump

# Watch for live tree changes as JSON diff events
lvt --name notepad --watch --interval 250
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
| `--watch` | Emit live JSON tree diff events until Ctrl+C |
| `--interval <ms>` | Polling interval for `--watch` (default: 500) |
| `--element <ref>` | Scope to a specific element subtree by positional `eN` id, durable key, or `uia:<RuntimeId>` |
| `--query <ref> [prop]` | Output an element, or one property, by positional `eN` id, durable key, or `uia:<RuntimeId>` |
| `--uia` | Emit the UI Automation tree instead of the visual tree |
| `--uia-view <view>` | UIA tree view: `control` (default), `raw`, or `content` |
| `--uia-props <list>` | Comma-separated extra UIA properties to include |
| `--uia-timeout <ms>` | Deadline for the UIA walk (default: 10000, `0` = none) |
| `--frameworks` | Just list detected frameworks |
| `--depth <n>` | Max tree traversal depth |

## UI Automation tree (`--uia`)

The visual tree answers *"what is this UI made of?"*. `--uia` answers *"how do I
drive it?"* — it emits the target's UI Automation tree, so every element carries
the identifiers an automation client needs:

```powershell
# Automation-grade view of an app
lvt --name myapp --uia

# Everything UIA can see, including framework scaffolding
lvt --name myapp --uia --uia-view raw

# Add properties that are not in the default set
lvt --name myapp --uia --uia-props ProviderDescription,IsPassword

# Address an element by its UIA RuntimeId
lvt --name myapp --uia --query uia:42.3150138.4.5 AutomationId
```

Elements report `AutomationId`, `ControlType`, `LocalizedControlType`,
`FrameworkId`, `RuntimeId`, interaction state (`IsEnabled`, `IsOffscreen`,
`HasKeyboardFocus`, …), the `SupportedPatterns` list, and the state belonging to
those patterns (`Value.Value`, `Toggle.ToggleState`, `ExpandCollapse.State`,
`RangeValue.*`, `Scroll.*`, …).

Pattern-backed properties are **only emitted where the pattern is supported**.
UIA will otherwise tell you a `Window`'s `Toggle.ToggleState` is
`Indeterminate` — not because it has one, but because `GetCachedPropertyValue`
substitutes the type's default for unsupported properties. lvt reads them with
`ignoreDefaultValue` so UIA reports "not supported" instead, and a `Button`
shows `SupportedPatterns="Invoke,…"` with no toggle state while a `CheckBox`
shows `Toggle.ToggleState="On"`.

Everything that works on the visual tree works here: `eN` ids, durable keys,
`--element`, `--query`, `--depth`, `--watch`, `--format xml`, and annotated
screenshots.

### Works across architectures

`--uia` injects nothing into the target, so unlike the visual-tree providers it
is not restricted to processes matching lvt's own architecture:

```powershell
# x64 lvt.exe reading a 32-bit process — refused for the visual tree, fine for UIA
lvt --pid 51748 --uia
```

### Relationship to the visual tree

`--uia` **replaces** the visual tree rather than enriching it; the two are
separate views of the same window, and the visual tree still never depends on
UIA. Use the visual tree for framework-native structure and internals, and
`--uia` for automation identity and actionable state.

## Output format

### Watch mode

`--watch` repeatedly rebuilds the target tree and writes newline-delimited JSON
events to stdout. The first tick emits the current tree as `added` events; later
ticks emit `added`, `removed`, and `changed` events with old/new field values.
Element matching uses stable framework/type/class/path-derived keys instead of
the positional `e0`, `e1`, ... ids, so unique moved elements are reported as
`changed` events with a `path` field change.

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
- **WpfProvider** — walks WPF visual trees via managed DLL injection
- **Plugins** — extensible framework support (e.g. [Avalonia](avalonia-plugin.md)) via C ABI plugin interface

See [docs/architecture.md](docs/architecture.md) for details.

## Design principles

- **Framework-native by default** — the visual tree uses framework-native APIs directly for speed and accuracy, and never depends on UI Automation. `--uia` is an opt-in, automation-grade *second* view, not a replacement for that
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

## Plugin system

lvt supports a plugin architecture for adding new framework providers. Plugins are DLLs that implement a simple C interface and are loaded automatically from `%USERPROFILE%\.lvt\plugins\`.

See [src/plugin.h](src/plugin.h) for the plugin interface.

### Optional plugins

| Plugin | Framework | Docs |
|--------|-----------|------|
| **Avalonia** | [Avalonia UI](https://avaloniaui.net/) desktop apps | [docs/avalonia-plugin.md](docs/avalonia-plugin.md) |
| **Chromium** | Chrome/Edge browser DOM trees | [docs/chromium-plugin.md](docs/chromium-plugin.md) |

These plugins are built from source alongside lvt and deployed to `%USERPROFILE%\.lvt\plugins\`. See each plugin's documentation for installation and usage details.

## Future work

- WebView2 provider
- MAUI provider

## License

MIT — see [LICENSE](LICENSE).
