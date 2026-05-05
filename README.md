# ZeroTier VPN ACAP

A ZeroTier VPN client that runs directly on Axis cameras as an ACAP application, enabling secure remote access without requiring any other equipment or network configuration. ZeroTier achieves this in a secure, simple, and lightweight way.

Current version: **1.16.9**

The app runs entirely in userspace using [libzt](https://github.com/zerotier/libzt) (ZeroTier Sockets SDK + lwIP TCP/IP stack) with ZeroTierOne 1.16.0 as the core engine, which means:

- **No root required** — runs as the standard unprivileged `sdk` ACAP user (ACAP 4 builds)
- **Compatible with Axis OS 9.x through 12** — see the Compatibility section below
- **No kernel TUN device** — all networking is handled inside the process

Download the pre-built `.eap` for your camera's architecture from the
[latest release](https://github.com/Mo3he/Axis_Cam_ZeroTier/releases/latest)
and install via the camera's web interface under **Apps → Add app**.

[![Buy Me A Coffee](https://img.shields.io/badge/Buy%20Me%20A%20Coffee-support-orange?style=flat&logo=buy-me-a-coffee)](https://www.buymeacoffee.com/mo3he)

> **Disclaimer:** This is an independent, community-developed ACAP package and is not an official Axis Communications product. It is not affiliated with, endorsed by, or supported by Axis Communications AB. Use it at your own risk. For official Axis software, visit axis.com 

> **ZeroTier Notice:** ZeroTier is a product of ZeroTier, Inc. This package independently redistributes ZeroTier components (ZeroTierOne, libzt, lwIP) under their respective licenses (MPL 2.0, Apache 2.0, BSD 3-Clause — see [LICENSE](LICENSE)) and is not affiliated with, endorsed by, or supported by ZeroTier, Inc. For the official ZeroTier client, visit [zerotier.com](https://zerotier.com).

## Compatibility

| Build | Axis OS | Architecture | File |
|---|---|---|---|
| ACAP 4 native SDK | 11.11+ (incl. OS 12) | aarch64 | `ZeroTier_VPN_1_16_8_aarch64.eap` |
| ACAP 4 native SDK | 11.11+ (incl. OS 12) | armv7hf | `ZeroTier_VPN_1_16_8_armv7hf.eap` |
| ACAP 3 SDK | 9.x – 10.x | armv7hf | `ZeroTier_VPN_1_16_8_armv7hf_acap3.eap` |

The ACAP 3 build targets cameras with `EmbeddedDevelopment.Version=2.x` (e.g. M1065-LW, M4206-V).
Cameras on Axis OS 6.x or earlier (EmbeddedDevelopment 1.x) are not supported.

To check your camera's OS and EmbeddedDevelopment version:
```
curl --digest -u <username>:<password> \
  "http://<device-ip>/axis-cgi/param.cgi?action=list&group=root.EmbeddedDevelopment"
```

## Installing

Download the `.eap` for your camera from the
[latest release](https://github.com/Mo3he/Axis_Cam_ZeroTier/releases/latest)
and install via the camera's web interface under Apps → Add app.

## Configuration

### Via the web UI

1. Start the app.
2. Go to the app's settings page (⋮ → Settings).
3. Enter your **ZeroTier Network ID** (16-character hex string from
   [my.zerotier.com](https://my.zerotier.com)).
4. *(Optional)* If using a **private ZeroTier planet** (self-hosted root
   server), upload your planet file using the **Planet File** field.
   Leave it empty to use the default ZeroTier public infrastructure.
5. Click **Open** to view the status page and logs.
6. **Authorize the device** — go to
   [ZeroTier Central](https://my.zerotier.com) (or your private controller),
   find the camera's Node ID in your network members, and check the "Auth" box.

### Via the Axis parameter API

The Network ID can be set programmatically using the camera's `param.cgi` endpoint.

**Read the current Network ID:**
```
curl --digest -u <username>:<password> \
  "http://<device-ip>/axis-cgi/param.cgi?action=list&group=root.ZeroTier_VPN.NetworkID"
```

**Set a new Network ID:**
```
curl --digest -u <username>:<password> \
  --data "action=update&root.ZeroTier_VPN.NetworkID=<16-char-hex-id>" \
  "http://<device-ip>/axis-cgi/param.cgi"
```

**Clear the Network ID** (disconnects from ZeroTier):
```
curl --digest -u <username>:<password> \
  --data "action=update&root.ZeroTier_VPN.NetworkID=" \
  "http://<device-ip>/axis-cgi/param.cgi"
```

A successful response returns `OK`. The app detects the parameter change and
reconnects automatically — no restart required.

Once authorized, the camera will receive a ZeroTier IP address and all proxies
and port forwarders will start automatically.

When uninstalling the ACAP, all changes and files are removed from the camera.

## How it works

Once connected, the camera is reachable from the ZeroTier network via:

- **Direct port forwarding** — ports 80 (HTTP), 443 (HTTPS), and 554 (RTSP) on
  the ZeroTier IP are transparently forwarded to the camera's local services.
  Point your browser or RTSP client directly at the ZeroTier IP.
- **Inbound SOCKS5 on `<zerotier-ip>:1080`** — configure any SOCKS5-aware
  client to use `<zerotier-ip>:1080` for access to any camera port from the
  ZeroTier network.
- **Outbound HTTP CONNECT proxy on `127.0.0.1:8080`** \* — routes camera-initiated
  HTTP/HTTPS traffic out through ZeroTier.
- **Outbound SOCKS5 on `127.0.0.1:1080`** \* — routes camera-initiated TCP
  connections out through ZeroTier for apps that support SOCKS5.

\* *If another VPN ACAP is already listening on port 8080 or 1080, ZeroTier VPN
automatically falls back to the next available port (8181/8282/8383 for HTTP,
1081/1082/1083 for SOCKS5). The actual port in use is shown in the web UI under
**Proxy Addresses**.*

## Building from source

Requires Docker or Podman. The build scripts auto-detect which is available.
Two separate build scripts cover the two SDK generations.

The slow step (cloning and compiling [libzt](https://github.com/zerotier/libzt)
+ ZeroTierOne) is isolated into a **base image** that is built once and reused
on every subsequent rebuild. After the first run, rebuilding after a code change
takes under a minute.

**ACAP 4 native SDK (Axis OS 11.11+, aarch64 + armv7hf):**
```
./build.sh
```

**ACAP 3 SDK (Axis OS 9.x – 10.x, armv7hf only):**
```
./acap3/build.sh
```

To force a rebuild of the libzt base images (e.g. after upgrading libzt or the
SDK version), pass `--build-base`:
```
./build.sh --build-base
./acap3/build.sh --build-base
```

## Roadmap

### AXIS OS 13 Preparation

AXIS OS 13 is scheduled for release in September 2026 and introduces several breaking changes that affect all ACAP applications. A preview build with all breaking changes is available for testing.

- [ ] **Recompile for 64-bit time (Y2038)** - AXIS OS 13 switches to a 64-bit time interface to solve the Year 2038 problem. All ACAP applications must be recompiled against the updated SDK or the device will roll back the OS upgrade. Both ACAP 4 and ACAP 3 builds need to be updated.
- [ ] **Sign the ACAP via the ACAP Portal** - Unsigned ACAP applications will no longer be installable in production environments. The app needs to go through the official Axis signing process once AXIS OS 13 launches.
- [ ] **Migrate manifest to Schema v2** - The `manifest.json` must use Manifest Schema v2 and explicitly declare which AXIS OS versions the app is compatible with to prevent compatibility issues after updates.
- [ ] **Audit for executable stack usage** - Verify that neither the ACAP binary nor any bundled library (libzt, lwIP) uses an executable stack, as this will be blocked by new security restrictions in OS 13.
- [ ] **Review HTTP port forwarding under HTTPS-only policy** - OS 13 enforces HTTPS-only connections by default. Evaluate the impact on the port 80 (HTTP) forwarding feature and update documentation or behaviour accordingly.
- [ ] **Review install/uninstall script compliance** - Check any post-install or pre-uninstall scripts against the updated rules introduced in OS 13.

Reference: [AXIS OS 13 Breaking Changes](https://www.axis.com/for-developers/news/AXIS-OS-13-breaking-changes) | [Full change list](https://help.axis.com/en-us/axis-os#changes-in-axis-os-13)

## Links

- [ZeroTier](https://zerotier.com/)
- [ZeroTier GitHub](https://github.com/zerotier)
- [libzt (ZeroTier Sockets SDK)](https://github.com/zerotier/libzt)
- [Axis Communications](https://www.axis.com/)
