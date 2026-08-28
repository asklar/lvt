using System;
using System.Diagnostics;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;

namespace WpfSample;

public partial class MainWindow : Window
{
    private readonly EventWaitHandle blockTrigger;
    private readonly EventWaitHandle blockEntered;
    private readonly EventWaitHandle blockRelease;

    public string OrdinaryClrProperty { get; set; } = "not a dependency property";

    public MainWindow()
    {
        InitializeComponent();
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
}
