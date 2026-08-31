#pragma once

#include <Windows.h>

#include <atomic>
#include <string>

#include "apps/NativeControlsFixture/native_controls_fixture_ids.h"

namespace lvt::test_support {

enum class ExactHwndRecycleOutcome {
    achieved,
    searchUnavailable,
    forcedUnavailable,
    hardFailure,
};

enum class ExactHwndRecycleTestMode {
    normal,
    forceUnavailable,
    forceHardFailure,
    forceGlobalCap,
};

struct ExactHwndRecycleOptions {
    DWORD maximumAttempts =
        native_fixture::kExactHwndRecycleMaximumAttempts;
    ExactHwndRecycleTestMode testMode =
        ExactHwndRecycleTestMode::normal;
    bool rememberUnavailable = true;
};

struct ExactHwndRecycleResult {
    ExactHwndRecycleOutcome outcome =
        ExactHwndRecycleOutcome::hardFailure;
    HWND replacement = nullptr;
    DWORD peakHeldWindows = 0;
    DWORD remainingHeldWindows = 0;
    native_fixture::ExactHwndRecycleFailureStage failureStage =
        native_fixture::ExactHwndRecycleFailureStage::none;
    DWORD win32Error = ERROR_SUCCESS;
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

inline bool exact_hwnd_recycle_is_unavailable(
    const ExactHwndRecycleResult& result) {
    return result.outcome ==
               ExactHwndRecycleOutcome::searchUnavailable ||
           result.outcome ==
               ExactHwndRecycleOutcome::forcedUnavailable;
}

inline const char* exact_hwnd_recycle_failure_stage_name(
    native_fixture::ExactHwndRecycleFailureStage stage) {
    switch (stage) {
    case native_fixture::ExactHwndRecycleFailureStage::none:
        return "none";
    case native_fixture::ExactHwndRecycleFailureStage::invalidTarget:
        return "invalid target";
    case native_fixture::ExactHwndRecycleFailureStage::destroyOriginal:
        return "destroy original";
    case native_fixture::ExactHwndRecycleFailureStage::
        createSearchCandidate:
        return "create search candidate";
    case native_fixture::ExactHwndRecycleFailureStage::
        destroySearchCandidate:
        return "destroy search candidate";
    case native_fixture::ExactHwndRecycleFailureStage::cleanupHeldWindow:
        return "cleanup held window";
    case native_fixture::ExactHwndRecycleFailureStage::restoreTarget:
        return "restore target";
    case native_fixture::ExactHwndRecycleFailureStage::protocol:
        return "protocol";
    }
    return "unknown";
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
            .outcome = ExactHwndRecycleOutcome::hardFailure,
            .failureStage =
                native_fixture::ExactHwndRecycleFailureStage::
                    invalidTarget,
            .win32Error = ERROR_INVALID_WINDOW_HANDLE,
            .reason =
                "the exact-recycle fixture target was not a valid window"};
    }

    if (options.testMode ==
            ExactHwndRecycleTestMode::normal &&
        options.rememberUnavailable &&
        exact_hwnd_recycle_search_suppressed()) {
        return {
            .outcome =
                ExactHwndRecycleOutcome::searchUnavailable,
            .replacement = original,
            .reason = exact_hwnd_recycle_unavailable_reason()};
    }

