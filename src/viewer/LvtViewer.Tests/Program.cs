using LvtViewer.Models;
using LvtViewer.Services;
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
    return TypedRowFromDescriptor(descriptor, value);
}

static PropertyRowViewModel TypedRowFromDescriptor(
    PropertyDescriptorDto descriptor, string value)
{
    descriptor.PreparePresentation();
    var row = new PropertyRowViewModel(descriptor.Name, value);
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
    new PropertyRowViewModel("RangeValue.Value", "10"),
]);

var valueRow = TypedRow("Value.Value", "Value", "before", "string");
var toggleRow = TypedRow(
    "Toggle.ToggleState", "Toggle state", "Off", "enum",
    ("Off", "Off"), ("On", "On"));
var numberRow = TypedRow(
    "RangeValue.Value", "Range value", "10", "number");
node.ReplaceTypedPropertyRows([valueRow, toggleRow, numberRow]);

Assert(node.PropertyRows.Count == 3, "typed rows should replace raw rows by provider identity");
Assert(node.FindProperty("Value.Value") == valueRow, "Value row lost provider identity");
Assert(node.FindProperty("Toggle.ToggleState") == toggleRow, "Toggle row lost provider identity");
Assert(node.FindProperty("RangeValue.Value") == numberRow, "Range row lost provider identity");

valueRow.EditText = "pending text";
toggleRow.EditText = "On";
numberRow.EditText = "12.5";
node.SetProperty("Value.Value", "external value");
node.SetProperty("Toggle.ToggleState", "Indeterminate");
node.SetProperty("RangeValue.Value", "11");
Assert(valueRow.Value == "external value", "Value patch updated a duplicate raw row");
Assert(valueRow.EditText == "pending text", "text patch overwrote a pending edit");
Assert(toggleRow.EditText == "On", "enum patch overwrote a pending edit");
Assert(numberRow.EditText == "12.5", "number patch overwrote a pending edit");
Assert(valueRow.HasExternalConflict, "text conflict was not surfaced");
Assert(toggleRow.HasExternalConflict, "enum conflict was not surfaced");
Assert(numberRow.HasExternalConflict, "number conflict was not surfaced");
Assert(node.PropertyRows.Count == 3, "resource patches created duplicate property rows");

var refreshedValue = TypedRowFromDescriptor(
    new PropertyDescriptorDto
    {
        DescriptorId = "descriptor:Value.Value:readonly",
        Name = "Value.Value",
        DisplayName = "Value",
        Kind = "readonly",
        Writable = false,
    },
    "new effective");
var refreshedToggle = TypedRowFromDescriptor(
    new PropertyDescriptorDto
    {
        DescriptorId = "descriptor:Toggle.ToggleState:updated",
        Name = "Toggle.ToggleState",
        DisplayName = "Toggle state",
        Kind = "enum",
        Writable = true,
        Choices =
        [
            new PropertyChoiceDto { Value = "Off", Label = "Off" },
        ],
    },
    "Off");
var refreshedNumber = TypedRowFromDescriptor(
    new PropertyDescriptorDto
    {
        DescriptorId = "descriptor:RangeValue.Value:updated",
        Name = "RangeValue.Value",
        DisplayName = "Range value",
        Kind = "number",
        Writable = true,
        Minimum = 0,
        Maximum = 20,
    },
    "11");
node.ReplaceTypedPropertyRows(
    [refreshedValue, refreshedToggle, refreshedNumber],
    preservePendingEdits: true);
var preservedValue = node.FindProperty("Value.Value")!;
var preservedToggle = node.FindProperty("Toggle.ToggleState")!;
var preservedNumber = node.FindProperty("RangeValue.Value")!;
Assert(preservedValue.EditText == "pending text", "metadata refresh lost the pending edit");
Assert(preservedValue.HasExternalConflict, "metadata refresh hid the edit conflict");
Assert(preservedValue.Kind == PropertyEditorKind.ReadOnly, "writability/kind metadata did not refresh");
Assert(preservedToggle.Choices.Count == 1, "enum choices did not refresh");
Assert(!preservedToggle.CanApply, "removed enum choice remained applicable");
Assert(preservedNumber.Maximum == 20, "numeric limits did not refresh");
Assert(preservedNumber.EditText == "12.5", "numeric metadata refresh lost pending edit");
preservedValue.UpdateProviderValue("accepted text", preservePendingEdit: false);
Assert(!preservedValue.IsDirty, "successful refresh did not reset dirty state");
Assert(preservedValue.EditText == "accepted text", "successful refresh did not accept provider value");

preservedToggle.EditText = "not-a-provider-value";
Assert(!preservedToggle.CanApply, "arbitrary enum text was accepted");
preservedToggle.EditText = "Off";
Assert(preservedToggle.CanApply, "provider enum choice was rejected");

var booleanRow = TypedRow("Flag", "Flag", "true", "boolean");
booleanRow.EditText = "TRUE";
Assert(booleanRow.CanApply, "boolean validation stopped being case-insensitive");

var liveTree = new LiveTree();
liveTree.Apply(new TreeChangeEventDto
{
    Event = "added",
    Key = "parent",
    Path = "0",
    Element = new ElementDto { Type = "List", Framework = "uia" },
});
liveTree.Apply(new TreeChangeEventDto
{
    Event = "added",
    Key = "selected",
    Path = "0.0",
    Element = new ElementDto { Type = "ListItem", Framework = "uia" },
});
var selected = liveTree.Roots[0].Children[0];
Assert(MainViewModel.PatchAffectsTypedSchema(
    new TreeChangeEventDto
    {
        Event = "changed",
        Key = "selected",
        Fields = new()
        {
            ["properties.RangeValue.Maximum"] = new FieldChangeDto
            {
                Old = "10",
                New = "20",
            },
        },
    },
    selected), "selected range-bound patch did not request a descriptor refresh");
Assert(MainViewModel.PatchAffectsTypedSchema(
    new TreeChangeEventDto
    {
        Event = "changed",
        Key = "parent",
        Fields = new()
        {
            ["properties.Selection.CanSelectMultiple"] = new FieldChangeDto
            {
                Old = "false",
                New = "true",
            },
        },
    },
    selected), "ancestor selection-capability patch did not request a descriptor refresh");
Assert(!MainViewModel.PatchAffectsTypedSchema(
    new TreeChangeEventDto
    {
        Event = "changed",
        Key = "selected",
        Fields = new()
        {
            ["properties.Value.Value"] = new FieldChangeDto
            {
                Old = "a",
                New = "b",
            },
        },
    },
    selected), "ordinary value patch incorrectly requested a descriptor refresh");

Console.WriteLine("Viewer property regressions passed.");
