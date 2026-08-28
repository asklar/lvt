using System.Windows;

namespace WpfSample;

public partial class MainWindow : Window
{
    public string OrdinaryClrProperty { get; set; } = "not a dependency property";

    public MainWindow()
    {
        InitializeComponent();
    }
}
