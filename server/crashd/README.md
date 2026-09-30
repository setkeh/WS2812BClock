# crashd — core dump receiver

Receives ESP32 core dumps from the clocks and writes them to disk.

It lives in this repository rather than the infrastructure one because its wire
format *is* the firmware's: the two change together, so they version together.
The infrastructure repository owns how it runs — the systemd unit, the Caddy
route, directory ownership and the firewall.

## Why not a general-purpose upload module

This endpoint is reachable from the IoT VLAN. The firmware images it sits
beside are signed, so nobody can install a malicious one — but `latest.json` is
**not** signed, and anything with write access to that tree can pin the fleet to
an old version or fill the disk. A WebDAV module scoped tightly enough to
prevent that is more thinking than this is.

What this can do is create one new file per request, under a name it chooses
itself, in a directory that should not be the one serving firmware.

## Interface

```
POST /crash/{model}
  X-OTA-Token         shared token, the same one the firmware sends for updates
  X-Device-Hostname   which clock (optional, defaults to "unknown")
  X-Firmware-Version  what it was running (optional)
  X-App-Elf-Sha256    identifies the build whose symbols decode this dump
  body                the raw coredump partition, verbatim

201 Created  {"stored": "model/name.elf", "bytes": 13344}
400          malformed model, empty body, or a truncated upload
403          wrong or missing token
413          larger than -max-bytes
```

`GET /healthz` returns `ok`, for a systemd or monitoring check.

Stored as `{dir}/{model}/{hostname}-{version}-{unix}.elf`. The server picks the
filename; nothing a client sends reaches a path without being matched against
`^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$` first. Uploads are written to a temporary
file and renamed, so a partial transfer never looks like a complete dump.

## Running it

```
crashd -addr 127.0.0.1:8099 \
       -dir /srv/crash \
       -token-file /etc/crashd/token \
       -max-bytes 131072
```

`CRASHD_ADDR`, `CRASHD_DIR` and `CRASHD_TOKEN_FILE` work as environment
variables too. There is no default token and the service refuses to start
without one.

Bind to localhost and put it behind the existing TLS terminator; it speaks
plain HTTP and knows nothing about certificates. The devices reach it at the
same host, with the same CA and token as update checks.

## Decoding what it stores

The uploaded bytes are the coredump partition verbatim, which `esp-coredump`
reads directly. It needs the ELF from the *exact* build that crashed —
published beside each release, and named by the `app_elf_sha256` in the crash
report:

```bash
scp ota:/srv/ota/<model>/<model>-<version>.elf.gz .
gunzip <model>-<version>.elf.gz
esp-coredump info_corefile -c <dump>.elf -t raw <model>-<version>.elf
```

## Deploying

See `DEPLOYING.md` for what the service needs from its host, including the
constraint that its storage directory must not sit inside the firmware tree.

## Building

```bash
cd server/crashd
go test ./...
CGO_ENABLED=0 GOOS=linux GOARCH=amd64 go build -trimpath -o crashd .
```

Releases are built by `.github/workflows/release-crashd.yml` on a `crashd-v*`
tag and attached as a static `linux/amd64` binary with a `.sha256` beside it,
so the target host needs no Go toolchain.
