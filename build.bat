@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"

REM ===================================================================
REM  One-click build (Release, Ninja)
REM  Usage:
REM    build.bat                                  build everything
REM    build.bat --check                          print detected toolchain only
REM    build.bat -DDABAN_ENABLE_MYSQL_JOURNAL=OFF extra args are passed to cmake
REM
REM  工具链路径由 tools\detect_env.bat 自动探测，可用环境变量覆盖：
REM    QTDIR / VSINSTALLDIR / VCVER / SDKVER / MYSQLDIR
REM  不调用 vcvars64.bat：它依赖 reg.exe，受限/沙箱环境会被应用控制策略拦截
REM ===================================================================

call "%~dp0tools\detect_env.bat"
if errorlevel 1 goto :fail_env
if not defined VCDIR goto :fail_msvc
if not defined SDKVER goto :fail_sdk

REM ---- 手动注入 MSVC + Windows SDK 环境 ----
set "PATH=%VCDIR%\bin\Hostx64\x64;%SDKDIR%\bin\%SDKVER%\x64;%QTDIR%\bin;%PATH%"
set "INCLUDE=%VCDIR%\include;%SDKDIR%\Include\%SDKVER%\ucrt;%SDKDIR%\Include\%SDKVER%\um;%SDKDIR%\Include\%SDKVER%\shared;%SDKDIR%\Include\%SDKVER%\winrt"
set "LIB=%VCDIR%\lib\x64;%SDKDIR%\Lib\%SDKVER%\ucrt\x64;%SDKDIR%\Lib\%SDKVER%\um\x64"
set "LIBPATH=%VCDIR%\lib\x64;%SDKDIR%\Lib\%SDKVER%\ucrt\x64;%SDKDIR%\Lib\%SDKVER%\um\x64"
set "CMAKE_PREFIX_PATH=%QTDIR%"
REM 交给 cmake -D 的路径必须用正斜杠：缓存文件里反斜杠会被当成转义符
set "RC_EXE=%SDKDIR%\bin\%SDKVER%\x64\rc.exe"
set "MT_EXE=%SDKDIR%\bin\%SDKVER%\x64\mt.exe"
set "RC_EXE=!RC_EXE:\=/!"
set "MT_EXE=!MT_EXE:\=/!"
set "QT_PREFIX=!QTDIR:\=/!"
set "CMAKE_MT=%MT_EXE%"
set "CMAKE_RC=%RC_EXE%"

REM ---- 构建工具链探测：优先使用 VS 自带的 CMake/Ninja（与 cl 同源，版本最稳） ----
REM 注意：绝不能把「可能不存在」的目录塞进 PATH，否则 CMakeCache 会记住无效路径，
REM 后续 configure 直接报 "ninja.exe --version failed"。
set "VSCMAKE=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
set "VSNINJA=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
if exist "%VSCMAKE%\cmake.exe" set "PATH=%VSCMAKE%;%PATH%"
if exist "%VSNINJA%\ninja.exe" set "PATH=%VSNINJA%;%PATH%"

if /i "%~1"=="--check" goto :summary

where cmake >nul 2>nul
if errorlevel 1 goto :fail_cmake
where ninja >nul 2>nul
if errorlevel 1 goto :fail_ninja
where cl >nul 2>nul || goto :fail_msvc

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=cl -DCMAKE_PREFIX_PATH="!QT_PREFIX!" -DCMAKE_RC_COMPILER="!RC_EXE!" -DCMAKE_MT="!MT_EXE!" %*
if errorlevel 1 exit /b 1
cmake --build build --parallel
if errorlevel 1 exit /b 1

REM ---- 部署 Qt 运行时到 build\ ----
REM 没有这一步，build\DabanQuant.exe 在全新检出后**启动不了**（动态链接找不到
REM Qt6Core.dll —— EXE 同目录找不到时才会去 PATH 找）。
REM 部署后 build\ 下的三个 exe 都能直接双击运行，省掉"先配 PATH"的隐式依赖。
REM 说明：--compiler-runtime 依赖 VCINSTALLDIR，本脚本不设该变量，
REM 缺 VC 运行库时的分发由 package.bat 显式拷贝，故此处不用该参数。
echo ---- deploy Qt runtime into build\ ----
windeployqt --release --no-translations build\DabanQuant.exe
if errorlevel 1 goto :fail_deploy
exit /b 0

:summary
echo ---- toolchain ----
echo Qt        : %QTDIR%
if defined VCDIR echo MSVC      : %VCDIR%
if defined SDKVER echo Win SDK   : %SDKDIR%\Include\%SDKVER%
for /f "delims=" %%c in ('where cmake 2^>nul') do echo CMake     : %%c
for /f "delims=" %%n in ('where ninja 2^>nul') do echo Ninja     : %%n
exit /b 0

:fail_env
echo [ERROR] required toolchain (Qt 6) not found - see message above
exit /b 1
:fail_msvc
echo [ERROR] MSVC (cl.exe) not found - install VS 2022 "Desktop development with C++"
echo         or set VSINSTALLDIR to the Visual Studio installation directory
exit /b 1
:fail_sdk
echo [ERROR] Windows 10 SDK not found - install it from the VS installer
echo         or set SDKVER (e.g. 10.0.26100.0)
exit /b 1
:fail_cmake
echo [ERROR] cmake not found in PATH
exit /b 1
:fail_ninja
echo [ERROR] ninja not found in PATH (ships with VS: CMake\Ninja)
exit /b 1
:fail_deploy
echo [ERROR] windeployqt failed - build\DabanQuant.exe may not start without Qt DLLs
exit /b 1
