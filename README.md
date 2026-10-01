# ESP32 WiFi Repeater — Ibn e Ilyas Technologies

A real internet-sharing WiFi repeater for the ESP32 DevKit V1, built on
ESP-IDF (not the Arduino core) so that NAT actually works. Built by
Sarfraz Qureshi.

See **[FEATURES.md](FEATURES.md)** for the full feature list, and
**[LEARN_TO_EDIT.md](LEARN_TO_EDIT.md)** for where to change any text you
see on the device's screens without touching the surrounding code.

## Defaults

- Repeater WiFi: **SSID `Sarfraz`**, **password `Sarfraz1`**
- Admin login: **username `Sarfraz`** (fixed, not changeable), **password
  `admin`** (change this from Advanced Settings after first boot)
- Dashboard, once connected to the repeater's own WiFi: `http://192.168.4.1`
- Public WiFi Tools page (no login needed): `http://192.168.4.1/tools`

## Repository layout

| Path | Purpose |
|---|---|
| `main/main.c` | Firmware: WiFi AP+STA, NAT/NAPT, guest filtering, bandwidth limits, deauth monitor, web server |
| `main/login.html` | Public branded login page + "About Developer" popup |
| `main/admin.html` | Admin dashboard (after login) |
| `main/tools.html` | Public WiFi Tools page (analyzer, deauth monitor, speed test, diagnose) |
| `sdkconfig.defaults` | Enables real NAT (`LWIP_IPV4_NAPT`) and tuned network buffers |
| `partitions.csv` | 4MB flash partition layout |
| `.github/workflows/build.yml` | Builds the firmware on GitHub's servers on every push |
| `flash.sh` | Optional one-command Termux flashing script |
| `FEATURES.md` | Full A-to-Z feature list — update this whenever a feature is added |
| `LEARN_TO_EDIT.md` | Where to find and safely edit any on-screen text |

## Step 0 — Start Termux clean (do this first)

If Termux already has old sessions or stuck commands open:

```bash
# To see other/background Termux sessions (if using Termux:Widget or
# multiple sessions), start a fresh one:
#   swipe from the left edge in the Termux app -> "New session"
# To close an old session from inside it:
exit

# If a command is stuck in the foreground:
# press Ctrl (volume-down + C) in that session, then:
exit

# Start from a clean folder so old files/clones don't mix in:
cd ~
mkdir -p esp32-repeater-project
cd esp32-repeater-project
```

Do the rest of the steps below inside this fresh folder.

## Step 1 — Push this repo to GitHub

```bash
pkg install -y git
git init
git remote add origin https://github.com/<your-username>/esp32-wifi-repeater.git
git add .
git commit -m "Initial ESP32 NAT WiFi repeater firmware"
git branch -M main
git push -u origin main
```

(Create an empty `esp32-wifi-repeater` repository on GitHub first via the
browser, then replace `<your-username>` above with your actual username.
GitHub will ask for a Personal Access Token instead of your password when
you push — Settings → Developer settings → Personal access tokens.)

## Step 2 — The firmware builds itself

Pushing triggers `.github/workflows/build.yml`, which compiles the
firmware on GitHub's own servers (nothing compiles on your phone/PC).
Takes about 3–6 minutes:

- Watch progress under the repo's **Actions** tab
- Once it's green, the repo's **Releases** page gets a new release with
  five files: `bootloader.bin`, `partition-table.bin`,
  `esp32-wifi-repeater.bin`, `full-flash-0x0.bin`, and
  **`full-flash-padded-0x0.bin`**

## Step 3 — Flash it

**Use `full-flash-padded-0x0.bin` from the Releases page** (not
Actions → Artifacts, which always wraps everything in a zip and has
caused confusion before). It's a single, pre-merged image — flash it at
offset **`0x0`**, nothing else to configure.

With an Android flashing app (e.g. "ESP32 Flash/Erase"):
- Chip: ESP32, Baud: 115200, spiMode: DIO, spiFreq: 40m, flashSize: 4MB
- **Compression: OFF** (some Android USB-serial drivers silently drop the
  tail of the image when compression is on — this caused real boot
  failures during development)
- Erase first, then flash the single file at offset `0x0`

From Termux with `esptool` instead:
```bash
pkg install -y python
pip install esptool
python3 -m esptool --chip esp32 --port /dev/ttyUSB0 --baud 115200 \
  write_flash -z 0x0 full-flash-padded-0x0.bin
```

## Step 4 — First use

1. Connect to WiFi **`Sarfraz`** (password `Sarfraz1`) — 2.4 GHz only
2. Open `http://192.168.4.1` in a browser
3. Log in: username `Sarfraz`, password `admin`
4. Under **Upstream WiFi**, enter your real internet router's SSID/password
   and save — the repeater reboots and starts sharing internet
5. Under **MAC Whitelist**, add your own device's MAC first — an empty
   whitelist allows everyone; once you add even one entry, only listed
   devices get full access (others still connect but get no internet,
   unless "Guests: connect but no internet" is turned off)
6. Change the admin password from **Advanced Settings**

## Hardware factory reset

Hold the **BOOT** button on the board for about 8 seconds. When the blue
LED starts blinking quickly, release it — all settings are wiped back to
defaults and the repeater reboots.

## Known limitations

See the "Known limitations" section at the bottom of
[FEATURES.md](FEATURES.md) — in short: per-device data usage and
per-device bandwidth limits only cover **uploads** (not downloads), and
Ethernet support needs an external PHY chip wired to the board (a
hardware addition, not a firmware-only change).

## Version history

- **v1** — Initial NAT-based repeater: AP+STA, web dashboard, MAC
  whitelist, config import/export
- **v2** — Fixed username `Sarfraz`, all settings made editable with
  defaults (Advanced Settings), hardware factory-reset button, single
  padded flash image for reliable Android flashing
- **v3** — Guest access (connect but no internet unless whitelisted),
  public `/tools` page (WiFi analyzer, deauth monitor, speed test,
  diagnose), "About Developer" popup, kick button, firmware version shown
  in the dashboard
- **v4** — Fixed the vendor-lookup "Unknown" issue's first cause, added
  password show/hide toggles, upstream-stability log, "Share this WiFi"
  button, `LEARN_TO_EDIT.md`
- **v5** — Per-device data usage (weekly/monthly) and upload bandwidth
  limits, auto-restart schedule, static upstream IP for multi-repeater
  setups, internet speed-history chart, `FEATURES.md`
- **v6** — Fixed the real cause of the vendor-lookup false "no internet"
  banner, added accurate whole-repeater usage tracking covering both
  directions, and an overall (all-devices-combined) bandwidth cap for
  both upload and download
- **v7** (current) — Public opt-in AI chat page (`/chat`, admin supplies
  their own API key, OFF by default), a factory-reset counter so a reset
  can always be verified, rewrote this README and FEATURES.md fully in
  English. Google Drive backup sync: Cloud Console setup documented, the
  on-device OAuth implementation is intentionally phased as a dedicated
  follow-up (see FEATURES.md "Known limitations")
