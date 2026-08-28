using System.Collections.Generic;

namespace LvtViewer.Models;

/// <summary>
/// One element-change event in an MCP tree-resource patch. The shape is
/// produced by watch_diff.cpp/serialize_change_event: "added" and "removed"
/// carry the full element; "changed" carries only differing fields keyed by
/// field name ("path", "type", "framework", "className", "text", "bounds",
/// or "properties.&lt;Name&gt;"). "parentKey" is an explicit relocation
/// signal for a provider object whose parent changed even if its absolute path
/// did not.
/// </summary>
public sealed class TreeChangeEventDto
{
    public string Event { get; set; } = "";
    public string Key { get; set; } = "";
    public string Path { get; set; } = "";
    public ElementDto? Element { get; set; }
    public Dictionary<string, FieldChangeDto>? Fields { get; set; }
}

public sealed class FieldChangeDto
{
    public string Old { get; set; } = "";
    public string New { get; set; } = "";
}
