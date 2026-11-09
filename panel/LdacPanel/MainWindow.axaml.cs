using System;
using Avalonia.Controls;
using Avalonia.Media;
using Avalonia.Threading;

namespace LdacPanel;

/// <summary>纯 UI 客户端：所有状态和动作都走服务管道，自己不碰进程和驱动。</summary>
public partial class MainWindow : Window
{
    private static readonly IBrush Grey = SolidColorBrush.Parse("#5B6472");
    private static readonly IBrush Green = SolidColorBrush.Parse("#37D67A");
    private static readonly IBrush Amber = SolidColorBrush.Parse("#FFB020");
    private static readonly IBrush Red = SolidColorBrush.Parse("#FF5C5C");

    private readonly DispatcherTimer _timer = new() { Interval = TimeSpan.FromMilliseconds(600) };
    private bool _ready;      // 构造期间控件会触发变更事件，别当成用户操作
    private bool _syncing;    // 正在按服务状态回填下拉框，别当成用户操作
    private bool _codecSynced; // 已经从服务拿到过一次真实 codec
    private LogWindow? _logWindow;

    public bool AllowClose { get; set; }

    public MainWindow()
    {
        InitializeComponent();

        CodecBox.SelectionChanged += (_, _) =>
        {
            UpdateCodecUi();
            PushCodec();
        };
        Rate990.IsCheckedChanged += (_, _) => PushEqmid();
        Rate660.IsCheckedChanged += (_, _) => PushEqmid();
        Rate330.IsCheckedChanged += (_, _) => PushEqmid();

        Closing += (_, e) =>
        {
            if (AllowClose) return;
            e.Cancel = true;
            Hide();
        };

        LogBtn.Click += (_, _) => ShowLog();

        _timer.Tick += (_, _) => Refresh();
        _timer.Start();
        _ready = true;
        Refresh();
    }

    /// <summary>LDAC 才有"码率"三档；AAC 的码率是协商死的，显示说明代替。</summary>
    private void UpdateCodecUi()
    {
        var isAac = CodecBox.SelectedIndex == 1;
        RateLabel.IsVisible = !isAac;
        RateRow.IsVisible = !isAac;
        AacInfo.IsVisible = isAac;
    }

    private void PushCodec()
    {
        if (!_ready || _syncing) return;
        var codec = CodecBox.SelectedIndex == 1 ? "aac" : "ldac";
        var reply = ApiClient.Send($"codec {codec}");
        if (reply?.Ok != true)
            Subtitle.Text = $"切换 {codec.ToUpperInvariant()} 失败：{reply?.Error}";
    }

    private void ShowLog()
    {
        if (_logWindow is null || !_logWindow.IsVisible)
        {
            _logWindow = new LogWindow();
            _logWindow.Closed += (_, _) => _logWindow = null;
            _logWindow.Show(this);
        }
        else
        {
            _logWindow.Activate();
        }
    }

    private void PushEqmid()
    {
        if (!_ready) return;
        var eqmid = Rate330.IsChecked == true ? "mq" : Rate660.IsChecked == true ? "sq" : "hq";
        ApiClient.Send($"eqmid {eqmid}");
    }

    private void Refresh()
    {
        var reply = ApiClient.Send("status");
        if (reply?.Status is null)
        {
            StateText.Text = "NO SERVICE";
            StateText.Foreground = Grey;
            Dot.Background = Grey;
            Subtitle.Text = "后台服务未运行 · sc start LdacA2dpSvc";
            PpsText.Text = "— 包/秒";
            SenderText.Text = "未知";
            SenderText.Foreground = Grey;
            return;
        }

        var s = reply.Status;

        //  第一次拿到真实 codec 时把下拉框对齐内核（0=LDAC 1=SBC 2=AAC）。
        //  不这么做的话，重启后面板显示 LDAC，实际内核在跑 AAC，一碰下拉框就"切"成错的。
        if (!_codecSynced && s.Ok)
        {
            _codecSynced = true;
            _syncing = true;
            CodecBox.SelectedIndex = s.Codec switch { 2 => 1, 1 => 1 /*SBC 暂无 UI*/, _ => 0 };
            _syncing = false;
            UpdateCodecUi();
        }

        if (!s.Ok)
        {
            StateText.Text = "NO DRIVER";
            StateText.Foreground = Grey;
            Dot.Background = Grey;
            Subtitle.Text = $"控制设备不可用 · 0x{s.DriverError:X8}";
            DeviceText.Text = @"\\.\LdacCtl 打不开";
            RateText.Text = ModeText.Text = SigMtuText.Text = MediaMtuText.Text = SeidText.Text = "—";
        }
        else
        {
            var state = (LdacState)s.State;
            StateText.Text = state switch
            {
                LdacState.Idle => "IDLE",
                LdacState.Starting => "STARTING",
                LdacState.Streaming => "STREAMING",
                _ => "ERROR",
            };
            StateText.Foreground = state switch
            {
                LdacState.Streaming => Green,
                LdacState.Starting => Amber,
                LdacState.Error => Red,
                _ => Grey,
            };
            Dot.Background = StateText.Foreground;
            var codecName = s.Codec switch { 2 => "AAC", 1 => "SBC", _ => "LDAC" };
            Subtitle.Text = $"{codecName} A2DP Audio" +
                (s.UserModeAvdtp ? " · 用户态 AVDTP" : " · 内核驱动");
            DeviceText.Text = state == LdacState.Streaming ? "UGREEN-BT505（已连接）" : "UGREEN-BT505";

            RateText.Text = s.RateBit switch
            {
                0x20 => "44.1 kHz",
                0x10 => "48 kHz",
                0x08 => "88.2 kHz",
                0x04 => "96 kHz",
                _ => $"0x{s.RateBit:X2}",
            };
            ModeText.Text = s.ModeBit switch
            {
                0x01 => "立体声",
                0x02 => "双声道",
                0x04 => "单声道",
                _ => $"0x{s.ModeBit:X2}",
            };
            SigMtuText.Text = s.SigMtu.ToString();
            MediaMtuText.Text = s.MediaMtu.ToString();
            SeidText.Text = $"0x{s.Seid:X2}";
            if (s.DriverError != 0) Subtitle.Text = $"最近错误 0x{s.DriverError:X8}";
        }

        if (!s.SenderRunning)
        {
            PpsText.Text = "— 包/秒";
            SenderText.Text = "未运行";
            SenderText.Foreground = Grey;
            return;
        }

        SenderText.Text = $"运行中 {s.UptimeSec / 60} 分";
        SenderText.Foreground = Green;
        PpsText.Text = $"{s.Pps:0} 包/秒 · {s.Kbps:0} kbps";
        PeakText.Text = $"peak {s.Peak} · {s.Frames} 帧/包";
        PeakChart.Push(s.Peak);

        var filledSeconds = s.Fill / 8.0 / 48000.0;
        HintText.Text = s.Fill > 0
            ? $"补静音 {filledSeconds:0.0}s（采集侧断过流，接收端没饿）"
            : "无补静音";
        LogText.Text = $"{s.SenderLog}   ·   服务托管，关闭窗口不影响声音";
    }
}
