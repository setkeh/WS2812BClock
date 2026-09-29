# OTA server certificate

Only needed when `OTA_CERT_SOURCE_EMBEDDED` is selected in menuconfig.

Put the PEM the device should trust here as `server_ca.pem`. Prefer your own
**CA** certificate over the server's own cert: server certs can then be
reissued without reflashing every clock, which matters because a device that
cannot trust the server cannot receive the update that would fix it.

The file is gitignored — it is deployment-specific, not project source.

    # one-off CA, long expiry
    openssl req -x509 -newkey rsa:4096 -keyout ca.key -out server_ca.pem \
        -days 3650 -nodes -subj "/CN=WS2812BClock OTA CA"

The server certificate must be issued by that CA, and its CN/SAN must match
the host in `OTA_UPDATE_URL`.
