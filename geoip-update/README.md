# GeoIP Update Mirror

This helper stack downloads the current MaxMind GeoLite2 databases and makes them available through an internal NGINX mirror.

It uses the official `geoipupdate` container for downloading and validation. A custom entry point additionally preserves the original MaxMind MD5 checksum and creates a SHA-256 checksum for every database.

## Features

- Uses the official MaxMind `geoipupdate` binary and download API
- Downloads GeoLite2 ASN, City, and Country databases
- Runs immediately and checks for updates periodically
- Validates each database against the MD5 supplied by MaxMind
- Publishes individual MD5 and SHA-256 checksum files
- Publishes database metadata as JSON
- Serves the files through an internal NGINX mirror
- Provides directory browsing and a health endpoint
- Keeps MaxMind credentials outside the Compose environment

## Directory layout

```text
.
├── compose.yaml
├── entrypoint.sh
├── nginx.conf
└── secrets/
    └── maxmind.conf
```

## Credentials

Create `secrets/maxmind.conf`:

```sh
GEOIPUPDATE_ACCOUNT_ID='123456'
GEOIPUPDATE_LICENSE_KEY='your-license-key'
```

Protect the directory and file:

```bash
chmod 700 secrets
chmod 600 secrets/maxmind.conf
```

Do not commit the credentials:

```gitignore
secrets/
```

The directory is mounted read-only at `/run/secrets` inside the updater container.

## Start the stack

Create the database directory and start the containers:

```bash
sudo mkdir -p /var/lib/GeoIP
docker compose up -d
```

Follow the updater log:

```bash
docker compose logs -f geoipupdate
```

Check the downloaded files:

```bash
ls -lh /var/lib/GeoIP
```

## Published files

For every configured database, the mirror publishes:

```text
GeoLite2-Country.mmdb
GeoLite2-Country.mmdb.md5
GeoLite2-Country.mmdb.sha256
GeoLite2-Country.mmdb.metadata.json
```

The `.md5` value is obtained from MaxMind and verified against the downloaded MMDB. The `.sha256` value is generated locally after successful verification.

## Mirror access

Open the directory index:

```text
http://localhost:8081/
```

Health check:

```bash
curl -fsS http://localhost:8081/health
```

Download and verify one database:

```bash
curl -fsSLO http://localhost:8081/GeoLite2-Country.mmdb
curl -fsSLO http://localhost:8081/GeoLite2-Country.mmdb.sha256
sha256sum --check GeoLite2-Country.mmdb.sha256
```

The mirror should be restricted to trusted internal networks. If it is accessed across an untrusted network, use HTTPS.

## Using the databases

Other containers can mount the shared database directory read-only:

```yaml
volumes:
  - /var/lib/GeoIP:/var/lib/GeoIP:ro
```

## Attribution

This product includes GeoLite data created by MaxMind, available from [https://www.maxmind.com](https://www.maxmind.com).
