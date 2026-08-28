using LvtViewer.Models;
using LvtViewer.ViewModels;

static void Assert(bool condition, string message)
{
    if (!condition)
        throw new InvalidOperationException(message);
}

static PropertyRowViewModel TypedRow(
    string providerName, string displayName, string value, string kind,
    params (string Value, string Label)[] choices)
{
    var descriptor = new PropertyDescriptorDto
    {
        DescriptorId = $"descriptor:{providerName}",
        Name = providerName,
        DisplayName = displayName,
        Kind = kind,
        Writable = true,
    };
    foreach (var choice in choices)
    {
        descriptor.Choices.Add(new PropertyChoiceDto
        {
            Value = choice.Value,
            Label = choice.Label,
        });
    }
    descriptor.PreparePresentation();
    var row = new PropertyRowViewModel(providerName, value);
    row.UpdateTypedProperty(
        descriptor,
        new PropertyValueDto
        {
            DescriptorId = descriptor.DescriptorId,
            Value = value,
        });
    return row;
}

var node = new ElementNodeViewModel();
node.ReplacePropertyRows(
[
    new PropertyRowViewModel("Value.Value", "before"),
    new PropertyRowViewModel("Toggle.ToggleState", "Off"),
]);

var valueRow = TypedRow("Value.Value", "Value", "before", "string");
var toggleRow = TypedRow(
    "Toggle.ToggleState", "Toggle state", "Off", "enum",
    ("Off", "Off"), ("On", "On"));
node.ReplaceTypedPropertyRows([valueRow, toggleRow]);

Assert(node.PropertyRows.Count == 2, "typed rows should replace raw rows by provider identity");
Assert(node.FindProperty("Value.Value") == valueRow, "Value row lost provider identity");
Assert(node.FindProperty("Toggle.ToggleState") == toggleRow, "Toggle row lost provider identity");

node.SetProperty("Value.Value", "external value");
node.SetProperty("Toggle.ToggleState", "On");
Assert(valueRow.Value == "external value", "Value patch updated a duplicate raw row");
Assert(toggleRow.Value == "On", "Toggle patch updated a duplicate raw row");
Assert(node.PropertyRows.Count == 2, "resource patches created duplicate property rows");

toggleRow.EditText = "not-a-provider-value";
Assert(!toggleRow.CanApply, "arbitrary enum text was accepted");
toggleRow.EditText = "On";
Assert(toggleRow.CanApply, "provider enum choice was rejected");

var booleanRow = TypedRow("Flag", "Flag", "true", "boolean");
booleanRow.EditText = "TRUE";
Assert(booleanRow.CanApply, "boolean validation stopped being case-insensitive");

Console.WriteLine("Viewer property regressions passed.");
