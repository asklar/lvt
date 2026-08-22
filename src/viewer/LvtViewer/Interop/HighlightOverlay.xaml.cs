using System;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;

namespace LvtViewer.Interop;

/// <summary>
/// A borderless, click-through, always-on-top window that draws a highlight
/// rectangle around whatever top-level window the crosshair drag is
/// currently hovering — the same visual feedback Inspect.exe gives while its
/// viewfinder is being dragged.
/// </summary>
public partial class HighlightOverlay : Window
{
    private const int GWL_EXSTYLE = -20;
    private const int WS_EX_TRANSPARENT = 0x00000020;
    private const int WS_EX_LAYERED = 0x00080000;
    private const int WS_EX_TOOLWINDOW = 0x00000080;
    private const int WS_EX_NOACTIVATE = 0x08000000;

    private const uint SWP_NOACTIVATE = 0x0010;
    private const uint SWP_NOZORDER = 0x0004;

    [DllImport("user32.dll")]
    private static extern int GetWindowLong(IntPtr hwnd, int index);

    [DllImport("user32.dll")]
    private static extern int SetWindowLong(IntPtr hwnd, int index, int value);

    [DllImport("user32.dll")]
    private static extern bool SetWindowPos(IntPtr hWnd, IntPtr hWndInsertAfter,
        int x, int y, int cx, int cy, uint uFlags);

    [DllImport("user32.dll")]
    private static extern uint GetDpiForSystem();

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
    /// Repositions the highlight over the rectangle lvt.exe reports (see
    /// NativeMethods.GetVisibleFrame / lvt's own "bounds" fields).
    ///
    /// lvt.exe is a plain console app with no DPI-awareness declaration, so
    /// Windows silently virtualizes every Win32 coordinate query it makes
    /// (GetWindowRect and friends) down to a 96-DPI-equivalent space — on
    /// a system at 150% scaling, verified live, lvt.exe reported a Microsoft
    /// Store window's rect as (161,319)-(702,826) while its true physical
    /// rect was (242,479)-(1053,1240): exactly a 1/1.5 scale-down. This
    /// viewer, on the other hand, is Per-Monitor-V2 DPI aware (the .NET
    /// default), so positioning its own HWND via SetWindowPos needs true
    /// physical pixels — using lvt's virtualized rect there directly (the
    /// previous bug) put the highlight in the wrong place by exactly that
    /// scale factor.
    ///
    /// Fixed by scaling the incoming rect up by the system DPI factor before
    /// calling SetWindowPos. This assumes one scale factor system-wide,
    /// which holds for a single monitor (this was reproduced and verified
    /// fixed on one) or a uniformly-scaled multi-monitor setup — the same
    /// assumption lvt.exe's own DPI-unaware status already makes, since
    /// Windows' virtualization for an unaware process has already collapsed
    /// away which specific monitor a coordinate came from by the time lvt
    /// reports it. WPF's own Left/Top/Width/Height need no such conversion:
    /// they are device-independent units, which is the same 96-DPI-
    /// equivalent space lvt's own numbers are already in.
    /// </summary>
    public void MoveTo(RECT lvtFrame)
    {
        var hwnd = new WindowInteropHelper(this).EnsureHandle();

        double dpiScale = GetDpiForSystem() / 96.0;
        int left = (int)Math.Round(lvtFrame.Left * dpiScale);
        int top = (int)Math.Round(lvtFrame.Top * dpiScale);
        int width = Math.Max(0, (int)Math.Round(lvtFrame.Width * dpiScale));
        int height = Math.Max(0, (int)Math.Round(lvtFrame.Height * dpiScale));
        SetWindowPos(hwnd, IntPtr.Zero, left, top, width, height, SWP_NOACTIVATE | SWP_NOZORDER);

        Left = lvtFrame.Left;
        Top = lvtFrame.Top;
        Width = Math.Max(0, lvtFrame.Width);
        Height = Math.Max(0, lvtFrame.Height);
    }
}
