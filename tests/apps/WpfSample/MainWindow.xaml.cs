using System;
using System.Diagnostics;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Interop;

namespace WpfSample;

public partial class MainWindow : Window
{
    internal const int ReparentElementMessage = 0x84D1;
    private static MainWindow primaryWindow = null!;
    private static MainWindow secondaryWindow = null!;
    private readonly EventWaitHandle blockTrigger;
    private readonly EventWaitHandle blockEntered;
    private readonly EventWaitHandle blockRelease;

    public string OrdinaryClrProperty { get; set; } = "not a dependency property";

    public MainWindow()
        : this(false)
    {
    }

    internal MainWindow(bool secondary)
    {
        InitializeComponent();
        if (secondary)
        {
            secondaryWindow = this;
            Name = "SecondaryWindowRoot";
            Title = "LVT WPF Secondary";
            WindowStartupLocation = WindowStartupLocation.Manual;
            Left = 620;
            Top = 140;
            NameBox.Name = "SecondaryNameBox";
            NameBox.Text = "Secondary Ada";
            OkButton.Name = "SecondaryOkButton";
        }
        else
        {
            primaryWindow = this;
        }
        SourceInitialized += (_, _) =>
        {
            HwndSource source = HwndSource.FromHwnd(
                new WindowInteropHelper(this).Handle);
            source?.AddHook(WindowMessage);
        };
        var prefix = $@"Local\LvtWpfSampleUiBlock_{Process.GetCurrentProcess().Id}";
        blockTrigger = new EventWaitHandle(
            false, EventResetMode.AutoReset, prefix + "_trigger");
        blockEntered = new EventWaitHandle(
            false, EventResetMode.ManualReset, prefix + "_entered");
        blockRelease = new EventWaitHandle(
            false, EventResetMode.AutoReset, prefix + "_release");
        _ = Task.Run(() =>
        {
            while (blockTrigger.WaitOne())
            {
                Dispatcher.BeginInvoke(new Action(() =>
                {
                    blockEntered.Set();
                    blockRelease.WaitOne();
                    blockEntered.Reset();
                }));
            }
        });
    }

    private nint WindowMessage(
        nint hwnd, int message, nint wParam,
        nint lParam, ref bool handled)
    {
        if (message != ReparentElementMessage ||
            !ReferenceEquals(this, primaryWindow) ||
            secondaryWindow == null)
        {
            return 0;
        }

        RootLayout.Children.Remove(NameBox);
        secondaryWindow.RootLayout.Children.Add(NameBox);
        Grid.SetRow(NameBox, 0);
        Grid.SetColumn(NameBox, 1);
        handled = true;
        return 1;
    }
}
