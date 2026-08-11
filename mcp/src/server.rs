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

use rmcp::handler::server::router::tool::ToolRouter;
use rmcp::handler::server::wrapper::Parameters;
use rmcp::model::{CallToolResult, ContentBlock, Implementation, ServerCapabilities, ServerInfo};
use rmcp::{tool, tool_handler, tool_router, ErrorData, ServerHandler};
use serde::Deserialize;
use serde_json::json;

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
    /// Return only this element and its descendants. Accepts an "eN" id, a
    /// durable key, or "uia:<RuntimeId>".
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
    /// Element to describe: an "eN" id, a durable key, or "uia:<RuntimeId>".
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
    /// Target element: an "eN" id, a durable key, or "uia:<RuntimeId>".
    pub element: String,
    /// UIA tree view used to resolve the element: "control" (default),
    /// "content", or "raw". Must match the view the id came from.
    pub view: Option<String>,    /// Set false when the element id came from get_visual_tree rather than the
    /// UIA tree. Durable keys are recognised automatically; a bare "eN" id is
    /// ambiguous, so it needs this. lvt then matches the visual element to the
    /// UI Automation element at the same place on screen.
    pub uia: Option<bool>,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
pub struct ClickArgs {
    /// Session id returned by connect.
    pub session: String,
    /// Element to click: an "eN" id, a durable key, or "uia:<RuntimeId>".
    pub element: String,
    /// Mouse button: 0 left (default), 1 right, 2 middle.
    pub button: Option<i32>,
    /// Force a real mouse click even when the element supports Invoke. Needed
    /// for controls that behave differently under synthetic input.
    pub synthetic: Option<bool>,
    /// UIA tree view used to resolve the element.
    pub view: Option<String>,    /// Set false when the element id came from get_visual_tree rather than the
    /// UIA tree. Durable keys are recognised automatically; a bare "eN" id is
    /// ambiguous, so it needs this. lvt then matches the visual element to the
    /// UI Automation element at the same place on screen.
    pub uia: Option<bool>,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
pub struct SetValueArgs {
    /// Session id returned by connect.
    pub session: String,
    /// Element to set: an "eN" id, a durable key, or "uia:<RuntimeId>".
    pub element: String,
    /// The value to set. Replaces the element's current value outright.
    pub text: String,
    /// UIA tree view used to resolve the element.
    pub view: Option<String>,    /// Set false when the element id came from get_visual_tree rather than the
    /// UIA tree. Durable keys are recognised automatically; a bare "eN" id is
    /// ambiguous, so it needs this. lvt then matches the visual element to the
    /// UI Automation element at the same place on screen.
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
    pub view: Option<String>,    /// Set false when the element id came from get_visual_tree rather than the
    /// UIA tree. Durable keys are recognised automatically; a bare "eN" id is
    /// ambiguous, so it needs this. lvt then matches the visual element to the
    /// UI Automation element at the same place on screen.
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
    pub view: Option<String>,    /// Set false when the element id came from get_visual_tree rather than the
    /// UIA tree. Durable keys are recognised automatically; a bare "eN" id is
    /// ambiguous, so it needs this. lvt then matches the visual element to the
    /// UI Automation element at the same place on screen.
    pub uia: Option<bool>,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
pub struct ScrollArgs {
    /// Session id returned by connect.
    pub session: String,
    /// Element to scroll: an "eN" id, a durable key, or "uia:<RuntimeId>".
    pub element: String,
    /// Direction: "up", "down", "left", or "right". Defaults to "down".
    pub direction: Option<String>,
    /// Number of scroll increments. Defaults to 1.
    pub amount: Option<i32>,
    /// UIA tree view used to resolve the element.
    pub view: Option<String>,    /// Set false when the element id came from get_visual_tree rather than the
    /// UIA tree. Durable keys are recognised automatically; a bare "eN" id is
    /// ambiguous, so it needs this. lvt then matches the visual element to the
    /// UI Automation element at the same place on screen.
    pub uia: Option<bool>,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
pub struct SelectArgs {
    /// Session id returned by connect.
    pub session: String,
    /// Element to select: an "eN" id, a durable key, or "uia:<RuntimeId>".
    pub element: String,
    /// "replace" (default) clears any other selection, "add" extends it,
    /// "remove" deselects just this element.
    pub mode: Option<String>,
    /// UIA tree view used to resolve the element.
    pub view: Option<String>,    /// Set false when the element id came from get_visual_tree rather than the
    /// UIA tree. Durable keys are recognised automatically; a bare "eN" id is
    /// ambiguous, so it needs this. lvt then matches the visual element to the
    /// UI Automation element at the same place on screen.
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
    pub view: Option<String>,    /// Set false when the element id came from get_visual_tree rather than the
    /// UIA tree. Durable keys are recognised automatically; a bare "eN" id is
    /// ambiguous, so it needs this. lvt then matches the visual element to the
    /// UI Automation element at the same place on screen.
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
    /// Element to watch: an "eN" id, a durable key, or "uia:<RuntimeId>".
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
    pub view: Option<String>,    /// Set false when the element id came from get_visual_tree rather than the
    /// UIA tree. Durable keys are recognised automatically; a bare "eN" id is
    /// ambiguous, so it needs this. lvt then matches the visual element to the
    /// UI Automation element at the same place on screen.
    pub uia: Option<bool>,
}

// --- server -------------------------------------------------------------

#[derive(Clone)]
pub struct LvtServer {
    tool_router: ToolRouter<LvtServer>,
    allow_input: bool,
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

async fn forward(
    method: &str,
    params: serde_json::Value,
    allow_input: bool,
) -> Result<CallToolResult, ErrorData> {
    let result = call_lvt(method, params, allow_input).await?;
    if result.ok {
        Ok(CallToolResult::success(vec![ContentBlock::text(result.json)]))
    } else {
        Ok(CallToolResult::error(vec![ContentBlock::text(result.json)]))
    }
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
                       know which window to target."
    )]
    async fn list_apps(&self, Parameters(a): Parameters<ListAppsArgs>) -> Result<CallToolResult, ErrorData> {
        forward("list_apps", compact(json!({ "name": a.name, "title": a.title })), self.allow_input).await
    }

