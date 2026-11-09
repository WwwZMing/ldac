using System;
using System.Linq;
using System.Threading;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Markup.Xaml;
using Avalonia.Platform;
using Avalonia.Threading;

namespace LdacPanel;

public partial class App : Application
{
    private Mutex? _single;

    public override void Initialize()
    {
        AvaloniaXamlLoader.Load(this);
    }

    public override void OnFrameworkInitializationCompleted()
    {
        if (ApplicationLifetime is not IClassicDesktopStyleApplicationLifetime desktop)
        {
            base.OnFrameworkInitializationCompleted();
            return;
        }

        // 单实例：两个面板抢同一个端点只会互相打架
        _single = new Mutex(true, @"Global\LdacPanel.Single", out var created);
        if (!created)
        {
            desktop.Shutdown();
            return;
        }

        var minimized = Environment.GetCommandLineArgs()
            .Any(a => a.Equals("--minimized", StringComparison.OrdinalIgnoreCase));

        Dispatcher.UIThread.UnhandledException += (_, e) =>
        {
            PanelLog.Write($"UNHANDLED: {e.Exception}");
            e.Handled = true;
        };

        var window = new MainWindow();
        desktop.MainWindow = window;
        TryInstallTray(window, desktop);

        if (minimized) window.Hide();

        base.OnFrameworkInitializationCompleted();
    }

    private void TryInstallTray(MainWindow window, IClassicDesktopStyleApplicationLifetime desktop)
    {
        try
        {
            var icon = new WindowIcon(AssetLoader.Open(new Uri("avares://LdacPanel/Assets/icon.png")));
            var tray = new TrayIcon { Icon = icon, ToolTipText = "LDAC A2DP" };

            var show = new NativeMenuItem("显示面板");
            show.Click += (_, _) => Restore(window);

            var autoConnect = new NativeMenuItem("启动时自动连接")
            {
                ToggleType = MenuItemToggleType.CheckBox,
                IsChecked = Settings.AutoConnect,
            };
            autoConnect.Click += (_, _) =>
            {
                Settings.AutoConnect = !Settings.AutoConnect;
                autoConnect.IsChecked = Settings.AutoConnect;
            };

            var autoStart = new NativeMenuItem("开机自启")
            {
                ToggleType = MenuItemToggleType.CheckBox,
                IsChecked = Settings.AutoStart,
            };
            autoStart.Click += (_, _) =>
            {
                Settings.AutoStart = !Settings.AutoStart;
                autoStart.IsChecked = Settings.AutoStart;
            };

            var quit = new NativeMenuItem("退出");
            quit.Click += (_, _) =>
            {
                window.AllowClose = true;
                desktop.Shutdown();
            };

            var menu = new NativeMenu();
            menu.Items.Add(show);
            menu.Items.Add(new NativeMenuItemSeparator());
            menu.Items.Add(autoConnect);
            menu.Items.Add(autoStart);
            menu.Items.Add(new NativeMenuItemSeparator());
            menu.Items.Add(quit);
            tray.Menu = menu;
            tray.Clicked += (_, _) => Restore(window);

            TrayIcon.SetIcons(this, new TrayIcons { tray });
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"tray: {ex.Message}");
        }
    }

    private static void Restore(MainWindow window)
    {
        window.Show();
        window.WindowState = WindowState.Normal;
        window.Activate();
    }
}
