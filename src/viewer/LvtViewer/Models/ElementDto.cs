using System.Collections.Generic;

namespace LvtViewer.Models;

/// <summary>
/// Mirrors lvt::Bounds as serialized by json_serializer.cpp / watch_diff.cpp
/// ("x", "y", "width", "height").
/// </summary>
public sealed class BoundsDto
{
    public int X { get; set; }
    public int Y { get; set; }
    public int Width { get; set; }
    public int Height { get; set; }
}

/// <summary>
/// Mirrors lvt::Element (src/element.h) as serialized to JSON. Used both for
/// the one-shot "dump" output and for the "element" field of a watch "added"
/// event. Property names are matched case-insensitively against lvt's JSON,
/// so this does not need explicit JsonPropertyName attributes.
/// </summary>
public sealed class ElementDto
{
    public string Id { get; set; } = "";
    public string Key { get; set; } = "";
    public string Type { get; set; } = "";
    public string Framework { get; set; } = "";
    public string? ClassName { get; set; }
    public string? Text { get; set; }
    public BoundsDto Bounds { get; set; } = new();
    public Dictionary<string, string>? Properties { get; set; }
    public List<ElementDto>? Children { get; set; }
}

/// <summary>Top-level shape of "lvt dump --format json" (see json_serializer.cpp).</summary>
public sealed class DumpResultDto
{
    public TargetDto? Target { get; set; }
    public List<string>? Frameworks { get; set; }
    public ElementDto? Root { get; set; }
}

public sealed class TargetDto
{
    public string Hwnd { get; set; } = "";
    public uint Pid { get; set; }
    public string ProcessName { get; set; } = "";
}
