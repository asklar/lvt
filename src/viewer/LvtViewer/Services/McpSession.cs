using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using LvtViewer.Models;

namespace LvtViewer.Services;

public sealed record McpStartResult(bool Ok, string Error = "");

public sealed record McpToolResult(bool Ok, JsonElement Payload, string Error = "");

/// <summary>
/// One long-lived MCP conversation with `lvt mcp --allow-input`.
///
/// The viewer intentionally uses the same public MCP surface an agent does:
/// connect establishes a session, a mode-specific tree resource supplies the
/// initial snapshot and later patches, resources/updated wakes the client
/// without client-side polling, and property/action tools run through that
/// same session. This replaces the former split architecture of `lvt watch`
/// plus unrelated one-shot CLI processes.
/// </summary>
public sealed class McpSession : IAsyncDisposable, IDisposable
{
    private readonly object _stateGate = new();
    private readonly Dictionary<long, TaskCompletionSource<JsonElement>> _pending = new();
    private readonly SemaphoreSlim _writeGate = new(1, 1);
    private readonly SemaphoreSlim _lifecycleGate = new(1, 1);
    private readonly SemaphoreSlim _resourceReadGate = new(1, 1);

    private Process? _process;
    private CancellationTokenSource? _cts;
    private string? _sessionId;
    private string? _resourceUri;
    private long _nextRequestId;
    private int _generation;
    private bool _resourceReadRunning;
    private bool _resourceReadPending;
    private int _disposed;

    public event Action<int, TreePatchDto>? PatchReceived;
    public event Action<string>? DiagnosticReceived;
    public event Action<int>? Exited;

    public string? SessionId
    {
        get
        {
            lock (_stateGate)
                return _sessionId;
        }
    }

    public int CurrentGeneration => Volatile.Read(ref _generation);

