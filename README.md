# WS2812BClock

A wall clock built from 88 addressable SK6812 pixels driven by an ESP32: a
60-pixel ring showing hour, minute and second hands, and a four-digit
seven-segment display showing the time and date. Time comes from NTP, and
firmware updates arrive over HTTPS from a private update server.

Hardware geometry, the LED pinout and the printable soldering jigs live in
[`CAD/README.md`](CAD/README.md).

## Setting up on a new machine

Everything except three local secrets is in the repo.

### 1. Toolchain

The Nix flake pins ESP-IDF v5.5.2 and every toolchain:

```bash
direnv allow          # or: nix develop
idf.py --version      # ESP-IDF v5.5.2
```

For VS Code, install the **ESP-IDF** and **direnv** extensions. The flake
registers itself with the ESP-IDF extension automatically (see the shell hook
in `flake.nix`), so no manual "configure extension" step is needed.

### 2. The three things that are *not* in the repo

`sdkconfig` is gitignored, so each machine needs these set once:

| What | Where | Notes |
| --- | --- | --- |
| WiFi SSID and password | `idf.py menuconfig` → *WS2812B Clock Wifi* | Never put these in `sdkconfig.defaults` — that file is committed. |
| OTA server token | *WS2812B Clock OTA* → `OTA_AUTH_TOKEN` | Same reasoning. |
| Update server root CA | `components/ota/certs/server_ca.pem` | Copy the CA root there. `*.pem` is gitignored. |

Everything else has a sensible default in `sdkconfig.defaults` (4 MB flash, OTA
partition table, rollback, display pins and brightness, timezone).

### 3. Signing key

Images are signed, so **every build needs the key**. It lives in 1Password and
is fetched to tmpfs only for the duration of a build — see *Signing* below.

### 4. First flash

The partition table has two OTA slots, so a board coming from an older layout
must be erased once:

```bash
idf.py -p /dev/ttyUSB1 erase-flash
idf.py -p /dev/ttyUSB1 flash monitor
```

Afterwards, plain `flash` is enough.

## Configuration reference

All under `idf.py menuconfig`:

| Menu | Options |
| --- | --- |
| *WS2812B Clock Display* | Ring and digit GPIOs and pixel counts, day/night brightness for each, night window (default 19:00–06:00), time/date alternation |
| *WS2812B Clock Wifi* | SSID, password, `WIFI_MAXIMUM_RETRY` |
| *WS2812B Clock NTP* | Server (default `pool.ntp.org`), timezone (default Sydney), sync method |
| *WS2812B Clock OTA* | Base URL, model directory, token, certificate source, check-at-boot |
| *WS2812B Clock Debugger* | `DEBUG_BUILD` logging, status LED pin |
| *WS2812B Clock Fans* | Fan PWM pin |

Shipped defaults: ring on GPIO32 (60 px) at 5% day / 1% night, digits on
GPIO25 (28 px) at 50% day / 5% night. Brightness is worth tuning per build
once the diffusers are in — the values in a local `sdkconfig` win over these.

## Scripts

### `scripts/ota-key.sh` — signing key handling

```bash
./scripts/ota-key.sh fetch    # 1Password -> /run/user/$UID/ota_key.pem (tmpfs)
./scripts/ota-key.sh shred    # remove it again
```

The path must match `CONFIG_SECURE_BOOT_SIGNING_KEY`. Overridable with
`OTA_SIGNING_KEY` and `OTA_SIGNING_KEY_ITEM`.

VS Code runs these automatically: `idf.preBuildTask` fetches, `idf.postBuildTask`
and `idf.postFlashTask` shred. From a terminal, do it by hand:

```bash
./scripts/ota-key.sh fetch && idf.py build && ./scripts/ota-key.sh shred
```

A failed build skips the post task and leaves the key in tmpfs — RAM only,
owner-readable, gone on reboot, but `shred` cleans it up sooner.

**1Password on a fresh machine:** `op` must be the setgid wrapper, which on
NixOS comes from the system modules, not just the package:

```nix
programs._1password.enable = true;
programs._1password-gui = { enable = true; polkitPolicyOwners = [ "setkeh" ]; };
users.users.setkeh.extraGroups = [ "onepassword" "onepassword-cli" ];
```

Then enable *Settings → Developer → Integrate with 1Password CLI* in the app.
Without that, `op` fails with "connecting to desktop app"; setting
`OP_BIOMETRIC_UNLOCK_ENABLED=false` and using `op signin` is the fallback.

