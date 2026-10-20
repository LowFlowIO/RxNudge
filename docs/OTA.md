# Firmware update (OTA)

## Normal update from the web portal
1. Build the firmware: `pio run -e sticky`. This produces `.pio/build/sticky/firmware.bin` and `firmware.bin.sha256`.
2. On the device, open Settings → Web portal. Note the code shown there.
3. On a phone or computer on the same Wi-Fi, open `http://rx-nudge.local` and sign in with the code.
4. Go to More → Firmware update. Choose `firmware.bin` and either paste the SHA-256 or pick the `.sha256` file.
5. The device checks the upload, then restarts. If the new firmware fails to start, the device returns to the previous version by itself.

## Versions
The version comes from the `VERSION` file only. It ends up in the app descriptor (`PROJECT_VER`) and in
`MT_FW_VERSION`, so the portal shows the running version and, after the upload header arrives, the version of the
uploaded image. Versions are shown, not compared: installing an older or the same version is allowed on purpose,
so a bad release can be undone. Check the shown version before confirming.
Release builds also come as `rxnudge-v<ver>-binaries/firmware.bin` with `firmware.bin.sha256` (see
docs/BUILD_FLASH.md section 8).

## What is checked
| Check | When | Result if wrong |
|---|---|---|
| SHA-256 present, 64 hex digits | before writing | 422, nothing written |
| Size fits the inactive slot (6.25 MB) | before writing | 422 |
| Image magic 0xE9, chip ID ESP32-S3 | after the first 288 bytes | upload aborted, slot left inactive |
| App descriptor present, project = `rxnudge` | after the first 288 bytes | aborted |
| Received size = announced size | end | aborted |
| SHA-256 of all bytes = supplied value | end | aborted, slot never bootable |
| ESP-IDF image validation (`esp_ota_end`) | end | aborted |
| New firmware starts healthily (storage mounts, first frame drawn) | next boot | automatic rollback |

The SHA-256 protects against corrupted or wrong files. It does **not** prove who built the firmware: anyone who has the portal code can upload an image with a matching hash. The portal code is shown only on the device, so this requires physical access.

## Signing option (stronger authenticity)
ESP-IDF can refuse any image that you did not sign yourself:

1. **Signed app images without Secure Boot** (`CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT=y`, `CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT=y`, scheme RSA-3072 / Secure Boot V2 format).
   - `esp_ota_end()` then rejects an image that is not signed with your key, and the OTA code above needs no change.
   - The public key is compiled into the running firmware. The private key stays on the build machine (`espsecure.py generate_signing_key --version 2 signing_key.pem`, `CONFIG_SECURE_BOOT_SIGNING_KEY`).
   - This is reversible: no eFuses are burned.
2. **Full Secure Boot V2** adds verification of the bootloader and app on every boot, which ROM enforces.
   - It burns eFuses and **cannot be undone**.
   - A lost key means the board can never be updated again.
   - It also affects USB flashing (the reference web flasher).
   - Do this only with a key-management plan; it is out of scope for this project.

Option 1 is the recommended next step if the device is shared or lives in a care home. It adds about 1 KB of signature per image, and it requires a key backup.

## Rollback details
- `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` is set in `sdkconfig.defaults` and `sdkconfig.sticky`.
- The bootloader is only updated over USB. Flash once with `scripts/flash_sticky.sh` (or `pio run -t upload`) after this change.
- `net::otaConfirmBoot()` marks the image valid before any deep sleep can happen, so a timer wake can never be mistaken for a failed boot.
