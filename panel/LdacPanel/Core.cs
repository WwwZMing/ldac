using System;
using System.Text;
using System.Threading;
using System.Text.RegularExpressions;

namespace LdacPanel;

/// <summary>服务端独占：驱动 IOCTL + 发送端进程 + 实时指标。</summary>
internal sealed class Core
{
    private static readonly Regex PktRx =
        new(@"\[pkt (\d+)\] peak=(\d+) frames=(\d+) len=(\d+)(?: fill=(\d+))?", RegexOptions.Compiled);

    private readonly Sender _sender = new();
    private string _codec = "ldac";   // "ldac" | "aac" | "sbc"
    private readonly object _sync = new();
    private long _lastPackets;
    private long _lastTicks;
    private double _pps;
    private DateTime _started = DateTime.MinValue;

    //  LocalSystem 的 LOCALAPPDATA 在 systemprofile 下，GUI 和诊断脚本都够不着，
    //  统一放 ProgramData（服务有全权，管理员可读）。
    public string SenderLogPath { get; } = @"C:\ProgramData\LdacPanel\sender.log";

    /// <summary>优先用 exe 旁边的发送端（发布布局），否则退到开发目录。</summary>
    public string SenderExe { get; } =
        System.IO.File.Exists(System.IO.Path.Combine(AppContext.BaseDirectory, "ldac-send.exe"))
            ? System.IO.Path.Combine(AppContext.BaseDirectory, "ldac-send.exe")
            : @"C:\Developer\ldac\bin-host\Debug\ldac-send.exe";

    public string LastError { get; private set; } = "";

    public bool Start()
    {
        lock (_sync)
        {
            EnsureSwitchKey();
            if (_sender.Running) return true;

            //  服务起来前可能留着上一代的孤儿（服务被强杀时子进程不会跟着死）。
            //  发送端有全局单实例互斥，孤儿在 = 新进程秒退，所以必须先清干净。
            KillStraySenders();

            _sender.LogPath = SenderLogPath;
            var args = $"--loopback --match LDAC --codec {EffectiveCodec()} --log \"{SenderLogPath}\" --eqmid {Aeqmid()}";

            for (var attempt = 0; attempt < 2; attempt++)
            {
                if (_sender.Start(SenderExe, args))
                {
                    Thread.Sleep(250);
                    if (_sender.Running)
                    {
                        _started = DateTime.Now;
                        LastError = "";          // 之前那次失败的错误别留在界面上
                        PanelLog.Write($"core: sender started {args}");
                        return true;
                    }
                    LastError = $"sender exited immediately (attempt {attempt + 1})";
                    PanelLog.Write($"core: {LastError}");
                    KillStraySenders();
                }
                else
                {
                    LastError = _sender.LastError;
                    PanelLog.Write($"core: start failed: {LastError}");
                }
            }
            return false;
        }
    }

    /// <summary>
    /// 真正生效的编解码器以**内核**为准：注册表默认值和面板上次的选择都可能已经
    /// 不是这次会话协商的那个。发送端拿错编解码器就是"数据在流、只有噪音"。
    /// </summary>
    private string EffectiveCodec()
    {
        var st = Ctl.Status(out _);
        if (st is { } s)
        {
            var name = s.Codec switch { 1 => "sbc", 2 => "aac", _ => "ldac" };
            if (name != _codec) PanelLog.Write($"core: kernel codec={name} (panel had {_codec})");
            return name;
        }
        return _codec;
    }

    private static void KillStraySenders()
    {
        foreach (var p in System.Diagnostics.Process.GetProcessesByName("ldac-send"))
        {
            try
            {
                PanelLog.Write($"core: killing stray sender pid={p.Id}");
                p.Kill(entireProcessTree: true);
                p.WaitForExit(1500);
            }
            catch
            {
                // 已经退了
            }
        }
    }

    public void Stop()
    {
        lock (_sync)
        {
            _sender.Stop();
            KillStraySenders();
            _avdtp?.Dispose();
            _avdtp = null;
            Ctl.Stop(out _);
            PanelLog.Write("core: stopped");
        }
    }

