//! MCP tool surface.
//!
//! Every tool is a thin shim: validate nothing here that lvt already validates,
//! just name the method and forward the arguments. The schemas exist to tell
//! the model what the arguments *mean*, which is the part lvt's JSON cannot
//! convey.
//!
//! Tools are split across two routers so that `--allow-input` can withhold the
//! mutating half entirely. Withholding, rather than refusing at call time,
//! matters: a tool the model cannot see is a tool it cannot be talked into
//! trying, and it keeps the read-only mode honest in `tools/list`.

use std::collections::HashMap;
use std::sync::Arc;
use std::time::Duration;

use rmcp::handler::server::router::tool::ToolRouter;
use rmcp::handler::server::wrapper::Parameters;
use rmcp::model::{
    CallToolResult, ContentBlock, Implementation, ListResourcesResult, PaginatedRequestParams,
    ReadResourceRequestParams, ReadResourceResponse, ReadResourceResult, Resource,
    ResourceContents, ResourceUpdatedNotificationParam, ServerCapabilities, ServerInfo,
    SubscribeRequestParams, SubscriptionFilter, UnsubscribeRequestParams,
};
use rmcp::service::{RequestContext, SubscriptionContext, SubscriptionSink};
use rmcp::{tool, tool_handler, tool_router, ErrorData, Peer, RoleServer, ServerHandler};
use serde::{Deserialize, Serialize};
use serde_json::json;
use tokio::sync::{watch, Mutex};

use crate::ffi;

// --- argument types -----------------------------------------------------
//
// Deliberately reused across tools where the shape is identical. Each field's
// doc comment becomes its description in the JSON schema the model sees, so
// they are written for that audience.

