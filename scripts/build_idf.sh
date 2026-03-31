#!/usr/bin/env bash
set -euo pipefail

if [[ -z "${IDF_PATH:-}" ]]; then
  echo "IDF_PATH is not set. Source ESP-IDF export first:"
  echo "  . ~/esp/esp-idf/export.sh"
  exit 1
fi

idf.py set-target esp32
idf.py build

echo "Build completed. Flash with: idf.py -p /dev/ttyUSB0 flash monitor"
