#!/bin/sh

set -eu

database_dir="${GEOIPUPDATE_DB_DIR:-/usr/share/GeoIP}"
frequency="${GEOIPUPDATE_FREQUENCY:-72}"
credentials_file="${GEOIPUPDATE_CREDENTIALS_FILE:-/run/secrets/maxmind.conf}"
debug_enabled="${GEOIPUPDATE_DEBUG:-0}"
active_pid=""

log()
{
  printf '%s %s\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')" "$*"
}

error()
{
  printf '%s ERROR: %s\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')" "$*" >&2
}

debug()
{
  case "$debug_enabled" in
    1|true|yes|on)
      printf '%s DEBUG: %s\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')" "$*" >&2
      ;;
  esac
}

terminate()
{
  log "Stopping GeoIP updater"

  if [ -n "$active_pid" ]; then
    debug "Sending SIGTERM to process $active_pid"
    kill -TERM "$active_pid" 2>/dev/null || true
    wait "$active_pid" 2>/dev/null || true
  fi

  exit 0
}

trap terminate TERM INT

if [ ! -r "$credentials_file" ]; then
  error "Cannot read credentials file: $credentials_file"
  exit 1
fi

debug "Loading credentials from $credentials_file"

. "$credentials_file"

if [ -z "${GEOIPUPDATE_ACCOUNT_ID:-}" ]; then
  error "GEOIPUPDATE_ACCOUNT_ID is not configured in $credentials_file"
  exit 1
fi

if [ -z "${GEOIPUPDATE_LICENSE_KEY:-}" ]; then
  error "GEOIPUPDATE_LICENSE_KEY is not configured in $credentials_file"
  exit 1
fi

if [ -z "${GEOIPUPDATE_EDITION_IDS:-}" ]; then
  error "GEOIPUPDATE_EDITION_IDS is not configured"
  exit 1
fi

case "$frequency" in
  ''|*[!0-9]*)
    error "GEOIPUPDATE_FREQUENCY must be a non-negative integer"
    exit 1
    ;;
esac

export GEOIPUPDATE_ACCOUNT_ID
export GEOIPUPDATE_LICENSE_KEY

mkdir -p "$database_dir"

authorization="$(printf '%s:%s' "$GEOIPUPDATE_ACCOUNT_ID" "$GEOIPUPDATE_LICENSE_KEY" | base64 | tr -d '\r\n')"

log "GeoIP updater initialized"
log "Database directory: $database_dir"
log "Database editions: $GEOIPUPDATE_EDITION_IDS"
log "Update frequency: $frequency hours"

debug "Credentials file: $credentials_file"
debug "Account ID length: ${#GEOIPUPDATE_ACCOUNT_ID}"
debug "License key length: ${#GEOIPUPDATE_LICENSE_KEY}"
debug "Preserve file times: ${GEOIPUPDATE_PRESERVE_FILE_TIMES:-0}"
debug "Verbose geoipupdate output: ${GEOIPUPDATE_VERBOSE:-0}"

