# bt-capture-pairing.ps1 - capture a clean A2DP session (inbox driver) as a reference sample.
#
#   Run elevated (gsudo). It:
#     1. turns on full HCI payload logging (BthPort registry knobs)
#     2. restarts bthserv  -> drops every Bluetooth link, so you can start from pairing
#     3. starts an ETW trace on BTHPORT + BTHUSB
#     4. waits WaitSeconds for YOU to: remove device -> pair again -> play music
#     5. stops the trace and converts it to XML
#
#   Output: log\bt-pairing.etl / log\bt-pairing.xml
#
#   No product driver involved - this must stay a pure "what does the healthy
#   Windows stack actually put on the wire" sample.

param([int]$WaitSeconds = 120)

$ErrorActionPreference = 'Continue'
$root = 'C:\Developer\ldac'
$etl  = "$root\log\bt-pairing.etl"
$xml  = "$root\log\bt-pairing.xml"
$pkey = 'HKLM:\SYSTEM\CurrentControlSet\Services\BthPort\Parameters'

Write-Host '=== [1/5] enabling full HCI payload logging ==='
New-Item -Path $pkey -Force | Out-Null
Set-ItemProperty -Path $pkey -Name 'EtwLogSensitiveData' -Value 1      -Type DWord -Force
Set-ItemProperty -Path $pkey -Name 'MaxEtwBytes'         -Value 0x400  -Type DWord -Force
Set-ItemProperty -Path $pkey -Name 'EtwDropLargeEvents'  -Value 0      -Type DWord -Force

Write-Host '=== [2/5] restarting Bluetooth service (all links drop) ==='
Restart-Service bthserv -Force -ErrorAction SilentlyContinue
Start-Sleep 5

Remove-Item $etl, $xml -Force -ErrorAction SilentlyContinue

Write-Host '=== [3/5] trace STARTED - do your thing now ==='
netsh trace start capture=no tracefile=$etl report=no maxsize=256 overwrite=yes `
    provider=Microsoft-Windows-BTH-BTHPORT `
    provider=Microsoft-Windows-BTH-BTHUSB | Out-Null

for ($i = $WaitSeconds; $i -gt 0; $i--) {
    if ($i % 10 -eq 0) { Write-Host ("  capturing... {0}s left" -f $i) }
    Start-Sleep 1
}

Write-Host '=== [4/5] stopping trace ==='
netsh trace stop | Out-Null
Start-Sleep 2

Write-Host '=== [5/5] converting to XML ==='
tracerpt $etl -o $xml -of xml -y 2>&1 | Out-Null

if (Test-Path $xml) {
    $mb = [math]::Round((Get-Item $xml).Length / 1MB, 2)
    Write-Host "DONE: $xml ($mb MB)"
} else {
    Write-Host 'FAILED: tracerpt produced no xml'
}
