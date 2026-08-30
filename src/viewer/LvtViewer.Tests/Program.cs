using System.IO;
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

static async Task<bool> ApplyDelayedTypedSnapshotAsync(
    Task release,
    ElementNodeViewModel node,
    long propertyVersion,
    TypedPropertyRefreshState refreshState,
    TypedPropertyRefreshState.Token refreshToken,
    IReadOnlyList<PropertyRowViewModel> rows,
    string? acceptedProviderName = null,
    long? acceptedEditRevision = null)
{
    await release;
    if (node.PropertyVersion != propertyVersion ||
        !refreshState.IsCurrent(refreshToken))
    {
        return false;
    }
    node.ReplaceTypedPropertyRows(
        rows,
        preservePendingEdits: true,
        acceptedProviderName: acceptedProviderName,
        acceptedEditRevision: acceptedEditRevision);
    return true;
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

var submittedValueRevision = valueRow.EditRevision;
var acceptedValue = TypedRow(
    "Value.Value", "Value", "accepted text", "string");
var retainedToggle = TypedRow(
    "Toggle.ToggleState", "Toggle state", "Indeterminate", "enum",
    ("Off", "Off"), ("On", "On"));
var retainedNumber = TypedRow(
    "RangeValue.Value", "Range value", "11", "number");
node.ReplaceTypedPropertyRows(
    [acceptedValue, retainedToggle, retainedNumber],
    preservePendingEdits: true,
    acceptedProviderName: "Value.Value",
    acceptedEditRevision: submittedValueRevision);
var acceptedValueRow = node.FindProperty("Value.Value")!;
var unrelatedToggle = node.FindProperty("Toggle.ToggleState")!;
var unrelatedNumber = node.FindProperty("RangeValue.Value")!;
Assert(!acceptedValueRow.IsDirty, "submitted row was not explicitly accepted");
Assert(acceptedValueRow.EditText == "accepted text", "submitted row did not reset");
Assert(unrelatedToggle.EditText == "On", "setting one row discarded another enum edit");
Assert(unrelatedNumber.EditText == "12.5", "setting one row discarded another number edit");

var exactReadbackRow = TypedRow("Value.Value", "Value", "old", "string");
exactReadbackRow.EditText = "submitted";
var exactReadbackRevision = exactReadbackRow.EditRevision;
exactReadbackRow.ApplyMutationValue("submitted", exactReadbackRevision);
Assert(exactReadbackRow.Value == "submitted" &&
       exactReadbackRow.EditText == "submitted",
    "direct set readback did not apply the accepted value");
Assert(!exactReadbackRow.IsDirty && !exactReadbackRow.HasExternalConflict,
    "direct set readback left stale dirty state before its follow-up refresh");

var setRaceNode = new ElementNodeViewModel();
var setRaceRow = TypedRow("Value.Value", "Value", "old", "string");
setRaceNode.ReplaceTypedPropertyRows([setRaceRow]);
setRaceRow.EditText = "A";
var submittedSetRevision = setRaceRow.EditRevision;
setRaceRow.EditText = "B";
setRaceRow.ApplyMutationValue("A", submittedSetRevision);
Assert(setRaceRow.Value == "A", "set completion did not update effective provider value");
Assert(setRaceRow.EditText == "B", "set completion overwrote newer pending edit");
Assert(setRaceRow.IsDirty && setRaceRow.HasExternalConflict,
    "set completion did not preserve newer edit as a conflict");
var setRefreshRow = TypedRow("Value.Value", "Value", "A", "string");
setRaceNode.ReplaceTypedPropertyRows(
    [setRefreshRow],
    preservePendingEdits: true,
    acceptedProviderName: "Value.Value",
    acceptedEditRevision: submittedSetRevision);
setRaceRow = setRaceNode.FindProperty("Value.Value")!;
Assert(setRaceRow.Value == "A" && setRaceRow.EditText == "B",
    "follow-up set refresh suppressed preservation of a later edit");

var revertedSetNode = new ElementNodeViewModel();
var revertedSetRow = TypedRow("Value.Value", "Value", "old", "string");
revertedSetNode.ReplaceTypedPropertyRows([revertedSetRow]);
revertedSetRow.EditText = "A";
var revertedSetRevision = revertedSetRow.EditRevision;
revertedSetRow.EditText = "old";
Assert(!revertedSetRow.IsDirty,
    "set revision regression did not return to a clean provider value");
revertedSetRow.ApplyMutationValue("A", revertedSetRevision);
Assert(revertedSetRow.Value == "A" && revertedSetRow.EditText == "old",
    "set completion overwrote a newer clean edit action");
Assert(revertedSetRow.IsDirty && revertedSetRow.HasExternalConflict,
    "set completion did not compare the newer action with direct readback");
revertedSetNode.ReplaceTypedPropertyRows(
    [TypedRow("Value.Value", "Value", "A", "string")],
    preservePendingEdits: true,
    acceptedProviderName: "Value.Value",
    acceptedEditRevision: revertedSetRevision);
revertedSetRow = revertedSetNode.FindProperty("Value.Value")!;
Assert(revertedSetRow.EditText == "old" && revertedSetRow.IsDirty,
    "set refresh ignored a newer revision because it was previously non-dirty");

var clearRaceNode = new ElementNodeViewModel();
var clearRaceRow = TypedRow("Value.Value", "Value", "effective", "string");
clearRaceNode.ReplaceTypedPropertyRows([clearRaceRow]);
clearRaceRow.EditText = "clear-A";
var submittedClearRevision = clearRaceRow.EditRevision;
clearRaceRow.EditText = "clear-B";
Assert(!clearRaceRow.TryDiscardSubmittedEdit(submittedClearRevision),
    "clear completion discarded a newer edit");
var clearRefreshRow = TypedRow("Value.Value", "Value", "default", "string");
clearRaceNode.ReplaceTypedPropertyRows(
    [clearRefreshRow],
    preservePendingEdits: true,
    acceptedProviderName: "Value.Value",
    acceptedEditRevision: submittedClearRevision);
clearRaceRow = clearRaceNode.FindProperty("Value.Value")!;
Assert(clearRaceRow.Value == "default" && clearRaceRow.EditText == "clear-B",
    "follow-up clear refresh overwrote a newer pending edit");
Assert(clearRaceRow.IsDirty && clearRaceRow.HasExternalConflict,
    "clear race did not remain visible as a conflict");

var revertedClearNode = new ElementNodeViewModel();
var revertedClearRow = TypedRow(
    "Value.Value", "Value", "effective", "string");
revertedClearNode.ReplaceTypedPropertyRows([revertedClearRow]);
var revertedClearRevision = revertedClearRow.EditRevision;
revertedClearRow.EditText = "temporary";
revertedClearRow.EditText = "effective";
Assert(!revertedClearRow.IsDirty,
    "clear revision regression did not return to a clean provider value");
Assert(!revertedClearRow.TryDiscardSubmittedEdit(revertedClearRevision),
    "clear completion discarded a newer clean edit action");
revertedClearNode.ReplaceTypedPropertyRows(
    [TypedRow("Value.Value", "Value", "default", "string")],
    preservePendingEdits: true,
    acceptedProviderName: "Value.Value",
    acceptedEditRevision: revertedClearRevision);
revertedClearRow = revertedClearNode.FindProperty("Value.Value")!;
Assert(revertedClearRow.Value == "default" &&
       revertedClearRow.EditText == "effective" &&
       revertedClearRow.IsDirty,
    "clear refresh ignored a newer revision because it was previously non-dirty");

var commandNode = new ElementNodeViewModel();
var commandRow = TypedRow(
    "Selection.Command", "Selection", "", "command",
    ("add", "Add"), ("remove", "Remove"));
commandNode.ReplaceTypedPropertyRows([commandRow]);
var submittedCommandRevision = commandRow.EditRevision;
commandRow.EditText = "remove";
commandRow.ApplyMutationValue("", submittedCommandRevision);
Assert(commandRow.EditText == "remove" && !commandRow.IsDirty,
    "command completion overwrote a newer non-dirty action");
commandNode.ReplaceTypedPropertyRows(
    [
        TypedRow(
            "Selection.Command", "Selection", "", "command",
            ("add", "Add"), ("remove", "Remove")),
    ],
    preservePendingEdits: true,
    acceptedProviderName: "Selection.Command",
    acceptedEditRevision: submittedCommandRevision);
commandRow = commandNode.FindProperty("Selection.Command")!;
Assert(commandRow.EditText == "remove" && !commandRow.IsDirty,
    "command refresh ignored a newer action because command rows are non-dirty");
commandNode.ReplaceTypedPropertyRows(
    [],
    preservePendingEdits: true,
    acceptedProviderName: "Selection.Command",
    acceptedEditRevision: submittedCommandRevision);
commandRow = commandNode.FindProperty("Selection.Command")!;
Assert(commandRow.EditText == "remove",
    "disappearing command descriptor lost a newer action");
Assert(commandRow.Kind == PropertyEditorKind.ReadOnly,
    "disappearing command descriptor was not retained");

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
Assert(preservedValue.Kind == PropertyEditorKind.ReadOnly, "writability/kind metadata did not refresh");
Assert(preservedToggle.Choices.Count == 1, "enum choices did not refresh");
Assert(!preservedToggle.CanApply, "removed enum choice remained applicable");
Assert(preservedNumber.Maximum == 20, "numeric limits did not refresh");
Assert(preservedNumber.EditText == "12.5", "numeric metadata refresh lost pending edit");

node.ReplaceTypedPropertyRows(
    [refreshedValue, refreshedToggle],
    preservePendingEdits: true);
preservedNumber = node.FindProperty("RangeValue.Value")!;
Assert(preservedNumber.Kind == PropertyEditorKind.ReadOnly, "disappearing dirty descriptor was not retained read-only");
Assert(preservedNumber.EditText == "12.5", "numeric metadata refresh lost pending edit");
Assert(preservedNumber.HasExternalConflict, "disappearing dirty descriptor conflict was hidden");

preservedToggle.EditText = "not-a-provider-value";
Assert(!preservedToggle.CanApply, "arbitrary enum text was accepted");
preservedToggle.EditText = "Off";
Assert(preservedToggle.CanApply, "provider enum choice was rejected");

var booleanRow = TypedRow("Flag", "Flag", "true", "boolean");
booleanRow.EditText = "TRUE";
Assert(booleanRow.CanApply, "boolean validation stopped being case-insensitive");

var refreshState = new TypedPropertyRefreshState();
var requested = refreshState.Request();
Assert(refreshState.TryBegin(out var firstAttempt), "schema refresh did not start");
Assert(firstAttempt == requested, "schema refresh generation drifted");
refreshState.Complete(firstAttempt, applied: false);
Assert(refreshState.HasPending, "ordinary value patch racing initial load lost its retry");
Assert(refreshState.TryBegin(out var retryAttempt), "stale schema refresh was not retried");
refreshState.Complete(retryAttempt, applied: true);
Assert(!refreshState.HasPending, "latest schema refresh was not marked applied");

var latestWinsState = new TypedPropertyRefreshState();
latestWinsState.Request();
Assert(latestWinsState.TryBegin(out var supersededAttempt),
    "superseded schema refresh did not start");
var latestRequest = latestWinsState.Request();
Assert(!latestWinsState.IsCurrent(supersededAttempt),
    "newer schema request did not retire the active snapshot");
Assert(latestWinsState.Complete(supersededAttempt, applied: false),
    "retired schema refresh did not release the coordinator");
Assert(latestWinsState.TryBegin(out var latestAttempt) &&
       latestAttempt == latestRequest,
    "latest schema request was not serialized after the retired snapshot");
latestWinsState.Complete(latestAttempt, applied: true);

var delayedSetNode = new ElementNodeViewModel();
var delayedSetRow = TypedRow("Value.Value", "Value", "old", "string");
delayedSetNode.ReplaceTypedPropertyRows([delayedSetRow]);
var delayedSetState = new TypedPropertyRefreshState();
delayedSetState.Request();
Assert(delayedSetState.TryBegin(out var delayedSetOldToken),
    "delayed pre-set refresh did not start");
var delayedSetOldVersion = delayedSetNode.PropertyVersion;
var releaseDelayedSet = new TaskCompletionSource(
    TaskCreationOptions.RunContinuationsAsynchronously);
var delayedSetOldSnapshot = ApplyDelayedTypedSnapshotAsync(
    releaseDelayedSet.Task,
    delayedSetNode,
    delayedSetOldVersion,
    delayedSetState,
    delayedSetOldToken,
    [TypedRow("Value.Value", "Value", "old", "string")]);
delayedSetRow.EditText = "A";
var delayedSetRevision = delayedSetRow.EditRevision;
delayedSetRow.EditText = "newer";
delayedSetRow.ApplyMutationValue("A", delayedSetRevision);
delayedSetNode.InvalidatePropertySnapshots();
var delayedSetFollowup = delayedSetState.Request();
Assert(delayedSetNode.PropertyVersion != delayedSetOldVersion,
    "set completion did not invalidate the pre-mutation property version");
Assert(!delayedSetState.IsCurrent(delayedSetOldToken),
    "set follow-up did not retire the pre-mutation refresh generation");
releaseDelayedSet.SetResult();
Assert(!await delayedSetOldSnapshot,
    "delayed pre-set snapshot applied after direct mutation readback");
Assert(delayedSetState.Complete(delayedSetOldToken, applied: false),
    "delayed pre-set snapshot did not release the refresh coordinator");
Assert(delayedSetState.TryBegin(out var delayedSetFollowupToken) &&
       delayedSetFollowupToken == delayedSetFollowup,
    "set follow-up was not queued behind the delayed snapshot");
var delayedSetApplied = await ApplyDelayedTypedSnapshotAsync(
    Task.CompletedTask,
    delayedSetNode,
    delayedSetNode.PropertyVersion,
    delayedSetState,
    delayedSetFollowupToken,
    [TypedRow("Value.Value", "Value", "A", "string")],
    "Value.Value",
    delayedSetRevision);
delayedSetState.Complete(delayedSetFollowupToken, delayedSetApplied);
delayedSetRow = delayedSetNode.FindProperty("Value.Value")!;
Assert(delayedSetRow.Value == "A" &&
       delayedSetRow.EditText == "newer" &&
       delayedSetRow.IsDirty &&
       delayedSetRow.HasExternalConflict,
    "serialized set follow-up lost provider readback or the newer edit");

var delayedClearNode = new ElementNodeViewModel();
var delayedClearRow = TypedRow(
    "Value.Value", "Value", "effective", "string");
delayedClearNode.ReplaceTypedPropertyRows([delayedClearRow]);
var delayedClearState = new TypedPropertyRefreshState();
delayedClearState.Request();
Assert(delayedClearState.TryBegin(out var delayedClearOldToken),
    "delayed pre-clear refresh did not start");
var delayedClearOldVersion = delayedClearNode.PropertyVersion;
var releaseDelayedClear = new TaskCompletionSource(
    TaskCreationOptions.RunContinuationsAsynchronously);
var delayedClearOldSnapshot = ApplyDelayedTypedSnapshotAsync(
    releaseDelayedClear.Task,
    delayedClearNode,
    delayedClearOldVersion,
    delayedClearState,
    delayedClearOldToken,
    [TypedRow("Value.Value", "Value", "effective", "string")]);
var delayedClearRevision = delayedClearRow.EditRevision;
delayedClearRow.EditText = "newer";
Assert(!delayedClearRow.TryDiscardSubmittedEdit(delayedClearRevision),
    "clear completion discarded the newer delayed-race edit");
delayedClearNode.InvalidatePropertySnapshots();
var delayedClearFollowup = delayedClearState.Request();
Assert(delayedClearNode.PropertyVersion != delayedClearOldVersion,
    "clear completion did not invalidate the pre-mutation property version");
Assert(!delayedClearState.IsCurrent(delayedClearOldToken),
    "clear follow-up did not retire the pre-mutation refresh generation");
releaseDelayedClear.SetResult();
Assert(!await delayedClearOldSnapshot,
    "delayed pre-clear snapshot applied after mutation completion");
Assert(delayedClearState.Complete(delayedClearOldToken, applied: false),
    "delayed pre-clear snapshot did not release the refresh coordinator");
Assert(delayedClearState.TryBegin(out var delayedClearFollowupToken) &&
       delayedClearFollowupToken == delayedClearFollowup,
    "clear follow-up was not queued behind the delayed snapshot");
var delayedClearApplied = await ApplyDelayedTypedSnapshotAsync(
    Task.CompletedTask,
    delayedClearNode,
    delayedClearNode.PropertyVersion,
    delayedClearState,
    delayedClearFollowupToken,
    [TypedRow("Value.Value", "Value", "default", "string")],
    "Value.Value",
    delayedClearRevision);
delayedClearState.Complete(delayedClearFollowupToken, delayedClearApplied);
delayedClearRow = delayedClearNode.FindProperty("Value.Value")!;
Assert(delayedClearRow.Value == "default" &&
       delayedClearRow.EditText == "newer" &&
       delayedClearRow.IsDirty &&
       delayedClearRow.HasExternalConflict,
    "serialized clear follow-up lost provider readback or the newer edit");

var abaState = new TypedPropertyRefreshState();
abaState.Request();
Assert(abaState.TryBegin(out var selectionA), "selection A refresh did not start");
abaState.Reset();
abaState.Request();
Assert(abaState.TryBegin(out var selectionB), "selection B refresh did not start");
abaState.Reset();
abaState.Request();
Assert(abaState.TryBegin(out var selectionAAgain), "reselected A refresh did not start");
Assert(!abaState.Complete(selectionA, applied: true),
    "stale selection A completion was accepted after reset");
Assert(!abaState.Complete(selectionB, applied: true),
    "stale selection B completion was accepted after A→B→A");
Assert(abaState.IsCurrent(selectionAAgain),
    "stale completion cleared the active reselected-A token");
Assert(abaState.HasPending,
    "stale completion marked the reselected-A generation applied");
Assert(abaState.Complete(selectionAAgain, applied: true),
    "current reselected-A completion was rejected");
Assert(!abaState.HasPending, "current reselected-A completion did not settle");

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
        Key = "selected",
        Fields = new()
        {
            ["properties.ExpandCollapse.State"] = new FieldChangeDto
            {
                Old = "LeafNode",
                New = "Collapsed",
            },
        },
    },
    selected), "ExpandCollapse schema transition did not request a refresh");
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
Assert(MainViewModel.PatchAffectsTypedSchema(
    new TreeChangeEventDto
    {
        Event = "changed",
        Key = "parent",
        Fields = new()
        {
            ["path"] = new FieldChangeDto
            {
                Old = "0",
                New = "1",
            },
        },
    },
    selected), "ancestor reparent did not request a descriptor refresh");
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

