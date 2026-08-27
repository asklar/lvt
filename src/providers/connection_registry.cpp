// connection_registry.cpp — per-process refcounted registry of live
// IFrameworkConnections. See connection_registry.h for the rationale.

#include "connection_registry.h"

namespace lvt {

ConnectionHandle::ConnectionHandle(DWORD pid, std::string frameworkLabel,
                                   std::shared_ptr<IFrameworkConnection> connection)
    : m_pid(pid), m_frameworkLabel(std::move(frameworkLabel)), m_connection(std::move(connection)) {
}

ConnectionHandle::~ConnectionHandle() {
    release_if_held();
}

ConnectionHandle::ConnectionHandle(ConnectionHandle&& other) noexcept
    : m_pid(other.m_pid),
      m_frameworkLabel(std::move(other.m_frameworkLabel)),
      m_connection(std::move(other.m_connection)) {
    other.m_connection.reset();
}

ConnectionHandle& ConnectionHandle::operator=(ConnectionHandle&& other) noexcept {
    if (this != &other) {
        release_if_held();
        m_pid = other.m_pid;
        m_frameworkLabel = std::move(other.m_frameworkLabel);
        m_connection = std::move(other.m_connection);
        other.m_connection.reset();
    }
    return *this;
}

void ConnectionHandle::reset() {
    release_if_held();
    m_connection.reset();
}

void ConnectionHandle::release_if_held() {
    if (m_connection) {
        ConnectionRegistry::instance().release(m_pid, m_frameworkLabel);
    }
}

ConnectionRegistry& ConnectionRegistry::instance() {
    static ConnectionRegistry registry;
    return registry;
}

ConnectionHandle ConnectionRegistry::acquire(DWORD pid, HWND hwnd, const std::string& frameworkLabel,
                                             const Factory& factory) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto key = std::make_pair(pid, frameworkLabel);
    auto it = m_entries.find(key);
    if (it != m_entries.end() && it->second.connection && it->second.connection->is_alive()) {
        it->second.refCount++;
        return ConnectionHandle(pid, frameworkLabel, it->second.connection);
    }

    // No live entry (or a stale one whose connection died) - (re)create it.
    // The factory call happens with the registry lock held: connecting is
    // relatively rare (once per watch session / MCP session, not once per
    // tick) and serializing it avoids two racing acquirers both trying to
    // inject into the same target at once.
    auto connection = factory(hwnd, pid);
    if (!connection) {
        m_entries.erase(key);
        return ConnectionHandle();
    }

    Entry entry;
    entry.connection = connection;
    entry.refCount = 1;
    m_entries[key] = entry;
    return ConnectionHandle(pid, frameworkLabel, connection);
}

void ConnectionRegistry::release(DWORD pid, const std::string& frameworkLabel) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto key = std::make_pair(pid, frameworkLabel);
    auto it = m_entries.find(key);
    if (it == m_entries.end())
        return;
    if (--it->second.refCount <= 0) {
        // Dropping the shared_ptr here runs the connection's destructor,
        // which is where each provider performs its clean disconnect
        // (DISCONNECT + UnadviseVisualTreeChange + DestroyWindow, etc).
        m_entries.erase(it);
    }
}

} // namespace lvt
