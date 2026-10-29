#!/usr/bin/env bash
set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$DIR"

mkdir -p screenshots

PLATFORMS=("${@}")
if [ ${#PLATFORMS[@]} -eq 0 ]; then
  PLATFORMS=("emery" "chalk" "basalt" "diorite" "gabbro")
fi

echo "==> Building app bundle..."
pebble build

cleanup() {
  pebble kill >/dev/null 2>&1 || true
}
trap cleanup EXIT

for p in "${PLATFORMS[@]}"; do
  echo ""
  echo "=========================================="
  echo "Capturing screenshots for platform: $p"
  echo "=========================================="

  pebble kill >/dev/null 2>&1 || true
  sleep 1

  echo "-> Installing to emulator ($p)..."
  pebble install --emulator "$p"

  # Wait for cold-start interstitial to settle onto the first station (Muzeum)
  echo "-> Waiting for initial board to settle..."
  sleep 3

  echo "-> Capturing hero screenshot (Muzeum)..."
  pebble screenshot --emulator "$p" "screenshots/hero-${p}.png"

  echo "-> Switching to second station (Hlavní nádraží)..."
  pebble emu-button --emulator "$p" click down
  sleep 2

  echo "-> Capturing second screenshot (Hlavní nádraží)..."
  pebble screenshot --emulator "$p" "screenshots/tram-${p}.png"

  echo "✓ Captured screenshots for $p"
done

echo ""
echo "==> All done! Screenshots updated in screenshots/:"
ls -lh screenshots/hero-*.png screenshots/tram-*.png 2>/dev/null || true
