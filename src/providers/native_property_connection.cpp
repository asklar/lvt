#include "native_property_connection.h"

#include "native_message.h"

#include <CommCtrl.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

namespace lvt {
namespace {

constexpr size_t kMaximumNativeTextChars = 1024 * 1024;
constexpr size_t kMaximumIdentityScanItems = 256;
constexpr size_t kControlTextChars = 4096;
constexpr uint64_t kSyntheticHandleBase = UINT64_C(0x8000000000000000);

enum class TargetKind {
    hwnd,
    listviewItem,
    treeviewItem,
    toolbarButton,
    statusbarPart,
    tabItem,
};

enum class Operation {
    text,
    enabled,
    buttonCheckState,
    editSelectionStart,
    editSelectionEnd,
    editReadOnly,
    selectedIndex,
    scrollMinimum,
    scrollMaximum,
    scrollPageSize,
    scrollPosition,
    listviewMode,
    listviewItemSelected,
    listviewItemFocused,
    listviewItemText,
    treeviewItemSelected,
    treeviewItemExpanded,
    treeviewItemText,
    toolbarButtonChecked,
    toolbarButtonEnabled,
    toolbarButtonText,
    statusbarPartText,
    tabSelectedIndex,
    tabItemText,
};

struct Target {
    TargetKind kind = TargetKind::hwnd;
    NativeWindowIdentity window;
    int index = -1;
    uintptr_t itemHandle = 0;
    int commandId = 0;
    std::string schemaId;
    std::string snapshotIdentity;
    bool hasSnapshot = false;
};

struct LiveTarget {
    Target target;
    LONG_PTR style = 0;
    bool pointerAllowed = false;
    bool ownerData = false;
    bool identityVerified = true;
    bool checkable = false;
    std::string identity;
    std::string identityReason;
};

struct Mutation {
    std::string schemaId;
    Operation operation = Operation::text;
    bool writable = false;
    bool supportsClear = false;
};

struct ReadResult {
    bool ok = true;
    bool available = true;
    HRESULT hresult = S_OK;
    std::string error;
    std::string value;
};

std::string lower(std::string value) {
    std::transform(
        value.begin(), value.end(), value.begin(),
        [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
    return value;
}

std::string hex_u64(uint64_t value) {
    std::ostringstream out;
    out << std::hex << std::uppercase << value;
    return out.str();
}

uint64_t stable_hash(std::string_view value) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (unsigned char ch : value) {
        hash ^= ch;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

PropertyMutationResult mutation_failure(
    HRESULT hresult, std::string error) {
    PropertyMutationResult result;
    result.hresult = hresult;
    result.error = std::move(error);
    return result;
}

PropertyMutationResult mutation_failure(
    const NativeMessageResult& native) {
    return mutation_failure(native.hresult, native.error);
}

ReadResult read_failure(const NativeMessageResult& native) {
    ReadResult result;
    result.ok = false;
    result.hresult = native.hresult;
    result.error = native.error;
    return result;
}

ReadResult unavailable(std::string reason) {
    ReadResult result;
    result.available = false;
    result.error = std::move(reason);
    return result;
}

bool is_comctl_class(std::string_view className) {
    return className == "syslistview32" ||
           className == "systreeview32" ||
           className == "toolbarwindow32" ||
           className == "msctls_statusbar32" ||
           className == "systabcontrol32";
}

bool operation_needs_remote_pointer(Operation operation) {
    switch (operation) {
    case Operation::text:
    case Operation::editSelectionStart:
    case Operation::editSelectionEnd:
    case Operation::scrollMinimum:
    case Operation::scrollMaximum:
    case Operation::scrollPageSize:
    case Operation::scrollPosition:
    case Operation::listviewItemSelected:
    case Operation::listviewItemFocused:
    case Operation::listviewItemText:
    case Operation::treeviewItemSelected:
    case Operation::treeviewItemExpanded:
    case Operation::treeviewItemText:
    case Operation::toolbarButtonChecked:
    case Operation::toolbarButtonEnabled:
    case Operation::toolbarButtonText:
    case Operation::statusbarPartText:
    case Operation::tabItemText:
        return true;
    default:
        return false;
    }
}

std::string target_kind_name(TargetKind kind) {
    switch (kind) {
    case TargetKind::hwnd: return "hwnd";
    case TargetKind::listviewItem: return "listview-item";
    case TargetKind::treeviewItem: return "treeview-item";
    case TargetKind::toolbarButton: return "toolbar-button";
    case TargetKind::statusbarPart: return "statusbar-part";
    case TargetKind::tabItem: return "tab-item";
    }
    return "unknown";
}

LONG_PTR capability_style(const LiveTarget& live) {
    const auto& className = live.target.window.normalizedClass;
    if (className == "button")
        return live.style & BS_TYPEMASK;
    if (className == "edit")
        return live.style & (ES_MULTILINE | ES_PASSWORD | ES_NUMBER);
    if (className == "combobox")
        return live.style & 0x000F;
    if (className == "listbox")
        return live.style & (LBS_MULTIPLESEL | LBS_EXTENDEDSEL);
    if (className == "scrollbar")
        return live.style & (SBS_VERT | SBS_SIZEBOX);
    if (className == "syslistview32")
        return live.style & (LVS_TYPEMASK | LVS_OWNERDATA | LVS_SINGLESEL);
    if (className == "systreeview32")
        return live.style & (TVS_CHECKBOXES | TVS_FULLROWSELECT);
    return 0;
}

bool button_style_supports_check(LONG_PTR style) {
    switch (style & BS_TYPEMASK) {
    case BS_CHECKBOX:
    case BS_AUTOCHECKBOX:
    case BS_RADIOBUTTON:
    case BS_3STATE:
    case BS_AUTO3STATE:
    case BS_AUTORADIOBUTTON:
        return true;
    default:
        return false;
    }
}

bool button_style_supports_indeterminate(LONG_PTR style) {
    const LONG_PTR type = style & BS_TYPEMASK;
    return type == BS_3STATE || type == BS_AUTO3STATE;
}

bool listbox_has_single_selection(LONG_PTR style) {
    return (style & (LBS_MULTIPLESEL | LBS_EXTENDEDSEL)) == 0;
}

bool win32_text_is_meaningful(const LiveTarget& live) {
    const auto& className = live.target.window.normalizedClass;
    return className != "combobox" &&
           className != "listbox" &&
           className != "scrollbar";
}

bool comctl_text_is_meaningful(const LiveTarget& live) {
    return live.target.kind != TargetKind::hwnd;
}

std::string cross_bitness_reason() {
    return "Pointer-bearing native control operations are disabled across "
           "process architectures";
}

ReadResult read_window_text(const LiveTarget& live) {
    if (!live.pointerAllowed)
        return unavailable(cross_bitness_reason());

    auto length = send_native_message(live.target.window, WM_GETTEXTLENGTH);
    if (!length.ok)
        return read_failure(length);
    if (length.value < 0 ||
        static_cast<uint64_t>(length.value) > kMaximumNativeTextChars) {
        ReadResult result;
        result.ok = false;
        result.hresult = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        result.error = "The native control reported an invalid text length";
        return result;
    }

    const size_t chars = static_cast<size_t>(length.value) + 1;
    std::vector<wchar_t> text(chars, L'\0');
    // WM_GETTEXT is below WM_USER and User32 marshals its string buffer.
    // Common-control WM_USER messages do not, so those use RemoteBuffer.
    auto message = send_native_message(
        live.target.window, WM_GETTEXT, chars,
        reinterpret_cast<LPARAM>(text.data()));
    if (!message.ok)
        return read_failure(message);
    const size_t copied = std::min(
        static_cast<size_t>((std::max<LRESULT>)(message.value, 0)),
        chars - 1);
    ReadResult result;
    result.value = native_utf16_to_utf8(
        std::wstring_view(text.data(), copied));
    return result;
}

PropertyMutationResult set_window_text(
    const LiveTarget& live, const std::string& value) {
    if (!live.pointerAllowed)
        return mutation_failure(E_ACCESSDENIED, cross_bitness_reason());

    std::wstring text;
    std::string conversionError;
    if (!native_utf8_to_utf16(value, text, conversionError))
        return mutation_failure(E_INVALIDARG, conversionError);
    text.push_back(L'\0');
    // WM_SETTEXT has the same system marshalling guarantee as WM_GETTEXT.
    auto message = send_native_message(
        live.target.window, WM_SETTEXT, 0,
        reinterpret_cast<LPARAM>(text.data()));
    if (!message.ok)
        return mutation_failure(message);
    if (!message.value) {
        return mutation_failure(
            HRESULT_FROM_WIN32(ERROR_INVALID_DATA),
            "The native control rejected the text value");
    }
    return {.ok = true, .hresult = S_OK};
}

ReadResult read_edit_selection(
    const LiveTarget& live, bool startValue) {
    if (!live.pointerAllowed)
        return unavailable(cross_bitness_reason());

    DWORD values[2]{};
    // EM_GETSEL is a system Edit message, so User32 marshals these DWORD
    // outputs. EM_* messages that carry control-defined structures would not.
    auto message = send_native_message(
        live.target.window, EM_GETSEL,
        reinterpret_cast<WPARAM>(&values[0]),
        reinterpret_cast<LPARAM>(&values[1]));
    if (!message.ok)
        return read_failure(message);

    ReadResult result;
    result.value = std::to_string(values[startValue ? 0 : 1]);
    return result;
}

bool read_scroll_info(
    const LiveTarget& live, SCROLLINFO& info,
    NativeMessageResult& failureResult) {
    if (!live.pointerAllowed) {
        failureResult.hresult = E_ACCESSDENIED;
        failureResult.win32Error = ERROR_ACCESS_DENIED;
        failureResult.error = cross_bitness_reason();
        return false;
    }
    info = {sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS};
    // Scroll-bar SBM_* messages are system messages and marshal SCROLLINFO.
    auto message = send_native_message(
        live.target.window, SBM_GETSCROLLINFO, 0,
        reinterpret_cast<LPARAM>(&info));
    if (!message.ok) {
        failureResult = std::move(message);
        return false;
    }
    if (!message.value) {
        failureResult.hresult = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        failureResult.win32Error = ERROR_INVALID_DATA;
        failureResult.error =
            "The scroll bar did not return its current range";
        return false;
    }
    failureResult.ok = true;
    failureResult.hresult = S_OK;
    failureResult.win32Error = ERROR_SUCCESS;
    failureResult.error.clear();
    return true;
}

PropertyMutationResult write_scroll_info(
    const LiveTarget& live, const SCROLLINFO& info) {
    auto message = send_native_message(
        live.target.window, SBM_SETSCROLLINFO, TRUE,
        reinterpret_cast<LPARAM>(&info));
    if (!message.ok)
        return mutation_failure(message);
    return {.ok = true, .hresult = S_OK};
}

struct ListViewItemData {
    std::string text;
    LPARAM parameter = 0;
    UINT state = 0;
};

bool read_listview_item(
    const LiveTarget& live, int index, ListViewItemData& data,
    NativeMessageResult& failureResult) {
    if (!live.pointerAllowed) {
        failureResult.hresult = E_ACCESSDENIED;
        failureResult.win32Error = ERROR_ACCESS_DENIED;
        failureResult.error = cross_bitness_reason();
        return false;
    }

    constexpr size_t itemSize = sizeof(LVITEMW);
    constexpr size_t textBytes = kControlTextChars * sizeof(wchar_t);
    auto remote = RemoteBuffer::allocate(
        live.target.window, itemSize + textBytes, failureResult);
    if (!remote)
        return false;

    LVITEMW item{};
    item.mask = LVIF_TEXT | LVIF_PARAM | LVIF_STATE;
    item.iItem = index;
    item.iSubItem = 0;
    item.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
    item.pszText = reinterpret_cast<wchar_t*>(
        static_cast<std::byte*>(remote.address()) + itemSize);
    item.cchTextMax = static_cast<int>(kControlTextChars);
    if (!remote.write(&item, sizeof(item), failureResult))
        return false;

    auto message = send_native_message(
        live.target.window, LVM_GETITEMW, 0,
        reinterpret_cast<LPARAM>(remote.address()));
    if (!message.ok) {
        failureResult = std::move(message);
        return false;
    }
    if (!message.value) {
        failureResult.hresult = HRESULT_FROM_WIN32(ERROR_INVALID_INDEX);
        failureResult.win32Error = ERROR_INVALID_INDEX;
        failureResult.error = "The list-view item no longer exists";
        return false;
    }

    LVITEMW returned{};
    std::vector<wchar_t> text(kControlTextChars, L'\0');
    if (!remote.read(&returned, sizeof(returned), failureResult) ||
        !remote.read(
            text.data(), textBytes, failureResult, itemSize)) {
        return false;
    }
    const size_t length = wcsnlen_s(text.data(), text.size());
    data.text = native_utf16_to_utf8(
        std::wstring_view(text.data(), length));
    data.parameter = returned.lParam;
    data.state = returned.state;
    return true;
}

PropertyMutationResult write_listview_item_state(
    const LiveTarget& live, UINT bit, bool value) {
    NativeMessageResult native;
    auto remote =
        RemoteBuffer::allocate(live.target.window, sizeof(LVITEMW), native);
    if (!remote)
        return mutation_failure(native);
    LVITEMW item{};
    item.stateMask = bit;
    item.state = value ? bit : 0;
    if (!remote.write(&item, sizeof(item), native))
        return mutation_failure(native);
    auto message = send_native_message(
        live.target.window, LVM_SETITEMSTATE,
        static_cast<WPARAM>(live.target.index),
        reinterpret_cast<LPARAM>(remote.address()));
    if (!message.ok)
        return mutation_failure(message);
    if (!message.value) {
        return mutation_failure(
            HRESULT_FROM_WIN32(ERROR_INVALID_INDEX),
            "The list-view item rejected the state change");
    }
    return {.ok = true, .hresult = S_OK};
}

PropertyMutationResult write_listview_item_text(
    const LiveTarget& live, const std::string& value) {
    std::wstring text;
    std::string conversionError;
    if (!native_utf8_to_utf16(value, text, conversionError))
        return mutation_failure(E_INVALIDARG, conversionError);
    text.push_back(L'\0');

    const size_t itemSize = sizeof(LVITEMW);
    const size_t textBytes = text.size() * sizeof(wchar_t);
    NativeMessageResult native;
    auto remote = RemoteBuffer::allocate(
        live.target.window, itemSize + textBytes, native);
    if (!remote)
        return mutation_failure(native);
    LVITEMW item{};
    item.iSubItem = 0;
    item.pszText = reinterpret_cast<wchar_t*>(
        static_cast<std::byte*>(remote.address()) + itemSize);
    if (!remote.write(&item, sizeof(item), native) ||
        !remote.write(text.data(), textBytes, native, itemSize)) {
        return mutation_failure(native);
    }
    auto message = send_native_message(
        live.target.window, LVM_SETITEMTEXTW,
        static_cast<WPARAM>(live.target.index),
        reinterpret_cast<LPARAM>(remote.address()));
    if (!message.ok)
        return mutation_failure(message);
    if (!message.value) {
        return mutation_failure(
            HRESULT_FROM_WIN32(ERROR_INVALID_INDEX),
            "The list-view item rejected the text value");
    }
    return {.ok = true, .hresult = S_OK};
}

struct TreeViewItemData {
    std::string text;
    UINT state = 0;
};

bool read_treeview_item(
    const LiveTarget& live, TreeViewItemData& data,
    NativeMessageResult& failureResult) {
    constexpr size_t itemSize = sizeof(TVITEMW);
    constexpr size_t textBytes = kControlTextChars * sizeof(wchar_t);
    auto remote = RemoteBuffer::allocate(
        live.target.window, itemSize + textBytes, failureResult);
    if (!remote)
        return false;

    TVITEMW item{};
    item.mask = TVIF_TEXT | TVIF_STATE;
    item.hItem = reinterpret_cast<HTREEITEM>(live.target.itemHandle);
    item.stateMask = TVIS_SELECTED | TVIS_EXPANDED;
    item.pszText = reinterpret_cast<wchar_t*>(
        static_cast<std::byte*>(remote.address()) + itemSize);
    item.cchTextMax = static_cast<int>(kControlTextChars);
    if (!remote.write(&item, sizeof(item), failureResult))
        return false;
    auto message = send_native_message(
        live.target.window, TVM_GETITEMW, 0,
        reinterpret_cast<LPARAM>(remote.address()));
    if (!message.ok) {
        failureResult = std::move(message);
        return false;
    }
    if (!message.value) {
        failureResult.hresult = HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE);
        failureResult.win32Error = ERROR_INVALID_HANDLE;
        failureResult.error = "The tree-view item no longer exists";
        return false;
    }

    TVITEMW returned{};
    std::vector<wchar_t> text(kControlTextChars, L'\0');
    if (!remote.read(&returned, sizeof(returned), failureResult) ||
        !remote.read(text.data(), textBytes, failureResult, itemSize)) {
        return false;
    }
    const size_t length = wcsnlen_s(text.data(), text.size());
    data.text = native_utf16_to_utf8(
        std::wstring_view(text.data(), length));
    data.state = returned.state;
    return true;
}

PropertyMutationResult write_treeview_item_text(
    const LiveTarget& live, const std::string& value) {
    std::wstring text;
    std::string conversionError;
    if (!native_utf8_to_utf16(value, text, conversionError))
        return mutation_failure(E_INVALIDARG, conversionError);
    text.push_back(L'\0');

    const size_t itemSize = sizeof(TVITEMW);
    const size_t textBytes = text.size() * sizeof(wchar_t);
    NativeMessageResult native;
    auto remote = RemoteBuffer::allocate(
        live.target.window, itemSize + textBytes, native);
    if (!remote)
        return mutation_failure(native);
    TVITEMW item{};
    item.mask = TVIF_TEXT;
    item.hItem = reinterpret_cast<HTREEITEM>(live.target.itemHandle);
    item.pszText = reinterpret_cast<wchar_t*>(
        static_cast<std::byte*>(remote.address()) + itemSize);
    if (!remote.write(&item, sizeof(item), native) ||
        !remote.write(text.data(), textBytes, native, itemSize)) {
        return mutation_failure(native);
    }
    auto message = send_native_message(
        live.target.window, TVM_SETITEMW, 0,
        reinterpret_cast<LPARAM>(remote.address()));
    if (!message.ok)
        return mutation_failure(message);
    if (!message.value) {
        return mutation_failure(
            HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE),
            "The tree-view item rejected the text value");
    }
    return {.ok = true, .hresult = S_OK};
}

bool read_toolbar_button(
    const LiveTarget& live, TBBUTTON& button,
    NativeMessageResult& failureResult) {
    auto remote = RemoteBuffer::allocate(
        live.target.window, sizeof(TBBUTTON), failureResult);
    if (!remote)
        return false;
    auto message = send_native_message(
        live.target.window, TB_GETBUTTON,
        static_cast<WPARAM>(live.target.index),
        reinterpret_cast<LPARAM>(remote.address()));
    if (!message.ok) {
        failureResult = std::move(message);
        return false;
    }
    if (!message.value) {
        failureResult.hresult = HRESULT_FROM_WIN32(ERROR_INVALID_INDEX);
        failureResult.win32Error = ERROR_INVALID_INDEX;
        failureResult.error = "The toolbar button no longer exists";
        return false;
    }
    return remote.read(&button, sizeof(button), failureResult);
}

ReadResult read_toolbar_text(const LiveTarget& live) {
    NativeMessageResult native;
    auto remote = RemoteBuffer::allocate(
        live.target.window, kControlTextChars * sizeof(wchar_t), native);
    if (!remote)
        return read_failure(native);
    std::vector<wchar_t> zero(kControlTextChars, L'\0');
    if (!remote.write(zero.data(), zero.size() * sizeof(wchar_t), native))
        return read_failure(native);
    auto message = send_native_message(
        live.target.window, TB_GETBUTTONTEXTW,
        static_cast<WPARAM>(live.target.commandId),
        reinterpret_cast<LPARAM>(remote.address()));
    if (!message.ok)
        return read_failure(message);
    if (message.value < 0) {
        ReadResult result;
        result.ok = false;
        result.hresult = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        result.error = "The toolbar button text is unavailable";
        return result;
    }
    std::vector<wchar_t> text(kControlTextChars, L'\0');
    if (!remote.read(text.data(), text.size() * sizeof(wchar_t), native))
        return read_failure(native);
    const size_t length = std::min(
        static_cast<size_t>(message.value), text.size() - 1);
    ReadResult result;
    result.value = native_utf16_to_utf8(
        std::wstring_view(text.data(), length));
    return result;
}

PropertyMutationResult write_toolbar_text(
    const LiveTarget& live, const std::string& value) {
    std::wstring text;
    std::string conversionError;
    if (!native_utf8_to_utf16(value, text, conversionError))
        return mutation_failure(E_INVALIDARG, conversionError);
    text.push_back(L'\0');

    const size_t infoSize = sizeof(TBBUTTONINFOW);
    const size_t textBytes = text.size() * sizeof(wchar_t);
    NativeMessageResult native;
    auto remote = RemoteBuffer::allocate(
        live.target.window, infoSize + textBytes, native);
    if (!remote)
        return mutation_failure(native);
    TBBUTTONINFOW info{sizeof(info)};
    info.dwMask = TBIF_TEXT;
    info.pszText = reinterpret_cast<wchar_t*>(
        static_cast<std::byte*>(remote.address()) + infoSize);
    if (!remote.write(&info, sizeof(info), native) ||
        !remote.write(text.data(), textBytes, native, infoSize)) {
        return mutation_failure(native);
    }
    auto message = send_native_message(
        live.target.window, TB_SETBUTTONINFOW,
        static_cast<WPARAM>(live.target.commandId),
        reinterpret_cast<LPARAM>(remote.address()));
    if (!message.ok)
        return mutation_failure(message);
    if (!message.value) {
        return mutation_failure(
            HRESULT_FROM_WIN32(ERROR_INVALID_DATA),
            "The toolbar button rejected the text value");
    }
    return {.ok = true, .hresult = S_OK};
}

struct StatusTextData {
    std::string text;
    UINT style = 0;
};

bool read_status_text(
    const LiveTarget& live, StatusTextData& data,
    NativeMessageResult& failureResult) {
    auto length = send_native_message(
        live.target.window, SB_GETTEXTLENGTHW,
        static_cast<WPARAM>(live.target.index));
    if (!length.ok) {
        failureResult = std::move(length);
        return false;
    }
    const size_t chars = static_cast<size_t>(LOWORD(length.value)) + 1;
    data.style = HIWORD(length.value);
    auto remote = RemoteBuffer::allocate(
        live.target.window, chars * sizeof(wchar_t), failureResult);
    if (!remote)
        return false;
    std::vector<wchar_t> zero(chars, L'\0');
    if (!remote.write(zero.data(), zero.size() * sizeof(wchar_t), failureResult))
        return false;
    auto message = send_native_message(
        live.target.window, SB_GETTEXTW,
        static_cast<WPARAM>(live.target.index),
        reinterpret_cast<LPARAM>(remote.address()));
    if (!message.ok) {
        failureResult = std::move(message);
        return false;
    }
    std::vector<wchar_t> text(chars, L'\0');
    if (!remote.read(
            text.data(), text.size() * sizeof(wchar_t), failureResult)) {
        return false;
    }
    const size_t copied =
        std::min(static_cast<size_t>(LOWORD(message.value)), chars - 1);
    data.text = native_utf16_to_utf8(
        std::wstring_view(text.data(), copied));
    return true;
}

PropertyMutationResult write_status_text(
    const LiveTarget& live, const std::string& value) {
    StatusTextData current;
    NativeMessageResult native;
    if (!read_status_text(live, current, native))
        return mutation_failure(native);

    std::wstring text;
    std::string conversionError;
    if (!native_utf8_to_utf16(value, text, conversionError))
        return mutation_failure(E_INVALIDARG, conversionError);
    text.push_back(L'\0');
    auto remote = RemoteBuffer::allocate(
        live.target.window, text.size() * sizeof(wchar_t), native);
    if (!remote)
        return mutation_failure(native);
    if (!remote.write(text.data(), text.size() * sizeof(wchar_t), native))
        return mutation_failure(native);
    auto message = send_native_message(
        live.target.window, SB_SETTEXTW,
        static_cast<WPARAM>(live.target.index) | current.style,
        reinterpret_cast<LPARAM>(remote.address()));
    if (!message.ok)
        return mutation_failure(message);
    if (!message.value) {
        return mutation_failure(
            HRESULT_FROM_WIN32(ERROR_INVALID_DATA),
            "The status-bar part rejected the text value");
    }
    return {.ok = true, .hresult = S_OK};
}

ReadResult read_tab_text(const LiveTarget& live) {
    constexpr size_t itemSize = sizeof(TCITEMW);
    constexpr size_t textBytes = kControlTextChars * sizeof(wchar_t);
    NativeMessageResult native;
    auto remote = RemoteBuffer::allocate(
        live.target.window, itemSize + textBytes, native);
    if (!remote)
        return read_failure(native);
    TCITEMW item{};
    item.mask = TCIF_TEXT;
    item.pszText = reinterpret_cast<wchar_t*>(
        static_cast<std::byte*>(remote.address()) + itemSize);
    item.cchTextMax = static_cast<int>(kControlTextChars);
    if (!remote.write(&item, sizeof(item), native))
        return read_failure(native);
    auto message = send_native_message(
        live.target.window, TCM_GETITEMW,
        static_cast<WPARAM>(live.target.index),
        reinterpret_cast<LPARAM>(remote.address()));
    if (!message.ok)
        return read_failure(message);
    if (!message.value) {
        ReadResult result;
        result.ok = false;
        result.hresult = HRESULT_FROM_WIN32(ERROR_INVALID_INDEX);
        result.error = "The tab item no longer exists";
        return result;
    }
    std::vector<wchar_t> text(kControlTextChars, L'\0');
    if (!remote.read(text.data(), textBytes, native, itemSize))
        return read_failure(native);
    const size_t length = wcsnlen_s(text.data(), text.size());
    ReadResult result;
    result.value = native_utf16_to_utf8(
        std::wstring_view(text.data(), length));
    return result;
}

PropertyMutationResult write_tab_text(
    const LiveTarget& live, const std::string& value) {
    std::wstring text;
    std::string conversionError;
    if (!native_utf8_to_utf16(value, text, conversionError))
        return mutation_failure(E_INVALIDARG, conversionError);
    text.push_back(L'\0');

    const size_t itemSize = sizeof(TCITEMW);
    const size_t textBytes = text.size() * sizeof(wchar_t);
    NativeMessageResult native;
    auto remote = RemoteBuffer::allocate(
        live.target.window, itemSize + textBytes, native);
    if (!remote)
        return mutation_failure(native);
    TCITEMW item{};
    item.mask = TCIF_TEXT;
    item.pszText = reinterpret_cast<wchar_t*>(
        static_cast<std::byte*>(remote.address()) + itemSize);
    if (!remote.write(&item, sizeof(item), native) ||
        !remote.write(text.data(), textBytes, native, itemSize)) {
        return mutation_failure(native);
    }
    auto message = send_native_message(
        live.target.window, TCM_SETITEMW,
        static_cast<WPARAM>(live.target.index),
        reinterpret_cast<LPARAM>(remote.address()));
    if (!message.ok)
        return mutation_failure(message);
    if (!message.value) {
        return mutation_failure(
            HRESULT_FROM_WIN32(ERROR_INVALID_INDEX),
            "The tab item rejected the text value");
    }
    return {.ok = true, .hresult = S_OK};
}

} // namespace

namespace native_property_detail {

bool parse_boolean(const std::string& value, bool& parsed) {
    const auto normalized = lower(value);
    if (normalized == "true") {
        parsed = true;
        return true;
    }
    if (normalized == "false") {
        parsed = false;
        return true;
    }
    return false;
}

bool parse_integer(const std::string& value, int& parsed) {
    if (value.empty())
        return false;
    int result = 0;
    const auto converted = std::from_chars(
        value.data(), value.data() + value.size(), result);
    if (converted.ec != std::errc() ||
        converted.ptr != value.data() + value.size()) {
        return false;
    }
    parsed = result;
    return true;
}

int effective_scroll_max(int minimum, int maximum, unsigned pageSize) {
    if (maximum < minimum)
        return minimum;
    const int64_t pageAdjustment =
        pageSize > 0 ? static_cast<int64_t>(pageSize) - 1 : 0;
    const int64_t effective =
        static_cast<int64_t>(maximum) - pageAdjustment;
    return static_cast<int>((std::max)(
        static_cast<int64_t>(minimum), effective));
}

} // namespace native_property_detail

struct NativePropertyConnection::Impl {
    NativeWindowIdentity root;
    Architecture targetArchitecture = Architecture::unknown;
    std::string provider;
    std::string controlVersion;
    mutable std::mutex mutex;
    std::map<uint64_t, Target> targets;
    std::map<std::string, uint64_t> itemHandles;
    uint64_t nextSyntheticHandle = kSyntheticHandleBase;
    std::map<std::string, std::shared_ptr<const PropertySchema>> schemasByKey;
    std::map<std::string, Mutation> mutationsByDescriptor;

