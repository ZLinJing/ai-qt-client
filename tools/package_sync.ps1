# ===================================================================
# package_sync.ps1 —— 把新包同步进 dist\（package.bat 第 8 步）
#   参数：-Source <暂存目录>  -Target <dist 目录>
#   契约（package.bat 注释同款）：
#     · 同步只换二进制（旧 exe 先删除再放入新的），**保留运行时数据**
#       （daban_quant.db / backups\），否则升级会把交易日志一起冲掉；
#     · Target 独有的文件一律不动（保守同步，不做镜像删除）；
#     · 旧 exe 被占用（程序正在运行）时立即失败 —— 正在运行的旧版
#       不能被就地升级，宁可让本次打包失败，也不留半新半旧的部署目录。
# ===================================================================
param(
    [Parameter(Mandatory = $true)][string]$Source,
    [Parameter(Mandatory = $true)][string]$Target
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path $Source)) {
    Write-Output "[ERROR] staging source missing: $Source"
    exit 1
}
if (-not (Test-Path $Target)) {
    New-Item -ItemType Directory -Path $Target -Force | Out-Null
}

# ---- 1. 删除旧的 exe（再由下面第 2 步放入新 exe）----
$oldExe = Join-Path $Target 'DabanQuant.exe'
if (Test-Path $oldExe) {
    try { Remove-Item $oldExe -Force }
    catch {
        Write-Output "[ERROR] old DabanQuant.exe is locked - is the app still running?"
        Write-Output "        $($_.Exception.Message)"
        exit 1
    }
}

# ---- 2. 顶层文件：新二进制 / 配置样例覆盖，运行时数据不覆盖 ----
$protected = @('daban_quant.db')
foreach ($f in (Get-ChildItem $Source -File)) {
    if ($protected -contains $f.Name) { continue }
    Copy-Item $f.FullName -Destination (Join-Path $Target $f.Name) -Force
}

# ---- 3. 子目录（platforms / sqldrivers / styles / tls / imageformats ...）：内容级覆盖 ----
foreach ($d in (Get-ChildItem $Source -Directory)) {
    $dst = Join-Path $Target $d.Name
    if (-not (Test-Path $dst)) { New-Item -ItemType Directory -Path $dst -Force | Out-Null }
    Copy-Item (Join-Path $d.FullName '*') -Destination $dst -Recurse -Force
}

if (-not (Test-Path (Join-Path $Target 'DabanQuant.exe'))) {
    Write-Output "[ERROR] sync finished but DabanQuant.exe missing in target"
    exit 1
}

Write-Output ("sync ok: {0} -> {1} (old exe removed, runtime data kept)" -f $Source, $Target)
exit 0
