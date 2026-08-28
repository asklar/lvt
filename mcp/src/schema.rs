//! Output schemas for the tools.
//!
//! A tool that declares `outputSchema` is promising that its `structuredContent`
//! conforms to it, and a client is entitled to validate and reject anything that
//! does not. So these schemas are written to be *true of every response*, not to
//! be as descriptive as possible.
//!
//! The thing that makes that non-trivial: lvt reports failures as data. A failed
//! call comes back as `{"ok": false, "error": "..."}` with none of the fields the
//! success shape has, and it still travels as `structuredContent` because a model
//! reading "no element matched" should get it as structure like anything else. A
//! schema that required the success fields would therefore be violated by a
//! perfectly correct refusal — declaring more than we can honour, which is the
//! failure mode this whole server is written against.
//!
//! Hence every schema is `anyOf: [success, failure]`. `anyOf` rather than `oneOf`
//! deliberately: an action result carries `ok` on the way out whether it
//! succeeded or not, so the branches overlap, and `oneOf` would reject exactly
//! the payloads that match both.
//!
//! `additionalProperties` is never set to false. Adding a field to a response
//! should not break a validating client.

use std::sync::{Arc, OnceLock};

use rmcp::model::JsonObject;
use serde_json::{json, Value};

fn object(value: Value) -> Arc<JsonObject> {
    Arc::new(
        value
            .as_object()
            .cloned()
            .expect("a schema literal is always a JSON object"),
    )
}

/// The failure branch every tool shares.
fn failure() -> Value {
    json!({
        "type": "object",
        "description": "A failure. lvt reports these as data rather than as \
                        protocol errors, so the message is meant to be read and acted on.",
        "properties": {
            "ok": { "const": false },
            "error": { "type": "string", "description": "What went wrong, and where possible what to do instead." }
        },
        "required": ["error"]
    })
}

/// Success shape plus the failure envelope.
fn result(success: Value) -> Arc<JsonObject> {
    object(json!({
        "$schema": "https://json-schema.org/draft/2020-12/schema",
        "anyOf": [success, failure()]
    }))
}

/// One element, as every tool that returns elements spells it.
///
/// `children` appears only in tree responses and `properties` only when the
/// element has any, so neither is required. `ref` is the form to prefer: it
/// names the tree the id belongs to.
fn element() -> Value {
    json!({
        "type": "object",
        "properties": {
            "id": { "type": "string", "description": "Position in the tree you fetched, e.g. \"e12\"." },
            "ref": { "type": "string", "description": "Qualified reference, e.g. \"uia:e12\" or \"visual:e30\". Prefer this." },
            "key": { "type": "string", "description": "Durable key; survives more change than an id." },
            "type": { "type": "string" },
            "framework": { "type": "string" },
            "className": { "type": "string" },
            "text": { "type": "string" },
            "uiaRef": {
                "type": "string",
                "description": "This element's own UI Automation counterpart. Only from get_visual_tree with correlate:true."
            },
            "uiaAncestorRef": {
                "type": "string",
                "description": "The counterpart of the control this element sits inside. Context, not a target."
            },
            "bounds": {
                "type": "object",
                "description": "Screen rectangle in pixels.",
                "properties": {
                    "x": { "type": "integer" },
                    "y": { "type": "integer" },
                    "width": { "type": "integer" },
                    "height": { "type": "integer" }
                }
            },
            "properties": {
                "type": "object",
                "description": "Framework or UI Automation properties, as strings.",
                "additionalProperties": { "type": "string" }
            },
            "children": { "type": "array", "items": { "type": "object" } }
        },
        "required": ["id", "type"]
    })
}

fn truncated() -> Value {
    json!({
        "type": "string",
        "description": "Present when the walk hit its deadline, so the answer is partial. \
                        Raise timeoutMs and ask again before concluding something is absent."
    })
}

macro_rules! cached {
    ($name:ident, $body:expr) => {
        pub fn $name() -> Arc<JsonObject> {
            static CELL: OnceLock<Arc<JsonObject>> = OnceLock::new();
            CELL.get_or_init(|| $body).clone()
        }
    };
}

cached!(apps, {
    result(json!({
        "type": "object",
        "properties": {
            "apps": {
                "type": "array",
                "items": {
                    "type": "object",
                    "properties": {
                        "hwnd": { "type": "string", "description": "Window handle as 0x-prefixed hex. The unambiguous way to connect." },
                        "pid": { "type": "integer" },
                        "processName": { "type": "string" },
                        "title": { "type": "string" }
                    }
                }
            }
        },
        "required": ["apps"]
    }))
});

