#!/usr/bin/env sh
set -eu

REPO_ROOT=$(cd -P "$(dirname "$0")" && pwd)

# Auto-detect container runtime. Honor an explicit RUNTIME override
# (RUNTIME=docker|podman); otherwise prefer docker if its daemon is running and
# fall back to podman.
# Usage: ./build.sh [aarch64|armv7hf ...] [--build-base]   (default: both)
if [ -n "${RUNTIME:-}" ]; then
	CTR="$RUNTIME"
elif command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; then
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

# Build the slow libzt base images if they are missing.
# Pass --build-base to force a rebuild of the base (needed when upgrading
# LIBZT_VERSION, ZT_CORE_VERSION, or the SDK image version).
FORCE_BASE=0
ARCHS=''
for arg in "$@"; do
	case "$arg" in
	--build-base) FORCE_BASE=1 ;;
	*) ARCHS="${ARCHS} $arg" ;;
	esac
done
[ -z "$ARCHS" ] && ARCHS='aarch64 armv7hf'

for ARCH in $ARCHS; do
	BASE_TAG="zerotier-libzt-base-${ARCH}"
	if [ "$FORCE_BASE" = '1' ] || ! ${CTR} image exists "${BASE_TAG}" 2>/dev/null; then
		echo "==> Building libzt base for ${ARCH} (one-time, ~10-20 min)..."
		${CTR} build ${BUILD_FLAGS} \
			--build-arg ARCH="${ARCH}" \
			--tag "${BASE_TAG}" \
			-f "$REPO_ROOT/Dockerfile.libzt" \
			"$REPO_ROOT"
	else
		echo "==> libzt base for ${ARCH} already built — skipping"
	fi
done

# Remove old .eap files so only the freshly built ones remain
echo '==> Cleaning old .eap files...'
rm -f "$REPO_ROOT"/*.eap

for ARCH in $ARCHS; do
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