var schemaCache = new PropertyDescriptorSchemaCache();
for (int i = 0; i < PropertyDescriptorSchemaCache.MaximumSchemas * 4; ++i)
{
    schemaCache.Store(
        $"schema-{i}",
        [
            new PropertyDescriptorDto
            {
                DescriptorId = $"descriptor-{i}",
                Name = "Value.Value",
            },
        ],
        $"schema-{i}");
    Assert(
        schemaCache.Count <= PropertyDescriptorSchemaCache.MaximumSchemas,
        "Viewer schema cache grew without bound");
}
Assert(schemaCache.TryGet(
    $"schema-{PropertyDescriptorSchemaCache.MaximumSchemas * 4 - 1}",
    out var currentSchema), "current Viewer schema was evicted");
Assert(currentSchema.Count == 1, "current Viewer schema descriptors were lost");

var missingExecutable = Path.Combine(
    AppContext.BaseDirectory,
    $"missing-lvt-{Guid.NewGuid():N}.exe");
var failedSession = new McpSession();
var firstFailure = await failedSession.StartAsync(
    missingExecutable, "0x1", uia: true);
Assert(!firstFailure.Ok, "missing MCP executable unexpectedly started");
var secondFailure = await failedSession.StartAsync(
    missingExecutable, "0x1", uia: true);
Assert(!secondFailure.Ok, "second missing-executable start unexpectedly succeeded");
failedSession.Dispose();

Console.WriteLine("Viewer property regressions passed.");
