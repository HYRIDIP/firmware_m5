# M5StickC Plus 1.1 Platform Firmware (ESP-IDF)

This repository now targets **ESP-IDF** (not Arduino IDE), so firmware can be built/flashed entirely from CLI.

## Build and flash (without Arduino IDE)

```bash
. ~/esp/esp-idf/export.sh
./scripts/build_idf.sh
idf.py -p /dev/ttyUSB0 flash monitor
```

## Current minimal platform functionality

- Terminal-based UI shell (UART) with command menu.
- Neofetch-like device information with ASCII M5 logo.
- Passive Wi-Fi scan/inventory tools.
- Embedded JS-like scripting runtime for platform extension:
  - `print("text")`, `let`, `add`, `sub`, `sleep_ms`, `wifi.scan`, `goto_if_gt`, `vars`.
  - interactive script input mode from serial console.

## Security scope

This platform intentionally excludes offensive RF attack features (e.g., deauth/jamming). Commands requesting those actions are blocked by design.

