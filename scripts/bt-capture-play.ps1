# bt-capture-play.ps1 - capture a LIVE A2DP session WITHOUT touching the link.
#
# Unlike bt-capture-pairing.ps1 this does NOT restart bthserv - the headset stays
# connected, so whatever is already working keeps working. Just play music during
# the window and we get the full AVDTP negotiation + media stream.
param([int]$WaitSeconds = 60)

$root = 'C:\Developer\ldac'
$etl  = "$root\log\bt-play.etl"
$xml  = "$root\log\bt-play.xml"
$pkey = 'HKLM:\SYSTEM\CurrentControlSet\Services\BthPort\Parameters'

Write-Host '=== [1/4] HCI payload logging (best effort, no service restart) ==='
try {
    New-Item -Path $pkey -Force -ErrorAction Stop | Out-Null
    Set-ItemProperty -Path $pkey -Name 'EtwLogSensitiveData' -Value 1     -Type DWord -Force -ErrorAction Stop
    Set-ItemProperty -Path $pkey -Name 'MaxEtwBytes'         -Value 0x400 -Type DWord -Force -ErrorAction Stop
    Set-ItemProperty -Path $pkey -Name 'EtwDropLargeEvents'  -Value 0      -Type DWord -Force -ErrorAction Stop
    Write-Host '  reg knobs set'
} catch {
    Write-Host "  reg knob skipped ($($_.Exception.Message)) - previous value still applies"
}

Remove-Item $etl, $xml -Force -ErrorAction SilentlyContinue

Write-Host '=== [2/4] TRACE STARTED - play music NOW (link is untouched) ==='
netsh trace start capture=no tracefile=$etl report=no maxsize=512 overwrite=yes `
    provider=Microsoft-Windows-BTH-BTHPORT `
    provider=Microsoft-Windows-BTH-BTHUSB | Out-Null

for ($i = $WaitSeconds; $i -gt 0; $i--) {
    if ($i % 10 -eq 0) { Write-Host ("  {0}s left" -f $i) }
    Start-Sleep 1
}

Write-Host '=== [3/4] stop + convert ==='
netsh trace stop | Out-Null
Start-Sleep 2
tracerpt $etl -o $xml -of xml -y 2>&1 | Out-Null

if (Test-Path $xml) {
    Write-Host ("DONE: {0} ({1} MB)" -f $xml, [math]::Round((Get-Item $xml).Length / 1MB, 2))
} else {
    Write-Host 'FAILED: no xml'
}
