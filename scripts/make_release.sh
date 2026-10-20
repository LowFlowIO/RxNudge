#!/usr/bin/env bash
# Clean build of both environments, all tests, previews, then the release artefacts:
#   ../rxnudge-<ver>-binaries/ (+ .tar.gz) and ../rxnudge-<ver>.tar.gz (sources, no .pio)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
VER="$(head -n1 VERSION)"
OUT="$(dirname "$ROOT")"
B=.pio/build/sticky
rm -rf .pio/build
pio test -e native
pio run -e sticky
./tools/render_previews.sh
ESPTOOL="python3 $HOME/.platformio/packages/tool-esptoolpy/esptool.py"
DIR="$OUT/rxnudge-v${VER}-binaries"
rm -rf "$DIR" && mkdir -p "$DIR"
cp "$B/firmware.bin" "$B/firmware.bin.sha256" "$B/bootloader.bin" "$B/partitions.bin" "$B/ota_data_initial.bin" "$DIR/"
$ESPTOOL --chip esp32s3 merge_bin -o "$DIR/rxnudge-v${VER}-flash-full.bin" \
  --flash_mode dio --flash_freq 40m --flash_size 16MB \
  0x0 "$B/bootloader.bin" 0x8000 "$B/partitions.bin" 0xe000 "$B/ota_data_initial.bin" 0x10000 "$B/firmware.bin"
cat > "$DIR/FLASH.txt" <<TXT
RxNudge v${VER} for Seeed reTerminal Sticky (ESP32-S3, 16 MB image)

Full install over USB (erases nothing else; resets the OTA boot selection to app0):
  esptool.py --chip esp32s3 --port /dev/sticky --baud 460800 write_flash \\
    --flash_mode dio --flash_freq 40m --flash_size 16MB 0x0 rxnudge-v${VER}-flash-full.bin
or the separate parts:
  0x0 bootloader.bin   0x8000 partitions.bin   0xe000 ota_data_initial.bin   0x10000 firmware.bin
Update from the web portal: upload firmware.bin with the value in firmware.bin.sha256.
Check files: sha256sum -c SHA256SUMS
TXT
(cd "$DIR" && sha256sum *.bin > SHA256SUMS)
tar -C "$OUT" -czf "$DIR.tar.gz" "$(basename "$DIR")"
tar -C "$OUT" --exclude="$(basename "$ROOT")/.pio" --exclude="$(basename "$ROOT")/.git" \
  -czf "$OUT/rxnudge-v${VER}.tar.gz" "$(basename "$ROOT")"
echo "release: $OUT/rxnudge-v${VER}.tar.gz, $DIR(.tar.gz)"
