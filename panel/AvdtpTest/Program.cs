using System;
using System.Collections.Generic;
using LdacPanel;

namespace LdacPanel
{
    internal static class PanelLog
    {
        public static void Write(string s) => Console.WriteLine("    [log] " + s);
    }
}

internal sealed class FakeL2cap : IL2cap
{
    public readonly List<byte[]> Sent = new();
    private readonly Queue<byte[]> _rsp = new();
    private int _next = 1;

    public FakeL2cap Enqueue(params byte[][] packets)
    {
        foreach (var p in packets) _rsp.Enqueue(p);
        return this;
    }

    public uint Open(ushort psm, byte[]? addr6, out uint mtu, out int status)
    {
        mtu = 679; status = 0;
        return (uint)(_next++);
    }

    public bool Send(uint slot, byte[] payload, int offset, int length)
    {
        var copy = new byte[length];
        Array.Copy(payload, offset, copy, 0, length);
        Sent.Add(copy);
        return true;
    }

    /// <summary>先制造 N 次收包超时（模拟"第一条 Discover 没人应答"）。</summary>
    public int FailNextRecv;

    /// <summary>回放时把应答的 TL 改写成最后一条命令的 TL（默认开）。</summary>
    public bool RewriteTl = true;

    public byte[]? Recv(uint slot, uint timeoutMs, out int status)
    {
        status = 0;
        if (FailNextRecv > 0) { FailNextRecv--; return null; }
        if (_rsp.Count == 0) return null;
        var p = _rsp.Dequeue();
        if (RewriteTl && Sent.Count > 0)
            p[0] = (byte)(((Sent[^1][0] >> 4) << 4) | (p[0] & 0x0F));
        return p;
    }

    public bool Close(uint slot) => true;
}

internal static class Program
{
    private static int _fail;

    private static void Check(string what, bool ok)
    {
        Console.WriteLine((ok ? "  PASS  " : "  FAIL  ") + what);
        if (!ok) _fail++;
    }

    private static string Hex(byte[] b) => BitConverter.ToString(b).Replace("-", " ");

    //  AVDTP 应答：头 = (tl<<4)|mt。tl 固定为 0 的用例用手写数组。
    private static byte[] Resp(int tl, byte mt, params byte[] body)
    {
        var p = new byte[2 + body.Length];
        p[0] = (byte)((tl << 4) | mt);
        p[1] = 0x00;
        Array.Copy(body, 0, p, 2, body.Length);
        return p;
    }

    private static byte[] DiscoverRsp(int tl) => Resp(tl, 0x02, 0x04, 0x08);
    private static byte[] CapsRsp(int tl, byte rate, byte mode) =>
        Resp(tl, 0x02, 0x04, 0x08,
             0x01, 0x00,
             0x07, 0x0A, 0x00, 0xFF, 0x2D, 0x01, 0x00, 0x00, 0xAA, 0x00, rate, mode);
    private static byte[] Accept(int tl) => Resp(tl, 0x02);

