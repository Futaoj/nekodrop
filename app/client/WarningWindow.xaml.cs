// SPDX-License-Identifier: GPL-2.0-only
using System.Windows;
using System.Windows.Input;

namespace NekoDrop;

/// <summary>
/// The one warning surface for a file that cannot be sent: a folder or package,
/// a missing or non-regular object, an unreadable file, or two files with the
/// same name. It follows the app's card style instead of a Win32 message box.
/// </summary>
public partial class WarningWindow : Window
{
    public WarningWindow(string title, string lead, string body)
    {
        InitializeComponent();
        Title = title;
        TitleText.Text = title;
        LeadText.Text = lead;
        BodyText.Text = body;
        PillText.Text = L.T("dialog.warning");
        ConfirmButton.Content = L.T("dialog.confirm");
        Loaded += (_, _) => ConfirmButton.Focus();
    }

    internal static void Show(Window? owner, SendProblem problem) =>
        Show(owner, problem.Title, problem.Lead, problem.Body);

    internal static void Show(Window? owner, string title, string lead, string body)
    {
        var window = new WarningWindow(title, lead, body);
        if (owner is { IsVisible: true }) window.Owner = owner;
        window.ShowDialog();
    }

    void HeaderDrag(object sender, MouseButtonEventArgs e)
    {
        if (e.ChangedButton != MouseButton.Left) return;
        try { DragMove(); }
        catch (InvalidOperationException) { }
    }

    void Close_Click(object sender, RoutedEventArgs e) => Close();

    void Window_KeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key == Key.Escape) Close();
    }
}