    /// <summary>切换编解码器：更新内核偏好、换发送端参数、重建会话。</summary>
    public bool SetCodec(string codec)
    {
        codec = (codec ?? "").Trim().ToLowerInvariant();
        if (codec != "ldac" && codec != "aac" && codec != "sbc")
        {
            LastError = "codec must be ldac, aac or sbc";
            return false;
        }

        var wasRunning = _sender.Running;
        _codec = codec;

        var codecId = codec == "aac" ? 2 : codec == "sbc" ? 1 : 0;
        if (!Ctl.SetCodec(codecId, out var err))
        {
            LastError = $"ioctl set codec failed: {err}";
            return false;
        }

        //  写进驱动的开机默认值：IOCTL 只改运行时变量，重启后驱动会重新读
        //  Parameters\Codec，不落盘的话"切到 LDAC、重启又变回 AAC"。
        //  服务是 LocalSystem，写 HKLM 不需要额外权限。
        try
        {
            using var key = Microsoft.Win32.Registry.LocalMachine.OpenSubKey(
                @"SYSTEM\CurrentControlSet\Services\LdacA2dp\Parameters", writable: true);
            key?.SetValue("Codec", codecId, Microsoft.Win32.RegistryValueKind.DWord);
            PanelLog.Write($"core: codec {codec} -> registry Codec={codecId}");
        }
        catch (Exception ex)
        {
            PanelLog.Write($"core: codec {codec} not persisted: {ex.Message}");
        }

        //  会话配置在建立时决定，所以要重建一次：停发送端 -> STOP -> START。
        //  ⚠ STOP 之后必须留一下再 START：驱动要把两条 L2CAP 关掉、btport 要
        //  回收句柄，紧接着 START 会拿到 STATUS_INVALID_DEVICE_STATE
        //  （0xC0000184，实测：切 AAC 时发送端连续两次"立即退出"）。
        if (wasRunning)
        {
            Stop();
            //  ⚠ 这里**不要**同步 Start()。实测：STOP 之后立刻 START 会拿到
            //  ERROR_DEVICE_NOT_CONNECTED(1167) —— 拆掉 A2DP 会话时对端把 ACL
            //  也放了，得重新 page 回来（几秒到十几秒）。同步重试只会把
            //  一次"等它回来"的常态显示成红色错误，而且 GUI 线程会被堵住。
            //  交给 supervisor：它本来就有 1s..30s 的退避重试，成功后自己清错误。
            lock (_sync)
            {
                _backoffMs = BackoffMinMs;
                _nextLinkAttempt = DateTime.MinValue;
            }
        }
        return true;
    }

    public void SetEqmid(string eqmid)
    {
        lock (_sync) { _eqmid = eqmid; }
        if (_sender.Running) { Stop(); Start(); }
    }

    private System.Threading.Timer? _watchdog;
    private DateTime _nextLinkAttempt = DateTime.MinValue;
    private int _backoffMs = BackoffMinMs;
    private int _supervising;

    private const int BackoffMinMs = 1000;
    private const int BackoffMaxMs = 30000;

    /// <summary>
    /// 用户态 supervisor —— **整条重连策略都在这里**（内核不做任何重试）。
    ///
    /// 内核只提供 START/STOP/SEND/STATUS 四个原子动作；什么时候重试、退避多久、
    /// 发送端什么时候起，全由这里决定。这样重连逻辑崩了只是进程重启，
    /// 不会把整机蓝屏（2026-11-08 重构的初衷）。
    /// </summary>
    public void StartWatchdog()
    {
        _watchdog = new System.Threading.Timer(_ =>
        {
            //  START 在设备不在时可能阻塞几秒，别让定时器重入
            if (Interlocked.Exchange(ref _supervising, 1) != 0) return;
            try { Supervise(); }
            catch (Exception ex) { PanelLog.Write($"supervisor: {ex.Message}"); }
            finally { Interlocked.Exchange(ref _supervising, 0); }
        }, null, 2000, 2000);
    }

    private Avdtp? _avdtp;

