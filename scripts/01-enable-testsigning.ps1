# 开启测试签名（需管理员 + 重启）
# 本机实测：Secure Boot 已关闭（UEFISecureBootEnabled=0）、无 TPM、BitLocker 未启用
# → 不需要进 UEFI 改设置，也不会有恢复密钥风险

$ErrorActionPreference = 'Stop'

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) { throw "需要管理员权限运行：右键 PowerShell → 以管理员身份运行" }

Write-Host "== 当前状态 ==" -ForegroundColor Cyan
$sb = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\SecureBoot\State' -ErrorAction SilentlyContinue).UEFISecureBootEnabled
Write-Host "Secure Boot enabled = $sb  (0 = 已关闭，good)"
bcdedit /enum '{current}' | Select-String -Pattern 'testsigning|nointegritychecks|hypervisorlaunchtype'

Write-Host "`n== 开启 testsigning ==" -ForegroundColor Cyan
bcdedit /set testsigning on
bcdedit /set nointegritychecks off

Write-Host "`n== 结果 ==" -ForegroundColor Cyan
bcdedit /enum '{current}' | Select-String -Pattern 'testsigning'
Write-Host "需要重启生效。重启后桌面右下角会出现「测试模式」水印，这是正常的。" -ForegroundColor Yellow
Write-Host "退出测试模式：bcdedit /set testsigning off 然后重启。" -ForegroundColor Yellow
