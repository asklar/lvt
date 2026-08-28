#pragma once

#include <Windows.h>

namespace lvt::native_fixture {

inline constexpr wchar_t kWindowClass[] = L"LvtNativePropertyFixtureWindow";
inline constexpr wchar_t kWindowTitle[] = L"LVT Native Property Fixture";
inline constexpr wchar_t kGenericChildClass[] = L"LvtNativePropertyFixtureText";

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
inline constexpr LRESULT kSummaryProtocolVersion = 2;

} // namespace lvt::native_fixture
