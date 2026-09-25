# Built on the pre-built libzt base so code-only rebuilds skip the slow clone and
# cmake steps. Build the base first with:  sh build.sh --build-base
ARG ARCH=aarch64
FROM zerotier-libzt-base-${ARCH}
# Re-declare after FROM so it's available to RUN instructions
ARG ARCH

COPY ./app /opt/app/
WORKDIR /opt/app

# Only the arch is patched; the version lives directly in manifest.json.
RUN sed -i "s/\"BUILDARCH\"/\"${ARCH}\"/" manifest.json

# libzt is C++ internally, hence -lstdc++.
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
