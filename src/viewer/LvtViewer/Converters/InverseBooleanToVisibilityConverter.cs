using System;
using System.Globalization;
using System.Windows;
using System.Windows.Data;

namespace LvtViewer.Converters;

/// <summary>
/// Inverse of the standard BooleanToVisibilityConverter: true -&gt; Collapsed,
/// false -&gt; Visible. Used for the "editing needs UI Automation tree" hint
/// (item 3), which should show only when NOT in UIA mode.
/// </summary>
public sealed class InverseBooleanToVisibilityConverter : IValueConverter
{
    public object Convert(object value, Type targetType, object parameter, CultureInfo culture)
        => value is bool b && b ? Visibility.Collapsed : Visibility.Visible;

    public object ConvertBack(object value, Type targetType, object parameter, CultureInfo culture)
        => throw new NotSupportedException();
}
