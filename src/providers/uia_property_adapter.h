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
    static constexpr size_t kMaximumSchemas = 256;

    std::shared_ptr<const PropertySchema> get_or_create(
        const Element& element,
        const UiaSelectionCapabilities& selection);

    size_t size() const { return m_schemas.size(); }

private:
    struct CachedSchema {
        std::shared_ptr<const PropertySchema> schema;
        uint64_t lastUsed = 0;
    };

    void trim();

    uint64_t m_clock = 0;
    std::unordered_map<std::string, CachedSchema> m_schemas;
};

PropertySnapshotResult make_uia_property_snapshot(
    const Element& element,
    const UiaSelectionCapabilities& selection,
    UiaPropertySchemaCache& cache);

} // namespace lvt
