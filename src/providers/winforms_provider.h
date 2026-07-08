#pragma once
#include "provider.h"

namespace lvt {

class WinFormsProvider : public IProvider {
public:
    void enrich(Element& root, HWND hwnd, DWORD pid);
};

} // namespace lvt
