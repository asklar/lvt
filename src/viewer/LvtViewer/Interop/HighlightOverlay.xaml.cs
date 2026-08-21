using System;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;

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

    [DllImport("user32.dll")]
    private static extern int GetWindowLong(IntPtr hwnd, int index);

    [DllImport("user32.dll")]
    private static extern int SetWindowLong(IntPtr hwnd, int index, int value);

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

    /// <summary>Repositions the highlight over a window's visible frame (in physical pixels).</summary>
    public void MoveTo(RECT physicalFrame)
    {
        var dpiScale = VisualTreeHelper.GetDpi(this).DpiScaleX;
        Left = physicalFrame.Left / dpiScale;
        Top = physicalFrame.Top / dpiScale;
        Width = Math.Max(0, physicalFrame.Width / dpiScale);
        Height = Math.Max(0, physicalFrame.Height / dpiScale);
    }
}
