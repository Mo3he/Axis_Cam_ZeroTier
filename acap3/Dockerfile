# App image — starts from the pre-built libzt base so the slow git clone and
# cmake steps are skipped entirely on every code-only rebuild.
# Build the base first with:  sh acap3/build.sh --build-base
FROM zerotier-libzt-base-acap3

COPY ./app /opt/app/
# copy the repo LICENSE so create-package.sh can find it
COPY ./LICENSE /opt/app/LICENSE
WORKDIR /opt/app

# ACAP 2.x firmware expects the settings page at the app root, not in html/
RUN cp html/index.html index.html

# The version is stored directly in package.conf, not injected at build time.

# Build the main ACAP binary (config_bridge.c)
RUN . /opt/axis/acapsdk/environment-setup* && make

# Cross-compile the proxy binary (statically linked against libzt)
RUN . /opt/axis/acapsdk/environment-setup* && \
    mkdir -p lib && \
    CC_BIN=$(echo $CC | awk '{print $1}') && \
    $CC_BIN --sysroot=${SDKTARGETSYSROOT} -O2 -g -Wall -std=gnu11 \
        -I/tmp/libzt/include \
        proxy/proxy.c \
        /tmp/libzt/build/lib/libzt.a \
        -lstdc++ -lpthread -lm \
        -static \
        -o lib/zerotier-userspace && \
    chmod 755 lib/zerotier-userspace
# Must be the SDK's cross strip: the host `strip` cannot read ARM ELF and failed
# silently here, leaving ~19 MB of symbols (90% of the binary) in the package.
# The unstripped copy lives outside /opt/app so it stays out of the .eap.
RUN mkdir -p /opt/debug && \
    cp lib/zerotier-userspace /opt/debug/zerotier-userspace.unstripped
RUN . /opt/axis/acapsdk/environment-setup* && \
    "${STRIP:?SDK environment did not set STRIP}" lib/zerotier-userspace

# Package the ACAP 3 .eap
# Note: create-package.sh sets APPTYPE=native which ACAP 2.x firmware rejects.
# Older cameras (EmbeddedDevelopment 2.x) expect APPTYPE to be the architecture
# string. Repack the .eap with the corrected value after creation.
RUN . /opt/axis/acapsdk/environment-setup* && create-package.sh && \
    EAP=$(ls *.eap) && \
    PKGVER=$(sed -n 's/^VERSION=//p' package.conf) && \
    MAJOR=$(echo ${PKGVER} | cut -d. -f1) && \
    MINOR=$(echo ${PKGVER} | cut -d. -f2) && \
    MICRO=$(echo ${PKGVER} | cut -d. -f3) && \
    mkdir -p /tmp/eapfix && tar -xzf "$EAP" -C /tmp/eapfix && \
    sed -i \
        -e 's/APPTYPE="native"/APPTYPE="armv7hf"/' \
        -e 's/LICENSENAME="Available"/LICENSENAME="None"/' \
        -e 's/APPUSR="sdk"/APPUSR="root"/' \
        -e 's/APPGRP="sdk"/APPGRP="root"/' \
        -e 's/STARTMODE="never"/STARTMODE="respawn"/' \
        -e "s/^APPMAJORVERSION=.*/APPMAJORVERSION=\"${MAJOR}\"/" \
        -e "s/^APPMINORVERSION=.*/APPMINORVERSION=\"${MINOR}\"/" \
        -e "s/^APPMICROVERSION=.*/APPMICROVERSION=\"${MICRO}\"/" \
        -e 's/^REQEMBDEVVERSION=.*/REQEMBDEVVERSION="2.0"/' \
        /tmp/eapfix/package.conf && \
    tar -czf "$EAP" -C /tmp/eapfix . && \
    rm -rf /tmp/eapfix