    bool capture_live(
        const Target& target, LiveTarget& live,
        HRESULT& hresult, std::string& error) const {
        auto valid = validate_native_window(target.window);
        if (!valid.ok) {
            hresult = valid.hresult;
            error = valid.error;
            return false;
        }

        live.target = target;
        live.style = GetWindowLongPtrW(target.window.hwnd, GWL_STYLE);
        live.pointerAllowed = native_pointer_operations_allowed(
            get_host_architecture(), targetArchitecture);
        live.ownerData =
            target.window.normalizedClass == "syslistview32" &&
            (live.style & LVS_OWNERDATA) != 0;

        switch (target.kind) {
        case TargetKind::hwnd:
            return true;
        case TargetKind::listviewItem: {
            auto count = send_native_message(target.window, LVM_GETITEMCOUNT);
            if (!count.ok) {
                hresult = count.hresult;
                error = count.error;
                return false;
            }
            if (target.index < 0 || target.index >= count.value) {
                hresult = HRESULT_FROM_WIN32(ERROR_INVALID_INDEX);
                error = "The list-view item index is no longer valid";
                return false;
            }
            if (live.ownerData) {
                live.identityVerified = false;
                live.identityReason =
                    "Owner-data list views keep item identity and text in the "
                    "application; editing is not safe";
                return true;
            }
            if (!live.pointerAllowed) {
                live.identityVerified = false;
                live.identityReason = cross_bitness_reason();
                return true;
            }

            ListViewItemData current;
            NativeMessageResult native;
            if (!read_listview_item(live, target.index, current, native)) {
                hresult = native.hresult;
                error = native.error;
                return false;
            }
            if (count.value > static_cast<LRESULT>(kMaximumIdentityScanItems)) {
                live.identityVerified = false;
                live.identityReason =
                    "The list view is too large to prove that this item's "
                    "current identity is unique";
                live.identity =
                    current.parameter != 0
                        ? "param:" +
                              hex_u64(static_cast<uint64_t>(
                                  static_cast<ULONG_PTR>(
                                      current.parameter)))
                        : "text:" + current.text;
                return true;
            }

            int matchingIdentity = 0;
            for (int index = 0; index < count.value; ++index) {
                ListViewItemData candidate;
                if (!read_listview_item(live, index, candidate, native)) {
                    hresult = native.hresult;
                    error = native.error;
                    return false;
                }
                if (current.parameter != 0
                        ? candidate.parameter == current.parameter
                        : candidate.text == current.text) {
                    ++matchingIdentity;
                }
            }
            live.identity =
                current.parameter != 0
                    ? "param:" +
                          hex_u64(static_cast<uint64_t>(
                              static_cast<ULONG_PTR>(current.parameter)))
                    : "text:" + current.text;
            if (matchingIdentity != 1) {
                live.identityVerified = false;
                live.identityReason =
                    "This list-view item has no unique application identity "
                    "or unique current text";
            }
            return true;
        }
        case TargetKind::treeviewItem: {
            if (!live.pointerAllowed) {
                live.identityVerified = false;
                live.identityReason = cross_bitness_reason();
                return true;
            }
            TreeViewItemData item;
            NativeMessageResult native;
            if (!read_treeview_item(live, item, native)) {
                hresult = native.hresult;
                error = native.error;
                return false;
            }
            auto parent = send_native_message(
                target.window, TVM_GETNEXTITEM, TVGN_PARENT,
                static_cast<LPARAM>(target.itemHandle));
            if (!parent.ok) {
                hresult = parent.hresult;
                error = parent.error;
                return false;
            }
            live.identity =
                "item:" + hex_u64(target.itemHandle) +
                "|parent:" +
                hex_u64(static_cast<uint64_t>(parent.value)) +
                "|text:" + item.text;
            return true;
        }
        case TargetKind::toolbarButton: {
            if (!live.pointerAllowed) {
                live.identityVerified = false;
                live.identityReason = cross_bitness_reason();
                return true;
            }
            TBBUTTON button{};
            NativeMessageResult native;
            if (!read_toolbar_button(live, button, native)) {
                hresult = native.hresult;
                error = native.error;
                return false;
            }
            if (button.idCommand != target.commandId) {
                hresult = HRESULT_FROM_WIN32(ERROR_INVALID_INDEX);
                error =
                    "The toolbar index now refers to a different command";
                return false;
            }
            live.checkable = (button.fsStyle & BTNS_CHECK) != 0;
            live.identity = "command:" + std::to_string(target.commandId);
            return true;
        }
        case TargetKind::statusbarPart: {
            auto count =
                send_native_message(target.window, SB_GETPARTS, 0, 0);
            if (!count.ok) {
                hresult = count.hresult;
                error = count.error;
                return false;
            }
            if (target.index < 0 || target.index >= count.value) {
                hresult = HRESULT_FROM_WIN32(ERROR_INVALID_INDEX);
                error = "The status-bar part index is no longer valid";
                return false;
            }
            live.identity = "part:" + std::to_string(target.index);
            return true;
        }
        case TargetKind::tabItem: {
            auto count =
                send_native_message(target.window, TCM_GETITEMCOUNT, 0, 0);
            if (!count.ok) {
                hresult = count.hresult;
                error = count.error;
                return false;
            }
            if (target.index < 0 || target.index >= count.value) {
                hresult = HRESULT_FROM_WIN32(ERROR_INVALID_INDEX);
                error = "The tab item index is no longer valid";
                return false;
            }
            if (!live.pointerAllowed) {
                live.identityVerified = false;
                live.identityReason = cross_bitness_reason();
                return true;
            }
            auto text = read_tab_text(live);
            if (!text.ok) {
                hresult = text.hresult;
                error = text.error;
                return false;
            }
            live.identity = "text:" + text.value;
            return true;
        }
        }
        return true;
    }

