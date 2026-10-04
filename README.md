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
| *WS2812B Clock Display* | Ring and digit GPIOs and pixel counts, day/night brightness for each, night window (default 19:00–06:00), rotation and `DISPLAY_CYCLE_SECONDS` |
| *WS2812B Clock Environment Sensor* | `DHT_ENABLE`, data GPIO, reading interval, retries |
| *WS2812B Clock Wifi* | SSID, password, `WIFI_MAXIMUM_RETRY`, `WIFI_POWER_SAVE` (off: modem sleep costs OTA throughput and log latency for power a mains-powered clock does not need) |
| *WS2812B Clock NTP* | Server (default `pool.ntp.org`), timezone (default Sydney), sync method |
| *WS2812B Clock OTA* | Base URL, model directory, token, certificate source, check-at-boot and interval, `OTA_PROGRESS_STEP_PCT`, `OTA_UPLOAD_COREDUMP`, `OTA_CRASH_URL` |
| *WS2812B Clock Remote Logging* | Collector host and UDP port, syslog APP-NAME, line length, queue depth, core dump reporting |
| *WS2812B Clock Debugger* | `DEBUG_BUILD` logging, status LED pin |
| *WS2812B Clock Fans* | Fan PWM pin |

Shipped defaults: ring on GPIO32 (60 px) at 5% day / 1% night, digits on
GPIO25 (28 px) at 50% day / 5% night. Brightness is worth tuning per build
once the diffusers are in — the values in a local `sdkconfig` win over these.

### Device identity

One value names the device, and everything follows it:

```
CONFIG_LWIP_LOCAL_HOSTNAME="hallway-clock"
```

It is not under any of this project's own menus — it lives in menuconfig under
**Component config → LWIP → Local netif hostname**, and defaults to
`espressif`, which is worth changing before a second device exists.

That one setting becomes the DHCP name on the network, the `host` label in
Loki, and the name a crash dump is filed under on the server. Nothing else
sets it: `logship` and the crash upload both read it back from the station
interface through `wifi_hostname()`, so they cannot disagree.

The distinction matters once there is more than one device, and it follows
RFC 5424:

| Field | Means | Set by | Example |
| --- | --- | --- | --- |
| HOSTNAME | which **device** | `CONFIG_LWIP_LOCAL_HOSTNAME` | `hallway-clock` |
| APP-NAME | which **firmware** | `CONFIG_LOGSHIP_APP_NAME` | `ws2812bclock` |

So every clock running this firmware shares an APP-NAME and has its own
HOSTNAME. `{app="ws2812bclock"}` selects the fleet; `{host="hallway-clock"}`
selects one of them. The same shape works for any other device built on this
pattern.

### Site-specific settings and backing them up

Three things are not in this repository and never should be: the WiFi
credentials, the OTA token, and the update server's root CA. The first two, plus
the update server and log collector addresses, belong in
**`sdkconfig.defaults.local`** — gitignored, and the one file worth backing up.
Copy `sdkconfig.defaults.local.example` and fill it in.

```bash
cp sdkconfig.defaults.local.example sdkconfig.defaults.local
$EDITOR sdkconfig.defaults.local
```

`CMakeLists.txt` appends it to `SDKCONFIG_DEFAULTS` when it exists, so a fresh
clone configures itself. Note what that does and does not do: these are
*defaults*. An existing `sdkconfig` overrides them, so editing this file after
the fact changes nothing until `sdkconfig` is regenerated —
`rm sdkconfig && idf.py reconfigure`. What it buys you is that a lost or reset
`sdkconfig` costs one command instead of an evening of remembering what was in
which menu.

The firmware also complains at boot, at warning level, if it has no update
server or update checking is off — a clock that silently cannot update itself
looks perfectly healthy from across the room.

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

## The centre display

The two plates rotate through four readings, ten seconds each by default
(`DISPLAY_CYCLE_SECONDS`):

| Phase | Shows | Example |
| --- | --- | --- |
| Time | `HH MM` | `18 19` |
| Date | `DD MM` | `01 10` |
| Temperature | `NN °C` | `21 °C` |
| Humidity | `NN rH` | `47 rH` |

With no sensor fitted it rotates through time and date only.

The phase comes from seconds since midnight rather than `tm_sec`, so it does not
jump at the top of each minute. At the default ten seconds the full cycle is 40
seconds and 86400 divides exactly by that, so midnight passes without a step
either. Other values work, they just stutter once a day.

A temperature or humidity the sensor has not supplied shows as dashes rather
than a stale number. A sensor that has stopped answering should be visible on
the clock face, not quietly skipped.

## Environment sensor

A DHT11 on a single data line, read every `DHT_INTERVAL_S` seconds (default 30).
The part is specified to 1 °C and 1 % and updates about once a second at best,
so there is nothing to gain from reading it more often.

