# ============================================================================
#  analyze-play-log.ps1 -- 从 ldac-send 的日志里读出发包节拍 / 静音段 / 补静音量
# ============================================================================
#  用法：
#    .\scripts\analyze-play-log.ps1 -Path .\log\host-play.log
#    .\scripts\analyze-play-log.ps1 -Path .\log\incident-2026-11-06-audio-drop\play-full.log
#
#  日志格式：[pkt N] peak=P frames=F len=L fill=B
#    每 100 包一行；1 包 = 2 帧 × 128 样本 = 5.33ms @48k
#    fill = 采集断流时补进去的静音字节数（累计，单调不减）
#
#  输出：时间轴覆盖、静音段（peak 恒 0）、fill 增长事件、包率异常
# ============================================================================
param(
    [Parameter(Mandatory = $true)][string]$Path,
    [int]$Top = 10
)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path $Path)) { throw "找不到 $Path" }

$lines = Get-Content $Path
#  fill= 是后加的字段，旧日志没有 → 整段可选
$rx = '\[pkt (\d+)\] peak=(\d+) frames=(\d+) len=(\d+)(?: fill=(\d+))?'
$rows = New-Object System.Collections.Generic.List[object]
foreach ($l in $lines) {
    $m = [regex]::Match($l, $rx)
    if (-not $m.Success) { continue }
    $rows.Add([pscustomobject]@{
        Pkt   = [long]$m.Groups[1].Value
        Peak  = [int]$m.Groups[2].Value
        Frames= [int]$m.Groups[3].Value
        Len   = [int]$m.Groups[4].Value
        Fill  = if ($m.Groups[5].Success -and $m.Groups[5].Value) { [long]$m.Groups[5].Value } else { 0 }
    })
}

if ($rows.Count -lt 2) { "没有可解析的行（格式不对？）"; return }

$first = $rows[0]
$last  = $rows[$rows.Count - 1]
$perPktMs = 5.33        # 1 包 = 2 帧 × 128 样本 @48k
#  注意：Pkt 是真实的包序号（每包 +1），日志只是每 100 包打一行
$spanSec = ($last.Pkt - $first.Pkt) * $perPktMs / 1000.0

"=== $Path ==="
"包号范围 : {0:N0} .. {1:N0}" -f $first.Pkt, $last.Pkt
"可解析行 : {0:N0}" -f $rows.Count
"时间覆盖 : 约 {0:N1} 秒（{1:N1} 分钟）" -f $spanSec, ($spanSec / 60)
"平均包率 : {0:N1} 包/秒" -f (($last.Pkt - $first.Pkt) / $spanSec)

# ---- 静音段 ----
$runs = New-Object System.Collections.Generic.List[object]
$startPkt = $null; $n = 0
foreach ($r in $rows) {
    if ($r.Peak -eq 0) {
        if ($null -eq $startPkt) { $startPkt = $r.Pkt }
        $n++
    } else {
        if ($null -ne $startPkt) {
            $runs.Add([pscustomobject]@{
                From = $startPkt; To = $r.Pkt
                Lines = $n; Sec = [math]::Round($n * 100 * $perPktMs / 1000, 1)
            })
            $startPkt = $null; $n = 0
        }
    }
}
if ($null -ne $startPkt) {
    $runs.Add([pscustomobject]@{ From = $startPkt; To = $last.Pkt; Lines = $n
        Sec = [math]::Round($n * 100 * $perPktMs / 1000, 1) })
}

$totalQuiet = ($runs | Measure-Object -Property Sec -Sum).Sum
"";"静音段   : {0} 段，合计 {1:N1} 秒（占 {2:N1}%）" -f $runs.Count, $totalQuiet, (100 * $totalQuiet / [math]::Max($spanSec, 1))
if ($runs.Count -gt 0) {
    "最长的 $Top 段："
    $runs | Sort-Object Sec -Descending | Select-Object -First $Top |
        Format-Table -AutoSize | Out-String -Width 100
}

# ---- fill 增长（补静音 = 采集断流）----
$fillEvents = New-Object System.Collections.Generic.List[object]
$prev = $rows[0].Fill
$prevPkt = $rows[0].Pkt
foreach ($r in $rows) {
    if ($r.Fill -gt $prev) {
        $fillEvents.Add([pscustomobject]@{
            AtPkt = $r.Pkt
            Bytes = $r.Fill - $prev
            Ms    = [math]::Round(($r.Fill - $prev) / 8.0 / 48.0, 0)   # 48k/32bit/2ch = 8 B/帧
        })
        $prev = $r.Fill
    }
}
"";"fill 累计 : {0:N0} 字节（≈ {1:N1} 秒静音）" -f $last.Fill, ($last.Fill / 8.0 / 48000)
"fill 增长 : {0} 行有增量（连续增长 = 一段断流，逐行累加没意义）" -f $fillEvents.Count
if ($fillEvents.Count -gt 0) {
    "最大的补静音事件："
    $fillEvents | Sort-Object Bytes -Descending | Select-Object -First $Top |
        Format-Table -AutoSize | Out-String -Width 100
}

# ---- 包率异常（掉速 = 发包跟不上）----
"";"包率异常（相邻两行间隔 > 1.5 倍正常值）："
$normalDelta = 100.0 / 187.0
$dips = New-Object System.Collections.Generic.List[object]
for ($i = 1; $i -lt $rows.Count; $i++) {
    $d = $rows[$i].Pkt - $rows[$i - 1].Pkt
    if ($d -ne 100) {   # 日志每 100 包一行，缺行 = 掉速
        $dips.Add([pscustomobject]@{ AtPkt = $rows[$i].Pkt; Delta = $d })
    }
}
if ($dips.Count -eq 0) { "  无" } else { $dips | Select-Object -First $Top | Format-Table -AutoSize | Out-String -Width 100 }
