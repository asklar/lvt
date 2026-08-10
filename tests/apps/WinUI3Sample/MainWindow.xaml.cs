using Microsoft.UI.Xaml;

namespace WinUI3Sample;

public sealed partial class MainWindow : Window
{
    private int _clicks;

    public MainWindow()
    {
        InitializeComponent();
        Title = "LVT WinUI3 Sample";
    }

    // Interaction tests need an effect a UIA walk can observe.
    private void OnPrimaryClick(object sender, RoutedEventArgs e)
    {
        StatusText.Text = $"clicks:{++_clicks}";
    }
}
