#pragma once
#include "provider.h"
#include "framework_connection.h"

#include <memory>

namespace lvt {

class WinFormsProvider : public IProvider {
public:
    std::shared_ptr<IFrameworkConnection> open_connection(HWND hwnd, DWORD pid);
    bool enrich_with_connection(Element& root, IFrameworkConnection& connection);
    bool enrich(Element& root, HWND hwnd, DWORD pid);
};

} // namespace lvt
