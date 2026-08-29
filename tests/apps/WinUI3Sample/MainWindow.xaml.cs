using System.Collections.Generic;
using Microsoft.UI.Xaml;

namespace WinUI3Sample;

public sealed partial class MainWindow : Window
{
    private int _clicks;
    private readonly List<string> _items = [];
    private bool _listIsSmall;

    public MainWindow()
    {
        InitializeComponent();
        Title = "LVT WinUI3 Sample";

        // Long enough that the list virtualizes, so VirtualizedItem.Realize and
        // scrolling have something real to act on.
        for (int i = 0; i < 200; i++)
            _items.Add($"Item {i:D3}");
        ItemsList.ItemsSource = _items;
    }

    // Interaction tests need an effect a UIA walk can observe.
    private void OnPrimaryClick(object sender, RoutedEventArgs e)
    {
        StatusText.Text = $"clicks:{++_clicks}";
    }

    private void OnToggleListSizeClick(object sender, RoutedEventArgs e)
    {
        _listIsSmall = !_listIsSmall;
        ItemsList.ItemsSource = _listIsSmall ? _items.GetRange(0, 1) : _items;
    }
}
