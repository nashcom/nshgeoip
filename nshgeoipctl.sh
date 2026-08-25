#!/bin/bash

###############################################################################
#
# nshgeoipctl - installs, configures and operates the nshgeoip daemon
#
# Run from a checked-out nshgeoip source tree (it shells out to "make" for
# the actual build/install of the binary). See "nshgeoipctl.sh help" for the
# full command list.
#
###############################################################################

NSHGEOIPCTL_VERSION="0.9.0"

# BASH_SOURCE[0], not $0: $0 is this script's own path when it's executed
# directly, but reflects the *caller's* path instead when this script is
# sourced (e.g. by tests/test_nshgeoipctl.sh, or by hand for manual
# verification) -- resolving via $0 in that case silently pointed
# SCRIPT_DIR at the sourcing script's directory instead of this file's own,
# breaking default_geoip_dirs.sh/etc/nshgeoip.conf.example lookups below
# whenever this script was sourced from anywhere other than its own
# directory. BASH_SOURCE[0] is always this file's own path, sourced or not.
SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

PREFIX="${PREFIX:-/usr/local}"
SBINDIR="$PREFIX/sbin"
BINARY_DEST="$SBINDIR/nshgeoip"

SERVICE_NAME="nshgeoip"
SYSTEMD_DIR="/etc/systemd/system"

CONF_DIR="/etc/nshgeoip"
CONF_FILE="$CONF_DIR/nshgeoip.conf"
CONF_EXAMPLE="$SCRIPT_DIR/etc/nshgeoip.conf.example"

# shellcheck source=default_geoip_dirs.sh
. "$SCRIPT_DIR/default_geoip_dirs.sh"

# Default install directory for the GeoLite2 databases -- matches
# geoipupdate's own default DatabaseDirectory (confirmed against a real
# "geoipupdate -v" run: "Using database directory /var/lib/GeoIP" when
# GEOIP_CONF has no explicit DatabaseDirectory line), so nshgeoip and
# geoipupdate agree on where the files live even if only one of the two is
# actually in use. An explicit DatabaseDirectory line in GEOIP_CONF
# overrides this, same as it would for geoipupdate itself. This is where
# download-db writes to -- NSHGEOIP_DEFAULT_GEOIP_DIRS (default_geoip_dirs.sh)
# is the broader read-only list (this directory plus CrowdSec's) used to
# check whether a database is already available before downloading it.
GEOIP_DIR_DEFAULT="${NSHGEOIP_DEFAULT_GEOIP_DIRS[0]}"

# geoipupdate's own config file (https://github.com/maxmind/geoipupdate) --
# read (never written) for AccountID/LicenseKey/EditionIDs/DatabaseDirectory
# so a system that already has one set up (e.g. for other MaxMind tooling)
# doesn't need the same settings entered twice.
GEOIP_CONF="/etc/GeoIP.conf"

SUDO=""


###############################################################################
# Generic helpers
###############################################################################

print_delim()
{
  echo "--------------------------------------------------------------------------------"
}


log_err()
{
  echo
  echo "Error: $*"
  echo
}


log_info()
{
  echo "Info: $*"
}


have_command()
{
  command -v "$1" >/dev/null 2>&1
}


setup_sudo()
{
  if [ "$(id -u)" -eq 0 ]; then
    SUDO=""
    return 0
  fi

  if have_command sudo; then
    SUDO="sudo"
    return 0
  fi

  log_err "This operation requires root privileges and sudo is not available."
  return 1
}


require_root()
{
  setup_sudo || return 1

  if [ -n "$SUDO" ]; then
    $SUDO -v || return 1
  fi
}


# Reads the value of "key=" from the config file, ignoring commented-out
# (#-prefixed) lines. Used so download-db and show_config work off whatever
# is actually configured, not a hardcoded assumption.
read_config_value()
{
  local key="$1"

  [ -e "$CONF_FILE" ] || return 0
  $SUDO grep -E "^${key}=" "$CONF_FILE" 2>/dev/null | tail -n1 | cut -d= -f2-
}


