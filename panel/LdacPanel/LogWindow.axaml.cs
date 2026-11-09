using System;
using Avalonia.Controls;
using Avalonia.Threading;

namespace LdacPanel;

/// <summary>驱动日志窗口：直接增量读 C:\Users\Public\ldac-probe.log。</summary>
public partial class LogWindow : Window
{
    private const int MaxChars = 300 * 1000;   // UI 里只留最后约 300 KB

    private readonly LogTail _tail = new(LogTail.DefaultPath);
    private readonly DispatcherTimer _timer = new() { Interval = TimeSpan.FromMilliseconds(400) };
    private bool _stickToEnd = true;

    public LogWindow()
    {
        InitializeComponent();
        PathText.Text = LogTail.DefaultPath;

        Follow.IsCheckedChanged += (_, _) => _stickToEnd = Follow.IsChecked == true;
        ClearBtn.Click += (_, _) => { View.Text = ""; CountText.Text = ""; };
        _timer.Tick += (_, _) => Pump();
        Closed += (_, _) => { _timer.Stop(); _tail.Dispose(); };

        _timer.Start();
        Pump();
    }

    private void Pump()
    {
        var chunk = _tail.ReadNew();
        if (string.IsNullOrEmpty(chunk))
        {
            CountText.Text = $"{(View.Text?.Length ?? 0) / 1024} KB";
            return;
        }

        var text = (View.Text ?? "") + chunk;
        if (text.Length > MaxChars) text = text[^MaxChars..];
        View.Text = text;

        CountText.Text = $"{text.Length / 1024} KB";
        if (_stickToEnd)
        {
            View.CaretIndex = text.Length;
            View.SelectionStart = text.Length;
            View.SelectionEnd = text.Length;
        }
    }
}