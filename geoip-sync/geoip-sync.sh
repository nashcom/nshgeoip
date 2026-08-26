#!/bin/sh

set -eu

source_url="${GEOIP_SYNC_SOURCE_URL:?GEOIP_SYNC_SOURCE_URL is required}"
edition_ids="${GEOIP_SYNC_EDITION_IDS:-GeoLite2-ASN GeoLite2-City GeoLite2-Country}"
database_dir="${GEOIP_SYNC_DB_DIR:-/var/lib/GeoIP}"
sync_interval="${GEOIP_SYNC_INTERVAL:-300}"
active_pid=""

source_url="${source_url%/}"

terminate()
{
  echo "Stopping GeoIP synchronization"

  if [ -n "$active_pid" ]; then
    kill -TERM "$active_pid" 2>/dev/null || true
    wait "$active_pid" 2>/dev/null || true
  fi

  exit 0
}

trap terminate TERM INT

case "$sync_interval" in
  ''|*[!0-9]*)
    echo "ERROR: GEOIP_SYNC_INTERVAL must be a non-negative integer" >&2
    exit 1
    ;;
esac

mkdir -p "$database_dir"

download_file()
{
  url="$1"
  destination="$2"
  headers="${3:-}"

  if [ -n "$headers" ]; then
    wget -S -O "$destination" "$url" 2> "$headers" &
  else
    wget -q -O "$destination" "$url" &
  fi

  active_pid="$!"

  if wait "$active_pid"; then
    download_status=0
  else
    download_status="$?"
  fi

  active_pid=""

  return "$download_status"
}

