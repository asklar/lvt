# Copilot Instructions for lvt

## What is lvt?

lvt (Live Visual Tree) is a Windows C++20 CLI tool that inspects the visual tree of running Windows applications. It targets apps by HWND, PID, process name, or window title, detects UI frameworks (Win32, ComCtl, XAML, WinUI3), and outputs a unified element tree as JSON or XML markup. It also captures annotated PNG screenshots. The primary consumer is AI agents (e.g. Copilot) that need a textual representation of an app's UI.

## Build

Requires an **x64 Visual Studio Developer Command Prompt** (VS 2022+) and `VCPKG_ROOT` set.

```powershell
# One-time configure
cmake --preset default

# Build (always x64)
cmake --build build
```

Produces `build/lvt.exe` and `build/lvt_tap.dll`. There are no tests or linters currently.

**Important:** `lvt_tap.dll` gets locked by target processes after injection. You must kill the target app before rebuilding the TAP DLL.

## Architecture

### Pipeline

`main.cpp` orchestrates a 4-stage pipeline:

1. **Target resolution** (`target.cpp`) — resolve `--hwnd`/`--pid`/`--name`/`--title` to an HWND+PID
2. **Framework detection** (`framework_detector.cpp`) — enumerate loaded DLLs in the target process to detect Win32, ComCtl, XAML, WinUI3 with versions
3. **Tree building** (`tree_builder.cpp`) — Win32 HWND walk is the base layer; framework-specific providers enrich/overlay it
4. **Serialization** (`json_serializer.cpp`) — output as JSON or XML; screenshot capture (`screenshot.cpp`) is optional

### Provider layering

The tree always starts with the Win32 provider (HWND enumeration). Framework providers layer on top:

- **Win32Provider** — base HWND tree with class names, text, bounds, styles
- **ComCtlProvider** — enriches known ComCtl32 controls (ListView items, TreeView nodes, etc.)
- **XamlProvider** / **WinUI3Provider** — inject the TAP DLL to walk XAML visual trees, then graft XAML subtrees into matching `DesktopChildSiteBridge` elements in the Win32 tree

Providers live in `src/providers/`. Each has a header declaring its public API.

### TAP DLL injection (src/tap/)

XAML tree walking requires injecting `lvt_tap.dll` into the target process via `InitializeXamlDiagnosticsEx`. The TAP DLL:

- Implements `IObjectWithSite` + `IVisualTreeServiceCallback2` as a COM class
- Receives `IXamlDiagnostics` via `SetSite`, QIs for `IVisualTreeService`
- Calls `AdviseVisualTreeChange` which replays the existing tree synchronously
- Serializes the tree as JSON and sends it back to lvt.exe over a named pipe

Shared injection/pipe/grafting logic lives in `xaml_diag_common.cpp`.

### Element model

`Element` (in `element.h`) is the unified node type across all frameworks. Elements get deterministic IDs (`e0`, `e1`, ...) assigned in depth-first order by `assign_element_ids()`. These IDs are stable within a single tree walk and are used for `--element` scoping and screenshot annotations.

## Key conventions

- **No UIA** — the tool uses framework-native APIs directly, never UI Automation
- **x64 only** for `lvt.exe` itself; bitness matching is only needed for injected DLLs into the target process
- **Static CRT for TAP DLL** — `lvt_tap.dll` uses `/MT` (static CRT) to avoid CRT version conflicts when injected into arbitrary processes
- **XAML type name sanitization** — the XAML runtime returns type names with embedded control characters (e.g. literal `\n`). All strings from XAML must be sanitized (strip chars < 0x20) before serialization
- **Connection name iteration** — system XAML uses `"VisualDiagConnection1"`, `"VisualDiagConnection2"`, etc.; WinUI3 uses `"WinUIVisualDiagConnection1"`, etc. Must try names until one doesn't return `ERROR_NOT_FOUND`
- **Bridge-to-XAML matching** — `DesktopChildSiteBridge` elements (Win32 tree) map 1:1 to `DesktopWindowXamlSource` roots (XAML tree), matched by enumeration order
- **GDI alpha fix** — GDI drawing functions don't set alpha. After any GDI annotation on screenshots, all pixel alpha bytes must be set to 255
- **TAP DLL threading** — `SetSite` runs on the XAML UI thread. Never block it (no `WaitForSingleObject`). Use fire-and-forget worker threads for tree collection
- **TAP DLL lifetime** — do NOT call `FreeLibrary(GetCurrentModuleHandle())` in the TAP DLL. Unlike Windhawk (which has 2 LoadLibrary refs), our DLL only has 1 ref from `InitializeXamlDiagnosticsEx`
- **Debug logging** — TAP DLL logs to `%TEMP%\lvt_tap.log` since OutputDebugString may not be visible. Use `C:\Debuggers\cdb.exe` for debugging injection issues

## CLI usage

```
lvt --name notepad --format xml --element e2 --depth 5
lvt --pid 1234 --screenshot out.png
lvt --hwnd 0x1A0B3C --frameworks
```

## Dependencies

Managed via vcpkg: `wil` (Windows Implementation Library), `nlohmann-json`. System libs: `dwmapi`, `d3d11`, `dxgi`, `windowsapp`, `windowscodecs`.
