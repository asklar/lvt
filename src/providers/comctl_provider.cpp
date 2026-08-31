#include "comctl_provider.h"

#include "native_message.h"
#include "native_property_connection.h"

#include <CommCtrl.h>
#include <cstdio>
#include <map>
#include <optional>
#include <string_view>
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

uint64_t stable_identity_hash(std::string_view value) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (const unsigned char byte : value) {
        hash ^= byte;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

std::string hashed_identity(
    const char* kind, std::string_view value) {
    char buffer[80]{};
    snprintf(
        buffer, sizeof(buffer), "%s:%zu:0x%016llX",
        kind, value.size(),
        static_cast<unsigned long long>(
            stable_identity_hash(value)));
    return buffer;
}

bool read_toolbar_button(
    const NativeWindowIdentity& identity, int index,
    TBBUTTON& button) {
    NativeMessageResult native;
    auto buttonBuffer =
        allocate_remote(identity, sizeof(TBBUTTON), native);
    if (!buttonBuffer)
        return false;
    const auto got = send_pointer(
        identity, TB_GETBUTTON, index,
        reinterpret_cast<LPARAM>(buttonBuffer->address()),
        buttonBuffer);
    return got && *got && read_remote(buttonBuffer, button);
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
    {
        // HTREEITEM is the documented, control-owned item identity and is
        // unique within the live tree control. Fingerprinting it keeps the
        // pointer-shaped value private; the 100-node display cap cannot hide
        // another item with the same handle.
        const uint64_t itemValue = static_cast<uint64_t>(
            reinterpret_cast<uintptr_t>(itemHandle));
        item.durableIdentity = hashed_identity(
            "tree-item",
            std::string_view(
                reinterpret_cast<const char*>(&itemValue),
                sizeof(itemValue)));
    }
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

    DWORD elementPid = 0;
    GetWindowThreadProcessId(hwnd, &elementPid);
    const Architecture elementArchitecture =
        elementPid != 0
            ? detect_process_architecture(elementPid)
            : Architecture::unknown;
    const bool pointerAllowed =
        native_pointer_operations_allowed(
            get_host_architecture(),
            elementArchitecture);

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
    el.nativeLifetimeHandle =
        native_window_provider_handle(identity);
    if (properties)
        el.providerHandle = properties->register_hwnd(hwnd);

    const auto countResult = send(identity, LVM_GETITEMCOUNT);
    const int count =
        countResult && *countResult > 0
            ? static_cast<int>(*countResult)
            : 0;
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
    // Identity proof is independent of display truncation. Either inspect the
    // complete control or assign no text identity at all.
    bool identityScanComplete =
        static_cast<size_t>(count) <=
        kMaximumNativeIdentityScanItems;
    std::map<std::string, int> textCounts;
    std::vector<std::optional<std::string>> scannedTexts(
        static_cast<size_t>(maxItems));
    if (identityScanComplete) {
        for (int index = 0; index < count; ++index) {
            std::string text;
            const auto read = read_native_listview_item_text(
                identity, index, text);
            if (!read.ok) {
                identityScanComplete = false;
                textCounts.clear();
                break;
            }
            if (index < maxItems)
                scannedTexts[static_cast<size_t>(index)] = text;
            if (!text.empty())
                ++textCounts[text];
        }
    }

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
        const auto& scanned =
            scannedTexts[static_cast<size_t>(index)];
        if (scanned) {
            item.text = *scanned;
        } else {
            read_native_listview_item_text(
                identity, index, item.text);
        }
        if (identityScanComplete && !item.text.empty() &&
            textCounts[item.text] == 1) {
            item.durableIdentity =
                hashed_identity("item-text", item.text);
        }
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
    el.nativeLifetimeHandle =
        native_window_provider_handle(identity);
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
    el.nativeLifetimeHandle =
        native_window_provider_handle(identity);
    if (properties)
        el.providerHandle = properties->register_hwnd(hwnd);

    const auto countResult = send(identity, TB_BUTTONCOUNT);
    const int count =
        countResult && *countResult > 0
            ? static_cast<int>(*countResult)
            : 0;
    el.properties["buttonCount"] = std::to_string(count);

    if (!pointerAllowed)
        return;

    const int visibleCount = (std::min)(count, 50);
    std::vector<std::optional<TBBUTTON>> buttons(
        static_cast<size_t>(visibleCount));
    std::map<int, int> commandCounts;
    // As with list items, the first 50 buttons are only the display budget.
    // Command uniqueness is proven over the complete toolbar.
    bool identityScanComplete =
        static_cast<size_t>(count) <=
        kMaximumNativeIdentityScanItems;
    for (int index = 0; index < visibleCount; ++index) {
        TBBUTTON button{};
        if (!read_toolbar_button(identity, index, button)) {
            identityScanComplete = false;
            continue;
        }
        buttons[static_cast<size_t>(index)] = button;
        if (!(button.fsStyle & BTNS_SEP))
            ++commandCounts[button.idCommand];
    }
    if (identityScanComplete) {
        for (int index = visibleCount; index < count; ++index) {
            TBBUTTON button{};
            if (!read_toolbar_button(identity, index, button)) {
                identityScanComplete = false;
                commandCounts.clear();
                break;
            }
            if (!(button.fsStyle & BTNS_SEP))
                ++commandCounts[button.idCommand];
        }
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
            identityScanComplete &&
            commandCounts[button.idCommand] != 1;
        if (!(button.fsStyle & BTNS_SEP) &&
            identityScanComplete && !ambiguous) {
            item.durableIdentity =
                "command-id:" + std::to_string(button.idCommand);
        } else if (!(button.fsStyle & BTNS_SEP) &&
                   !identityScanComplete) {
            item.properties["commandIdentityUnverified"] = "true";
        }
        if (ambiguous)
            item.properties["ambiguousCommandId"] = "true";
        if (!(button.fsStyle & BTNS_SEP) &&
            identityScanComplete && !ambiguous) {
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
    el.nativeLifetimeHandle =
        native_window_provider_handle(identity);
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
    el.nativeLifetimeHandle =
        native_window_provider_handle(identity);
    if (properties)
        el.providerHandle = properties->register_hwnd(hwnd);

    const auto countResult = send(identity, TCM_GETITEMCOUNT);
    const auto selectedResult = send(identity, TCM_GETCURSEL);
    const int count =
        countResult && *countResult > 0
            ? static_cast<int>(*countResult)
            : 0;
    const int selected =
        selectedResult ? static_cast<int>(*selectedResult) : -1;
    el.properties["tabCount"] = std::to_string(count);
    el.properties["selectedIndex"] = std::to_string(selected);

    if (!pointerAllowed)
        return;

    bool identityScanComplete =
        static_cast<size_t>(count) <=
        kMaximumNativeIdentityScanItems;
    // Tab item data is application-defined when TCM_SETITEMEXTRA is used, so
    // text is the only generic safe identity. Prove it across every tab.
    std::map<std::string, int> textCounts;
    std::vector<std::optional<std::string>> texts(
        static_cast<size_t>(count));
    for (int index = 0; index < count; ++index) {
        std::string text;
        const auto read =
            read_native_tab_item_text(identity, index, text);
        if (!read.ok) {
            identityScanComplete = false;
            continue;
        }
        texts[static_cast<size_t>(index)] = std::move(text);
        if (identityScanComplete &&
            !texts[static_cast<size_t>(index)]->empty()) {
            ++textCounts[*texts[static_cast<size_t>(index)]];
        }
    }
    if (!identityScanComplete)
        textCounts.clear();

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

        const auto& text = texts[static_cast<size_t>(index)];
        if (text)
            item.text = *text;
        if (identityScanComplete && !item.text.empty() &&
            textCounts[item.text] == 1) {
            item.durableIdentity =
                hashed_identity("item-text", item.text);
        }
        el.children.push_back(std::move(item));
    }
}

} // namespace lvt
