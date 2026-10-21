# Runtime provenance

`vendor/runtime/` comes from TB322_ZUXOS_1.5.10.259_Tool, not a local DSP build.
The original images are `image/super_7.img` (vendor ext4) and `image/dspso.bin` (DSP ext4).

```sh
sudo bash scripts/extract-runtime.sh /path/to/super_7.img /path/to/dspso.bin runtime.tar.gz
mkdir -p vendor/runtime
tar -xzf runtime.tar.gz -C vendor/runtime
```

- `/etc/sensors/config` becomes `vendor/runtime/sensors/config`.
- `/etc/sensors/sns_reg_config` becomes `vendor/runtime/sensors/sns_reg.conf`.
- `/adsp` becomes `vendor/runtime/dsp/adsp`.

`vendor/fallback-registry/` is the existing read-only persist extraction from the development tablet.
It comes from `/sensors/registry`, not the mutable Linux DSP working state.
It is a last resort, not device-specific calibration for other tablets.
Accuracy is not guaranteed. Armbian logs a warning on every fallback boot.
No complete persist image is included. Redistribution rights require separate review.

`vendor/runtime.sha256` and `vendor/fallback-registry.sha256` record the imported files.
Run checksum verification from `vendor/`; paths in the manifests are relative to that directory.
DSP access to vendor runtime resources is read-only through hexagonrpcd.
Only a separate Linux registry working copy is writable.
