#pragma once

#include <cstdint>

namespace lvt::managed_host_abi {

using hostfxr_handle = void*;

struct hostfxr_initialize_parameters;

enum class hostfxr_delegate_type : int32_t {
    load_assembly_and_get_function_pointer = 5,
};

using hostfxr_initialize_for_runtime_config_fn = int32_t(__cdecl*)(
    const wchar_t* runtimeConfigPath,
    const hostfxr_initialize_parameters* parameters,
    hostfxr_handle* hostContext);

using hostfxr_get_runtime_delegate_fn = int32_t(__cdecl*)(
    hostfxr_handle hostContext,
    hostfxr_delegate_type delegateType,
    void** delegate);

using hostfxr_close_fn = int32_t(__cdecl*)(hostfxr_handle hostContext);

using load_assembly_and_get_function_pointer_fn = int(__stdcall*)(
    const wchar_t* assemblyPath,
    const wchar_t* typeName,
    const wchar_t* methodName,
    const wchar_t* delegateTypeName,
    void* reserved,
    void** delegate);

// Official default managed component entry point returned when
// delegate_type_name is null.
using component_entry_point_fn = int(__stdcall*)(
    void* arguments, int32_t argumentBytes);

} // namespace lvt::managed_host_abi
