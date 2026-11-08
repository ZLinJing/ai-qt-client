@echo off
REM ===================================================================
REM  Toolchain detection shared by build.bat / package.bat
REM  - Assigns variables only (no setlocal) so the caller inherits them
REM  - Never calls vcvars64.bat: it depends on reg.exe, which is blocked
REM    by application-control policy in restricted / sandboxed shells
REM  - Every path can be overridden from the environment:
REM      QTDIR / VSINSTALLDIR / VCVER / SDKVER / MYSQLDIR
REM  - 只负责赋值，不 setlocal；QTDIR 缺失视为致命错误（exit /b 1），
REM    MSVC / SDK / MySQL 缺失只告警，由调用方决定是否必需
REM ===================================================================

REM 64-bit Windows only (Qt msvc2022_64)
set "PF86=%ProgramFiles(x86)%"

REM ------------------------------ Qt ------------------------------
if defined QTDIR if exist "%QTDIR%\bin\windeployqt.exe" goto :qt_ready
set "QTDIR="
if defined CMAKE_PREFIX_PATH if exist "%CMAKE_PREFIX_PATH%\bin\windeployqt.exe" set "QTDIR=%CMAKE_PREFIX_PATH%"
if defined QTDIR goto :qt_ready
REM 常见安装位置：<drive>:\Qt\6.x\msvc20xx_64
REM 注意：FindFirstFile 只支持最后一段的 * 通配，路径中间的通配必须逐层 for /d 展开
for %%r in ("%SystemDrive%\Qt" "D:\Qt" "E:\Qt") do if exist "%%~r" for /d %%v in ("%%~r\6.*") do for /d %%a in ("%%~v\msvc*_64") do if exist "%%~a\bin\windeployqt.exe" set "QTDIR=%%~a"
:qt_ready
if not defined QTDIR goto :err_qt
if not exist "%QTDIR%\bin\windeployqt.exe" goto :err_qt

REM ------------------------- Visual Studio -------------------------
set "VSROOT="
if defined VSINSTALLDIR if exist "%VSINSTALLDIR%\VC\Tools\MSVC" set "VSROOT=%VSINSTALLDIR%"
if defined VSROOT goto :vs_ready
REM 从「开发人员命令提示」里调用时可直接复用 VCINSTALLDIR（<VS>\VC\）
if defined VCINSTALLDIR for %%p in ("%VCINSTALLDIR%..") do if exist "%%~fp\VC\Tools\MSVC" set "VSROOT=%%~fp"
if defined VSROOT goto :vs_ready

set "VSWHERE=%PF86%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :vs_glob
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
:vs_glob
if defined VSROOT goto :vs_ready
for /d %%d in ("%PF86%\Microsoft Visual Studio\2022\*" "%ProgramFiles%\Microsoft Visual Studio\2022\*") do if exist "%%~d\VC\Tools\MSVC" set "VSROOT=%%~d"
:vs_ready

REM 取该 VS 安装下最新的 MSVC 工具集（可用 VCVER 指定）
if not defined VSROOT goto :msvc_missing
if not defined VCVER for /d %%v in ("%VSROOT%\VC\Tools\MSVC\14.*") do set "VCVER=%%~nxv"
if defined VCVER set "VCDIR=%VSROOT%\VC\Tools\MSVC\%VCVER%"
if not defined VCDIR goto :msvc_missing
if not exist "%VCDIR%\bin\Hostx64\x64\cl.exe" goto :msvc_missing
goto :msvc_ready

:msvc_missing
set "VCDIR="
set "VCVER="
echo [warn] MSVC toolchain not found - set VSINSTALLDIR (VS 2022 with C++ workload) to build
:msvc_ready

REM --------------------------- Windows SDK -------------------------
set "SDKDIR=%PF86%\Windows Kits\10"
if defined SDKVER goto :sdk_ready
set "SDKVER="
for /d %%s in ("%SDKDIR%\Include\10.*") do set "SDKVER=%%~nxs"
:sdk_ready
if not defined SDKVER echo [warn] Windows SDK 10 not found under "%SDKDIR%"
if defined SDKVER if not exist "%SDKDIR%\Include\%SDKVER%\um\windows.h" set "SDKVER="

REM ------------------------ MySQL Connector/C ----------------------
if defined MYSQLDIR goto :mysql_ready
for /d %%d in ("%ProgramFiles%\MySQL\MySQL Server *") do if exist "%%~d\lib\libmysql.dll" set "MYSQLDIR=%%~d\lib"
:mysql_ready

echo [env] Qt          : %QTDIR%
if defined VCDIR     echo [env] MSVC        : %VCDIR%
if defined SDKVER    echo [env] Windows SDK : %SDKDIR%\Include\%SDKVER%
if defined MYSQLDIR  echo [env] MySQL lib   : %MYSQLDIR%
exit /b 0

:err_qt
echo [ERROR] Qt 6 not found.
echo         Install Qt 6 (msvc2022_64) and set QTDIR, e.g.:
echo             set QTDIR=D:\Qt\6.8.3\msvc2022_64
exit /b 1
