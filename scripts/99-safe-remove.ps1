# ============================================================================
#  99-safe-remove.ps1 -- 安全模式下一键清理：关 Verifier + 删 LDAC 驱动
# ============================================================================
#  用在绿屏之后。安全模式下蓝牙栈是残的，但"关 verifier + 删驱动包 + 删服务
#  + 删 sys"一定做得成；做完下次正常启动就既不会加载驱动，也不会被 verifier
#  卡住。
# ============================================================================
$ErrorActionPreference = 'Continue'

"=== LDAC A2DP 安全模式清理 ==="
"时间: " + (Get-Date)
"管理员: " + ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
""

# 0) 关掉 Driver Verifier（否则下次启动可能继续崩在验证检查上）
"--- Verifier ---"
& verifier /reset 2>&1 | Out-Null
& verifier /volatile /reset 2>&1 | Out-Null
$vq = (& verifier /query 2>&1 | Out-String)
if ($vq -match 'No drivers are currently verified|没有任何驱动程序') { "Verifier: 已全部关闭" }
else { "Verifier: 仍有配置（重启后生效）" }
""

# 1) 停并删服务
foreach ($svc in 'LdacA2dp', 'LdacA2dpSvc') {
    & sc.exe stop $svc 2>&1 | Out-Null
    & sc.exe delete $svc 2>&1 | Out-Null
    "服务 $svc : 已尝试停止并删除"
}
""

# 2) 删掉所有 LdacA2dp 驱动包
$enum = & pnputil /enum-drivers 2>&1 | Out-String
$published = @()
$cur = $null
foreach ($line in ($enum -split "`r?`n")) {
    if ($line -match '(oem\d+\.inf)') { $cur = $Matches[1] }
    if ($line -match 'LdacA2dp\.inf' -and $cur) { $published += $cur; $cur = $null }
}
$published = $published | Sort-Object -Unique
if ($published.Count -eq 0) { "没有找到 LdacA2dp 驱动包" }
foreach ($pkg in $published) {
    "删除驱动包 $pkg"
    & pnputil /delete-driver $pkg /uninstall /force 2>&1 | Out-String
}
""

# 3) 删设备节点
$victims = @(Get-PnpDevice -EA 0 | Where-Object { $_.InstanceId -match '0000110B' })
foreach ($d in $victims) {
    $svc = (Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName 'DEVPKEY_Device_Service' -EA 0).Data
    if ($svc -eq 'LdacA2dp') {
        "移除设备节点 " + $d.InstanceId
        & pnputil /remove-device $d.InstanceId 2>&1 | Out-String
    }
}
""

# 4) 删驱动文件
$sys = 'C:\Windows\System32\drivers\LdacA2dp.sys'
if (Test-Path $sys) {
    try { Remove-Item $sys -Force -EA Stop; "已删除 $sys" } catch { "删不掉 $sys（重启后由 PnP 清理）: " + $_.Exception.Message }
} else {
    "$sys 不存在"
}
""

"=== 结果 ==="
$still = & pnputil /enum-drivers 2>&1 | Out-String
if ($still -match 'LdacA2dp\.inf') { "⚠ 仍有驱动包残留，再跑一次" } else { "✅ 驱动包已清空" }
if (Test-Path $sys) { "⚠ 驱动文件仍在" } else { "✅ 驱动文件已删" }
""
"可以正常重启了。"
