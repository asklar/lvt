#include "comctl_provider.h"

#include "native_message.h"
#include "native_property_connection.h"

#include <CommCtrl.h>
#include <map>
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

std::optional<LRESULT> send_pointer(
    const NativeWindowIdentity& identity, UINT message,
    WPARAM wParam, LPARAM lParam, std::shared_ptr<void> keepAlive) {
    auto result = send_native_pointer_message(
        identity, message, wParam, lParam, std::move(keepAlive));
    if (!result.ok)
        return std::nullopt;
    return result.value;
}

bool identity_for(HWND hwnd, NativeWindowIdentity& identity) {
    return capture_native_window_identity(hwnd, 0, identity).ok;
}

template <typename T>
bool read_remote(
    const std::shared_ptr<RemoteBuffer>& remote, T& value) {
    NativeMessageResult result;
    return remote->read(&value, sizeof(value), result);
}

bool read_remote_text(
    const std::shared_ptr<RemoteBuffer>& remote,
    size_t offset, size_t charCount,
    std::string& text) {
    std::vector<wchar_t> buffer(charCount, L'\0');
    NativeMessageResult result;
    if (!remote->read(
            buffer.data(), buffer.size() * sizeof(wchar_t), result, offset)) {
        return false;
    }
    const size_t length = wcsnlen_s(buffer.data(), buffer.size());
    text = native_utf16_to_utf8(
        std::wstring_view(buffer.data(), length));
    return true;
}

std::shared_ptr<RemoteBuffer> allocate_remote(
    const NativeWindowIdentity& identity, size_t size,
    NativeMessageResult& result) {
    auto remote = std::make_shared<RemoteBuffer>(
        RemoteBuffer::allocate(identity, size, result));
    return *remote ? remote : nullptr;
}

std::shared_ptr<RemoteBuffer> allocate_remote(
    const NativeWindowIdentity& identity, size_t size) {
    NativeMessageResult result;
    return allocate_remote(identity, size, result);
}

void enrich_tree_item(
    Element& parent, const NativeWindowIdentity& identity,
    HTREEITEM itemHandle, NativePropertyConnection* properties,
    int& added) {
    if (!itemHandle || added >= 100)
        return;

    Element item;
    item.type = "TreeViewItem";
    item.framework = "comctl";
    if (properties) {
        item.providerHandle = properties->register_treeview_item(
            identity.hwnd, itemHandle);
    }

    TVITEMW value{};
    value.mask = TVIF_STATE | TVIF_CHILDREN;
    value.hItem = itemHandle;
    value.stateMask = TVIS_SELECTED | TVIS_EXPANDED;
    NativeMessageResult native;
    auto remote = allocate_remote(identity, sizeof(TVITEMW), native);
    if (remote && remote->write(&value, sizeof(value), native)) {
        auto got = send_pointer(
            identity, TVM_GETITEMW, 0,
            reinterpret_cast<LPARAM>(remote->address()), remote);
        TVITEMW returned{};
        if (got && *got && read_remote(remote, returned)) {
            if (returned.state & TVIS_SELECTED)
                item.properties["selected"] = "true";
            if (returned.state & TVIS_EXPANDED)
                item.properties["expanded"] = "true";
            if (returned.cChildren > 0)
                item.properties["hasChildren"] = "true";
        }
    }
    read_native_treeview_item_text(
        identity, itemHandle, item.text);
    ++added;

    auto child = send(
        identity, TVM_GETNEXTITEM, TVGN_CHILD,
        reinterpret_cast<LPARAM>(itemHandle));
    HTREEITEM currentChild = child
        ? reinterpret_cast<HTREEITEM>(*child)
        : nullptr;
    while (currentChild && added < 100) {
        enrich_tree_item(
            item, identity, currentChild, properties, added);
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
    Element& root, Architecture targetArchitecture,
    NativePropertyConnection* properties) {
    enrich_recursive(root, targetArchitecture, properties);
}

void ComCtlProvider::enrich_recursive(
    Element& el, Architecture targetArchitecture,
    NativePropertyConnection* properties) {
    HWND hwnd = reinterpret_cast<HWND>(el.nativeHandle);
    if (!hwnd)
        return;

    const bool pointerAllowed =
        native_pointer_operations_allowed(
            get_host_architecture(), targetArchitecture) &&
        (!properties || properties->pointer_operations_allowed());

    if (el.className == "SysListView32") {
        enrich_listview(el, hwnd, pointerAllowed, properties);
    } else if (el.className == "SysTreeView32") {
        enrich_treeview(el, hwnd, pointerAllowed, properties);
    } else if (el.className == "ToolbarWindow32") {
        enrich_toolbar(el, hwnd, pointerAllowed, properties);
    } else if (el.className == "msctls_statusbar32") {
        enrich_statusbar(el, hwnd, pointerAllowed, properties);
    } else if (el.className == "SysTabControl32") {
        enrich_tabcontrol(el, hwnd, pointerAllowed, properties);
    }

    for (auto& child : el.children)
        enrich_recursive(child, targetArchitecture, properties);
}

void ComCtlProvider::enrich_listview(
    Element& el, HWND hwnd, bool pointerAllowed,
    NativePropertyConnection* properties) {
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
        value.mask = LVIF_STATE;
        value.iItem = index;
        value.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
        NativeMessageResult native;
        auto remote = allocate_remote(
            identity, sizeof(LVITEMW), native);
        if (!remote)
            break;

        if (remote->write(&value, sizeof(value), native)) {
            const auto got = send_pointer(
                identity, LVM_GETITEMW, 0,
                reinterpret_cast<LPARAM>(remote->address()), remote);
            LVITEMW returned{};
            if (got && *got && read_remote(remote, returned)) {
                if (returned.state & LVIS_SELECTED)
                    item.properties["selected"] = "true";
                if (returned.state & LVIS_FOCUSED)
                    item.properties["focused"] = "true";
            }
        }
        read_native_listview_item_text(identity, index, item.text);
        el.children.push_back(std::move(item));
    }
    if (count > maxItems)
        el.properties["truncated"] = "true";
}

