#!/bin/bash
set -e

VERBOSE=0
if [[ "$2" == "--verbose" ]]; then
  VERBOSE=1
fi

ARCH=$1

if [[ -z "$ARCH" ]]; then
  echo "Usage: $0 <arch> [--verbose]"
  echo "  arch: armv6 | armhf | arm64"
  echo "  Targets: armv6l (Pi Zero/1), armhf (Pi 2/3/4 32-bit), arm64 (Pi 3/4/5 64-bit)"
  exit 1
fi

COMPONENT=brcmfmac_iovar
DOCKERFILE="docker/Dockerfile.${COMPONENT}.${ARCH}"
IMAGE_TAG="brcmfmac-iovar-${ARCH}"

declare -A ARCH_FLAGS
ARCH_FLAGS=(
  ["armv6"]="linux/arm/v7"
  ["armhf"]="linux/arm/v7"
  ["arm64"]="linux/arm64"
)

if [[ -z "${ARCH_FLAGS[$ARCH]}" ]]; then
  echo "Error: Unknown architecture: $ARCH"
  echo "Available: ${!ARCH_FLAGS[@]}"
  exit 1
fi

PLATFORM="${ARCH_FLAGS[$ARCH]}"

if [[ ! -f "$DOCKERFILE" ]]; then
  echo "Missing Dockerfile: $DOCKERFILE"
  exit 1
fi

echo "[+] Building Docker image for $ARCH ($PLATFORM)..."
if [[ "$VERBOSE" -eq 1 ]]; then
  DOCKER_BUILDKIT=1 docker build --platform=$PLATFORM --progress=plain -t $IMAGE_TAG -f $DOCKERFILE .
else
  docker build --platform=$PLATFORM --progress=auto -t $IMAGE_TAG -f $DOCKERFILE .
fi

echo "[+] Building brcm-iovar in Docker ($ARCH)..."
if [[ "$ARCH" == "armv6" ]]; then
  docker run --rm --platform=$PLATFORM -v "$PWD":/build -w /build $IMAGE_TAG bash -c "\
    make clean || true && \
    make EXTRA_CFLAGS='-march=armv6 -mfpu=vfp -mfloat-abi=hard -marm' && \
    make strip"
else
  docker run --rm --platform=$PLATFORM -v "$PWD":/build -w /build $IMAGE_TAG bash -c "\
    make clean || true && \
    make && \
    make strip"
fi

mkdir -p out/$ARCH
cp -f brcm-iovar out/$ARCH/
make clean 2>/dev/null || true

echo "[OK] Binary: out/$ARCH/brcm-iovar"