###############################################################################
# install
###############################################################################

create_user_group()
{
  if getent group nshgeoip >/dev/null 2>&1; then
    echo "[group nshgeoip] already exists"
  else
    $SUDO groupadd --system nshgeoip || return 1
    echo "[group nshgeoip] created"
  fi

  if getent passwd nshgeoip >/dev/null 2>&1; then
    echo "[user nshgeoip] already exists"
  else
    $SUDO useradd --system --no-create-home --shell /usr/sbin/nologin -g nshgeoip nshgeoip || return 1
    echo "[user nshgeoip] created"
  fi
}


ensure_real_config()
{
  if $SUDO test -e "$CONF_FILE"; then
    echo "[$CONF_FILE] already exists, leaving it untouched"
    return 0
  fi

  if [ ! -e "$CONF_EXAMPLE" ]; then
    log_err "example config not found: $CONF_EXAMPLE"
    return 1
  fi

  $SUDO install -m 0755 -d "$CONF_DIR" || return 1
  $SUDO install -o root -g root -m 0644 "$CONF_EXAMPLE" "$CONF_FILE" || return 1
  echo "[$CONF_FILE] created from $(basename "$CONF_EXAMPLE")"
}


# Generated rather than kept as a separate packaging/systemd/nshgeoip.service
# file so the whole install lives in this one script. ExecStart/config path
# track $BINARY_DEST/$CONF_FILE so a custom PREFIX is reflected correctly.
generate_systemd_unit()
{
  cat <<EOF
[Unit]
Description=nshgeoip - local GeoIP lookup daemon
After=network.target

[Service]
Type=simple
ExecStart=$BINARY_DEST --config $CONF_FILE

User=nshgeoip
Group=nshgeoip
# Grants read access to the UNIX socket to whichever group NGINX (or
# another local client) runs as, without putting nshgeoip in that group's
# other privileges. Adjust or remove to match your system.
SupplementaryGroups=nginx

# Creates /run/nshgeoip (owned by User=/Group=) before the service starts
# and removes it on stop; nshgeoip itself also removes its socket file on a
# clean shutdown.
RuntimeDirectory=nshgeoip
RuntimeDirectoryMode=0750

Restart=on-failure
RestartSec=2

# --- Sandboxing / hardening -------------------------------------------
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=strict
ProtectHome=true
ProtectKernelTunables=true
ProtectKernelModules=true
ProtectKernelLogs=true
ProtectControlGroups=true
ProtectClock=true
ProtectHostname=true
RestrictNamespaces=true
RestrictSUIDSGID=true
RestrictRealtime=true
LockPersonality=true
MemoryDenyWriteExecute=true
RemoveIPC=true
UMask=0007

# nshgeoip only ever accepts on a UNIX domain socket by default. If you
# enable the optional TCP listener (tcp_port= in the config file), add
# AF_INET and AF_INET6 here too -- nshgeoip will otherwise be killed by
# the sandbox the moment it tries to bind a TCP socket.
RestrictAddressFamilies=AF_UNIX

# No special privileges are required.
CapabilityBoundingSet=
AmbientCapabilities=

SystemCallFilter=@system-service
SystemCallErrorNumber=EPERM

[Install]
WantedBy=multi-user.target
EOF
}


install_systemd_unit()
{
  local tmp
  tmp=$(mktemp) || return 1
  generate_systemd_unit > "$tmp"
  $SUDO install -o root -g root -m 0644 "$tmp" "$SYSTEMD_DIR/${SERVICE_NAME}.service"
  local rc=$?
  rm -f "$tmp"
  [ $rc -eq 0 ] && echo "[$SYSTEMD_DIR/${SERVICE_NAME}.service] installed"
  return $rc
}


