using System;
using System.Configuration;
using System.Data;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Threading;
using LvtViewer.Services;

namespace LvtViewer;

/// <summary>
/// Interaction logic for App.xaml
/// </summary>
public partial class App : Application
{
    public App()
    {
        // No logging existed anywhere in the viewer before this — a crash or
        // a live-only bug (a tree rebuild that disrupts navigation, the
        // crosshair picker going unexpectedly disabled) had no trail to
        // diagnose from afterward. These three handlers are what let an
        // otherwise-silent crash leave a record instead of just vanishing.
        DispatcherUnhandledException += (_, e) =>
        {
            Logger.LogException("app", "Unhandled UI-thread exception", e.Exception);
            // Intentionally not marking e.Handled = true: swallowing it would
            // hide the crash's real cause from the user too, and the log
            // entry above already preserves it either way.
        };
        AppDomain.CurrentDomain.UnhandledException += (_, e) =>
        {
            if (e.ExceptionObject is Exception ex)
                Logger.LogException("app", "Unhandled non-UI-thread exception", ex);
        };
        TaskScheduler.UnobservedTaskException += (_, e) =>
        {
            Logger.LogException("app", "Unobserved task exception", e.Exception);
            e.SetObserved();
        };
        Logger.Log("app", $"Starting, log file: {Logger.Path_}");
    }
}