cached!(session, {
    result(json!({
        "type": "object",
        "properties": {
            "session": { "type": "string", "description": "Pass this to every other tool." },
            "hwnd": { "type": "string" },
            "pid": { "type": "integer" },
            "processName": { "type": "string" },
            "architecture": { "type": "string" },
            "mode": {
                "type": "string",
                "enum": ["uia", "visual"],
                "description": "Which tree this session speaks. Fixed for the session's lifetime."
            },
            "frameworks": { "type": "array", "items": { "type": "string" } }
        },
        "required": ["session", "mode"]
    }))
});

cached!(disconnected, {
    result(json!({
        "type": "object",
        "properties": { "disconnected": { "type": "string" } },
        "required": ["disconnected"]
    }))
});

cached!(frameworks, {
    result(json!({
        "type": "object",
        "properties": { "frameworks": { "type": "array", "items": { "type": "string" } } },
        "required": ["frameworks"]
    }))
});

cached!(uia_tree, {
    result(json!({
        "type": "object",
        "properties": {
            "root": element(),
            "tree": { "type": "string", "enum": ["uia"] },
            "truncated": truncated()
        },
        "required": ["root", "tree"]
    }))
});

cached!(visual_tree, {
    result(json!({
        "type": "object",
        "properties": {
            "root": element(),
            "tree": { "type": "string", "enum": ["visual"] },
            "truncated": truncated(),
            "correlated": {
                "type": "integer",
                "description": "How many elements in this response have a UI Automation counterpart. Only with correlate:true."
            },
            "correlationFailed": {
                "type": "string",
                "description": "The UI Automation side could not be read, so the absence of counterparts says nothing about the app."
            },
            "correlationPartial": { "type": "string" }
        },
        "required": ["root", "tree"]
    }))
});

cached!(visual_tree_changes, {
    result(json!({
        "type": "object",
        "properties": {
            "tree": { "type": "string", "enum": ["visual"] },
            "snapshot": {
                "type": "boolean",
                "description": "True when events describe a complete initial snapshot rather than a diff."
            },
            "events": {
                "type": "array",
                "items": {
                    "type": "object",
                    "properties": {
                        "event": { "type": "string", "enum": ["added", "removed", "changed"] },
                        "key": { "type": "string" },
                        "path": { "type": "string" },
                        "element": element(),
                        "fields": { "type": "object" }
                    },
                    "required": ["event", "key"]
                }
            }
        },
        "required": ["tree", "snapshot", "events"]
    }))
});

cached!(uia_tree_changes, {
    result(json!({
        "type": "object",
        "properties": {
            "tree": { "type": "string", "enum": ["uia"] },
            "snapshot": {
                "type": "boolean",
                "description": "True when events describe a complete initial snapshot rather than a diff."
            },
            "events": {
                "type": "array",
                "items": {
                    "type": "object",
                    "properties": {
                        "event": { "type": "string", "enum": ["added", "removed", "changed"] },
                        "key": { "type": "string" },
                        "path": { "type": "string" },
                        "element": element(),
                        "fields": { "type": "object" }
                    },
                    "required": ["event", "key"]
                }
            }
        },
        "required": ["tree", "snapshot", "events"]
    }))
});

cached!(editable_properties, {
    result(json!({
        "type": "object",
        "properties": {
            "ok": { "const": true },
            "element": { "type": "string" },
            "schemaId": { "type": "string" },
            "descriptors": {
                "type": "array",
                "items": {
                    "type": "object",
                    "properties": {
                        "descriptorId": { "type": "string" },
                        "name": { "type": "string" },
                        "displayName": { "type": "string" },
                        "provider": { "type": "string" },
                        "framework": { "type": "string" },
                        "declaringType": { "type": "string" },
                        "propertyType": { "type": "string" },
                        "kind": {
                            "type": "string",
                            "enum": ["readonly", "string", "boolean", "integer", "number", "enum"]
                        },
                        "choices": {
                            "type": "array",
                            "items": {
                                "type": "object",
                                "properties": {
                                    "value": { "type": "string" },
                                    "label": { "type": "string" }
                                },
                                "required": ["value", "label"]
                            }
                        },
                        "minimum": { "type": "number" },
                        "maximum": { "type": "number" },
                        "step": { "type": "number" },
                        "writable": { "type": "boolean" },
                        "supportsClear": { "type": "boolean" },
                        "description": { "type": "string" }
                    },
                    "required": [
                        "descriptorId", "name", "displayName", "provider", "framework",
                        "declaringType", "propertyType", "kind", "choices", "writable",
                        "supportsClear", "description"
                    ]
                }
            },
            "values": {
                "type": "array",
                "items": {
                    "type": "object",
                    "properties": {
                        "descriptorId": { "type": "string" },
                        "value": { "type": "string" },
                        "runtimeType": { "type": "string" },
                        "canClear": { "type": "boolean" },
                        "overridden": { "type": "boolean" },
                        "source": { "type": "string" },
                        "unavailableReason": { "type": "string" },
                        "readOnlyReason": { "type": "string" }
                    },
                    "required": [
                        "descriptorId", "value", "runtimeType", "canClear", "overridden",
                        "source", "unavailableReason", "readOnlyReason"
                    ]
                }
            }
        },
        "required": ["ok", "element", "schemaId", "descriptors", "values"]
    }))
});

