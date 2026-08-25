using System;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;

namespace LvtViewer.Interop;

/// <summary>
/// A borderless, click-through window that draws a highlight rectangle
/// around whatever top-level window the crosshair drag is currently
/// hovering — the same visual feedback Inspect.exe gives while its
/// viewfinder is being dragged.
///
/// Deliberately NOT WS_EX_TOPMOST/Topmost="True": an earlier version used
/// that, which kept the highlight visible on top of *everything* even when
/// the window it was supposedly highlighting was itself minimized or
/// covered by some unrelated window — Topmost places a window in its own
/// always-on-top band, entirely independent of whatever it is meant to be
/// annotating. Instead, SetOwner makes this window a native Win32-owned
/// window of whichever HWND it is currently highlighting: Windows enforces
/// that an owned window always stays directly above its owner in z-order
/// (and only there), so if some other window covers the target, the
/// highlight is covered right along with it — exactly the behavior wanted,
/// with no separate occlusion-detection logic required.
/// </summary>
public partial class HighlightOverlay : Window
{
    private const int GWL_EXSTYLE = -20;
    private const int GWLP_HWNDPARENT = -8;
    private const int WS_EX_TRANSPARENT = 0x00000020;
    private const int WS_EX_LAYERED = 0x00080000;
    private const int WS_EX_TOOLWINDOW = 0x00000080;
    private const int WS_EX_NOACTIVATE = 0x08000000;

    private const uint SWP_NOSIZE = 0x0001;
    private const uint SWP_NOMOVE = 0x0002;
    private const uint SWP_NOACTIVATE = 0x0010;
    private const uint SWP_NOZORDER = 0x0004;

    [DllImport("user32.dll")]
    private static extern int GetWindowLong(IntPtr hwnd, int index);

    [DllImport("user32.dll")]
    private static extern int SetWindowLong(IntPtr hwnd, int index, int value);

    // GWLP_HWNDPARENT stores a window handle, which is pointer-sized (64-bit
    // on x64 — this whole project is x64-only, but the distinction still
    // matters here specifically): the plain 32-bit SetWindowLong/GetWindowLong
    // pair above is fine for GWL_EXSTYLE (a genuinely 32-bit style bitmask)
    // but would silently truncate an HWND passed through it, so the owner
    // relationship needs its own, pointer-width pair.
    [DllImport("user32.dll", EntryPoint = "SetWindowLongPtr")]
    private static extern IntPtr SetWindowLongPtr(IntPtr hwnd, int index, IntPtr value);

    [DllImport("user32.dll")]
    private static extern bool SetWindowPos(IntPtr hWnd, IntPtr hWndInsertAfter,
        int x, int y, int cx, int cy, uint uFlags);

    private IntPtr _owner = IntPtr.Zero;

    public HighlightOverlay()
    {
        InitializeComponent();
        SourceInitialized += OnSourceInitialized;
    }

    private void OnSourceInitialized(object? sender, EventArgs e)
    {
        var hwnd = new WindowInteropHelper(this).Handle;
        var style = GetWindowLong(hwnd, GWL_EXSTYLE);
        SetWindowLong(hwnd, GWL_EXSTYLE,
            style | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE);
    }

    /// <summary>
    /// Makes this window a native owned window of <paramref name="targetHwnd"/>
    /// — see the class comment for why this replaces Topmost. Both callers
    /// (CrosshairPicker, which retargets on every dragged-over window, and
    /// MainWindow, which retargets whenever the connected target or the
    /// selected element's host changes) are expected to call this before
    /// every Show()/MoveTo(), not just once, since the window being
    /// highlighted can change across the highlight's lifetime.
    /// </summary>
    public void SetOwner(IntPtr targetHwnd)
    {
        if (targetHwnd == IntPtr.Zero || targetHwnd == _owner)
            return;
        _owner = targetHwnd;

        var hwnd = new WindowInteropHelper(this).EnsureHandle();
        SetWindowLongPtr(hwnd, GWLP_HWNDPARENT, targetHwnd);

        // Changing GWLP_HWNDPARENT only updates which window this one is now
        // considered owned by; the OS re-enforces "an owned window stays
        // directly above its owner" the next time the *owner's* own
        // z-position changes, not necessarily the instant ownership itself
        // is reassigned. Without this explicit repositioning, retargeting
        // the highlight onto a new window could leave it showing at
        // whatever z-position it last held — e.g. still on top of some
        // unrelated window that happens to cover the new target — until
        // something else nudges the target's z-order.
        SetWindowPos(hwnd, targetHwnd, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    /// <summary>
    /// Repositions the highlight over an absolute-screen rectangle that is
    /// already in true physical pixels — this class does no DPI conversion
    /// of its own and must not, because its two callers' rects do not start
    /// out in the same coordinate space:
    ///
    ///  - CrosshairPicker calls NativeMethods.GetVisibleFrame(hwnd) directly,
    ///    from this (Per-Monitor-V2 DPI aware, the .NET default) process, so
    ///    that rect is already true physical pixels with no conversion
    ///    needed.
    ///  - MainWindow builds a rect from ElementNodeViewModel's Bounds*
    ///    fields, which ultimately came from lvt.exe — a plain console app
    ///    with no DPI-awareness declaration, so Windows silently virtualizes
    ///    every Win32 coordinate query it makes down to a 96-DPI-equivalent
    ///    space. That rect needs scaling up to physical pixels *before* it
    ///    reaches this method (see MainWindow.ToPhysicalRect).
    ///
    /// An earlier version of this method applied that lvt-specific scaling
    /// unconditionally, which was correct for the second caller and broke
    /// the first: it double-scaled CrosshairPicker's already-physical rect,
    /// observed live as the crosshair-drag preview highlight landing
    /// nowhere near the actual window boundary. Converting at each call
    /// site instead, rather than here, is what lets this method make a
    /// single unconditional assumption (true physical pixels in) instead of
    /// somehow needing to know which caller it is being invoked from.
    /// </summary>
    public void MoveTo(RECT physicalFrame)
    {
        var hwnd = new WindowInteropHelper(this).EnsureHandle();

        int width = Math.Max(0, physicalFrame.Width);
        int height = Math.Max(0, physicalFrame.Height);
        SetWindowPos(hwnd, IntPtr.Zero, physicalFrame.Left, physicalFrame.Top, width, height,
                     SWP_NOACTIVATE | SWP_NOZORDER);

        // SetWindowPos moves the raw HWND, but WPF's own composition/render
        // pipeline tracks position and size through this Window's *own*
        // Left/Top/Width/Height DPs, entirely independent of the HWND's
        // actual Win32 position — SetWindowPos alone leaves that WPF-side
        // state stale. Observed live: the highlight stopped visually
        // following the target window as it moved, and only caught up once
        // something else (refocusing the viewer) forced WPF to redraw from
        // scratch. Syncing Left/Top/Width/Height here, right after the
        // move, keeps WPF's own understanding of where it is consistent
        // with reality, which is what makes it keep rendering continuously
        // on its own. GetDpi is queried *after* SetWindowPos specifically
        // so it reflects whichever monitor the window is on *now* — before
        // the move, it would still reflect the old one, reintroducing the
        // cross-monitor mismatch the physical-pixel-first design here
        // exists to avoid.
        var dpi = VisualTreeHelper.GetDpi(this);
        Left = physicalFrame.Left / dpi.DpiScaleX;
        Top = physicalFrame.Top / dpi.DpiScaleY;
        Width = width / dpi.DpiScaleX;
        Height = height / dpi.DpiScaleY;
    }
}
