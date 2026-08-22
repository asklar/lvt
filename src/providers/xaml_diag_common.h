#pragma once
#include "../element.h"
#include <Windows.h>
#include <string>

namespace lvt {

// Inject the TAP DLL into a target process using InitializeXamlDiagnosticsEx,
// collect the XAML visual tree, and graft it into the element tree.
// `xamlDiagDll` is passed as wszDllXamlDiagnostics to the init function.
// `initDllPath` is the DLL to load InitializeXamlDiagnosticsEx from
//   (e.g. L"Windows.UI.Xaml.dll" or full path to FrameworkUdk.dll).
// `connPrefix` is the connection endpoint name prefix to use
//   (e.g. L"VisualDiagConnection" for system XAML, L"WinUIVisualDiagConnection" for WinUI3).
// `fastProperties`, when true, tells the TAP DLL to skip
//   IVisualTreeService::GetPropertyValuesChain (the dominant per-element cost
//   of a rich tree, ~4.5ms/element measured live) and collect bounds/Text/
//   Content the cheaper way CollectPositionsAndText already used for
//   position: direct WinRT property reads on an already-obtained
//   IInspectable. Trades the exhaustive per-element property set (custom
//   DPs, anything outside Text/Content/bounds) for speed — see lvt_tap.cpp's
//   CollectBounds/CollectPositionsAndText for exactly what is and isn't
//   captured in this mode.
// Returns true if the tree was successfully enriched.
bool inject_and_collect_xaml_tree(
    Element& root,
    HWND hwnd,
    DWORD pid,
    const std::wstring& xamlDiagDll,
    const std::wstring& initDllPath,
    const std::string& frameworkLabel,
    const std::wstring& connPrefix = L"VisualDiagConnection",
    bool fastProperties = false);

} // namespace lvt