    std::string schema_key(const LiveTarget& live) const {
        std::ostringstream key;
        key << provider << '|'
            << live.target.window.normalizedClass << '|'
            << controlVersion << '|'
            << target_kind_name(live.target.kind) << '|'
            << static_cast<uint64_t>(capability_style(live)) << '|'
            << architecture_name(get_host_architecture()) << '|'
            << architecture_name(targetArchitecture) << '|'
            << (live.pointerAllowed ? "same-abi" : "cross-abi") << '|'
            << (live.ownerData ? "owner-data" : "stored-data") << '|'
            << (live.identityVerified ? "verified" : "unverified") << '|'
            << (live.checkable ? "checkable" : "not-checkable");
        return key.str();
    }

    std::string read_only_reason(
        Operation operation, const LiveTarget& live) const {
        if (operation_needs_remote_pointer(operation) &&
            !live.pointerAllowed) {
            return cross_bitness_reason();
        }
        if (!live.identityVerified &&
            live.target.kind != TargetKind::hwnd) {
            return live.identityReason.empty()
                ? "The provider could not revalidate this native item identity"
                : live.identityReason;
        }

        switch (operation) {
        case Operation::text:
            if (provider == "win32" &&
                live.target.window.normalizedClass == "edit" &&
                (live.style & ES_PASSWORD) != 0) {
                return "Password Edit text is not exposed for mutation";
            }
            if (provider == "win32" && !win32_text_is_meaningful(live)) {
                return "This control class does not define mutable window text; "
                       "use its curated state property instead";
            }
            if (provider == "comctl" && !comctl_text_is_meaningful(live)) {
                return "This common control exposes text through its items or "
                       "parts rather than window text";
            }
            break;
        case Operation::buttonCheckState:
            if (!button_style_supports_check(live.style)) {
                return "This Button style is not a checkbox, radio button, or "
                       "three-state button";
            }
            break;
        case Operation::selectedIndex:
            if (live.target.window.normalizedClass == "listbox" &&
                !listbox_has_single_selection(live.style)) {
                return "Multi-selection list boxes do not have one selected index";
            }
            break;
        case Operation::scrollPageSize:
            return "Page size is owned by the control layout; it is exposed "
                   "read-only in this conservative editor";
        case Operation::toolbarButtonChecked:
            if (!live.checkable)
                return "This toolbar button style is not checkable";
            break;
        default:
            break;
        }
        return {};
    }

