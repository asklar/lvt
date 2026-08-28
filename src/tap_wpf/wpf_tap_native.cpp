#include "../tap_managed/managed_tap_host.h"

#include <Windows.h>

namespace {

const ManagedTapHostConfig kConfig{
    L"LvtWpfTap.dll",
    L"LvtWpfTap.WpfTreeWalker",
    L"LvtWpfTap.WpfTreeWalker, LvtWpfTap",
    L"LvtWpfTap.WpfTreeWalker+RunServerDelegate, LvtWpfTap",
    L"RunServer",
    L"lvt_wpf_pipe",
    L"lvt_wpf_tap.log",
};

} // namespace

extern "C" BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        StartManagedTapHost(module, kConfig);
    }
    return TRUE;
}
