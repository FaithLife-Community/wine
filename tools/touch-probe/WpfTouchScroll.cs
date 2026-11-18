// Build with the .NET Framework 4.8 C# compiler, targeting x64 and referencing
// WindowsBase, PresentationCore, PresentationFramework and System.Xaml.
using System;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;

internal static class WpfTouchScroll
{
    [STAThread]
    private static void Main()
    {
        AppContext.SetSwitch("Switch.System.Windows.Input.Stylus.EnablePointerSupport", true);
        RunWindow();
    }

    private static void RunWindow()
    {
        RenderOptions.ProcessRenderMode = System.Windows.Interop.RenderMode.SoftwareOnly;
        var rows = new StackPanel();
        for (int row = 1; row <= 100; ++row)
            rows.Children.Add(new TextBlock { Text = "Reading line " + row, FontSize = 22,
                Margin = new Thickness(16, 6, 16, 6) });

        var reader = new ScrollViewer { Content = rows, PanningMode = PanningMode.VerticalOnly,
            VerticalScrollBarVisibility = ScrollBarVisibility.Visible };
        var window = new Window { Title = "Oinos WPF touch scrolling", Width = 600, Height = 500,
            Left = 40, Top = 40, WindowStartupLocation = WindowStartupLocation.Manual, Content = reader };
        window.Loaded += delegate {
            Console.WriteLine("WPF {0}, tablet count={1}", typeof(Window).Assembly.FullName,
                Tablet.TabletDevices.Count);
            foreach (TabletDevice tablet in Tablet.TabletDevices)
                Console.WriteLine("tablet={0} type={1} cursors={2}", tablet.Name, tablet.Type, tablet.StylusDevices.Count);
        };
        window.PreviewTouchDown += delegate(object sender, TouchEventArgs e) {
            Console.WriteLine("touch down id={0} y={1:F1}", e.TouchDevice.Id, e.GetTouchPoint(reader).Position.Y);
        };
        window.PreviewTouchUp += delegate(object sender, TouchEventArgs e) {
            Console.WriteLine("touch up id={0} y={1:F1}", e.TouchDevice.Id, e.GetTouchPoint(reader).Position.Y);
        };
        reader.ScrollChanged += delegate(object sender, ScrollChangedEventArgs e) {
            if (e.VerticalChange != 0)
                Console.WriteLine("vertical offset={0:F1}", e.VerticalOffset);
        };
        new Application().Run(window);
    }
}
