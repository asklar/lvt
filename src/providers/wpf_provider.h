#pragma once
#include "provider.h"
#include "framework_connection.h"

#include <memory>

namespace lvt {

class WpfProvider : public IProvider {
public:
    std::shared_ptr<IFrameworkConnection> open_connection(HWND hwnd, DWORD pid);
    void enrich_with_connection(Element& root, IFrameworkConnection& connection);
    void enrich(Element& root, HWND hwnd, DWORD pid);
};

} // namespace lvt
