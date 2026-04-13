ARG ARCH=aarch64
ARG VERSION=1.15.1
ARG UBUNTU_VERSION=22.04
ARG REPO=axisecp
ARG SDK=acap-native-sdk

FROM ${REPO}/${SDK}:${VERSION}-${ARCH}-ubuntu${UBUNTU_VERSION}

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

# Write a cmake toolchain file for cross-compilation, then build libzt
ARG ARCH
RUN . /opt/axis/acapsdk/environment-setup* && \
    CC_BIN=$(echo $CC | awk '{print $1}') && \
    CXX_BIN=$(echo $CXX | awk '{print $1}') && \
    CC_PATH=$(which $CC_BIN) && \
    CXX_PATH=$(which $CXX_BIN) && \
    if [ "$ARCH" = "aarch64" ]; then CMAKE_PROC=aarch64; else CMAKE_PROC=arm; fi && \
    printf 'set(CMAKE_SYSTEM_NAME Linux)\n' > /tmp/toolchain.cmake && \
    printf 'set(CMAKE_SYSTEM_PROCESSOR %s)\n' "${CMAKE_PROC}" >> /tmp/toolchain.cmake && \
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
WORKDIR /opt/app

# Patch the architecture and version placeholders in manifest.json
ARG ZT_CORE_VERSION
RUN sed -i "s/\"BUILDARCH\"/\"${ARCH}\"/" manifest.json && \
    sed -i "s/BUILDVER/${ZT_CORE_VERSION}/" manifest.json

# Cross-compile the proxy binary (linked against static libzt) and place in lib/.
# libzt is C++ internally so we link with the C++ compiler.
RUN . /opt/axis/acapsdk/environment-setup* && \
    mkdir -p lib && \
    CC_BIN=$(echo $CC | awk '{print $1}') && \
    CXX_BIN=$(echo $CXX | awk '{print $1}') && \
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

# Build the ACAP package (compiles config_bridge.c and packages everything)
RUN . /opt/axis/acapsdk/environment-setup* && acap-build .
