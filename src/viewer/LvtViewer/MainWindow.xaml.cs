using System.Collections.Generic;
using System.ComponentModel;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Threading;
using LvtViewer.Interop;
using LvtViewer.ViewModels;

namespace LvtViewer;

/// <summary>
/// Interaction logic for MainWindow.xaml. Wires the crosshair picker (which
/// needs a real HWND and mouse-capture semantics, so it lives in code-behind
/// rather than being bindable from the view model) and the TreeView's
/// selection (TreeView.SelectedItem has no setter, so it cannot be bound
/// directly either).
/// </summary>
public partial class MainWindow : Window
{
    private readonly MainViewModel _viewModel;
    private CrosshairPicker? _picker;
    private ElementPicker? _elementPicker;

    // Reuses the same borderless/click-through HighlightOverlay class the
    // crosshair-drag gesture uses (its own separate instance — see
    // CrosshairPicker's own _overlay field) to show SelectedElement's
    // bounds on the *target* app (item 1). The two never show at once: the
    // crosshair overlay only appears while dragging, before a target is
    // connected; this one only appears once an element is selected in an
    // already-connected tree.
    private readonly HighlightOverlay _selectionHighlight = new();
    private ElementNodeViewModel? _highlightedNode;

    public MainWindow()
    {
        InitializeComponent();
        _viewModel = new MainViewModel(Dispatcher);
        DataContext = _viewModel;
        _viewModel.SearchMatchFound += SelectElementInTree;

        Tree.SelectedItemChanged += (_, e) =>
        {
            _viewModel.SelectedElement = e.NewValue as ElementNodeViewModel;
            UpdateSelectionHighlight();
        };

        _viewModel.PropertyChanged += (_, e) =>
        {
            // HighlightSelected: the user toggled the checkbox. SelectedElement:
            // covers MainViewModel clearing it itself (e.g. on watch.Exited,
            // when the target process crashed or closed — see MainViewModel's
            // constructor) as well as the TreeView.SelectedItemChanged case
            // already handled above; without this, a programmatic clear would
            // leave the overlay pointing at a now-gone window's last-known
            // bounds forever.
            if (e.PropertyName is nameof(MainViewModel.HighlightSelected) or nameof(MainViewModel.SelectedElement))
                UpdateSelectionHighlight();
        };

        Loaded += (_, _) =>
        {
            _picker = new CrosshairPicker(CrosshairHandle, this);
            _picker.TargetPicked += hwnd => _viewModel.ConnectTo(hwnd);
            _picker.HintChanged += hint => _viewModel.StatusText = hint;

            _elementPicker = new ElementPicker(ElementPickHandle, this);
            _elementPicker.HintChanged += hint => _viewModel.StatusText = hint;
            _elementPicker.Dragging += pt => PreviewElementAt(pt);
            _elementPicker.Picked += pt =>
            {
                var node = pt.HasValue ? FindDeepestElementAtPhysicalPoint(pt.Value) : null;
                if (node != null)
                {
                    SelectElementInTree(node);
                    _viewModel.StatusText = $"Selected {node.DisplayName}.";
                }
                else
                {
                    _viewModel.StatusText = "No element found at that point — try releasing further inside the target's UI.";
                    UpdateSelectionHighlight(); // drop the drag preview, restore the real selection's highlight
                }
            };
        };

        Closed += (_, _) =>
        {
            _viewModel.Dispose();
            _selectionHighlight.Close();
        };
    }

    /// <summary>
    /// Shows a live preview highlight while dragging the element-pick
    /// crosshair (item 2), without disturbing SelectedElement's own tracked
    /// highlight (that resumes automatically once a pick actually lands).
    /// </summary>
    private void PreviewElementAt(POINT pt)
    {
        var node = FindDeepestElementAtPhysicalPoint(pt);
        if (node == null || IsTargetMinimized())
        {
            _selectionHighlight.Hide();
            return;
        }
        var rect = ToPhysicalRect(node);
        if (rect.Width <= 0 || rect.Height <= 0)
        {
            _selectionHighlight.Hide();
            return;
        }
        // Element-picking only ever operates within the already-connected
        // target, so the owner is always CurrentHwnd here. Track() owns +
        // positions + starts occlusion/minimize polling (see
        // HighlightOverlay's class comment).
        _selectionHighlight.Track(_viewModel.CurrentHwnd, rect);
    }

