using System;
using System.Collections.Generic;

namespace LdacPanel;

/// <summary>L2CAP 原语的抽象：生产环境走 Ctl（内核 IOCTL），测试环境走假实现。</summary>
internal interface IL2cap
{
    uint Open(ushort psm, byte[]? addr6, out uint mtu, out int status);
    bool Send(uint slot, byte[] payload, int offset, int length);
    byte[]? Recv(uint slot, uint timeoutMs, out int status);
    bool Close(uint slot);
}

/// <summary>
/// 用户态 AVDTP 客户端（INT 角色）。
///
/// 内核只提供 L2CAP 的 open/send/recv/close 四个原语；Discover / GetCapabilities /
/// SetConfiguration / Open / Start 这些协议动作全部在这里完成。
/// 这样协议逻辑崩了只是进程重启，不会蓝屏（见 docs/refactor-plan.md）。
/// </summary>
internal sealed class Avdtp : IDisposable
{
    public const byte Psm = 25;

    private const byte SigDiscover   = 0x01;
    private const byte SigGetCaps    = 0x02;
    private const byte SigSetConfig  = 0x03;
    private const byte SigOpen       = 0x06;
    private const byte SigStart      = 0x07;

    private const byte MtCmd    = 0x00;
    private const byte MtGeneral= 0x01;
    private const byte MtAccept = 0x02;
    private const byte MtReject = 0x03;

    private const ushort SonyVendor = 0x012D;
    private const ushort LdacCodec  = 0x00AA;

    private const byte CatMediaTransport = 0x01;
    private const byte CatMediaCodec     = 0x07;

    internal sealed class Sep
    {
        public byte Id;
        public bool IsSink;
        public bool HasLdac;
        public byte[] Caps = Array.Empty<byte>();
        public int CodecDataOffset = -1;   // Media Codec 元素里 codec 数据的起点（相对 Caps）
        public int RateOffset = -1;        // LDAC codec specific 的采样率字节（相对 Caps）
        public int ModeOffset = -1;
        public byte RateBits;
        public byte ModeBits;
    }

    private readonly IL2cap _l2;
    private uint _sigSlot;
    private uint _mediaSlot;
    private byte _tl;

    public Avdtp(IL2cap l2) => _l2 = l2;

    public Sep? LdacSep { get; private set; }
    public byte ChosenRateBit { get; private set; }
    public byte ChosenModeBit { get; private set; }
    public uint MediaSlot => _mediaSlot;
    public uint MediaMtu { get; private set; }
    public uint SigMtu { get; private set; }

    public bool SignallingOpen => _sigSlot != 0;

    public bool OpenSignalling(byte[]? addr6, out string error)
    {
        _sigSlot = _l2.Open(Psm, addr6, out var mtu, out var status);
        if (_sigSlot == 0)
        {
            error = $"L2CAP open(PSM 25) failed status=0x{status:X8}";
            return false;
        }
        SigMtu = mtu;
        if (mtu > 0)
            PanelLog.Write($"avdtp: signalling up slot={_sigSlot} mtu={mtu}");
        error = "";
        return true;
    }