    HANDLE mutex = CreateMutexW(
        nullptr, FALSE, L"Local\\LvtExactHwndRecycleFixture");
    if (!mutex) {
        return {
            .outcome = ExactHwndRecycleOutcome::hardFailure,
            .failureStage =
                native_fixture::ExactHwndRecycleFailureStage::protocol,
            .win32Error = GetLastError(),
            .reason =
                "failed to create the exact-recycle fixture mutex"};
    }
    const DWORD wait = WaitForSingleObject(
        mutex,
        native_fixture::kExactHwndRecycleMessageTimeoutMs * 2);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
        CloseHandle(mutex);
        return {
            .outcome = ExactHwndRecycleOutcome::hardFailure,
            .failureStage =
                native_fixture::ExactHwndRecycleFailureStage::protocol,
            .win32Error =
                wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError(),
            .reason =
                "timed out waiting to serialize the exact-recycle fixture"};
    }
    ScopedMutexRelease releaseMutex(mutex);

    WPARAM recycleRequest = options.maximumAttempts;
    switch (options.testMode) {
    case ExactHwndRecycleTestMode::normal:
        break;
    case ExactHwndRecycleTestMode::forceUnavailable:
        recycleRequest =
            native_fixture::kForceExactHwndRecycleUnavailable;
        break;
    case ExactHwndRecycleTestMode::forceHardFailure:
        recycleRequest =
            native_fixture::kForceExactHwndRecycleHardFailure;
        break;
    case ExactHwndRecycleTestMode::forceGlobalCap:
        recycleRequest =
            native_fixture::kForceExactHwndRecycleGlobalCap;
        break;
    }

    DWORD_PTR statusValue = 0;
    SetLastError(ERROR_SUCCESS);
    const LRESULT sent = SendMessageTimeoutW(
        fixtureRoot,
        native_fixture::kRecycleEventChildHwndMessage,
        recycleRequest,
        targetId,
        SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT,
        native_fixture::kExactHwndRecycleMessageTimeoutMs,
        &statusValue);
    const DWORD sendError = GetLastError();

    if (!sent) {
        return {
            .outcome = ExactHwndRecycleOutcome::hardFailure,
            .failureStage =
                native_fixture::ExactHwndRecycleFailureStage::protocol,
            .win32Error = sendError,
            .reason =
                "the exact-recycle fixture message failed (error " +
                std::to_string(sendError) + ")"};
    }

    const auto queryResult = [&](
        native_fixture::ExactHwndRecycleResultField field,
        DWORD_PTR& value) {
        SetLastError(ERROR_SUCCESS);
        if (SendMessageTimeoutW(
                fixtureRoot,
                native_fixture::kGetExactHwndRecycleResultMessage,
                static_cast<WPARAM>(field), 0,
                SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, 2000,
                &value)) {
            return true;
        }
        return false;
    };

    DWORD_PTR replacementValue = 0;
    DWORD_PTR peakHeldWindows = 0;
    DWORD_PTR remainingHeldWindows = 0;
    DWORD_PTR failureStage = 0;
    DWORD_PTR win32Error = 0;
    if (!queryResult(
            native_fixture::ExactHwndRecycleResultField::replacement,
            replacementValue) ||
        !queryResult(
            native_fixture::ExactHwndRecycleResultField::peakHeldWindows,
            peakHeldWindows) ||
        !queryResult(
            native_fixture::ExactHwndRecycleResultField::
                remainingHeldWindows,
            remainingHeldWindows) ||
        !queryResult(
            native_fixture::ExactHwndRecycleResultField::failureStage,
            failureStage) ||
        !queryResult(
            native_fixture::ExactHwndRecycleResultField::win32Error,
            win32Error)) {
        return {
            .outcome = ExactHwndRecycleOutcome::hardFailure,
            .failureStage =
                native_fixture::ExactHwndRecycleFailureStage::protocol,
            .win32Error = GetLastError(),
            .reason =
                "failed to read the exact-recycle fixture result"};
    }

    ExactHwndRecycleResult result;
    result.replacement =
        reinterpret_cast<HWND>(replacementValue);
    result.peakHeldWindows =
        static_cast<DWORD>(peakHeldWindows);
    result.remainingHeldWindows =
        static_cast<DWORD>(remainingHeldWindows);
    result.failureStage =
        static_cast<
            native_fixture::ExactHwndRecycleFailureStage>(
            failureStage);
    result.win32Error = static_cast<DWORD>(win32Error);

    switch (
        static_cast<native_fixture::ExactHwndRecycleStatus>(
            statusValue)) {
    case native_fixture::ExactHwndRecycleStatus::achieved:
        result.outcome = ExactHwndRecycleOutcome::achieved;
        break;
    case native_fixture::ExactHwndRecycleStatus::searchUnavailable:
        result.outcome =
            ExactHwndRecycleOutcome::searchUnavailable;
        break;
    case native_fixture::ExactHwndRecycleStatus::forcedUnavailable:
        result.outcome =
            ExactHwndRecycleOutcome::forcedUnavailable;
        break;
    case native_fixture::ExactHwndRecycleStatus::hardFailure:
        result.outcome = ExactHwndRecycleOutcome::hardFailure;
        break;
    default:
        result.outcome = ExactHwndRecycleOutcome::hardFailure;
        result.failureStage =
            native_fixture::ExactHwndRecycleFailureStage::protocol;
        result.win32Error = ERROR_INVALID_DATA;
        result.reason =
            "the exact-recycle fixture returned an unknown status";
        return result;
    }

    const bool replacementValid =
        result.replacement &&
        IsWindow(result.replacement) &&
        GetDlgItem(fixtureRoot, controlId) ==
            result.replacement;
    if (result.outcome == ExactHwndRecycleOutcome::hardFailure) {
        if (result.failureStage ==
            native_fixture::ExactHwndRecycleFailureStage::none) {
            result.failureStage =
                native_fixture::ExactHwndRecycleFailureStage::protocol;
            result.win32Error = ERROR_INVALID_DATA;
        }
        result.reason =
            "the exact-recycle fixture encountered a hard failure at " +
            std::string(exact_hwnd_recycle_failure_stage_name(
                result.failureStage)) +
            " (error " + std::to_string(result.win32Error) +
            ", replacement valid " +
            (replacementValid ? "yes" : "no") +
            ", peak held " +
            std::to_string(result.peakHeldWindows) +
            ", remaining held " +
            std::to_string(result.remainingHeldWindows) + ")";
        return result;
    }

    if (result.peakHeldWindows >
            native_fixture::kExactHwndRecycleMaximumHeldWindows ||
        result.remainingHeldWindows != 0 ||
        !replacementValid) {
        result.outcome = ExactHwndRecycleOutcome::hardFailure;
        result.failureStage =
            native_fixture::ExactHwndRecycleFailureStage::protocol;
        result.win32Error = ERROR_INVALID_DATA;
        result.reason =
            "the exact-recycle fixture violated its cleanup/result "
            "protocol (peak held " +
            std::to_string(result.peakHeldWindows) +
            ", remaining held " +
            std::to_string(result.remainingHeldWindows) + ")";
        return result;
    }

    if (result.outcome == ExactHwndRecycleOutcome::achieved &&
        result.replacement != original) {
        result.outcome = ExactHwndRecycleOutcome::hardFailure;
        result.failureStage =
            native_fixture::ExactHwndRecycleFailureStage::protocol;
        result.win32Error = ERROR_INVALID_DATA;
        result.reason =
            "the exact-recycle fixture reported success with a "
            "different HWND";
        return result;
    }
    if (exact_hwnd_recycle_is_unavailable(result) &&
        result.replacement == original) {
        result.outcome = ExactHwndRecycleOutcome::hardFailure;
        result.failureStage =
            native_fixture::ExactHwndRecycleFailureStage::protocol;
        result.win32Error = ERROR_INVALID_DATA;
        result.reason =
            "the exact-recycle fixture reported unavailable without "
            "a distinct replacement HWND";
        return result;
    }

    if (options.rememberUnavailable &&
        exact_hwnd_recycle_is_unavailable(result)) {
        exact_hwnd_recycle_unavailable_state().store(
            true, std::memory_order_release);
    }
    if (exact_hwnd_recycle_is_unavailable(result))
        result.reason = exact_hwnd_recycle_unavailable_reason();
    return result;
}

} // namespace lvt::test_support