cached!(property_mutation, {
    result(json!({
        "type": "object",
        "properties": {
            "ok": { "const": true },
            "element": { "type": "string" },
            "descriptorId": { "type": "string" },
            "value": { "type": "string" },
            "runtimeType": { "type": "string" },
            "canClear": { "type": "boolean" },
            "overridden": { "type": "boolean" },
            "source": { "type": "string" },
            "cleared": { "type": "boolean" }
        },
        "required": ["ok", "element", "descriptorId"]
    }))
});

cached!(elements, {
    result(json!({
        "type": "object",
        "properties": {
            "elements": { "type": "array", "items": element() },
            "searched": { "type": "integer", "description": "How many elements were examined." },
            "tree": { "type": "string", "enum": ["uia", "visual"] },
            "truncated": truncated()
        },
        "required": ["elements", "tree"]
    }))
});

cached!(element_properties, {
    // Two shapes share this tool. Asked for the whole element, `element` is the
    // element object; asked for named properties, it is the element's id and the
    // values arrive under `properties`. So `element` is deliberately left
    // untyped — constraining it would make one of the two correct answers
    // invalid.
    result(json!({
        "type": "object",
        "properties": {
            "element": {
                "description": "The element object when no properties were named; otherwise the element's id."
            },
            "ref": { "type": "string" },
            "tree": { "type": "string", "enum": ["uia", "visual"] },
            "properties": { "type": "object", "additionalProperties": { "type": "string" } },
            "notPresent": {
                "type": "array",
                "items": { "type": "string" },
                "description": "Requested properties this element does not have, as distinct from having them empty."
            },
            "truncated": truncated()
        },
        "required": ["element", "tree"]
    }))
});

cached!(hit_test, {
    result(json!({
        "type": "object",
        "properties": {
            "element": element(),
            "ancestors": {
                "type": "array",
                "items": { "type": "string" },
                "description": "Ids of the enclosing elements, outermost last."
            },
            "tree": { "type": "string", "enum": ["uia", "visual"] },
            "truncated": truncated()
        },
        "required": ["element", "tree"]
    }))
});

cached!(screenshot, {
    result(json!({
        "type": "object",
        "properties": {
            "annotated": {
                "type": "boolean",
                "description": "Whether element ids were drawn on the image. False means the tree could not be read."
            },
            "idsFrom": {
                "type": "string",
                "enum": ["uia", "visual"],
                "description": "Which tree the drawn ids belong to. Only present when annotated."
            },
            "path": { "type": "string", "description": "Where the PNG was written. Absent for an inline capture." },
            "truncated": truncated()
        },
        "required": ["annotated"]
    }))
});

cached!(action, {
    // Actions report failure two ways, and both are normal: a refusal lvt makes
    // itself arrives as the failure envelope, while an action that ran and did
    // not take effect comes back with `ok: false` alongside the fields below.
    result(json!({
        "type": "object",
        "properties": {
            "action": { "type": "string" },
            "ok": { "type": "boolean" },
            "element": { "type": "string", "description": "The reference as it was passed in." },
            "method": {
                "type": "string",
                "description": "How it was carried out — a UI Automation pattern such as \"InvokePattern\", or \"SendInput\" for real input."
            },
            "mode": { "type": "string", "enum": ["visual"], "description": "Present when the session drove the app by geometry." },
            "at": {
                "type": "object",
                "description": "Where a synthetic click landed, in screen pixels.",
                "properties": { "x": { "type": "integer" }, "y": { "type": "integer" } }
            },
            "broughtToForeground": { "type": "boolean" },
            "result": element(),
            "error": { "type": "string" }
        },
        "required": ["action"]
    }))
});
