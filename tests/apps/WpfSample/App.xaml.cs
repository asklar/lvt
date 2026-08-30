using System;
using System.Windows;
using System.Windows.Threading;

namespace WpfSample;

public partial class App : Application
{
    private Window secondaryWindow = null!;

    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        if (Environment.GetEnvironmentVariable("LVT_TEST_SECONDARY_WPF_WINDOW") != "1")
            return;

        Dispatcher.BeginInvoke(
            DispatcherPriority.ApplicationIdle,
            new Action(() =>
            {
                secondaryWindow = new MainWindow(true);
                secondaryWindow.Show();
            }));
    }
}
