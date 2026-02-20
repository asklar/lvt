#include "comctl_provider.h"
#include <CommCtrl.h>
#include <wil/resource.h>
#include <vector>

namespace lvt {

// Timeout in ms for cross-process SendMessage calls
static constexpr UINT kSendMsgTimeout = 1000;

// Safe cross-process SendMessage with timeout to avoid hanging on unresponsive windows
static LRESULT SafeSendMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    DWORD_PTR result = 0;
    LRESULT lr = SendMessageTimeoutW(hwnd, msg, wParam, lParam,
        SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, kSendMsgTimeout, &result);
    if (lr == 0) return 0; // timeout or error
    return static_cast<LRESULT>(result);
}

static std::string wstr_to_str(const wchar_t* ws, int len = -1) {
    if (!ws || (len == 0)) return {};
    if (len < 0) len = static_cast<int>(wcslen(ws));
    int sz = WideCharToMultiByte(CP_UTF8, 0, ws, len, nullptr, 0, nullptr, nullptr);
    std::string s(sz, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws, len, s.data(), sz, nullptr, nullptr);
    return s;
}

// RAII wrapper for memory allocated in a remote process via VirtualAllocEx.
struct RemoteBuffer {
    HANDLE process = nullptr;
    void* ptr = nullptr;
    SIZE_T size = 0;

    RemoteBuffer() = default;
    RemoteBuffer(HANDLE proc, SIZE_T sz)
        : process(proc)
        , ptr(VirtualAllocEx(proc, nullptr, sz, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE))
        , size(sz) {}
    ~RemoteBuffer() { if (ptr) VirtualFreeEx(process, ptr, 0, MEM_RELEASE); }
    RemoteBuffer(const RemoteBuffer&) = delete;
    RemoteBuffer& operator=(const RemoteBuffer&) = delete;

    explicit operator bool() const { return ptr != nullptr; }

    bool write(const void* data, SIZE_T len) const {
        return WriteProcessMemory(process, ptr, data, len, nullptr) != FALSE;
    }
    bool read(void* data, SIZE_T len) const {
        return ReadProcessMemory(process, ptr, data, len, nullptr) != FALSE;
    }
};

// Open the process that owns the given HWND.
static wil::unique_handle open_hwnd_process(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return {};
    return wil::unique_handle(OpenProcess(
        PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE, FALSE, pid));
}

void ComCtlProvider::enrich(Element& root) {
    enrich_recursive(root);
}

void ComCtlProvider::enrich_recursive(Element& el) {
    HWND hwnd = reinterpret_cast<HWND>(el.nativeHandle);
    if (!hwnd) return;

    if (el.className == "SysListView32") {
        enrich_listview(el, hwnd);
    } else if (el.className == "SysTreeView32") {
        enrich_treeview(el, hwnd);
    } else if (el.className == "ToolbarWindow32") {
        enrich_toolbar(el, hwnd);
    } else if (el.className == "msctls_statusbar32") {
        enrich_statusbar(el, hwnd);
    } else if (el.className == "SysTabControl32") {
        enrich_tabcontrol(el, hwnd);
    }

    for (auto& child : el.children) {
        enrich_recursive(child);
    }
}

