using System;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Text.RegularExpressions;

namespace LdacPanel;

/// <summary>ldac-send.exe 的进程管理：拉起、读回显、停掉。</summary>
internal sealed class Sender
{
    private static readonly Regex PktLine =
        new(@"\[pkt (\d+)\] peak=(\d+) frames=(\d+) len=(\d+)", RegexOptions.Compiled);

    private Process? _process;
    private DateTime _startedAt;

    /// <summary>发送端自己写的日志路径（我们只读它，不打开）。</summary>
    public string LogPath { get; set; } = "";

    public long Packets { get; private set; }
    public int Peak { get; private set; }
    public int Frames { get; private set; }
    public int Length { get; private set; }
    public string LastLine { get; private set; } = "";
    public string LastError { get; private set; } = "";

    public bool Running => _process is { HasExited: false };

    public event Action? Updated;

    public bool Start(string exePath, string arguments)
    {
        if (Running) return true;
        try
        {
            var info = new ProcessStartInfo(exePath, arguments)
            {
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
            };
            var process = new Process { StartInfo = info, EnableRaisingEvents = true };
            process.OutputDataReceived += (_, e) => OnLine(e.Data);
            process.ErrorDataReceived += (_, e) => OnLine(e.Data);
            process.Exited += (_, _) =>
            {
                var code = SafeExitCode(process);
                var uptime = _startedAt == default ? 0 : (DateTime.Now - _startedAt).TotalSeconds;
                LastLine = $"发送端已退出 (code={code})";
                PanelLog.Write($"sender: exited code={code} uptime={uptime:F1}s");
            };
            if (!process.Start()) return false;
            _startedAt = DateTime.Now;
            PanelLog.Write($"sender: started pid={process.Id}");
            //  ⚠ 日志文件由 ldac-send.exe 自己创建并写入（--log）。
            //  面板绝不能先 fopen 它：StreamWriter 默认 FileShare.Read 会独占，
            //  子进程 fopen("w") 直接失败，结果就是"文件 0 字节"。
            process.BeginOutputReadLine();
            process.BeginErrorReadLine();
            _process = process;
            LastError = "";
            return true;
        }
        catch (Exception ex)
        {
            LastError = ex.Message;
            return false;
        }
    }

    public void Stop()
    {
        var process = _process;
        if (process is null) return;
        try
        {
            if (!process.HasExited)
            {
                process.Kill(entireProcessTree: true);
                process.WaitForExit(3000);
            }
        }
        catch (Exception ex)
        {
            LastError = ex.Message;
        }
        finally
        {
            process.Dispose();
            _process = null;
            Packets = Peak = Frames = Length = 0;
            LastLine = "已停止";
        }
    }

    private static int SafeExitCode(Process process)
    {
        try { return process.ExitCode; } catch { return -1; }
    }

    private void OnLine(string? line)
    {
        if (string.IsNullOrWhiteSpace(line)) return;
        LastLine = line.Trim();

        var match = PktLine.Match(LastLine);
        if (!match.Success) return;
        Packets = long.Parse(match.Groups[1].Value, CultureInfo.InvariantCulture);
        Peak = int.Parse(match.Groups[2].Value, CultureInfo.InvariantCulture);
        Frames = int.Parse(match.Groups[3].Value, CultureInfo.InvariantCulture);
        Length = int.Parse(match.Groups[4].Value, CultureInfo.InvariantCulture);
        Updated?.Invoke();
    }
}
