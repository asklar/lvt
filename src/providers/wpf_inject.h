#pragma once
#include "../element.h"
#include <Windows.h>
#include <string>

namespace lvt {

// Inject the WPF TAP DLL into a target process via CreateRemoteThread+LoadLibrary,
// collect the WPF visual tree via the managed WpfTreeWalker, and graft it into
// the element tree.
// Returns true if the tree was successfully enriched.
bool inject_and_collect_wpf_tree(Element& root, HWND hwnd, DWORD pid);

} // namespace lvt
