#!/bin/sh
# eMMC健康扫描 构建脚本
# 1) 设备端: 宿主单测 + 三架构交叉编译 + ELF 校验
# 2) Windows 端: 拷贝资源 + dotnet 编译 + 单文件发布
set -e

ROOT="$(cd "$(dirname "$0")" && pwd)"
ZIG="${ZIG:-zig}"
ADB_SRC="${ADB_SRC:-host/resource/adb}"
cd "$ROOT"

echo "== [1/4] 设备端宿主单测 =="
"$ZIG" cc -O2 -DEMHC_HOST_TEST device/emmc_health.c -o build/emmc_test.exe
./build/emmc_test.exe

echo "== [2/4] 设备端交叉编译 (v7a / v8a / x64检查) =="
"$ZIG" cc -target aarch64-linux-musl     -O2 -static device/emmc_health.c -o build/emmc_health_v8a
"$ZIG" cc -target arm-linux-musleabihf   -O2 -static device/emmc_health.c -o build/emmc_health_v7a
"$ZIG" cc -target x86_64-linux-musl      -O2 -static device/emmc_health.c -o build/emmc_check_x64

# ELF 头校验：字节 18-19 必须是目标机器码
V8A_EM=$(head -c 20 build/emmc_health_v8a | od -An -tx1 | tr -d ' \n' | cut -c37-40)
V7A_EM=$(head -c 20 build/emmc_health_v7a | od -An -tx1 | tr -d ' \n' | cut -c37-40)
echo "v8a e_machine=$V8A_EM (期望 b700)  v7a e_machine=$V7A_EM (期望 2800)"
[ "$V8A_EM" = "b700" ] || { echo "FATAL: v8a ELF 头错误"; exit 1; }
[ "$V7A_EM" = "2800" ] || { echo "FATAL: v7a ELF 头错误"; exit 1; }

echo "== [3/4] 拷贝嵌入资源 =="
mkdir -p host/resource/adb host/resource/device
cp -f "$ADB_SRC/adb.exe" "$ADB_SRC/AdbWinApi.dll" "$ADB_SRC/AdbWinUsbApi.dll" host/resource/adb/ 2>/dev/null || true
cp -f build/emmc_health_v7a host/resource/device/emmc_health_v7a
cp -f build/emmc_health_v8a host/resource/device/emmc_health_v8a
ls -la host/resource/adb host/resource/device

echo "== [4/4] dotnet 编译 + 单文件发布 =="
cd host
dotnet build -c Release -v q --nologo
OUT="../build/publish"
dotnet publish -c Release -r win-x64 --self-contained true \
  -p:PublishSingleFile=true -p:IncludeNativeLibrariesForSelfExtract=true \
  -o "$OUT" -v q --nologo
cp -f "$OUT/EmmcScan.exe" "$ROOT/build/eMMC健康扫描.exe"
ls -la "$ROOT/build/eMMC健康扫描.exe"
echo "== 构建完成 =="
