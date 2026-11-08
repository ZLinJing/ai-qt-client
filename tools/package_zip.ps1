# ===================================================================
# package_zip.ps1 —— 把暂存目录压成发行 zip（package.bat 第 7 步）
#   参数：-Source <暂存目录>  -Target <zip 路径>
#   说明：独立成脚本是因为 cmd 里内联 "powershell -Command ..." 的引号
#         解析太脆弱（package.bat 注释同款原因）；同名旧 zip 先删再压，
#         避免 Compress-Archive 对已存在目标的追加语义。
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

if (Test-Path $Target) { Remove-Item $Target -Force }

Compress-Archive -Path (Join-Path $Source '*') -DestinationPath $Target `
    -CompressionLevel Optimal -Force

if (-not (Test-Path $Target)) {
    Write-Output "[ERROR] zip was not created: $Target"
    exit 1
}

Write-Output ("zip ok : {0} ({1} bytes)" -f $Target, (Get-Item $Target).Length)
exit 0