    /// <summary>注册表开关：HKLM\SOFTWARE\LdacPanel\UserModeAvdtp（DWORD，默认 0）。</summary>
    private static bool UserModeAvdtpEnabled()
    {
        try
        {
            using var k = Microsoft.Win32.Registry.LocalMachine.OpenSubKey(@"SOFTWARE\LdacPanel");
            if (k == null)
            {
                PanelLog.Write("switch: HKLM\\SOFTWARE\\LdacPanel not visible to this process");
                return false;
            }
            var raw = k.GetValue("UserModeAvdtp");
            var on = raw is int v && v != 0;
            if (_switchLogged != on)
            {
                _switchLogged = on;
                PanelLog.Write($"switch: UserModeAvdtp={raw ?? "(null)"} -> userMode={on} " +
                               $"(proc bits={IntPtr.Size * 8})");
            }
            return on;
        }
        catch (Exception ex)
        {
            PanelLog.Write("switch: read failed: " + ex.Message);
            return false;
        }
    }

    private static bool? _switchLogged;

    private void Supervise()
    {
        if (UserModeAvdtpEnabled()) { SuperviseUserModeAvdtp(); return; }

        var st = Ctl.Status(out _);
        if (st is null)
        {
            //  驱动不在（未装/设备已移除）：什么都不做，等它回来。
            return;
        }

        if (st.Value.State == (int)LdacState.Streaming)
        {
            _backoffMs = BackoffMinMs;
            _nextLinkAttempt = DateTime.MinValue;
            if (!_sender.Running) Start();
            return;
        }

        if (DateTime.UtcNow < _nextLinkAttempt) return;

        //  只有在"曾经连上过"（STREAMING）时才需要 STOP 去清残留死通道。
        //  ⚠ 不能在 ERROR 时也 STOP：耳机不在时每个重试都会变成
        //  "开通道→关通道"的churn，而 0x139 那类内核链表损坏正是
        //  高频 open/close BRB 时最容易触发（2026-11-08 实测）。
        if (st.Value.State == (int)LdacState.Streaming) Ctl.Stop(out _);

        //  Addr 全零 = 用驱动的默认目标；RateBit 0 = 选对端支持的最高采样率。
        var req = new byte[7];
        var ok = Ctl.Start(req, out var err);
        var after = Ctl.Status(out _)?.State ?? -1;
        _backoffMs = (ok && after == (int)LdacState.Streaming)
            ? BackoffMinMs
            : Math.Min(_backoffMs * 2, BackoffMaxMs);
        _nextLinkAttempt = DateTime.UtcNow.AddMilliseconds(_backoffMs);
        PanelLog.Write($"supervisor: START ok={ok} err={err} state={after} next-attempt-in={_backoffMs}ms");
    }

    /// <summary>
    /// 用户态 AVDTP 路径：面板自己建链，然后把媒体槽位交给发送端。
    /// 内核完全不知道 AVDTP 的存在（只有 L2CAP 原语）。
    /// </summary>
    private uint _umLastPackets;
    private DateTime _umLastPacketsAt = DateTime.MinValue;

    private void SuperviseUserModeAvdtp()
    {
        //  链路存活检测：STREAMING 但内核计数不动 = 实际已经断了。
        //  没有这个检查，用户态 AVDTP 会在"通道还在、对端不回"时一直假装活着。
        if (_avdtp != null && _sender.Running)
        {
            var st0 = Ctl.Status(out _);
            var packets = st0?.Packets ?? 0;
            if (packets != _umLastPackets)
            {
                _umLastPackets = packets;
                _umLastPacketsAt = DateTime.UtcNow;
                return;
            }
            if (_umLastPacketsAt != DateTime.MinValue &&
                (DateTime.UtcNow - _umLastPacketsAt).TotalSeconds < 6)
                return;

            PanelLog.Write($"supervisor(um-avdtp): no packets for 6s (packets={packets}), rebuilding");
            _avdtp.Dispose();
            _avdtp = null;
            _sender.Stop();
            KillStraySenders();
            _umLastPacketsAt = DateTime.UtcNow;
            _nextLinkAttempt = DateTime.MinValue;
        }

        if (_avdtp != null) { _avdtp.Dispose(); _avdtp = null; _sender.Stop(); }
        if (DateTime.UtcNow < _nextLinkAttempt) return;

        var a = new Avdtp(new CtlL2cap());
        if (a.Connect(null, 1, ProbeMixRateBit, out var err))
        {
            _avdtp = a;
            _backoffMs = BackoffMinMs;
            _umLastPackets = Ctl.Status(out _)?.Packets ?? 0;
            _umLastPacketsAt = DateTime.UtcNow;
            StartSenderThroughSlot(a);
        }
        else
        {
            PanelLog.Write($"supervisor(um-avdtp): failed: {err}");
            a.Dispose();
            _backoffMs = Math.Min(_backoffMs * 2, BackoffMaxMs);
        }
        _nextLinkAttempt = DateTime.UtcNow.AddMilliseconds(_backoffMs);
    }

