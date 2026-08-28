#include "native_message.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <utility>

namespace lvt {
namespace {

NativeMessageResult success(LRESULT value = 0) {
    NativeMessageResult result;
    result.ok = true;
    result.hresult = S_OK;
    result.win32Error = ERROR_SUCCESS;
    result.value = value;
    return result;
}

std::string win32_error_message(DWORD error) {
    wchar_t* buffer = nullptr;
    const DWORD count = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    if (!count || !buffer)
        return "Win32 error " + std::to_string(error);
    std::wstring text(buffer, count);
    LocalFree(buffer);
    while (!text.empty() &&
           (text.back() == L'\r' || text.back() == L'\n' ||
            text.back() == L' ' || text.back() == L'.')) {
        text.pop_back();
    }
    auto converted = native_utf16_to_utf8(text);
    return converted.empty()
        ? "Win32 error " + std::to_string(error)
        : converted;
}

NativeMessageResult failure(DWORD error, std::string context) {
    if (error == ERROR_SUCCESS)
        error = ERROR_GEN_FAILURE;
    NativeMessageResult result;
    result.win32Error = error;
    result.hresult = HRESULT_FROM_WIN32(error);
    result.timedOut = error == ERROR_TIMEOUT;
    result.error = std::move(context) + ": " + win32_error_message(error);
    return result;
}

} // namespace

std::string normalize_native_class_name(std::string_view className) {
    std::string normalized(className);
    std::transform(
        normalized.begin(), normalized.end(), normalized.begin(),
        [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
    return normalized;
}

NativeMessageResult capture_native_window_identity(
    HWND hwnd, DWORD expectedPid, NativeWindowIdentity& identity) {
    if (!hwnd || !IsWindow(hwnd))
        return failure(ERROR_INVALID_WINDOW_HANDLE, "The native window is closed");

    DWORD pid = 0;
    if (!GetWindowThreadProcessId(hwnd, &pid) || pid == 0)
        return failure(GetLastError(), "Could not identify the native window owner");
    if (expectedPid != 0 && pid != expectedPid) {
        return failure(
            ERROR_INVALID_WINDOW_HANDLE,
            "The native window handle now belongs to a different process");
    }

    wchar_t className[256]{};
    const int copied =
        GetClassNameW(hwnd, className, static_cast<int>(_countof(className)));
    if (copied <= 0)
        return failure(GetLastError(), "Could not read the native window class");

    identity.hwnd = hwnd;
    identity.pid = pid;
    identity.normalizedClass = normalize_native_class_name(
        native_utf16_to_utf8(std::wstring_view(className, copied)));
    return success();
}

NativeMessageResult validate_native_window(
    const NativeWindowIdentity& identity) {
    NativeWindowIdentity current;
    auto result =
        capture_native_window_identity(identity.hwnd, identity.pid, current);
    if (!result.ok)
        return result;
    if (current.normalizedClass != identity.normalizedClass) {
        return failure(
            ERROR_INVALID_WINDOW_HANDLE,
            "The native window class changed; the handle may have been reused");
    }
    return success();
}

NativeMessageResult send_native_message(
    const NativeWindowIdentity& identity, UINT message,
    WPARAM wParam, LPARAM lParam, UINT timeoutMs) {
    auto valid = validate_native_window(identity);
    if (!valid.ok)
        return valid;

    DWORD_PTR value = 0;
    SetLastError(ERROR_SUCCESS);
    const LRESULT sent = SendMessageTimeoutW(
        identity.hwnd, message, wParam, lParam,
        SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, timeoutMs, &value);
    if (!sent) {
        DWORD error = GetLastError();
        if (error == ERROR_SUCCESS)
            error = ERROR_TIMEOUT;
        return failure(
            error,
            error == ERROR_TIMEOUT
                ? "The native control did not answer before the timeout"
                : "The native control message failed");
    }
    return success(static_cast<LRESULT>(value));
}

bool native_pointer_operations_allowed(
    Architecture host, Architecture target) {
    return host != Architecture::unknown &&
           target != Architecture::unknown &&
           host == target;
}

std::string native_utf16_to_utf8(std::wstring_view value) {
    if (value.empty())
        return {};
    if (value.size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
        return {};
    const int size = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0)
        return {};
    std::string converted(static_cast<size_t>(size), '\0');
    if (!WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), converted.data(), size,
            nullptr, nullptr)) {
        return {};
    }
    return converted;
}

bool native_utf8_to_utf16(
    std::string_view value, std::wstring& converted, std::string& error) {
    converted.clear();
    if (value.empty())
        return true;
    if (value.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) {
        error = "The string is too large";
        return false;
    }
    const int size = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) {
        error = "The value is not valid UTF-8";
        return false;
    }
    converted.resize(static_cast<size_t>(size));
    if (!MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), converted.data(), size)) {
        error = "The value is not valid UTF-8";
        converted.clear();
        return false;
    }
    return true;
}

