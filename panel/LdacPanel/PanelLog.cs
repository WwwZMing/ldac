using System;
using System.IO;

namespace LdacPanel;

/// <summary>面板/服务的诊断日志。GUI 没有控制台，出问题只能靠它。</summary>
internal static class PanelLog
{
    private static readonly object Sync = new();
    private static string? _path;

    //  单文件超过 4 MiB 就轮转：panel.log -> panel.1.log -> panel.2.log，最多三份。
    //  服务常驻，没有上限的日志迟早把 ProgramData 塞满。
    private const long MaxBytes = 4L * 1024 * 1024;
    private const int Keep = 2;

    public static string Path
    {
        get
        {
            if (_path is not null) return _path;

            //  先试机器级的（服务和 GUI 都能读），写不了再退到用户目录
            foreach (var candidate in new[]
                     {
                         @"C:\ProgramData\LdacPanel\panel.log",
                         System.IO.Path.Combine(
                             Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                             "LdacPanel", "panel.log"),
                     })
            {
                try
                {
                    var dir = System.IO.Path.GetDirectoryName(candidate);
                    if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
                    using (var probe = new FileStream(candidate, FileMode.Append, FileAccess.Write, FileShare.ReadWrite))
                    {
                    }
                    return _path = candidate;
                }
                catch
                {
                    // 下一个候选
                }
            }

            return _path = System.IO.Path.GetTempPath() + "ldac-panel.log";
        }
    }

    public static void Write(string message)
    {
        try
        {
            lock (Sync)
            {
                var path = Path;
                RotateIfNeeded(path);
                File.AppendAllText(path, $"{DateTime.Now:HH:mm:ss.fff} {message}{Environment.NewLine}");
            }
        }
        catch
        {
            // 日志失败不能影响主流程
        }
    }

    /// <summary>进程启动横幅：把版本、启动模式、开机时间打一行，便于对照时间线。</summary>
    public static void Banner(string role)
    {
        string boot;
        try { boot = DateTime.Now.AddMilliseconds(-Environment.TickCount64).ToString("MM-dd HH:mm:ss"); }
        catch { boot = "?"; }
        Write($"==== {role} start pid={Environment.ProcessId} os={Environment.OSVersion.VersionString} boot~{boot} ====");
    }

    private static void RotateIfNeeded(string path)
    {
        try
        {
            var fi = new FileInfo(path);
            if (!fi.Exists || fi.Length < MaxBytes) return;
            var dir = fi.DirectoryName ?? ".";
            var stem = System.IO.Path.GetFileNameWithoutExtension(path);
            var ext = fi.Extension;
            var oldest = System.IO.Path.Combine(dir, $"{stem}.{Keep}{ext}");
            if (File.Exists(oldest)) File.Delete(oldest);
            for (var i = Keep - 1; i >= 1; i--)
            {
                var src = System.IO.Path.Combine(dir, $"{stem}.{i}{ext}");
                if (File.Exists(src)) File.Move(src, System.IO.Path.Combine(dir, $"{stem}.{i + 1}{ext}"), true);
            }
            File.Move(path, System.IO.Path.Combine(dir, $"{stem}.1{ext}"), true);
        }
        catch
        {
            //  轮转失败就继续写原文件
        }
    }
}
