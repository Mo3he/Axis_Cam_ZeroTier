# ZeroTier VPN ACAP

A ZeroTier VPN client that runs directly on Axis cameras as an ACAP application.

Current version: 1.16.0

Download the pre-built `.eap` for your camera's architecture from the
[latest release](https://github.com/Mo3he/Axis_Cam_ZeroTier/releases/latest)
and install via the camera's web interface under Apps → Add app.

### Disclaimer: This is an independent, community-developed ACAP package and is not an official Axis Communications product. It was developed entirely on personal time and is not affiliated with, endorsed by, or supported by Axis Communications AB. Use it at your own risk. For official Axis software, visit axis.com

> **ZeroTier Notice:** ZeroTier is a product of ZeroTier, Inc. This package independently redistributes ZeroTier components (ZeroTierOne, libzt, lwIP) under their respective licenses (MPL 2.0, Apache 2.0, BSD 3-Clause — see [LICENSE](LICENSE)) and is not affiliated with, endorsed by, or supported by ZeroTier, Inc. For the official ZeroTier client, visit [zerotier.com](https://zerotier.com).

## Overview

Adding a VPN client directly to the camera allows secure remote access without
requiring any other equipment or network configuration. ZeroTier achieves this
in a secure, simple, and lightweight way.

Version 1.16.0 runs entirely in userspace using [libzt](https://github.com/zerotier/libzt) (ZeroTier
Sockets SDK + lwIP TCP/IP stack) with ZeroTierOne 1.16.0 as the core engine, which means:

- **No root required** — runs as the standard unprivileged `sdk` ACAP user
- **Compatible with Axis OS 11 and 12** — OS 12 blocked root ACAP apps; this version works on both
- **No kernel TUN device** — all networking is handled inside the process

## How it works

Once connected, the camera is reachable from the ZeroTier network via:

- **Direct port forwarding** — ports 80 (HTTP), 443 (HTTPS), and 554 (RTSP) on
  the ZeroTier IP are transparently forwarded to the camera's local services.
  Point your browser or RTSP client directly at the ZeroTier IP.
- **SOCKS5 proxy on port 1080** — configure any SOCKS5-aware client to use
  `<zerotier-ip>:1080` for access to any camera port.

## Compatibility

Works on Axis cameras with ARM or aarch64 SoCs running Axis OS 11.11 (LTS) or
later, including Axis OS 12.

To check your camera's architecture:

```
curl --digest -u <username>:<password> \
  http://<device-ip>/axis-cgi/param.cgi?action=list&group=Properties.System.Architecture
```

## Installing

Download the pre-built `.eap` for your camera's architecture from the
[latest release](https://github.com/Mo3he/Axis_Cam_ZeroTier/releases/latest)
and install via the camera's web interface under Apps → Add app.

| Architecture               | File                              |
|---------------------------|-----------------------------------|
| aarch64 (most cameras 2019+) | `ZeroTier_VPN_1_16_0_aarch64.eap` |
| armv7hf (older cameras)      | `ZeroTier_VPN_1_16_0_armv7hf.eap` |

## Configuration

1. Start the app.
2. Go to the app's settings page (⋮ → Settings).
3. Enter your **ZeroTier Network ID** (16-character hex string from
   [my.zerotier.com](https://my.zerotier.com)).
4. Click **Open** to view the status page and logs.
5. **Authorize the device** — go to
   [ZeroTier Central](https://my.zerotier.com), find the camera's Node ID in
   your network members, and check the "Auth" box.

Once authorized, the camera will receive a ZeroTier IP address and the port
forwarders will start automatically.

When uninstalling the ACAP, all changes and files are removed from the camera.

## Building from source

Requires Docker.

```
./build.sh
```

This builds both architectures, copies the `.eap` files to the repo root, and
cleans up temporary containers.

The build:
1. Cross-compiles [libzt](https://github.com/zerotier/libzt) for the target
   architecture using the ACAP SDK toolchain
2. Builds the userspace proxy binary (linked with libzt)
3. Packages everything as an ACAP `.eap`

## Legacy versions

The old root-based ACAP (v1.x) is preserved in the `aarch64/` and `arm/`
directories for reference. These versions require root and will **not** work on
Axis OS 12+.

## Links

- [ZeroTier](https://zerotier.com/)
- [ZeroTier GitHub](https://github.com/zerotier)
- [libzt (ZeroTier Sockets SDK)](https://github.com/zerotier/libzt)
- [Axis Communications](https://www.axis.com/)

