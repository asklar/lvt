namespace LvtViewer.ViewModels;

/// <summary>
/// One checkbox in the framework-filter dropdown: a framework/content type
/// discovered in the live tree so far (e.g. "win32", "xaml", "winui3",
/// "wpf", "comctl", "dui"), and whether it is currently included. Only
/// meaningful in visual-tree mode (see MainViewModel.IsFrameworkFilterActive)
/// — UIA's tree does not carry the same per-node framework distinction.
/// </summary>
public sealed class FrameworkFilterOption : ObservableObject
{
    private bool _isChecked = true;

    public FrameworkFilterOption(string name) => Name = name;

    public string Name { get; }

    public bool IsChecked
    {
        get => _isChecked;
        set => SetField(ref _isChecked, value);
    }
}