void ComCtlProvider::enrich_treeview(
    Element& el, HWND hwnd, bool pointerAllowed,
    NativePropertyConnection* properties) {
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

    if (!pointerAllowed)
        return;

    const auto rootValue =
        send(identity, TVM_GETNEXTITEM, TVGN_ROOT, 0);
    HTREEITEM item =
        rootValue ? reinterpret_cast<HTREEITEM>(*rootValue) : nullptr;
    int added = 0;
    while (item && added < 100) {
        enrich_tree_item(
            el, identity, item, properties, added);
        const auto next = send(
            identity, TVM_GETNEXTITEM, TVGN_NEXT,
            reinterpret_cast<LPARAM>(item));
        item = next ? reinterpret_cast<HTREEITEM>(*next) : nullptr;
    }
    if (count && *count > added)
        el.properties["truncated"] = "true";
}

void ComCtlProvider::enrich_toolbar(
    Element& el, HWND hwnd, bool pointerAllowed,
    NativePropertyConnection* properties) {
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

    if (!pointerAllowed)
        return;

    const int visibleCount = (std::min)(count, 50);
    std::vector<std::optional<TBBUTTON>> buttons(
        static_cast<size_t>(visibleCount));
    std::map<int, int> commandCounts;
    for (int index = 0; index < visibleCount; ++index) {
        NativeMessageResult native;
        auto buttonBuffer =
            allocate_remote(identity, sizeof(TBBUTTON), native);
        if (!buttonBuffer)
            break;
        const auto got = send_pointer(
            identity, TB_GETBUTTON, index,
            reinterpret_cast<LPARAM>(buttonBuffer->address()),
            buttonBuffer);
        TBBUTTON button{};
        if (!got || !*got || !read_remote(buttonBuffer, button))
            continue;
        buttons[static_cast<size_t>(index)] = button;
        if (!(button.fsStyle & BTNS_SEP))
            ++commandCounts[button.idCommand];
    }

    for (int index = 0; index < visibleCount; ++index) {
        const auto& stored = buttons[static_cast<size_t>(index)];
        if (!stored)
            continue;
        const auto& button = *stored;

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

        const bool ambiguous =
            !(button.fsStyle & BTNS_SEP) &&
            commandCounts[button.idCommand] != 1;
        if (ambiguous)
            item.properties["ambiguousCommandId"] = "true";
        if (!(button.fsStyle & BTNS_SEP) && !ambiguous) {
            read_native_toolbar_button_text(
                identity, index, button.idCommand, item.text);
        }
        if (button.fsState & TBSTATE_CHECKED)
            item.properties["checked"] = "true";
        if (!(button.fsState & TBSTATE_ENABLED))
            item.properties["enabled"] = "false";
        el.children.push_back(std::move(item));
    }
}

void ComCtlProvider::enrich_statusbar(
    Element& el, HWND hwnd, bool pointerAllowed,
    NativePropertyConnection* properties) {
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

    if (!pointerAllowed)
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
            const UINT style = HIWORD(*length);
            if (style & SBT_OWNERDRAW) {
                item.properties["ownerDraw"] = "true";
                el.children.push_back(std::move(item));
                continue;
            }
            constexpr size_t chars = 0x10000;
            NativeMessageResult native;
            auto textBuffer = allocate_remote(
                identity, chars * sizeof(wchar_t), native);
            if (textBuffer) {
                const auto got = send_pointer(
                    identity, SB_GETTEXTW, index,
                    reinterpret_cast<LPARAM>(
                        textBuffer->address()),
                    textBuffer);
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
    Element& el, HWND hwnd, bool pointerAllowed,
    NativePropertyConnection* properties) {
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

    if (!pointerAllowed)
        return;

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

        read_native_tab_item_text(identity, index, item.text);
        el.children.push_back(std::move(item));
    }
}

} // namespace lvt
