# ============================================================================
#  vm-install-ldac.ps1  —  客户机侧：安装 LdacA2dp（PortCls 端点 + AVDTP）
# ============================================================================
#  两种目标：
#    -Target root  ：装到 ROOT\LdacA2dp。VM 里没蓝牙节点时用它单独验证
#                    「端点 + 控制设备」这两件事，不去碰耳机的设备节点。
#    -Target bt    ：装到真实的 BTHENUM A2DP 子设备上（顶替 inbox btha2dp）。
#                    这一步会真的接管耳机，端点没了就是没了。
#
#  纯 ASCII 输出。
# ============================================================================
param(
    [ValidateSet('root', 'bt', 'uninstall')]
    [string]$Target = 'root',
    [string]$PkgDir = 'C:\ldac\ldacdrv'
)

$ErrorActionPreference = 'Continue'
$inf  = Join-Path $PkgDir 'LdacA2dp.inf'
$cer  = Join-Path $PkgDir 'LdacA2dp.cer'
$rootHwid = 'ROOT\LdacA2dp'
$svcName  = 'LdacA2dp'

function Get-Hwid([string]$id) {
    try { return (Get-PnpDeviceProperty -InstanceId $id -KeyName 'DEVPKEY_Device_HardwareIds' -ErrorAction Stop).Data }
    catch { return @() }
}

function Get-LdacPackages {
    $lines = & pnputil /enum-drivers 2>&1
    $pub = $null
    $res = @()
    foreach ($l in $lines) {
        if ($l -match ':\s*(oem\d+\.inf)\s*$') { $pub = $Matches[1] }
        elseif ($l -match ':\s*(\S*ldaca2dp\.inf)\s*$' -and $pub) { $res += $pub }
    }
    return $res
}

"=== target = $Target ==="

# ---------------------------------------------------------------- 找 BT 节点
$btNode = $null
$btNodes = @(Get-PnpDevice -ErrorAction SilentlyContinue |
             Where-Object { $_.InstanceId -match '0000110B' })
"=== A2DP sink nodes ==="
foreach ($n in $btNodes) { "  [$($n.Status)] $($n.InstanceId)"; "        $($n.FriendlyName)" }

if ($Target -eq 'uninstall') {
    "=== remove our devices ==="
    $victims = @()
    foreach ($d in (Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like 'ROOT\*' })) {
        if ((Get-Hwid $d.InstanceId) -contains $rootHwid) { $victims += $d }
    }
    foreach ($d in $victims) {
        "  remove $($d.InstanceId)"
        & pnputil /remove-device $d.InstanceId 2>&1 | Out-String | ForEach-Object { "    $_" }
    }
    "=== delete packages ==="
    foreach ($p in (Get-LdacPackages)) {
        "  delete $p"
        & pnputil /delete-driver $p /uninstall /force 2>&1 | Out-String | ForEach-Object { "    $_" }
    }
    "=== restore inbox btha2dp on BT node ==="
    foreach ($n in $btNodes) {
        if ((Get-Hwid $n.InstanceId) -match 'LOCALMFG') {
            "  reinstall inbox on $($n.InstanceId)"
            & pnputil /add-driver 'C:\Windows\INF\bth.inf' 2>&1 | Out-String | ForEach-Object { "    $_" }
        }
    }
    "done"
    return
}

if ($Target -eq 'bt') {
    if ($btNodes.Count -eq 0) {
        "!! no A2DP sink node in this VM. Is the headphone paired/connected?"
        return
    }
    $btNode = $btNodes[0]
    "  picked: $($btNode.InstanceId)"
}

# ------------------------------------------------------------ 1) 信任测试证书
"=== 1) trust test cert ==="
if (Test-Path $cer) {
    & certutil -addstore -f Root $cer 2>&1 | Select-Object -Last 1 | ForEach-Object { "  Root  : $_" }
    & certutil -addstore -f TrustedPublisher $cer 2>&1 | Select-Object -Last 1 | ForEach-Object { "  TrustP: $_" }
} else { "  !! missing $cer" }

