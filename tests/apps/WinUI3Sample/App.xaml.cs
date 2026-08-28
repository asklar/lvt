using System;
using System.Runtime.InteropServices;
using Microsoft.UI.Xaml;

namespace WinUI3Sample;

public partial class App : Application
{
    private Window? window;
    private nint secondaryHwnd;

    public App()
    {
        InitializeComponent();
    }

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        window = new MainWindow();
        window.Activate();

        if (Environment.GetEnvironmentVariable("LVT_TEST_SECONDARY_WINDOW") != "1")
            return;

        const uint overlappedWindow = 0x00CF0000;
        const uint visible = 0x10000000;
        const uint child = 0x40000000;
        const uint tabStop = 0x00010000;
        const uint autoHScroll = 0x0080;
        const uint autoCheckBox = 0x00000003;
        secondaryHwnd = CreateWindowExW(
            0, "STATIC", "LVT Native Secondary",
            overlappedWindow | visible,
            -2000, -2000, 360, 180,
            0, 0, GetModuleHandleW(null), 0);
        if (secondaryHwnd == 0)
            return;
        CreateWindowExW(
            0x00000200, "EDIT", "secondary value",
            child | visible | tabStop | autoHScroll,
            20, 24, 300, 28,
            secondaryHwnd, 2001, GetModuleHandleW(null), 0);
        CreateWindowExW(
            0, "BUTTON", "Secondary ready",
            child | visible | tabStop | autoCheckBox,
            20, 70, 200, 28,
            secondaryHwnd, 2002, GetModuleHandleW(null), 0);
        ShowWindow(secondaryHwnd, 5);
        UpdateWindow(secondaryHwnd);
    }

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern nint CreateWindowExW(
        uint exStyle, string className, string windowName, uint style,
        int x, int y, int width, int height,
        nint parent, nint menu, nint instance, nint parameter);

    [DllImport("user32.dll")]
    private static extern bool ShowWindow(nint hwnd, int command);

    [DllImport("user32.dll")]
    private static extern bool UpdateWindow(nint hwnd);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    private static extern nint GetModuleHandleW(string? moduleName);
}
