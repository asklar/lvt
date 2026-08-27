using System;
using System.Diagnostics;
using System.Text;
using System.Threading.Tasks;

namespace LvtViewer.Services;

public sealed record LvtCliResult(int ExitCode, string StdOut, string StdErr)
{
    public bool Ok => ExitCode == 0;
}

/// <summary>
/// Runs lvt.exe as a short-lived subprocess for one-shot verbs (the action
/// verbs used by property editing: "toggle", "set-value"). The long-running
/// "watch" verb is handled separately by <see cref="WatchSession"/>.
/// </summary>
public sealed class LvtCli
{
    private readonly string _exePath;

    public LvtCli(string exePath) => _exePath = exePath;

    public async Task<LvtCliResult> RunAsync(params string[] args)
    {
        var psi = new ProcessStartInfo
        {
            FileName = _exePath,
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8,
            StandardErrorEncoding = Encoding.UTF8,
            CreateNoWindow = true,
        };
        foreach (var arg in args)
            psi.ArgumentList.Add(arg);

        using var process = new Process { StartInfo = psi };
        var stdout = new StringBuilder();
        var stderr = new StringBuilder();
        process.OutputDataReceived += (_, e) => { if (e.Data != null) stdout.AppendLine(e.Data); };
        process.ErrorDataReceived += (_, e) => { if (e.Data != null) stderr.AppendLine(e.Data); };

        try
        {
            process.Start();
        }
        catch (Exception ex)
        {
            return new LvtCliResult(-1, "", $"could not start '{_exePath}': {ex.Message}");
        }

        process.BeginOutputReadLine();
        process.BeginErrorReadLine();
        await process.WaitForExitAsync();
        return new LvtCliResult(process.ExitCode, stdout.ToString(), stderr.ToString());
    }
}