RemoteBuffer::~RemoteBuffer() {
    reset();
}

RemoteBuffer::RemoteBuffer(RemoteBuffer&& other) noexcept
    : m_process(std::exchange(other.m_process, nullptr)),
      m_address(std::exchange(other.m_address, nullptr)),
      m_size(std::exchange(other.m_size, 0)) {}

RemoteBuffer& RemoteBuffer::operator=(RemoteBuffer&& other) noexcept {
    if (this != &other) {
        reset();
        m_process = std::exchange(other.m_process, nullptr);
        m_address = std::exchange(other.m_address, nullptr);
        m_size = std::exchange(other.m_size, 0);
    }
    return *this;
}

RemoteBuffer RemoteBuffer::allocate(
    const NativeWindowIdentity& identity, size_t size,
    NativeMessageResult& result) {
    auto valid = validate_native_window(identity);
    if (!valid.ok) {
        result = std::move(valid);
        return {};
    }
    if (size == 0) {
        result = failure(ERROR_INVALID_PARAMETER, "Remote buffer size is zero");
        return {};
    }

    HANDLE process = OpenProcess(
        PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE,
        FALSE, identity.pid);
    if (!process) {
        result = failure(
            GetLastError(),
            "Could not open the target process for remote control memory");
        return {};
    }
    void* address = VirtualAllocEx(
        process, nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!address) {
        const DWORD error = GetLastError();
        CloseHandle(process);
        result = failure(error, "Could not allocate remote control memory");
        return {};
    }
    result = success();
    return RemoteBuffer(process, address, size);
}

bool RemoteBuffer::write(
    const void* data, size_t size, NativeMessageResult& result,
    size_t offset) const {
    if (!m_address || !data || offset > m_size || size > m_size - offset) {
        result = failure(ERROR_INVALID_PARAMETER, "Invalid remote buffer write");
        return false;
    }
    SIZE_T written = 0;
    if (!WriteProcessMemory(
            m_process, static_cast<std::byte*>(m_address) + offset,
            data, size, &written) ||
        written != size) {
        result = failure(
            GetLastError(), "Could not write remote control memory");
        return false;
    }
    result = success();
    return true;
}

bool RemoteBuffer::read(
    void* data, size_t size, NativeMessageResult& result,
    size_t offset) const {
    if (!m_address || !data || offset > m_size || size > m_size - offset) {
        result = failure(ERROR_INVALID_PARAMETER, "Invalid remote buffer read");
        return false;
    }
    SIZE_T read = 0;
    if (!ReadProcessMemory(
            m_process, static_cast<const std::byte*>(m_address) + offset,
            data, size, &read) ||
        read != size) {
        result = failure(
            GetLastError(), "Could not read remote control memory");
        return false;
    }
    result = success();
    return true;
}

void RemoteBuffer::reset() {
    if (m_address && m_process)
        VirtualFreeEx(m_process, m_address, 0, MEM_RELEASE);
    if (m_process)
        CloseHandle(m_process);
    m_process = nullptr;
    m_address = nullptr;
    m_size = 0;
}

} // namespace lvt
