using System;
using System.IO;
using System.Text;

namespace LvtViewer.Services;

/// <summary>
/// Minimal file-based diagnostic log for the viewer, in the same spirit as
/// the TAP DLL's %TEMP%\lvt_tap.log (see lvt_tap.cpp's LogMsg): a plain,
/// timestamped, append-only text file is what actually let live bugs get
/// root-caused this session (the runaway-cycle investigation, the DPI/
/// coordinate-space bugs), because it captures what really happened across
/// a run instead of relying on reproducing a bug live under a debugger.
///
/// The viewer previously had no logging at all, so live-only bugs (a tree
/// rebuild that resets navigation state, the crosshair picker going
/// unexpectedly disabled) had no trail to diagnose from after the fact.
///
/// Written to %TEMP%\lvt_viewer_&lt;pid&gt;.log — pid-suffixed, unlike the
/// TAP DLL's log, because the viewer is not injected into a single target
/// process; several viewer instances (or several across relaunches during
/// development) can be live at once, and interleaving their output into one
/// file would make any single run's story impossible to follow.
/// </summary>
public static class Logger
{
    private static readonly object Gate = new();
    private static readonly string LogPath = Path.Combine(
        Path.GetTempPath(), $"lvt_viewer_{Environment.ProcessId}.log");
    private static readonly System.Diagnostics.Stopwatch Clock = System.Diagnostics.Stopwatch.StartNew();

    public static string Path_ => LogPath;

    /// <summary>
    /// Logs one line, tagged with an elapsed-ms timestamp (comparable across
    /// a single run the way the TAP DLL's GetTickCount64 timestamps are) and
    /// a short category so a log can be filtered/grepped by subsystem
    /// (e.g. "watch", "tree", "picker", "highlight").
    /// </summary>
    public static void Log(string category, string message)
    {
        var line = $"[{Clock.ElapsedMilliseconds}][{category}] {message}";
        lock (Gate)
        {
            try
            {
                File.AppendAllText(LogPath, line + Environment.NewLine, Encoding.UTF8);
            }
            catch
            {
                // A logging failure must never take down the app it exists to help debug.
            }
        }
    }

    /// <summary>Logs an exception with its type, message, and stack trace.</summary>
    public static void LogException(string category, string context, Exception ex)
    {
        Log(category, $"{context}: {ex.GetType().Name}: {ex.Message}\n{ex.StackTrace}");
    }
}
