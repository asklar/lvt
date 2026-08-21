using System.Windows;
using LvtViewer.Interop;
using LvtViewer.ViewModels;

namespace LvtViewer;

/// <summary>
/// Interaction logic for MainWindow.xaml. Wires the crosshair picker (which
/// needs a real HWND and mouse-capture semantics, so it lives in code-behind
/// rather than being bindable from the view model) and the TreeView's
/// selection (TreeView.SelectedItem has no setter, so it cannot be bound
/// directly either).
/// </summary>
public partial class MainWindow : Window
{
    private readonly MainViewModel _viewModel;
    private CrosshairPicker? _picker;

    public MainWindow()
    {
        InitializeComponent();
        _viewModel = new MainViewModel(Dispatcher);
        DataContext = _viewModel;

        Tree.SelectedItemChanged += (_, e) =>
            _viewModel.SelectedElement = e.NewValue as ElementNodeViewModel;

        Loaded += (_, _) =>
        {
            _picker = new CrosshairPicker(CrosshairHandle, this);
            _picker.TargetPicked += hwnd => _viewModel.ConnectTo(hwnd);
            _picker.HintChanged += hint => _viewModel.StatusText = hint;
        };

        Closed += (_, _) => _viewModel.Dispose();
    }
}