    /// <summary>
    /// 完整建链：Discover → GetCaps → SetConfig → (开媒体通道) → Start。
    /// prefRate 是系统混音率对应的 LDAC 位（0 = 不指定，选对端最高）：
    /// 协商率必须和混音率一致，否则不重采样的情况下音调会错。
    /// </summary>
    /// <param name="probeRate">
    /// 在建链过程中（Discover 之后、SetConfiguration 之前）回调，用来取系统混音率。
    /// 必须在这里取：音频端点是跟着 AVDTP 通道出现的，开跑之前它还不存在。
    /// </param>
    public bool Connect(byte[]? addr6, byte intSeid, Func<byte>? probeRate, out string error)
    {
        if (!OpenSignalling(addr6, out error)) return false;

        var seps = Discover(out error);
        if (seps == null) return false;
        PanelLog.Write($"avdtp: discover -> {seps.Count} SEP(s)");

        foreach (var sep in seps)
        {
            if (!GetCapabilities(sep, out var capErr))
            {
                PanelLog.Write($"avdtp: getcaps seid={sep.Id} failed: {capErr}");
                continue;
            }
            if (sep.HasLdac) { LdacSep = sep; break; }
        }
        if (LdacSep == null)
        {
            error = "no LDAC SEP found";
            return false;
        }
        //  这时候 AVDTP 通道已经起来 → 驱动注册了音频端点 → 才能探到混音率。
        var prefRate = probeRate?.Invoke() ?? 0;
        PickRateMode(LdacSep, prefRate);

        if (!SetConfiguration(LdacSep, intSeid, out error)) return false;
        PanelLog.Write($"avdtp: configured acp={LdacSep.Id} int={intSeid} rate=0x{ChosenRateBit:X2} mode=0x{ChosenModeBit:X2}");

        //  媒体通道：同一 PSM 25 的第二条 L2CAP。
        _mediaSlot = _l2.Open(Psm, addr6, out var mediaMtu, out var mst);
        if (_mediaSlot == 0)
        {
            error = $"L2CAP open(media) failed status=0x{mst:X8}";
            return false;
        }
        MediaMtu = mediaMtu;
        PanelLog.Write($"avdtp: media slot={_mediaSlot} mtu={mediaMtu}");

        //  ⚠ 这两个信号是**容忍式**的，和内核原来的行为一致：
        //  实测 BT505 的 TG 对 AVDTP Open / Start 经常不应答，但照样出声。
        //  内核当初只发 Start（没发 Open），超时也直接判 STREAMING；
        //  这里保留同样的容忍度，否则"规范上更对"的写法反而会让它不出声。
        if (!Simple(SigOpen, LdacSep.Id, out var openErr))
        {
            PanelLog.Write($"avdtp: open not acked ({openErr}) -- continuing");
        }
        if (!Simple(SigStart, LdacSep.Id, out var startErr))
        {
            PanelLog.Write($"avdtp: start not acked ({startErr}) -- proceeding anyway");
        }

        PanelLog.Write("avdtp: STREAMING");
        error = "";
        return true;
    }

    private List<Sep>? Discover(out string error)
    {
        //  ⚠ 必须重试：实测这个 sink 对"刚开通道后的第一条 Discover"经常不应答，
        //  换一条通道/过一秒再发就正常（内核 AvdtpDiscover 也是 3 次 × 5s + 1.5s 间隔）。
        //  只发一次会让整个会话在第一步就失败。
        byte[]? rsp = null;
        error = "discover not attempted";
        for (var attempt = 0; attempt < 3; attempt++)
        {
            rsp = Xact(SigDiscover, Array.Empty<byte>(), 5000, out var e);
            if (rsp != null) { error = ""; break; }
            error = e;
            PanelLog.Write($"avdtp: discover attempt {attempt + 1}/3 failed: {e}");
            if (attempt < 2) System.Threading.Thread.Sleep(1500);
        }
        if (rsp == null) return null;

        var list = new List<Sep>();
        for (var i = 2; i + 1 < rsp.Length; i += 2)
        {
            var b0 = rsp[i];
            var b1 = rsp[i + 1];
            list.Add(new Sep
            {
                Id = (byte)((b0 >> 2) & 0x3F),
                IsSink = ((b1 >> 3) & 0x01) == 1,
            });
        }
        return list;
    }

    private bool GetCapabilities(Sep sep, out string error)
    {
        var rsp = Xact(SigGetCaps, new[] { (byte)(sep.Id << 2) }, 4000, out error);
        if (rsp == null) return false;
        if (rsp.Length < 4) { error = "short capabilities"; return false; }

        sep.Caps = new byte[rsp.Length - 4];
        Array.Copy(rsp, 4, sep.Caps, 0, sep.Caps.Length);

        var caps = sep.Caps;
        var i = 0;
        while (i + 1 < caps.Length)
        {
            var cat = (byte)(caps[i] & 0x3F);
            var losc = (byte)((caps[i] >> 6) & 0x03);
            var hdr = losc != 0 ? 1 : 2;
            var len = losc != 0 ? losc : caps[i + 1];
            if (i + hdr + len > caps.Length) break;

            if (cat == CatMediaCodec && len >= 6)
            {
                var d = i + hdr;
                if (caps[d] == 0x00 && caps[d + 1] == 0xFF && len >= 10)
                {
                    var vendor = (ushort)(caps[d + 2] | (caps[d + 3] << 8));
                    var codec  = (ushort)(caps[d + 6] | (caps[d + 7] << 8));
                    if (vendor == SonyVendor && codec == LdacCodec)
                    {
                        sep.HasLdac = true;
                        sep.CodecDataOffset = d;
                        sep.RateBits = caps[d + 8];
                        sep.ModeBits = caps[d + 9];
                        //  codec specific 格式：00(media) FF(codec) vendor(4) codec(2) rate mode
                        sep.RateOffset = d + 8;
                        sep.ModeOffset = d + 9;
                    }
                }
            }
            i += hdr + len;
        }
        return true;
    }