update_database()
{
  edition="$1"
  filename="$edition.mmdb"

  database="$database_dir/$filename"
  md5_file="$database.md5"
  sha256_file="$database.sha256"
  metadata_file="$database.metadata.json"

  temporary_database="$database.tmp"
  temporary_md5="$md5_file.tmp"
  temporary_sha256="$sha256_file.tmp"
  temporary_metadata="$metadata_file.tmp"
  temporary_headers="$database.headers.tmp"

  rm -f "$temporary_database" "$temporary_md5" "$temporary_sha256" "$temporary_metadata" "$temporary_headers"

  echo "Checking $edition"

  if ! download_file "$source_url/$filename.sha256" "$temporary_sha256"; then
    rm -f "$temporary_sha256"
    echo "ERROR: Cannot download SHA-256 checksum for $edition" >&2
    return 1
  fi

  expected_sha256="$(awk 'NR == 1 { print $1 }' "$temporary_sha256")"

  if ! printf '%s\n' "$expected_sha256" | grep -Eq '^[0-9a-fA-F]{64}$'; then
    rm -f "$temporary_sha256"
    echo "ERROR: Invalid SHA-256 checksum for $edition: $expected_sha256" >&2
    return 1
  fi

  expected_sha256="$(printf '%s' "$expected_sha256" | tr 'A-F' 'a-f')"

  if [ -f "$database" ] && [ -f "$sha256_file" ]; then
    current_sha256="$(awk 'NR == 1 { print $1 }' "$sha256_file")"
    current_sha256="$(printf '%s' "$current_sha256" | tr 'A-F' 'a-f')"

    if [ "$current_sha256" = "$expected_sha256" ]; then
      rm -f "$temporary_sha256"
      echo "$edition is current"
      return 0
    fi
  fi

  echo "Downloading $edition"

  if ! download_file "$source_url/$filename" "$temporary_database" "$temporary_headers"; then
    rm -f "$temporary_database" "$temporary_sha256" "$temporary_headers"
    echo "ERROR: Cannot download database for $edition" >&2
    return 1
  fi

  actual_sha256="$(sha256sum "$temporary_database" | awk '{ print $1 }')"
  actual_sha256="$(printf '%s' "$actual_sha256" | tr 'A-F' 'a-f')"

  if [ "$actual_sha256" != "$expected_sha256" ]; then
    rm -f "$temporary_database" "$temporary_sha256" "$temporary_headers"
    echo "ERROR: SHA-256 checksum mismatch for $edition" >&2
    echo "Expected: $expected_sha256" >&2
    echo "Actual:   $actual_sha256" >&2
    return 1
  fi

  if ! download_file "$source_url/$filename.md5" "$temporary_md5"; then
    rm -f "$temporary_database" "$temporary_md5" "$temporary_sha256" "$temporary_headers"
    echo "ERROR: Cannot download MaxMind MD5 checksum for $edition" >&2
    return 1
  fi

  expected_md5="$(awk 'NR == 1 { print $1 }' "$temporary_md5")"

  if ! printf '%s\n' "$expected_md5" | grep -Eq '^[0-9a-fA-F]{32}$'; then
    rm -f "$temporary_database" "$temporary_md5" "$temporary_sha256" "$temporary_headers"
    echo "ERROR: Invalid MaxMind MD5 checksum for $edition: $expected_md5" >&2
    return 1
  fi

  expected_md5="$(printf '%s' "$expected_md5" | tr 'A-F' 'a-f')"
  actual_md5="$(md5sum "$temporary_database" | awk '{ print $1 }')"
  actual_md5="$(printf '%s' "$actual_md5" | tr 'A-F' 'a-f')"

  if [ "$actual_md5" != "$expected_md5" ]; then
    rm -f "$temporary_database" "$temporary_md5" "$temporary_sha256" "$temporary_headers"
    echo "ERROR: MaxMind MD5 checksum mismatch for $edition" >&2
    echo "Expected: $expected_md5" >&2
    echo "Actual:   $actual_md5" >&2
    return 1
  fi

  if ! download_file "$source_url/$filename.metadata.json" "$temporary_metadata"; then
    rm -f "$temporary_database" "$temporary_md5" "$temporary_sha256" "$temporary_metadata" "$temporary_headers"
    echo "ERROR: Cannot download metadata for $edition" >&2
    return 1
  fi

  last_modified="$(sed -n 's/^[[:space:]]*[Ll]ast-[Mm]odified:[[:space:]]*//p' "$temporary_headers" | tail -n 1 | tr -d '\r')"

  if [ -z "$last_modified" ]; then
    rm -f "$temporary_database" "$temporary_md5" "$temporary_sha256" "$temporary_metadata" "$temporary_headers"
    echo "ERROR: Mirror did not return Last-Modified for $edition" >&2
    return 1
  fi

  if ! touch_timestamp="$(date -u -D '%a, %d %b %Y %H:%M:%S GMT' -d "$last_modified" '+%Y%m%d%H%M.%S')"; then
    rm -f "$temporary_database" "$temporary_md5" "$temporary_sha256" "$temporary_metadata" "$temporary_headers"
    echo "ERROR: Cannot parse Last-Modified for $edition: $last_modified" >&2
    return 1
  fi

  if ! touch -t "$touch_timestamp" "$temporary_database"; then
    rm -f "$temporary_database" "$temporary_md5" "$temporary_sha256" "$temporary_metadata" "$temporary_headers"
    echo "ERROR: Cannot preserve modification time for $edition: $last_modified" >&2
    return 1
  fi

  rm -f "$temporary_headers"

  mv "$temporary_database" "$database"
  mv "$temporary_md5" "$md5_file"
  mv "$temporary_metadata" "$metadata_file"
  mv "$temporary_sha256" "$sha256_file"

  echo "Updated $edition"
  echo "Preserved modification time: $last_modified"
}

run_sync()
{
  sync_status=0

  for edition in $edition_ids; do
    if ! update_database "$edition"; then
      sync_status=1
    fi
  done

  return "$sync_status"
}

while true; do
  if ! run_sync; then
    echo "ERROR: GeoIP synchronization cycle failed" >&2
  fi

  if [ "$sync_interval" -eq 0 ]; then
    exit 0
  fi

  echo "Next check in $sync_interval seconds"

  sleep "$sync_interval" &
  active_pid="$!"

  wait "$active_pid" || true
  active_pid=""
done
