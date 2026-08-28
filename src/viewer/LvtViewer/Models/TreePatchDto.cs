using System.Collections.Generic;

namespace LvtViewer.Models;

/// <summary>
/// One read of an MCP session tree resource. The first read is a complete
/// snapshot expressed as added events; subsequent reads contain only diffs.
/// </summary>
public sealed class TreePatchDto
{
    public string Tree { get; set; } = "";
    public bool Snapshot { get; set; }
    public List<TreeChangeEventDto> Events { get; set; } = new();
}
