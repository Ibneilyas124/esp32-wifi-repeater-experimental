# ESP32 WiFi Repeater — Sarfraz Ibn E Ilyas

ESP32 DevKit V1 par asli internet-sharing WiFi repeater (NAT/NAPT based),
ESP-IDF (Arduino nahi) par likha gaya, taake proper NAT kaam kare.

- Default AP: **SSID `Sarfraz`**, **Password `Sarfraz`**
- Admin dashboard login: **user `admin`**, **password `Sarfraz`** (dashboard se change kar sakte ho)
- Dashboard URL jab AP se connect ho: `http://192.168.4.1`

## Step 0 — Termux ko clean shuru karna (pehle yeh karo)

Purana kaam/files/sessions band karne ke liye:

```bash
# Sab background/other Termux sessions dekhne ke liye (agar Termux:Widget ya
# multiple sessions use kar rahe ho), naya session start karo:
#   Termux app me left-edge se swipe karo -> "New session"
# Purana session band karna ho to us session me:
exit

# Agar koi command background me atka hai:
# Ctrl (volume-down + C) dabao us session me, phir:
exit

# Naya kaam shuru karne se pehle purani directory se nikal ke fresh
# folder banao, taake purane files/clones mix na hon:
cd ~
mkdir -p esp32-repeater-project
cd esp32-repeater-project

# (Optional) agar purana koi repo clone tha usi naam se, use rename/remove karo:
# mv old-folder-name old-folder-name-backup
```

Ab is fresh folder ke andar niche wale steps follow karo.

## Step 1 — Yeh repo GitHub par push karo

```bash
pkg install -y git
git init
git remote add origin https://github.com/<tumhara-username>/esp32-wifi-repeater.git
git add .
git commit -m "Initial ESP32 NAT wifi repeater firmware"
git branch -M main
git push -u origin main
```

(GitHub par pehle ek empty repo `esp32-wifi-repeater` bana lena browser se,
phir upar wala `<tumhara-username>` apne username se replace karna.)

## Step 2 — Firmware apne aap ban jayega

Push karte hi `.github/workflows/build.yml` GitHub Actions ko trigger karega.
GitHub ke server par ESP-IDF firmware compile hoga (tumhare phone/PC par
kuch bhi compile nahi hoga). 2-4 minute me:

- Repo ke "Actions" tab me build ki progress dekh sakte ho
- Complete hone ke baad repo ke "Releases" section me
  `bootloader.bin`, `partition-table.bin`, `esp32-wifi-repeater.bin` mil jayengi

## Step 3 — Flash karna (Termux se)

Teeno `.bin` files download karke isi folder me daalo, phir:

```bash
pkg install -y python
pip install esptool
bash flash.sh /dev/ttyUSB0
```

(USB-OTG cable se ESP32 connect hona chahiye; agar `/dev/ttyUSB0` na mile to
`termux-usb -l` se port dekho.)

## Step 4 — Use karna

1. Phone/laptop se WiFi `Sarfraz` (password `Sarfraz`) se connect karo
2. Browser me `http://192.168.4.1` kholo
3. Admin login: `admin` / `Sarfraz`
4. "Upstream WiFi" section me apne asli internet wale router ka SSID/password
   dalo aur Save karo — ESP32 reboot hoga aur internet sharing shuru ho jayegi
5. "MAC Whitelist" section se sirf apne devices ko allow kar sakte ho

## Files in this repo

| File | Purpose |
|---|---|
| `main/main.c` | Firmware: WiFi STA+AP, NAT/NAPT, web dashboard, whitelist, config import/export |
| `sdkconfig.defaults` | Enables real NAT (`LWIP_IPV4_NAPT`) — the part that was missing in ArduinoDroid |
| `partitions.csv` | 4MB flash partition layout |
| `.github/workflows/build.yml` | Builds firmware on GitHub's servers on every push |
| `flash.sh` | One-command Termux flashing script |

## v2 notes
- Login: user `Sarfraz` (fixed), default password `admin` (changeable in Advanced Settings)
- Advanced Settings (all editable, all with defaults): WiFi name/password/hidden/channel/max devices,
  repeater IP, DNS for clients (default 8.8.8.8), hostname shown in the main router,
  custom STA MAC, TX power, admin password
- Hardware reset: hold the BOOT button ~8 s; when the blue LED blinks fast, release
- Flash `full-flash-padded-0x0.bin` at offset `0x0` (compression OFF, baud 115200) from the Releases page


## v3 notes (guest access, WiFi tools, About Developer)
- Guests can now associate with the repeater's password but get **no internet** unless their
  MAC is in the whitelist (Advanced Settings > "Guests: connect but no internet", default ON).
  They can still reach the admin login page and /tools. Set it to 0 to go back to instant-kick.
- New **/tools** page (no login): WiFi Analyzer (scans nearby networks, shows hidden BSSIDs,
  vendor from OUI, channel congestion chart), Deauth attack monitor (passive), Speed test
  (repeater link + real internet), Internet diagnosis (step-by-step).
- Admin dashboard: connected-clients table now shows signal, whether each device currently has
  internet, and a Kick button; header shows firmware version.
- Login page footer replaced with "About Developer" (popup) and a link to /tools.
- New advanced settings: guest_filter, deauth_monitor, deauth_alert (all with safe defaults).

## v5 notes (this pass)
- Fixed vendor-lookup "Unknown" root cause: online lookup needs internet on the
  BROWSING device, which the v3 guest-filter blocks for non-whitelisted devices.
  /tools now shows a clear banner explaining this and what to do about it.
- Cleaned up duplicate/conflicting settings left over from an earlier interrupted
  session (two competing static-IP systems, two usage-tracking systems). The
  more complete, already-wired system was kept in every case.
- Added the missing admin-dashboard UI for: internet speed-history chart.
  (Data usage, bandwidth-limit-per-device, and restart-schedule backends and
  their dashboard UI were already complete from the previous session.)
- Added FEATURES.md (full A-to-Z feature list, update this file going forward).
- See chat for the Ethernet-port hardware explanation (not a firmware-only change).
