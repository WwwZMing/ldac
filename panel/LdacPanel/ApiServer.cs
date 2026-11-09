using System;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading;

namespace LdacPanel;

/// <summary>服务侧 API：127.0.0.1:48620，一行命令一行 JSON。
/// 比命名管道少一堆实例/超时的坑，脚本也能直接测。</summary>
internal sealed class ApiServer
{
    public const int Port = 48620;

    private readonly Core _core;
    private volatile bool _stop;
    private TcpListener? _listener;

    public ApiServer(Core core) => _core = core;

    public void Start()
    {
        try
        {
            _listener = new TcpListener(IPAddress.Loopback, Port);
            _listener.Start();
            new Thread(Loop) { IsBackground = true, Name = "ldac-api" }.Start();
            PanelLog.Write($"api: listening on 127.0.0.1:{Port}");
        }
        catch (Exception ex)
        {
            PanelLog.Write($"api: listen failed: {ex.Message}");
        }
    }

    public void Stop()
    {
        _stop = true;
        try { _listener?.Stop(); } catch { }
    }

    private void Loop()
    {
        while (!_stop && _listener is not null)
        {
            try
            {
                using var client = _listener.AcceptTcpClient();
                client.ReceiveTimeout = 2000;
                client.SendTimeout = 2000;
                using var stream = client.GetStream();
                using var reader = new StreamReader(stream, Encoding.UTF8);
                using var writer = new StreamWriter(stream, new UTF8Encoding(false)) { AutoFlush = true };

                var line = reader.ReadLine();
                if (line is null) continue;
                writer.WriteLine(Handle(line.Trim()));
            }
            catch (Exception ex)
            {
                if (!_stop) PanelLog.Write($"api: {ex.GetType().Name}: {ex.Message}");
                Thread.Sleep(50);
            }
        }
    }

    private string Handle(string command)
    {
        try
        {
            var parts = command.Split(' ', StringSplitOptions.RemoveEmptyEntries);
            if (parts.Length == 0) return Json.One(new Reply { Ok = false, Error = "empty" });

            switch (parts[0].ToLowerInvariant())
            {
                case "status":
                    return Json.One(new Reply { Ok = true, Status = _core.Status() });
                case "start":
                    return Json.One(new Reply { Ok = _core.Start(), Error = _core.LastError });
                case "stop":
                    _core.Stop();
                    return Json.One(new Reply { Ok = true });
                case "codec" when parts.Length > 1:
                    return Json.One(new Reply { Ok = _core.SetCodec(parts[1]), Error = _core.LastError });
                case "eqmid" when parts.Length > 1:
                    _core.SetEqmid(parts[1]);
                    return Json.One(new Reply { Ok = true });
                default:
                    return Json.One(new Reply { Ok = false, Error = "unknown command" });
            }
        }
        catch (Exception ex)
        {
            PanelLog.Write($"api: handle '{command}' failed: {ex.Message}");
            return Json.One(new Reply { Ok = false, Error = ex.Message });
        }
    }
}
