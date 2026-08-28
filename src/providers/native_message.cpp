#include "native_message.h"

#include <CommCtrl.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace lvt {
namespace {

constexpr size_t kMaximumDeferredPointerMessages = 64;
std::atomic<size_t> g_pointerMessageSlots{0};
std::mutex g_permanentRetirementMutex;
struct PermanentRetirement {
    HANDLE process = nullptr;
    std::shared_ptr<void> keepAlive;
};
std::vector<PermanentRetirement> g_permanentRetirements;

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

bool reserve_pointer_message_slot() {
    size_t current = g_pointerMessageSlots.load(std::memory_order_relaxed);
    while (current < kMaximumDeferredPointerMessages) {
        if (g_pointerMessageSlots.compare_exchange_weak(
                current, current + 1, std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

void release_pointer_message_slot() {
    g_pointerMessageSlots.fetch_sub(1, std::memory_order_acq_rel);
}

void retain_until_process_exit(
    HANDLE process,
    std::shared_ptr<void> keepAlive) {
    try {
        std::thread(
            [process, keepAlive = std::move(keepAlive)]() mutable {
                WaitForSingleObject(process, INFINITE);
                CloseHandle(process);
                keepAlive.reset();
                release_pointer_message_slot();
            })
            .detach();
        return;
    } catch (const std::system_error&) {
    }

    // If a waiter cannot be created, retain the allocation permanently rather
    // than risk freeing memory the target may still dereference. The global
    // slot cap keeps this fallback bounded.
    std::lock_guard<std::mutex> lock(g_permanentRetirementMutex);
    g_permanentRetirements.push_back(
        {process, std::move(keepAlive)});
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

NativeMessageResult send_native_pointer_message(
    const NativeWindowIdentity& identity, UINT message,
    WPARAM wParam, LPARAM lParam, std::shared_ptr<void> keepAlive,
    UINT timeoutMs) {
    if (!keepAlive) {
        return failure(
            ERROR_INVALID_PARAMETER,
            "Pointer-bearing native message has no lifetime owner");
    }
    auto valid = validate_native_window(identity);
    if (!valid.ok)
        return valid;
    if (!reserve_pointer_message_slot()) {
        return failure(
            ERROR_NOT_ENOUGH_MEMORY,
            "Too many timed-out native pointer messages are awaiting target exit");
    }
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, identity.pid);
    if (!process) {
        release_pointer_message_slot();
        return failure(
            GetLastError(),
            "Could not open the target process for pointer-message lifetime");
    }

    auto result =
        send_native_message(identity, message, wParam, lParam, timeoutMs);
    if (result.ok) {
        CloseHandle(process);
        release_pointer_message_slot();
        return result;
    }

    // SendMessageTimeout may return while the target WndProc is still using
    // lParam/wParam. Retain caller memory until the target exits; this is more
    // conservative than guessing when a reentrant WndProc has really returned.
    retain_until_process_exit(process, std::move(keepAlive));
    return result;
}

size_t deferred_native_pointer_message_count_for_testing() {
    return g_pointerMessageSlots.load(std::memory_order_acquire);
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

NativeMessageResult read_native_toolbar_button_text(
    const NativeWindowIdentity& identity, int commandId,
    std::string& text, size_t maximumChars) {
    text.clear();
    if (maximumChars == 0 ||
        maximumChars >
            static_cast<size_t>((std::numeric_limits<int>::max)() - 1)) {
        return failure(
            ERROR_INVALID_PARAMETER,
            "Invalid toolbar text capacity limit");
    }

    constexpr int kMaximumAttempts = 3;
    for (int attempt = 0; attempt < kMaximumAttempts; ++attempt) {
        auto length = send_native_message(
            identity, TB_GETBUTTONTEXTW,
            static_cast<WPARAM>(commandId), 0);
        if (!length.ok)
            return length;
        if (length.value < 0)
            return failure(
                ERROR_INVALID_DATA,
                "The toolbar button text is unavailable");
        if (static_cast<uint64_t>(length.value) > maximumChars) {
            return failure(
                ERROR_BUFFER_OVERFLOW,
                "The toolbar button text exceeds the configured safety limit");
        }

        const size_t chars = static_cast<size_t>(length.value) + 1;
        const size_t infoSize = sizeof(TBBUTTONINFOW);
        NativeMessageResult native;
        auto remote = std::make_shared<RemoteBuffer>(
            RemoteBuffer::allocate(
                identity, infoSize + chars * sizeof(wchar_t), native));
        if (!*remote)
            return native;

        TBBUTTONINFOW info{sizeof(info)};
        info.dwMask = TBIF_TEXT;
        info.pszText = reinterpret_cast<wchar_t*>(
            static_cast<std::byte*>(remote->address()) + infoSize);
        info.cchText = static_cast<int>(chars);
        std::vector<wchar_t> zero(chars, L'\0');
        if (!remote->write(&info, sizeof(info), native) ||
            !remote->write(
                zero.data(), zero.size() * sizeof(wchar_t),
                native, infoSize)) {
            return native;
        }

        auto copied = send_native_pointer_message(
            identity, TB_GETBUTTONINFOW,
            static_cast<WPARAM>(commandId),
            reinterpret_cast<LPARAM>(remote->address()), remote);
        if (!copied.ok)
            return copied;
        if (copied.value < 0) {
            return failure(
                ERROR_INVALID_DATA,
                "The toolbar button no longer exists");
        }

        auto after = send_native_message(
            identity, TB_GETBUTTONTEXTW,
            static_cast<WPARAM>(commandId), 0);
        if (!after.ok)
            return after;
        if (after.value < 0)
            return failure(
                ERROR_INVALID_DATA,
                "The toolbar button text became unavailable");
        if (static_cast<uint64_t>(after.value) > maximumChars) {
            return failure(
                ERROR_BUFFER_OVERFLOW,
                "The toolbar button text exceeds the configured safety limit");
        }
        if (after.value > length.value)
            continue;

        std::vector<wchar_t> value(chars, L'\0');
        if (!remote->read(
                value.data(), value.size() * sizeof(wchar_t),
                native, infoSize)) {
            return native;
        }
        const size_t valueLength =
            wcsnlen_s(value.data(), value.size());
        text = native_utf16_to_utf8(
            std::wstring_view(value.data(), valueLength));
        return success(static_cast<LRESULT>(valueLength));
    }

    return failure(
        ERROR_RETRY,
        "The toolbar button text kept growing while it was read");
}

} // namespace lvt
