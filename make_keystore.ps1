# 生成自签名 keystore（Windows JDK）
#
# 密码写入本地 keystore.properties，该文件已加入 .gitignore
# 用法: .\make_keystore.ps1

$ErrorActionPreference = "Stop"

$KeyDir = "C:\Users\Assistant\projects\super-feiyu\keys"
$KeyStore = Join-Path $KeyDir "release.keystore"
$Props = Join-Path $KeyDir "keystore.properties"
$Keytool = "C:\Program Files\Android\openjdk\jdk-21.0.8\bin\keytool.exe"

if (-not (Test-Path $Keytool)) {
    # 退化到 PATH 里的 keytool
    $Keytool = (Get-Command keytool -ErrorAction SilentlyContinue).Source
}
if (-not $Keytool) {
    Write-Host "找不到 keytool" -ForegroundColor Red
    exit 1
}

New-Item -ItemType Directory -Force -Path $KeyDir | Out-Null

if (Test-Path $KeyStore) {
    Write-Host "keystore 已存在: $KeyStore"
    Write-Host "要重新生成请先删除它"
    exit 0
}

# 生成 32 位随机密码（只含字母数字）
$bytes = New-Object byte[] 32
[System.Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
$Pass = -join ($bytes | ForEach-Object { "abcdefghijkmnpqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789"[$_ % 56] })

Write-Host "=== 生成 keystore" -ForegroundColor Cyan
$dname = "CN=Sonder-Traveller, OU=super-feiyu, O=Sonder, L=Unknown, ST=Unknown, C=CN"
& $Keytool -genkeypair `
    -keystore $KeyStore `
    -alias super-feiyu `
    -keyalg RSA `
    -keysize 4096 `
    -validity 10000 `
    -storepass $Pass `
    -keypass $Pass `
    -dname $dname 2>&1 | Select-Object -First 3

Write-Host "`n=== 写入 keystore.properties" -ForegroundColor Cyan
$content = @"
# APK 签名配置（本地文件，已 gitignore，不要提交）
storeFile=$($KeyStore -replace '\\','\\')
storePassword=$Pass
keyAlias=super-feiyu
keyPassword=$Pass
"@
Set-Content -Path $Props -Value $content -Encoding UTF8

# 权限：只允许当前用户
icacls $Props /inheritance:r /grant:r "$($env:USERNAME):(R,W)" 2>&1 | Out-Null
icacls $KeyStore /inheritance:r /grant:r "$($env:USERNAME):(R,W)" 2>&1 | Out-Null

Write-Host "  $Props"
Write-Host "  $KeyStore"

Write-Host "`n=== 验证" -ForegroundColor Cyan
& $Keytool -list -keystore $KeyStore -storepass $Pass 2>&1 |
    Select-String -Pattern "Keystore type|您的密钥库|super-feiyu" |
    Select-Object -First 3

Write-Host "`n=== 密码（同时存于 keystore.properties）" -ForegroundColor Yellow
Write-Host "  $Pass"