#[derive(Debug, Deserialize, schemars::JsonSchema)]
pub struct ListAppsArgs {
    /// Only list windows belonging to this process name, e.g. "notepad".
    pub name: Option<String>,
    /// Only list windows whose title contains this text (case-insensitive).
    pub title: Option<String>,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
pub struct ConnectArgs {
    /// Process name to connect to, e.g. "notepad". Fails if several windows match.
    pub name: Option<String>,
    /// Window title substring to connect to (case-insensitive).
    pub title: Option<String>,
    /// Process id to connect to.
    pub pid: Option<u32>,
    /// Window handle, decimal or 0x-prefixed hex. The unambiguous option.
    pub hwnd: Option<String>,
    /// How this session drives the app.
    ///
    /// "uia" (default) works through UI Automation: element references come
    /// from get_uia_tree/find_elements, and actions use the control's own
    /// patterns — no mouse movement, no focus stealing, and it works across
    /// architectures.
    ///
    /// "visual" works through the framework-native tree instead: references
    /// come from get_visual_tree, and actions are real mouse clicks and
    /// keystrokes aimed at where the element is. Use it for UIs that UI
    /// Automation cannot see properly — custom-drawn controls, canvas, games —
    /// or when the app must observe genuine input. Only click, scroll, type,
    /// press_key, focus and the window commands exist in this mode, because
    /// geometry cannot express what toggle or set_value mean.
    pub mode: Option<String>,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
pub struct SessionArgs {
    /// Session id returned by connect.
    pub session: String,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
pub struct TreeArgs {
    /// Session id returned by connect.
    pub session: String,
    /// Element reference. Use the "ref" a tool in this session gave you
    /// ("uia:e15" or "visual:e33"): it names the tree it came from, and a
    /// session only accepts its own tree's references — the other tree's are
    /// refused, never matched to something that looks similar. A durable key
    /// or "uia:<RuntimeId>" also works. A bare "eN" is read against this
    /// session's tree.
    pub element: Option<String>,
    /// Maximum depth below the root. Omit for the whole tree.
    pub depth: Option<i32>,
    /// UIA tree view: "control" (default, what a user perceives), "content"
    /// (narrower still), or "raw" (everything, including layout scaffolding).
    pub view: Option<String>,
    /// Extra UIA properties to include beyond the default set.
    pub properties: Option<Vec<String>>,
    /// How long the UI Automation walk may take, in milliseconds (default
    /// 10000). Raise this if a result comes back marked "truncated".
    #[serde(rename = "timeoutMs")]
    pub timeout_ms: Option<i32>,
}

/// The visual tree takes everything TreeArgs does plus `correlate`. The two
/// are separate types rather than one shared type with an ignored field: a
/// flag the UIA tool advertises but cannot honour is an invitation to pass it.
#[derive(Debug, Deserialize, schemars::JsonSchema)]
pub struct VisualTreeArgs {
    /// Session id returned by connect.
    pub session: String,
    /// Element reference. Use the "ref" a tool in this session gave you
    /// ("uia:e15" or "visual:e33"): it names the tree it came from, and a
    /// session only accepts its own tree's references — the other tree's are
    /// refused, never matched to something that looks similar. A durable key
    /// or "uia:<RuntimeId>" also works. A bare "eN" is read against this
    /// session's tree.
    pub element: Option<String>,
    /// Maximum depth below the root. Omit for the whole tree.
    pub depth: Option<i32>,
    /// UIA tree view applied to the correlation walk: "control" (default),
    /// "content", or "raw".
    pub view: Option<String>,
    /// Extra UIA properties to include beyond the default set.
    pub properties: Option<Vec<String>>,
    /// How long the UI Automation walk may take, in milliseconds (default
    /// 10000). Raise this if a result comes back marked "truncated".
    #[serde(rename = "timeoutMs")]
    pub timeout_ms: Option<i32>,
    /// Also report each element's UI Automation counterpart. "uiaRef" is the
    /// element's own counterpart; "uiaAncestorRef" is the counterpart of the
    /// control it sits inside, which is context only. Many visual nodes are
    /// template parts with no counterpart of their own.
    ///
    /// Use this to find out what UI Automation can and cannot see: an element
    /// with no "uiaRef" is not exposed to assistive tech at all, which is
    /// usually an accessibility gap in the app, and tells you that element can
    /// only be driven in "visual" mode. References are never translated between
    /// trees, so this is information, not a way to make another tool accept a
    /// visual reference.
    ///
    /// Costs a second walk, so it is off by default.
    pub correlate: Option<bool>,
    /// Skip the XAML/WinUI3 full property-chain walk in favor of cheaper
    /// direct property reads. Much faster on a rich tree, but only reports
    /// bounds/Text/Content/basic state — not arbitrary custom properties.
    /// Off by default (full properties, matching get_element_properties).
    pub fast: Option<bool>,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
pub struct VisualTreeChangesArgs {
    /// Session id returned by connect.
    pub session: String,
    /// Use the cheaper XAML/WinUI3 property set. Changing this setting resets
    /// the session's diff baseline and returns a fresh snapshot.
    pub fast: Option<bool>,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
pub struct UiaTreeChangesArgs {
    /// Session id returned by connect.
    pub session: String,
    /// UIA tree view: "control" (default), "content", or "raw". Changing it
    /// resets the session's diff baseline and returns a fresh snapshot.
    pub view: Option<String>,
    /// Extra UIA properties to include beyond the default set. Changing this
    /// list resets the diff baseline.
    pub properties: Option<Vec<String>>,
    /// How long the UI Automation walk may take, in milliseconds (default
    /// 10000). Changing it resets the diff baseline.
    #[serde(rename = "timeoutMs")]
    pub timeout_ms: Option<i32>,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
pub struct VisualPropertiesArgs {
    /// Session id returned by connect.
    pub session: String,
    /// Compact XAML diagnostics key from get_visual_tree, such as
    /// "xaml:0x123" or "winui3:0xABC".
    pub key: String,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
pub struct SetVisualPropertyArgs {
    /// Session id returned by connect.
    pub session: String,
    /// Compact XAML diagnostics key from get_visual_tree.
    pub key: String,
    /// Dependency-property index returned by get_visual_properties.
    #[serde(rename = "propertyIndex")]
    pub property_index: u32,
    /// xamlOM value type returned by get_visual_properties, such as "Double".
    #[serde(rename = "valueType")]
    pub value_type: String,
    /// Text passed to IVisualTreeService::CreateInstance for conversion.
    pub value: String,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
pub struct ClearVisualPropertyArgs {
    /// Session id returned by connect.
    pub session: String,
    /// Compact XAML diagnostics key from get_visual_tree.
    pub key: String,
    /// Dependency-property index returned by get_visual_properties.
    #[serde(rename = "propertyIndex")]
    pub property_index: u32,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
pub struct FindArgs {
    /// Session id returned by connect.
    pub session: String,
    /// Match this exact AutomationId. The most reliable way to find a control.
    #[serde(rename = "automationId")]
    pub automation_id: Option<String>,
    /// Match elements whose name/text contains this (case-insensitive).
    pub name: Option<String>,
    /// Match elements whose control type contains this, e.g. "Button".
    #[serde(rename = "type")]
    pub control_type: Option<String>,
    /// Match elements supporting this UIA pattern, e.g. "Invoke", "Toggle".
    pub pattern: Option<String>,
    /// Maximum number of matches to return. Defaults to 50.
    pub limit: Option<i32>,
    /// Search the UIA tree (default) or the framework-native visual tree.
    pub uia: Option<bool>,
    /// UIA tree view to search: "control" (default), "content", or "raw".
    pub view: Option<String>,
    /// How long the UI Automation walk may take, in milliseconds (default
    /// 10000). Raise this if a result comes back marked "truncated".
    #[serde(rename = "timeoutMs")]
    pub timeout_ms: Option<i32>,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
pub struct ElementPropertiesArgs {
    /// Session id returned by connect.
    pub session: String,
    /// Element reference. Use the "ref" a tool in this session gave you
    /// ("uia:e15" or "visual:e33"): it names the tree it came from, and a
    /// session only accepts its own tree's references — the other tree's are
    /// refused, never matched to something that looks similar. A durable key
    /// or "uia:<RuntimeId>" also works. A bare "eN" is read against this
    /// session's tree.
    pub element: String,
    /// Specific properties to return. Omit for the element's standard fields.
    pub properties: Option<Vec<String>>,
    /// Read from the UIA tree (default) or the framework-native visual tree.
    pub uia: Option<bool>,
    /// UIA tree view: "control" (default), "content", or "raw".
    pub view: Option<String>,
    /// How long the UI Automation walk may take, in milliseconds (default
    /// 10000). Raise this if a result comes back marked "truncated".
    #[serde(rename = "timeoutMs")]
    pub timeout_ms: Option<i32>,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
pub struct ScreenshotArgs {
    /// Session id returned by connect.
    pub session: String,
    /// Write the PNG here instead of returning the image inline. Creates or
    /// overwrites the file, so it needs --allow-input. Use it for large
    /// windows, where an inline image costs a lot of context.
    pub path: Option<String>,
    /// Annotate only this element and its descendants.
    pub element: Option<String>,
    /// Annotate using the UIA tree (default) or the framework-native visual
    /// tree. Leave this alone unless you specifically want visual-tree ids:
    /// the ids drawn on the image only mean anything to the other tools if
    /// they come from the same tree those tools resolve against.
    pub uia: Option<bool>,
    /// How long the UI Automation walk may take, in milliseconds (default 10000).
    #[serde(rename = "timeoutMs")]
    pub timeout_ms: Option<i32>,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
pub struct HitTestArgs {
    /// Session id returned by connect.
    pub session: String,
    /// Screen x coordinate, in physical pixels.
    pub x: i32,
    /// Screen y coordinate, in physical pixels.
    pub y: i32,
    /// Hit-test the UIA tree (default) or the framework-native visual tree.
    pub uia: Option<bool>,
    /// UIA tree view: "control" (default), "content", or "raw".
    pub view: Option<String>,
    /// How long the UI Automation walk may take, in milliseconds (default
    /// 10000). Raise this if a result comes back marked "truncated".
    #[serde(rename = "timeoutMs")]
    pub timeout_ms: Option<i32>,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
pub struct ElementArgs {
    /// Session id returned by connect.
    pub session: String,
    /// Element reference. Use the "ref" a tool in this session gave you
    /// ("uia:e15" or "visual:e33"): it names the tree it came from, and a
    /// session only accepts its own tree's references — the other tree's are
    /// refused, never matched to something that looks similar. A durable key
    /// or "uia:<RuntimeId>" also works. A bare "eN" is read against this
    /// session's tree.
    pub element: String,
    /// UIA tree view used to resolve the element: "control" (default),
    /// "content", or "raw". Must match the view the id came from.
    pub view: Option<String>,
    /// Which tree a bare "eN" id should be read against: true for the UIA
    /// tree, false for the visual tree. Defaults to this session's mode.
    /// Qualified references ("uia:e15", "visual:e33") and durable keys already
    /// name their tree and ignore this. An action still only accepts
    /// references from its own session's tree.
    pub uia: Option<bool>,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
pub struct ClickArgs {
    /// Session id returned by connect.
    pub session: String,
    /// Element reference. Use the "ref" a tool in this session gave you
    /// ("uia:e15" or "visual:e33"): it names the tree it came from, and a
    /// session only accepts its own tree's references — the other tree's are
    /// refused, never matched to something that looks similar. A durable key
    /// or "uia:<RuntimeId>" also works. A bare "eN" is read against this
    /// session's tree.
    pub element: String,
    /// Mouse button: 0 left (default), 1 right, 2 middle.
    pub button: Option<i32>,
    /// Force a real mouse click even when the element supports Invoke. Needed
    /// for controls that behave differently under synthetic input.
    pub synthetic: Option<bool>,
    /// UIA tree view used to resolve the element.
    pub view: Option<String>,
    /// Which tree a bare "eN" id should be read against: true for the UIA
    /// tree, false for the visual tree. Defaults to this session's mode.
    /// Qualified references ("uia:e15", "visual:e33") and durable keys already
    /// name their tree and ignore this. An action still only accepts
    /// references from its own session's tree.
    pub uia: Option<bool>,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
pub struct SetValueArgs {
    /// Session id returned by connect.
    pub session: String,
    /// Element reference. Use the "ref" a tool in this session gave you
    /// ("uia:e15" or "visual:e33"): it names the tree it came from, and a
    /// session only accepts its own tree's references — the other tree's are
    /// refused, never matched to something that looks similar. A durable key
    /// or "uia:<RuntimeId>" also works. A bare "eN" is read against this
    /// session's tree.
    pub element: String,
    /// The value to set. Replaces the element's current value outright.
    pub text: String,
    /// UIA tree view used to resolve the element.
    pub view: Option<String>,
    /// Which tree a bare "eN" id should be read against: true for the UIA
    /// tree, false for the visual tree. Defaults to this session's mode.
    /// Qualified references ("uia:e15", "visual:e33") and durable keys already
    /// name their tree and ignore this. An action still only accepts
    /// references from its own session's tree.
    pub uia: Option<bool>,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
pub struct TypeTextArgs {
    /// Session id returned by connect.
    pub session: String,
    /// Text to type, character by character, into whatever has focus.
    pub text: String,
    /// Focus this element first. Omit to type into the current focus.
    pub element: Option<String>,
    /// UIA tree view used to resolve the element.
    pub view: Option<String>,
    /// Which tree a bare "eN" id should be read against: true for the UIA
    /// tree, false for the visual tree. Defaults to this session's mode.
    /// Qualified references ("uia:e15", "visual:e33") and durable keys already
    /// name their tree and ignore this. An action still only accepts
    /// references from its own session's tree.
    pub uia: Option<bool>,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
pub struct PressKeyArgs {
    /// Session id returned by connect.
    pub session: String,
    /// Key or chord, e.g. "Enter", "Ctrl+S", "Ctrl+Shift+Tab", "F5".
    pub text: String,
    /// Focus this element first. Omit to send to the current focus.
    pub element: Option<String>,
    /// UIA tree view used to resolve the element.
    pub view: Option<String>,
    /// Which tree a bare "eN" id should be read against: true for the UIA
    /// tree, false for the visual tree. Defaults to this session's mode.
    /// Qualified references ("uia:e15", "visual:e33") and durable keys already
    /// name their tree and ignore this. An action still only accepts
    /// references from its own session's tree.
    pub uia: Option<bool>,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
pub struct ScrollArgs {
    /// Session id returned by connect.
    pub session: String,
    /// Element reference. Use the "ref" a tool in this session gave you
    /// ("uia:e15" or "visual:e33"): it names the tree it came from, and a
    /// session only accepts its own tree's references — the other tree's are
    /// refused, never matched to something that looks similar. A durable key
    /// or "uia:<RuntimeId>" also works. A bare "eN" is read against this
    /// session's tree.
    pub element: String,
    /// Direction: "up", "down", "left", or "right". Defaults to "down".
    pub direction: Option<String>,
    /// Number of scroll increments. Defaults to 1.
    pub amount: Option<i32>,
    /// UIA tree view used to resolve the element.
    pub view: Option<String>,
    /// Which tree a bare "eN" id should be read against: true for the UIA
    /// tree, false for the visual tree. Defaults to this session's mode.
    /// Qualified references ("uia:e15", "visual:e33") and durable keys already
    /// name their tree and ignore this. An action still only accepts
    /// references from its own session's tree.
    pub uia: Option<bool>,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
pub struct SelectArgs {
    /// Session id returned by connect.
    pub session: String,
    /// Element reference. Use the "ref" a tool in this session gave you
    /// ("uia:e15" or "visual:e33"): it names the tree it came from, and a
    /// session only accepts its own tree's references — the other tree's are
    /// refused, never matched to something that looks similar. A durable key
    /// or "uia:<RuntimeId>" also works. A bare "eN" is read against this
    /// session's tree.
    pub element: String,
    /// "replace" (default) clears any other selection, "add" extends it,
    /// "remove" deselects just this element.
    pub mode: Option<String>,
    /// UIA tree view used to resolve the element.
    pub view: Option<String>,
    /// Which tree a bare "eN" id should be read against: true for the UIA
    /// tree, false for the visual tree. Defaults to this session's mode.
    /// Qualified references ("uia:e15", "visual:e33") and durable keys already
    /// name their tree and ignore this. An action still only accepts
    /// references from its own session's tree.
    pub uia: Option<bool>,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
pub struct SetExpandedArgs {
    /// Session id returned by connect.
    pub session: String,
    /// Element to expand or collapse.
    pub element: String,
    /// true to expand, false to collapse.
    pub expanded: bool,
    /// UIA tree view used to resolve the element.
    pub view: Option<String>,
    /// Which tree a bare "eN" id should be read against: true for the UIA
    /// tree, false for the visual tree. Defaults to this session's mode.
    /// Qualified references ("uia:e15", "visual:e33") and durable keys already
    /// name their tree and ignore this. An action still only accepts
    /// references from its own session's tree.
    pub uia: Option<bool>,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
pub struct WindowActionArgs {
    /// Session id returned by connect.
    pub session: String,
    /// One of "minimize", "maximize", "restore", "close".
    pub action: String,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
pub struct WaitForArgs {
    /// Session id returned by connect.
    pub session: String,
    /// Element reference. Use the "ref" a tool in this session gave you
    /// ("uia:e15" or "visual:e33"): it names the tree it came from, and a
    /// session only accepts its own tree's references — the other tree's are
    /// refused, never matched to something that looks similar. A durable key
    /// or "uia:<RuntimeId>" also works. A bare "eN" is read against this
    /// session's tree.
    pub element: String,
    /// Wait for the element to disappear instead of for a property value.
    pub gone: Option<bool>,
    /// Property to watch, e.g. "IsEnabled", "Toggle.ToggleState", "Value.Value".
    #[serde(rename = "waitProperty")]
    pub wait_property: Option<String>,
    /// Value that property must reach.
    #[serde(rename = "waitValue")]
    pub wait_value: Option<String>,
    /// How long to wait, in milliseconds. Defaults to 5000.
    #[serde(rename = "timeoutMs")]
    pub timeout_ms: Option<i32>,
    /// UIA tree view used to resolve the element.
    pub view: Option<String>,
    /// Which tree a bare "eN" id should be read against: true for the UIA
    /// tree, false for the visual tree. Defaults to this session's mode.
    /// Qualified references ("uia:e15", "visual:e33") and durable keys already
    /// name their tree and ignore this. An action still only accepts
    /// references from its own session's tree.
    pub uia: Option<bool>,
}

const RESOURCE_PREFIX: &str = "lvt://session/";
const VISUAL_RESOURCE_SUFFIX: &str = "/visual-tree";
const UIA_RESOURCE_SUFFIX: &str = "/uia-tree";
const RESOURCE_POLL_INTERVAL: Duration = Duration::from_millis(500);
const MAX_CACHED_RESOURCE_EVENTS: usize = 10_000;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum TreeMode {
    Visual,
    Uia,
}

impl TreeMode {
    fn from_session_mode(mode: &str) -> Option<Self> {
        match mode {
            "visual" => Some(Self::Visual),
            "uia" => Some(Self::Uia),
            _ => None,
        }
    }

    fn name(self) -> &'static str {
        match self {
            Self::Visual => "visual",
            Self::Uia => "uia",
        }
    }

    fn resource_suffix(self) -> &'static str {
        match self {
            Self::Visual => VISUAL_RESOURCE_SUFFIX,
            Self::Uia => UIA_RESOURCE_SUFFIX,
        }
    }

