#pragma once

#include <Windows.h>

struct ManagedTapHostConfig {
    const wchar_t* managedAssemblyName;
    const wchar_t* frameworkTypeName;
    const wchar_t* coreTypeName;
    const wchar_t* frameworkMethodName;
    const wchar_t* coreMethodName;
    const wchar_t* sidecarStem;
    const wchar_t* logFileName;
};

bool StartManagedTapHost(HMODULE module, const ManagedTapHostConfig& config);
