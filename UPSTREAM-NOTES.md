# Source and modification notes

- Upstream base: DrWalls source archive supplied by the user (Dr. Maker), released under Apache License 2.0.
- Sensing dependency: Espressif `esp_wifi_sensing`, resolved from the declared ESP-IDF Component Manager dependencies; the registry lists ESP32-S3 as a supported target and requires ESP-IDF >= 5.4.
- Changes in this package: ESP32-S3 cloud-build workflow; chip-ID verification before artifact publication; browser installer package; local PowerShell server to avoid Python/local tool installations; Arabic device dashboard; runtime sensing controls, NVS persistence, baseline reset, training actions, and Arabic toggle for the serial monitor.
- The original precompiled images were removed because their image-header chip ID is 0 (classic ESP32), and the upstream manifest/build instructions selected `ESP32`. The workflow intentionally refuses to publish a binary unless the new application image header identifies ESP32-S3 (chip ID 9).

No claim is made that the updated source has already been compiled or hardware-tested in this environment. The GitHub Actions workflow performs the actual target build and verifies the application image before making a flash artifact available.
