using System.Collections.Generic;
using System.ComponentModel;
using System.Runtime.CompilerServices;

namespace LvtViewer.ViewModels;

/// <summary>
/// Minimal INotifyPropertyChanged base. Hand-rolled rather than pulling in
/// CommunityToolkit.Mvvm so the viewer has no NuGet dependency beyond the
/// WPF/BCL surface that ships with the .NET SDK.
/// </summary>
public abstract class ObservableObject : INotifyPropertyChanged
{
    public event PropertyChangedEventHandler? PropertyChanged;

    protected void OnPropertyChanged([CallerMemberName] string? name = null) =>
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));

    protected bool SetField<T>(ref T field, T value, [CallerMemberName] string? name = null)
    {
        if (EqualityComparer<T>.Default.Equals(field, value))
            return false;
        field = value;
        OnPropertyChanged(name);
        return true;
    }
}