    fn changes_method(self) -> &'static str {
        match self {
            Self::Visual => "get_visual_tree_changes",
            Self::Uia => "get_uia_tree_changes",
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct SessionResource {
    session: String,
    tree: TreeMode,
}

impl SessionResource {
    fn new(session: impl Into<String>, tree: TreeMode) -> Self {
        Self {
            session: session.into(),
            tree,
        }
    }

    fn parse(uri: &str) -> Option<Self> {
        let remainder = uri.strip_prefix(RESOURCE_PREFIX)?;
        let (session, tree) = if let Some(session) = remainder.strip_suffix(VISUAL_RESOURCE_SUFFIX)
        {
            (session, TreeMode::Visual)
        } else if let Some(session) = remainder.strip_suffix(UIA_RESOURCE_SUFFIX) {
            (session, TreeMode::Uia)
        } else {
            return None;
        };
        let mut chars = session.chars();
        if chars.next() != Some('s')
            || chars.clone().next().is_none()
            || !chars.all(|ch| ch.is_ascii_digit())
        {
            return None;
        }
        Some(Self::new(session, tree))
    }

    fn uri(&self) -> String {
        format!(
            "{RESOURCE_PREFIX}{}{}",
            self.session,
            self.tree.resource_suffix()
        )
    }
}

fn session_resource(resource: &SessionResource, process_name: &str) -> Resource {
    let tree_title = match resource.tree {
        TreeMode::Visual => "Visual",
        TreeMode::Uia => "UI Automation",
    };
    Resource::new(
        resource.uri(),
        format!("{}-tree-{}", resource.tree.name(), resource.session),
    )
    .with_title(format!("{tree_title} tree changes for {process_name}"))
    .with_description(
        "A session-scoped snapshot/diff stream. Read after a resources/updated \
         notification to receive the cached tree patch.",
    )
    .with_mime_type("application/json")
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
struct TreePatch {
    tree: String,
    snapshot: bool,
    #[serde(default)]
    events: Vec<serde_json::Value>,
}

fn tree_patch_params(resource: &SessionResource, reset: bool) -> serde_json::Value {
    let mut params = json!({ "session": resource.session, "reset": reset });
    if resource.tree == TreeMode::Visual {
        params["fast"] = true.into();
    }
    params
}

async fn get_tree_patch(resource: &SessionResource, reset: bool) -> Result<TreePatch, String> {
    let result = call_lvt(
        resource.tree.changes_method(),
        tree_patch_params(resource, reset),
        false,
    )
    .await
    .map_err(|error| error.message.to_string())?;
    let response: serde_json::Value = serde_json::from_str(&result.json)
        .map_err(|error| format!("lvt returned malformed tree-change JSON: {error}"))?;
    if !result.ok {
        return Err(response
            .get("error")
            .and_then(|value| value.as_str())
            .unwrap_or("tree change polling failed")
            .to_string());
    }
    let patch: TreePatch = serde_json::from_value(response)
        .map_err(|error| format!("lvt returned an invalid tree patch: {error}"))?;
    if patch.tree != resource.tree.name() {
        return Err(format!(
            "lvt returned a '{}' patch for a '{}' resource",
            patch.tree,
            resource.tree.name()
        ));
    }
    Ok(patch)
}

#[derive(Debug, Clone)]
struct SessionDescriptor {
    session: String,
    process_name: String,
    tree: TreeMode,
}

async fn list_session_descriptors() -> Result<Vec<SessionDescriptor>, ErrorData> {
    let result = call_lvt("list_sessions", json!({}), false).await?;
    if !result.ok {
        return Err(ErrorData::internal_error(result.json, None));
    }
    let response: serde_json::Value = serde_json::from_str(&result.json).map_err(|error| {
        ErrorData::internal_error(
            format!("lvt returned malformed session JSON: {error}"),
            None,
        )
    })?;
    let mut sessions = Vec::new();
    for session in response
        .get("sessions")
        .and_then(|value| value.as_array())
        .into_iter()
        .flatten()
    {
        let Some(id) = session.get("session").and_then(|value| value.as_str()) else {
            continue;
        };
        let Some(tree) = session
            .get("mode")
            .and_then(|value| value.as_str())
            .and_then(TreeMode::from_session_mode)
        else {
            continue;
        };
        sessions.push(SessionDescriptor {
            session: id.to_string(),
            process_name: session
                .get("processName")
                .and_then(|value| value.as_str())
                .unwrap_or("application")
                .to_string(),
            tree,
        });
    }
    Ok(sessions)
}

async fn validate_resource(resource: &SessionResource) -> Result<(), ErrorData> {
    let sessions = list_session_descriptors().await?;
    match sessions
        .iter()
        .find(|session| session.session == resource.session)
    {
        Some(session) if session.tree == resource.tree => Ok(()),
        Some(session) => Err(ErrorData::invalid_params(
            format!(
                "session '{}' exposes its {} tree, not the requested {} tree",
                resource.session,
                session.tree.name(),
                resource.tree.name()
            ),
            None,
        )),
        None => Err(ErrorData::invalid_params(
            format!("unknown session '{}'", resource.session),
            None,
        )),
    }
}

#[derive(Clone)]
enum ResourceSubscriber {
    Legacy(Peer<RoleServer>),
    Modern(SubscriptionSink),
}

impl ResourceSubscriber {
    async fn notify(&self, uri: String) -> bool {
        match self {
            Self::Legacy(peer) => peer
                .notify_resource_updated(ResourceUpdatedNotificationParam::new(uri))
                .await
                .is_ok(),
            Self::Modern(sink) => sink.notify_resource_updated(uri).await.is_ok(),
        }
    }
}

struct WatcherHandle {
    generation: u64,
    cancel: watch::Sender<bool>,
}

#[derive(Default)]
struct ResourceEntry {
    initialized: bool,
    cached: Option<TreePatch>,
    watcher: Option<WatcherHandle>,
    subscribers: HashMap<u64, ResourceSubscriber>,
}

#[derive(Default)]
struct ResourceRegistryInner {
    // Modern listen and legacy subscribe share these entries so only one
    // native diff poll can consume a session/tree baseline at a time.
    entries: HashMap<String, ResourceEntry>,
    legacy_ids: HashMap<String, u64>,
    session_epochs: HashMap<String, u64>,
    next_subscriber: u64,
    next_generation: u64,
}

#[derive(Clone, Default)]
struct ResourceRegistry {
    inner: Arc<Mutex<ResourceRegistryInner>>,
}

struct RegisterResult {
    subscriber_id: u64,
    watcher: Option<(u64, watch::Receiver<bool>)>,
}

enum CacheResult {
    Stale,
    NoChange,
    Cached,
    NeedsSnapshot,
}

enum MergeResult {
    NoChange,
    Cached,
    NeedsSnapshot,
}

fn merge_cached_patch(cached: &mut Option<TreePatch>, patch: TreePatch) -> MergeResult {
    if patch.events.is_empty() && !patch.snapshot {
        return MergeResult::NoChange;
    }
    if patch.snapshot {
        *cached = Some(patch);
        return MergeResult::Cached;
    }
    if patch.events.len() > MAX_CACHED_RESOURCE_EVENTS {
        return MergeResult::NeedsSnapshot;
    }
    if let Some(current) = cached.as_mut() {
        if current.events.len().saturating_add(patch.events.len()) > MAX_CACHED_RESOURCE_EVENTS {
            // Dropping arbitrary diffs would make the remaining sequence
            // impossible to apply. The watcher replaces it with a snapshot.
            return MergeResult::NeedsSnapshot;
        }
        current.events.extend(patch.events);
    } else {
        *cached = Some(patch);
    }
    MergeResult::Cached
}

impl ResourceRegistry {
    async fn read_state(&self, resource: &SessionResource) -> (Option<TreePatch>, bool, u64) {
        let uri = resource.uri();
        let mut inner = self.inner.lock().await;
        let epoch = *inner.session_epochs.get(&resource.session).unwrap_or(&0);
        let Some(entry) = inner.entries.get_mut(&uri) else {
            return (None, false, epoch);
        };
        (entry.cached.take(), entry.initialized, epoch)
    }

    async fn session_epoch(&self, session: &str) -> u64 {
        let inner = self.inner.lock().await;
        *inner.session_epochs.get(session).unwrap_or(&0)
    }

    async fn store_initial(
        &self,
        resource: &SessionResource,
        epoch: u64,
        patch: TreePatch,
    ) -> bool {
        let mut inner = self.inner.lock().await;
        if *inner.session_epochs.get(&resource.session).unwrap_or(&0) != epoch {
            return false;
        }
        let entry = inner.entries.entry(resource.uri()).or_default();
        if let Some(watcher) = entry.watcher.take() {
            let _ = watcher.cancel.send(true);
        }
        entry.initialized = true;
        entry.cached = Some(patch);
        true
    }

    async fn mark_initialized(&self, resource: &SessionResource, epoch: u64) -> bool {
        let mut inner = self.inner.lock().await;
        if *inner.session_epochs.get(&resource.session).unwrap_or(&0) != epoch {
            return false;
        }
        inner.entries.entry(resource.uri()).or_default().initialized = true;
        true
    }

    async fn register(
        &self,
        resource: &SessionResource,
        epoch: u64,
        subscriber: ResourceSubscriber,
        legacy: bool,
    ) -> Option<RegisterResult> {
        let uri = resource.uri();
        let mut inner = self.inner.lock().await;
        if *inner.session_epochs.get(&resource.session).unwrap_or(&0) != epoch {
            return None;
        }

        inner.next_subscriber += 1;
        let subscriber_id = inner.next_subscriber;
        let previous_legacy = legacy
            .then(|| inner.legacy_ids.insert(uri.clone(), subscriber_id))
            .flatten();
        let needs_watcher = inner
            .entries
            .get(&uri)
            .is_none_or(|entry| entry.watcher.is_none());
        let watcher = if needs_watcher {
            inner.next_generation += 1;
            let generation = inner.next_generation;
            let (cancel, cancelled) = watch::channel(false);
            Some((generation, cancel, cancelled))
        } else {
            None
        };
        let entry = inner.entries.entry(uri).or_default();
        if let Some(previous) = previous_legacy {
            entry.subscribers.remove(&previous);
        }
        entry.subscribers.insert(subscriber_id, subscriber);

        let watcher = watcher.map(|(generation, cancel, cancelled)| {
            entry.watcher = Some(WatcherHandle { generation, cancel });
            (generation, cancelled)
        });
        Some(RegisterResult {
            subscriber_id,
            watcher,
        })
    }

    async fn cache_from_watcher(
        &self,
        resource: &SessionResource,
        epoch: u64,
        generation: u64,
        patch: TreePatch,
    ) -> CacheResult {
        let uri = resource.uri();
        let mut inner = self.inner.lock().await;
        if *inner.session_epochs.get(&resource.session).unwrap_or(&0) != epoch {
            return CacheResult::Stale;
        }
        let Some(entry) = inner.entries.get_mut(&uri) else {
            return CacheResult::Stale;
        };
        if entry.watcher.as_ref().map(|watcher| watcher.generation) != Some(generation)
            || entry.subscribers.is_empty()
        {
            return CacheResult::Stale;
        }
        entry.initialized = true;
        match merge_cached_patch(&mut entry.cached, patch) {
            MergeResult::NoChange => CacheResult::NoChange,
            MergeResult::Cached => CacheResult::Cached,
            MergeResult::NeedsSnapshot => CacheResult::NeedsSnapshot,
        }
    }

    async fn subscribers(
        &self,
        resource: &SessionResource,
        generation: u64,
    ) -> Vec<(u64, ResourceSubscriber)> {
        let inner = self.inner.lock().await;
        let Some(entry) = inner.entries.get(&resource.uri()) else {
            return Vec::new();
        };
        if entry.watcher.as_ref().map(|watcher| watcher.generation) != Some(generation) {
            return Vec::new();
        }
        entry
            .subscribers
            .iter()
            .map(|(id, subscriber)| (*id, subscriber.clone()))
            .collect()
    }

    async fn remove_subscribers(
        &self,
        resource: &SessionResource,
        generation: u64,
        subscriber_ids: &[u64],
    ) {
        let uri = resource.uri();
        let mut cancel = None;
        {
            let mut inner = self.inner.lock().await;
            let remove_entry = {
                let Some(entry) = inner.entries.get_mut(&uri) else {
                    return;
                };
                if entry.watcher.as_ref().map(|watcher| watcher.generation) != Some(generation) {
                    return;
                }
                for id in subscriber_ids {
                    entry.subscribers.remove(id);
                }
                if entry.subscribers.is_empty() {
                    cancel = entry.watcher.take().map(|watcher| watcher.cancel);
                    true
                } else {
                    false
                }
            };
            inner
                .legacy_ids
                .retain(|legacy_uri, id| legacy_uri != &uri || !subscriber_ids.contains(id));
            if remove_entry {
                inner.entries.remove(&uri);
            }
        }
        if let Some(cancel) = cancel {
            let _ = cancel.send(true);
        }
    }

    async fn unregister(&self, resource: &SessionResource, subscriber_id: u64) {
        let generation = {
            let inner = self.inner.lock().await;
            inner
                .entries
                .get(&resource.uri())
                .and_then(|entry| entry.watcher.as_ref())
                .map(|watcher| watcher.generation)
        };
        if let Some(generation) = generation {
            self.remove_subscribers(resource, generation, &[subscriber_id])
                .await;
        }
    }

    async fn unregister_legacy(&self, uri: &str) {
        let parsed = SessionResource::parse(uri);
        let subscriber_id = {
            let inner = self.inner.lock().await;
            inner.legacy_ids.get(uri).copied()
        };
        if let (Some(resource), Some(subscriber_id)) = (parsed, subscriber_id) {
            self.unregister(&resource, subscriber_id).await;
        }
    }

    async fn clear_resource(&self, resource: &SessionResource, epoch: u64) {
        let uri = resource.uri();
        let cancel = {
            let mut inner = self.inner.lock().await;
            if *inner.session_epochs.get(&resource.session).unwrap_or(&0) != epoch {
                return;
            }
            *inner
                .session_epochs
                .entry(resource.session.clone())
                .or_default() += 1;
            inner.legacy_ids.remove(&uri);
            inner
                .entries
                .remove(&uri)
                .and_then(|entry| entry.watcher.map(|watcher| watcher.cancel))
        };
        if let Some(cancel) = cancel {
            let _ = cancel.send(true);
        }
    }

    async fn clear_session(&self, session: &str) {
        let mut cancellations = Vec::new();
        {
            let mut inner = self.inner.lock().await;
            *inner.session_epochs.entry(session.to_string()).or_default() += 1;
            let uris = inner
                .entries
                .keys()
                .filter(|uri| {
                    SessionResource::parse(uri).is_some_and(|resource| resource.session == session)
                })
                .cloned()
                .collect::<Vec<_>>();
            for uri in uris {
                inner.legacy_ids.remove(&uri);
                if let Some(cancel) = inner
                    .entries
                    .remove(&uri)
                    .and_then(|entry| entry.watcher.map(|watcher| watcher.cancel))
                {
                    cancellations.push(cancel);
                }
            }
        }
        for cancel in cancellations {
            let _ = cancel.send(true);
        }
    }
}

async fn notify_resource_subscribers(
    registry: &ResourceRegistry,
    resource: &SessionResource,
    generation: u64,
) {
    let subscribers = registry.subscribers(resource, generation).await;
    let mut failed = Vec::new();
    let uri = resource.uri();
    for (id, subscriber) in subscribers {
        if !subscriber.notify(uri.clone()).await {
            failed.push(id);
        }
    }
    if !failed.is_empty() {
        registry
            .remove_subscribers(resource, generation, &failed)
            .await;
    }
}

async fn run_resource_watcher(
    resource: SessionResource,
    registry: ResourceRegistry,
    epoch: u64,
    generation: u64,
    mut cancelled: watch::Receiver<bool>,
) {
    let start = tokio::time::Instant::now() + RESOURCE_POLL_INTERVAL;
    let mut interval = tokio::time::interval_at(start, RESOURCE_POLL_INTERVAL);
    interval.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);
    loop {
        tokio::select! {
            changed = cancelled.changed() => {
                if changed.is_err() || *cancelled.borrow() {
                    break;
                }
            }
            _ = interval.tick() => {
                let patch = match get_tree_patch(&resource, false).await {
                    Ok(patch) => patch,
                    // Preserve the subscription across a transient target
                    // read failure. lvt_core keeps the previous diff baseline
                    // in that case, so the next successful pass remains
                    // coherent instead of requiring a resubscribe.
                    Err(_) => continue,
                };
                match registry
                    .cache_from_watcher(&resource, epoch, generation, patch)
                    .await
                {
                    CacheResult::Stale => break,
                    CacheResult::NoChange => {}
                    CacheResult::Cached => {
                        notify_resource_subscribers(&registry, &resource, generation).await;
                    }
                    CacheResult::NeedsSnapshot => {
                        let snapshot = match get_tree_patch(&resource, true).await {
                            Ok(snapshot) => snapshot,
                            Err(_) => continue,
                        };
                        match registry
                            .cache_from_watcher(&resource, epoch, generation, snapshot)
                            .await
                        {
                            CacheResult::Cached => {
                                notify_resource_subscribers(
                                    &registry,
                                    &resource,
                                    generation,
                                ).await;
                            }
                            CacheResult::Stale => break,
                            CacheResult::NoChange | CacheResult::NeedsSnapshot => {}
                        }
                    }
                }
            }
        }
    }
}

// --- server -------------------------------------------------------------

#[derive(Clone)]
pub struct LvtServer {
    tool_router: ToolRouter<LvtServer>,
    allow_input: bool,
    resources: ResourceRegistry,
}

/// Forwards to lvt and turns the result into MCP content.
///
/// The FFI call runs on the blocking pool, not on a worker thread. That is not
/// a refinement: `ffi::call` is synchronous and can take many seconds — a UIA
/// walk of a busy app, or a `wait_for` blocking on its deadline — and it may
/// also park on lvt's per-target mutex. rmcp dispatches every request as its
/// own task, and the task that reads stdin lives on the same runtime, so two
/// concurrent blocking calls on a two-worker runtime starve the reader and the
/// server stops accepting requests entirely. Measured before this: one
/// in-flight `wait_for` left an unrelated `list_apps` at 0.07s, two pushed it
/// to 11.14s.
///
/// The blocking pool is separate and grows on demand, so calls queue on lvt's
/// own locks — which is what those locks are for — instead of on the runtime.
///
/// lvt's failures come back as `isError` tool results rather than protocol
/// errors, because they are information the model should act on — "no element
/// matched", "the window closed" — not transport faults. Only the FFI boundary
/// failing produces a real `ErrorData`.
async fn call_lvt(
    method: &str,
    params: serde_json::Value,
    allow_input: bool,
) -> Result<ffi::ApiResult, ErrorData> {
    let method = method.to_string();
    tokio::task::spawn_blocking(move || ffi::call(&method, &params, allow_input))
        .await
        .map_err(|e| ErrorData::internal_error(format!("lvt call did not complete: {e}"), None))?
        .map_err(|message| ErrorData::internal_error(message, None))
}

/// Build a tool result that carries the same answer twice: as the JSON text a
/// client can show or log, and as `structuredContent` it can consume as data.
///
/// The spec's rule is that a tool returning structured content should also
/// return the serialised JSON in a text block, for clients that predate the
/// field — so this is not a choice between the two. Without the structured
/// copy, every caller has to re-parse a string we already had as JSON, and a
/// client that validates against `outputSchema` has nothing to validate.
///
/// `structuredContent` must be a JSON *object*: the spec models it as one, and
/// a schema cannot describe a bare array or scalar. Every lvt method returns an
/// object, so this is a fact about our shapes rather than a restriction, but it
/// is checked rather than assumed — if lvt ever returned something else, the
/// text block still carries it instead of the call failing.
fn tool_result(json_text: String, ok: bool) -> CallToolResult {
    let structured = serde_json::from_str::<serde_json::Value>(&json_text)
        .ok()
        .filter(|value| value.is_object());
    let content = vec![ContentBlock::text(json_text)];
    let mut result = if ok {
        CallToolResult::success(content)
    } else {
        CallToolResult::error(content)
    };
    result.structured_content = structured;
    result
}

async fn forward(
    method: &str,
    params: serde_json::Value,
    allow_input: bool,
) -> Result<CallToolResult, ErrorData> {
    let result = call_lvt(method, params, allow_input).await?;
    Ok(tool_result(result.json, result.ok))
}

/// Drops `null` members so lvt sees an absent option as absent, and applies its
/// own default, rather than receiving an explicit null it would have to treat
/// as a value.
fn compact(mut value: serde_json::Value) -> serde_json::Value {
    if let Some(map) = value.as_object_mut() {
        map.retain(|_, v| !v.is_null());
    }
    value
}

#[tool_router(router = inspect_router, vis = "pub")]
impl LvtServer {
    #[tool(
        description = "List top-level application windows on this desktop, with their process \
                       name, pid, window handle and title. Start here when you do not already \
                       know which window to target.",
        output_schema = crate::schema::apps(),
        annotations(read_only_hint = true, open_world_hint = true)
    )]
    async fn list_apps(
        &self,
        Parameters(a): Parameters<ListAppsArgs>,
    ) -> Result<CallToolResult, ErrorData> {
        forward(
            "list_apps",
            compact(json!({ "name": a.name, "title": a.title })),
            self.allow_input,
        )
        .await
    }