install_nshgeoip()
{
  require_root || return 1

  echo "Building and installing binary + example config..."
  ( cd "$SCRIPT_DIR" && make ) || return 1
  ( cd "$SCRIPT_DIR" && $SUDO make install PREFIX="$PREFIX" ) || return 1

  echo
  install_systemd_unit || return 1

  echo
  create_user_group || return 1

  echo
  ensure_real_config || return 1

  echo
  echo "Enabling and starting the $SERVICE_NAME service..."
  $SUDO systemctl daemon-reload || return 1
  $SUDO systemctl enable --now "$SERVICE_NAME" || return 1

  echo
  echo "nshgeoip installed and started."
  echo "Edit $CONF_FILE (real country_db/asn_db/city_db paths, see 'download-db'"
  echo "to fetch them) then run: $0 systemd restart"

  show_config
}


###############################################################################
# config
###############################################################################

show_config()
{
  echo
  echo "Config file: $CONF_FILE"
  print_delim
  if [ -e "$CONF_FILE" ]; then
    $SUDO cat "$CONF_FILE"
  else
    echo "(not created yet -- run '$0 install' first)"
  fi
  print_delim
}


###############################################################################
# download-db
###############################################################################

# Accepts either the short nshgeoipctl.sh name (country/asn/city) or the
# full MaxMind edition_id (as written in /etc/GeoIP.conf's EditionIDs, e.g.
# "GeoLite2-City") and prints the short name. Only the three free GeoLite2
# editions nshgeoip's config file has a slot for are recognized -- an
# EditionIDs line naming something else (e.g. a paid GeoIP2-ISP edition) is
# simply not something this script can install anywhere, not an error by
# itself.
normalize_edition()
{
  case "$1" in
    country|GeoLite2-Country) echo "country" ;;
    asn|GeoLite2-ASN)         echo "asn" ;;
    city|GeoLite2-City)       echo "city" ;;
    *)                        return 1 ;;
  esac
}


# Maps a short edition name (as used on the nshgeoipctl.sh command line) to
# MaxMind's edition_id and the config key that points at its install path.
edition_id_for()
{
  case "$1" in
    country) echo "GeoLite2-Country" ;;
    asn)     echo "GeoLite2-ASN" ;;
    city)    echo "GeoLite2-City" ;;
    *)       return 1 ;;
  esac
}


config_key_for()
{
  case "$1" in
    country) echo "country_db" ;;
    asn)     echo "asn_db" ;;
    city)    echo "city_db" ;;
    *)       return 1 ;;
  esac
}


default_path_for()
{
  local edition="$1"
  local geoip_dir="${2:-$GEOIP_DIR_DEFAULT}"

  case "$edition" in
    country) echo "$geoip_dir/GeoLite2-Country.mmdb" ;;
    asn)     echo "$geoip_dir/GeoLite2-ASN.mmdb" ;;
    city)    echo "$geoip_dir/GeoLite2-City.mmdb" ;;
    *)       return 1 ;;
  esac
}


# The path `edition` is/would be stored at under our own management: the
# nshgeoip.conf key it maps to, if already configured, otherwise the
# default path under `geoip_dir` -- i.e. exactly what download_one_db()
# below would write to. Always prints a path (never fails); whether that
# path currently exists is a separate question (see download_one_db()'s
# own -r check, which decides update-vs-fresh-download).
owned_db_path()
{
  local edition="$1"
  local geoip_dir="$2"
  local configured

  configured=$(read_config_value "$(config_key_for "$edition")")
  if [ -n "$configured" ]; then
    echo "$configured"
    return 0
  fi

  default_path_for "$edition" "$geoip_dir"
}


# Read-only check for whether `edition` is already available somewhere
# nshgeoip would find it on its own that ISN'T under our own management
# (owned_db_path() above) -- i.e. CrowdSec's bundled copy, or any other
# entry in NSHGEOIP_DEFAULT_GEOIP_DIRS besides `geoip_dir` itself. Prints
# the path and returns 0 if found, returns 1 otherwise. Used by
# download_db() to leave a database we don't manage alone entirely
# (never downloaded, never update-checked), as opposed to one already
# sitting at our own managed location, which gets update-checked instead.
external_db_path()
{
  local edition="$1"
  local geoip_dir="$2"
  local filename
  filename="$(edition_id_for "$edition").mmdb"

  local other_dirs=() dir
  for dir in "${NSHGEOIP_DEFAULT_GEOIP_DIRS[@]}"; do
    [ "$dir" = "$geoip_dir" ] && continue
    other_dirs+=("$dir")
  done

  nshgeoip_find_db "$filename" "${other_dirs[@]}"
}


