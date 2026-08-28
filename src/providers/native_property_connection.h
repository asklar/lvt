#pragma once

#include "framework_connection.h"

#include <Windows.h>
#include <CommCtrl.h>
#include <memory>
#include <string>

namespace lvt {

namespace native_property_detail {

bool parse_boolean(const std::string& value, bool& parsed);
bool parse_integer(const std::string& value, int& parsed);
int effective_scroll_max(int minimum, int maximum, unsigned pageSize);

} // namespace native_property_detail

// Persistent per-session adapter for curated Win32 and common-control typed
// properties. Providers register the native identities they emit in the tree;
// callers only receive opaque handles and descriptor ids.
class NativePropertyConnection final : public IFrameworkConnection {
public:
    static std::shared_ptr<NativePropertyConnection> connect(
        HWND root, DWORD pid, std::string provider,
        std::string controlVersion = {},
        bool requirePublishedTargets = false);

    ~NativePropertyConnection() override;

    const std::string& provider() const;
    bool pointer_operations_allowed() const;

    uint64_t register_hwnd(HWND hwnd);
    uint64_t register_listview_item(HWND hwnd, int index);
    uint64_t register_treeview_item(HWND hwnd, HTREEITEM item);
    uint64_t register_toolbar_button(HWND hwnd, int index, int commandId);
    uint64_t register_statusbar_part(HWND hwnd, int index);
    uint64_t register_tab_item(HWND hwnd, int index);
    void publish_targets(const Element& root);

    size_t cached_schema_count_for_testing() const;

    bool get_tree(
        Element&, bool, const std::string& = {}) override {
        return true;
    }
    std::vector<ConnectionEvent> poll_events() override { return {}; }
    bool is_alive() const override;
    PropertySnapshotResult get_property_snapshot(uint64_t handle) override;
    PropertyMutationResult set_property(
        uint64_t handle, const std::string& descriptorId,
        const std::string& value) override;
    PropertyMutationResult clear_property(
        uint64_t handle, const std::string& descriptorId) override;

private:
    struct Impl;
    explicit NativePropertyConnection(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> m_impl;
};

} // namespace lvt
