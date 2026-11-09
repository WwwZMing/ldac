using System;
using System.IO;

namespace LdacPanel;

/// <summary>面板/服务的诊断日志。GUI 没有控制台，出问题只能靠它。</summary>
internal static class PanelLog
{
    private static readonly object Sync = new();
    private static string? _path;

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
                File.AppendAllText(Path, $"{DateTime.Now:HH:mm:ss.fff} {message}{Environment.NewLine}");
            }
        }
        catch
        {
            // 日志失败不能影响主流程
        }
    }
}
