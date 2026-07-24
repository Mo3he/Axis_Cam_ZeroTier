# Changelog

All notable changes to this project are documented here. Each version
links to its full release notes on GitHub.

The format is based on [Keep a Changelog](https://keepachangelog.com/).

## [1.16.13] - 2026-07-24 - Save settings on recorder / access-control devices

- Fix: settings can now be saved on Axis devices that do not expose
  `/axis-cgi/param.cgi`, such as recorder/NVR products (e.g. S3008) and
  access-control controllers (e.g. A1610, A1710, A1810). On these devices the
  web UI previously appeared unable to persist configuration (issue #9).
- The app now exposes a small settings endpoint at
  `/local/ZeroTier_VPN/api/settings` through a manifest reverse-proxy. The web
  UI uses `param.cgi` when available and transparently falls back to this
  endpoint when it is not, so configuration is written directly through the
  ACAP parameter store.

## [1.16.12-Signed] - 2026-07-21 - ZeroTier VPN 1.16.12 (Signed)

- Packages are now signed with the Axis ACAP signing service and install
  normally on AXIS OS 12.10 and later.
- Vendor updated to `moshe@mohome.net` with the registered vendor ID.
- The `acap3` variant remains unsigned (manifest schema v1.x).
- Upgrading from an earlier unsigned version can fail with "Couldn't
  install: app" (device log: "Vendor ID in manifest does not match the
  vendor ID of the previous version"). Back up your config, uninstall the
  old version, then install this one.

## [1.16.12] - 2026-07-16 - Fix web UI dropping to "System is getting ready"

- Fix: the web interface no longer intermittently reverts to the "System is
  getting ready" screen when accessed over the ZeroTier IP. The relay was
  applying its 10 s connect timeout to established connections, tearing down
  idle keep-alive/websocket connections after 10 s of silence (issue #7).
- TCP keepalive is now enabled on both relay sockets so peers that vanish
  without a clean close are still reaped.

## [1.16.11] - 2026-07-16 - Managed route gateway support

- Reach devices on subnets behind a ZeroTier gateway (e.g. a VMS on a routed
  LAN), not just direct ZeroTier members. The userspace stack now routes
  off-subnet traffic through a gateway member.
- Auto-detects the gateway from the network's managed routes, or set it
  explicitly via the new **Managed Routes (Advanced)** setting.
- Managed routes and the active gateway are shown in the status page for
  troubleshooting.
- Fix: changing the Network ID now cleanly restarts the service so the new
  network is always joined.

## [1.16.10] - 2026-07-07

## [1.16.9] - 2026-04-25

## [1.16.8] - 2026-04-24

## [1.16.7] - 2026-04-23

## [1.16.6] - 2026-04-17 - ZeroTier VPN v1.16.6

## [1.16.5] - 2026-04-17

## [1.16.4] - 2026-04-17

## [1.16.3] - 2026-04-15 - Custom planet file support (beta)

## [1.16.2] - 2026-04-14

## [1.16.1] - 2026-04-14

## [1.16.0] - 2026-04-13 - Latest ZeroTierOne core

## [1.14.2] - 2026-04-13 - Upgraded ZeroTierOne core

## [1.8.10] - 2026-04-13 - Userspace rewrite, no root required

## [1.12.2] - 2023-09-20 - Updated binaries

## [1.10.6] - 2023-07-07 - Static binaries from source

[1.16.11]: https://github.com/Mo3he/Axis_Cam_ZeroTier/releases/tag/v1.16.11
[1.16.10]: https://github.com/Mo3he/Axis_Cam_ZeroTier/releases/tag/v1.16.10
[1.16.9]: https://github.com/Mo3he/Axis_Cam_ZeroTier/releases/tag/v1.16.9
[1.16.8]: https://github.com/Mo3he/Axis_Cam_ZeroTier/releases/tag/v1.16.8
[1.16.7]: https://github.com/Mo3he/Axis_Cam_ZeroTier/releases/tag/v1.16.7
[1.16.6]: https://github.com/Mo3he/Axis_Cam_ZeroTier/releases/tag/v1.16.6
[1.16.5]: https://github.com/Mo3he/Axis_Cam_ZeroTier/releases/tag/v1.16.5
[1.16.4]: https://github.com/Mo3he/Axis_Cam_ZeroTier/releases/tag/v1.16.4
[1.16.3]: https://github.com/Mo3he/Axis_Cam_ZeroTier/releases/tag/v1.16.3
[1.16.2]: https://github.com/Mo3he/Axis_Cam_ZeroTier/releases/tag/v1.16.2
[1.16.1]: https://github.com/Mo3he/Axis_Cam_ZeroTier/releases/tag/v1.16.1
[1.16.0]: https://github.com/Mo3he/Axis_Cam_ZeroTier/releases/tag/v1.16.0
[1.14.2]: https://github.com/Mo3he/Axis_Cam_ZeroTier/releases/tag/v1.14.2
[1.8.10]: https://github.com/Mo3he/Axis_Cam_ZeroTier/releases/tag/v1.8.10
[1.12.2]: https://github.com/Mo3he/Axis_Cam_ZeroTier/releases/tag/1.12.2
[1.10.6]: https://github.com/Mo3he/Axis_Cam_ZeroTier/releases/tag/1.10.6
