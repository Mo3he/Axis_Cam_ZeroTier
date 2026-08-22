# App image — starts from the pre-built libzt base so the slow git clone and
# cmake steps are skipped entirely on every code-only rebuild.
# Build the base first with:  sh build.sh --build-base
ARG ARCH=aarch64
FROM zerotier-libzt-base-${ARCH}
# Re-declare after FROM so it's available to RUN instructions
ARG ARCH

COPY ./app /opt/app/
WORKDIR /opt/app

# Patch the architecture placeholder in manifest.json.
# The version is stored directly in manifest.json, not injected at build time.
RUN sed -i "s/\"BUILDARCH\"/\"${ARCH}\"/" manifest.json

# Cross-compile the proxy binary (linked against static libzt) and place in lib/.
# libzt is C++ internally so we link with the C++ compiler.
RUN . /opt/axis/acapsdk/environment-setup* && \
    mkdir -p lib && \
    CC_BIN=$(echo $CC | awk '{print $1}') && \
    CXX_BIN=$(echo $CXX | awk '{print $1}') && \
    $CC_BIN --sysroot=${SDKTARGETSYSROOT} -O2 -g -Wall -std=gnu11 \
        -I/tmp/libzt/include \
        proxy/proxy.c \
        /tmp/libzt/build/lib/libzt.a \
        -lstdc++ -lpthread -lm \
        -static \
        -Wl,-z,noexecstack \
        -o lib/zerotier-userspace && \
    chmod 755 lib/zerotier-userspace
# Must be the SDK's cross strip: the host `strip` cannot read ARM ELF and failed
# silently here, leaving ~20 MB of symbols (86% of the binary) in the package.
# The unstripped copy lives outside /opt/app so it stays out of the .eap.
RUN mkdir -p /opt/debug && \
    cp lib/zerotier-userspace /opt/debug/zerotier-userspace.unstripped
RUN . /opt/axis/acapsdk/environment-setup* && \
    "${STRIP:?SDK environment did not set STRIP}" lib/zerotier-userspace

# Build the ACAP package (compiles config_bridge.c and packages everything)
RUN . /opt/axis/acapsdk/environment-setup* && acap-build .
