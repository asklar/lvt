using System.Collections.Generic;
using System.Text.Json.Serialization;

namespace LvtViewer.Models;

public enum PropertyEditorKind
{
    ReadOnly,
    String,
    Boolean,
    Integer,
    Number,
    Enumeration,
    Command,
}

public sealed class PropertyChoiceDto
{
    public string Value { get; set; } = "";
    public string Label { get; set; } = "";
}

public sealed class PropertyDescriptorDto
{
    public string DescriptorId { get; set; } = "";
    public string Name { get; set; } = "";
    public string DisplayName { get; set; } = "";
    public string Provider { get; set; } = "";
    public string Framework { get; set; } = "";
    public string DeclaringType { get; set; } = "";
    public string PropertyType { get; set; } = "";
    public string Kind { get; set; } = "readonly";
    public List<PropertyChoiceDto> Choices { get; set; } = [];
    public double? Minimum { get; set; }
    public double? Maximum { get; set; }
    public double? Step { get; set; }
    public bool Writable { get; set; }
    public bool SupportsClear { get; set; }
    public string Description { get; set; } = "";

    [JsonIgnore]
    public PropertyEditorKind EditorKind { get; private set; }

    public void PreparePresentation()
    {
        EditorKind = Kind switch
        {
            "string" => PropertyEditorKind.String,
            "boolean" => PropertyEditorKind.Boolean,
            "integer" => PropertyEditorKind.Integer,
            "number" => PropertyEditorKind.Number,
            "enum" => PropertyEditorKind.Enumeration,
            "command" => PropertyEditorKind.Command,
            _ => PropertyEditorKind.ReadOnly,
        };
    }
}

public sealed class PropertyValueDto
{
    public string DescriptorId { get; set; } = "";
    public string Value { get; set; } = "";
    public string RuntimeType { get; set; } = "";
    public bool CanClear { get; set; }
    public bool Overridden { get; set; }
    public string Source { get; set; } = "";
    public string UnavailableReason { get; set; } = "";
    public string ReadOnlyReason { get; set; } = "";
}

public sealed class PropertySnapshotDto
{
    public string SchemaId { get; set; } = "";
    public List<PropertyDescriptorDto> Descriptors { get; set; } = [];
    public List<PropertyValueDto> Values { get; set; } = [];
}
