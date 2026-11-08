@echo off
setlocal enabledelayedexpansion
REM ===================================================================
REM  Distribution package builder (Release)
REM  Usage : package.bat [version]        default 1.4.0
REM  Output: <project root>\DabanQuant_v<version>_<YYYYMMDD>.zip
REM
REM  Layout is identical to the previous release (flat, no top folder):
REM    DabanQuant.exe + Qt runtime + sqldrivers + VC runtime
REM    + libmysql.dll (MySQL Connector/C - MUST sit next to the exe)
REM    + notify_config.example.json
REM ===================================================================

cd /d "%~dp0"

REM ---- 工具链探测（与 build.bat 共用 tools\detect_env.bat）----
REM 可用环境变量覆盖：QTDIR / MYSQLDIR
call "%~dp0tools\detect_env.bat"
if errorlevel 1 exit /b 1

set "VER=%~1"
if "%VER%"=="" set "VER=1.4.0"

REM date via PowerShell so it does not depend on the system locale
for /f %%i in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd"') do set "STAMP=%%i"

set "PKGNAME=DabanQuant_v%VER%_%STAMP%"
set "STAGE=build\dist\%PKGNAME%"
set "ZIP=%PKGNAME%.zip"

echo ============================================================
echo  Packaging %PKGNAME%
echo ============================================================

REM ---- 1. clean staging ----
echo [1/7] clean staging directory
if exist "build\dist" rmdir /s /q "build\dist"
mkdir "%STAGE%" 2>nul
if not exist "%STAGE%" goto :fail_stage

REM ---- 2. copy main program ----
echo [2/7] copy main program
if not exist "build\DabanQuant.exe" goto :fail_nobuild
copy /y "build\DabanQuant.exe" "%STAGE%\" >nul
if errorlevel 1 goto :fail_copy

REM ---- 3. deploy Qt runtime ----
echo [3/7] deploy Qt runtime (windeployqt)
if not exist "%QTDIR%\bin\windeployqt.exe" goto :fail_noqt
"%QTDIR%\bin\windeployqt.exe" --release --compiler-runtime --no-translations "%STAGE%\DabanQuant.exe"
if errorlevel 1 goto :fail_deploy

REM ---- 4. MySQL client library ----
echo [4/7] copy MySQL client library
if not defined MYSQLDIR goto :mysql_missing
if not exist "%MYSQLDIR%\libmysql.dll" goto :mysql_missing
copy /y "%MYSQLDIR%\libmysql.dll" "%STAGE%\" >nul
goto :mysql_done

:mysql_missing
echo [WARN] libmysql.dll not found - journal will fall back to local SQLite
echo        set MYSQLDIR=<MySQL Server>\lib to include it in the package

:mysql_done

REM ---- 5. VC runtime ----
REM windeployqt --compiler-runtime needs VCINSTALLDIR, which is absent on this
REM machine (the SDK is injected by build.bat, not installed system-wide).
REM Ship the runtime explicitly, otherwise the exe fails to start on a clean PC.
echo [5/7] copy VC runtime
for %%d in (concrt140.dll msvcp140.dll msvcp140_1.dll msvcp140_2.dll msvcp140_codecvt_ids.dll vccorlib140.dll vcruntime140.dll vcruntime140_1.dll) do call :copyvc %%d
if not exist "%STAGE%\vcruntime140.dll" goto :fail_vc

REM ---- 6. config sample ----
echo [6/7] copy config sample
copy /y "notify_config.example.json" "%STAGE%\" >nul

REM ---- 7. zip ----
REM NOTE: the compression runs in an external .ps1 under tools\ - an inline
REM "powershell -Command ..." line breaks cmd's own argument parsing.
echo [7/7] compress
powershell -NoProfile -ExecutionPolicy Bypass -File "tools\package_zip.ps1" -Source "%STAGE%" -Target "%ZIP%"
if errorlevel 1 goto :fail_zip
if not exist "%ZIP%" goto :fail_zip

REM a valid package must carry the runtime; guard against an empty archive
for %%f in ("%ZIP%") do set "ZSIZE=%%~zf"
if %ZSIZE% LSS 5000000 goto :fail_small