    /// <summary>
    /// 确保 HKLM\SOFTWARE\LdacPanel 存在，并写入 UserModeAvdtp 的默认值 0。
    /// 驱动也会读这个键（用户态接管时拒绝内核 START），所以它必须可预测地存在。
    /// 已存在的值绝不覆盖。
    /// </summary>
    private static void EnsureSwitchKey()
    {
        try
        {
            using var k = Microsoft.Win32.Registry.LocalMachine.CreateSubKey(@"SOFTWARE\LdacPanel");
            if (k == null) return;
            if (k.GetValue("UserModeAvdtp") == null)
                k.SetValue("UserModeAvdtp", 0, Microsoft.Win32.RegistryValueKind.DWord);
        }
        catch (Exception ex) { PanelLog.Write("core: switch key: " + ex.Message); }
    }

    /// <summary>
    /// 问发送端"系统混音率是多少"（--probe-rate 只开 WASAPI、不碰驱动），
    /// 换成 LDAC 的采样率位。协商率必须和混音率一致，否则音调是错的。
    /// </summary>
    private byte ProbeMixRateBit()
    {
        //  端点刚注册时音频栈要几百毫秒才建成，前几次探测可能失败 —— 重试几次。
        for (var attempt = 0; attempt < 4; attempt++)
        {
            var bit = ProbeMixRateBitOnce();
            if (bit != 0) return bit;
            System.Threading.Thread.Sleep(400);
        }
        PanelLog.Write("supervisor(um-avdtp): mix-rate probe failed (endpoint not ready?)");
        return 0;
    }

    private byte ProbeMixRateBitOnce()
    {
        try
        {
            var psi = new System.Diagnostics.ProcessStartInfo(SenderExe, "--probe-rate --match LDAC")
            {
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                UseShellExecute = false,
                CreateNoWindow = true,
            };
            using var p = System.Diagnostics.Process.Start(psi);
            if (p == null) return 0;
            var outp = p.StandardOutput.ReadToEnd();
            p.WaitForExit(8000);
            foreach (var line in outp.Split((char)10))
            {
                var t = line.Trim();
                if (!t.StartsWith("RATE ")) continue;
                if (!int.TryParse(t.Substring(5).Trim(), out var hz)) continue;
                var bit = hz switch { 96000 => (byte)0x04, 88200 => (byte)0x08, 48000 => (byte)0x10, 44100 => (byte)0x20, _ => (byte)0 };
                PanelLog.Write($"supervisor(um-avdtp): mix rate {hz} Hz -> rate bit 0x{bit:X2}");
                return bit;
            }
        }
        catch (Exception ex) { PanelLog.Write($"supervisor(um-avdtp): probe failed: {ex.Message}"); }
        return 0;
    }

    private void StartSenderThroughSlot(Avdtp a)
    {
        lock (_sync)
        {
            if (_sender.Running) return;
            KillStraySenders();
            _sender.LogPath = SenderLogPath;
            var args = $"--loopback --match LDAC --codec {EffectiveCodec()} --media-slot {a.MediaSlot} " +
                       $"--media-mtu {a.MediaMtu} --log \"{SenderLogPath}\" --eqmid {Aeqmid()}";
            for (var attempt = 0; attempt < 2; attempt++)
            {
                if (_sender.Start(SenderExe, args))
                {
                    Thread.Sleep(250);
                    if (_sender.Running)
                    {
                        _started = DateTime.Now;
                        PanelLog.Write($"core: sender started (um-avdtp slot={a.MediaSlot} mtu={a.MediaMtu})");
                        return;
                    }
                    LastError = $"sender exited immediately (attempt {attempt + 1})";
                    PanelLog.Write($"core: {LastError}");
                    KillStraySenders();
                }
                else
                {
                    LastError = _sender.LastError;
                    PanelLog.Write($"core: start failed: {LastError}");
                }
            }
        }
    }

