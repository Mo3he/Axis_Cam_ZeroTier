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

# Patch the version placeholder in package.conf (ACAP3 uses major.minor.patch)
ARG ACAP_VERSION=1.16.9
RUN sed -i "s/VERSION=BUILDVER/VERSION=${ACAP_VERSION}/" package.conf

# Build the main ACAP binary (config_bridge.c)
RUN . /opt/axis/acapsdk/environment-setup* && make

# Cross-compile the proxy binary (statically linked against libzt)
RUN . /opt/axis/acapsdk/environment-setup* && \
    mkdir -p lib && \
    CC_BIN=$(echo $CC | awk '{print $1}') && \
    $CC_BIN --sysroot=${SDKTARGETSYSROOT} -O2 -Wall -std=gnu11 \
        -I/tmp/libzt/include \
        proxy/proxy.c \
        /tmp/libzt/build/lib/libzt.a \
        -lstdc++ -lpthread -lm \
        -static \
        -o lib/zerotier-userspace && \
    chmod 755 lib/zerotier-userspace
RUN . /opt/axis/acapsdk/environment-setup* && \
    strip lib/zerotier-userspace 2>/dev/null || true

# Package the ACAP 3 .eap
# Note: create-package.sh sets APPTYPE=native which ACAP 2.x firmware rejects.
# Older cameras (EmbeddedDevelopment 2.x) expect APPTYPE to be the architecture
# string. Repack the .eap with the corrected value after creation.
RUN . /opt/axis/acapsdk/environment-setup* && create-package.sh && \
    EAP=$(ls *.eap) && \
    MAJOR=$(echo ${ACAP_VERSION} | cut -d. -f1) && \
    MINOR=$(echo ${ACAP_VERSION} | cut -d. -f2) && \
    MICRO=$(echo ${ACAP_VERSION} | cut -d. -f3) && \
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