publish_checksums()
{
  edition="$1"
  filename="$edition.mmdb"
  database="$database_dir/$filename"
  md5_file="$database.md5"
  sha256_file="$database.sha256"
  metadata_file="$database.metadata.json"
  md5_temporary="$md5_file.tmp"
  sha256_temporary="$sha256_file.tmp"
  metadata_temporary="$metadata_file.tmp"
  metadata_url="https://updates.maxmind.com/geoip/updates/metadata?edition_id=$edition"

  rm -f "$md5_temporary" "$sha256_temporary" "$metadata_temporary"

  if [ ! -f "$database" ]; then
    error "Database not found: $database"
    return 1
  fi

  debug "Processing database $database"

  case "$debug_enabled" in
    1|true|yes|on)
      ls -ln "$database" >&2
      ;;
  esac

  if [ -f "$md5_file" ] && [ -f "$sha256_file" ] && [ -f "$metadata_file" ] && ! [ "$database" -nt "$metadata_file" ]; then
    debug "Checksum files for $edition are newer than the database; skipping"
    return 0
  fi

  log "Retrieving MaxMind metadata for $edition"
  debug "Metadata URL: $metadata_url"

  case "$debug_enabled" in
    1|true|yes|on)
      if ! metadata="$(wget -S -O - --header "Authorization: Basic $authorization" "$metadata_url")"; then
        error "Failed to retrieve MaxMind metadata for $edition"
        return 1
      fi
      ;;
    *)
      if ! metadata="$(wget -q -O - --header "Authorization: Basic $authorization" "$metadata_url")"; then
        error "Failed to retrieve MaxMind metadata for $edition"
        return 1
      fi
      ;;
  esac

  if [ -z "$metadata" ]; then
    error "MaxMind returned an empty metadata response for $edition"
    return 1
  fi

  debug "Raw metadata response: $metadata"

  if ! selected_metadata="$(printf '%s\n' "$metadata" | jq -ce --arg edition "$edition" '.databases[] | select(.edition_id == $edition)')"; then
    error "MaxMind metadata does not contain edition $edition"
    return 1
  fi

  if ! expected_md5="$(printf '%s\n' "$selected_metadata" | jq -er '.md5')"; then
    error "MaxMind metadata does not contain an MD5 for $edition"
    return 1
  fi

  expected_md5="$(printf '%s' "$expected_md5" | tr 'A-F' 'a-f')"

  if ! printf '%s\n' "$expected_md5" | grep -Eq '^[0-9a-f]{32}$'; then
    error "MaxMind returned an invalid MD5 for $edition: $expected_md5"
    return 1
  fi

  if ! actual_md5="$(md5sum "$database" | awk '{print $1}')"; then
    error "Failed to calculate MD5 for $database"
    return 1
  fi

  actual_md5="$(printf '%s' "$actual_md5" | tr 'A-F' 'a-f')"

  debug "Expected MD5 for $edition: $expected_md5"
  debug "Actual MD5 for $edition:   $actual_md5"

  if [ "$actual_md5" != "$expected_md5" ]; then
    error "MaxMind checksum mismatch for $edition"
    error "Expected MD5: $expected_md5"
    error "Actual MD5:   $actual_md5"
    return 1
  fi

  if ! sha256="$(sha256sum "$database" | awk '{print $1}')"; then
    error "Failed to calculate SHA-256 for $database"
    return 1
  fi

  debug "Calculated SHA-256 for $edition: $sha256"

  printf '%s  %s\n' "$expected_md5" "$filename" > "$md5_temporary"
  printf '%s  %s\n' "$sha256" "$filename" > "$sha256_temporary"

  if ! printf '%s\n' "$selected_metadata" | jq --arg sha256 "$sha256" '. + {sha256: $sha256}' > "$metadata_temporary"; then
    rm -f "$md5_temporary" "$sha256_temporary" "$metadata_temporary"
    error "Failed to create metadata file for $edition"
    return 1
  fi

  mv "$md5_temporary" "$md5_file"
  mv "$sha256_temporary" "$sha256_file"
  mv "$metadata_temporary" "$metadata_file"

  log "Published verified checksums for $edition"
}

run_geoipupdate()
{
  log "Starting GeoIP database update"

  geoipupdate &
  active_pid="$!"

  debug "geoipupdate started as process $active_pid"

  if wait "$active_pid"; then
    update_status=0
  else
    update_status="$?"
  fi

  debug "geoipupdate exited with status $update_status"

  active_pid=""

  if [ "$update_status" -ne 0 ]; then
    error "geoipupdate failed with status $update_status"
    return "$update_status"
  fi

  log "GeoIP database update completed"

  checksum_status=0

  for edition in $GEOIPUPDATE_EDITION_IDS; do
    if ! publish_checksums "$edition"; then
      checksum_status=1
    fi
  done

  return "$checksum_status"
}

while true; do
  if ! run_geoipupdate; then
    error "GeoIP update cycle failed"
  fi

  if [ "$frequency" -eq 0 ]; then
    log "One-time update completed"
    exit 0
  fi

  sleep_seconds=$((frequency * 3600))

  log "Next update in $frequency hours"
  debug "Sleeping for $sleep_seconds seconds"

  sleep "$sleep_seconds" &
  active_pid="$!"

  wait "$active_pid" || true
  active_pid=""
done
