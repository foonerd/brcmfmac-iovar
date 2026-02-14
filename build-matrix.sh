#!/bin/bash
set -e
# Build brcm-iovar for all Raspberry Pi targets (armv6l, armhf, arm64)

ARCHS=("armv6" "armhf" "arm64")
VERBOSE=""

for arg in "$@"; do
  case "$arg" in
    --verbose) VERBOSE="--verbose" ;;
  esac
done

for ARCH in "${ARCHS[@]}"; do
  echo ""
  echo "====================================="
  echo ">> Building for: $ARCH"
  echo "====================================="
  ./docker/run-docker-brcmfmac_iovar.sh "$ARCH" $VERBOSE
done

echo ""
echo "[OK] All builds completed. Binaries are in out/<arch>/"