    private void PickRateMode(Sep sep, byte prefRate)
    {
        //  有偏好且对端支持就用偏好（混音率匹配，避免音调错）；
        //  否则从高到低：96k/48k/44.1k。模式优先立体声。
        var rate = (byte)0;
        if (prefRate != 0 && (sep.RateBits & prefRate) != 0) rate = prefRate;
        else if ((sep.RateBits & 0x04) != 0) rate = 0x04;        // 96k
        else if ((sep.RateBits & 0x10) != 0) rate = 0x10;        // 48k
        else if ((sep.RateBits & 0x20) != 0) rate = 0x20;        // 44.1k
        else rate = sep.RateBits;

        var mode = (byte)((sep.ModeBits & 0x01) != 0 ? 0x01 : sep.ModeBits);
        ChosenRateBit = rate;
        ChosenModeBit = mode;
    }

    private bool SetConfiguration(Sep sep, byte intSeid, out string error)
    {
        var caps = new List<byte> { (byte)(sep.Id << 2), (byte)(intSeid << 2) };

        //  回放对端的能力元素，只留 Media Transport + Media Codec，并把
        //  codec specific 的 rate/mode 换成"单比特实际配置"（内核 Avdtp.c 同款）。
        var i = 0;
        while (i + 1 < sep.Caps.Length)
        {
            var cat = (byte)(sep.Caps[i] & 0x3F);
            var losc = (byte)((sep.Caps[i] >> 6) & 0x03);
            var hdr = losc != 0 ? 1 : 2;
            var len = losc != 0 ? losc : sep.Caps[i + 1];
            if (i + hdr + len > sep.Caps.Length) break;

            if (cat == CatMediaTransport || cat == CatMediaCodec)
            {
                var start = caps.Count;
                for (var k = 0; k < hdr + len; k++) caps.Add(sep.Caps[i + k]);
                if (cat == CatMediaCodec && sep.HasLdac && sep.RateOffset >= 0)
                {
                    //  注意：写的是 codec specific 里的 rate/mode 两字节，
                    //  不是元素开头（那里是 media type / codec type）。
                    var rel = sep.RateOffset - (i + hdr);
                    if (rel >= 0 && rel + 2 <= len)
                    {
                        caps[start + hdr + rel]     = ChosenRateBit;
                        caps[start + hdr + rel + 1] = ChosenModeBit;
                    }
                }
            }
            i += hdr + len;
        }

        var rsp = Xact(SigSetConfig, caps.ToArray(), 6000, out error);
        return rsp != null;
    }

    private bool Simple(byte sig, byte seid, out string error)
    {
        var rsp = Xact(sig, new[] { (byte)(seid << 2) }, 4000, out error);
        return rsp != null;
    }

    /// <summary>一个 AVDTP 事务：发命令、等匹配 TL 的应答。</summary>
    private byte[]? Xact(byte sig, byte[] param, int timeoutMs, out string error)
    {
        error = "";
        var pkt = new byte[2 + param.Length];
        pkt[0] = (byte)((_tl << 4) | MtCmd);
        pkt[1] = sig;
        Array.Copy(param, 0, pkt, 2, param.Length);
        var sentTl = _tl;
        _tl = (byte)((_tl + 1) & 0x0F);

        if (!_l2.Send(_sigSlot, pkt, 0, pkt.Length))
        {
            error = $"send sig=0x{sig:X2} failed";
            return null;
        }

        var deadline = Environment.TickCount64 + timeoutMs;
        while (Environment.TickCount64 < deadline)
        {
            var left = (uint)Math.Max(1, deadline - Environment.TickCount64);
            var rsp = _l2.Recv(_sigSlot, left, out var status);
            if (rsp == null)
            {
                error = $"recv sig=0x{sig:X2} status=0x{status:X8}";
                return null;
            }
            if (rsp.Length < 2) continue;

            var mt = (byte)(rsp[0] & 0x03);
            var tl = (byte)(rsp[0] >> 4);
            if (mt == MtCmd) continue;          // 对端命令：这一版只做 INT
            if (tl != sentTl) continue;
            if (mt == MtReject || mt == MtGeneral)
            {
                error = $"sig=0x{sig:X2} rejected mt=0x{mt:X2}";
                return null;
            }
            return rsp;
        }
        error = $"sig=0x{sig:X2} timeout";
        return null;
    }

    public void Dispose()
    {
        if (_mediaSlot != 0) { _l2.Close(_mediaSlot); _mediaSlot = 0; }
        if (_sigSlot != 0)   { _l2.Close(_sigSlot);   _sigSlot = 0; }
    }
}
