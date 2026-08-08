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

The visual tree deliberately avoids UIA. It is slow, unreliable, and hard to use correctly. Each provider talks to the framework's native APIs directly.

`--uia` (`src/providers/uia_provider.cpp`) is the deliberate, opt-in exception: a *separate* automation-grade view for callers that need `AutomationId`s and patterns. It never participates in building the visual tree, so the principle above still holds for everything else.

Two rules matter when working on it:

- **Batch through the cache.** UIA properties are cross-process calls. Always fetch via `IUIAutomationCacheRequest` (`TreeScope_Subtree` + `AddProperty`/`AddPattern`, then `GetCachedChildren`/`GetCachedPropertyValue`). Reading live properties per node turns a fast walk into a multi-second one.
- **Gate pattern-backed properties on pattern support.** UIA answers them on every element regardless, so an ungated walk reports a `Window`'s `Toggle.ToggleState`. `uia_property_owner_pattern()` in `uia_props.cpp` is what prevents that; new pattern-backed properties must declare their owner.

### UIA runs on its own MTA thread

UIA clients want an MTA, but `screenshot.cpp` initializes an STA on the calling thread and a thread cannot be both. All UIA work is marshalled onto a dedicated MTA thread (`run_on_mta` in `uia_provider.cpp`); calling it inline yields `RPC_E_CHANGED_MODE`.

### Static CRT for TAP DLL

`lvt_tap.dll` uses `/MT` (static CRT linking) to avoid CRT version conflicts when injected into arbitrary processes. Do not change this.

### XAML string sanitization

The XAML runtime returns type names with embedded control characters. All strings from XAML must be sanitized (strip chars < 0x20) before use in output.

### TAP DLL rebuilds

After `lvt_tap.dll` is injected into a target process, the file is locked. You must kill the target app before rebuilding the TAP DLL.

### Threading in TAP DLL

`GetPropertyValuesChain` has strict thread affinity — it must run on the XAML UI thread. The TAP DLL uses a message-only window + `SendMessage` pattern to dispatch these calls. See [docs/tap-dll-design.md](docs/tap-dll-design.md).

### Resource management: WIL everywhere

No raw COM pointers, no raw handles, no manual `Release()` / `CloseHandle()` / `FreeLibrary()` / `SysFreeString()` / `CoTaskMemFree()` / `VariantClear()` / `CoUninitialize()`.

| Use | Type |
|-----|------|
| COM interfaces | `wil::com_ptr<T>` |
| Win32 handles | `wil::unique_handle`, `wil::unique_hmodule`, `wil::unique_hfile`, `wil::unique_event`, … |
| GDI objects | `wil::unique_hdc`, `wil::unique_hbitmap`, `wil::unique_hgdiobj` |
| `BSTR` / `CoTaskMem` / `VARIANT` | `wil::unique_bstr`, `wil::unique_cotaskmem`, `wil::unique_variant` |
| `CoInitializeEx` | `wil::unique_couninitialize_call` |
| Arbitrary scope cleanup | `wil::scope_exit` |

`CoInitializeEx` is a common trap: pairing it with a `bool needUninit` and a single `CoUninitialize()` at the end leaks the apartment on every early return. Use `wil::unique_couninitialize_call`, `release()`d if the init itself failed.

### Error handling: WIL result macros

Internal helpers return `HRESULT` and use the macros; public boundaries keep their `bool` / `std::optional` / `Element` signatures and convert at the edge:

```cpp
static HRESULT do_work_hr(...) {
    RETURN_IF_FAILED(factory->CreateStream(&stream));
    RETURN_HR_IF(E_INVALIDARG, width <= 0);
    return S_OK;
}

static bool do_work(...) {
    return SUCCEEDED(LOG_IF_FAILED(do_work_hr(...)));
}
```

This is not cosmetic: a bare `return false` discards the failing `HRESULT` and its location, whereas the macros report the originating file, line, function, and expression.

- `lvt::install_wil_result_logger()` (in `wil_diagnostics.h`) routes those reports to **stderr**, gated on `lvt::g_debug` (`--debug`).
- **Never write diagnostics to stdout.** stdout carries machine-readable payloads — the JSON/XML tree today, and more later. `tests/unit_tests.cpp` has `WilResultLogger` tests that enforce this.
- At any `extern "C"` boundary, wrap the body in `CATCH_RETURN()` / `CATCH_LOG()` so exceptions never escape into a foreign runtime.

### Public headers and dependency visibility

Headers listed in `LVT_PUBLIC_HEADERS` are installed and consumed externally. If one uses a type from a dependency, that dependency must be linked `PUBLIC` on `lvt_core`, or consumers fail with "Cannot open include file". `lvt_public_headers_test` is a compile-only target that links `lvt_core` with no include directories of its own, so it sees exactly what an external consumer sees and catches this at build time.

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
- Use WIL smart pointers and result macros — see [Key conventions](#key-conventions)
- Keep comments minimal — only where behavior is non-obvious
- Use `static` for file-scope helpers

## Pull requests

- Keep changes focused and minimal
- Add tests for new functionality
- Ensure `lvt_unit_tests` passes before submitting
- Integration tests may fail in CI (they need a desktop session with Notepad)
