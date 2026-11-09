# ============================================================================
#  vm-bt-cycle.ps1 -- 在客户机里自动开关蓝牙无线电（等价于设置里的开关）
# ============================================================================
#  用法： powershell -File C:\Users\Public\bt-cycle.ps1 -Cycles 5 -OffSeconds 4 -OnSeconds 12
#  输出： 每轮一行带时间戳，方便和驱动日志/dump 对照
# ============================================================================
param(
    [int]$Cycles = 5,
    [int]$OffSeconds = 4,
    [int]$OnSeconds = 12
)

$ErrorActionPreference = 'Continue'
Add-Type -AssemblyName System.Runtime.WindowsRuntime | Out-Null
$null = [Windows.Devices.Radios.Radio, Windows.Devices.Radios, ContentType = WindowsRuntime]
$null = [Windows.Devices.Radios.RadioState, Windows.Devices.Radios, ContentType = WindowsRuntime]
$null = [Windows.Devices.Radios.RadioAccessStatus, Windows.Devices.Radios, ContentType = WindowsRuntime]

$asTask = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
    $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and
    $_.GetParameters()[0].ParameterType.Name -like 'IAsyncOperation*' })[0]

function Await($op, $type, $ms) {
    $task = $asTask.MakeGenericMethod($type).Invoke($null, @($op))
    if (-not $task.Wait($ms)) { return $null }
    $task.Result
}

function Stamp([string]$msg) {
    (Get-Date).ToString('HH:mm:ss') + '  ' + $msg
}

$listType = [System.Collections.Generic.IReadOnlyList[Windows.Devices.Radios.Radio]]
$radios = Await ([Windows.Devices.Radios.Radio]::GetRadiosAsync()) $listType 10000
if ($null -eq $radios) { 'ERROR: GetRadiosAsync timeout'; exit 1 }

$bt = $radios | Where-Object { $_.Kind.ToString() -eq 'Bluetooth' } | Select-Object -First 1
if ($null -eq $bt) { 'ERROR: no Bluetooth radio found'; exit 1 }

Stamp ('radio: ' + $bt.Name + '  state=' + $bt.State) | Write-Output

for ($i = 1; $i -le $Cycles; $i++) {
    Stamp ("cycle $i/$Cycles : OFF") | Write-Output
    $r = Await ($bt.SetStateAsync([Windows.Devices.Radios.RadioState]::Off)) ([Windows.Devices.Radios.RadioAccessStatus]) 15000
    Stamp ('  off -> ' + $r + '  state=' + $bt.State) | Write-Output
    Start-Sleep -Seconds $OffSeconds

    Stamp ("cycle $i/$Cycles : ON") | Write-Output
    $r = Await ($bt.SetStateAsync([Windows.Devices.Radios.RadioState]::On)) ([Windows.Devices.Radios.RadioAccessStatus]) 15000
    Stamp ('  on  -> ' + $r + '  state=' + $bt.State) | Write-Output
    Start-Sleep -Seconds $OnSeconds
}
Stamp 'done' | Write-Output
