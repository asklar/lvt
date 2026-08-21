using System;
using System.Globalization;
using System.Windows;
using System.Windows.Data;
using LvtViewer.ViewModels;

namespace LvtViewer.Converters;

/// <summary>
/// Shows an element in the property panel's row template only when the
/// row's <see cref="PropertyEditKind"/> matches the converter parameter
/// ("Toggle" or "TextValue"), so one DataTemplate can host all three
/// row shapes without a DataTemplateSelector.
/// </summary>
public sealed class EditKindToVisibilityConverter : IValueConverter
{
    public object Convert(object value, Type targetType, object parameter, CultureInfo culture)
    {
        if (value is not PropertyEditKind kind || parameter is not string wanted)
            return Visibility.Collapsed;
        return kind.ToString() == wanted ? Visibility.Visible : Visibility.Collapsed;
    }

    public object ConvertBack(object value, Type targetType, object parameter, CultureInfo culture)
        => throw new NotSupportedException();
}
