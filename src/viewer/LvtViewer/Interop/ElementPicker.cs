using System;
using System.Windows;
using System.Windows.Input;

namespace LvtViewer.Interop;

/// <summary>
/// Press-and-drag gesture with the same interaction shape as CrosshairPicker
/// (item 2's point-to-select), but for picking an *element* within the
/// already-connected target rather than a top-level window to connect to.
///
/// Unlike CrosshairPicker, this class reports only raw screen points: it has
/// no idea what an "element" is, and hit-testing against the live element
/// tree is lvt-specific data this class deliberately stays decoupled from
/// (see MainWindow.FindDeepestElementAt / SelectElementInTree, which own
/// that logic instead).
/// </summary>
public sealed class ElementPicker
{
    private readonly FrameworkElement _handle;
    private bool _dragging;

    /// <summary>Fires continuously while dragging, with the current cursor position.</summary>
    public event Action<POINT>? Dragging;

    /// <summary>Fires once on release; null only if the cursor position could not be read.</summary>
    public event Action<POINT?>? Picked;

    /// <summary>Fires with a short hint whenever dragging starts/stops, for a status-bar cue.</summary>
    public event Action<string>? HintChanged;

    public ElementPicker(FrameworkElement handle)
    {
        _handle = handle;
        _handle.MouseLeftButtonDown += OnMouseDown;
        _handle.MouseMove += OnMouseMove;
        _handle.MouseLeftButtonUp += OnMouseUp;
        _handle.LostMouseCapture += OnLostCapture;
    }

    private void OnMouseDown(object sender, MouseButtonEventArgs e)
    {
        _dragging = true;
        _handle.CaptureMouse();
        _handle.Cursor = Cursors.Cross;
        HintChanged?.Invoke("Release over the target's UI to select that element in the tree…");
        e.Handled = true;
    }

    private void OnMouseMove(object sender, MouseEventArgs e)
    {
        if (!_dragging)
            return;
        if (NativeMethods.GetCursorPos(out var pt))
            Dragging?.Invoke(pt);
    }

    private void OnMouseUp(object sender, MouseButtonEventArgs e)
    {
        if (!_dragging)
            return;
        _dragging = false;
        _handle.ReleaseMouseCapture();
        POINT? result = NativeMethods.GetCursorPos(out var pt) ? pt : null;
        Picked?.Invoke(result);
    }

    private void OnLostCapture(object sender, MouseEventArgs e)
    {
        _dragging = false;
        _handle.Cursor = null;
    }
}
