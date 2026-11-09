@echo off
rem LDAC A2DP driver - 用户态单测编译脚本
rem 用法: build_tests.bat "C:\path\to\vcvars64.bat"
setlocal
if "%~1"=="" (
    echo 用法: build_tests.bat "vcvars64.bat 的完整路径"
    exit /b 2
)
call "%~1" >nul
if errorlevel 1 (
    echo vcvars 初始化失败
    exit /b 3
)

cd /d "%~dp0"

cl /nologo /W4 /utf-8 /D_CRT_SECURE_NO_WARNINGS ^
   /I"%~dp0shim" /I"%~dp0..\src" ^
   "..\src\Avdtp.c" test_avdtp.c ^
   /Fe:test_avdtp.exe

exit /b %ERRORLEVEL%
