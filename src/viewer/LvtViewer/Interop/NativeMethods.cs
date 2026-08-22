using System;
using System.Runtime.InteropServices;

namespace LvtViewer.Interop;

[StructLayout(LayoutKind.Sequential)]
public struct POINT
{
    public int X;
    public int Y;
}

[StructLayout(LayoutKind.Sequential)]
public struct RECT
{
    public int Left;
    public int Top;
    public int Right;
    public int Bottom;

    public int Width => Right - Left;
    public int Height => Bottom - Top;
}

/// <summary>
/// P/Invoke declarations backing the crosshair-drag window picker
/// (Interop/CrosshairPicker.cs), which resolves a screen point to a
/// top-level window the same way Inspect.exe does: WindowFromPoint +
/// GetAncestor(GA_ROOT) + GetWindowThreadProcessId.
/// </summary>
public static class NativeMethods
{
    public const uint GA_ROOT = 2;

    // DWMWA_EXTENDED_FRAME_BOUNDS gives the visible window rectangle
    // (excluding the invisible resize-border padding Windows 10/11 add
    // around top-level windows), which is what should be highlighted —
    // GetWindowRect alone would draw the highlight noticeably outside the
    // window's visible edge.
    public const int DWMWA_EXTENDED_FRAME_BOUNDS = 9;

    [DllImport("user32.dll")]
    public static extern bool GetCursorPos(out POINT point);

    [DllImport("user32.dll")]
    public static extern IntPtr WindowFromPoint(POINT point);

    [DllImport("user32.dll")]
    public static extern IntPtr GetAncestor(IntPtr hwnd, uint flags);

    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint processId);

    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetWindowText(IntPtr hwnd, System.Text.StringBuilder text, int maxCount);

    [DllImport("user32.dll")]
    public static extern bool IsWindow(IntPtr hwnd);

    [DllImport("user32.dll")]
    public static extern bool IsIconic(IntPtr hwnd);

    [DllImport("dwmapi.dll")]
    public static extern int DwmGetWindowAttribute(IntPtr hwnd, int attribute, out RECT value, int size);

    /// <summary>The visible frame of <paramref name="hwnd"/>, preferring DWM's extended frame bounds.</summary>
    public static RECT GetVisibleFrame(IntPtr hwnd)
    {
        if (DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, out var dwmRect,
                Marshal.SizeOf<RECT>()) == 0)
        {
            return dwmRect;
        }
        GetWindowRect(hwnd, out var rect);
        return rect;
    }

    public static string GetWindowTitle(IntPtr hwnd)
    {
        var sb = new System.Text.StringBuilder(512);
        GetWindowText(hwnd, sb, sb.Capacity);
        return sb.ToString();
    }
}
