using System;
using System.Globalization;
using System.Windows;
using System.Windows.Data;
using LvtViewer.Models;

namespace LvtViewer.Converters;

/// <summary>
/// Shows an element in the property panel's row template only when the
/// row's <see cref="PropertyEditorKind"/> matches the converter parameter,
/// so one DataTemplate can host the provider-neutral typed editors and the
/// temporary legacy UIA editors
/// row shapes without a DataTemplateSelector.
/// </summary>
public sealed class EditKindToVisibilityConverter : IValueConverter
{
    public object Convert(object value, Type targetType, object parameter, CultureInfo culture)
    {
        if (value is not PropertyEditorKind kind || parameter is not string wanted)
            return Visibility.Collapsed;
        foreach (var candidate in wanted.Split('|', StringSplitOptions.RemoveEmptyEntries))
        {
            if (kind.ToString() == candidate)
                return Visibility.Visible;
        }
        return Visibility.Collapsed;
    }

    public object ConvertBack(object value, Type targetType, object parameter, CultureInfo culture)
        => throw new NotSupportedException();
}