    #[tool(
        description = "Connect to an application window and open a session. Returns a session id \
                       that every other tool needs, plus the window's pid, architecture and \
                       detected UI frameworks. Identify the window by hwnd (unambiguous), pid, \
                       process name, or title substring. Pass mode 'visual' to drive the app by \
                       real mouse and keyboard input instead of UI Automation patterns.",
        output_schema = crate::schema::session(),
        annotations(read_only_hint = true, open_world_hint = true)
    )]
    async fn connect(
        &self,
        Parameters(a): Parameters<ConnectArgs>,
    ) -> Result<CallToolResult, ErrorData> {
        forward(
            "connect",
            compact(
                json!({ "name": a.name, "title": a.title, "pid": a.pid, "hwnd": a.hwnd,
                            "mode": a.mode }),
            ),
            self.allow_input,
        )
        .await
    }

    #[tool(
        description = "Close a session opened by connect and release its resources.",
        output_schema = crate::schema::disconnected(),
        annotations(read_only_hint = true, idempotent_hint = true, open_world_hint = true)
    )]
    async fn disconnect(
        &self,
        Parameters(a): Parameters<SessionArgs>,
    ) -> Result<CallToolResult, ErrorData> {
        let session = a.session;
        let result = call_lvt(
            "disconnect",
            json!({ "session": session }),
            self.allow_input,
        )
        .await;
        self.resources.clear_session(&session).await;
        let result = result?;
        Ok(tool_result(result.json, result.ok))
    }