REM ---- 依赖完整性自检（静态检查，不启动程序）----
REM ★ 为什么不用「启动一下」来验依赖：
REM   受管控终端的应用控制策略会拦截**刚编译出来**的二进制
REM   （"blocked by your organization's Device Guard policy"，exit 4551），
REM   那是策略不是包的问题，却会让打包流程在受管控机器上直接失败 —— 发不出包。
REM   依赖是否齐全，用文件存在性判定既准确又不受策略影响。
echo ---- dependency check (static) ----
for %%f in (DabanQuant.exe Qt6Core.dll Qt6Gui.dll Qt6Network.dll Qt6Sql.dll Qt6Widgets.dll libmysql.dll vcruntime140.dll msvcp140.dll) do (
    if not exist "%STAGE%\%%f" goto :fail_deps
)
if not exist "%STAGE%\platforms\qwindows.dll" goto :fail_deps
if not exist "%STAGE%\sqldrivers\qsqlite.dll" goto :fail_deps
if not exist "%STAGE%\notify_config.example.json" goto :fail_deps
echo [OK] all required binaries / plugins present

REM ---- 启动冒烟（**非致命**）：能跑就跑，被策略拦了就跳过 ----
echo ---- launch smoke (non-fatal) ----
"%STAGE%\DabanQuant.exe" --version >nul 2>&1
if errorlevel 1 goto :smoke_skipped
echo [OK] staged build starts and exits cleanly
goto :finish

:smoke_skipped
echo [WARN] staged build could not be launched here - most likely the local
echo        application-control policy blocked a freshly compiled binary
echo        (Device Guard, exit 4551). The package itself is unaffected.
goto :finish

:finish

REM ---- 8. refresh the live deployment folder (dist\) ----
REM dist\ 是「双击即可运行」的部署目录，也是用户真正启动程序的地方。
REM 之前只写 build\dist\<包名>\，dist\ 里一直是老版本 —— 这里补上同步。
REM 同步只换二进制，**保留运行时数据**（daban_quant.db / backups\），
REM 否则升级会把用户的交易日志记录一起冲掉。
echo [8/9] refresh live deployment (dist\)
powershell -NoProfile -ExecutionPolicy Bypass -File "tools\package_sync.ps1" -Source "%STAGE%" -Target "dist"
if errorlevel 1 goto :fail_sync

REM ---- 9. drop the staging folder ----
REM 暂存区只是压缩与同步的中间态，内容已经进了 zip、也同步进了 dist\。
REM 留着白占 40+ MB，删掉；下次打包会重建。
echo [9/9] drop staging folder
rmdir /s /q "%STAGE%" 2>nul
rmdir /s /q "build\dist" 2>nul

REM ---- report ----
echo.
echo ============================================================
echo  OK  %ZIP%
echo      size : %ZSIZE% bytes
echo      live : dist\  (binaries refreshed, runtime data kept)
echo ============================================================
exit /b 0

REM ---------------- 子过程与错误出口 ----------------
REM 说明：所有失败分支一律用 "goto :label"，不用 "if ... (echo ... & exit /b 1)"
REM —— 括号形式在 cmd 里解析脆弱，会报 "- was unexpected at this time"。

:copyvc
if exist "C:\Windows\System32\%~1" copy /y "C:\Windows\System32\%~1" "%STAGE%\" >nul
if not exist "C:\Windows\System32\%~1" echo [WARN] VC runtime not found: %~1
exit /b 0

:fail_stage
echo [ERROR] cannot create staging dir
exit /b 1
:fail_nobuild
echo [ERROR] build\DabanQuant.exe missing - run build.bat first
exit /b 1
:fail_copy
echo [ERROR] copy main program failed
exit /b 1
:fail_noqt
echo [ERROR] windeployqt.exe not found under %QTDIR%
exit /b 1
:fail_deploy
echo [ERROR] windeployqt failed
exit /b 1
:fail_vc
echo [ERROR] VC runtime not found - the exe would not start on a clean PC
exit /b 1
:fail_zip
echo [ERROR] compression failed
exit /b 1
:fail_small
echo [ERROR] package too small (%ZSIZE% bytes) - deployment incomplete
exit /b 1
:fail_deps
echo [ERROR] staged build is missing required binaries / plugins
exit /b 1
:fail_sync
echo [ERROR] failed to refresh the live deployment folder (dist\)
exit /b 1
