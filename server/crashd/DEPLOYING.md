# Deploying crashd

What the service needs from whatever runs it. The deployment itself — the
service manager unit, the reverse proxy route, directory ownership — belongs
with the infrastructure that hosts it. This file states the requirements that
deployment has to satisfy.

## Artifact

Built by `.github/workflows/release-crashd.yml` on a `crashd-v*` tag and
attached to the release as `crashd-linux-amd64`, static (`CGO_ENABLED=0`), with
a `.sha256` beside it. The host needs no Go toolchain.

Tagged separately from the firmware, which is versioned through
`CONFIG_APP_PROJECT_VER` rather than git tags.

## Invocation

```
crashd -addr 127.0.0.1:8099 \
       -dir /srv/crash \
       -token-file /etc/crashd/token \
       -max-bytes 131072
```

`CRASHD_ADDR`, `CRASHD_DIR` and `CRASHD_TOKEN_FILE` are equivalent environment
variables. There is no default token and the service refuses to start without
one.

## Requirements

**Bind to loopback.** It speaks plain HTTP and knows nothing about
certificates. It goes behind the same TLS terminator that already serves the
update endpoint, so devices reach it on a host and certificate they already
trust.

**Run it unprivileged.** It needs write access to the storage directory and
read access to the token file. Nothing else.

**The storage directory must not sit inside the firmware tree, and the service
must have no write access to that tree.** This is the constraint the whole
design exists to satisfy. The endpoint is reachable from the network the
devices are on. Firmware images are signed, so a bad image cannot be installed
— but `latest.json` is not signed, and anything able to write to the firmware
directory can pin every device to an old version or fill the disk. That is why
this is a service that can only create one new file per request, under a name
it chooses itself, rather than a general-purpose upload module.

With systemd, `ProtectSystem=strict` and `ReadWritePaths=<storage dir>` express
this well.

**Token.** The same shared secret the firmware sends as `X-OTA-Token` for
update checks. The file should be mode 0400 and owned by the service user.

**Logging** goes to stdout, so whatever collects the service manager's journal
picks it up. One line per stored dump, including its size and the
`app_elf_sha256` identifying which build's symbols decode it.

## Reverse proxy

Route `/crash/*` to the listen address. With Caddy:

```
reverse_proxy /crash/* 127.0.0.1:8099
```

No authentication is needed at that layer — crashd compares the token itself,
in constant time. Checking it at the proxy as well is harmless, but the header
must still reach the backend.

`GET /healthz` returns `ok` and needs no token, which makes it a reasonable
health check.

## Network

If it sits behind the same terminator, on the same host and port the devices
already use for updates, no new firewall rule is needed. Anywhere else needs
the equivalent of the rule that lets devices reach the update endpoint.

## Sizing

A dump is at most 64 KB — the size of the coredump partition — and one is
produced only when a device actually crashes. The default 128 KB cap is twice
the partition; anything larger is not a dump.

Nothing prunes the storage directory. Retention is a decision for whoever
operates it.

## Enabling the token, in the right order

The firmware only sends `X-OTA-Token` when `CONFIG_OTA_AUTH_TOKEN` is
non-empty. crashd always requires it.

The two endpoints are not equally risky, and separating them makes this safe:

- **crashd can require the token from the moment it is deployed.** Nothing
  depends on the upload path yet, so there is no existing behaviour to break.
- **The update endpoint is the one to be careful with.** Publish firmware that
  sends the token and confirm every device is running it *before* requiring it
  there. Doing it the other way round means devices can no longer fetch
  updates, and recovering from that needs physical access to each one.

For more than one device, "every device" is the operative part: a single clock
still on an older build is one that has to be taken off the wall. The device's
own logs are the check — each reports its version at boot.

If enforcement does go wrong, removing it at the proxy restores updates
immediately and without touching any device, so that is the rollback.

A consequence worth recording: once the token is required, any machine that
builds firmware needs it, or the devices it builds cannot update themselves.
It belongs with the signing key, not in a tracked file.

## Verifying a deployment

```bash
# expected: 403
curl -X POST -H 'X-OTA-Token: wrong' --data-binary 'x' \
     https://<host>/crash/<model>

# expected: 201, and a file on disk
curl -X POST -H "X-OTA-Token: $TOKEN" \
     -H 'X-Device-Hostname: testhost' -H 'X-Firmware-Version: 0.0.0' \
     --data-binary 'not-a-real-dump' \
     https://<host>/crash/<model>
```

The second returns
`{"stored":"<model>/testhost-0.0.0-<unix>.elf","bytes":15}` and leaves that
file behind; delete it afterwards.

Nothing arrives from a real device until `OTA_UPLOAD_COREDUMP` is enabled in
the firmware with `OTA_CRASH_URL` pointing at this endpoint.