    std::shared_ptr<const PropertySchema> schema_for(
        const LiveTarget& live) {
        const auto key = schema_key(live);
        auto found = schemasByKey.find(key);
        if (found != schemasByKey.end())
            return found->second;

        const auto hash = hex_u64(stable_hash(key));
        auto schema = std::make_shared<PropertySchema>();
        schema->schemaId =
            "native-v1:" + provider + ":s" + hash;

        struct Spec {
            Operation operation;
            const char* name;
            const char* displayName;
            const char* declaringType;
            const char* propertyType;
            PropertyEditorKind kind;
            bool supportsClear = false;
            std::vector<PropertyChoice> choices;
            const char* description = "";
        };
        std::vector<Spec> specs;
        const auto& className = live.target.window.normalizedClass;
        if (live.target.kind == TargetKind::hwnd) {
            specs.push_back({
                Operation::text, "Text", "Text", className.c_str(), "String",
                PropertyEditorKind::string, false, {},
                "Window or control text"});
            specs.push_back({
                Operation::enabled, "Enabled", "Enabled", className.c_str(),
                "Boolean", PropertyEditorKind::boolean, false,
                {{"false", "False"}, {"true", "True"}},
                "Whether the HWND accepts input"});

            if (provider == "win32" && className == "button") {
                std::vector<PropertyChoice> choices{
                    {"unchecked", "Unchecked"},
                    {"checked", "Checked"},
                };
                if (button_style_supports_indeterminate(live.style))
                    choices.push_back({"indeterminate", "Indeterminate"});
                specs.push_back({
                    Operation::buttonCheckState, "CheckState", "Check state",
                    "Button", "Enum", PropertyEditorKind::enumeration,
                    false, std::move(choices),
                    "Checkbox, radio-button, or three-state value"});
            } else if (provider == "win32" && className == "edit") {
                specs.push_back({
                    Operation::editSelectionStart, "SelectionStart",
                    "Selection start", "Edit", "Int32",
                    PropertyEditorKind::integer, false, {},
                    "UTF-16 code-unit offset"});
                specs.push_back({
                    Operation::editSelectionEnd, "SelectionEnd",
                    "Selection end", "Edit", "Int32",
                    PropertyEditorKind::integer, false, {},
                    "UTF-16 code-unit offset"});
                specs.push_back({
                    Operation::editReadOnly, "ReadOnly", "Read-only", "Edit",
                    "Boolean", PropertyEditorKind::boolean, false,
                    {{"false", "False"}, {"true", "True"}},
                    "Whether user text editing is disabled"});
            } else if (
                provider == "win32" &&
                (className == "combobox" || className == "listbox")) {
                specs.push_back({
                    Operation::selectedIndex, "SelectedIndex",
                    "Selected index", className.c_str(), "Int32",
                    PropertyEditorKind::integer, true, {},
                    "-1 means no selection"});
            } else if (
                provider == "win32" && className == "scrollbar") {
                specs.push_back({
                    Operation::scrollMinimum, "Minimum", "Minimum",
                    "ScrollBar", "Int32", PropertyEditorKind::integer});
                specs.push_back({
                    Operation::scrollMaximum, "Maximum", "Maximum",
                    "ScrollBar", "Int32", PropertyEditorKind::integer});
                specs.push_back({
                    Operation::scrollPageSize, "PageSize", "Page size",
                    "ScrollBar", "UInt32", PropertyEditorKind::integer});
                specs.push_back({
                    Operation::scrollPosition, "Position", "Position",
                    "ScrollBar", "Int32", PropertyEditorKind::integer, false,
                    {}, "Validated against the effective scroll range"});
            } else if (
                provider == "comctl" && className == "syslistview32") {
                specs.push_back({
                    Operation::listviewMode, "ViewMode", "View mode",
                    "SysListView32", "Enum", PropertyEditorKind::enumeration,
                    false,
                    {{"icon", "Icon"},
                     {"details", "Details"},
                     {"smallIcon", "Small icon"},
                     {"list", "List"},
                     {"tile", "Tile"}}});
            } else if (
                provider == "comctl" && className == "systabcontrol32") {
                specs.push_back({
                    Operation::tabSelectedIndex, "SelectedIndex",
                    "Selected index", "SysTabControl32", "Int32",
                    PropertyEditorKind::integer, false, {},
                    "-1 removes the current selection"});
            }
        } else if (live.target.kind == TargetKind::listviewItem) {
            specs = {
                {Operation::listviewItemSelected, "Selected", "Selected",
                 "SysListView32.Item", "Boolean",
                 PropertyEditorKind::boolean, false,
                 {{"false", "False"}, {"true", "True"}}},
                {Operation::listviewItemFocused, "Focused", "Focused",
                 "SysListView32.Item", "Boolean",
                 PropertyEditorKind::boolean, false,
                 {{"false", "False"}, {"true", "True"}}},
                {Operation::listviewItemText, "Text", "Text",
                 "SysListView32.Item", "String",
                 PropertyEditorKind::string},
            };
        } else if (live.target.kind == TargetKind::treeviewItem) {
            specs = {
                {Operation::treeviewItemSelected, "Selected", "Selected",
                 "SysTreeView32.Item", "Boolean",
                 PropertyEditorKind::boolean, false,
                 {{"false", "False"}, {"true", "True"}}},
                {Operation::treeviewItemExpanded, "Expanded", "Expanded",
                 "SysTreeView32.Item", "Boolean",
                 PropertyEditorKind::boolean, false,
                 {{"false", "False"}, {"true", "True"}}},
                {Operation::treeviewItemText, "Text", "Text",
                 "SysTreeView32.Item", "String",
                 PropertyEditorKind::string},
            };
        } else if (live.target.kind == TargetKind::toolbarButton) {
            specs = {
                {Operation::toolbarButtonChecked, "Checked", "Checked",
                 "ToolbarWindow32.Button", "Boolean",
                 PropertyEditorKind::boolean, false,
                 {{"false", "False"}, {"true", "True"}}},
                {Operation::toolbarButtonEnabled, "Enabled", "Enabled",
                 "ToolbarWindow32.Button", "Boolean",
                 PropertyEditorKind::boolean, false,
                 {{"false", "False"}, {"true", "True"}}},
                {Operation::toolbarButtonText, "Text", "Text",
                 "ToolbarWindow32.Button", "String",
                 PropertyEditorKind::string},
            };
        } else if (live.target.kind == TargetKind::statusbarPart) {
            specs = {
                {Operation::statusbarPartText, "Text", "Text",
                 "msctls_statusbar32.Part", "String",
                 PropertyEditorKind::string},
            };
        } else if (live.target.kind == TargetKind::tabItem) {
            specs = {
                {Operation::tabItemText, "Text", "Text",
                 "SysTabControl32.Item", "String",
                 PropertyEditorKind::string},
            };
        }

        for (size_t index = 0; index < specs.size(); ++index) {
            auto& spec = specs[index];
            const auto reason = read_only_reason(spec.operation, live);
            const bool writable = reason.empty();

            PropertyDescriptor descriptor;
            descriptor.descriptorId =
                "native-v1:" + provider + ":s" + hash + ":p" +
                std::to_string(index);
            descriptor.name = spec.name;
            descriptor.displayName = spec.displayName;
            descriptor.provider = provider;
            descriptor.framework = provider;
            descriptor.declaringType = spec.declaringType;
            descriptor.propertyType = spec.propertyType;
            descriptor.kind =
                writable ? spec.kind : PropertyEditorKind::readonly;
            descriptor.choices = std::move(spec.choices);
            if (spec.kind == PropertyEditorKind::integer)
                descriptor.step = 1;
            descriptor.writable = writable;
            descriptor.supportsClear = writable && spec.supportsClear;
            descriptor.description = spec.description;

            mutationsByDescriptor.emplace(
                descriptor.descriptorId,
                Mutation{
                    schema->schemaId, spec.operation, descriptor.writable,
                    descriptor.supportsClear});
            schema->descriptors.push_back(std::move(descriptor));
        }

        std::shared_ptr<const PropertySchema> immutable = schema;
        schemasByKey.emplace(key, immutable);
        return immutable;
    }

