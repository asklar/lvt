using System.Collections.Generic;

namespace LvtViewer.Models;

/// <summary>
/// One line of "lvt watch"'s JSON event stream (see watch_diff.cpp,
/// serialize_change_event). "added" and "removed" carry the full element;
/// "changed" carries only the fields that differ, keyed by field name
/// ("type", "framework", "className", "text", "bounds", or
/// "properties.&lt;Name&gt;" for a single UIA/framework property).
/// </summary>
public sealed class WatchEventDto
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
