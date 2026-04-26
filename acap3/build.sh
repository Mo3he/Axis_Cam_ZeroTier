#!/usr/bin/env sh
set -eu

REPO_ROOT=$(cd -P "$(dirname "$0")" && pwd)
# The acap3 eap is placed in the parent repo root alongside the other eaps
PARENT_ROOT=$(cd -P "$(dirname "$0")/.." && pwd)
VERSION=1.16.9

# Auto-detect container runtime (prefer docker if daemon is running, fall back to podman)
if command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; then
    CTR=docker
elif command -v podman >/dev/null 2>&1; then
    CTR=podman
else
    echo 'Error: neither docker (with running daemon) nor podman found' >&2
    exit 1
fi
echo "==> Using container runtime: ${CTR}"

# Resolve a real (non-symlink) temp directory - Podman on macOS can't follow
# the /tmp -> /private/tmp symlink when using 'cp'.
TMPBASE=$(cd -P "${TMPDIR:-/tmp}" && pwd)

# Build flags - enable layer caching for podman (docker always caches)
BUILD_FLAGS=''
if [ "${CTR}" = 'podman' ]; then BUILD_FLAGS='--layers'; fi

# Build the slow libzt base image if it is missing.
# Pass --build-base to force a rebuild (needed when upgrading LIBZT_VERSION,
# ZT_CORE_VERSION, or the SDK image version).
FORCE_BASE=0
for arg in "$@"; do
    [ "$arg" = '--build-base' ] && FORCE_BASE=1
done

if [ "$FORCE_BASE" = '1' ] || ! ${CTR} image exists 'zerotier-libzt-base-acap3' 2>/dev/null; then
    echo '==> Building libzt base for ACAP 3 armv7hf (one-time, ~10-20 min)...'
    ${CTR} build ${BUILD_FLAGS} \
        --tag 'zerotier-libzt-base-acap3' \
        -f "$REPO_ROOT/Dockerfile.libzt" \
        "$REPO_ROOT"
else
    echo '==> libzt base for ACAP 3 already built — skipping'
fi

# Remove old acap3 .eap files from the parent repo root so only fresh ones remain
echo '==> Cleaning old .eap files...'
rm -f "$PARENT_ROOT"/*_acap3.eap

echo '==> Building ACAP 3 armv7hf (for AXIS OS 9.x / 10.x cameras)...'
${CTR} build ${BUILD_FLAGS} --build-arg ACAP_VERSION="${VERSION}" --tag 'zerotier-vpn-acap3-armv7hf' "$REPO_ROOT"

echo '==> Extracting .eap package...'
CID=$(${CTR} create 'zerotier-vpn-acap3-armv7hf')
${CTR} cp "${CID}":/opt/app/ "${TMPBASE}/acap3-out"
# Rename to consistent naming scheme regardless of what create-package.sh calls it
find "${TMPBASE}/acap3-out" -name '*.eap' -exec cp {} \
    "$PARENT_ROOT/ZeroTier_VPN_$(echo "${VERSION}" | tr '.' '_')_armv7hf_acap3.eap" \;
${CTR} rm "${CID}" >/dev/null
rm -rf "${TMPBASE}/acap3-out"

echo '==> Done!'
ls -lh "$PARENT_ROOT"/*.eap