    ReadResult read_operation(
        Operation operation, const LiveTarget& live) const {
        const auto reason = read_only_reason(operation, live);
        if (operation_needs_remote_pointer(operation) &&
            !live.pointerAllowed) {
            return unavailable(reason);
        }
        if (!live.identityVerified &&
            live.target.kind != TargetKind::hwnd) {
            return unavailable(reason);
        }

        switch (operation) {
        case Operation::text:
            if (!reason.empty())
                return unavailable(reason);
            return read_window_text(live);
        case Operation::enabled: {
            ReadResult result;
            result.value =
                IsWindowEnabled(live.target.window.hwnd) ? "true" : "false";
            return result;
        }
        case Operation::buttonCheckState: {
            auto checked =
                send_native_message(live.target.window, BM_GETCHECK);
            if (!checked.ok)
                return read_failure(checked);
            ReadResult result;
            if (checked.value == BST_CHECKED)
                result.value = "checked";
            else if (checked.value == BST_INDETERMINATE)
                result.value = "indeterminate";
            else
                result.value = "unchecked";
            return result;
        }
        case Operation::editSelectionStart:
            return read_edit_selection(live, true);
        case Operation::editSelectionEnd:
            return read_edit_selection(live, false);
        case Operation::editReadOnly: {
            ReadResult result;
            result.value =
                (GetWindowLongPtrW(
                     live.target.window.hwnd, GWL_STYLE) &
                 ES_READONLY)
                    ? "true"
                    : "false";
            return result;
        }
        case Operation::selectedIndex: {
            const UINT message =
                live.target.window.normalizedClass == "combobox"
                    ? CB_GETCURSEL
                    : LB_GETCURSEL;
            auto selected = send_native_message(live.target.window, message);
            if (!selected.ok)
                return read_failure(selected);
            ReadResult result;
            result.value = std::to_string(
                selected.value == CB_ERR || selected.value == LB_ERR
                    ? -1
                    : static_cast<int>(selected.value));
            return result;
        }
        case Operation::scrollMinimum:
        case Operation::scrollMaximum:
        case Operation::scrollPageSize:
        case Operation::scrollPosition: {
            SCROLLINFO info{};
            NativeMessageResult native;
            if (!read_scroll_info(live, info, native)) {
                if (native.hresult == E_ACCESSDENIED)
                    return unavailable(native.error);
                return read_failure(native);
            }
            ReadResult result;
            if (operation == Operation::scrollMinimum)
                result.value = std::to_string(info.nMin);
            else if (operation == Operation::scrollMaximum)
                result.value = std::to_string(info.nMax);
            else if (operation == Operation::scrollPageSize)
                result.value = std::to_string(info.nPage);
            else
                result.value = std::to_string(info.nPos);
            return result;
        }
        case Operation::listviewMode: {
            auto mode = send_native_message(live.target.window, LVM_GETVIEW);
            if (!mode.ok)
                return read_failure(mode);
            ReadResult result;
            switch (mode.value) {
            case LV_VIEW_ICON: result.value = "icon"; break;
            case LV_VIEW_DETAILS: result.value = "details"; break;
            case LV_VIEW_SMALLICON: result.value = "smallIcon"; break;
            case LV_VIEW_LIST: result.value = "list"; break;
            case LV_VIEW_TILE: result.value = "tile"; break;
            default:
                result.ok = false;
                result.hresult = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
                result.error = "The list view reported an unknown view mode";
                break;
            }
            return result;
        }
        case Operation::listviewItemSelected:
        case Operation::listviewItemFocused:
        case Operation::listviewItemText: {
            ListViewItemData item;
            NativeMessageResult native;
            if (!read_listview_item(
                    live, live.target.index, item, native)) {
                return read_failure(native);
            }
            ReadResult result;
            if (operation == Operation::listviewItemText)
                result.value = item.text;
            else if (operation == Operation::listviewItemSelected)
                result.value =
                    (item.state & LVIS_SELECTED) ? "true" : "false";
            else
                result.value =
                    (item.state & LVIS_FOCUSED) ? "true" : "false";
            return result;
        }
        case Operation::treeviewItemSelected:
        case Operation::treeviewItemExpanded:
        case Operation::treeviewItemText: {
            TreeViewItemData item;
            NativeMessageResult native;
            if (!read_treeview_item(live, item, native))
                return read_failure(native);
            ReadResult result;
            if (operation == Operation::treeviewItemText)
                result.value = item.text;
            else if (operation == Operation::treeviewItemSelected)
                result.value =
                    (item.state & TVIS_SELECTED) ? "true" : "false";
            else
                result.value =
                    (item.state & TVIS_EXPANDED) ? "true" : "false";
            return result;
        }
        case Operation::toolbarButtonChecked:
        case Operation::toolbarButtonEnabled: {
            auto state = send_native_message(
                live.target.window, TB_GETSTATE,
                static_cast<WPARAM>(live.target.commandId));
            if (!state.ok)
                return read_failure(state);
            if (state.value < 0) {
                ReadResult result;
                result.ok = false;
                result.hresult = HRESULT_FROM_WIN32(ERROR_INVALID_INDEX);
                result.error = "The toolbar command no longer exists";
                return result;
            }
            ReadResult result;
            result.value =
                (state.value &
                 (operation == Operation::toolbarButtonChecked
                      ? TBSTATE_CHECKED
                      : TBSTATE_ENABLED))
                    ? "true"
                    : "false";
            return result;
        }
        case Operation::toolbarButtonText:
            return read_toolbar_text(live);
        case Operation::statusbarPartText: {
            StatusTextData data;
            NativeMessageResult native;
            if (!read_status_text(live, data, native))
                return read_failure(native);
            ReadResult result;
            result.value = data.text;
            return result;
        }
        case Operation::tabSelectedIndex: {
            auto selected =
                send_native_message(live.target.window, TCM_GETCURSEL);
            if (!selected.ok)
                return read_failure(selected);
            ReadResult result;
            result.value = std::to_string(
                selected.value < 0 ? -1 : static_cast<int>(selected.value));
            return result;
        }
        case Operation::tabItemText:
            return read_tab_text(live);
        }
        return unavailable("Unsupported native property");
    }

