using System;
using System.Diagnostics;
using System.Globalization;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using LvtViewer.Models;

namespace LvtViewer.Services;

/// <summary>
/// Runs "lvt watch" as a long-lived subprocess and turns its stdout — one
/// JSON change event per line, per watch_diff.cpp's serialize_change_event —
/// into <see cref="WatchEventDto"/> events on this class.
///
/// This is the viewer's *only* tree data source (see README.md): the first
/// burst of "added" events a freshly started `lvt watch` emits (run_watch_loop
/// -> snapshot_added_events) already is a full, self-consistent snapshot of
/// the current tree, and every later line is a true incremental diff against
/// that exact same internal walk. Seeding from a separate "lvt dump" call
/// first and then layering "lvt watch" on top would race two independent
/// walks of the target and could leave stale nodes with no way to know they
/// were ever removed, so lvt dump is deliberately not used here.
/// </summary>
public sealed class WatchSession : IDisposable
{
    private Process? _process;
    private CancellationTokenSource? _cts;

    public event Action<WatchEventDto>? EventReceived;
    public event Action<string>? DiagnosticReceived;
    public event Action<int>? Exited;

    public bool IsRunning => _process is { HasExited: false };

    public void Start(string exePath, string hwndHex, bool uia, int intervalMs = 500)
    {
        Stop();

        var psi = new ProcessStartInfo
        {
            FileName = exePath,
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true,
        };
        psi.ArgumentList.Add("watch");
        psi.ArgumentList.Add("--hwnd");
        psi.ArgumentList.Add(hwndHex);
        if (uia)
            psi.ArgumentList.Add("--uia");
        psi.ArgumentList.Add("--interval");
        psi.ArgumentList.Add(intervalMs.ToString(CultureInfo.InvariantCulture));

        var process = new Process { StartInfo = psi, EnableRaisingEvents = true };
        var cts = new CancellationTokenSource();
        _process = process;
        _cts = cts;

        process.Exited += (_, _) =>
        {
            try
            {
                Exited?.Invoke(process.ExitCode);
            }
            catch
            {
                // Best-effort notification; never let a handler fault the Exited callback.
            }
        };

        try
        {
            process.Start();
        }
        catch (Exception ex)
        {
            DiagnosticReceived?.Invoke($"could not start '{exePath}': {ex.Message}");
            _process = null;
            return;
        }

        _ = PumpStdOutAsync(process, cts.Token);
        _ = PumpStdErrAsync(process, cts.Token);
    }

    public void Stop()
    {
        _cts?.Cancel();
        _cts = null;

        var process = _process;
        _process = null;
        if (process == null)
            return;

        try
        {
            if (!process.HasExited)
                process.Kill(entireProcessTree: true);
        }
        catch
        {
            // Already exited, or exiting racily — nothing further to do.
        }
        finally
        {
            process.Dispose();
        }
    }

    private async Task PumpStdOutAsync(Process process, CancellationToken token)
    {
        try
        {
            while (!token.IsCancellationRequested)
            {
                var line = await process.StandardOutput.ReadLineAsync(token).ConfigureAwait(false);
                if (line == null)
                    break; // stdout closed: the process is exiting
                if (string.IsNullOrWhiteSpace(line))
                    continue;

                WatchEventDto? evt;
                try
                {
                    evt = JsonSerializer.Deserialize<WatchEventDto>(line, JsonDefaults.Options);
                }
                catch (JsonException)
                {
                    continue; // tolerate a stray non-JSON line rather than tearing down the session
                }
                if (evt != null)
                    EventReceived?.Invoke(evt);
            }
        }
        catch (OperationCanceledException)
        {
            // Expected on Stop().
        }
    }

    private async Task PumpStdErrAsync(Process process, CancellationToken token)
    {
        try
        {
            while (!token.IsCancellationRequested)
            {
                var line = await process.StandardError.ReadLineAsync(token).ConfigureAwait(false);
                if (line == null)
                    break;
                if (!string.IsNullOrWhiteSpace(line))
                    DiagnosticReceived?.Invoke(line);
            }
        }
        catch (OperationCanceledException)
        {
            // Expected on Stop().
        }
    }

    public void Dispose() => Stop();
}