    public async Task<McpStartResult> StartAsync(
        string exePath, string hwndHex, bool uia, CancellationToken cancellationToken = default)
    {
        ObjectDisposedException.ThrowIf(Volatile.Read(ref _disposed) != 0, this);
        // A new target/mode supersedes an in-flight slow connection
        // immediately, rather than waiting behind its initial tree read.
        int generation = Interlocked.Increment(ref _generation);
        CancellationTokenSource? previous;
        lock (_stateGate)
            previous = _cts;
        previous?.Cancel();

        await _lifecycleGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            if (generation != CurrentGeneration)
                return new(false, "connection was superseded");
            await StopCoreAsync().ConfigureAwait(false);
            if (generation != CurrentGeneration || Volatile.Read(ref _disposed) != 0)
                return new(false, "connection was superseded");

            var utf8NoBom = new UTF8Encoding(encoderShouldEmitUTF8Identifier: false);
            var psi = new ProcessStartInfo
            {
                FileName = exePath,
                UseShellExecute = false,
                RedirectStandardInput = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                StandardInputEncoding = utf8NoBom,
                StandardOutputEncoding = Encoding.UTF8,
                StandardErrorEncoding = Encoding.UTF8,
                CreateNoWindow = true,
            };
            psi.ArgumentList.Add("mcp");
            psi.ArgumentList.Add("--allow-input");

            var process = new Process { StartInfo = psi, EnableRaisingEvents = true };
            var cts = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
            lock (_stateGate)
            {
                _process = process;
                _cts = cts;
                _sessionId = null;
                _resourceUri = null;
                _resourceReadRunning = false;
                _resourceReadPending = false;
            }

            try
            {
                process.Start();
            }
            catch (Exception ex)
            {
                lock (_stateGate)
                    _process = null;
                process.Dispose();
                cts.Dispose();
                return new(false, $"could not start '{exePath} mcp': {ex.Message}");
            }

            Logger.Log("mcp", $"Started server, pid={process.Id}");
            _ = PumpStdOutAsync(process, generation, cts.Token);
            _ = PumpStdErrAsync(process, generation, cts.Token);
            _ = MonitorExitAsync(process, generation);

            try
            {
                EnsureProtocolSuccess(await RequestAsync(
                    "initialize",
                    new
                    {
                        protocolVersion = "2025-06-18",
                        capabilities = new { },
                        clientInfo = new { name = "lvt-viewer", version = "1" },
                    },
                    TimeSpan.FromSeconds(15),
                    cts.Token).ConfigureAwait(false));
                await NotifyAsync("notifications/initialized", null, cts.Token).ConfigureAwait(false);

                var connected = await CallToolAsync(
                    "connect",
                    new { hwnd = hwndHex, mode = uia ? "uia" : "visual" },
                    cts.Token).ConfigureAwait(false);
                if (!connected.Ok)
                {
                    await StopCoreAsync().ConfigureAwait(false);
                    return new(false, connected.Error);
                }

                if (!connected.Payload.TryGetProperty("session", out var sessionProperty) ||
                    string.IsNullOrWhiteSpace(sessionProperty.GetString()))
                {
                    await StopCoreAsync().ConfigureAwait(false);
                    return new(false, "MCP connect returned no session id");
                }

                string session = sessionProperty.GetString()!;
                string resourceUri = await FindSessionResourceAsync(
                    session, uia, cts.Token).ConfigureAwait(false);
                lock (_stateGate)
                {
                    _sessionId = session;
                    _resourceUri = resourceUri;
                    // Notifications can arrive immediately after subscribe.
                    // Mark the initial read as the one active read before
                    // subscribing, so QueueResourceRead only records another
                    // pass rather than launching a concurrent destructive
                    // snapshot/diff read.
                    _resourceReadRunning = true;
                }

                EnsureProtocolSuccess(await RequestAsync(
                    "resources/subscribe",
                    new { uri = resourceUri },
                    TimeSpan.FromSeconds(15),
                    cts.Token).ConfigureAwait(false));
                await ReadResourceAsync(resourceUri, generation, cts.Token).ConfigureAwait(false);
                FinishInitialResourceRead(resourceUri, generation);
                return new(true);
            }
            catch (Exception ex) when (ex is not OperationCanceledException)
            {
                Logger.LogException("mcp", "Connection setup failed", ex);
                await StopCoreAsync().ConfigureAwait(false);
                return new(false, ex.Message);
            }
        }
        finally
        {
            _lifecycleGate.Release();
        }
    }

    public Task<McpToolResult> GetElementPropertiesAsync(string element) =>
        CallSessionToolAsync("get_element_properties", new { element });

    public Task<McpToolResult> ToggleAsync(string element) =>
        CallSessionToolAsync("toggle", new { element });

    public Task<McpToolResult> SetValueAsync(string element, string text) =>
        CallSessionToolAsync("set_value", new { element, text });

    public Task<McpToolResult> GetVisualPropertiesAsync(string key) =>
        CallSessionToolAsync("get_visual_properties", new { key });

    public Task<McpToolResult> SetVisualPropertyAsync(
        string key, uint propertyIndex, string valueType, string value) =>
        CallSessionToolAsync(
            "set_visual_property",
            new { key, propertyIndex, valueType, value });

    public Task<McpToolResult> ClearVisualPropertyAsync(string key, uint propertyIndex) =>
        CallSessionToolAsync("clear_visual_property", new { key, propertyIndex });

    public async Task<McpStartResult> RefreshResourceAsync()
    {
        string? uri;
        CancellationToken token;
        int generation = CurrentGeneration;
        lock (_stateGate)
        {
            uri = _resourceUri;
            token = _cts?.Token ?? CancellationToken.None;
        }
        if (uri == null)
            return new(false, "MCP tree resource is not connected");
        try
        {
            await ReadResourceAsync(uri, generation, token).ConfigureAwait(false);
            return new(true);
        }
        catch (Exception ex) when (ex is not OperationCanceledException)
        {
            return new(false, ex.Message);
        }
    }

    private async Task<McpToolResult> CallSessionToolAsync(string name, object arguments)
    {
        string? session = SessionId;
        CancellationToken token;
        lock (_stateGate)
            token = _cts?.Token ?? CancellationToken.None;
        if (session == null)
            return new(false, default, "MCP session is not connected");

        using var argsDocument = JsonDocument.Parse(JsonSerializer.Serialize(arguments));
        var withSession = new Dictionary<string, object?> { ["session"] = session };
        foreach (var property in argsDocument.RootElement.EnumerateObject())
            withSession[property.Name] = property.Value.Clone();
        return await CallToolAsync(name, withSession, token).ConfigureAwait(false);
    }

    private async Task<McpToolResult> CallToolAsync(
        string name, object arguments, CancellationToken cancellationToken,
        TimeSpan? timeout = null)
    {
        JsonElement response;
        try
        {
            response = await RequestAsync(
                "tools/call",
                new { name, arguments },
                timeout ?? TimeSpan.FromSeconds(60),
                cancellationToken).ConfigureAwait(false);
        }
        catch (Exception ex) when (ex is not OperationCanceledException)
        {
            return new(false, default, ex.Message);
        }

        if (!response.TryGetProperty("result", out var result))
            return new(false, default, ReadProtocolError(response));

        JsonElement payload = default;
        if (result.TryGetProperty("structuredContent", out var structured) &&
            structured.ValueKind == JsonValueKind.Object)
        {
            payload = structured.Clone();
        }
        else if (result.TryGetProperty("content", out var content) &&
                 content.ValueKind == JsonValueKind.Array)
        {
            foreach (var block in content.EnumerateArray())
            {
                if (block.TryGetProperty("type", out var type) &&
                    type.GetString() == "text" &&
                    block.TryGetProperty("text", out var text))
                {
                    using var payloadDocument = JsonDocument.Parse(text.GetString() ?? "{}");
                    payload = payloadDocument.RootElement.Clone();
                    break;
                }
            }
        }

        bool isError = result.TryGetProperty("isError", out var errorProperty) &&
                       errorProperty.ValueKind == JsonValueKind.True;
        bool payloadFailed = payload.ValueKind == JsonValueKind.Object &&
                             payload.TryGetProperty("ok", out var okProperty) &&
                             okProperty.ValueKind == JsonValueKind.False;
        if (isError || payloadFailed)
        {
            string error = payload.ValueKind == JsonValueKind.Object &&
                           payload.TryGetProperty("error", out var message)
                ? message.GetString() ?? $"{name} failed"
                : $"{name} failed";
            return new(false, payload, NormalizeError(error));
        }
        return new(true, payload);
    }

    private async Task ReadResourceAsync(
        string resourceUri, int generation, CancellationToken cancellationToken)
    {
        await _resourceReadGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            JsonElement response = await RequestAsync(
                "resources/read",
                new { uri = resourceUri },
                TimeSpan.FromSeconds(90),
                cancellationToken).ConfigureAwait(false);
            if (generation != Volatile.Read(ref _generation))
                return;
            if (!response.TryGetProperty("result", out var result) ||
                !result.TryGetProperty("contents", out var contents) ||
                contents.ValueKind != JsonValueKind.Array)
                throw new InvalidOperationException(ReadProtocolError(response));

            foreach (var content in contents.EnumerateArray())
            {
                if (!content.TryGetProperty("text", out var text))
                    continue;
                var patch = JsonSerializer.Deserialize<TreePatchDto>(
                    text.GetString() ?? "{}", JsonDefaults.Options);
                if (patch == null || string.IsNullOrEmpty(patch.Tree))
                    throw new InvalidOperationException("MCP tree resource returned an invalid patch");
                PatchReceived?.Invoke(generation, patch);
                return;
            }
            throw new InvalidOperationException("MCP tree resource returned no text content");
        }
        finally
        {
            _resourceReadGate.Release();
        }
    }

    private void QueueResourceRead(string uri, int generation)
    {
        bool start;
        lock (_stateGate)
        {
            if (generation != _generation || uri != _resourceUri || _cts == null)
                return;
            _resourceReadPending = true;
            start = !_resourceReadRunning;
            if (start)
                _resourceReadRunning = true;
        }
        if (start)
            _ = DrainResourceReadsAsync(uri, generation, alreadyRunning: true);
    }

    private void FinishInitialResourceRead(string uri, int generation)
    {
        bool startDrain;
        lock (_stateGate)
        {
            if (generation != _generation || uri != _resourceUri)
                return;
            _resourceReadRunning = false;
            startDrain = _resourceReadPending;
            if (startDrain)
                _resourceReadRunning = true;
        }
        if (startDrain)
            _ = DrainResourceReadsAsync(uri, generation, alreadyRunning: true);
    }

    private async Task DrainResourceReadsAsync(
        string uri, int generation, bool alreadyRunning = false)
    {
        if (!alreadyRunning)
        {
            lock (_stateGate)
            {
                if (_resourceReadRunning)
                    return;
                _resourceReadRunning = true;
            }
        }
        for (;;)
        {
            CancellationToken token;
            lock (_stateGate)
            {
                if (generation != _generation || _cts == null)
                {
                    _resourceReadRunning = false;
                    return;
                }
                if (!_resourceReadPending)
                {
                    _resourceReadRunning = false;
                    return;
                }
                _resourceReadPending = false;
                token = _cts.Token;
            }

            try
            {
                await ReadResourceAsync(uri, generation, token).ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
                return;
            }
            catch (Exception ex)
            {
                Logger.LogException("mcp", "Resource read failed", ex);
                DiagnosticReceived?.Invoke($"MCP tree update failed: {ex.Message}");
            }
        }
    }

    private async Task<JsonElement> RequestAsync(
        string method, object? parameters, TimeSpan timeout, CancellationToken cancellationToken)
    {
        long id = Interlocked.Increment(ref _nextRequestId);
        var completion = new TaskCompletionSource<JsonElement>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        lock (_stateGate)
            _pending.Add(id, completion);

        try
        {
            await WriteMessageAsync(
                new Dictionary<string, object?>
                {
                    ["jsonrpc"] = "2.0",
                    ["id"] = id,
                    ["method"] = method,
                    ["params"] = parameters ?? new { },
                },
                cancellationToken).ConfigureAwait(false);
            return await completion.Task.WaitAsync(timeout, cancellationToken).ConfigureAwait(false);
        }
        finally
        {
            lock (_stateGate)
                _pending.Remove(id);
        }
    }

    private Task NotifyAsync(string method, object? parameters, CancellationToken cancellationToken) =>
        WriteMessageAsync(
            new Dictionary<string, object?>
            {
                ["jsonrpc"] = "2.0",
                ["method"] = method,
                ["params"] = parameters ?? new { },
            },
            cancellationToken);

    private async Task WriteMessageAsync(object message, CancellationToken cancellationToken)
    {
        Process? process;
        lock (_stateGate)
            process = _process;
        if (process == null || process.HasExited)
            throw new InvalidOperationException("MCP server is not running");

        string json = JsonSerializer.Serialize(message);
        await _writeGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            await process.StandardInput.WriteAsync((json + "\n").AsMemory(), cancellationToken)
                .ConfigureAwait(false);
            await process.StandardInput.FlushAsync(cancellationToken).ConfigureAwait(false);
        }
        finally
        {
            _writeGate.Release();
        }
    }

    private async Task PumpStdOutAsync(
        Process process, int generation, CancellationToken cancellationToken)
    {
        try
        {
            while (!cancellationToken.IsCancellationRequested)
            {
                string? line = await process.StandardOutput.ReadLineAsync(cancellationToken)
                    .ConfigureAwait(false);
                if (line == null)
                    return;
                if (string.IsNullOrWhiteSpace(line))
                    continue;

                try
                {
                    using var document = JsonDocument.Parse(line);
                    var root = document.RootElement;
                    if (root.ValueKind != JsonValueKind.Object)
                    {
                        Logger.Log("mcp", "Ignoring non-object JSON-RPC message");
                        continue;
                    }

                    // A message carrying `method` is a server notification or
                    // request, even when it also carries an id. JSON-RPC
                    // request ids are independent in each direction.
                    if (root.TryGetProperty("method", out var methodProperty))
                    {
                        string? method = methodProperty.ValueKind == JsonValueKind.String
                            ? methodProperty.GetString()
                            : null;
                        if (method == "notifications/resources/updated" &&
                            root.TryGetProperty("params", out var notificationParams) &&
                            notificationParams.TryGetProperty("uri", out var uriProperty))
                        {
                            string? uri = uriProperty.ValueKind == JsonValueKind.String
                                ? uriProperty.GetString()
                                : null;
                            if (uri != null)
                                QueueResourceRead(uri, generation);
                        }
                        else if (root.TryGetProperty("id", out var serverId))
                        {
                            object response = method == "ping"
                                ? new Dictionary<string, object?>
                                {
                                    ["jsonrpc"] = "2.0",
                                    ["id"] = serverId.Clone(),
                                    ["result"] = new { },
                                }
                                : new Dictionary<string, object?>
                                {
                                    ["jsonrpc"] = "2.0",
                                    ["id"] = serverId.Clone(),
                                    ["error"] = new
                                    {
                                        code = -32601,
                                        message = $"Method not supported by lvt Viewer: {method}",
                                    },
                                };
                            await WriteMessageAsync(response, cancellationToken).ConfigureAwait(false);
                        }
                        continue;
                    }

                    if (root.TryGetProperty("id", out var idProperty) &&
                        idProperty.TryGetInt64(out long id))
                    {
                        TaskCompletionSource<JsonElement>? completion = null;
                        lock (_stateGate)
                            _pending.TryGetValue(id, out completion);
                        completion?.TrySetResult(root.Clone());
                    }
                }
                catch (Exception ex) when (ex is not OperationCanceledException)
                {
                    Logger.Log("mcp", $"Ignoring malformed/unexpected stdout message: {ex.Message}");
                }
            }
        }
        catch (OperationCanceledException)
        {
            // Expected during reconnect/shutdown.
        }
        catch (Exception ex)
        {
            Logger.LogException("mcp", "stdout pump failed", ex);
            DiagnosticReceived?.Invoke($"MCP protocol error: {ex.Message}");
        }
        finally
        {
            FailPending("MCP server output closed");
        }
    }

    private async Task PumpStdErrAsync(
        Process process, int generation, CancellationToken cancellationToken)
    {
        try
        {
            while (!cancellationToken.IsCancellationRequested)
            {
                string? line = await process.StandardError.ReadLineAsync(cancellationToken)
                    .ConfigureAwait(false);
                if (line == null)
                    return;
                if (!string.IsNullOrWhiteSpace(line) &&
                    generation == Volatile.Read(ref _generation))
                {
                    Logger.Log("mcp", $"stderr: {line}");
                    DiagnosticReceived?.Invoke(line);
                }
            }
        }
        catch (OperationCanceledException)
        {
            // Expected during reconnect/shutdown.
        }
    }

    private async Task MonitorExitAsync(Process process, int generation)
    {
        try
        {
            await process.WaitForExitAsync().ConfigureAwait(false);
            if (generation == Volatile.Read(ref _generation) &&
                ReferenceEquals(process, _process))
                Exited?.Invoke(process.ExitCode);
        }
        catch (ObjectDisposedException)
        {
            // StopCoreAsync disposed an already-replaced process.
        }
    }

    private async Task StopCoreAsync()
    {
        Process? process;
        CancellationTokenSource? cts;
        string? session;
        string? resourceUri;
        lock (_stateGate)
        {
            process = _process;
            cts = _cts;
            session = _sessionId;
            resourceUri = _resourceUri;
        }
        if (process == null)
            return;

        if (!process.HasExited && cts != null)
        {
            try
            {
                if (resourceUri != null)
                    await RequestAsync(
                        "resources/unsubscribe", new { uri = resourceUri },
                        TimeSpan.FromSeconds(2), cts.Token).ConfigureAwait(false);
                if (session != null)
                    await CallToolAsync(
                        "disconnect", new { session }, cts.Token,
                        TimeSpan.FromSeconds(2)).ConfigureAwait(false);
            }
            catch
            {
                // Best-effort graceful teardown; closing stdin below is the
                // transport-level shutdown fallback.
            }
        }

        cts?.Cancel();
        FailPending("MCP session stopped");
        try
        {
            process.StandardInput.Close();
            if (!process.HasExited)
                await process.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(3))
                    .ConfigureAwait(false);
        }
        catch
        {
            if (!process.HasExited)
                process.Kill(entireProcessTree: true);
        }

        lock (_stateGate)
        {
            if (ReferenceEquals(_process, process))
            {
                _process = null;
                _cts = null;
                _sessionId = null;
                _resourceUri = null;
                _resourceReadPending = false;
                _resourceReadRunning = false;
            }
        }
        process.Dispose();
        cts?.Dispose();
    }

    private void FailPending(string error)
    {
        List<TaskCompletionSource<JsonElement>> pending;
        lock (_stateGate)
        {
            pending = new(_pending.Values);
            _pending.Clear();
        }
        var failure = JsonSerializer.SerializeToElement(
            new { jsonrpc = "2.0", error = new { message = error } });
        foreach (var completion in pending)
            completion.TrySetResult(failure);
    }

    private static string ReadProtocolError(JsonElement response)
    {
        if (response.ValueKind == JsonValueKind.Object &&
            response.TryGetProperty("error", out var error))
        {
            if (error.ValueKind == JsonValueKind.Object &&
                error.TryGetProperty("message", out var message))
                return message.GetString() ?? "MCP request failed";
            return error.ToString();
        }
        return "MCP request failed";
    }

    private async Task<string> FindSessionResourceAsync(
        string session, bool uia, CancellationToken cancellationToken)
    {
        JsonElement response = await RequestAsync(
            "resources/list", new { }, TimeSpan.FromSeconds(15), cancellationToken)
            .ConfigureAwait(false);
        EnsureProtocolSuccess(response);
        string suffix = uia ? "/uia-tree" : "/visual-tree";
        if (response.GetProperty("result").TryGetProperty("resources", out var resources))
        {
            foreach (var resource in resources.EnumerateArray())
            {
                if (!resource.TryGetProperty("uri", out var uriProperty))
                    continue;
                string? uri = uriProperty.GetString();
                if (uri != null &&
                    uri.Contains($"/{session}/", StringComparison.Ordinal) &&
                    uri.EndsWith(suffix, StringComparison.Ordinal))
                    return uri;
            }
        }
        throw new InvalidOperationException(
            $"MCP server did not expose the {suffix[1..]} resource for session {session}");
    }

    private static string NormalizeError(string error)
    {
        if (!error.StartsWith('{'))
            return error;
        try
        {
            using var document = JsonDocument.Parse(error);
            if (document.RootElement.TryGetProperty("error", out var nested))
                return nested.GetString() ?? error;
        }
        catch (JsonException)
        {
        }
        return error;
    }

    private static void EnsureProtocolSuccess(JsonElement response)
    {
        if (!response.TryGetProperty("result", out _))
            throw new InvalidOperationException(ReadProtocolError(response));
    }

    public async Task StopAsync()
    {
        Interlocked.Increment(ref _generation);
        CancellationTokenSource? current;
        lock (_stateGate)
            current = _cts;
        current?.Cancel();

        await _lifecycleGate.WaitAsync().ConfigureAwait(false);
        try
        {
            await StopCoreAsync().ConfigureAwait(false);
        }
        finally
        {
            _lifecycleGate.Release();
        }
    }

    public async ValueTask DisposeAsync()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0)
            return;
        Interlocked.Increment(ref _generation);
        CancellationTokenSource? current;
        lock (_stateGate)
            current = _cts;
        current?.Cancel();

        await _lifecycleGate.WaitAsync().ConfigureAwait(false);
        try
        {
            await StopCoreAsync().ConfigureAwait(false);
        }
        finally
        {
            _lifecycleGate.Release();
        }
    }

    public void Dispose()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0)
            return;

        Interlocked.Increment(ref _generation);
        Process? process;
        CancellationTokenSource? cts;
        lock (_stateGate)
        {
            process = _process;
            cts = _cts;
            _process = null;
            _cts = null;
            _sessionId = null;
            _resourceUri = null;
            _resourceReadPending = false;
            _resourceReadRunning = false;
        }
        cts?.Cancel();
        FailPending("MCP session disposed");
        try
        {
            if (process is { HasExited: false })
                process.Kill(entireProcessTree: true);
        }
        catch
        {
        }
        process?.Dispose();
        cts?.Dispose();
    }
}