    bool get_target(uint64_t handle, Target& target) {
        std::lock_guard<std::mutex> lock(mutex);
        auto found = targets.find(handle);
        if (found != targets.end()) {
            target = found->second;
            return true;
        }

        if (handle > static_cast<uint64_t>(
                         (std::numeric_limits<uintptr_t>::max)())) {
            return false;
        }
        NativeWindowIdentity identity;
        auto captured = capture_native_window_identity(
            reinterpret_cast<HWND>(static_cast<uintptr_t>(handle)),
            root.pid, identity);
        if (!captured.ok)
            return false;
        const bool common = is_comctl_class(identity.normalizedClass);
        if ((provider == "comctl") != common)
            return false;

        Target registered;
        registered.window = std::move(identity);
        targets.emplace(handle, registered);
        target = std::move(registered);
        return true;
    }

    uint64_t register_target(
        TargetKind kind, HWND hwnd, int index,
        uintptr_t itemHandle, int commandId) {
        NativeWindowIdentity identity;
        auto captured =
            capture_native_window_identity(hwnd, root.pid, identity);
        if (!captured.ok)
            return 0;

        if (kind == TargetKind::hwnd) {
            const uint64_t handle =
                static_cast<uint64_t>(reinterpret_cast<uintptr_t>(hwnd));
            std::lock_guard<std::mutex> lock(mutex);
            auto [found, inserted] = targets.emplace(handle, Target{});
            if (inserted) {
                found->second.kind = kind;
                found->second.window = std::move(identity);
            }
            return handle;
        }

        std::ostringstream key;
        key << target_kind_name(kind) << ':'
            << hex_u64(reinterpret_cast<uintptr_t>(hwnd)) << ':'
            << index << ':' << hex_u64(itemHandle) << ':' << commandId;

        std::lock_guard<std::mutex> lock(mutex);
        auto existing = itemHandles.find(key.str());
        if (existing != itemHandles.end())
            return existing->second;
        if (nextSyntheticHandle == 0)
            return 0;

        const uint64_t handle = nextSyntheticHandle++;
        Target target;
        target.kind = kind;
        target.window = std::move(identity);
        target.index = index;
        target.itemHandle = itemHandle;
        target.commandId = commandId;
        targets.emplace(handle, std::move(target));
        itemHandles.emplace(key.str(), handle);
        return handle;
    }

    bool resolve_mutation(
        uint64_t handle, const std::string& descriptorId,
        bool clearing, Target& target, LiveTarget& live,
        Mutation& mutation, PropertyMutationResult& failure) {
        if (!get_target(handle, target)) {
            failure = mutation_failure(
                HRESULT_FROM_WIN32(ERROR_NOT_FOUND),
                "The native property target is unknown or closed");
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(mutex);
            const auto current = targets.find(handle);
            if (current == targets.end() || !current->second.hasSnapshot) {
                failure = mutation_failure(
                    HRESULT_FROM_WIN32(ERROR_INVALID_STATE),
                    "No property schema has been read for this element; call "
                    "get_editable_properties first");
                return false;
            }
            target = current->second;
            const auto found = mutationsByDescriptor.find(descriptorId);
            if (found == mutationsByDescriptor.end() ||
                found->second.schemaId != target.schemaId) {
                failure = mutation_failure(
                    E_INVALIDARG,
                    "The property descriptor is unknown, stale, or does not "
                    "apply to this native element");
                return false;
            }
            mutation = found->second;
        }

        HRESULT hresult = S_OK;
        std::string error;
        if (!capture_live(target, live, hresult, error)) {
            failure = mutation_failure(hresult, std::move(error));
            return false;
        }

        std::shared_ptr<const PropertySchema> currentSchema;
        {
            std::lock_guard<std::mutex> lock(mutex);
            currentSchema = schema_for(live);
        }
        if (currentSchema->schemaId != target.schemaId) {
            failure = mutation_failure(
                E_INVALIDARG,
                "The native control capabilities changed; refresh its property "
                "descriptors before writing");
            return false;
        }
        if (live.target.kind != TargetKind::hwnd &&
            (!live.identityVerified ||
             live.identity != target.snapshotIdentity)) {
            failure = mutation_failure(
                HRESULT_FROM_WIN32(ERROR_INVALID_STATE),
                !live.identityVerified
                    ? live.identityReason
                    : "The native item at this index changed since its "
                      "properties were read");
            return false;
        }
        if (clearing ? !mutation.supportsClear : !mutation.writable) {
            failure = mutation_failure(
                E_ACCESSDENIED,
                clearing
                    ? "The native property does not support clearing"
                    : "The native property descriptor is read-only");
            return false;
        }
        return true;
    }

