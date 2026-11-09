# ============================================================================
#  50-install-service.ps1 -- 把 LdacPanel.exe 注册成后台服务
# ============================================================================
#  服务是唯一拥有 ldac-send.exe 的地方：
#    - GUI 崩 / 关 / 注销都不影响声音
#    - 失败自动重启（sc failure）
#    - 开机自启（LocalSystem，不需要用户登录）
#
#  用法（管理员）：
#    .\scripts\50-install-service.ps1                 # 安装 + 启动
#    .\scripts\50-install-service.ps1 -Action stop
#    .\scripts\50-install-service.ps1 -Action uninstall
#    .\scripts\50-install-service.ps1 -Action status
# ============================================================================
param(
    [ValidateSet('install', 'uninstall', 'start', 'stop', 'status')]
    [string]$Action = 'install',
    [string]$ServiceName = 'LdacA2dpSvc',
    [string]$Exe = 'C:\Developer\ldac\panel\LdacPanel\bin\Debug\net10.0\LdacPanel.exe'
)

$ErrorActionPreference = 'Continue'

$id = [Security.Principal.WindowsIdentity]::GetCurrent()
if (-not (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw '必须用管理员权限运行'
}

function Test-Installed {
    $q = & sc.exe query $ServiceName 2>&1 | Out-String
    return ($q -match 'SERVICE_NAME')
}

switch ($Action) {
    'status' {
        & sc.exe query $ServiceName 2>&1 | Out-String
        & sc.exe qc $ServiceName 2>&1 | Out-String
    }

    'stop' {
        & sc.exe stop $ServiceName 2>&1 | Out-String
        Start-Sleep -Seconds 2
        Get-Process ldac-send -ErrorAction SilentlyContinue | Stop-Process -Force
    }

    'uninstall' {
        & sc.exe stop $ServiceName 2>&1 | Out-Null
        Start-Sleep -Seconds 2
        & sc.exe delete $ServiceName 2>&1 | Out-String
        Get-Process ldac-send -ErrorAction SilentlyContinue | Stop-Process -Force
    }

    'start' {
        & sc.exe start $ServiceName 2>&1 | Out-String
        Start-Sleep -Seconds 3
        Get-Process ldac-send -ErrorAction SilentlyContinue |
            Select-Object Id,StartTime | Format-Table -AutoSize | Out-String
    }

    'install' {
        if (-not (Test-Path $Exe)) { throw "找不到 $Exe（先 dotnet build）" }

        if (Test-Installed) {
            & sc.exe stop $ServiceName 2>&1 | Out-Null
            Start-Sleep -Seconds 2
            & sc.exe delete $ServiceName 2>&1 | Out-Null
            Start-Sleep -Seconds 2
        }
        # 服务被强杀时子进程会变孤儿：安装前必须清掉，否则新发送端被单实例互斥挡掉
        Get-Process ldac-send -ErrorAction SilentlyContinue | Stop-Process -Force
        Start-Sleep -Milliseconds 500

        $binPath = '"' + $Exe + '" --service'
        & sc.exe create $ServiceName binPath= $binPath start= auto obj= LocalSystem DisplayName= 'LDAC A2DP Service' 2>&1 | Out-String
        & sc.exe description $ServiceName 'LDAC A2DP background service: owns ldac-send.exe so the GUI can crash without stopping audio' 2>&1 | Out-Null
        & sc.exe failure $ServiceName reset= 86400 actions= restart/5000/restart/10000/restart/30000 2>&1 | Out-String

        & sc.exe start $ServiceName 2>&1 | Out-String
        Start-Sleep -Seconds 4

        '--- service ---'
        & sc.exe query $ServiceName 2>&1 | Out-String
        '--- sender child ---'
        Get-CimInstance Win32_Process -Filter "Name='ldac-send.exe'" |
            Select-Object ProcessId,ParentProcessId,CommandLine | Format-List | Out-String
        '--- panel.log ---'
        Get-Content 'C:\ProgramData\LdacPanel\panel.log' -Tail 6 -ErrorAction SilentlyContinue
    }
}
