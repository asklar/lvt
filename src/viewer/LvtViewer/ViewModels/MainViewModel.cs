using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Diagnostics;
using System.Globalization;
using System.Linq;
using System.Windows.Data;
using System.Windows.Threading;
using LvtViewer.Models;
using LvtViewer.Services;

namespace LvtViewer.ViewModels;

/// <summary>
/// Top-level view model: owns the resolved lvt.exe path, the current
/// target's MCP session, the live tree, and the property-editing actions.
/// </summary>
public sealed class MainViewModel : ObservableObject, IDisposable
{
    private readonly Dispatcher _dispatcher;
    private readonly LiveTree _liveTree = new();
    private readonly McpSession _mcp = new();
    private readonly Dictionary<string, IReadOnlyList<PropertyDescriptorDto>>
        _propertySchemas = new(StringComparer.Ordinal);

    private string _statusText = "Drag the crosshair onto a window to inspect it.";
    private string _targetText = "No target";
    private bool _useUia = true;
    private ElementNodeViewModel? _selectedElement;
    private ICollectionView? _selectedPropertyRows;
    private string _propertySearchQuery = "";
    private bool _isPropertyPanelLoading;
    private string? _currentHwndHex;
    private IntPtr _currentHwnd;
    private int _connectionGeneration;
    private readonly DispatcherTimer _targetLivenessTimer;
    private bool _awaitingSnapshot;
    private bool _resourceProbeRunning;
    private int _resourceProbeFailures;
    private DateTime _lastPatchUtc = DateTime.UtcNow;

