# bt-capture-test.ps1 - one-shot: enable HCI payload logging + capture + ldac-send + stop + convert
# Run elevated (gsudo). Output in log\bt-hci.etl / bt-hci.xml
$ErrorActionPreference = 'Continue'
$root = 'C:\Developer\ldac'
$etl  = "$root\log\bt-hci.etl"
$xml  = "$root\log\bt-hci.xml"
$pkey = 'HKLM:\SYSTEM\CurrentControlSet\Services\BthPort\Parameters'

Write-Host "[0/6] enabling full HCI payload logging..."
New-Item -Path $pkey -Force | Out-Null
Set-ItemProperty -Path $pkey -Name 'EtwLogSensitiveData' -Value 1 -Type DWord -Force
Set-ItemProperty -Path $pkey -Name 'MaxEtwBytes'        -Value 0x400 -Type DWord -Force
Set-ItemProperty -Path $pkey -Name 'EtwDropLargeEvents' -Value 0 -Type DWord -Force
Restart-Service bthserv -Force -ErrorAction SilentlyContinue
Start-Sleep 3

Write-Host "[0.5/6] installing driver package..."
pnputil /add-driver "$root\bin-a2dp\x64\Debug\LdacA2dp\LdacA2dp.inf" /install | Select-Object -Last 3

Write-Host "[1/5] starting BT HCI trace..."
netsh trace start capture=no tracefile=$etl report=no `
    provider=Microsoft-Windows-BTH-BTHPORT `
    provider=Microsoft-Windows-BTH-BTHUSB | Out-Null

Write-Host "[2/5] running ldac-send --loopback (fails on purpose)..."
& "$root\bin-host\Debug\ldac-send.exe" --loopback 2>&1 | Out-String | Write-Host

Write-Host "[3/5] stopping trace..."
netsh trace stop | Out-Null

Write-Host "[4/5] converting to XML..."
tracerpt $etl -o $xml -of xml 2>&1 | Out-Null

Write-Host "[5/5] kernel log tail:"
Get-Content C:\Users\Public\ldac-probe.log -Tail 12
Write-Host "done: $xml"
