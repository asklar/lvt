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

    public PropertyEditKind Kind { get; }

    public bool IsEditable => Kind != PropertyEditKind.None;

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

    private static PropertyEditKind ClassifyEditKind(string name) => name switch
    {
        "Toggle.ToggleState" => PropertyEditKind.Toggle,
        "Value.Value" => PropertyEditKind.TextValue,
        "RangeValue.Value" => PropertyEditKind.TextValue,
        _ => PropertyEditKind.None,
    };
}
