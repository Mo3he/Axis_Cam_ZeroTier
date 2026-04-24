# App image — starts from the pre-built libzt base so the slow git clone and
# cmake steps are skipped entirely on every code-only rebuild.
# Build the base first with:  sh build.sh --build-base
ARG ARCH=aarch64
FROM zerotier-libzt-base-${ARCH}
# Re-declare after FROM so it's available to RUN instructions
ARG ARCH

COPY ./app /opt/app/
WORKDIR /opt/app

# Patch the architecture and version placeholders in manifest.json
ARG ACAP_VERSION=1.16.8
RUN sed -i "s/\"BUILDARCH\"/\"${ARCH}\"/" manifest.json && \
    sed -i "s/BUILDVER/${ACAP_VERSION}/" manifest.json

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
