#pragma once

#include "framework_connection.h"

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>

namespace lvt {

struct UiaSelectionCapabilities {
    bool known = false;
    bool canSelectMultiple = false;
    bool isSelectionRequired = true;
};

// Immutable UIA property schemas are shared by controls with equivalent
// provider capabilities. Live values remain in PropertySnapshotResult and are
// never retained by this cache.
class UiaPropertySchemaCache {
public:
    std::shared_ptr<const PropertySchema> get_or_create(
        const Element& element,
        const UiaSelectionCapabilities& selection);

    size_t size() const { return m_schemas.size(); }

private:
    std::unordered_map<std::string, std::shared_ptr<const PropertySchema>> m_schemas;
};

PropertySnapshotResult make_uia_property_snapshot(
    const Element& element,
    const UiaSelectionCapabilities& selection,
    UiaPropertySchemaCache& cache);

} // namespace lvt
