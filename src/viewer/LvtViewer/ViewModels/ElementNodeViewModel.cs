using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Linq;
using LvtViewer.Models;

namespace LvtViewer.ViewModels;

/// <summary>
/// One node of the live element tree shown in the TreeView, and the source
/// of the property panel when selected. Instances are long-lived: the same
/// object is reused across watch ticks (keyed by lvt's durable "key"), so
/// updating its bound properties in place is what makes the TreeView/property
/// panel refresh live without losing selection or expansion state.
/// </summary>
public sealed class ElementNodeViewModel : ObservableObject
{
    private string _id = "";
    private string _type = "";
    private string _framework = "";
    private string _className = "";
    private string _text = "";
    private int _x, _y, _width, _height;

    /// <summary>lvt's durable, path-based identity key. Never changes for this node.</summary>
    public string Key { get; internal set; } = "";

    /// <summary>
    /// lvt's positional dot-path (e.g. "0.2.1"), used only to reconstruct
    /// parent/child relationships from the flat event stream. Not bound to UI.
    /// </summary>
    public string Path { get; internal set; } = "";

    /// <summary>
    /// This node's parent in the current hierarchy, kept in sync by
    /// LiveTree.RebuildHierarchy. Null for a root. Used to expand a node's
    /// ancestor chain when programmatically selecting it in the TreeView
    /// (see MainWindow.SelectElementInTree, item 2's point-to-select).
    /// </summary>
    public ElementNodeViewModel? Parent { get; internal set; }

    public string Id
    {
        get => _id;
        set => SetField(ref _id, value);
    }

    public string Type
    {
        get => _type;
        set
        {
            if (SetField(ref _type, value))
                OnPropertyChanged(nameof(DisplayName));
        }
    }

    public string Framework
    {
        get => _framework;
        set => SetField(ref _framework, value);
    }

    public string ClassName
    {
        get => _className;
        set => SetField(ref _className, value);
    }

    public string Text
    {
        get => _text;
        set
        {
            if (SetField(ref _text, value))
                OnPropertyChanged(nameof(DisplayName));
        }
    }

    public int BoundsX
    {
        get => _x;
        set { if (SetField(ref _x, value)) OnPropertyChanged(nameof(BoundsText)); }
    }

    public int BoundsY
    {
        get => _y;
        set { if (SetField(ref _y, value)) OnPropertyChanged(nameof(BoundsText)); }
    }

    public int BoundsWidth
    {
        get => _width;
        set { if (SetField(ref _width, value)) OnPropertyChanged(nameof(BoundsText)); }
    }

    public int BoundsHeight
    {
        get => _height;
        set { if (SetField(ref _height, value)) OnPropertyChanged(nameof(BoundsText)); }
    }

    public string BoundsText => $"{BoundsX}, {BoundsY}, {BoundsWidth} x {BoundsHeight}";

    // Checked in priority order when Text is empty, so structural containers
    // (Grid, Border, StackPanel, ...) that never carry visible text still
    // show *something* identifying in the tree — a UIA/AutomationProperties
    // name, an AutomationId, or the XAML developer's x:Name — rather than a
    // bare, indistinguishable "Grid" repeated at every level.
    private static readonly string[] IdentifyingPropertyPriority =
    {
        "AutomationProperties.Name",
        "Name",
        "AutomationProperties.AutomationId",
        "AutomationId",
        "name", // x:Name, captured as a plain property by the XAML TAP (see xaml_diag_common.cpp)
    };

    /// <summary>What the TreeView shows for this node.</summary>
    public string DisplayName
    {
        get
        {
            if (!string.IsNullOrEmpty(Text))
                return $"{Type} \"{Text}\"";
            foreach (var propName in IdentifyingPropertyPriority)
            {
                var row = FindProperty(propName);
                if (row != null && !string.IsNullOrEmpty(row.Value))
                    return $"{Type} \"{row.Value}\"";
            }
            return Type;
        }
    }

    public ObservableCollection<PropertyRowViewModel> PropertyRows { get; } = new();

    public ObservableCollection<ElementNodeViewModel> Children { get; } = new();