**The pin must be output capable.** The protocol is bidirectional on one wire:
the host pulls the line low for about 20 ms to request a reading, then releases
it and listens. **GPIO34–39 cannot be used** — they are input only on the ESP32,
which is why `DHT_GPIO` stops at 33. The line idles high and needs an external
4.7–10 kΩ pull-up to the sensor's supply; the internal pull-up is enabled as a
backstop so a missing resistor shows up as flaky readings rather than silence.

Keep its wire away from the two LED data runs.

### Why it does not disable interrupts

The timing is tight — a bit is a 50 µs low followed by roughly 27 µs of high for
a zero or 70 µs for a one — and the usual way to read one of these is to mask
interrupts for the whole 4 ms exchange. That would be the wrong trade here: it
would disturb the RMT refills driving the LEDs and upset WiFi, and this build
has had quite enough trouble with marginal LED timing already.

Instead the driver times with `esp_timer_get_time()` and leaves interrupts
alone. A read that gets preempted mid-bit fails the sensor's own checksum and is
retried. At one reading every thirty seconds, retries are free, and the result
is self-correcting rather than timing-fragile.

## Remote logging

A clock on a wall has no serial cable, so every log line is also copied to a
syslog collector over UDP. Set the collector in `idf.py menuconfig` →
*WS2812B Clock Remote Logging*; with no host configured the component does
nothing and costs nothing.

```
CONFIG_LOGSHIP_HOST="logs.example.lan"
CONFIG_LOGSHIP_PORT=5514
```

The device sends **RFC 5424** frames, one datagram per line, facility `local0`,
severity taken from the ESP-IDF level letter. Before the first NTP sync the
timestamp field is `-`, which tells the collector to stamp the line on arrival
rather than filing boot messages in 1970. HOSTNAME is the device and APP-NAME
is the firmware — see *Device identity* below.

Watch the stream with nothing but netcat, before any collector exists:

```bash
nc -ul 5514
```

Frames end in a newline (RFC 6587 non-transparent framing). Receivers choose
their parser from the first byte — `<` means newline-delimited — and then treat
the datagrams from one sender as a single stream, so the terminator is what
keeps consecutive messages apart.

Any syslog receiver works. As an example, a promtail scrape config:

```yaml
scrape_configs:
  - job_name: ws2812bclock
    syslog:
      listen_address: 0.0.0.0:5514
      listen_protocol: udp          # defaults to tcp
      use_incoming_timestamp: true
      labels:
        job: ws2812bclock
    relabel_configs:
      - source_labels: [__syslog_message_hostname]
        target_label: host
      - source_labels: [__syslog_message_severity]
        target_label: level
      - source_labels: [__syslog_message_app_name]
        target_label: app
```

The relabelling is not optional, whichever receiver you use: both promtail and
Alloy drop every `__`-prefixed label, so without it the logs arrive with no
hostname and no severity and one clock cannot be told from another. Alloy's
`loki.source.syslog` takes the same settings under different names — `address`
and `protocol` rather than `listen_address` and `listen_protocol` — and moves
relabelling to a `relabel_rules` attribute, so the config is not a
transliteration.

### What it costs, and what it drops

The log hook runs on whichever task called `ESP_LOGx`, so it never sends
anything itself: it formats the line, copies it into a queue and returns. A
sender task does the network work. If the collector is unreachable the lines
stay queued — so the boot messages survive the wait for WiFi — and once the
queue is full the newest lines are dropped and counted, with the count sent
once the link comes back. The clock never blocks or stalls because logging
cannot get out.

The queue is the only real cost: `LOGSHIP_QUEUE_DEPTH` x `LOGSHIP_LINE_MAX`
from the heap (about 6 KB at the defaults) plus a 4 KB sender task.

Volume is controlled by `DEBUG_BUILD`, which is what it is for. With it set,
the tick loop logs once a second, which is far too much to ship. With it
clear, a working clock is nearly silent: a heartbeat every five minutes with
uptime and heap figures, plus whatever actually goes wrong.

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

After a crash, the next boot logs the dump's summary — faulting task, program
counter, `EXCCAUSE`, and a backtrace — once the network is up, so it arrives at
the collector like any other log line and the clock never has to be unplugged
to find out what happened. Paste the backtrace into `addr2line` against the
matching build, which the report identifies by its app ELF SHA256.

With `OTA_UPLOAD_COREDUMP` set, the dump itself is then posted to the crash
receiver in `server/crashd` and erased once the server has taken it. Erasing
only on success makes it exactly-once: a clock that cannot reach the server
keeps its dump and retries on the next boot. Without it the dump stays in flash
and `idf.py coredump-info` over USB is the way to get a full trace.

