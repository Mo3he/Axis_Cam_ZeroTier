# ZeroTier VPN ACAP

A ZeroTier VPN client that runs directly on Axis cameras as an ACAP application.

Current version: 1.16.3

Download the pre-built `.eap` for your camera's architecture from the
[latest release](https://github.com/Mo3he/Axis_Cam_ZeroTier/releases/latest)
and install via the camera's web interface under Apps → Add app.

> **Disclaimer:** This is an independent, community-developed ACAP package and is not an official Axis Communications product. It is not affiliated with, endorsed by, or supported by Axis Communications AB. Use it at your own risk. For official Axis software, visit axis.com 

> **ZeroTier Notice:** ZeroTier is a product of ZeroTier, Inc. This package independently redistributes ZeroTier components (ZeroTierOne, libzt, lwIP) under their respective licenses (MPL 2.0, Apache 2.0, BSD 3-Clause — see [LICENSE](LICENSE)) and is not affiliated with, endorsed by, or supported by ZeroTier, Inc. For the official ZeroTier client, visit [zerotier.com](https://zerotier.com).

## Overview

Adding a VPN client directly to the camera allows secure remote access without
requiring any other equipment or network configuration. ZeroTier achieves this
in a secure, simple, and lightweight way.

Version 1.16.3 runs entirely in userspace using [libzt](https://github.com/zerotier/libzt) (ZeroTier
Sockets SDK + lwIP TCP/IP stack) with ZeroTierOne 1.16.0 as the core engine, which means:

- **No root required** — runs as the standard unprivileged `sdk` ACAP user (ACAP 4 builds)
- **Compatible with Axis OS 9.x through 12** — see the Compatibility section below
- **No kernel TUN device** — all networking is handled inside the process

## Compatibility

| Build | Axis OS | Architecture | File |
|---|---|---|---|
| ACAP 4 native SDK | 11.11+ (incl. OS 12) | aarch64 | `ZeroTier_VPN_1_16_3_aarch64.eap` |
| ACAP 4 native SDK | 11.11+ (incl. OS 12) | armv7hf | `ZeroTier_VPN_1_16_3_armv7hf.eap` |
| ACAP 3 SDK | 9.x – 10.x | armv7hf | `ZeroTier_VPN_1_16_3_armv7hf_acap3.eap` |

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
4. Click **Open** to view the status page and logs.
5. **Authorize the device** — go to
   [ZeroTier Central](https://my.zerotier.com), find the camera's Node ID in
   your network members, and check the "Auth" box.

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
- **Outbound HTTP CONNECT proxy on `127.0.0.1:8080`** — routes camera-initiated
  HTTP/HTTPS traffic out through ZeroTier.
- **Outbound SOCKS5 on `127.0.0.1:1080`** — routes camera-initiated TCP
  connections out through ZeroTier for apps that support SOCKS5.

## What's new in v1.16.3

### Custom planet file support *(beta)*
Allows connecting to a **self-hosted ZeroTier controller** by uploading a custom planet file through the web UI.

> **Beta notice:** This feature is functional but has had limited real-world testing against self-hosted controllers. Please report any issues.

**Via the web UI:**
1. Go to the app's settings page (⋮ → Settings).
2. In the **Custom Planet (Self-Hosted Controller)** card, click **Choose File** and select your custom `planet` binary.
3. Click **Upload**. The proxy restarts automatically with the new planet.
4. To revert to the default ZeroTier planet, click **Reset**.

**Via the parameter API:**

The planet file must be uploaded as a base64-encoded string:

```bash
# Encode your planet file
B64=$(base64 -i /path/to/planet)

# Upload to the camera
curl --digest -u <username>:<password> \
  --data "action=update&root.ZeroTier_VPN.PlanetFile=${B64}" \
  "http://<device-ip>/axis-cgi/param.cgi"
```

**Clear the custom planet** (reverts to default ZeroTier planet):
```bash
curl --digest -u <username>:<password> \
  --data "action=update&root.ZeroTier_VPN.PlanetFile=" \
  "http://<device-ip>/axis-cgi/param.cgi"
```

A planet change triggers a full proxy restart (unlike a Network ID change which only reloads the network config).

---

## What's new in v1.16.2

### HTTP CONNECT proxy (port 8080)
Routes outbound HTTP/HTTPS traffic from the camera through ZeroTier. Set
`http://127.0.0.1:8080` wherever an HTTP or HTTPS proxy field is available.

**System → Network → Global proxies** (general camera outbound traffic):
- HTTP proxy: `http://127.0.0.1:8080`
- HTTPS proxy: `http://127.0.0.1:8080`

**System → MQTT → Broker** (built-in MQTT client):
- HTTP proxy: `http://127.0.0.1:8080`
- HTTPS proxy: `http://127.0.0.1:8080`

### Outbound SOCKS5 proxy (localhost:1080)
For ACAP apps or services that support SOCKS5, set their proxy to
`127.0.0.1:1080`.

### Web UI
The **Connection Details** panel now shows the proxy addresses instead of the
legacy per-port forward addresses.

### ACAP 3 build for older cameras (Axis OS 9.x)
Cameras running Axis OS 9.x with EmbeddedDevelopment 2.x (e.g. M1065-LW,
M4206-V) are now supported via a separate ACAP 3 SDK build. Use the
`_armv7hf_acap3.eap` file from the release assets.

## Building from source

Requires Docker. Two separate build scripts cover the two SDK generations.

**ACAP 4 native SDK (Axis OS 11.11+, aarch64 + armv7hf):**
```
./build.sh
```

**ACAP 3 SDK (Axis OS 9.x – 10.x, armv7hf only):**
```
cd acap3 && ./build.sh
```

Each build cross-compiles [libzt](https://github.com/zerotier/libzt), builds
the userspace proxy binary, and packages everything as an ACAP `.eap`.

## Legacy versions

The old root-based ACAP (v1.x) is preserved in the `aarch64/` and `arm/`
directories for reference. These versions require root and will **not** work on
Axis OS 12+.

## Links

- [ZeroTier](https://zerotier.com/)
- [ZeroTier GitHub](https://github.com/zerotier)
- [libzt (ZeroTier Sockets SDK)](https://github.com/zerotier/libzt)
- [Axis Communications](https://www.axis.com/)
