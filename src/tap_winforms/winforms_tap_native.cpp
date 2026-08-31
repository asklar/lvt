#include "../tap_managed/managed_tap_host.h"

#include <Windows.h>

namespace {

const ManagedTapHostConfig kConfig{
    L"LvtWinFormsTap.dll",
    L"LvtWinFormsTap.WinFormsTreeWalker",
    L"LvtWinFormsTap.WinFormsTreeWalker, LvtWinFormsTap",
    L"RunServer",
    L"RunServerCore",
    L"lvt_winforms_pipe",
    L"lvt_winforms_tap.log",
};

} // namespace

extern "C" BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        StartManagedTapHost(module, kConfig);
    }
    return TRUE;
}
