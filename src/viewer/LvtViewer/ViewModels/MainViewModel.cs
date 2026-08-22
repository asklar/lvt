using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Diagnostics;
using System.Globalization;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Threading;
using LvtViewer.Models;
using LvtViewer.Services;

namespace LvtViewer.ViewModels;

/// <summary>
/// Top-level view model: owns the resolved lvt.exe path, the current
/// target's watch session, the live tree, and the property-editing actions.
/// </summary>
public sealed class MainViewModel : ObservableObject, IDisposable
{
    private readonly Dispatcher _dispatcher;
    private readonly LiveTree _liveTree = new();
    private readonly WatchSession _watch = new();
    private readonly LvtCli _cli;

    private string _statusText = "Drag the crosshair onto a window to inspect it.";
    private string _targetText = "No target";
    private bool _useUia = true;
    private ElementNodeViewModel? _selectedElement;
    private string? _currentHwndHex;
    private IntPtr _currentHwnd;
    private CancellationTokenSource? _slowConnectHintCts;

    public MainViewModel(Dispatcher dispatcher)
    {
        _dispatcher = dispatcher;
        LvtExePath = LvtLocator.Find();
        _cli = new LvtCli(LvtExePath);

        _watch.EventReceived += evt => _dispatcher.BeginInvoke(() => OnWatchEvent(evt));
        _watch.DiagnosticReceived += line => _dispatcher.BeginInvoke(() => StatusText = line);
        _watch.Exited += code => _dispatcher.BeginInvoke(() =>
        {
            Logger.Log("viewmodel", $"watch Exited(code={code}) -> IsConnected=false, clearing tree/selection");
            _slowConnectHintCts?.Cancel(); // don't let a pending hint override this status
            StatusText = $"lvt watch exited (code {code}) — target likely closed. Re-pick a window to reconnect.";
            // The target's own process may have crashed or been closed: its
            // tree is no longer meaningful, and clearing SelectedElement is
            // what tells MainWindow's highlight overlay (item 1) to hide
            // rather than being left pointing at the last-known bounds of a
            // now-gone window forever. IsConnected also drops, disabling
            // the element-pick crosshair (item 2) until a fresh connection
            // gives it something live to pick from again.
            _liveTree.Reset();
            SelectedElement = null;
            IsConnected = false;
        });

        ToggleCommand = new RelayCommand(p => _ = ToggleAsync(p as PropertyRowViewModel));
        SetValueCommand = new RelayCommand(p => _ = SetValueAsync(p as PropertyRowViewModel));
        ReconnectCommand = new RelayCommand(_ => Reconnect(), _ => _currentHwndHex != null);
        FindNextCommand = new RelayCommand(_ => FindNext());
        FindPreviousCommand = new RelayCommand(_ => FindPrevious());
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
    /// checkbox. Populated lazily as watch events report elements with a
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
            if (!SetField(ref _selectedElement, value))
                return;
            // The live tree only carries the fast/cheap property set by
            // default (see ConnectTo's --fast comment) — a selected node's
            // *exhaustive* set is worth the cost of one extra one-shot call,
            // since it only happens when the user actually looks at one
            // element, not for the whole tree on every tick.
            _ = RefreshFullPropertiesAsync(value);
        }
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
    public RelayCommand ReconnectCommand { get; }
    public RelayCommand FindNextCommand { get; }
    public RelayCommand FindPreviousCommand { get; }

    private void OnWatchEvent(WatchEventDto evt)
    {
        // Data has arrived: whatever the slow-connect hint below was about
        // to say (or already said) no longer applies — the first tick
        // finished, however long it took.
        _slowConnectHintCts?.Cancel();
        _liveTree.Apply(evt);
        DiscoverFrameworks(evt);
        ApplyFrameworkFilter();
    }

    /// <summary>
    /// A rich UI tree's first connect can take well over the couple of
    /// seconds a user would wait before assuming the viewer is stuck — a
    /// UWP app like Microsoft Store measured at ~20s for its first watch
    /// tick (InitializeXamlDiagnosticsEx replaying its whole tree, then the
    /// property/bounds walk, all before the first line of output). Rather
    /// than silently sitting on "Connecting…" that whole time, update the
    /// status once a delay threshold passes to say so explicitly. Canceled
    /// by OnWatchEvent the moment real data arrives, so this never overwrites
    /// a status that has since moved on.
    private async void ArmSlowConnectHint()
    {
        _slowConnectHintCts?.Cancel();
        var cts = new CancellationTokenSource();
        _slowConnectHintCts = cts;
        try
        {
            await Task.Delay(TimeSpan.FromSeconds(4), cts.Token);
            if (!cts.IsCancellationRequested)
                StatusText = "Still connecting — a rich UI tree (e.g. Microsoft Store, File Explorer) " +
                              "can take 15\u201320+ seconds to load on the first connect. Please wait…";
        }
        catch (TaskCanceledException)
        {
            // Data arrived first; nothing to show.
        }
    }

