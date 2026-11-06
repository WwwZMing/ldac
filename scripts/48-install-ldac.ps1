# ============================================================================
#  48-install-ldac.ps1 -- 真机安装 LdacA2dp（PortCls 端点 + AVDTP 通道）
# ============================================================================
#  和 VM 里那套是同一份驱动。差别只有一个：真机上没有快照，出事要能手撤。
#  所以这个脚本跑完会明确告诉你回滚命令（scripts\47-uninstall-ldac.ps1）。
#
#  行为边界（很重要）：
#    - 只 stage 驱动包 + 在 **BTHENUM A2DP 子设备** 上做驱动更新，
#      不会去碰其它 MEDIA 设备（曾经被 /install 顺带装到无关设备上过）。
#    - 装上之后耳机的音频端点会从「耳机 (UGREEN-BT505)」变成
#      「LDAC A2DP Audio (Abu)」—— 这是预期，因为函数驱动换人了。
#
#  用法（管理员）：
#      .\scripts\48-install-ldac.ps1
# ============================================================================
param(
    [ValidateSet('Debug', 'Release')] [string]$Configuration = 'Debug',
    [switch]$SkipStage
)

$ErrorActionPreference = 'Continue'

function Assert-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $p  = New-Object Security.Principal.WindowsPrincipal($id)
    if (-not $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw "必须用管理员权限运行（gsudo 或管理员 PowerShell）"
    }
}
Assert-Admin

$root = Split-Path -Parent $PSScriptRoot
if (-not (Test-Path (Join-Path $root 'AGENTS.md'))) { $root = 'C:\Developer\ldac' }
$pkg = Join-Path $root "bin-a2dp\x64\$Configuration\LdacA2dp"
$inf = Join-Path $pkg 'LdacA2dp.inf'
$cer = Join-Path $root "bin-a2dp\x64\$Configuration\LdacA2dp.cer"

if (-not (Test-Path $inf)) { throw "找不到 $inf，先跑 build.ps1" }

Write-Host "== 0) 体检 ==" -ForegroundColor Cyan
$os = Get-CimInstance Win32_OperatingSystem
Write-Host "  OS: $($os.Caption) build $($os.BuildNumber)"
$sb = $null
try { $sb = Confirm-SecureBootUEFI } catch {}
Write-Host "  SecureBoot: $sb"
$ts = & "$env:SystemRoot\System32\bcdedit.exe" /enum '{current}' 2>&1
$tsOn = @($ts | Where-Object { $_ -match 'testsigning\s+Yes' }).Count -gt 0
Write-Host "  testsigning: $(if ($tsOn) { 'YES' } else { 'NO  <-- 驱动加载不了' })" `
    -ForegroundColor $(if ($tsOn) { 'Green' } else { 'Red' })
if (-not $tsOn) {
    Write-Host "  先执行： bcdedit /set testsigning on  然后重启" -ForegroundColor Yellow
    return
}

Write-Host "`n== 1) 信任测试证书 ==" -ForegroundColor Cyan
if (Test-Path $cer) {
    & certutil -addstore -f Root $cer 2>&1 | Select-Object -Last 1 | ForEach-Object { Write-Host "  Root  : $_" }
    & certutil -addstore -f TrustedPublisher $cer 2>&1 | Select-Object -Last 1 | ForEach-Object { Write-Host "  TrustP: $_" }
} else { Write-Host "  !! 缺 $cer" -ForegroundColor Yellow }

Write-Host "`n== 2) 找 A2DP 子设备 ==" -ForegroundColor Cyan
$nodes = @(Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -match '0000110B' })
if ($nodes.Count -eq 0) {
    Write-Host "  !! 没有 A2DP 节点。耳机没配对/没连接过？" -ForegroundColor Red
    return
}
foreach ($n in $nodes) {
    $svc = $null
    try { $svc = (Get-PnpDeviceProperty -InstanceId $n.InstanceId -KeyName 'DEVPKEY_Device_Service' -ErrorAction Stop).Data } catch {}
    Write-Host "  [$($n.Status)] $($n.FriendlyName)  service=$svc"
    Write-Host "        $($n.InstanceId)"
}

Write-Host "`n== 3) 记录回滚信息 ==" -ForegroundColor Cyan
$backup = Join-Path $root "log\ldac-install-backup.txt"
$lines = @(
    "# 安装前状态 $(Get-Date -Format o)",
    ($nodes | ForEach-Object { "node=$($_.InstanceId) service=$((Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName 'DEVPKEY_Device_Service' -ErrorAction SilentlyContinue).Data)" })
)
New-Item -ItemType Directory -Path (Split-Path $backup) -Force | Out-Null
Set-Content -LiteralPath $backup -Value $lines -Encoding UTF8
Write-Host "  已写入 $backup"
Write-Host "  回滚命令： .\scripts\47-uninstall-ldac.ps1 -Reboot" -ForegroundColor Yellow

if (-not $SkipStage) {
    Write-Host "`n== 4) stage 驱动包（不带 /install，避免误装到别的设备） ==" -ForegroundColor Cyan
    & pnputil /add-driver $inf 2>&1 | ForEach-Object { Write-Host "  $_" }
}

Write-Host "`n== 5) 在 A2DP 节点上做驱动更新 ==" -ForegroundColor Cyan
& pnputil /add-driver $inf /install 2>&1 | ForEach-Object { Write-Host "  $_" }
Start-Sleep -Seconds 5

Write-Host "`n== 6) 结果 ==" -ForegroundColor Cyan
foreach ($n in $nodes) {
    $d = Get-PnpDevice -InstanceId $n.InstanceId -ErrorAction SilentlyContinue
    if ($d) {
        $svc = $null
        try { $svc = (Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName 'DEVPKEY_Device_Service' -ErrorAction Stop).Data } catch {}
        Write-Host "  [$($d.Status)] $($d.FriendlyName)  service=$svc  problem=$($d.Problem)"
    }
}
$s = Get-Service LdacA2dp -ErrorAction SilentlyContinue
Write-Host "  LdacA2dp service = $(if ($s) { $s.Status } else { '(absent)' })"

Write-Host "`n  --- audio endpoints ---"
Get-PnpDevice -Class AudioEndpoint -ErrorAction SilentlyContinue |
    ForEach-Object { Write-Host "    [$($_.Status)] $($_.FriendlyName)" }

Write-Host "`n  --- control device ---"
try {
    $h = [IO.File]::Open('\\.\LdacCtl', 'Open', 'ReadWrite', 'None')
    Write-Host "    \\.\LdacCtl open OK" -ForegroundColor Green
    $h.Close()
} catch {
    Write-Host "    \\.\LdacCtl: $($_.Exception.Message)" -ForegroundColor Yellow
}

Write-Host "`n  --- driver log (tail 40) ---"
$logp = 'C:\Users\Public\ldac-probe.log'
if (Test-Path $logp) { Get-Content $logp -Tail 40 | ForEach-Object { Write-Host "    $_" } }
else { Write-Host "    (no log)" }

Write-Host "`n完成。出问题跑： .\scripts\47-uninstall-ldac.ps1 -Reboot" -ForegroundColor Green
