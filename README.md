# nshgeoip

A small, local GeoIP lookup daemon for Linux. It answers `GET /lookup?ip=<address>` over a UNIX domain socket (and,optionally, TCP)
See [TCP listener](#tcp-listener)) using MaxMind MMDB databases (via `libmaxminddb`), so NGINX (or any other local process)
can annotate a request with country/ASN/city information via an `auth_request` subrequest.

`nshgeoip` performs **annotation only**. It never decides whether an IP address should be allowed or blocked.

```text
IP address -> nshgeoip -> local MMDB lookup -> GeoIP annotation
```

## Contents

- [Architecture](#architecture)
- [Dependencies](#dependencies)
- [Compilation](#compilation)
  - [Static Alpine build](#static-alpine-build)
- [Configuration](#configuration)
- [Installation](#installation)
- [Management script](#management-script)
- [systemd setup](#systemd-setup)
- [UNIX socket permissions](#unix-socket-permissions)
- [TCP listener](#tcp-listener)
- [HTTP API](#http-api)
- [Health and metrics](#health-and-metrics)
- [curl examples](#curl-examples)
- [NGINX auth_request example](#nginx-auth_request-example)
- [IPv6 support](#ipv6-support)
- [Concurrency model](#concurrency-model)
- [Signals](#signals)
- [Security considerations](#security-considerations)
- [Testing](#testing)
- [Attribution](#attribution)

## Architecture

```text
                 GeoLite2 MMDB
                       |
                 libmaxminddb
                       |
                 +------------+
                 |  nshgeoip  |
                 +------------+
                       |
                  UNIX socket
                       |
          +------------+------------+
          |            |            |
        NGINX       local app     curl/test
```

(An optional TCP listener can stand in for the UNIX socket above -- see [TCP listener](#tcp-listener) -- for a client
that can't reach a UNIX socket path at all, e.g. running in a different container/network namespace.)

The MMDB databases are opened once at startup with `libmaxminddb`'s normal memory-mapped access and kept open for the
daemon's lifetime -- there is no per-request reopening and no application-level cache on top; the mmap'd lookup is
already fast. `SIGHUP` re-opens the configured databases and atomically swaps them in (see [Signals](#signals)) without
restarting the process or interrupting in-flight requests.

`GeoLite2-City.mmdb` is supported alongside Country and ASN (`city_db` in the config) -- city name, postal code,
latitude, longitude, and accuracy radius. City fields are always sourced from `city_db` specifically, never inferred
from `country_db`, even though a real `GeoLite2-City.mmdb` also carries country/continent data too: whenever
`country_db` is configured it stays authoritative for those fields, so two independently-updated databases can't
silently disagree. But if `country_db` isn't configured at all, country/continent fall back to `city_db`'s own
fields instead, since MaxMind's City schema already includes them -- so a `city_db` + `asn_db` setup (no
`country_db` needed) still gets full country/continent/city/ASN annotation. That's enough for consumers like
CrowdSec, which only care about city and ASN.

## Dependencies

- A C++17 compiler (`g++`)
- [`libmaxminddb`](https://github.com/maxmind/libmaxminddb) (library + development headers)
- POSIX threads (`pthread`)
- Linux (uses `signalfd`, `accept4`; not portable to other platforms by design)

No Boost, no HTTP framework, no JSON library, no Prometheus client library -- `nshgeoip` implements just enough HTTP/1.1
parsing/serialization, JSON output, and Prometheus text exposition format for its three endpoints (`/lookup`, `/health`,
`/metrics`).

On Debian/Ubuntu:

```bash
sudo apt-get install build-essential libmaxminddb-dev
```

On RHEL/Fedora:

```bash
sudo dnf install gcc-c++ libmaxminddb-devel
```

## Compilation

```bash
make
```

Produces the `nshgeoip` binary in the project root. `make test` builds and runs the unit-test binary (`tests/test_nshgeoip`, see [Testing](#testing)).

```bash
make clean
```

### Static Alpine build

```bash
./build.sh                    # build the image, extract the binary to ./nshgeoip
./build.sh ./out/nshgeoip     # ...to a specific path instead

IMAGE_TAG=myregistry/nshgeoip:1.0 ./build.sh   # override the image tag
```

Builds a fully static (musl, no shared `libmaxminddb`) `nshgeoip` binary using Alpine's own `libmaxminddb-static`
package, inside a multi-stage `Dockerfile`. `build.sh` runs `docker build`, then copies the resulting binary out of
the image to disk -- the image itself (`FROM alpine ... AS runtime`, running as a non-root `nshgeoip` user, no
package repo needed at all since the binary is static) is also usable directly as a container without extracting
anything. The actual compile/link steps live in [docker/compile_alpine_static.sh](docker/compile_alpine_static.sh)
(a plain `sh` script, run inside the build stage), not inline in the Dockerfile.

A static musl binary has no runtime dependency on `libmaxminddb` (or glibc) being installed at all, which is useful
for copying `nshgeoip` onto a minimal/distroless target or a system where you'd rather not add package-manager
dependencies just to run one small daemon.

## Configuration

`nshgeoip` reads a simple `key=value` file, by default `/etc/nshgeoip/nshgeoip.conf` (override with `--config PATH`). See
[etc/nshgeoip.conf.example](etc/nshgeoip.conf.example):

```ini
country_db=/var/lib/GeoIP/GeoLite2-Country.mmdb
asn_db=/var/lib/GeoIP/GeoLite2-ASN.mmdb
city_db=/var/lib/GeoIP/GeoLite2-City.mmdb
socket=/run/nshgeoip/nshgeoip.sock
socket_mode=0660
#threads=8   # default: CPU core count, clamped to 4-20
max_request_bytes=8192
#debug_log=false
```

- Blank lines and lines starting with `#` are ignored.
- At least one of `country_db` / `asn_db` / `city_db` must be set; any of them can be omitted (or commented out) to
  disable that lookup type.
- Unknown keys are ignored but logged as a warning at startup, so the format can grow without breaking old config files.
- `socket_mode` is octal (matches `chmod` notation, e.g. `0660`).

Command-line options:

```text
nshgeoip --config /etc/nshgeoip/nshgeoip.conf              # use a specific config file
nshgeoip --check-db /var/lib/GeoIP/GeoLite2-City.mmdb      # print an .mmdb file's own metadata and exit
nshgeoip --check-db PATH --format json                     # ...as one JSON object instead
nshgeoip --check-db PATH --format ini                      # ...as "key=value" lines instead
nshgeoip --version                                         # print version and exit
nshgeoip --help                                            # print usage and exit
```

The config file is optional at its default path (`/etc/nshgeoip/nshgeoip.conf`) -- run with no config file at all and
`nshgeoip` falls back to built-in defaults plus whatever `NSHGEOIP_*` environment variables are set, which is the
intended way to configure it in a container. A `--config PATH` that's explicitly given and missing is still an error.
`nshgeoip --help` prints every config key alongside its environment variable, description, and default.

`--check-db PATH` is a standalone diagnostic mode, independent of any configured `country_db`/`asn_db`/`city_db` (or
even a config file at all): it opens the given `.mmdb` file directly and prints everything libmaxminddb's own metadata
exposes -- database type, IP version, binary format, node count, record size, build date, age in days, languages, and
description -- then exits. `--format table` (the default) is an aligned table for a human at a terminal; `--format
json`/`--format ini` are for scripting, and use libmaxminddb's own `MMDB_metadata_s` field names (`database_type`,
`ip_version`, `binary_format_major_version`, `node_count`, `record_size`, `build_epoch`, `languages`, `description`)
plus `build_date`/`age_days`, which the library doesn't itself provide (derived from `build_epoch`):

```json
{"path":"...","database_type":"GeoLite2-City","ip_version":6,"binary_format_major_version":2,
 "binary_format_minor_version":0,"node_count":6095709,"record_size":28,"build_epoch":1787323152,
 "build_date":"2026-08-21T14:39:12Z","age_days":3.8,"age_ms":330720000,"age_ns":330720000000000,
 "languages":["de","en","es","fr","ja","pt-BR","ru","zh-CN"],
 "description":"GeoLite2 City database",
 "description_list":[{"language":"en","description":"GeoLite2 City database"}]}
```

`age_days`/`age_ms`/`age_ns` are the same age in different units (days to one decimal place; whole milliseconds/
nanoseconds) -- `build_epoch` only has whole-second resolution to begin with, so `age_ms`/`age_ns` are exact unit
conversions of that same whole-second difference, not independently more precise measurements.

`description` is a convenience field: just the English text, which is what's wanted most of the time (empty string if
there's no `en` entry). `description_list` is the full list, as an array of `{language, description}` objects
rather than a `{lang: text}` map -- `MMDB_description_s` itself has exactly those two fields per entry, so this mirrors
the underlying struct shape directly. The `ini` format gets the same treatment: a plain `description=` line alongside
`description_<lang>=` for each language. `languages` stays a flat array, matching the struct's plain `const char **`
there.

The same build-date/age reporting also happens automatically for whichever databases are actually configured, logged
once at startup and again on every `SIGHUP` reload:

```text
opened city database: /var/lib/GeoIP/GeoLite2-City.mmdb (GeoLite2-City, built 2026-08-21T14:39:12Z, 3.8 days old)
```

### MaxMind database configuration

Download `GeoLite2-Country.mmdb`, `GeoLite2-ASN.mmdb`, and (optionally) `GeoLite2-City.mmdb` from your MaxMind account
(a free GeoLite2 account is enough) and point `country_db` / `asn_db` / `city_db` at wherever you keep them, e.g.
`/var/lib/GeoIP/`. The `nshgeoip` process user needs read access to those files, nothing more -- `nshgeoip` never writes to
them. [nshgeoipctl.sh download-db](#management-script) automates this download.

Any of the three left unset falls back, at startup, to `GeoLite2-{Country,ASN,City}.mmdb` under the first of
`/var/lib/GeoIP` (geoipupdate's own default location) or `/var/lib/crowdsec/data` (CrowdSec's bundled copy -- ASN/City
only, no Country) where that file actually exists -- see `nshgeoip --help` for the exact search order. Explicit
configuration (config file, environment variable) always wins; auto-detection only ever fills in a field left
completely unset, and each substitution it makes is logged at startup so it's never silent. `country_db`'s
auto-detection is skipped entirely once `city_db` ends up set (by config or by this same auto-detection), even if a
`GeoLite2-Country.mmdb` also exists at one of those locations -- a real `city_db` already carries the
`country_code`/`country_name`/`continent_code` fields (see [HTTP API](#http-api)), so opening a whole separate Country
database on top would add nothing.

## Installation

```bash
./nshgeoipctl.sh install
```

This builds `nshgeoip`, installs the binary to `/usr/local/sbin` (override with `PREFIX=/usr ./nshgeoipctl.sh install`),
creates the `nshgeoip` system user/group if missing, generates and installs the systemd unit, creates
`/etc/nshgeoip/nshgeoip.conf` from [etc/nshgeoip.conf.example](etc/nshgeoip.conf.example) if one doesn't already exist,
and enables + starts the service -- then prints the resulting config so you can see what to edit. Existing config files
and users/groups are left untouched on a re-run. See [Management script](#management-script) below for the rest of its
commands (`download-db`, `systemd <cmd>`, `status`, ...).

To install just the binary + an example config without any of that (e.g. for scripting your own setup), `make install`
still works on its own:

```bash
make install                # installs to /usr/local/sbin by default
# or: make install PREFIX=/usr
```

## Management script

[nshgeoipctl.sh](nshgeoipctl.sh) is the single entry point for installing, configuring, and operating `nshgeoip` --
run it from this checked-out source tree (it shells out to `make` for the actual build):

```text
./nshgeoipctl.sh install                    # build, install, create user/config, enable + start the service
./nshgeoipctl.sh config                     # show the current config file
./nshgeoipctl.sh download-db                # fetch/update GeoLite2-Country/-ASN/-City from MaxMind
./nshgeoipctl.sh download-db city           # fetch/update just GeoLite2-City
./nshgeoipctl.sh systemd restart            # start|stop|restart|enable|disable|status
./nshgeoipctl.sh status                     # same as: systemd status
./nshgeoipctl.sh version
```

`download-db` talks directly to the same `updates.maxmind.com` API [geoipupdate](https://github.com/maxmind/geoipupdate)
itself uses (`curl`/`tar`, Basic Auth with an account ID + license key) -- not the underlying `geoipupdate` binary, which
isn't packaged for Alpine and so isn't a dependency this project can lean on for its container image. It's safe (and
intended) to run repeatedly, e.g. from a cron job or timer: for each requested edition it checks whether a copy already
exists somewhere nshgeoip would find it that download-db doesn't itself manage (e.g. CrowdSec's bundled copy at
`/var/lib/crowdsec/data`) and leaves that alone entirely; otherwise, if it already owns a copy at its own managed
location, it MD5-checks that file against MaxMind's update endpoint and only replaces it if MaxMind reports something
newer (an HTTP 304 back means "already current," the same thing `geoipupdate -v` itself reports as "No new updates
available") -- so a database that's already up to date costs one small request, not a re-download. Only a
completely-missing edition triggers a full fresh download. Any archive actually received is SHA256-verified against
the checksum MaxMind publishes at the same download URL (`suffix=tar.gz.sha256` instead of `suffix=tar.gz`) before it's
extracted or installed -- a download that fails this check is rejected outright, not installed.

If `/etc/GeoIP.conf` (geoipupdate's own config file) exists, its `AccountID`/`LicenseKey`/`EditionIDs`/`DatabaseDirectory`
are read and reused automatically, so a system already set up for `geoipupdate` doesn't need anything entered twice.
Otherwise the account ID and license key come from `NSHGEOIP_MAXMIND_ACCOUNT_ID`/`NSHGEOIP_MAXMIND_LICENSE_KEY`, or an
interactive prompt (the license key is never echoed, logged, or printed) -- none of that is needed at all if every
requested edition turns out to already be available. Each `.mmdb` is installed at (or updated at) the path the nshgeoip
config file's `country_db`/`asn_db`/`city_db` already points at, or else `/etc/GeoIP.conf`'s `DatabaseDirectory`, or else
the documented `/var/lib/GeoIP/` default -- matching geoipupdate's own default database directory.

## systemd setup

`nshgeoipctl.sh install` generates the unit (see `generate_systemd_unit()` in the script) and runs `nshgeoip` as the
unprivileged `nshgeoip` user, with `RuntimeDirectory=nshgeoip` (so `/run/nshgeoip` is created/removed automatically) and
a fairly aggressive sandbox: `NoNewPrivileges`, `ProtectSystem=strict`, `ProtectHome`, `RestrictAddressFamilies=AF_UNIX`,
an empty `CapabilityBoundingSet`, and several more `Protect*`/`Restrict*` options. `RestrictAddressFamilies=AF_UNIX`
matches the default (TCP listener disabled) -- if you enable `tcp_port=` (see [TCP listener](#tcp-listener)), add
`AF_INET AF_INET6` to that line too, or the sandbox will kill `nshgeoip` the moment it tries to bind a TCP socket.
Adjust `SupplementaryGroups=nginx` to whatever group your local client(s) run as.

```bash
sudo systemctl status nshgeoip
sudo journalctl -u nshgeoip -f
sudo systemctl reload nshgeoip   # sends SIGHUP: reopens the MMDB files
```

## UNIX socket permissions

`nshgeoip` binds `socket` (default `/run/nshgeoip/nshgeoip.sock`) and `chmod`s it to `socket_mode` (default `0660`) right
after binding. Combined with the systemd unit's `User=nshgeoip` and the client's group membership (e.g. `nginx`), this
gives a working `nshgeoip:nginx 0660` setup out of the box. On startup, a leftover socket file from a previous run is
removed automatically -- but only if it actually is a socket (`S_ISSOCK`); if some other kind of file occupies that
path, `nshgeoip` refuses to start rather than deleting it.

On clean shutdown (`SIGTERM`/`SIGINT`), `nshgeoip` stops accepting new connections, lets in-flight requests finish, closes
the listening socket, and removes the socket file.

## TCP listener

The UNIX socket is always available; TCP is an off-by-default option for consumers that can't reach a UNIX socket path
(e.g. running in a different container/network namespace than `nshgeoip`). Enable it with `tcp_port` (config file,
`NSHGEOIP_TCP_PORT`, or `--tcp-port`... see below):

```ini
tcp_port=8080        # 0 or unset = disabled (default)
#tcp_address=127.0.0.1   # unset = bind loopback on both IPv4 (127.0.0.1) and IPv6 (::1)
```

With `tcp_address` unset, `nshgeoip` binds loopback on both address families (`IPV6_V6ONLY` is set on the IPv6 socket so
it doesn't also grab IPv4 as a side effect); setting `tcp_address` binds that one address only, instead of the loopback
default. Every config key (including `tcp_port`/`tcp_address`) has a matching `NSHGEOIP_<KEY>` environment variable that
overrides it -- see `nshgeoip --help` for the full list and precedence order (environment > config file > default).

Remember to update `RestrictAddressFamilies` in the systemd unit (see [systemd setup](#systemd-setup)) if you enable
this -- it's `AF_UNIX` only by default, matching TCP being off.

## HTTP API

```text
GET /lookup?ip=<address>
```

Successful response:

```json
{
  "ip": "8.8.8.8",
  "country_code": "US",
  "country_name": "United States",
  "continent_code": "NA",
  "asn": 15169,
  "as_org": "GOOGLE",
  "city_name": "Mountain View",
  "postal_code": "94043",
  "latitude": 37.4056,
  "longitude": -122.0775,
  "accuracy_radius": 1000
}
```

The `city_name`/`postal_code`/`latitude`/`longitude`/`accuracy_radius` fields come from `city_db` (see
[Configuration](#configuration)); if it isn't configured, they're always `null`.

### Content negotiation: JSON or plain text

Send `Accept: text/plain` to get the same data as `key=value` lines instead of JSON -- handy for a shell script that
would rather `grep`/`cut` than parse JSON:

```text
$ curl -sS -H 'Accept: text/plain' --unix-socket /run/nshgeoip/nshgeoip.sock \
    'http://localhost/lookup?ip=8.8.8.8'
ip=8.8.8.8
country_code=US
country_name=United States
continent_code=NA
asn=15169
as_org=GOOGLE
```

A missing field is simply an omitted line here (not a JSON-style `null` -- INI-style output has no native "null" to
write), and every value goes through the same CR/LF-stripping sanitization as the `X-GeoIP-*` headers, so an MMDB value
can't forge an extra `key=value` line. Error responses (400/404/405/500) become a single `error=...` line in this format
too. `Content-Type` is `application/json` or `text/plain; charset=utf-8` accordingly. No `Accept` header, or anything
other than `text/plain` (including `*/*`), defaults to JSON.

A field whose value is not available in the configured databases is `null` in the JSON body (the key is always present);
nshgeoip never invents a value. Response headers work the other way: a header is **omitted** entirely rather than sent
empty when there is nothing to put in it.

| Header                       | Present when                    |
|------------------------------|---------------------------------|
| `X-GeoIP-Country`            | `country_code` is available     |
| `X-GeoIP-Continent`          | `continent_code` is available   |
| `X-GeoIP-ASN`                | `asn` is available              |
| `X-GeoIP-AS-Org`             | `as_org` is available           |
| `X-GeoIP-City`               | `city_name` is available        |
| `X-GeoIP-Postal-Code`        | `postal_code` is available      |
| `X-GeoIP-Latitude`           | `latitude` is available         |
| `X-GeoIP-Longitude`          | `longitude` is available        |
| `X-GeoIP-Accuracy-Radius`    | `accuracy_radius` is available  |

All header values are stripped of CR/LF and other control characters before being written, so MMDB data (a downloaded
database file, still treated as untrusted input here) can never inject a header or split the response.

### Status codes

| Code | Meaning                                                                      |
|------|------------------------------------------------------------------------------|
| 200  | Successful lookup (may still have `null` fields)                             |
| 400  | Missing or syntactically invalid `ip` parameter, or a malformed HTTP request |
| 404  | Syntactically valid IP, but no GeoIP data for it, or an unknown path         |
| 405  | Method other than `GET`                                                      |
| 500  | Internal/database error                                                      |


`GET` and `HEAD` are implemented (anything else gets 405). `HEAD` returns the same status, `X-GeoIP-*`/`Allow` headers,
and `Content-Length` a `GET` for the same URL would, but without a body -- and without `Content-Type`, since there's no
body left to describe. This lets a client that only wants the `X-GeoIP-*` headers (e.g. `curl -I`) skip paying for or
parsing the JSON. Every response includes `Connection: close` (see [Concurrency model](#concurrency-model) for why).

## Health and metrics

`GET /health` and `GET /metrics` are always available -- no config flag turns them off; restrict access with NGINX (or
a firewall) if that's needed for your deployment. `/metrics` is never negotiated: always Prometheus text exposition
format, regardless of `Accept`.

`/health` is different on purpose: most container health checks (Docker `HEALTHCHECK`, Kubernetes
liveness/readiness probes) only ever look at the HTTP status code and rarely send an `Accept` header at all, so
`/health` defaults to a minimal text body -- unlike `/lookup`, where no `Accept` header means JSON. The full
per-database JSON detail is opt-in, for a human or a dashboard that explicitly asks for it:

```text
$ curl --unix-socket /run/nshgeoip/nshgeoip.sock 'http://localhost/health'
status=ok

$ curl -H 'Accept: application/json' --unix-socket /run/nshgeoip/nshgeoip.sock 'http://localhost/health'
{"status":"ok","version":"0.9.0","libmaxminddb_version":"1.9.1","uptime_seconds":12345,"databases":{
  "country":{"open":false},
  "asn":{"open":true,"database_type":"GeoLite2-ASN","build_epoch":1787559317,
          "build_date":"2026-08-24T08:15:17Z","age_days":1.1,"age_ms":97314000,"age_ns":97314000000000},
  "city":{"open":true,"database_type":"GeoLite2-City","build_epoch":1787323152,
          "build_date":"2026-08-21T14:39:12Z","age_days":3.9,"age_ms":330720000,"age_ns":330720000000000}}}
```

`status` is always `"ok"` -- nshgeoip has no other status to report today; if something were actually broken at startup
it wouldn't be running to answer this at all. The per-database `open`/`build_epoch`/`build_date`/`age_days`/`age_ms`/
`age_ns` fields are the useful part of the JSON body -- the same data `--check-db` and the startup log already show
(see [MaxMind database configuration](#maxmind-database-configuration)) -- so a dashboard or alert can watch `age_days`
directly instead of cross-referencing logs.

```text
$ curl --unix-socket /run/nshgeoip/nshgeoip.sock 'http://localhost/metrics'
# HELP nshgeoip_build_info nshgeoip build information.
# TYPE nshgeoip_build_info gauge
nshgeoip_build_info{version="0.9.0",libmaxminddb_version="1.9.1"} 1
# HELP nshgeoip_db_age_seconds Seconds since the database was built.
# TYPE nshgeoip_db_age_seconds gauge
nshgeoip_db_age_seconds{db="asn"} 97314
nshgeoip_db_age_seconds{db="city"} 330720
# HELP nshgeoip_requests_total Total HTTP requests received, by path.
# TYPE nshgeoip_requests_total counter
nshgeoip_requests_total{path="lookup"} 30
nshgeoip_requests_total{path="health"} 8
nshgeoip_requests_total{path="metrics"} 4
nshgeoip_requests_total{path="other"} 0
# HELP nshgeoip_http_responses_total HTTP responses sent, by status code.
# TYPE nshgeoip_http_responses_total counter
nshgeoip_http_responses_total{code="200"} 30
...
# HELP nshgeoip_lookup_results_total /lookup outcomes, by result.
# TYPE nshgeoip_lookup_results_total counter
nshgeoip_lookup_results_total{result="found"} 25
nshgeoip_lookup_results_total{result="not_found"} 5
```

`nshgeoip_requests_total` is split by path (`lookup`/`health`/`metrics`/`other`) so self-monitoring traffic -- health
checks, Prometheus scraping `/metrics` itself -- never gets conflated with real GeoIP lookup traffic; sum across labels
for a plain "total HTTP requests" figure. `other` covers an unknown path and a request whose path couldn't even be
determined (malformed, too large).

Request/response counters (`nshgeoip_requests_total{path=...}`, `nshgeoip_http_responses_total{code=...}`,
`nshgeoip_lookup_results_total{result=...}`) are plain `std::atomic<uint64_t>` with relaxed memory ordering,
incremented from whichever worker thread handles each connection (see [Concurrency model](#concurrency-model)) --
they're independent counters with no ordering relationship to any other memory a reader needs to observe alongside
them, so relaxed is sufficient and cheapest.

### Writing metrics to a file

`metrics_file` is a separate, optional setting: a path to periodically write the exact same content `/metrics` serves,
independent of the HTTP endpoint (which stays available regardless). This is for delivery that doesn't involve
scraping nshgeoip directly -- e.g. [node_exporter's textfile
collector](https://github.com/prometheus/node_exporter#textfile-collector) reading `.prom` files off disk.

```ini
metrics_file=/var/lib/node_exporter/textfile_collector/nshgeoip.prom
metrics_interval_seconds=60   # default; 1-86400
```

Written once immediately at startup and then every `metrics_interval_seconds`, atomically (a temp file in the same
directory, then `rename()`, which is atomic on POSIX within one filesystem) so a reader never sees a partial write
mid-update. A `metrics_file` problem (bad permissions, missing directory) is logged as a warning, never fatal -- it
should never be able to take the daemon down. Empty/unset (the default) disables the feature entirely, with no
periodic wake-up cost: nshgeoip's main loop only switches from an indefinite `poll()` wait to a timed one when
`metrics_file` is actually configured.

## curl examples

```bash
curl --unix-socket /run/nshgeoip/nshgeoip.sock 'http://localhost/lookup?ip=8.8.8.8'

curl --unix-socket /run/nshgeoip/nshgeoip.sock 'http://localhost/lookup?ip=2001:4860:4860::8888'
```

[examples/curl-example.sh](examples/curl-example.sh) runs a handful of these (including the 400/405 error cases)
against a running daemon.

## NGINX auth_request example

[examples/nginx-geoip.conf](examples/nginx-geoip.conf) has a full working example. The core of it:


```nginx
location = /_geoip {
    internal;

    proxy_pass http://unix:/run/nshgeoip/nshgeoip.sock:/lookup?ip=$remote_addr;
    proxy_pass_request_body off;
    proxy_set_header Content-Length "";

    # nginx's auth_request module treats any non-2xx/401/403 subrequest
    # response as a hard error for the real request. Since nshgeoip's 400/404
    # ("no data for this address") must never block or break the request,
    # coerce them (and a 500 from nshgeoip) into a plain 200 with no
    # X-GeoIP-* headers instead:
    proxy_intercept_errors on;
    error_page 400 404 405 500 502 503 504 = @geoip_unavailable;
}

location @geoip_unavailable {
    internal;
    return 200;
}

location / {
    auth_request /_geoip;

    auth_request_set $geoip_country   $upstream_http_x_geoip_country;
    auth_request_set $geoip_continent $upstream_http_x_geoip_continent;
    auth_request_set $geoip_asn       $upstream_http_x_geoip_asn;
    auth_request_set $geoip_as_org    $upstream_http_x_geoip_as_org;

    # ... use $geoip_country etc., e.g. proxy_set_header to your app,
    # or `if ($geoip_country = XX) { return 403; }` for actual policy.
}
```

That last point matters: **policy decisions belong in this NGINX config (or your application), not in nshgeoip.** nshgeoip
only ever answers "what do you know about this address," never "should this be allowed."

## IPv6 support

`ip` accepts both IPv4 and IPv6 literals (e.g. `2001:4860:4860::8888`). Parsing uses `inet_pton()` only -- never
`getaddrinfo()` or any other resolver path -- so a client-supplied address can never trigger a DNS lookup or any other
outbound network activity; `nshgeoip` never makes an outbound network connection of any kind.

## Concurrency model

A fixed-size worker thread pool (`threads` in the config, default the host's CPU core count clamped to 4-20 -- see
[Configuration](#configuration)): the accept loop hands each accepted connection off to the pool, and idle workers pick
connections up from a queue.

This was chosen over the alternatives because of what nshgeoip actually serves: short-lived, low-QPS, synchronous local
subrequests (an NGINX `auth_request` blocks the real request on this call, so there's never a large number of them in
flight at once). Thread-per-connection would be simpler still but leaves thread count unbounded under a burst; a
single-threaded `epoll` reactor would need an asynchronous HTTP parser for no real benefit at this request volume. A
small bounded pool gives predictable resource use with a plain, easy-to-audit blocking-I/O implementation per
connection.

`libmaxminddb` lookups are read-only against the memory-mapped database and safe to call concurrently from multiple
threads; a `std::shared_mutex` in `GeoIpDatabases` lets lookups run fully in parallel (shared/read lock) while `SIGHUP`
briefly takes an exclusive lock only to swap in newly reloaded database handles.

Each connection is handled synchronously end-to-end (read request, look up, write response, close) with no
persistent/keep-alive connections -- every response is `Connection: close`. This avoids needing to implement pipelining,
chunked transfer, or partial-body edge cases for a backend that only ever needs to answer one small request at a time.

## Signals

| Signal               | Effect                                                         |
|----------------------|----------------------------------------------------------------|
| `SIGTERM` / `SIGINT` | Clean shutdown: stop accepting, finish in-flight requests, close the listening socket, remove the socket file, close the MMDB databases |
| `SIGHUP`             | Re-open the configured MMDB files and atomically swap them in, without dropping the listening socket or interrupting in-flight requests |

Signals are delivered via `signalfd`, polled alongside the listening socket in the same loop -- no async-signal-unsafe
code runs in a signal handler.

## Security considerations

- All request data is treated as untrusted, including on the UNIX socket.
- IPv4/IPv6 addresses are validated with `inet_pton()` before ever being passed to `libmaxminddb`; invalid input never reaches the lookup path.
- Request header size is bounded (`max_request_bytes`, default 8 KiB) and reading a request has a receive timeout, so a slow or oversized request can't tie up a worker thread indefinitely.
- The HTTP parser only implements what's needed for `GET /lookup?ip=...` -- no chunked transfer, no arbitrary methods, nothing decoding a client-supplied filesystem path.
- No shell execution anywhere in the request path.
- Response headers built from MMDB data are sanitized (CR/LF and other control characters stripped) so a database value can never inject a header or split the response.
- `nshgeoip` never makes an outbound network connection -- no DNS, no HTTP client, nothing. It only ever *accepts* connections: its UNIX socket (always)
  and only if explicitly enabled, an optional TCP listener (see [TCP listener](#tcp-listener)) -- off by default.
- RAII wrappers manage the listening socket, each connection's file descriptor, and the `MMDB_s` database handles, so error paths and shutdown can't leak them.
- The systemd unit runs as an unprivileged user with `NoNewPrivileges`, `ProtectSystem=strict`, an empty capability set and, by default
  (matching TCP being off), `RestrictAddressFamilies=AF_UNIX` -- see [TCP listener](#tcp-listener) for what to add if you enable it.

## Testing

```bash
make test     # unit tests: IP parsing, JSON/header sanitization, config
              # parsing, Prometheus/health rendering (tests/test_nshgeoip.cpp)

make          # build nshgeoip itself first

bash tests/integration_test.sh

bash tests/test_nshgeoipctl.sh
```

`tests/integration_test.sh` runs a real `nshgeoip` process and covers what the unit tests can't: concurrent requests, a
malformed raw HTTP request on the wire, a stale/leftover socket file left by a crashed run, refusing to clobber a
non-socket file at the socket path, a clean shutdown on `SIGTERM`, a `SIGHUP` reload (and that the daemon keeps serving
afterward), the `/health`/`/metrics` endpoints, `metrics_file`'s periodic write, `--check-db` (both a real file and a
clean failure on a nonexistent one), and the TCP listener -- default dual-stack IPv4+IPv6, an explicit `tcp_address`
restricting to one family, and that the UNIX socket keeps working with TCP also enabled.

It needs at least one real `.mmdb` file to start `nshgeoip` at all, but that's often already true with zero setup: if a
database is auto-detectable at a standard location (`/var/lib/GeoIP`, `/var/lib/crowdsec/data` -- see [MaxMind database
configuration](#maxmind-database-configuration)), the command above just works, using that real data. Content-specific
assertions (an exact `country_code`, city name, etc.) are the exception -- those only run against
`TEST_COUNTRY_DB`/`TEST_ASN_DB`/`TEST_CITY_DB`, since real production data won't contain MaxMind's synthetic test
values. Point those at MaxMind's small redistributable test fixtures (used by `libmaxminddb`'s own test suite) to
exercise them too:

```bash
TEST_COUNTRY_DB=/path/to/GeoLite2-Country-Test.mmdb \
TEST_ASN_DB=/path/to/GeoLite2-ASN-Test.mmdb \
TEST_CITY_DB=/path/to/GeoLite2-City-Test.mmdb \
  bash tests/integration_test.sh
```

<https://github.com/maxmind/MaxMind-DB/tree/main/test-data> has those fixtures. `TEST_KNOWN_IP`/`TEST_KNOWN_COUNTRY`
asserts on a real address instead, against whatever database is actually active -- real or fixture.

`tests/test_nshgeoipctl.sh` covers [nshgeoipctl.sh](#management-script) itself, offline: edition/config-key mapping,
parsing `/etc/GeoIP.conf`, resolving owned vs. externally-available database paths, and `download_db`'s skip logic
(nothing to do when every edition is already available externally, skipping `country` when `city` covers it on an
auto-defaulted run but never on an explicit request). It doesn't touch the network or need credentials.

[examples/curl-example.sh](examples/curl-example.sh) is the quick manual smoke test.

## Attribution

This product uses GeoLite2 data created by MaxMind, available from <https://www.maxmind.com>. `nshgeoip` doesn't
ship or bundle any GeoLite2 database itself -- you download it separately (directly, via `nshgeoipctl.sh
download-db`, or via `geoipupdate`; see [MaxMind database configuration](#maxmind-database-configuration)) and
point `nshgeoip` at it.

Don't have a MaxMind account yet? Sign up for GeoLite2 (free) at <https://www.maxmind.com/en/geolite2/signup>. If
your use case needs more accuracy or update frequency than GeoLite2 offers, MaxMind's paid
[GeoIP2](https://www.maxmind.com/en/geoip2-databases) databases are drop-in compatible -- same `.mmdb` format,
same `libmaxminddb` reader, just point `nshgeoip` at the GeoIP2 file instead.

`nshgeoip` links against [`libmaxminddb`](https://github.com/maxmind/libmaxminddb) (Copyright MaxMind, Inc.,
Apache License 2.0) -- statically in the [static Alpine build](#static-alpine-build)'s release binaries,
dynamically otherwise. See [NOTICE](NOTICE) for the reproduced copyright notice.