    /// <summary>
    /// GetCursorPos (which ElementPicker uses) returns true physical pixels
    /// from this already-DPI-aware process; ElementNodeViewModel's Bounds*
    /// fields are in lvt's virtualized 96-DPI-equivalent space (see
    /// NativeMethods.LvtToPhysicalDpiScale). Hit-testing one against the
    /// other directly would only work by accident at 100% scaling —
    /// dividing the physical point down to lvt's space first is what makes
    /// FindDeepestElementAt's comparisons apples-to-apples on any scaled
    /// display.
    /// </summary>
    private ElementNodeViewModel? FindDeepestElementAtPhysicalPoint(POINT physicalPt)
    {
        double scale = NativeMethods.LvtToPhysicalDpiScale;
        int lvtX = (int)Math.Round(physicalPt.X / scale);
        int lvtY = (int)Math.Round(physicalPt.Y / scale);
        return FindDeepestElementAt(_viewModel.Roots, lvtX, lvtY);
    }

    /// <summary>
    /// Converts an ElementNodeViewModel's Bounds* (lvt's virtualized space)
    /// to the true physical pixels HighlightOverlay.MoveTo requires — see
    /// NativeMethods.LvtToPhysicalDpiScale for why this conversion exists at
    /// all and why it belongs at this call site specifically, not inside
    /// HighlightOverlay itself.
    /// </summary>
    private static Interop.RECT ToPhysicalRect(ElementNodeViewModel node)
    {
        double scale = NativeMethods.LvtToPhysicalDpiScale;
        int left = (int)Math.Round(node.BoundsX * scale);
        int top = (int)Math.Round(node.BoundsY * scale);
        int width = (int)Math.Round(node.BoundsWidth * scale);
        int height = (int)Math.Round(node.BoundsHeight * scale);
        return new Interop.RECT
        {
            Left = left,
            Top = top,
            Right = left + width,
            Bottom = top + height,
        };
    }

    /// <summary>
    /// Finds the deepest element whose bounds contain (x, y), searching the
    /// live tree client-side — lvt's bounds are already absolute screen
    /// pixels, so no round-trip to lvt.exe is needed for this (item 2).
    /// An element with zero/unknown bounds (a collapsed or never-laid-out
    /// node — see the XAML/WinUI3 bounds-collection path in lvt_tap.cpp)
    /// cannot be ruled out as "outside", so its children are still checked.
    ///
    /// A match with real matching descendants (bestDeep) always wins over a
    /// sibling that only matches itself (bestShallow), regardless of which
    /// one comes later in iteration order. This matters for exactly the
    /// case that surfaced it: UWP/ApplicationFrameHost windows carry a
    /// full-bounds "ApplicationFrameInputSinkWindow" utility HWND (a bare
    /// win32 leaf, no children of interest) as a *later* sibling of the
    /// actual XAML-hosting bridge — naively preferring "the last sibling
    /// that matches" (treating later-in-order as topmost/most-specific)
    /// picked the input sink over real content underneath it every time.
    /// Only among siblings in the *same* tier (both deep, or both shallow)
    /// does later-wins still apply, as a same-tier z-order tiebreaker.
    /// </summary>
    private static ElementNodeViewModel? FindDeepestElementAt(
        IEnumerable<ElementNodeViewModel> nodes, int x, int y)
    {
        ElementNodeViewModel? bestDeep = null;
        ElementNodeViewModel? bestShallow = null;
        foreach (var node in nodes)
        {
            bool hasBounds = node.BoundsWidth > 0 && node.BoundsHeight > 0;
            bool inside = hasBounds &&
                          x >= node.BoundsX && x < node.BoundsX + node.BoundsWidth &&
                          y >= node.BoundsY && y < node.BoundsY + node.BoundsHeight;
            if (hasBounds && !inside)
                continue; // definitely outside this subtree

            var childMatch = FindDeepestElementAt(node.Children, x, y);
            if (childMatch != null)
                bestDeep = childMatch;
            else if (inside)
                bestShallow = node;
        }
        return bestDeep ?? bestShallow;
    }

    /// <summary>
    /// Expands every ancestor and selects <paramref name="node"/> in the
    /// TreeView. WPF's TreeView has no bindable SelectedItem setter and only
    /// realizes a TreeViewItem container once its parent is actually
    /// expanded, so this expands the view-model side first (which flows to
    /// the TreeViewItems through their existing OneWay IsExpanded binding),
    /// then walks down resolving containers level by level, retrying a
    /// bounded number of times if a container has not been generated yet.
    /// </summary>
    private void SelectElementInTree(ElementNodeViewModel node)
    {
        var ancestors = new List<ElementNodeViewModel>();
        for (var p = node.Parent; p != null; p = p.Parent)
            ancestors.Add(p);
        ancestors.Reverse();
        foreach (var ancestor in ancestors)
            ancestor.IsExpanded = true;

        ancestors.Add(node);
        Dispatcher.InvokeAsync(() => ExpandAndSelectPath(ancestors, 0), DispatcherPriority.Loaded);
    }

