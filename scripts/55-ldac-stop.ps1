$sig = @'
using System;
using System.Runtime.InteropServices;
public static class LdacIo {
  [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
  public static extern IntPtr CreateFileW(string n, uint a, uint s, IntPtr sec, uint d, uint f, IntPtr t);
  [DllImport("kernel32.dll", SetLastError=true)]
  public static extern bool DeviceIoControl(IntPtr h, uint code, IntPtr i, uint isz, IntPtr o, uint osz, out uint ret, IntPtr ov);
  [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
}
'@
Add-Type -TypeDefinition $sig
$h = [LdacIo]::CreateFileW('\\.\LdacCtl', [uint32]3221225472, [uint32]3, [IntPtr]::Zero, [uint32]3, [uint32]0, [IntPtr]::Zero)
if ($h -eq [IntPtr]::new(-1)) { Write-Output 'ctl open failed' } else {
  $ret = [uint32]0
  $ok = [LdacIo]::DeviceIoControl($h, [uint32]((0x22 -shl 16) -bor (0x801 -shl 2)), [IntPtr]::Zero, 0, [IntPtr]::Zero, 0, [ref]$ret, [IntPtr]::Zero)
  Write-Output ("stop ok=" + $ok)
  [LdacIo]::CloseHandle($h) | Out-Null
}
