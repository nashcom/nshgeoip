# GeoIP Sync

`geoip-sync` keeps a local GeoIP database directory synchronized with an internal GeoIP HTTP mirror.

It is intended for servers that should not download directly from MaxMind and do not need MaxMind credentials. The service uses a small Alpine container and the included `geoip-sync.sh` script.

## Features

- Downloads GeoLite2 ASN, City, and Country databases from an internal mirror
- Checks periodically and downloads only changed databases
- Verifies every download using SHA-256
- Verifies the original MaxMind MD5 checksum
- Downloads the checksum and metadata sidecars
- Installs files atomically using temporary files and `mv`
- Publishes the SHA-256 sidecar last as the completion marker
- Preserves the mirror's HTTP `Last-Modified` timestamp on the local MMDB
- Requires no MaxMind account or license key

## Files

```text
.
├── compose.yaml
└── geoip-sync.sh
```

The synchronized directory contains:

```text
GeoLite2-ASN.mmdb
GeoLite2-ASN.mmdb.md5
GeoLite2-ASN.mmdb.sha256
GeoLite2-ASN.mmdb.metadata.json

GeoLite2-City.mmdb
GeoLite2-City.mmdb.md5
GeoLite2-City.mmdb.sha256
GeoLite2-City.mmdb.metadata.json

GeoLite2-Country.mmdb
GeoLite2-Country.mmdb.md5
GeoLite2-Country.mmdb.sha256
GeoLite2-Country.mmdb.metadata.json
```

## Compose configuration

```yaml
services:

  geoip-sync:
    image: alpine:3.22
    container_name: geoip-sync
    hostname: geoip-sync
    restart: unless-stopped
    network_mode: host

    environment:
      GEOIP_SYNC_SOURCE_URL: "${GEOIP_SYNC_SOURCE_URL:?GEOIP_SYNC_SOURCE_URL is required}"
      GEOIP_SYNC_EDITION_IDS: "${GEOIP_SYNC_EDITION_IDS:-GeoLite2-ASN GeoLite2-City GeoLite2-Country}"
      GEOIP_SYNC_INTERVAL: "${GEOIP_SYNC_INTERVAL:-300}"
      GEOIP_SYNC_DB_DIR: /var/lib/GeoIP

    entrypoint: ["/bin/sh", "/usr/local/bin/geoip-sync.sh"]

    volumes:
      - ./geoip-sync.sh:/usr/local/bin/geoip-sync.sh:ro
      - /var/lib/GeoIP:/var/lib/GeoIP

    read_only: true

    security_opt:
      - no-new-privileges:true
```

Host networking is useful when the mirror is available through a service bound to the host's loopback interface. It can be removed when the mirror is reachable normally through DNS or an IP address.

## Configuration

Create `.env`:

```text
GEOIP_SYNC_SOURCE_URL=http://geoip-mirror.example.internal:81
```

Optional variables:

| Variable | Default | Description |
|---|---:|---|
| `GEOIP_SYNC_EDITION_IDS` | `GeoLite2-ASN GeoLite2-City GeoLite2-Country` | Space-separated database editions |
| `GEOIP_SYNC_INTERVAL` | `300` | Seconds between checks; use `0` for a one-time synchronization |
| `GEOIP_SYNC_DB_DIR` | `/var/lib/GeoIP` | Local database directory inside the container |

## Start the service

```bash
sudo mkdir -p /var/lib/GeoIP
docker compose up -d
```

Follow the log:

```bash
docker compose logs -f geoip-sync
```

A successful initial synchronization looks like:

```text
Checking GeoLite2-Country
Downloading GeoLite2-Country
Updated GeoLite2-Country
Preserved modification time: Tue, 25 Aug 2026 16:45:49 GMT
```

Subsequent checks report unchanged databases without downloading the MMDB again:

```text
GeoLite2-Country is current
```

## Verification

Verify a synchronized database:

```bash
cd /var/lib/GeoIP
sha256sum --check GeoLite2-Country.mmdb.sha256
md5sum --check GeoLite2-Country.mmdb.md5
```

Compare the preserved local modification time with the mirror:

```bash
stat /var/lib/GeoIP/GeoLite2-Country.mmdb
curl -I http://geoip-mirror.example.internal:81/GeoLite2-Country.mmdb
```

The local `Modify` time should represent the same instant as the HTTP `Last-Modified` header. HTTP dates are displayed in GMT, while `stat` normally displays the host's local timezone.

## Update behavior

For every database, `geoip-sync`:

1. Downloads the remote SHA-256 sidecar.
2. Compares it with the local SHA-256 sidecar.
3. Downloads the MMDB when the checksum has changed.
4. Verifies the downloaded MMDB using SHA-256.
5. Downloads and verifies the original MaxMind MD5 sidecar.
6. Downloads the metadata JSON.
7. Applies the HTTP `Last-Modified` timestamp to the MMDB.
8. Moves the verified files into place.
9. Publishes the SHA-256 sidecar last.

If any download or checksum verification fails, the existing database remains in place and the synchronization is retried during the next cycle.

## Using the databases

Applications can mount the synchronized directory read-only:

```yaml
volumes:
  - /var/lib/GeoIP:/var/lib/GeoIP:ro
```

## Attribution

This product includes GeoLite data created by MaxMind, available from [https://www.maxmind.com](https://www.maxmind.com).
