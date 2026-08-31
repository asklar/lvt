#include <Windows.h>

#include <cwchar>

extern "C" BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH)
        return TRUE;

    DisableThreadLibraryCalls(module);

    // Test-only: signal after entering DllMain, then deliberately retain the
    // process loader lock past the Avalonia plugin's diagnostic threshold.
    wchar_t eventName[96]{};
    swprintf_s(
        eventName, L"Local\\LvtLoaderLockBlocker_%lu",
        GetCurrentProcessId());
    HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, eventName);
    if (ready) {
        SetEvent(ready);
        CloseHandle(ready);
    }

    Sleep(8000);
    // Make LoadLibraryW fail so this helper does not remain loaded afterward.
    return FALSE;
}