# ------------------------------------------------------------ 2) 清旧包
"=== 2) remove stale devices/packages ==="
if ($Target -eq 'root') {
    #  root 设备是纯测试用的，可以随便删干净重来
    foreach ($d in (Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like 'ROOT\*' })) {
        if ((Get-Hwid $d.InstanceId) -contains $rootHwid) {
            "  remove $($d.InstanceId)"
            & pnputil /remove-device $d.InstanceId 2>&1 | Out-Null
        }
    }
    foreach ($p in (Get-LdacPackages)) {
        "  delete $p"
        & pnputil /delete-driver $p /uninstall /force 2>&1 | Out-Null
    }
    if (Test-Path "HKLM:\SYSTEM\CurrentControlSet\Services\$svcName") {
        & sc.exe delete $svcName 2>&1 | Out-Null
        for ($i = 0; $i -lt 45; $i++) {
            if (-not (Test-Path "HKLM:\SYSTEM\CurrentControlSet\Services\$svcName")) { break }
            Start-Sleep -Seconds 1
        }
        "  service key gone: $(-not (Test-Path "HKLM:\SYSTEM\CurrentControlSet\Services\$svcName"))"
    }
} else {
    #  ⚠ bt 目标**绝不能删包**：设备正绑在旧驱动上，delete-driver 会把服务
    #    标记成 marked-for-delete，紧接着装新包必然报 1072（已踩两次）。
    #    正确做法是只 stage 新包（DriverVer 更高），PnP 自己做驱动更新。
    "  (bt target: keep existing package, rely on version-based update)"
    "  service present: $(Test-Path "HKLM:\SYSTEM\CurrentControlSet\Services\$svcName")"
}
Remove-Item 'C:\Users\Public\ldac-probe.log' -Force -ErrorAction SilentlyContinue
Remove-Item 'C:\Users\Public\ldac-mark-*.txt' -Force -ErrorAction SilentlyContinue

# ------------------------------------------------------------ 3) 装包
#  ⚠ 这里**故意不加 /install**：加了 pnputil 会立刻把包装到所有硬件 ID 匹配的
#    *现存* 设备上 —— 也就是会当场顶掉耳机节点上的 inbox btha2dp，绕过
#    -Target root 的隔离验证。只 stage，绑定留给下面的 devcon。
"=== 3) add driver package (staged, no /install) ==="
& pnputil /add-driver $inf 2>&1 | Out-String | ForEach-Object { "  $_" }

# ------------------------------------------------------------ 4) 建/绑设备
"=== 4) bind ==="
if ($Target -eq 'root') {
    & 'C:\ldac\devcon.exe' install $inf $rootHwid 2>&1 | Out-String | ForEach-Object { "  $_" }
} else {
    $hw = (Get-Hwid $btNode.InstanceId)
    "  hardware ids: $($hw -join ' , ')"
    #  devcon update 在"MEDIA 类的包顶掉蓝牙类节点的 inbox 驱动"这个场景会被拒，
    #  pnputil 的 /install 是强制路径（相当于 setupapi 的 INSTALLFLAG_FORCE）。
    "  pnputil /add-driver /install (forced)"
    & pnputil /add-driver $inf /install 2>&1 | Out-String | ForEach-Object { "  $_" }
}
Start-Sleep -Seconds 5

#  失败的时候把 setupapi 的真实原因抓出来，比"devcon failed"有用
if (-not (Get-Service -Name $svcName -ErrorAction SilentlyContinue)) {
    "  !! service not created -- setupapi tail:"
    $sp = 'C:\Windows\INF\setupapi.dev.log'
    if (Test-Path $sp) {
        Get-Content $sp -Tail 40 -ErrorAction SilentlyContinue |
            Where-Object { $_ -match 'ldac|!!!|error|fail' } |
            Select-Object -Last 15 | ForEach-Object { "    $_" }
    }
}

# ------------------------------------------------------------ 5) 设备状态
"=== 5) device / service ==="
foreach ($d in (Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like 'ROOT\*' })) {
    if ((Get-Hwid $d.InstanceId) -contains $rootHwid) {
        "  ROOT: [$($d.Status)] problem=$($d.Problem) $($d.InstanceId)  $($d.FriendlyName)"
    }
}
if ($btNode) {
    $d2 = Get-PnpDevice -InstanceId $btNode.InstanceId -ErrorAction SilentlyContinue
    if ($d2) { "  BT  : [$($d2.Status)] problem=$($d2.Problem) $($d2.FriendlyName)" }
}
$s = Get-Service -Name $svcName -ErrorAction SilentlyContinue
if ($s) { "  service $svcName = $($s.Status)" } else { "  service $svcName = (absent)" }

# ------------------------------------------------------------ 6) 音频端点
"=== 6) audio endpoints ==="
Get-PnpDevice -Class AudioEndpoint -ErrorAction SilentlyContinue |
    ForEach-Object { "  [$($_.Status)] $($_.FriendlyName)" }

# ------------------------------------------------------------ 7) 控制设备
"=== 7) control device \\\\.\\LdacCtl ==="
try {
    $h = [IO.File]::Open('\\.\LdacCtl', 'Open', 'ReadWrite', 'None')
    "  open OK, can seek: $($h.CanSeek)"
    $h.Close()
} catch {
    "  !! open failed: $($_.Exception.Message)"
}

# ------------------------------------------------------------ 8) 驱动日志
"=== 8) driver log ==="
if (Test-Path 'C:\Users\Public\ldac-probe.log') {
    Get-Content 'C:\Users\Public\ldac-probe.log' | ForEach-Object { "  $_" }
} else { "  (no log)" }

"=== 9) marks ==="
Get-ChildItem 'C:\Users\Public\ldac-mark-*.txt' -ErrorAction SilentlyContinue |
    Sort-Object Name | ForEach-Object { "  $($_.BaseName)" }
