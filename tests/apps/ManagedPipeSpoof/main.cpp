#include <Windows.h>

#include <string>

int wmain(int argc, wchar_t** argv) {
    if (argc != 2)
        return 2;

    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 50; ++attempt) {
        pipe = CreateFileW(
            argv[1], GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE)
            break;
        if (GetLastError() != ERROR_PIPE_BUSY)
            return 3;
        WaitNamedPipeW(argv[1], 100);
    }
    if (pipe == INVALID_HANDLE_VALUE)
        return 4;

    const char forged[] =
        "READY\t{\"protocol\":1,\"connectionId\":\"forged\","
        "\"assemblyInstanceId\":\"forged\",\"serverStartCount\":1,"
        "\"commands\":[\"GET_TREE\"]}\n";
    DWORD written = 0;
    WriteFile(
        pipe, forged, static_cast<DWORD>(sizeof(forged) - 1),
        &written, nullptr);

    char byte = 0;
    DWORD read = 0;
    ReadFile(pipe, &byte, 1, &read, nullptr);
    CloseHandle(pipe);
    return 0;
}
