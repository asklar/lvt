using System.Collections.Generic;
using System.ComponentModel;
using System.Windows;
using System.Windows.Controls;
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

    // Reuses the same borderless/click-through/topmost overlay the
    // crosshair-drag gesture uses (Interop/HighlightOverlay.xaml) to show
    // SelectedElement's bounds on the *target* app (item 1). The two never
    // show at once: the crosshair overlay only appears while dragging,
    // before a target is connected; this one only appears once an element
    // is selected in an already-connected tree.
    private readonly HighlightOverlay _selectionHighlight = new();
    private ElementNodeViewModel? _highlightedNode;

    public MainWindow()
    {
        InitializeComponent();
        _viewModel = new MainViewModel(Dispatcher);
        DataContext = _viewModel;

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
            // WPF requires a window to have been shown before it can be
            // assigned as another window's Owner, so this can only happen
            // once MainWindow itself is loaded — setting it in the
            // constructor throws InvalidOperationException immediately on
            // every launch.
            _selectionHighlight.Owner = this;

            _picker = new CrosshairPicker(CrosshairHandle, this);
            _picker.TargetPicked += hwnd => _viewModel.ConnectTo(hwnd);
            _picker.HintChanged += hint => _viewModel.StatusText = hint;

            _elementPicker = new ElementPicker(ElementPickHandle);
            _elementPicker.HintChanged += hint => _viewModel.StatusText = hint;
            _elementPicker.Dragging += pt => PreviewElementAt(pt);
            _elementPicker.Picked += pt =>
            {
                var node = pt.HasValue ? FindDeepestElementAt(_viewModel.Roots, pt.Value.X, pt.Value.Y) : null;
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
        var node = FindDeepestElementAt(_viewModel.Roots, pt.X, pt.Y);
        if (node == null)
        {
            _selectionHighlight.Hide();
            return;
        }
        var rect = new Interop.RECT
        {
            Left = node.BoundsX,
            Top = node.BoundsY,
            Right = node.BoundsX + node.BoundsWidth,
            Bottom = node.BoundsY + node.BoundsHeight,
        };
        if (rect.Width <= 0 || rect.Height <= 0)
        {
            _selectionHighlight.Hide();
            return;
        }
        _selectionHighlight.MoveTo(rect);
        if (_selectionHighlight.Visibility != Visibility.Visible)
            _selectionHighlight.Show();
    }

    /// <summary>
    /// Finds the deepest element whose bounds contain (x, y), searching the
    /// live tree client-side — lvt's bounds are already absolute screen
    /// pixels, so no round-trip to lvt.exe is needed for this (item 2).
    /// An element with zero/unknown bounds (a collapsed or never-laid-out
    /// node — see the XAML/WinUI3 bounds-collection budget in lvt_tap.cpp)
    /// cannot be ruled out as "outside", so its children are still checked;
    /// among siblings that do match, the last one wins, which lines up with
    /// lvt's enumeration order generally reporting later/topmost content last.
    /// </summary>
    private static ElementNodeViewModel? FindDeepestElementAt(
        IEnumerable<ElementNodeViewModel> nodes, int x, int y)
    {
        ElementNodeViewModel? best = null;
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
                best = childMatch;
            else if (inside)
                best = node;
        }
        return best;
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

        // lvt's bounds are already absolute screen (physical) pixels — the
        // same coordinate space HighlightOverlay.MoveTo expects, since it is
        // also fed directly from GetVisibleFrame/GetWindowRect for the
        // crosshair-drag case (see NativeMethods.GetVisibleFrame).
        var rect = new Interop.RECT
        {
            Left = _highlightedNode.BoundsX,
            Top = _highlightedNode.BoundsY,
            Right = _highlightedNode.BoundsX + _highlightedNode.BoundsWidth,
            Bottom = _highlightedNode.BoundsY + _highlightedNode.BoundsHeight,
        };
        if (rect.Width <= 0 || rect.Height <= 0)
        {
            // A collapsed or never-laid-out element reports zero bounds;
            // there is nothing sensible to draw a rectangle around.
            _selectionHighlight.Hide();
            return;
        }

        _selectionHighlight.MoveTo(rect);
        if (_selectionHighlight.Visibility != Visibility.Visible)
            _selectionHighlight.Show();
    }
}

