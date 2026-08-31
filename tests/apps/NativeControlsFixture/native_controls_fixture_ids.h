#pragma once

#include <Windows.h>
#include <cstddef>

namespace lvt::native_fixture {

inline constexpr wchar_t kWindowClass[] = L"LvtNativePropertyFixtureWindow";
inline constexpr wchar_t kWindowTitle[] = L"LVT Native Property Fixture";
inline constexpr wchar_t kGenericChildClass[] = L"LvtNativePropertyFixtureText";
inline constexpr wchar_t kOutOfTreeTitle[] = L"LVT Native Out-of-Tree Window";

inline constexpr int kCheckboxId = 1001;
inline constexpr int kRadioId = 1002;
inline constexpr int kButtonId = 1003;
inline constexpr int kEditId = 1004;
inline constexpr int kReadOnlyEditId = 1005;
inline constexpr int kComboBoxId = 1006;
inline constexpr int kListBoxId = 1007;
inline constexpr int kScrollBarId = 1008;
inline constexpr int kListViewId = 1009;
inline constexpr int kTreeViewId = 1010;
inline constexpr int kToolbarId = 1011;
inline constexpr int kStatusBarId = 1012;
inline constexpr int kTabControlId = 1013;
inline constexpr int kGenericTextId = 1014;
inline constexpr int kStateSummaryId = 1015;
inline constexpr int kEventChildId = 1016;

inline constexpr int kToolbarApplyCommand = 2001;
inline constexpr int kToolbarPinCommand = 2002;
inline constexpr int kToolbarDisabledCommand = 2003;

// This is a read-only observation hook: it refreshes the visible summary from
// the real control state and returns the summary protocol version.
inline constexpr UINT kRefreshSummaryMessage = WM_APP + 0x100;
inline constexpr UINT kCloseMessage = WM_APP + 0x101;
inline constexpr UINT kMutateListViewIdentityMessage = WM_APP + 0x102;
inline constexpr UINT kRestoreListViewIdentityMessage = WM_APP + 0x103;
inline constexpr UINT kHangMessage = WM_APP + 0x104;
inline constexpr UINT kMutateToolbarIdentityMessage = WM_APP + 0x105;
inline constexpr UINT kRestoreToolbarIdentityMessage = WM_APP + 0x106;
inline constexpr UINT kSetLongToolbarTextMessage = WM_APP + 0x107;
inline constexpr UINT kRestoreToolbarTextMessage = WM_APP + 0x108;
inline constexpr UINT kArmDelayedPointerMessage = WM_APP + 0x109;
inline constexpr UINT kGetDelayedPointerStateMessage = WM_APP + 0x10A;
inline constexpr UINT kGetOutOfTreeHwndMessage = WM_APP + 0x10B;
inline constexpr UINT kDeleteFirstTabMessage = WM_APP + 0x10C;
inline constexpr UINT kMakeDuplicateTabsMessage = WM_APP + 0x10D;
inline constexpr UINT kRestoreTabsMessage = WM_APP + 0x10E;
inline constexpr UINT kValidateOwnerDrawStatusMessage = WM_APP + 0x10F;
inline constexpr UINT kSetTabItemExtraMessage = WM_APP + 0x110;
inline constexpr UINT kValidateTabItemExtraMessage = WM_APP + 0x111;
inline constexpr UINT kDuplicateToolbarCommandsMessage = WM_APP + 0x112;
inline constexpr UINT kRestoreToolbarCommandsMessage = WM_APP + 0x113;
inline constexpr UINT kMoveToolbarApplyMessage = WM_APP + 0x114;
inline constexpr UINT kRestoreToolbarOrderMessage = WM_APP + 0x115;
inline constexpr UINT kReparentGenericOutOfTreeMessage = WM_APP + 0x116;
inline constexpr UINT kRestoreGenericParentMessage = WM_APP + 0x117;
inline constexpr UINT kCreateEventChildMessage = WM_APP + 0x118;
inline constexpr UINT kDestroyEventChildMessage = WM_APP + 0x119;
inline constexpr UINT kReorderEventChildrenMessage = WM_APP + 0x11A;
inline constexpr UINT kBurstEventMessage = WM_APP + 0x11B;
inline constexpr UINT kNotifyRootClientChildDestroyMessage = WM_APP + 0x11C;
inline constexpr UINT kDestroyOutOfTreeWindowMessage = WM_APP + 0x11D;
inline constexpr UINT kPopulateLargeListHiddenDuplicateMessage = WM_APP + 0x11E;
inline constexpr UINT kDeleteHiddenListDuplicateMessage = WM_APP + 0x11F;
inline constexpr UINT kReorderLargeListMessage = WM_APP + 0x120;
inline constexpr UINT kRestoreDefaultListMessage = WM_APP + 0x121;
inline constexpr UINT kPopulateLargeToolbarHiddenDuplicateMessage = WM_APP + 0x122;
inline constexpr UINT kDeleteHiddenToolbarDuplicateMessage = WM_APP + 0x123;
inline constexpr UINT kRestoreDefaultToolbarMessage = WM_APP + 0x124;
inline constexpr UINT kPopulateOversizedListMessage = WM_APP + 0x125;
inline constexpr UINT kPopulateOversizedToolbarMessage = WM_APP + 0x126;
inline constexpr UINT kDuplicateSecondListIdentityMessage = WM_APP + 0x127;
inline constexpr UINT kRestoreSecondListIdentityMessage = WM_APP + 0x128;
inline constexpr UINT kDeleteFirstListItemMessage = WM_APP + 0x129;
inline constexpr UINT kInsertExternalFirstListItemMessage = WM_APP + 0x12A;
inline constexpr UINT kInsertAlphaFirstListItemMessage = WM_APP + 0x12B;
inline constexpr UINT kPopulateAdjacentToolbarSeparatorsMessage = WM_APP + 0x12C;
inline constexpr UINT kDeleteFirstToolbarSeparatorMessage = WM_APP + 0x12D;
// Kept after the native-key/scoped-watch fixture range during series rebase.
inline constexpr UINT kRecycleEventChildHwndMessage = WM_APP + 0x12E;
inline constexpr UINT kGetExactHwndRecycleResultMessage = WM_APP + 0x12F;
inline constexpr WPARAM kForceExactHwndRecycleUnavailable =
    static_cast<WPARAM>(~static_cast<WPARAM>(0));
inline constexpr WPARAM kForceExactHwndRecycleHardFailure =
    kForceExactHwndRecycleUnavailable - 1;
inline constexpr WPARAM kForceExactHwndRecycleGlobalCap =
    kForceExactHwndRecycleUnavailable - 2;
inline constexpr DWORD kExactHwndRecycleMaximumAttempts = 131072;
inline constexpr DWORD kExactHwndRecycleMaximumHeldWindows = 1024;
inline constexpr DWORD kExactHwndRecycleSearchBudgetMs = 5000;
inline constexpr DWORD kExactHwndRecycleMessageTimeoutMs = 20000;

enum class ExactHwndRecycleStatus : LRESULT {
    notRun = 0,
    achieved = 1,
    searchUnavailable = 2,
    forcedUnavailable = 3,
    hardFailure = 4,
};

enum class ExactHwndRecycleFailureStage : LRESULT {
    none = 0,
    invalidTarget = 1,
    destroyOriginal = 2,
    createSearchCandidate = 3,
    destroySearchCandidate = 4,
    cleanupHeldWindow = 5,
    restoreTarget = 6,
    protocol = 7,
};

enum class ExactHwndRecycleResultField : WPARAM {
    replacement = 0,
    peakHeldWindows = 1,
    remainingHeldWindows = 2,
    failureStage = 3,
    win32Error = 4,
};

inline constexpr LRESULT kSummaryProtocolVersion = 2;
inline constexpr size_t kLongToolbarTextLength = 6000;
inline constexpr size_t kLongItemTextLength = 5000;
inline constexpr ULONG_PTR kOwnerDrawStatusData = 0x1234ABCD;

} // namespace lvt::native_fixture
