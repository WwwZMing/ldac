using System;
using System.IO;
using System.Text;

namespace LdacPanel;

/// <summary>
/// 增量读取驱动日志（内核写 C:\Users\Public\ldac-probe.log）。
/// 只读、共享打开：驱动正在写也不要紧；文件被截断（重启驱动）时自动从头读。
/// </summary>
public sealed class LogTail : IDisposable
{
    public const string DefaultPath = @"C:\Users\Public\ldac-probe.log";

    private readonly string _path;
    private FileStream? _fs;
    private long _pos;

    public LogTail(string path) => _path = path;

    public string Path => _path;

    /// <summary>返回自上次调用以来的新文本；没有新内容返回 null。</summary>
    public string? ReadNew(int maxBytes = 64 * 1024)
    {
        try
        {
            if (_fs is null)
            {
                if (!File.Exists(_path)) return null;
                _fs = new FileStream(_path, FileMode.Open, FileAccess.Read,
                                     FileShare.ReadWrite | FileShare.Delete,
                                     4096, FileOptions.SequentialScan);
                _pos = Math.Max(0, _fs.Length - 48 * 1024);   // 先给一屏历史，不刷整个文件
            }

            if (_fs.Length < _pos) _pos = 0;                   // 文件被截断
            if (_fs.Length == _pos) return null;

            _fs.Seek(_pos, SeekOrigin.Begin);
            int want = (int)Math.Min(_fs.Length - _pos, maxBytes);
            var buf = new byte[want];
            int n = _fs.Read(buf, 0, want);
            if (n <= 0) return null;
            _pos += n;
            return Encoding.UTF8.GetString(buf, 0, n);
        }
        catch (IOException) { return null; }
        catch (UnauthorizedAccessException) { return null; }
    }

    public void Dispose()
    {
        _fs?.Dispose();
        _fs = null;
    }
}