    PropertyMutationResult apply(
        uint64_t handle, const std::string& descriptorId,
        const std::optional<std::string>& requested) {
        Target target;
        LiveTarget live;
        Mutation mutation;
        PropertyMutationResult failure;
        if (!resolve_mutation(
                handle, descriptorId, !requested.has_value(),
                target, live, mutation, failure)) {
            return failure;
        }

        const auto invalidBoolean = [] {
            return mutation_failure(
                E_INVALIDARG, "The value must be 'true' or 'false'");
        };
        const auto invalidInteger = [] {
            return mutation_failure(
                E_INVALIDARG, "The value must be a base-10 32-bit integer");
        };

        PropertyMutationResult write{.ok = true, .hresult = S_OK};
        std::string expected;
        if (!requested) {
            expected = "-1";
            const UINT message =
                live.target.window.normalizedClass == "combobox"
                    ? CB_SETCURSEL
                    : LB_SETCURSEL;
            auto result = send_native_message(
                live.target.window, message,
                static_cast<WPARAM>(static_cast<INT_PTR>(-1)));
            if (!result.ok)
                return mutation_failure(result);
            if (result.value != CB_ERR && result.value != LB_ERR) {
                return mutation_failure(
                    HRESULT_FROM_WIN32(ERROR_INVALID_DATA),
                    "The control did not clear its selected index");
            }
        } else {
            expected = *requested;
            switch (mutation.operation) {
            case Operation::text:
                write = set_window_text(live, *requested);
                break;
            case Operation::enabled: {
                bool enabled = false;
                if (!native_property_detail::parse_boolean(
                        *requested, enabled)) {
                    return invalidBoolean();
                }
                expected = enabled ? "true" : "false";
                auto valid = validate_native_window(live.target.window);
                if (!valid.ok)
                    return mutation_failure(valid);
                LONG_PTR style = GetWindowLongPtrW(
                    live.target.window.hwnd, GWL_STYLE);
                LONG_PTR changed = enabled
                    ? style & ~static_cast<LONG_PTR>(WS_DISABLED)
                    : style | static_cast<LONG_PTR>(WS_DISABLED);
                SetLastError(ERROR_SUCCESS);
                const LONG_PTR previous = SetWindowLongPtrW(
                    live.target.window.hwnd, GWL_STYLE, changed);
                if (previous == 0 && GetLastError() != ERROR_SUCCESS) {
                    return mutation_failure(
                        HRESULT_FROM_WIN32(GetLastError()),
                        "Could not change the HWND enabled style");
                }
                auto notified = send_native_message(
                    live.target.window, WM_ENABLE, enabled ? TRUE : FALSE);
                if (!notified.ok) {
                    SetWindowLongPtrW(
                        live.target.window.hwnd, GWL_STYLE, style);
                    return mutation_failure(notified);
                }
                break;
            }
            case Operation::buttonCheckState: {
                int state = BST_UNCHECKED;
                const auto normalized = lower(*requested);
                if (normalized == "checked")
                    state = BST_CHECKED;
                else if (
                    normalized == "indeterminate" &&
                    button_style_supports_indeterminate(live.style)) {
                    state = BST_INDETERMINATE;
                } else if (normalized != "unchecked") {
                    return mutation_failure(
                        E_INVALIDARG,
                        "The check state is not valid for this Button style");
                }
                expected = normalized;
                auto result = send_native_message(
                    live.target.window, BM_SETCHECK,
                    static_cast<WPARAM>(state));
                if (!result.ok)
                    return mutation_failure(result);
                break;
            }
            case Operation::editSelectionStart:
            case Operation::editSelectionEnd: {
                int changed = 0;
                if (!native_property_detail::parse_integer(
                        *requested, changed) ||
                    changed < 0) {
                    return invalidInteger();
                }
                auto start = read_edit_selection(live, true);
                auto end = read_edit_selection(live, false);
                auto length =
                    send_native_message(live.target.window, WM_GETTEXTLENGTH);
                if (!start.ok)
                    return mutation_failure(start.hresult, start.error);
                if (!end.ok)
                    return mutation_failure(end.hresult, end.error);
                if (!length.ok)
                    return mutation_failure(length);
                int selectionStart = std::stoi(start.value);
                int selectionEnd = std::stoi(end.value);
                if (changed > length.value) {
                    return mutation_failure(
                        E_INVALIDARG,
                        "The selection offset exceeds the current text length");
                }
                if (mutation.operation == Operation::editSelectionStart)
                    selectionStart = changed;
                else
                    selectionEnd = changed;
                if (selectionStart > selectionEnd) {
                    return mutation_failure(
                        E_INVALIDARG,
                        "SelectionStart cannot exceed SelectionEnd");
                }
                auto result = send_native_message(
                    live.target.window, EM_SETSEL,
                    static_cast<WPARAM>(selectionStart),
                    static_cast<LPARAM>(selectionEnd));
                if (!result.ok)
                    return mutation_failure(result);
                expected = std::to_string(changed);
                break;
            }
            case Operation::editReadOnly: {
                bool readOnly = false;
                if (!native_property_detail::parse_boolean(
                        *requested, readOnly)) {
                    return invalidBoolean();
                }
                expected = readOnly ? "true" : "false";
                auto result = send_native_message(
                    live.target.window, EM_SETREADONLY,
                    readOnly ? TRUE : FALSE);
                if (!result.ok)
                    return mutation_failure(result);
                if (!result.value) {
                    return mutation_failure(
                        HRESULT_FROM_WIN32(ERROR_INVALID_DATA),
                        "The Edit control rejected the read-only change");
                }
                break;
            }
            case Operation::selectedIndex: {
                int index = 0;
                if (!native_property_detail::parse_integer(
                        *requested, index)) {
                    return invalidInteger();
                }
                const bool combo =
                    live.target.window.normalizedClass == "combobox";
                auto count = send_native_message(
                    live.target.window, combo ? CB_GETCOUNT : LB_GETCOUNT);
                if (!count.ok)
                    return mutation_failure(count);
                if (index < -1 || index >= count.value) {
                    return mutation_failure(
                        E_INVALIDARG,
                        "The selected index is outside the current item range");
                }
                auto result = send_native_message(
                    live.target.window,
                    combo ? CB_SETCURSEL : LB_SETCURSEL,
                    static_cast<WPARAM>(static_cast<INT_PTR>(index)));
                if (!result.ok)
                    return mutation_failure(result);
                if ((index == -1 && result.value != CB_ERR &&
                     result.value != LB_ERR) ||
                    (index >= 0 && result.value != index)) {
                    return mutation_failure(
                        HRESULT_FROM_WIN32(ERROR_INVALID_DATA),
                        "The control rejected the selected index");
                }
                expected = std::to_string(index);
                break;
            }
            case Operation::scrollMinimum:
            case Operation::scrollMaximum:
            case Operation::scrollPosition: {
                int changed = 0;
                if (!native_property_detail::parse_integer(
                        *requested, changed)) {
                    return invalidInteger();
                }
                SCROLLINFO info{};
                NativeMessageResult native;
                if (!read_scroll_info(live, info, native))
                    return mutation_failure(native);
                if (mutation.operation == Operation::scrollMinimum)
                    info.nMin = changed;
                else if (mutation.operation == Operation::scrollMaximum)
                    info.nMax = changed;
                else
                    info.nPos = changed;
                if (info.nMin > info.nMax) {
                    return mutation_failure(
                        E_INVALIDARG,
                        "The scroll minimum cannot exceed the maximum");
                }
                const int effectiveMaximum =
                    native_property_detail::effective_scroll_max(
                        info.nMin, info.nMax, info.nPage);
                if (info.nPos < info.nMin || info.nPos > effectiveMaximum) {
                    return mutation_failure(
                        E_INVALIDARG,
                        "The scroll position is outside the effective range");
                }
                info.fMask =
                    mutation.operation == Operation::scrollPosition
                        ? SIF_POS
                        : SIF_RANGE;
                write = write_scroll_info(live, info);
                expected = std::to_string(changed);
                break;
            }
            case Operation::scrollPageSize:
                return mutation_failure(
                    E_ACCESSDENIED,
                    "Page size is exposed read-only");
            case Operation::listviewMode: {
                const auto normalized = lower(*requested);
                int mode = -1;
                if (normalized == "icon")
                    mode = LV_VIEW_ICON;
                else if (normalized == "details")
                    mode = LV_VIEW_DETAILS;
                else if (normalized == "smallicon")
                    mode = LV_VIEW_SMALLICON;
                else if (normalized == "list")
                    mode = LV_VIEW_LIST;
                else if (normalized == "tile")
                    mode = LV_VIEW_TILE;
                else
                    return mutation_failure(
                        E_INVALIDARG, "Unknown list-view mode");
                expected =
                    mode == LV_VIEW_SMALLICON ? "smallIcon" : normalized;
                auto result = send_native_message(
                    live.target.window, LVM_SETVIEW,
                    static_cast<WPARAM>(mode));
                if (!result.ok)
                    return mutation_failure(result);
                if (result.value == 0) {
                    return mutation_failure(
                        HRESULT_FROM_WIN32(ERROR_INVALID_DATA),
                        "The list view rejected the requested view mode");
                }
                break;
            }
            case Operation::listviewItemSelected:
            case Operation::listviewItemFocused: {
                bool state = false;
                if (!native_property_detail::parse_boolean(
                        *requested, state)) {
                    return invalidBoolean();
                }
                expected = state ? "true" : "false";
                write = write_listview_item_state(
                    live,
                    mutation.operation == Operation::listviewItemSelected
                        ? LVIS_SELECTED
                        : LVIS_FOCUSED,
                    state);
                break;
            }
            case Operation::listviewItemText:
                if (target.snapshotIdentity.rfind("text:", 0) == 0) {
                    auto count = send_native_message(
                        live.target.window, LVM_GETITEMCOUNT);
                    if (!count.ok)
                        return mutation_failure(count);
                    for (int index = 0; index < count.value; ++index) {
                        if (index == live.target.index)
                            continue;
                        ListViewItemData candidate;
                        NativeMessageResult native;
                        if (!read_listview_item(
                                live, index, candidate, native)) {
                            return mutation_failure(native);
                        }
                        if (candidate.text == *requested) {
                            return mutation_failure(
                                E_INVALIDARG,
                                "The new list-view text would make this "
                                "index identity ambiguous");
                        }
                    }
                }
                write = write_listview_item_text(live, *requested);
                break;
            case Operation::treeviewItemSelected: {
                bool selected = false;
                if (!native_property_detail::parse_boolean(
                        *requested, selected)) {
                    return invalidBoolean();
                }
                expected = selected ? "true" : "false";
                auto current = send_native_message(
                    live.target.window, TVM_GETNEXTITEM,
                    TVGN_CARET, 0);
                if (!current.ok)
                    return mutation_failure(current);
                const auto item = reinterpret_cast<HTREEITEM>(
                    live.target.itemHandle);
                if (!selected &&
                    reinterpret_cast<HTREEITEM>(current.value) != item) {
                    break;
                }
                auto result = send_native_message(
                    live.target.window, TVM_SELECTITEM, TVGN_CARET,
                    selected
                        ? reinterpret_cast<LPARAM>(item)
                        : 0);
                if (!result.ok)
                    return mutation_failure(result);
                if (!result.value) {
                    return mutation_failure(
                        HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE),
                        "The tree view rejected the selection change");
                }
                break;
            }
            case Operation::treeviewItemExpanded: {
                bool expanded = false;
                if (!native_property_detail::parse_boolean(
                        *requested, expanded)) {
                    return invalidBoolean();
                }
                expected = expanded ? "true" : "false";
                auto result = send_native_message(
                    live.target.window, TVM_EXPAND,
                    expanded ? TVE_EXPAND : TVE_COLLAPSE,
                    static_cast<LPARAM>(live.target.itemHandle));
                if (!result.ok)
                    return mutation_failure(result);
                if (!result.value) {
                    return mutation_failure(
                        HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE),
                        "The tree view rejected the expansion change");
                }
                break;
            }
            case Operation::treeviewItemText:
                write = write_treeview_item_text(live, *requested);
                break;
            case Operation::toolbarButtonChecked:
            case Operation::toolbarButtonEnabled: {
                bool state = false;
                if (!native_property_detail::parse_boolean(
                        *requested, state)) {
                    return invalidBoolean();
                }
                expected = state ? "true" : "false";
                auto result = send_native_message(
                    live.target.window,
                    mutation.operation == Operation::toolbarButtonChecked
                        ? TB_CHECKBUTTON
                        : TB_ENABLEBUTTON,
                    static_cast<WPARAM>(live.target.commandId),
                    MAKELPARAM(state ? TRUE : FALSE, 0));
                if (!result.ok)
                    return mutation_failure(result);
                if (!result.value) {
                    return mutation_failure(
                        HRESULT_FROM_WIN32(ERROR_INVALID_INDEX),
                        "The toolbar command rejected the state change");
                }
                break;
            }
            case Operation::toolbarButtonText:
                write = write_toolbar_text(live, *requested);
                break;
            case Operation::statusbarPartText:
                write = write_status_text(live, *requested);
                break;
            case Operation::tabSelectedIndex: {
                int index = 0;
                if (!native_property_detail::parse_integer(
                        *requested, index)) {
                    return invalidInteger();
                }
                auto count = send_native_message(
                    live.target.window, TCM_GETITEMCOUNT);
                if (!count.ok)
                    return mutation_failure(count);
                if (index < -1 || index >= count.value) {
                    return mutation_failure(
                        E_INVALIDARG,
                        "The selected tab index is outside the current range");
                }
                auto result = send_native_message(
                    live.target.window, TCM_SETCURSEL,
                    static_cast<WPARAM>(static_cast<INT_PTR>(index)));
                if (!result.ok)
                    return mutation_failure(result);
                expected = std::to_string(index);
                break;
            }
            case Operation::tabItemText:
                write = write_tab_text(live, *requested);
                break;
            }
        }
        if (!write.ok)
            return write;

