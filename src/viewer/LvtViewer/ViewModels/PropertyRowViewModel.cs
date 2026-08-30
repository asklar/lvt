using System;
using System.Collections.Generic;
using System.Globalization;
using LvtViewer.Models;

namespace LvtViewer.ViewModels;

/// <summary>One property row plus its provider-selected editor presentation.</summary>
public sealed class PropertyRowViewModel : ObservableObject
{
    private static readonly IReadOnlyList<PropertyChoiceDto> BooleanChoices =
    [
        new() { Value = "false", Label = "False" },
        new() { Value = "true", Label = "True" },
    ];

    private string _value = "";
    private string _editText = "";
    private string _validationError = "";
    private PropertyEditorKind _kind;
    private bool _isDirty;
    private bool _hasExternalConflict;
    private bool _hasPendingAction;
    private long _editRevision;

    public PropertyRowViewModel(string name, string value)
    {
        Name = name;
        ProviderName = name;
        Kind = PropertyEditorKind.ReadOnly;
        Value = value;
    }

    public string Name { get; private set; }
    public string ProviderName { get; private set; }

    public PropertyEditorKind Kind
    {
        get => _kind;
        private set
        {
            if (SetField(ref _kind, value))
            {
                OnPropertyChanged(nameof(IsEditable));
                OnPropertyChanged(nameof(CanApply));
            }
        }
    }

    public bool IsEditable => Kind != PropertyEditorKind.ReadOnly;
    public bool IsTypedProperty { get; private set; }
    public long EditRevision => _editRevision;
    public bool IsDirty
    {
        get => _isDirty;
        private set => SetField(ref _isDirty, value);
    }
    public bool HasExternalConflict
    {
        get => _hasExternalConflict;
        private set
        {
            if (SetField(ref _hasExternalConflict, value))
                OnPropertyChanged(nameof(ConflictMessage));
        }
    }
    public bool HasPendingAction
    {
        get => _hasPendingAction;
        private set => SetField(ref _hasPendingAction, value);
    }
    public string ConflictMessage => HasExternalConflict
        ? $"Target changed to “{Value}” while your pending edit is “{EditText}”."
        : "";
    public string DescriptorId { get; private set; } = "";
    public string PropertyType { get; private set; } = "";
    public string DeclaringType { get; private set; } = "";
    public string Source { get; private set; } = "";
    public string Description { get; private set; } = "";
    public bool CanClear { get; private set; }
    public double? Minimum { get; private set; }
    public double? Maximum { get; private set; }
    public double? Step { get; private set; }
    public IReadOnlyList<PropertyChoiceDto> Choices { get; private set; } = [];

    public string Details
    {
        get
        {
            var typeAndSource = string.Join(
                " · ",
                new[] { PropertyType, Source }
                    .Where(value => !string.IsNullOrWhiteSpace(value)));
            return string.IsNullOrWhiteSpace(Description)
                ? typeAndSource
                : string.IsNullOrWhiteSpace(typeAndSource)
                    ? Description
                    : $"{typeAndSource} · {Description}";
        }
    }

    /// <summary>The last value reported by the provider.</summary>
    public string Value
    {
        get => _value;
        set => UpdateProviderValue(value, preservePendingEdit: false);
    }

    /// <summary>The pending value in an editor, before Set is invoked.</summary>
    public string EditText
    {
        get => _editText;
        set => SetEditText(value, incrementRevision: true);
    }

    public string ValidationError
    {
        get => _validationError;
        private set
        {
            if (SetField(ref _validationError, value))
                OnPropertyChanged(nameof(CanApply));
        }
    }

    public bool CanApply =>
        IsTypedProperty &&
        Kind != PropertyEditorKind.ReadOnly &&
        ValidationError.Length == 0;

