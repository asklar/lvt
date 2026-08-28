#pragma once

#include "framework_connection.h"

#include <Windows.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lvt {

struct ManagedConnectionOptions {
    std::string frameworkLabel;
    std::wstring tapStem;
    std::wstring managedAssemblyName;
    std::wstring pipePrefix;
    std::wstring sidecarStem;
    DWORD connectTimeoutMs = 15000;
    DWORD commandTimeoutMs = 15000;
};

struct ManagedConnectionCapabilities {
    uint32_t protocolVersion = 0;
    std::string connectionId;
    std::string assemblyInstanceId;
    uint32_t serverStartCount = 0;
    std::vector<std::string> commands;
};

using ManagedTreeApplier = std::function<bool(Element&, const std::string&)>;

std::shared_ptr<IFrameworkConnection> open_managed_framework_connection(
    HWND hwnd, DWORD pid, ManagedConnectionOptions options, ManagedTreeApplier applyTree);

std::optional<ManagedConnectionCapabilities> managed_connection_capabilities(
    IFrameworkConnection& connection);

} // namespace lvt