    #[tool(
        description = "Get the UI Automation tree: AutomationIds, control types, names, states \
                       and supported patterns. This is the tree to automate against in the \
                       default 'uia' session mode — its identifiers are stable, its elements \
                       expose the patterns the action tools drive, and it works against any \
                       process regardless of architecture.",
        output_schema = crate::schema::uia_tree(),
        annotations(read_only_hint = true, open_world_hint = true)
    )]
    async fn get_uia_tree(
        &self,
        Parameters(a): Parameters<TreeArgs>,
    ) -> Result<CallToolResult, ErrorData> {
        forward("get_uia_tree", tree_params(a), self.allow_input).await
    }

    #[tool(
        description = "Return incremental UI Automation tree changes for this session. The first \
                       call returns snapshot-added events; later calls return added, removed, and \
                       changed events from the previous call. This is also the patch format used \
                       by a UIA session's subscribable tree resource.",
        output_schema = crate::schema::uia_tree_changes(),
        annotations(read_only_hint = true, open_world_hint = true)
    )]
    async fn get_uia_tree_changes(
        &self,
        Parameters(a): Parameters<UiaTreeChangesArgs>,
    ) -> Result<CallToolResult, ErrorData> {
        forward(
            "get_uia_tree_changes",
            compact(json!({
                "session": a.session,
                "view": a.view,
                "properties": a.properties,
                "timeoutMs": a.timeout_ms,
            })),
            self.allow_input,
        )
        .await
    }

    #[tool(
        description = "Get the framework-native visual tree (Win32 windows, XAML/WPF/WinForms/\
                       Avalonia/Chromium elements). Use this to understand how a UI is built — \
                       it shows implementation structure the UIA tree hides. Its elements are a \
                       different, finer-grained set than the UIA tree's, and its references only \
                       work in a session connected with mode 'visual'. Pass correlate:true to see \
                       which of these elements UI Automation exposes and which it does not. Pass \
                       fast:true on a large XAML/WinUI3 tree to trade the full per-element \
                       property set for a much quicker walk (still reports bounds, Text, Content, \
                       and basic state — enough to browse or search by, not exhaustive). \
                       Requires lvt and the target to share an architecture.",
        output_schema = crate::schema::visual_tree(),
        annotations(read_only_hint = true, open_world_hint = true)
    )]
    async fn get_visual_tree(
        &self,
        Parameters(a): Parameters<VisualTreeArgs>,
    ) -> Result<CallToolResult, ErrorData> {
        forward("get_visual_tree", visual_tree_params(a), self.allow_input).await
    }

    #[tool(
        description = "Return incremental framework-native visual-tree changes for this session. \
                       The first call returns snapshot-added events; later calls return diffs \
                       from the previous call, retaining watch-style patch semantics without a \
                       separate watch subprocess.",
        output_schema = crate::schema::visual_tree_changes(),
        annotations(read_only_hint = true, open_world_hint = true)
    )]
    async fn get_visual_tree_changes(
        &self,
        Parameters(a): Parameters<VisualTreeChangesArgs>,
    ) -> Result<CallToolResult, ErrorData> {
        forward(
            "get_visual_tree_changes",
            compact(json!({ "session": a.session, "fast": a.fast })),
            self.allow_input,
        )
        .await
    }

    #[tool(
        description = "Get the complete XAML/WinUI3 dependency-property chain for one visual \
                       element, including property indexes, value types, source, override state, \
                       and metadata bits needed to decide whether each value is writable.",
        output_schema = crate::schema::visual_properties(),
        annotations(read_only_hint = true, open_world_hint = true)
    )]
    async fn get_visual_properties(
        &self,
        Parameters(a): Parameters<VisualPropertiesArgs>,
    ) -> Result<CallToolResult, ErrorData> {
        forward(
            "get_visual_properties",
            json!({ "session": a.session, "key": a.key }),
            self.allow_input,
        )
        .await
    }

    #[tool(
        description = "List the UI frameworks detected in the connected application, with versions.",
        output_schema = crate::schema::frameworks(),
        annotations(read_only_hint = true, open_world_hint = true)
    )]
    async fn get_frameworks(
        &self,
        Parameters(a): Parameters<SessionArgs>,
    ) -> Result<CallToolResult, ErrorData> {
        forward(
            "get_frameworks",
            json!({ "session": a.session }),
            self.allow_input,
        )
        .await
    }

    #[tool(
        description = "Find elements by AutomationId, name, control type, or supported pattern. \
                       Much cheaper than fetching a whole tree when you know what you are looking \
                       for. Returns each match's element id, which the action tools accept.",
        output_schema = crate::schema::elements(),
        annotations(read_only_hint = true, open_world_hint = true)
    )]
    async fn find_elements(
        &self,
        Parameters(a): Parameters<FindArgs>,
    ) -> Result<CallToolResult, ErrorData> {
        forward(
            "find_elements",
            compact(json!({
                "session": a.session,
                "automationId": a.automation_id,
                "name": a.name,
                "type": a.control_type,
                "pattern": a.pattern,
                "limit": a.limit,
                "uia": a.uia,
                "view": a.view,
                "timeoutMs": a.timeout_ms,
            })),
            self.allow_input,
        )
        .await
    }

    #[tool(
        description = "Read one element's full set of properties, or a chosen subset. Use this to \
                       check state — whether a checkbox is on, what a text box currently holds, \
                       which patterns a control supports.",
        output_schema = crate::schema::element_properties(),
        annotations(read_only_hint = true, open_world_hint = true)
    )]
    async fn get_element_properties(
        &self,
        Parameters(a): Parameters<ElementPropertiesArgs>,
    ) -> Result<CallToolResult, ErrorData> {
        forward(
            "get_element_properties",
            compact(json!({
                "session": a.session,
                "element": a.element,
                "properties": a.properties,
                "uia": a.uia,
                "view": a.view,
                "timeoutMs": a.timeout_ms,
            })),
            self.allow_input,
        )
        .await
    }

    #[tool(
        description = "Capture a PNG of the connected window with element ids drawn on it. \
                       The ids are the same ones find_elements and the action tools use, so you \
                       can read one off the image and act on it. Returns the image inline unless \
                       a path is given. Use it to see a UI whose tree is ambiguous, or to confirm \
                       an action had the effect you expected.",
        output_schema = crate::schema::screenshot(),
        annotations(read_only_hint = true, open_world_hint = true)
    )]
    async fn screenshot(
        &self,
        Parameters(a): Parameters<ScreenshotArgs>,
    ) -> Result<CallToolResult, ErrorData> {
        let params = compact(json!({
            "session": a.session,
            "path": a.path,
            "element": a.element,
            "uia": a.uia,
            "timeoutMs": a.timeout_ms,
        }));
        let result = call_lvt("screenshot", params, self.allow_input).await?;
        if !result.ok {
            return Ok(tool_result(result.json, false));
        }

        // An inline capture comes back as base64, which becomes an image
        // content block. The base64 is stripped from the JSON summary first —
        // repeating a megabyte of it as text would be useless and expensive,
        // and it would bloat the structured copy for no gain, since the image
        // is already carried as an image block.
        let mut parsed: serde_json::Value = serde_json::from_str(&result.json).map_err(|e| {
            ErrorData::internal_error(format!("lvt returned malformed JSON: {e}"), None)
        })?;
        let image = parsed
            .as_object_mut()
            .and_then(|m| m.remove("imageBase64"))
            .and_then(|v| v.as_str().map(str::to_owned));

        let mut content = vec![ContentBlock::text(parsed.to_string())];
        if let Some(data) = image {
            content.push(ContentBlock::image(data, "image/png"));
        }
        let mut out = CallToolResult::success(content);
        out.structured_content = parsed.is_object().then_some(parsed);
        Ok(out)
    }

    #[tool(
        description = "Find the element at a screen coordinate. Returns the smallest element \
                       covering the point — what a click there would hit — plus the ids of its \
                       ancestors. Useful for turning something you saw in a screenshot into an \
                       element id you can act on.",
        output_schema = crate::schema::hit_test(),
        annotations(read_only_hint = true, open_world_hint = true)
    )]
    async fn hit_test(
        &self,
        Parameters(a): Parameters<HitTestArgs>,
    ) -> Result<CallToolResult, ErrorData> {
        forward(
            "hit_test",
            compact(json!({
                "session": a.session, "x": a.x, "y": a.y, "uia": a.uia, "view": a.view,
                "timeoutMs": a.timeout_ms,
            })),
            self.allow_input,
        )
        .await
    }

    #[tool(
        description = "Block until an element reaches a property value, or until it disappears. \
                       Use this instead of guessing at delays after an action that starts work.",
        output_schema = crate::schema::action(),
        annotations(read_only_hint = true, idempotent_hint = true, open_world_hint = true)
    )]
    async fn wait_for(
        &self,
        Parameters(a): Parameters<WaitForArgs>,
    ) -> Result<CallToolResult, ErrorData> {
        let method = if a.gone.unwrap_or(false) {
            "wait_gone"
        } else {
            "wait_for"
        };
        forward(
            method,
            compact(json!({
                "session": a.session,
                "element": a.element,
                "waitProperty": a.wait_property,
                "waitValue": a.wait_value,
                "timeoutMs": a.timeout_ms,
                "uia": a.uia, "view": a.view,
            })),
            self.allow_input,
        )
        .await
    }
}

