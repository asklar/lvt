using System;
using System.Globalization;
using System.Windows.Data;
using System.Windows.Media;

namespace LvtViewer.Converters;

/// <summary>Maps an lvt Element's "framework" string to a small color swatch in the tree view.</summary>
public sealed class FrameworkToBrushConverter : IValueConverter
{
    private static readonly Brush Default = Brushes.Gray;

    public object Convert(object value, Type targetType, object parameter, CultureInfo culture)
    {
        var framework = (value as string ?? "").ToLowerInvariant();
        return framework switch
        {
            "win32" => Brushes.SlateGray,
            "comctl" => Brushes.SteelBlue,
            "uia" => Brushes.MediumPurple,
            "xaml" => Brushes.DarkOrange,
            "winui3" => Brushes.OrangeRed,
            "wpf" => Brushes.MediumSeaGreen,
            "winforms" => Brushes.Goldenrod,
            "avalonia" => Brushes.DeepPink,
            "chromium" => Brushes.DodgerBlue,
            _ => Default,
        };
    }

    public object ConvertBack(object value, Type targetType, object parameter, CultureInfo culture)
        => throw new NotSupportedException();
}
