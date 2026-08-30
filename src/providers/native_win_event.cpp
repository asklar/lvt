#include "native_win_event.h"

#include <wil/resource.h>

#include <mutex>
#include <set>

namespace lvt::native_eventing_detail {
namespace {

bool is_relevant_event(DWORD event) noexcept {
    switch (event) {
    case EVENT_OBJECT_CREATE:
    case EVENT_OBJECT_DESTROY:
    case EVENT_OBJECT_SHOW:
    case EVENT_OBJECT_HIDE:
    case EVENT_OBJECT_REORDER:
    case EVENT_OBJECT_FOCUS:
    case EVENT_OBJECT_SELECTION:
    case EVENT_OBJECT_SELECTIONADD:
    case EVENT_OBJECT_SELECTIONREMOVE:
    case EVENT_OBJECT_SELECTIONWITHIN:
    case EVENT_OBJECT_STATECHANGE:
    case EVENT_OBJECT_LOCATIONCHANGE:
    case EVENT_OBJECT_NAMECHANGE:
    case EVENT_OBJECT_VALUECHANGE:
    case EVENT_OBJECT_PARENTCHANGE:
        return true;
    default:
        return false;
    }
}

} // namespace

bool event_destroys_root_window(
    HWND root, const NativeWinEventRecord& event,
    bool rootIsWindow) noexcept {
    if (event.event != EVENT_OBJECT_DESTROY ||
        event.hwnd != root ||
        event.childId != CHILDID_SELF) {
        return false;
    }

    return event.objectId == OBJID_WINDOW || !rootIsWindow;
}

struct NativeWinEventSource::State {
    HWND root = nullptr;
    DWORD pid = 0;
    wil::unique_event_nothrow ready{
        CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    wil::unique_event_nothrow stop{
        CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    wil::unique_event_nothrow rootGone{
        CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    wil::unique_handle process;
    NativeWinEventQueue<kNativeWinEventQueueCapacity> queue;
    std::shared_ptr<NativeWinEventDiagnostics> diagnostics;
    std::atomic<uintptr_t> hook{0};
    std::atomic<bool> accepting{false};
    std::atomic<bool> rootDestroyed{false};
    std::atomic<bool> targetExited{false};
    std::atomic_flag callbackActive = ATOMIC_FLAG_INIT;
    mutable std::mutex publishedMutex;
    std::set<HWND> publishedWindows;
};

thread_local NativeWinEventSource::State*
    NativeWinEventSource::s_callbackState = nullptr;

std::unique_ptr<NativeWinEventSource> NativeWinEventSource::create(
    HWND root, DWORD pid,
    std::shared_ptr<NativeWinEventDiagnostics> diagnostics) {
    auto state = std::make_shared<State>();
    state->root = root;
    state->pid = pid;
    state->diagnostics = diagnostics
        ? std::move(diagnostics)
        : std::make_shared<NativeWinEventDiagnostics>();
    state->process.reset(OpenProcess(SYNCHRONIZE, FALSE, pid));

    auto source = std::unique_ptr<NativeWinEventSource>(
        new NativeWinEventSource(std::move(state)));
    if (!source->m_state->ready || !source->m_state->stop ||
        !source->m_state->rootGone) {
        return source;
    }

    try {
        source->m_thread = std::thread(
            &NativeWinEventSource::run, source->m_state);
    } catch (...) {
        return source;
    }

    // Registration normally completes immediately. A timeout merely disables
    // the optimization for now; native tree/property polling remains valid.
    WaitForSingleObject(source->m_state->ready.get(), 5000);
    return source;
}

NativeWinEventSource::NativeWinEventSource(std::shared_ptr<State> state)
    : m_state(std::move(state)) {
}

NativeWinEventSource::~NativeWinEventSource() {
    if (m_state && m_state->stop)
        SetEvent(m_state->stop.get());
    if (m_thread.joinable())
        m_thread.join();
}

void NativeWinEventSource::run(
    const std::shared_ptr<State>& state) noexcept {
    s_callbackState = state.get();

    // SetWinEventHook requires the installing thread to own and pump a message
    // queue. Creating it before registration also makes shutdown wakeups
    // deterministic.
    MSG message{};
    PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

    DWORD flags = WINEVENT_OUTOFCONTEXT;
    if (state->pid != GetCurrentProcessId())
        flags |= WINEVENT_SKIPOWNPROCESS;

    const HWINEVENTHOOK hook = SetWinEventHook(
        EVENT_OBJECT_CREATE, EVENT_OBJECT_PARENTCHANGE,
        nullptr, &NativeWinEventSource::callback,
        state->pid, 0, flags);
    state->hook.store(
        reinterpret_cast<uintptr_t>(hook), std::memory_order_release);
    state->accepting.store(hook != nullptr, std::memory_order_release);
    if (hook)
        state->diagnostics->hookInstalls.fetch_add(
            1, std::memory_order_relaxed);
    SetEvent(state->ready.get());

    if (hook) {
        HANDLE handles[3]{
            state->stop.get(),
            state->rootGone.get(),
            state->process.get()};
        const DWORD handleCount = state->process ? 3u : 2u;
        for (;;) {
            const DWORD wait = MsgWaitForMultipleObjectsEx(
                handleCount, handles, INFINITE, QS_ALLINPUT,
                MWMO_INPUTAVAILABLE);
            if (wait == WAIT_OBJECT_0)
                break;
            if (wait == WAIT_OBJECT_0 + 1)
                break;
            if (handleCount == 3 && wait == WAIT_OBJECT_0 + 2) {
                state->targetExited.store(true, std::memory_order_release);
                break;
            }
            if (wait != WAIT_OBJECT_0 + handleCount)
                break;

            bool quit = false;
            while (PeekMessageW(
                       &message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) {
                    quit = true;
                    break;
                }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (quit)
                break;
        }

        state->accepting.store(false, std::memory_order_release);
        // Out-of-context callbacks run on this installing thread. Stop
        // accepting first, then unhook here so no callback can race state
        // destruction on another thread.
        UnhookWinEvent(hook);
        state->diagnostics->unhooks.fetch_add(
            1, std::memory_order_relaxed);
    }

    state->hook.store(0, std::memory_order_release);
    state->accepting.store(false, std::memory_order_release);
    s_callbackState = nullptr;
}

void CALLBACK NativeWinEventSource::callback(
    HWINEVENTHOOK hook, DWORD event, HWND hwnd,
    LONG objectId, LONG childId,
    DWORD, DWORD) {
    State* state = s_callbackState;
    if (!state ||
        !state->accepting.load(std::memory_order_acquire) ||
        reinterpret_cast<uintptr_t>(hook) !=
            state->hook.load(std::memory_order_acquire) ||
        !hwnd || !is_relevant_event(event)) {
        return;
    }

    state->diagnostics->callbacks.fetch_add(
        1, std::memory_order_relaxed);
    if (state->callbackActive.test_and_set(std::memory_order_acquire)) {
        state->queue.require_snapshot();
        state->diagnostics->reentrantDrops.fetch_add(
            1, std::memory_order_relaxed);
        return;
    }

    const NativeWinEventRecord record{
        event, hwnd, objectId, childId};
    bool rootIsWindow = true;
    // OBJID_CLIENT and non-self child IDs describe logical accessibility
    // objects, not the HWND's lifetime.
    if (event == EVENT_OBJECT_DESTROY &&
        hwnd == state->root &&
        childId == CHILDID_SELF &&
        objectId != OBJID_WINDOW) {
        rootIsWindow = IsWindow(state->root) != FALSE;
    }
    if (event_destroys_root_window(
            state->root, record, rootIsWindow)) {
        state->rootDestroyed.store(true, std::memory_order_release);
        SetEvent(state->rootGone.get());
    }

    const bool queued = state->queue.push(record);
    state->diagnostics
        ->queued.fetch_add(queued ? 1u : 0u, std::memory_order_relaxed);
    state->diagnostics
        ->overflows.fetch_add(queued ? 0u : 1u, std::memory_order_relaxed);
    state->callbackActive.clear(std::memory_order_release);
}

void NativeWinEventSource::publish_windows(
    std::vector<HWND> windows) {
    if (!m_state)
        return;
    std::lock_guard<std::mutex> lock(m_state->publishedMutex);
    m_state->publishedWindows.clear();
    m_state->publishedWindows.insert(m_state->root);
    for (HWND hwnd : windows) {
        if (hwnd)
            m_state->publishedWindows.insert(hwnd);
    }
}

std::vector<ConnectionEvent> NativeWinEventSource::poll_events() {
    std::vector<ConnectionEvent> result;
    if (!m_state)
        return result;

    bool relevant = false;
    {
        std::lock_guard<std::mutex> lock(m_state->publishedMutex);
        (void)m_state->queue.drain(
            [&](const NativeWinEventRecord& event) noexcept {
                if (event.hwnd == m_state->root ||
                    m_state->publishedWindows.contains(event.hwnd)) {
                    relevant = true;
                    return;
                }

                // EVENT_OBJECT_DESTROY must never probe the destroyed HWND.
                // If it was not in the last published snapshot, it was not in
                // this root's tree. Other event kinds may safely be checked
                // off-callback; a raced destroy simply makes IsWindow false.
                if (event.event == EVENT_OBJECT_DESTROY ||
                    !IsWindow(m_state->root) ||
                    !IsWindow(event.hwnd)) {
                    return;
                }
                relevant = IsChild(m_state->root, event.hwnd) != FALSE;
            });
    }

    // Overflow is meaningful only when at least one retained record belonged
    // to this root. This avoids an unrelated top-level window in the same
    // process forcing a cross-session reset; a dropped relevant record is
    // still recovered by the unchanged correctness poll.
    if (relevant) {
        ConnectionEvent event;
        event.mutation = ConnectionEvent::Mutation::snapshotRequired;
        result.push_back(std::move(event));
    }
    return result;
}

bool NativeWinEventSource::hook_active() const {
    return m_state &&
           m_state->hook.load(std::memory_order_acquire) != 0;
}

bool NativeWinEventSource::root_destroyed() const {
    return m_state &&
           m_state->rootDestroyed.load(std::memory_order_acquire);
}

bool NativeWinEventSource::target_exited() const {
    return m_state &&
           m_state->targetExited.load(std::memory_order_acquire);
}

std::shared_ptr<NativeWinEventDiagnostics>
NativeWinEventSource::diagnostics() const {
    return m_state ? m_state->diagnostics : nullptr;
}

} // namespace lvt::native_eventing_detail