    private static int Main()
    {
        // ---- 用例 1：完整 LDAC 会话，逐字节核对 ----
        Console.WriteLine("case 1: LDAC sink, byte-exact");
        var l2 = new FakeL2cap().Enqueue(
            DiscoverRsp(0), CapsRsp(1, 0x30, 0x07), Accept(2), Accept(3), Accept(4));
        var a = new Avdtp(l2);
        var ok = a.Connect(null, 1, null, out var err);
        Check("connect ok (" + err + ")", ok);
        Check("rate 0x10 (48k preferred over 44.1k)", a.ChosenRateBit == 0x10);
        Check("mode 0x01 (stereo)", a.ChosenModeBit == 0x01);
        Check("media mtu 679", a.MediaMtu == 679);
        if (l2.Sent.Count == 5)
        {
            Check("discover 00 01", Hex(l2.Sent[0]) == "00 01");
            Check("getcaps 10 02 04", Hex(l2.Sent[1]) == "10 02 04");
            Check("setconfig bytes", Hex(l2.Sent[2]) == "20 03 04 04 01 00 07 0A 00 FF 2D 01 00 00 AA 00 10 01");
            Check("open 30 06 04", Hex(l2.Sent[3]) == "30 06 04");
            Check("start 40 07 04", Hex(l2.Sent[4]) == "40 07 04");
        }
        a.Dispose();

        // ---- 用例 2：只有 SBC ----
        Console.WriteLine("case 2: no LDAC SEP");
        var l2b = new FakeL2cap().Enqueue(
            DiscoverRsp(0), Resp(1, 0x02, 0x04, 0x08, 0x01, 0x00, 0x07, 0x06, 0x00, 0x00, 0xFF, 0xFF, 0x02, 0x35));
        var b = new Avdtp(l2b);
        var okb = b.Connect(null, 1, null, out var errb);
        Check("rejected (" + errb + ")", !okb && errb.Contains("no LDAC SEP"));
        b.Dispose();

        // ---- 用例 3：只支持 44.1k 的 sink ----
        Console.WriteLine("case 3: 44.1k only");
        var l2c = new FakeL2cap().Enqueue(
            DiscoverRsp(0), CapsRsp(1, 0x20, 0x01), Accept(2), Accept(3), Accept(4));
        var c = new Avdtp(l2c);
        var okc = c.Connect(null, 1, null, out _);
        Check("rate 0x20", okc && c.ChosenRateBit == 0x20);
        c.Dispose();

        // ---- 用例 4：SetConfiguration 被拒 ----
        Console.WriteLine("case 4: setconfig rejected");
        var l2d = new FakeL2cap().Enqueue(
            DiscoverRsp(0), CapsRsp(1, 0x30, 0x07), Resp(2, 0x03, 0x00));
        var d = new Avdtp(l2d);
        var okd = d.Connect(null, 1, null, out var errd);
        Check("connect fails (" + errd + ")", !okd && errd.Contains("rejected"));
        d.Dispose();

        // ---- 用例 5：TL 不匹配的应答要被忽略 ----
        Console.WriteLine("case 5: stray TL ignored");
        var l2e = new FakeL2cap { RewriteTl = false }.Enqueue(
            Resp(7, 0x02, 0x04, 0x08),   // 错误的 TL，应被丢弃
            DiscoverRsp(0), CapsRsp(1, 0x30, 0x07), Accept(2), Accept(3), Accept(4));
        var e = new Avdtp(l2e);
        var oke = e.Connect(null, 1, null, out var erre);
        Check("still connects (" + erre + ")", oke);
        e.Dispose();

        // ---- 用例 6：对端穿插自己的命令（MT=CMD）要被跳过 ----
        Console.WriteLine("case 6: peer command interleaved");
        var l2f = new FakeL2cap().Enqueue(
            DiscoverRsp(0),
            Resp(1, 0x00, 0x01),          // 对端的 Discover 命令，不是我们的应答
            CapsRsp(1, 0x30, 0x07), Accept(2), Accept(3), Accept(4));
        var f = new Avdtp(l2f);
        var okf = f.Connect(null, 1, null, out var errf);
        Check("still connects (" + errf + ")", okf);
        f.Dispose();

        // ---- 用例 7：Open/Start 不应答也要能进 STREAMING（和内核行为一致）----
        Console.WriteLine("case 7: open/start not acked");
        var l2g = new FakeL2cap();
        l2g.Enqueue(DiscoverRsp(0), CapsRsp(1, 0x30, 0x07), Accept(2));   // 之后没有应答
        var g = new Avdtp(l2g);
        var okg = g.Connect(null, 1, null, out var errg);
        Check("proceeds without open/start ack (" + errg + ")", okg);
        g.Dispose();

        // ---- 用例 8：混音率偏好（88.2k）被采纳 ----
        Console.WriteLine("case 8: pref rate honoured");
        var l2h = new FakeL2cap().Enqueue(
            DiscoverRsp(0), CapsRsp(1, 0x3C, 0x07), Accept(2), Accept(3), Accept(4));
        var h = new Avdtp(l2h);
        var okh = h.Connect(null, 1, () => (byte)0x08, out _);
        Check("rate 0x08 from preference", okh && h.ChosenRateBit == 0x08);
        h.Dispose();

        // ---- 用例 9：第一条 Discover 超时后要重试并成功 ----
        Console.WriteLine("case 9: discover retry");
        var l2i = new FakeL2cap { FailNextRecv = 1 }.Enqueue(
            DiscoverRsp(0), CapsRsp(1, 0x30, 0x07), Accept(2), Accept(3), Accept(4));
        var i9 = new Avdtp(l2i);
        var ok9 = i9.Connect(null, 1, null, out var err9);
        Check("connects after one timeout (" + err9 + ")", ok9);
        i9.Dispose();

        Console.WriteLine(_fail == 0 ? "ALL PASS" : _fail + " FAILED");
        return _fail == 0 ? 0 : 1;
    }
}