### `scripts/deploy-firmware.sh` — publish a release

```bash
./scripts/deploy-firmware.sh              # fetch key, build, publish, shred key
./scripts/deploy-firmware.sh --no-build   # publish the existing build
./scripts/deploy-firmware.sh --dry-run    # print the manifest, upload nothing
```

Version and model come from `sdkconfig`, so a release is: bump
`CONFIG_APP_PROJECT_VER` in menuconfig, run the script. It uploads to
`$OTA_HOST:$OTA_ROOT/<model>/` (default `ota:/srv/ota`), image first and
`latest.json` second, so devices never see a manifest pointing at a missing
file.

## OTA

### How a clock updates itself

1. After boot, once WiFi is up and NTP has synced, the running image marks
   itself valid — which cancels the rollback the bootloader would otherwise
   perform on the next reset.
2. If `OTA_CHECK_ON_BOOT` is set, it fetches `<base>/<model>/latest.json`.
3. If `version` there **differs** from the running version, it downloads the
   named file and reboots into it. Differing in either direction means a
   manifest edit can also roll a fleet *back*.
4. `esp_https_ota` verifies the image signature during install, so an unsigned
   or corrupted download is rejected before it can boot.

### Server layout

```
/srv/ota/ws2812bclock-esp32/
├── latest.json                        {"version","file","sha256","size"}
├── ws2812bclock-esp32-0.1.0.bin
└── ws2812bclock-esp32-0.2.0.bin
```

The model directory names the hardware *and* the chip target, so a build for
one target can never be served to another.

### Certificates

`OTA_CERT_SOURCE` picks how the server is verified:

- **Public CA** — uses the ESP-IDF certificate bundle. For a publicly trusted
  certificate.
- **Embedded certificate** — trusts only `components/ota/certs/server_ca.pem`,
  compiled into the firmware. This is the private-CA path.

Embed the **root**, not the leaf or intermediate: server certificates can then
be reissued without reflashing. The server must serve the full chain
(leaf + intermediate), because the device only holds the root.

The device needs working DNS for the server's name and a reachable NTP server —
TLS validation fails if the clock's time is wrong, and short-lived certificates
make that stricter.

### Signing

Signing uses `SECURE_SIGNED_APPS_NO_SECURE_BOOT`, which verifies image
signatures **without** burning eFuses, so a mistake cannot brick a board.

The scheme is **ECDSA (NIST P-256)**, because this ESP32 is a rev v1.0
D0WDQ6: RSA-3072 (secure boot v2) needs rev v3.0 or later. Newer chips
(S3, C6) support RSA-3072, so a move to that hardware means a new key and a
scheme change.

**The key must match the scheme**, or the build fails at the signing step with
"Incorrect ECDSA private key specified":

```bash
# this hardware (secure boot v1 scheme)
espsecure.py generate_signing_key --version 1 --scheme ecdsa256 key.pem

# future S3/C6 hardware (secure boot v2)
espsecure.py generate_signing_key --version 2 --scheme rsa3072 key.pem
```

Store each in 1Password as its own document and point `OTA_SIGNING_KEY_ITEM`
at the one matching the target.

Losing the signing key means signed clocks stop accepting updates and must be
reflashed over USB. It lives in 1Password; keep that recoverable.

## Core dumps

Panic output goes straight to the UART as the chip resets, so on a wall-mounted
clock it is lost. `esp_core_dump` writes a dump to the 64 KB `coredump`
partition instead, which survives the reset:

```bash
idf.py -p <port> coredump-info      # decode: panic reason, backtrace, all tasks
```

An empty partition decodes as version `0xffff` and size `4294967295` — that is
erased flash, not a failure.

Debug builds accept **`crash`** typed on the serial console, which calls
`abort()` on purpose so the path can be tested without waiting for a real
fault. The command lives in the UART receive task, which only exists when
`DEBUG_BUILD` is set, so a release build does not have it.

Shipping dumps off the device over WiFi is issue #17.

## Layout

```
components/
├── display/   both LED chains, position maps, font, brightness
├── ntp/       SNTP, timezone, NTP_EVENT, ntp_time_is_valid()
├── wifi/      station mode, retries forever with back-off
├── ota/       manifest check, HTTPS update, rollback handling
├── fan/       LEDC PWM fan control with kick-start
└── debug/     activity LED on serial traffic
main/          startup, the second-aligned tick loop, rendering
CAD/           jigs, clock geometry, KiCad project
scripts/       signing key and release helpers
```