### Symbols are published with every release

A core dump is undecodable without the exact ELF that produced it, and `build/`
is overwritten by the next build. `deploy-firmware.sh` therefore publishes
`<model>-<version>.elf.gz` alongside each image and names it in `latest.json`.
The crash report logs the crashing build's `app_elf_sha256`, so the report
identifies which one it needs — it just has to still exist.

To decode a dump against a release that is no longer in `build/`:

```bash
scp ota:/srv/ota/<model>/<model>-<version>.elf.gz .
gunzip <model>-<version>.elf.gz
esp-coredump info_corefile -c <dump> <model>-<version>.elf
```

The ELF is about ten times the size of the image it describes and compresses
roughly three to one, so releases cost a few megabytes each on the update
server. Old images and symbols are never cleaned up; that retention policy is
still to be decided.

## The crash receiver — `server/crashd`

### Why it exists

Everything else the clock says travels as syslog over UDP, which is lossy on
purpose: a clock must never stall because logging cannot get out. A core dump
cannot travel that way. It is a binary ELF of tens of kilobytes that has to
arrive byte-perfect, and a log store has nothing useful to do with it — you
cannot search or aggregate a core dump, and base64 in a text index would cost a
third more for nothing. So it needs its own path, and that path needs something
at the far end to catch it.

### Why not just let Caddy accept the upload

Caddy's standard build has no upload handler, so this would mean rebuilding it
with `xcaddy` and a WebDAV module. That was rejected deliberately.

The endpoint is reachable from the network the devices are on. Firmware
images are signed, so
nobody can install a malicious one — but **`latest.json` is not signed**, and
anything that can write to the firmware tree can pin the whole fleet to an old
version or simply fill the disk. Scoping a general-purpose write module
tightly enough to prevent that is more thinking than a service whose only
ability is to create one new file per request, under a name it chooses itself,
in a directory that is not the firmware one.

### How it operates

A single static Go binary. No dependencies, no database, no TLS of its own.

```
POST /crash/{model}
  X-OTA-Token         the same shared token the firmware sends for updates
  X-Device-Hostname   which clock
  X-Firmware-Version  what it was running
  X-App-Elf-Sha256    identifies the build whose symbols decode this dump
  body                the coredump partition, verbatim
```

It listens on localhost and sits behind the existing Caddy terminator, so
devices reach it on the same host, with the same certificate and token as
update checks — which also means it needs no firewall rule of its own.

Stored as `{dir}/{model}/{hostname}-{version}-{unix}.elf`. The server picks
that name; nothing a client sends reaches a filesystem path without matching
`^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$` first. Uploads are written to a temporary
file and renamed, so a transfer that dies half way never looks like a complete
dump. The token is compared in constant time, and the body is capped at twice
the partition size.

On the device, the dump is streamed in 1 KB chunks from its own task, and
**erased only on a 2xx**. That makes the whole thing exactly-once and
self-limiting: a clock that cannot reach the server keeps its dump and tries
again on the next boot, and a crash loop uploads one dump per crash rather than
the same one repeatedly.

### Deploying it

Releases are built by `.github/workflows/release-crashd.yml` on a `crashd-v*`
tag — tagged separately from the firmware, which is versioned through
`CONFIG_APP_PROJECT_VER` rather than git tags — and attached as a static
`linux/amd64` binary with a `.sha256` beside it. The host needs no Go
toolchain.

It runs alongside the update server, deployed by whatever manages that host.
**This repository owns the code and the wire format; the infrastructure that
hosts it owns how it runs** — the service unit, the reverse proxy route,
directory ownership. The split is deliberate: the receiver's contract is the
firmware's, so those two change together, in one commit, here.

The one constraint that matters when deploying it: its storage directory must
not sit under the firmware tree, and the unit should have no write access to
`/srv/ota` at all. That is the whole reason it exists in this shape.

`server/crashd/README.md` has the full interface, flags and decode recipe.

## Layout

```
components/
├── display/   both LED chains, position maps, font, brightness
├── ntp/       SNTP, timezone, NTP_EVENT, ntp_time_is_valid()
├── wifi/      station mode, retries forever with back-off
├── ota/       manifest check, HTTPS update, rollback, crash dump upload
├── dht/       DHT11 temperature and humidity, sampled in the background
├── fan/       LEDC PWM fan control with kick-start
├── logship/   syslog-over-UDP log shipping, crash reports on boot
└── debug/     activity LED on serial traffic
main/          startup, the second-aligned tick loop, rendering
CAD/           jigs, clock geometry, KiCad project
scripts/       signing key and release helpers
server/crashd/ core dump receiver, deployed beside the OTA server
.github/       CI for server/crashd, and its tagged release build
```
