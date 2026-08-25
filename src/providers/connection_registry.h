#pragma once
#include "framework_connection.h"
#include <Windows.h>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace lvt {

// RAII handle to an acquired IFrameworkConnection. Move-only; releasing the
// registry's reference happens automatically in the destructor.
//
// This exists specifically so "acquire a connection, forget to release it"
// cannot happen by omission at a call site - the exact failure mode
// (something is created once and never explicitly torn down) that produced
// the confirmed, unbounded per-tick window leak this whole mechanism
// replaces. A default-constructed/empty handle is valid and falsy.
class ConnectionHandle {
public:
    ConnectionHandle() = default;
    ConnectionHandle(DWORD pid, std::string frameworkLabel,
                      std::shared_ptr<IFrameworkConnection> connection);
    ~ConnectionHandle();

    ConnectionHandle(ConnectionHandle&& other) noexcept;
    ConnectionHandle& operator=(ConnectionHandle&& other) noexcept;
    ConnectionHandle(const ConnectionHandle&) = delete;
    ConnectionHandle& operator=(const ConnectionHandle&) = delete;

    IFrameworkConnection* operator->() const { return m_connection.get(); }
    IFrameworkConnection* get() const { return m_connection.get(); }
    explicit operator bool() const { return m_connection != nullptr; }

    // Drop this handle's reference early, before it would otherwise go out
    // of scope (e.g. because is_alive() went false and the caller wants a
    // fresh acquire() on the next attempt rather than holding a dead one).
    void reset();

private:
    void release_if_held();

    DWORD m_pid = 0;
    std::string m_frameworkLabel;
    std::shared_ptr<IFrameworkConnection> m_connection;
};

// Per-process (i.e. per lvt_core instance - a one-shot CLI command links
// this fresh each run, while lvt_core stays loaded for the whole life of an
// `lvt watch` process or an `lvt mcp` server process) registry of live
// IFrameworkConnections, keyed by (pid, framework label, e.g. "xaml" /
// "winui3"). Refcounted: multiple acquirers within the SAME process share
// one underlying connection; it is only torn down once the last holder
// releases (or lets its ConnectionHandle go out of scope).
//
// Deliberately per-process only - a connection acquired by one lvt.exe
// invocation is not visible to another separately-running lvt.exe process
// targeting the same window. Broader cross-process sharing was considered
// and explicitly deferred; see docs/architecture.md.
class ConnectionRegistry {
public:
    static ConnectionRegistry& instance();

    // Attempts to establish a NEW connection for (hwnd, pid). Returning
    // nullptr means "could not connect"; the caller falls back to whatever
    // one-shot path it used before this registry existed.
    using Factory = std::function<std::shared_ptr<IFrameworkConnection>(HWND hwnd, DWORD pid)>;

    // Returns the existing live connection for (pid, frameworkLabel) if one
    // is registered and still is_alive(); otherwise invokes `factory` to
    // create one and registers it. The returned handle is empty (falsy) if
    // no live connection exists and `factory` also failed.
    ConnectionHandle acquire(DWORD pid, HWND hwnd, const std::string& frameworkLabel,
                             const Factory& factory);

private:
    friend class ConnectionHandle;
    void release(DWORD pid, const std::string& frameworkLabel);

    struct Entry {
        std::shared_ptr<IFrameworkConnection> connection;
        int refCount = 0;
    };

    std::mutex m_mutex;
    std::map<std::pair<DWORD, std::string>, Entry> m_entries;
};

} // namespace lvt
