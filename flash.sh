#!/data/data/com.termux/files/usr/bin/bash
# Flash the built firmware onto an ESP32 DevKit V1 from Termux.
#
# Usage:
#   1. Download bootloader.bin, partition-table.bin, esp32-wifi-repeater.bin
#      from the GitHub Actions "Releases" page of this repo, into this folder.
#   2. Connect the ESP32 via USB-OTG cable.
#   3. Run:  bash flash.sh /dev/ttyUSB0
#      (run "ls /dev/tty*" first if unsure of the port; termux-usb may be needed)

set -e

PORT="${1:-/dev/ttyUSB0}"

pip install esptool 2>/dev/null || pkg install -y python esptool

python3 -m esptool --chip esp32 --port "$PORT" --baud 460800 \
  --before default_reset --after hard_reset write_flash -z \
  --flash_mode dio --flash_freq 40m --flash_size 4MB \
  0x1000  bootloader.bin \
  0x8000  partition-table.bin \
  0x10000 esp32-wifi-repeater.bin

echo "Done. Device rebooting. Connect to WiFi SSID 'Sarfraz' (password 'Sarfraz')"
echo "and open http://192.168.4.1 in a browser to reach the admin dashboard."
echo "Admin login -> user: admin   password: Sarfraz  (change it from the dashboard)."
