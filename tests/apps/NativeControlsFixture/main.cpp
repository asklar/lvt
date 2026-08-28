#include "native_controls_fixture_ids.h"

#include <CommCtrl.h>
#include <sstream>
#include <string>

namespace fixture = lvt::native_fixture;

namespace {

struct FixtureControls {
    HWND checkbox = nullptr;
    HWND radio = nullptr;
    HWND button = nullptr;
    HWND edit = nullptr;
    HWND readOnlyEdit = nullptr;
    HWND comboBox = nullptr;
    HWND listBox = nullptr;
    HWND scrollBar = nullptr;
    HWND listView = nullptr;
    HWND treeView = nullptr;
    HWND toolbar = nullptr;
    HWND statusBar = nullptr;
    HWND tabControl = nullptr;
    HWND genericText = nullptr;
    HWND stateSummary = nullptr;
    HWND outOfTree = nullptr;
    HTREEITEM treeRoot = nullptr;
    HTREEITEM treeChild = nullptr;
    HTREEITEM treeGrandchild = nullptr;
} g_controls;

bool g_delayNextToolbarPointerMessage = false;
LONG g_delayedPointerState = 0;

bool committed_pointer(const void* pointer) {
    if (!pointer)
        return false;
    MEMORY_BASIC_INFORMATION info{};
    return VirtualQuery(pointer, &info, sizeof(info)) == sizeof(info) &&
           info.State == MEM_COMMIT;
}

LRESULT CALLBACK toolbar_subclass_proc(
    HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
    UINT_PTR, DWORD_PTR) {
    if (message == TB_GETBUTTONINFOW &&
        g_delayNextToolbarPointerMessage) {
        g_delayNextToolbarPointerMessage = false;
        InterlockedExchange(&g_delayedPointerState, 1);
        Sleep(1500);

        auto* info = reinterpret_cast<TBBUTTONINFOW*>(lParam);
        if (!committed_pointer(info) ||
            !committed_pointer(info->pszText)) {
            InterlockedExchange(&g_delayedPointerState, 3);
            return -1;
        }
        const auto result =
            DefSubclassProc(hwnd, message, wParam, lParam);
        InterlockedExchange(&g_delayedPointerState, 4);
        return result;
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

std::wstring window_text(HWND hwnd) {
    if (!hwnd)
        return {};
    const int length = GetWindowTextLengthW(hwnd);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(hwnd, text.data(), length + 1);
    text.resize(static_cast<size_t>(copied));
    return text;
}

std::wstring combo_item_text(HWND hwnd, int index) {
    if (index < 0)
        return L"<none>";
    const LRESULT length = SendMessageW(hwnd, CB_GETLBTEXTLEN, index, 0);
    if (length == CB_ERR)
        return L"<error>";
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    SendMessageW(hwnd, CB_GETLBTEXT, index, reinterpret_cast<LPARAM>(text.data()));
    text.resize(static_cast<size_t>(length));
    return text;
}

std::wstring listbox_item_text(HWND hwnd, int index) {
    if (index < 0)
        return L"<none>";
    const LRESULT length = SendMessageW(hwnd, LB_GETTEXTLEN, index, 0);
    if (length == LB_ERR)
        return L"<error>";
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    SendMessageW(hwnd, LB_GETTEXT, index, reinterpret_cast<LPARAM>(text.data()));
    text.resize(static_cast<size_t>(length));
    return text;
}

std::wstring listview_item_text(HWND hwnd, int index) {
    if (index < 0)
        return L"<none>";
    wchar_t buffer[128]{};
    ListView_GetItemText(hwnd, index, 0, buffer, static_cast<int>(_countof(buffer)));
    return buffer;
}

std::wstring tree_item_text(HWND hwnd, HTREEITEM item) {
    wchar_t buffer[128]{};
    TVITEMW value{};
    value.mask = TVIF_TEXT;
    value.hItem = item;
    value.pszText = buffer;
    value.cchTextMax = static_cast<int>(_countof(buffer));
    TreeView_GetItem(hwnd, &value);
    return buffer;
}

std::wstring toolbar_state(HWND hwnd, int commandId) {
    const LRESULT state = SendMessageW(hwnd, TB_GETSTATE, commandId, 0);
    std::wstring result = (state & TBSTATE_ENABLED) ? L"enabled" : L"disabled";
    if (state & TBSTATE_CHECKED)
        result += L"+checked";
    return result;
}

std::wstring toolbar_text(HWND hwnd, int commandId) {
    const LRESULT length = SendMessageW(
        hwnd, TB_GETBUTTONTEXTW, commandId, 0);
    if (length < 0)
        return L"<error>";
    std::wstring buffer(static_cast<size_t>(length) + 1, L'\0');
    const LRESULT copied = SendMessageW(
        hwnd, TB_GETBUTTONTEXTW, commandId,
        reinterpret_cast<LPARAM>(buffer.data()));
    if (copied < 0)
        return L"<error>";
    buffer.resize(static_cast<size_t>(copied));
    return buffer;
}

std::wstring statusbar_text(HWND hwnd, int part) {
    const LRESULT info = SendMessageW(hwnd, SB_GETTEXTLENGTHW, part, 0);
    const int length = LOWORD(info);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    SendMessageW(hwnd, SB_GETTEXTW, part, reinterpret_cast<LPARAM>(text.data()));
    text.resize(static_cast<size_t>(length));
    return text;
}

std::wstring tab_text(HWND hwnd, int index) {
    if (index < 0)
        return L"<none>";
    wchar_t buffer[128]{};
    TCITEMW item{};
    item.mask = TCIF_TEXT;
    item.pszText = buffer;
    item.cchTextMax = static_cast<int>(_countof(buffer));
    TabCtrl_GetItem(hwnd, index, &item);
    return buffer;
}

void refresh_state_summary() {
    if (!g_controls.stateSummary)
        return;

    const int comboSelection =
        static_cast<int>(SendMessageW(g_controls.comboBox, CB_GETCURSEL, 0, 0));
    const int listSelection =
        static_cast<int>(SendMessageW(g_controls.listBox, LB_GETCURSEL, 0, 0));
    const int listViewSelection =
        ListView_GetNextItem(g_controls.listView, -1, LVNI_SELECTED);
    const int listViewFocus =
        ListView_GetNextItem(g_controls.listView, -1, LVNI_FOCUSED);
    const int tabSelection = TabCtrl_GetCurSel(g_controls.tabControl);
    const HTREEITEM treeSelection =
        TreeView_GetSelection(g_controls.treeView);

    DWORD editSelectionStart = 0;
    DWORD editSelectionEnd = 0;
    SendMessageW(
        g_controls.edit, EM_GETSEL,
        reinterpret_cast<WPARAM>(&editSelectionStart),
        reinterpret_cast<LPARAM>(&editSelectionEnd));
    DWORD readOnlySelectionStart = 0;
    DWORD readOnlySelectionEnd = 0;
    SendMessageW(
        g_controls.readOnlyEdit, EM_GETSEL,
        reinterpret_cast<WPARAM>(&readOnlySelectionStart),
        reinterpret_cast<LPARAM>(&readOnlySelectionEnd));

    SCROLLINFO scrollInfo{sizeof(scrollInfo), SIF_RANGE | SIF_PAGE | SIF_POS};
    GetScrollInfo(g_controls.scrollBar, SB_CTL, &scrollInfo);

    const bool rootExpanded =
        (TreeView_GetItemState(g_controls.treeView, g_controls.treeRoot, TVIS_EXPANDED) &
         TVIS_EXPANDED) != 0;
    const bool childExpanded =
        (TreeView_GetItemState(g_controls.treeView, g_controls.treeChild, TVIS_EXPANDED) &
         TVIS_EXPANDED) != 0;

    std::wostringstream summary;
    summary
        << L"protocol=" << fixture::kSummaryProtocolVersion << L"\n"
        << L"checkbox(id=1001)="
        << SendMessageW(g_controls.checkbox, BM_GETCHECK, 0, 0) << L"\n"
        << L"radio(id=1002)="
        << SendMessageW(g_controls.radio, BM_GETCHECK, 0, 0) << L"\n"
        << L"button(id=1003)=" << window_text(g_controls.button) << L"\n"
        << L"edit(id=1004)=" << window_text(g_controls.edit)
        << L";selection=" << editSelectionStart << L"," << editSelectionEnd
        << L";readonly="
        << ((GetWindowLongPtrW(g_controls.edit, GWL_STYLE) & ES_READONLY) ? 1 : 0)
        << L"\n"
        << L"readonly(id=1005)=" << window_text(g_controls.readOnlyEdit)
        << L";selection=" << readOnlySelectionStart << L","
        << readOnlySelectionEnd << L";readonly="
        << ((GetWindowLongPtrW(g_controls.readOnlyEdit, GWL_STYLE) & ES_READONLY)
                ? 1
                : 0)
        << L"\n"
        << L"combo(id=1006)=" << comboSelection << L":"
        << combo_item_text(g_controls.comboBox, comboSelection)
        << L";items=" << combo_item_text(g_controls.comboBox, 0) << L"|"
        << combo_item_text(g_controls.comboBox, 1) << L"|"
        << combo_item_text(g_controls.comboBox, 2) << L"\n"
        << L"listbox(id=1007)=" << listSelection << L":"
        << listbox_item_text(g_controls.listBox, listSelection)
        << L";items=" << listbox_item_text(g_controls.listBox, 0) << L"|"
        << listbox_item_text(g_controls.listBox, 1) << L"|"
        << listbox_item_text(g_controls.listBox, 2) << L"|"
        << listbox_item_text(g_controls.listBox, 3) << L"\n"
        << L"scrollbar(id=1008)=" << scrollInfo.nMin << L"," << scrollInfo.nMax
        << L"," << scrollInfo.nPage << L"," << scrollInfo.nPos << L"\n"
        << L"listview(id=1009)=" << listViewSelection << L":"
        << listview_item_text(g_controls.listView, listViewSelection)
        << L";focus=" << listViewFocus
        << L";items=" << listview_item_text(g_controls.listView, 0) << L"|"
        << listview_item_text(g_controls.listView, 1) << L"|"
        << listview_item_text(g_controls.listView, 2) << L"\n"
        << L"tree(id=1010)=selected:"
        << tree_item_text(g_controls.treeView, treeSelection) << L";"
        << tree_item_text(g_controls.treeView, g_controls.treeRoot)
        << L"[" << (rootExpanded ? L"expanded" : L"collapsed") << L"]/"
        << tree_item_text(g_controls.treeView, g_controls.treeChild)
        << L"[" << (childExpanded ? L"expanded" : L"collapsed") << L"]/"
        << tree_item_text(g_controls.treeView, g_controls.treeGrandchild) << L"\n"
        << L"toolbar(id=1011)=2001:"
        << toolbar_text(g_controls.toolbar, fixture::kToolbarApplyCommand)
        << L":" << toolbar_state(g_controls.toolbar, fixture::kToolbarApplyCommand)
        << L",2002:"
        << toolbar_text(g_controls.toolbar, fixture::kToolbarPinCommand)
        << L":" << toolbar_state(g_controls.toolbar, fixture::kToolbarPinCommand)
        << L",2003:"
        << toolbar_text(g_controls.toolbar, fixture::kToolbarDisabledCommand)
        << L":" << toolbar_state(
               g_controls.toolbar, fixture::kToolbarDisabledCommand)
        << L"\n"
        << L"status(id=1012)=" << statusbar_text(g_controls.statusBar, 0) << L"|"
        << statusbar_text(g_controls.statusBar, 1) << L"|"
        << statusbar_text(g_controls.statusBar, 2) << L"\n"
        << L"tab(id=1013)=" << tabSelection << L":"
        << tab_text(g_controls.tabControl, tabSelection)
        << L";items=" << tab_text(g_controls.tabControl, 0) << L"|"
        << tab_text(g_controls.tabControl, 1) << L"|"
        << tab_text(g_controls.tabControl, 2) << L"\n"
        << L"generic(id=1014)=" << window_text(g_controls.genericText)
        << L";enabled=" << (IsWindowEnabled(g_controls.genericText) ? 1 : 0);

    SetWindowTextW(g_controls.stateSummary, summary.str().c_str());
}

HWND create_child(
    HWND parent,
    DWORD exStyle,
    const wchar_t* className,
    const wchar_t* text,
    DWORD style,
    int x,
    int y,
    int width,
    int height,
    int controlId) {
    return CreateWindowExW(
        exStyle,
        className,
        text,
        WS_CHILD | WS_VISIBLE | style,
        x,
        y,
        width,
        height,
        parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(controlId)),
        GetModuleHandleW(nullptr),
        nullptr);
}

void set_default_font(HWND parent) {
    const HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    EnumChildWindows(
        parent,
        [](HWND hwnd, LPARAM fontValue) -> BOOL {
            SendMessageW(hwnd, WM_SETFONT, static_cast<WPARAM>(fontValue), TRUE);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(font));
}

void populate_listview() {
    ListView_SetExtendedListViewStyle(
        g_controls.listView, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    column.cx = 160;
    column.pszText = const_cast<LPWSTR>(L"Name");
    ListView_InsertColumn(g_controls.listView, 0, &column);

    column.cx = 120;
    column.iSubItem = 1;
    column.pszText = const_cast<LPWSTR>(L"Value");
    ListView_InsertColumn(g_controls.listView, 1, &column);

    const wchar_t* names[] = {L"Alpha row", L"Beta row", L"Gamma row"};
    const wchar_t* values[] = {L"One", L"Two", L"Three"};
    for (int index = 0; index < 3; ++index) {
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = index;
        item.pszText = const_cast<LPWSTR>(names[index]);
        ListView_InsertItem(g_controls.listView, &item);
        ListView_SetItemText(
            g_controls.listView, index, 1, const_cast<LPWSTR>(values[index]));
    }
    ListView_SetItemState(
        g_controls.listView, 1, LVIS_SELECTED, LVIS_SELECTED);
}

void populate_treeview() {
    TVINSERTSTRUCTW insert{};
    insert.hInsertAfter = TVI_LAST;
    insert.item.mask = TVIF_TEXT;

    insert.hParent = TVI_ROOT;
    insert.item.pszText = const_cast<LPWSTR>(L"Fixture Root");
    g_controls.treeRoot = TreeView_InsertItem(g_controls.treeView, &insert);

    insert.hParent = g_controls.treeRoot;
    insert.item.pszText = const_cast<LPWSTR>(L"Fixture Child");
    g_controls.treeChild = TreeView_InsertItem(g_controls.treeView, &insert);

    insert.hParent = g_controls.treeChild;
    insert.item.pszText = const_cast<LPWSTR>(L"Fixture Grandchild");
    g_controls.treeGrandchild = TreeView_InsertItem(g_controls.treeView, &insert);

    TreeView_Expand(g_controls.treeView, g_controls.treeRoot, TVE_EXPAND);
    TreeView_Expand(g_controls.treeView, g_controls.treeChild, TVE_EXPAND);
    TreeView_SelectItem(g_controls.treeView, g_controls.treeChild);
}

void populate_toolbar() {
    SendMessageW(g_controls.toolbar, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
    SendMessageW(
        g_controls.toolbar, TB_SETEXTENDEDSTYLE, 0, TBSTYLE_EX_MIXEDBUTTONS);

    const int firstString = static_cast<int>(SendMessageW(
        g_controls.toolbar,
        TB_ADDSTRINGW,
        0,
        reinterpret_cast<LPARAM>(L"Apply\0Pinned\0Disabled\0\0")));

    TBBUTTON buttons[] = {
        {I_IMAGENONE, fixture::kToolbarApplyCommand, TBSTATE_ENABLED,
         BTNS_BUTTON | BTNS_SHOWTEXT, {0}, 0, firstString},
        {I_IMAGENONE, fixture::kToolbarPinCommand,
         static_cast<BYTE>(TBSTATE_ENABLED | TBSTATE_CHECKED),
         BTNS_CHECK | BTNS_SHOWTEXT, {0}, 0, firstString + 1},
        {I_IMAGENONE, fixture::kToolbarDisabledCommand, 0,
         BTNS_BUTTON | BTNS_SHOWTEXT, {0}, 0, firstString + 2},
    };
    SendMessageW(
        g_controls.toolbar,
        TB_ADDBUTTONS,
        static_cast<WPARAM>(_countof(buttons)),
        reinterpret_cast<LPARAM>(buttons));
    SendMessageW(g_controls.toolbar, TB_AUTOSIZE, 0, 0);
}

void populate_statusbar() {
    int parts[] = {220, 440, -1};
    SendMessageW(
        g_controls.statusBar,
        SB_SETPARTS,
        static_cast<WPARAM>(_countof(parts)),
        reinterpret_cast<LPARAM>(parts));
    SendMessageW(
        g_controls.statusBar, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(L"Ready"));
    SendMessageW(
        g_controls.statusBar, SB_SETTEXTW, 1, reinterpret_cast<LPARAM>(L"3 items"));
    SendMessageW(
        g_controls.statusBar, SB_SETTEXTW, 2, reinterpret_cast<LPARAM>(L"Idle"));
}

void populate_tabs(bool duplicateLabels = false, bool includeIdentity = true) {
    while (TabCtrl_GetItemCount(g_controls.tabControl) > 0)
        TabCtrl_DeleteItem(g_controls.tabControl, 0);

    const wchar_t* normalLabels[] = {L"Overview", L"Details", L"Advanced"};
    const wchar_t* duplicate[] = {L"Overview", L"Overview", L"Advanced"};
    const auto* labels = duplicateLabels ? duplicate : normalLabels;
    for (int index = 0; index < 3; ++index) {
        TCITEMW item{};
        item.mask = TCIF_TEXT | TCIF_PARAM;
        item.pszText = const_cast<LPWSTR>(labels[index]);
        item.lParam = includeIdentity ? 3001 + index : 0;
        TabCtrl_InsertItem(g_controls.tabControl, index, &item);
    }
    TabCtrl_SetCurSel(g_controls.tabControl, 1);
}

bool create_controls(HWND parent) {
    g_controls.checkbox = create_child(
        parent, 0, WC_BUTTONW, L"Tri-state check",
        WS_TABSTOP | BS_AUTO3STATE, 20, 20, 180, 24, fixture::kCheckboxId);
    g_controls.radio = create_child(
        parent, 0, WC_BUTTONW, L"Primary radio",
        WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON,
        20, 52, 180, 24, fixture::kRadioId);
    g_controls.button = create_child(
        parent, 0, WC_BUTTONW, L"Fixture action",
        WS_TABSTOP | BS_PUSHBUTTON, 20, 84, 150, 30, fixture::kButtonId);
    g_controls.edit = create_child(
        parent, WS_EX_CLIENTEDGE, WC_EDITW, L"Editable seed",
        WS_TABSTOP | ES_AUTOHSCROLL, 20, 126, 240, 26, fixture::kEditId);
    g_controls.readOnlyEdit = create_child(
        parent, WS_EX_CLIENTEDGE, WC_EDITW, L"Read-only seed",
        WS_TABSTOP | ES_AUTOHSCROLL | ES_READONLY,
        20, 162, 240, 26, fixture::kReadOnlyEditId);
    g_controls.comboBox = create_child(
        parent, 0, WC_COMBOBOXW, L"",
        WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
        20, 200, 240, 140, fixture::kComboBoxId);
    g_controls.listBox = create_child(
        parent, WS_EX_CLIENTEDGE, WC_LISTBOXW, L"",
        WS_TABSTOP | LBS_NOTIFY | WS_VSCROLL,
        20, 242, 240, 92, fixture::kListBoxId);
    g_controls.scrollBar = create_child(
        parent, 0, WC_SCROLLBARW, L"",
        SBS_HORZ | WS_TABSTOP, 20, 346, 240, 24, fixture::kScrollBarId);

    g_controls.listView = create_child(
        parent, WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        300, 20, 320, 158, fixture::kListViewId);
    g_controls.treeView = create_child(
        parent, WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"",
        WS_TABSTOP | TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT |
            TVS_SHOWSELALWAYS,
        640, 20, 310, 158, fixture::kTreeViewId);
    g_controls.toolbar = create_child(
        parent, 0, TOOLBARCLASSNAMEW, L"",
        WS_TABSTOP | TBSTYLE_FLAT | TBSTYLE_TOOLTIPS | CCS_NORESIZE,
        300, 190, 650, 34, fixture::kToolbarId);
    g_controls.tabControl = create_child(
        parent, 0, WC_TABCONTROLW, L"",
        WS_TABSTOP | TCS_TABS, 300, 238, 320, 138, fixture::kTabControlId);
    g_controls.genericText = create_child(
        parent, WS_EX_CLIENTEDGE, fixture::kGenericChildClass,
        L"Generic child seed", 0, 640, 238, 310, 30,
        fixture::kGenericTextId);
    g_controls.stateSummary = create_child(
        parent, WS_EX_CLIENTEDGE, WC_STATICW, L"",
        SS_LEFT | SS_EDITCONTROL, 300, 282, 650, 300,
        fixture::kStateSummaryId);
    g_controls.statusBar = create_child(
        parent, 0, STATUSCLASSNAMEW, L"",
        SBARS_SIZEGRIP, 0, 0, 0, 0, fixture::kStatusBarId);

    if (!g_controls.checkbox || !g_controls.radio || !g_controls.button ||
        !g_controls.edit || !g_controls.readOnlyEdit || !g_controls.comboBox ||
        !g_controls.listBox || !g_controls.scrollBar || !g_controls.listView ||
        !g_controls.treeView || !g_controls.toolbar || !g_controls.statusBar ||
        !g_controls.tabControl || !g_controls.genericText ||
        !g_controls.stateSummary) {
        return false;
    }

    SendMessageW(g_controls.checkbox, BM_SETCHECK, BST_INDETERMINATE, 0);
    SendMessageW(g_controls.radio, BM_SETCHECK, BST_CHECKED, 0);

    for (const wchar_t* item : {L"Red", L"Green", L"Blue"})
        SendMessageW(
            g_controls.comboBox, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
    SendMessageW(g_controls.comboBox, CB_SETCURSEL, 1, 0);

    for (const wchar_t* item : {L"Alpha", L"Beta", L"Gamma", L"Delta"})
        SendMessageW(
            g_controls.listBox, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
    SendMessageW(g_controls.listBox, LB_SETCURSEL, 2, 0);

    SCROLLINFO scrollInfo{
        sizeof(scrollInfo), SIF_RANGE | SIF_PAGE | SIF_POS, 10, 110, 10, 42, 0};
    SetScrollInfo(g_controls.scrollBar, SB_CTL, &scrollInfo, TRUE);

    populate_listview();
    populate_treeview();
    populate_toolbar();
    populate_statusbar();
    populate_tabs();
    SetWindowSubclass(
        g_controls.toolbar, toolbar_subclass_proc, 1, 0);

    g_controls.outOfTree = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        fixture::kGenericChildClass,
        fixture::kOutOfTreeTitle,
        WS_OVERLAPPED | WS_CAPTION,
        1100, 80, 280, 100,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g_controls.outOfTree)
        return false;
    ShowWindow(g_controls.outOfTree, SW_SHOWNOACTIVATE);

    set_default_font(parent);
    refresh_state_summary();
    return true;
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case fixture::kRefreshSummaryMessage:
        refresh_state_summary();
        return fixture::kSummaryProtocolVersion;
    case fixture::kMutateListViewIdentityMessage:
        ListView_SetItemText(
            g_controls.listView, 0, 0,
            const_cast<LPWSTR>(L"External replacement"));
        refresh_state_summary();
        return TRUE;
    case fixture::kRestoreListViewIdentityMessage:
        ListView_SetItemText(
            g_controls.listView, 0, 0,
            const_cast<LPWSTR>(L"Alpha row"));
        refresh_state_summary();
        return TRUE;
    case fixture::kMutateToolbarIdentityMessage: {
        TBBUTTONINFOW info{sizeof(info)};
        info.dwMask = TBIF_COMMAND;
        info.idCommand = 2999;
        return SendMessageW(
            g_controls.toolbar, TB_SETBUTTONINFOW,
            fixture::kToolbarApplyCommand,
            reinterpret_cast<LPARAM>(&info));
    }
    case fixture::kRestoreToolbarIdentityMessage: {
        TBBUTTONINFOW info{sizeof(info)};
        info.dwMask = TBIF_COMMAND;
        info.idCommand = fixture::kToolbarApplyCommand;
        return SendMessageW(
            g_controls.toolbar, TB_SETBUTTONINFOW, 2999,
            reinterpret_cast<LPARAM>(&info));
    }
    case fixture::kSetLongToolbarTextMessage: {
        std::wstring text(
            fixture::kLongToolbarTextLength, L'X');
        TBBUTTONINFOW info{sizeof(info)};
        info.dwMask = TBIF_TEXT;
        info.pszText = text.data();
        return SendMessageW(
            g_controls.toolbar, TB_SETBUTTONINFOW,
            fixture::kToolbarApplyCommand,
            reinterpret_cast<LPARAM>(&info));
    }
    case fixture::kRestoreToolbarTextMessage: {
        TBBUTTONINFOW info{sizeof(info)};
        info.dwMask = TBIF_TEXT;
        info.pszText = const_cast<LPWSTR>(L"Apply");
        return SendMessageW(
            g_controls.toolbar, TB_SETBUTTONINFOW,
            fixture::kToolbarApplyCommand,
            reinterpret_cast<LPARAM>(&info));
    }
    case fixture::kArmDelayedPointerMessage:
        InterlockedExchange(&g_delayedPointerState, 0);
        g_delayNextToolbarPointerMessage = true;
        return TRUE;
    case fixture::kGetDelayedPointerStateMessage:
        return InterlockedCompareExchange(
            &g_delayedPointerState, 0, 0);
    case fixture::kGetOutOfTreeHwndMessage:
        return reinterpret_cast<LRESULT>(g_controls.outOfTree);
    case fixture::kDeleteFirstTabMessage:
        return TabCtrl_DeleteItem(g_controls.tabControl, 0);
    case fixture::kMakeDuplicateTabsMessage:
        populate_tabs(true, false);
        refresh_state_summary();
        return TRUE;
    case fixture::kRestoreTabsMessage:
        populate_tabs();
        refresh_state_summary();
        return TRUE;
    case fixture::kHangMessage:
        Sleep(2500);
        return TRUE;
    case fixture::kCloseMessage:
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_COMMAND:
        if (lParam != 0)
            refresh_state_summary();
        break;
    case WM_HSCROLL:
    case WM_NOTIFY:
        refresh_state_summary();
        break;
    case WM_SIZE:
        if (g_controls.statusBar)
            SendMessageW(g_controls.statusBar, WM_SIZE, wParam, lParam);
        break;
    case WM_DESTROY:
        if (g_controls.outOfTree && IsWindow(g_controls.outOfTree)) {
            DestroyWindow(g_controls.outOfTree);
            g_controls.outOfTree = nullptr;
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

bool register_fixture_classes(HINSTANCE instance) {
    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.lpfnWndProc = window_proc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.lpszClassName = fixture::kWindowClass;
    if (!RegisterClassExW(&windowClass))
        return false;

    WNDCLASSEXW childClass{sizeof(childClass)};
    childClass.lpfnWndProc = DefWindowProcW;
    childClass.hInstance = instance;
    childClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    childClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    childClass.lpszClassName = fixture::kGenericChildClass;
    return RegisterClassExW(&childClass) != 0;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    INITCOMMONCONTROLSEX commonControls{
        sizeof(commonControls),
        ICC_WIN95_CLASSES | ICC_LISTVIEW_CLASSES | ICC_TREEVIEW_CLASSES |
            ICC_BAR_CLASSES | ICC_TAB_CLASSES};
    if (!InitCommonControlsEx(&commonControls) ||
        !register_fixture_classes(instance)) {
        return 1;
    }

    HWND window = CreateWindowExW(
        WS_EX_APPWINDOW | WS_EX_NOACTIVATE,
        fixture::kWindowClass,
        fixture::kWindowTitle,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        80,
        80,
        1000,
        680,
        nullptr,
        nullptr,
        instance,
        nullptr);
    if (!window)
        return 2;
    if (!create_controls(window)) {
        DestroyWindow(window);
        return 3;
    }

    ShowWindow(window, SW_SHOWNOACTIVATE);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}
