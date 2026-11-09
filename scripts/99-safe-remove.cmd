@echo off
rem ===========================================================================
rem  99-safe-remove.cmd -- 安全模式下一键删掉 LDAC 驱动
rem  双击运行；不需要仓库、不需要网络、不需要蓝牙
rem ===========================================================================
setlocal
title LDAC driver safe-mode removal
echo.
echo Removing the LDAC A2DP driver. Safe mode is fine.
echo.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp099-safe-remove.ps1"
echo.
echo Done. Reboot normally when ready.
pause