void ComCtlProvider::enrich_listview(Element& el, HWND hwnd) {
    el.type = "ListView";
    el.framework = "comctl";

    // These messages don't use pointers — safe cross-process
    int count = static_cast<int>(SafeSendMessage(hwnd, LVM_GETITEMCOUNT, 0, 0));
    el.properties["itemCount"] = std::to_string(count);

    DWORD viewMode = static_cast<DWORD>(SafeSendMessage(hwnd, LVM_GETVIEW, 0, 0));
    switch (viewMode) {
    case LV_VIEW_ICON:      el.properties["viewMode"] = "icon"; break;
    case LV_VIEW_DETAILS:   el.properties["viewMode"] = "details"; break;
    case LV_VIEW_SMALLICON: el.properties["viewMode"] = "smallicon"; break;
    case LV_VIEW_LIST:      el.properties["viewMode"] = "list"; break;
    case LV_VIEW_TILE:      el.properties["viewMode"] = "tile"; break;
    }

    HWND header = reinterpret_cast<HWND>(SafeSendMessage(hwnd, LVM_GETHEADER, 0, 0));
    if (header) {
        int colCount = static_cast<int>(SafeSendMessage(header, HDM_GETITEMCOUNT, 0, 0));
        el.properties["columnCount"] = std::to_string(colCount);
    }

    // Cross-process: allocate buffers in target process
    auto proc = open_hwnd_process(hwnd);
    if (!proc) return;

    constexpr int kTextBufSize = 512;
    constexpr SIZE_T kRemoteSize = sizeof(LVITEMW) + kTextBufSize * sizeof(wchar_t);
    RemoteBuffer remote(proc.get(), kRemoteSize);
    if (!remote) return;

    auto* remoteItem = reinterpret_cast<LVITEMW*>(remote.ptr);
    auto* remoteText = reinterpret_cast<wchar_t*>(
        static_cast<char*>(remote.ptr) + sizeof(LVITEMW));

    int maxItems = (count < 50) ? count : 50;
    for (int i = 0; i < maxItems; i++) {
        Element item;
        item.type = "ListViewItem";
        item.framework = "comctl";
        item.properties["index"] = std::to_string(i);

        LVITEMW lvi{};
        lvi.mask = LVIF_TEXT | LVIF_STATE;
        lvi.iItem = i;
        lvi.stateMask = LVIS_SELECTED;
        lvi.pszText = remoteText;  // pointer valid in target process
        lvi.cchTextMax = kTextBufSize;

        if (remote.write(&lvi, sizeof(lvi))) {
            SafeSendMessage(hwnd, LVM_GETITEMW, 0, reinterpret_cast<LPARAM>(remoteItem));

            LVITEMW result{};
            remote.read(&result, sizeof(result));
            wchar_t textBuf[kTextBufSize]{};
            remote.read(textBuf, sizeof(textBuf));
            item.text = wstr_to_str(textBuf);

            if (result.state & LVIS_SELECTED)
                item.properties["selected"] = "true";
        }

        el.children.push_back(std::move(item));
    }
    if (count > 50) {
        el.properties["truncated"] = "true";
    }
}

void ComCtlProvider::enrich_treeview(Element& el, HWND hwnd) {
    el.type = "TreeView";
    el.framework = "comctl";

    int count = static_cast<int>(SafeSendMessage(hwnd, TVM_GETCOUNT, 0, 0));
    el.properties["itemCount"] = std::to_string(count);

    // TVM_GETNEXTITEM/TVM_GETROOT don't use pointers — safe
    HTREEITEM hItem = reinterpret_cast<HTREEITEM>(
        SafeSendMessage(hwnd, TVM_GETNEXTITEM, TVGN_ROOT, 0));

    auto proc = open_hwnd_process(hwnd);
    if (!proc || !hItem) return;

    constexpr int kTextBufSize = 512;
    constexpr SIZE_T kRemoteSize = sizeof(TVITEMW) + kTextBufSize * sizeof(wchar_t);
    RemoteBuffer remote(proc.get(), kRemoteSize);
    if (!remote) return;

    auto* remoteItem = reinterpret_cast<TVITEMW*>(remote.ptr);
    auto* remoteText = reinterpret_cast<wchar_t*>(
        static_cast<char*>(remote.ptr) + sizeof(TVITEMW));

    int added = 0;
    while (hItem && added < 100) {
        Element item;
        item.type = "TreeViewItem";
        item.framework = "comctl";

        TVITEMW tvi{};
        tvi.mask = TVIF_TEXT | TVIF_STATE | TVIF_CHILDREN;
        tvi.hItem = hItem;
        tvi.stateMask = TVIS_SELECTED | TVIS_EXPANDED;
        tvi.pszText = remoteText;
        tvi.cchTextMax = kTextBufSize;

        if (remote.write(&tvi, sizeof(tvi))) {
            SafeSendMessage(hwnd, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(remoteItem));

            TVITEMW result{};
            remote.read(&result, sizeof(result));
            wchar_t textBuf[kTextBufSize]{};
            remote.read(textBuf, sizeof(textBuf));
            item.text = wstr_to_str(textBuf);

            if (result.state & TVIS_SELECTED)
                item.properties["selected"] = "true";
            if (result.state & TVIS_EXPANDED)
                item.properties["expanded"] = "true";
            if (result.cChildren > 0)
                item.properties["hasChildren"] = "true";
        }

        el.children.push_back(std::move(item));
        hItem = reinterpret_cast<HTREEITEM>(
            SafeSendMessage(hwnd, TVM_GETNEXTITEM, TVGN_NEXT,
                            reinterpret_cast<LPARAM>(hItem)));
        added++;
    }
}

