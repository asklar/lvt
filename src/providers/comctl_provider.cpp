#include "comctl_provider.h"

#include "native_message.h"
#include "native_property_connection.h"

#include <CommCtrl.h>
#include <optional>
#include <vector>

namespace lvt {
namespace {

std::optional<LRESULT> send(
    const NativeWindowIdentity& identity, UINT message,
    WPARAM wParam = 0, LPARAM lParam = 0) {
    auto result =
        send_native_message(identity, message, wParam, lParam);
    if (!result.ok)
        return std::nullopt;
    return result.value;
}

bool identity_for(HWND hwnd, NativeWindowIdentity& identity) {
    return capture_native_window_identity(hwnd, 0, identity).ok;
}

template <typename T>
bool read_remote(const RemoteBuffer& remote, T& value) {
    NativeMessageResult result;
    return remote.read(&value, sizeof(value), result);
}

bool read_remote_text(
    const RemoteBuffer& remote, size_t offset, size_t charCount,
    std::string& text) {
    std::vector<wchar_t> buffer(charCount, L'\0');
    NativeMessageResult result;
    if (!remote.read(
            buffer.data(), buffer.size() * sizeof(wchar_t), result, offset)) {
        return false;
    }
    const size_t length = wcsnlen_s(buffer.data(), buffer.size());
    text = native_utf16_to_utf8(
        std::wstring_view(buffer.data(), length));
    return true;
}

bool initialize_remote(
    const NativeWindowIdentity& identity, size_t size,
    RemoteBuffer& remote) {
    NativeMessageResult result;
    remote = RemoteBuffer::allocate(identity, size, result);
    return static_cast<bool>(remote);
}

void enrich_tree_item(
    Element& parent, const NativeWindowIdentity& identity,
    HTREEITEM itemHandle, NativePropertyConnection* properties,
    RemoteBuffer& remote, int& added) {
    if (!itemHandle || added >= 100)
        return;

    constexpr size_t itemSize = sizeof(TVITEMW);
    constexpr size_t textChars = 512;
    Element item;
    item.type = "TreeViewItem";
    item.framework = "comctl";
    if (properties) {
        item.providerHandle = properties->register_treeview_item(
            identity.hwnd, itemHandle);
    }

    TVITEMW value{};
    value.mask = TVIF_TEXT | TVIF_STATE | TVIF_CHILDREN;
    value.hItem = itemHandle;
    value.stateMask = TVIS_SELECTED | TVIS_EXPANDED;
    value.pszText = reinterpret_cast<wchar_t*>(
        static_cast<std::byte*>(remote.address()) + itemSize);
    value.cchTextMax = static_cast<int>(textChars);
    NativeMessageResult native;
    if (remote.write(&value, sizeof(value), native)) {
        auto got = send(
            identity, TVM_GETITEMW, 0,
            reinterpret_cast<LPARAM>(remote.address()));
        TVITEMW returned{};
        if (got && *got && read_remote(remote, returned)) {
            read_remote_text(remote, itemSize, textChars, item.text);
            if (returned.state & TVIS_SELECTED)
                item.properties["selected"] = "true";
            if (returned.state & TVIS_EXPANDED)
                item.properties["expanded"] = "true";
            if (returned.cChildren > 0)
                item.properties["hasChildren"] = "true";
        }
    }
    ++added;

    auto child = send(
        identity, TVM_GETNEXTITEM, TVGN_CHILD,
        reinterpret_cast<LPARAM>(itemHandle));
    HTREEITEM currentChild = child
        ? reinterpret_cast<HTREEITEM>(*child)
        : nullptr;
    while (currentChild && added < 100) {
        enrich_tree_item(
            item, identity, currentChild, properties, remote, added);
        auto next = send(
            identity, TVM_GETNEXTITEM, TVGN_NEXT,
            reinterpret_cast<LPARAM>(currentChild));
        currentChild =
            next ? reinterpret_cast<HTREEITEM>(*next) : nullptr;
    }
    parent.children.push_back(std::move(item));
}

} // namespace

void ComCtlProvider::enrich(
    Element& root, NativePropertyConnection* properties) {
    enrich_recursive(root, properties);
}

void ComCtlProvider::enrich_recursive(
    Element& el, NativePropertyConnection* properties) {
    HWND hwnd = reinterpret_cast<HWND>(el.nativeHandle);
    if (!hwnd)
        return;

    if (el.className == "SysListView32") {
        enrich_listview(el, hwnd, properties);
    } else if (el.className == "SysTreeView32") {
        enrich_treeview(el, hwnd, properties);
    } else if (el.className == "ToolbarWindow32") {
        enrich_toolbar(el, hwnd, properties);
    } else if (el.className == "msctls_statusbar32") {
        enrich_statusbar(el, hwnd, properties);
    } else if (el.className == "SysTabControl32") {
        enrich_tabcontrol(el, hwnd, properties);
    }

    for (auto& child : el.children)
        enrich_recursive(child, properties);
}

void ComCtlProvider::enrich_listview(
    Element& el, HWND hwnd, NativePropertyConnection* properties) {
    NativeWindowIdentity identity;
    if (!identity_for(hwnd, identity))
        return;

    el.type = "ListView";
    el.framework = "comctl";
    if (properties)
        el.providerHandle = properties->register_hwnd(hwnd);

    const auto countResult = send(identity, LVM_GETITEMCOUNT);
    const int count =
        countResult ? static_cast<int>(*countResult) : 0;
    el.properties["itemCount"] = std::to_string(count);

    const auto viewMode = send(identity, LVM_GETVIEW);
    if (viewMode) {
        switch (*viewMode) {
        case LV_VIEW_ICON: el.properties["viewMode"] = "icon"; break;
        case LV_VIEW_DETAILS: el.properties["viewMode"] = "details"; break;
        case LV_VIEW_SMALLICON:
            el.properties["viewMode"] = "smallicon";
            break;
        case LV_VIEW_LIST: el.properties["viewMode"] = "list"; break;
        case LV_VIEW_TILE: el.properties["viewMode"] = "tile"; break;
        }
    }

    const bool pointerAllowed =
        !properties || properties->pointer_operations_allowed();
    if (pointerAllowed) {
        const auto headerValue = send(identity, LVM_GETHEADER);
        HWND header =
            headerValue ? reinterpret_cast<HWND>(*headerValue) : nullptr;
        NativeWindowIdentity headerIdentity;
        if (header && identity_for(header, headerIdentity)) {
            const auto columns =
                send(headerIdentity, HDM_GETITEMCOUNT);
            if (columns)
                el.properties["columnCount"] = std::to_string(*columns);
        }
    }
    if (!pointerAllowed)
        return;

    constexpr size_t itemSize = sizeof(LVITEMW);
    constexpr size_t textChars = 512;
    RemoteBuffer remote;
    if (!initialize_remote(
            identity, itemSize + textChars * sizeof(wchar_t), remote)) {
        return;
    }

    const int maxItems = (std::min)(count, 50);
    for (int index = 0; index < maxItems; ++index) {
        Element item;
        item.type = "ListViewItem";
        item.framework = "comctl";
        item.properties["index"] = std::to_string(index);
        if (properties) {
            item.providerHandle =
                properties->register_listview_item(hwnd, index);
        }

        LVITEMW value{};
        value.mask = LVIF_TEXT | LVIF_STATE;
        value.iItem = index;
        value.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
        value.pszText = reinterpret_cast<wchar_t*>(
            static_cast<std::byte*>(remote.address()) + itemSize);
        value.cchTextMax = static_cast<int>(textChars);

        NativeMessageResult native;
        if (remote.write(&value, sizeof(value), native)) {
            const auto got = send(
                identity, LVM_GETITEMW, 0,
                reinterpret_cast<LPARAM>(remote.address()));
            LVITEMW returned{};
            if (got && *got && read_remote(remote, returned)) {
                read_remote_text(
                    remote, itemSize, textChars, item.text);
                if (returned.state & LVIS_SELECTED)
                    item.properties["selected"] = "true";
                if (returned.state & LVIS_FOCUSED)
                    item.properties["focused"] = "true";
            }
        }
        el.children.push_back(std::move(item));
    }
    if (count > maxItems)
        el.properties["truncated"] = "true";
}

void ComCtlProvider::enrich_treeview(
    Element& el, HWND hwnd, NativePropertyConnection* properties) {
    NativeWindowIdentity identity;
    if (!identity_for(hwnd, identity))
        return;

    el.type = "TreeView";
    el.framework = "comctl";
    if (properties)
        el.providerHandle = properties->register_hwnd(hwnd);

    const auto count = send(identity, TVM_GETCOUNT);
    el.properties["itemCount"] =
        std::to_string(count ? *count : 0);

    if (properties && !properties->pointer_operations_allowed())
        return;

    constexpr size_t itemSize = sizeof(TVITEMW);
    constexpr size_t textChars = 512;
    RemoteBuffer remote;
    if (!initialize_remote(
            identity, itemSize + textChars * sizeof(wchar_t), remote)) {
        return;
    }

    const auto rootValue =
        send(identity, TVM_GETNEXTITEM, TVGN_ROOT, 0);
    HTREEITEM item =
        rootValue ? reinterpret_cast<HTREEITEM>(*rootValue) : nullptr;
    int added = 0;
    while (item && added < 100) {
        enrich_tree_item(
            el, identity, item, properties, remote, added);
        const auto next = send(
            identity, TVM_GETNEXTITEM, TVGN_NEXT,
            reinterpret_cast<LPARAM>(item));
        item = next ? reinterpret_cast<HTREEITEM>(*next) : nullptr;
    }
    if (count && *count > added)
        el.properties["truncated"] = "true";
}

void ComCtlProvider::enrich_toolbar(
    Element& el, HWND hwnd, NativePropertyConnection* properties) {
    NativeWindowIdentity identity;
    if (!identity_for(hwnd, identity))
        return;

    el.type = "Toolbar";
    el.framework = "comctl";
    if (properties)
        el.providerHandle = properties->register_hwnd(hwnd);

    const auto countResult = send(identity, TB_BUTTONCOUNT);
    const int count =
        countResult ? static_cast<int>(*countResult) : 0;
    el.properties["buttonCount"] = std::to_string(count);

    if (properties && !properties->pointer_operations_allowed())
        return;

    RemoteBuffer buttonBuffer;
    RemoteBuffer textBuffer;
    constexpr size_t textChars = 256;
    if (!initialize_remote(identity, sizeof(TBBUTTON), buttonBuffer) ||
        !initialize_remote(
            identity, textChars * sizeof(wchar_t), textBuffer)) {
        return;
    }

    for (int index = 0; index < count && index < 50; ++index) {
        const auto got = send(
            identity, TB_GETBUTTON, index,
            reinterpret_cast<LPARAM>(buttonBuffer.address()));
        TBBUTTON button{};
        if (!got || !*got || !read_remote(buttonBuffer, button))
            continue;

        Element item;
        item.type =
            (button.fsStyle & BTNS_SEP)
                ? "ToolbarSeparator"
                : "ToolbarButton";
        item.framework = "comctl";
        item.properties["index"] = std::to_string(index);
        item.properties["commandId"] =
            std::to_string(button.idCommand);
        if (properties && !(button.fsStyle & BTNS_SEP)) {
            item.providerHandle = properties->register_toolbar_button(
                hwnd, index, button.idCommand);
        }

        if (!(button.fsStyle & BTNS_SEP)) {
            const auto length = send(
                identity, TB_GETBUTTONTEXTW, button.idCommand,
                reinterpret_cast<LPARAM>(textBuffer.address()));
            if (length && *length >= 0) {
                read_remote_text(
                    textBuffer, 0, textChars, item.text);
            }
        }
        if (button.fsState & TBSTATE_CHECKED)
            item.properties["checked"] = "true";
        if (!(button.fsState & TBSTATE_ENABLED))
            item.properties["enabled"] = "false";
        el.children.push_back(std::move(item));
    }
}

void ComCtlProvider::enrich_statusbar(
    Element& el, HWND hwnd, NativePropertyConnection* properties) {
    NativeWindowIdentity identity;
    if (!identity_for(hwnd, identity))
        return;

    el.type = "StatusBar";
    el.framework = "comctl";
    if (properties)
        el.providerHandle = properties->register_hwnd(hwnd);

    const auto partsResult = send(identity, SB_GETPARTS);
    const int parts =
        partsResult ? static_cast<int>(*partsResult) : 0;
    el.properties["partCount"] = std::to_string(parts);

    if (properties && !properties->pointer_operations_allowed())
        return;

    for (int index = 0; index < parts; ++index) {
        Element item;
        item.type = "StatusBarPart";
        item.framework = "comctl";
        item.properties["index"] = std::to_string(index);
        if (properties) {
            item.providerHandle =
                properties->register_statusbar_part(hwnd, index);
        }

        const auto length =
            send(identity, SB_GETTEXTLENGTHW, index);
        if (length) {
            const size_t chars =
                static_cast<size_t>(LOWORD(*length)) + 1;
            RemoteBuffer textBuffer;
            if (initialize_remote(
                    identity, chars * sizeof(wchar_t), textBuffer)) {
                const auto got = send(
                    identity, SB_GETTEXTW, index,
                    reinterpret_cast<LPARAM>(textBuffer.address()));
                if (got) {
                    read_remote_text(
                        textBuffer, 0, chars, item.text);
                }
            }
        }
        el.children.push_back(std::move(item));
    }
}

void ComCtlProvider::enrich_tabcontrol(
    Element& el, HWND hwnd, NativePropertyConnection* properties) {
    NativeWindowIdentity identity;
    if (!identity_for(hwnd, identity))
        return;

    el.type = "TabControl";
    el.framework = "comctl";
    if (properties)
        el.providerHandle = properties->register_hwnd(hwnd);

    const auto countResult = send(identity, TCM_GETITEMCOUNT);
    const auto selectedResult = send(identity, TCM_GETCURSEL);
    const int count =
        countResult ? static_cast<int>(*countResult) : 0;
    const int selected =
        selectedResult ? static_cast<int>(*selectedResult) : -1;
    el.properties["tabCount"] = std::to_string(count);
    el.properties["selectedIndex"] = std::to_string(selected);

    if (properties && !properties->pointer_operations_allowed())
        return;

    constexpr size_t itemSize = sizeof(TCITEMW);
    constexpr size_t textChars = 256;
    RemoteBuffer remote;
    if (!initialize_remote(
            identity, itemSize + textChars * sizeof(wchar_t), remote)) {
        return;
    }

    for (int index = 0; index < count; ++index) {
        Element item;
        item.type = "Tab";
        item.framework = "comctl";
        item.properties["index"] = std::to_string(index);
        if (index == selected)
            item.properties["selected"] = "true";
        if (properties)
            item.providerHandle =
                properties->register_tab_item(hwnd, index);

        TCITEMW value{};
        value.mask = TCIF_TEXT;
        value.pszText = reinterpret_cast<wchar_t*>(
            static_cast<std::byte*>(remote.address()) + itemSize);
        value.cchTextMax = static_cast<int>(textChars);
        NativeMessageResult native;
        if (remote.write(&value, sizeof(value), native)) {
            const auto got = send(
                identity, TCM_GETITEMW, index,
                reinterpret_cast<LPARAM>(remote.address()));
            if (got && *got) {
                read_remote_text(
                    remote, itemSize, textChars, item.text);
            }
        }
        el.children.push_back(std::move(item));
    }
}

} // namespace lvt
