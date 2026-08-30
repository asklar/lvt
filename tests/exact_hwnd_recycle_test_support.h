#pragma once

#include <Windows.h>

#include <atomic>
#include <string>

#include "apps/NativeControlsFixture/native_controls_fixture_ids.h"

namespace lvt::test_support {

enum class ExactHwndRecycleOutcome {
    achieved,
    unavailable,
    failed,
};

struct ExactHwndRecycleOptions {
    DWORD maximumAttempts =
        native_fixture::kExactHwndRecycleMaximumAttempts;
    bool forceUnavailable = false;
    bool rememberUnavailable = true;
};

struct ExactHwndRecycleResult {
    ExactHwndRecycleOutcome outcome =
        ExactHwndRecycleOutcome::failed;
    HWND replacement = nullptr;
    std::string reason;
};

inline std::atomic<bool>& exact_hwnd_recycle_unavailable_state() {
    static std::atomic<bool> unavailable{false};
    return unavailable;
}

inline bool exact_hwnd_recycle_search_suppressed() {
    return exact_hwnd_recycle_unavailable_state().load(
        std::memory_order_acquire);
}

inline const char* exact_hwnd_recycle_unavailable_reason() {
    return "exact numeric HWND reuse is unavailable in the shared USER "
           "handle table; the fixture restored a valid replacement window";
}

class ScopedEventSignal {
public:
    explicit ScopedEventSignal(HANDLE event) : event_(event) {}
    ~ScopedEventSignal() {
        signal();
    }

    ScopedEventSignal(const ScopedEventSignal&) = delete;
    ScopedEventSignal& operator=(const ScopedEventSignal&) = delete;

    void signal() {
        if (!event_)
            return;
        SetEvent(event_);
        event_ = nullptr;
    }

private:
    HANDLE event_ = nullptr;
};

class ScopedMutexRelease {
public:
    explicit ScopedMutexRelease(HANDLE mutex) : mutex_(mutex) {}
    ~ScopedMutexRelease() {
        if (mutex_) {
            ReleaseMutex(mutex_);
            CloseHandle(mutex_);
        }
    }

    ScopedMutexRelease(const ScopedMutexRelease&) = delete;
    ScopedMutexRelease& operator=(const ScopedMutexRelease&) = delete;

private:
    HANDLE mutex_ = nullptr;
};

inline ExactHwndRecycleResult recycle_event_child_exact(
    HWND fixtureRoot, HWND original, int targetId = 0,
    ExactHwndRecycleOptions options = {}) {
    const int controlId =
        targetId != 0 ? targetId : native_fixture::kEventChildId;
    if (!fixtureRoot || !IsWindow(fixtureRoot) ||
        !original || !IsWindow(original)) {
        return {
            ExactHwndRecycleOutcome::failed, nullptr,
            "the exact-recycle fixture target was not a valid window"};
    }

    if (!options.forceUnavailable &&
        options.rememberUnavailable &&
        exact_hwnd_recycle_search_suppressed()) {
        return {
            ExactHwndRecycleOutcome::unavailable, original,
            exact_hwnd_recycle_unavailable_reason()};
    }

    HANDLE mutex = CreateMutexW(
        nullptr, FALSE, L"Local\\LvtExactHwndRecycleFixture");
    if (!mutex) {
        return {
            ExactHwndRecycleOutcome::failed, nullptr,
            "failed to create the exact-recycle fixture mutex"};
    }
    const DWORD wait = WaitForSingleObject(
        mutex,
        native_fixture::kExactHwndRecycleMessageTimeoutMs * 2);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
        CloseHandle(mutex);
        return {
            ExactHwndRecycleOutcome::failed, nullptr,
            "timed out waiting to serialize the exact-recycle fixture"};
    }
    ScopedMutexRelease releaseMutex(mutex);

    DWORD_PTR recycledValue = 0;
    SetLastError(ERROR_SUCCESS);
    const LRESULT sent = SendMessageTimeoutW(
        fixtureRoot,
        native_fixture::kRecycleEventChildHwndMessage,
        options.forceUnavailable
            ? native_fixture::kForceExactHwndRecycleUnavailable
            : static_cast<WPARAM>(options.maximumAttempts),
        targetId,
        SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT,
        native_fixture::kExactHwndRecycleMessageTimeoutMs,
        &recycledValue);
    const DWORD sendError = GetLastError();

    if (!sent) {
        return {
            ExactHwndRecycleOutcome::failed, nullptr,
            "the exact-recycle fixture message failed (error " +
                std::to_string(sendError) + ")"};
    }

    const HWND recycled =
        reinterpret_cast<HWND>(recycledValue);
    if (recycled) {
        if (recycled != original || !IsWindow(recycled) ||
            GetDlgItem(fixtureRoot, controlId) != recycled) {
            return {
                ExactHwndRecycleOutcome::failed, recycled,
                "the exact-recycle fixture returned an invalid success "
                "window"};
        }
        return {
            ExactHwndRecycleOutcome::achieved, recycled, {}};
    }

    const HWND replacement =
        GetDlgItem(fixtureRoot, controlId);
    if (!replacement || !IsWindow(replacement) ||
        replacement == original) {
        return {
            ExactHwndRecycleOutcome::failed, replacement,
            "the exact-recycle fixture reported reuse unavailable without "
            "restoring a distinct valid replacement window"};
    }

    if (options.rememberUnavailable &&
        !options.forceUnavailable) {
        exact_hwnd_recycle_unavailable_state().store(
            true, std::memory_order_release);
    }
    return {
        ExactHwndRecycleOutcome::unavailable, replacement,
        exact_hwnd_recycle_unavailable_reason()};
}

} // namespace lvt::test_support