    public void StopWatchdog() => _watchdog?.Dispose();

    private string _eqmid = "hq";
    private string Aeqmid() => _eqmid is "hq" or "sq" or "mq" ? _eqmid : "hq";

    public Snapshot Status()
    {
        var snap = new Snapshot { SenderLog = SenderLogPath, Error = LastError };

        var status = Ctl.Status(out var error);
        if (status is null)
        {
            snap.Ok = false;
            snap.DriverError = error;
        }
        else
        {
            snap.Ok = true;
            snap.State = status.Value.State;
            snap.DriverError = status.Value.LastError;
            snap.SigMtu = status.Value.SigMtu;
            snap.MediaMtu = status.Value.MediaMtu;
            snap.RateBit = status.Value.RateBit;
            snap.ModeBit = status.Value.ModeBit;
            snap.Seid = status.Value.SinkSeid;
            snap.Codec = status.Value.Codec;
        }

        //  用户态 AVDTP 模式下内核状态永远是 IDLE（内核没参与协商），
        //  必须用会话自己的状态覆盖，否则面板会显示"未连接"但实际在出声。
        snap.UserModeAvdtp = UserModeAvdtpEnabled();
        if (snap.UserModeAvdtp)
        {
            snap.State = (_avdtp != null && _sender.Running)
                ? (int)LdacState.Streaming
                : (int)LdacState.Idle;
            snap.SigMtu = _avdtp?.SigMtu ?? 0;
            snap.MediaMtu = _avdtp?.MediaMtu ?? 0;
            snap.RateBit = _avdtp?.ChosenRateBit ?? 0;
            snap.ModeBit = _avdtp?.ChosenModeBit ?? 0;
            snap.Seid = _avdtp?.LdacSep?.Id ?? 0;
            snap.UmMediaSlot = _avdtp?.MediaSlot ?? 0;
        }

        snap.SenderRunning = _sender.Running;
        snap.UptimeSec = _started == DateTime.MinValue ? 0 : (int)(DateTime.Now - _started).TotalSeconds;

        if (_sender.Running)
        {
            var line = ReadLastPacketLine(SenderLogPath);
            if (line is not null)
            {
                var m = PktRx.Match(line);
                if (m.Success)
                {
                    var packets = long.Parse(m.Groups[1].Value);
                    snap.Peak = int.Parse(m.Groups[2].Value);
                    snap.Frames = int.Parse(m.Groups[3].Value);
                    snap.Length = int.Parse(m.Groups[4].Value);
                    snap.Fill = m.Groups[5].Success ? long.Parse(m.Groups[5].Value) : 0;
                    snap.Packets = packets;

                    var now = System.Diagnostics.Stopwatch.GetTimestamp();
                    var seconds = (now - _lastTicks) / (double)System.Diagnostics.Stopwatch.Frequency;
                    if (seconds > 0.2 && packets >= _lastPackets)
                    {
                        _pps = _pps * 0.6 + (packets - _lastPackets) / seconds * 0.4;
                        _lastPackets = packets;
                        _lastTicks = now;
                    }
                    snap.Pps = _pps;
                    snap.Kbps = _pps * snap.Length * 8 / 1000.0;
                }
            }
        }

        return snap;
    }

    private static string? ReadLastPacketLine(string path)
    {
        try
        {
            using var fs = new System.IO.FileStream(path, System.IO.FileMode.Open, System.IO.FileAccess.Read,
                System.IO.FileShare.ReadWrite | System.IO.FileShare.Delete);
            var take = (int)Math.Min(fs.Length, 16 * 1024);
            if (take <= 0) return null;
            fs.Seek(-take, System.IO.SeekOrigin.End);
            var buffer = new byte[take];
            var read = fs.Read(buffer, 0, take);
            var text = Encoding.UTF8.GetString(buffer, 0, read);
            for (var i = text.Length - 1; i >= 0; i--)
            {
                var end = i;
                while (i >= 0 && text[i] != '\n') i--;
                var line = text.Substring(i + 1, end - i).Trim();
                if (line.StartsWith("[pkt ", StringComparison.Ordinal)) return line;
            }
        }
        catch
        {
            // 还没写出来
        }
        return null;
    }
}