    public void UpdateTypedProperty(
        PropertyDescriptorDto descriptor, PropertyValueDto value)
    {
        IsTypedProperty = true;
        ProviderName = descriptor.Name;
        Name = string.IsNullOrWhiteSpace(descriptor.DisplayName)
            ? descriptor.Name
            : descriptor.DisplayName;
        DescriptorId = descriptor.DescriptorId;
        PropertyType = descriptor.PropertyType;
        DeclaringType = descriptor.DeclaringType;
        Source = value.Source;
        Description = value.UnavailableReason.Length != 0
            ? value.UnavailableReason
            : value.ReadOnlyReason.Length != 0
                ? value.ReadOnlyReason
                : descriptor.Description;
        CanClear = descriptor.SupportsClear && value.CanClear;
        Minimum = descriptor.Minimum;
        Maximum = descriptor.Maximum;
        Step = descriptor.Step;
        Choices = descriptor.EditorKind == PropertyEditorKind.Boolean &&
                  descriptor.Choices.Count == 0
            ? BooleanChoices
            : descriptor.Choices;
        Kind = descriptor.EditorKind == PropertyEditorKind.Enumeration &&
               Choices.Count == 0
            ? PropertyEditorKind.ReadOnly
            : descriptor.EditorKind;
        if (descriptor.EditorKind == PropertyEditorKind.Enumeration &&
            Choices.Count == 0)
        {
            Description = string.IsNullOrWhiteSpace(Description)
                ? "The provider did not supply enum choices."
                : $"{Description} · The provider did not supply enum choices.";
        }
        UpdateProviderValue(value.Value, preservePendingEdit: false);
        if (Kind == PropertyEditorKind.Command && Choices.Count != 0)
            SetEditText(Choices[0].Value, incrementRevision: false);
        Validate();

        OnPropertyChanged(nameof(Name));
        OnPropertyChanged(nameof(ProviderName));
        OnPropertyChanged(nameof(IsTypedProperty));
        OnPropertyChanged(nameof(DescriptorId));
        OnPropertyChanged(nameof(PropertyType));
        OnPropertyChanged(nameof(DeclaringType));
        OnPropertyChanged(nameof(Source));
        OnPropertyChanged(nameof(Description));
        OnPropertyChanged(nameof(Details));
        OnPropertyChanged(nameof(CanClear));
        OnPropertyChanged(nameof(Minimum));
        OnPropertyChanged(nameof(Maximum));
        OnPropertyChanged(nameof(Step));
        OnPropertyChanged(nameof(Choices));
        OnPropertyChanged(nameof(CanApply));
    }

    public void UpdateProviderValue(
        string value,
        bool preservePendingEdit,
        bool preserveCurrentAction = false)
    {
        var wasDirty = IsDirty;
        var pending = EditText;
        var changed = SetField(ref _value, value, nameof(Value));
        var shouldPreserve =
            preservePendingEdit && (wasDirty || preserveCurrentAction);

        if (!shouldPreserve)
        {
            SetEditText(value, incrementRevision: false);
            if (changed)
            {
                Validate();
                UpdateDirtyState();
            }
            HasPendingAction = false;
            HasExternalConflict = false;
            return;
        }

        if (!changed)
            return;

        Validate();
        UpdateDirtyState();
        HasExternalConflict =
            IsDirty &&
            !string.Equals(pending, value, StringComparison.Ordinal);
        OnPropertyChanged(nameof(ConflictMessage));
    }

    public void PreservePendingEditFrom(PropertyRowViewModel existing)
    {
        _editRevision = existing.EditRevision;
        OnPropertyChanged(nameof(EditRevision));
        SetEditText(existing.EditText, incrementRevision: false);
        HasPendingAction = existing.HasPendingAction;
        HasExternalConflict =
            IsDirty &&
            (existing.HasExternalConflict ||
             !string.Equals(existing.Value, Value, StringComparison.Ordinal));
    }