fn tree_params(a: TreeArgs) -> serde_json::Value {
    compact(json!({
        "session": a.session,
        "element": a.element,
        "depth": a.depth,
        "view": a.view,
        "properties": a.properties,
        "timeoutMs": a.timeout_ms,
    }))
}

fn visual_tree_params(a: VisualTreeArgs) -> serde_json::Value {
    compact(json!({
        "session": a.session,
        "element": a.element,
        "depth": a.depth,
        "view": a.view,
        "properties": a.properties,
        "timeoutMs": a.timeout_ms,
        "correlate": a.correlate,
        "fast": a.fast,
    }))
}

#[tool_router(router = input_router, vis = "pub")]
impl LvtServer {
    #[tool(
        description = "Click an element. Uses the element's Invoke pattern when it has one, \
                       falling back to a real mouse click at its centre. Set synthetic to force \
                       the mouse path.",
        output_schema = crate::schema::action(),
        annotations(destructive_hint = true, open_world_hint = true)
    )]
    async fn click(
        &self,
        Parameters(a): Parameters<ClickArgs>,
    ) -> Result<CallToolResult, ErrorData> {
        forward(
            "click",
            compact(json!({
                "session": a.session,
                "element": a.element,
                "button": a.button,
                "synthetic": a.synthetic,
                "uia": a.uia, "view": a.view,
            })),
            self.allow_input,
        )
        .await
    }

    #[tool(
        description = "Invoke an element's default action via its UIA Invoke pattern, without \
                       moving the mouse. Prefer this over click when the element supports it.",
        output_schema = crate::schema::action(),
        annotations(destructive_hint = true, open_world_hint = true)
    )]
    async fn invoke(
        &self,
        Parameters(a): Parameters<ElementArgs>,
    ) -> Result<CallToolResult, ErrorData> {
        forward("invoke", element_params(a), self.allow_input).await
    }

    #[tool(
        description = "Toggle a checkbox or other togglable control to its next state.",
        output_schema = crate::schema::action(),
        annotations(destructive_hint = true, open_world_hint = true)
    )]
    async fn toggle(
        &self,
        Parameters(a): Parameters<ElementArgs>,
    ) -> Result<CallToolResult, ErrorData> {
        forward("toggle", element_params(a), self.allow_input).await
    }

    #[tool(
        description = "Set an element's value outright — the reliable way to fill a text box or \
                       move a slider. Prefer this over type_text, which depends on focus.",
        output_schema = crate::schema::action(),
        annotations(destructive_hint = true, idempotent_hint = true, open_world_hint = true)
    )]
    async fn set_value(
        &self,
        Parameters(a): Parameters<SetValueArgs>,
    ) -> Result<CallToolResult, ErrorData> {
        forward(
            "set_value",
            compact(json!({
                "session": a.session, "element": a.element, "text": a.text, "uia": a.uia, "view": a.view,
            })), self.allow_input).await
    }

    #[tool(
        description = "Set one writable scalar XAML/WinUI3 dependency property using the \
                       property index and value type returned by get_visual_properties. The \
                       value is converted by xamlOM before being applied.",
        output_schema = crate::schema::visual_property_mutation(),
        annotations(destructive_hint = true, idempotent_hint = true, open_world_hint = true)
    )]
    async fn set_visual_property(
        &self,
        Parameters(a): Parameters<SetVisualPropertyArgs>,
    ) -> Result<CallToolResult, ErrorData> {
        forward(
            "set_visual_property",
            json!({
                "session": a.session,
                "key": a.key,
                "propertyIndex": a.property_index,
                "valueType": a.value_type,
                "value": a.value,
            }),
            self.allow_input,
        )
        .await
    }

    #[tool(
        description = "Clear one XAML/WinUI3 dependency property's local value so its inherited, \
                       style, or default value becomes effective again. Use the property index \
                       returned by get_visual_properties.",
        output_schema = crate::schema::visual_property_mutation(),
        annotations(destructive_hint = true, idempotent_hint = true, open_world_hint = true)
    )]
    async fn clear_visual_property(
        &self,
        Parameters(a): Parameters<ClearVisualPropertyArgs>,
    ) -> Result<CallToolResult, ErrorData> {
        forward(
            "clear_visual_property",
            json!({
                "session": a.session,
                "key": a.key,
                "propertyIndex": a.property_index,
            }),
            self.allow_input,
        )
        .await
    }

    #[tool(
        description = "Expand or collapse a tree item, combo box, or other expandable control.",
        output_schema = crate::schema::action(),
        annotations(destructive_hint = false, idempotent_hint = true, open_world_hint = true)
    )]
    async fn set_expanded(
        &self,
        Parameters(a): Parameters<SetExpandedArgs>,
    ) -> Result<CallToolResult, ErrorData> {
        let method = if a.expanded { "expand" } else { "collapse" };
        forward(
            method,
            compact(
                json!({ "session": a.session, "element": a.element, "uia": a.uia, "view": a.view }),
            ),
            self.allow_input,
        )
        .await
    }

    #[tool(
        description = "Select a list item, tab, or similar. mode 'replace' clears any other \
                       selection, 'add' extends a multi-select, 'remove' deselects.",
        output_schema = crate::schema::action(),
        annotations(destructive_hint = true, idempotent_hint = true, open_world_hint = true)
    )]
    async fn select(
        &self,
        Parameters(a): Parameters<SelectArgs>,
    ) -> Result<CallToolResult, ErrorData> {
        let method = match a.mode.as_deref().unwrap_or("replace") {
            "add" => "add_to_selection",
            "remove" => "remove_from_selection",
            "replace" => "select",
            other => {
                return Ok(tool_result(
                    json!({
                        "ok": false,
                        "error": format!("mode must be replace, add or remove, not '{other}'"),
                    })
                    .to_string(),
                    false,
                ))
            }
        };
        forward(
            method,
            compact(
                json!({ "session": a.session, "element": a.element, "uia": a.uia, "view": a.view }),
            ),
            self.allow_input,
        )
        .await
    }

    #[tool(
        description = "Give an element keyboard focus.",
        output_schema = crate::schema::action(),
        annotations(destructive_hint = false, idempotent_hint = true, open_world_hint = true)
    )]
    async fn focus(
        &self,
        Parameters(a): Parameters<ElementArgs>,
    ) -> Result<CallToolResult, ErrorData> {
        forward("focus", element_params(a), self.allow_input).await
    }

    #[tool(
        description = "Select all of a text control's contents.",
        output_schema = crate::schema::action(),
        annotations(destructive_hint = false, idempotent_hint = true, open_world_hint = true)
    )]
    async fn select_text(
        &self,
        Parameters(a): Parameters<ElementArgs>,
    ) -> Result<CallToolResult, ErrorData> {
        forward("select_text", element_params(a), self.allow_input).await
    }

    #[tool(
        description = "Scroll an element. Uses its Scroll pattern when it has one, falling back \
                       to mouse wheel input over the element.",
        output_schema = crate::schema::action(),
        annotations(destructive_hint = false, open_world_hint = true)
    )]
    async fn scroll(
        &self,
        Parameters(a): Parameters<ScrollArgs>,
    ) -> Result<CallToolResult, ErrorData> {
        forward(
            "scroll",
            compact(json!({
                "session": a.session,
                "element": a.element,
                "direction": a.direction,
                "amount": a.amount,
                "uia": a.uia, "view": a.view,
            })),
            self.allow_input,
        )
        .await
    }

    #[tool(
        description = "Type text as keystrokes. This goes to whatever has focus, so pass an \
                       element to focus first. set_value is more reliable where it works; use \
                       this when the app must see real key events.",
        output_schema = crate::schema::action(),
        annotations(destructive_hint = true, open_world_hint = true)
    )]
    async fn type_text(
        &self,
        Parameters(a): Parameters<TypeTextArgs>,
    ) -> Result<CallToolResult, ErrorData> {
        forward(
            "type_text",
            compact(json!({
                "session": a.session, "element": a.element, "text": a.text, "uia": a.uia, "view": a.view,
            })), self.allow_input).await
    }

    #[tool(
        description = "Press a key or chord, e.g. \"Enter\", \"Ctrl+S\", \"Alt+F4\", \"F5\". \
                       Use this for shortcuts and for keys that have no element to click.",
        output_schema = crate::schema::action(),
        annotations(destructive_hint = true, open_world_hint = true)
    )]
    async fn press_key(
        &self,
        Parameters(a): Parameters<PressKeyArgs>,
    ) -> Result<CallToolResult, ErrorData> {
        forward(
            "press_key",
            compact(json!({
                "session": a.session, "element": a.element, "text": a.text, "uia": a.uia, "view": a.view,
            })), self.allow_input).await
    }

    #[tool(
        description = "Minimize, maximize, restore, or close the connected window.",
        output_schema = crate::schema::action(),
        annotations(destructive_hint = true, open_world_hint = true)
    )]
    async fn window_action(
        &self,
        Parameters(a): Parameters<WindowActionArgs>,
    ) -> Result<CallToolResult, ErrorData> {
        let method = match a.action.as_str() {
            "minimize" => "minimize_window",
            "maximize" => "maximize_window",
            "restore" => "restore_window",
            "close" => "close_window",
            other => {
                return Ok(tool_result(
                    json!({
                        "ok": false,
                        "error": format!(
                            "action must be minimize, maximize, restore or close, not '{other}'"
                        ),
                    })
                    .to_string(),
                    false,
                ))
            }
        };
        forward(method, json!({ "session": a.session }), self.allow_input).await
    }
}

fn element_params(a: ElementArgs) -> serde_json::Value {
    compact(json!({ "session": a.session, "element": a.element, "uia": a.uia, "view": a.view }))
}

impl LvtServer {
    pub fn new(allow_input: bool) -> Self {
        let mut router = Self::inspect_router();
        if allow_input {
            router += Self::input_router();
        }
        Self {
            tool_router: router,
            allow_input,
            resources: ResourceRegistry::default(),
        }
    }

