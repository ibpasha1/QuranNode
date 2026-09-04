#!/usr/bin/env bash
# ota-push.sh — push firmware.bin straight to a QuranNode in Wi-Fi update mode.
# No browser: this is the curl upload the device's /update endpoint accepts.
#
#   tools/ota-push.sh [ip]
#
# If no IP is given it scans your /24 for a device serving the QuranNode update
# page. You can always read the exact IP off the device's "Update firmware" screen.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/firmware/.pio/build/devkit_st7796s/firmware.bin"
[ -f "$BIN" ] || { echo "No firmware.bin — build first:  cd firmware && pio run -e devkit_st7796s"; exit 1; }

is_qn() { curl -s -m 2 "http://$1/" 2>/dev/null | grep -qi "QuranNode"; }

IP="${1:-}"
if [ -z "$IP" ]; then
  MYIP="$(ipconfig getifaddr en0 2>/dev/null || ipconfig getifaddr en1 2>/dev/null || true)"
  [ -n "$MYIP" ] || { echo "Can't find your LAN IP; pass the device IP:  tools/ota-push.sh 192.168.x.x"; exit 1; }
  BASE="${MYIP%.*}"
  echo "Scanning $BASE.0/24 for a QuranNode in update mode..."
  # Probe the whole subnet in parallel, keep the first that serves our page.
  for i in $(seq 1 254); do
    ( is_qn "$BASE.$i" && echo "$BASE.$i" >>"/tmp/qn_found.$$" ) &
  done
  wait
  IP="$(head -n1 "/tmp/qn_found.$$" 2>/dev/null || true)"; rm -f "/tmp/qn_found.$$"
fi

[ -n "$IP" ] || { echo "No QuranNode found. On the reader: Settings > Update firmware (or hold the 5-way center at power-on), then re-run."; exit 1; }
is_qn "$IP" || { echo "http://$IP/ isn't responding as a QuranNode — is it in update mode?"; exit 1; }

echo "Pushing $(basename "$BIN") ($(wc -c <"$BIN" | tr -d ' ') bytes) -> http://$IP/update ..."
curl -s -m 180 --data-binary @"$BIN" -H "Content-Type: application/octet-stream" \
     "http://$IP/update" -w $'\n[HTTP %{http_code} in %{time_total}s]\n'
