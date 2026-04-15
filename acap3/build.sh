#!/bin/bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")" && pwd)"
VERSION="1.16.3"

echo "==> Building ACAP 3 armv7hf (for AXIS OS 9.x / 10.x cameras)..."
docker build --build-arg ACAP_VERSION="${VERSION}" --tag "zerotier-vpn-acap3-armv7hf" "$REPO_ROOT"

echo "==> Extracting .eap package..."
CID=$(docker create "zerotier-vpn-acap3-armv7hf")
docker cp "${CID}":/opt/app/ "/tmp/acap3-out"
# Rename to consistent naming scheme regardless of what create-package.sh calls it
find "/tmp/acap3-out" -name "*.eap" -exec cp {} "$REPO_ROOT/ZeroTier_VPN_${VERSION//./_}_armv7hf_acap3.eap" \;
docker rm "${CID}" >/dev/null
rm -rf "/tmp/acap3-out"

echo "==> Done!"
ls -lh "$REPO_ROOT"/*.eap