# Reads geoipupdate's own /etc/GeoIP.conf (format: "Key Value" lines, '#'
# comments, e.g. "AccountID 123" / "LicenseKey xxx" / "EditionIDs
# GeoLite2-ASN GeoLite2-City GeoLite2-Country" / "DatabaseDirectory
# /usr/share/GeoIP") so a system that already has it set up for geoipupdate
# doesn't need the same settings entered again. Sets GEOIP_CONF_ACCOUNT_ID /
# GEOIP_CONF_LICENSE_KEY / GEOIP_CONF_EDITION_IDS (an array) /
# GEOIP_CONF_DATABASE_DIR as a side effect rather than returning them --
# bash has no clean multi-value return, and these are only ever read right
# after calling this. Returns 1 (with everything left empty) if the file
# doesn't exist.
read_geoip_conf()
{
  GEOIP_CONF_ACCOUNT_ID=""
  GEOIP_CONF_LICENSE_KEY=""
  GEOIP_CONF_EDITION_IDS=()
  GEOIP_CONF_DATABASE_DIR=""

  $SUDO test -e "$GEOIP_CONF" || return 1

  local line key value
  while IFS= read -r line; do
    line="${line%%#*}"
    line="${line#"${line%%[![:space:]]*}"}" # trim leading whitespace
    line="${line%"${line##*[![:space:]]}"}" # trim trailing whitespace
    [ -z "$line" ] && continue

    key="${line%%[[:space:]]*}"
    value="${line#"$key"}"
    value="${value#"${value%%[![:space:]]*}"}"

    case "$key" in
      AccountID)         GEOIP_CONF_ACCOUNT_ID="$value" ;;
      LicenseKey)        GEOIP_CONF_LICENSE_KEY="$value" ;;
      DatabaseDirectory) GEOIP_CONF_DATABASE_DIR="$value" ;;
      EditionIDs) read -r -a GEOIP_CONF_EDITION_IDS <<< "$value" ;;
    esac
  done < <($SUDO cat "$GEOIP_CONF" 2>/dev/null)
}


