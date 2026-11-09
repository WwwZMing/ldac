using System;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace LdacPanel;

internal enum LdacState
{
    Idle = 0,
    Starting = 1,
    Streaming = 2,
    Error = 3,
}

[StructLayout(LayoutKind.Sequential)]
internal struct LdacStatus
{
    public uint Version;
    public int State;
    public int LastError;
    public uint SigMtu;
    public uint MediaMtu;
    public uint RateBit;
    public uint ModeBit;
    public uint SinkSeid;
    public uint Packets;   // v2：内核累计发出的包数（判断链路是否还活着）
    public uint Bytes;
    public uint Codec;     // v3：0=LDAC 1=SBC 2=AAC（本次会话实际协商的）
}

internal static class Ctl
{
    private const uint FileDeviceUnknown = 0x00000022;
    private const uint FileAnyAccess = 0;
    private const uint MethodBuffered = 0;

    private const uint GenericRead = 0x80000000;
    private const uint GenericWrite = 0x40000000;
    private const uint OpenExisting = 3;
    private const uint FileShareRead = 0x00000001;
    private const uint FileShareWrite = 0x00000002;

    public static uint Code(uint function) =>
        (FileDeviceUnknown << 16) | (FileAnyAccess << 14) | (function << 2) | MethodBuffered;

    public static readonly uint IoctlStart = Code(0x800);
    public static readonly uint IoctlStop = Code(0x801);
    public static readonly uint IoctlStatus = Code(0x802);
    public static readonly uint IoctlSetCodec = Code(0x805);

    [DllImport("kernel32.dll", EntryPoint = "CreateFileW", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern SafeFileHandle CreateFile(string name, uint access, uint share, IntPtr security,
        uint disposition, uint flags, IntPtr template);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool DeviceIoControl(SafeFileHandle device, uint code, byte[]? inBuffer, uint inSize,
        byte[]? outBuffer, uint outSize, out uint returned, IntPtr overlapped);

    public static SafeFileHandle? Open(out int error)
    {
        var handle = CreateFile(@"\\.\LdacCtl", GenericRead | GenericWrite,
            FileShareRead | FileShareWrite, IntPtr.Zero, OpenExisting, 0, IntPtr.Zero);
        error = handle.IsInvalid ? Marshal.GetLastWin32Error() : 0;
        if (handle.IsInvalid)
        {
            handle.Dispose();
            return null;
        }
        return handle;
    }

    /// <summary>0 = LDAC，1 = SBC，2 = AAC。只影响下一次会话建立。</summary>
    public static bool SetCodec(int codec, out int error)
    {
        error = 0;
        lock (Sync)
        {
            using var handle = Open(out error);
            if (handle is null) return false;
            var input = BitConverter.GetBytes((uint)codec);
            var ok = DeviceIoControl(handle, IoctlSetCodec, input, (uint)input.Length,
                                     null, 0, out _, IntPtr.Zero);
            if (!ok) error = Marshal.GetLastWin32Error();
            return ok;
        }
    }

    public static LdacStatus? Status(out int error)
    {
        error = 0;
        lock (Sync)
        {
            using var handle = Open(out error);
            if (handle is null) return null;
            var buffer = new byte[44];
            if (!DeviceIoControl(handle, IoctlStatus, null, 0, buffer, 44, out _, IntPtr.Zero))
            {
                error = Marshal.GetLastWin32Error();
                return null;
            }
            return MemoryMarshal.Read<LdacStatus>(buffer);
        }
    }

    public static bool Start(byte[] request, out int error)
    {
        lock (Sync)
        {
            using var handle = Open(out error);
            if (handle is null) return false;
            var ok = DeviceIoControl(handle, IoctlStart, request, (uint)request.Length, null, 0, out _, IntPtr.Zero);
            if (!ok) error = Marshal.GetLastWin32Error();
            return ok;
        }
    }

    public static bool Stop(out int error)
    {
        lock (Sync)
        {
            using var handle = Open(out error);
            if (handle is null) return false;
            var ok = DeviceIoControl(handle, IoctlStop, null, 0, null, 0, out _, IntPtr.Zero);
            if (!ok) error = Marshal.GetLastWin32Error();
            return ok;
        }
    }

    private static readonly object Sync = new();

    public static readonly uint IoctlL2capOpen  = Code(0x810);
    public static readonly uint IoctlL2capSend  = Code(0x811);
    public static readonly uint IoctlL2capRecv  = Code(0x812);
    public static readonly uint IoctlL2capClose = Code(0x813);

    /// <summary>开一条 L2CAP 通道；返回槽位句柄（0 = 失败）。</summary>
    public static uint L2capOpen(ushort psm, byte[]? addr6, out uint mtu, out int status)
    {
        mtu = 0; status = 0;
        var req = new byte[12];
        BitConverter.GetBytes((uint)psm).CopyTo(req, 0);
        if (addr6 is { Length: 6 }) Array.Copy(addr6, 0, req, 4, 6);
        lock (Sync)
        {
            using var handle = Open(out var err);
            if (handle is null) { status = -err; return 0; }
            var rsp = new byte[12];
            if (!DeviceIoControl(handle, IoctlL2capOpen, req, (uint)req.Length, rsp, (uint)rsp.Length, out _, IntPtr.Zero))
            {
                status = -Marshal.GetLastWin32Error();
                return 0;
            }
            var h = BitConverter.ToUInt32(rsp, 0);
            mtu = BitConverter.ToUInt32(rsp, 4);
            status = BitConverter.ToInt32(rsp, 8);
            return h;
        }
    }

    public static bool L2capSend(uint slot, byte[] payload, int offset, int length)
    {
        var req = new byte[8 + length];
        BitConverter.GetBytes(slot).CopyTo(req, 0);
        BitConverter.GetBytes((uint)length).CopyTo(req, 4);
        Array.Copy(payload, offset, req, 8, length);
        lock (Sync)
        {
            using var handle = Open(out _);
            if (handle is null) return false;
            return DeviceIoControl(handle, IoctlL2capSend, req, (uint)req.Length, null, 0, out _, IntPtr.Zero);
        }
    }

    /// <summary>阻塞收一段载荷；超时返回 null（status = 0xC00000B5）。</summary>
    public static byte[]? L2capRecv(uint slot, uint timeoutMs, out int status)
    {
        status = 0;
        var req = new byte[8];
        BitConverter.GetBytes(slot).CopyTo(req, 0);
        BitConverter.GetBytes(timeoutMs).CopyTo(req, 4);
        var buf = new byte[1500];
        lock (Sync)
        {
            using var handle = Open(out var err);
            if (handle is null) { status = -err; return null; }
            if (!DeviceIoControl(handle, IoctlL2capRecv, req, (uint)req.Length, buf, (uint)buf.Length, out var got, IntPtr.Zero))
            {
                status = -Marshal.GetLastWin32Error();
                return null;
            }
            var outBuf = new byte[got];
            Array.Copy(buf, outBuf, (int)got);
            return outBuf;
        }
    }

    public static bool L2capClose(uint slot)
    {
        var req = new byte[4];
        BitConverter.GetBytes(slot).CopyTo(req, 0);
        lock (Sync)
        {
            using var handle = Open(out _);
            if (handle is null) return false;
            return DeviceIoControl(handle, IoctlL2capClose, req, (uint)req.Length, null, 0, out _, IntPtr.Zero);
        }
    }
}
