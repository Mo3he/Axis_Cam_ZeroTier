ARG UBUNTU_VERSION=20.04
FROM axisecp/acap-sdk:3.5-armv7hf-ubuntu${UBUNTU_VERSION}

# Install build tools for libzt (cmake, git)
RUN apt-get update -qq && apt-get install -y --no-install-recommends \
    cmake git ca-certificates && \
    apt-get clean && rm -rf /var/lib/apt/lists/*

# Clone libzt (ZeroTier Sockets SDK — userspace networking, no TUN needed)
ARG LIBZT_VERSION=1.8.10
ARG ZT_CORE_VERSION=1.16.0
RUN git clone --branch ${LIBZT_VERSION} \
    https://github.com/zerotier/libzt.git /tmp/libzt && \
    cd /tmp/libzt && git submodule update --init --recursive && \
    cd ext/ZeroTierOne && git fetch --tags && git checkout ${ZT_CORE_VERSION} && cd /tmp/libzt && \
    sed -i '/include_directories(\${ZTO_SRC_DIR}\/ext\/libnatpmp)/a include_directories(\${ZTO_SRC_DIR}\/ext\/prometheus-cpp-lite-1.0\/core\/include)\ninclude_directories(\${ZTO_SRC_DIR}\/ext\/prometheus-cpp-lite-1.0\/simpleapi\/include)' CMakeLists.txt && \
    sed -i 's|_node = new Node(this, (void\*)0, \&cb, OSUtils::now());|ZT_Node_Config ztcfg = {}; _node = new Node(this, (void*)0, \&ztcfg, \&cb, OSUtils::now());|' src/NodeService.cpp && \
    printf 'import sys\ns=open("src/Utilities.cpp").read()\ns=s.replace("#include <node/C25519.hpp>","#include <node/ECC.hpp>")\ns=s.replace("#include <node/World.hpp>\\n","")\nstart=s.find("int zts_util_sign_root_set(")\nbrace=s.index("{",start)\nd,i=0,brace\nwhile i<len(s):\n    if s[i]=="{":d+=1\n    elif s[i]=="}":d-=1\n    if d==0:break\n    i+=1\ns=s[:brace]+"{ return ZTS_ERR_GENERAL; }"+s[i+1:]\nopen("src/Utilities.cpp","w").write(s)\n' > /tmp/patch_utils.py && \
    python3 /tmp/patch_utils.py

# Write cmake toolchain file for armv7hf cross-compilation, then build libzt
RUN . /opt/axis/acapsdk/environment-setup* && \
    CC_BIN=$(echo $CC | awk '{print $1}') && \
    CXX_BIN=$(echo $CXX | awk '{print $1}') && \
    CC_PATH=$(which $CC_BIN) && \
    CXX_PATH=$(which $CXX_BIN) && \
    printf 'set(CMAKE_SYSTEM_NAME Linux)\n' > /tmp/toolchain.cmake && \
    printf 'set(CMAKE_SYSTEM_PROCESSOR arm)\n' >> /tmp/toolchain.cmake && \
    printf 'set(CMAKE_C_COMPILER %s)\n' "${CC_PATH}" >> /tmp/toolchain.cmake && \
    printf 'set(CMAKE_CXX_COMPILER %s)\n' "${CXX_PATH}" >> /tmp/toolchain.cmake && \
    printf 'set(CMAKE_SYSROOT %s)\n' "${SDKTARGETSYSROOT}" >> /tmp/toolchain.cmake && \
    printf 'set(CMAKE_FIND_ROOT_PATH %s)\n' "${SDKTARGETSYSROOT}" >> /tmp/toolchain.cmake && \
    printf 'set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)\n' >> /tmp/toolchain.cmake && \
    printf 'set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)\n' >> /tmp/toolchain.cmake && \
    printf 'set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)\n' >> /tmp/toolchain.cmake && \
    printf 'set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)\n' >> /tmp/toolchain.cmake && \
    cat /tmp/toolchain.cmake && \
    cd /tmp/libzt && \
    sed -i 's|^//#define ZT_AES_NO_ACCEL|#define ZT_AES_NO_ACCEL|' ext/ZeroTierOne/node/AES.hpp && \
    mkdir build && cd build && \
    cmake .. \
        -DCMAKE_TOOLCHAIN_FILE=/tmp/toolchain.cmake \
        -DCMAKE_BUILD_TYPE=Release \
        -DZTS_DISABLE_CENTRAL_API=1 \
        -DBUILD_STATIC_LIB=ON \
        -DBUILD_SHARED_LIB=OFF \
        -DBUILD_HOST_SELFTEST=OFF \
        -DALLOW_INSTALL_TARGET=OFF && \
    make -j$(nproc) zt-static

COPY ./app /opt/app/
# copy the repo LICENSE so create-package.sh can find it
COPY ./LICENSE /opt/app/LICENSE
WORKDIR /opt/app

# ACAP 2.x firmware expects the settings page at the app root, not in html/
RUN cp html/index.html index.html

# Patch the version placeholder in package.conf (ACAP3 uses major.minor.patch)
ARG ACAP_VERSION=1.16.6
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
