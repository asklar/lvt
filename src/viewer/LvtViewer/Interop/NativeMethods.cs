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
/// (Interop/CrosshairPicker.cs), which resolves a screen point to the
/// topmost actually-visible top-level window there via EnumWindows (Z-order)
/// filtered by IsWindowVisible/IsIconic/IsCloaked and a rect hit test — see
/// CrosshairPicker.ResolveWindowUnderCursor for why WindowFromPoint alone
/// is not enough.
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

    // A cloaked window (DWM hides it — a UWP app on another virtual desktop,
    // or one DWM is mid-transition on) is still a perfectly valid HWND that
    // WindowFromPoint/EnumWindows will happily return, but it is not what
    // the user can actually see on screen, and its rect can be stale
    // garbage from whenever it was last actually shown. Skipping cloaked
    // windows is what keeps the crosshair from ever picking one.
    public const int DWMWA_CLOAKED = 14;

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

    [DllImport("user32.dll")]
    public static extern bool IsWindowVisible(IntPtr hwnd);

    [DllImport("kernel32.dll")]
    public static extern uint GetCurrentProcessId();

    public delegate bool EnumWindowsProc(IntPtr hwnd, IntPtr lParam);

    // Enumerates top-level windows in top-to-bottom Z-order — exactly the
    // order a hit test needs to try them in, so the first one whose rect
    // contains the point (after skipping minimized/invisible/cloaked ones)
    // is correctly the topmost *visible* window at that point, not merely
    // the topmost window of any kind.
    [DllImport("user32.dll")]
    public static extern bool EnumWindows(EnumWindowsProc callback, IntPtr lParam);

    [DllImport("dwmapi.dll")]
    public static extern int DwmGetWindowAttribute(IntPtr hwnd, int attribute, out RECT value, int size);

    [DllImport("dwmapi.dll", EntryPoint = "DwmGetWindowAttribute")]
    public static extern int DwmGetWindowAttributeInt(IntPtr hwnd, int attribute, out int value, int size);

    [DllImport("user32.dll")]
    public static extern uint GetDpiForSystem();

    /// <summary>
    /// lvt.exe is a plain console app with no DPI-awareness declaration, so
    /// Windows silently virtualizes every Win32 coordinate query it makes
    /// (GetWindowRect and friends) down to a 96-DPI-equivalent space —
    /// verified live on a 150%-scaled system: lvt.exe reported a window's
    /// rect scaled down by exactly 1/1.5 from its true physical rect. Every
    /// bounds value the viewer gets *from lvt* (ElementNodeViewModel's
    /// Bounds* fields, ultimately from an MCP tree-resource payload) is
    /// in that same virtualized space, and must be scaled by this factor
    /// before it can be compared against or used to position anything this
    /// (Per-Monitor-V2 DPI aware, the .NET default) process gets directly
    /// from Win32 itself — e.g. GetCursorPos, or another window's
    /// GetVisibleFrame — which are already true physical pixels needing no
    /// conversion at all. Two real bugs came from conflating these: the
    /// selection highlight landing nowhere near the actual element (lvt's
    /// virtualized bounds used as if already physical), and — after a first
    /// attempt fixed that by scaling unconditionally inside HighlightOverlay
    /// — the crosshair-drag preview highlight breaking instead (it was
    /// already-physical, and got double-scaled).
    /// </summary>
    public static double LvtToPhysicalDpiScale => GetDpiForSystem() / 96.0;

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

    /// <summary>Whether DWM is currently hiding this window — see DWMWA_CLOAKED's comment.</summary>
    public static bool IsCloaked(IntPtr hwnd) =>
        DwmGetWindowAttributeInt(hwnd, DWMWA_CLOAKED, out int cloaked, sizeof(int)) == 0 && cloaked != 0;

    /// <summary>
    /// True if some other, actually-visible top-level window — not
    /// belonging to this process, and not <paramref name="targetHwnd"/>
    /// itself — is stacked above <paramref name="targetHwnd"/> at
    /// <paramref name="point"/>, i.e. targetHwnd is not what the user would
    /// actually see there right now.
    ///
    /// This exists because Win32's owned-window z-order rule ("an owned
    /// window always stays above its owner") only guarantees that one
    /// direction: it does not guarantee staying *below* whatever unrelated
    /// window already happens to be above the owner. Any subsequent z-order
    /// recalculation re-snaps an owned window directly above its owner
    /// regardless of what unrelated window was on top a moment
    /// before — so HighlightOverlay cannot rely on ownership/SetWindowPos
    /// alone to stay hidden behind a covering app; it must actually check.
    /// Mirrors CrosshairPicker.ResolveWindowUnderCursor's top-to-bottom
    /// EnumWindows technique so both share one source of truth for "is my
    /// target actually visible here".
    /// </summary>
    public static bool IsOccludedAt(IntPtr targetHwnd, POINT point)
    {
        uint ownPid = GetCurrentProcessId();
        bool occluded = false;
        bool reachedTarget = false;

        EnumWindows((hwnd, _) =>
        {
            if (hwnd == targetHwnd)
            {
                reachedTarget = true;
                return false; // stop — nothing above targetHwnd covered the point
            }

            // Never let our own viewer/overlay windows count as "occluding"
            // the target, even if one is visually positioned over it.
            GetWindowThreadProcessId(hwnd, out var pid);
            if (pid == ownPid)
                return true;

            if (!IsWindowVisible(hwnd) || IsIconic(hwnd) || IsCloaked(hwnd))
                return true;

            var rect = GetVisibleFrame(hwnd);
            if (rect.Width <= 0 || rect.Height <= 0)
                return true;
            if (point.X < rect.Left || point.X >= rect.Right ||
                point.Y < rect.Top || point.Y >= rect.Bottom)
                return true;

            occluded = true;
            return false;
        }, IntPtr.Zero);

        // If targetHwnd was never reached (e.g. it has since been
        // destroyed, or is no longer a top-level window), treat it as
        // occluded/not-visible rather than assuming it is fine to show.
        return occluded || !reachedTarget;
    }
}
