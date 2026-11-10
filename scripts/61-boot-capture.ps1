# ============================================================================
#  61-boot-capture.ps1 -- 开机自动抓 BT 空口 + 采样面板状态
# ============================================================================
#  由计划任务 LdacBootCapture 在 SYSTEM 启动后拉起，抓开机后前 N 秒的 HCI 与
#  发包节奏，用于复现“开机后声音滋滋”。必须用 pwsh 7 运行：Windows PowerShell
#  5.1 的 [IO.File]::Open 打不开 \\.\USBPcapN 这类设备路径。
#
#  用法： .\scripts\61-boot-capture.ps1 -Seconds 300
# ============================================================================
param(
    [int]$Seconds = 300,
    [string]$OutDir = 'C:\Developer\ldac\log\boot'
)

$ErrorActionPreference = 'Continue'
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$summary = Join-Path $OutDir "summary-$stamp.txt"

function Say($s) { $s | Tee-Object -FilePath $summary -Append | Out-Null }

$boot = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime
Say "psver=$($PSVersionTable.PSVersion)  boot=$($boot.ToString('o'))  start=$(Get-Date -Format o)  seconds=$Seconds"

$probe = 'C:\Users\Public\ldac-probe.log'
if (Test-Path $probe) { Say "probe_start=$((Get-Item $probe).Length)" }
$senderLog = 'C:\ProgramData\LdacPanel\sender.log'
if (Test-Path $senderLog) { Say "sender_start=$((Get-Item $senderLog).Length)" }

#  不探测设备是否存在（USBPcap 控制设备不认 Test-Path，5.1 的 [IO.File]::Open
#  也打不开），直接对 1..8 各起一路；开机早期 USB 栈可能还没就绪，
#  起不来就整体重试，最多等 2 分钟。
$caps = @()
$alive = @()
$try = 0
for ($try = 1; $try -le 24; $try++) {
    $caps = @()
    foreach ($n in 1..8) {
        $out = Join-Path $OutDir "$stamp-usbpcap$n.pcap"
        $args1 = @('-d', "\\.\USBPcap$n", '-A', '--inject-descriptors', '-o', $out)
        try {
            $p = Start-Process 'C:\Program Files\USBPcap\USBPcapCMD.exe' -ArgumentList $args1 -PassThru -NoNewWindow -RedirectStandardOutput (Join-Path $OutDir "$stamp-u$n.out") -RedirectStandardError (Join-Path $OutDir "$stamp-u$n.err")
            $caps += [pscustomobject]@{ N = $n; Proc = $p; File = $out }
        } catch { Say "capture$n start failed: $($_.Exception.Message)" }
    }
    Start-Sleep -Seconds 5
    $alive = @($caps | Where-Object { (Test-Path $_.File) -and ((Get-Item $_.File).Length -gt 0) })
    if ($alive.Count -gt 0) { break }
    Say "capture retry $try (usb stack not ready)"
    foreach ($c in $caps) { Stop-Process -Id $c.Proc.Id -Force -EA 0 }
}
if ($alive.Count -eq 0) { Say 'all captures empty after retries; abort'; exit 1 }
Say "capture_alive=$($alive | ForEach-Object { $_.N } | Join-String -Separator ',') retry=$try"

$mon = Join-Path $OutDir "$stamp-monitor.txt"
'sec state pkt peak pps kbps' | Out-File $mon
$apiOk = $false
for ($i = 0; $i -lt $Seconds; $i++) {
    $line = "$i - - - - -"
    try {
        $c = New-Object System.Net.Sockets.TcpClient('127.0.0.1', 48620)
        $s = $c.GetStream(); $w = New-Object IO.StreamWriter($s); $r = New-Object IO.StreamReader($s)
        $w.WriteLine('status'); $w.Flush(); $j = $r.ReadLine(); $c.Close()
        $o = $j | ConvertFrom-Json
        if (-not $apiOk) { $apiOk = $true; Say "api_first_at_sec=$i" }
        $line = "{0} {1} {2} {3} {4:N1} {5:N0}" -f $i, $o.status.state, $o.status.packets, $o.status.peak, $o.status.pps, $o.status.kbps
    } catch { }
    $line | Out-File $mon -Append
    Start-Sleep -Seconds 1
}

foreach ($c in $caps) { Stop-Process -Id $c.Proc.Id -Force -EA 0 }
Start-Sleep -Seconds 2
foreach ($c in $caps) {
    if ((Test-Path $c.File) -and ((Get-Item $c.File).Length -gt 0)) {
        Say "capture$($c.N) size=$((Get-Item $c.File).Length)"
    } else {
        Say "capture$($c.N) empty"
        Remove-Item $c.File -Force -EA 0
        Remove-Item (Join-Path $OutDir "$stamp-u$($c.N).out") -Force -EA 0
        Remove-Item (Join-Path $OutDir "$stamp-u$($c.N).err") -Force -EA 0
    }
}
Say "end=$(Get-Date -Format o)"
