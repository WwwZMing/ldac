using System.Text.Json;
using System.Text.Json.Serialization;

namespace LdacPanel;

/// <summary>服务 → GUI 的状态快照。</summary>
internal sealed class Snapshot
{
    public bool Ok { get; set; }
    public int State { get; set; }
    public int DriverError { get; set; }
    public uint SigMtu { get; set; }
    public uint MediaMtu { get; set; }
    public uint RateBit { get; set; }
    public uint ModeBit { get; set; }
    public uint Seid { get; set; }
    //  0 = LDAC / 1 = SBC / 2 = AAC（内核当前选的那个，GUI 用它初始化下拉框）
    public uint Codec { get; set; }
    public bool SenderRunning { get; set; }
    //  true = 建链走用户态 AVDTP（Avdtp.cs），此时内核状态恒为 IDLE，
    //  真实状态由这里的 State 覆盖给出。
    public bool UserModeAvdtp { get; set; }
    public uint UmMediaSlot { get; set; }
    public int Peak { get; set; }
    public int Frames { get; set; }
    public int Length { get; set; }
    public long Packets { get; set; }
    public long Fill { get; set; }
    public double Pps { get; set; }
    public double Kbps { get; set; }
    public int UptimeSec { get; set; }
    public string Error { get; set; } = "";
    public string SenderLog { get; set; } = "";
}

internal sealed class Reply
{
    public bool Ok { get; set; }
    public string Error { get; set; } = "";
    public Snapshot? Status { get; set; }
}

internal static class Json
{
    public static readonly JsonSerializerOptions Options = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
    };

    public static string One(object value) => JsonSerializer.Serialize(value, Options);
}