    public MainViewModel(Dispatcher dispatcher)
    {
        _dispatcher = dispatcher;
        LvtExePath = LvtLocator.Find();

        // 60ms: short enough that a live filter refresh still feels
        // immediate, while coalescing the thousands of element events an
        // initial MCP resource snapshot can contain into one filter walk.
        _filterDebounceTimer = new DispatcherTimer(DispatcherPriority.Background)
        {
            Interval = TimeSpan.FromMilliseconds(60),
        };
        _filterDebounceTimer.Tick += (_, _) =>
        {
            _filterDebounceTimer.Stop();
            ApplyFrameworkFilter();
        };

        _mcp.PatchReceived += (generation, patch) =>
            _dispatcher.BeginInvoke(() => OnTreePatch(generation, patch));
        _mcp.DiagnosticReceived += line => _dispatcher.BeginInvoke(() => StatusText = line);
        _mcp.Exited += code => _dispatcher.BeginInvoke(() =>
        {
            Logger.Log("viewmodel", $"MCP server exited (code={code})");
            IsConnecting = false;
            StatusText = $"lvt MCP server exited (code {code}). Reconnect or pick a window again.";
            _liveTree.Reset();
            SelectedElement = null;
            IsConnected = false;
        });

        ToggleCommand = new RelayCommand(p => _ = ToggleAsync(p as PropertyRowViewModel));
        SetValueCommand = new RelayCommand(p => _ = SetValueAsync(p as PropertyRowViewModel));
        SetPropertyCommand =
            new RelayCommand(p => _ = SetPropertyAsync(p as PropertyRowViewModel));
        ClearPropertyCommand =
            new RelayCommand(p => _ = ClearPropertyAsync(p as PropertyRowViewModel));
        ReconnectCommand = new RelayCommand(_ => Reconnect(), _ => _currentHwndHex != null);
        FindNextCommand = new RelayCommand(_ => FindNext());
        FindPreviousCommand = new RelayCommand(_ => FindPrevious());

        _targetLivenessTimer = new DispatcherTimer
        {
            Interval = TimeSpan.FromSeconds(1),
        };
        _targetLivenessTimer.Tick += async (_, _) =>
        {
            if (_currentHwnd != IntPtr.Zero &&
                (IsConnecting || IsConnected) &&
                !Interop.NativeMethods.IsWindow(_currentHwnd))
            {
                await HandleTargetClosedAsync();
                return;
            }

            // Resource notifications are the normal path. A low-frequency
            // read is only a watchdog for a subscription task that failed
            // while the target window remained alive; it also keeps a quiet
            // app distinguishable from a frozen session.
            if (IsConnected && !_resourceProbeRunning &&
                DateTime.UtcNow - _lastPatchUtc > TimeSpan.FromSeconds(5))
            {
                _resourceProbeRunning = true;
                try
                {
                    var result = await _mcp.RefreshResourceAsync();
                    if (result.Ok)
                    {
                        _resourceProbeFailures = 0;
                        _lastPatchUtc = DateTime.UtcNow;
                    }
                    else if (++_resourceProbeFailures >= 2)
                    {
                        IsConnected = false;
                        SelectedElement = null;
                        _liveTree.Reset();
                        StatusText = $"The MCP tree subscription stopped: {result.Error}";
                    }
                }
                finally
                {
                    _resourceProbeRunning = false;
                }
            }
        };
        _targetLivenessTimer.Start();
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
            if (SetField(ref _useUia, value))
            {
                OnPropertyChanged(nameof(IsFrameworkFilterActive));
                ApplyFrameworkFilter();
                if (_currentHwndHex != null)
                    Reconnect();
            }
        }
    }

    private bool _isConnected;

    /// <summary>Whether a target window is currently connected — gates the element-pick gesture (item 2).</summary>
    public bool IsConnected
    {
        get => _isConnected;
        private set
        {
            if (SetField(ref _isConnected, value))
                Logger.Log("viewmodel", $"IsConnected -> {value} (crosshair {(value ? "enabled" : "disabled")})");
        }
    }

    private bool _isConnecting;

    /// <summary>
    /// True from starting an MCP session until its first resource snapshot arrives.
    /// Drives the status bar's indeterminate progress indicator: the native
    /// protocol currently exposes phases/timings in logs but no truthful
    /// percentage during its initial AdviseVisualTreeChange replay.
    /// </summary>
    public bool IsConnecting
    {
        get => _isConnecting;
        private set => SetField(ref _isConnecting, value);
    }

    /// <summary>
    /// The connected target's HWND, so MainWindow can check IsIconic before
    /// showing a highlight overlay — a minimized target's last-known bounds
    /// are meaningless to draw a rectangle around. IntPtr.Zero when nothing
    /// is connected.
    /// </summary>
    public IntPtr CurrentHwnd => _currentHwnd;

    /// <summary>
    /// Discovered framework/content types (win32, xaml, winui3, wpf, comctl,
    /// dui, ...) for the current target, each with its own include/exclude
    /// checkbox. Populated lazily as MCP patches report elements with a
    /// framework value not seen yet this session.
    /// </summary>
    public ObservableCollection<FrameworkFilterOption> FrameworkFilters { get; } = new();

    /// <summary>
    /// UIA's tree has no per-node framework distinction the way the visual
    /// tree does (see docs/mcp-server.md's "Modes"), so the filter only
    /// applies — and only needs to be shown — in visual-tree mode.
    /// </summary>
    public bool IsFrameworkFilterActive => !UseUia;

    public ElementNodeViewModel? SelectedElement
    {
        get => _selectedElement;
        set
        {
            if (ReferenceEquals(_selectedElement, value))
                return;
            if (_selectedElement != null)
                _selectedElement.PropertyChanged -= OnSelectedElementPropertyChanged;
            SetField(ref _selectedElement, value);
            if (_selectedElement != null)
                _selectedElement.PropertyChanged += OnSelectedElementPropertyChanged;
            RebuildPropertyView();
            IsPropertyPanelLoading = value != null;
            if (UseUia)
                _ = RefreshUiaPropertiesAsync(value);
            else
                _ = RefreshTypedPropertiesAsync(value);
        }
    }

    public ICollectionView? SelectedPropertyRows
    {
        get => _selectedPropertyRows;
        private set => SetField(ref _selectedPropertyRows, value);
    }

    public string PropertySearchQuery
    {
        get => _propertySearchQuery;
        set
        {
            if (SetField(ref _propertySearchQuery, value))
                SelectedPropertyRows?.Refresh();
        }
    }

    public bool IsPropertyPanelLoading
    {
        get => _isPropertyPanelLoading;
        private set => SetField(ref _isPropertyPanelLoading, value);
    }

    private bool _highlightSelected = true;

    /// <summary>
    /// Whether MainWindow draws a highlight overlay around SelectedElement's
    /// bounds on the target app, the same visual style the crosshair-drag
    /// gesture already uses to show which window it is over (item 1).
    /// </summary>
    public bool HighlightSelected
    {
        get => _highlightSelected;
        set => SetField(ref _highlightSelected, value);
    }

    private string _searchQuery = "";

    /// <summary>Free-text search over Text/ClassName/Type/properties, case-insensitive substring match.</summary>
    public string SearchQuery
    {
        get => _searchQuery;
        set
        {
            if (SetField(ref _searchQuery, value))
                _searchCursor = -1; // a changed query starts a fresh search from the first match
        }
    }

    private int _searchCursor = -1;

    /// <summary>Fired when FindNext resolves a match, so MainWindow can select+highlight it in the TreeView.</summary>
    public event Action<ElementNodeViewModel>? SearchMatchFound;

    public RelayCommand ToggleCommand { get; }
    public RelayCommand SetValueCommand { get; }
    public RelayCommand SetPropertyCommand { get; }
    public RelayCommand ClearPropertyCommand { get; }
    public RelayCommand ReconnectCommand { get; }
    public RelayCommand FindNextCommand { get; }
    public RelayCommand FindPreviousCommand { get; }

    private readonly DispatcherTimer _filterDebounceTimer;

    private void OnSelectedElementPropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName == nameof(ElementNodeViewModel.PropertyRows))
            RebuildPropertyView();
    }

    private void RebuildPropertyView()
    {
        if (SelectedElement == null)
        {
            SelectedPropertyRows = null;
            return;
        }

        var view = new ListCollectionView(SelectedElement.PropertyRows);
        view.Filter = item =>
        {
            if (item is not PropertyRowViewModel row)
                return false;
            var query = PropertySearchQuery.Trim();
            return query.Length == 0 ||
                   row.Name.Contains(query, StringComparison.OrdinalIgnoreCase) ||
                   row.Value.Contains(query, StringComparison.OrdinalIgnoreCase);
        };
        if (view.CanChangeLiveFiltering)
        {
            view.LiveFilteringProperties.Add(nameof(PropertyRowViewModel.Value));
            view.IsLiveFiltering = true;
        }
        SelectedPropertyRows = view;
    }

    private void OnTreePatch(int mcpGeneration, TreePatchDto patch)
    {
        if (mcpGeneration != _mcp.CurrentGeneration)
            return;
        string expectedTree = UseUia ? "uia" : "visual";
        if (!string.Equals(patch.Tree, expectedTree, StringComparison.Ordinal))
        {
            Logger.Log("mcp", $"Ignoring stale {patch.Tree} patch while showing {expectedTree}");
            return;
        }

        if (!patch.Snapshot && _awaitingSnapshot)
        {
            IsConnecting = false;
            IsConnected = false;
            StatusText = "MCP sent a diff before the initial snapshot. Reconnect to resynchronize.";
            _ = _mcp.StopAsync();
            return;
        }
        if (patch.Snapshot)
            _awaitingSnapshot = false;

        if (patch.Snapshot)
        {
            _liveTree.Reset();
            SelectedElement = null;
            FrameworkFilters.Clear();
            _propertySchemas.Clear();
        }

        Logger.Log(
            "tree",
            $"Applying {patch.Tree} {(patch.Snapshot ? "snapshot" : "diff")} " +
            $"with {patch.Events.Count} event(s)");
        foreach (var evt in patch.Events)
        {
            _liveTree.Apply(evt);
            DiscoverFrameworks(evt);
        }
        ScheduleFilterRefresh();
        _lastPatchUtc = DateTime.UtcNow;
        _resourceProbeFailures = 0;

        if (IsConnecting)
        {
            IsConnecting = false;
            IsConnected = true;
            StatusText = UseUia
                ? "Watching the UI Automation tree live."
                : "Watching the visual tree live.";
        }
    }

    /// <summary>
    /// ApplyFrameworkFilter walks every currently-known node to recompute
    /// IsVisible — genuinely O(tree size), unlike LiveTree.Apply above, and
    /// unlike IsVisible it does not need to be current after every single
    /// event, only eventually. Debouncing this (instead of running it after
    /// every event) is what actually still needs a timer here.
    ///
    /// The resource delivers a patch as a batch. Resetting an actual timer
    /// while applying it guarantees filtering runs once after the batch,
    /// rather than once per added/changed/removed element.
    /// </summary>
    private void ScheduleFilterRefresh()
    {
        _filterDebounceTimer.Stop();
        _filterDebounceTimer.Start();
    }

    /// <summary>Adds a checkbox (default checked) for any not-yet-seen framework value in this event.</summary>
    private void DiscoverFrameworks(TreeChangeEventDto evt)
    {
        string? framework = evt.Element?.Framework;
        if (evt.Fields != null && evt.Fields.TryGetValue("framework", out var change))
            framework = change.New;
        if (string.IsNullOrEmpty(framework))
            return;
        if (FrameworkFilters.Any(f => f.Name == framework))
            return;

        var option = new FrameworkFilterOption(framework);
        option.PropertyChanged += (_, _) => ApplyFrameworkFilter();
        FrameworkFilters.Add(option);
    }

    /// <summary>
    /// Recomputes ElementNodeViewModel.IsVisible across the whole tree from
    /// the current FrameworkFilters selection. A container stays visible if
    /// any descendant is visible, even when its own framework was excluded,
    /// so excluding one type never hides an unrelated matching descendant
    /// deeper in the same branch.
    /// </summary>
    private void ApplyFrameworkFilter()
    {
        HashSet<string>? enabled = IsFrameworkFilterActive
            ? new HashSet<string>(FrameworkFilters.Where(f => f.IsChecked).Select(f => f.Name))
            : null; // null = no filtering (UIA mode, or nothing discovered yet)

        bool Visit(ElementNodeViewModel node)
        {
            bool childVisible = false;
            foreach (var child in node.Children)
                childVisible |= Visit(child);

            bool selfMatches = enabled == null ||
                                string.IsNullOrEmpty(node.Framework) ||
                                enabled.Contains(node.Framework);
            bool visible = selfMatches || childVisible;
            node.IsVisible = visible;
            return visible;
        }

        foreach (var root in Roots)
            Visit(root);
    }

    /// <summary>
    /// Free-text search over the live tree (item: search bar), matched
    /// case-insensitively against Text, ClassName, Type, and every property
    /// name/value — so a search for a property like an AutomationId or a
    /// UIA pattern name finds elements the Text/ClassName alone would miss.
    /// Cycles through matches in tree (depth-first) order on repeated calls
    /// (forward for FindNext, backward for FindPrevious, both wrapping); a
    /// changed SearchQuery resets the cursor so the next call starts fresh.
    /// </summary>
    private void FindNext() => Find(step: 1);

    private void FindPrevious() => Find(step: -1);

    private void Find(int step)
    {
        var query = SearchQuery?.Trim();
        if (string.IsNullOrEmpty(query))
        {
            StatusText = "Enter a search term (matches Text, Class, Type, or any property).";
            return;
        }

        var matches = new List<ElementNodeViewModel>();
        void Visit(ElementNodeViewModel node)
        {
            if (ElementMatchesQuery(node, query))
                matches.Add(node);
            foreach (var child in node.Children)
                Visit(child);
        }
        foreach (var root in Roots)
            Visit(root);

        if (matches.Count == 0)
        {
            StatusText = $"No elements match \"{query}\".";
            return;
        }

        // A fresh search (cursor reset by a changed query, or never searched
        // this session) starts at the natural end for the requested
        // direction — the first match for Next, the last for Previous —
        // rather than both directions starting from the same spot.
        _searchCursor = _searchCursor < 0
            ? (step > 0 ? 0 : matches.Count - 1)
            : ((_searchCursor + step) % matches.Count + matches.Count) % matches.Count;

        var match = matches[_searchCursor];
        StatusText = $"Match {_searchCursor + 1}/{matches.Count}: {match.DisplayName}";
        SearchMatchFound?.Invoke(match);
    }

    private static bool ElementMatchesQuery(ElementNodeViewModel node, string query)
    {
        if (Contains(node.Text, query) || Contains(node.ClassName, query) || Contains(node.Type, query))
            return true;
        foreach (var row in node.PropertyRows)
        {
            if (Contains(row.Name, query) || Contains(row.Value, query))
                return true;
        }
        return false;
    }

    private static bool Contains(string? haystack, string needle) =>
        !string.IsNullOrEmpty(haystack) && haystack.IndexOf(needle, StringComparison.OrdinalIgnoreCase) >= 0;

    /// <summary>Called by the crosshair picker once a target window is resolved.</summary>
    public void ConnectTo(IntPtr hwnd) => _ = ConnectAsync(hwnd, reconnect: false);

    private async System.Threading.Tasks.Task ConnectAsync(IntPtr hwnd, bool reconnect)
    {
        int generation = ++_connectionGeneration;
        NativeMethodsWindowInfo(hwnd, out var pid, out var title);
        Logger.Log(
            "viewmodel",
            $"MCP connect hwnd=0x{hwnd.ToInt64():X} pid={pid} title=\"{title}\" uia={UseUia}");

        _currentHwnd = hwnd;
        _currentHwndHex = "0x" + hwnd.ToInt64().ToString("X", CultureInfo.InvariantCulture);
        IsConnected = false;
        IsConnecting = true;
        _awaitingSnapshot = true;
        _resourceProbeFailures = 0;
        _lastPatchUtc = DateTime.UtcNow;

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
        StatusText = reconnect ? "Reconnecting…" : "Connecting…";
        _liveTree.Reset();
        SelectedElement = null;
        FrameworkFilters.Clear();

        McpStartResult result;
        try
        {
            result = await _mcp.StartAsync(LvtExePath, _currentHwndHex, UseUia);
        }
        catch (OperationCanceledException)
        {
            return;
        }
        if (generation != _connectionGeneration)
            return;
        if (!result.Ok)
        {
            IsConnecting = false;
            IsConnected = false;
            StatusText = $"Could not connect: {result.Error}";
        }
    }

    private void Reconnect()
    {
        if (_currentHwndHex == null)
            return;
        _ = ConnectAsync(_currentHwnd, reconnect: true);
    }

    private static void NativeMethodsWindowInfo(IntPtr hwnd, out uint pid, out string title)
    {
        Interop.NativeMethods.GetWindowThreadProcessId(hwnd, out pid);
        title = Interop.NativeMethods.GetWindowTitle(hwnd);
    }

    private async System.Threading.Tasks.Task RefreshUiaPropertiesAsync(ElementNodeViewModel? node)
    {
        if (node == null)
        {
            IsPropertyPanelLoading = false;
            return;
        }

        long propertyVersion = node.PropertyVersion;
        var stopwatch = Stopwatch.StartNew();
        var result = await _mcp.GetElementPropertiesAsync(node.Key);
        if (SelectedElement != node || node.PropertyVersion != propertyVersion)
            return;
        if (!result.Ok)
        {
            StatusText = $"Could not read UI Automation properties: {result.Error}";
            IsPropertyPanelLoading = false;
            return;
        }

        if (result.Payload.TryGetProperty("element", out var element) &&
            element.ValueKind == System.Text.Json.JsonValueKind.Object &&
            element.TryGetProperty("properties", out var properties) &&
            properties.ValueKind == System.Text.Json.JsonValueKind.Object)
        {
            string propertyJson = properties.GetRawText();
            var rows = await System.Threading.Tasks.Task.Run(() =>
            {
                using var document = System.Text.Json.JsonDocument.Parse(propertyJson);
                return document.RootElement.EnumerateObject()
                    .Select(property => new PropertyRowViewModel(
                        property.Name,
                        property.Value.ValueKind == System.Text.Json.JsonValueKind.String
                            ? property.Value.GetString() ?? ""
                            : property.Value.ToString()))
                    .ToList();
            });
            if (SelectedElement != node || node.PropertyVersion != propertyVersion)
                return;
            node.ReplacePropertyRows(rows);
        }
        IsPropertyPanelLoading = false;
        Logger.Log("properties", $"Loaded UIA properties in {stopwatch.ElapsedMilliseconds} ms");
    }

    private async System.Threading.Tasks.Task RefreshTypedPropertiesAsync(ElementNodeViewModel? node)
    {
        if (node == null)
        {
            IsPropertyPanelLoading = false;
            return;
        }

        long propertyVersion = node.PropertyVersion;
        var stopwatch = Stopwatch.StartNew();
        var result = await _mcp.GetEditablePropertiesAsync(node.Key);
        if (SelectedElement != node || node.PropertyVersion != propertyVersion)
            return;
        if (!result.Ok)
        {
            Logger.Log(
                "properties",
                $"No typed property provider for {node.Key}: {result.Error}");
            IsPropertyPanelLoading = false;
            return;
        }

        string snapshotJson = result.Payload.GetRawText();
        var snapshot = await System.Threading.Tasks.Task.Run(() =>
            System.Text.Json.JsonSerializer.Deserialize<PropertySnapshotDto>(
                snapshotJson, JsonDefaults.Options));
        if (SelectedElement != node || node.PropertyVersion != propertyVersion)
            return;
        if (snapshot == null || string.IsNullOrWhiteSpace(snapshot.SchemaId))
        {
            IsPropertyPanelLoading = false;
            return;
        }

        if (!_propertySchemas.TryGetValue(snapshot.SchemaId, out var descriptors))
        {
            foreach (var descriptor in snapshot.Descriptors)
                descriptor.PreparePresentation();
            descriptors = snapshot.Descriptors;
            _propertySchemas[snapshot.SchemaId] = descriptors;
        }

        var values = snapshot.Values.ToDictionary(
            value => value.DescriptorId, StringComparer.Ordinal);
        var rows = descriptors
            .Where(descriptor => values.ContainsKey(descriptor.DescriptorId))
            .Select(descriptor =>
            {
                var value = values[descriptor.DescriptorId];
                var row = new PropertyRowViewModel(descriptor.Name, value.Value);
                row.UpdateTypedProperty(descriptor, value);
                return row;
            })
            .ToList();

        node.ReplaceTypedPropertyRows(rows);
        IsPropertyPanelLoading = false;
        Logger.Log(
            "properties",
            $"Loaded {rows.Count} typed properties from schema {snapshot.SchemaId} " +
            $"in {stopwatch.ElapsedMilliseconds} ms");
    }

    private async System.Threading.Tasks.Task ToggleAsync(PropertyRowViewModel? row)
    {
        if (row == null || SelectedElement == null)
            return;
        StatusText = $"Toggling {SelectedElement.DisplayName}…";
        var result = await _mcp.ToggleAsync(SelectedElement.Key);
        StatusText = result.Ok
            ? "Toggled. The live tree will confirm the new state shortly."
            : $"Toggle failed: {result.Error}";
    }

    private async System.Threading.Tasks.Task SetValueAsync(PropertyRowViewModel? row)
    {
        if (row == null || SelectedElement == null)
            return;
        StatusText = $"Setting {row.Name} on {SelectedElement.DisplayName}…";
        var result = await _mcp.SetValueAsync(SelectedElement.Key, row.EditText);
        StatusText = result.Ok
            ? "Value set. The live tree will confirm it shortly."
            : $"set-value failed: {result.Error}";
    }

    private async System.Threading.Tasks.Task SetPropertyAsync(PropertyRowViewModel? row)
    {
        var node = SelectedElement;
        if (row == null || node == null || !row.IsTypedProperty || !row.CanApply)
            return;

        StatusText = $"Setting {row.Name}…";
        var result = await _mcp.SetPropertyAsync(
            node.Key, row.DescriptorId, row.EditText);
        if (SelectedElement != node)
            return;
        if (!result.Ok)
        {
            StatusText = $"Set failed: {result.Error}";
            return;
        }
        StatusText = $"{row.Name} updated.";
        await RefreshTypedPropertiesAsync(node);
    }

    private async System.Threading.Tasks.Task ClearPropertyAsync(PropertyRowViewModel? row)
    {
        var node = SelectedElement;
        if (row == null || node == null || !row.IsTypedProperty)
            return;

        StatusText = $"Clearing {row.Name}…";
        var result = await _mcp.ClearPropertyAsync(node.Key, row.DescriptorId);
        if (SelectedElement != node)
            return;
        if (!result.Ok)
        {
            StatusText = $"Clear failed: {result.Error}";
            return;
        }
        StatusText = $"{row.Name} restored.";
        await RefreshTypedPropertiesAsync(node);
    }

    private async System.Threading.Tasks.Task HandleTargetClosedAsync()
    {
        if (_currentHwnd == IntPtr.Zero)
            return;
        ++_connectionGeneration;
        _currentHwnd = IntPtr.Zero;
        _currentHwndHex = null;
        IsConnecting = false;
        IsConnected = false;
        SelectedElement = null;
        _liveTree.Reset();
        TargetText = "No target";
        StatusText = "The target window closed. Drag the crosshair onto another window.";
        await _mcp.StopAsync();
    }

    public void Dispose()
    {
        _filterDebounceTimer.Stop();
        _targetLivenessTimer.Stop();
        _mcp.Dispose();
    }
}
