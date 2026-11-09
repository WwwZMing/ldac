using System;
using System.IO;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;

namespace LdacPanel;

/// <summary>GUI 侧客户端：一次请求一条连接，无长连接状态。</summary>
internal static class ApiClient
{
    public static Reply? Send(string command, int timeoutMs = 1500)
    {
        try
        {
            using var client = new TcpClient();
            if (!client.ConnectAsync("127.0.0.1", ApiServer.Port).Wait(timeoutMs))
            {
                PanelLog.Write($"api '{command}': connect timeout");
                return null;
            }

            client.ReceiveTimeout = timeoutMs;
            using var stream = client.GetStream();
            using var writer = new StreamWriter(stream, new UTF8Encoding(false)) { AutoFlush = true };
            using var reader = new StreamReader(stream, Encoding.UTF8);
            writer.WriteLine(command);
            var line = reader.ReadLine();
            if (line is null) return null;
            return JsonSerializer.Deserialize<Reply>(line, Json.Options);
        }
        catch (Exception ex)
        {
            PanelLog.Write($"api '{command}' failed: {ex.GetType().Name}: {ex.Message}");
            return null;
        }
    }
}