    private void ExpandAndSelectPath(IReadOnlyList<ElementNodeViewModel> path, int attempt)
    {
        ItemsControl? container = Tree;
        for (var i = 0; i < path.Count; i++)
        {
            container?.UpdateLayout();
            if (container?.ItemContainerGenerator.ContainerFromItem(path[i]) is not TreeViewItem item)
            {
                if (attempt < 15)
                    Dispatcher.InvokeAsync(() => ExpandAndSelectPath(path, attempt + 1), DispatcherPriority.Loaded);
                return;
            }

            if (i == path.Count - 1)
            {
                item.IsSelected = true;
                item.BringIntoView();
                item.Focus();
                return;
            }

            item.IsExpanded = true;
            container = item;
        }
    }

    /// <summary>
    /// (Re)subscribes to the currently selected node's bounds changes and
    /// shows/hides/repositions the highlight overlay accordingly. Called
    /// whenever the selection changes or the "highlight selection" toggle
    /// flips.
    /// </summary>
    private void UpdateSelectionHighlight()
    {
        if (_highlightedNode != null)
            _highlightedNode.PropertyChanged -= OnHighlightedNodeBoundsChanged;

        _highlightedNode = _viewModel.HighlightSelected ? _viewModel.SelectedElement : null;

        if (_highlightedNode != null)
        {
            _highlightedNode.PropertyChanged += OnHighlightedNodeBoundsChanged;
            ShowHighlightForCurrentNode();
        }
        else
        {
            _selectionHighlight.Hide();
        }
    }

    private void OnHighlightedNodeBoundsChanged(object? sender, PropertyChangedEventArgs e)
    {
        // A live watch tick can move/resize the selected element (e.g. the
        // target window was resized); keep the overlay in sync rather than
        // leaving it pointing at a stale rectangle.
        if (e.PropertyName is nameof(ElementNodeViewModel.BoundsX) or nameof(ElementNodeViewModel.BoundsY)
            or nameof(ElementNodeViewModel.BoundsWidth) or nameof(ElementNodeViewModel.BoundsHeight))
            ShowHighlightForCurrentNode();
    }

    private void ShowHighlightForCurrentNode()
    {
        if (_highlightedNode == null)
            return;

        if (IsTargetMinimized())
        {
            _selectionHighlight.Hide();
            return;
        }

        // See ToPhysicalRect / NativeMethods.LvtToPhysicalDpiScale: lvt's
        // bounds are in its own virtualized (DPI-unaware) coordinate space,
        // not the true physical pixels HighlightOverlay.MoveTo requires.
        var rect = ToPhysicalRect(_highlightedNode);
        if (rect.Width <= 0 || rect.Height <= 0)
        {
            // A collapsed or never-laid-out element reports zero bounds;
            // there is nothing sensible to draw a rectangle around.
            _selectionHighlight.Hide();
            return;
        }

        // Tracks the connected target's own top-level window, not this
        // (the viewer's) window — see HighlightOverlay's class comment.
        // Called on every show (cheap no-op if unchanged) rather than once
        // at connect time so the overlay keeps tracking the right window
        // even across a reconnect.
        _selectionHighlight.Track(_viewModel.CurrentHwnd, rect);
    }

    /// <summary>
    /// A minimized target's bounds are meaningless to draw a highlight
    /// around (Windows moves a minimized window to a fixed off-screen
    /// "iconic" position, which is itself what naturally triggers this via
    /// the live tree's bounds-change wiring — the target's own window
    /// minimizing is a real bounds change, no separate polling needed).
    /// </summary>
    private bool IsTargetMinimized()
    {
        var hwnd = _viewModel.CurrentHwnd;
        return hwnd != IntPtr.Zero && NativeMethods.IsIconic(hwnd);
    }

    /// <summary>Enter finds the next match; Shift+Enter finds the previous — no need to tab to a button.</summary>
    private void SearchBox_KeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key != Key.Enter)
            return;
        var command = Keyboard.Modifiers.HasFlag(ModifierKeys.Shift)
            ? _viewModel.FindPreviousCommand
            : _viewModel.FindNextCommand;
        if (command.CanExecute(null))
        {
            command.Execute(null);
            e.Handled = true;
        }
    }
}

