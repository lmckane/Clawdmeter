# Codex desktop hooks and ESP32-C6 experiments

This fork keeps the native Codex hook integration and C6 firmware work together.

The desktop app's Hooks settings page installs or removes Codex lifecycle hooks in `~/.codex/hooks.json`: `UserPromptSubmit`, `PostToolUse`, `PermissionRequest`, and `Stop`. Codex CLI requires review and trust after these commands are installed or changed. The app reads lifecycle events locally and forwards compact activity or attention updates over Clawdmeter's existing BLE link. The original Claude hook path remains separate.

The normal `waveshare_amoled_216_c6` firmware target remains the Bluetooth Clawdmeter build with chime. The optional `waveshare_amoled_216_c6_settings` target adds the shake-open settings overlay and Wi-Fi setup AP with up to four saved network profiles. That variant disables chime to make room in internal RAM. Wi-Fi save and automatic reconnect were not hardware verified; MicroLink is not integrated into that display build.

The separate `experiments/c6-tailnet-probe` ESP-IDF firmware proved Wi-Fi, MicroLink registration, and HTTP access to a Codex usage relay on the C6, but has no display UI. `tools/codex-usage-relay` contains the Python relay used for that test. Neither experiment is a travel-ready all-in-one firmware.

No device NVS image, Wi-Fi credentials, tailnet key, Codex authentication file, or personal relay address is stored in this repository.