        LiveTarget after;
        HRESULT hresult = S_OK;
        std::string error;
        if (!capture_live(target, after, hresult, error))
            return mutation_failure(hresult, std::move(error));
        auto readback = read_operation(mutation.operation, after);
        if (!readback.ok)
            return mutation_failure(readback.hresult, readback.error);
        if (!readback.available || readback.value != expected) {
            return mutation_failure(
                HRESULT_FROM_WIN32(ERROR_INVALID_DATA),
                "The native control did not retain the requested value");
        }

        {
            std::lock_guard<std::mutex> lock(mutex);
            auto found = targets.find(handle);
            if (found != targets.end() &&
                found->second.schemaId == target.schemaId) {
                found->second.snapshotIdentity = after.identity;
            }
        }

        PropertyMutationResult result;
        result.ok = true;
        result.hresult = S_OK;
        result.hasValue = requested.has_value();
        result.value = readback.value;
        result.cleared = !requested.has_value();
        return result;
    }
};

std::shared_ptr<NativePropertyConnection>
NativePropertyConnection::connect(
    HWND root, DWORD pid, std::string provider,
    std::string controlVersion) {
    if (provider != "win32" && provider != "comctl")
        return nullptr;

    NativeWindowIdentity identity;
    auto captured =
        capture_native_window_identity(root, pid, identity);
    if (!captured.ok)
        return nullptr;

    auto impl = std::make_unique<Impl>();
    impl->root = std::move(identity);
    impl->targetArchitecture = detect_process_architecture(pid);
    impl->provider = std::move(provider);
    impl->controlVersion = std::move(controlVersion);
    return std::shared_ptr<NativePropertyConnection>(
        new NativePropertyConnection(std::move(impl)));
}

NativePropertyConnection::NativePropertyConnection(
    std::unique_ptr<Impl> impl)
    : m_impl(std::move(impl)) {}

NativePropertyConnection::~NativePropertyConnection() = default;

const std::string& NativePropertyConnection::provider() const {
    return m_impl->provider;
}

bool NativePropertyConnection::pointer_operations_allowed() const {
    return native_pointer_operations_allowed(
        get_host_architecture(), m_impl->targetArchitecture);
}

uint64_t NativePropertyConnection::register_hwnd(HWND hwnd) {
    return m_impl->register_target(
        TargetKind::hwnd, hwnd, -1, 0, 0);
}

uint64_t NativePropertyConnection::register_listview_item(
    HWND hwnd, int index) {
    return m_impl->register_target(
        TargetKind::listviewItem, hwnd, index, 0, 0);
}

uint64_t NativePropertyConnection::register_treeview_item(
    HWND hwnd, HTREEITEM item) {
    return m_impl->register_target(
        TargetKind::treeviewItem, hwnd, -1,
        reinterpret_cast<uintptr_t>(item), 0);
}

uint64_t NativePropertyConnection::register_toolbar_button(
    HWND hwnd, int index, int commandId) {
    return m_impl->register_target(
        TargetKind::toolbarButton, hwnd, index, 0, commandId);
}

uint64_t NativePropertyConnection::register_statusbar_part(
    HWND hwnd, int index) {
    return m_impl->register_target(
        TargetKind::statusbarPart, hwnd, index, 0, 0);
}

uint64_t NativePropertyConnection::register_tab_item(
    HWND hwnd, int index) {
    return m_impl->register_target(
        TargetKind::tabItem, hwnd, index, 0, 0);
}

size_t NativePropertyConnection::cached_schema_count_for_testing() const {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->schemasByKey.size();
}

bool NativePropertyConnection::is_alive() const {
    return validate_native_window(m_impl->root).ok;
}

PropertySnapshotResult NativePropertyConnection::get_property_snapshot(
    uint64_t handle) {
    PropertySnapshotResult result;
    Target target;
    if (!m_impl->get_target(handle, target)) {
        result.hresult = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        result.error = "The native property target is unknown or closed";
        return result;
    }

    LiveTarget live;
    if (!m_impl->capture_live(
            target, live, result.hresult, result.error)) {
        return result;
    }

    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        result.schema = m_impl->schema_for(live);
    }
    if (!result.schema) {
        result.hresult = E_FAIL;
        result.error = "The native provider could not create a property schema";
        return result;
    }

    result.values.reserve(result.schema->descriptors.size());
    for (const auto& descriptor : result.schema->descriptors) {
        Mutation mutation;
        {
            std::lock_guard<std::mutex> lock(m_impl->mutex);
            const auto found =
                m_impl->mutationsByDescriptor.find(descriptor.descriptorId);
            if (found == m_impl->mutationsByDescriptor.end()) {
                result.hresult = E_FAIL;
                result.error =
                    "The native provider lost a property descriptor mapping";
                return result;
            }
            mutation = found->second;
        }

        auto current = m_impl->read_operation(mutation.operation, live);
        if (!current.ok) {
            result.hresult = current.hresult;
            result.error = current.error;
            return result;
        }

        PropertyValue value;
        value.descriptorId = descriptor.descriptorId;
        value.value = current.value;
        value.runtimeType = descriptor.propertyType;
        value.source = "native";
        value.canClear =
            descriptor.supportsClear && current.value != "-1";
        value.overridden = value.canClear;
        if (!current.available)
            value.unavailableReason = current.error;
        if (!descriptor.writable)
            value.readOnlyReason =
                m_impl->read_only_reason(mutation.operation, live);
        result.values.push_back(std::move(value));
    }

    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        auto found = m_impl->targets.find(handle);
        if (found != m_impl->targets.end()) {
            found->second.schemaId = result.schema->schemaId;
            found->second.snapshotIdentity = live.identity;
            found->second.hasSnapshot = true;
        }
    }

    result.ok = true;
    result.hresult = S_OK;
    result.error.clear();
    return result;
}

PropertyMutationResult NativePropertyConnection::set_property(
    uint64_t handle, const std::string& descriptorId,
    const std::string& value) {
    return m_impl->apply(handle, descriptorId, value);
}

PropertyMutationResult NativePropertyConnection::clear_property(
    uint64_t handle, const std::string& descriptorId) {
    return m_impl->apply(handle, descriptorId, std::nullopt);
}

} // namespace lvt
