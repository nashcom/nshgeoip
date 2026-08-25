#!/bin/sh
# Manual smoke test against a running nshgeoip, talking to it exactly the
# way NGINX's auth_request subrequest does: HTTP/1.1 over the UNIX socket.
#
# Defaults to the production socket path; override for local testing, e.g.
#   NSHGEOIP_SOCKET=/tmp/nshgeoip/nshgeoip.sock ./examples/curl-example.sh
set -eu

SOCKET="${NSHGEOIP_SOCKET:-/run/nshgeoip/nshgeoip.sock}"

echo "--- IPv4 lookup ---"
curl -sS -i --unix-socket "$SOCKET" 'http://localhost/lookup?ip=8.8.8.8'
echo
echo

echo "--- IPv6 lookup ---"
curl -sS -i --unix-socket "$SOCKET" 'http://localhost/lookup?ip=2001:4860:4860::8888'
echo
echo

echo "--- headers only, no JSON body (HEAD) ---"
curl -sS -I --unix-socket "$SOCKET" 'http://localhost/lookup?ip=8.8.8.8'
echo

echo "--- plain text (INI-style) instead of JSON ---"
curl -sS -i -H 'Accept: text/plain' --unix-socket "$SOCKET" 'http://localhost/lookup?ip=8.8.8.8'
echo
echo

echo "--- missing ip parameter (expect 400) ---"
curl -sS -i --unix-socket "$SOCKET" 'http://localhost/lookup'
echo
echo

echo "--- invalid ip (expect 400) ---"
curl -sS -i --unix-socket "$SOCKET" 'http://localhost/lookup?ip=not-an-ip'
echo
echo

echo "--- unsupported method (expect 405) ---"
curl -sS -i -X POST --unix-socket "$SOCKET" 'http://localhost/lookup?ip=8.8.8.8'
echo