void ComCtlProvider::enrich_toolbar(Element& el, HWND hwnd) {
    el.type = "Toolbar";
    el.framework = "comctl";

    int count = static_cast<int>(SafeSendMessage(hwnd, TB_BUTTONCOUNT, 0, 0));
    el.properties["buttonCount"] = std::to_string(count);

    auto proc = open_hwnd_process(hwnd);
    if (!proc) return;

    // TB_GETBUTTON needs a remote TBBUTTON struct
    RemoteBuffer remoteBtnBuf(proc.get(), sizeof(TBBUTTON));
    if (!remoteBtnBuf) return;

    constexpr int kTextBufSize = 256;
    RemoteBuffer remoteTextBuf(proc.get(), kTextBufSize * sizeof(wchar_t));
    if (!remoteTextBuf) return;

    for (int i = 0; i < count && i < 50; i++) {
        SafeSendMessage(hwnd, TB_GETBUTTON, i, reinterpret_cast<LPARAM>(remoteBtnBuf.ptr));

        TBBUTTON btn{};
        remoteBtnBuf.read(&btn, sizeof(btn));

        Element item;
        item.type = "ToolbarButton";
        item.framework = "comctl";
        item.properties["index"] = std::to_string(i);
        item.properties["commandId"] = std::to_string(btn.idCommand);

        if (btn.fsStyle & BTNS_SEP) {
            item.type = "ToolbarSeparator";
        } else {
            SafeSendMessage(hwnd, TB_GETBUTTONTEXTW, btn.idCommand,
                         reinterpret_cast<LPARAM>(remoteTextBuf.ptr));
            wchar_t textBuf[kTextBufSize]{};
            remoteTextBuf.read(textBuf, sizeof(textBuf));
            item.text = wstr_to_str(textBuf);
        }

        if (btn.fsState & TBSTATE_CHECKED)
            item.properties["checked"] = "true";
        if (!(btn.fsState & TBSTATE_ENABLED))
            item.properties["enabled"] = "false";

        el.children.push_back(std::move(item));
    }
}

void ComCtlProvider::enrich_statusbar(Element& el, HWND hwnd) {
    el.type = "StatusBar";
    el.framework = "comctl";

    int parts = static_cast<int>(SafeSendMessage(hwnd, SB_GETPARTS, 0, 0));
    el.properties["partCount"] = std::to_string(parts);

    auto proc = open_hwnd_process(hwnd);
    if (!proc) return;

    constexpr int kTextBufSize = 512;
    RemoteBuffer remoteTextBuf(proc.get(), kTextBufSize * sizeof(wchar_t));
    if (!remoteTextBuf) return;

    for (int i = 0; i < parts; i++) {
        Element item;
        item.type = "StatusBarPart";
        item.framework = "comctl";
        item.properties["index"] = std::to_string(i);

        // SB_GETTEXTW with a remote buffer
        SafeSendMessage(hwnd, SB_GETTEXTW, i, reinterpret_cast<LPARAM>(remoteTextBuf.ptr));
        wchar_t textBuf[kTextBufSize]{};
        remoteTextBuf.read(textBuf, sizeof(textBuf));
        item.text = wstr_to_str(textBuf);

        el.children.push_back(std::move(item));
    }
}

void ComCtlProvider::enrich_tabcontrol(Element& el, HWND hwnd) {
    el.type = "TabControl";
    el.framework = "comctl";

    int count = static_cast<int>(SafeSendMessage(hwnd, TCM_GETITEMCOUNT, 0, 0));
    int selected = static_cast<int>(SafeSendMessage(hwnd, TCM_GETCURSEL, 0, 0));
    el.properties["tabCount"] = std::to_string(count);
    el.properties["selectedIndex"] = std::to_string(selected);

    auto proc = open_hwnd_process(hwnd);
    if (!proc) return;

    constexpr int kTextBufSize = 256;
    constexpr SIZE_T kRemoteSize = sizeof(TCITEMW) + kTextBufSize * sizeof(wchar_t);
    RemoteBuffer remote(proc.get(), kRemoteSize);
    if (!remote) return;

    auto* remoteItem = reinterpret_cast<TCITEMW*>(remote.ptr);
    auto* remoteText = reinterpret_cast<wchar_t*>(
        static_cast<char*>(remote.ptr) + sizeof(TCITEMW));

    for (int i = 0; i < count; i++) {
        Element item;
        item.type = "Tab";
        item.framework = "comctl";
        item.properties["index"] = std::to_string(i);
        if (i == selected)
            item.properties["selected"] = "true";

        TCITEMW tci{};
        tci.mask = TCIF_TEXT;
        tci.pszText = remoteText;
        tci.cchTextMax = kTextBufSize;

        if (remote.write(&tci, sizeof(tci))) {
            SafeSendMessage(hwnd, TCM_GETITEMW, i, reinterpret_cast<LPARAM>(remoteItem));

            wchar_t textBuf[kTextBufSize]{};
            remote.read(textBuf, sizeof(textBuf));
            item.text = wstr_to_str(textBuf);
        }

        el.children.push_back(std::move(item));
    }
}

} // namespace lvt
