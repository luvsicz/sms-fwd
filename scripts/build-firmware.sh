#!/usr/bin/env bash
# 编译 ML307A 短信转发器固件，并把「能烧的四个文件」汇总到一个目录里。
#
# 用法：
#   scripts/build-firmware.sh                # 输出到 build/firmware-<时间戳>/
#   OUT=build/fw scripts/build-firmware.sh   # 指定输出目录
#   ARDUINO_CLI=/path/to/arduino-cli scripts/build-firmware.sh
#   BUILD_PATH=build/tmp scripts/build-firmware.sh   # 复用编译缓存（增量编译快很多）
#
# 注意：产物里嵌着 code/wifi_config.h 的默认 SSID/密码，别提交、别外发。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SKETCH="$ROOT/code"
ARDUINO_CLI="${ARDUINO_CLI:-arduino-cli}"
CORE_PKG="${CORE_PKG:-esp32}"
CORE_VER="${CORE_VER:-}"           # 留空 = 用已安装的最新版
FQBN="${FQBN:-esp32:esp32:esp32c3:PartitionScheme=custom,FlashMode=dio,FlashFreq=80,FlashSize=4M}"
STAMP="$(date +%Y%m%d-%H%M)"
OUT="${OUT:-$ROOT/build/firmware-$STAMP}"
SLOT_BYTES=$((0x1B0000))           # 双槽布局 app 槽大小 1,769,472

command -v "$ARDUINO_CLI" >/dev/null 2>&1 || {
  echo "找不到 arduino-cli（可用 ARDUINO_CLI=/abs/path 指定）" >&2; exit 1; }
[ -f "$SKETCH/partitions.csv" ] || {
  echo "缺少 $SKETCH/partitions.csv —— 没有它 PartitionScheme=custom 会退回默认单槽" >&2; exit 1; }

mkdir -p "$OUT"
BUILD_ARGS=()
if [ -n "${BUILD_PATH:-}" ]; then
  mkdir -p "$BUILD_PATH"
  BUILD_ARGS=(--build-path "$BUILD_PATH")
fi

echo "==> 编译（$FQBN）"
"$ARDUINO_CLI" compile --fqbn "$FQBN" "${BUILD_ARGS[@]}" --output-dir "$OUT" "$SKETCH" \
  | grep -E 'Sketch uses|Global variables' || true

# boot_app0.bin 不在 output-dir 里，要从 ESP32 core 的 tools/partitions 取
BOOT_APP0=""
for d in "$HOME/.arduino15/packages/$CORE_PKG/hardware/$CORE_PKG"/*/tools/partitions; do
  [ -f "$d/boot_app0.bin" ] || continue
  ver="$(basename "$(dirname "$(dirname "$d")")")"
  if [ -z "$CORE_VER" ] || [ "$ver" = "$CORE_VER" ]; then
    BOOT_APP0="$d/boot_app0.bin"; CORE_VER="$ver"
  fi
done
[ -n "$BOOT_APP0" ] || { echo "找不到 boot_app0.bin（core $CORE_PKG $CORE_VER 未安装？）" >&2; exit 1; }
cp -f "$BOOT_APP0" "$OUT/boot_app0.bin"

APP="$OUT/code.ino.bin"
APP_SIZE=$(stat -c%s "$APP" 2>/dev/null || stat -f%z "$APP")
if [ "$APP_SIZE" -gt "$SLOT_BYTES" ]; then
  echo "app 镜像 $APP_SIZE 字节 > 单槽 $SLOT_BYTES 字节，烧不进双槽布局" >&2; exit 1
fi

{
  echo "# ML307A 短信转发器固件产物"
  echo "时间: $(date '+%F %T %Z')"
  echo "FQBN: $FQBN"
  echo "core: $CORE_PKG $CORE_VER"
  echo "app: $APP_SIZE / $SLOT_BYTES 字节（余量 $((SLOT_BYTES-APP_SIZE))）"
  echo
  ( cd "$OUT" && sha256sum code.ino.bootloader.bin code.ino.partitions.bin boot_app0.bin code.ino.bin )
} > "$OUT/SHA256SUMS.txt"

cat > "$OUT/flash.sh" <<'EOF'
#!/usr/bin/env bash
# 用法: ./flash.sh /dev/ttyACM0   （Windows 用 COM5 这类端口名，或直接看 docs/BUILD_AND_FLASH.md）
set -euo pipefail
PORT="${1:?用法: ./flash.sh <串口>}"
ESPTOOL="${ESPTOOL:-esptool}"
"$ESPTOOL" --chip esp32c3 -p "$PORT" -b 921600 \
  write-flash --flash-mode dio --flash-freq 80m --flash-size 4MB \
  0x0     code.ino.bootloader.bin \
  0x8000  code.ino.partitions.bin \
  0xe000  boot_app0.bin \
  0x10000 code.ino.bin
EOF
chmod +x "$OUT/flash.sh"

echo
echo "==> 产物: $OUT"
ls -l "$OUT"/code.ino.bin "$OUT"/code.ino.bootloader.bin "$OUT"/code.ino.partitions.bin "$OUT"/boot_app0.bin \
  | awk '{printf "  %-32s %10d\n", $9, $5}'
echo "  code.ino.merged.bin 不要烧（整片 4MB，会连 NVS + SPIFFS 一起覆盖）"
echo
echo "烧录: $OUT/flash.sh /dev/ttyACM0"
