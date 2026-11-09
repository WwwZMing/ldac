# 等 A2DP 节点出现 -> 同时抓两个 USB 根集线器 -> 放 20 秒 AAC -> 收尾
$ErrorActionPreference = 'Continue'
$out = 'C:\Developer\ldac\log\capture-run.txt'
"=== start $(Get-Date -Format 'HH:mm:ss')" | Out-File $out
$deadline = (Get-Date).AddMinutes(6)
$ready = $false
while ((Get-Date) -lt $deadline) {
    $node = Get-PnpDevice -EA 0 | Where-Object { $_.InstanceId -match '0000110B' -and $_.Present }
    $ctl = Test-Path '\\.\LdacCtl'
    if ($node -and $ctl) { $ready = $true; break }
    Start-Sleep -Seconds 3
}
"ready=$ready at $(Get-Date -Format 'HH:mm:ss')" | Out-File $out -Append
if (-not $ready) { "give up: speaker never connected" | Out-File $out -Append; exit 1 }

Remove-Item 'C:\Developer\ldac\log\bt1.pcap','C:\Developer\ldac\log\bt2.pcap' -EA SilentlyContinue
$u1 = Start-Process -FilePath 'C:\Program Files\USBPcap\USBPcapCMD.exe' -ArgumentList '-d','\\.\USBPcap1','-o','C:\Developer\ldac\log\bt1.pcap' -PassThru -NoNewWindow -RedirectStandardOutput 'C:\Developer\ldac\log\u1.out' -RedirectStandardError 'C:\Developer\ldac\log\u1.err'
$u2 = Start-Process -FilePath 'C:\Program Files\USBPcap\USBPcapCMD.exe' -ArgumentList '-d','\\.\USBPcap2','-o','C:\Developer\ldac\log\bt2.pcap' -PassThru -NoNewWindow -RedirectStandardOutput 'C:\Developer\ldac\log\u2.out' -RedirectStandardError 'C:\Developer\ldac\log\u2.err'
Start-Sleep -Seconds 3
"capture running $(Get-Date -Format 'HH:mm:ss')" | Out-File $out -Append

reg add 'HKLM\SYSTEM\CurrentControlSet\Services\LdacA2dp\Parameters' /v Codec /t REG_DWORD /d 2 /f | Out-Null
& 'C:\Developer\ldac\bin-host\Debug\ldac-send.exe' 'C:\Developer\ldac\log\tone48k20.wav' --codec aac 2>&1 |
    Select-String -Pattern 'frames=|drained|START failed|FATAL' | ForEach-Object { $_.Line } | Out-File $out -Append
"tone done $(Get-Date -Format 'HH:mm:ss')" | Out-File $out -Append

Start-Sleep -Seconds 2
Stop-Process -Id $u1.Id,$u2.Id -Force -EA SilentlyContinue
Start-Sleep -Seconds 2
foreach ($f in 'bt1','bt2') {
    $p = "C:\Developer\ldac\log\$f.pcap"
    if (Test-Path $p) { ("$f.pcap = " + (Get-Item $p).Length + " bytes") | Out-File $out -Append }
    else { "$f.pcap missing" | Out-File $out -Append }
}
"=== end $(Get-Date -Format 'HH:mm:ss')" | Out-File $out -Append
