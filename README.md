# ZeroTier ACAP for Axis Cameras

[![Release](https://img.shields.io/github/v/release/Mo3he/Axis_Cam_ZeroTier?style=flat)](https://github.com/Mo3he/Axis_Cam_ZeroTier/releases)
[![License](https://img.shields.io/github/license/Mo3he/Axis_Cam_ZeroTier?style=flat)](LICENSE)
[![Build](https://github.com/Mo3he/Axis_Cam_ZeroTier/actions/workflows/build.yml/badge.svg)](https://github.com/Mo3he/Axis_Cam_ZeroTier/actions/workflows/build.yml)
[![Super-Linter](https://github.com/Mo3he/Axis_Cam_ZeroTier/actions/workflows/super-linter.yml/badge.svg)](https://github.com/Mo3he/Axis_Cam_ZeroTier/actions/workflows/super-linter.yml)
[![Sponsor](https://img.shields.io/badge/Sponsor%20My%20Work-EA4AAA?style=flat&logo=github&logoColor=white)](https://github.com/sponsors/Mo3he)
[![Buy Me A Coffee](https://img.shields.io/badge/Buy%20Me%20A%20Coffee-FFDD00?style=flat&logo=buy-me-a-coffee&logoColor=black)](https://www.buymeacoffee.com/mo3he)

A ZeroTier VPN client that runs directly on Axis cameras as an ACAP application,
enabling secure remote access without requiring any other equipment or network
configuration. ZeroTier achieves this in a secure, simple, and lightweight way.

> **Disclaimer:** Independent, community-developed ACAP package. Not an official
> Axis product and not affiliated with, endorsed by, or supported by Axis
> Communications AB or ZeroTier, Inc. Use at your own risk.

> **ZeroTier Notice:** ZeroTier is a product of ZeroTier, Inc. This package
> independently redistributes ZeroTier components (ZeroTierOne, libzt, lwIP)
> under their respective licenses (MPL 2.0, Apache 2.0, BSD 3-Clause) and is not
> affiliated with, endorsed by, or supported by ZeroTier, Inc. For the official
> ZeroTier client, visit [zerotier.com](https://zerotier.com).

## Table of Contents

- [Overview](#overview)
- [Compatibility](#compatibility)
- [Installation](#installation)
- [Configuration](#configuration)
- [Ports & security](#ports--security)
- [How it works](#how-it-works)
- [Build from source](#build-from-source)
- [Roadmap](#roadmap)
- [Links](#links)
- [License](#license)

## Overview

The app runs entirely in userspace using
[libzt](https://github.com/zerotier/libzt) (ZeroTier Sockets SDK + lwIP TCP/IP
stack) with ZeroTierOne 1.16.0 as the core engine, which means:

- **No root required:** runs as the standard unprivileged `sdk` ACAP user (ACAP
  4 builds).
- **Compatible with AXIS OS 9.x through 13:** see the Compatibility section
  below.
- **No kernel TUN device:** all networking is handled inside the process.

## Compatibility

| Build | AXIS OS | Architecture | Notes |
|---|---|---|---|
| ACAP 4 (native SDK) | 10.x – 13 | aarch64 | Standard build |
| ACAP 4 (native SDK) | 10.x – 13 | armv7hf | Standard build |
| ACAP 3 (legacy SDK) | 9.x – 10.x | armv7hf | Legacy cameras (`EmbeddedDevelopment.Version=2.x`) |

> Most cameras use the **ACAP 4** build. Use the **ACAP 3** build only on legacy
> cameras that don't support ACAP 4 (typically AXIS OS 9–10).

## Installation

Download the `.eap` for your camera from the
[latest release](https://github.com/Mo3he/Axis_Cam_ZeroTier/releases/latest) and
install via the camera's web interface under **Apps -> Add app**.

## Configuration

### Via the web UI

1. Start the app.
2. Go to the app's settings page (dots menu -> Settings).
3. Enter your **ZeroTier Network ID** (16-character hex string from
   [my.zerotier.com](https://my.zerotier.com)).
4. *(Optional)* If using a **private ZeroTier planet** (self-hosted root server),
   upload your planet file using the **Planet File** field. Leave it empty to use
   the default ZeroTier public infrastructure.
5. Click **Open** to view the status page and logs.
6. **Authorize the device:** go to [ZeroTier Central](https://my.zerotier.com)
   (or your private controller), find the camera's Node ID in your network
   members, and check the "Auth" box.

### Via the Axis parameter API

The Network ID can be set programmatically using the camera's `param.cgi`
endpoint.

**Read the current Network ID:**

```sh
curl --digest -u <username>:<password> \
  "http://<device-ip>/axis-cgi/param.cgi?action=list&group=root.ZeroTier_VPN.NetworkID"
```

**Set a new Network ID:**

```sh
curl --digest -u <username>:<password> \
  --data "action=update&root.ZeroTier_VPN.NetworkID=<16-char-hex-id>" \
  "http://<device-ip>/axis-cgi/param.cgi"
```

**Clear the Network ID** (disconnects from ZeroTier):

```sh
curl --digest -u <username>:<password> \
  --data "action=update&root.ZeroTier_VPN.NetworkID=" \
  "http://<device-ip>/axis-cgi/param.cgi"
```

A successful response returns `OK`. The app detects the parameter change and
reconnects automatically, no restart required. Once authorized, the camera will
receive a ZeroTier IP address and all proxies and port forwarders will start
automatically. When uninstalling the ACAP, all changes and files are removed from
the camera.

### Reaching routed subnets (managed routes)

By default the camera can talk to other ZeroTier members directly. To let the
camera reach devices on a **routed network behind a ZeroTier gateway** (for
example a VMS or NVR on a separate LAN/VLAN), the app can route camera-initiated
traffic for those subnets through the gateway.

Requirements:

- A **managed route** must be defined in your ZeroTier network config
  (`<subnet> via <gateway-member-ip>`). This is configured in the controller:
  ZeroTier Central (a plan that permits custom routes) or a self-hosted
  controller. ZeroTier will not carry traffic to a subnet that has no managed
  route, regardless of the app.
- A ZeroTier member on the target network acting as the gateway for that subnet.

Usage: open the app's settings page -> **Managed Routes (Advanced)**.

- Leave the gateway field **blank** to auto-detect it from the network's managed
  routes (recommended).
- Or enter the gateway member's ZeroTier IP explicitly.

The installed managed routes and the active gateway are shown on the status page
for troubleshooting. The gateway can also be set through the parameter API:

```sh
curl --digest -u <username>:<password> \
  --data "action=update&root.ZeroTier_VPN.ManagedGateway=<gateway-ip>" \
  "http://<device-ip>/axis-cgi/param.cgi"
```

## Ports & security

Once connected, the camera is reachable from the ZeroTier network via:

| Service | Address | Purpose |
|---|---|---|
| Direct port forwarding | `<zerotier-ip>:80 / 443 / 554` | Camera HTTP, HTTPS, and RTSP over ZeroTier |
| Inbound SOCKS5 | `<zerotier-ip>:1080` | Reach any camera port from the ZeroTier network |
| Outbound HTTP CONNECT | `127.0.0.1:8080` * | Camera -> ZeroTier for HTTP/HTTPS |
| Outbound SOCKS5 | `127.0.0.1:1080` * | Camera -> ZeroTier for SOCKS5-aware apps |

\* *If another VPN ACAP is already listening on port 8080 or 1080, ZeroTier VPN
automatically falls back to the next available port (8181/8282/8383 for HTTP,
1081/1082/1083 for SOCKS5). The actual port in use is shown in the web UI under
**Proxy Addresses**.*

> **Security:** the direct port forwarders and inbound SOCKS5 are reachable by
> any authorized member of your ZeroTier network. Control access with ZeroTier's
> member authorization and flow rules, and keep the camera behind its normal
> authentication.

## How it works

- **Direct port forwarding:** ports 80 (HTTP), 443 (HTTPS), and 554 (RTSP) on
  the ZeroTier IP are transparently forwarded to the camera's local services.
  Point your browser or RTSP client directly at the ZeroTier IP.
- **Inbound SOCKS5 on `<zerotier-ip>:1080`:** configure any SOCKS5-aware client
  to use `<zerotier-ip>:1080` for access to any camera port from the ZeroTier
  network.
- **Outbound HTTP CONNECT proxy on `127.0.0.1:8080`:** routes camera-initiated
  HTTP/HTTPS traffic out through ZeroTier.
- **Outbound SOCKS5 on `127.0.0.1:1080`:** routes camera-initiated TCP
  connections out through ZeroTier for apps that support SOCKS5.

## Build from source

Requires Docker or Podman. The build scripts auto-detect which is available. Two
separate build scripts cover the two SDK generations.

The slow step (cloning and compiling
[libzt](https://github.com/zerotier/libzt), ZeroTierOne) is isolated into a
**base image** that is built once and reused on every subsequent rebuild. After
the first run, rebuilding after a code change takes under a minute.

**ACAP 4 native SDK (AXIS OS 11.11+, aarch64 + armv7hf):**

```sh
./build.sh
```

**ACAP 3 SDK (AXIS OS 9.x – 10.x, armv7hf only):**

```sh
./acap3/build.sh
```

To force a rebuild of the libzt base images (e.g. after upgrading libzt or the
SDK version), pass `--build-base`:

```sh
./build.sh --build-base
./acap3/build.sh --build-base
```

## Roadmap

### AXIS OS 13 Preparation

AXIS OS 13 is scheduled for release in September 2026 and introduces several
breaking changes that affect all ACAP applications. A preview build with all
breaking changes is available for testing.

- [ ] **Recompile for 64-bit time (Y2038)** - AXIS OS 13 switches to a 64-bit
  time interface to solve the Year 2038 problem. All ACAP applications must be
  recompiled against the updated SDK or the device will roll back the OS upgrade.
  Both ACAP 4 and ACAP 3 builds need to be updated.
- [ ] **Sign the ACAP via the ACAP Portal** - Unsigned ACAP applications will no
  longer be installable in production environments.
- [ ] **Migrate manifest to Schema v2** - The `manifest.json` must use Manifest
  Schema v2 and explicitly declare which AXIS OS versions the app is compatible
  with.
- [ ] **Audit for executable stack usage** - Verify that neither the ACAP binary
  nor any bundled library (libzt, lwIP) uses an executable stack.
- [ ] **Review HTTP port forwarding under HTTPS-only policy** - OS 13 enforces
  HTTPS-only connections by default. Evaluate the impact on the port 80 (HTTP)
  forwarding feature.
- [ ] **Review install/uninstall script compliance** - Check any post-install or
  pre-uninstall scripts against the updated rules introduced in OS 13.

Reference:
[AXIS OS 13 Breaking Changes](https://www.axis.com/for-developers/news/AXIS-OS-13-breaking-changes)
| [Full change list](https://help.axis.com/en-us/axis-os#changes-in-axis-os-13)

## Links

- [ZeroTier](https://zerotier.com/)
- [ZeroTier GitHub](https://github.com/zerotier)
- [libzt (ZeroTier Sockets SDK)](https://github.com/zerotier/libzt)
- [Axis Communications](https://www.axis.com/)

## License

The packaging code in this repository is licensed under BSD 3-Clause (see
[LICENSE](LICENSE)). Bundled upstream components (ZeroTierOne MPL-2.0, libzt
Apache-2.0, lwIP BSD) are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
