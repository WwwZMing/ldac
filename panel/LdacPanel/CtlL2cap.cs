namespace LdacPanel;

/// <summary>生产环境的 IL2cap：转发到内核 IOCTL。测试工程不链接这个文件。</summary>
internal sealed class CtlL2cap : IL2cap
{
    public uint Open(ushort psm, byte[]? addr6, out uint mtu, out int status)
        => Ctl.L2capOpen(psm, addr6, out mtu, out status);
    public bool Send(uint slot, byte[] payload, int offset, int length)
        => Ctl.L2capSend(slot, payload, offset, length);
    public byte[]? Recv(uint slot, uint timeoutMs, out int status)
        => Ctl.L2capRecv(slot, timeoutMs, out status);
    public bool Close(uint slot) => Ctl.L2capClose(slot);
}
