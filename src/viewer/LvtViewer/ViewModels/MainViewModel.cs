using System;
using System.Collections.ObjectModel;
using System.Diagnostics;
using System.Globalization;
using System.Windows.Threading;
using LvtViewer.Models;
using LvtViewer.Services;

namespace LvtViewer.ViewModels;

/// <summary>
/// Top-level view model: owns the resolved lvt.exe path, the current
/// target's watch session, the live tree, and the property-editing actions.
/// </summary>
public sealed class MainViewModel : ObservableObject, IDisposable
{
    private readonly Dispatcher _dispatcher;
    private readonly LiveTree _liveTree = new();
    private readonly WatchSession _watch = new();
    private readonly LvtCli _cli;

    private string _statusText = "Drag the crosshair onto a window to inspect it.";
    private string _targetText = "No target";
    private bool _useUia = true;
    private ElementNodeViewModel? _selectedElement;
    private string? _currentHwndHex;

    public MainViewModel(Dispatcher dispatcher)
    {
        _dispatcher = dispatcher;
        LvtExePath = LvtLocator.Find();
        _cli = new LvtCli(LvtExePath);

        _watch.EventReceived += evt => _dispatcher.BeginInvoke(() => _liveTree.Apply(evt));
        _watch.DiagnosticReceived += line => _dispatcher.BeginInvoke(() => StatusText = line);
        _watch.Exited += code => _dispatcher.BeginInvoke(() =>
            StatusText = $"lvt watch exited (code {code}) — target likely closed. Re-pick a window to reconnect.");

        ToggleCommand = new RelayCommand(p => _ = ToggleAsync(p as PropertyRowViewModel));
        SetValueCommand = new RelayCommand(p => _ = SetValueAsync(p as PropertyRowViewModel));
        ReconnectCommand = new RelayCommand(_ => Reconnect(), _ => _currentHwndHex != null);
    }

    public string LvtExePath { get; }

    public ObservableCollection<ElementNodeViewModel> Roots => _liveTree.Roots;

    public string StatusText
    {
        get => _statusText;
        set => SetField(ref _statusText, value);
    }

    public string TargetText
    {
        get => _targetText;
        set => SetField(ref _targetText, value);
    }

    public bool UseUia
    {
        get => _useUia;
        set
        {
            if (SetField(ref _useUia, value) && _currentHwndHex != null)
                Reconnect();
        }
    }

    public ElementNodeViewModel? SelectedElement
    {
        get => _selectedElement;
        set => SetField(ref _selectedElement, value);
    }

    public RelayCommand ToggleCommand { get; }
    public RelayCommand SetValueCommand { get; }
    public RelayCommand ReconnectCommand { get; }

    /// <summary>Called by the crosshair picker once a target window is resolved.</summary>
    public void ConnectTo(IntPtr hwnd)
    {
        NativeMethodsWindowInfo(hwnd, out var pid, out var title);

        _currentHwndHex = "0x" + hwnd.ToInt64().ToString("X", CultureInfo.InvariantCulture);

        string processName = "?";
        try
        {
            processName = Process.GetProcessById((int)pid).ProcessName;
        }
        catch
        {
            // Access-denied or the process just exited; keep the placeholder.
        }

        TargetText = $"{processName} (pid {pid})  —  hwnd {_currentHwndHex}  —  \"{title}\"";
        StatusText = "Connecting…";
        _liveTree.Reset();
        _watch.Start(LvtExePath, _currentHwndHex, UseUia);
        StatusText = UseUia
            ? "Watching the UI Automation tree live."
            : "Watching the visual tree live.";
    }

    private void Reconnect()
    {
        if (_currentHwndHex == null)
            return;
        _liveTree.Reset();
        StatusText = "Reconnecting…";
        _watch.Start(LvtExePath, _currentHwndHex, UseUia);
        StatusText = UseUia
            ? "Watching the UI Automation tree live."
            : "Watching the visual tree live.";
    }

    private static void NativeMethodsWindowInfo(IntPtr hwnd, out uint pid, out string title)
    {
        Interop.NativeMethods.GetWindowThreadProcessId(hwnd, out pid);
        title = Interop.NativeMethods.GetWindowTitle(hwnd);
    }

    private async System.Threading.Tasks.Task ToggleAsync(PropertyRowViewModel? row)
    {
        if (row == null || SelectedElement == null || _currentHwndHex == null)
            return;
        StatusText = $"Toggling {SelectedElement.DisplayName}…";
        var result = await _cli.RunAsync("toggle", SelectedElement.Key, "--hwnd", _currentHwndHex);
        StatusText = result.Ok
            ? "Toggled. The live tree will confirm the new state shortly."
            : $"Toggle failed: {result.StdErr.Trim()}";
    }

    private async System.Threading.Tasks.Task SetValueAsync(PropertyRowViewModel? row)
    {
        if (row == null || SelectedElement == null || _currentHwndHex == null)
            return;
        StatusText = $"Setting {row.Name} on {SelectedElement.DisplayName}…";
        var result = await _cli.RunAsync(
            "set-value", SelectedElement.Key, row.EditText, "--hwnd", _currentHwndHex);
        StatusText = result.Ok
            ? "Value set. The live tree will confirm it shortly."
            : $"set-value failed: {result.StdErr.Trim()}";
    }

    public void Dispose() => _watch.Dispose();
}