    public void MarkUnavailable(string reason)
    {
        Kind = PropertyEditorKind.ReadOnly;
        CanClear = false;
        Description = string.IsNullOrWhiteSpace(Description)
            ? reason
            : $"{Description} · {reason}";
        HasExternalConflict = IsDirty;
        OnPropertyChanged(nameof(CanClear));
        OnPropertyChanged(nameof(Description));
        OnPropertyChanged(nameof(Details));
        OnPropertyChanged(nameof(CanApply));
    }

    public void DiscardPendingEdit()
    {
        SetEditText(Value, incrementRevision: false);
        HasPendingAction = false;
        HasExternalConflict = false;
    }

    public void ApplyMutationValue(string value, long submittedRevision)
    {
        bool hasNewerAction = EditRevision != submittedRevision;
        if (!hasNewerAction)
        {
            UpdateProviderValue(value, preservePendingEdit: false);
            return;
        }

        var newerEditText = EditText;
        var newerEditRevision = EditRevision;
        var hasPendingAction = HasPendingAction;
        UpdateProviderValue(value, preservePendingEdit: false);
        _editRevision = newerEditRevision;
        OnPropertyChanged(nameof(EditRevision));
        SetEditText(newerEditText, incrementRevision: false);
        HasPendingAction = hasPendingAction;
        if (IsDirty)
            HasExternalConflict = true;
    }

    public bool TryDiscardSubmittedEdit(long submittedRevision)
    {
        if (EditRevision != submittedRevision)
        {
            if (IsDirty)
                HasExternalConflict = true;
            return false;
        }
        DiscardPendingEdit();
        return true;
    }

    private void Validate()
    {
        string error = "";
        if (Kind == PropertyEditorKind.Boolean &&
            !string.Equals(EditText, "true", StringComparison.OrdinalIgnoreCase) &&
            !string.Equals(EditText, "false", StringComparison.OrdinalIgnoreCase)) {
            error = "Choose True or False.";
        } else if (Kind == PropertyEditorKind.Integer) {
            if (!decimal.TryParse(
                    EditText, NumberStyles.Integer, CultureInfo.InvariantCulture,
                    out var number)) {
                error = "Enter a whole number.";
            } else {
                error = ValidateRange((double)number);
            }
        } else if (Kind == PropertyEditorKind.Number) {
            if (!double.TryParse(
                    EditText, NumberStyles.Float, CultureInfo.InvariantCulture,
                    out var number) ||
                !double.IsFinite(number)) {
                error = "Enter a finite number.";
            } else {
                error = ValidateRange(number);
            }
        } else if ((Kind == PropertyEditorKind.Enumeration ||
                    Kind == PropertyEditorKind.Command) &&
                   !Choices.Any(choice =>
                       string.Equals(
                           choice.Value, EditText, StringComparison.Ordinal))) {
            error = "Choose a provider-supplied option.";
        }
        ValidationError = error;
    }

    private string ValidateRange(double number)
    {
        if (Minimum is double minimum && number < minimum)
            return $"Minimum: {minimum.ToString(CultureInfo.InvariantCulture)}.";
        if (Maximum is double maximum && number > maximum)
            return $"Maximum: {maximum.ToString(CultureInfo.InvariantCulture)}.";
        return "";
    }

    private void UpdateDirtyState()
    {
        IsDirty =
            IsTypedProperty &&
            Kind != PropertyEditorKind.Command &&
            !string.Equals(EditText, Value, StringComparison.Ordinal);
        if (!IsDirty)
            HasExternalConflict = false;
        OnPropertyChanged(nameof(ConflictMessage));
    }

    private void SetEditText(string value, bool incrementRevision)
    {
        if (!SetField(ref _editText, value, nameof(EditText)))
            return;
        if (incrementRevision)
        {
            ++_editRevision;
            OnPropertyChanged(nameof(EditRevision));
            if (Kind == PropertyEditorKind.Command)
                HasPendingAction = true;
        }
        Validate();
        UpdateDirtyState();
    }

}
