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
    private readonly PropertyDescriptorSchemaCache _propertySchemas = new();

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
    private readonly DispatcherTimer _typedPropertyRefreshTimer;
    private readonly TypedPropertyRefreshState _typedPropertyRefreshState = new();
    private readonly TypedPropertyRefreshRetryBudget
        _typedPropertyRefreshRetryBudget = new();
    private int _typedPropertyRefreshDelayMs =
        TypedPropertyRefreshPolicy.InitialDelayMs;
    private bool _typedRefreshNeedsFullUiaLoad;
    private bool _typedRefreshPreservePendingEdits;
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
        _typedPropertyRefreshTimer = new DispatcherTimer(
            DispatcherPriority.Background, _dispatcher)
        {
            Interval = TimeSpan.FromMilliseconds(100),
        };
        _typedPropertyRefreshTimer.Tick += async (_, _) =>
        {
            _typedPropertyRefreshTimer.Stop();
            if (!_typedPropertyRefreshState.TryBegin(out var refreshToken))
                return;
            var node = SelectedElement;
            var generation = _connectionGeneration;
            var fullUiaLoad = _typedRefreshNeedsFullUiaLoad;
            var preservePendingEdits = _typedRefreshPreservePendingEdits;
            _typedRefreshNeedsFullUiaLoad = false;
            _typedRefreshPreservePendingEdits = false;
            var attemptNumber =
                _typedPropertyRefreshRetryBudget.BeginAttempt();
            var refreshResult = TypedPropertyRefreshAttemptResult.Retry(
                "No property snapshot was applied.");
            try
            {
                refreshResult =
                    await TypedPropertyRefreshPolicy.RunAttemptAsync(
                        async () =>
                        {
                            if (node == null)
                            {
                                return TypedPropertyRefreshAttemptResult
                                    .OwnershipLost(
                                        "No property target is selected.");
                            }
                            var result = UseUia && fullUiaLoad
                                ? await RefreshUiaPropertiesAsync(
                                    node, preservePendingEdits, refreshToken)
                                : await RefreshTypedPropertiesAsync(
                                    node, preservePendingEdits, refreshToken);
                            if (result.Status ==
                                    TypedPropertyRefreshAttemptStatus.Applied &&
                                (generation != _connectionGeneration ||
                                 SelectedElement != node))
                            {
                                return TypedPropertyRefreshAttemptResult
                                    .OwnershipLost(
                                        "The property target changed during refresh.");
                            }
                            return result;
                        },
                        () => generation != _connectionGeneration ||
                              SelectedElement != node,
                        ex => Logger.LogException(
                            "properties", "Property refresh failed", ex));
            }
            finally
            {
                bool wasLatest =
                    _typedPropertyRefreshState.IsLatest(refreshToken);
                bool applied =
                    refreshResult.Status ==
                    TypedPropertyRefreshAttemptStatus.Applied;
                var completionAccepted =
                    _typedPropertyRefreshState.Complete(
                        refreshToken, applied);
                if (completionAccepted)
                {
                    if (!wasLatest)
                    {
                        _typedRefreshNeedsFullUiaLoad |= fullUiaLoad;
                        _typedRefreshPreservePendingEdits |=
                            preservePendingEdits;
                    }
                    else if (applied)
                    {
                        node?.SettleCompletedPropertyMutations();
                        _typedPropertyRefreshRetryBudget.Reset();
                        _typedPropertyRefreshDelayMs =
                            TypedPropertyRefreshPolicy.InitialDelayMs;
                        IsPropertyPanelLoading = false;
                    }
                    else if (refreshResult.Status is
                             TypedPropertyRefreshAttemptStatus.Terminal or
                             TypedPropertyRefreshAttemptStatus.OwnershipLost)
                    {
                        if (refreshResult.Status ==
                            TypedPropertyRefreshAttemptStatus.OwnershipLost)
                        {
                            node?.ClearPropertyMutations();
                        }
                        else
                        {
                            _typedRefreshNeedsFullUiaLoad |= fullUiaLoad;
                            _typedRefreshPreservePendingEdits |=
                                preservePendingEdits;
                        }
                        _typedPropertyRefreshState.Reset();
                        IsPropertyPanelLoading = false;
                        if (SelectedElement == node)
                        {
                            StatusText =
                                $"Could not refresh properties: {refreshResult.Error}";
                        }
                    }
                    else if (!_typedPropertyRefreshRetryBudget.CanRetry)
                    {
                        _typedRefreshNeedsFullUiaLoad |= fullUiaLoad;
                        _typedRefreshPreservePendingEdits |=
                            preservePendingEdits;
                        _typedPropertyRefreshState.Reset();
                        IsPropertyPanelLoading = false;
                        if (SelectedElement == node)
                        {
                            StatusText =
                                "Could not refresh properties after "
                                + $"{attemptNumber} attempts: {refreshResult.Error}";
                        }
                    }
                    else
                    {
                        _typedRefreshNeedsFullUiaLoad |= fullUiaLoad;
                        _typedRefreshPreservePendingEdits |=
                            preservePendingEdits;
                        _typedPropertyRefreshDelayMs =
                            _typedPropertyRefreshRetryBudget.RetryDelayMs;
                    }

                    if (_typedPropertyRefreshState.HasPending &&
                        SelectedElement != null)
                    {
                        _typedPropertyRefreshTimer.Interval =
                            TimeSpan.FromMilliseconds(
                                _typedPropertyRefreshDelayMs);
                        _typedPropertyRefreshTimer.Start();
                    }
                }
            }
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
            {
                _selectedElement.PropertyChanged -= OnSelectedElementPropertyChanged;
                _selectedElement.ClearPropertyMutations();
            }
            SetField(ref _selectedElement, value);
            if (_selectedElement != null)
                _selectedElement.PropertyChanged += OnSelectedElementPropertyChanged;
            RebuildPropertyView();
            _typedPropertyRefreshTimer.Stop();
            _typedPropertyRefreshState.Reset();
            _typedRefreshNeedsFullUiaLoad = false;
            _typedRefreshPreservePendingEdits = false;
            _typedPropertyRefreshRetryBudget.Reset();
            _typedPropertyRefreshDelayMs =
                TypedPropertyRefreshPolicy.InitialDelayMs;
            IsPropertyPanelLoading = value != null;
            if (value != null)
            {
                RequestTypedPropertySchemaRefresh(
                    fullUiaLoad: UseUia,
                    preservePendingEdits: false);
            }
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
        bool refreshTypedSchema = false;
        foreach (var evt in patch.Events)
        {
            if (UseUia && SelectedElement != null &&
                PatchAffectsTypedSchema(evt, SelectedElement))
            {
                refreshTypedSchema = true;
            }
            _liveTree.Apply(evt);
            DiscoverFrameworks(evt);
        }
        if (refreshTypedSchema)
            RequestTypedPropertySchemaRefresh();
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

    private async System.Threading.Tasks.Task<TypedPropertyRefreshAttemptResult>
        RefreshUiaPropertiesAsync(
        ElementNodeViewModel? node,
        bool preservePendingEdits = false,
        TypedPropertyRefreshState.Token? refreshToken = null)
    {
        if (node == null)
        {
            IsPropertyPanelLoading = false;
            return TypedPropertyRefreshAttemptResult.OwnershipLost(
                "No property target is selected.");
        }

        long propertyVersion = node.PropertyVersion;
        var stopwatch = Stopwatch.StartNew();
        var result = await _mcp.GetElementPropertiesAsync(node.Key);
        if (SelectedElement != node)
        {
            return TypedPropertyRefreshAttemptResult.OwnershipLost(
                "The property target changed during refresh.");
        }
        if (node.PropertyVersion != propertyVersion ||
            (refreshToken.HasValue &&
             !_typedPropertyRefreshState.IsCurrent(refreshToken.Value)))
        {
            return TypedPropertyRefreshAttemptResult.Retry(
                "The property snapshot was superseded.");
        }
        if (!result.Ok)
        {
            StatusText = $"Could not read UI Automation properties: {result.Error}";
            return TypedPropertyRefreshAttemptResult.Failure(result.Error);
        }

        if (result.Payload.ValueKind !=
                System.Text.Json.JsonValueKind.Object ||
            !result.Payload.TryGetProperty("element", out var element) ||
            element.ValueKind != System.Text.Json.JsonValueKind.Object ||
            !element.TryGetProperty("properties", out var properties) ||
            properties.ValueKind != System.Text.Json.JsonValueKind.Object)
        {
            Logger.Log(
                "properties",
                $"UI Automation property response for {node.Key} had no property snapshot");
            return TypedPropertyRefreshAttemptResult.Retry(
                "UI Automation property response had no property snapshot.");
        }

        try
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
            if (SelectedElement != node)
            {
                return TypedPropertyRefreshAttemptResult.OwnershipLost(
                    "The property target changed during refresh.");
            }
            if (node.PropertyVersion != propertyVersion ||
                (refreshToken.HasValue &&
                 !_typedPropertyRefreshState.IsCurrent(refreshToken.Value)))
            {
                return TypedPropertyRefreshAttemptResult.Retry(
                    "The property snapshot was superseded.");
            }
            node.ReplacePropertyRows(
                rows, preserveTypedRows: preservePendingEdits);
        }
        catch (Exception ex) when (
            ex is System.Text.Json.JsonException or
                InvalidOperationException or
                NotSupportedException)
        {
            Logger.LogException(
                "properties", "Could not parse UI Automation properties", ex);
            return TypedPropertyRefreshAttemptResult.Retry(ex.Message);
        }
        Logger.Log("properties", $"Loaded UIA properties in {stopwatch.ElapsedMilliseconds} ms");
        return await RefreshTypedPropertiesAsync(
            node,
            preservePendingEdits,
            refreshToken: refreshToken,
            clearLoadingOnCompletion: false);
    }

    private void RequestTypedPropertySchemaRefresh(
        bool fullUiaLoad = false,
        bool preservePendingEdits = true)
    {
        _typedRefreshNeedsFullUiaLoad |= fullUiaLoad;
        _typedRefreshPreservePendingEdits |= preservePendingEdits;
        _typedPropertyRefreshRetryBudget.Reset();
        _typedPropertyRefreshDelayMs =
            TypedPropertyRefreshPolicy.InitialDelayMs;
        _typedPropertyRefreshState.Request();
        if (_typedPropertyRefreshState.IsRunning)
            return;
        _typedPropertyRefreshTimer.Interval =
            TimeSpan.FromMilliseconds(_typedPropertyRefreshDelayMs);
        _typedPropertyRefreshTimer.Stop();
        _typedPropertyRefreshTimer.Start();
    }

    private async System.Threading.Tasks.Task<TypedPropertyRefreshAttemptResult>
        RefreshTypedPropertiesAsync(
        ElementNodeViewModel? node, bool preservePendingEdits = false,
        TypedPropertyRefreshState.Token? refreshToken = null,
        bool clearLoadingOnCompletion = true)
    {
        if (node == null)
        {
            IsPropertyPanelLoading = false;
            return TypedPropertyRefreshAttemptResult.OwnershipLost(
                "No property target is selected.");
        }

        long propertyVersion = node.PropertyVersion;
        var stopwatch = Stopwatch.StartNew();
        var result = await _mcp.GetEditablePropertiesAsync(node.Key);
        if (SelectedElement != node)
        {
            return TypedPropertyRefreshAttemptResult.OwnershipLost(
                "The property target changed during refresh.");
        }
        if (node.PropertyVersion != propertyVersion ||
            (refreshToken.HasValue &&
             !_typedPropertyRefreshState.IsCurrent(refreshToken.Value)))
        {
            return TypedPropertyRefreshAttemptResult.Retry(
                "The typed property snapshot was superseded.");
        }
        if (!result.Ok)
        {
            Logger.Log(
                "properties",
                $"No typed property provider for {node.Key}: {result.Error}");
            return TypedPropertyRefreshAttemptResult.Failure(result.Error);
        }

        if (!TypedPropertyRefreshPolicy.TryValidateSnapshotPayload(
                result.Payload, out var validationError))
        {
            Logger.Log("properties", validationError);
            return TypedPropertyRefreshAttemptResult.Retry(validationError);
        }

        PropertySnapshotDto? snapshot;
        try
        {
            string snapshotJson = result.Payload.GetRawText();
            snapshot = await System.Threading.Tasks.Task.Run(() =>
                System.Text.Json.JsonSerializer.Deserialize<PropertySnapshotDto>(
                    snapshotJson, JsonDefaults.Options));
        }
        catch (Exception ex) when (
            ex is System.Text.Json.JsonException or
                InvalidOperationException or
                NotSupportedException)
        {
            Logger.LogException(
                "properties", "Could not parse typed property snapshot", ex);
            return TypedPropertyRefreshAttemptResult.Retry(ex.Message);
        }
        if (SelectedElement != node)
        {
            return TypedPropertyRefreshAttemptResult.OwnershipLost(
                "The property target changed during refresh.");
        }
        if (node.PropertyVersion != propertyVersion ||
            (refreshToken.HasValue &&
             !_typedPropertyRefreshState.IsCurrent(refreshToken.Value)))
        {
            return TypedPropertyRefreshAttemptResult.Retry(
                "The typed property snapshot was superseded.");
        }
        if (snapshot == null || string.IsNullOrWhiteSpace(snapshot.SchemaId))
        {
            Logger.Log(
                "properties",
                $"Typed property response for {node.Key} had no schema");
            return TypedPropertyRefreshAttemptResult.Retry(
                "Typed property response had no schema.");
        }

        try
        {
            if (!_propertySchemas.TryGet(snapshot.SchemaId, out var descriptors))
            {
                foreach (var descriptor in snapshot.Descriptors)
                    descriptor.PreparePresentation();
                descriptors = snapshot.Descriptors;
                _propertySchemas.Store(
                    snapshot.SchemaId, descriptors, snapshot.SchemaId);
            }

            var values = snapshot.Values.ToDictionary(
                value => value.DescriptorId, StringComparer.Ordinal);
            var rows = descriptors
                .Where(descriptor => values.ContainsKey(descriptor.DescriptorId))
                .Select(descriptor =>
                {
                    var value = values[descriptor.DescriptorId];
                    var row = new PropertyRowViewModel(
                        descriptor.Name, value.Value);
                    row.UpdateTypedProperty(descriptor, value);
                    return row;
                })
                .ToList();

            node.ReplaceTypedPropertyRows(rows, preservePendingEdits);
            Logger.Log(
                "properties",
                $"Loaded {rows.Count} typed properties from schema {snapshot.SchemaId} " +
                $"in {stopwatch.ElapsedMilliseconds} ms");
        }
        catch (Exception ex) when (
            ex is ArgumentException or
                InvalidOperationException or
                NullReferenceException)
        {
            Logger.LogException(
                "properties", "Invalid typed property snapshot", ex);
            return TypedPropertyRefreshAttemptResult.Retry(ex.Message);
        }
        if (clearLoadingOnCompletion)
            IsPropertyPanelLoading = false;
        return TypedPropertyRefreshAttemptResult.Applied();
    }

    private async System.Threading.Tasks.Task SetPropertyAsync(PropertyRowViewModel? row)
    {
        var node = SelectedElement;
        if (row == null || node == null || !row.IsTypedProperty || !row.CanApply)
            return;

        var submittedProviderName = row.ProviderName;
        var submittedRevision = row.EditRevision;
        var submittedValue = row.EditText;
        var mutation = node.BeginPropertyMutation(
            submittedProviderName, submittedRevision);
        RequestTypedPropertySchemaRefresh(preservePendingEdits: true);
        StatusText = $"Setting {row.Name}…";
        McpToolResult result;
        try
        {
            result = await _mcp.SetPropertyAsync(
                node.Key, row.DescriptorId, submittedValue);
        }
        catch (Exception ex)
        {
            if (node.CancelPropertyMutation(mutation) &&
                SelectedElement == node)
            {
                StatusText = $"Set failed: {ex.Message}";
            }
            return;
        }
        if (SelectedElement != node)
        {
            node.CancelPropertyMutation(mutation);
            return;
        }
        if (!result.Ok)
        {
            if (node.CancelPropertyMutation(mutation))
                StatusText = $"Set failed: {result.Error}";
            return;
        }
        if (!node.TryCompletePropertyMutation(mutation))
            return;
        var acceptedValue = submittedValue;
        if (result.Payload.ValueKind ==
                System.Text.Json.JsonValueKind.Object &&
            result.Payload.TryGetProperty("value", out var returnedValue) &&
            returnedValue.ValueKind == System.Text.Json.JsonValueKind.String)
        {
            acceptedValue = returnedValue.GetString() ?? acceptedValue;
        }
        var currentRow = node.FindProperty(submittedProviderName);
        currentRow?.ApplyMutationValue(
            acceptedValue, submittedRevision);
        StatusText = $"{row.Name} updated.";
        RequestTypedPropertySchemaRefresh(preservePendingEdits: true);
    }

    private async System.Threading.Tasks.Task ClearPropertyAsync(PropertyRowViewModel? row)
    {
        var node = SelectedElement;
        if (row == null || node == null || !row.IsTypedProperty)
            return;

        var submittedProviderName = row.ProviderName;
        var submittedRevision = row.EditRevision;
        var mutation = node.BeginPropertyMutation(
            submittedProviderName, submittedRevision);
        RequestTypedPropertySchemaRefresh(preservePendingEdits: true);
        StatusText = $"Clearing {row.Name}…";
        McpToolResult result;
        try
        {
            result = await _mcp.ClearPropertyAsync(
                node.Key, row.DescriptorId);
        }
        catch (Exception ex)
        {
            if (node.CancelPropertyMutation(mutation) &&
                SelectedElement == node)
            {
                StatusText = $"Clear failed: {ex.Message}";
            }
            return;
        }
        if (SelectedElement != node)
        {
            node.CancelPropertyMutation(mutation);
            return;
        }
        if (!result.Ok)
        {
            if (node.CancelPropertyMutation(mutation))
                StatusText = $"Clear failed: {result.Error}";
            return;
        }
        if (!node.TryCompletePropertyMutation(mutation))
            return;
        var currentRow = node.FindProperty(submittedProviderName);
        currentRow?.TryDiscardSubmittedEdit(submittedRevision);
        StatusText = $"{row.Name} restored.";
        RequestTypedPropertySchemaRefresh(preservePendingEdits: true);
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
        _typedPropertyRefreshTimer.Stop();
        _targetLivenessTimer.Stop();
        _mcp.Dispose();
    }

    public static bool PatchAffectsTypedSchema(
        TreeChangeEventDto evt, ElementNodeViewModel selected)
    {
        bool isSelected = evt.Key == selected.Key;
        bool isSelectionAncestor = false;
        for (var ancestor = selected.Parent;
             ancestor != null;
             ancestor = ancestor.Parent)
        {
            if (ancestor.Key == evt.Key)
            {
                isSelectionAncestor = true;
                break;
            }
        }
        bool selectedOrAncestor = isSelected || isSelectionAncestor;
        if (evt.Event == "removed" && selectedOrAncestor)
            return true;
        if (evt.Event == "added" &&
            !string.IsNullOrEmpty(evt.Path) &&
            selected.Path.StartsWith(evt.Path + ".", StringComparison.Ordinal))
        {
            return true;
        }
        if (evt.Event != "changed" || evt.Fields == null)
            return false;
        if (selectedOrAncestor && evt.Fields.ContainsKey("path"))
            return true;
        if (isSelected && evt.Fields.ContainsKey("type"))
            return true;

        foreach (var field in evt.Fields.Keys)
        {
            if (!field.StartsWith("properties.", StringComparison.Ordinal))
                continue;
            var property = field["properties.".Length..];
            if (isSelected && property is (
                "IsEnabled" or
                "SupportedPatterns" or
                "Value.IsReadOnly" or
                "RangeValue.IsReadOnly" or
                "RangeValue.Minimum" or
                "RangeValue.Maximum" or
                "ExpandCollapse.State" or
                "Scroll.HorizontallyScrollable" or
                "Scroll.VerticallyScrollable"))
            {
                return true;
            }
            if (isSelectionAncestor && property is (
                "SupportedPatterns" or
                "Selection.CanSelectMultiple" or
                "Selection.IsSelectionRequired"))
            {
                return true;
            }
        }
        return false;
    }
}
