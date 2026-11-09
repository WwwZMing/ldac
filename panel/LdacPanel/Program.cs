using System;
using System.Linq;
using System.ServiceProcess;
using Avalonia;

namespace LdacPanel;

internal static class Program
{
    [STAThread]
    public static void Main(string[] args)
    {
        //  服务模式：不初始化 Avalonia，纯后台。
        if (args.Any(a => a.Equals("--service", StringComparison.OrdinalIgnoreCase)))
        {
            ServiceBase.Run(new LdacService());
            return;
        }

        BuildAvaloniaApp().StartWithClassicDesktopLifetime(args);
    }

    public static AppBuilder BuildAvaloniaApp()
        => AppBuilder.Configure<App>()
            .UsePlatformDetect()
            .WithInterFont()
            .LogToTrace();
}
