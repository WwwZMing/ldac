using System;
using Avalonia;
using Avalonia.Headless;
using Avalonia.Threading;
using LdacPanel;

internal static class Program
{
    [STAThread]
    private static void Main(string[] args)
    {
        AppBuilder.Configure<App>()
            .UseHeadless(new AvaloniaHeadlessPlatformOptions { UseHeadlessDrawing = false })
            .UseSkia()
            .SetupWithoutStarting();

        var window = new MainWindow { Width = 940, Height = 620 };
        window.Show();
        Dispatcher.UIThread.RunJobs();

        var frame = window.CaptureRenderedFrame();
        if (frame is null)
        {
            Console.WriteLine("capture returned null");
            return;
        }

        var path = args.Length > 0 ? args[0] : "shot.png";
        frame.Save(path);
        Console.WriteLine($"saved {path}  {frame.PixelSize.Width}x{frame.PixelSize.Height}");
    }
}
