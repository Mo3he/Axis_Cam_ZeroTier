#!/usr/bin/env sh
set -eu

REPO_ROOT=$(cd -P "$(dirname "$0")" && pwd)

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

# Remove old .eap files so only the freshly built ones remain
echo '==> Cleaning old .eap files...'
rm -f "$REPO_ROOT"/*.eap

for ARCH in aarch64 armv7hf; do
    echo "==> Building ${ARCH}..."
    ${CTR} build ${BUILD_FLAGS} --build-arg ARCH="${ARCH}" --tag "zerotier-vpn-${ARCH}" "$REPO_ROOT"

    echo "==> Extracting ${ARCH} .eap package..."
    CID=$(${CTR} create "zerotier-vpn-${ARCH}")
    ${CTR} cp "${CID}":/opt/app/ "${TMPBASE}/${ARCH}-out"
    cp "${TMPBASE}/${ARCH}-out"/*.eap "$REPO_ROOT/"
    ${CTR} rm "${CID}" >/dev/null
    rm -rf "${TMPBASE}/${ARCH}-out"
done

echo '==> Done!'
ls -lh "$REPO_ROOT"/*.eap
