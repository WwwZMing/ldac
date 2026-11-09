# ============================================================================
#  play-music.ps1  —  客户机侧：常驻起 LDAC 发送端（放歌用）
# ============================================================================
#  为什么用「计划任务 + SYSTEM」而不是 Start-Process：
#   1) SSH 会话一断，Start-Process 的子进程会被一起带走 —— 现象是"响一下就没声"；
#   2) 交互式任务（/it）会带一个控制台黑窗口，那个窗口被关掉进程就死 ——
#      现象是"黑窗口不显示就没声音"；
#   3) SYSTEM 任务跑在 session 0，**根本没有可见窗口**，而 loopback 采集
#      （--match 按端点名选）在 session 0 实测正常。
#
#  用法（客户机，管理员）：
#      .\play-music.ps1                 # 起发送端
#      .\play-music.ps1 -Vol 70         # 顺便把端点音量设到 70%
#      .\play-music.ps1 -Stop           # 停
# ============================================================================
param(
    [string]$Match = 'LDAC',
    [int]$Vol = -1,
    [string]$Exe = 'C:\Users\test\ldac-send.exe',
    [string]$Log = 'C:\Users\test\play.log',
    [switch]$Stop
)

$ErrorActionPreference = 'Continue'
$taskName = 'ldac-send'

if ($Stop) {
    & schtasks /delete /tn $taskName /f 2>&1 | Out-Null
    Get-Process ldac-send -ErrorAction SilentlyContinue | Stop-Process -Force
    "stopped"
    return
}

if (-not (Test-Path $Exe)) { throw "missing $Exe" }

& schtasks /delete /tn $taskName /f 2>&1 | Out-Null
Get-Process ldac-send -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 600

if ($Vol -ge 0) {
    $ep = Join-Path (Split-Path $Exe) 'epmeter.exe'
    if (Test-Path $ep) { & $ep --match $Match --vol $Vol --sec 0.1 | Select-Object -Last 1 }
}

Remove-Item $Log -ErrorAction SilentlyContinue
$tr = 'cmd /c "' + $Exe + '" --loopback --match ' + $Match + ' > ' + $Log + ' 2>&1'
& schtasks /create /tn $taskName /tr $tr /sc once /st 23:59 /f /ru SYSTEM /rl HIGHEST 2>&1 |
    ForEach-Object { "  $_" }
& schtasks /run /tn $taskName 2>&1 | ForEach-Object { "  $_" }

Start-Sleep -Seconds 4
$p = Get-Process ldac-send -ErrorAction SilentlyContinue
if ($p) { "running: pid=$($p.Id) session=$($p.SessionId)" } else { "!! not running" }
Get-Content $Log -ErrorAction SilentlyContinue | Select-Object -First 5
