# ESP32-C6 Tailscale connectivity probe

This standalone ESP-IDF v5.5.3 experiment tests Wi-Fi setup, MicroLink tailnet registration, and HTTP polling on the Waveshare ESP32-C6-Touch-AMOLED-2.16. It does not initialize the display or run Clawdmeter's UI. Flashing it replaces the screen firmware.

The MicroLink and WireGuard source in `components/` is copied from [CamM2325/microlink](https://github.com/CamM2325/microlink) commit `1cb8d1c8a8408ad1b9a339b18bed3e970b71dafe`; its license files are retained. Set the relay address on the setup page. The `100.0.0.0:8765` fallback in `main.c` is a placeholder.

Build with `idf.py set-target esp32c6` and `idf.py build`. On first boot, join `Clawdmeter-Setup` using the test password `clawdmeter-setup` and enter Wi-Fi credentials at `http://192.168.4.1/`. The one-time tailnet approval link appears on USB serial at 115200 baud. Replace the fixed AP password before using this away from a controlled test environment.

The physical C6 connected to Wi-Fi and Tailscale, then received HTTP 200 responses containing Clawdmeter `pv:"codex"` JSON from a VM relay. This probe has no PSRAM and did not establish that MicroLink and the full LVGL screen can coexist in memory.
