# 打包自签名 APK
#
# 签名信息从 keys/keystore.properties 读（该文件已 gitignore）
# 用法: .\build_apk.ps1

$ErrorActionPreference = "Stop"

$Root = "C:\Users\Assistant\projects\super-feiyu"
$GuiDir = Join-Path $Root "gui"
$Sdk = "C:\Users\Assistant\AppData\Local\Android\Sdk"
$Jdk = "C:\Program Files\Android\openjdk\jdk-21.0.8"
$Manifest = Join-Path $GuiDir "manifest.json"
$Props = Join-Path $Root "keys\keystore.properties"

Write-Host "=== 1. 环境" -ForegroundColor Cyan
$env:JAVA_HOME = $Jdk
$env:ANDROID_HOME = $Sdk
$env:ANDROID_SDK_ROOT = $Sdk
$env:FLET_PYTHON_BUILD_MANIFEST = $Manifest
$env:PATH = "$Jdk\bin;$env:PATH"
Write-Host "  JAVA_HOME=$env:JAVA_HOME"

Write-Host "`n=== 2. 读取签名配置" -ForegroundColor Cyan
if (-not (Test-Path $Props)) {
    Write-Host "  找不到 $Props" -ForegroundColor Red
    Write-Host "  先运行 .\make_keystore.ps1" -ForegroundColor Yellow
    exit 1
}

$sign = @{}
foreach ($line in Get-Content $Props) {
    if ($line -match '^\s*#' -or $line -match '^\s*$') { continue }
    $kv = $line -split '=', 2
    if ($kv.Count -eq 2) { $sign[$kv[0].Trim()] = $kv[1].Trim() }
}

$StoreFile = $sign["storeFile"] -replace '\\\\', '\'
foreach ($k in @("storeFile", "storePassword", "keyAlias", "keyPassword")) {
    if (-not $sign[$k]) {
        Write-Host "  keystore.properties 缺少 $k" -ForegroundColor Red
        exit 1
    }
}
if (-not (Test-Path $StoreFile)) {
    Write-Host "  keystore 不存在: $StoreFile" -ForegroundColor Red
    exit 1
}
Write-Host "  keystore: $StoreFile"
Write-Host "  alias:    $($sign['keyAlias'])"
Write-Host "  （密码不打印）"

Write-Host "`n=== 3. 准备 Python 构建清单" -ForegroundColor Cyan
if (-not (Test-Path $Manifest)) {
    $url = "https://github.com/flet-dev/python-build/releases/download/20261027/manifest.json"
    Invoke-WebRequest -Uri $url -OutFile $Manifest -TimeoutSec 60 -UseBasicParsing
}
Write-Host "  $((Get-Item $Manifest).Length) 字节"

Write-Host "`n=== 4. 打包（首次约 13 分钟）" -ForegroundColor Cyan
Push-Location $GuiDir
try {
    flet build apk `
        --arch arm64-v8a `
        --android-signing-key-store $StoreFile `
        --android-signing-key-store-password $sign["storePassword"] `
        --android-signing-key-alias $sign["keyAlias"] `
        --android-signing-key-password $sign["keyPassword"] `
        --verbose
    $rc = $LASTEXITCODE
} finally {
    Pop-Location
}

if ($rc -ne 0) {
    Write-Host "`n打包失败，退出码 $rc" -ForegroundColor Red
    exit $rc
}

Write-Host "`n=== 5. 产物" -ForegroundColor Green
$apkDir = Join-Path $GuiDir "build\apk"
$apk = Get-ChildItem $apkDir -Filter "*.apk" -ErrorAction SilentlyContinue | Select-Object -First 1
if ($apk) {
    Write-Host "  $($apk.FullName)"
    Write-Host "  $([math]::Round($apk.Length/1MB,1)) MB"

    Write-Host "`n=== 6. 签名验证" -ForegroundColor Cyan
    $apksigner = Join-Path $Sdk "build-tools\34.0.0\apksigner.bat"
    if (Test-Path $apksigner) {
        & $apksigner verify --print-certs $apk.FullName 2>&1 |
            Select-String -Pattern "Signer #1 certificate DN|SHA-256 digest" |
            Select-Object -First 2
    }

    $desktop = Join-Path ([Environment]::GetFolderPath("Desktop")) "super-feiyu-control.apk"
    Copy-Item $apk.FullName $desktop -Force
    Write-Host "`n  已复制到桌面: $desktop" -ForegroundColor Green
}
