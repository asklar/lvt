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
        // Mouse capture does not affect keyboard focus, so Escape has to be
        // caught at the window level rather than on _handle itself.
        _ownerWindow.PreviewKeyDown += OnPreviewKeyDown;
    }

    private void OnMouseDown(object sender, MouseButtonEventArgs e)
    {
        _dragging = true;
        _handle.CaptureMouse();
        _handle.Cursor = Cursors.Cross;
        HintChanged?.Invoke("Release over a window to inspect it… (Esc to cancel)");
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

    private void OnPreviewKeyDown(object sender, KeyEventArgs e)
    {
        if (!_dragging || e.Key != Key.Escape)
            return;
        e.Handled = true;
        HintChanged?.Invoke("Cancelled. Drag the crosshair onto a window to inspect it.");
        // Releasing capture routes through OnLostCapture, which already
        // does the rest of the cancel (clear _dragging, hide the highlight,
        // restore the cursor) — no need to duplicate that here.
        _handle.ReleaseMouseCapture();
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

        // Owns the overlay to whichever window is now under the cursor, so
        // its z-order tracks that window (see HighlightOverlay's class
        // comment) rather than staying independently on top of everything
        // as dragging moves from one candidate window to another.
        _overlay.SetOwner(hwnd);
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

    /// <summary>
    /// Finds the topmost window actually visible at the cursor — not just
    /// "whatever WindowFromPoint returns", which only reports Z-order among
    /// windows WindowFromPoint itself considers, and does not know about
    /// DWM cloaking (a UWP app on another virtual desktop, or one DWM is
    /// mid-transition on, is still cloaked-but-"there" and can report a
    /// completely stale rect — this was the direct cause of a highlight
    /// landing nowhere near any real window).
    ///
    /// EnumWindows visits top-level windows in top-to-bottom Z-order, so
    /// the first one that (a) is not our own toolbar/overlay, (b) is
    /// visible, not minimized, and not cloaked, and (c) actually contains
    /// the point is exactly the topmost visible window there — anything
    /// occluded by it, however large, is correctly never reached, and a
    /// minimized window (parked off-screen or not) is never a candidate at
    /// all rather than incidentally excluded by its rect missing the point.
    /// </summary>
    private IntPtr ResolveWindowUnderCursor()
    {
        if (!NativeMethods.GetCursorPos(out var pt))
            return IntPtr.Zero;

        var ownHwnd = new WindowInteropHelper(_ownerWindow).Handle;
        var overlayHwnd = new WindowInteropHelper(_overlay).Handle;

        var found = IntPtr.Zero;
        NativeMethods.EnumWindows((hwnd, _) =>
        {
            if (hwnd == ownHwnd || hwnd == overlayHwnd)
                return true; // keep looking

            if (!NativeMethods.IsWindowVisible(hwnd) || NativeMethods.IsIconic(hwnd) ||
                NativeMethods.IsCloaked(hwnd))
                return true;

            var rect = NativeMethods.GetVisibleFrame(hwnd);
            if (rect.Width <= 0 || rect.Height <= 0)
                return true;
            if (pt.X < rect.Left || pt.X >= rect.Right || pt.Y < rect.Top || pt.Y >= rect.Bottom)
                return true;

            found = hwnd;
            return false; // stop — found the topmost visible window at this point
        }, IntPtr.Zero);

        return found;
    }
}
