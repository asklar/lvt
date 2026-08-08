// Compile-only guard for the installed-library contract.
//
// This target links lvt_core and adds NO include directories of its own, so it
// sees exactly what an external consumer of lvt::core sees: the interface
// include directories lvt_core propagates. If a public header starts using a
// dependency that lvt_core links PRIVATE (e.g. plugin_loader.h exposing
// wil::unique_hmodule while WIL::WIL is PRIVATE), this fails to compile here
// instead of in a downstream project.

#include "element.h"
#include "element_key.h"
#include "framework_detector.h"
#include "json_serializer.h"
#include "module_util.h"
#include "plugin.h"
#include "plugin_loader.h"
#include "screenshot.h"
#include "target.h"
#include "tree_builder.h"
#include "watch_diff.h"

// Touch the types that carry dependency-provided members across the ABI.
static_assert(sizeof(lvt::Element) > 0, "Element must be a complete type");
static_assert(sizeof(lvt::LoadedPlugin) > 0, "LoadedPlugin must be a complete type");

int main() {
    return 0;
}