# Downloads (fresh) or updates (MD5-checked) one GeoLite2 edition from
# MaxMind and installs it at owned_db_path()'s path. Uses the same
# account_id:license_key Basic Auth API geoipupdate itself uses
# (updates.maxmind.com) -- the older license-key-only download.maxmind.com
# permalink is a legacy path MaxMind no longer issues credentials for on
# new accounts. When a file already exists at the destination, this is an
# update check, not a blind re-download: MaxMind's update endpoint takes
# the local file's MD5 and returns HTTP 304 (nothing to do, matching what
# real geoipupdate reports as "No new updates available") if it's already
# current, or 200 with fresh content otherwise -- so a database that's
# already up to date costs one small request, not a multi-megabyte
# re-download. Any actual archive received (200) is SHA256-verified
# against the checksum MaxMind publishes at the same download URL with
# ".sha256" appended to suffix, before it's extracted or installed --
# integrity protection against transit corruption or tampering, on top of
# TLS. Never echoes $license_key.
download_one_db()
{
  local edition="$1"
  local account_id="$2"
  local license_key="$3"
  local geoip_dir="$4"
  local edition_id dest_path tmpdir archive url

  edition_id=$(edition_id_for "$edition") || { log_err "unknown database edition: $edition"; return 1; }
  dest_path=$(owned_db_path "$edition" "$geoip_dir")

  tmpdir=$(mktemp -d) || return 1
  archive="$tmpdir/db.tar.gz"

  if [ -r "$dest_path" ] && have_command md5sum; then
    local local_md5
    local_md5=$(md5sum "$dest_path" | cut -d' ' -f1)
    url="https://updates.maxmind.com/geoip/databases/${edition_id}/update?db_md5=${local_md5}"
    echo "Checking $edition_id for updates ($dest_path) ..."
  else
    if [ -r "$dest_path" ]; then
      log_info "md5sum not found -- doing a fresh download instead of an update check for $edition_id"
    fi
    url="https://updates.maxmind.com/geoip/databases/${edition_id}/download?suffix=tar.gz"
    echo "Downloading $edition_id ..."
  fi

  local http_status curl_rc
  # -L: both endpoints redirect to the actual object storage (confirmed
  # against the real API) whenever there's real content to serve -- the
  # 304 "already up to date" case from the update endpoint doesn't
  # redirect at all, so -L is a no-op there. %{http_code} reports the
  # final status after following, which is what the checks below want.
  http_status=$(curl -sS -L -u "${account_id}:${license_key}" -w '%{http_code}' -o "$archive" "$url")
  curl_rc=$?

  if [ "$curl_rc" -ne 0 ]; then
    log_err "download failed for $edition_id (curl exit $curl_rc) -- check network connectivity."
    rm -rf "$tmpdir"
    return 1
  fi

  if [ "$http_status" = "304" ]; then
    echo "[$edition_id] already up to date ($dest_path)"
    rm -rf "$tmpdir"
    return 0
  fi

  if [ "$http_status" != "200" ]; then
    log_err "download failed for $edition_id (HTTP $http_status) -- check the account ID/license key and that this edition is enabled on your MaxMind account."
    rm -rf "$tmpdir"
    return 1
  fi

  # MaxMind publishes a SHA256 of the exact same archive at the same
  # download endpoint with ".sha256" appended to suffix (confirmed against
  # the real API: a redirect to a small text file in standard "sha256sum"
  # format, "<hash>  <filename>"). Always fetched from the plain /download
  # URL (not /update?db_md5=...), since it's "the current published
  # archive's checksum" regardless of which URL actually got us the bytes.
  if have_command sha256sum; then
    local expected_sha256 actual_sha256
    expected_sha256=$(curl -sS -u "${account_id}:${license_key}" -L \
      "https://updates.maxmind.com/geoip/databases/${edition_id}/download?suffix=tar.gz.sha256" | awk '{print $1}')

    if [ -z "$expected_sha256" ]; then
      log_err "could not fetch SHA256 checksum for $edition_id -- refusing to install unverified data."
      rm -rf "$tmpdir"
      return 1
    fi

    actual_sha256=$(sha256sum "$archive" | awk '{print $1}')
    if [ "$actual_sha256" != "$expected_sha256" ]; then
      log_err "SHA256 mismatch for $edition_id (expected $expected_sha256, got $actual_sha256) -- refusing to install; archive may be corrupted or tampered with in transit."
      rm -rf "$tmpdir"
      return 1
    fi
    echo "[$edition_id] SHA256 verified"
  else
    log_info "sha256sum not found -- skipping checksum verification for $edition_id"
  fi

  if ! tar -xzf "$archive" -C "$tmpdir"; then
    log_err "downloaded file for $edition_id is not a valid archive -- check the account ID/license key."
    rm -rf "$tmpdir"
    return 1
  fi

  local mmdb_path
  mmdb_path=$(find "$tmpdir" -maxdepth 2 -iname '*.mmdb' | head -n1)
  if [ -z "$mmdb_path" ]; then
    log_err "no .mmdb file found in the downloaded $edition_id archive."
    rm -rf "$tmpdir"
    return 1
  fi

  $SUDO install -m 0755 -d "$(dirname "$dest_path")" || { rm -rf "$tmpdir"; return 1; }
  $SUDO install -o root -g root -m 0644 "$mmdb_path" "$dest_path" || { rm -rf "$tmpdir"; return 1; }
  rm -rf "$tmpdir"

  echo "[$dest_path] updated"
}


