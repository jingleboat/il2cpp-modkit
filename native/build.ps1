# 构建 libmodkit.so（arm64-v8a，API 23）
#
# 前置：
#   1. 装 Android NDK（r26 以上），把路径填到 $ndk
#   2. 把 addresses.example.h 复制成 addresses.h，并按注释把地址填好
#
# 用法： pwsh -File build.ps1

$ErrorActionPreference = 'Stop'

# 改成本机的 NDK 路径，或者设环境变量 ANDROID_NDK_HOME
$ndk = if ($env:ANDROID_NDK_HOME) { $env:ANDROID_NDK_HOME } else { 'C:\Users\you\android-ndk-r27c' }
$cc = "$ndk\toolchains\llvm\prebuilt\windows-x86_64\bin\aarch64-linux-android23-clang.cmd"

Push-Location $PSScriptRoot
try {
    if (-not (Test-Path 'addresses.h')) {
        throw '缺少 addresses.h：先 copy addresses.example.h addresses.h，再按注释填入自己的地址'
    }
    Write-Host '--- libmodkit.so'
    & $cc -shared -fPIC -O2 -Wall -Wextra -o libmodkit.so `
        main.cpp trade.cpp ui.cpp hook.cpp -llog -lm `
        '-Wl,-soname,libmodkit.so' '-Wl,--build-id=none'
    if ($LASTEXITCODE) { throw 'clang failed' }

    Get-ChildItem libmodkit.so | Select-Object Name, Length | Format-Table -AutoSize
} finally {
    Pop-Location
}
