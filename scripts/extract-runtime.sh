#!/bin/bash
# Extract vendor files from the Lenovo flash package.
set -euo pipefail
vendor="${1:?Usage: extract-runtime.sh VENDOR_EXT4 DSP_EXT4 OUTPUT_TAR}"
dsp="${2:?Missing DSP ext4 image}"
output="${3:?Missing output archive}"
[[ $EUID == 0 ]] || { echo "Run with sudo for debugfs rdump." >&2; exit 1; }
work="$(mktemp -d)"
mkdir -p "$work/vendor" "$work/runtime/dsp" "$work/runtime/sensors"
debugfs -R "rdump /etc/sensors $work/vendor" "$vendor"
debugfs -R "rdump /adsp $work/runtime/dsp" "$dsp"
test -s "$work/vendor/sensors/config/pakala_icm4x6xa_0.json"
test -s "$work/vendor/sensors/sns_reg_config"
test -n "$(find "$work/runtime/dsp/adsp" -type f -print -quit)"
cp -a "$work/vendor/sensors/config" "$work/runtime/sensors/"
cp "$work/vendor/sensors/sns_reg_config" "$work/runtime/sensors/sns_reg.conf"
tar --owner=0 --group=0 -C "$work/runtime" -czf "$output" .
echo "Runtime archive: $output"
echo "Extraction directory: $work"