    #[tool(
        description = "Connect to an application window and open a session. Returns a session id \
                       that every other tool needs, plus the window's pid, architecture and \
                       detected UI frameworks. Identify the window by hwnd (unambiguous), pid, \
                       process name, or title substring."
    )]
    async fn connect(&self, Parameters(a): Parameters<ConnectArgs>) -> Result<CallToolResult, ErrorData> {
        forward(
            "connect",
            compact(json!({ "name": a.name, "title": a.title, "pid": a.pid, "hwnd": a.hwnd })), self.allow_input).await
    }

    #[tool(description = "Close a session opened by connect and release its resources.")]
    async fn disconnect(&self, Parameters(a): Parameters<SessionArgs>) -> Result<CallToolResult, ErrorData> {
        forward("disconnect", json!({ "session": a.session }), self.allow_input).await
    }

    #[tool(
        description = "Get the UI Automation tree: AutomationIds, control types, names, states \
                       and supported patterns. This is the tree to use for automating an app — \
                       its identifiers are stable and its elements are actionable. Works against \
                       any process regardless of architecture."
    )]
    async fn get_uia_tree(&self, Parameters(a): Parameters<TreeArgs>) -> Result<CallToolResult, ErrorData> {
        forward("get_uia_tree", tree_params(a), self.allow_input).await
    }

    #[tool(
        description = "Get the framework-native visual tree (Win32 windows, XAML/WPF/WinForms/\
                       Avalonia/Chromium elements). Use this to understand how a UI is built — \
                       it shows implementation structure the UIA tree hides. It cannot be used \
                       to drive the app, and it requires lvt and the target to share an \
                       architecture."
    )]
    async fn get_visual_tree(&self, Parameters(a): Parameters<TreeArgs>) -> Result<CallToolResult, ErrorData> {
        forward("get_visual_tree", tree_params(a), self.allow_input).await
    }

    #[tool(description = "List the UI frameworks detected in the connected application, with versions.")]
    async fn get_frameworks(&self, Parameters(a): Parameters<SessionArgs>) -> Result<CallToolResult, ErrorData> {
        forward("get_frameworks", json!({ "session": a.session }), self.allow_input).await
    }

    #[tool(
        description = "Find elements by AutomationId, name, control type, or supported pattern. \
                       Much cheaper than fetching a whole tree when you know what you are looking \
                       for. Returns each match's element id, which the action tools accept."
    )]
    async fn find_elements(&self, Parameters(a): Parameters<FindArgs>) -> Result<CallToolResult, ErrorData> {
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
            })), self.allow_input).await
    }

    #[tool(
        description = "Read one element's full set of properties, or a chosen subset. Use this to \
                       check state — whether a checkbox is on, what a text box currently holds, \
                       which patterns a control supports."
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
            })), self.allow_input).await
    }

    #[tool(
        description = "Capture a PNG of the connected window with element ids drawn on it. \
                       The ids are the same ones find_elements and the action tools use, so you \
                       can read one off the image and act on it. Returns the image inline unless \
                       a path is given. Use it to see a UI whose tree is ambiguous, or to confirm \
                       an action had the effect you expected."
    )]
    async fn screenshot(&self, Parameters(a): Parameters<ScreenshotArgs>) -> Result<CallToolResult, ErrorData> {
        let params = compact(json!({
            "session": a.session,
            "path": a.path,
            "element": a.element,
            "uia": a.uia,
            "timeoutMs": a.timeout_ms,
        }));
        let result = call_lvt("screenshot", params, self.allow_input).await?;
        if !result.ok {
            return Ok(CallToolResult::error(vec![ContentBlock::text(result.json)]));
        }

        // An inline capture comes back as base64, which becomes an image
        // content block. The base64 is stripped from the JSON summary first —
        // repeating a megabyte of it as text would be useless and expensive.
        let mut parsed: serde_json::Value = serde_json::from_str(&result.json)
            .map_err(|e| ErrorData::internal_error(format!("lvt returned malformed JSON: {e}"), None))?;
        let image = parsed
            .as_object_mut()
            .and_then(|m| m.remove("imageBase64"))
            .and_then(|v| v.as_str().map(str::to_owned));

        let mut content = vec![ContentBlock::text(parsed.to_string())];
        if let Some(data) = image {
            content.push(ContentBlock::image(data, "image/png"));
        }
        Ok(CallToolResult::success(content))
    }

    #[tool(
        description = "Find the element at a screen coordinate. Returns the smallest element \
                       covering the point — what a click there would hit — plus the ids of its \
                       ancestors. Useful for turning something you saw in a screenshot into an \
                       element id you can act on."
    )]
    async fn hit_test(&self, Parameters(a): Parameters<HitTestArgs>) -> Result<CallToolResult, ErrorData> {
        forward(
            "hit_test",
            compact(json!({
                "session": a.session, "x": a.x, "y": a.y, "uia": a.uia, "uia": a.uia, "view": a.view,
                "timeoutMs": a.timeout_ms,
            })), self.allow_input).await
    }

    #[tool(
        description = "Block until an element reaches a property value, or until it disappears. \
                       Use this instead of guessing at delays after an action that starts work."
    )]
    async fn wait_for(&self, Parameters(a): Parameters<WaitForArgs>) -> Result<CallToolResult, ErrorData> {
        let method = if a.gone.unwrap_or(false) { "wait_gone" } else { "wait_for" };
        forward(
            method,
            compact(json!({
                "session": a.session,
                "element": a.element,
                "waitProperty": a.wait_property,
                "waitValue": a.wait_value,
                "timeoutMs": a.timeout_ms,
                "uia": a.uia, "view": a.view,
            })), self.allow_input).await
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

#[tool_router(router = input_router, vis = "pub")]
impl LvtServer {
    #[tool(
        description = "Click an element. Uses the element's Invoke pattern when it has one, \
                       falling back to a real mouse click at its centre. Set synthetic to force \
                       the mouse path."
    )]
    async fn click(&self, Parameters(a): Parameters<ClickArgs>) -> Result<CallToolResult, ErrorData> {
        forward(
            "click",
            compact(json!({
                "session": a.session,
                "element": a.element,
                "button": a.button,
                "synthetic": a.synthetic,
                "uia": a.uia, "view": a.view,
            })), self.allow_input).await
    }

    #[tool(
        description = "Invoke an element's default action via its UIA Invoke pattern, without \
                       moving the mouse. Prefer this over click when the element supports it."
    )]
    async fn invoke(&self, Parameters(a): Parameters<ElementArgs>) -> Result<CallToolResult, ErrorData> {
        forward("invoke", element_params(a), self.allow_input).await
    }

    #[tool(description = "Toggle a checkbox or other togglable control to its next state.")]
    async fn toggle(&self, Parameters(a): Parameters<ElementArgs>) -> Result<CallToolResult, ErrorData> {
        forward("toggle", element_params(a), self.allow_input).await
    }

    #[tool(
        description = "Set an element's value outright — the reliable way to fill a text box or \
                       move a slider. Prefer this over type_text, which depends on focus."
    )]
    async fn set_value(&self, Parameters(a): Parameters<SetValueArgs>) -> Result<CallToolResult, ErrorData> {
        forward(
            "set_value",
            compact(json!({
                "session": a.session, "element": a.element, "text": a.text, "uia": a.uia, "view": a.view,
            })), self.allow_input).await
    }

    #[tool(description = "Expand or collapse a tree item, combo box, or other expandable control.")]
    async fn set_expanded(&self, Parameters(a): Parameters<SetExpandedArgs>) -> Result<CallToolResult, ErrorData> {
        let method = if a.expanded { "expand" } else { "collapse" };
        forward(
            method,
            compact(json!({ "session": a.session, "element": a.element, "uia": a.uia, "view": a.view })), self.allow_input).await
    }

    #[tool(
        description = "Select a list item, tab, or similar. mode 'replace' clears any other \
                       selection, 'add' extends a multi-select, 'remove' deselects."
    )]
    async fn select(&self, Parameters(a): Parameters<SelectArgs>) -> Result<CallToolResult, ErrorData> {
        let method = match a.mode.as_deref().unwrap_or("replace") {
            "add" => "add_to_selection",
            "remove" => "remove_from_selection",
            "replace" => "select",
            other => {
                return Ok(CallToolResult::error(vec![ContentBlock::text(format!(
                    "{{\"ok\":false,\"error\":\"mode must be replace, add or remove, not '{other}'\"}}"
                ))]))
            }
        };
        forward(
            method,
            compact(json!({ "session": a.session, "element": a.element, "uia": a.uia, "view": a.view })), self.allow_input).await
    }

    #[tool(description = "Give an element keyboard focus.")]
    async fn focus(&self, Parameters(a): Parameters<ElementArgs>) -> Result<CallToolResult, ErrorData> {
        forward("focus", element_params(a), self.allow_input).await
    }

    #[tool(description = "Select all of a text control's contents.")]
    async fn select_text(&self, Parameters(a): Parameters<ElementArgs>) -> Result<CallToolResult, ErrorData> {
        forward("select_text", element_params(a), self.allow_input).await
    }

    #[tool(
        description = "Scroll an element. Uses its Scroll pattern when it has one, falling back \
                       to mouse wheel input over the element."
    )]
    async fn scroll(&self, Parameters(a): Parameters<ScrollArgs>) -> Result<CallToolResult, ErrorData> {
        forward(
            "scroll",
            compact(json!({
                "session": a.session,
                "element": a.element,
                "direction": a.direction,
                "amount": a.amount,
                "uia": a.uia, "view": a.view,
            })), self.allow_input).await
    }

    #[tool(
        description = "Type text as keystrokes. This goes to whatever has focus, so pass an \
                       element to focus first. set_value is more reliable where it works; use \
                       this when the app must see real key events."
    )]
    async fn type_text(&self, Parameters(a): Parameters<TypeTextArgs>) -> Result<CallToolResult, ErrorData> {
        forward(
            "type_text",
            compact(json!({
                "session": a.session, "element": a.element, "text": a.text, "uia": a.uia, "view": a.view,
            })), self.allow_input).await
    }

    #[tool(
        description = "Press a key or chord, e.g. \"Enter\", \"Ctrl+S\", \"Alt+F4\", \"F5\". \
                       Use this for shortcuts and for keys that have no element to click."
    )]
    async fn press_key(&self, Parameters(a): Parameters<PressKeyArgs>) -> Result<CallToolResult, ErrorData> {
        forward(
            "press_key",
            compact(json!({
                "session": a.session, "element": a.element, "text": a.text, "uia": a.uia, "view": a.view,
            })), self.allow_input).await
    }

    #[tool(description = "Minimize, maximize, restore, or close the connected window.")]
    async fn window_action(&self, Parameters(a): Parameters<WindowActionArgs>) -> Result<CallToolResult, ErrorData> {
        let method = match a.action.as_str() {
            "minimize" => "minimize_window",
            "maximize" => "maximize_window",
            "restore" => "restore_window",
            "close" => "close_window",
            other => {
                return Ok(CallToolResult::error(vec![ContentBlock::text(format!(
                    "{{\"ok\":false,\"error\":\"action must be minimize, maximize, restore or close, not '{other}'\"}}"
                ))]))
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
        Self { tool_router: router, allow_input }
    }

    /// Tool names this server is currently exposing, sorted. Exists so tests
    /// can assert the `--allow-input` gate without standing up a transport.
    pub fn tool_names(&self) -> Vec<String> {
        let mut names: Vec<String> =
            self.tool_router.list_all().into_iter().map(|t| t.name.to_string()).collect();
        names.sort();
        names
    }
}

#[tool_handler(router = self.tool_router)]
impl ServerHandler for LvtServer {
    fn get_info(&self) -> ServerInfo {
        let mut implementation = Implementation::default();
        implementation.name = "lvt".into();
        implementation.version = ffi::version();

        let mut info = ServerInfo::default();
        info.capabilities = ServerCapabilities::builder().enable_tools().build();
        info.server_info = implementation;
        info.instructions = Some(
            if self.allow_input {
                "Inspect and drive Windows application UIs.\n\n\
                 Call connect first to open a session, then pass its session id to every other \
                 tool. get_uia_tree and find_elements give you element ids; the action tools take \
                 those ids.\n\n\
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

    const INSPECT_TOOLS: [&str; 11] = [
        "connect",
        "disconnect",
        "find_elements",
        "get_element_properties",
        "get_frameworks",
        "get_uia_tree",
        "get_visual_tree",
        "hit_test",
        "list_apps",
        "screenshot",
        "wait_for",
    ];

    const INPUT_TOOLS: [&str; 12] = [
        "click",
        "focus",
        "invoke",
        "press_key",
        "scroll",
        "select",
        "select_text",
        "set_expanded",
        "set_value",
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
            assert!(!names.contains(&tool.to_string()), "{tool} must not be exposed without --allow-input");
        }
    }

    #[test]
    fn allow_input_server_exposes_both_halves() {
        let names = LvtServer::new(true).tool_names();
        let mut expected: Vec<String> =
            INSPECT_TOOLS.iter().chain(INPUT_TOOLS.iter()).map(|s| s.to_string()).collect();
        expected.sort();
        assert_eq!(names, expected);
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
            let required = schema.get("required").and_then(|r| r.as_array()).cloned().unwrap_or_default();
            assert!(
                required.iter().any(|v| v.as_str() == Some("session")),
                "tool '{}' must require a session id",
                tool.name
            );
        }
    }

    #[test]
    fn compact_drops_nulls_so_lvt_applies_its_own_defaults() {
        let value = compact(json!({ "session": "s1", "view": serde_json::Value::Null, "depth": 3 }));
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
        assert!(read_only.contains("--allow-input"), "read-only mode must explain how to enable input");
        assert_ne!(read_only, full);
    }

    #[test]
    fn server_reports_a_version() {
        let info = LvtServer::new(false).get_info();
        assert_eq!(info.server_info.name, "lvt");
        assert!(!info.server_info.version.is_empty());
    }
}
