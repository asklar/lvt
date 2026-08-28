#pragma once

#include "../target.h"

#include <Windows.h>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

namespace lvt {

struct NativeWindowIdentity {
    HWND hwnd = nullptr;
    DWORD pid = 0;
    std::string normalizedClass;
};

struct NativeMessageResult {
    bool ok = false;
    bool timedOut = false;
    HRESULT hresult = E_FAIL;
    DWORD win32Error = ERROR_GEN_FAILURE;
    LRESULT value = 0;
    std::string error;
};

std::string normalize_native_class_name(std::string_view className);
NativeMessageResult capture_native_window_identity(
    HWND hwnd, DWORD expectedPid, NativeWindowIdentity& identity);
NativeMessageResult validate_native_window(
    const NativeWindowIdentity& identity);
NativeMessageResult send_native_message(
    const NativeWindowIdentity& identity, UINT message,
    WPARAM wParam = 0, LPARAM lParam = 0, UINT timeoutMs = 1000);
NativeMessageResult send_native_pointer_message(
    const NativeWindowIdentity& identity, UINT message,
    WPARAM wParam, LPARAM lParam, std::shared_ptr<void> keepAlive,
    UINT timeoutMs = 1000);

// Pointer messages reserve one of a bounded set of lifetime slots. On timeout,
// keepAlive is retained until the target process exits because
// SendMessageTimeout may return before the target WndProc stops dereferencing
// caller-provided memory.
size_t deferred_native_pointer_message_count_for_testing();

bool native_pointer_operations_allowed(
    Architecture host, Architecture target);

std::string native_utf16_to_utf8(std::wstring_view value);
bool native_utf8_to_utf16(
    std::string_view value, std::wstring& converted, std::string& error);

// Owns both the minimally-privileged target process handle and one allocation
// in that process. Pointer-bearing native control messages must point at one
// of these buffers, never at caller memory in lvt.exe.
class RemoteBuffer {
public:
    RemoteBuffer() = default;
    ~RemoteBuffer();
    RemoteBuffer(RemoteBuffer&& other) noexcept;
    RemoteBuffer& operator=(RemoteBuffer&& other) noexcept;
    RemoteBuffer(const RemoteBuffer&) = delete;
    RemoteBuffer& operator=(const RemoteBuffer&) = delete;

    static RemoteBuffer allocate(
        const NativeWindowIdentity& identity, size_t size,
        NativeMessageResult& result);

    explicit operator bool() const { return m_address != nullptr; }
    void* address() const { return m_address; }
    size_t size() const { return m_size; }

    bool write(
        const void* data, size_t size, NativeMessageResult& result,
        size_t offset = 0) const;
    bool read(
        void* data, size_t size, NativeMessageResult& result,
        size_t offset = 0) const;

private:
    RemoteBuffer(HANDLE process, void* address, size_t size)
        : m_process(process), m_address(address), m_size(size) {}
    void reset();

    HANDLE m_process = nullptr;
    void* m_address = nullptr;
    size_t m_size = 0;
};

NativeMessageResult read_native_toolbar_button_text(
    const NativeWindowIdentity& identity, int commandId,
    std::string& text, size_t maximumChars = 1024 * 1024);

} // namespace lvt
