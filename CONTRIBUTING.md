# Contributing to lvt

## Getting started

### Prerequisites

- Visual Studio 2022 or later with the **C++ Desktop Development** workload
- [vcpkg](https://vcpkg.io) installed and `VCPKG_ROOT` environment variable set
- CMake 3.20+
- An **x64 Developer Command Prompt** (or equivalent environment)

### Building

```powershell
cmake --preset default
cmake --build build
```

This produces `build/lvt.exe` and `build/lvt_tap_x64.dll`.

For ARM64:

```powershell
cmake --preset arm64
cmake --build build-arm64
```

### Running tests

```powershell
# Unit tests (no live app required)
build\lvt_unit_tests.exe

# Integration tests (launches Notepad)
build\lvt_integration_tests.exe
```

## Project structure

```
src/
  main.cpp                    CLI entry point, argument parsing
  target.h/.cpp               Target acquisition (HWND/PID/name/title resolution)
  framework_detector.h/.cpp   Detect UI frameworks via loaded DLLs
  tree_builder.h/.cpp         Orchestrate providers, assign element IDs
  element.h                   Element data model
  json_serializer.h/.cpp      JSON and XML serialization
  screenshot.h/.cpp           Window capture + annotation overlay
  providers/
    provider.h                Abstract provider interface
    win32_provider.h/.cpp     Win32 HWND enumeration
    comctl_provider.h/.cpp    Common Controls enrichment
    xaml_provider.h/.cpp      Windows XAML (UWP) via TAP DLL
    winui3_provider.h/.cpp    WinUI 3 via TAP DLL
    xaml_diag_common.h/.cpp   Shared XAML injection/pipe/grafting logic
  tap/
    lvt_tap.cpp               TAP DLL (injected into target process)
    lvt_tap.def               DLL export definitions
    tap_clsid.h               Shared CLSID for the TAP COM class
tests/
  unit_tests.cpp              GoogleTest unit tests
  integration_tests.cpp       GoogleTest integration tests (require Notepad)
docs/
  architecture.md             Detailed architecture documentation
  tap-dll-design.md           TAP DLL design and threading model
```

## Key conventions

### No UI Automation

We deliberately avoid UIA. It is slow, unreliable, and hard to use correctly. Each provider talks to the framework's native APIs directly.

### Static CRT for TAP DLL

`lvt_tap.dll` uses `/MT` (static CRT linking) to avoid CRT version conflicts when injected into arbitrary processes. Do not change this.

### XAML string sanitization

The XAML runtime returns type names with embedded control characters. All strings from XAML must be sanitized (strip chars < 0x20) before use in output.

### TAP DLL rebuilds

After `lvt_tap.dll` is injected into a target process, the file is locked. You must kill the target app before rebuilding the TAP DLL.

### Threading in TAP DLL

`GetPropertyValuesChain` has strict thread affinity — it must run on the XAML UI thread. The TAP DLL uses a message-only window + `SendMessage` pattern to dispatch these calls. See [docs/tap-dll-design.md](docs/tap-dll-design.md).

## Adding a new provider

1. Create `src/providers/myframework_provider.h/.cpp`
2. Implement the enrichment logic (walk the framework's native tree, add/replace elements)
3. Add the framework enum value to `Framework` in `framework_detector.h`
4. Add detection logic in `framework_detector.cpp` (check for loaded DLLs, window classes, etc.)
5. Wire it up in `tree_builder.cpp`'s `build_tree()` switch statement
6. Add the new source files to `CMakeLists.txt` (both `lvt` and `lvt_unit_tests` targets)
7. Add tests

## Code style

- C++20
- No exceptions in TAP DLL code (use SEH or error codes)
- Prefer WIL smart pointers where available
- Keep comments minimal — only where behavior is non-obvious
- Use `static` for file-scope helpers

## Pull requests

- Keep changes focused and minimal
- Add tests for new functionality
- Ensure `lvt_unit_tests` passes before submitting
- Integration tests may fail in CI (they need a desktop session with Notepad)
