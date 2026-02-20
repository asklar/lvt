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
// Returns true if the tree was successfully enriched.
bool inject_and_collect_xaml_tree(
    Element& root,
    HWND hwnd,
    DWORD pid,
    const std::wstring& xamlDiagDll,
    const std::wstring& initDllPath,
    const std::string& frameworkLabel,
    const std::wstring& connPrefix = L"VisualDiagConnection");

} // namespace lvt
