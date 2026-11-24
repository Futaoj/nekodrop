// SPDX-License-Identifier: GPL-2.0-only
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;
using Microsoft.Win32;

namespace NekoDrop;

/// <summary>Windows 11 window chrome: Mica backdrop, dark title bar, rounded corners.</summary>
public static class Native
{
    const int DWMWA_USE_IMMERSIVE_DARK_MODE = 20;
    const int DWMWA_WINDOW_CORNER_PREFERENCE = 33;
    const int DWMWA_SYSTEMBACKDROP_TYPE = 38;

    [DllImport("dwmapi.dll")]
    static extern int DwmSetWindowAttribute(IntPtr hwnd, int attr, ref int value, int size);

    public static bool AppsUseDarkTheme()
    {
        using var key = Registry.CurrentUser.OpenSubKey(@"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize");
        return key?.GetValue("AppsUseLightTheme") is int v && v == 0;
    }

    [StructLayout(LayoutKind.Sequential)]
    struct Margins { public int Left, Right, Top, Bottom; }

    [DllImport("dwmapi.dll")]
    static extern int DwmExtendFrameIntoClientArea(IntPtr hwnd, ref Margins margins);

    static bool TransparencyEnabled()
    {
        using var key = Registry.CurrentUser.OpenSubKey(@"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize");
        return key?.GetValue("EnableTransparency") is not int v || v != 0;
    }

    /// <summary>
    /// Dark title bar, rounded corners, and Mica on Windows 11 22H2+ with
    /// transparency effects on. Mica needs the DWM frame over the whole client
    /// area and a transparent WPF surface; without both the window turns black,
    /// so either all of it applies (true: make the background transparent) or
    /// the plain theme background stays (false).
    /// </summary>
    public static bool ApplyMica(Window w)
    {
        var hwnd = new WindowInteropHelper(w).EnsureHandle();
        int dark = AppsUseDarkTheme() ? 1 : 0;
        DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, ref dark, sizeof(int));
        int round = 2;
        DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, ref round, sizeof(int));
        if (Environment.OSVersion.Version.Build < 22621 || !TransparencyEnabled()) return false;
        if (HwndSource.FromHwnd(hwnd) is not { CompositionTarget: { } target }) return false;
        var margins = new Margins { Left = -1, Right = -1, Top = -1, Bottom = -1 };
        if (DwmExtendFrameIntoClientArea(hwnd, ref margins) != 0) return false;
        int mica = 2;
        if (DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, ref mica, sizeof(int)) != 0)
        {
            var none = new Margins();
            DwmExtendFrameIntoClientArea(hwnd, ref none);
            return false;
        }
        target.BackgroundColor = System.Windows.Media.Colors.Transparent;
        return true;
    }

    /// <summary>
    /// The design's window: no system title bar (each window draws its own,
    /// 40 px, with its own buttons), Mica where Windows has it, the theme's
    /// solid background elsewhere.
    /// </summary>
    public static void Chrome(Window w, double caption = 40)
    {
        System.Windows.Shell.WindowChrome.SetWindowChrome(w, new System.Windows.Shell.WindowChrome
        {
            CaptionHeight = caption,
            GlassFrameThickness = new Thickness(-1),
            ResizeBorderThickness = new Thickness(6),
            UseAeroCaptionButtons = false,
        });
        w.SetResourceReference(System.Windows.Controls.Control.BackgroundProperty, "MicaSolid");
        w.SourceInitialized += (_, _) =>
        {
            NoSystemButtons(w);
            FitWorkArea(w);
            if (ApplyMica(w)) w.Background = System.Windows.Media.Brushes.Transparent;
        };
    }

    const int GWL_STYLE = -16;
    const int WS_SYSMENU = 0x00080000;

    [DllImport("user32.dll")]
    static extern short GetAsyncKeyState(int key);

    /// <summary>The left mouse button is held now, whichever program has the focus (#45).</summary>
    public static bool LeftButtonDown()
    {
        // VK_LBUTTON is the physical left button; with the buttons swapped it is VK_RBUTTON.
        var key = System.Windows.Forms.SystemInformation.MouseButtonsSwapped ? 0x02 : 0x01;
        return (GetAsyncKeyState(key) & 0x8000) != 0;
    }

    [DllImport("user32.dll", EntryPoint = "GetWindowLongW")]
    static extern int GetWindowLong(IntPtr hwnd, int index);

    [DllImport("user32.dll", EntryPoint = "SetWindowLongW")]
    static extern int SetWindowLong(IntPtr hwnd, int index, int value);

    /// <summary>
    /// With the DWM frame over the whole window (Mica), Windows draws its own
    /// minimize / maximize / close buttons over the design's, a second set in
    /// the corner. Without the system menu style it draws none; the window
    /// still minimizes from the taskbar and closes with Alt+F4.
    /// </summary>
    static void NoSystemButtons(Window w)
    {
        var hwnd = new WindowInteropHelper(w).EnsureHandle();
        SetWindowLong(hwnd, GWL_STYLE, GetWindowLong(hwnd, GWL_STYLE) & ~WS_SYSMENU);
    }

    /// <summary>The work area (no taskbar) of the screen under the mouse, in WPF units.</summary>
    public static Rect WorkArea(Window w)
    {
        var area = System.Windows.Forms.Screen.FromPoint(System.Windows.Forms.Cursor.Position).WorkingArea;
        var m = PresentationSource.FromVisual(w)?.CompositionTarget?.TransformFromDevice ?? System.Windows.Media.Matrix.Identity;
        var tl = m.Transform(new Point(area.Left, area.Top));
        var br = m.Transform(new Point(area.Right, area.Bottom));
        return new Rect(tl, br);
    }

    /// <summary>
    /// A window taller or wider than the screen (a laptop at 150 % has 720
    /// units) is cut off at the bottom: shrink it to the work area, centred.
    /// </summary>
    static void FitWorkArea(Window w)
    {
        if (w.Left < -10000) return; // drawn off screen for a snapshot
        var a = WorkArea(w);
        var fit = false;
        if (w.Height > a.Height - 16) { w.Height = a.Height - 16; fit = true; }
        if (w.Width > a.Width - 16) { w.Width = a.Width - 16; fit = true; }
        if (w.MinHeight > w.Height) w.MinHeight = w.Height;
        if (w.MinWidth > w.Width) w.MinWidth = w.Width;
        if (fit && w.WindowStartupLocation == WindowStartupLocation.CenterScreen)
        {
            w.Left = a.Left + (a.Width - w.Width) / 2;
            w.Top = a.Top + (a.Height - w.Height) / 2;
        }
    }

    /// <summary>The user's accent color (DWM colorization), opaque.</summary>
    public static System.Windows.Media.Color Accent()
    {
        using var key = Registry.CurrentUser.OpenSubKey(@"Software\Microsoft\Windows\DWM");
        if (key?.GetValue("AccentColor") is int abgr)
            return System.Windows.Media.Color.FromRgb((byte)abgr, (byte)(abgr >> 8), (byte)(abgr >> 16));
        return System.Windows.Media.Color.FromRgb(0x00, 0x67, 0xC0);
    }
}