    private bool _isExpanded;

    /// <summary>Whether this node's TreeViewItem starts expanded. Set once for the root by LiveTree.</summary>
    public bool IsExpanded
    {
        get => _isExpanded;
        set => SetField(ref _isExpanded, value);
    }

    private bool _isVisible = true;

    /// <summary>
    /// Whether the framework filter (MainViewModel.ApplyFrameworkFilter)
    /// keeps this node in the TreeView. True whenever no filter is active
    /// (e.g. UIA mode), this node's own framework passes the filter, or any
    /// descendant's does — a container should stay visible to reach a
    /// matching descendant even if its own type was filtered out.
    /// </summary>
    public bool IsVisible
    {
        get => _isVisible;
        set => SetField(ref _isVisible, value);
    }

    /// <summary>Applies a full element snapshot (from an "added" event or a full dump).</summary>
    public void UpdateFrom(ElementDto dto, string key, string path)
    {
        Key = key;
        Path = path;
        Id = dto.Id;
        Type = dto.Type;
        Framework = dto.Framework;
        ClassName = dto.ClassName ?? "";
        Text = dto.Text ?? "";
        BoundsX = dto.Bounds.X;
        BoundsY = dto.Bounds.Y;
        BoundsWidth = dto.Bounds.Width;
        BoundsHeight = dto.Bounds.Height;

        var seen = new HashSet<string>();
        if (dto.Properties != null)
        {
            foreach (var (name, value) in dto.Properties)
            {
                SetProperty(name, value);
                seen.Add(name);
            }
        }
        // Drop rows for properties that no longer appear at all (rare on a
        // full snapshot, but keeps a re-added node from carrying stale rows).
        foreach (var stale in PropertyRows.Where(r => !seen.Contains(r.Name) && !r.IsEditable).ToList())
            PropertyRows.Remove(stale);
    }

    /// <summary>Applies one changed-field update by name ("type", "framework", "className", "text").</summary>
    public void SetScalarField(string fieldName, string newValue)
    {
        switch (fieldName)
        {
            case "type": Type = newValue; break;
            case "framework": Framework = newValue; break;
            case "className": ClassName = newValue; break;
            case "text": Text = newValue; break;
        }
    }

    /// <summary>Parses lvt's "x,y,width,height" bounds string (watch_diff.cpp's bounds_to_string).</summary>
    public void SetBoundsFromString(string value)
    {
        var parts = value.Split(',');
        if (parts.Length != 4)
            return;
        if (int.TryParse(parts[0], out var x)) BoundsX = x;
        if (int.TryParse(parts[1], out var y)) BoundsY = y;
        if (int.TryParse(parts[2], out var w)) BoundsWidth = w;
        if (int.TryParse(parts[3], out var h)) BoundsHeight = h;
    }

    /// <summary>Upserts a property row, or removes a non-editable row whose value went empty.</summary>
    public void SetProperty(string name, string value)
    {
        var row = PropertyRows.FirstOrDefault(r => r.Name == name);
        if (row == null)
        {
            if (value.Length == 0)
                return; // never was there; nothing to show
            PropertyRows.Add(new PropertyRowViewModel(name, value));
            NotifyIfIdentifyingProperty(name);
            return;
        }

        if (value.Length == 0 && !row.IsEditable)
        {
            PropertyRows.Remove(row);
            NotifyIfIdentifyingProperty(name);
            return;
        }
        row.Value = value;
        NotifyIfIdentifyingProperty(name);
    }

    /// <summary>
    /// DisplayName reads PropertyRows directly rather than being a bound
    /// field of its own, so a live update to one of the properties it falls
    /// back to (when Text is empty) needs its own change notification —
    /// unlike Type/Text, which already raise this via their own setters.
    /// </summary>
    private void NotifyIfIdentifyingProperty(string name)
    {
        if (Array.IndexOf(IdentifyingPropertyPriority, name) >= 0)
            OnPropertyChanged(nameof(DisplayName));
    }

    public PropertyRowViewModel? FindProperty(string name) =>
        PropertyRows.FirstOrDefault(r => r.Name == name);
}
