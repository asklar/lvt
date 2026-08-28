namespace LvtViewer.Models;

/// <summary>
/// Editable XAML/WinUI dependency-property metadata returned by MCP.
/// </summary>
public sealed class VisualPropertyDto
{
    public string Name { get; set; } = "";
    public string Value { get; set; } = "";
    public string ValueType { get; set; } = "";
    public string DeclaringType { get; set; } = "";
    public uint PropertyIndex { get; set; }
    public ulong MetadataBits { get; set; }
    public bool Overridden { get; set; }
    public string Source { get; set; } = "";
}