download_db()
{
  local editions=("$@")
  # Whether editions came from the caller's own explicit arguments, as
  # opposed to defaulting from GEOIP_CONF's EditionIDs or our own
  # (country asn city) fallback below -- gates the country-skip
  # optimization further down: auto-detection deciding to skip
  # country_db on your behalf only makes sense when nothing specific was
  # actually asked for. "download-db country" always downloads country,
  # full stop, even if city is also in play.
  local auto_defaulted=0

  local account_id="$NSHGEOIP_MAXMIND_ACCOUNT_ID"
  local license_key="$NSHGEOIP_MAXMIND_LICENSE_KEY"
  local geoip_dir="$GEOIP_DIR_DEFAULT"
  local conf_editions=()

  if read_geoip_conf; then
    echo "Using credentials from $GEOIP_CONF"
    [ -z "$account_id" ] && account_id="$GEOIP_CONF_ACCOUNT_ID"
    [ -z "$license_key" ] && license_key="$GEOIP_CONF_LICENSE_KEY"
    [ -n "$GEOIP_CONF_DATABASE_DIR" ] && geoip_dir="$GEOIP_CONF_DATABASE_DIR"
    conf_editions=("${GEOIP_CONF_EDITION_IDS[@]}")
  fi

  if [ ${#editions[@]} -eq 0 ]; then
    auto_defaulted=1
    if [ ${#conf_editions[@]} -gt 0 ]; then
      local raw normalized
      for raw in "${conf_editions[@]}"; do
        if normalized=$(normalize_edition "$raw"); then
          editions+=("$normalized")
        else
          log_info "skipping $GEOIP_CONF EditionIDs entry nshgeoip has no config slot for: $raw"
        fi
      done
    fi
    [ ${#editions[@]} -eq 0 ] && editions=(country asn city)
  fi

  local edition
  for edition in "${editions[@]}"; do
    edition_id_for "$edition" >/dev/null || { log_err "unknown database edition: $edition (expected: country, asn, or city)"; return 1; }
  done

  # --- pre-check: leave anything already available at an external
  # location (CrowdSec's directory, or wherever else in
  # NSHGEOIP_DEFAULT_GEOIP_DIRS -- see external_db_path()) alone entirely.
  # Everything else proceeds to download_one_db(), which itself checks
  # whether it already owns a copy and, if so, does an MD5-based update
  # check against MaxMind instead of a blind fresh download -- see
  # download_one_db() for that logic. Runs before asking for credentials
  # so a run that turns out to need nothing from the network (every
  # edition external) doesn't get prompted for anything at all. ---
  local to_process=() found
  local city_in_play=0
  for edition in "${editions[@]}"; do
    [ "$edition" = "city" ] && city_in_play=1
    if found=$(external_db_path "$edition" "$geoip_dir"); then
      echo "[$edition] already available at $found (not managed by nshgeoipctl.sh -- leaving it alone)"
    else
      to_process+=("$edition")
    fi
  done

  # Mirrors apply_default_db_paths()'s city-covers-country optimization in
  # src/config.cpp -- but only when nothing specific was asked for (see
  # auto_defaulted above); an explicit "download-db country" always
  # processes country_db, city or no city.
  if [ "$auto_defaulted" -eq 1 ] && [ "$city_in_play" -eq 1 ]; then
    local filtered=()
    for edition in "${to_process[@]}"; do
      if [ "$edition" = "country" ]; then
        echo "[country] skipping: city already supplies country/continent data"
      else
        filtered+=("$edition")
      fi
    done
    to_process=("${filtered[@]}")
  fi

  if [ ${#to_process[@]} -eq 0 ]; then
    echo "Nothing to do -- every requested edition is already available at an external location."
    return 0
  fi

  if [ -z "$account_id" ]; then
    read -r -p "MaxMind account ID: " account_id
  fi
  if [ -z "$license_key" ]; then
    read -r -s -p "MaxMind license key: " license_key
    echo
  fi
  if [ -z "$account_id" ] || [ -z "$license_key" ]; then
    log_err "account ID and license key are both required (argument env vars NSHGEOIP_MAXMIND_ACCOUNT_ID/NSHGEOIP_MAXMIND_LICENSE_KEY, $GEOIP_CONF, or prompt)."
    return 1
  fi

  if ! have_command curl; then
    log_err "curl is required for download-db but was not found."
    return 1
  fi
  if ! have_command tar; then
    log_err "tar is required for download-db but was not found."
    return 1
  fi

  require_root || return 1

  local rc=0
  for edition in "${to_process[@]}"; do
    download_one_db "$edition" "$account_id" "$license_key" "$geoip_dir" || rc=1
  done

  return $rc
}


###############################################################################
# systemd
###############################################################################

systemd_command()
{
  local command="$1"

  if [ -z "$command" ] || [ "$command" = "status" ]; then
    systemctl status "$SERVICE_NAME"
    return
  fi

  require_root || return 1

  case "$command" in
    start|stop|restart|enable|disable)
      $SUDO systemctl "$command" "$SERVICE_NAME"
      ;;
    *)
      log_err "Unknown systemd option: $command"
      return 1
      ;;
  esac
}


###############################################################################
# version / help
###############################################################################

show_version()
{
  echo "nshgeoipctl $NSHGEOIPCTL_VERSION"

  if have_command "$BINARY_DEST"; then
    "$BINARY_DEST" --version
  elif [ -x "$SCRIPT_DIR/nshgeoip" ]; then
    "$SCRIPT_DIR/nshgeoip" --version
  fi
}


show_help()
{
  echo "Usage: $0 <command> [args]"
  echo
  printf "  %-32s%s\n" "install" "Build, install, create user/config, enable and start the service"
  printf "  %-32s%s\n" "config (cfg)" "Show the current config file"
  printf "  %-32s%s\n" "download-db [editions]" "Download or update GeoLite2 database(s) from MaxMind (default: country asn city)"
  printf "  %-32s%s\n" "systemd [cmd]" "start|stop|restart|enable|disable|status (default: status)"
  printf "  %-32s%s\n" "status" "Show service status (same as: systemd status)"
  printf "  %-32s%s\n" "version" "Show version information"
  printf "  %-32s%s\n" "help" "Show this help"
  echo
  echo "download-db checks each edition, in order: if it's already available at an"
  echo "external location (e.g. CrowdSec's bundled copy) it's left alone; if we already"
  echo "own a copy, it's MD5-checked against MaxMind and only replaced if actually newer"
  echo "(same as geoipupdate's own update check); otherwise it's a fresh download. Any"
  echo "archive actually received is SHA256-verified against MaxMind's own published"
  echo "checksum before being installed."
  echo
  echo "download-db needs a MaxMind AccountID + LicenseKey, taken (in order) from:"
  echo "  1. NSHGEOIP_MAXMIND_ACCOUNT_ID / NSHGEOIP_MAXMIND_LICENSE_KEY environment variables"
  echo "  2. $GEOIP_CONF, geoipupdate's own config file, if present (also supplies the"
  echo "     default edition list from its EditionIDs line)"
  echo "  3. an interactive prompt (license key is not echoed)"
  echo
  echo "Examples:"
  echo "  $0 install"
  echo "  $0 download-db                          # prompts for account ID + license key"
  echo "  $0 download-db city                      # only GeoLite2-City"
  echo "  NSHGEOIP_MAXMIND_ACCOUNT_ID=123 NSHGEOIP_MAXMIND_LICENSE_KEY=... $0 download-db"
  echo "  $0 systemd restart"
  echo
}


###############################################################################
# Main
###############################################################################

main()
{
  case "$1" in
    install)
      install_nshgeoip
      ;;

    config|cfg)
      show_config
      ;;

    download-db|fetch-db)
      shift
      download_db "$@"
      ;;

    systemd)
      systemd_command "$2"
      ;;

    status)
      systemd_command "status"
      ;;

    version|-v|--version)
      show_version
      ;;

    help|-h|--help|"")
      show_help
      ;;

    *)
      log_err "Unknown command: '$1'"
      show_help
      return 1
      ;;
  esac
}

main "$@"
