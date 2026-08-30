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
    IReadOnlyList<PropertyRowViewModel> rows)
{
    await release;
    if (node.PropertyVersion != propertyVersion ||
        !refreshState.IsCurrent(refreshToken))
    {
        return false;
    }
    node.ReplaceTypedPropertyRows(
        rows,
        preservePendingEdits: true);
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
var acceptedValueMutation = node.BeginPropertyMutation(
    "Value.Value", submittedValueRevision);
Assert(node.TryCompletePropertyMutation(acceptedValueMutation),
    "submitted value mutation was unexpectedly superseded");
node.ReplaceTypedPropertyRows(
    [acceptedValue, retainedToggle, retainedNumber],
    preservePendingEdits: true);
node.SettleCompletedPropertyMutations();
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
var setRaceMutation = setRaceNode.BeginPropertyMutation(
    "Value.Value", submittedSetRevision);
setRaceRow.EditText = "B";
Assert(setRaceNode.TryCompletePropertyMutation(setRaceMutation),
    "set race mutation was unexpectedly superseded");
setRaceRow.ApplyMutationValue("A", submittedSetRevision);
Assert(setRaceRow.Value == "A", "set completion did not update effective provider value");
Assert(setRaceRow.EditText == "B", "set completion overwrote newer pending edit");
Assert(setRaceRow.IsDirty && setRaceRow.HasExternalConflict,
    "set completion did not preserve newer edit as a conflict");
var setRefreshRow = TypedRow("Value.Value", "Value", "A", "string");
setRaceNode.ReplaceTypedPropertyRows(
    [setRefreshRow],
    preservePendingEdits: true);
setRaceNode.SettleCompletedPropertyMutations();
setRaceRow = setRaceNode.FindProperty("Value.Value")!;
Assert(setRaceRow.Value == "A" && setRaceRow.EditText == "B",
    "follow-up set refresh suppressed preservation of a later edit");

var revertedSetNode = new ElementNodeViewModel();
var revertedSetRow = TypedRow("Value.Value", "Value", "old", "string");
revertedSetNode.ReplaceTypedPropertyRows([revertedSetRow]);
revertedSetRow.EditText = "A";
var revertedSetRevision = revertedSetRow.EditRevision;
var revertedSetMutation = revertedSetNode.BeginPropertyMutation(
    "Value.Value", revertedSetRevision);
revertedSetRow.EditText = "old";
Assert(!revertedSetRow.IsDirty,
    "set revision regression did not return to a clean provider value");
Assert(revertedSetNode.TryCompletePropertyMutation(revertedSetMutation),
    "reverted set mutation was unexpectedly superseded");
revertedSetRow.ApplyMutationValue("A", revertedSetRevision);
Assert(revertedSetRow.Value == "A" && revertedSetRow.EditText == "old",
    "set completion overwrote a newer clean edit action");
Assert(revertedSetRow.IsDirty && revertedSetRow.HasExternalConflict,
    "set completion did not compare the newer action with direct readback");
revertedSetNode.ReplaceTypedPropertyRows(
    [TypedRow("Value.Value", "Value", "A", "string")],
    preservePendingEdits: true);
revertedSetNode.SettleCompletedPropertyMutations();
revertedSetRow = revertedSetNode.FindProperty("Value.Value")!;
Assert(revertedSetRow.EditText == "old" && revertedSetRow.IsDirty,
    "set refresh ignored a newer revision because it was previously non-dirty");

var clearRaceNode = new ElementNodeViewModel();
var clearRaceRow = TypedRow("Value.Value", "Value", "effective", "string");
clearRaceNode.ReplaceTypedPropertyRows([clearRaceRow]);
clearRaceRow.EditText = "clear-A";
var submittedClearRevision = clearRaceRow.EditRevision;
var clearRaceMutation = clearRaceNode.BeginPropertyMutation(
    "Value.Value", submittedClearRevision);
clearRaceRow.EditText = "clear-B";
Assert(clearRaceNode.TryCompletePropertyMutation(clearRaceMutation),
    "clear race mutation was unexpectedly superseded");
Assert(!clearRaceRow.TryDiscardSubmittedEdit(submittedClearRevision),
    "clear completion discarded a newer edit");
var clearRefreshRow = TypedRow("Value.Value", "Value", "default", "string");
clearRaceNode.ReplaceTypedPropertyRows(
    [clearRefreshRow],
    preservePendingEdits: true);
clearRaceNode.SettleCompletedPropertyMutations();
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
var revertedClearMutation = revertedClearNode.BeginPropertyMutation(
    "Value.Value", revertedClearRevision);
revertedClearRow.EditText = "temporary";
revertedClearRow.EditText = "effective";
Assert(!revertedClearRow.IsDirty,
    "clear revision regression did not return to a clean provider value");
Assert(revertedClearNode.TryCompletePropertyMutation(revertedClearMutation),
    "reverted clear mutation was unexpectedly superseded");
Assert(!revertedClearRow.TryDiscardSubmittedEdit(revertedClearRevision),
    "clear completion discarded a newer clean edit action");
revertedClearNode.ReplaceTypedPropertyRows(
    [TypedRow("Value.Value", "Value", "default", "string")],
    preservePendingEdits: true);
revertedClearNode.SettleCompletedPropertyMutations();
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
var commandMutation = commandNode.BeginPropertyMutation(
    "Selection.Command", submittedCommandRevision);
commandRow.EditText = "remove";
Assert(commandNode.TryCompletePropertyMutation(commandMutation),
    "command mutation was unexpectedly superseded");
commandRow.ApplyMutationValue("", submittedCommandRevision);
Assert(commandRow.EditText == "remove" && !commandRow.IsDirty,
    "command completion overwrote a newer non-dirty action");
commandNode.ReplaceTypedPropertyRows(
    [
        TypedRow(
            "Selection.Command", "Selection", "", "command",
            ("add", "Add"), ("remove", "Remove")),
    ],
    preservePendingEdits: true);
commandRow = commandNode.FindProperty("Selection.Command")!;
Assert(commandRow.EditText == "remove" && !commandRow.IsDirty,
    "command refresh ignored a newer action because command rows are non-dirty");
commandNode.SettleCompletedPropertyMutations();

var disappearingCommandNode = new ElementNodeViewModel();
var disappearingCommandRow = TypedRow(
    "Selection.Command", "Selection", "", "command",
    ("add", "Add"), ("remove", "Remove"));
disappearingCommandNode.ReplaceTypedPropertyRows([disappearingCommandRow]);
var disappearingCommandRevision = disappearingCommandRow.EditRevision;
var disappearingCommandMutation =
    disappearingCommandNode.BeginPropertyMutation(
        "Selection.Command", disappearingCommandRevision);
disappearingCommandRow.EditText = "remove";
Assert(disappearingCommandNode.TryCompletePropertyMutation(
        disappearingCommandMutation),
    "disappearing command mutation was unexpectedly superseded");
disappearingCommandRow.ApplyMutationValue(
    "", disappearingCommandRevision);
disappearingCommandNode.ReplaceTypedPropertyRows(
    [],
    preservePendingEdits: true);
disappearingCommandRow =
    disappearingCommandNode.FindProperty("Selection.Command")!;
Assert(disappearingCommandRow.EditText == "remove",
    "disappearing command descriptor lost a newer action");
Assert(disappearingCommandRow.Kind == PropertyEditorKind.ReadOnly,
    "disappearing command descriptor was not retained");

var exactCommandNode = new ElementNodeViewModel();
var exactCommandRow = TypedRow(
    "Selection.Command", "Selection", "", "command",
    ("add", "Add"), ("remove", "Remove"));
exactCommandNode.ReplaceTypedPropertyRows([exactCommandRow]);
exactCommandRow.EditText = "remove";
var exactCommandRevision = exactCommandRow.EditRevision;
var exactCommandMutation = exactCommandNode.BeginPropertyMutation(
    "Selection.Command", exactCommandRevision);
Assert(exactCommandNode.TryCompletePropertyMutation(exactCommandMutation),
    "exact command mutation was unexpectedly superseded");
exactCommandRow.ApplyMutationValue("", exactCommandRevision);
exactCommandNode.ReplaceTypedPropertyRows(
    [
        TypedRow(
            "Selection.Command", "Selection", "", "command",
            ("add", "Add"), ("remove", "Remove")),
    ],
    preservePendingEdits: true);
exactCommandNode.SettleCompletedPropertyMutations();
exactCommandRow = exactCommandNode.FindProperty("Selection.Command")!;
Assert(exactCommandRow.EditText == "add" &&
       !exactCommandRow.HasPendingAction,
    "exact command completion did not accept the submitted action");

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
delayedSetRow.EditText = "A";
var delayedSetRevision = delayedSetRow.EditRevision;
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
var delayedSetMutation = delayedSetNode.BeginPropertyMutation(
    "Value.Value", delayedSetRevision);
var delayedSetProtection = delayedSetState.Request();
Assert(delayedSetNode.PropertyVersion != delayedSetOldVersion,
    "set submission did not invalidate the pre-mutation property version");
Assert(!delayedSetState.IsCurrent(delayedSetOldToken),
    "set submission did not retire the pre-mutation refresh generation");
releaseDelayedSet.SetResult();
Assert(!await delayedSetOldSnapshot,
    "delayed pre-set snapshot applied while the mutation was awaiting MCP");
Assert(delayedSetState.Complete(delayedSetOldToken, applied: false),
    "delayed pre-set snapshot did not release the refresh coordinator");
Assert(delayedSetState.TryBegin(out var delayedSetProtectionToken) &&
       delayedSetProtectionToken == delayedSetProtection,
    "set protection refresh was not queued at mutation submission");
var delayedSetProtectionApplied = await ApplyDelayedTypedSnapshotAsync(
    Task.CompletedTask,
    delayedSetNode,
    delayedSetNode.PropertyVersion,
    delayedSetState,
    delayedSetProtectionToken,
    [TypedRow("Value.Value", "Value", "old", "string")]);
delayedSetState.Complete(
    delayedSetProtectionToken, delayedSetProtectionApplied);
delayedSetRow = delayedSetNode.FindProperty("Value.Value")!;
Assert(delayedSetRow.EditText == "A" && delayedSetRow.IsDirty,
    "refresh during set await discarded the submitted dirty edit");
delayedSetRow.EditText = "newer";
Assert(delayedSetNode.TryCompletePropertyMutation(delayedSetMutation),
    "set completion lost its in-flight mutation context");
delayedSetRow.ApplyMutationValue("A", delayedSetRevision);
var delayedSetFollowup = delayedSetState.Request();
Assert(delayedSetState.TryBegin(out var delayedSetFollowupToken) &&
       delayedSetFollowupToken == delayedSetFollowup,
    "set completion did not enqueue its settlement refresh");
var delayedSetApplied = await ApplyDelayedTypedSnapshotAsync(
    Task.CompletedTask,
    delayedSetNode,
    delayedSetNode.PropertyVersion,
    delayedSetState,
    delayedSetFollowupToken,
    [TypedRow("Value.Value", "Value", "A", "string")]);
delayedSetState.Complete(delayedSetFollowupToken, delayedSetApplied);
delayedSetNode.SettleCompletedPropertyMutations();
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
var delayedClearRevision = delayedClearRow.EditRevision;
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
var delayedClearMutation = delayedClearNode.BeginPropertyMutation(
    "Value.Value", delayedClearRevision);
var delayedClearProtection = delayedClearState.Request();
delayedClearRow.EditText = "temporary";
delayedClearRow.EditText = "effective";
var delayedClearNewerRevision = delayedClearRow.EditRevision;
Assert(!delayedClearRow.IsDirty,
    "clear await setup did not return to a non-dirty newer action");
Assert(delayedClearNode.PropertyVersion != delayedClearOldVersion,
    "clear submission did not invalidate the pre-mutation property version");
Assert(!delayedClearState.IsCurrent(delayedClearOldToken),
    "clear submission did not retire the pre-mutation refresh generation");
releaseDelayedClear.SetResult();
Assert(!await delayedClearOldSnapshot,
    "delayed pre-clear snapshot applied while the mutation was awaiting MCP");
Assert(delayedClearState.Complete(delayedClearOldToken, applied: false),
    "delayed pre-clear snapshot did not release the refresh coordinator");
Assert(delayedClearState.TryBegin(out var delayedClearProtectionToken) &&
       delayedClearProtectionToken == delayedClearProtection,
    "clear protection refresh was not queued at mutation submission");
var delayedClearProtectionApplied = await ApplyDelayedTypedSnapshotAsync(
    Task.CompletedTask,
    delayedClearNode,
    delayedClearNode.PropertyVersion,
    delayedClearState,
    delayedClearProtectionToken,
    [TypedRow("Value.Value", "Value", "effective", "string")]);
delayedClearState.Complete(
    delayedClearProtectionToken, delayedClearProtectionApplied);
delayedClearRow = delayedClearNode.FindProperty("Value.Value")!;
Assert(delayedClearRow.EditRevision == delayedClearNewerRevision &&
       delayedClearRow.EditText == "effective" &&
       !delayedClearRow.IsDirty,
    "refresh during clear await lost a newer non-dirty edit revision");
Assert(delayedClearNode.TryCompletePropertyMutation(delayedClearMutation),
    "clear completion lost its in-flight mutation context");
Assert(!delayedClearRow.TryDiscardSubmittedEdit(delayedClearRevision),
    "clear completion discarded the newer delayed-race action");
var delayedClearFollowup = delayedClearState.Request();
Assert(delayedClearState.TryBegin(out var delayedClearFollowupToken) &&
       delayedClearFollowupToken == delayedClearFollowup,
    "clear completion did not enqueue its settlement refresh");
var delayedClearApplied = await ApplyDelayedTypedSnapshotAsync(
    Task.CompletedTask,
    delayedClearNode,
    delayedClearNode.PropertyVersion,
    delayedClearState,
    delayedClearFollowupToken,
    [TypedRow("Value.Value", "Value", "default", "string")]);
delayedClearState.Complete(delayedClearFollowupToken, delayedClearApplied);
delayedClearNode.SettleCompletedPropertyMutations();
delayedClearRow = delayedClearNode.FindProperty("Value.Value")!;
Assert(delayedClearRow.Value == "default" &&
       delayedClearRow.EditText == "effective" &&
       delayedClearRow.IsDirty &&
       delayedClearRow.HasExternalConflict,
    "serialized clear follow-up lost provider readback or the newer edit");

var inFlightCommandNode = new ElementNodeViewModel();
var inFlightCommandRow = TypedRow(
    "Selection.Command", "Selection", "", "command",
    ("add", "Add"), ("remove", "Remove"));
inFlightCommandNode.ReplaceTypedPropertyRows([inFlightCommandRow]);
var inFlightCommandRevision = inFlightCommandRow.EditRevision;
var inFlightCommandMutation = inFlightCommandNode.BeginPropertyMutation(
    "Selection.Command", inFlightCommandRevision);
inFlightCommandRow.EditText = "remove";
var inFlightCommandNewerRevision = inFlightCommandRow.EditRevision;
inFlightCommandNode.SetProperty("Selection.Command", "stale patch");
Assert(inFlightCommandRow.EditText == "remove" &&
       inFlightCommandRow.EditRevision == inFlightCommandNewerRevision,
    "live patch during command await overwrote the newer action");
inFlightCommandNode.ReplacePropertyRows(
    [new PropertyRowViewModel("Selection.Command", "stale full load")],
    preserveTypedRows: true);
inFlightCommandRow =
    inFlightCommandNode.FindProperty("Selection.Command")!;
Assert(inFlightCommandRow.EditText == "remove" &&
       inFlightCommandRow.EditRevision == inFlightCommandNewerRevision &&
       inFlightCommandRow.IsTypedProperty,
    "full UIA refresh during command await replaced the protected row");
inFlightCommandNode.ReplaceTypedPropertyRows(
    [
        TypedRow(
            "Selection.Command", "Selection", "", "command",
            ("add", "Add"), ("remove", "Remove")),
    ],
    preservePendingEdits: true);
inFlightCommandRow =
    inFlightCommandNode.FindProperty("Selection.Command")!;
Assert(inFlightCommandRow.EditText == "remove" &&
       inFlightCommandRow.EditRevision == inFlightCommandNewerRevision,
    "typed refresh during command await overwrote the newer action");
Assert(inFlightCommandNode.TryCompletePropertyMutation(
        inFlightCommandMutation),
    "command completion lost its in-flight mutation context");
inFlightCommandRow.ApplyMutationValue("", inFlightCommandRevision);
inFlightCommandNode.ReplaceTypedPropertyRows(
    [
        TypedRow(
            "Selection.Command", "Selection", "", "command",
            ("add", "Add"), ("remove", "Remove")),
    ],
    preservePendingEdits: true);
inFlightCommandNode.SettleCompletedPropertyMutations();
inFlightCommandRow =
    inFlightCommandNode.FindProperty("Selection.Command")!;
Assert(inFlightCommandRow.EditText == "remove",
    "command settlement lost the newer action");
Assert(inFlightCommandRow.HasPendingAction,
    "command settlement forgot that the newer action is still pending");
inFlightCommandNode.ReplaceTypedPropertyRows(
    [
        TypedRow(
            "Selection.Command", "Selection", "", "command",
            ("add", "Add"), ("remove", "Remove")),
    ],
    preservePendingEdits: true);
inFlightCommandRow =
    inFlightCommandNode.FindProperty("Selection.Command")!;
Assert(inFlightCommandRow.EditText == "remove",
    "later refresh lost a settled command action");
inFlightCommandNode.ReplaceTypedPropertyRows(
    [],
    preservePendingEdits: true);
inFlightCommandRow =
    inFlightCommandNode.FindProperty("Selection.Command")!;
Assert(inFlightCommandRow.EditText == "remove" &&
       inFlightCommandRow.Kind == PropertyEditorKind.ReadOnly,
    "later descriptor disappearance lost a settled command action");

var coalescedNode = new ElementNodeViewModel();
var coalescedSetRow = TypedRow(
    "Value.Value", "Value", "old", "string");
var coalescedClearRow = TypedRow(
    "RangeValue.Value", "Range", "10", "number");
coalescedNode.ReplaceTypedPropertyRows(
    [coalescedSetRow, coalescedClearRow]);
coalescedSetRow.EditText = "A";
var coalescedSetRevision = coalescedSetRow.EditRevision;
var coalescedSetMutation = coalescedNode.BeginPropertyMutation(
    "Value.Value", coalescedSetRevision);
var coalescedClearRevision = coalescedClearRow.EditRevision;
var coalescedClearMutation = coalescedNode.BeginPropertyMutation(
    "RangeValue.Value", coalescedClearRevision);
coalescedClearRow.EditText = "12";
coalescedClearRow.EditText = "10";
Assert(coalescedNode.TryCompletePropertyMutation(coalescedSetMutation),
    "first coalesced row mutation was lost");
coalescedSetRow.ApplyMutationValue("A", coalescedSetRevision);
Assert(coalescedNode.TryCompletePropertyMutation(coalescedClearMutation),
    "second coalesced row mutation was lost");
Assert(!coalescedClearRow.TryDiscardSubmittedEdit(
        coalescedClearRevision),
    "coalesced clear discarded its newer revision");
coalescedNode.ReplaceTypedPropertyRows(
    [
        TypedRow("Value.Value", "Value", "A", "string"),
        TypedRow("RangeValue.Value", "Range", "0", "number"),
    ],
    preservePendingEdits: true);
coalescedSetRow = coalescedNode.FindProperty("Value.Value")!;
coalescedClearRow =
    coalescedNode.FindProperty("RangeValue.Value")!;
Assert(coalescedSetRow.Value == "A" && !coalescedSetRow.IsDirty,
    "coalesced refresh did not accept the exact set revision");
Assert(coalescedClearRow.Value == "0" &&
       coalescedClearRow.EditText == "10" &&
       coalescedClearRow.IsDirty,
    "coalesced refresh did not preserve the clear row's newer action");
coalescedNode.SettleCompletedPropertyMutations();
coalescedNode.SetProperty("Value.Value", "after settlement");
Assert(coalescedSetRow.EditText == "after settlement" &&
       !coalescedSetRow.IsDirty,
    "successful coalesced refresh did not clear completed contexts");

var sameRowNode = new ElementNodeViewModel();
var sameRow = TypedRow("Value.Value", "Value", "old", "string");
sameRowNode.ReplaceTypedPropertyRows([sameRow]);
sameRow.EditText = "A";
var sameRowRevision = sameRow.EditRevision;
var supersededSet = sameRowNode.BeginPropertyMutation(
    "Value.Value", sameRowRevision);
var latestClear = sameRowNode.BeginPropertyMutation(
    "Value.Value", sameRowRevision);
Assert(!sameRowNode.TryCompletePropertyMutation(supersededSet),
    "older same-row operation was accepted after supersession");
Assert(sameRowNode.TryCompletePropertyMutation(latestClear),
    "latest same-row operation was not accepted");
Assert(sameRow.TryDiscardSubmittedEdit(sameRowRevision),
    "latest same-row clear did not discard its exact submitted edit");
sameRowNode.ReplaceTypedPropertyRows(
    [TypedRow("Value.Value", "Value", "default", "string")],
    preservePendingEdits: true);
sameRowNode.SettleCompletedPropertyMutations();
sameRow = sameRowNode.FindProperty("Value.Value")!;
Assert(sameRow.Value == "default" &&
       sameRow.EditText == "default" &&
       !sameRow.IsDirty,
    "same-row latest operation did not deterministically win");

var visualSetNode = new ElementNodeViewModel();
var visualSetRow = TypedRow("Value.Value", "Value", "old", "string");
visualSetNode.ReplaceTypedPropertyRows([visualSetRow]);
visualSetRow.EditText = "visual set";
var visualSetRevision = visualSetRow.EditRevision;
var visualSetMutation = visualSetNode.BeginPropertyMutation(
    "Value.Value", visualSetRevision);
Assert(visualSetNode.TryCompletePropertyMutation(visualSetMutation),
    "visual set mutation was unexpectedly superseded");
visualSetRow.ApplyMutationValue("visual set", visualSetRevision);
visualSetNode.ReplaceTypedPropertyRows(
    [TypedRow("Value.Value", "Value", "visual set", "string")],
    preservePendingEdits: true);
visualSetNode.SettleCompletedPropertyMutations();
visualSetRow = visualSetNode.FindProperty("Value.Value")!;
Assert(visualSetRow.Value == "visual set" &&
       visualSetRow.EditText == "visual set" &&
       !visualSetRow.IsDirty,
    "generic visual set refresh did not settle provider readback");

var visualClearNode = new ElementNodeViewModel();
var visualClearRow = TypedRow(
    "Value.Value", "Value", "effective", "string");
visualClearNode.ReplaceTypedPropertyRows([visualClearRow]);
var visualClearRevision = visualClearRow.EditRevision;
var visualClearMutation = visualClearNode.BeginPropertyMutation(
    "Value.Value", visualClearRevision);
Assert(visualClearNode.TryCompletePropertyMutation(visualClearMutation),
    "visual clear mutation was unexpectedly superseded");
Assert(visualClearRow.TryDiscardSubmittedEdit(visualClearRevision),
    "visual clear did not discard its exact submitted revision");
visualClearNode.ReplaceTypedPropertyRows(
    [TypedRow("Value.Value", "Value", "visual default", "string")],
    preservePendingEdits: true);
visualClearNode.SettleCompletedPropertyMutations();
visualClearRow = visualClearNode.FindProperty("Value.Value")!;
Assert(visualClearRow.Value == "visual default" &&
       visualClearRow.EditText == "visual default" &&
       !visualClearRow.IsDirty,
    "generic visual clear refresh left the stale effective value");

var deselectedNode = new ElementNodeViewModel();
var deselectedRow = TypedRow(
    "Value.Value", "Value", "selected value", "string");
deselectedNode.ReplaceTypedPropertyRows([deselectedRow]);
var deselectedRevision = deselectedRow.EditRevision;
var deselectedMutation = deselectedNode.BeginPropertyMutation(
    "Value.Value", deselectedRevision);
deselectedRow.EditText = "temporary";
deselectedRow.EditText = "selected value";
Assert(!deselectedRow.IsDirty,
    "selection-change setup did not create a newer non-dirty action");
deselectedNode.ClearPropertyMutations();
Assert(!deselectedNode.TryCompletePropertyMutation(deselectedMutation),
    "operation completion retained ownership after node deselection");
deselectedNode.ReplaceTypedPropertyRows(
    [TypedRow("Value.Value", "Value", "new owner", "string")],
    preservePendingEdits: true);
deselectedRow = deselectedNode.FindProperty("Value.Value")!;
Assert(deselectedRow.Value == "new owner" &&
       deselectedRow.EditText == "new owner" &&
       !deselectedRow.IsDirty,
    "deselected node retained stale mutation preservation state");

var resetTree = new LiveTree();
resetTree.Apply(new TreeChangeEventDto
{
    Event = "added",
    Key = "reset-node",
    Path = "0",
    Element = new ElementDto { Type = "Edit", Framework = "uia" },
});
var resetNode = resetTree.Roots[0];
var resetMutation = resetNode.BeginPropertyMutation("Value.Value", 0);
resetTree.Reset();
Assert(!resetNode.TryCompletePropertyMutation(resetMutation),
    "LiveTree reset retained a mutation from the prior session");

var failedRefreshNode = new ElementNodeViewModel();
var failedRefreshRow = TypedRow(
    "Value.Value", "Value", "effective", "string");
failedRefreshNode.ReplaceTypedPropertyRows([failedRefreshRow]);
var failedRefreshRevision = failedRefreshRow.EditRevision;
var failedRefreshMutation = failedRefreshNode.BeginPropertyMutation(
    "Value.Value", failedRefreshRevision);
failedRefreshRow.EditText = "temporary";
failedRefreshRow.EditText = "effective";
Assert(!failedRefreshRow.IsDirty,
    "failed-refresh setup did not return to a non-dirty newer action");
Assert(failedRefreshNode.TryCompletePropertyMutation(
        failedRefreshMutation),
    "failed-refresh mutation was unexpectedly superseded");
Assert(!failedRefreshRow.TryDiscardSubmittedEdit(failedRefreshRevision),
    "failed refresh setup discarded the newer non-dirty action");
var failedRefreshState = new TypedPropertyRefreshState();
failedRefreshState.Request();
Assert(failedRefreshState.TryBegin(out var failedRefreshAttempt),
    "failed typed refresh did not start");
Assert(failedRefreshState.Complete(
        failedRefreshAttempt, applied: false),
    "failed typed refresh did not release the retry coordinator");
Assert(failedRefreshState.HasPending,
    "failed typed refresh was incorrectly marked applied");
Assert(failedRefreshState.TryBegin(out var successfulRetry),
    "failed typed refresh did not retry");
var successfulRetryApplied = await ApplyDelayedTypedSnapshotAsync(
    Task.CompletedTask,
    failedRefreshNode,
    failedRefreshNode.PropertyVersion,
    failedRefreshState,
    successfulRetry,
    [TypedRow("Value.Value", "Value", "default", "string")]);
Assert(failedRefreshState.Complete(
        successfulRetry, successfulRetryApplied),
    "successful typed retry did not complete");
failedRefreshNode.SettleCompletedPropertyMutations();
failedRefreshRow =
    failedRefreshNode.FindProperty("Value.Value")!;
Assert(failedRefreshRow.Value == "default" &&
       failedRefreshRow.EditText == "effective" &&
       failedRefreshRow.IsDirty,
    "successful retry did not preserve the action retained by the failure");
failedRefreshRow.EditText = "default";
failedRefreshNode.SetProperty("Value.Value", "after retry");
Assert(failedRefreshRow.EditText == "after retry" &&
       !failedRefreshRow.IsDirty,
    "successful retry did not settle its completed mutation context");

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
