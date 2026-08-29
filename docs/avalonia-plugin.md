# Avalonia Plugin for lvt

The Avalonia plugin adds visual tree inspection support for [Avalonia UI](https://avaloniaui.net/) desktop applications.

## How it works

The plugin follows the same DLL injection pattern as the built-in WPF provider:

1. **Detection** — The plugin checks if `Avalonia.Base.dll` is loaded in the target process
2. **Injection** — A native TAP DLL (`lvt_avalonia_tap_x64.dll`) is injected into the target process via `CreateRemoteThread` + `LoadLibraryW`
3. **CLR hosting** — The TAP DLL hosts the .NET runtime via `hostfxr` and loads the managed tree walker assembly (`LvtAvaloniaTreeWalker.dll`)
4. **Tree walking** — The managed code walks the Avalonia visual tree using `Visual.VisualChildren` (via reflection), serializes each element's type, name, bounds, text content, and visibility to JSON
5. **Communication** — The JSON tree is sent back to lvt over a named pipe

## Installation

### From a release

Copy the plugin files to `%USERPROFILE%\.lvt\plugins\`:

```
%USERPROFILE%\.lvt\plugins\
├── lvt_avalonia_plugin.dll          # Plugin DLL (loaded by lvt)
└── avalonia\                        # Subdirectory for TAP + managed DLLs
    ├── lvt_avalonia_tap_x64.dll     # Native TAP DLL (injected into target)
    ├── LvtAvaloniaTreeWalker.dll    # Managed tree walker
    └── LvtAvaloniaTreeWalker.runtimeconfig.json
```

> **Important:** The TAP DLL and managed assembly must be in the `avalonia\` subdirectory, not directly in the `plugins\` directory. This prevents the plugin loader from attempting to load them as plugins.

### From source

```powershell
# Build managed assembly first
dotnet build src/plugin_avalonia/LvtAvaloniaTreeWalker/LvtAvaloniaTreeWalker.csproj -c Release

# Build native components (from VS Developer Command Prompt)
cmake --preset default
cmake --build build
```

The build outputs plugin files to `build/plugins/` which can be copied to `%USERPROFILE%\.lvt\plugins\`.

## Usage

Once installed, the plugin is loaded automatically. No special flags are needed:

```bash
# Inspect an Avalonia app
lvt --name MyAvaloniaApp

# Detect frameworks (should show "avalonia X.Y.Z")
lvt --name MyAvaloniaApp --frameworks

# XML output
lvt --name MyAvaloniaApp --format xml
```

### Example output

```
$ lvt --name AvaloniaTestApp --frameworks
win32
avalonia 11.2.7.0
```

```
$ lvt --name AvaloniaTestApp --format xml --depth 3
<LiveVisualTree hwnd="0x008D18E8" pid="12345" process="AvaloniaTestApp.exe" frameworks="win32,avalonia 11.2.7.0">
  <Window id="e0" framework="win32" text="LVT Avalonia Test" bounds="156,156,416,339">
    <MainWindow id="e1" framework="avalonia" className="AvaloniaTestApp.MainWindow" bounds="156,156,400,300">
      <Panel id="e2" framework="avalonia" className="Avalonia.Controls.Panel" bounds="156,156,400,300">
        ...
      </Panel>
    </MainWindow>
  </Window>
</LiveVisualTree>
```

## Captured element properties

| Property | Source |
|----------|--------|
| `type` | Last segment of the Avalonia type name (e.g. `TextBlock`, `Button`) |
| `className` | Full Avalonia type name (e.g. `Avalonia.Controls.TextBlock`) |
| `text` | Text content from `Text`, `Content`, `Header`, `Title`, or `Watermark` properties |
| `name` | Element's `x:Name` (from `StyledElement.Name`) — used as fallback for `text` |
| `bounds` | Screen coordinates computed via `PointToScreen` |
| `visible` | `false` if `IsVisible` is false (omitted when visible) |
| `enabled` | `false` if `IsEnabled` is false (omitted when enabled) |

## Requirements

- Target Avalonia app must be .NET 6+ (Avalonia 11.x)
- Target process must match lvt's architecture (x86, x64, or ARM64)
- The .NET runtime (`hostfxr.dll`) must be installed on the system

## Persistent connections (optional)

`src/plugin.h`'s plugin ABI (v2) has a persistent lifetime group:
`lvt_connection_open`, `lvt_connection_get_tree`, and
`lvt_connection_close` (plus the existing `lvt_plugin_free`) must all be
implemented before lvt enables the connection path. The optional
`lvt_connection_poll_events`/`lvt_connection_events_free` pair adds push
event draining. Core automatically acquires and reuses a capable plugin in
`watch` and MCP, forwards the same provider option/filter on every tree
request, reconnects a dead handle, and closes it when the session ends. It
does not silently switch a v2-capable session back to repeated one-shot
injection after a connection failure. This plugin does not implement the
group yet, so it continues to use `lvt_enrich_tree` on every refresh
unchanged.

Persistent implementations must return an object or array of object nodes in
the documented tree schema. Field types, nesting, and node counts are
validated before grafting. Event arrays are bounded and every event must
report the current `LvtConnectionEvent::struct_size`; core frees plugin-owned
tree and event allocations on every path, including malformed responses.

## Test app

A simple Avalonia test application is included in `tests/avalonia_test_app/`:

```powershell
cd tests/avalonia_test_app
dotnet run
```

This creates a window with a TextBlock, Button, and TextBox — useful for verifying the plugin works correctly.
