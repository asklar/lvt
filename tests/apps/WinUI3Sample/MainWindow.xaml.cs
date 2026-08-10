using System.Collections.Generic;
using Microsoft.UI.Xaml;

namespace WinUI3Sample;

public sealed partial class MainWindow : Window
{
    private int _clicks;

    public MainWindow()
    {
        InitializeComponent();
        Title = "LVT WinUI3 Sample";

        // Long enough that the list virtualizes, so VirtualizedItem.Realize and
        // scrolling have something real to act on.
        var items = new List<string>();
        for (int i = 0; i < 200; i++)
            items.Add($"Item {i:D3}");
        ItemsList.ItemsSource = items;
    }

    // Interaction tests need an effect a UIA walk can observe.
    private void OnPrimaryClick(object sender, RoutedEventArgs e)
    {
        StatusText.Text = $"clicks:{++_clicks}";
    }
}
