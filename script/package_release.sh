#!/usr/bin/env bash
# 本地打包 ESP32-C6 固件到 release/（full / update / ota / littlefs / elf + SHA256SUMS）
#
# 用法：
#   ./script/package_release.sh
#   OTA_GITHUB_REPO=<owner>/<repo> ./script/package_release.sh   # 指定在线升级仓库
set -euo pipefail
cd "$(dirname "$0")/.."

ENV_NAME="${ENV_NAME:-esp32c6_supermini}"
if [ -d /root/DSH/.local/lib ]; then HOME=/root/DSH; fi
export HOME
export PLATFORMIO_CORE_DIR="${PLATFORMIO_CORE_DIR:-$HOME/.platformio}"
export PLATFORMIO_PENV_DIR="${PLATFORMIO_PENV_DIR:-$PLATFORMIO_CORE_DIR/penv}"
export PATH="$HOME/.local/bin:$PATH"
export CI=1

PIO="python3 -m platformio"
BUILD_DIR=".pio/build/$ENV_NAME"
ESPTOOL="${ESPTOOL:-$PLATFORMIO_CORE_DIR/packages/tool-esptool/esptool}"

echo "[package] clean + build firmware ($ENV_NAME)"
# 先清理，确保 OTA_GITHUB_REPO 等编译宏变更一定生效
$PIO run -e "$ENV_NAME" -t clean >/dev/null
$PIO run -e "$ENV_NAME"

echo "[package] build LittleFS image"
python3 script/compress_data.py
$PIO run -e "$ENV_NAME" -t buildfs
python3 script/restore_data.py

mkdir -p release
echo "[package] merge binaries"
"$ESPTOOL" --chip esp32c6 merge_bin --flash-mode keep --flash-freq keep --flash-size 4MB \
  -o "$BUILD_DIR/full.bin" \
  0x0 "$BUILD_DIR/bootloader.bin" \
  0x8000 "$BUILD_DIR/partitions.bin" \
  0x10000 "$BUILD_DIR/firmware.bin" \
  0x3B0000 "$BUILD_DIR/littlefs.bin" >/dev/null

"$ESPTOOL" --chip esp32c6 merge_bin --flash-mode keep --flash-freq keep --flash-size 4MB \
  --target-offset 0x10000 -o "$BUILD_DIR/update.bin" \
  0x10000 "$BUILD_DIR/firmware.bin" \
  0x3B0000 "$BUILD_DIR/littlefs.bin" >/dev/null

cp "$BUILD_DIR/full.bin"     release/esp32c6-sms-forwarding-full.bin
cp "$BUILD_DIR/update.bin"   release/esp32c6-sms-forwarding-update.bin
cp "$BUILD_DIR/firmware.bin" release/esp32c6-sms-forwarding-ota.bin
cp "$BUILD_DIR/littlefs.bin" release/esp32c6-sms-forwarding-littlefs.bin
cp "$BUILD_DIR/firmware.elf" release/esp32c6-sms-forwarding.elf

( cd release && sha256sum *.bin *.elf > SHA256SUMS.txt )
echo "[package] done:"
ls -l release/
