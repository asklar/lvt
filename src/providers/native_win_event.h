#pragma once

#include "framework_connection.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

namespace lvt::native_eventing_detail {

struct NativeWinEventRecord {
    DWORD event = 0;
    HWND hwnd = nullptr;
    LONG objectId = 0;
    LONG childId = 0;
};

// Single-producer/single-consumer storage used directly by WinEventProc.
// push() is allocation-free, lock-free, and bounded; overflow only records a
// reset marker. The out-of-context hook is installed on one dedicated message
// thread, so it is the queue's only producer.
template <size_t Capacity>
class NativeWinEventQueue {
public:
    static_assert(Capacity > 0);

    bool push(const NativeWinEventRecord& event) noexcept {
        const uint64_t write = m_write.load(std::memory_order_relaxed);
        const uint64_t read = m_read.load(std::memory_order_acquire);
        if (write - read >= Capacity) {
            require_snapshot();
            return false;
        }

        m_records[write % Capacity] = event;
        m_write.store(write + 1, std::memory_order_release);
        return true;
    }

    void require_snapshot() noexcept {
        m_snapshotRequired.store(true, std::memory_order_release);
    }

    template <typename Visitor>
    bool drain(Visitor&& visitor) noexcept {
        const bool snapshotRequired =
            m_snapshotRequired.exchange(false, std::memory_order_acq_rel);
        uint64_t read = m_read.load(std::memory_order_relaxed);
        const uint64_t write = m_write.load(std::memory_order_acquire);
        while (read != write) {
            visitor(m_records[read % Capacity]);
            ++read;
        }
        m_read.store(read, std::memory_order_release);
        return snapshotRequired;
    }

private:
    std::array<NativeWinEventRecord, Capacity> m_records{};
    std::atomic<uint64_t> m_read{0};
    std::atomic<uint64_t> m_write{0};
    std::atomic<bool> m_snapshotRequired{false};
};

inline constexpr size_t kNativeWinEventQueueCapacity = 256;

bool event_destroys_root_window(
    HWND root, const NativeWinEventRecord& event,
    bool rootIsWindow) noexcept;

// Shared with integration tests so lifecycle assertions survive destruction of
// the subscription they observe. Production code does not branch on these
// counters.
struct NativeWinEventDiagnostics {
    std::atomic<uint32_t> hookInstalls{0};
    std::atomic<uint32_t> unhooks{0};
    std::atomic<uint32_t> callbacks{0};
    std::atomic<uint32_t> queued{0};
    std::atomic<uint32_t> overflows{0};
    std::atomic<uint32_t> reentrantDrops{0};
};

// Process-filtered WinEvent subscription for one native root HWND. Raw
// callback records are filtered to the root subtree off-callback, then
// coalesced to one provider-neutral snapshotRequired notification.
class NativeWinEventSource {
public:
    static std::unique_ptr<NativeWinEventSource> create(
        HWND root, DWORD pid,
        std::shared_ptr<NativeWinEventDiagnostics> diagnostics = {});

    ~NativeWinEventSource();

    NativeWinEventSource(const NativeWinEventSource&) = delete;
    NativeWinEventSource& operator=(const NativeWinEventSource&) = delete;

    void publish_windows(std::vector<HWND> windows);
    std::vector<ConnectionEvent> poll_events();

    bool hook_active() const;
    bool root_destroyed() const;
    bool target_exited() const;
    std::shared_ptr<NativeWinEventDiagnostics> diagnostics() const;

private:
    struct State;

    explicit NativeWinEventSource(std::shared_ptr<State> state);
    static void run(const std::shared_ptr<State>& state) noexcept;
    static void CALLBACK callback(
        HWINEVENTHOOK hook, DWORD event, HWND hwnd,
        LONG objectId, LONG childId,
        DWORD eventThread, DWORD eventTime);

    static thread_local State* s_callbackState;

    std::shared_ptr<State> m_state;
    std::thread m_thread;
};

} // namespace lvt::native_eventing_detail