    /// <summary>Adds a checkbox (default checked) for any not-yet-seen framework value in this event.</summary>
    private void DiscoverFrameworks(WatchEventDto evt)
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
    public void ConnectTo(IntPtr hwnd)
    {
        NativeMethodsWindowInfo(hwnd, out var pid, out var title);
        Logger.Log("viewmodel", $"ConnectTo hwnd=0x{hwnd.ToInt64():X} pid={pid} title=\"{title}\" uia={UseUia}");

        _currentHwnd = hwnd;
        _currentHwndHex = "0x" + hwnd.ToInt64().ToString("X", CultureInfo.InvariantCulture);
        IsConnected = true;

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
        StatusText = "Connecting…";
        _liveTree.Reset();
        FrameworkFilters.Clear(); // new target: fresh discovery, previous app's types no longer apply
        // --fast: the live tree only needs bounds/Text/Content/basic state to
        // browse, search, and highlight/hit-test by (see MainWindow.xaml.cs) —
        // it does not need every XAML/WinUI3 element's full property chain
        // eagerly. A selected node's exhaustive property set is fetched
        // on demand instead (see MainViewModel's property-panel wiring),
        // so this trades nothing the viewer actually shows by default for a
        // dramatically faster live connect on a rich tree.
        _watch.Start(LvtExePath, _currentHwndHex, UseUia, fastProperties: true);
        ArmSlowConnectHint();
        StatusText = UseUia
            ? "Watching the UI Automation tree live."
            : "Watching the visual tree live.";
    }

    private void Reconnect()
    {
        if (_currentHwndHex == null)
            return;
        Logger.Log("viewmodel", $"Reconnect hwnd={_currentHwndHex} uia={UseUia}");
        _liveTree.Reset();
        StatusText = "Reconnecting…";
        IsConnected = true;
        _watch.Start(LvtExePath, _currentHwndHex, UseUia, fastProperties: true);
        ArmSlowConnectHint();
        StatusText = UseUia
            ? "Watching the UI Automation tree live."
            : "Watching the visual tree live.";
    }

    private static void NativeMethodsWindowInfo(IntPtr hwnd, out uint pid, out string title)
    {
        Interop.NativeMethods.GetWindowThreadProcessId(hwnd, out pid);
        title = Interop.NativeMethods.GetWindowTitle(hwnd);
    }

    /// <summary>
    /// Fetches <paramref name="node"/>'s full property set with a one-shot
    /// "lvt query" call (no --fast, so this always gets everything the
    /// exhaustive GetPropertyValuesChain walk reports, regardless of the
    /// live tree's own fast/full mode) and merges it into the node's
    /// PropertyRows. "query" without a property name dumps every property
    /// as flat top-level JSON fields (main.cpp's query_element_to_json) —
    /// a different shape from watch's nested {properties: {...}}, so this
    /// is parsed directly rather than reusing ElementDto.
    /// </summary>
    private async System.Threading.Tasks.Task RefreshFullPropertiesAsync(ElementNodeViewModel? node)
    {
        if (node == null || _currentHwndHex == null)
            return;

        var result = await _cli.RunAsync("query", node.Key, "--hwnd", _currentHwndHex);
        // The selection (or the connection) may have moved on while this
        // one-shot call was in flight; a stale result must never overwrite
        // whatever is selected now.
        if (!result.Ok || SelectedElement != node)
            return;

        System.Text.Json.JsonDocument doc;
        try
        {
            doc = System.Text.Json.JsonDocument.Parse(result.StdOut);
        }
        catch (System.Text.Json.JsonException)
        {
            return; // tolerate a malformed/partial response rather than crashing the panel
        }

        using (doc)
        {
            foreach (var prop in doc.RootElement.EnumerateObject())
            {
                if (prop.Name is "id" or "key" or "type" or "framework" or "className" or "text" or "bounds")
                    continue; // already shown by dedicated ElementNodeViewModel fields, not a property row
                var value = prop.Value.ValueKind == System.Text.Json.JsonValueKind.String
                    ? prop.Value.GetString() ?? ""
                    : prop.Value.ToString();
                node.SetProperty(prop.Name, value);
            }
        }
    }

    private async System.Threading.Tasks.Task ToggleAsync(PropertyRowViewModel? row)
    {
        if (row == null || SelectedElement == null || _currentHwndHex == null)
            return;
        StatusText = $"Toggling {SelectedElement.DisplayName}…";
        var result = await _cli.RunAsync("toggle", SelectedElement.Key, "--hwnd", _currentHwndHex);
        StatusText = result.Ok
            ? "Toggled. The live tree will confirm the new state shortly."
            : $"Toggle failed: {result.StdErr.Trim()}";
    }

    private async System.Threading.Tasks.Task SetValueAsync(PropertyRowViewModel? row)
    {
        if (row == null || SelectedElement == null || _currentHwndHex == null)
            return;
        StatusText = $"Setting {row.Name} on {SelectedElement.DisplayName}…";
        var result = await _cli.RunAsync(
            "set-value", SelectedElement.Key, row.EditText, "--hwnd", _currentHwndHex);
        StatusText = result.Ok
            ? "Value set. The live tree will confirm it shortly."
            : $"set-value failed: {result.StdErr.Trim()}";
    }

    public void Dispose()
    {
        _slowConnectHintCts?.Cancel();
        _watch.Dispose();
    }
}