    /// Tool names this server is currently exposing, sorted. Exists so tests
    /// can assert the `--allow-input` gate without standing up a transport.
    #[cfg(test)]
    pub fn tool_names(&self) -> Vec<String> {
        let mut names: Vec<String> = self
            .tool_router
            .list_all()
            .into_iter()
            .map(|t| t.name.to_string())
            .collect();
        names.sort();
        names
    }
}

#[tool_handler(router = self.tool_router)]
#[allow(deprecated)]
impl ServerHandler for LvtServer {
    fn list_resources(
        &self,
        _request: Option<PaginatedRequestParams>,
        _context: RequestContext<RoleServer>,
    ) -> impl std::future::Future<Output = Result<ListResourcesResult, ErrorData>> + Send + '_ {
        async move {
            let resources = list_session_descriptors()
                .await?
                .into_iter()
                .map(|session| {
                    session_resource(
                        &SessionResource::new(session.session, session.tree),
                        &session.process_name,
                    )
                })
                .collect();
            Ok(ListResourcesResult::with_all_items(resources))
        }
    }

    fn read_resource(
        &self,
        request: ReadResourceRequestParams,
        _context: RequestContext<RoleServer>,
    ) -> impl std::future::Future<Output = Result<ReadResourceResponse, ErrorData>> + Send + '_
    {
        let registry = self.resources.clone();
        async move {
            let resource = SessionResource::parse(&request.uri).ok_or_else(|| {
                ErrorData::invalid_params(
                    format!("unknown lvt tree resource '{}'", request.uri),
                    None,
                )
            })?;
            let (cached, initialized, epoch) = registry.read_state(&resource).await;
            let patch = if let Some(cached) = cached {
                cached
            } else {
                if let Err(error) = validate_resource(&resource).await {
                    return Err(error);
                }
                match get_tree_patch(&resource, !initialized).await {
                    Ok(patch) => {
                        if !registry.mark_initialized(&resource, epoch).await {
                            return Err(ErrorData::invalid_params(
                                format!("session '{}' was disconnected", resource.session),
                                None,
                            ));
                        }
                        patch
                    }
                    Err(error) => {
                        registry.clear_resource(&resource, epoch).await;
                        return Err(ErrorData::internal_error(error, None));
                    }
                }
            };
            let text = serde_json::to_string(&patch).map_err(|error| {
                ErrorData::internal_error(
                    format!("could not serialize the cached tree patch: {error}"),
                    None,
                )
            })?;
            Ok(
                ReadResourceResult::new(vec![ResourceContents::TextResourceContents {
                    uri: request.uri,
                    mime_type: Some("application/json".to_string()),
                    text,
                    meta: None,
                }])
                .into(),
            )
        }
    }

    fn accepted_subscription_filter(
        &self,
        requested: &SubscriptionFilter,
    ) -> Option<SubscriptionFilter> {
        let uris = requested
            .resource_subscriptions
            .as_ref()
            .into_iter()
            .flatten()
            .filter(|uri| SessionResource::parse(uri).is_some())
            .cloned()
            .collect::<Vec<_>>();
        Some(
            SubscriptionFilter::builder()
                .resource_subscriptions(uris)
                .build(),
        )
    }

    fn listen(
        &self,
        context: SubscriptionContext,
    ) -> impl std::future::Future<Output = Result<(), ErrorData>> + Send + '_ {
        let registry = self.resources.clone();
        async move {
            let uris = context
                .accepted()
                .resource_subscriptions
                .clone()
                .unwrap_or_default();
            let sink = context.sink().clone();
            let mut registered = Vec::new();
            for uri in uris {
                let Some(resource) = SessionResource::parse(&uri) else {
                    continue;
                };
                if let Err(error) = validate_resource(&resource).await {
                    for (resource, subscriber_id) in registered {
                        registry.unregister(&resource, subscriber_id).await;
                    }
                    return Err(error);
                }
                let epoch = registry.session_epoch(&resource.session).await;
                let snapshot = match get_tree_patch(&resource, true).await {
                    Ok(snapshot) => snapshot,
                    Err(error) => {
                        registry.clear_resource(&resource, epoch).await;
                        for (resource, subscriber_id) in registered {
                            registry.unregister(&resource, subscriber_id).await;
                        }
                        return Err(ErrorData::internal_error(error, None));
                    }
                };
                if !registry.store_initial(&resource, epoch, snapshot).await {
                    for (resource, subscriber_id) in registered {
                        registry.unregister(&resource, subscriber_id).await;
                    }
                    return Err(ErrorData::invalid_params(
                        format!("session '{}' was disconnected", resource.session),
                        None,
                    ));
                }
                let Some(result) = registry
                    .register(
                        &resource,
                        epoch,
                        ResourceSubscriber::Modern(sink.clone()),
                        false,
                    )
                    .await
                else {
                    for (resource, subscriber_id) in registered {
                        registry.unregister(&resource, subscriber_id).await;
                    }
                    return Err(ErrorData::invalid_params(
                        format!("session '{}' was disconnected", resource.session),
                        None,
                    ));
                };
                if let Some((generation, cancelled)) = result.watcher {
                    tokio::spawn(run_resource_watcher(
                        resource.clone(),
                        registry.clone(),
                        epoch,
                        generation,
                        cancelled,
                    ));
                }
                registered.push((resource, result.subscriber_id));
                if sink.notify_resource_updated(uri).await.is_err() {
                    for (resource, subscriber_id) in registered.drain(..) {
                        registry.unregister(&resource, subscriber_id).await;
                    }
                    return Ok(());
                }
            }
            context.cancelled().await;
            for (resource, subscriber_id) in registered {
                registry.unregister(&resource, subscriber_id).await;
            }
            Ok(())
        }
    }

    fn subscribe(
        &self,
        request: SubscribeRequestParams,
        context: RequestContext<RoleServer>,
    ) -> impl std::future::Future<Output = Result<(), ErrorData>> + Send + '_ {
        let registry = self.resources.clone();
        async move {
            let resource = SessionResource::parse(&request.uri).ok_or_else(|| {
                ErrorData::invalid_params(
                    format!("unknown lvt tree resource '{}'", request.uri),
                    None,
                )
            })?;
            if let Err(error) = validate_resource(&resource).await {
                return Err(error);
            }
            let epoch = registry.session_epoch(&resource.session).await;
            let snapshot = match get_tree_patch(&resource, true).await {
                Ok(snapshot) => snapshot,
                Err(error) => {
                    registry.clear_resource(&resource, epoch).await;
                    return Err(ErrorData::internal_error(error, None));
                }
            };
            if !registry.store_initial(&resource, epoch, snapshot).await {
                return Err(ErrorData::invalid_params(
                    format!("session '{}' was disconnected", resource.session),
                    None,
                ));
            }
            let Some(result) = registry
                .register(
                    &resource,
                    epoch,
                    ResourceSubscriber::Legacy(context.peer),
                    true,
                )
                .await
            else {
                return Err(ErrorData::invalid_params(
                    format!("session '{}' was disconnected", resource.session),
                    None,
                ));
            };
            if let Some((generation, cancelled)) = result.watcher {
                tokio::spawn(run_resource_watcher(
                    resource, registry, epoch, generation, cancelled,
                ));
            }
            Ok(())
        }
    }

    fn unsubscribe(
        &self,
        request: UnsubscribeRequestParams,
        _context: RequestContext<RoleServer>,
    ) -> impl std::future::Future<Output = Result<(), ErrorData>> + Send + '_ {
        let registry = self.resources.clone();
        async move {
            registry.unregister_legacy(&request.uri).await;
            Ok(())
        }
    }

    fn get_info(&self) -> ServerInfo {
        let mut implementation = Implementation::default();
        implementation.name = "lvt".into();
        implementation.version = ffi::version();

        let mut info = ServerInfo::default();
        info.capabilities = ServerCapabilities::builder()
            .enable_resources()
            .enable_resources_subscribe()
            .enable_tools()
            .build();
        info.server_info = implementation;
        info.instructions = Some(
            if self.allow_input {
                "Inspect and drive Windows application UIs.\n\n\
                 Call connect first to open a session, then pass its session id to every other \
                 tool. get_uia_tree and find_elements give you element ids; the action tools take \
                 those ids.\n\n\
                 A session speaks one tree. In the default \"uia\" mode, use references from \
                 get_uia_tree and find_elements; references from get_visual_tree are refused, \
                 because the two trees describe different sets of nodes and matching between them \
                 would be a guess. To drive an app by geometry instead, connect a second session \
                 with mode \"visual\". A session's mode cannot be changed after connecting, but \
                 sessions are cheap and you can hold several at once.\n\n\
                 Element ids like \"e12\" are positions in the tree you fetched, so re-fetch after \
                 anything that changes the UI's structure. An element's durable key or \
                 \"uia:<RuntimeId>\" survives more change and is worth preferring for anything \
                 long-lived.\n\n\
                 Prefer pattern-based tools (invoke, set_value, toggle) over click and type_text: \
                 they do not depend on window focus or cursor position, so they neither disturb \
                 the user nor get disturbed by them."
            } else {
                "Inspect Windows application UIs.\n\n\
                 Call connect first to open a session, then pass its session id to every other \
                 tool. get_uia_tree and find_elements give you element ids.\n\n\
                 This server is running read-only. Tools that would click, type, or otherwise \
                 change the target application are not available; lvt must be started with \
                 --allow-input to expose them."
            }
            .into(),
        );
        info
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::Value;

    const INSPECT_TOOLS: [&str; 14] = [
        "connect",
        "disconnect",
        "find_elements",
        "get_element_properties",
        "get_frameworks",
        "get_uia_tree",
        "get_uia_tree_changes",
        "get_visual_properties",
        "get_visual_tree",
        "get_visual_tree_changes",
        "hit_test",
        "list_apps",
        "screenshot",
        "wait_for",
    ];

    const INPUT_TOOLS: [&str; 14] = [
        "clear_visual_property",
        "click",
        "focus",
        "invoke",
        "press_key",
        "scroll",
        "select",
        "select_text",
        "set_expanded",
        "set_value",
        "set_visual_property",
        "toggle",
        "type_text",
        "window_action",
    ];

    #[test]
    fn read_only_server_exposes_only_inspection_tools() {
        let names = LvtServer::new(false).tool_names();
        assert_eq!(names, INSPECT_TOOLS);
    }

    #[test]
    fn read_only_server_hides_every_mutating_tool() {
        // The security property in one assertion: a tool that can change the
        // target must not be listed at all when input is not allowed.
        let names = LvtServer::new(false).tool_names();
        for tool in INPUT_TOOLS {
            assert!(
                !names.contains(&tool.to_string()),
                "{tool} must not be exposed without --allow-input"
            );
        }
    }

    #[test]
    fn allow_input_server_exposes_both_halves() {
        let names = LvtServer::new(true).tool_names();
        let mut expected: Vec<String> = INSPECT_TOOLS
            .iter()
            .chain(INPUT_TOOLS.iter())
            .map(|s| s.to_string())
            .collect();
        expected.sort();
        assert_eq!(names, expected);
    }

    #[test]
    fn every_tool_declares_an_output_schema_that_admits_a_failure() {
        // Declaring `outputSchema` is a promise that `structuredContent` conforms
        // to it, and a client may validate and reject anything that does not.
        // lvt reports failures as data — `{"ok": false, "error": "..."}` with none
        // of the success fields — so a schema that only described success would
        // be violated by a perfectly correct refusal.
        let failure = json!({ "ok": false, "error": "no element matched" });
        for tool in LvtServer::new(true).tool_router.list_all() {
            let schema = tool
                .output_schema
                .as_ref()
                .unwrap_or_else(|| panic!("tool '{}' declares no output schema", tool.name));
            let schema = serde_json::to_value(schema.as_ref()).unwrap();

            let branches = schema
                .get("anyOf")
                .and_then(|b| b.as_array())
                .unwrap_or_else(|| panic!("tool '{}' should accept success or failure", tool.name));
            assert_eq!(branches.len(), 2, "tool '{}'", tool.name);

            // The failure branch has to actually admit a failure payload. This
            // is the assertion that would fail if someone "tightened" the
            // schemas by dropping it.
            let error_branch = &branches[1];
            let required = error_branch
                .get("required")
                .and_then(|r| r.as_array())
                .cloned()
                .unwrap_or_default();
            assert!(
                required
                    .iter()
                    .all(|field| failure.get(field.as_str().unwrap()).is_some()),
                "tool '{}' would reject its own failure result: {error_branch}",
                tool.name
            );
        }
    }

    #[test]
    fn read_only_tools_say_so_and_input_tools_admit_what_they_do() {
        // Annotations are how a client decides whether to prompt before a call.
        // Getting them backwards is worse than omitting them, so the two halves
        // are asserted against each other rather than tool by tool.
        let server = LvtServer::new(true);
        let read_only = LvtServer::new(false).tool_names();
        for tool in server.tool_router.list_all() {
            let annotations = tool
                .annotations
                .as_ref()
                .unwrap_or_else(|| panic!("tool '{}' carries no annotations", tool.name));
            // Every tool reaches outside this process into another application.
            assert_eq!(
                annotations.open_world_hint,
                Some(true),
                "tool '{}' drives another process",
                tool.name
            );
            if read_only.iter().any(|name| name == tool.name.as_ref()) {
                assert_eq!(
                    annotations.read_only_hint,
                    Some(true),
                    "tool '{}' is exposed without --allow-input, so it must be marked read-only",
                    tool.name
                );
            } else {
                assert_ne!(
                    annotations.read_only_hint,
                    Some(true),
                    "tool '{}' changes the target application",
                    tool.name
                );
            }
        }
    }

    #[test]
    fn a_result_carries_the_same_answer_as_text_and_as_structure() {
        // The text block is for clients that predate structured content; the
        // structured copy is for everything else. They must not drift, or a
        // client sees one answer and a log shows another.
        let payload = r#"{"tree":"uia","elements":[{"id":"e1"}]}"#;
        let result = tool_result(payload.to_string(), true);
        let structured = result
            .structured_content
            .expect("structured content is missing");
        assert_eq!(structured, serde_json::from_str::<Value>(payload).unwrap());

        let ContentBlock::Text(text) = &result.content[0] else {
            panic!("the first content block should still be text");
        };
        assert_eq!(
            serde_json::from_str::<Value>(&text.text).unwrap(),
            structured
        );
    }

    #[test]
    fn a_failure_is_still_structured() {
        // A model reading "no element matched" should get it as data like any
        // other answer, so the error path carries structured content too.
        let result = tool_result(r#"{"ok":false,"error":"nope"}"#.to_string(), false);
        assert_eq!(result.is_error, Some(true));
        assert_eq!(
            result
                .structured_content
                .and_then(|v| v.get("error").cloned()),
            Some(Value::String("nope".into()))
        );
    }

    #[test]
    fn a_non_object_answer_still_travels_as_text() {
        // structuredContent is specified as an object, so anything else must not
        // be forced into the field — the text block still carries it rather than
        // the call failing.
        let result = tool_result("[1,2,3]".to_string(), true);
        assert!(result.structured_content.is_none());
        assert_eq!(result.content.len(), 1);
    }

    #[test]
    fn every_tool_has_a_description() {
        // The description is the only thing telling a model when to reach for a
        // tool, so an empty one is a real defect rather than a style nit.
        for tool in LvtServer::new(true).tool_router.list_all() {
            let description = tool.description.as_deref().unwrap_or("");
            assert!(
                description.len() > 30,
                "tool '{}' needs a description that explains when to use it",
                tool.name
            );
        }
    }

    #[test]
    fn every_tool_takes_a_session_except_the_two_that_cannot() {
        // Forgetting `session` on a tool is an easy mistake that only shows up
        // at runtime as a confusing "unknown session ''", so pin it here.
        for tool in LvtServer::new(true).tool_router.list_all() {
            if tool.name == "list_apps" || tool.name == "connect" {
                continue;
            }
            let schema = serde_json::to_value(&tool.input_schema).unwrap();
            let required = schema
                .get("required")
                .and_then(|r| r.as_array())
                .cloned()
                .unwrap_or_default();
            assert!(
                required.iter().any(|v| v.as_str() == Some("session")),
                "tool '{}' must require a session id",
                tool.name
            );
        }
    }

    #[test]
    fn compact_drops_nulls_so_lvt_applies_its_own_defaults() {
        let value =
            compact(json!({ "session": "s1", "view": serde_json::Value::Null, "depth": 3 }));
        assert_eq!(value, json!({ "session": "s1", "depth": 3 }));
    }

    #[test]
    fn compact_keeps_false_and_zero() {
        // A naive "drop falsy" would silently discard `uia: false`, flipping the
        // caller's request to the opposite default.
        let value = compact(json!({ "uia": false, "amount": 0, "text": "" }));
        assert_eq!(value, json!({ "uia": false, "amount": 0, "text": "" }));
    }

    #[test]
    fn instructions_differ_between_modes_and_mention_the_gate() {
        let read_only = LvtServer::new(false).get_info().instructions.unwrap();
        let full = LvtServer::new(true).get_info().instructions.unwrap();
        assert!(
            read_only.contains("--allow-input"),
            "read-only mode must explain how to enable input"
        );
        assert_ne!(read_only, full);
    }

    #[test]
    fn server_advertises_subscribable_resources() {
        let capabilities = LvtServer::new(false).get_info().capabilities;
        let resources = capabilities
            .resources
            .expect("resources capability is missing");
        assert_eq!(resources.subscribe, Some(true));
    }

    #[test]
    fn resource_uris_round_trip_sessions_and_modes() {
        let visual = SessionResource::new("s42", TreeMode::Visual);
        let uia = SessionResource::new("s43", TreeMode::Uia);
        assert_eq!(visual.uri(), "lvt://session/s42/visual-tree");
        assert_eq!(uia.uri(), "lvt://session/s43/uia-tree");
        assert_eq!(SessionResource::parse(&visual.uri()), Some(visual));
        assert_eq!(SessionResource::parse(&uia.uri()), Some(uia));
        assert!(SessionResource::parse("file:///not-lvt").is_none());
        assert!(SessionResource::parse("lvt://session/nope/visual-tree").is_none());
    }

    #[test]
    fn visual_resource_polls_always_use_fast_snapshots() {
        let visual = SessionResource::new("s1", TreeMode::Visual);
        let uia = SessionResource::new("s2", TreeMode::Uia);
        assert_eq!(
            tree_patch_params(&visual, true),
            json!({"session": "s1", "reset": true, "fast": true})
        );
        assert_eq!(
            tree_patch_params(&visual, false),
            json!({"session": "s1", "reset": false, "fast": true})
        );
        assert_eq!(
            tree_patch_params(&uia, true),
            json!({"session": "s2", "reset": true})
        );
    }

    #[test]
    fn modern_subscription_filter_accepts_both_tree_resource_kinds() {
        let requested = SubscriptionFilter::builder()
            .resource_subscription("lvt://session/s1/visual-tree")
            .resource_subscription("lvt://session/s2/uia-tree")
            .resource_subscription("file:///not-lvt")
            .build();
        let accepted = LvtServer::new(false)
            .accepted_subscription_filter(&requested)
            .expect("subscriptions/listen should be implemented");
        assert_eq!(
            accepted.resource_subscriptions,
            Some(vec![
                "lvt://session/s1/visual-tree".to_string(),
                "lvt://session/s2/uia-tree".to_string(),
            ])
        );
    }

    #[test]
    fn cached_patches_append_in_order_and_snapshots_replace_older_work() {
        let mut cached = Some(TreePatch {
            tree: "uia".into(),
            snapshot: false,
            events: vec![json!({"event": "changed", "key": "first"})],
        });
        assert!(matches!(
            merge_cached_patch(
                &mut cached,
                TreePatch {
                    tree: "uia".into(),
                    snapshot: false,
                    events: vec![json!({"event": "changed", "key": "second"})],
                }
            ),
            MergeResult::Cached
        ));
        assert_eq!(
            cached.as_ref().unwrap().events,
            vec![
                json!({"event": "changed", "key": "first"}),
                json!({"event": "changed", "key": "second"}),
            ]
        );

        merge_cached_patch(
            &mut cached,
            TreePatch {
                tree: "uia".into(),
                snapshot: true,
                events: vec![json!({"event": "added", "key": "snapshot"})],
            },
        );
        assert!(cached.as_ref().unwrap().snapshot);
        assert_eq!(
            cached.as_ref().unwrap().events,
            vec![json!({"event": "added", "key": "snapshot"})]
        );
    }

    #[test]
    fn an_oversized_pending_diff_requests_a_replacement_snapshot() {
        let mut cached = None;
        let result = merge_cached_patch(
            &mut cached,
            TreePatch {
                tree: "visual".into(),
                snapshot: false,
                events: vec![Value::Null; MAX_CACHED_RESOURCE_EVENTS + 1],
            },
        );
        assert!(matches!(result, MergeResult::NeedsSnapshot));
        assert!(
            cached.is_none(),
            "an incomplete oversized diff must not be served"
        );
    }

    #[test]
    fn server_reports_a_version() {
        let info = LvtServer::new(false).get_info();
        assert_eq!(info.server_info.name, "lvt");
        assert!(!info.server_info.version.is_empty());
    }
}
