using System;
using System.Windows;
using System.Windows.Input;
using System.Windows.Interop;

namespace LvtViewer.Interop;

/// <summary>
/// Implements Inspect.exe-style "viewfinder" targeting: press-and-drag a
/// crosshair handle; while dragging, whatever top-level window is under the
/// cursor is highlighted; on release, that window is resolved to a PID/HWND
/// and reported via <see cref="TargetPicked"/>.
/// </summary>
public sealed class CrosshairPicker
{
    private readonly FrameworkElement _handle;
    private readonly Window _ownerWindow;
    private readonly HighlightOverlay _overlay = new();
    private bool _dragging;
    private IntPtr _lastHighlighted = IntPtr.Zero;

    public event Action<IntPtr>? TargetPicked;

    /// <summary>Fires with a short hint whenever dragging starts/stops, for a status-bar cue.</summary>
    public event Action<string>? HintChanged;

    public CrosshairPicker(FrameworkElement handle, Window ownerWindow)
    {
        _handle = handle;
        _ownerWindow = ownerWindow;
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
        HintChanged?.Invoke("Release over a window to inspect it…");
        e.Handled = true;
    }

    private void OnMouseMove(object sender, MouseEventArgs e)
    {
        if (!_dragging)
            return;
        UpdateHighlight();
    }

    private void OnMouseUp(object sender, MouseButtonEventArgs e)
    {
        if (!_dragging)
            return;
        _dragging = false;
        _handle.ReleaseMouseCapture();
        var hwnd = ResolveWindowUnderCursor();
        HideHighlight();
        if (hwnd != IntPtr.Zero)
            TargetPicked?.Invoke(hwnd);
        else
            HintChanged?.Invoke("No window was under the cursor on release. Drag the crosshair onto a window to inspect it.");
    }

    private void OnLostCapture(object sender, MouseEventArgs e)
    {
        _dragging = false;
        _handle.Cursor = null;
        HideHighlight();
    }

    private void UpdateHighlight()
    {
        var hwnd = ResolveWindowUnderCursor();
        if (hwnd == _lastHighlighted)
            return;
        _lastHighlighted = hwnd;

        if (hwnd == IntPtr.Zero)
        {
            HideHighlight();
            return;
        }

        var rect = NativeMethods.GetVisibleFrame(hwnd);
        _overlay.MoveTo(rect);
        if (_overlay.Visibility != Visibility.Visible)
            _overlay.Show();
    }

    private void HideHighlight()
    {
        _lastHighlighted = IntPtr.Zero;
        if (_overlay.IsVisible)
            _overlay.Hide();
    }

    private IntPtr ResolveWindowUnderCursor()
    {
        if (!NativeMethods.GetCursorPos(out var pt))
            return IntPtr.Zero;

        var hwnd = NativeMethods.WindowFromPoint(pt);
        if (hwnd == IntPtr.Zero)
            return IntPtr.Zero;

        var root = NativeMethods.GetAncestor(hwnd, NativeMethods.GA_ROOT);
        if (root == IntPtr.Zero)
            return IntPtr.Zero;

        // Never resolve to our own toolbar/overlay windows.
        var ownHwnd = new WindowInteropHelper(_ownerWindow).Handle;
        var overlayHwnd = new WindowInteropHelper(_overlay).Handle;
        if (root == ownHwnd || root == overlayHwnd)
            return IntPtr.Zero;

        return root;
    }
}
