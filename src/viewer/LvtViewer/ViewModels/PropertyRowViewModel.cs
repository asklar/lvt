using LvtViewer.Models;

namespace LvtViewer.ViewModels;

/// <summary>
/// How (if at all) the property panel lets a row be edited. Kept to a closed
/// set of properties lvt already knows how to write via an action verb
/// (src/providers/uia_actions.cpp) — never an arbitrary property.
/// </summary>
public enum PropertyEditKind
{
    None,
    /// <summary>Toggle.ToggleState — flips via the "toggle" verb.</summary>
    Toggle,
    /// <summary>Value.Value / RangeValue.Value — set via the "set-value" verb.</summary>
    TextValue,
    /// <summary>A writable scalar XAML/WinUI dependency property.</summary>
    VisualValue,
}

/// <summary>One row in the property panel: a name/value pair, plus how it can be edited.</summary>
public sealed class PropertyRowViewModel : ObservableObject
{
    private string _value = "";
    private string _editText = "";

    public PropertyRowViewModel(string name, string value)
    {
        Name = name;
        Kind = ClassifyEditKind(name);
        Value = value;
    }

    public string Name { get; }

    private PropertyEditKind _kind;
    public PropertyEditKind Kind
    {
        get => _kind;
        private set
        {
            if (SetField(ref _kind, value))
                OnPropertyChanged(nameof(IsEditable));
        }
    }

    public bool IsEditable => Kind != PropertyEditKind.None;
    public bool IsVisualProperty { get; private set; }
    public bool CanClear { get; private set; }
    public uint PropertyIndex { get; private set; }
    public string ValueType { get; private set; } = "";
    public string DeclaringType { get; private set; } = "";
    public string Source { get; private set; } = "";
    public ulong MetadataBits { get; private set; }

    /// <summary>The last known-committed value, as reported by lvt.</summary>
    public string Value
    {
        get => _value;
        set
        {
            if (SetField(ref _value, value))
            {
                // Keep the edit box in sync with the live value until the user
                // starts typing a pending edit of their own.
                EditText = value;
            }
        }
    }

    /// <summary>The text currently in the edit box, for TextValue rows (not yet applied).</summary>
    public string EditText
    {
        get => _editText;
        set => SetField(ref _editText, value);
    }

    public void UpdateVisualProperty(VisualPropertyDto property)
    {
        const ulong IsPropertyReadOnly = 0x2;
        const ulong IsValueHandle = 0x1;
        const ulong IsValueCollection = 0x4;
        const ulong IsValueCollectionReadOnly = 0x8;
        const ulong IsValueBindingExpression = 0x10;

        IsVisualProperty = true;
        PropertyIndex = property.PropertyIndex;
        ValueType = property.ValueType;
        DeclaringType = property.DeclaringType;
        Source = property.Source;
        MetadataBits = property.MetadataBits;
        CanClear = property.Overridden;

        bool writableScalar =
            (MetadataBits & (IsValueHandle | IsPropertyReadOnly | IsValueCollection |
                             IsValueCollectionReadOnly | IsValueBindingExpression)) == 0 &&
            ValueType.Length != 0;
        Kind = writableScalar ? PropertyEditKind.VisualValue : PropertyEditKind.None;
        Value = property.Value;

        OnPropertyChanged(nameof(IsVisualProperty));
        OnPropertyChanged(nameof(CanClear));
        OnPropertyChanged(nameof(PropertyIndex));
        OnPropertyChanged(nameof(ValueType));
        OnPropertyChanged(nameof(DeclaringType));
        OnPropertyChanged(nameof(Source));
        OnPropertyChanged(nameof(MetadataBits));
    }

    private static PropertyEditKind ClassifyEditKind(string name) => name switch
    {
        "Toggle.ToggleState" => PropertyEditKind.Toggle,
        "Value.Value" => PropertyEditKind.TextValue,
        "RangeValue.Value" => PropertyEditKind.TextValue,
        _ => PropertyEditKind.None,
    };
}
