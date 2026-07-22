#!/usr/bin/env bash
set -euo pipefail
board="${1:-s3}"; out_dir="${2:-.}"
[[ -f config.toml ]] || { echo 'config.toml is required' >&2; exit 1; }
case "$board" in
  s3) target=xtensa-esp32s3-espidf; chip=esp32s3; flash=16mb; layout=16m; partitions=partitions_ota_16m.csv; sdk="$PWD/sdkconfig.esp32s3.defaults" ;;
  r2) target=xtensa-esp32-espidf; chip=esp32; flash=4mb; layout=4m; partitions=partitions_ota_4m.csv; sdk="$PWD/sdkconfig.defaults" ;;
  *) echo 'board must be r2 or s3' >&2; exit 2 ;;
esac
mkdir -p "$out_dir"
if [[ -z "${CARGO_TARGET_DIR:-}" && "${OS:-}" == Windows_NT ]]; then
  if [[ "$board" == s3 ]]; then export CARGO_TARGET_DIR='C:\ts3'; else export CARGO_TARGET_DIR='C:\t'; fi
fi
for spec in '0 software-only' '1 with-config'; do
  read -r apply name <<<"$spec"
  SMSGATE_APPLY_COMPILED_CONFIG="$apply" SMSGATE_FLASH_LAYOUT="$layout" ESP_IDF_SDKCONFIG_DEFAULTS="$sdk" \
    cargo +esp build --release --target "$target"
  elf="${CARGO_TARGET_DIR:-target}/$target/release/smsgate"
  espflash save-image --chip "$chip" --flash-size "$flash" --partition-table "$partitions" \
    --target-app-partition ota_0 "$elf" "$out_dir/smsgate-$board-ota-$name.bin"
done
