# ============================================================================
#  remote.ps1 -- 带硬超时的客户机远程执行器
# ============================================================================
#  裸写 ssh 一旦客户机蓝屏/重启，会挂几十秒甚至更久，把调用方整个卡死。
#  这里用 Process + WaitForExit(超时) 兜住，到点直接 Kill，永远返回。
#
#  用法：
#    .\scripts\remote.ps1 -Command '"hello"'
#    .\scripts\remote.ps1 -Command '...' -TimeoutSec 20
#
#  退出码：0 正常，124 超时
# ============================================================================
param(
    [Parameter(Mandatory = $true)][string]$Command,
    [string]$Guest = 'test@192.168.144.130',
    [string]$Key = "$env:USERPROFILE\.ssh\id_ed25519_vm_test",
    [int]$TimeoutSec = 30
)

$ErrorActionPreference = 'Stop'

$b64 = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($Command))
$guestCmd = 'C:\PROGRA~1\PowerShell\7\pwsh.exe -NoProfile -EncodedCommand ' + $b64

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = 'ssh.exe'
$psi.Arguments = '-i "' + $Key + '" -o BatchMode=yes -o ConnectTimeout=5 ' +
                  '-o ServerAliveInterval=3 -o ServerAliveCountMax=1 ' + $Guest + ' "' + $guestCmd + '"'
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.UseShellExecute = $false
$psi.CreateNoWindow = $true

$p = [System.Diagnostics.Process]::Start($psi)
$out = $p.StandardOutput.ReadToEndAsync()
$err = $p.StandardError.ReadToEndAsync()

if (-not $p.WaitForExit($TimeoutSec * 1000)) {
    try { $p.Kill($true) } catch { }
    Write-Output ("[remote] TIMEOUT after " + $TimeoutSec + "s -- killed (guest probably crashed or rebooting)")
    exit 124
}

$p.WaitForExit()
if ($out.Result) { Write-Output $out.Result.TrimEnd() }
if ($err.Result) { Write-Output ("[stderr] " + $err.Result.TrimEnd()) }
exit $p.ExitCode
